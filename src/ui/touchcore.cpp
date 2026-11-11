#include "touchcore.h"

// Milliseconds from aThen to aNow. A wrapping clock is harmless; a time earlier than one
// already seen counts as no time passed, not as 49 days.
static unsigned tcElapsed(unsigned aNow, unsigned aThen)
	{
	const unsigned d = aNow - aThen;
	return d >= 0x80000000u ? 0 : d;
	}

static int vpadAbs(int aV)
	{
	return aV < 0 ? -aV : aV;
	}

static int vpadMin(int aA, int aB)
	{
	return aA < aB ? aA : aB;
	}

static int vpadMax(int aA, int aB)
	{
	return aA > aB ? aA : aB;
	}

static int vpadClamp(int aV, int aLo, int aHi)
	{
	if (aV < aLo)
		return aLo;
	if (aV > aHi)
		return aHi;
	return aV;
	}

// Pointer coordinates are clamped on entry, which keeps every product in the rules
// within 32 bits.
static int vpadClampCoord(int aV)
	{
	return vpadClamp(aV, -8192, 8191);
	}

// Tenths of a millimetre to pixels at aScale percent, rounded to nearest.
static int vpadPx(int aTenths, int aScale, int aUmPerPx)
	{
	return (aTenths * aScale + aUmPerPx / 2) / aUmPerPx;
	}

// dist(a, b) <= r and dist(a, b) >= r, both on squares. The radius is clamped so that
// r*r cannot overflow; no legal layout comes near the clamp.
static int vpadNear(int aDx, int aDy, int aR)
	{
	const int r = vpadMin(aR, 32767);
	return aDx * aDx + aDy * aDy <= r * r;
	}

static int vpadFar(int aDx, int aDy, int aR)
	{
	const int r = vpadMin(aR, 32767);
	return aDx * aDx + aDy * aDy >= r * r;
	}

static int vpadCount(const TVpadLayout &aL)
	{
	return vpadClamp(aL.iCount, 0, KVpadMaxControls);
	}

// The d-pad and the stick are circles whatever iShape says.
static int vpadIsRect(const TVpadControl &aC)
	{
	return aC.iShape == EVpadShapeRect && aC.iKind != EVpadKindDpad && aC.iKind != EVpadKindStick;
	}

// A control with iRx <= 0, or a rect with iRy <= 0, is ignored by every function.
static int vpadValid(const TVpadControl &aC)
	{
	return aC.iRx > 0 && (!vpadIsRect(aC) || aC.iRy > 0);
	}

static int vpadCtlOk(const TVpadLayout &aL, int aIdx)
	{
	return aIdx >= 0 && aIdx < vpadCount(aL) && vpadValid(aL.iC[aIdx]);
	}

// Zone and threshold percentages are taken within 0..400: that keeps iRx * pct and the
// square of a zone radius within 32 bits whatever the caller put in the layout.
static int vpadPct(int aPct)
	{
	return vpadClamp(aPct, 0, 400);
	}

static int vpadZone(int aSize, int aPct)
	{
	return aSize * vpadPct(aPct) / 100;
	}

// Dead-centre radius of a d-pad or stick.
static int vpadDead(const TVpadLayout &aL, const TVpadControl &aC)
	{
	return aC.iRx * vpadClamp(aL.iDeadPct, 0, 60) / 100;
	}

// Press zone (aPct = iHitPct) or leash zone (aPct = iLeashPct) of a control.
static int vpadInside(const TVpadControl &aC, int aPct, int aX, int aY)
	{
	const int dx = aX - aC.iCx;
	const int dy = aY - aC.iCy;
	if (vpadIsRect(aC))
		{
		const int hw = vpadZone(aC.iRx, aPct);
		const int hh = vpadZone(aC.iRy, aPct);
		return vpadAbs(dx) <= hw && vpadAbs(dy) <= hh;
		}
	const int r = vpadZone(aC.iRx, aPct);
	return dx * dx + dy * dy <= r * r;
	}

// Normalised distance inside a zone of aPct; the caller only asks for points inside.
// A zone of zero size can contain only its centre, which is distance 0.
static int vpadNorm(const TVpadControl &aC, int aPct, int aX, int aY)
	{
	const int dx = aX - aC.iCx;
	const int dy = aY - aC.iCy;
	if (vpadIsRect(aC))
		{
		const int hw = vpadZone(aC.iRx, aPct);
		const int hh = vpadZone(aC.iRy, aPct);
		const int a = hw > 0 ? vpadAbs(dx) * 1000 / hw : 0;
		const int b = hh > 0 ? vpadAbs(dy) * 1000 / hh : 0;
		return vpadMax(a, b);
		}
	const int r = vpadZone(aC.iRx, aPct);
	if (r <= 0)
		return 0;
	return VpadIsqrt(dx * dx + dy * dy) * 1000 / r;
	}

// Another pointer holds this control (only d-pads and sticks are exclusive).
static int vpadTakenByOther(const TVpadState &aS, int aId, int aCtl)
	{
	for (int p = 0; p < KVpadMaxPointers; p++)
		if (p != aId && aS.iP[p].iDown && aS.iP[p].iCtl == aCtl)
			return 1;
	return 0;
	}

// Index is iDiagPct / 10 (touchcore.h, d-pad diagonal table).
static const int KVpadDiagT[11] = { 1000, 924, 854, 788, 727, 668, 613, 560, 510, 461, 414 };

