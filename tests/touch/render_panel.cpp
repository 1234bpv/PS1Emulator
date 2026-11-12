// Host viewer of the touch panel. Renders touchdraw for two screens, two layouts, three
// backgrounds, three states and two opacities into 24-bit BMP files in the directory given
// as the first argument, and checks four things: the RGB565 path agrees with the XRGB8888
// path up to quantisation; the ink stays inside the boxes that VpadArtClearBars clears, and
// the rest of the frame is untouched; every control has ink at rest; the centre-right of the
// pressed cross is white. With the argument "bench" it also prints the time and the work of
// one frame at rest, and the buffer sizes of more screens. Exit code 0: every file written
// and every check passed. 1: a file could not be written. 2: the paths disagree by more than
// KTolerance in some channel. 3: the layout or the art could not be built. 4: a geometry
// check failed.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "touchcore.h"
#include "touchdraw.h"

enum
	{
	KFrameW = 640,
	KFrameH = 360,
	// Two steps of the 5-bit red and blue channels: each conversion to RGB565 rounds by up to
	// one step (eight levels), and the background and the blend round once each.
	KTolerance = 16
	};

struct TScreen
	{
	int iW, iH, iUmPerPx;
	const char *iName;
	};

static const TScreen KScreens[2] = { { 640, 360, 137, "640x360" }, { 320, 240, 165, "320x240" } };
static const char *const KLayoutNames[2] = { "twelve", "sticks" };
static const char *const KBgNames[3] = { "white", "black", "stripes" };
static const char *const KStateNames[3] = { "rest", "pressed", "stick" };
static const int KOpacities[2] = { 100, 40 };
static const unsigned KStripes[8] =
	{ 0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFF00u, 0xFF00FFu, 0x00FFFFu, 0xFF8000u, 0xFFFFFFu };

static unsigned gBase32[KFrameW * KFrameH];
static unsigned gFrame32[KFrameW * KFrameH];
static unsigned short gFrame16[KFrameW * KFrameH];
static unsigned char gInside[KFrameW * KFrameH];

static void putLe(unsigned char *aP, unsigned long aV, int aBytes)
	{
	for (int k = 0; k < aBytes; k++)
		aP[k] = (unsigned char)((aV >> (8 * k)) & 255);
	}

// Writes an XRGB8888 frame of aW x aH pixels as a 24-bit BMP, bottom row first. The widths
// used here are multiples of four pixels, so rows need no padding.
static int writeBmp(const char *aPath, const unsigned *aFrame, int aW, int aH)
	{
	FILE *f = fopen(aPath, "wb");
	if (!f)
		return 0;
	unsigned char head[54];
	memset(head, 0, sizeof(head));
	const unsigned long rowB = (unsigned long)aW * 3;
	head[0] = 'B';
	head[1] = 'M';
	putLe(head + 2, 54 + rowB * aH, 4);
	putLe(head + 10, 54, 4);
	putLe(head + 14, 40, 4);
	putLe(head + 18, aW, 4);
	putLe(head + 22, aH, 4);
	putLe(head + 26, 1, 2);
	putLe(head + 28, 24, 2);
	putLe(head + 34, rowB * aH, 4);
	putLe(head + 38, 2835, 4);
	putLe(head + 42, 2835, 4);
	int ok = fwrite(head, 1, sizeof(head), f) == sizeof(head);
	unsigned char *row = new unsigned char[rowB];
	for (int y = aH - 1; y >= 0 && ok; y--)
		{
		for (int x = 0; x < aW; x++)
			{
			const unsigned v = aFrame[y * aW + x];
			row[x * 3] = (unsigned char)(v & 255);
			row[x * 3 + 1] = (unsigned char)((v >> 8) & 255);
			row[x * 3 + 2] = (unsigned char)((v >> 16) & 255);
			}
		ok = fwrite(row, 1, rowB, f) == rowB;
		}
	delete[] row;
	return fclose(f) == 0 && ok;
	}

static unsigned backgroundPixel(int aBg, int aX)
	{
	if (aBg == 0)
		return 0xFFFFFFu;
	if (aBg == 1)
		return 0;
	return KStripes[(aX / 24) % 8];
	}

static unsigned short toRgb565(unsigned aV)
	{
	return (unsigned short)((((aV >> 19) & 31) << 11) | (((aV >> 10) & 63) << 5) | ((aV >> 3) & 31));
	}

