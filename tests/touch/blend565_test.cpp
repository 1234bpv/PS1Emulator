// Exhaustive test of RGB565 single-multiply blending vs previous formula (0a6e13b).
// Checks that across all R (0..31), G (0..63), B (0..31), all alpha / coverage (0..255)
// and both ink colors (white and black), the single-multiply 565 blend matches the
// previous formula or deviates by at most 1 LSB per channel.

#include <stdio.h>
#include <stdlib.h>
#include "touchdraw.cpp"

// Previous blend formula from commit 0a6e13b:
static inline unsigned dcMix16_old(unsigned aV, int aWhite, int aA)
	{
	unsigned r = aV >> 11, g = (aV >> 5) & 63, b = aV & 31;
	if (aWhite)
		{
		r += ((31 - r) * (aA + 1)) >> 8;
		g += ((63 - g) * (aA + 1)) >> 8;
		b += ((31 - b) * (aA + 1)) >> 8;
		}
	else
		{
		r = (r * (256 - aA)) >> 8;
		g = (g * (256 - aA)) >> 8;
		b = (b * (256 - aA)) >> 8;
		}
	return (r << 11) | (g << 5) | b;
	}

int main(void)
	{
	int maxDiffR = 0, maxDiffG = 0, maxDiffB = 0;
	unsigned long totalTests = 0;
	unsigned long exactMatches = 0;
	unsigned long withinOneLsb = 0;
	unsigned long badCount = 0;

	for (int white = 0; white <= 1; white++)
		{
		for (int a = 0; a <= 255; a++)
			{
			for (int v = 0; v <= 65535; v++)
				{
				// As in dcRun:
				// a == 255 is direct solid ink (white: 0xFFFF, black: 0x0000).
				// a == 0 is untouched background pixel.
				// a in 1..254 is blended with ink.
				unsigned short oldVal;
				if (a == 255)
					oldVal = (unsigned short)(white ? 0xFFFF : 0x0000);
				else if (a == 0)
					oldVal = (unsigned short)v;
				else
					oldVal = (unsigned short)dcMix16_old(v, white, a);

				// New implementation: dcMixCov16 is called for 1..254, but dcMixCov16
				// also evaluates cleanly for 0 and 255.
				const unsigned short newVal = (a == 255)
					? (unsigned short)(white ? 0xFFFF : 0x0000)
					: (a == 0 ? (unsigned short)v : dcMixCov16((unsigned short)v, a, white));

				const int oldR = oldVal >> 11;
				const int oldG = (oldVal >> 5) & 63;
				const int oldB = oldVal & 31;

				const int newR = newVal >> 11;
				const int newG = (newVal >> 5) & 63;
				const int newB = newVal & 31;

				const int diffR = abs(oldR - newR);
				const int diffG = abs(oldG - newG);
				const int diffB = abs(oldB - newB);

				if (diffR > maxDiffR) maxDiffR = diffR;
				if (diffG > maxDiffG) maxDiffG = diffG;
				if (diffB > maxDiffB) maxDiffB = diffB;

				totalTests++;
				if (diffR == 0 && diffG == 0 && diffB == 0)
					exactMatches++;
				if (diffR <= 1 && diffG <= 1 && diffB <= 1)
					withinOneLsb++;
				else
					{
					badCount++;
					if (badCount <= 10)
						{
						printf("FAIL: white=%d a=%d v=0x%04X (R=%d G=%d B=%d) old=(%d,%d,%d) new=(%d,%d,%d) diff=(%d,%d,%d)\n",
							white, a, v, v >> 11, (v >> 5) & 63, v & 31, oldR, oldG, oldB, newR, newG, newB, diffR, diffG, diffB);
						}
					}
				}
			}
		}

	printf("blend565_test: total=%lu, exact=%lu (%.2f%%), within_1lsb=%lu, bad=%lu\n",
		totalTests, exactMatches, 100.0 * (double)exactMatches / (double)totalTests, withinOneLsb, badCount);
	printf("blend565_test: max channel diff: R=%d, G=%d, B=%d\n", maxDiffR, maxDiffG, maxDiffB);

	if (badCount != 0 || maxDiffR > 1 || maxDiffG > 1 || maxDiffB > 1)
		{
		printf("blend565_test: FAIL (difference exceeded 1 LSB)\n");
		return 1;
		}

	printf("blend565_test: PASS (all channels within <= 1 LSB)\n");
	return 0;
	}
