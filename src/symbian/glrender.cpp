// glrender.cpp - the PS1 rasteriser on MBX through OpenGL ES 1.1.
//
// Why: section 14 of the report measures the same Gran Turismo frame at 6.9 ms
// here against 14.4 for "gpu_unai + blit". Numbers from the device, not estimates.
//
// It takes the same command packets gpu_unai parses, assembles triangles from
// them, unpacks the needed texture pages out of guest VRAM and draws.
// The swoff flag turns the software pass off entirely, leaving this the only
// scene; with the pass on, VRAM stays truthful for reads and copies.
//
// Measured limits the design follows (section 14):
//   - a draw call costs 9.5-14 us, so primitives are batched;
//   - submission order must NOT change (on PS1 it decides overlap),
//     so only ADJACENT primitives with the same state are batched;
//   - changing the bound texture costs as much as a draw call.
//
// A 2048x2048 texture atlas was tried, to stop page changes from breaking
// batches, but device measurements showed it is the wrong answer: a bound
// 256x256 texture costs nothing, while 2048x2048 costs 20 ms per frame, because
// the driver will not keep an 8 MB object resident (313 s against 178 s without
// the atlas). So one texture per page stays, and draw calls are batched only
// by adjacency.

#include <e32std.h>
#include <f32file.h>
#include <GLES/gl.h>

extern "C" void ps1LogFmt(const char *aType, const char *aFmt, ...);
extern "C" int  voutSymbianGlSwap(void);
extern "C" int  voutSymbianGlVramQuad(void);
extern "C" int  voutSymbianGlDepthBits(void);
extern "C" void voutSymbianGlSize(int *aW, int *aH);
extern "C" void voutSymbianDbgState(int *aFbDirty, int *aFbDirtyArea, int *aStatus,
	int *aSrcX, int *aSrcY, int *aScrW, int *aScrH, int *aVoutOffset, int *aStale);
extern "C" unsigned int ndrc_symbian_fastfreq(void);
extern "C" unsigned int ndrc_symbian_fastcount(void);

// State gpu_unai passes along with every packet. The order is set by
// gpulib_if.cpp; keeping both sides in the same order is this comment header
// job, because there is deliberately no shared type (it would be a file shared
// between two repositories).
enum TGlrState
	{
	EStOfsX = 0, EStOfsY,
	EStClipX0, EStClipY0, EStClipX1, EStClipY1,
	EStBlendMode,     // 0: B/2+F/2  1: B+F  2: B-F  3: B+F/4
	EStTextMode,      // 32/64/96 - 4, 8, 16 bits per texel
	EStTba,           // texture page offset in VRAM, halfwords
	EStCba,           // CLUT index
	EStTwU,           // texture window mask along U, texels
	EStTwV,           // mask along V
	EStCount
	};

// Ceiling per frame. The old one, 1400, came from 324 primitives measured on a
// replay and four times that for headroom. A real race is nothing like it: with
// traffic on screen the vertex array filled every frame and 750 to 860 triangles
// were thrown away per frame - and being thrown away at the END of the frame,
// they were the interface and the cars nearest the camera, which is exactly how
// it looked. Measured on the device, not estimated.
static const TInt KMaxPrims = 6000;
static const TInt KMaxVerts = KMaxPrims * 6;
// Batches per frame. 256 was too few: in Gran Turismo every quad comes with its
// own palette, that is its own batch, and past the limit batchFor returned
// silently while emitVertex kept appending vertices to the LAST batch - so
// everything past it drew with its texture - half a frame flooded with grass.
static const TInt KMaxBatch = 8192;

// The z carries the submission index: on PS1 a later primitive covers an earlier
// one, and with a depth buffer that rule holds without keeping the draw order, so
// primitives may be regrouped by texture. The spare short is padding - the driver
// wants the colour and texture offsets four-byte aligned.
struct TGlrVert
	{
	GLshort x, y, z, pad;
	GLshort u, v;
	GLubyte r, g, b, a;
	};

struct TGlrBatch
	{
	TInt iFirst;      // first vertex
	TInt iCount;      // how many vertices
	TInt iBlend;      // -1 = opaque, otherwise mode 0..3
	TInt iTex;        // index into the texture cache, -1 = untextured
	TInt iClip;       // index of the draw area this batch was submitted under
	};

// A frame has more than one draw area: besides the back buffer there are
// transient ones (on the device boot showed 0,0..640,512). The projection is
// built from the one with the most vertices drawn under it, not the first and
// not the largest by area - both of those rules already produced a wrong
// picture. Primitives of other areas are not drawn: they would land at random.
static const TInt KMaxClips = 8;

static TBool     g_on;            // renderer enabled by flag
static TBool     g_ready;         // initialised and holding a context
static TGlrVert *g_v;             // interleaved vertex array (12 bytes per vertex)
static TInt      g_nVerts;
static TGlrBatch g_batch[KMaxBatch];
static TInt      g_nBatch;
static TInt      g_clip[4];       // draw area the projection is built from
static TInt      g_clips[KMaxClips][4];
static TInt      g_ofs[KMaxClips][2];   // group draw offset: this is what names the buffer
static TInt      g_clipVerts[KMaxClips];
static TInt      g_nClips;
static TInt      g_curClip;       // draw area of the current primitive, -1 = did not fit
static TInt      g_vpW, g_vpH;
static TInt      g_over;          // how many primitives did not fit
static TInt      g_logged;      // how many diagnostic frames have been written
// A direct write into guest VRAM - an image upload, a VRAM copy, an MDEC video
// frame - carries no primitives, so none of it can reach this rasteriser. The
// bounding box of such writes is collected per frame; when it lands in the
// buffer the frame was drawn into, the frame belongs to the old VRAM display
// path and this one steps aside.
static TInt      g_wrX0, g_wrY0, g_wrX1, g_wrY1;   // empty while iX1 < iX0
static TInt      g_vramFrames;    // frames handed back to the VRAM path
static TInt      g_mixedFrames;   // frames whose scene sat on a VRAM picture
static TInt      g_bgFrames;      // ... of those, ones where the picture went down
static TBool     g_bgForce;       // probe: put the VRAM picture under every frame
// Two silent losses, counted rather than left to be mistaken for a drawing fault:
// primitives whose group did not fit in the table, and whole groups that were not
// the one the projection was built from.
static TInt      g_clipFull;      // primitives with no group left to join
static TInt      g_dropVerts;     // vertices skipped for belonging to another group
static TInt      g_dropGroups;    // groups skipped, summed over frames
static TInt      g_holdFrames;    // frames with nothing new, screen left alone
static TBool     g_batchFull;     // the batch table is full and this primitive is lost
static TInt      g_dropPrims;     // primitives dropped for want of a batch
static TInt      g_lossLogged;    // how many per-frame loss lines have been written
// Every counter above is cumulative, which cannot say WHICH frame lost something
// nor how close that frame ran to the limits. These hold the values at frame start.
static TInt      g_f0Nofit, g_f0BatchOver, g_f0DropPrims, g_f0Over;
static TInt      g_f0ClipFull, g_f0DropVerts, g_f0Evict, g_f0Miss;
static TInt      g_primIdx;       // primitives seen this frame, counted for the depth
static GLshort   g_primZ;         // depth of the primitive being assembled
static TBool     g_depth;         // a depth buffer exists and is in use
static TInt      g_draws;         // draw calls issued, summed over frames
static TInt     *g_order;         // batch indices in the order they go out
static TInt     *g_batchNext;     // next batch with the same texture, -1 ends
static TInt     *g_texHead, *g_texTail;   // first and last batch per texture
static unsigned short *g_idx;     // vertex indices of the call being assembled
static TInt      g_lastSrcX = -1, g_lastSrcY = -1;   // display start last frame

