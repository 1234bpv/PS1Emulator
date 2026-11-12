// Host viewer of the touch panel. Renders touchdraw for two screens, two layouts, three
// backgrounds, three states and two opacities into 24-bit BMP files in the directory given
// as the first argument, and checks that the RGB565 path agrees with the XRGB8888 path up
// to quantisation. Exit code 0: every file written and the paths agree. 1: a file could not
// be written. 2: the paths disagree by more than KTolerance in some channel. 3: the layout
// or the art could not be built.
#include <stdio.h>
#include <string.h>
#include "touchcore.h"
#include "touchdraw.h"

enum
	{
	KFrameW = 640,
	KFrameH = 360,
	KSpanCap = 16384,
	KAlphaCap = 262144,
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

static TVpadSpan gSpan[KSpanCap];
static unsigned char gAlpha[KAlphaCap];
static unsigned gFrame32[KFrameW * KFrameH];
static unsigned short gFrame16[KFrameW * KFrameH];

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

int main(int argc, char **argv)
	{
	if (argc < 2)
		{
		fprintf(stderr, "usage: render_panel <output directory>\n");
		return 1;
		}
	const char *dir = argv[1];
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
			TVpadArt art;
			art.iSpan = gSpan;
			art.iSpanCap = KSpanCap;
			art.iAlpha = gAlpha;
			art.iAlphaCap = KAlphaCap;
			if (!VpadArtBuild(art, layout, sc.iUmPerPx))
				{
				printf("art %s %s: build failed\n", sc.iName, KLayoutNames[l]);
				return 3;
				}
			printf("art %s %s: controls=%d spans=%d alpha=%d fits=%d\n", sc.iName, KLayoutNames[l],
				art.iCount, art.iSpanCount, art.iAlphaCount, VpadLayoutFits(layout, sc.iW, sc.iH));

			// The game picture, placed as the shell places it: 4:3, fitted, centred.
			int dstW = sc.iW, dstH = sc.iW * 3 / 4;
			if (dstH > sc.iH)
				{
				dstH = sc.iH;
				dstW = sc.iH * 4 / 3;
				}
			const int picX = (sc.iW - dstW) / 2, picY = (sc.iH - dstH) / 2;

			for (int bg = 0; bg < 3; bg++)
				for (int st = 0; st < 3; st++)
					for (int o = 0; o < 2; o++)
						{
						for (int y = 0; y < sc.iH; y++)
							for (int x = 0; x < sc.iW; x++)
								{
								const unsigned c = backgroundPixel(bg, x);
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
						VpadArtPaint(art, layout, state, c32, KOpacities[o], 1);
						VpadArtPaint(art, layout, state, c16, KOpacities[o], 1);
						const int diff = compareFrames(sc.iW, sc.iH);
						if (diff > worst)
							worst = diff;
						if (diff > KTolerance)
							{
							printf("mismatch %s %s %s %s op%d: %d\n", sc.iName, KLayoutNames[l],
								KBgNames[bg], KStateNames[st], KOpacities[o], diff);
							status = 2;
							}
						char path[512];
						sprintf(path, "%s/%s_%s_%s_%s_op%d.bmp", dir, sc.iName, KLayoutNames[l],
							KBgNames[bg], KStateNames[st], KOpacities[o]);
						if (!writeBmp(path, gFrame32, sc.iW, sc.iH))
							{
							printf("cannot write %s\n", path);
							status = status == 2 ? 2 : 1;
							}
						else
							printf("wrote %s\n", path);
						}
			}
	printf("compare: worst channel difference %d (tolerance %d)\n", worst, KTolerance);
	return status;
	}
