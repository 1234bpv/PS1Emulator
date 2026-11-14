// Portrait mode 1D downscale smoothing tests (320 -> 240 pixels).
// Tests blitRow320to240_565 and blitRow320to240_8888 against reference formulas,
// verifies buffer boundary integrity (canary guards), and exercises various
// visual patterns (black, white, gradients, 1-pixel checkerboard vertical lines, random).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned short TUint16;
typedef unsigned int TUint32;
typedef unsigned int TUint;
typedef int TInt;

// PS1 VRAM - BGR555 to RGB565 conversion (vout_symbian.cpp lines 705-712).
static inline TUint16 Bgr555ToRgb565(TUint16 aPix)
{
	const TUint r = aPix & 0x1f;
	const TUint g = (aPix >> 5) & 0x1f;
	const TUint b = (aPix >> 10) & 0x1f;
	const TUint g6 = (g << 1) | (g >> 4);
	return (TUint16)((r << 11) | (g6 << 5) | b);
}

// PS1 VRAM - BGR555 to XRGB8888 conversion (vout_symbian.cpp lines 716-724).
static inline TUint32 Bgr555ToXrgb8888(TUint32 aPix)
{
	const TUint32 r = aPix & 0x1f;
	const TUint32 g = (aPix >> 5) & 0x1f;
	const TUint32 b = (aPix >> 10) & 0x1f;
	return 0xff000000u | (((r << 3) | (r >> 2)) << 16)
	                   | (((g << 3) | (g >> 2)) << 8)
	                   |  ((b << 3) | (b >> 2));
}

// Production 16-bit blitter (vout_symbian.cpp lines 664-701).
static void __attribute__((__noinline__))
blitRow320to240_565(TUint16 *aDst, const TUint16 *aSrc)
{
	TUint32 cAvg = 0x7bdeu;
	TUint32 cRB = 0x001f001fu, cG54 = 0x07c007c0u, cG0 = 0x00200020u;
	__asm__ __volatile__ ("" : "+r"(cAvg), "+r"(cRB), "+r"(cG54), "+r"(cG0));
	const TUint32 *s = (const TUint32 *)aSrc;
	TUint32 *d = (TUint32 *)aDst;
	TInt blocks = 40;
	do
		{
		const TUint32 w0 = s[0];
		const TUint32 w1 = s[1];
		const TUint32 w2 = s[2];
		const TUint32 w3 = s[3];
		s += 4;

		const TUint32 s0 = w0 & 0x7fffu;
		const TUint32 s1 = (w0 >> 16) & 0x7fffu;
		const TUint32 s2 = w1 & 0x7fffu;
		const TUint32 s3 = (w1 >> 16) & 0x7fffu;
		const TUint32 m0 = (s1 & s2) + (((s1 ^ s2) & cAvg) >> 1);

		const TUint32 s4 = w2 & 0x7fffu;
		const TUint32 s5 = (w2 >> 16) & 0x7fffu;
		const TUint32 s6 = w3 & 0x7fffu;
		const TUint32 s7 = (w3 >> 16) & 0x7fffu;
		const TUint32 m1 = (s5 & s6) + (((s5 ^ s6) & cAvg) >> 1);

		const TUint32 p0 = s0 | (m0 << 16);
		const TUint32 p1 = s3 | (s4 << 16);
		const TUint32 p2 = m1 | (s7 << 16);

		d[0] = ((p0 & cRB) << 11) | ((p0 << 1) & cG54) | ((p0 >> 4) & cG0) | ((p0 >> 10) & cRB);
		d[1] = ((p1 & cRB) << 11) | ((p1 << 1) & cG54) | ((p1 >> 4) & cG0) | ((p1 >> 10) & cRB);
		d[2] = ((p2 & cRB) << 11) | ((p2 << 1) & cG54) | ((p2 >> 4) & cG0) | ((p2 >> 10) & cRB);
		d += 3;
		} while (--blocks);
}