// Direction bits of the offset from (aOx, aOy) to the pointer. aFourWayPct is the d-pad
// four-way limit, or 0 for the stick, which has none. Sets aP.iBits and the classification
// in aP.iDiag: 1 one bit, 2 two bits. The hysteresis reads the previous classification
// before it is overwritten.
static void vpadDirBits(const TVpadLayout &aL, const TVpadControl &aC, int aOx, int aOy, int aFourWayPct, TVpadPointer &aP)
	{
	const int dx = aP.iX - aOx;
	const int dy = aP.iY - aOy;
	const int ax = vpadAbs(dx);
	const int ay = vpadAbs(dy);
	const int hi = vpadMax(ax, ay);
	const int lo = vpadMin(ax, ay);
	if (hi == 0)
		{
		aP.iBits = 0;
		aP.iDiag = 0;
		return;
		}
	const int dist = VpadIsqrt(dx * dx + dy * dy);
	const int diagPct = vpadClamp(aL.iDiagPct, 0, 100);
	int diagonal = 0;
	if (diagPct >= 10 && dist * 100 >= aC.iRx * aFourWayPct)
		{
		int t = KVpadDiagT[diagPct / 10];
		if (aP.iDiag == 2)
			t -= 35;
		else if (aP.iDiag == 1)
			t += 35;
		diagonal = (lo * 1000 / hi >= t);
		}
	int bits = 0;
	if (diagonal)
		bits = (dx > 0 ? (1 << EVpadRight) : (1 << EVpadLeft)) | (dy > 0 ? (1 << EVpadDown) : (1 << EVpadUp));
	else if (ax >= ay)
		bits = dx > 0 ? (1 << EVpadRight) : (1 << EVpadLeft);
	else
		bits = dy > 0 ? (1 << EVpadDown) : (1 << EVpadUp);
	aP.iBits = (unsigned short)bits;
	aP.iDiag = (unsigned char)(diagonal ? 2 : 1);
	}

// Button rule. The pointer presses the Button controls of the captured group whose press
// zone holds the point; with none, it keeps what it pressed while the point stays inside
// the leash zone of a remembered control.
static void vpadEvalButton(const TVpadLayout &aL, int aCtl, TVpadPointer &aP)
	{
	const int n = vpadCount(aL);
	const unsigned char g = aL.iC[aCtl].iGroup;
	unsigned short sBits = 0;
	unsigned short sHeld = 0;
	for (int j = 0; j < n; j++)
		{
		const TVpadControl &o = aL.iC[j];
		const int same = g == 0 ? j == aCtl : o.iGroup == g;
		if (!vpadValid(o) || o.iKind != EVpadKindButton || !same)
			continue;
		if (!vpadInside(o, aL.iHitPct, aP.iX, aP.iY))
			continue;
		sBits = (unsigned short)(sBits | o.iBits);
		sHeld = (unsigned short)(sHeld | (1 << j));
		}
	if (sHeld != 0)
		{
		aP.iBits = sBits;
		aP.iHeld = sHeld;
		return;
		}
	for (int j = 0; j < n; j++)
		{
		if (((aP.iHeld >> j) & 1) && vpadValid(aL.iC[j]) && vpadInside(aL.iC[j], aL.iLeashPct, aP.iX, aP.iY))
			return;
		}
	aP.iBits = 0;
	aP.iHeld = 0;
	}

// D-pad rule: nothing at the centre or inside the dead zone, which is 3/4 of its size while
// the pointer was pressing something; otherwise the direction bits.
static void vpadEvalDpad(const TVpadLayout &aL, const TVpadControl &aC, TVpadPointer &aP)
	{
	const int dx = aP.iX - aC.iCx;
	const int dy = aP.iY - aC.iCy;
	const int dist = VpadIsqrt(dx * dx + dy * dy);
	int dead = vpadDead(aL, aC);
	if (aP.iBits != 0)
		dead = dead * 3 / 4;
	if (dist == 0 || dist < dead)
		{
		aP.iBits = 0;
		aP.iDiag = 0;
		return;
		}
	vpadDirBits(aL, aC, aC.iCx, aC.iCy, vpadPct(aL.iFourWayPct), aP);
	}

// Stick digital fallback: the d-pad rule without its dead zone and four-way test, and only
// from iStickDigitalPct of the radius. Below that, and while the stick itself reads as
// centred, it presses nothing.
static void vpadEvalStick(const TVpadLayout &aL, const TVpadControl &aC, TVpadPointer &aP)
	{
	const int dx = aP.iX - aP.iOx;
	const int dy = aP.iY - aP.iOy;
	const int dist = VpadIsqrt(dx * dx + dy * dy);
	const int dead = vpadDead(aL, aC);
	const int digital = vpadPct(aL.iStickDigitalPct);
	if (digital > 0 && dist >= dead && dist * 100 >= aC.iRx * digital)
		{
		vpadDirBits(aL, aC, aP.iOx, aP.iOy, 0, aP);
		return;
		}
	aP.iBits = 0;
	aP.iDiag = 0;
	}

