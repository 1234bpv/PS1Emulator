// On-screen gamepad, Symbian side. Pointer events of the game window go to the portable
// core (src/ui/touchcore.h); its pad word and stick are merged into PAD1_readPort; the
// panel is drawn into the frame just before the frame is shown. Without a touch screen
// (AknLayoutUtils::PenEnabled) nothing here runs except the probe, which returns EFalse.
#include <e32std.h>
#include <w32std.h>
#include <hal.h>
#include <aknutils.h>
#include "touchcore.h"

extern "C" void ps1LogFmt(const char *aType, const char *aFmt, ...);

// Pixel pitch used when HAL does not report the display size: the Nokia E7 value.
const TInt KTouchDefaultUmPerPx = 138;

// An advanced pointer event carries this flag in iModifiers (EModifierAdvancedPointerEvent,
// e32keys.h of the Symbian kernel). Its pointer number is the byte at offset 28: iData holds
// { TInt32 iPosZ; TUint8 iPointerNumber; } right after the 24-byte TPointerEvent.
const TUint KTouchAdvancedFlag = 0x10000000u;
const TInt KTouchPointerNumberOffset = 28;

// RWindowBase::EnableAdvancedPointers is not in the FP1 import library; ws32 exports it
// as ordinal 609 in the EABI build (WS322U.DEF).
const TInt KTouchEnableAdvancedOrdinal = 609;

// Outline thickness of every control, px.
const TInt KTouchLine = 2;

typedef void (*TTouchEnableAdvanced)(RWindowBase *);

typedef char KTouchPointerEventSize[sizeof(TPointerEvent) == 24 ? 1 : -1];

static TBool g_touchPen;     // the device has a pen
static TBool g_touchOff;     // command-line run: no panel, no input
static TBool g_touchOn;      // pen, not off, landscape, and the layout fits the frame
static TBool g_touchHidden;  // menu open or focus lost: no input, no panel
static TInt g_scrW;          // frame size the layout was built for
static TInt g_scrH;
static TInt g_umPerPx;
static TVpadLayout g_layout;
static TVpadState g_state;

// What the panel last showed. The game picture is rewritten every frame, the bars around
// it are not: when this changes, the panel blackens its own boxes on the bars first.
static TBool g_keyValid;
static TUint g_key;

// Target of the draw call in progress.
static TUint8 *g_cDst;
static TInt g_cStride;
static TInt g_cPix;
static TInt g_cW;
static TInt g_cH;

static TUint32 tpColour(TUint aRgb)
	{
	if (g_cPix == 4)
		return aRgb;
	const TUint r = (aRgb >> 16) & 0xFF;
	const TUint g = (aRgb >> 8) & 0xFF;
	const TUint b = aRgb & 0xFF;
	return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	}

static void tpPut(TInt aX, TInt aY, TUint32 aC)
	{
	if (aX < 0 || aY < 0 || aX >= g_cW || aY >= g_cH)
		return;
	TUint8 *p = g_cDst + aY * g_cStride + aX * g_cPix;
	if (g_cPix == 4)
		*(TUint32 *)p = aC;
	else
		*(TUint16 *)p = (TUint16)aC;
	}

// Horizontal run from aX0 to aX1 inclusive, clipped to the canvas. A stippled run writes
// every other pixel in a checkerboard: the pressed fill. Unlike a blend it reads nothing
// back and gives the same pixels however many frames repeat it over the bars.
static void tpSpan(TInt aY, TInt aX0, TInt aX1, TUint32 aC, TBool aStipple)
	{
	if (aY < 0 || aY >= g_cH)
		return;
	if (aX0 < 0)
		aX0 = 0;
	if (aX1 >= g_cW)
		aX1 = g_cW - 1;
	const TInt step = aStipple ? 2 : 1;
	if (aStipple && ((aX0 + aY) & 1))
		aX0++;
	for (TInt x = aX0; x <= aX1; x += step)
		tpPut(x, aY, aC);
	}

// Circle of radius aR, outline aT px thick inside its edge, or a filled disc. Each row is
// an integer chord, so the work is per row, not per pixel of the bounding box.
static void tpCircle(TInt aCx, TInt aCy, TInt aR, TInt aT, TUint32 aC, TBool aFill, TBool aStipple)
	{
	const TInt inner = aR - aT;
	for (TInt dy = -aR; dy <= aR; dy++)
		{
		const TInt xo = VpadIsqrt(aR * aR - dy * dy);
		if (aFill || inner <= 0 || dy * dy >= inner * inner)
			tpSpan(aCy + dy, aCx - xo, aCx + xo, aC, aStipple);
		else
			{
			const TInt xi = VpadIsqrt(inner * inner - dy * dy);
			tpSpan(aCy + dy, aCx - xo, aCx - xi - 1, aC, aStipple);
			tpSpan(aCy + dy, aCx + xi + 1, aCx + xo, aC, aStipple);
			}
		}
	}