// Production 32-bit blitter (vout_symbian.cpp blitRow320to240_8888).
static void __attribute__((__noinline__))
blitRow320to240_8888(TUint32 *aDst, const TUint16 *aSrc)
{
	TUint32 cAvg = 0x7bdeu;
	__asm__ __volatile__ ("" : "+r"(cAvg));
	const TUint32 *s = (const TUint32 *)aSrc;
	TUint32 *d = aDst;
	TInt blocks = 40;
	do
		{
		const TUint32 w0 = s[0];
		const TUint32 w1 = s[1];
		const TUint32 w2 = s[2];
		const TUint32 w3 = s[3];
		s += 4;

		const TUint32 s0 = w0 & 0x7fffu;
		const TUint32 s1 = (w0 >> 16) & 0x7fffu;
		const TUint32 s2 = w1 & 0x7fffu;
		const TUint32 s3 = (w1 >> 16) & 0x7fffu;
		const TUint32 m0 = (s1 & s2) + (((s1 ^ s2) & cAvg) >> 1);

		const TUint32 s4 = w2 & 0x7fffu;
		const TUint32 s5 = (w2 >> 16) & 0x7fffu;
		const TUint32 s6 = w3 & 0x7fffu;
		const TUint32 s7 = (w3 >> 16) & 0x7fffu;
		const TUint32 m1 = (s5 & s6) + (((s5 ^ s6) & cAvg) >> 1);

		d[0] = Bgr555ToXrgb8888(s0);
		d[1] = Bgr555ToXrgb8888(m0);
		d[2] = Bgr555ToXrgb8888(s3);
		d[3] = Bgr555ToXrgb8888(s4);
		d[4] = Bgr555ToXrgb8888(m1);
		d[5] = Bgr555ToXrgb8888(s7);
		d += 6;
		} while (--blocks);
}

// Reference implementations for specification conformance.
static void RefBlitRow320to240_565(TUint16 *aDst, const TUint16 *aSrc)
{
	int b;
	for (b = 0; b < 40; ++b) {
		const TUint32 s0 = aSrc[b * 8 + 0] & 0x7fffu;
		const TUint32 s1 = aSrc[b * 8 + 1] & 0x7fffu;
		const TUint32 s2 = aSrc[b * 8 + 2] & 0x7fffu;
		const TUint32 s3 = aSrc[b * 8 + 3] & 0x7fffu;
		const TUint32 s4 = aSrc[b * 8 + 4] & 0x7fffu;
		const TUint32 s5 = aSrc[b * 8 + 5] & 0x7fffu;
		const TUint32 s6 = aSrc[b * 8 + 6] & 0x7fffu;
		const TUint32 s7 = aSrc[b * 8 + 7] & 0x7fffu;

		const TUint32 m0 = (s1 & s2) + (((s1 ^ s2) & 0x7bdeu) >> 1);
		const TUint32 m1 = (s5 & s6) + (((s5 ^ s6) & 0x7bdeu) >> 1);

		aDst[b * 6 + 0] = Bgr555ToRgb565((TUint16)s0);
		aDst[b * 6 + 1] = Bgr555ToRgb565((TUint16)m0);
		aDst[b * 6 + 2] = Bgr555ToRgb565((TUint16)s3);
		aDst[b * 6 + 3] = Bgr555ToRgb565((TUint16)s4);
		aDst[b * 6 + 4] = Bgr555ToRgb565((TUint16)m1);
		aDst[b * 6 + 5] = Bgr555ToRgb565((TUint16)s7);
	}
}

static void RefBlitRow320to240_8888(TUint32 *aDst, const TUint16 *aSrc)
{
	int b;
	for (b = 0; b < 40; ++b) {
		const TUint32 s0 = aSrc[b * 8 + 0] & 0x7fffu;
		const TUint32 s1 = aSrc[b * 8 + 1] & 0x7fffu;
		const TUint32 s2 = aSrc[b * 8 + 2] & 0x7fffu;
		const TUint32 s3 = aSrc[b * 8 + 3] & 0x7fffu;
		const TUint32 s4 = aSrc[b * 8 + 4] & 0x7fffu;
		const TUint32 s5 = aSrc[b * 8 + 5] & 0x7fffu;
		const TUint32 s6 = aSrc[b * 8 + 6] & 0x7fffu;
		const TUint32 s7 = aSrc[b * 8 + 7] & 0x7fffu;

		const TUint32 m0 = (s1 & s2) + (((s1 ^ s2) & 0x7bdeu) >> 1);
		const TUint32 m1 = (s5 & s6) + (((s5 ^ s6) & 0x7bdeu) >> 1);

		aDst[b * 6 + 0] = Bgr555ToXrgb8888(s0);
		aDst[b * 6 + 1] = Bgr555ToXrgb8888(m0);
		aDst[b * 6 + 2] = Bgr555ToXrgb8888(s3);
		aDst[b * 6 + 3] = Bgr555ToXrgb8888(s4);
		aDst[b * 6 + 4] = Bgr555ToXrgb8888(m1);
		aDst[b * 6 + 5] = Bgr555ToXrgb8888(s7);
	}
}

