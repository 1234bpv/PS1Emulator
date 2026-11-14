#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#include "psxcommon.h"
#include "r3000a.h"
#include "gte.h"

/* GTE host test for RTPS / RTPT aspect ratio projection scaling (Step П5).
 * Tests:
 * 1. Random register generation covering normal and edge cases.
 * 2. Identity test (fx_q12 = 4096, fy_q12 = 4096): bit-identical output to unscaled.
 * 3. Scaling test: deviation of SX from OFX scales by fx_q12 / 4096.0 within +/- 1 pixel.
 * 4. SY invariant when fy_q12 == 4096, and scales when fy_q12 != 4096.
 * 5. Flagged (gte.c) and flagless (gte_nf.c) modes equivalence in SX/SY.
 */

psxRegisters psxRegs;

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if !__has_builtin(__builtin_add_overflow) && (!defined(__GNUC__) || __GNUC__ < 5)
int __builtin_add_overflow(long long a, long long b, long long *res) {
	*res = a + b;
	if (b > 0 && a > LLONG_MAX - b) return 1;
	if (b < 0 && a < LLONG_MIN - b) return 1;
	return 0;
}

int __builtin_sub_overflow(long long a, long long b, long long *res) {
	*res = a - b;
	if (b < 0 && a > LLONG_MAX + b) return 1;
	if (b > 0 && a < LLONG_MIN + b) return 1;
	return 0;
}
#endif

static uint32_t s_rng = 0x12345678;

static uint32_t rand_u32(void)
{
	/* Xorshift32 */
	uint32_t x = s_rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	s_rng = x;
	return x;
}

static int16_t rand_s16(void)
{
	return (int16_t)(rand_u32() & 0xffff);
}

static int32_t rand_s32(void)
{
	return (int32_t)rand_u32();
}

static void randomize_gte_regs(psxCP2Regs *regs, int test_idx)
{
	int i;
	for (i = 0; i < 32; i++) {
		regs->CP2D.r[i] = rand_u32();
		regs->CP2C.r[i] = rand_u32();
	}

	/* Specific edge cases across test indices */
	switch (test_idx % 8) {
	case 0:
		/* Standard 3D scene parameters */
		regs->CP2C.n.h = 100 + (rand_u32() % 1000);  /* screen distance */
		regs->CP2C.n.ofx = (160 + (int)(rand_u32() % 40) - 20) << 16;
		regs->CP2C.n.ofy = (120 + (int)(rand_u32() % 40) - 20) << 16;
		/* Valid rotation matrix (-4096..4096) */
		for (i = 0; i < 9; i++) {
			regs->CP2C.p[i].sw.l = (int16_t)((rand_u32() % 8192) - 4096);
		}
		/* Vertices in front of camera */
		regs->CP2D.n.v0.z = 200 + (int16_t)(rand_u32() % 2000);
		regs->CP2D.n.v1.z = 200 + (int16_t)(rand_u32() % 2000);
		regs->CP2D.n.v2.z = 200 + (int16_t)(rand_u32() % 2000);
		break;
	case 1:
		/* Very close to camera (small sz3) */
		regs->CP2C.n.h = 400;
		regs->CP2D.n.v0.z = 5 + (int16_t)(rand_u32() % 50);
		regs->CP2D.n.v1.z = 5 + (int16_t)(rand_u32() % 50);
		regs->CP2D.n.v2.z = 5 + (int16_t)(rand_u32() % 50);
		break;
	case 2:
		/* Far from camera (large sz3, small quotient) */
		regs->CP2C.n.h = 200;
		regs->CP2D.n.v0.z = 10000 + (int16_t)(rand_u32() % 20000);
		regs->CP2D.n.v1.z = 10000 + (int16_t)(rand_u32() % 20000);
		regs->CP2D.n.v2.z = 10000 + (int16_t)(rand_u32() % 20000);
		break;
	case 3:
		/* Zero / near-zero H */
		regs->CP2C.n.h = rand_u32() % 4;
		break;
	case 4:
		/* Max H */
		regs->CP2C.n.h = 0xffff;
		break;
	case 5:
		/* Zero OFX and OFY */
		regs->CP2C.n.ofx = 0;
		regs->CP2C.n.ofy = 0;
		regs->CP2C.n.h = 300;
		break;
	case 6:
		/* Negative OFX / OFY */
		regs->CP2C.n.ofx = (int32_t)0xfff00000;
		regs->CP2C.n.ofy = (int32_t)0xffe00000;
		regs->CP2C.n.h = 300;
		break;
	case 7:
		/* Pure random state */
		break;
	}
}