// --- texture cache -----------------------------------------------------------
// Measured (section 14): per frame the game uses 12.7 distinct "page + CLUT"
// pairs and only 2.88 distinct pages, and changes VRAM contents 0.7 times per
// frame. So a cache of a few dozen entries should almost always hit, and that
// is exactly what it is sized for. Three measured rules shaped it:
//   - load WHOLE PAGES: the load cost has a fixed part of 0.7 ms, so a hundred
//     small pieces would cost seventy milliseconds;
//   - do NOT write into a texture the current frame draws from (a separate MBX penalty);
//   - the driver paletted format does not pay off on a whole page, so unpack ourselves.
static const TInt KTexSide   = 256;       // a PS1 page is always 256x256 texels
// A race frame asked for more than sixty-four distinct page-and-palette pairs and
// 130 to 210 primitives per frame were left with no texture at all - drawn flat,
// which is the white silhouette where a car should be. A slot is a 256x256 page
// at two bytes a texel, so this is 16 MB of texture memory against 8.
static const TInt KMaxTex    = 128;

struct TGlrTex
	{
	// The state this unpacked page reproduces. Kept whole: folding it into one
	// key collided between 4 and 8 bits per texel.
	GLuint  iTex;
	TInt    iTba, iCba, iMode, iMaskU, iMaskV;
	TInt    iFrame;      // last frame that drew from it
	TBool   iValid;
	// Where this page sits in VRAM - needed so only what the game actually
	// overwrote gets dropped. Dropping the whole cache cost 4.5 ms per frame:
	// measured, because that is how it was done first. The rectangle is the
	// window, not the full 256x256: outside the window the page is not read.
	TInt    iX, iY, iW, iH;  // in VRAM halfwords
	TInt    iCx, iCy, iCw;  // palette: it lives in VRAM too and can also change
	TInt    iUseU0, iUseV0, iUseU1, iUseV1;
	};

static TGlrTex   g_tex[KMaxTex];
static TInt      g_nTex;
static TInt      g_lastTex = -1;  // last texFor hit; adjacent primitives repeat it
// Probe only: one extra texture of a chosen side, bound before every real bind.
// Comparing runs at different sides prices a bind by texture SIZE, which decides
// whether an atlas smaller than the failed 2048 one is worth building.
static TInt      g_dummySide;
static GLuint    g_dummyTex;
static TUint16  *g_decode;        // page unpack buffer
static const TUint16 *g_vram;     // guest VRAM, 1024x512 halfwords
static TInt      g_frame;         // frame counter for eviction
static TInt      g_miss, g_hit, g_evict;
// How many times a texture found no room: every entry is already taken by this
// same frame. Such a primitive is drawn WITHOUT a texture, and on screen that
// looks like a fill with the vertex colour - a silent loss invisible without a counter.
static TInt      g_nofit;
// How many times a batch ran out. Silent truncation here cost half a frame.
static TInt      g_batchOver;
// Why the batch broke (texture change, blend mode change, or a buffer switch).
static TInt      g_brkTex, g_brkBlend, g_brkClip, g_batchSum, g_frameSum;
// How many sprites need texture wrapping (u0+w or v0+h past 256). Whether a
// texture atlas is possible depends on this number: in one, wrapping would run
// into the neighbouring page. For polygons the question does not arise - there U and V are bytes.
static TInt      g_spriteWrap, g_spriteAll;
static unsigned long long g_tSetup, g_tClear, g_tDraw, g_tSwap;
static unsigned long long g_useTexels, g_fullTexels, g_useSlots;


// A snapshot of what GL showed. Needed right here: with the software pass off,
// VRAM no longer holds the scene, so the old snapshot route (a screen dump
// through DSA) sees nothing under GL, and the picture has to be checked somehow.
static char  g_shotPath[64];
static TBool g_shotWanted;
static TBool g_shotDeferred;

extern "C" void glrShot(const char *aPath)
	{
	if (!aPath)
		return;
	TInt n = 0;
	while (aPath[n] && n < (TInt)sizeof(g_shotPath) - 1)
		{
		g_shotPath[n] = aPath[n];
		n++;
		}
	g_shotPath[n] = 0;
	g_shotWanted = ETrue;
	g_shotDeferred = EFalse;
	}