static int channelDiff(unsigned aA, unsigned aB)
	{
	return aA > aB ? (int)(aA - aB) : (int)(aB - aA);
	}

// Largest difference of a channel between the two frames, the 16-bit one expanded to 8 bits.
static int compareFrames(int aW, int aH)
	{
	int worst = 0;
	for (int k = 0; k < aW * aH; k++)
		{
		const unsigned v16 = gFrame16[k];
		const unsigned r5 = (v16 >> 11) & 31, g6 = (v16 >> 5) & 63, b5 = v16 & 31;
		const unsigned r = (r5 << 3) | (r5 >> 2);
		const unsigned g = (g6 << 2) | (g6 >> 4);
		const unsigned b = (b5 << 3) | (b5 >> 2);
		const unsigned v32 = gFrame32[k];
		const int dr = channelDiff(r, (v32 >> 16) & 255);
		const int dg = channelDiff(g, (v32 >> 8) & 255);
		const int db = channelDiff(b, v32 & 255);
		if (dr > worst)
			worst = dr;
		if (dg > worst)
			worst = dg;
		if (db > worst)
			worst = db;
		}
	return worst;
	}

// Marks in gInside the boxes that VpadArtClearBars clears, 1 inside and 0 outside.
static void markBoxes(const TVpadLayout &aLayout, int aW, int aH)
	{
	memset(gInside, 0, sizeof(gInside));
	for (int i = 0; i < aLayout.iCount; i++)
		{
		const TVpadControl &c = aLayout.iC[i];
		int mx = 0, my = 0;
		if (!VpadArtBox(c, mx, my))
			continue;
		for (int y = c.iCy - my; y <= c.iCy + my; y++)
			for (int x = c.iCx - mx; x <= c.iCx + mx; x++)
				if (x >= 0 && y >= 0 && x < aW && y < aH)
					gInside[y * aW + x] = 1;
		}
	}

// The geometry checks of one painted frame (32-bit). Returns the number of failed checks,
// each printed on its own line.
static int checkGeometry(const TVpadLayout &aLayout, const TScreen &aSc, const char *aLayoutName,
	int aBg, int aSt, int aO)
	{
	const int W = aSc.iW, H = aSc.iH;
	int bad = 0;
	int outside = 0;
	for (int k = 0; k < W * H; k++)
		if (!gInside[k] && gFrame32[k] != gBase32[k])
			outside++;
	if (outside)
		{
		printf("geometry %s %s %s %s op%d: %d pixels outside the control boxes changed\n", aSc.iName,
			aLayoutName, KBgNames[aBg], KStateNames[aSt], KOpacities[aO], outside);
		bad++;
		}
	for (int i = 0; i < aLayout.iCount; i++)
		{
		const TVpadControl &c = aLayout.iC[i];
		int mx = 0, my = 0;
		if (!VpadArtBox(c, mx, my))
			continue;
		if (aSt == 0)
			{
			int changed = 0;
			for (int y = c.iCy - my; y <= c.iCy + my; y++)
				for (int x = c.iCx - mx; x <= c.iCx + mx; x++)
					if (x >= 0 && y >= 0 && x < W && y < H && gFrame32[y * W + x] != gBase32[y * W + x])
						changed++;
			if (!changed)
				{
				printf("geometry %s %s control %d at rest: no ink in its box, bg=%s op%d\n", aSc.iName,
					aLayoutName, i, KBgNames[aBg], KOpacities[aO]);
				bad++;
				}
			}
		if (aSt == 1 && c.iKind == EVpadKindButton && (c.iBits & (1u << EVpadCross)))
			{
			// Inside the fill and outside the cross glyph: the pressed fill is white.
			const int x = c.iCx + c.iRx * 4 / 5, y = c.iCy;
			if (gFrame32[y * W + x] != 0xFFFFFFu)
				{
				printf("geometry %s pressed cross at (%d,%d): %06x, expected ffffff\n", aSc.iName, x, y,
					gFrame32[y * W + x]);
				bad++;
				}
			}
		}
	return bad;
	}