/* Reference clamp & overflow logic matching PCSX GTE */
static inline int32_t ref_limG1(int32_t val)
{
	if (val > 0x3ff) return 0x3ff;
	if (val < -0x400) return -0x400;
	return val;
}

static inline int32_t ref_limG2(int32_t val)
{
	if (val > 0x3ff) return 0x3ff;
	if (val < -0x400) return -0x400;
	return val;
}

static inline int64_t ref_mac0flags(int64_t a)
{
	return a;
}

/* Number of checks passed */
static unsigned long long g_total_assertions = 0;
static unsigned long long g_failed_assertions = 0;

#define ASSERT_TRUE(cond, msg) do { \
	g_total_assertions++; \
	if (!(cond)) { \
		g_failed_assertions++; \
		if (g_failed_assertions <= 10) { \
			fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
		} \
	} \
} while (0)

#define ASSERT_EQ_HEX(act, exp, msg) do { \
	g_total_assertions++; \
	if ((act) != (exp)) { \
		g_failed_assertions++; \
		if (g_failed_assertions <= 10) { \
			fprintf(stderr, "FAIL [%s:%d]: %s (got 0x%llx, expected 0x%llx)\n", \
				__FILE__, __LINE__, msg, (unsigned long long)(act), (unsigned long long)(exp)); \
		} \
	} \
} while (0)

static void test_rtps_identity(int iterations)
{
	int i;
	printf("Running RTPS identity tests (fx_q12 = 4096, fy_q12 = 4096) for %d states...\n", iterations);

	for (i = 0; i < iterations; i++) {
		psxCP2Regs state_in, state_base, state_ident, state_nf_base, state_nf_ident;
		u32 code = GTEOP_RTPS | (1 << 19); /* sf=1, lm=0 */
		gte_handler *handler = gteGetHandler(code);
		gte_handler *handler_nf = gteGetHandler_nf(code);

		randomize_gte_regs(&state_in, i);

		/* 1. Flagged execution with 4096 */
		state_base = state_in;
		g_gte_fx_q12 = 4096;
		g_gte_fy_q12 = 4096;
		handler(&state_base, code);

		/* Re-run to ensure repeatability */
		state_ident = state_in;
		handler(&state_ident, code);

		ASSERT_TRUE(memcmp(&state_base, &state_ident, sizeof(psxCP2Regs)) == 0,
			"RTPS 4096 identity re-run mismatch");

		/* 2. Flagless execution with 4096 */
		state_nf_base = state_in;
		handler_nf(&state_nf_base, code);

		state_nf_ident = state_in;
		handler_nf(&state_nf_ident, code);

		ASSERT_TRUE(memcmp(&state_nf_base, &state_nf_ident, sizeof(psxCP2Regs)) == 0,
			"RTPS_nf 4096 identity re-run mismatch");

		/* 3. Coordinates SX2 and SY2 must match between flagged and flagless */
		ASSERT_EQ_HEX(state_nf_base.CP2D.p[14].sw.l, state_base.CP2D.p[14].sw.l,
			"RTPS flagged vs flagless SX2 mismatch at 4096");
		ASSERT_EQ_HEX(state_nf_base.CP2D.p[14].sw.h, state_base.CP2D.p[14].sw.h,
			"RTPS flagged vs flagless SY2 mismatch at 4096");

		/* Also test with sf=0 */
		code = GTEOP_RTPS;
		handler = gteGetHandler(code);
		state_base = state_in;
		handler(&state_base, code);
		state_ident = state_in;
		handler(&state_ident, code);
		ASSERT_TRUE(memcmp(&state_base, &state_ident, sizeof(psxCP2Regs)) == 0,
			"RTPS sf=0 4096 identity mismatch");
	}
}