// Test harness infrastructure.
static int gChecks = 0;
static int gFails = 0;
static char gWhere[200] = "";
static const char *gTest = "";
static const char *gOnly = 0;
static int gListOnly = 0;

#define CHECK(cond) \
	do { \
		++gChecks; \
		if (!(cond)) { ++gFails; printf("FAIL [%s] %s:%d: %s\n", gWhere, __FILE__, __LINE__, #cond); } \
	} while (0)

#define CHECK_EQ(actual, expected) \
	do { \
		unsigned long a_ = (unsigned long)(actual); \
		unsigned long e_ = (unsigned long)(expected); \
		++gChecks; \
		if (a_ != e_) { \
			++gFails; \
			printf("FAIL [%s] %s:%d: %s == %s: expected 0x%lx, got 0x%lx\n", \
				gWhere, __FILE__, __LINE__, #actual, #expected, e_, a_); \
		} \
	} while (0)

static void Run(const char *aName, void (*aFn)())
{
	gTest = aName;
	strcpy(gWhere, aName);
	aFn();
}

#define RUN(fn) \
	do { \
		if (gListOnly) printf("TEST %s\n", #fn); \
		else if (gOnly == 0 || strcmp(gOnly, #fn) == 0) Run(#fn, fn); \
	} while (0)

// Canary guards layout: 64 guard bytes before and after dst buffer.
static const int KGuard = 64;
static const unsigned char KCanary = 0xA5;

struct Buffer565 {
	unsigned char guardBefore[KGuard];
	TUint16 dst[240];
	unsigned char guardAfter[KGuard];
} __attribute__((aligned(4)));

struct Buffer8888 {
	unsigned char guardBefore[KGuard];
	TUint32 dst[240];
	unsigned char guardAfter[KGuard];
} __attribute__((aligned(4)));

static void InitBuffer565(Buffer565 &b)
{
	memset(b.guardBefore, KCanary, sizeof(b.guardBefore));
	memset(b.dst, 0x5A, sizeof(b.dst));
	memset(b.guardAfter, KCanary, sizeof(b.guardAfter));
}

static void InitBuffer8888(Buffer8888 &b)
{
	memset(b.guardBefore, KCanary, sizeof(b.guardBefore));
	memset(b.dst, 0x5A, sizeof(b.dst));
	memset(b.guardAfter, KCanary, sizeof(b.guardAfter));
}

static void CheckCanaries565(const Buffer565 &b)
{
	int i;
	int badBefore = 0;
	int badAfter = 0;
	for (i = 0; i < KGuard; ++i) {
		if (b.guardBefore[i] != KCanary) ++badBefore;
		if (b.guardAfter[i] != KCanary) ++badAfter;
	}
	CHECK_EQ(badBefore, 0);
	CHECK_EQ(badAfter, 0);
}

static void CheckCanaries8888(const Buffer8888 &b)
{
	int i;
	int badBefore = 0;
	int badAfter = 0;
	for (i = 0; i < KGuard; ++i) {
		if (b.guardBefore[i] != KCanary) ++badBefore;
		if (b.guardAfter[i] != KCanary) ++badAfter;
	}
	CHECK_EQ(badBefore, 0);
	CHECK_EQ(badAfter, 0);
}

static void Compare565(const TUint16 *actual, const TUint16 *ref)
{
	int diffCount = 0;
	int i;
	for (i = 0; i < 240; ++i) {
		if (actual[i] != ref[i]) {
			++diffCount;
			if (diffCount <= 5) {
				CHECK_EQ(actual[i], ref[i]);
			}
		}
	}
	CHECK_EQ(diffCount, 0);
}

static void Compare8888(const TUint32 *actual, const TUint32 *ref)
{
	int diffCount = 0;
	int i;
	for (i = 0; i < 240; ++i) {
		if (actual[i] != ref[i]) {
			++diffCount;
			if (diffCount <= 5) {
				CHECK_EQ(actual[i], ref[i]);
			}
		}
	}
	CHECK_EQ(diffCount, 0);
}

// Simple deterministic PRNG for reproducability.
static TUint32 LcgNext(TUint32 &seed)
{
	seed = seed * 1664525u + 1013904223u;
	return seed;
}

// ---------------------------------------------------------------- Tests

static void TestBlackRow565()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer565 buf;
	TUint16 ref[240];
	int i;

	memset(src, 0, sizeof(src));
	InitBuffer565(buf);
	RefBlitRow320to240_565(ref, src);

	blitRow320to240_565(buf.dst, src);

	CheckCanaries565(buf);
	Compare565(buf.dst, ref);
	for (i = 0; i < 240; ++i) {
		CHECK_EQ(buf.dst[i], 0x0000u);
	}
}

static void TestBlackRow8888()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer8888 buf;
	TUint32 ref[240];
	int i;

	memset(src, 0, sizeof(src));
	InitBuffer8888(buf);
	RefBlitRow320to240_8888(ref, src);

	blitRow320to240_8888(buf.dst, src);

	CheckCanaries8888(buf);
	Compare8888(buf.dst, ref);
	for (i = 0; i < 240; ++i) {
		CHECK_EQ(buf.dst[i], 0xff000000u);
	}
}

static void TestWhiteRow565()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer565 buf;
	TUint16 ref[240];
	int i;

	for (i = 0; i < 320; ++i) src[i] = 0x7fffu;
	InitBuffer565(buf);
	RefBlitRow320to240_565(ref, src);

	blitRow320to240_565(buf.dst, src);

	CheckCanaries565(buf);
	Compare565(buf.dst, ref);
	for (i = 0; i < 240; ++i) {
		CHECK_EQ(buf.dst[i], 0xffffu);
	}
}

static void TestWhiteRow8888()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer8888 buf;
	TUint32 ref[240];
	int i;

	for (i = 0; i < 320; ++i) src[i] = 0x7fffu;
	InitBuffer8888(buf);
	RefBlitRow320to240_8888(ref, src);

	blitRow320to240_8888(buf.dst, src);

	CheckCanaries8888(buf);
	Compare8888(buf.dst, ref);
	for (i = 0; i < 240; ++i) {
		CHECK_EQ(buf.dst[i], 0xffffffffu);
	}
}

static void TestGradientRow565()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer565 buf;
	TUint16 ref[240];
	int i;

	for (i = 0; i < 320; ++i) {
		TUint32 r = (i * 31) / 319;
		TUint32 g = ((319 - i) * 31) / 319;
		TUint32 b = (i * 17) % 32;
		src[i] = (TUint16)(r | (g << 5) | (b << 10));
	}
	InitBuffer565(buf);
	RefBlitRow320to240_565(ref, src);

	blitRow320to240_565(buf.dst, src);

	CheckCanaries565(buf);
	Compare565(buf.dst, ref);
}

static void TestGradientRow8888()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer8888 buf;
	TUint32 ref[240];
	int i;

	for (i = 0; i < 320; ++i) {
		TUint32 r = (i * 31) / 319;
		TUint32 g = ((319 - i) * 31) / 319;
		TUint32 b = (i * 17) % 32;
		src[i] = (TUint16)(r | (g << 5) | (b << 10));
	}
	InitBuffer8888(buf);
	RefBlitRow320to240_8888(ref, src);

	blitRow320to240_8888(buf.dst, src);

	CheckCanaries8888(buf);
	Compare8888(buf.dst, ref);
}

