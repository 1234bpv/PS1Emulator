// Panel drawing, portable half: see touchdraw.h. Integer arithmetic only. Distances and
// sizes are in 1/16 pixel; coverage is 0..255 with a ramp one pixel wide across each edge.
#include "touchdraw.h"

enum
	{
	KDrawRow = 512,        // widest control the builder accepts, px
	KDrawMaxPrims = 96,    // primitives of one control, all passes together
	KDrawHalo = 16,        // dark halo beyond a light stroke: one pixel, 1/16 px units
	KDrawMinHalf = 8,      // half thickness of a light stroke: at least half a pixel
	KDrawMargin = 4        // px around a control's box that its ink can reach
	};

enum { EDrawPassFill = 0, EDrawPassDark = 1, EDrawPassLight = 2 };

enum
	{
	EDrawRing = 0,        // circle outline: v0 cx, v1 cy, v2 radius, v3 half thickness
	EDrawRectRing = 1,    // rectangle outline: v0 cx, v1 cy, v2 half width, v3 half height, v4 half thickness
	EDrawDisc = 2,        // filled circle: v0 cx, v1 cy, v2 radius, v3 growth
	EDrawRectFill = 3,    // filled rectangle: v0 cx, v1 cy, v2 half width, v3 half height, v4 growth
	EDrawSeg = 4,         // segment: v0..v3 the ends, v4 half thickness
	EDrawTri = 5          // filled triangle: v0..v5 the corners, v6 growth
	};

// Stroke font of the labels. A letter is 4 units wide and 6 high and advances 6 units;
// a row is the character, the segment count, then x0 y0 x1 y1 per segment.
static const signed char KDrawFont[9][22] =
	{
	{ 'L', 2, 0,0, 0,6,  0,6, 4,6 },
	{ 'R', 5, 0,0, 0,6,  0,0, 4,0,  4,0, 4,3,  4,3, 0,3,  1,3, 4,6 },
	{ 'S', 5, 4,0, 0,0,  0,0, 0,3,  0,3, 4,3,  4,3, 4,6,  4,6, 0,6 },
	{ 'E', 4, 4,0, 0,0,  0,0, 0,6,  0,6, 4,6,  0,3, 3,3 },
	{ 'C', 3, 4,0, 0,0,  0,0, 0,6,  0,6, 4,6 },
	{ 'T', 2, 0,0, 4,0,  2,0, 2,6 },
	{ 'A', 3, 0,6, 2,0,  2,0, 4,6,  1,4, 3,4 },
	{ '1', 2, 1,1, 2,0,  2,0, 2,6 },
	{ '2', 5, 0,1, 1,0,  1,0, 3,0,  3,0, 4,1,  4,1, 0,6,  0,6, 4,6 }
	};

// Direction k of the d-pad (0 up, 1 right, 2 down, 3 left) as the pad bit and the unit step.
static const int KDrawDirBit[4] = { EVpadUp, EVpadRight, EVpadDown, EVpadLeft };
static const int KDrawDirX[4] = { 0, 1, 0, -1 };
static const int KDrawDirY[4] = { -1, 0, 1, 0 };

struct TDrawPrim
	{
	unsigned char iKind;
	unsigned char iPass;
	unsigned char iPart;
	int x0, y0, x1, y1;    // bounding box in pixels, inclusive
	int v[7];              // geometry in 1/16 pixel, see the kinds above
	};

// Scratch of the builder: the primitives of one control and one row of coverage.
static TDrawPrim gPrim[KDrawMaxPrims];
static int gPrimCount;
static unsigned char gRowCov[KDrawRow];
static unsigned char gRowPart[KDrawRow];

static int dcClamp255(int aV)
	{
	return aV < 0 ? 0 : (aV > 255 ? 255 : aV);
	}

static int dcAbs(int aV)
	{
	return aV < 0 ? -aV : aV;
	}

static int dcMax(int aA, int aB)
	{
	return aA > aB ? aA : aB;
	}

static int dcMin(int aA, int aB)
	{
	return aA < aB ? aA : aB;
	}

