// On-screen gamepad, Symbian side. Pointer events of the game window go to the portable
// core (src/ui/touchcore.h); its pad word and sticks are merged into PAD1_readPort; the
// panel is painted by the portable drawing (src/ui/touchdraw.h) into the frame just before
// the frame is shown. Without a touch screen (AknLayoutUtils::PenEnabled) nothing here runs
// except the probe, which returns EFalse.
#include <e32std.h>
#include <w32std.h>
#include <hal.h>
#include <aknutils.h>
#include "touchcore.h"
#include "touchdraw.h"

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

// Room for the art of the largest layout: the E7 sticks layout needs 5985 spans and 87048
// coverage bytes, the rest is headroom for the rounding of the pixel pitch.
const TInt KTouchArtSpans = 8192;
const TInt KTouchArtAlpha = 131072;

typedef void (*TTouchEnableAdvanced)(RWindowBase *);

typedef char KTouchPointerEventSize[sizeof(TPointerEvent) == 24 ? 1 : -1];

static TBool g_touchPen;     // the device has a pen
static TBool g_touchOff;     // command-line run: no panel, no input
static TBool g_touchOn;      // pen, not off, landscape, and the layout fits and has its art
static TBool g_touchHidden;  // menu open or focus lost: no input, no panel
static TInt g_scrW;          // frame size the layout was built for
static TInt g_scrH;
static TInt g_umPerPx;
static TBool g_fits;         // the layout of the current frame fits the screen
static TVpadLayout g_layout;
static TVpadState g_state;
static TBool g_optSticks;    // command line: the two-stick layout
static TInt g_alphaPct = 100;  // command line: opacity of the untouched panel, percent
static TVpadArt g_art;
static TBool g_artOk;
static TVpadSpan *g_artSpan; // buffers of g_art, taken from the heap on the first layout
static TUint8 *g_artAlpha;

// What the panel last showed. The game picture is rewritten every frame, the bars around
// it are not: when this changes, the panel clears its own boxes on the bars first.
static TBool g_keyValid;
static TUint g_key;
static TUint g_axes;

// Builds the art of g_layout into the buffers, which are allocated once for the whole run.
static TBool touchArtBuild(void)
	{
	if (!g_artSpan)
		{
		g_artSpan = (TVpadSpan *)User::Alloc((TInt)(KTouchArtSpans * sizeof(TVpadSpan)));
		g_artAlpha = (TUint8 *)User::Alloc(KTouchArtAlpha);
		if (!g_artSpan || !g_artAlpha)
			{
			if (g_artSpan)
				User::Free(g_artSpan);
			if (g_artAlpha)
				User::Free(g_artAlpha);
			g_artSpan = NULL;
			g_artAlpha = NULL;
			return EFalse;
			}
		}
	g_art.iSpan = g_artSpan;
	g_art.iSpanCap = KTouchArtSpans;
	g_art.iAlpha = g_artAlpha;
	g_art.iAlphaCap = KTouchArtAlpha;
	return VpadArtBuild(g_art, g_layout, g_umPerPx) != 0;
	}