static void tpRect(TInt aCx, TInt aCy, TInt aHw, TInt aHh, TInt aT, TUint32 aC, TBool aFill, TBool aStipple)
	{
	for (TInt dy = -aHh; dy <= aHh; dy++)
		{
		const TInt y = aCy + dy;
		if (aFill || dy < -aHh + aT || dy > aHh - aT)
			tpSpan(y, aCx - aHw, aCx + aHw, aC, aStipple);
		else
			{
			tpSpan(y, aCx - aHw, aCx - aHw + aT - 1, aC, aStipple);
			tpSpan(y, aCx + aHw - aT + 1, aCx + aHw, aC, aStipple);
			}
		}
	}

// Segment drawn as Bresenham steps of 2x2 blocks: a line KTouchLine px thick.
static void tpLine(TInt aX0, TInt aY0, TInt aX1, TInt aY1, TUint32 aC)
	{
	TInt dx = aX1 - aX0;
	TInt dy = aY1 - aY0;
	const TInt sx = dx < 0 ? -1 : 1;
	const TInt sy = dy < 0 ? -1 : 1;
	if (dx < 0)
		dx = -dx;
	if (dy < 0)
		dy = -dy;
	TInt err = dx - dy;
	for (;;)
		{
		tpPut(aX0, aY0, aC);
		tpPut(aX0 + 1, aY0, aC);
		tpPut(aX0, aY0 + 1, aC);
		tpPut(aX0 + 1, aY0 + 1, aC);
		if (aX0 == aX1 && aY0 == aY1)
			break;
		const TInt e2 = 2 * err;
		if (e2 > -dy)
			{
			err -= dy;
			aX0 += sx;
			}
		if (e2 < dx)
			{
			err += dx;
			aY0 += sy;
			}
		}
	}

// Outline triangle pointing along (aDx, aDy): tip at aTip from the centre, base at aBase,
// base half width aHalf.
static void tpArrow(TInt aCx, TInt aCy, TInt aDx, TInt aDy, TInt aTip, TInt aBase, TInt aHalf, TUint32 aC)
	{
	const TInt tx = aCx + aDx * aTip;
	const TInt ty = aCy + aDy * aTip;
	const TInt bx = aCx + aDx * aBase;
	const TInt by = aCy + aDy * aBase;
	const TInt ax = bx - aDy * aHalf;
	const TInt ay = by + aDx * aHalf;
	const TInt cx = bx + aDy * aHalf;
	const TInt cy = by - aDx * aHalf;
	tpLine(tx, ty, ax, ay, aC);
	tpLine(tx, ty, cx, cy, aC);
	tpLine(ax, ay, cx, cy, aC);
	}

// Label font: 3x5 pixels, one row per byte, bit 2 is the left column.
static const char KTouchFontChars[] = "LRSECTA12";
static const TUint8 KTouchFontRows[9][5] =
	{
	{ 4, 4, 4, 4, 7 },
	{ 6, 5, 6, 5, 5 },
	{ 7, 4, 7, 1, 7 },
	{ 7, 4, 6, 4, 7 },
	{ 7, 4, 4, 4, 7 },
	{ 7, 2, 2, 2, 2 },
	{ 2, 5, 7, 5, 5 },
	{ 2, 6, 2, 2, 7 },
	{ 6, 1, 2, 4, 7 }
	};

// Label centred at (aCx, aCy), each font pixel drawn 2x2: a glyph is 6x10 px, 2 px apart.
static void tpText(TInt aCx, TInt aCy, const char *aText, TUint32 aC)
	{
	TInt n = 0;
	while (aText[n])
		n++;
	TInt x0 = aCx - (n * 8 - 2) / 2;
	const TInt y0 = aCy - 5;
	for (TInt i = 0; i < n; i++, x0 += 8)
		{
		TInt g = -1;
		for (TInt k = 0; k < 9; k++)
			if (KTouchFontChars[k] == aText[i])
				g = k;
		if (g < 0)
			continue;
		for (TInt r = 0; r < 5; r++)
			for (TInt c = 0; c < 3; c++)
				if (KTouchFontRows[g][r] & (4 >> c))
					{
					const TInt px = x0 + c * 2;
					const TInt py = y0 + r * 2;
					tpPut(px, py, aC);
					tpPut(px + 1, py, aC);
					tpPut(px, py + 1, aC);
					tpPut(px + 1, py + 1, aC);
					}
		}
	}