static int dcDist(int aDx, int aDy)
	{
	return VpadIsqrt(aDx * aDx + aDy * aDy);
	}

// Coverage of a stroke whose centre line is aE away, half thickness aHalf, 1/16 px.
static int dcStroke(int aE, int aHalf)
	{
	return dcClamp255((aHalf + 8 - aE) * 16);
	}

// Coverage of a fill whose signed distance inside is aS (positive inside), grown by aGrow.
static int dcFill(int aS, int aGrow)
	{
	return dcClamp255((aS + aGrow + 8) * 16);
	}

static int dcSegDist(int aX, int aY, int aX0, int aY0, int aX1, int aY1)
	{
	const int vx = aX1 - aX0, vy = aY1 - aY0;
	const int wx = aX - aX0, wy = aY - aY0;
	const int l2 = vx * vx + vy * vy;
	const int dot = wx * vx + wy * vy;
	if (l2 == 0 || dot <= 0)
		return dcDist(wx, wy);
	if (dot >= l2)
		return dcDist(aX - aX1, aY - aY1);
	return dcAbs(vx * wy - vy * wx) / VpadIsqrt(l2);
	}

static int dcCover(const TDrawPrim &aP, int aX, int aY)
	{
	const int *v = aP.v;
	switch (aP.iKind)
		{
		case EDrawRing:
			return dcStroke(dcAbs(dcDist(aX - v[0], aY - v[1]) - v[2]), v[3]);
		case EDrawRectRing:
			return dcStroke(dcAbs(dcMax(dcAbs(aX - v[0]) - v[2], dcAbs(aY - v[1]) - v[3])), v[4]);
		case EDrawDisc:
			return dcFill(v[2] - dcDist(aX - v[0], aY - v[1]), v[3]);
		case EDrawRectFill:
			return dcFill(dcMin(v[2] - dcAbs(aX - v[0]), v[3] - dcAbs(aY - v[1])), v[4]);
		case EDrawSeg:
			return dcStroke(dcSegDist(aX, aY, v[0], v[1], v[2], v[3]), v[4]);
		default:
			{
			// Convex and counter-clockwise (dcTri makes it so): inside is positive for every edge.
			int s = 0x7fffffff;
			for (int e = 0; e < 3; e++)
				{
				const int ax = v[2 * e], ay = v[2 * e + 1];
				const int ex = v[(2 * e + 2) % 6] - ax, ey = v[(2 * e + 3) % 6] - ay;
				const int len = dcMax(VpadIsqrt(ex * ex + ey * ey), 1);
				const int d = (ex * (aY - ay) - ey * (aX - ax)) / len;
				if (d < s)
					s = d;
				}
			return dcFill(s, v[6]);
			}
		}
	}

// The box of a primitive from its geometry, given in 1/16 pixel, inclusive. Two pixels of
// margin cover the truncation of the division and the ramp.
static void dcBox(TDrawPrim &aP, int aX0, int aY0, int aX1, int aY1)
	{
	aP.x0 = aX0 / 16 - 2;
	aP.y0 = aY0 / 16 - 2;
	aP.x1 = aX1 / 16 + 2;
	aP.y1 = aY1 / 16 + 2;
	}

static TDrawPrim *dcNew(int aKind, int aPass, int aPart)
	{
	if (gPrimCount >= KDrawMaxPrims)
		return 0;
	TDrawPrim *p = &gPrim[gPrimCount++];
	p->iKind = (unsigned char)aKind;
	p->iPass = (unsigned char)aPass;
	p->iPart = (unsigned char)aPart;
	return p;
	}

static int dcRing(int aPass, int aPart, int aCx, int aCy, int aR, int aHalf)
	{
	TDrawPrim *p = dcNew(EDrawRing, aPass, aPart);
	if (!p)
		return 0;
	p->v[0] = aCx; p->v[1] = aCy; p->v[2] = aR; p->v[3] = aHalf;
	dcBox(*p, aCx - aR - aHalf, aCy - aR - aHalf, aCx + aR + aHalf, aCy + aR + aHalf);
	return 1;
	}

