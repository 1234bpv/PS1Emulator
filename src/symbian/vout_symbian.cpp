// Frame output for Symbian. Replaces plugins/gpulib/vout_pl.c, which relies on
// frontend/plugin_lib.h and libpicofe - a layer that is not ported to Symbian.
//
// Stage 2 intentionally does NOT output to screen, but dumps VRAM to a file: so checking
// gpu_unai rasterization (done on host) is separated from Direct Screen Access
// (a separate task with its own bugs). Mixing them means looking for a rendering bug
// where the output is actually broken.

#include <e32std.h>
#include <e32base.h>
#include <w32std.h>
#include <fbs.h>
#include <hal.h>
#include <gles/egl.h>
#include <gles/gl.h>

#include <stdio.h>
#include <string.h>
#include "compat_stdio.h"

// Explicit path: libpcsxcore has its own gpu.h, and by USERINCLUDE list it is found
// first. Short "gpu.h" picks up that one, but this one is needed.
extern "C" {
#include "../../pcsx_rearmed/plugins/gpulib/gpu.h"
#include "../../pcsx_rearmed/frontend/plugin_lib.h"
}

// Last display window state reported by gpulib.
static struct {
	const void *vram;
	int offset;      // byte offset of window start in VRAM
	int rgb24;
	int x, y, w, h;
	int updates;     // how many times gpulib requested to present a frame
	int blanks;
} g_vout;

// Shell log (stage1.cpp): vout does not write JSONL itself, but EGL/GL state is visible only here.
extern "C" void ps1LogFmt(const char *aType, const char *aFmt, ...);

// Duplicate even VRAM line instead of stale odd line (rasterizer's "Skip lines" mode
// does not overwrite odd lines at all). Enabled by stage1 together with the hack.
static int g_ilaceOut;
// Set in vout_update, cleared after present: whether there is a frame not yet presented.
// One at start - the first frame must be shown before any drawing.
static int g_voutPending = 1;
// How many times gpulib ITSELF reported a new frame (Z2): fraction of total frame count
// shows the upper bound of gain from latch-based presenting.
static int g_voutGpulib;
// Flag for our own vout_update call to avoid setting the latch with it.
static int g_voutSelfCall;
// Number of consecutive frames where present was skipped due to no new content.
static int g_voutStale;
// Region actually shown last time: change of these values means the game
// flipped buffers, even if it didn't draw anything.
static int g_lastShownOffset = -1, g_lastShownW = -1, g_lastShownH = -1;
// Present only on latch instead of unconditional per-frame present ("dirty" flag).
static TBool g_dirtyOnly;

extern "C" {

int vout_init(void)
	{
	memset(&g_vout, 0, sizeof(g_vout));
	return 0;
	}

int vout_finish(void)
	{
	return 0;
	}

int vout_update(struct psx_gpu *gpu, int src_x, int src_y)
	{
	int bpp = (gpu->status & PSX_GPU_STATUS_RGB24) ? 24 : 16;
	int x = gpu->screen.x;
	int y = gpu->screen.y;
	int w = gpu->screen.w;
	int h = gpu->screen.h;
	int src_x2 = 0;

	if (x < 0) { w += x; src_x2 = -x; x = 0; }
	if (y < 0) { h += y; src_y -= y;  y = 0; }
	if (w <= 0 || h <= 0)
		return 0;

	// gpu_unai skips odd lines when downscaling
	if (h > 256 && gpu->state.downscale_enable && (src_y & 1))
		src_y++;

	g_vout.vram   = gpu->vram;
	g_vout.offset = (src_y * 1024 + src_x) * 2 + src_x2 * bpp / 8;
	g_vout.rgb24  = (bpp == 24);
	g_vout.x = x; g_vout.y = y; g_vout.w = w; g_vout.h = h;
	g_vout.updates++;
	// "Has new frame" latch: gpulib calls vout_update only when the display area
	// actually changed (GPUupdateLace exits early without fb_dirty_display_area).
	// Our own call from voutSymbianPresent does not set latch and does not increment counter -
	// otherwise Z2 would just show total frame count.
	if (!g_voutSelfCall)
		{
		g_voutPending = 1;
		g_voutGpulib++;
		}

	gpu->state.dims_changed = 0;
	return 1;
	}

void vout_blank(struct psx_gpu * /*gpu*/)
	{
	g_vout.blanks++;
	}

void vout_set_config(const struct rearmed_cbs * /*cbs*/)
	{
	}

long GPUopen(unsigned long * /*disp*/, char * /*cap*/, char * /*cfg*/)
	{
	gpu.frameskip.active = 0;
	gpu.frameskip.frame_ready = 1;
	return 0;
	}

long GPUclose(void)
	{
	return 0;
	}

} // extern "C"

// --- Our extension: dump for host verification ------------------------------

// The format is intentionally the simplest possible: text header, then raw VRAM 1024x512x16.
// Parsing on the host is trivial, and format errors that could be confused with
// rasterization bugs do not occur here.
extern "C" int voutSymbianDump(const char *path)
	{
	if (!g_vout.vram)
		return -1;

	FILE *f = fopen(path, "wb");
	if (!f)
		return -2;

	char hdr[128];
	int n = snprintf(hdr, sizeof(hdr),
		"PS1VRAM 1024 512 16 win=%d,%d,%d,%d off=%d rgb24=%d upd=%d blank=%d\n",
		g_vout.x, g_vout.y, g_vout.w, g_vout.h,
		g_vout.offset, g_vout.rgb24, g_vout.updates, g_vout.blanks);
	fwrite(hdr, 1, n, f);
	fwrite(g_vout.vram, 1, 1024 * 512 * 2, f);
	fclose(f);
	return 0;
	}

extern "C" int voutSymbianUpdates(void)
	{
	return g_vout.updates;
	}


// Z2: how many frames gpulib considered new. Lower than total frame count by exactly the amount
// that latch-based presenting can save.
extern "C" int voutSymbianGpulibUpdates(void)
	{
	return g_voutGpulib;
	}

// Frozen screen diagnostics. Distinguishes two cases that look identical from outside:
// (a) game is not drawing anything and not flipping buffers - freezing is CORRECT;
// (b) game draws or changes frame start address, but we do not show it - DEFECT.
// For (b), fb_dirty and current gpu.screen vs our g_vout are decisive.
extern "C" void voutSymbianDbgState(int *aFbDirty, int *aFbDirtyArea, int *aStatus,
                                    int *aSrcX, int *aSrcY, int *aScrW, int *aScrH,
                                    int *aVoutOffset, int *aStale)
	{
	if (aFbDirty)     *aFbDirty     = gpu.state.fb_dirty;
	if (aFbDirtyArea) *aFbDirtyArea = gpu.state.fb_dirty_display_area;
	if (aStatus)      *aStatus      = (int)gpu.status;
	if (aSrcX)        *aSrcX        = gpu.screen.src_x;
	if (aSrcY)        *aSrcY        = gpu.screen.src_y;
	if (aScrW)        *aScrW        = gpu.screen.w;
	if (aScrH)        *aScrH        = gpu.screen.h;
	if (aVoutOffset)  *aVoutOffset  = g_vout.offset;
	if (aStale)       *aStale       = g_voutStale;
	}

// --- Live frame output to screen (window server + BitBlt, Stage 2) ------------
// Window/context/bitmap are created ONCE (Init), frame is blitted EVERY FRAME
// (Present), at the end - hold and cleanup (Finish). This way the animation itself
// (BIOS boot, game) is visible on screen, not a single frame at the end of the run -
// previously presenting at the end caught BIOS already idle (black screen).
//
// Present updates g_vout itself with current frame (vout_update), because gpulib presents
// rarely - only on fb_dirty_display_area (which is why dump had upd=1). We need
// every frame, so we call vout_update directly and read fresh VRAM.
//
// DSA (CDirectScreenAccess) is intentionally avoided: it is CActive with CActiveScheduler
// and AbortNow/Restart handling; synchronous BitBlt is sufficient for our loop.

static TBool g_wsReady;
static RWsSession g_ws;
static CWsScreenDevice *g_dev;
static RWindowGroup g_grp;
static RWindow g_win;
static CWindowGc *g_gc;
static CFbsBitmap *g_bmp;
static TSize g_scr;

// Window is either ours (console branch creates g_ws/g_grp/g_win itself) or foreign - from
// CCoeControl in AVKON shell, because MBX-EGL requires window under CCoeEnv. Work goes
// through pointers; we close only what we created ourselves.
static TBool g_ownWindow = ETrue;
static RWsSession *g_wsp = &g_ws;
static RWindow *g_winp = &g_win;

// Letterboxing geometry cache: black bars (FillZ) are cleared only when it changes,
// not every frame - the rest of the bitmap is fully overwritten by frame copy anyway.
// Reset in initL on bitmap (re)creation.
static TInt g_blitOx = -1, g_blitOy, g_blitW, g_blitH;

// Blit time breakdown: fill = scaling loop into bitmap, present = BitBlt+Flush in
// window server. Shows what to optimize - loop (cache) or presentation (DSA/MBX).
static unsigned long long g_blitFillTicks, g_blitPresentTicks;
extern "C" unsigned int ndrc_symbian_fastfreq(void);

// DSA (Direct Screen Access) + direct-FB: frame is scaled and converted
// directly into screen buffer, so fill and present are merged into a single pass.
// STATUS (2026-08-04): WORKING, enabled by default, -0.93 ms per frame (30.13 vs
// 31.06 with lighting and semi-transparency). Disabled by "Direct draw" menu item
// or missing "dsa" argument in command line runs.
//
// It broke due to ONE thing: sd->Update() without arguments pushed nothing.
// We write bypassing bitgdi, so its own "dirty" region remains empty.
// Update must be requested with an explicit region - using DrawingRegion() of DSA itself.
//
// The 2026-08-01 diagnosis ("HAL returns rotated geometry", "WS composites on top")
// was NOT confirmed in ANY part: single buffer (halAddr == Bits() == 0xcb400000),
// single page (fbPages=0/1 over 1500 frames), and driver layout is correct -
// pattern written by bitgdi reads back with raw pointer 64/64.
// That probe ran in console branch, while game runs in GUI - hence false conclusion.
//
// rgb24 is not allowed in this path (branch writes 16-bit words and is untested),
// so 24-bit splash screens and videos take window path. That is why the defect was once
// interpreted as "video is visible, but 2D is not".
static TBool g_useDsa;
static CDirectScreenAccess *g_dsa;
static CFbsBitGc *g_dsaGc;
static TBool g_dsaActive;
static CActiveScheduler *g_dsaSched;   // non-null only if created by DSA
// Whether active scheduler can be pumped from blit. In console branch nobody else does this,
// in GUI - forbidden (reentrancy, same rule as in snd_symbian).
static TBool g_pumpAo = ETrue;

// Tilt control. The accelerometer reads about 300 units per g, so the threshold
// below is roughly a quarter of a g, with a lower release level so a hand held
// near the edge does not chatter. The neutral position is whatever the phone was
// in when tilt was switched on.
static TBool g_tilt;
static TBool g_tiltZeroed;
static TInt  g_tiltZeroX;
static TInt  g_tiltZeroY;
static unsigned short g_tiltPad;
static TInt  g_tiltReads;
static TInt  g_tiltLastX;
static TInt  g_tiltLastY;
// The digital bits are a fallback for a game that never leaves digital mode, so
// they fire only where the stick is already at full deflection.
const TInt KTiltOn  = 140;
const TInt KTiltOff = 110;
const TInt KTiltPitchOn  = 200;
const TInt KTiltPitchOff = 160;
// Analog: the point of tilt is a proportional turn, so the dead zone is only what
// a hand cannot hold still, and full deflection sits at a comfortable wrist angle.
const TInt KTiltDead      = 10;
const TInt KTiltSpan      = 140;
const TInt KTiltPitchDead = 40;
const TInt KTiltPitchSpan = 200;
static unsigned char g_tiltAX = 127;
static unsigned char g_tiltAY = 127;
static unsigned short tiltPad(void);

// Direct write to LCD buffer (HAL): scale frame directly to screen, merging fill
// and present into single pass (profiling showed: present is 150KB copy bandwidth,
// not overhead). Only when DSA is active (owns screen) and format = RGB565.
static TBool    g_fbOk;
static TUint16 *g_fbBase;    // first pixel address
static TInt     g_fbStride;  // 16-bit words per row (compatibility with old code)
static TInt     g_fbStrideB; // bytes per row - the only reliable quantity
static TInt     g_fbPixBytes;// bytes per pixel of screen buffer: 2 (565) or 4 (XRGB8888)
static TInt     g_fbMode;    // TDisplayMode from platform, -1 = unknown
static TInt     g_fbW, g_fbH;
// Raw HAL values (for diagnosing "two windows" - stride/dimensions/buffer).
static TInt g_fbAddr, g_fbBpp, g_fbLineBytes, g_fbFirstOff, g_fbXpix, g_fbYpix;
// Base and stride provided by platform (CFbsScreenDevice), not HAL.
static TBool g_fbFromPlatform;
// Present via row-by-row Mem::Copy into screen buffer instead of CWindowGc/CFbsBitGc::BitBlt.
// Frame is still converted to bitmap (known correct path), but goes to screen
// via simple row copy - without bitgdi mechanics, which cost most of present.
static TBool g_useRowCopy;

// EGL/GL ES 1.1 output (MBX): PS1 frame -> texture -> textured quad ->
// eglSwapBuffers. GPU handles scaling and present. Under "gl" flag, with
// fallback to window-BitBlt. Color: BGR555->RGB565 during texture fill.
//
// STATUS (2026-08-02): WORKS and gives correct picture under AVKON/CONE (on bare
// RWindow of console version eglCreateWindowSurface panicked - MBX-EGL relies
// on CCoeEnv::Static()), but on N95 8GB is 4x SLOWER than bitmap path, so
// remains disabled by default ("gl" flag).
//
// Measured on GT, identical input trace (ms per frame):
//   bitmap: fill  8.0 + present  3.7 = 11.7  -> ~30 fps
//   GL:     fill 23.9 + present 22.3 = 46.2  -> ~15 fps
//
// Two limitations, both measured and both unavoidable on this device:
//  1. Driver provides ONLY 32-bit configs (configs=4, have16=0). Surface is therefore
//     necessarily 32-bit, while panel is 16-bit - 32->16 conversion is unavoidable:
//     either in EGL or in compositor. Hence 22.3 ms for present, which alone is
//     twice as much as the ENTIRE bitmap path.
//  2. MBX stores textures in twiddled order, so uploading a frame costs
//     23.9 ms (5.4 MB/s) even after switching to texture exactly matching source
//     size and full glTexImage2D instead of subregion (initially was 73 ms).
//
// General conclusion: GL wins where GPU draws GEOMETRY. Passing a ready frame
// computed by CPU through it on MBX Lite is always more expensive than
// direct blit. Real gain would come only from PS1 rasterizer on GPU instead of
// gpu_unai - that is a separate renderer, not a setting.
static const TInt KGlTexW = 1024;   // POT, covers display width <=1024
static const TInt KGlTexH = 256;    // POT, covers field <=256 rows
static TBool      g_useGl;
static TBool      g_glReady;
static EGLDisplay g_eglDpy = EGL_NO_DISPLAY;
static EGLSurface g_eglSurf = EGL_NO_SURFACE;
static EGLContext g_eglCtx = EGL_NO_CONTEXT;
static EGLConfig  g_eglCfg;
static GLuint     g_glTex;
static TInt       g_glW, g_glH;      // EGL surface dimensions (not window)
static TBool      g_glLogged;        // log first frame diagnostics only once
static TUint16   *g_glBuf;           // conversion buffer (rows*w, RGB565)
static TBool glInitOnWindow();       // defined below, called from initL
static void  glShutdown();
extern "C" int voutSymbianScreenDump(const char *aPath);   // defined below