static TBool tpCtlHeld(TInt aIx)
	{
	for (TInt p = 0; p < KVpadMaxPointers; p++)
		if (g_state.iP[p].iDown && g_state.iP[p].iCtl == aIx)
			return ETrue;
	return EFalse;
	}

// Everything the panel shows, as one number: a change of it means a redraw.
static TUint tpKey(void)
	{
	TUint k = g_state.iBits;
	if (tpCtlHeld(EVpadIxMenu))
		k |= 0x10000u;
	if (g_touchHidden)
		k |= 0x20000u;
	return k;
	}

static void tpGlyph(TInt aIx, TInt aCx, TInt aCy, TInt aR, TUint32 aC)
	{
	const TInt s = aR / 2;
	switch (aIx)
		{
		case EVpadIxTriangle:
			tpLine(aCx, aCy - s, aCx + s, aCy + s * 2 / 3, aC);
			tpLine(aCx + s, aCy + s * 2 / 3, aCx - s, aCy + s * 2 / 3, aC);
			tpLine(aCx - s, aCy + s * 2 / 3, aCx, aCy - s, aC);
			break;
		case EVpadIxSquare:
			tpRect(aCx, aCy, s * 3 / 4, s * 3 / 4, KTouchLine, aC, EFalse, EFalse);
			break;
		case EVpadIxCircle:
			tpCircle(aCx, aCy, s * 3 / 4, KTouchLine, aC, EFalse, EFalse);
			break;
		case EVpadIxCross:
			tpLine(aCx - s * 3 / 4, aCy - s * 3 / 4, aCx + s * 3 / 4, aCy + s * 3 / 4, aC);
			tpLine(aCx - s * 3 / 4, aCy + s * 3 / 4, aCx + s * 3 / 4, aCy - s * 3 / 4, aC);
			break;
		case EVpadIxMenu:
			tpLine(aCx - aR / 2, aCy - aR / 3, aCx + aR / 2, aCy - aR / 3, aC);
			tpLine(aCx - aR / 2, aCy, aCx + aR / 2, aCy, aC);
			tpLine(aCx - aR / 2, aCy + aR / 3, aCx + aR / 2, aCy + aR / 3, aC);
			break;
		default:
			break;
		}
	}

static void tpControl(TInt aIx, const TVpadControl &aC)
	{
	const TBool pressed = aIx == EVpadIxMenu ? tpCtlHeld(aIx) : (g_state.iBits & aC.iBits) != 0;
	const TUint32 white = tpColour(0xFFFFFF);
	const TUint32 black = tpColour(0x000000);
	const TUint32 fg = pressed ? black : white;
	if (aC.iShape == EVpadShapeRect)
		{
		if (pressed)
			tpRect(aC.iCx, aC.iCy, aC.iRx, aC.iRy, KTouchLine, white, ETrue, ETrue);
		tpRect(aC.iCx, aC.iCy, aC.iRx + 2, aC.iRy + 2, KTouchLine, black, EFalse, EFalse);
		tpRect(aC.iCx, aC.iCy, aC.iRx, aC.iRy, KTouchLine, white, EFalse, EFalse);
		const char *label = "";
		switch (aIx)
			{
			case EVpadIxL1: label = "L1"; break;
			case EVpadIxL2: label = "L2"; break;
			case EVpadIxR1: label = "R1"; break;
			case EVpadIxR2: label = "R2"; break;
			case EVpadIxSelect: label = "SELECT"; break;
			case EVpadIxStart: label = "START"; break;
			default: break;
			}
		tpText(aC.iCx, aC.iCy, label, fg);
		return;
		}
	if (pressed)
		tpCircle(aC.iCx, aC.iCy, aC.iRx, 0, white, ETrue, ETrue);
	tpCircle(aC.iCx, aC.iCy, aC.iRx + 2, KTouchLine, black, EFalse, EFalse);
	tpCircle(aC.iCx, aC.iCy, aC.iRx, KTouchLine, white, EFalse, EFalse);
	tpGlyph(aIx, aC.iCx, aC.iCy, aC.iRx, fg);
	}