static int dcRectRing(int aPass, int aPart, int aCx, int aCy, int aHw, int aHh, int aHalf)
	{
	TDrawPrim *p = dcNew(EDrawRectRing, aPass, aPart);
	if (!p)
		return 0;
	p->v[0] = aCx; p->v[1] = aCy; p->v[2] = aHw; p->v[3] = aHh; p->v[4] = aHalf;
	dcBox(*p, aCx - aHw - aHalf, aCy - aHh - aHalf, aCx + aHw + aHalf, aCy + aHh + aHalf);
	return 1;
	}

static int dcDisc(int aPass, int aPart, int aCx, int aCy, int aR, int aGrow)
	{
	TDrawPrim *p = dcNew(EDrawDisc, aPass, aPart);
	if (!p)
		return 0;
	p->v[0] = aCx; p->v[1] = aCy; p->v[2] = aR; p->v[3] = aGrow;
	dcBox(*p, aCx - aR - aGrow, aCy - aR - aGrow, aCx + aR + aGrow, aCy + aR + aGrow);
	return 1;
	}

static int dcRectFill(int aPass, int aPart, int aCx, int aCy, int aHw, int aHh, int aGrow)
	{
	TDrawPrim *p = dcNew(EDrawRectFill, aPass, aPart);
	if (!p)
		return 0;
	p->v[0] = aCx; p->v[1] = aCy; p->v[2] = aHw; p->v[3] = aHh; p->v[4] = aGrow;
	dcBox(*p, aCx - aHw - aGrow, aCy - aHh - aGrow, aCx + aHw + aGrow, aCy + aHh + aGrow);
	return 1;
	}

static int dcSeg(int aPass, int aPart, int aX0, int aY0, int aX1, int aY1, int aHalf)
	{
	TDrawPrim *p = dcNew(EDrawSeg, aPass, aPart);
	if (!p)
		return 0;
	p->v[0] = aX0; p->v[1] = aY0; p->v[2] = aX1; p->v[3] = aY1; p->v[4] = aHalf;
	dcBox(*p, dcMin(aX0, aX1) - aHalf, dcMin(aY0, aY1) - aHalf,
		dcMax(aX0, aX1) + aHalf, dcMax(aY0, aY1) + aHalf);
	return 1;
	}

// Triangle with corners aXy[0..5] (x, y pairs) and growth aGrow. The corners are put in
// counter-clockwise order by the sign of the area, which dcCover relies on.
static int dcTri(int aPass, int aPart, const int *aXy, int aGrow)
	{
	TDrawPrim *p = dcNew(EDrawTri, aPass, aPart);
	if (!p)
		return 0;
	int area = 0;
	for (int e = 0; e < 3; e++)
		area += aXy[2 * e] * aXy[(2 * e + 3) % 6] - aXy[(2 * e + 2) % 6] * aXy[2 * e + 1];
	for (int k = 0; k < 6; k++)
		p->v[k] = aXy[k];
	if (area < 0)
		{
		const int tx = p->v[2], ty = p->v[3];
		p->v[2] = p->v[4];
		p->v[3] = p->v[5];
		p->v[4] = tx;
		p->v[5] = ty;
		}
	p->v[6] = aGrow;
	int x0 = p->v[0], x1 = p->v[0], y0 = p->v[1], y1 = p->v[1];
	for (int k = 0; k < 6; k += 2)
		{
		x0 = dcMin(x0, p->v[k]); x1 = dcMax(x1, p->v[k]);
		y0 = dcMin(y0, p->v[k + 1]); y1 = dcMax(y1, p->v[k + 1]);
		}
	dcBox(*p, x0 - aGrow, y0 - aGrow, x1 + aGrow, y1 + aGrow);
	return 1;
	}

static int dcFontRow(int aChar)
	{
	for (int k = 0; k < 9; k++)
		if (KDrawFont[k][0] == aChar)
			return k;
	return -1;
	}