// The read happens BEFORE the buffer swap: after it the back buffer contents
// are undefined.
static void writeShot(void)
	{
	g_shotWanted = EFalse;
	const TInt bytes = g_vpW * g_vpH * 4;
	TUint8 *buf = (TUint8 *)User::Alloc(bytes);
	if (!buf)
		{
		ps1LogFmt("glr", "shot: alloc %d failed", bytes);
		return;
		}
	glReadPixels(0, 0, g_vpW, g_vpH, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	const GLenum err = glGetError();
	RFs fs;
	TInt rc = fs.Connect();
	if (rc == KErrNone)
		{
		TPtrC8 p8((const TUint8 *)g_shotPath);
		TBuf<64> path;
		path.Copy(p8);
		RFile f;
		rc = f.Replace(fs, path, EFileWrite);
		if (rc == KErrNone)
			{
			TPtrC8 data(buf, bytes);
			rc = f.Write(data);
			f.Close();
			}
		fs.Close();
		}
	User::Free(buf);
	ps1LogFmt("glr", "shot: %s %dx%d rgba rc=%d err=%u glrframe=%d deferred=%d",
		g_shotPath, (int)g_vpW, (int)g_vpH, (int)rc, (unsigned)err,
		(int)g_frame, (int)g_shotDeferred);
	}

extern "C" int glrEnabled(void)
	{
	return (g_on && g_ready) ? 1 : 0;
	}

extern "C" void glrSetEnabled(int aOn)
	{
	g_on = aOn ? ETrue : EFalse;
	}

extern "C" int glrInit(void)
	{
	if (g_ready)
		return 0;
	if (!g_v)
		g_v = (TGlrVert *)User::Alloc(KMaxVerts * sizeof(TGlrVert));
	if (!g_decode)
		g_decode = (TUint16 *)User::Alloc(KTexSide * KTexSide * 2);
	if (!g_idx)
		g_idx = (unsigned short *)User::Alloc(KMaxVerts * sizeof(unsigned short));
	if (!g_order)
		g_order = (TInt *)User::Alloc(KMaxBatch * sizeof(TInt));
	if (!g_batchNext)
		g_batchNext = (TInt *)User::Alloc(KMaxBatch * sizeof(TInt));
	if (!g_texHead)
		g_texHead = (TInt *)User::Alloc((KMaxTex + 1) * sizeof(TInt));
	if (!g_texTail)
		g_texTail = (TInt *)User::Alloc((KMaxTex + 1) * sizeof(TInt));
	if (!g_v || !g_decode || !g_idx || !g_order || !g_batchNext ||
	    !g_texHead || !g_texTail)
		{
		User::Free(g_v); g_v = NULL;
		User::Free(g_decode); g_decode = NULL;
		User::Free(g_idx); g_idx = NULL;
		User::Free(g_order); g_order = NULL;
		User::Free(g_batchNext); g_batchNext = NULL;
		User::Free(g_texHead); g_texHead = NULL;
		User::Free(g_texTail); g_texTail = NULL;
		ps1LogFmt("glr", "init failed: out of memory");
		return -1;
		}

	voutSymbianGlSize(&g_vpW, &g_vpH);
	if (g_vpW <= 0 || g_vpH <= 0)
		{ g_vpW = 320; g_vpH = 240; }

	glViewport(0, 0, g_vpW, g_vpH);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glDisable(GL_DITHER);
	glShadeModel(GL_SMOOTH);
	glClearColor(0, 0, 0, 1);
		// Ordinary modulation. The factor of two missing from the PS1 rule is
		// folded into the vertex colour itself (modColor) rather than GL_RGB_SCALE:
		// that is a texture unit extension and the MBX driver did not apply it.
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	g_ready = ETrue;
	GLint maxTex = 0, maxUnits = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
	glGetIntegerv(GL_MAX_TEXTURE_UNITS, &maxUnits);
	ps1LogFmt("glr", "init ok: viewport=%dx%d prims<=%d texenv_err=%u"
		" maxTex=%d units=%d vendor=%s renderer=%s", g_vpW, g_vpH,
		(int)KMaxPrims, (unsigned)glGetError(), (int)maxTex, (int)maxUnits,
		(const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER));
	return 0;
	}

// The window was re-attached - on this device a screen rotation does that - and
// the EGL context went with it. Texture names belong to the dead context, so they
// are dropped rather than deleted, and the next frame initialises again.
extern "C" void glrLoseContext(void)
	{
	g_ready = EFalse;
	g_nTex = 0;
	g_lastTex = -1;
	}

extern "C" void glrShutdown(void)
	{
	for (TInt i = 0; i < g_nTex; i++)
		glDeleteTextures(1, &g_tex[i].iTex);
	g_nTex = 0;
	User::Free(g_v); g_v = NULL;
	User::Free(g_decode); g_decode = NULL;
	g_ready = EFalse;
	}

// --- textures ----------------------------------------------------------------

// A VRAM pixel (1 semi-transparency bit + 5 B + 5 G + 5 R) into what GL takes as
// GL_UNSIGNED_SHORT_5_5_5_1. Alpha here means not PS1 semi-transparency but the
// PRESENCE of a texel. A hole is defined by the colour VALUE 0x0000, not by
// palette index 0: gpu_inner.h checks "if (!uSrc)" after the CLUT fetch, so
// opaque black (0x8000) at index 0 is drawn, while a zero entry at any other
// index is not.
static inline TUint16 texelToGl(TUint16 aV)
	{
	const TUint r = aV & 0x1F, g = (aV >> 5) & 0x1F, b = (aV >> 10) & 0x1F;
	return (TUint16)((r << 11) | (g << 6) | (b << 1) | (aV ? 1 : 0));
	}

// Unpacks a 256x256 page into g_decode. Page width in VRAM depends on the bit
// depth: 64 halfwords at 4 bits, 128 at 8, 256 at 16.
//
// Texture window masks are baked INTO THE PAGE ITSELF: a texel is taken at
// (u & maskU, v & maskV), so the window repeats across the whole 256x256. That
// makes GL with GL_REPEAT produce exactly what gpu_unai computes per pixel
// (gpu_inner.h masks u and v on every fetch), and removes the need to mask
// vertex coordinates - masking the primitive corners alone gave a wrong
// picture everywhere the primitive was wider than the window. It also removes
// reads past VRAM: a page in the lower half plus a window offset produced a row past 512.
static void decodePage(TUint32 aTba, TUint32 aCba, TInt aMode, TInt aMaskU, TInt aMaskV)
	{
	// A CLUT address from the GPU can point past VRAM; the palette read would run off the buffer.
	static const TUint16 KNoClut[256] = { 0 };
	const TUint32 cbase = (TUint32)aCba << 4;
	const TUint32 pal = (aMode == 32) ? 16u : ((aMode == 64) ? 256u : 0u);
	const TUint16 *clut = (cbase + pal <= 1024u * 512u) ? (g_vram + cbase) : KNoClut;
	TUint16 *out = g_decode;
	// How many texels per halfword: 4, 2 or 1.
	const TInt tpw = (aMode == 32) ? 4 : (aMode == 64 ? 2 : 1);
	for (TInt y = 0; y < KTexSide; y++)
		{
		const TUint32 row = aTba + (TUint32)(y & aMaskV) * 1024;
		for (TInt x = 0; x < KTexSide; x++)
			{
			const TInt sx = x & aMaskU;
			const TUint32 off = row + (TUint32)(sx / tpw);
			// A page can sit right at the VRAM edge; do not read past it.
			if (off >= 1024u * 512u)
				{ out[x] = 0; continue; }
			const TUint16 w = g_vram[off];
			TUint16 t;
			if (aMode == 32)
				t = clut[(w >> ((sx & 3) << 2)) & 0xF];
			else if (aMode == 64)
				t = clut[(w >> ((sx & 1) << 3)) & 0xFF];
			else
				t = w;
			out[x] = texelToGl(t);
			}
		out += KTexSide;
		}
	}

// Index of the wanted texture in the cache; unpacks and uploads it if needed.
// Returns -1 when there is nowhere to take it from (no VRAM, or cache full this frame).
static TInt texFor(TUint32 aTba, TUint32 aCba, TInt aMode, TInt aMaskU, TInt aMaskV)
	{
	if (!g_vram || !g_decode)
		return -1;
	// Compare the whole state, not a fold of it. Folding was a mistake here:
	// in "aMode << 27" with aMode 32 and 64 the one ran past 32 bits, so the bit
	// depth never reached the key at all, and a 4-bit page was substituted by an
	// 8-bit page at the same address.
	const TInt cba = (aMode == 96) ? 0 : (TInt)aCba;   // 16-bit does not read a palette
	// Adjacent primitives almost always share a texture, so try the last hit
	// before walking the whole table.
	if (g_lastTex >= 0 && g_lastTex < g_nTex)
		{
		const TGlrTex &t = g_tex[g_lastTex];
		if (t.iValid && t.iTba == (TInt)aTba && t.iCba == cba && t.iMode == aMode &&
		    t.iMaskU == aMaskU && t.iMaskV == aMaskV)
			{
			g_tex[g_lastTex].iFrame = g_frame;
			g_hit++;
			return g_lastTex;
			}
		}
	for (TInt i = 0; i < g_nTex; i++)
		if (g_tex[i].iValid && g_tex[i].iTba == (TInt)aTba && g_tex[i].iCba == cba &&
		    g_tex[i].iMode == aMode && g_tex[i].iMaskU == aMaskU &&
		    g_tex[i].iMaskV == aMaskV)
			{
			g_tex[i].iFrame = g_frame;
			g_hit++;
			g_lastTex = i;
			return i;
			}

	TInt slot = -1;
	if (g_nTex < KMaxTex)
		{
		slot = g_nTex++;
		glGenTextures(1, &g_tex[slot].iTex);
		}
	else
		{
			// Evict the least recently used - and NEVER the one this frame draws
			// from: writing into a texture in use serialises with the scene on MBX
			// and costs twice an ordinary upload (measured, section 14.5).
		TInt oldest = -1;
		for (TInt i = 0; i < g_nTex; i++)
			if (g_tex[i].iFrame != g_frame &&
			    (oldest < 0 || g_tex[i].iFrame < g_tex[oldest].iFrame))
				oldest = i;
		if (oldest < 0)
			{
			g_nofit++;
			return -1;
			}
		slot = oldest;
		g_evict++;
		}

	decodePage(aTba, aCba, aMode, aMaskU, aMaskV);
	glBindTexture(GL_TEXTURE_2D, g_tex[slot].iTex);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, KTexSide, KTexSide, 0,
		GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, g_decode);
	g_tex[slot].iTba = (TInt)aTba;
	g_tex[slot].iCba = cba;
	g_tex[slot].iMode = aMode;
	g_tex[slot].iMaskU = aMaskU;
	g_tex[slot].iMaskV = aMaskV;
	g_tex[slot].iFrame = g_frame;
	g_tex[slot].iValid = ETrue;
	// The rectangle this page really reads: the window, not the full 256x256.
	g_tex[slot].iX = (TInt)(aTba % 1024);
	g_tex[slot].iY = (TInt)(aTba / 1024);
	const TInt tpw = (aMode == 32) ? 4 : (aMode == 64 ? 2 : 1);
	g_tex[slot].iW = (aMaskU + 1 + tpw - 1) / tpw;
	g_tex[slot].iH = aMaskV + 1;
	const TUint32 cOff = aCba << 4;
	g_tex[slot].iCx = (TInt)(cOff % 1024);
	g_tex[slot].iCy = (TInt)(cOff / 1024);
	g_tex[slot].iCw = (aMode == 32) ? 16 : (aMode == 64 ? 256 : 0);
	g_miss++;
	g_lastTex = slot;
	return slot;
	}

static void ensureDummyTex(void)
	{
	if (g_dummySide <= 0 || g_dummyTex)
		return;
	if (g_dummySide > 0)
		{
		const TInt px = g_dummySide * g_dummySide;
		TUint16 *z = (TUint16 *)User::AllocZ(px * 2);
		if (z)
			{
			glGenTextures(1, &g_dummyTex);
			glBindTexture(GL_TEXTURE_2D, g_dummyTex);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_dummySide, g_dummySide, 0,
				GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, z);
			User::Free(z);
			ps1LogFmt("glr", "dummy tex side=%d id=%u err=0x%x",
				(int)g_dummySide, (unsigned)g_dummyTex, (unsigned)glGetError());
			}
		}
	g_dummySide = g_dummyTex ? g_dummySide : 0;
	}