static void queryFramebuffer()
	{
	g_fbOk = EFalse;
	g_fbAddr = g_fbBpp = g_fbLineBytes = g_fbFirstOff = g_fbXpix = g_fbYpix = 0;
	HAL::Get(HALData::EDisplayMemoryAddress, g_fbAddr);
	HAL::Get(HALData::EDisplayBitsPerPixel, g_fbBpp);
	HAL::Get(HALData::EDisplayOffsetBetweenLines, g_fbLineBytes);
	HAL::Get(HALData::EDisplayOffsetToFirstPixel, g_fbFirstOff);
	HAL::Get(HALData::EDisplayXPixels, g_fbXpix);
	HAL::Get(HALData::EDisplayYPixels, g_fbYpix);
	if (g_fbAddr == 0 || g_fbBpp != 16 || g_fbLineBytes < 2 || g_fbXpix < 1 || g_fbYpix < 1)
		return;   // format unsupported - stay on window path
	g_fbBase     = (TUint16 *)((TUint8 *)g_fbAddr + g_fbFirstOff);
	g_fbStrideB  = g_fbLineBytes;
	g_fbPixBytes = 2;
	g_fbW = g_fbXpix; g_fbH = g_fbYpix;
	g_fbFromPlatform = EFalse;
	g_fbMode = -1;

	// Take layout FROM PLATFORM when it provides it: Stride() is
	// driver's ScanLineBytes(), i.e. what bitgdi draws with, and Bits() is start
	// of first pixel. HAL values remain fallback. Previous direct-FB
	// attempt relied only on HAL and gave distorted frame; this is the cheapest
	// answer to whether layouts are different at all.
	CFbsScreenDevice *sd = g_dsa ? g_dsa->ScreenDevice() : NULL;
	if (sd)
		{
		const TUint32 *bits = sd->Bits();
		const TInt strideB  = sd->Stride();
		const TSize sz      = sd->SizeInPixels();
		const TDisplayMode dm = sd->DisplayMode();
		const TInt pix = (dm == EColor64K) ? 2 : ((dm == EColor16MU || dm == EColor16MA) ? 4 : 0);
		if (bits && pix && strideB >= sz.iWidth * pix && sz.iWidth > 0 && sz.iHeight > 0)
			{
			g_fbBase     = (TUint16 *)bits;
			g_fbStrideB  = strideB;
			g_fbPixBytes = pix;
			g_fbW = sz.iWidth; g_fbH = sz.iHeight;
			g_fbFromPlatform = ETrue;
			g_fbMode = (TInt)dm;
			}
		}
	g_fbStride = g_fbStrideB / 2;
	g_fbOk = ETrue;
	}

// How many times driver returned different first pixel address and which exact addresses.
// If more than one - driver has two pages, and writing to cached address
// goes into the one not currently scanned by panel. This is exactly what the device
// owner described: part of old frame plus artifacts in certain areas.
static TInt   g_fbBaseChanges;
static TInt   g_fbBaseSeen;
static TUint32 g_fbBaseList[4];

// Re-read layout from driver. Cheap (accessors), so can run every frame.
static void refreshFbFromPlatform()
	{
	CFbsScreenDevice *sd = (g_dsa && g_dsaActive) ? g_dsa->ScreenDevice() : NULL;
	if (!sd)
		return;
	const TUint32 *bits = sd->Bits();
	if (!bits)
		return;
	const TUint32 b = (TUint32)bits;
	TInt i;
	for (i = 0; i < g_fbBaseSeen; i++)
		if (g_fbBaseList[i] == b)
			break;
	if (i == g_fbBaseSeen && g_fbBaseSeen < 4)
		g_fbBaseList[g_fbBaseSeen++] = b;
	if ((TUint32)g_fbBase != b)
		{
		g_fbBaseChanges++;
		g_fbBase    = (TUint16 *)bits;
		g_fbStrideB = sd->Stride();
		g_fbStride  = g_fbStrideB / 2;
		}
	}

extern "C" void voutSymbianFbPages(int *aChanges, int *aSeen, int *aB0, int *aB1)
	{
	if (aChanges) *aChanges = g_fbBaseChanges;
	if (aSeen)    *aSeen    = g_fbBaseSeen;
	if (aB0)      *aB0      = (int)(g_fbBaseSeen > 0 ? g_fbBaseList[0] : 0);
	if (aB1)      *aB1      = (int)(g_fbBaseSeen > 1 ? g_fbBaseList[1] : 0);
	}

static void queryFramebuffer();

// Whether WS gave us entire screen. CFbsBitGc respects clipping region itself, but raw
// write and row copy do not: they write the entire rectangle. If a system dialog
// pops up on top, region shrinks, and without this check we would draw over it.
static TBool dsaCoversScreen()
	{
	if (!g_dsa)
		return EFalse;
	const RRegion *rgn = g_dsa->DrawingRegion();
	if (!rgn || rgn->Count() != 1)
		return EFalse;
	const TRect r = rgn->BoundingRect();
	return (r.iTl.iX == 0 && r.iTl.iY == 0 &&
	        r.Width() >= g_scr.iWidth && r.Height() >= g_scr.iHeight);
	}

static void dsaStartL()
	{
	g_dsa->StartL();
	g_dsaGc = g_dsa->Gc();
	g_dsaGc->SetClippingRegion(g_dsa->DrawingRegion());
	// Geometry is re-queried on EVERY start, not once in initL: after
	// ETerminateRotation screen mode differs, and old base/stride would give a distorted
	// frame - an independent cause of "offset" that was not separated on 2026-08-01.
	queryFramebuffer();
	g_dsaActive = ETrue;
	}

// DSA observer: WS calls AbortNow when taking screen (dialog/notification), and
// Restart when it can be resumed. Between them we draw via window path (fallback).
class TDsaObserver : public MDirectScreenAccess
	{
public:
	void AbortNow(RDirectScreenAccess::TTerminationReasons aR)
		{
		g_dsaActive = EFalse;
		ps1LogFmt("dsa", "abort reason=%d", (int)aR);
		}
	void Restart(RDirectScreenAccess::TTerminationReasons aR)
		{
		g_dsaActive = EFalse;
		TInt e = KErrNotReady;
		if (g_dsa) { TRAP(e, dsaStartL()); }
		ps1LogFmt("dsa", "restart reason=%d err=%d active=%d", (int)aR, (int)e,
			(int)g_dsaActive);
		}
	};
static TDsaObserver g_dsaObs;

// Input: non-blocking polling of RWsSession events. g_evStat is armed once in initL,
// every frame voutSymbianPollPad() drains queue and holds g_pad (DKEY_* bits, active-high).
static TRequestStatus g_evStat;
static unsigned short g_pad;

// Keyboard layout -> PS1 buttons (DKEY_* bits from plugin_lib.h). Table, not
// switch: shell allows remapping keys from menu, and single source of truth for
// default layout must remain here - console branch doesn't go through menu.
// Order = order of rows in controls menu.
const TInt KPadButtons = 14;
static const unsigned char KPadBit[KPadButtons] =
	{
	DKEY_UP, DKEY_DOWN, DKEY_LEFT, DKEY_RIGHT,
	DKEY_CROSS, DKEY_CIRCLE, DKEY_SQUARE, DKEY_TRIANGLE,
	DKEY_L1, DKEY_R1, DKEY_L2, DKEY_R2, DKEY_SELECT, DKEY_START
	};

// Default N95 layout: arrows - D-pad, center - Cross (confirm),
// digits - face buttons. Second keycode per button - for keys with two sources.
static const short KPadDefScan[KPadButtons] =
	{
	EStdKeyUpArrow, EStdKeyDownArrow, EStdKeyLeftArrow, EStdKeyRightArrow,
	EStdKeyDevice3, '0', '7', '9',
	'1', '3', '4', '6', '2', '8'
	};
static const short KPadDefAlt[KPadButtons] =
	{ -1, -1, -1, -1, '5', -1, -1, -1, -1, -1, -1, -1, -1, -1 };

static short g_keyScan[KPadButtons];
static short g_keyAlt[KPadButtons];
// Toggle buttons: one press holds button indefinitely, next releases.
// In racing games this holds throttle without holding the key for the entire race.
static unsigned short g_padToggle;
static TBool g_keyMapReady;

static void keyMapDefaults()
	{
	for (TInt i = 0; i < KPadButtons; i++)
		{ g_keyScan[i] = KPadDefScan[i]; g_keyAlt[i] = KPadDefAlt[i]; }
	g_padToggle = 0;
	g_keyMapReady = ETrue;
	}