static const char *dcLabel(int aIx)
	{
	switch (aIx)
		{
		case EVpadIxL1: return "L1";
		case EVpadIxL2: return "L2";
		case EVpadIxR1: return "R1";
		case EVpadIxR2: return "R2";
		case EVpadIxSelect: return "SELECT";
		case EVpadIxStart: return "START";
		default: return "";
		}
	}

// A control is a rectangle when its shape says so, but the d-pad and the sticks are always circles.
static int dcIsRect(const TVpadControl &aC)
	{
	return aC.iShape == EVpadShapeRect && aC.iKind != EVpadKindDpad && aC.iKind != EVpadKindStick;
	}

// Travel of the stick head from the centre of the stick at full deflection, px. The head's
// dark halo stays clear of the outline by two pixels.
static int dcKnobTravel(const TVpadControl &aC)
	{
	return aC.iRx - aC.iRx / 3 - 3;
	}

// Label of a rectangle control as strokes of the stroke font, fitted into the box: the cap
// height is two thirds of the box height, and the label is at most 0.8 of the box width.
static int dcLabelPrims(const TVpadControl &aC, const char *aText, int aPass, int aHalf)
	{
	int n = 0;
	while (aText[n])
		n++;
	if (!n)
		return 1;
	const int units = n * 6 - 2;
	int k = (aC.iRy * 2 / 3) * 16 / 6;
	const int kw = (aC.iRx * 16 * 8) / (5 * units);
	if (kw < k)
		k = kw;
	if (k < 1)
		k = 1;
	const int x0 = aC.iCx * 16 - units * k / 2;
	const int y0 = aC.iCy * 16 - 3 * k;
	for (int j = 0; j < n; j++)
		{
		const int f = dcFontRow(aText[j]);
		if (f < 0)
			continue;
		const signed char *g = KDrawFont[f];
		const int ox = x0 + j * 6 * k;
		for (int s = 0; s < g[1]; s++)
			{
			const signed char *q = g + 2 + 4 * s;
			if (!dcSeg(aPass, EVpadPartBody, ox + q[0] * k, y0 + q[1] * k,
				ox + q[2] * k, y0 + q[3] * k, aHalf))
				return 0;
			}
		}
	return 1;
	}

// Symbol of a round button: aS is its half size, 1/16 px.
static int dcGlyph(int aIx, int aCx, int aCy, int aS, int aPass, int aHalf)
	{
	const int q = aS * 3 / 4;
	const int by = aCy + aS * 2 / 3;
	switch (aIx)
		{
		case EVpadIxTriangle:
			return dcSeg(aPass, EVpadPartBody, aCx, aCy - aS, aCx + aS, by, aHalf)
				&& dcSeg(aPass, EVpadPartBody, aCx + aS, by, aCx - aS, by, aHalf)
				&& dcSeg(aPass, EVpadPartBody, aCx - aS, by, aCx, aCy - aS, aHalf);
		case EVpadIxCircle:
			return dcRing(aPass, EVpadPartBody, aCx, aCy, q, aHalf);
		case EVpadIxCross:
			return dcSeg(aPass, EVpadPartBody, aCx - q, aCy - q, aCx + q, aCy + q, aHalf)
				&& dcSeg(aPass, EVpadPartBody, aCx - q, aCy + q, aCx + q, aCy - q, aHalf);
		case EVpadIxSquare:
			return dcRectRing(aPass, EVpadPartBody, aCx, aCy, q, q, aHalf);
		default:
			return 1;
		}
	}