// Probe: the background normally goes down only when a direct VRAM write landed
// in the drawn buffer, which is rare enough to be hard to catch. This forces it.
extern "C" void glrSetBgForce(int aOn)
	{
	g_bgForce = aOn ? ETrue : EFalse;
	}

extern "C" void glrSetDummyTex(int aSide)
	{
	g_dummySide = aSide;
	}

// gpu_unai hands over guest VRAM once, from renderer_init.
extern "C" void glrSetVram(const void *aVram)
	{
	g_vram = (const TUint16 *)aVram;
	}

// The game overwrote a VRAM rectangle. Drop only the pages it touched: at first
// the whole cache was dropped here, and that cost 4.5 ms per frame, because
// after every write the entire working set had to be unpacked again. The
// rectangle is in halfwords, like everything in VRAM.
extern "C" void glrInvalidateRect(int aX, int aY, int aW, int aH)
	{
	if (g_on)
		{
		// A transfer that runs off the edge wraps on the hardware, so the box it
		// really touched is the whole width or height, not the range as given.
		if (aX + aW > 1024) { aX = 0; aW = 1024; }
		if (aY + aH > 512)  { aY = 0; aH = 512; }
		if (g_wrX1 < g_wrX0) { g_wrX0 = aX; g_wrY0 = aY; g_wrX1 = aX + aW; g_wrY1 = aY + aH; }
		else
			{
			if (aX < g_wrX0) g_wrX0 = aX;
			if (aY < g_wrY0) g_wrY0 = aY;
			if (aX + aW > g_wrX1) g_wrX1 = aX + aW;
			if (aY + aH > g_wrY1) g_wrY1 = aY + aH;
			}
		}
	for (TInt i = 0; i < g_nTex; i++)
		{
		if (!g_tex[i].iValid)
			continue;
		const TBool hitPage =
			!(aX >= g_tex[i].iX + g_tex[i].iW || aX + aW <= g_tex[i].iX) &&
			!(aY >= g_tex[i].iY + g_tex[i].iH || aY + aH <= g_tex[i].iY);
			// An 8-bit palette is 256 halfwords and may not fit in one VRAM row.
			// When it wraps to the next one, the X check is dropped: a spurious
			// drop costs one unpack, a missed one puts a foreign palette on screen.
		const TInt cWrap = (g_tex[i].iCx + g_tex[i].iCw > 1024) ? 1 : 0;
		const TBool hitClut = g_tex[i].iCw > 0 &&
			!(aY >= g_tex[i].iCy + 1 + cWrap || aY + aH <= g_tex[i].iCy) &&
			(cWrap ||
			 !(aX >= g_tex[i].iCx + g_tex[i].iCw || aX + aW <= g_tex[i].iCx));
		if (hitPage || hitClut)
			g_tex[i].iValid = EFalse;
		}
	}

// The palette has to be re-read too, and we do not keep where it lives in the
// entry - so a state change (register reload) drops everything.
extern "C" void glrInvalidate(void)
	{
	for (TInt i = 0; i < g_nTex; i++)
		g_tex[i].iValid = EFalse;
	}

// --- vertex assembly ---------------------------------------------------------

// Opens a new batch if the state changed. Submission order is preserved: only
// ADJACENT primitives are merged, because on PS1 a later primitive overlaps an
// earlier one and they cannot be reordered.
static void batchFor(TInt aBlend, TInt aTex)
	{
	if (g_nBatch > 0 && g_batch[g_nBatch - 1].iBlend == aBlend
	                 && g_batch[g_nBatch - 1].iTex == aTex
	                 && g_batch[g_nBatch - 1].iClip == g_curClip)
		{
		g_batchFull = EFalse;
		return;
		}
	if (g_nBatch > 0)
		{
		const TGlrBatch &b = g_batch[g_nBatch - 1];
		if (b.iTex != aTex)          g_brkTex++;
		else if (b.iBlend != aBlend) g_brkBlend++;
		else                         g_brkClip++;
		}
	if (g_nBatch >= KMaxBatch)
		{
		// The table is full and this primitive needs a state of its own. Letting it
		// through would append it to the LAST batch and draw it with a foreign
		// texture - that is how half a frame once came out flooded with grass.
		g_batchOver++;
		g_batchFull = ETrue;
		return;
		}
	g_batch[g_nBatch].iFirst = g_nVerts;
	g_batch[g_nBatch].iCount = 0;
	g_batch[g_nBatch].iBlend = aBlend;
	g_batch[g_nBatch].iTex   = aTex;
	g_batch[g_nBatch].iClip  = g_curClip;
	g_nBatch++;
	g_batchFull = EFalse;
	}

static inline void emitVertex(TInt aX, TInt aY, TUint32 aBgr, TInt aAlpha,
	TInt aU, TInt aV)
	{
	TGlrVert *v = g_v + g_nVerts;
	v->x = (GLshort)aX;
	v->y = (GLshort)aY;
	v->z = g_primZ;
	v->pad = 0;
	v->u = (GLshort)aU;
	v->v = (GLshort)aV;
	v->r = (GLubyte)(aBgr & 0xFF);
	v->g = (GLubyte)((aBgr >> 8) & 0xFF);
	v->b = (GLubyte)((aBgr >> 16) & 0xFF);
	v->a = (GLubyte)aAlpha;
	g_nVerts++;
	g_batch[g_nBatch - 1].iCount++;
	if (g_curClip >= 0)
		g_clipVerts[g_curClip]++;
	}

// A triangle from three vertices; colours and texture coordinates are separate,
// because Gouraud has different colours and a textured primitive different texel coordinates.
static void emitTri(const TInt *aX, const TInt *aY, const TUint32 *aCol,
	TInt aAlpha, const TInt *aU, const TInt *aV)
	{
	if (g_batchFull || g_nBatch == 0) { g_dropPrims++; return; }
	if (g_nVerts + 3 > KMaxVerts) { g_over++; return; }
	emitVertex(aX[0], aY[0], aCol[0], aAlpha, aU[0], aV[0]);
	emitVertex(aX[1], aY[1], aCol[1], aAlpha, aU[1], aV[1]);
	emitVertex(aX[2], aY[2], aCol[2], aAlpha, aU[2], aV[2]);
	}

// Sign of a PS1 coordinate: eleven bits, the high ones are sign extension.
// The "texwhite" probe: a textured primitive is drawn with a white vertex
// colour, so the texture itself reaches the screen unmodulated. It separates a
// colour fault from an unpack or upload fault - otherwise they are indistinguishable.
static TBool g_texWhite;
// Bit mask of the classes NOT to draw: 1 - polygons, 2 - rectangles,
// 4 - fills. A probe for finding whatever flooded the frame.
static TInt g_skipMask;

extern "C" void glrSetTexWhite(int aOn)
	{
	g_texWhite = aOn ? ETrue : EFalse;
	}

extern "C" void glrSetSkip(int aMask)
	{
	g_skipMask = aMask;
	}

// PS1 modulates as (tex*col)/128, so 0x80 is the neutral colour; GL multiplies
// by col/255, which is short by exactly a factor of two. Setting it through
// GL_RGB_SCALE did not work: that is a texture unit extension and the MBX driver
// silently ignores it - the whole textured scene came out twice as dark as the
// software one. So the vertex colour itself is doubled, with saturation, as on PS1.
static inline TUint32 modColor(TUint32 aC)
	{
	TUint r = (aC & 0xFF) << 1, g = ((aC >> 8) & 0xFF) << 1, b = ((aC >> 16) & 0xFF) << 1;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return r | (g << 8) | (b << 16);
	}
static inline TInt expandSign(TUint32 aV)
	{
	return ((TInt)(aV << 21)) >> 21;
	}

// --- command intake ----------------------------------------------------------