// The d-pad: a ring with four arrows; a pressed direction fills its arm and turns its arrow dark.
static void tpDpad(const TVpadControl &aC)
	{
	const TInt cx = aC.iCx;
	const TInt cy = aC.iCy;
	const TInt r = aC.iRx;
	const TUint32 white = tpColour(0xFFFFFF);
	const TUint32 black = tpColour(0x000000);
	const TInt bit[4] = { EVpadUp, EVpadRight, EVpadDown, EVpadLeft };
	const TInt dx[4] = { 0, 1, 0, -1 };
	const TInt dy[4] = { -1, 0, 1, 0 };
	TBool held[4];
	for (TInt k = 0; k < 4; k++)
		held[k] = (g_state.iBits & (1u << bit[k])) != 0;
	for (TInt k = 0; k < 4; k++)
		if (held[k])
			tpRect(cx + dx[k] * r * 3 / 5, cy + dy[k] * r * 3 / 5, r / 5, r / 5, 0, white, ETrue, ETrue);
	tpCircle(cx, cy, r + 2, KTouchLine, black, EFalse, EFalse);
	tpCircle(cx, cy, r, KTouchLine, white, EFalse, EFalse);
	for (TInt k = 0; k < 4; k++)
		tpArrow(cx, cy, dx[k], dy[k], r * 4 / 5, r * 2 / 5, r / 7, held[k] ? black : white);
	}

// Blackens the part of a control's box that lies on the bars, outside the game picture
// aFx..aFx+aFw-1 by aFy..aFy+aFh-1. The margin covers the dark outer outline.
static void tpClearOnBars(const TVpadControl &aC, TInt aFx, TInt aFy, TInt aFw, TInt aFh)
	{
	const TInt ry = (aC.iShape == EVpadShapeRect ? aC.iRy : aC.iRx) + 3;
	const TInt x0 = aC.iCx - aC.iRx - 3;
	const TInt x1 = aC.iCx + aC.iRx + 3;
	for (TInt y = aC.iCy - ry; y <= aC.iCy + ry; y++)
		{
		if (y < aFy || y >= aFy + aFh)
			{
			tpSpan(y, x0, x1, 0, EFalse);
			continue;
			}
		tpSpan(y, x0, x1 < aFx ? x1 : aFx - 1, 0, EFalse);
		tpSpan(y, x0 > aFx + aFw ? x0 : aFx + aFw, x1, 0, EFalse);
		}
	}

// Builds the layout for a frame of aW x aH pixels. The frame, not the window at
// construction, decides: the shell asks for landscape before the window exists, and the
// screen may turn only afterwards.
static void touchLayout(TInt aW, TInt aH)
	{
	g_scrW = aW;
	g_scrH = aH;
	VpadReset(g_state);
	g_keyValid = EFalse;
	const TBool fits = aW > aH && VpadDefaultLayout(g_layout, aW, aH, g_umPerPx, 100, 0) != 0;
	g_touchOn = g_touchPen && !g_touchOff && fits;
	ps1LogFmt("touch", "layout size=%dx%d umpx=%d on=%d controls=%d", (int)aW, (int)aH,
		(int)g_umPerPx, (int)g_touchOn, (int)(fits ? g_layout.iCount : 0));
	}

// Loads ws32 and asks for advanced (multi-touch) pointers. Returns EFalse when the library
// or the ordinal is missing; the pad then runs on one pointer.
static TBool touchEnableAdvanced(RWindowBase &aWin)
	{
	RLibrary lib;
	TInt err = lib.Load(_L("ws32.dll"));
	if (err != KErrNone)
		err = lib.Load(_L("ws32{000a0000}.dll"));
	if (err != KErrNone)
		return EFalse;
	const TLibraryFunction fn = lib.Lookup(KTouchEnableAdvancedOrdinal);
	if (!fn)
		{
		lib.Close();
		return EFalse;
		}
	reinterpret_cast<TTouchEnableAdvanced>(fn)(&aWin);
	lib.Close();
	return ETrue;
	}

// touchcore wants a clock that never steps back; the wall clock does when the user sets it.
static TUint touchNowMs(void)
	{
	return User::NTickCount();
	}

// Called from CPs1Container::ConstructL after SetRect and before ActivateL: multi-touch
// must be requested before the window is activated. Returns ETrue when the device has a pen.
TBool voutSymbianTouchProbe(RWindowBase &aWin)
	{
	// HAL EPen does not tell a touch screen from a keypad phone: the N95 8GB, which has
	// no touch screen, reports 1 (measured 2026-11-12). Avkon's own setting is what counts.
	TInt halPen = 0;
	HAL::Get(HAL::EPen, halPen);
	const TBool pen = AknLayoutUtils::PenEnabled();
	ps1LogFmt("touch", "probe hal=%d akn=%d", (int)halPen, (int)pen);
	if (!pen)
		return EFalse;
	TInt twips = 0;
	TInt pixels = 0;
	g_umPerPx = KTouchDefaultUmPerPx;
	if (HAL::Get(HAL::EDisplayXTwips, twips) == KErrNone &&
	    HAL::Get(HAL::EDisplayXPixels, pixels) == KErrNone && twips > 0 && pixels > 0)
		g_umPerPx = (twips * 25400 / 1440 + pixels / 2) / pixels;
	g_touchPen = ETrue;
	const TBool multi = touchEnableAdvanced(aWin);
	ps1LogFmt("touch", "pen=1 twips=%d pixels=%d multi=%d", (int)twips, (int)pixels, (int)multi);
	const TSize size = aWin.Size();
	touchLayout(size.iWidth, size.iHeight);
	return ETrue;
	}