// Primitives of control aI, all passes. aHl and aHt are the half thickness of the light
// strokes of outlines and of symbols and labels, 1/16 px. The dark pass uses the same
// shapes thickened by KDrawHalo; the fill pass holds the pressed fills.
static int dcControl(const TVpadLayout &aL, int aI, int aHl, int aHt)
	{
	const TVpadControl &c = aL.iC[aI];
	const int cx = c.iCx * 16, cy = c.iCy * 16, r = c.iRx * 16;
	const int rect = dcIsRect(c);
	const int rh = c.iRy * 16;
	for (int pass = EDrawPassFill; pass <= EDrawPassLight; pass++)
		{
		const int dark = pass == EDrawPassDark;
		const int hl = dark ? aHl + KDrawHalo : aHl;
		const int ht = dark ? aHt + KDrawHalo : aHt;
		const int grow = dark ? KDrawHalo : 0;
		if (pass == EDrawPassFill)
			{
			if (c.iKind == EVpadKindDpad)
				{
				const int armD = c.iRx * 3 / 5 * 16, armH = c.iRx / 5 * 16;
				for (int k = 0; k < 4; k++)
					if (!dcRectFill(pass, EVpadPartArm + k, cx + KDrawDirX[k] * armD,
						cy + KDrawDirY[k] * armD, armH, armH, 0))
						return 0;
				}
			else if (c.iKind == EVpadKindButton || c.iKind == EVpadKindAction)
				{
				if (rect ? !dcRectFill(pass, EVpadPartFill, cx, cy, r - aHl, rh - aHl, 0)
					: !dcDisc(pass, EVpadPartFill, cx, cy, r - aHl, 0))
					return 0;
				}
			continue;
			}
		if (c.iKind == EVpadKindDpad)
			{
			if (!dcRing(pass, EVpadPartBody, cx, cy, r, hl))
				return 0;
			const int tip = c.iRx * 4 / 5 * 16, base = c.iRx * 2 / 5 * 16;
			int half = c.iRx / 7 * 16;
			if (half < KDrawMinHalf)
				half = KDrawMinHalf;
			for (int k = 0; k < 4; k++)
				{
				const int dx = KDrawDirX[k], dy = KDrawDirY[k];
				const int bx = cx + dx * base, by = cy + dy * base;
				const int xy[6] = { cx + dx * tip, cy + dy * tip,
					bx - dy * half, by + dx * half, bx + dy * half, by - dx * half };
				if (!dcTri(pass, EVpadPartUp + k, xy, grow))
					return 0;
				}
			}
		else if (c.iKind == EVpadKindStick)
			{
			if (!dcRing(pass, EVpadPartBody, cx, cy, r, hl)
				|| !dcDisc(pass, EVpadPartKnob, cx, cy, c.iRx / 3 * 16, grow))
				return 0;
			}
		else if (rect)
			{
			if (!dcRectRing(pass, EVpadPartBody, cx, cy, r, rh, hl)
				|| !dcLabelPrims(c, dcLabel(aI), pass, ht))
				return 0;
			}
		else
			{
			if (!dcRing(pass, EVpadPartBody, cx, cy, r, hl))
				return 0;
			if (c.iKind == EVpadKindAction)
				{
				const int hr = c.iRx * 8, ro = c.iRx * 16 / 3;
				if (!dcSeg(pass, EVpadPartBody, cx - hr, cy - ro, cx + hr, cy - ro, ht)
					|| !dcSeg(pass, EVpadPartBody, cx - hr, cy, cx + hr, cy, ht)
					|| !dcSeg(pass, EVpadPartBody, cx - hr, cy + ro, cx + hr, cy + ro, ht))
					return 0;
				}
			else if (!dcGlyph(aI, cx, cy, c.iRx * 8, pass, ht))
				return 0;
			}
		}
	return 1;
	}