// One primitive. Called from do_cmd_list after gpu_unai has already drawn it
// into software VRAM, so all state is already current.
extern "C" void glrPrim(unsigned aCmd, const unsigned *aPacket, const int *aState)
	{
	if (!g_on || !g_ready)
		return;
	// Later primitives must win, and the projection makes a smaller z the nearer
	// one, so the depth counts down as the frame goes on.
	g_primZ = (GLshort)((g_primIdx < KMaxPrims) ? (KMaxPrims - g_primIdx) : 1);
	g_primIdx++;
	// Which back buffer this primitive belongs to. The area alone is not enough:
	// Gran Turismo keeps both buffers under one area 0,0..640,480 and tells them
	// apart only by the draw offset, so group by the pair.
	g_curClip = -1;
	for (TInt i = 0; i < g_nClips; i++)
		if (g_clips[i][0] == aState[EStClipX0] && g_clips[i][1] == aState[EStClipY0] &&
		    g_clips[i][2] == aState[EStClipX1] && g_clips[i][3] == aState[EStClipY1] &&
		    g_ofs[i][0] == aState[EStOfsX] && g_ofs[i][1] == aState[EStOfsY])
			{ g_curClip = i; break; }
	if (g_curClip < 0 && g_nClips < KMaxClips)
		{
		g_curClip = g_nClips++;
		g_clips[g_curClip][0] = aState[EStClipX0];
		g_clips[g_curClip][1] = aState[EStClipY0];
		g_clips[g_curClip][2] = aState[EStClipX1];
		g_clips[g_curClip][3] = aState[EStClipY1];
		g_ofs[g_curClip][0] = aState[EStOfsX];
		g_ofs[g_curClip][1] = aState[EStOfsY];
		g_clipVerts[g_curClip] = 0;
		}
	if (g_curClip < 0)
		g_clipFull++;

	const TInt ofsX = aState[EStOfsX], ofsY = aState[EStOfsY];
	// Semi-transparency is carried by bit 1 of the command code; the mode is in the state.
	const TInt blend = (aCmd & 2) ? aState[EStBlendMode] : -1;
	// Mode 3 (B+F/4) is expressible through ordinary addition if the colour
	// itself is divided by four; alpha serves as that divisor here.
	const TInt alpha = (blend == 0) ? 128 : (blend == 3 ? 64 : 255);

	if (aCmd >= 0x20 && aCmd <= 0x3F)
		{
		if (g_skipMask & 1)
			return;
		const TBool tex = (aCmd & 4) != 0;
		const TBool gouraud = (aCmd & 0x10) != 0;
		const TBool quad = (aCmd & 8) != 0;
		const TInt stride = 1 + (tex ? 1 : 0) + (gouraud ? 1 : 0);
		const TInt nv = quad ? 4 : 3;
		TInt x[4], y[4], u[4], v[4];
		TUint32 col[4];
		// A "raw" texture (bit 0) carries no colour in the packet - take the
		// neutral one for modulation, otherwise there would be black patches.
		const TUint32 flat = (tex && (aCmd & 1)) ? 0x808080
		                                        : (aPacket[0] & 0xFFFFFF);
		for (TInt i = 0; i < nv; i++)
			{
			const TUint32 c = aPacket[1 + i * stride];
			x[i] = expandSign(c) + ofsX;
			y[i] = expandSign(c >> 16) + ofsY;
			col[i] = gouraud ? (aPacket[i * stride] & 0xFFFFFF) : flat;
			if (tex)
				col[i] = g_texWhite ? 0xFFFFFF : modColor(col[i]);
			if (tex)
				{
				const TUint32 t = aPacket[2 + i * stride];
					// Window masks are already baked into the unpacked page, do not
					// touch them here: masking the corners alone broke anything wider than the window.
				u[i] = (TInt)(t & 0xFF);
				v[i] = (TInt)((t >> 8) & 0xFF);
				}
			else
				{ u[i] = 0; v[i] = 0; }
			}
		const TInt ti = tex ? texFor((TUint32)aState[EStTba], (TUint32)aState[EStCba],
			aState[EStTextMode], aState[EStTwU], aState[EStTwV]) : -1;
		if (ti < 0)
			{
			for (TInt i = 0; i < nv; i++)
				{ u[i] = 0; v[i] = 0; }
			}
		else if (tex)
			{
			for (TInt i = 0; i < nv; i++)
				{
				if (u[i] < g_tex[ti].iUseU0) g_tex[ti].iUseU0 = u[i];
				if (u[i] > g_tex[ti].iUseU1) g_tex[ti].iUseU1 = u[i];
				if (v[i] < g_tex[ti].iUseV0) g_tex[ti].iUseV0 = v[i];
				if (v[i] > g_tex[ti].iUseV1) g_tex[ti].iUseV1 = v[i];
				}
			}
		batchFor(blend, ti);
		TInt tx[3], ty[3], tu[3], tv[3]; TUint32 tc[3];
		for (TInt k = 0; k < 3; k++)
			{ tx[k]=x[k]; ty[k]=y[k]; tc[k]=col[k]; tu[k]=u[k]; tv[k]=v[k]; }
		emitTri(tx, ty, tc, alpha, tu, tv);
		if (quad)
			{
			for (TInt k = 0; k < 3; k++)
				{
				const TInt s = k + 1;
				tx[k]=x[s]; ty[k]=y[s]; tc[k]=col[s]; tu[k]=u[s]; tv[k]=v[s];
				}
			emitTri(tx, ty, tc, alpha, tu, tv);
			}
		return;
		}

	if (aCmd >= 0x60 && aCmd <= 0x7F)
		{
		if (g_skipMask & 2)
			return;
		// Rectangle. The size is set by bits 3-4 of the code: 0 - in the packet,
		// otherwise a fixed side of 1, 8 or 16.
		const TBool tex = (aCmd & 4) != 0;
		const TUint32 xy = aPacket[1];
		// For a rectangle gpu_unai sign-extends FROM THE SUM with the offset
		// (gpu_raster_sprite.h), not from the coordinate itself.
		const TInt x0 = expandSign((xy & 0xFFFF) + (TUint32)ofsX);
		const TInt y0 = expandSign((xy >> 16) + (TUint32)ofsY);
		TInt w = 0, h = 0;
		switch (aCmd & 0x18)
			{
			case 0x08: w = 1;  h = 1;  break;
			case 0x10: w = 8;  h = 8;  break;
			case 0x18: w = 16; h = 16; break;
			default:
				{
				const TUint32 wh = aPacket[tex ? 3 : 2];
				w = (TInt)(wh & 0x3FF);
				h = (TInt)((wh >> 16) & 0x1FF);
				} break;
			}
		if (w <= 0 || h <= 0)
			return;
		TUint32 c = (tex && (aCmd & 1)) ? 0x808080 : (aPacket[0] & 0xFFFFFF);
		if (tex)
			c = g_texWhite ? 0xFFFFFF : modColor(c);
			// For a rectangle the texture coordinate is given only for the top
			// left corner, the rest follows from its size.
		TInt u0 = 0, v0 = 0, uw = 0, vh = 0;
		if (tex)
			{
			u0 = (TInt)(aPacket[2] & 0xFF);
			v0 = (TInt)((aPacket[2] >> 8) & 0xFF);
			uw = w;
			vh = h;
			g_spriteAll++;
			if (u0 + w > 256 || v0 + h > 256)
				g_spriteWrap++;
			}
		const TInt ti = tex ? texFor((TUint32)aState[EStTba], (TUint32)aState[EStCba],
			aState[EStTextMode], aState[EStTwU], aState[EStTwV]) : -1;
		if (ti < 0)
			{
			u0 = 0; v0 = 0; uw = 0; vh = 0;
			}
		else if (tex)
			{
			TInt u1 = u0 + w - 1;
			TInt v1 = v0 + h - 1;
			if (u1 > 255) u1 = 255;
			if (v1 > 255) v1 = 255;
			if (u0 < g_tex[ti].iUseU0) g_tex[ti].iUseU0 = u0;
			if (u1 > g_tex[ti].iUseU1) g_tex[ti].iUseU1 = u1;
			if (v0 < g_tex[ti].iUseV0) g_tex[ti].iUseV0 = v0;
			if (v1 > g_tex[ti].iUseV1) g_tex[ti].iUseV1 = v1;
			}
		batchFor(blend, ti);
		TInt tx[3], ty[3], tu[3], tv[3]; TUint32 tc[3];
		tc[0] = tc[1] = tc[2] = c;
		tx[0]=x0;   ty[0]=y0;   tu[0]=u0;    tv[0]=v0;
		tx[1]=x0+w; ty[1]=y0;   tu[1]=u0+uw; tv[1]=v0;
		tx[2]=x0;   ty[2]=y0+h; tu[2]=u0;    tv[2]=v0+vh;
		emitTri(tx, ty, tc, alpha, tu, tv);
		tx[0]=x0+w; ty[0]=y0;   tu[0]=u0+uw; tv[0]=v0;
		tx[1]=x0+w; ty[1]=y0+h; tu[1]=u0+uw; tv[1]=v0+vh;
		tx[2]=x0;   ty[2]=y0+h; tu[2]=u0;    tv[2]=v0+vh;
		emitTri(tx, ty, tc, alpha, tu, tv);
		return;
		}

	if (aCmd == 0x02)
		{
		if (g_skipMask & 4)
			return;
		// Fill. It alone ignores both the offset and clipping, and is always
		// opaque - it is not a primitive but a write into VRAM.
		const TInt x0 = (TInt)(aPacket[1] & 0x3F0);
		const TInt y0 = (TInt)((aPacket[1] >> 16) & 0x1FF);
		const TInt w = (TInt)(((aPacket[2] & 0x3FF) + 15) & ~15);
		const TInt h = (TInt)((aPacket[2] >> 16) & 0x1FF);
		if (w <= 0 || h <= 0)
			return;
		const TUint32 c = aPacket[0] & 0xFFFFFF;
		batchFor(-1, -1);
		TInt tx[3], ty[3]; TUint32 tc[3];
		const TInt z[3] = { 0, 0, 0 };
		tc[0] = tc[1] = tc[2] = c;
		tx[0]=x0;   ty[0]=y0;   tx[1]=x0+w; ty[1]=y0;   tx[2]=x0; ty[2]=y0+h;
		emitTri(tx, ty, tc, 255, z, z);
		tx[0]=x0+w; ty[0]=y0;   tx[1]=x0+w; ty[1]=y0+h; tx[2]=x0; ty[2]=y0+h;
		emitTri(tx, ty, tc, 255, z, z);
		return;
		}
	// Lines (0x40-0x5F) are not drawn yet: there are 6.5 per frame (section 13).
	}