// Re-evaluates what a captured pointer presses at its current position.
static void vpadEvaluate(const TVpadLayout &aL, TVpadPointer &aP)
	{
	if (!vpadCtlOk(aL, aP.iCtl))
		{
		aP.iBits = 0;
		aP.iHeld = 0;
		aP.iDiag = 0;
		return;
		}
	const TVpadControl &c = aL.iC[aP.iCtl];
	if (c.iKind == EVpadKindButton)
		{
		vpadEvalButton(aL, aP.iCtl, aP);
		return;
		}
	aP.iHeld = 0;
	if (c.iKind == EVpadKindDpad)
		vpadEvalDpad(aL, c, aP);
	else if (c.iKind == EVpadKindStick)
		vpadEvalStick(aL, c, aP);
	else
		{
		aP.iBits = 0;
		aP.iDiag = 0;
		}
	}

// Stick output from the pointer's current position. Centred inside the dead zone and
// exactly at the origin; the deflection grows to 127 at the drawn rim and stays there.
static void vpadStickOut(const TVpadLayout &aL, const TVpadControl &aC, const TVpadPointer &aP, int &aLx, int &aLy)
	{
	const int dx = aP.iX - aP.iOx;
	const int dy = aP.iY - aP.iOy;
	const int dist = VpadIsqrt(dx * dx + dy * dy);
	const int dead = vpadDead(aL, aC);
	aLx = KVpadStickCentre;
	aLy = KVpadStickCentre;
	if (dist == 0 || dist < dead)
		return;
	const int mag = vpadMin(dist, aC.iRx) - dead;
	const int span = aC.iRx - dead;
	const int m = mag * 127 / span;
	aLx = KVpadStickCentre + dx * m / dist;
	aLy = KVpadStickCentre + dy * m / dist;
	}

// Recomputes the state-wide fields after every call. Edges are the bits that went from 0
// to 1 in the overall word, not per pointer.
static void vpadAggregate(TVpadState &aS, const TVpadLayout &aL)
	{
	unsigned short bits = 0;
	int lx = KVpadStickCentre;
	int ly = KVpadStickCentre;
	for (int p = 0; p < KVpadMaxPointers; p++)
		{
		const TVpadPointer &ptr = aS.iP[p];
		if (!ptr.iDown)
			continue;
		bits = (unsigned short)(bits | ptr.iBits);
		if (vpadCtlOk(aL, ptr.iCtl) && aL.iC[ptr.iCtl].iKind == EVpadKindStick)
			vpadStickOut(aL, aL.iC[ptr.iCtl], ptr, lx, ly);
		}
	aS.iEdges = (unsigned short)(aS.iEdges | (bits & ~aS.iBits));
	aS.iBits = bits;
	aS.iLx = (unsigned char)lx;
	aS.iLy = (unsigned char)ly;
	}

// Ordinary move of a pointer that is down: take the position and re-evaluate.
static void vpadMove(TVpadPointer &aP, const TVpadLayout &aL, int aX, int aY, unsigned aMs)
	{
	aP.iX = (short)aX;
	aP.iY = (short)aY;
	aP.iMs = aMs;
	vpadEvaluate(aL, aP);
	}

// Releases a pointer. An Action control counts when the last position is inside its leash.
static void vpadRelease(TVpadState &aS, const TVpadLayout &aL, int aId, unsigned aMs)
	{
	TVpadPointer &p = aS.iP[aId];
	if (vpadCtlOk(aL, p.iCtl))
		{
		const TVpadControl &c = aL.iC[p.iCtl];
		if (c.iKind == EVpadKindAction && c.iId < 32 && vpadInside(c, aL.iLeashPct, p.iX, p.iY))
			aS.iActions |= 1u << c.iId;
		}
	p.iDown = 0;
	p.iCtl = -1;
	p.iDiag = 0;
	p.iBits = 0;
	p.iHeld = 0;
	p.iMs = aMs;
	}

// Captures the control under a pointer that went down. Smallest normalised press distance
// wins; a d-pad or stick held by another pointer is skipped.
static void vpadCapture(TVpadState &aS, const TVpadLayout &aL, int aId, int aX, int aY, unsigned aMs)
	{
	const int n = vpadCount(aL);
	int best = -1;
	int bestD = 0;
	for (int i = 0; i < n; i++)
		{
		const TVpadControl &c = aL.iC[i];
		if (!vpadValid(c) || !vpadInside(c, aL.iHitPct, aX, aY))
			continue;
		if ((c.iKind == EVpadKindDpad || c.iKind == EVpadKindStick) && vpadTakenByOther(aS, aId, i))
			continue;
		const int d = vpadNorm(c, aL.iHitPct, aX, aY);
		if (best < 0 || d < bestD)
			{
			best = i;
			bestD = d;
			}
		}
	TVpadPointer &p = aS.iP[aId];
	p.iDown = 1;
	p.iCtl = (signed char)best;
	p.iDiag = 0;
	p.iBits = 0;
	p.iHeld = 0;
	p.iX = (short)aX;
	p.iY = (short)aY;
	p.iMs = aMs;
	if (best >= 0)
		{
		p.iOx = (short)(aL.iStickFloat != 0 ? aX : aL.iC[best].iCx);
		p.iOy = (short)(aL.iStickFloat != 0 ? aY : aL.iC[best].iCy);
		}
	vpadEvaluate(aL, p);
	}