// Rasterises the primitives of one pass over the box of the control into spans.
static int dcEmit(TVpadArt &aArt, int aPass, int aX0, int aY0, int aX1, int aY1)
	{
	const int w = aX1 - aX0 + 1;
	for (int y = aY0; y <= aY1; y++)
		{
		for (int k = 0; k < w; k++)
			{
			gRowCov[k] = 0;
			gRowPart[k] = 0;
			}
		for (int p = 0; p < gPrimCount; p++)
			{
			const TDrawPrim &q = gPrim[p];
			if (q.iPass != aPass || y < q.y0 || y > q.y1)
				continue;
			const int xa = dcMax(q.x0, aX0), xb = dcMin(q.x1, aX1);
			for (int x = xa; x <= xb; x++)
				{
				const int c = dcCover(q, x * 16, y * 16);
				const int k = x - aX0;
				if (c > gRowCov[k])
					{
					gRowCov[k] = (unsigned char)c;
					gRowPart[k] = q.iPart;
					}
				}
			}
		int x = 0;
		while (x < w)
			{
			if (!gRowCov[x])
				{
				x++;
				continue;
				}
			const int part = gRowPart[x];
			const int start = x;
			while (x < w && gRowCov[x] && gRowPart[x] == part)
				x++;
			const int len = x - start;
			if (aArt.iSpanCount >= aArt.iSpanCap || aArt.iAlphaCount + len > aArt.iAlphaCap)
				return 0;
			TVpadSpan &s = aArt.iSpan[aArt.iSpanCount++];
			s.iX = (short)(aX0 + start);
			s.iY = (short)y;
			s.iLen = (unsigned short)len;
			s.iOff = (unsigned)aArt.iAlphaCount;
			s.iPart = (unsigned char)part;
			for (int k = 0; k < len; k++)
				aArt.iAlpha[aArt.iAlphaCount++] = gRowCov[start + k];
			}
		}
	return 1;
	}

int VpadArtBuild(TVpadArt &aArt, const TVpadLayout &aLayout, int aUmPerPx)
	{
	const int um = aUmPerPx < 20 ? 20 : aUmPerPx;
	// The light stroke is a quarter of a millimetre wide (half of it is the half thickness),
	// and never thinner than a pixel.
	int hl = (125 * 16 + um / 2) / um;
	if (hl < KDrawMinHalf)
		hl = KDrawMinHalf;
	int ht = hl * 3 / 4;
	if (ht < KDrawMinHalf)
		ht = KDrawMinHalf;
	int count = aLayout.iCount;
	if (count < 0)
		count = 0;
	if (count > KVpadMaxControls)
		count = KVpadMaxControls;
	aArt.iCount = count;
	aArt.iSpanCount = 0;
	aArt.iAlphaCount = 0;
	for (int i = 0; i < count; i++)
		{
		aArt.iFirst[i] = aArt.iSpanCount;
		aArt.iDark[i] = aArt.iSpanCount;
		aArt.iLight[i] = aArt.iSpanCount;
		const TVpadControl &c = aLayout.iC[i];
		if (c.iRx <= 0 || (dcIsRect(c) && c.iRy <= 0))
			continue;
		gPrimCount = 0;
		if (!dcControl(aLayout, i, hl, ht))
			{
			aArt.iCount = 0;
			return 0;
			}
		if (gPrimCount == 0)
			continue;
		int bx0 = gPrim[0].x0, bx1 = gPrim[0].x1, by0 = gPrim[0].y0, by1 = gPrim[0].y1;
		for (int p = 1; p < gPrimCount; p++)
			{
			bx0 = dcMin(bx0, gPrim[p].x0); bx1 = dcMax(bx1, gPrim[p].x1);
			by0 = dcMin(by0, gPrim[p].y0); by1 = dcMax(by1, gPrim[p].y1);
			}
		if (bx1 - bx0 + 1 > KDrawRow)
			{
			aArt.iCount = 0;
			return 0;
			}
		if (!dcEmit(aArt, EDrawPassFill, bx0, by0, bx1, by1))
			{
			aArt.iCount = 0;
			return 0;
			}
		aArt.iDark[i] = aArt.iSpanCount;
		if (!dcEmit(aArt, EDrawPassDark, bx0, by0, bx1, by1))
			{
			aArt.iCount = 0;
			return 0;
			}
		aArt.iLight[i] = aArt.iSpanCount;
		if (!dcEmit(aArt, EDrawPassLight, bx0, by0, bx1, by1))
			{
			aArt.iCount = 0;
			return 0;
			}
		}
	aArt.iFirst[count] = aArt.iSpanCount;
	return 1;
	}

// Pixel of RGB565 mixed with white (aWhite) or black by coverage aA, 0..255.
static unsigned dcMix16(unsigned aV, int aWhite, int aA)
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