// The state of the panel in one line. Logged after every change of the layout or of the
// command-line options, so that the line shows what is in effect, not what was asked for.
static void touchLog(void)
	{
	ps1LogFmt("touch", "layout size=%dx%d umpx=%d on=%d controls=%d sticks=%d alpha=%d art=%d spans=%d",
		(int)g_scrW, (int)g_scrH, (int)g_umPerPx, (int)g_touchOn, (int)(g_fits ? g_layout.iCount : 0),
		(int)g_optSticks, (int)g_alphaPct, (int)g_artOk, (int)g_art.iSpanCount);
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
	g_fits = aW > aH && VpadDefaultLayout(g_layout, aW, aH, g_umPerPx, 100,
		g_optSticks ? EVpadOptSticks : 0) != 0;
	g_artOk = g_fits && touchArtBuild();
	g_touchOn = g_touchPen && !g_touchOff && g_artOk;
	touchLog();
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
enum { KTouchTraceEvents = 120 };

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

// Layout options from the command line, read next to touchon: sticks selects the two-stick
// layout; aAlphaPct is the opacity of the untouched panel, 10..100 percent.
void voutSymbianTouchOptions(TInt aSticks, TInt aAlphaPct)
	{
	g_optSticks = aSticks ? ETrue : EFalse;
	g_alphaPct = aAlphaPct < 10 ? 10 : (aAlphaPct > 100 ? 100 : aAlphaPct);
	if (g_touchPen && g_scrW > 0)
		touchLayout(g_scrW, g_scrH);
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
	// The pointer number is read by offset, not through a header: the first events of a
	// run are logged so that a wrong offset or flag shows on a real touch screen.
	static TInt traced = 0;
	if (traced < KTouchTraceEvents)
		{
		traced++;
		ps1LogFmt("touch", "ev type=%d x=%d y=%d mod=%08x b28=%d id=%d bits=%04x", (int)aEvent.iType,
			(int)x, (int)y, (unsigned)aEvent.iModifiers,
			(int)((const TUint8 *)&aEvent)[KTouchPointerNumberOffset], (int)id, (unsigned)g_state.iBits);
		}
	return (VpadTakeActions(g_state) & 1u) != 0;
	}

// Turns the panel and its input off for the rest of the run (command-line runs).
void voutSymbianTouchOff(void)
	{
	g_touchOff = ETrue;
	g_touchOn = EFalse;
	touchLog();
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

// Axes of the left stick of the touch layout. The default layout has a d-pad, so this stays
// at the centre until a layout with EVpadOptSticks or EVpadOptStick is used. Returns 1 when
// the stick is deflected.
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

// The right stick, same contract as voutSymbianTouchStick.
extern "C" TInt voutSymbianTouchRightStick(unsigned char *aRX, unsigned char *aRY)
	{
	if (!g_touchOn || g_touchHidden)
		return 0;
	if (g_state.iRightX == KVpadStickCentre && g_state.iRightY == KVpadStickCentre)
		return 0;
	*aRX = g_state.iRightX;
	*aRY = g_state.iRightY;
	return 1;
	}

// Whether a pointer is down on control aIx.
static TUint tpCtlHeld(TInt aIx)
	{
	for (TInt p = 0; p < KVpadMaxPointers; p++)
		if (g_state.iP[p].iDown && g_state.iP[p].iCtl == aIx)
			return ETrue;
	return EFalse;
	}

// Everything the panel shows apart from the stick axes (see tpAxes), as one number: a
// change of it means the bars must be repainted.
static TUint tpKey(void)
	{
	TUint k = g_state.iBits;
	if (tpCtlHeld(EVpadIxMenu))
		k |= 0x10000u;
	if (g_touchHidden)
		k |= 0x20000u;
	return k;
	}

// The four stick axes in one number.
static TUint tpAxes(void)
	{
	return (TUint)g_state.iLx | ((TUint)g_state.iLy << 8) | ((TUint)g_state.iRightX << 16) |
		((TUint)g_state.iRightY << 24);
	}

// Draws the panel into the frame after conversion and overlay, before the frame is shown.
// aDst is the frame bitmap of aSW x aSH pixels, aPixB its bytes per pixel (2 or 4); the
// game picture occupies aFw x aFh pixels at (aFx, aFy), the rest is bars. The picture is
// painted on every call; the bars are cleared and painted only when the panel changed.
extern "C" void voutSymbianTouchDraw(TUint8 *aDst, TInt aStrideB, TInt aPixB, TInt aSW, TInt aSH,
	TInt aFx, TInt aFy, TInt aFw, TInt aFh)
	{
	if (g_touchPen && !g_touchOff && (aSW != g_scrW || aSH != g_scrH))
		touchLayout(aSW, aSH);
	const TUint key = tpKey();
	const TUint axes = tpAxes();
	const TBool changed = !g_keyValid || key != g_key || axes != g_axes;
	g_key = key;
	g_axes = axes;
	g_keyValid = ETrue;
	if (!g_touchOn)
		return;
	TVpadCanvas c;
	c.iDst = aDst;
	c.iStride = aStrideB;
	c.iPixB = aPixB;
	c.iW = aSW;
	c.iH = aSH;
	c.iPicX = aFx;
	c.iPicY = aFy;
	c.iPicW = aFw;
	c.iPicH = aFh;
	if (changed)
		VpadArtClearBars(g_layout, c);
	if (g_touchHidden)
		return;
	VpadArtPaint(g_art, g_layout, g_state, c, g_alphaPct, changed);
	}