// Some firmwares send center nav key as Enter. Normalize codes BEFORE lookup in
// table so remapping center also remaps Enter.
static TInt aliasScan(TInt aScan)
	{
	if (aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
		return EStdKeyDevice3;
	return aScan;
	}

// Mask of PS1 buttons mapped to this code. Multiple buttons per single code
// are intentionally allowed: so one key presses, say, both "up" and throttle.
static unsigned short maskForScan(TInt aScan)
	{
	if (!g_keyMapReady)
		keyMapDefaults();
	const TInt sc = aliasScan(aScan);
	unsigned short m = 0;
	for (TInt i = 0; i < KPadButtons; i++)
		if (g_keyScan[i] == sc || g_keyAlt[i] == sc)
			m |= (unsigned short)(1 << KPadBit[i]);
	return m;
	}

// Toggle buttons flip on press and ignore release; normal buttons
// simply mirror key state.
static void padApply(unsigned short aMask, TInt aDown)
	{
	const unsigned short tog  = (unsigned short)(aMask & g_padToggle);
	const unsigned short norm = (unsigned short)(aMask & ~g_padToggle);
	if (aDown)
		{
		g_pad ^= tog;
		g_pad |= norm;
		}
	else
		g_pad &= (unsigned short)~norm;
	}

// Hot conversion loop: two pixels per 32-bit word. Moved into separate
// function INTENTIONALLY - inside blitCurrent there are too many live variables, and GCCE 3.4.3
// spilled masks to stack, reloading them on each iteration (15 instructions
// instead of 11). Here the register allocator has free hands.
//
// Empty inline asm makes constants opaque to optimizer: otherwise
// compiler synthesizes each 32-bit mask with chain of four BICs directly in
// the loop - 16 of 28 instructions per two pixels (verified with disassembler).
//
// __builtin_prefetch was discarded in THIS placement: one PLD per two pixels is
// eight accesses per cache line. Measured +3.7% equals cost of one extra
// instruction in 28, i.e. no benefit. One PLD per 32 bytes was not tested.
// Underscore form is required: headers already have noinline macro.
static void __attribute__((__noinline__))
blitRow565(TUint32 *aDst, const TUint32 *aSrc, TInt aPairs)
	{
	TUint32 cRB = 0x001f001fu, cG54 = 0x07c007c0u, cG0 = 0x00200020u;
	__asm__ __volatile__ ("" : "+r"(cRB), "+r"(cG54), "+r"(cG0));
	do
		{
		const TUint32 p = *aSrc++;
		*aDst++ = ((p & cRB) << 11) | ((p << 1) & cG54)
		        | ((p >> 4) & cG0) | ((p >> 10) & cRB);
		} while (--aPairs);
	}

// Same for 32-bit screen buffer (EColor16MU/16MA, XRGB8888).
//
// MEASURED by 2026-08-03 probe: platform gives stride=1280B at 320 pixels and
// DisplayMode=EColor16MU, meaning screen buffer is 32-bit, while HAL reports
// bpp=16 and line=640B. The 2026-08-01 direct-FB attempt relied on HAL numbers -
// hence the "offset": we wrote 16-bit pixels with stride 640 into 32-bit buffer
// with stride 1280. Here we convert directly to target format, removing intermediate
// bitmap and 565->8888 conversion inside bitgdi (which is most of present).
//
// 5 bits -> 8 is done as (v<<3)|(v>>2): multiplication by 255/31 within precision of 1,
// without tables or division.
static void __attribute__((__noinline__))
blitRow8888(TUint32 *aDst, const TUint16 *aSrc, TInt aPixels)
	{
	TUint32 m5 = 0x1fu;
	__asm__ __volatile__ ("" : "+r"(m5));
	do
		{
		const TUint32 p = *aSrc++;
		const TUint32 r = p & m5;
		const TUint32 g = (p >> 5) & m5;
		const TUint32 b = (p >> 10) & m5;
		*aDst++ = 0xff000000u
		        | (((r << 3) | (r >> 2)) << 16)
		        | (((g << 3) | (g >> 2)) << 8)
		        |  ((b << 3) | (b >> 2));
		} while (--aPixels);
	}

// Average of two BGR555 pixels without overflow between adjacent fields:
// (a&b) + (((a^b) & mask) >> 1). Mask clears LSB of each of three
// 5-bit fields, so carry from one color to another is impossible.
static inline TUint16 Avg555(TUint16 aA, TUint16 aB)
	{
	return (TUint16)((aA & aB) + (((aA ^ aB) & 0x7bde) >> 1));
	}

// FXAA-lite for 1:1 scale. Luminance taken as green channel: in BGR555 this is two
// cycles, and green gives ~60% of luminance - full conversion would cost
// 10x more per each of five samples. Blur only where gradient
// exceeds threshold, and only ACROSS detected edge; result - 50% center plus
// 25% of each neighbor. Called only for 1 <= aX < dstW-1, so bounds are not checked.
static inline TUint16 FxaaPixel(const TUint16 *aRow, const TUint16 *aUp,
                                const TUint16 *aDn, TInt aX, TInt aThr, TInt aMax)
	{
	const TUint16 m = aRow[aX];
	const TUint16 w = aRow[aX - 1], e = aRow[aX + 1];
	const TUint16 n = aUp[aX], s = aDn[aX];
	const TInt lm = (m >> 5) & 0x1f;
	const TInt lw = (w >> 5) & 0x1f, le = (e >> 5) & 0x1f;
	const TInt ln = (n >> 5) & 0x1f, ls = (s >> 5) & 0x1f;
	TInt lo = lm, hi = lm;
	if (lw < lo) lo = lw; else if (lw > hi) hi = lw;
	if (le < lo) lo = le; else if (le > hi) hi = le;
	if (ln < lo) lo = ln; else if (ln > hi) hi = ln;
	if (ls < lo) lo = ls; else if (ls > hi) hi = ls;
	// Upper bound protects UI: white text on dark background gives almost full
	// green range, while steps on shaded edges give moderate. Without it
	// filter blurs both equally, as to it they are the same edge.
	const TInt range = hi - lo;
	if (range < aThr || range > aMax)
		return m;
	const TInt gv = (ln > ls) ? ln - ls : ls - ln;   // vertical gradient
	const TInt gh = (lw > le) ? lw - le : le - lw;   // ...and horizontal
	return (gv >= gh) ? Avg555(m, Avg555(n, s)) : Avg555(m, Avg555(w, e));
	}

// PS1 VRAM - BGR555 (bit15 mask, 14-10 B, 9-5 G, 4-0 R); screen EColor64K -
// RGB565. Expand green from 5 to 6 bits to preserve brightness.
static inline TUint16 Bgr555ToRgb565(TUint16 aPix)
	{
	const TUint r = aPix & 0x1f;
	const TUint g = (aPix >> 5) & 0x1f;
	const TUint b = (aPix >> 10) & 0x1f;
	const TUint g6 = (g << 1) | (g >> 4);
	return (TUint16)((r << 11) | (g6 << 5) | b);
	}

static inline TUint32 Bgr555ToXrgb8888(TUint16 aPix)
	{
	const TUint32 r = aPix & 0x1f;
	const TUint32 g = (aPix >> 5) & 0x1f;
	const TUint32 b = (aPix >> 10) & 0x1f;
	return 0xff000000u | (((r << 3) | (r >> 2)) << 16)
	                   | (((g << 3) | (g >> 2)) << 8)
	                   |  ((b << 3) | (b >> 2));
	}

static void initL()
	{
	g_ownWindow = ETrue;
	g_wsp = &g_ws;
	g_winp = &g_win;
	User::LeaveIfError(g_ws.Connect());
	g_dev = new (ELeave) CWsScreenDevice(g_ws);
	User::LeaveIfError(g_dev->Construct());
	g_grp = RWindowGroup(g_ws);
	User::LeaveIfError(g_grp.Construct((TUint32)&g_grp, ETrue));
	g_grp.SetOrdinalPosition(0);
	g_win = RWindow(g_ws);
	User::LeaveIfError(g_win.Construct(g_grp, (TUint32)&g_win));
	g_scr = g_dev->SizeInPixels();
	g_win.SetExtent(TPoint(0, 0), g_scr);
	// Under DSA/direct-FB background fill and redraw store give WS the right to redraw our
	// window over the frame that we put into screen bypassing it. SetBackgroundColor()
	// WITHOUT argument means "do not clear"; this is a correctness fix, not performance
	// (WS overhead here is measured to be zero).
	if (g_useDsa)
		{
		g_win.SetBackgroundColor();
		g_win.EnableRedrawStore(EFalse);
		g_win.SetNonFading(ETrue);
		}
	else
		g_win.SetBackgroundColor(KRgbBlack);
	g_win.Activate();
	if (g_useDsa)
		{
		// Validate window ONCE before capturing screen: Activate leaves window
		// invalid, and deferred server-initiated redraw would fire after
		// StartL, erasing the first frame.
		g_win.Invalidate();
		g_win.BeginRedraw();
		g_win.EndRedraw();
		g_ws.Flush();
		}
	g_gc = new (ELeave) CWindowGc(g_dev);
	User::LeaveIfError(g_gc->Construct());
	g_bmp = new (ELeave) CFbsBitmap;
	User::LeaveIfError(g_bmp->Create(g_scr, EColor64K));
	g_blitOx = -1;   // new bitmap - force FillZ on first frame
	g_blitFillTicks = g_blitPresentTicks = 0;

	// Input: group is already in foreground (ordinal 0), so it receives keys.
	// Arm event queue once; poll non-blocking every frame.
	g_pad = 0;
	g_ws.EventReady(&g_evStat);

	// GL ES output (MBX): if enabled and succeeded - present goes through GPU. Otherwise
	// (no 3D / error) stay on window-BitBlt.
	g_glReady = EFalse;
	if (g_useGl)
		{
		TRAPD(ge, { if (!glInitOnWindow()) glShutdown(); });
		if (ge != KErrNone) { glShutdown(); }
		}

	// DSA: requires CActiveScheduler (for abort/restart events). Usually already
	// created by audio subsystem; if not - create our own and remove in Finish.
	g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; g_dsaSched = NULL;
	if (g_useDsa)
		{
		if (!CActiveScheduler::Current())
			{
			g_dsaSched = new CActiveScheduler;
			if (g_dsaSched) CActiveScheduler::Install(g_dsaSched);
			}
		TRAPD(e, {
			g_dsa = CDirectScreenAccess::NewL(g_ws, *g_dev, g_win, g_dsaObs);
			dsaStartL();
			});
		if (e != KErrNone)
			{ delete g_dsa; g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; }
		// Geometry for direct-FB is queried by dsaStartL itself - and on every restart.
		}

	g_wsReady = ETrue;
	}

// Trace with immediate flush: survives panics (TRAP doesn't catch), to know at
// which EGL/GL call initialization crashes. Read from E:\gltrace.txt.
// EGL/GL ES initialization on existing window. Returns EFalse -> fallback to bitmap.
// Requires window under CCoeEnv (AVKON), otherwise eglCreateWindowSurface panics.
// What driver actually provided: surface dimensions and ACTUAL config depths. Requesting
// 5-6-5 does not obligate driver to give it, and mismatch between window format and
// surface looks like corrupted colors rather than an error.
static void glLogState(const char *aWhere)
	{
	EGLint sw = 0, sh = 0, r = 0, g = 0, b = 0, a = 0, id = 0;
	eglQuerySurface(g_eglDpy, g_eglSurf, EGL_WIDTH, &sw);
	eglQuerySurface(g_eglDpy, g_eglSurf, EGL_HEIGHT, &sh);
	if (g_eglCfg)
		{
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_RED_SIZE, &r);
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_GREEN_SIZE, &g);
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_BLUE_SIZE, &b);
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_ALPHA_SIZE, &a);
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_CONFIG_ID, &id);
		}
	EGLint bs = 0;
	if (g_eglCfg) eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_BUFFER_SIZE, &bs);
	ps1LogFmt("gl", "%s surf=%dx%d win=%dx%d cfg=%d rgba=%d%d%d%d bufsz=%d winmode=%d egl=0x%x glerr=0x%x",
		aWhere, (int)sw, (int)sh, (int)g_scr.iWidth, (int)g_scr.iHeight,
		(int)id, (int)r, (int)g, (int)b, (int)a, (int)bs,
		(int)(g_winp ? g_winp->DisplayMode() : 0),
		(unsigned)eglGetError(), (unsigned)glGetError());
	}

static TBool glInitOnWindow()
	{
	g_eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_eglDpy == EGL_NO_DISPLAY) return EFalse;
	if (!eglInitialize(g_eglDpy, NULL, NULL)) return EFalse;
	// Explicit RGB565: config must match window format.
	const EGLint attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_BUFFER_SIZE,  16,
		EGL_RED_SIZE,     5,
		EGL_GREEN_SIZE,   6,
		EGL_BLUE_SIZE,    5,
		EGL_DEPTH_SIZE,   0,
		EGL_NONE };
	// KEY for non-ScreenPlay devices (and N95/S60v3 is such). Symbian
	// Developer Library, "Symbian-Specific Behavior": RWindow display mode
	// MUST match EGL surface buffer depth. Otherwise surface content
	// is read as different format: 32-bit buffer read as 16-bit produces
	// both shifted colors (black appears yellow) and 2x wider image.
	//
	// eglChooseConfig additionally treats bit depths as MINIMUMS and on 5-6-5 request
	// returns 8888, so we have to choose ourselves - specifically matching depth that
	// window agreed to accept.
	EGLConfig cfgs[32]; EGLint n = 0;
	if (!eglChooseConfig(g_eglDpy, attribs, cfgs, 32, &n) || n < 1) return EFalse;

	EGLint want16 = 0, want32 = 0;
	for (EGLint i = 0; i < n; i++)
		{
		EGLint bs = 0;
		eglGetConfigAttrib(g_eglDpy, cfgs[i], EGL_BUFFER_SIZE, &bs);
		if (bs == 16) want16 = 1;
		if (bs == 32) want32 = 1;
		}
	// Request mode for which config actually exists; 16-bit is cheaper, so try first.
	TDisplayMode dm = EColor64K;
	if (!want16 && want32) dm = EColor16MU;
	g_winp->SetRequiredDisplayMode(dm);
	g_wsp->Flush();
	const TDisplayMode got = g_winp->DisplayMode();
	const EGLint gotBits = (got == EColor64K) ? 16 : (got == EColor16M ? 24 : 32);

	EGLConfig cfg = cfgs[0]; EGLint cfgBits = 0;
	for (EGLint i = 0; i < n; i++)
		{
		EGLint bs = 0;
		eglGetConfigAttrib(g_eglDpy, cfgs[i], EGL_BUFFER_SIZE, &bs);
		if (bs == gotBits) { cfg = cfgs[i]; cfgBits = bs; break; }
		}
	g_eglCfg = cfg;
	ps1LogFmt("gl", "configs=%d have16=%d have32=%d wantMode=%d gotMode=%d bits=%d cfgBits=%d",
		(int)n, (int)want16, (int)want32, (int)dm, (int)got, (int)gotBits, (int)cfgBits);
	g_eglSurf = eglCreateWindowSurface(g_eglDpy, cfg, (NativeWindowType)g_winp, NULL);
	if (g_eglSurf == EGL_NO_SURFACE) return EFalse;
	g_eglCtx = eglCreateContext(g_eglDpy, cfg, EGL_NO_CONTEXT, NULL);
	if (g_eglCtx == EGL_NO_CONTEXT) return EFalse;
	if (!eglMakeCurrent(g_eglDpy, g_eglSurf, g_eglSurf, g_eglCtx)) return EFalse;
	// No waiting for vertical sync: emulator does not maintain 60 Hz anyway,
	// and blocking in swap is simply added to frame time.
	eglSwapInterval(g_eglDpy, 0);

	g_glBuf = (TUint16 *)User::Alloc(KGlTexW * KGlTexH * 2);
	if (!g_glBuf) return EFalse;

	// Geometry - from surface, not window: window auto-rotates, and discrepancy
	// between them produces "incorrect scale".
	EGLint sw = 0, sh = 0;
	eglQuerySurface(g_eglDpy, g_eglSurf, EGL_WIDTH, &sw);
	eglQuerySurface(g_eglDpy, g_eglSurf, EGL_HEIGHT, &sh);
	if (sw > 0 && sh > 0) { g_glW = sw; g_glH = sh; }
	else                  { g_glW = g_scr.iWidth; g_glH = g_scr.iHeight; }

	glViewport(0, 0, g_glW, g_glH);
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	glOrthof(0, (GLfloat)g_glW, (GLfloat)g_glH, 0, -1, 1);  // top-left origin
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glClearColor(0, 0, 0, 1);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
	glGenTextures(1, &g_glTex);
	glBindTexture(GL_TEXTURE_2D, g_glTex);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	// With zeroes, not NULL: with NULL texture content is undefined, and everything
	// sampled outside the filled region shows driver garbage.
	Mem::FillZ(g_glBuf, KGlTexW * KGlTexH * 2);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, KGlTexW, KGlTexH, 0,
		GL_RGB, GL_UNSIGNED_SHORT_5_6_5, g_glBuf);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	g_glReady = ETrue;
	glLogState("init");
	return ETrue;
	}

// One frame via GL: convert field to buffer (BGR555->RGB565), upload to
// texture, draw letterboxed quad, swap. Scaling and present - on GPU.
static void glPresentFrame(const TUint8 *vbytes, TInt startByte,
                           TInt sw, TInt sh, TBool field, TInt srcRows)
	{
	// The same two counters as on bitmap path so the numbers are comparable:
	// "fill" = conversion + texture upload, "present" = drawing + swap.
	const TUint _gc0 = User::FastCounter();
	TInt hf = field ? srcRows : sh;
	if (hf > KGlTexH) hf = KGlTexH;
	TInt w = sw; if (w > KGlTexW) w = KGlTexW;

	// Texture sized exactly for source (smallest POT), not fixed 1024x256: MBX
	// stores textures in twiddled order, so driver rearranges ENTIRE region
	// on each upload, and extra megabytes are paid for every frame.
	TInt potW = 16; while (potW < w)  potW <<= 1;
	TInt potH = 16; while (potH < hf) potH <<= 1;

	for (TInt y = 0; y < hf; y++)
		{
		const TInt ssy = field ? y * 2 : y;
		const TUint16 *srow = (const TUint16 *)(vbytes + startByte) + ssy * 1024;
		TUint16 *drow = g_glBuf + y * potW;
		for (TInt x = 0; x < w; x++)
			drow[x] = Bgr555ToRgb565(srow[x]);
		}
	glBindTexture(GL_TEXTURE_2D, g_glTex);
	// Full level redefinition instead of subregion: partial upload into
	// twiddled texture is a read-modify-write of entire level.
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, potW, potH, 0,
		GL_RGB, GL_UNSIGNED_SHORT_5_6_5, g_glBuf);
	const TUint _gc1 = User::FastCounter();
	g_blitFillTicks += (unsigned)(_gc1 - _gc0);

	// Target is 4:3 regardless of source size - same rule as on blit path
	// (PS1 pixels are not square).
	const TInt SW = g_glW, SH = g_glH;
	TInt dstW = SW, dstH = SW * 3 / 4;
	if (dstH > SH) { dstH = SH; dstW = SH * 4 / 3; }
	if (dstW > SW) dstW = SW;
	const TInt ox = (SW - dstW) / 2, oy = (SH - dstH) / 2;
	const GLfloat u = (GLfloat)w / potW, v = (GLfloat)hf / potH;
	const GLfloat x0 = (GLfloat)ox, y0 = (GLfloat)oy, x1 = (GLfloat)(ox + dstW), y1 = (GLfloat)(oy + dstH);
	const GLfloat verts[8] = { x0, y0,  x1, y0,  x0, y1,  x1, y1 };
	const GLfloat texc[8]  = { 0, 0,   u, 0,    0, v,    u, v };

	glClear(GL_COLOR_BUFFER_BIT);
	glVertexPointer(2, GL_FLOAT, 0, verts);
	glTexCoordPointer(2, GL_FLOAT, 0, texc);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	eglSwapBuffers(g_eglDpy, g_eglSurf);
	g_blitPresentTicks += (unsigned)(User::FastCounter() - _gc1);

	// Once after first frame: GL error here means texture upload
	// or drawing failed, even though initialization succeeded.
	if (!g_glLogged)
		{
		g_glLogged = ETrue;
		ps1LogFmt("gl", "frame1 src=%dx%d field=%d tex=%dx%d dst=%dx%d+%d+%d",
			(int)sw, (int)sh, (int)field, (int)w, (int)hf,
			(int)dstW, (int)dstH, (int)ox, (int)oy);
		glLogState("frame1");
		}
	}