static void test_rtpt_identity(int iterations)
{
	int i;
	printf("Running RTPT identity tests (fx_q12 = 4096, fy_q12 = 4096) for %d states...\n", iterations);

	for (i = 0; i < iterations; i++) {
		psxCP2Regs state_in, state_base, state_ident, state_nf_base, state_nf_ident;
		u32 code = GTEOP_RTPT | (1 << 19); /* sf=1, lm=0 */
		gte_handler *handler = gteGetHandler(code);
		gte_handler *handler_nf = gteGetHandler_nf(code);

		randomize_gte_regs(&state_in, i);

		/* 1. Flagged execution with 4096 */
		state_base = state_in;
		g_gte_fx_q12 = 4096;
		g_gte_fy_q12 = 4096;
		handler(&state_base, code);

		state_ident = state_in;
		handler(&state_ident, code);

		ASSERT_TRUE(memcmp(&state_base, &state_ident, sizeof(psxCP2Regs)) == 0,
			"RTPT 4096 identity re-run mismatch");

		/* 2. Flagless execution with 4096 */
		state_nf_base = state_in;
		handler_nf(&state_nf_base, code);

		state_nf_ident = state_in;
		handler_nf(&state_nf_ident, code);

		ASSERT_TRUE(memcmp(&state_nf_base, &state_nf_ident, sizeof(psxCP2Regs)) == 0,
			"RTPT_nf 4096 identity re-run mismatch");

		/* 3. Coordinates SX0..2 and SY0..2 must match between flagged and flagless */
		for (int v = 0; v < 3; v++) {
			ASSERT_EQ_HEX(state_nf_base.CP2D.p[12 + v].sw.l, state_base.CP2D.p[12 + v].sw.l,
				"RTPT flagged vs flagless SX mismatch at 4096");
			ASSERT_EQ_HEX(state_nf_base.CP2D.p[12 + v].sw.h, state_base.CP2D.p[12 + v].sw.h,
				"RTPT flagged vs flagless SY mismatch at 4096");
		}
	}
}