// Second-finger inference, not yet active. Returns 1 when it became active and slot 1
// holds the second finger.
static int vpadDualStart(TVpadState &aS, const TVpadLayout &aL, int aX, int aY, unsigned aMs)
	{
	TVpadPointer &p0 = aS.iP[0];
	if (!vpadCtlOk(aL, p0.iCtl))
		return 0;
	const int dx = aX - p0.iX;
	const int dy = aY - p0.iY;
	if (!vpadFar(dx, dy, aL.iDualJumpPx))
		return 0;
	if (tcElapsed(aMs, p0.iMs) > (unsigned)KVpadDualMaxMs)
		return 0;
	const int px2 = vpadClampCoord(2 * aX - p0.iX);
	const int py2 = vpadClampCoord(2 * aY - p0.iY);
	const TVpadControl &first = aL.iC[p0.iCtl];
	const int n = vpadCount(aL);
	int best = -1;
	int bestD = 0;
	for (int i = 0; i < n; i++)
		{
		const TVpadControl &c = aL.iC[i];
		if (i == p0.iCtl || !vpadValid(c))
			continue;
		if (c.iKind != EVpadKindDpad && c.iKind != EVpadKindStick && c.iKind != EVpadKindButton)
			continue;
		if (first.iGroup != 0 && c.iGroup == first.iGroup)
			continue;
		if (!vpadInside(c, aL.iLeashPct, px2, py2))
			continue;
		const int d = vpadNorm(c, aL.iLeashPct, px2, py2);
		if (best < 0 || d < bestD)
			{
			best = i;
			bestD = d;
			}
		}
	if (best < 0)
		return 0;
	aS.iDual = 1;
	aS.iDualX = p0.iX;
	aS.iDualY = p0.iY;
	TVpadPointer &p1 = aS.iP[1];
	const TVpadControl &c = aL.iC[best];
	p1.iDown = 1;
	p1.iCtl = (signed char)best;
	p1.iDiag = 0;
	p1.iBits = 0;
	p1.iHeld = 0;
	p1.iX = (short)px2;
	p1.iY = (short)py2;
	p1.iMs = aMs;
	p1.iOx = (short)(aL.iStickFloat != 0 ? px2 : c.iCx);
	p1.iOy = (short)(aL.iStickFloat != 0 ? py2 : c.iCy);
	if (c.iKind == EVpadKindButton)
		{
		p1.iBits = c.iBits;
		p1.iHeld = (unsigned short)(1 << best);
		}
	else
		vpadEvaluate(aL, p1);
	return 1;
	}

// Second-finger inference, active. The reported point is taken as the midpoint of the two
// fingers; the first finger rests at (iDualX, iDualY) and slot 1 is the mirror image.
static void vpadDualMove(TVpadState &aS, const TVpadLayout &aL, int aX, int aY, unsigned aMs)
	{
	// At least 1: with a radius of 0 neither finger could ever be seen to leave.
	const int r = vpadClamp(aL.iDualJumpPx / 3, 1, 32767);
	TVpadPointer &p0 = aS.iP[0];
	TVpadPointer &p1 = aS.iP[1];
	if (vpadNear(aX - aS.iDualX, aY - aS.iDualY, r))
		{
		vpadRelease(aS, aL, 1, aMs);
		aS.iDual = 0;
		vpadMove(p0, aL, aX, aY, aMs);
		return;
		}
	if (vpadNear(aX - p1.iX, aY - p1.iY, r))
		{
		p0.iCtl = p1.iCtl;
		p0.iDiag = p1.iDiag;
		p0.iBits = p1.iBits;
		p0.iHeld = p1.iHeld;
		p0.iOx = p1.iOx;
		p0.iOy = p1.iOy;
		p1.iDown = 0;
		p1.iCtl = -1;
		p1.iDiag = 0;
		p1.iBits = 0;
		p1.iHeld = 0;
		aS.iDual = 0;
		vpadMove(p0, aL, aX, aY, aMs);
		return;
		}
	vpadMove(p1, aL, vpadClampCoord(2 * aX - aS.iDualX), vpadClampCoord(2 * aY - aS.iDualY), aMs);
	}

// Drawn box of a control: the stick and d-pad are circles; a rect uses iRx and iRy as is.
static void vpadBox(const TVpadControl &aC, int &aX0, int &aX1, int &aY0, int &aY1)
	{
	const int hh = vpadIsRect(aC) ? aC.iRy : aC.iRx;
	aX0 = aC.iCx - aC.iRx;
	aX1 = aC.iCx + aC.iRx;
	aY0 = aC.iCy - hh;
	aY1 = aC.iCy + hh;
	}

static void vpadPut(TVpadControl &aC, int aKind, int aShape, int aGroup, int aBits, int aCx, int aCy, int aRx, int aRy)
	{
	aC.iKind = (unsigned char)aKind;
	aC.iShape = (unsigned char)aShape;
	aC.iGroup = (unsigned char)aGroup;
	aC.iId = 0;
	aC.iBits = (unsigned short)aBits;
	aC.iCx = (short)aCx;
	aC.iCy = (short)aCy;
	aC.iRx = (short)aRx;
	aC.iRy = (short)aRy;
	}