// Pixel of XRGB8888 mixed the same way, channel by channel.
static unsigned dcMix32(unsigned aV, int aWhite, int aA)
	{
	unsigned out = 0;
	for (int sh = 0; sh < 24; sh += 8)
		{
		unsigned c = (aV >> sh) & 255;
		if (aWhite)
			c += ((255 - c) * (aA + 1)) >> 8;
		else
			c = (c * (256 - aA)) >> 8;
		out |= c << sh;
		}
	return out;
	}

// Mixes a row run aX0..aX1-1 (already inside the canvas) with the coverage aCov, which
// starts at aX0. aOp scales the coverage: 255 leaves it as it is.
static void dcRun(const TVpadCanvas &aC, int aY, int aX0, int aX1, const unsigned char *aCov,
	int aWhite, int aOp)
	{
	unsigned char *row = aC.iDst + aY * aC.iStride;
	// Most ink pixels of a stroke are fully covered: they take the ink colour without a mix.
	for (int x = aX0; x < aX1; x++, aCov++)
		{
		int a = *aCov;
		if (aOp != 255)
			a = (a * aOp + 127) / 255;
		if (!a)
			continue;
		if (aC.iPixB == 4)
			{
			unsigned *p = (unsigned *)(row + x * 4);
			*p = a == 255 ? (aWhite ? 0xFFFFFFu : 0u) : dcMix32(*p, aWhite, a);
			}
		else
			{
			unsigned short *p = (unsigned short *)(row + x * 2);
			*p = a == 255 ? (unsigned short)(aWhite ? 0xFFFF : 0) : (unsigned short)dcMix16(*p, aWhite, a);
			}
		}
	}

static void dcClearRun(const TVpadCanvas &aC, int aY, int aX0, int aX1)
	{
	if (aX0 < 0)
		aX0 = 0;
	if (aX1 > aC.iW)
		aX1 = aC.iW;
	if (aX0 >= aX1)
		return;
	unsigned char *row = aC.iDst + aY * aC.iStride;
	for (int x = aX0; x < aX1; x++)
		{
		if (aC.iPixB == 4)
			*(unsigned *)(row + x * 4) = 0;
		else
			*(unsigned short *)(row + x * 2) = 0;
		}
	}

// Draws one span with the shift (aDx, aDy). The part of the run on the game picture is
// always drawn; the part on the bars only when aBarsToo.
static void dcSpan(const TVpadCanvas &aC, const TVpadSpan &aS, const unsigned char *aCov,
	int aDx, int aDy, int aWhite, int aOp, int aBarsToo)
	{
	const int y = aS.iY + aDy;
	if (y < 0 || y >= aC.iH)
		return;
	int xs = aS.iX + aDx;
	int xe = xs + aS.iLen;
	const unsigned char *cov = aCov;
	if (xs < 0)
		{
		cov += -xs;
		xs = 0;
		}
	if (xe > aC.iW)
		xe = aC.iW;
	if (xs >= xe)
		return;
	const int picA = aC.iPicX, picB = aC.iPicX + aC.iPicW;
	if (y < aC.iPicY || y >= aC.iPicY + aC.iPicH)
		{
		if (aBarsToo)
			dcRun(aC, y, xs, xe, cov, aWhite, aOp);
		return;
		}
	const int a = dcMax(xs, picA), b = dcMin(xe, picB);
	if (a >= b)
		{
		if (aBarsToo)
			dcRun(aC, y, xs, xe, cov, aWhite, aOp);
		return;
		}
	if (aBarsToo)
		{
		if (xs < a)
			dcRun(aC, y, xs, a, cov, aWhite, aOp);
		if (b < xe)
			dcRun(aC, y, b, xe, cov + (b - xs), aWhite, aOp);
		}
	dcRun(aC, y, a, b, cov + (a - xs), aWhite, aOp);
	}

static int dcPressed(const TVpadLayout &aL, const TVpadState &aS, int aI)
	{
	const TVpadControl &c = aL.iC[aI];
	if (c.iKind == EVpadKindButton && (aS.iBits & c.iBits) != 0)
		return 1;
	if (c.iKind == EVpadKindAction)
		for (int p = 0; p < KVpadMaxPointers; p++)
			if (aS.iP[p].iDown && aS.iP[p].iCtl == aI)
				return 1;
	return 0;
	}