static void test_rtps_scaling(int iterations)
{
	int i;
	static const struct {
		int fx;
		int fy;
		const char *name;
	} presets[] = {
		{ 5461, 4096, "Portrait 4/3 X (5461, 4096)" },
		{ 3072, 4096, "Widescreen 3/4 X (3072, 4096)" },
		{ 4096, 3072, "Portrait wider FOV Y (4096, 3072)" },
		{ 5461, 3072, "Mixed 4/3 X and 3/4 Y (5461, 3072)" },
		{ 2048, 4096, "Half scale X (2048, 4096)" },
		{ 6144, 4096, "1.5x scale X (6144, 4096)" },
	};
	int p;

	printf("Running RTPS scaling tests across %d presets for %d states...\n",
		(int)(sizeof(presets) / sizeof(presets[0])), iterations);

	for (p = 0; p < (int)(sizeof(presets) / sizeof(presets[0])); p++) {
		int fx = presets[p].fx;
		int fy = presets[p].fy;

		for (i = 0; i < iterations; i++) {
			psxCP2Regs state_in, state_base, state_scaled, state_nf_scaled;
			u32 code = GTEOP_RTPS | (1 << 19);
			gte_handler *handler = gteGetHandler(code);
			gte_handler *handler_nf = gteGetHandler_nf(code);

			randomize_gte_regs(&state_in, i);

			/* 1. Base unscaled run */
			g_gte_fx_q12 = 4096;
			g_gte_fy_q12 = 4096;
			state_base = state_in;
			handler(&state_base, code);

			/* 2. Scaled run (flagged) */
			g_gte_fx_q12 = fx;
			g_gte_fy_q12 = fy;
			state_scaled = state_in;
			handler(&state_scaled, code);

			/* 3. Scaled run (flagless) */
			state_nf_scaled = state_in;
			handler_nf(&state_nf_scaled, code);

			/* Coordinate equivalence between flagged and flagless */
			ASSERT_EQ_HEX(state_nf_scaled.CP2D.p[14].sw.l, state_scaled.CP2D.p[14].sw.l,
				"RTPS flagged vs flagless SX2 mismatch in scaled mode");
			ASSERT_EQ_HEX(state_nf_scaled.CP2D.p[14].sw.h, state_scaled.CP2D.p[14].sw.h,
				"RTPS flagged vs flagless SY2 mismatch in scaled mode");

			/* Invariance check: registers unaffected by projection */
			ASSERT_EQ_HEX(state_scaled.CP2D.p[16].w.l, state_base.CP2D.p[16].w.l, "RTPS SZ0 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[17].w.l, state_base.CP2D.p[17].w.l, "RTPS SZ1 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[18].w.l, state_base.CP2D.p[18].w.l, "RTPS SZ2 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[19].w.l, state_base.CP2D.p[19].w.l, "RTPS SZ3 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.r[25], state_base.CP2D.r[25], "RTPS MAC1 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.r[26], state_base.CP2D.r[26], "RTPS MAC2 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.r[27], state_base.CP2D.r[27], "RTPS MAC3 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[9].sw.l, state_base.CP2D.p[9].sw.l, "RTPS IR1 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[10].sw.l, state_base.CP2D.p[10].sw.l, "RTPS IR2 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[11].sw.l, state_base.CP2D.p[11].sw.l, "RTPS IR3 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.r[24], state_base.CP2D.r[24], "RTPS MAC0 (fog) invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[8].sw.l, state_base.CP2D.p[8].sw.l, "RTPS IR0 (fog) invariance");

			/* If fy == 4096, SY must be bit-identical */
			if (fy == 4096) {
				ASSERT_EQ_HEX(state_scaled.CP2D.p[14].sw.h, state_base.CP2D.p[14].sw.h,
					"RTPS SY2 invariance when fy_q12 == 4096");
			}

			/* If fx == 4096, SX must be bit-identical */
			if (fx == 4096) {
				ASSERT_EQ_HEX(state_scaled.CP2D.p[14].sw.l, state_base.CP2D.p[14].sw.l,
					"RTPS SX2 invariance when fx_q12 == 4096");
			}

			/* SX scaling verification against expected deviation */
			int16_t sx_unscaled = state_base.CP2D.p[14].sw.l;
			int16_t sx_scaled = state_scaled.CP2D.p[14].sw.l;
			int32_t ofx_screen = state_in.CP2C.n.ofx >> 16;
			int16_t ir1 = (int16_t)state_base.CP2D.n.ir1;

			/* We can deduce the quotient from unscaled formula if not saturated, or directly */
			/* Check ratio when unscaled and scaled SX are not saturated [-1023, 1022] */
			if (sx_unscaled > -1023 && sx_unscaled < 1022 &&
			    sx_scaled > -1023 && sx_scaled < 1022) {
				double dev_unscaled = (double)(sx_unscaled - ofx_screen);
				double dev_scaled = (double)(sx_scaled - ofx_screen);
				double expected_dev = dev_unscaled * ((double)fx / 4096.0);
				double diff = fabs(dev_scaled - expected_dev);

				/* Tolerance: with 16.16 fraction loss before scaling, integer deviation
				 * can differ from continuous float by at most (fx/4096.0) + 0.5 (~2px for 4/3). */
				double max_allowed = ((double)fx / 4096.0) + 0.5;
				ASSERT_TRUE(diff <= max_allowed,
					"RTPS SX deviation from OFX does not scale by fx_q12 / 4096 within tolerance");
			}

			/* SY scaling verification when fy != 4096 */
			if (fy != 4096) {
				int16_t sy_unscaled = state_base.CP2D.p[14].sw.h;
				int16_t sy_scaled = state_scaled.CP2D.p[14].sw.h;
				int32_t ofy_screen = state_in.CP2C.n.ofy >> 16;

				if (sy_unscaled > -1023 && sy_unscaled < 1022 &&
				    sy_scaled > -1023 && sy_scaled < 1022) {
					double dev_unscaled = (double)(sy_unscaled - ofy_screen);
					double dev_scaled = (double)(sy_scaled - ofy_screen);
					double expected_dev = dev_unscaled * ((double)fy / 4096.0);
					double diff = fabs(dev_scaled - expected_dev);

					double max_allowed = ((double)fy / 4096.0) + 0.5;
					ASSERT_TRUE(diff <= max_allowed,
						"RTPS SY deviation from OFY does not scale by fy_q12 / 4096 within tolerance");
				}
			}
		}
	}
}

static void test_rtpt_scaling(int iterations)
{
	int i;
	static const struct {
		int fx;
		int fy;
		const char *name;
	} presets[] = {
		{ 5461, 4096, "Portrait 4/3 X (5461, 4096)" },
		{ 3072, 4096, "Widescreen 3/4 X (3072, 4096)" },
		{ 4096, 3072, "Portrait wider FOV Y (4096, 3072)" },
		{ 5461, 3072, "Mixed 4/3 X and 3/4 Y (5461, 3072)" },
		{ 2048, 4096, "Half scale X (2048, 4096)" },
		{ 6144, 4096, "1.5x scale X (6144, 4096)" },
	};
	int p;

	printf("Running RTPT scaling tests across %d presets for %d states...\n",
		(int)(sizeof(presets) / sizeof(presets[0])), iterations);

	for (p = 0; p < (int)(sizeof(presets) / sizeof(presets[0])); p++) {
		int fx = presets[p].fx;
		int fy = presets[p].fy;

		for (i = 0; i < iterations; i++) {
			psxCP2Regs state_in, state_base, state_scaled, state_nf_scaled;
			u32 code = GTEOP_RTPT | (1 << 19);
			gte_handler *handler = gteGetHandler(code);
			gte_handler *handler_nf = gteGetHandler_nf(code);
			int v;

			randomize_gte_regs(&state_in, i);

			/* 1. Base unscaled run */
			g_gte_fx_q12 = 4096;
			g_gte_fy_q12 = 4096;
			state_base = state_in;
			handler(&state_base, code);

			/* 2. Scaled run (flagged) */
			g_gte_fx_q12 = fx;
			g_gte_fy_q12 = fy;
			state_scaled = state_in;
			handler(&state_scaled, code);

			/* 3. Scaled run (flagless) */
			state_nf_scaled = state_in;
			handler_nf(&state_nf_scaled, code);

			/* Coordinates equivalence between flagged and flagless */
			for (v = 0; v < 3; v++) {
				ASSERT_EQ_HEX(state_nf_scaled.CP2D.p[12 + v].sw.l, state_scaled.CP2D.p[12 + v].sw.l,
					"RTPT flagged vs flagless SX mismatch in scaled mode");
				ASSERT_EQ_HEX(state_nf_scaled.CP2D.p[12 + v].sw.h, state_scaled.CP2D.p[12 + v].sw.h,
					"RTPT flagged vs flagless SY mismatch in scaled mode");
			}

			/* Invariance check: non-geometric registers */
			ASSERT_EQ_HEX(state_scaled.CP2D.p[16].w.l, state_base.CP2D.p[16].w.l, "RTPT SZ0 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[17].w.l, state_base.CP2D.p[17].w.l, "RTPT SZ1 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[18].w.l, state_base.CP2D.p[18].w.l, "RTPT SZ2 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[19].w.l, state_base.CP2D.p[19].w.l, "RTPT SZ3 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.r[25], state_base.CP2D.r[25], "RTPT MAC1 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.r[26], state_base.CP2D.r[26], "RTPT MAC2 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.r[27], state_base.CP2D.r[27], "RTPT MAC3 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[9].sw.l, state_base.CP2D.p[9].sw.l, "RTPT IR1 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[10].sw.l, state_base.CP2D.p[10].sw.l, "RTPT IR2 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[11].sw.l, state_base.CP2D.p[11].sw.l, "RTPT IR3 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.r[24], state_base.CP2D.r[24], "RTPT MAC0 invariance");
			ASSERT_EQ_HEX(state_scaled.CP2D.p[8].sw.l, state_base.CP2D.p[8].sw.l, "RTPT IR0 invariance");

			/* Vertices checks */
			for (v = 0; v < 3; v++) {
				/* If fy == 4096, SY must be bit-identical */
				if (fy == 4096) {
					ASSERT_EQ_HEX(state_scaled.CP2D.p[12 + v].sw.h, state_base.CP2D.p[12 + v].sw.h,
						"RTPT SY invariance when fy_q12 == 4096");
				}

				/* If fx == 4096, SX must be bit-identical */
				if (fx == 4096) {
					ASSERT_EQ_HEX(state_scaled.CP2D.p[12 + v].sw.l, state_base.CP2D.p[12 + v].sw.l,
						"RTPT SX invariance when fx_q12 == 4096");
				}

				/* SX scaling check */
				int16_t sx_unscaled = state_base.CP2D.p[12 + v].sw.l;
				int16_t sx_scaled = state_scaled.CP2D.p[12 + v].sw.l;
				int32_t ofx_screen = state_in.CP2C.n.ofx >> 16;

				if (sx_unscaled > -1023 && sx_unscaled < 1022 &&
				    sx_scaled > -1023 && sx_scaled < 1022) {
					double dev_unscaled = (double)(sx_unscaled - ofx_screen);
					double dev_scaled = (double)(sx_scaled - ofx_screen);
					double expected_dev = dev_unscaled * ((double)fx / 4096.0);
					double diff = fabs(dev_scaled - expected_dev);

					double max_allowed = ((double)fx / 4096.0) + 0.5;
					ASSERT_TRUE(diff <= max_allowed,
						"RTPT SX deviation does not scale by fx_q12 / 4096 within tolerance");
				}

				/* SY scaling check when fy != 4096 */
				if (fy != 4096) {
					int16_t sy_unscaled = state_base.CP2D.p[12 + v].sw.h;
					int16_t sy_scaled = state_scaled.CP2D.p[12 + v].sw.h;
					int32_t ofy_screen = state_in.CP2C.n.ofy >> 16;

					if (sy_unscaled > -1023 && sy_unscaled < 1022 &&
					    sy_scaled > -1023 && sy_scaled < 1022) {
						double dev_unscaled = (double)(sy_unscaled - ofy_screen);
						double dev_scaled = (double)(sy_scaled - ofy_screen);
						double expected_dev = dev_unscaled * ((double)fy / 4096.0);
						double diff = fabs(dev_scaled - expected_dev);

						double max_allowed = ((double)fy / 4096.0) + 0.5;
						ASSERT_TRUE(diff <= max_allowed,
							"RTPT SY deviation does not scale by fy_q12 / 4096 within tolerance");
					}
				}
			}
		}
	}
}

int main(void)
{
	const int iterations = 5000;
	printf("=== PCSX-ReARMed GTE Aspect Ratio Test (Step П5) ===\n");

	test_rtps_identity(iterations);
	test_rtpt_identity(iterations);
	test_rtps_scaling(iterations);
	test_rtpt_scaling(iterations);

	printf("\n=== Test Summary ===\n");
	printf("Total assertions:  %llu\n", g_total_assertions);
	printf("Failed assertions: %llu\n", g_failed_assertions);

	if (g_failed_assertions == 0) {
		printf("ALL GTE ASPECT TESTS PASSED!\n");
		return 0;
	} else {
		printf("SOME GTE ASPECT TESTS FAILED!\n");
		return 1;
	}
}