static void TestCheckerboardRow565()
{
	// 1-pixel alternating vertical lines (white, black, white, black...).
	// Exercises 1D smoothing: S1 and S2 (black and white) average to 50% gray,
	// preventing vertical 1-pixel lines from completely disappearing during downscale.
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer565 buf;
	TUint16 ref[240];
	int i;

	for (i = 0; i < 320; ++i) {
		src[i] = (i % 2 == 0) ? (TUint16)0x7fffu : (TUint16)0x0000u;
	}
	InitBuffer565(buf);
	RefBlitRow320to240_565(ref, src);

	blitRow320to240_565(buf.dst, src);

	CheckCanaries565(buf);
	Compare565(buf.dst, ref);

	// Verify the smoothing behaviour explicitly for block 0:
	// S0 (white) -> D0 (white)
	// Avg(S1=black, S2=white) -> D1 (gray)
	// S3 (black) -> D2 (black)
	// S4 (white) -> D3 (white)
	// Avg(S5=black, S6=white) -> D4 (gray)
	// S7 (black) -> D5 (black)
	CHECK_EQ(buf.dst[0], 0xffffu);
	CHECK(buf.dst[1] > 0x0000u && buf.dst[1] < 0xffffu);
	CHECK_EQ(buf.dst[2], 0x0000u);
	CHECK_EQ(buf.dst[3], 0xffffu);
	CHECK(buf.dst[4] > 0x0000u && buf.dst[4] < 0xffffu);
	CHECK_EQ(buf.dst[5], 0x0000u);
}