static void glShutdown()
	{
	if (g_eglDpy != EGL_NO_DISPLAY)
		{
		eglMakeCurrent(g_eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (g_eglCtx != EGL_NO_CONTEXT)  eglDestroyContext(g_eglDpy, g_eglCtx);
		if (g_eglSurf != EGL_NO_SURFACE) eglDestroySurface(g_eglDpy, g_eglSurf);
		eglTerminate(g_eglDpy);
		}
	g_eglDpy = EGL_NO_DISPLAY; g_eglSurf = EGL_NO_SURFACE; g_eglCtx = EGL_NO_CONTEXT;
	g_eglCfg = NULL;
	User::Free(g_glBuf); g_glBuf = NULL;
	g_glReady = EFalse;
	// Restore 16-bit window mode: under GL we promoted it to 32-bit, and
	// leaving it would mean paying for conversion on bitmap path as well,
	// which fallback leads to.
	if (g_winp)
		{
		g_winp->SetRequiredDisplayMode(EColor64K);
		if (g_wsp) g_wsp->Flush();
		}
	}

// --- Context access for GL rasterizer ---------------------------------------
// glrender.cpp draws on its own, but context and surface are held by this file. We provide
// it with exactly two things: surface size and swap. EGL does not leave
// this file.

extern "C" int voutSymbianGlSwap(void)
	{
	if (!g_glReady)
		return -1;
	return eglSwapBuffers(g_eglDpy, g_eglSurf) ? 0 : -1;
	}

extern "C" void voutSymbianGlSize(int *aW, int *aH)
	{
	if (aW) *aW = g_glReady ? g_glW : 0;
	if (aH) *aH = g_glReady ? g_glH : 0;
	}

// --- GL overhead probe ------------------------------------------------------
// MBX branch is not fillrate-bound: the game requests 93k pixels per frame,
// i.e. 1.21 screens, which is negligible for hardware rasterizer. It is bound
// by call count - 324 primitives per frame with texture page switches
// between them. How much draw call itself and binding switch cost
// on this driver cannot be learned from docs, so we measure
// synthetically: same number of primitives, but without emulation at all. The probe
// is deliberately not a PS1 renderer - it only answers whether
// 324 calls fit within frame budget.
static const TInt KBenchTex     = 16;    // distinct textures in set
static const TInt KBenchTexSz   = 128;   // texture side, POT
static const TInt KBenchMaxPrim = 648;
static const TInt KBenchFrames  = 200;
static const TInt KBenchWarm    = 10;    // warmup: driver uploads extra on first frame

struct TGlBenchCfg
	{
	const char *iName;
	TInt iPrims;    // quads per frame
	TInt iSide;     // quad side, pixels (<=0 = full surface)
	TInt iBinds;    // binds per frame (individual calls path)
	TInt iBlend;    // blending (GL_ONE, GL_ONE - PS1 mode 1)
	TInt iUploads;  // glTexImage2D per frame: page reload
	TInt iUpSize;   // side of reloading texture
	TInt iBatch;    // >0: all prims submitted in this many calls (with sorting)
	TInt iShort;    // GL_SHORT vertices instead of GL_FLOAT
	TInt iPal;      // upload as paletted texture (4-bit + CLUT)
	TInt iRead;     // side of square read back from GPU (0 = do not read)
	};

// First run (R9) gave frame floor of 4.07 ms and ~14 us per draw call.
// Second batch refines what could not be derived from those numbers: cost of A SINGLE
// vertex (different batch sizes per call), cost of real variant with
// sorting (13 different textures per frame - measured in R8), vertex format and
// texture upload dependence on size.
// Third batch (R11) targets what remained after R10 as only serious obstacle:
// texture upload runs at 22 MB/s, and full PS1 page expanded to
// RGB565 would cost ~6 ms. Driver has GL_OES_compressed_paletted_
// texture, and 4-bit PS1 page with CLUT is exactly what it
// accepts: 4x less data and zero unpacking on CPU. We measure
// both methods across three sizes, on the exact same texture.
// Fourth batch (R13) measures last unknown of branch, and it is decisive.
// Hardware renderer keeps frame in GPU memory, while game over 6099-frame run
// accesses VRAM bypassing rasterizer 0.33 times per frame via copy (on
// average 1024 pixels) and once per 359 frames via CPU read (R12).
// Every such access on deferred rasterizer means readback,
// i.e. forced pipeline flush. If it costs tens of milliseconds, entire
// gain of branch disappears; if single digits - it doesn't matter.
static const TGlBenchCfg KBench[] =
	{
	// Baseline for comparison: real frame without readback.
	{ "sort13",    324, 8, 0, 0, 0, 0,   13, 0, 0, 0 },
	// Readbacks of varying size, relative to frame.
	{ "rd8",       324, 8, 0, 0, 0, 0,   13, 0, 0, 8 },
	{ "rd32",      324, 8, 0, 0, 0, 0,   13, 0, 0, 32 },
	{ "rd128",     324, 8, 0, 0, 0, 0,   13, 0, 0, 128 },
	};

// Scattered quads in a single vertex stream. Vertex order matches present path
// (TRIANGLE_STRIP), while for batched variant the same quad expands into two
// triangles, because a strip cannot submit multiple quads in one call.
static void glBenchVerts(GLfloat *aStrip, GLfloat *aStripT, GLfloat *aList,
	GLfloat *aListT, GLshort *aListS, TInt aPrims, TInt aSide)
	{
	TUint32 rnd = 12345;
	const TInt sw = g_glW > aSide ? g_glW - aSide : 1;
	const TInt sh = g_glH > aSide ? g_glH - aSide : 1;
	for (TInt i = 0; i < aPrims; i++)
		{
		rnd = rnd * 1103515245u + 12345u; const TInt px = (TInt)((rnd >> 16) % (TUint32)sw);
		rnd = rnd * 1103515245u + 12345u; const TInt py = (TInt)((rnd >> 16) % (TUint32)sh);
		const GLfloat x0 = (GLfloat)px, y0 = (GLfloat)py;
		const GLfloat x1 = (GLfloat)(px + aSide), y1 = (GLfloat)(py + aSide);
		GLfloat *v = aStrip + i * 8, *t = aStripT + i * 8;
		v[0]=x0; v[1]=y0;  v[2]=x1; v[3]=y0;  v[4]=x0; v[5]=y1;  v[6]=x1; v[7]=y1;
		t[0]=0;  t[1]=0;   t[2]=1;  t[3]=0;   t[4]=0;  t[5]=1;   t[6]=1;  t[7]=1;
		GLfloat *w = aList + i * 12, *u = aListT + i * 12;
		w[0]=x0; w[1]=y0;  w[2]=x1; w[3]=y0;  w[4]=x0; w[5]=y1;
		w[6]=x1; w[7]=y0;  w[8]=x1; w[9]=y1;  w[10]=x0; w[11]=y1;
		u[0]=0;  u[1]=0;   u[2]=1;  u[3]=0;   u[4]=0;  u[5]=1;
		u[6]=1;  u[7]=0;   u[8]=1;  u[9]=1;   u[10]=0; u[11]=1;
		GLshort *s = aListS + i * 12;
		for (TInt k = 0; k < 12; k++)
			s[k] = (GLshort)w[k];
		}
	}

extern "C" int voutSymbianGlBench(int aFrames)
	{
	if (!g_glReady)
		{
		ps1LogFmt("glbench", "GL is not up - the run needs the gl flag");
		return -1;
		}
	const TInt frames = aFrames > 0 ? aFrames : KBenchFrames;
	const TInt vbytes = KBenchMaxPrim * 12 * 2 * (TInt)sizeof(GLfloat);
	GLfloat *strip  = (GLfloat *)User::Alloc(vbytes);
	GLfloat *stripT = (GLfloat *)User::Alloc(vbytes);
	GLfloat *list   = (GLfloat *)User::Alloc(vbytes);
	GLfloat *listT  = (GLfloat *)User::Alloc(vbytes);
	GLshort *listS  = (GLshort *)User::Alloc(vbytes);
	// For largest texture being measured: PS1 page is 256x256 texels.
	TUint16 *texbuf = (TUint16 *)User::Alloc(256 * 256 * 2);
	if (!strip || !stripT || !list || !listT || !listS || !texbuf)
		{
		User::Free(strip); User::Free(stripT); User::Free(list);
		User::Free(listT); User::Free(listS); User::Free(texbuf);
		ps1LogFmt("glbench", "out of memory");
		return -2;
		}

	// Texture set. Contents differ so driver doesn't collapse them into one.
	GLuint tex[KBenchTex];
	GLuint upTex = 0;
	glGenTextures(KBenchTex, tex);
	glGenTextures(1, &upTex);
	for (TInt k = 0; k < KBenchTex; k++)
		{
		for (TInt p = 0; p < KBenchTexSz * KBenchTexSz; p++)
			texbuf[p] = (TUint16)((p * 7 + k * 1013) & 0xFFFF);
		glBindTexture(GL_TEXTURE_2D, tex[k]);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, KBenchTexSz, KBenchTexSz, 0,
			GL_RGB, GL_UNSIGNED_SHORT_5_6_5, texbuf);
		}
	// Source for uploads: fill whole buffer, as we now measure 256x256 too.
	for (TInt p = 0; p < 256 * 256; p++)
		texbuf[p] = (TUint16)((p * 7) & 0xFFFF);
	const char *ext = (const char *)glGetString(GL_EXTENSIONS);
	TInt hasPal = 0;
	for (const char *s = ext; s && *s; s++)
		if (s[0] == 'p' && s[1] == 'a' && s[2] == 'l' && s[3] == 'e') { hasPal = 1; break; }
	ps1LogFmt("glbench", "begin: surface=%dx%d frames=%d textures=%dx%d paletted=%d err=%u",
		(int)g_glW, (int)g_glH, (int)frames, (int)KBenchTex, (int)KBenchTexSz,
		(int)hasPal, (unsigned)glGetError());

	const unsigned freq = ndrc_symbian_fastfreq();
	const TInt ncfg = (TInt)(sizeof(KBench) / sizeof(KBench[0]));
	for (TInt c = 0; c < ncfg; c++)
		{
		const TGlBenchCfg &cfg = KBench[c];
		TInt side = cfg.iSide;
		if (side <= 0) side = g_glH < g_glW ? g_glH : g_glW;
		glBenchVerts(strip, stripT, list, listT, listS, cfg.iPrims, side);
		const TInt bindEvery = cfg.iBinds > 0
			? (cfg.iPrims + cfg.iBinds - 1) / cfg.iBinds : 0;
		// Quads per batch in sorted variant. Last one picks up the remainder.
		const TInt perBatch = cfg.iBatch > 0
			? (cfg.iPrims + cfg.iBatch - 1) / cfg.iBatch : 0;
		if (cfg.iBlend)
			{ glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); }
		else
			glDisable(GL_BLEND);
		if (cfg.iBatch)
			{
			if (cfg.iShort) glVertexPointer(2, GL_SHORT, 0, listS);
			else            glVertexPointer(2, GL_FLOAT, 0, list);
			glTexCoordPointer(2, GL_FLOAT, 0, listT);
			}
		else
			{ glVertexPointer(2, GL_FLOAT, 0, strip); glTexCoordPointer(2, GL_FLOAT, 0, stripT); }

		TUint t0 = 0;
		for (TInt f = -KBenchWarm; f < frames; f++)
			{
			if (f == 0) { glFinish(); t0 = User::FastCounter(); }
			glClear(GL_COLOR_BUFFER_BIT);
			for (TInt u = 0; u < cfg.iUploads; u++)
				{
				const TInt us = cfg.iUpSize > 0 ? cfg.iUpSize : KBenchTexSz;
				// Same texture for all upload variants so methods are compared,
				// not different driver objects.
				glBindTexture(GL_TEXTURE_2D, upTex);
				if (cfg.iPal)
					// Palette (16 entries of 2 bytes) followed by 4-bit
					// indices - exact layout of 4-bit PS1 page.
					glCompressedTexImage2D(GL_TEXTURE_2D, 0,
						GL_PALETTE4_RGB5_A1_OES, us, us, 0, 32 + us * us / 2, texbuf);
				else
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, us, us, 0,
						GL_RGB, GL_UNSIGNED_SHORT_5_6_5, texbuf);
				}
			if (cfg.iBatch)
				{
				for (TInt b = 0; b < cfg.iBatch; b++)
					{
					const TInt first = b * perBatch;
					if (first >= cfg.iPrims) break;
					TInt cnt = perBatch;
					if (first + cnt > cfg.iPrims) cnt = cfg.iPrims - first;
					glBindTexture(GL_TEXTURE_2D, tex[b % KBenchTex]);
					glDrawArrays(GL_TRIANGLES, first * 6, cnt * 6);
					}
				}
			else
				{
				for (TInt i = 0; i < cfg.iPrims; i++)
					{
					if (bindEvery && (i % bindEvery) == 0)
						glBindTexture(GL_TEXTURE_2D, tex[(i / bindEvery) % KBenchTex]);
					glDrawArrays(GL_TRIANGLE_STRIP, i * 4, 4);
					}
				}
			// Readback happens BEFORE swap, as a renderer would do:
			// game requests VRAM content mid-frame, not after present.
			if (cfg.iRead > 0)
				glReadPixels(0, 0, cfg.iRead, cfg.iRead, GL_RGBA,
					GL_UNSIGNED_BYTE, texbuf);
			eglSwapBuffers(g_eglDpy, g_eglSurf);
			}
		glFinish();
		const TUint dt = User::FastCounter() - t0;
		// Microseconds per frame: milliseconds here would give only two significant digits
		// per 5 ms frame, and we care about difference between configurations.
		const unsigned us = (unsigned)((TUint64)dt * 1000000ULL / (freq ? freq : 1) / frames);
		ps1LogFmt("glbench", "%s prims=%d side=%d binds=%d blend=%d up=%dx%d batch=%d"
			" pal=%d read=%d calls=%d verts=%d us=%u px=%u err=%u",
			cfg.iName, (int)cfg.iPrims, (int)side, (int)cfg.iBinds, (int)cfg.iBlend,
			(int)cfg.iUploads, (int)(cfg.iUpSize > 0 ? cfg.iUpSize : KBenchTexSz),
			(int)cfg.iBatch, (int)cfg.iPal, (int)cfg.iRead,
			(int)(cfg.iBatch > 0 ? cfg.iBatch : cfg.iPrims),
			(int)(cfg.iBatch > 0 ? cfg.iPrims * 6 : cfg.iPrims * 4),
			us, (unsigned)(cfg.iPrims * side * side), (unsigned)glGetError());
		}

	glDisable(GL_BLEND);
	glDeleteTextures(KBenchTex, tex);
	glDeleteTextures(1, &upTex);
	User::Free(strip); User::Free(stripT); User::Free(list);
	User::Free(listT); User::Free(listS); User::Free(texbuf);
	ps1LogFmt("glbench", "done");
	return 0;
	}