// Places the twelve default controls for one scale, following the layout rules.
static void vpadBuild(TVpadLayout &aOut, int aW, int aH, int aUm, int aScale, unsigned aOptions)
	{
	const int land = aW >= aH;
	const int m = vpadPx(20, aScale, aUm);
	const int g = vpadPx(10, aScale, aUm);
	const int rd = vpadPx(110, aScale, aUm);
	const int rb = vpadPx(45, aScale, aUm);
	const int rr = vpadPx(75, aScale, aUm);
	const int tw = vpadPx(50, aScale, aUm);
	const int th = vpadPx(35, aScale, aUm);
	const int rm = vpadPx(35, aScale, aUm);
	const int sw = vpadPx(land ? 70 : 45, aScale, aUm);
	const int sh = vpadPx(40, aScale, aUm);
	const int half = land ? (g + 1) / 2 : rm + g;
	const int cx = aW / 2;
	const int faceX = aW - m - rr - rb;
	const int dirKind = (aOptions & EVpadOptStick) ? EVpadKindStick : EVpadKindDpad;
	int dirY = aH - m - rd;
	int faceY = aH - m - rr - rb;
	int shY = m + sh;
	int selY = aH - m - th;
	int menuY = m + rm;
	if (!land)
		{
		const int yP = aW * 3 / 4;
		const int y1 = yP + m + sh;
		const int yMain = ((y1 + sh + g) + (aH - m - 2 * rm - g)) / 2;
		shY = y1;
		menuY = aH - m - rm;
		dirY = yMain;
		faceY = yMain;
		}
	aOut.iCount = EVpadDefaultCount;
	aOut.iHitPct = 125;
	aOut.iLeashPct = 175;
	aOut.iDeadPct = 18;
	aOut.iDiagPct = 100;
	aOut.iFourWayPct = 40;
	aOut.iStickDigitalPct = 80;
	aOut.iStickFloat = 0;
	aOut.iDualJumpPx = (aOptions & EVpadOptResistiveDual) ? vpadPx(120, aScale, aUm) : 0;

	vpadPut(aOut.iC[EVpadIxDir], dirKind, EVpadShapeCircle, 1, 0, m + rd, dirY, rd, rd);
	vpadPut(aOut.iC[EVpadIxTriangle], EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadTriangle, faceX, faceY - rr, rb, rb);
	vpadPut(aOut.iC[EVpadIxCircle], EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCircle, faceX + rr, faceY, rb, rb);
	vpadPut(aOut.iC[EVpadIxCross], EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCross, faceX, faceY + rr, rb, rb);
	vpadPut(aOut.iC[EVpadIxSquare], EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadSquare, faceX - rr, faceY, rb, rb);
	vpadPut(aOut.iC[EVpadIxL1], EVpadKindButton, EVpadShapeRect, 3, 1 << EVpadL1, m + sw, shY, sw, sh);
	vpadPut(aOut.iC[EVpadIxL2], EVpadKindButton, EVpadShapeRect, 3, 1 << EVpadL2, m + 3 * sw + g, shY, sw, sh);
	vpadPut(aOut.iC[EVpadIxR1], EVpadKindButton, EVpadShapeRect, 4, 1 << EVpadR1, aW - m - sw, shY, sw, sh);
	vpadPut(aOut.iC[EVpadIxR2], EVpadKindButton, EVpadShapeRect, 4, 1 << EVpadR2, aW - m - 3 * sw - g, shY, sw, sh);
	vpadPut(aOut.iC[EVpadIxSelect], EVpadKindButton, EVpadShapeRect, 0, 1 << EVpadSelect, cx - half - tw, selY, tw, th);
	vpadPut(aOut.iC[EVpadIxStart], EVpadKindButton, EVpadShapeRect, 0, 1 << EVpadStart, cx + half + tw, selY, tw, th);
	vpadPut(aOut.iC[EVpadIxMenu], EVpadKindAction, EVpadShapeCircle, 0, 0, cx, menuY, rm, rm);
	}

// VpadLayoutFits ignores a control without a size, so a scale that rounds the whole pad
// away would pass it.
static int vpadAllSized(const TVpadLayout &aL)
	{
	for (int i = 0; i < EVpadDefaultCount; i++)
		if (!vpadValid(aL.iC[i]))
			return 0;
	return 1;
	}

int VpadDefaultLayout(TVpadLayout &aOut, int aScreenW, int aScreenH, int aUmPerPx, int aScalePct, unsigned aOptions)
	{
	const int um = aUmPerPx < 20 ? 20 : aUmPerPx;
	const int pct = vpadClamp(aScalePct, 25, 400);
	for (int i = 0; i < 20; i++)
		{
		const int s = pct * (32 - i) / 32;
		vpadBuild(aOut, aScreenW, aScreenH, um, s, aOptions);
		if (vpadAllSized(aOut) && VpadLayoutFits(aOut, aScreenW, aScreenH))
			return s;
		}
	return 0;
	}