static void TestCheckerboardRow8888()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer8888 buf;
	TUint32 ref[240];
	int i;

	for (i = 0; i < 320; ++i) {
		src[i] = (i % 2 == 0) ? (TUint16)0x7fffu : (TUint16)0x0000u;
	}
	InitBuffer8888(buf);
	RefBlitRow320to240_8888(ref, src);

	blitRow320to240_8888(buf.dst, src);

	CheckCanaries8888(buf);
	Compare8888(buf.dst, ref);

	// Explicit check of 1D smoothing:
	CHECK_EQ(buf.dst[0], 0xffffffffu);
	CHECK(buf.dst[1] > 0xff000000u && buf.dst[1] < 0xffffffffu);
	CHECK_EQ(buf.dst[2], 0xff000000u);
	CHECK_EQ(buf.dst[3], 0xffffffffu);
	CHECK(buf.dst[4] > 0xff000000u && buf.dst[4] < 0xffffffffu);
	CHECK_EQ(buf.dst[5], 0xff000000u);
}

static void TestCheckerboardInverted565()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer565 buf;
	TUint16 ref[240];
	int i;

	for (i = 0; i < 320; ++i) {
		src[i] = (i % 2 == 1) ? (TUint16)0x7fffu : (TUint16)0x0000u;
	}
	InitBuffer565(buf);
	RefBlitRow320to240_565(ref, src);

	blitRow320to240_565(buf.dst, src);

	CheckCanaries565(buf);
	Compare565(buf.dst, ref);

	// Inverted pattern: S0 is black, S3 is white, S4 is black, S7 is white.
	CHECK_EQ(buf.dst[0], 0x0000u);
	CHECK(buf.dst[1] > 0x0000u && buf.dst[1] < 0xffffu);
	CHECK_EQ(buf.dst[2], 0xffffu);
	CHECK_EQ(buf.dst[3], 0x0000u);
	CHECK(buf.dst[4] > 0x0000u && buf.dst[4] < 0xffffu);
	CHECK_EQ(buf.dst[5], 0xffffu);
}

static void TestCheckerboardInverted8888()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer8888 buf;
	TUint32 ref[240];
	int i;

	for (i = 0; i < 320; ++i) {
		src[i] = (i % 2 == 1) ? (TUint16)0x7fffu : (TUint16)0x0000u;
	}
	InitBuffer8888(buf);
	RefBlitRow320to240_8888(ref, src);

	blitRow320to240_8888(buf.dst, src);

	CheckCanaries8888(buf);
	Compare8888(buf.dst, ref);

	CHECK_EQ(buf.dst[0], 0xff000000u);
	CHECK(buf.dst[1] > 0xff000000u && buf.dst[1] < 0xffffffffu);
	CHECK_EQ(buf.dst[2], 0xffffffffu);
	CHECK_EQ(buf.dst[3], 0xff000000u);
	CHECK(buf.dst[4] > 0xff000000u && buf.dst[4] < 0xffffffffu);
	CHECK_EQ(buf.dst[5], 0xffffffffu);
}

static void TestHighBitMasking565()
{
	// Bit 15 in PS1 VRAM BGR555 is the mask/transparency bit.
	// Blitters must mask with 0x7fffu so bit 15 does not leak into color calculations.
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer565 bufWithBit;
	Buffer565 bufWithoutBit;
	int i;

	for (i = 0; i < 320; ++i) {
		TUint16 val = (TUint16)((i * 37) & 0x7fffu);
		src[i] = (TUint16)(val | 0x8000u); // Force bit 15 set
	}
	InitBuffer565(bufWithBit);
	blitRow320to240_565(bufWithBit.dst, src);

	for (i = 0; i < 320; ++i) {
		src[i] &= 0x7fffu; // Clear bit 15
	}
	InitBuffer565(bufWithoutBit);
	blitRow320to240_565(bufWithoutBit.dst, src);

	CheckCanaries565(bufWithBit);
	CheckCanaries565(bufWithoutBit);
	Compare565(bufWithBit.dst, bufWithoutBit.dst);
}