// --- Frame counter overlay --------------------------------------------------

// 5x7 font, only characters appearing in metrics string. Drawn directly, not via
// bitgdi text: under direct-FB we write to screen buffer bypassing it, so its
// text would go to another buffer, and separating the two paths would be needed anyway.
static const char KOvlChars[] = "0123456789. fpsm";
static const TUint8 KOvlFont[16][7] =
	{
	{ 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E },   // 0
	{ 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E },   // 1
	{ 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F },   // 2
	{ 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E },   // 3
	{ 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 },   // 4
	{ 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E },   // 5
	{ 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E },   // 6
	{ 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 },   // 7
	{ 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E },   // 8
	{ 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C },   // 9
	{ 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C },   // .
	{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00 },   // space
	{ 0x06,0x09,0x08,0x1C,0x08,0x08,0x08 },   // f
	{ 0x00,0x00,0x1E,0x11,0x1E,0x10,0x10 },   // p
	{ 0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E },   // s
	{ 0x00,0x00,0x1A,0x15,0x15,0x15,0x15 }    // m
	};

static const TInt KOvlScale = 2;    // 5x7 on 320x240 is unreadable without scaling
static const TInt KOvlCellW = 6 * KOvlScale;
static const TInt KOvlCellH = 8 * KOvlScale;
static const TInt KOvlCols  = 9;    // "999.9 fps" - longest string
static const TInt KOvlPad   = 2;

static TBool g_noUpdate;    // probe: skip screen Update() (see setter below)
static TBool g_smoothDown = ETrue;   // box filter downscaling (see blitCurrent)
static TInt  g_fxaaThr;              // lower FXAA threshold in green steps; 0 - disabled
static TInt  g_fxaaMax = 31;         // upper: above this untouched (UI text)

static TInt g_ovlOn;
static TInt g_ovlFpsX10 = -1;
static TInt g_ovlMsX10;
static unsigned int g_presents;   // frames actually presented to screen

// Show overlay. Numbers calculated by stage1 from presented frame counter
// (voutSymbianPresents) and its own frame time.
extern "C" void voutSymbianSetOverlay(int aOn)
	{
	g_ovlOn = aOn ? 1 : 0;
	}

extern "C" void voutSymbianSetStats(int aFpsX10, int aMsX10)
	{
	g_ovlFpsX10 = aFpsX10;
	g_ovlMsX10 = aMsX10;
	}

static void ovlFillRect(TUint8 *aDst, TInt aStrideB, TInt aPixB, TInt aX, TInt aY,
                        TInt aW, TInt aH, TUint32 aCol)
	{
	for (TInt y = 0; y < aH; y++)
		{
		TUint8 *row = aDst + (aY + y) * aStrideB + aX * aPixB;
		if (aPixB == 4)
			{
			TUint32 *d = (TUint32 *)row;
			for (TInt x = 0; x < aW; x++) d[x] = aCol;
			}
		else
			{
			TUint16 *d = (TUint16 *)row;
			for (TInt x = 0; x < aW; x++) d[x] = (TUint16)aCol;
			}
		}
	}

static void ovlText(TUint8 *aDst, TInt aStrideB, TInt aPixB, TInt aX, TInt aY,
                    const char *aStr, TUint32 aCol)
	{
	for (; *aStr; aStr++, aX += KOvlCellW)
		{
		const char *p = strchr(KOvlChars, *aStr);
		if (!p)
			continue;   // unknown character leaves empty cell
		const TUint8 *g = KOvlFont[p - KOvlChars];
		for (TInt gy = 0; gy < 7; gy++)
			{
			const TUint8 bits = g[gy];
			if (!bits)
				continue;
			for (TInt gx = 0; gx < 5; gx++)
				if (bits & (0x10 >> gx))
					ovlFillRect(aDst, aStrideB, aPixB, aX + gx * KOvlScale,
						aY + gy * KOvlScale, KOvlScale, KOvlScale, aCol);
			}
		}
	}

// Metrics line in top-left corner of destination. Clipping to screen size
// is required: under direct-FB destination is LCD buffer, and writing past it would go
// into foreign memory. Background is solid, because black bars are redrawn only on
// geometry change - without it previous digits would remain in place.
static void drawOverlay(TUint8 *aDst, TInt aStrideB, TInt aPixB, TInt aSW, TInt aSH)
	{
	if (!g_ovlOn || g_ovlFpsX10 < 0)
		return;
	const TInt boxW = KOvlCols * KOvlCellW + 2 * KOvlPad;
	const TInt boxH = 2 * KOvlCellH + 2 * KOvlPad;
	if (aSW < boxW || aSH < boxH)
		return;

	char l1[16], l2[16];
	snprintf(l1, sizeof(l1), "%d.%d fps", g_ovlFpsX10 / 10, g_ovlFpsX10 % 10);
	snprintf(l2, sizeof(l2), "%d.%d ms", g_ovlMsX10 / 10, g_ovlMsX10 % 10);

	const TUint32 white = (aPixB == 4) ? 0x00ffffffu : 0xffffu;
	ovlFillRect(aDst, aStrideB, aPixB, 0, 0, boxW, boxH, 0);
	ovlText(aDst, aStrideB, aPixB, KOvlPad, KOvlPad, l1, white);
	ovlText(aDst, aStrideB, aPixB, KOvlPad, KOvlPad + KOvlCellH, l2, white);
	}