int VpadLayoutFits(const TVpadLayout &aLayout, int aScreenW, int aScreenH)
	{
	const int n = vpadCount(aLayout);
	const int portrait = aScreenH > aScreenW;
	const int yP = aScreenW * 3 / 4;
	for (int i = 0; i < n; i++)
		{
		const TVpadControl &c = aLayout.iC[i];
		if (!vpadValid(c))
			continue;
		int x0 = 0;
		int x1 = 0;
		int y0 = 0;
		int y1 = 0;
		vpadBox(c, x0, x1, y0, y1);
		if (x0 < 0 || y0 < 0 || x1 >= aScreenW || y1 >= aScreenH)
			return 0;
		if (portrait && y0 < yP)
			return 0;
		}
	for (int i = 0; i < n; i++)
		{
		const TVpadControl &a = aLayout.iC[i];
		if (!vpadValid(a))
			continue;
		for (int j = i + 1; j < n; j++)
			{
			const TVpadControl &b = aLayout.iC[j];
			if (!vpadValid(b))
				continue;
			if (a.iGroup != 0 && a.iGroup == b.iGroup)
				continue;
			int ax0 = 0;
			int ax1 = 0;
			int ay0 = 0;
			int ay1 = 0;
			int bx0 = 0;
			int bx1 = 0;
			int by0 = 0;
			int by1 = 0;
			vpadBox(a, ax0, ax1, ay0, ay1);
			vpadBox(b, bx0, bx1, by0, by1);
			if (ax0 <= bx1 && bx0 <= ax1 && ay0 <= by1 && by0 <= ay1)
				return 0;
			}
		}
	return 1;
	}

void VpadReset(TVpadState &aState)
	{
	for (int p = 0; p < KVpadMaxPointers; p++)
		{
		TVpadPointer &q = aState.iP[p];
		q.iDown = 0;
		q.iCtl = -1;
		q.iDiag = 0;
		q.iPad = 0;
		q.iOx = 0;
		q.iOy = 0;
		q.iX = 0;
		q.iY = 0;
		q.iMs = 0;
		q.iBits = 0;
		q.iHeld = 0;
		}
	aState.iBits = 0;
	aState.iEdges = 0;
	aState.iActions = 0;
	aState.iLx = KVpadStickCentre;
	aState.iLy = KVpadStickCentre;
	aState.iDual = 0;
	aState.iPad = 0;
	aState.iDualX = 0;
	aState.iDualY = 0;
	}

void VpadPointerDown(TVpadState &aState, const TVpadLayout &aLayout, int aId, int aX, int aY, unsigned aMs)
	{
	if (aId < 0 || aId >= KVpadMaxPointers)
		return;
	if (aLayout.iDualJumpPx > 0 && aId != 0)
		return;
	if (aState.iP[aId].iDown)
		{
		VpadPointerMove(aState, aLayout, aId, aX, aY, aMs);
		return;
		}
	vpadCapture(aState, aLayout, aId, vpadClampCoord(aX), vpadClampCoord(aY), aMs);
	vpadAggregate(aState, aLayout);
	}

void VpadPointerMove(TVpadState &aState, const TVpadLayout &aLayout, int aId, int aX, int aY, unsigned aMs)
	{
	if (aId < 0 || aId >= KVpadMaxPointers)
		return;
	const int dual = aLayout.iDualJumpPx > 0;
	if (dual && aId != 0)
		return;
	if (!aState.iP[aId].iDown)
		return;
	const int x = vpadClampCoord(aX);
	const int y = vpadClampCoord(aY);
	if (dual)
		{
		if (aState.iDual)
			vpadDualMove(aState, aLayout, x, y, aMs);
		else if (!vpadDualStart(aState, aLayout, x, y, aMs))
			vpadMove(aState.iP[0], aLayout, x, y, aMs);
		}
	else
		vpadMove(aState.iP[aId], aLayout, x, y, aMs);
	vpadAggregate(aState, aLayout);
	}

void VpadPointerUp(TVpadState &aState, const TVpadLayout &aLayout, int aId, unsigned aMs)
	{
	if (aId < 0 || aId >= KVpadMaxPointers)
		return;
	if (aLayout.iDualJumpPx > 0 && aId != 0)
		return;
	if (!aState.iP[aId].iDown)
		return;
	if (aId == 0 && aState.iDual)
		{
		vpadRelease(aState, aLayout, 1, aMs);
		aState.iDual = 0;
		}
	vpadRelease(aState, aLayout, aId, aMs);
	vpadAggregate(aState, aLayout);
	}

unsigned VpadTakeEdges(TVpadState &aState)
	{
	const unsigned e = aState.iEdges;
	aState.iEdges = 0;
	return e;
	}

unsigned VpadTakeActions(TVpadState &aState)
	{
	const unsigned a = aState.iActions;
	aState.iActions = 0;
	return a;
	}

int VpadIsqrt(int aV)
	{
	if (aV <= 0)
		return 0;
	const unsigned v = (unsigned)aV;
	unsigned r = 0;
	for (unsigned bit = 1u << 15; bit != 0; bit >>= 1)
		{
		const unsigned c = r | bit;
		if (c * c <= v)
			r = c;
		}
	return (int)r;
	}

// ============================ Kinetic scroller ===============================

static int ksClampSize(int aV)
	{
	return vpadClamp(aV, 0, 4000000);
	}

// Millimetres converted to pixels, given in micrometres: (um + um/2) / aUmPerPx, at least 1.
static int ksPx(int aMicro, int aUmPerPx)
	{
	const int v = (aMicro + aUmPerPx / 2) / aUmPerPx;
	return v < 1 ? 1 : v;
	}

static int ksMax8(const TKScroll &aS)
	{
	const int d = aS.iContent - aS.iView;
	return d > 0 ? d * 256 : 0;
	}

// Raw value of an offset: the drag runs on this scale so that the list is continuous when
// a drag starts while over-scrolled.
static int ksRaw(int aP8, int aMax8)
	{
	if (aP8 < 0)
		return 2 * aP8;
	if (aP8 > aMax8)
		return aMax8 + 2 * (aP8 - aMax8);
	return aP8;
	}