// --- frame -------------------------------------------------------------------

extern "C" void glrBeginFrame(void)
	{
	if (!g_on)
		return;
	if (!g_ready && glrInit() != 0)
		return;
	g_nVerts = 0;
	g_nBatch = 0;
	g_nClips = 0;
	g_curClip = -1;
	g_over = 0;
	g_frame++;
	g_wrX0 = 0; g_wrY0 = 0; g_wrX1 = -1; g_wrY1 = -1;
	g_batchFull = EFalse;
	g_primIdx = 0;
	g_primZ = (GLshort)KMaxPrims;
	g_f0Nofit = g_nofit;         g_f0BatchOver = g_batchOver;
	g_f0DropPrims = g_dropPrims; g_f0Over = g_over;
	g_f0ClipFull = g_clipFull;   g_f0DropVerts = g_dropVerts;
	g_f0Evict = g_evict;         g_f0Miss = g_miss;
	for (TInt i = 0; i < KMaxTex; i++)
		{
		g_tex[i].iUseU0 = 0x7FFFFFFF;
		g_tex[i].iUseV0 = 0x7FFFFFFF;
		g_tex[i].iUseU1 = -1;
		g_tex[i].iUseV1 = -1;
		}
	}

// PS1 semi-transparency modes in GL terms. Mode 2 (B-F) has no equivalent;
// measured, the game uses it 35 times across 6099 frames (section 13), so
// darkening is taken as the nearest in meaning and said so here, not hidden.
static void applyBlend(TInt aMode)
	{
	if (aMode < 0)
		{
		glDisable(GL_BLEND);
		return;
		}
	glEnable(GL_BLEND);
	switch (aMode)
		{
		case 0:  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
		case 1:  glBlendFunc(GL_ONE, GL_ONE); break;
		case 2:  glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); break;
		default: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;   // B + F/4 through alpha
		}
	}

// A snapshot was asked for on a frame this rasteriser does not show. The request
// waits for a frame it does draw, and the log says which one that turned out to
// be, so the file is never mistaken for the frame that was asked for.
static void vramPathShot(void)
	{
	if (!g_shotWanted || g_shotDeferred)
		return;
	g_shotDeferred = ETrue;
	ps1LogFmt("glr", "shot: %s deferred, that frame came from VRAM", g_shotPath);
	}