static void TestHighBitMasking8888()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer8888 bufWithBit;
	Buffer8888 bufWithoutBit;
	int i;

	for (i = 0; i < 320; ++i) {
		TUint16 val = (TUint16)((i * 37) & 0x7fffu);
		src[i] = (TUint16)(val | 0x8000u); // Force bit 15 set
	}
	InitBuffer8888(bufWithBit);
	blitRow320to240_8888(bufWithBit.dst, src);

	for (i = 0; i < 320; ++i) {
		src[i] &= 0x7fffu; // Clear bit 15
	}
	InitBuffer8888(bufWithoutBit);
	blitRow320to240_8888(bufWithoutBit.dst, src);

	CheckCanaries8888(bufWithBit);
	CheckCanaries8888(bufWithoutBit);
	Compare8888(bufWithBit.dst, bufWithoutBit.dst);
}

static void TestRandomPatterns565()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer565 buf;
	TUint16 ref[240];
	TUint32 seed = 0x12345678u;
	int round, i;

	for (round = 0; round < 8; ++round) {
		for (i = 0; i < 320; ++i) {
			src[i] = (TUint16)(LcgNext(seed) & 0xffffu);
		}
		InitBuffer565(buf);
		RefBlitRow320to240_565(ref, src);

		blitRow320to240_565(buf.dst, src);

		CheckCanaries565(buf);
		Compare565(buf.dst, ref);
	}
}

static void TestRandomPatterns8888()
{
	TUint16 src[320] __attribute__((aligned(4)));
	Buffer8888 buf;
	TUint32 ref[240];
	TUint32 seed = 0x87654321u;
	int round, i;

	for (round = 0; round < 8; ++round) {
		for (i = 0; i < 320; ++i) {
			src[i] = (TUint16)(LcgNext(seed) & 0xffffu);
		}
		InitBuffer8888(buf);
		RefBlitRow320to240_8888(ref, src);

		blitRow320to240_8888(buf.dst, src);

		CheckCanaries8888(buf);
		Compare8888(buf.dst, ref);
	}
}

static void TestCanaryGuardsDetect565()
{
	Buffer565 buf;
	InitBuffer565(buf);
	// Initial state must be clean.
	CheckCanaries565(buf);
	// Corrupting canary before should be detected.
	buf.guardBefore[0] = 0x00;
	int bad = 0;
	int i;
	for (i = 0; i < KGuard; ++i) {
		if (buf.guardBefore[i] != KCanary) ++bad;
	}
	CHECK_EQ(bad, 1);
	// Corrupting canary after should be detected.
	buf.guardBefore[0] = KCanary;
	buf.guardAfter[KGuard - 1] = 0x00;
	bad = 0;
	for (i = 0; i < KGuard; ++i) {
		if (buf.guardAfter[i] != KCanary) ++bad;
	}
	CHECK_EQ(bad, 1);
}

static void TestCanaryGuardsDetect8888()
{
	Buffer8888 buf;
	InitBuffer8888(buf);
	CheckCanaries8888(buf);
	buf.guardBefore[0] = 0x00;
	int bad = 0;
	int i;
	for (i = 0; i < KGuard; ++i) {
		if (buf.guardBefore[i] != KCanary) ++bad;
	}
	CHECK_EQ(bad, 1);
	buf.guardBefore[0] = KCanary;
	buf.guardAfter[KGuard - 1] = 0x00;
	bad = 0;
	for (i = 0; i < KGuard; ++i) {
		if (buf.guardAfter[i] != KCanary) ++bad;
	}
	CHECK_EQ(bad, 1);
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--list") == 0) gListOnly = 1;
		else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) gOnly = argv[++i];
	}

	RUN(TestBlackRow565);
	RUN(TestBlackRow8888);
	RUN(TestWhiteRow565);
	RUN(TestWhiteRow8888);
	RUN(TestGradientRow565);
	RUN(TestGradientRow8888);
	RUN(TestCheckerboardRow565);
	RUN(TestCheckerboardRow8888);
	RUN(TestCheckerboardInverted565);
	RUN(TestCheckerboardInverted8888);
	RUN(TestHighBitMasking565);
	RUN(TestHighBitMasking8888);
	RUN(TestRandomPatterns565);
	RUN(TestRandomPatterns8888);
	RUN(TestCanaryGuardsDetect565);
	RUN(TestCanaryGuardsDetect8888);

	if (gListOnly) return 0;
	printf("portrait: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
}