// Time and work of one frame at rest: the game picture only (bars not painted), opacity 100,
// stripes behind. Best of three runs of 1000 frames. The work is the number of coverage
// bytes of the ink passes that land on the game picture: full (255) and partial (1..254).
static void benchLayout(const char *aScreen, const char *aLayoutName, const TVpadLayout &aLayout,
	const TVpadArt &aArt, const TVpadCanvas &aCanvas16, const TVpadCanvas &aCanvas32)
	{
	int full = 0, part = 0;
	for (int i = 0; i < aArt.iCount; i++)
		for (int s = aArt.iDark[i]; s < aArt.iFirst[i + 1]; s++)
			{
			const TVpadSpan &sp = aArt.iSpan[s];
			if (sp.iY < aCanvas16.iPicY || sp.iY >= aCanvas16.iPicY + aCanvas16.iPicH)
				continue;
			for (int k = 0; k < sp.iLen; k++)
				{
				const int x = sp.iX + k;
				if (x < aCanvas16.iPicX || x >= aCanvas16.iPicX + aCanvas16.iPicW)
					continue;
				const unsigned char a = aArt.iAlpha[sp.iOff + k];
				if (a == 255)
					full++;
				else if (a)
					part++;
				}
			}
	TVpadState state;
	VpadReset(state);
	// Opacity 40 is the padalpha case, where the coverage is scaled per pixel.
	for (int pct = 100; pct >= 40; pct -= 60)
		for (int bpp = 2; bpp <= 4; bpp += 2)
			{
			const TVpadCanvas &c = bpp == 4 ? aCanvas32 : aCanvas16;
			double best = 1e9;
			for (int rep = 0; rep < 3; rep++)
				{
				const clock_t t0 = clock();
				for (int f = 0; f < 1000; f++)
					VpadArtPaint(aArt, aLayout, state, c, pct, 0);
				const double us = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
				if (us < best)
					best = us;
				}
			printf("bench %s %s bpp%d op%d frame=%.1f us full=%d partial=%d\n", aScreen, aLayoutName, bpp * 8,
				pct, best, full, part);
			}
	}

// Buffer sizes of the art for screens other than the rendered ones: the measure the glue
// grows its buffers by, for both layouts.
static void benchSizes(void)
	{
	static const TScreen KSizes[5] = { { 640, 360, 90, "640x360" }, { 640, 360, 110, "640x360" },
		{ 800, 480, 95, "800x480" }, { 800, 480, 105, "800x480" }, { 320, 240, 165, "320x240" } };
	for (int s = 0; s < 5; s++)
		for (int l = 0; l < 2; l++)
			{
			TVpadLayout layout;
			const unsigned opts = l == 1 ? EVpadOptSticks : 0;
			if (!VpadDefaultLayout(layout, KSizes[s].iW, KSizes[s].iH, KSizes[s].iUmPerPx, 100, opts))
				{
				printf("size %s um=%d %s: no fit\n", KSizes[s].iName, KSizes[s].iUmPerPx, KLayoutNames[l]);
				continue;
				}
			int spans = 0, alpha = 0;
			if (!VpadArtMeasure(layout, KSizes[s].iUmPerPx, spans, alpha))
				printf("size %s um=%d %s: measure failed\n", KSizes[s].iName, KSizes[s].iUmPerPx, KLayoutNames[l]);
			else
				printf("size %s um=%d %s: spans=%d alpha=%d\n", KSizes[s].iName, KSizes[s].iUmPerPx,
					KLayoutNames[l], spans, alpha);
			}
	}