extern "C" int glrEndFrame(void)
	{
	if (!g_on || !g_ready)
		return 0;
	// Nothing was drawn. That happens for two unrelated reasons, and telling them
	// apart is what the picture depends on. A game running at half the video rate
	// has no new frame at all, and the screen must keep the one it shows: the
	// surface is double buffered, so swapping would put up the OTHER buffer, two
	// frames old - measured as the race alternating with a black frame at every
	// second vblank. But a video or a picture written straight into VRAM is new
	// content that carries no primitives, and that one has to be shown.
	TInt sx = 0, sy = 0, dsw = 0, dsh = 0;
	voutSymbianDbgState(NULL, NULL, NULL, &sx, &sy, &dsw, &dsh, NULL, NULL);
	const TBool moved = (sx != g_lastSrcX || sy != g_lastSrcY);
	g_lastSrcX = sx;
	g_lastSrcY = sy;
	if (g_nBatch == 0 || g_nClips == 0)
		{
		const TBool wrote = (g_wrX1 >= g_wrX0 && dsw > 0 && dsh > 0 &&
		    !(g_wrX0 >= sx + dsw || g_wrX1 <= sx ||
		      g_wrY0 >= sy + dsh || g_wrY1 <= sy));
		if (!wrote && !moved)
			{
			g_holdFrames++;
			return 1;
			}
		g_vramFrames++;
		vramPathShot();
		return 0;
		}
	// The back buffer is the group with the most vertices.
	g_dropGroups += g_nClips - 1;
	TInt best = 0;
	for (TInt i = 1; i < g_nClips; i++)
		if (g_clipVerts[i] > g_clipVerts[best])
			best = i;
	// The frame rectangle is NOT the draw area. Gran Turismo keeps the area
	// 0,0..640,480 for both 320x240 buffers, so a projection built from it put
	// the scene in a quarter of the screen and the second buffer in the
// neighbouring quarter. The right rectangle is this group draw offset plus the GPU display size.
	TInt dw = 0, dh = 0;
	voutSymbianDbgState(NULL, NULL, NULL, NULL, NULL, &dw, &dh, NULL, NULL);
	if (dw <= 0 || dh <= 0)
		{ dw = 320; dh = 240; }
	g_clip[0] = g_ofs[best][0];
	g_clip[1] = g_ofs[best][1];
	g_clip[2] = g_clip[0] + dw;
	g_clip[3] = g_clip[1] + dh;

	// The buffer this frame was drawn into was also written directly - a video
	// frame, a decoded picture, a VRAM to VRAM copy. Those bytes exist only in
	// guest VRAM and carry no primitives, so the scene goes on TOP of them
	// instead of replacing them.
	const TBool bg = g_bgForce || (g_wrX1 >= g_wrX0 &&
	    !(g_wrX0 >= g_clip[2] || g_wrX1 <= g_clip[0] ||
	      g_wrY0 >= g_clip[3] || g_wrY1 <= g_clip[1]));
	if (bg)
		g_mixedFrames++;

	// The window on this device rotates by itself, and the field of view was set
	// once at initialisation. After a rotation it stayed at the old size - the
	// very fault already seen in Hexen 2.
	TInt vw = 0, vh = 0;
	voutSymbianGlSize(&vw, &vh);
	if (vw > 0 && vh > 0 && (vw != g_vpW || vh != g_vpH))
		{
		g_vpW = vw; g_vpH = vh;
		glViewport(0, 0, g_vpW, g_vpH);
		ps1LogFmt("glr", "viewport changed: %dx%d", (int)g_vpW, (int)g_vpH);
		}

	const unsigned int tSetup0 = ndrc_symbian_fastcount();
	// The PS1 back buffer covers the whole window. Y axis downwards, as in VRAM.
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof((GLfloat)g_clip[0], (GLfloat)g_clip[2],
	         (GLfloat)g_clip[3], (GLfloat)g_clip[1], 0, -(GLfloat)KMaxPrims);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// Array and texture state is set EVERY TIME, not once at initialisation.
	// The display path in vout_symbian.cpp enables GL_TEXTURE_COORD_ARRAY and
	// leaves glTexCoordPointer aimed at an array living in its stack frame.
	glDisable(GL_TEXTURE_2D);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	// Palette index 0 on PS1 is a hole, not a colour. In the unpacked texture it
	// got alpha 0, so it is cut by an alpha test rather than by blending:
	// blending would have to apply where the primitive is opaque too.
	//
	// The threshold must sit BELOW the smallest blend factor. GL_MODULATE
	// multiplies alpha as well, and our vertex alpha carries that factor: 128 for
	// mode B/2+F/2 and 64 for B+F/4. At a threshold of 0.5 mode B+F/4 would be
	// cut entirely and B/2+F/2 would pass by one quantisation step. What has to
	// be cut is exactly zero, so the threshold is close to zero.
	glAlphaFunc(GL_GREATER, 0.05f);
	g_depth = (voutSymbianGlDepthBits() >= 16);
	if (g_depth)
		{
		glEnable(GL_DEPTH_TEST);
		// LEQUAL, not LESS. The projection puts primitive zero at depth 1.0, which is
		// exactly the cleared value, so LESS would throw away the first primitive of
		// every frame - usually the one that covers the whole screen. It also keeps
		// drawing in submission order if a frame ever runs past KMaxPrims and the
		// depths stop being distinct.
		glDepthFunc(GL_LEQUAL);
		glDepthMask(GL_TRUE);
		}
	else
		glDisable(GL_DEPTH_TEST);
	const unsigned int tSetup1 = ndrc_symbian_fastcount();
	g_tSetup += (unsigned int)(tSetup1 - tSetup0);

	const unsigned int tClear0 = ndrc_symbian_fastcount();
	glClear(g_depth ? (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
	                : GL_COLOR_BUFFER_BIT);
	const unsigned int tClear1 = ndrc_symbian_fastcount();
	g_tClear += (unsigned int)(tClear1 - tClear0);

	if (bg)
		{
		// The background quad has no colour array, and ours is enabled here with a
		// pointer that is only set further down - the driver followed it to NULL and
		// took the process with it.
		glDisableClientState(GL_COLOR_ARRAY);
		if (voutSymbianGlVramQuad())
			{
			// The background is drawn in window pixels and leaves that projection
			// behind, so ours has to be set again before the scene.
			glMatrixMode(GL_PROJECTION);
			glLoadIdentity();
			glOrthof((GLfloat)g_clip[0], (GLfloat)g_clip[2],
			         (GLfloat)g_clip[3], (GLfloat)g_clip[1], -1, 1);
			glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();
			if (g_depth)
				{ glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); }
			g_bgFrames++;
			}
		glEnableClientState(GL_COLOR_ARRAY);
		}

	const unsigned int tSetup2 = ndrc_symbian_fastcount();
	// Texture coords are raw PS1 texels; the divide by 256 is done by the texture
	// matrix. It must be set here and cleared below: the full-screen present path
	// in vout_symbian.cpp shares this context and passes normalised coords.
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glScalef(1.0f / 256.0f, 1.0f / 256.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glVertexPointer(3, GL_SHORT, sizeof(TGlrVert), &g_v[0].x);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(TGlrVert), &g_v[0].r);
	glTexCoordPointer(2, GL_SHORT, sizeof(TGlrVert), &g_v[0].u);
	const unsigned int tSetup3 = ndrc_symbian_fastcount();
	g_tSetup += (unsigned int)(tSetup3 - tSetup2);

		// Probes for splitting the cost across vertex arrays: 32 - no colour,
		// 64 - no texture. The picture is wrong meanwhile, that is the point of a probe.
	if (g_skipMask & 32)
		glDisableClientState(GL_COLOR_ARRAY);
	if (g_skipMask & 64)
		{
		glDisable(GL_TEXTURE_2D);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
		// Probes for separating the two causes of draw cost. Bit 8 keeps the same
		// geometry but clips it to one pixel: if the time did not drop, we are paying
		// for vertex processing, not for fill. Bit 16 does not draw at all - what
		// remains is the bare cost of the buffer swap.
	if (g_skipMask & 8)
		{
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, 0, 1, 1);
		}
	const unsigned int tDraw0 = ndrc_symbian_fastcount();
	ensureDummyTex();
	// The order the batches go out in. Measured on a lap of Gran Turismo: 404
	// batches per frame, and 99.6 per cent of the breaks between them were texture
	// changes, at 9.5 to 14 us for a draw call and as much again for a bind. With a
	// depth buffer the overlap rule no longer needs submission order, so opaque
	// batches are grouped by texture and merged into one indexed call per texture.
	// Translucent ones keep their order, come last and write no depth: they must
	// blend with what is already there, and only 2914 breaks in 1.5 million were
	// theirs, so nothing is lost by leaving them alone.
	TInt nOrder = 0;
	if (g_depth)
		{
		for (TInt t = 0; t <= g_nTex; t++)
			{ g_texHead[t] = -1; g_texTail[t] = -1; }
		for (TInt b = 0; b < g_nBatch; b++)
			{
			if (g_batch[b].iBlend >= 0)
				continue;
			const TInt t = (g_batch[b].iTex < 0) ? g_nTex : g_batch[b].iTex;
			g_batchNext[b] = -1;
			if (g_texTail[t] < 0) g_texHead[t] = b;
			else                  g_batchNext[g_texTail[t]] = b;
			g_texTail[t] = b;
			}
		for (TInt t = 0; t <= g_nTex; t++)
			for (TInt b = g_texHead[t]; b >= 0; b = g_batchNext[b])
				g_order[nOrder++] = b;
		for (TInt b = 0; b < g_nBatch; b++)
			if (g_batch[b].iBlend >= 0)
				g_order[nOrder++] = b;
		}
	else
		for (TInt b = 0; b < g_nBatch; b++)
			g_order[nOrder++] = b;

	TInt curTex = -2;
	TInt curBlend = -2;
	TInt idxN = 0;
	TBool depthWrite = ETrue;
	for (TInt o = 0; (g_skipMask & 16) == 0 && o < nOrder; o++)
		{
		const TInt b = g_order[o];
			// Primitives of other areas are skipped: in this projection they
			// would land at random, and there is nowhere to show them anyway.
		if (g_batch[b].iCount <= 0 || g_batch[b].iClip != best)
			{
			g_dropVerts += g_batch[b].iCount;
			continue;
			}
		const TInt bt = g_batch[b].iTex, bb = g_batch[b].iBlend;
		if (bt != curTex || bb != curBlend || idxN + g_batch[b].iCount > KMaxVerts)
			{
			if (idxN > 0)
				{
				glDrawElements(GL_TRIANGLES, idxN, GL_UNSIGNED_SHORT, g_idx);
				g_draws++;
				idxN = 0;
				}
			if (bt != curTex)
				{
				curTex = bt;
				if (curTex >= 0 && !(g_skipMask & 64))
					{
					glEnable(GL_TEXTURE_2D);
					glEnable(GL_ALPHA_TEST);
					if (g_dummyTex)
						glBindTexture(GL_TEXTURE_2D, g_dummyTex);
					glBindTexture(GL_TEXTURE_2D, g_tex[curTex].iTex);
					}
				else
					{
					glDisable(GL_TEXTURE_2D);
					glDisable(GL_ALPHA_TEST);
					}
				}
			if (bb != curBlend)
				{
				curBlend = bb;
				applyBlend(curBlend);
				}
			if (g_depth && depthWrite && bb >= 0)
				{ glDepthMask(GL_FALSE); depthWrite = EFalse; }
			}
		const TInt first = g_batch[b].iFirst;
		for (TInt v = 0; v < g_batch[b].iCount; v++)
			g_idx[idxN++] = (unsigned short)(first + v);
		}
	if (idxN > 0)
		{
		glDrawElements(GL_TRIANGLES, idxN, GL_UNSIGNED_SHORT, g_idx);
		g_draws++;
		}
	const unsigned int tDraw1 = ndrc_symbian_fastcount();
	g_tDraw += (unsigned int)(tDraw1 - tDraw0);

	// Only frames that actually lost something, and only the first forty of them.
	// A limit that quietly drops work reads as a rendering fault, so it says so,
	// together with how close that frame ran to each limit.
		{
		const TInt lNofit = g_nofit - g_f0Nofit;
		const TInt lBatch = g_batchOver - g_f0BatchOver;
		const TInt lPrim  = g_dropPrims - g_f0DropPrims;
		const TInt lVert  = g_over - g_f0Over;
		const TInt lClip  = g_clipFull - g_f0ClipFull;
		const TInt lDrop  = g_dropVerts - g_f0DropVerts;
		if (g_lossLogged < 40 &&
		    (lNofit || lBatch || lPrim || lVert || lClip || lDrop))
			{
			g_lossLogged++;
			ps1LogFmt("glrloss", "f=%d nofit=%d batchover=%d dropprim=%d vertover=%d"
				" clipfull=%d dropverts=%d tex=%d verts=%d batches=%d clips=%d"
				" upload=%d evict=%d",
				(int)g_frame, (int)lNofit, (int)lBatch, (int)lPrim, (int)lVert,
				(int)lClip, (int)lDrop, (int)g_nTex, (int)g_nVerts, (int)g_nBatch,
				(int)g_nClips, (int)(g_miss - g_f0Miss), (int)(g_evict - g_f0Evict));
			}
		}
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	// The VRAM display path shares this context and sets its projection once,
	// at context creation. Now that a frame can fall back to it at any moment,
	// the projection has to be handed back the way it was found.
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0, (GLfloat)g_vpW, (GLfloat)g_vpH, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	if (g_skipMask & 8)
		glDisable(GL_SCISSOR_TEST);

	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
		// Restore the state the vout display path expects, so flipping the flag
		// mid-session does not leave it with the arrays disabled.
	glDisableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnable(GL_TEXTURE_2D);
		// Together with the snapshot - a description of every texture this frame
		// drew from. Given those and a VRAM dump of the same frame, the same page
		// can be unpacked on the host and compared: otherwise an unpack fault
		// cannot be told from a coordinate fault or from the texture being like that.
	if (g_shotWanted)
		{
		for (TInt i = 0; i < g_nTex; i++)
			if (g_tex[i].iValid && g_tex[i].iFrame == g_frame)
				ps1LogFmt("glrtex", "slot=%d tba=%d cba=%d mode=%d mu=%d mv=%d"
					" x=%d y=%d w=%d h=%d",
					(int)i, g_tex[i].iTba, g_tex[i].iCba, g_tex[i].iMode,
					g_tex[i].iMaskU, g_tex[i].iMaskV,
					g_tex[i].iX, g_tex[i].iY, g_tex[i].iW, g_tex[i].iH);
		for (TInt b = 0; b < g_nBatch && b < 12; b++)
			ps1LogFmt("glrbatch", "b=%d n=%d tex=%d blend=%d clip=%d",
				(int)b, g_batch[b].iCount, (int)g_batch[b].iTex,
				g_batch[b].iBlend, g_batch[b].iClip);
		}
	for (TInt i = 0; i < KMaxTex; i++)
		{
		if (g_tex[i].iUseU1 >= g_tex[i].iUseU0 && g_tex[i].iUseV1 >= g_tex[i].iUseV0)
			{
			const unsigned long long tw = (unsigned long long)(g_tex[i].iUseU1 - g_tex[i].iUseU0 + 1);
			const unsigned long long th = (unsigned long long)(g_tex[i].iUseV1 - g_tex[i].iUseV0 + 1);
			g_useTexels += tw * th;
			g_fullTexels += 65536ULL;
			g_useSlots++;
			}
		}
	g_batchSum += g_nBatch;
	g_frameSum++;
	if (g_shotWanted)
		writeShot();

	const unsigned int tSwap0 = ndrc_symbian_fastcount();
	voutSymbianGlSwap();
	const unsigned int tSwap1 = ndrc_symbian_fastcount();
	g_tSwap += (unsigned int)(tSwap1 - tSwap0);

		// Two records: the first frame (did it get here at all) and the first real
		// scene (are the area and contents right). No more - the log is not per pixel.
	if (g_logged < 4 && (g_logged == 0 || g_nVerts > 300))
		{
		// Where the primitives actually landed. Without it the frame rectangle
		// alone cannot say whether the scene is drawn small inside a large
		// projection or the projection is simply wrong.
		TInt px0 = 32767, py0 = 32767, px1 = -32768, py1 = -32768;
		for (TInt i = 0; i < g_nVerts; i++)
			{
			if (g_v[i].x < px0) px0 = g_v[i].x;
			if (g_v[i].x > px1) px1 = g_v[i].x;
			if (g_v[i].y < py0) py0 = g_v[i].y;
			if (g_v[i].y > py1) py1 = g_v[i].y;
			}
		ps1LogFmt("glr", "frame%d: frame=%d,%d..%d,%d verts=%d/%d batches=%d clips=%d"
			" tex=%d hit=%d miss=%d evict=%d nofit=%d bover=%d over=%d vp=%dx%d"
			" prim=%d,%d..%d,%d vramwr=%d,%d..%d,%d err=%u",
			(int)g_logged, g_clip[0], g_clip[1], g_clip[2], g_clip[3],
			(int)g_clipVerts[best], (int)g_nVerts, (int)g_nBatch, (int)g_nClips,
			(int)g_nTex, (int)g_hit, (int)g_miss, (int)g_evict, (int)g_nofit, (int)g_batchOver,
			(int)g_over, (int)g_vpW, (int)g_vpH,
			(int)px0, (int)py0, (int)px1, (int)py1,
			(int)g_wrX0, (int)g_wrY0, (int)g_wrX1, (int)g_wrY1,
			(unsigned)glGetError());
		g_logged++;
		}
	return 1;
	}