// Whether a part is drawn in its pressed look, given whether its control is held.
static int dcActive(const TVpadState &aS, int aHeld, int aPart)
	{
	if (aPart == EVpadPartBody || aPart == EVpadPartFill)
		return aHeld;
	if (aPart >= EVpadPartUp && aPart < EVpadPartKnob)
		return (aS.iBits >> KDrawDirBit[aPart - EVpadPartUp]) & 1;
	if (aPart >= EVpadPartArm && aPart < EVpadPartArm + 4)
		return (aS.iBits >> KDrawDirBit[aPart - EVpadPartArm]) & 1;
	return 0;
	}

static void dcKnob(const TVpadControl &aC, const TVpadState &aS, int &aDx, int &aDy)
	{
	const int ax = aC.iId == 1 ? aS.iRightX : aS.iLx;
	const int ay = aC.iId == 1 ? aS.iRightY : aS.iLy;
	const int t = dcKnobTravel(aC);
	aDx = (ax - KVpadStickCentre) * t / KVpadStickCentre;
	aDy = (ay - KVpadStickCentre) * t / KVpadStickCentre;
	}

void VpadArtClearBars(const TVpadLayout &aLayout, const TVpadCanvas &aCanvas)
	{
	int n = aLayout.iCount;
	if (n > KVpadMaxControls)
		n = KVpadMaxControls;
	for (int i = 0; i < n; i++)
		{
		const TVpadControl &c = aLayout.iC[i];
		if (c.iRx <= 0 || (dcIsRect(c) && c.iRy <= 0))
			continue;
		const int mx = c.iRx + KDrawMargin;
		const int my = (dcIsRect(c) ? c.iRy : c.iRx) + KDrawMargin;
		const int bx0 = c.iCx - mx, bx1 = c.iCx + mx;
		for (int y = c.iCy - my; y <= c.iCy + my; y++)
			{
			if (y < 0 || y >= aCanvas.iH)
				continue;
			if (y < aCanvas.iPicY || y >= aCanvas.iPicY + aCanvas.iPicH)
				dcClearRun(aCanvas, y, bx0, bx1 + 1);
			else
				{
				dcClearRun(aCanvas, y, bx0, dcMin(bx1 + 1, aCanvas.iPicX));
				dcClearRun(aCanvas, y, dcMax(bx0, aCanvas.iPicX + aCanvas.iPicW), bx1 + 1);
				}
			}
		}
	}

void VpadArtPaint(const TVpadArt &aArt, const TVpadLayout &aLayout, const TVpadState &aState,
	const TVpadCanvas &aCanvas, int aOpacityPct, int aBarsToo)
	{
	int pct = aOpacityPct;
	if (pct < 10)
		pct = 10;
	if (pct > 100)
		pct = 100;
	const int op = (255 * pct + 50) / 100;
	for (int i = 0; i < aArt.iCount; i++)
		{
		const TVpadControl &c = aLayout.iC[i];
		if (c.iRx <= 0)
			continue;
		const int held = dcPressed(aLayout, aState, i);
		int kx = 0, ky = 0;
		if (c.iKind == EVpadKindStick)
			dcKnob(c, aState, kx, ky);
		const int last = aArt.iFirst[i + 1];
		for (int s = aArt.iFirst[i]; s < last; s++)
			{
			const TVpadSpan &sp = aArt.iSpan[s];
			const int act = dcActive(aState, held, sp.iPart);
			int white, o;
			if (s < aArt.iDark[i])
				{
				if (!act)
					continue;
				white = 1;
				o = 255;
				}
			else if (s < aArt.iLight[i])
				{
				white = 0;
				o = act ? 255 : op;
				}
			else
				{
				white = act ? 0 : 1;
				o = act ? 255 : op;
				}
			const int moved = sp.iPart == EVpadPartKnob;
			dcSpan(aCanvas, sp, aArt.iAlpha + sp.iOff, moved ? kx : 0, moved ? ky : 0,
				white, o, aBarsToo);
			}
		}
	}