int main(int argc, char **argv)
	{
	if (argc < 2)
		{
		fprintf(stderr, "usage: render_panel <output directory> [bench]\n");
		return 1;
		}
	const char *dir = argv[1];
	const int bench = argc > 2 && strcmp(argv[2], "bench") == 0;
	if (bench)
		benchSizes();
	int status = 0;
	int worst = 0;
	for (int s = 0; s < 2; s++)
		for (int l = 0; l < 2; l++)
			{
			const TScreen &sc = KScreens[s];
			TVpadLayout layout;
			const unsigned opts = l == 1 ? EVpadOptSticks : 0;
			if (!VpadDefaultLayout(layout, sc.iW, sc.iH, sc.iUmPerPx, 100, opts))
				{
				printf("layout %s %s: no fit\n", sc.iName, KLayoutNames[l]);
				return 3;
				}
			// The buffers have the size the glue takes from the same measure; a build that
			// disagrees with the measure is an error of the shared code, not of this viewer.
			int spans = 0, alpha = 0;
			if (!VpadArtMeasure(layout, sc.iUmPerPx, spans, alpha))
				{
				printf("art %s %s: measure failed\n", sc.iName, KLayoutNames[l]);
				return 3;
				}
			TVpadSpan *spanBuf = new TVpadSpan[spans];
			unsigned char *alphaBuf = new unsigned char[alpha];
			TVpadArt art;
			art.iSpan = spanBuf;
			art.iSpanCap = spans;
			art.iAlpha = alphaBuf;
			art.iAlphaCap = alpha;
			if (!VpadArtBuild(art, layout, sc.iUmPerPx))
				{
				printf("art %s %s: build failed\n", sc.iName, KLayoutNames[l]);
				return 3;
				}
			if (art.iSpanCount != spans || art.iAlphaCount != alpha)
				{
				printf("art %s %s: measure %d/%d differs from build %d/%d\n", sc.iName, KLayoutNames[l],
					spans, alpha, art.iSpanCount, art.iAlphaCount);
				return 3;
				}
			printf("art %s %s: controls=%d spans=%d alpha=%d fits=%d\n", sc.iName, KLayoutNames[l],
				art.iCount, art.iSpanCount, art.iAlphaCount, VpadLayoutFits(layout, sc.iW, sc.iH));
			markBoxes(layout, sc.iW, sc.iH);

			// The game picture, placed as the shell places it: 4:3, fitted, centred.
			int dstW = sc.iW, dstH = sc.iW * 3 / 4;
			if (dstH > sc.iH)
				{
				dstH = sc.iH;
				dstW = sc.iH * 4 / 3;
				}
			const int picX = (sc.iW - dstW) / 2, picY = (sc.iH - dstH) / 2;
			TVpadCanvas c32;
			c32.iDst = (unsigned char *)gFrame32;
			c32.iStride = sc.iW * 4;
			c32.iPixB = 4;
			c32.iW = sc.iW;
			c32.iH = sc.iH;
			c32.iPicX = picX;
			c32.iPicY = picY;
			c32.iPicW = dstW;
			c32.iPicH = dstH;
			TVpadCanvas c16 = c32;
			c16.iDst = (unsigned char *)gFrame16;
			c16.iStride = sc.iW * 2;
			c16.iPixB = 2;

			if (bench)
				{
				for (int y = 0; y < sc.iH; y++)
					for (int x = 0; x < sc.iW; x++)
						{
						const unsigned c = backgroundPixel(2, x);
						gFrame32[y * sc.iW + x] = c;
						gFrame16[y * sc.iW + x] = toRgb565(c);
						}
				benchLayout(sc.iName, KLayoutNames[l], layout, art, c16, c32);
				delete[] spanBuf;
				delete[] alphaBuf;
				continue;
				}

			for (int bg = 0; bg < 3; bg++)
				for (int st = 0; st < 3; st++)
					for (int o = 0; o < 2; o++)
						{
						for (int y = 0; y < sc.iH; y++)
							for (int x = 0; x < sc.iW; x++)
								{
								const unsigned c = backgroundPixel(bg, x);
								gBase32[y * sc.iW + x] = c;
								gFrame32[y * sc.iW + x] = c;
								gFrame16[y * sc.iW + x] = toRgb565(c);
								}
						TVpadState state;
						VpadReset(state);
						if (st == 1)
							state.iBits = (unsigned short)((1u << EVpadCross) | (1u << EVpadUp));
						if (st == 2)
							{
							state.iLx = 200;
							state.iLy = 100;
							state.iRightX = 60;
							state.iRightY = 190;
							}
						VpadArtPaint(art, layout, state, c32, KOpacities[o], 1);
						VpadArtPaint(art, layout, state, c16, KOpacities[o], 1);
						const int diff = compareFrames(sc.iW, sc.iH);
						if (diff > worst)
							worst = diff;
						if (diff > KTolerance)
							{
							printf("mismatch %s %s %s %s op%d: %d\n", sc.iName, KLayoutNames[l],
								KBgNames[bg], KStateNames[st], KOpacities[o], diff);
							if (!status)
								status = 2;
							}
						if (checkGeometry(layout, sc, KLayoutNames[l], bg, st, o) && !status)
							status = 4;
						char path[512];
						sprintf(path, "%s/%s_%s_%s_%s_op%d.bmp", dir, sc.iName, KLayoutNames[l],
							KBgNames[bg], KStateNames[st], KOpacities[o]);
						if (!writeBmp(path, gFrame32, sc.iW, sc.iH))
							{
							printf("cannot write %s\n", path);
							if (!status)
								status = 1;
							}
						else
							printf("wrote %s\n", path);
						}
			delete[] spanBuf;
			delete[] alphaBuf;
			}
	if (!bench)
		printf("compare: worst channel difference %d (tolerance %d)\n", worst, KTolerance);
	return status;
	}