// Scales current g_vout into g_bmp (nearest-neighbor, aspect ratio preservation,
// black bars) and blits into window.
static void blitCurrent()
	{
	const TUint8 *vbytes = (const TUint8 *)g_vout.vram;
	if (!vbytes)
		return;

	TInt startByte = 0;
	TInt sw = g_scr.iWidth, sh = g_scr.iHeight;
	const TBool rgb24 = g_vout.rgb24;
	if (g_vout.w > 0 && g_vout.h > 0)
		{
		startByte = g_vout.offset;
		sw = g_vout.w;
		sh = g_vout.h;
		}
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;
	const TInt maxW = rgb24 ? 682 : 1024;          // 2048 bytes per VRAM row
	if (sw > maxW) sw = maxW;
	const TInt startRow = startByte / 2048;
	if (sh > 512 - startRow) sh = 512 - startRow;  // do not read past 1024x512 VRAM bounds
	if (sh < 1) sh = 1;

	// Pump scheduler (DSA abort/restart) before picking target to avoid writing to
	// screen that WS has already reclaimed. Disabled in GUI (see g_pumpAo): there frame
	// ends with return to scheduler anyway, and on menu exit this blit runs
	// FROM WITHIN key handler, so pumping from here means re-entering
	// active object for window events. It will not re-arm afterwards, and focused keys
	// will not arrive at all - game remains running, but unresponsive.
	if (g_pumpAo && g_useDsa && CActiveScheduler::Current())
		{ TInt _e; while (CActiveScheduler::RunIfReady(_e, CActive::EPriorityIdle)) { } }
	// rgb24 is not allowed into direct buffer: that branch writes 16-bit words and is
	// untested on hardware, and a bug here means writing out of screen buffer bounds.
	const TBool directFb = (g_useDsa && g_dsaActive && g_fbOk && !g_useRowCopy &&
	                        !rgb24 && dsaCoversScreen());

	// Fit source into screen preserving aspect ratio. Do NOT rotate orientation
	// ourselves: device handles window auto-rotation via accelerometer. In directFb target
	// directly into physical LCD buffer (dimensions from HAL).
	const TInt SW = directFb ? g_fbW : g_scr.iWidth;
	const TInt SH = directFb ? g_fbH : g_scr.iHeight;

	// Interlaced modes (hi-res, sh>256) place fields in adjacent VRAM rows, so
	// linear reading produces combing artifacts. Take one field (even rows); calculate aspect ratio
	// using full height, because on hardware field is stretched over entire frame.
	const TBool field = (sh > 256);
	const TInt srcRows = field ? sh / 2 : sh;

	// GL ES path (MBX): GPU scales and presents (eglSwapBuffers). Replaces entire
	// bitmap+present below. rgb24 is kept on bitmap path for now (untested).
	// No overlay here: destination is GL texture, not linear buffer. Path is already
	// 4x slower than blit, so duplicating font for it is pointless.
	if (g_glReady && !rgb24)
		{
		glPresentFrame(vbytes, startByte, sw, sh, field, srcRows);
		return;
		}

	// Target is largest 4:3 rectangle fitting on screen, and source
	// is stretched into it COMPLETELY, regardless of its size. PS1 pixels are
	// not square: 256x240, 320x240, 512x240 and 640x480 are same full-screen TV
	// image, differing only in density. Aspect based on source size produced narrow image
	// in center of screen in 256-pixel modes (NFS menus, Colony Wars gameplay).
	// Colony Wars).
	TInt dstW = SW;
	TInt dstH = SW * 3 / 4;
	if (dstH > SH) { dstH = SH; dstW = SH * 4 / 3; }
	if (dstW > SW) dstW = SW;
	if (dstW < 1) dstW = 1;
	if (dstH < 1) dstH = 1;
	const TInt ox = (SW - dstW) / 2;
	const TInt oy = (SH - dstH) / 2;

	const TUint _fc0 = User::FastCounter();
	// Address rows in BYTES: screen buffer may be 32-bit (measured:
	// EColor16MU, stride 1280B at 320 pixels), while our bitmap is 16-bit.
	TUint8 *dstB;
	TInt dstStrideB, pixB;
	if (directFb)
		{
		refreshFbFromPlatform();    // driver may flip page between frames
		dstB = (TUint8 *)g_fbBase;  // directly into LCD - merge fill and present in single pass
		dstStrideB = g_fbStrideB;
		pixB = g_fbPixBytes;
		}
	else
		{
		// The bitmap is dropped when Create fails on low memory or a rotation,
		// and this is the only path that touches it.
		if (!g_bmp)
			return;
		g_bmp->LockHeap();
		dstB = (TUint8 *)g_bmp->DataAddress();
		dstStrideB = g_bmp->DataStride();
		pixB = 2;
		}
	// Black bars only when geometry changed: otherwise they are already black from previous
	// frame, and image is overwritten anyway. Eliminates ~150KB/frame memset.
	if (ox != g_blitOx || oy != g_blitOy || dstW != g_blitW || dstH != g_blitH)
		{
		Mem::FillZ(dstB, dstStrideB * SH);
		g_blitOx = ox; g_blitOy = oy; g_blitW = dstW; g_blitH = dstH;
		}

	// Nearest-neighbor via 16.16 fixed-point: accumulator instead of division for
	// every pixel. ARM11 lacks fast hardware division, and profiling showed
	// that blit with per-pixel division consumed ~half of frame time.
	const TInt srcH = field ? srcRows : sh;           // source rows for step
	const TUint sxStep = ((TUint)sw << 16) / (TUint)dstW;
	const TUint syStep = ((TUint)srcH << 16) / (TUint)dstH;

	// Downscaling >1.5x by simple decimation drops every second column, and with
	// it thin font strokes: in 640x480 step is exactly 2.0. Interlaced
	// modes additionally hold half the rows in second field, which we do not read
	// at all. So take average of 2x2 block - that is the resolution we actually
	// display. hAvg requires >1.5x downscaling, so sx+1 is guaranteed within row.
	// With "Skip lines" there is no vertical averaging: rasterizer does not overwrite
	// odd lines, so they contain leftovers from previous frames.
	const TBool hAvg = g_smoothDown && !rgb24 && sxStep > 0x18000u;
	const TBool vAvg = g_smoothDown && !rgb24 && field && !g_ilaceOut;
	const TInt  hOff = hAvg ? 1 : 0;
	// FXAA makes sense only at 1:1: there every source pixel maps to its screen pixel,
	// and aliasing is visible as-is. Where downscaling occurs, box filter already smooths.
	const TBool fxaa = g_fxaaThr > 0 && !rgb24 && sxStep == 0x10000u && !vAvg && dstW > 2;

	// Previous source row and where it was placed: when next destination row
	// reads same source (upscaling or line skipping), copying ready row
	// is cheaper than re-converting.
	TInt prevSsy = -1;
	const TUint16 *prevDrow = NULL;

	for (TInt dy = 0; dy < dstH; dy++)
		{
		const TInt srcLine = (TInt)((syStep * (TUint)dy) >> 16);
		TInt ssy = field ? srcLine * 2 : srcLine;
		// Rasterizer with "Skip lines" never overwrites odd VRAM lines, so
		// they contain content of previous frames (static overlays left traces).
		// Take even row instead of stale odd row.
		if (g_ilaceOut && !field)
			ssy &= ~1;
		TUint16 *drow = (TUint16 *)(dstB + (oy + dy) * dstStrideB + ox * pixB);
		// Copy ready row only into bitmap. With direct-FB destination is screen buffer,
		// and reading from it is not measured: only linear WRITE is measured. Uncached
		// read would cost full memory latency, so convert again there.
		if (ssy == prevSsy && prevDrow && !directFb)
			{
			Mem::Copy(drow, prevDrow, dstW * pixB);
			continue;
			}
		prevSsy = ssy; prevDrow = drow;
		TUint sxf = 0;
		if (rgb24)
			{
			// RGB24: 3 bytes per pixel (R,G,B). Not yet verified on hardware.
			const TUint8 *srow = vbytes + startByte + ssy * 2048;
			for (TInt dx = 0; dx < dstW; dx++)
				{
				const TUint8 *p = srow + (sxf >> 16) * 3;
				drow[dx] = (TUint16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
				sxf += sxStep;
				}
			}
		else
			{
			const TUint16 *srow = (const TUint16 *)(vbytes + startByte) + ssy * 1024;
			if (fxaa)
				{
				// Adjacent VRAM rows; on first and last take own row.
				const TUint16 *rowU = (ssy > 0)      ? srow - 1024 : srow;
				const TUint16 *rowD = (ssy + 1 < sh) ? srow + 1024 : srow;
				const TInt thr = g_fxaaThr, mx = g_fxaaMax;
				if (pixB == 4)
					{
					TUint32 *d32 = (TUint32 *)drow;
					d32[0] = Bgr555ToXrgb8888(srow[0]);
					for (TInt dx = 1; dx < dstW - 1; dx++)
						d32[dx] = Bgr555ToXrgb8888(FxaaPixel(srow, rowU, rowD, dx, thr, mx));
					d32[dstW - 1] = Bgr555ToXrgb8888(srow[dstW - 1]);
					}
				else
					{
					drow[0] = Bgr555ToRgb565(srow[0]);
					for (TInt dx = 1; dx < dstW - 1; dx++)
						drow[dx] = Bgr555ToRgb565(FxaaPixel(srow, rowU, rowD, dx, thr, mx));
					drow[dstW - 1] = Bgr555ToRgb565(srow[dstW - 1]);
					}
				}
			else if (hAvg || vAvg)
				{
				// The averaged pair must stay inside the display area, as in the fxaa branch.
				const TUint16 *s2 = (vAvg && ssy + 1 < sh) ? srow + 1024 : srow;
				if (pixB == 4)
					{
					TUint32 *d32 = (TUint32 *)drow;
					for (TInt dx = 0; dx < dstW; dx++, sxf += sxStep)
						{
						const TInt sx = (TInt)(sxf >> 16);
						d32[dx] = Bgr555ToXrgb8888(Avg555(
							Avg555(srow[sx], srow[sx + hOff]),
							Avg555(s2[sx],   s2[sx + hOff])));
						}
					}
				else
					{
					for (TInt dx = 0; dx < dstW; dx++, sxf += sxStep)
						{
						const TInt sx = (TInt)(sxf >> 16);
						drow[dx] = Bgr555ToRgb565(Avg555(
							Avg555(srow[sx], srow[sx + hOff]),
							Avg555(s2[sx],   s2[sx + hOff])));
						}
					}
				}
			// 32-bit screen buffer: convert directly to XRGB8888, without intermediate
			// 16-bit bitmap and without 565->8888 conversion in bitgdi.
			else if (pixB == 4)
				{
				TUint32 *d32 = (TUint32 *)drow;
				if (sxStep == 0x10000u)
					blitRow8888(d32, srow, dstW);
				else
					{
					for (TInt dx = 0; dx < dstW; dx++)
						{
						d32[dx] = Bgr555ToXrgb8888(srow[sxf >> 16]);
						sxf += sxStep;
						}
					}
				}
			// 1:1 horizontal scale - predominant case: PS1 output (256 or
			// 320) after fitting to 320x240 gives step exactly 0x10000. Then neither
			// accumulator nor shift is needed, and two pixels convert together
			// in 32-bit register: 2x fewer memory accesses and 2x
			// fewer operations per pixel.
			else if (sxStep == 0x10000u &&
				!(((TUint)srow | (TUint)drow) & 3) && !(dstW & 1))
				{
				// dstW is even and >=2 (branch condition), so counter is guaranteed >=1.
				blitRow565((TUint32 *)drow, (const TUint32 *)srow, dstW >> 1);
				}
			else if (sxStep == 0x10000u)
				{
				for (TInt dx = 0; dx < dstW; dx++)
					drow[dx] = Bgr555ToRgb565(srow[dx]);
				}
			else
				{
				for (TInt dx = 0; dx < dstW; dx++)
					{
					drow[dx] = Bgr555ToRgb565(srow[sxf >> 16]);
					sxf += sxStep;
					}
				}
			}
		}
	// After conversion, before present: overlay must sit on top of frame rather than being
	// overwritten by it. Cost is included in measured blit time intentionally - it is real.
	drawOverlay(dstB, dstStrideB, pixB, SW, SH);
	if (!directFb)
		g_bmp->UnlockHeap();
	const TUint _fc1 = User::FastCounter();
	g_blitFillTicks += (unsigned)(_fc1 - _fc0);

	if (directFb)
		{
		// Frame is already in screen buffer - present copy is not needed. Request update
		// with EXPLICIT region: we wrote bypassing bitgdi, so its own "dirty" region
		// remains empty and Update() without argument might push nothing.
		CFbsScreenDevice *sd = g_noUpdate ? NULL : (g_dsa ? g_dsa->ScreenDevice() : NULL);
		if (sd)
			{
			const RRegion *rgn = g_dsa->DrawingRegion();
			if (rgn && !rgn->CheckError()) sd->Update(*rgn);
			else                           sd->Update();
			}
		}
	// Row copy makes sense only when bitmap and screen formats match.
	// On this device screen is 32-bit, so path is inactive - remains as fallback
	// for devices with 16-bit buffer.
	else if (g_useRowCopy && g_dsaActive && g_fbOk && g_fbPixBytes == 2 &&
	         dsaCoversScreen())
		{
		// Row-by-row copy of bitmap into screen buffer. Measured that client-server
		// WS mechanics cost ~zero (our own bitgdi blit without IPC turned out MORE EXPENSIVE
		// than window path), so present is the cost of bitgdi copy procedure itself.
		// Flat Mem::Copy does the same work without it.
		g_bmp->LockHeap();
		const TUint8 *src = (const TUint8 *)g_bmp->DataAddress();
		const TInt sstride = g_bmp->DataStride();
		const TInt rows  = SH < g_fbH ? SH : g_fbH;
		const TInt bytes = (SW < g_fbW ? SW : g_fbW) * 2;
		TUint8 *dstp = (TUint8 *)g_fbBase;
		for (TInt y = 0; y < rows; y++)
			Mem::Copy(dstp + y * g_fbStride * 2, src + y * sstride, bytes);
		g_bmp->UnlockHeap();
		CFbsScreenDevice *sd = g_dsa->ScreenDevice();
		if (sd) sd->Update();
		}
	else if (g_useDsa && g_dsaActive && g_dsaGc)
		{
		g_dsaGc->BitBlt(TPoint(0, 0), g_bmp);
		CFbsScreenDevice *sd = g_dsa->ScreenDevice();
		if (sd) sd->Update();           // push to screen buffer
		}
	else if (g_ownWindow)
		{
		g_winp->Invalidate();
		g_winp->BeginRedraw();
		g_gc->Activate(*g_winp);
		g_gc->BitBlt(TPoint(0, 0), g_bmp);
		g_gc->Deactivate();
		g_winp->EndRedraw();
		g_wsp->Flush();
		}
	else
		{
		// CONE window: draw outside BeginRedraw/EndRedraw pair. With Invalidate
		// framework would respond with own redraw (container Draw() is
		// empty) and clear just placed frame with background color.
		g_gc->Activate(*g_winp);
		g_gc->BitBlt(TPoint(0, 0), g_bmp);
		g_gc->Deactivate();
		g_wsp->Flush();
		}
	g_blitPresentTicks += (unsigned)(User::FastCounter() - _fc1);
	}

// Init/Present/Finish wrapped in TRAP: display failure should not crash the run
// (numbers from log are valuable on their own).
extern "C" int voutSymbianInit(void)
	{
	if (g_wsReady)
		return KErrNone;
	TInt err;
	TRAP(err, initL());
	return err;
	}

// Foreign window attachment (AVKON shell): draw into CCoeControl window and
// take session/device/context from CCoeEnv. Replacement for initL in GUI branch - we do not
// create our own RWsSession/RWindowGroup/RWindow, so we do not close them in Finish.
void voutSymbianAttachWindow(RWsSession &aWs, CWsScreenDevice &aDev, RWindow &aWin,
                             CWindowGc &aGc)
	{
	// Called also on orientation change, so detach previous one first.
	if (g_eglDpy != EGL_NO_DISPLAY)
		glShutdown();

	g_ownWindow = EFalse;
	g_wsp = &aWs;
	g_winp = &aWin;
	g_dev = &aDev;
	g_gc = &aGc;
	g_scr = aWin.Size();

	delete g_bmp; g_bmp = NULL;
	g_bmp = new CFbsBitmap;
	if (!g_bmp || g_bmp->Create(g_scr, EColor64K) != KErrNone)
		{ delete g_bmp; g_bmp = NULL; return; }
	g_blitOx = -1;
	g_blitFillTicks = g_blitPresentTicks = 0;

	// Input arrives from CONE (OfferKeyEventL -> voutSymbianKeyEvent), so event queue
	// is not armed here - otherwise two readers of same session would split events.
	g_pad = 0;

	g_glReady = EFalse;
	if (g_useGl)
		{
		TRAPD(ge, { if (!glInitOnWindow()) glShutdown(); });
		if (ge != KErrNone) { glShutdown(); }
		}

	// DSA in GUI branch. Previously it started only in initL (console path), so
	// "gui ... dsa" runs silently fell back to window BitBlt: dsa active=0 in log.
	// Also called on screen rotation, so detach previous first.
	if (g_dsa) { delete g_dsa; g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; }
	if (g_useDsa)
		{
		// Background fill and redraw store give WS the right to redraw window over frame
		// that we place in screen bypassing it. No argument = "do not clear".
		aWin.SetBackgroundColor();
		aWin.EnableRedrawStore(EFalse);
		aWin.SetNonFading(ETrue);
		if (!CActiveScheduler::Current())
			{
			g_dsaSched = new CActiveScheduler;
			if (g_dsaSched) CActiveScheduler::Install(g_dsaSched);
			}
		TRAPD(e, {
			g_dsa = CDirectScreenAccess::NewL(aWs, aDev, aWin, g_dsaObs);
			dsaStartL();
			});
		if (e != KErrNone)
			{ delete g_dsa; g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; }
		}

	g_wsReady = ETrue;
	}

// Key from CONE: updates g_pad (DKEY_* bits, active-high). Returns 1 if
// code is mapped to a PS1 button (so container consumes event).
extern "C" int voutSymbianKeyEvent(int aScanCode, int aDown)
	{
	const unsigned short m = maskForScan(aScanCode);
	if (!m)
		return 0;
	padApply(m, aDown);
	return 1;
	}

// Layout from menu: one primary and one secondary keycode per button in order
// of KPadBit (-1 = unassigned), aToggle - DKEY_* bitmask for toggle buttons.
extern "C" void voutSymbianSetKeyMap(const int *aScan, const int *aAlt, int aCount,
                                     unsigned int aToggle)
	{
	keyMapDefaults();
	const TInt n = aCount < KPadButtons ? aCount : KPadButtons;
	for (TInt i = 0; i < n; i++)
		{
		if (aScan) g_keyScan[i] = (short)aScan[i];
		if (aAlt)  g_keyAlt[i]  = (short)aAlt[i];
		}
	g_padToggle = (unsigned short)aToggle;
	// A toggle active before layout change would remain pressed forever.
	g_pad = 0;
	}

extern "C" int voutSymbianPadButtons(void)
	{
	return KPadButtons;
	}

// Default keycode for button: menu constructs its own defaults from it,
// so layout is defined in one place.
extern "C" int voutSymbianDefaultKey(int aIdx, int aAlt)
	{
	if (aIdx < 0 || aIdx >= KPadButtons)
		return -1;
	return aAlt ? KPadDefAlt[aIdx] : KPadDefScan[aIdx];
	}

// DKEY_* bit of button by its index in menu.
extern "C" int voutSymbianPadBit(int aIdx)
	{
	return (aIdx >= 0 && aIdx < KPadButtons) ? KPadBit[aIdx] : -1;
	}

// Non-blocking key polling: drains window event queue, updates g_pad
// (DKEY_* bits, active-high) on presses/releases, returns current state.
// Without CActiveScheduler: checks armed TRequestStatus directly.
extern "C" unsigned short voutSymbianPollPad(void)
	{
	if (!g_wsReady)
		return 0;
	if (!g_ownWindow)
		{ tiltPad(); return g_pad; }   // events are dispatched by CONE, queue not armed here
	while (g_evStat != KRequestPending)
		{
		TWsEvent ev;
		g_ws.GetEvent(ev);
		const TInt t = ev.Type();
		if (t == EEventKeyDown || t == EEventKeyUp)
			{
			const unsigned short m = maskForScan(ev.Key()->iScanCode);
			if (m)
				padApply(m, t == EEventKeyDown);
			}
		g_ws.EventReady(&g_evStat);
		}
	tiltPad();
	return g_pad;
	}

extern "C" void voutSymbianPresent(void)
	{
	if (!g_wsReady)
		return;
	// Keep g_vout synchronized with current display area ALWAYS, even when
	// we do not intend to present. Otherwise on frame start address change we would show
	// old area - i.e. foreign buffer rather than frozen frame.
	g_voutSelfCall = 1;
	vout_update(&gpu, gpu.screen.src_x, gpu.screen.src_y);
	g_voutSelfCall = 0;

	// Skip gpulib rasterization: present ONLY frames it actually
	// rendered. Indicated by latch set by its own vout_update call
	// (GPUupdateLace exits early for skipped frames, gpu.c:1181-1185). Reasons (2)
	// and (3) below do not apply here: game flips buffers for unrendered frames too, so
	// they would display an empty buffer - that was the black screen with frameskip.
	if (gpu.frameskip.set && !g_voutPending)
		return;

	if (g_dirtyOnly)
		{
		// Three independent reasons to present frame:
		//  1) gpulib reported new visible area content;
		//  2) display area itself changed (game flips ready buffers without
		//     drawing anything - fb_dirty_display_area is not set, so no signal);
		//  3) staleness ceiling - safeguard against any missed signal
		//     and foreign drawing on top of our frame under DSA.
		// Measured on GT: 50% of frames update in racing phase; in menus there were
		// 300-frame windows where game drew only to invisible buffer without flipping -
		// there is truly nothing to show there, and (3) keeps screen from looking dead.
		const TBool areaChanged = (g_vout.offset != g_lastShownOffset ||
		                           g_vout.w != g_lastShownW ||
		                           g_vout.h != g_lastShownH);
		if (!g_voutPending && !areaChanged && ++g_voutStale < 10)
			return;
		}
	g_lastShownOffset = g_vout.offset;
	g_lastShownW = g_vout.w;
	g_lastShownH = g_vout.h;
	g_voutPending = 0;
	g_voutStale = 0;
	g_presents++;
	TRAP_IGNORE(blitCurrent());
	}

// Frames actually presented to screen: anything returning early above
// (skipped rasterization, "dirty only") is not counted here. This is the exact
// number seen by the eye, and goes to overlay.
extern "C" unsigned int voutSymbianPresents(void)
	{
	return g_presents;
	}

// Pause video output while in game menu. Tear down DSA: while it holds screen, window
// drawing would go underneath it. Re-enable on resume and immediately restore
// frame - "dirty only" latch would otherwise keep menu on screen until game
// redraws visible area, which on static scene takes long time.
extern "C" void voutSymbianSuspend(int aOn)
	{
	if (!g_wsReady)
		return;
	if (aOn)
		{
		// Keys held at menu opening will not receive release events:
		// menu consumes events. Toggles are retained - that is their purpose.
		g_pad &= g_padToggle;
		if (g_dsa && g_dsaActive)
			{ g_dsa->Cancel(); g_dsaActive = EFalse; }
		ps1LogFmt("dsa", "suspend on dsa=%d", (int)(g_dsa != NULL));
		return;
		}
	TInt se = KErrNone;
	if (g_dsa && !g_dsaActive)
		TRAP(se, dsaStartL());
	ps1LogFmt("dsa", "suspend off err=%d active=%d", (int)se, (int)g_dsaActive);
	g_blitOx = -1;
	g_voutPending = 1;
	g_voutStale = 0;
	TInt be = KErrNone;
	if (g_bmp)
		TRAP(be, blitCurrent());
	ps1LogFmt("dsa", "suspend done blit=%d", (int)be);
	}

extern "C" int voutSymbianFinish(int holdMs)
	{
	if (!g_wsReady)
		return KErrNotReady;
	if (holdMs < 0) holdMs = 0;
	if (holdMs > 30000) holdMs = 30000;
	if (holdMs > 0 && g_ownWindow)
		User::After(holdMs * 1000);
	if (g_ownWindow)
		g_ws.EventReadyCancel();   // cancel armed event request before closing
	// GL holds EGL surface on window - destroy before closing window.
	if (g_eglDpy != EGL_NO_DISPLAY) glShutdown();
	// DSA holds window/device - destroy it first.
	if (g_dsa) { delete g_dsa; g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; }
	delete g_bmp; g_bmp = NULL;
	if (g_ownWindow)
		{
		delete g_gc;  g_gc = NULL;
		g_win.Close();
		g_grp.Close();
		delete g_dev; g_dev = NULL;
		g_ws.Close();
		}
	else
		{
		// Session, device, context, and window belong to CCoeEnv/container - only
		// release pointers and return to our own (console) layout.
		g_gc = NULL; g_dev = NULL;
		g_ownWindow = ETrue;
		g_wsp = &g_ws;
		g_winp = &g_win;
		}
	// Clean up scheduler only if created by DSA itself (otherwise it belongs to audio
	// and will be removed by ps1SndCloseOutput).
	if (g_dsaSched) { CActiveScheduler::Install(NULL); delete g_dsaSched; g_dsaSched = NULL; }
	g_wsReady = EFalse;
	return KErrNone;
	}

// One-shot display (gputest path: static image): init + one present + hold.
extern "C" int voutSymbianShow(int holdMs)
	{
	TInt r = voutSymbianInit();
	if (r != KErrNone)
		return r;
	voutSymbianPresent();
	return voutSymbianFinish(holdMs);
	}

// Probe: do not call screen Update() at all. Under direct-FB pixels are already in LCD
// buffer, so question is only how much call itself costs - and whether it blocks on
// vertical refresh. Not for normal use: without Update() some devices
// show nothing.
extern "C" void voutSymbianSetNoUpdate(int aOn)
	{
	g_noUpdate = aOn ? ETrue : EFalse;
	}

// FXAA threshold in green steps (0..31); 0 disables filter. Prototype: active only
// at 1:1 scale, i.e. in 320x240 3D mode.
extern "C" void voutSymbianSetFxaa(int aThreshold, int aMax)
	{
	g_fxaaThr = (aThreshold > 0 && aThreshold < 32) ? aThreshold : 0;
	g_fxaaMax = (aMax > 0 && aMax < 32) ? aMax : 31;
	}

// Block averaging on downscaling instead of decimation. Font legibility in
// 640x* modes depends entirely on this.
extern "C" TInt accelStart(void);
extern "C" void accelStop(void);
extern "C" TInt accelRead(TInt *aX, TInt *aY, TInt *aZ);

// Tilt is switched on and off from the menu; on a phone without the sensor the
// library simply does not load and the setting stays inert.
extern "C" int voutSymbianTiltActive(void)
	{
	return g_tilt ? 1 : 0;
	}

// Diagnostics: how many samples the sensor delivered, what the last one was, and
// what the zero point and the resulting pad bits are.
extern "C" int voutSymbianTiltStats(int *aX, int *aY, int *aZeroX, int *aZeroY, int *aPad)
	{
	if (aX) *aX = g_tiltLastX;
	if (aY) *aY = g_tiltLastY;
	if (aZeroX) *aZeroX = g_tiltZeroX;
	if (aZeroY) *aZeroY = g_tiltZeroY;
	if (aPad) *aPad = g_tiltPad;
	return g_tiltReads;
	}

extern "C" void voutSymbianSetTilt(int aOn)
	{
	if (aOn && !g_tilt)
		{
		g_tilt = (accelStart() == KErrNone);
		g_tiltZeroed = EFalse;
		g_tiltPad = 0;
		}
	else if (!aOn && g_tilt)
		{
		accelStop();
		g_tilt = EFalse;
		g_tiltPad = 0;
		}
	}

// One axis of the pad from one axis of the sensor, with hysteresis.
static void tiltAxis(TInt aVal, TInt aOn, TInt aOff, unsigned short aNeg, unsigned short aPos)
	{
	if (aVal <= -aOn) { g_tiltPad |= aNeg; g_tiltPad = (unsigned short)(g_tiltPad & ~aPos); }
	else if (aVal >= aOn) { g_tiltPad |= aPos; g_tiltPad = (unsigned short)(g_tiltPad & ~aNeg); }
	else if (aVal > -aOff && aVal < aOff)
		g_tiltPad = (unsigned short)(g_tiltPad & ~(aNeg | aPos));
	}

// One axis of the stick from one axis of the sensor: 0..255 with 127 the centre.
static unsigned char tiltStick(TInt aVal, TInt aDead, TInt aSpan)
	{
	if (aVal > -aDead && aVal < aDead)
		return 127;
	TInt v = aVal + ((aVal > 0) ? -aDead : aDead);
	const TInt span = aSpan - aDead;
	if (v > span) v = span;
	if (v < -span) v = -span;
	return (unsigned char)(127 - (v * 127) / span);
	}

// Measured on the N95 held for play: tilting the phone left drives Y positive and
// tipping its far edge down drives X positive, so the pad follows the tilt the way
// a marble on the screen would. The pitch axis wanders about a hundred units while
// the phone is merely held, hence its own, larger threshold.
static unsigned short tiltPad(void)
	{
	if (!g_tilt)
		return 0;
	TInt x = 0, y = 0, z = 0;
	g_tiltReads = accelRead(&x, &y, &z);
	if (g_tiltReads <= 0)
		return 0;
	g_tiltLastX = x;
	g_tiltLastY = y;
	if (!g_tiltZeroed)
		{ g_tiltZeroX = x; g_tiltZeroY = y; g_tiltZeroed = ETrue; }
	tiltAxis(y - g_tiltZeroY, KTiltOn, KTiltOff,
		(unsigned short)(1 << DKEY_RIGHT), (unsigned short)(1 << DKEY_LEFT));
	tiltAxis(x - g_tiltZeroX, KTiltPitchOn, KTiltPitchOff,
		(unsigned short)(1 << DKEY_DOWN), (unsigned short)(1 << DKEY_UP));
	g_tiltAX = tiltStick(y - g_tiltZeroY, KTiltDead, KTiltSpan);
	g_tiltAY = tiltStick(x - g_tiltZeroX, KTiltPitchDead, KTiltPitchSpan);
	return g_tiltPad;
	}

// The stick the tilt drives, and the pad bits it would press if the game never
// leaves digital mode. Both come from the sample taken in the frame poll.
extern "C" int voutSymbianTiltAnalog(unsigned char *aX, unsigned char *aY)
	{
	if (!g_tilt || !g_tiltZeroed)
		return 0;
	if (aX) *aX = g_tiltAX;
	if (aY) *aY = g_tiltAY;
	return 1;
	}

extern "C" unsigned short voutSymbianTiltPadBits(void)
	{
	return g_tilt ? g_tiltPad : 0;
	}

extern "C" void voutSymbianSetSmooth(int aOn)
	{
	g_smoothDown = aOn ? ETrue : EFalse;
	}

// Enable DSA (direct screen access) for present. Call BEFORE voutSymbianInit.
extern "C" void voutSymbianSetDsa(int aOn)
	{
	g_useDsa = aOn ? ETrue : EFalse;
	}

// Paired with ps1SndSetPumpAo: in GUI the shell runs scheduler, and running it
// from blit is forbidden.
extern "C" void voutSymbianSetPumpAo(int aOn)
	{
	g_pumpAo = aOn ? ETrue : EFalse;
	}

// Enable GL ES output (MBX). Call BEFORE voutSymbianInit.
extern "C" void voutSymbianSetGl(int aOn)
	{
	g_useGl = aOn ? ETrue : EFalse;
	}

// Rasterizer runs in "Skip lines" mode: odd VRAM lines are stale.
extern "C" void voutSymbianSetIlaceOut(int aOn)
	{
	g_ilaceOut = aOn ? 1 : 0;
	}

// Present only frames for which gpulib reported a change.
extern "C" void voutSymbianSetDirtyOnly(int aOn)
	{
	g_dirtyOnly = aOn ? ETrue : EFalse;
	}

// Present via row-by-row copy to screen buffer instead of BitBlt. Requires DSA.
extern "C" void voutSymbianSetRowCopy(int aOn)
	{
	g_useRowCopy = aOn ? ETrue : EFalse;
	if (aOn)
		g_useDsa = ETrue;   // copy to screen without capturing screen is invalid
	}

// Where screen buffer base and stride were obtained from: 1 = from platform, 0 = from HAL.
extern "C" int voutSymbianFbFromPlatform(void)
	{
	return g_fbFromPlatform ? 1 : 0;
	}

// Frame geometry after scaling (Z3): what the game provides and where it is placed.
extern "C" void voutSymbianGeom(int *aSrcW, int *aSrcH, int *aDstW, int *aDstH,
                                int *aOx, int *aOy)
	{
	if (aSrcW) *aSrcW = g_vout.w;
	if (aSrcH) *aSrcH = g_vout.h;
	if (aDstW) *aDstW = g_blitW;
	if (aDstH) *aDstH = g_blitH;
	if (aOx)   *aOx   = g_blitOx;
	if (aOy)   *aOy   = g_blitOy;
	}

// Z1: screen layout FROM PLATFORM, not from HAL. CFbsScreenDevice::Bits()/Stride()
// are public (bitdev.h:145-146) and exported from bitgdi.dso; Stride() is
// driver's ScanLineBytes(), i.e. what platform itself draws with. Bits() may
// return NULL if driver does not implement MFastBlit2 - that is also an answer.
// Plus screen mode orientation: if not Normal, GC applies rotation, and raw
// linear write to HAL address produces skew - exactly what was seen on 2026-08-01.
extern "C" void voutSymbianScreenLayout(int *aBits, int *aStride, int *aW, int *aH,
                                        int *aMode, int *aRotation, int *aHalBase)
	{
	if (aBits)     *aBits     = 0;
	if (aStride)   *aStride   = 0;
	if (aW)        *aW        = 0;
	if (aH)        *aH        = 0;
	if (aMode)     *aMode     = -1;
	if (aRotation) *aRotation = -1;
	if (aHalBase)  *aHalBase  = (int)g_fbBase;

	CFbsScreenDevice *sd = (g_dsa && g_dsaActive) ? g_dsa->ScreenDevice() : NULL;
	if (sd)
		{
		if (aBits)   *aBits   = (int)sd->Bits();
		if (aStride) *aStride = sd->Stride();
		const TSize sz = sd->SizeInPixels();
		if (aW)      *aW      = sz.iWidth;
		if (aH)      *aH      = sz.iHeight;
		if (aMode)   *aMode   = (int)sd->DisplayMode();
		}
	if (g_dev && aRotation)
		{
		TPixelsAndRotation par;
		TRAPD(e, g_dev->GetScreenModeSizeAndRotation(g_dev->CurrentScreenMode(), par));
		if (e == KErrNone)
			*aRotation = (int)par.iRotation;
		}
	}

// Z1/M-RB: whether raw writes to HAL address reach scanned buffer and in which
// layout. Draw pattern via KNOWN WORKING path (DSA BitBlt), read back
// with raw pointer, and count matches. Interpretation: all match - HAL address is
// visible buffer; f(y,x) instead of f(x,y) - rotation; constant offset - different stride;
// nothing - double buffering or foreign buffer.
// aMode 0 = draw pattern and check, 1 = check only (across frames).
extern "C" int voutSymbianReadBack(int aMode, int *aMatch, int *aTotal, int *aFirstBad)
	{
	if (aMatch)   *aMatch   = 0;
	if (aTotal)   *aTotal   = 0;
	if (aFirstBad) { aFirstBad[0] = aFirstBad[1] = aFirstBad[2] = aFirstBad[3] = -1; }
	if (!g_fbOk || !g_fbBase || !g_bmp)
		return -1;

	const TInt W = g_fbW < g_scr.iWidth ? g_fbW : g_scr.iWidth;
	const TInt H = g_fbH < g_scr.iHeight ? g_fbH : g_scr.iHeight;
	if (W < 8 || H < 8)
		return -2;

	if (aMode == 0)
		{
		g_bmp->LockHeap();
		TUint16 *bits = (TUint16 *)g_bmp->DataAddress();
		const TInt bstride = g_bmp->DataStride() / 2;
		for (TInt y = 0; y < H; y++)
			for (TInt x = 0; x < W; x++)
				bits[y * bstride + x] = (TUint16)(x * 7 + y * 13 + 1);
		g_bmp->UnlockHeap();
		if (!g_dsaGc)
			return -3;
		g_dsaGc->BitBlt(TPoint(0, 0), g_bmp);
		CFbsScreenDevice *sd = g_dsa ? g_dsa->ScreenDevice() : NULL;
		if (sd) sd->Update();
		}

	TInt match = 0, total = 0, bad = 0;
	for (TInt gy = 0; gy < 8; gy++)
		for (TInt gx = 0; gx < 8; gx++)
			{
			const TInt x = gx * (W / 8) + 3;
			const TInt y = gy * (H / 8) + 3;
			if (x >= W || y >= H)
				continue;
			// Compare in 16 bits: bitmap is filled with RGB565, while screen buffer can
			// be 32-bit - then take same pixel in native format and
			// convert back to 565, otherwise "mismatch" would indicate only format difference.
			const TUint16 want = (TUint16)(x * 7 + y * 13 + 1);
			const TUint8 *rowp = (const TUint8 *)g_fbBase + y * g_fbStrideB;
			TUint16 got;
			if (g_fbPixBytes == 4)
				{
				const TUint32 v = ((const TUint32 *)rowp)[x];
				got = (TUint16)((((v >> 16) & 0xff) >> 3) << 11 |
				                (((v >> 8) & 0xff) >> 2) << 5 |
				                 ((v & 0xff) >> 3));
				}
			else
				got = ((const TUint16 *)rowp)[x];
			total++;
			if (want == got)
				match++;
			else if (!bad && aFirstBad)
				{
				aFirstBad[0] = x; aFirstBad[1] = y;
				aFirstBad[2] = want; aFirstBad[3] = got;
				bad = 1;
				}
			}
	if (aMatch) *aMatch = match;
	if (aTotal) *aTotal = total;
	return 0;
	}

// Whether GL is actually active (for log): 1 = present goes through GPU/EGL.
extern "C" int voutSymbianGlActive(void)
	{
	return g_glReady ? 1 : 0;
	}

// Whether DSA is actually active (for log): 1 = present goes through DSA.
extern "C" int voutSymbianDsaActive(void)
	{
	return (g_useDsa && g_dsaActive) ? 1 : 0;
	}

// Diagnostic test pattern directly into LCD buffer: blue background, white borders, multi-colored
// corners (RED=TL, GREEN=TR, bright-BLUE=BL, YELLOW=BR) and diagonal. What is visible identifies
// direct-FB issue precisely: offset=stride, empty=double buffer, mirrored=orientation.
extern "C" int voutSymbianFbTest(void)
	{
	if (!g_fbOk || !g_fbBase) return -1;
	const TInt W = g_fbW, H = g_fbH;
	TUint8 *fb = (TUint8 *)g_fbBase;
	// Colors defined in BGR555 and converted to buffer format: it can be either
	// 16-bit or 32-bit (on N95 8GB measured EColor16MU).
	const TUint16 BLUE = 0x7c00, WHITE = 0x7fff, RED = 0x001f,
	              GREEN = 0x03e0, YELLOW = 0x03ff, CYAN = 0x7fe0;
	for (TInt y = 0; y < H; y++)
		{
		TUint8 *row = fb + y * g_fbStrideB;
		for (TInt x = 0; x < W; x++)
			{
			TUint16 c = BLUE;
			if (x < 2 || x >= W - 2 || y < 2 || y >= H - 2) c = WHITE;      // border
			else if (x < 40 && y < 40) c = RED;                            // TL corner
			else if (x >= W - 40 && y < 40) c = GREEN;                     // TR corner
			else if (x < 40 && y >= H - 40) c = CYAN;                      // BL corner
			else if (x >= W - 40 && y >= H - 40) c = YELLOW;               // BR corner
			else if (x == y) c = WHITE;                                    // diagonal
			if (g_fbPixBytes == 4) ((TUint32 *)row)[x] = Bgr555ToXrgb8888(c);
			else                   ((TUint16 *)row)[x] = Bgr555ToRgb565(c);
			}
		}
	if (g_dsa && g_dsaActive)
		{ CFbsScreenDevice *sd = g_dsa->ScreenDevice(); if (sd) sd->Update(); }
	return 0;
	}

// Visual A/B for direct-FB: same image via two paths. aRaw=0 - through
// bitgdi (DSA BitBlt), i.e. known working path; aRaw=1 - raw write to
// same buffer. Colors differ so they are not confused: green field = bitgdi,
// red = raw write. What appears on panel answers
// whether raw write reaches display at all.
extern "C" int voutSymbianVisTest(int aRaw)
	{
	if (!g_fbOk || !g_fbBase || !g_bmp)
		return -1;
	const TInt W = aRaw ? g_fbW : g_scr.iWidth;
	const TInt H = aRaw ? g_fbH : g_scr.iHeight;
	if (W < 16 || H < 16)
		return -2;
	const TUint16 bg   = aRaw ? 0x001f : 0x03e0;   // BGR555: red / green
	const TUint16 fg   = 0x7fff;                   // white

	if (!aRaw)
		g_bmp->LockHeap();
	TUint8 *dst = aRaw ? (TUint8 *)g_fbBase : (TUint8 *)g_bmp->DataAddress();
	const TInt stride = aRaw ? g_fbStrideB : g_bmp->DataStride();
	const TInt pix    = aRaw ? g_fbPixBytes : 2;
	for (TInt y = 0; y < H; y++)
		{
		TUint8 *row = dst + y * stride;
		for (TInt x = 0; x < W; x++)
			{
			// Cross at center plus border: reveals both offset and rotation.
			const TBool cross = (x >= W / 2 - 3 && x <= W / 2 + 3) ||
			                    (y >= H / 2 - 3 && y <= H / 2 + 3);
			const TBool frame = (x < 3 || x >= W - 3 || y < 3 || y >= H - 3);
			const TUint16 c = (cross || frame) ? fg : bg;
			if (pix == 4) ((TUint32 *)row)[x] = Bgr555ToXrgb8888(c);
			else          ((TUint16 *)row)[x] = Bgr555ToRgb565(c);
			}
		}
	if (!aRaw)
		{
		g_bmp->UnlockHeap();
		if (!g_dsaGc)
			return -3;
		g_dsaGc->BitBlt(TPoint(0, 0), g_bmp);
		}
	CFbsScreenDevice *sd = g_dsa ? g_dsa->ScreenDevice() : NULL;
	if (sd)
		{
		const RRegion *rgn = g_dsa->DrawingRegion();
		if (rgn && !rgn->CheckError()) sd->Update(*rgn);
		else                           sd->Update();
		}
	return 0;
	}

// Dump of what is ACTUALLY in screen buffer. VRAM dump shows conversion
// input, so does not distinguish "game didn't draw" from "we didn't present".
// Here we read back what panel actually sees.
extern "C" int voutSymbianScreenDump(const char *aPath)
	{
	if (!g_fbOk || !g_fbBase)
		return -1;
	FILE *f = fopen(aPath, "wb");
	if (!f)
		return -2;
	char hdr[160];
	const int n = snprintf(hdr, sizeof(hdr),
		"PS1SCR %d %d %d %d vout=%d,%d,%d,%d off=%d rgb24=%d\n",
		g_fbW, g_fbH, g_fbStrideB, g_fbPixBytes,
		g_vout.x, g_vout.y, g_vout.w, g_vout.h, g_vout.offset, g_vout.rgb24);
	fwrite(hdr, 1, n, f);
	for (TInt y = 0; y < g_fbH; y++)
		fwrite((const TUint8 *)g_fbBase + y * g_fbStrideB, 1, g_fbW * g_fbPixBytes, f);
	fclose(f);
	return 0;
	}

// On-demand screenshot: what panel sees + what game rendered + display path state.
// Difference between two files localizes defect: valid VRAM and corrupted screen
// mean display output bug, corrupted VRAM - rasterization bug.
extern "C" void ps1ScreenShot(void)
	{
	const int sr = voutSymbianScreenDump("E:\\PS1\\logs\\ps1_scrkey.bin");
	const int vr = voutSymbianDump("E:\\PS1\\logs\\ps1_vramkey.bin");
	ps1LogFmt("screen_dump",
		"manual: scr=%d vram=%d vout=%d,%d,%d,%d off=%d rgb24=%d dsa=%d fbPlat=%d "
		"fb=%dx%d stride=%dB pix=%d mode=%d",
		sr, vr, g_vout.x, g_vout.y, g_vout.w, g_vout.h, g_vout.offset, g_vout.rgb24,
		(int)(g_useDsa && g_dsaActive), (int)g_fbFromPlatform,
		g_fbW, g_fbH, g_fbStrideB, g_fbPixBytes, g_fbMode);
	}

// Dump of buffer at HAL address - the one that (per 2026-08-01 measurements) is actually
// scanned by panel. Driver buffer (Bits()) and this one are different descriptions: driver says
// 1280 B/row and 32-bit, HAL - 640 B and 16. Comparing two dumps answers whether
// our frame reaches from driver buffer to scanned buffer.
// Raw screen buffer dump WITHOUT layout assumptions. Combined with
// x*7+y*13+1 pattern written via known working path (DSA BitBlt), allows determining
// bytes per pixel and row stride on host, without new device runs.
// aBytes is capped to what we already write per frame, i.e. definitely mapped.
extern "C" int voutSymbianRawDump(const char *aPath, int aBytes)
	{
	if (!g_fbOk || !g_fbBase)
		return -1;
	const int maxB = g_fbH * g_fbStrideB;
	if (aBytes <= 0 || aBytes > maxB)
		aBytes = maxB;
	FILE *f = fopen(aPath, "wb");
	if (!f)
		return -2;
	char hdr[160];
	const int n = snprintf(hdr, sizeof(hdr),
		"PS1RAW base=0x%x bytes=%d platStride=%d platPix=%d %dx%d hal=0x%x halLine=%d halBpp=%d %dx%d\n",
		(unsigned)g_fbBase, aBytes, g_fbStrideB, g_fbPixBytes, g_fbW, g_fbH,
		(unsigned)g_fbAddr, g_fbLineBytes, g_fbBpp, g_fbXpix, g_fbYpix);
	fwrite(hdr, 1, n, f);
	fwrite((const TUint8 *)g_fbBase, 1, aBytes, f);
	fclose(f);
	return 0;
	}

// Address separate from dump: if physical, reading will kill thread, and log should
// retain at least the number itself.
extern "C" int voutSymbianHalAddr(void)
	{
	return g_fbAddr;
	}

extern "C" int voutSymbianHalDump(const char *aPath)
	{
	if (!g_fbAddr || g_fbLineBytes < 2 || g_fbYpix < 1)
		return -1;
	FILE *f = fopen(aPath, "wb");
	if (!f)
		return -2;
	char hdr[160];
	const int n = snprintf(hdr, sizeof(hdr),
		"PS1HAL addr=0x%x bpp=%d line=%d first=%d %dx%d plat=0x%x platStride=%d platPix=%d\n",
		(unsigned)g_fbAddr, g_fbBpp, g_fbLineBytes, g_fbFirstOff, g_fbXpix, g_fbYpix,
		(unsigned)g_fbBase, g_fbStrideB, g_fbPixBytes);
	fwrite(hdr, 1, n, f);
	// Exactly as described by HAL: reading further goes out of mapping bounds.
	fwrite((const TUint8 *)g_fbAddr + g_fbFirstOff, 1, g_fbLineBytes * g_fbYpix, f);
	fclose(f);
	return 0;
	}

// Raw HAL screen buffer parameters + window size from WS (for direct-FB diagnostics).
extern "C" void voutSymbianFbInfo(int *aOk, int *aXpix, int *aYpix, int *aLineBytes,
                                  int *aBpp, int *aFirstOff, int *aWinW, int *aWinH)
	{
	if (aOk)        *aOk        = g_fbOk ? 1 : 0;
	if (aXpix)      *aXpix      = g_fbXpix;
	if (aYpix)      *aYpix      = g_fbYpix;
	if (aLineBytes) *aLineBytes = g_fbLineBytes;
	if (aBpp)       *aBpp       = g_fbBpp;
	if (aFirstOff)  *aFirstOff  = g_fbFirstOff;
	if (aWinW)      *aWinW      = g_scr.iWidth;
	if (aWinH)      *aWinH      = g_scr.iHeight;
	}

// Blit time breakdown in ms: fill = scaling loop, present = BitBlt+Flush.
extern "C" void voutSymbianBlitSplit(unsigned *aFillMs, unsigned *aPresentMs)
	{
	const unsigned f = ndrc_symbian_fastfreq();
	const unsigned freq = f ? f : 1;
	if (aFillMs)    *aFillMs    = (unsigned)(g_blitFillTicks * 1000ULL / freq);
	if (aPresentMs) *aPresentMs = (unsigned)(g_blitPresentTicks * 1000ULL / freq);
	}

// --- Wrappers for libpcsxcore -----------------------------------------------
// libpcsxcore/plugins.h declares GPUinit, GPUopen etc. as TYPES, whereas in gpulib these are
// functions with the same names. Including both headers in one file is not
// allowed, so rename here, where plugins.h is not needed.

extern "C" {

// GPU pipeline diagnostics: how many commands BIOS actually sends. Shows whether problem
// is command delivery (counters zero) or rasterization (non-zero, but VRAM
// black). Reset in ps1GpuInit (once per run).
static unsigned g_gpData, g_gpDataMem, g_gpDma, g_gpStatus;

long ps1GpuInit(void)
	{
	g_gpData = g_gpDataMem = g_gpDma = g_gpStatus = 0;
	const long r = GPUinit();
	// Skip odd lines when rasterizing interlaced (hi-res) content:
	// blitCurrent presents only even field anyway, so rendering odd lines is wasted.
	// Conditional on PSX_GPU_STATUS_INTERLACE (gpulib_if.cpp:116), so 240p is unaffected.
	// Placed AFTER GPUinit: it does memset(&gpu.state).
	gpu.state.downscale_enable = 1;
	return r;
	}
long ps1GpuShutdown(void)                               { return GPUshutdown(); }
long ps1GpuOpen(unsigned long *d, char *c, char *g)     { return GPUopen(d, c, g); }
long ps1GpuClose(void)                                  { return GPUclose(); }
void ps1GpuWriteStatus(uint32_t d)                      { g_gpStatus++; GPUwriteStatus(d); }
void ps1GpuWriteData(uint32_t d)                        { g_gpData++; GPUwriteData(d); }
void ps1GpuWriteDataMem(uint32_t *m, int c)             { g_gpDataMem++; GPUwriteDataMem(m, c); }
uint32_t ps1GpuReadStatus(void)                         { return GPUreadStatus(); }
uint32_t ps1GpuReadData(void)                           { return GPUreadData(); }
void ps1GpuReadDataMem(uint32_t *m, int c)              { GPUreadDataMem(m, c); }
long ps1GpuDmaChain(uint32_t *rb, uint32_t sa, uint32_t *pa, int32_t *cy)
                                                        { g_gpDma++; return GPUdmaChain(rb, sa, pa, cy); }

void voutSymbianGpuStats(unsigned *aData, unsigned *aDataMem, unsigned *aDma, unsigned *aStatus)
	{
	if (aData)    *aData = g_gpData;
	if (aDataMem) *aDataMem = g_gpDataMem;
	if (aDma)     *aDma = g_gpDma;
	if (aStatus)  *aStatus = g_gpStatus;
	}
long ps1GpuFreeze(uint32_t t, struct GPUFreeze *f, uint16_t **v) { return GPUfreeze(t, f, v); }
void ps1GpuVBlank(int v, int lcf)                       { GPUvBlank(v, lcf); }
void ps1GpuGetScreenInfo(int *y, int *hres)             { GPUgetScreenInfo(y, hres); }

// Minimal set: gpulib reads counters directly (gpu.c:1218-1264), remaining
// fields are zeroes. thread_rendering = 0 because USE_ASYNC_GPU is not set.
// cbs MUST outlive call: gpulib stores pointers to fskip_advice /
// fskip_force / fskip_dirty inside it, not copies.
// Adaptive frameskip: gpulib holds POINTER to fskip_advice inside cbs,
// so that exact field must be updated, not local copy of struct.
static struct rearmed_cbs *g_cbs;

void ps1GpuSetFrameskipAdvice(int aAdvice)
	{
	if (g_cbs)
		g_cbs->fskip_advice = aAdvice;
	}

void ps1GpuSetCallbacks(unsigned int *frameCount, unsigned int *hcnt, int aFrameskip)
	{
	static struct rearmed_cbs cbs;
	memset(&cbs, 0, sizeof(cbs));
	g_cbs = &cbs;
	cbs.gpu_frame_count = frameCount;
	cbs.gpu_hcnt = hcnt;
	cbs.thread_rendering = 0;
	// Rasterization frameskip by gpulib: how many consecutive frames allowed NOT
	// to render. Copied into gpu.frameskip.set here, so value must be
	// known before call. -1 - adaptive mode: no fixed setting,
	// per-frame decision from fskip_advice populated by pl_frame_limit.
	cbs.frameskip = aFrameskip;
	GPUrearmedCallbacks(&cbs);
	}

} // extern "C"