// How much the frame cost by composition: for the report at the end of a run.
extern "C" void glrStats(int *aVerts, int *aBatches, int *aOver,
	int *aHit, int *aMiss, int *aEvict)
	{
	if (aVerts)   *aVerts = g_nVerts;
	if (aBatches) *aBatches = g_nBatch;
	if (aOver)    *aOver = g_over;
	if (aHit)     *aHit = g_hit;
	if (aMiss)    *aMiss = g_miss;
	if (aEvict)   *aEvict = g_evict;
	if (aOver)    *aOver = g_nofit + g_batchOver;   // silent losses of both kinds
	ps1LogFmt("glrbrk", "batches=%d frames=%d brkTex=%d brkBlend=%d brkClip=%d"
		" spriteWrap=%d/%d vramFrames=%d mixedFrames=%d bgFrames=%d"
		" clipFull=%d dropVerts=%d dropGroups=%d holdFrames=%d dropPrims=%d"
		" draws=%d depth=%d",
		(int)g_batchSum, (int)g_frameSum, (int)g_brkTex, (int)g_brkBlend,
		(int)g_brkClip, (int)g_spriteWrap, (int)g_spriteAll,
		(int)g_vramFrames, (int)g_mixedFrames, (int)g_bgFrames,
		(int)g_clipFull, (int)g_dropVerts, (int)g_dropGroups, (int)g_holdFrames,
		(int)g_dropPrims, (int)g_draws, (int)g_depth);
	const unsigned int freq = ndrc_symbian_fastfreq();
	const unsigned int f = freq ? freq : 1;
	ps1LogFmt("glrtime", "setup=%ums clear=%ums draw=%ums swap=%ums frames=%d",
		(unsigned int)(g_tSetup * 1000ULL / f),
		(unsigned int)(g_tClear * 1000ULL / f),
		(unsigned int)(g_tDraw * 1000ULL / f),
		(unsigned int)(g_tSwap * 1000ULL / f),
		(int)g_frameSum);
	ps1LogFmt("glruv", "use=%uK full=%uK slots=%d frames=%d",
		(unsigned int)(g_useTexels / 1024ULL),
		(unsigned int)(g_fullTexels / 1024ULL),
		(int)(g_useSlots / (g_frameSum ? g_frameSum : 1)),
		(int)g_frameSum);
	}