static int ksOffset(int aRaw8, int aMax8)
	{
	if (aRaw8 < 0)
		return aRaw8 / 2;
	if (aRaw8 > aMax8)
		return aMax8 + (aRaw8 - aMax8) / 2;
	return aRaw8;
	}

// The offset is kept within KKsMaxOverPx of the range. Without the limit, drags piled up
// with no step between them grow the offset until doubling it in ksRaw overflows.
static int ksClampOver(int aP8, int aMax8)
	{
	return vpadClamp(aP8, -KKsMaxOverPx * 256, aMax8 + KKsMaxOverPx * 256);
	}

// At least 1, so that a coasting list always reaches the stopping threshold.
static int ksMinFling(const TKScroll &aS)
	{
	return vpadClamp(aS.iMinFling, 1, 4000000);
	}

// One millisecond of coasting.
static void ksFlingMs(TKScroll &aS)
	{
	aS.iPos8 += aS.iVel8 / 1000;
	const int max8 = ksMax8(aS);
	const int av = vpadAbs(aS.iVel8);
	// 32768 is the largest factor for which the low-half product below stays within int.
	const int lossQ16 = vpadClamp(aS.iFrictionLossQ16, 1, 32768);
	int loss = (av >> 16) * lossQ16 + (((av & 0xFFFF) * lossQ16) >> 16);
	if (loss < 1)
		loss = 1;
	int nav = av - loss;
	if (nav < 0)
		nav = 0;
	aS.iVel8 = aS.iVel8 < 0 ? -nav : nav;
	if (aS.iPos8 < 0 || aS.iPos8 > max8)
		{
		aS.iPos8 = aS.iPos8 < 0 ? 0 : max8;
		aS.iVel8 = 0;
		aS.iState = EKsIdle;
		return;
		}
	if (vpadAbs(aS.iVel8) < ksMinFling(aS) * 64)
		{
		aS.iPos8 = (aS.iPos8 + 128) / 256 * 256;
		aS.iVel8 = 0;
		aS.iState = EKsIdle;
		}
	}

// One millisecond of gliding to the target.
static void ksSettleMs(TKScroll &aS)
	{
	const int d = aS.iTarget8 - aS.iPos8;
	const int ad = vpadAbs(d);
	if (ad < 128)
		{
		aS.iPos8 = aS.iTarget8;
		aS.iVel8 = 0;
		aS.iState = EKsIdle;
		return;
		}
	const int step = ad >> KKsSettleShift;
	aS.iPos8 += d < 0 ? -step : step;
	}

void KScrollInit(TKScroll &aS, int aViewPx, int aContentPx, int aUmPerPx)
	{
	const int um = aUmPerPx < 20 ? 20 : aUmPerPx;
	aS.iView = ksClampSize(aViewPx);
	aS.iContent = ksClampSize(aContentPx);
	aS.iPos8 = 0;
	aS.iVel8 = 0;
	aS.iTarget8 = 0;
	aS.iSlop = ksPx(2000, um);
	aS.iMinFling = ksPx(30000, um);
	aS.iMaxFling = ksPx(800000, um);
	aS.iFrictionLossQ16 = KKsFrictionLossQ16;
	aS.iState = EKsIdle;
	aS.iCaught = 0;
	aS.iLongSent = 0;
	aS.iCount = 0;
	aS.iHead = 0;
	aS.iPad[0] = 0;
	aS.iPad[1] = 0;
	aS.iPad[2] = 0;
	aS.iDownCoord = 0;
	aS.iAnchorCoord = 0;
	aS.iAnchorPos8 = 0;
	aS.iDownMs = 0;
	aS.iStepMs = 0;
	for (int i = 0; i < KKsSamples; i++)
		{
		aS.iSampleCoord[i] = 0;
		aS.iSampleMs[i] = 0;
		}
	}

void KScrollSetExtent(TKScroll &aS, int aViewPx, int aContentPx)
	{
	aS.iView = ksClampSize(aViewPx);
	aS.iContent = ksClampSize(aContentPx);
	const int max8 = ksMax8(aS);
	if (aS.iState == EKsIdle)
		aS.iPos8 = vpadClamp(aS.iPos8, 0, max8);
	else
		aS.iPos8 = ksClampOver(aS.iPos8, max8);
	if (aS.iState == EKsSettling)
		aS.iTarget8 = vpadClamp(aS.iTarget8, 0, max8);
	}

void KScrollDown(TKScroll &aS, int aCoord, unsigned aMs)
	{
	const int moving = aS.iState == EKsFlinging || aS.iState == EKsSettling;
	aS.iCaught = (unsigned char)(moving ? 1 : 0);
	aS.iVel8 = 0;
	aS.iState = EKsPressed;
	aS.iLongSent = 0;
	aS.iDownCoord = vpadClampCoord(aCoord);
	aS.iDownMs = aMs;
	aS.iStepMs = aMs;
	aS.iCount = 1;
	aS.iHead = 0;
	aS.iSampleCoord[0] = aS.iDownCoord;
	aS.iSampleMs[0] = aMs;
	}