// Pointer event of the game window. Returns ETrue when the menu button was tapped; the
// caller then opens the menu.
TBool voutSymbianTouchEvent(const TPointerEvent &aEvent)
	{
	if (!g_touchOn || g_touchHidden)
		return EFalse;
	TInt id = 0;
	if (aEvent.iModifiers & KTouchAdvancedFlag)
		id = ((const TUint8 *)&aEvent)[KTouchPointerNumberOffset];
	if (id >= KVpadMaxPointers)
		return EFalse;
	const TInt x = aEvent.iPosition.iX;
	const TInt y = aEvent.iPosition.iY;
	const TUint ms = touchNowMs();
	switch (aEvent.iType)
		{
		case TPointerEvent::EButton1Down:
			VpadPointerDown(g_state, g_layout, id, x, y, ms);
			break;
		case TPointerEvent::EDrag:
		case TPointerEvent::EMove:
			VpadPointerMove(g_state, g_layout, id, x, y, ms);
			break;
		case TPointerEvent::EButton1Up:
			VpadPointerUp(g_state, g_layout, id, ms);
			break;
		default:
			return EFalse;
		}
	return (VpadTakeActions(g_state) & 1u) != 0;
	}

// Turns the panel and its input off for the rest of the run (command-line runs).
void voutSymbianTouchOff(void)
	{
	g_touchOff = ETrue;
	g_touchOn = EFalse;
	}

// Menu open or focus lost (called from voutSymbianSuspend): fingers down at that moment
// will not send their release to the game, so the pad forgets them.
extern "C" void voutSymbianTouchSuspend(TInt aOn)
	{
	if (!g_touchOn)
		return;
	VpadReset(g_state);
	g_touchHidden = aOn != 0;
	}

// Pad bits of the touch controls, OR-ed into buttonStatus in PAD1_readPort.
extern "C" TUint16 voutSymbianTouchBits(void)
	{
	if (!g_touchOn || g_touchHidden)
		return 0;
	return g_state.iBits;
	}

// Stick axes of the touch layout. The default layout has a d-pad, so this stays at the
// centre until a layout with EVpadOptStick is used. Returns 1 when the stick is deflected.
extern "C" TInt voutSymbianTouchStick(unsigned char *aLX, unsigned char *aLY)
	{
	if (!g_touchOn || g_touchHidden)
		return 0;
	if (g_state.iLx == KVpadStickCentre && g_state.iLy == KVpadStickCentre)
		return 0;
	*aLX = g_state.iLx;
	*aLY = g_state.iLy;
	return 1;
	}

// Draws the panel into the frame after conversion and overlay, before the frame is shown.
// aDst is the frame bitmap of aSW x aSH pixels, aPixB its bytes per pixel (2 or 4); the
// game picture occupies aFw x aFh pixels at (aFx, aFy), the rest is bars.
extern "C" void voutSymbianTouchDraw(TUint8 *aDst, TInt aStrideB, TInt aPixB, TInt aSW, TInt aSH,
	TInt aFx, TInt aFy, TInt aFw, TInt aFh)
	{
	if (g_touchPen && !g_touchOff && (aSW != g_scrW || aSH != g_scrH))
		touchLayout(aSW, aSH);
	const TUint key = tpKey();
	const TBool changed = !g_keyValid || key != g_key;
	g_key = key;
	g_keyValid = ETrue;
	if (!g_touchOn || g_touchHidden)
		return;
	g_cDst = aDst;
	g_cStride = aStrideB;
	g_cPix = aPixB;
	g_cW = aSW;
	g_cH = aSH;
	for (TInt i = 0; i < g_layout.iCount; i++)
		{
		const TVpadControl &c = g_layout.iC[i];
		if (c.iRx <= 0)
			continue;
		if (changed)
			tpClearOnBars(c, aFx, aFy, aFw, aFh);
		if (i == EVpadIxDir)
			tpDpad(c);
		else
			tpControl(i, c);
		}
	}