void KScrollMove(TKScroll &aS, int aCoord, unsigned aMs)
	{
	if (aS.iState != EKsPressed && aS.iState != EKsDragging)
		return;
	const int c = vpadClampCoord(aCoord);
	aS.iHead = (unsigned char)((aS.iHead + 1) % KKsSamples);
	aS.iSampleCoord[aS.iHead] = c;
	aS.iSampleMs[aS.iHead] = aMs;
	if (aS.iCount < KKsSamples)
		aS.iCount++;
	const int max8 = ksMax8(aS);
	if (aS.iState == EKsPressed)
		{
		if (vpadAbs(c - aS.iDownCoord) <= aS.iSlop)
			return;
		aS.iState = EKsDragging;
		aS.iAnchorCoord = c;
		aS.iAnchorPos8 = ksRaw(aS.iPos8, max8);
		}
	const int raw = aS.iAnchorPos8 + (aS.iAnchorCoord - c) * 256;
	aS.iPos8 = ksClampOver(ksOffset(raw, max8), max8);
	}

int KScrollUp(TKScroll &aS, unsigned aMs)
	{
	if (aS.iState != EKsPressed && aS.iState != EKsDragging)
		return EKsNone;
	const int max8 = ksMax8(aS);
	const int tap = aS.iState == EKsPressed && !aS.iCaught && !aS.iLongSent && tcElapsed(aMs, aS.iDownMs) < (unsigned)KKsLongPressMs;
	aS.iStepMs = aMs;
	aS.iVel8 = 0;
	if (aS.iPos8 < 0 || aS.iPos8 > max8)
		{
		aS.iTarget8 = aS.iPos8 < 0 ? 0 : max8;
		aS.iState = EKsSettling;
		return tap ? EKsTap : EKsNone;
		}
	if (aS.iState == EKsPressed)
		{
		aS.iState = EKsIdle;
		return tap ? EKsTap : EKsNone;
		}
	const int head = aS.iHead;
	const unsigned nMs = aS.iSampleMs[head];
	const int nCoord = aS.iSampleCoord[head];
	int oldest = head;
	for (int k = 1; k < aS.iCount; k++)
		{
		const int idx = (head - k + KKsSamples) % KKsSamples;
		if (nMs - aS.iSampleMs[idx] > (unsigned)KKsVelocityWindowMs)
			break;
		oldest = idx;
		}
	int v = 0;
	if (oldest != head && nMs != aS.iSampleMs[oldest] && tcElapsed(aMs, nMs) <= (unsigned)KKsVelocityWindowMs)
		v = (aS.iSampleCoord[oldest] - nCoord) * 1000 / (int)(nMs - aS.iSampleMs[oldest]);
	const int maxFling = vpadClamp(aS.iMaxFling, 0, 4000000);
	v = vpadClamp(v, -maxFling, maxFling);
	if (vpadAbs(v) >= ksMinFling(aS))
		{
		aS.iState = EKsFlinging;
		aS.iVel8 = v * 256;
		}
	else
		aS.iState = EKsIdle;
	return EKsNone;
	}

int KScrollStep(TKScroll &aS, unsigned aMs)
	{
	int ev = EKsNone;
	if (aS.iState == EKsPressed && !aS.iCaught && !aS.iLongSent && tcElapsed(aMs, aS.iDownMs) >= (unsigned)KKsLongPressMs)
		{
		aS.iLongSent = 1;
		ev = EKsLongPress;
		}
	const unsigned gap = tcElapsed(aMs, aS.iStepMs);
	unsigned dt = gap;
	if (dt > (unsigned)KKsMaxStepMs)
		dt = (unsigned)KKsMaxStepMs;
	for (unsigned t = 0; t < dt; t++)
		{
		if (aS.iState == EKsFlinging)
			ksFlingMs(aS);
		else if (aS.iState == EKsSettling)
			ksSettleMs(aS);
		else
			break;
		}
	aS.iStepMs += gap;
	return ev;
	}

int KScrollPos(const TKScroll &aS)
	{
	const int v = aS.iPos8 + 128;
	if (v >= 0)
		return v / 256;
	return -((-v + 255) / 256);
	}

int KScrollActive(const TKScroll &aS)
	{
	return aS.iState != EKsIdle && aS.iState != EKsPressed;
	}

void KScrollTo(TKScroll &aS, int aTargetPx, int aAnimate, unsigned aMs)
	{
	if (aS.iState == EKsPressed || aS.iState == EKsDragging)
		return;
	const int maxPx = ksMax8(aS) / 256;
	const int t8 = vpadClamp(aTargetPx, 0, maxPx) * 256;
	aS.iStepMs = aMs;
	if (aAnimate)
		{
		aS.iState = EKsSettling;
		aS.iTarget8 = t8;
		aS.iVel8 = 0;
		return;
		}
	aS.iPos8 = t8;
	aS.iVel8 = 0;
	aS.iState = EKsIdle;
	}

void KScrollEnsureVisible(TKScroll &aS, int aTop, int aBottom, int aAnimate, unsigned aMs)
	{
	const int base = aS.iState == EKsSettling ? aS.iTarget8 / 256 : KScrollPos(aS);
	const int top = ksClampSize(aTop);
	const int bottom = ksClampSize(aBottom);
	int to = 0;
	if (top < base)
		to = top;
	else if (bottom > base + aS.iView)
		to = bottom - top > aS.iView ? top : bottom - aS.iView;
	else
		return;
	KScrollTo(aS, to, aAnimate, aMs);
	}
