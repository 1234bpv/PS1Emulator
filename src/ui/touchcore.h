// Portable touch core: on-screen gamepad (Vpad*) and kinetic list scroller (KScroll*).
//
// Platform-free on purpose: the same code is built for the phone by GCC 3.4.3 and
// for the host by the test runner. Hence C++98, no STL, no exceptions, no RTTI, no
// heap, no floating point, no platform headers. All state lives in caller-owned
// structs. Coordinates are screen pixels, x to the right, y down. Times are
// milliseconds from any monotonic clock; only differences are used, and they are
// computed as unsigned so that a wrapping clock is harmless. A difference of 2^31
// or more means the time given is earlier than one already seen (two callers
// reading the clock at different moments): it counts as no time passed, and
// KScrollStep then leaves iStepMs where it was.
#ifndef PS1_TOUCHCORE_H
#define PS1_TOUCHCORE_H

// ============================ On-screen gamepad ==============================

// Bit numbers of the pad word. They equal DKEY_* of the emulator core, so the pad
// word produced here can be OR-ed with the one built from hardware keys.
enum TVpadBit
	{
	EVpadSelect = 0, EVpadL3 = 1, EVpadR3 = 2, EVpadStart = 3,
	EVpadUp = 4, EVpadRight = 5, EVpadDown = 6, EVpadLeft = 7,
	EVpadL2 = 8, EVpadR2 = 9, EVpadL1 = 10, EVpadR1 = 11,
	EVpadTriangle = 12, EVpadCircle = 13, EVpadCross = 14, EVpadSquare = 15
	};

enum TVpadKind
	{
	EVpadKindDpad = 0,     // 8-way direction pad, circle
	EVpadKindButton = 1,   // presses iBits while touched
	EVpadKindStick = 2,    // analogue stick, circle; iId 0 is the left one, 1 the right
	EVpadKindAction = 3    // not a pad button: reports iId when released inside
	};

enum TVpadShape { EVpadShapeCircle = 0, EVpadShapeRect = 1 };

// Fixed indices of the controls VpadDefaultLayout produces.
enum TVpadIndex
	{
	EVpadIxDir = 0,        // d-pad; the stick when EVpadOptStick is given and
	                       // EVpadOptSticks is not in effect
	EVpadIxTriangle = 1, EVpadIxCircle = 2, EVpadIxCross = 3, EVpadIxSquare = 4,
	EVpadIxL1 = 5, EVpadIxL2 = 6, EVpadIxR1 = 7, EVpadIxR2 = 8,
	EVpadIxSelect = 9, EVpadIxStart = 10, EVpadIxMenu = 11,
	EVpadDefaultCount = 12,
	// Present only with EVpadOptSticks:
	EVpadIxLStick = 12, EVpadIxRStick = 13,
	EVpadSticksCount = 14
	};

enum
	{
	KVpadMaxControls = 16,
	KVpadMaxPointers = 4,
	KVpadStickCentre = 127,   // stick output is 127 + d, d in [-127, 127]
	KVpadDualMaxMs = 60,      // a jump slower than this is a slide, not a second finger
	KVpadTapMs = 250,         // a stick touch no longer than this can be a tap...
	KVpadTapSlopPct = 30,     // ...if it ends this near where it began, percent of iRx
	KVpadMinSizePx = 8        // VpadResizeControl makes nothing smaller than this
	};

enum TVpadOption
	{
	EVpadOptStick = 1,          // the left control is an analogue stick instead of a d-pad;
	                            // no effect where EVpadOptSticks is in effect
	EVpadOptResistiveDual = 2,  // single-touch panel: infer a second finger (see VpadPointerMove)
	EVpadOptSticks = 4          // landscape only: the d-pad stays and two sticks are added
	};

struct TVpadControl
	{
	unsigned char iKind;     // TVpadKind
	unsigned char iShape;    // TVpadShape; ignored for d-pad and stick, always circles
	unsigned char iGroup;    // 0: stands alone. Same non-zero value: one finger may glide
	                         // between these controls and press several where they overlap
	unsigned char iId;       // EVpadKindAction: action number 0..31; above that the
	                         // action is never reported. EVpadKindStick: 1 is the right
	                         // stick, any other value the left one
	unsigned short iBits;    // EVpadKindButton: mask of pad bits (1 << EVpad*)
	short iCx, iCy;          // centre
	short iRx, iRy;          // circle: iRx is the radius, iRy is ignored;
	                         // rect: half width and half height. This is the DRAWN size.
	};

struct TVpadLayout
	{
	int iCount;         // taken as 0 when negative, as KVpadMaxControls when larger
	TVpadControl iC[KVpadMaxControls];
	int iHitPct;        // press zone = drawn size * iHitPct / 100; meant to be 100..200
	int iLeashPct;      // leash zone = drawn size * iLeashPct / 100; meant to be at
	                    // least iHitPct, but no rule relies on it: each zone is
	                    // tested on its own
	int iDeadPct;       // d-pad and stick: dead centre, percent of the radius, 0..60
	int iDiagPct;       // d-pad: width of a diagonal sector, percent of 45 degrees.
	                    // 100: eight equal sectors. 0: four-way, never two bits at once.
	int iFourWayPct;    // d-pad: nearer the centre than this percent of the radius a
	                    // press is never diagonal (the centre is where thumbs miss)
	int iStickDigitalPct; // stick: also press d-pad bits at this deflection or more,
	                    // percent of the radius; 0 turns the digital fallback off
	int iStickFloat;    // non-zero: the stick is centred where the finger lands
	int iStickClickMs;  // a stick touched again this soon after a tap on it presses
	                    // L3 or R3 (see "Stick click"); 0 turns that off; taken
	                    // within 0..2000
	int iDualJumpPx;    // > 0 turns on second-finger inference, see VpadPointerMove
	};

struct TVpadPointer
	{
	unsigned char iDown;
	signed char iCtl;        // index of the control hit on the way down, -1: none
	unsigned char iDiag;     // d-pad: last classification, 0 none, 1 one bit, 2 two bits
	unsigned char iClick;    // stick: this touch also presses L3 or R3
	short iX, iY;            // last position
	short iOx, iOy;          // stick: the point deflection is measured from
	unsigned iMs;            // time of the last event
	unsigned iDownMs;        // time the pointer went down
	short iDownX, iDownY;    // where it went down
	short iPad[2];
	unsigned short iBits;    // pad bits this pointer presses now
	unsigned short iHeld;    // buttons: bit i set if control i is in the remembered set
	};

struct TVpadState
	{
	TVpadPointer iP[KVpadMaxPointers];
	unsigned short iBits;    // OR over all pointers; maintained by every call
	unsigned short iEdges;   // bits that went from 0 to 1 since VpadTakeEdges
	unsigned iActions;       // bit iId set by a completed action, until VpadTakeActions
	unsigned char iLx, iLy;  // left stick, KVpadStickCentre when untouched
	unsigned char iDual;     // second-finger inference is active
	unsigned char iByDown;   // bit p set: pointer p holds a stick that VpadPointerDown
	                         // captured for it (not inference); see "Stick click"
	short iDualX, iDualY;    // where the first finger is assumed to rest
	unsigned char iRightX, iRightY; // right stick, KVpadStickCentre when untouched
	unsigned char iArmed[2]; // per stick side (0 left, 1 right): a tap that no capture
	                         // by VpadPointerDown has followed yet
	unsigned iArmMs[2];      // when that tap was lifted
	};

// --- Geometry rules shared by everything below --------------------------------
//
// Zones. A circle control with radius R has press radius R*iHitPct/100 and leash
// radius R*iLeashPct/100 (integer division). A point is inside a circle zone of
// radius Z when dx*dx + dy*dy <= Z*Z. A rect control with half sizes (W, H) has
// press half sizes (W*iHitPct/100, H*iHitPct/100), likewise for the leash, and a
// point is inside when |dx| <= halfW and |dy| <= halfH.
//
// Normalised distance, used to choose between overlapping zones: for a circle
// dist*1000/Z computed as isqrt(dx*dx+dy*dy)*1000/Z; for a rect
// max(|dx|*1000/halfW, |dy|*1000/halfH). Smaller wins, the lower index on a tie.
//
// "dist(a, b) <= r" below always means dx*dx + dy*dy <= r*r. A control with
// iRx <= 0, or a rect with iRy <= 0, is ignored by every function. iDeadPct above
// 60 is treated as 60, iDiagPct above 100 as 100, and a negative value of either
// as 0. iHitPct, iLeashPct, iFourWayPct and iStickDigitalPct are taken within
// 0..400. Every division in this file is integer division truncating towards zero.
//
// Range. Everything is 32-bit integer arithmetic. Coordinates given to the pointer
// calls are clamped to -8192..8191 on entry, and control geometry must lie within
// 0..8191, which keeps every product in the rules below inside 32 bits.

// Fills aOut with the default controls (twelve, or fourteen with EVpadOptSticks on
// a landscape screen) for a screen of aScreenW x aScreenH
// pixels whose pixel pitch is aUmPerPx micrometres, at aScalePct percent of the
// nominal physical size. Returns the scale actually used (never above the clamped aScalePct),
// or 0 if no scale tried fits (aOut then holds the smallest try).
// aUmPerPx below 20 is taken as 20 and aScalePct is clamped to 25..400. The screen
// must be no larger than 8191 pixels a side, here and in VpadLayoutFits.
//
// Tunables are set to: iHitPct 125, iLeashPct 175, iDeadPct 18, iDiagPct 100,
// iFourWayPct 40, iStickDigitalPct 80, iStickFloat 0, iStickClickMs 300 (so the
// stick of EVpadOptStick clicks L3 as well),
// iDualJumpPx 0, or px(120) (at the scale finally used) when EVpadOptResistiveDual.
//
// Sizes. With s the scale being tried, px(t) = (t * s + aUmPerPx / 2) / aUmPerPx
// converts tenths of a millimetre to pixels (t*100 micrometres, scaled by s/100,
// rounded). m = px(20) margin, g = px(10) gap, Rd = px(110) d-pad or stick
// radius, rb = px(45) face button radius, Rr = px(75) distance from the face
// cluster centre to a button centre, (tw, th) = (px(50), px(35)) half size of
// Select and Start, rm = px(35) menu button radius. Shoulder half size (sw, sh) is
// (px(70), px(40)) in landscape and (px(45), px(40)) in portrait.
//
// Landscape (aScreenW >= aScreenH), W and H being the screen size:
//   EVpadIxDir      circle Rd at (m + Rd, H - m - Rd), group 1
//   face centre F = (W - m - Rr - rb, H - m - Rr - rb); circles rb, group 2:
//   Triangle (F.x, F.y - Rr), Circle (F.x + Rr, F.y), Cross (F.x, F.y + Rr),
//   Square (F.x - Rr, F.y)
//   L1 rect at (m + sw, m + sh), L2 at (m + 3*sw + g, m + sh), group 3
//   R1 rect at (W - m - sw, m + sh), R2 at (W - m - 3*sw - g, m + sh), group 4
//   Select rect at (W/2 - (g + 1)/2 - tw, H - m - th), group 0
//   Start  rect at (W/2 + (g + 1)/2 + tw, H - m - th), group 0
//   Menu   circle rm at (W/2, m + rm), EVpadKindAction, iId 0, group 0
// Landscape with EVpadOptSticks: as above, except that Select and Start move up
// beside Menu and get the smaller half sizes (tw2, th2) = (px(43), px(27)):
//   Select rect at (W/2 - rm - g - tw2, m + rm), Start at (W/2 + rm + g + tw2, m + rm)
// and two sticks of radius Rs = px(70) take their place at the bottom:
//   EVpadIxLStick circle at (W/2 - g - Rs, H - m - Rs), EVpadKindStick, iId 0
//   EVpadIxRStick circle at (W/2 + g + Rs, H - m - Rs), EVpadKindStick, iId 1
//   both group 0, iBits 0. iCount is EVpadSticksCount; EVpadIxDir is the d-pad
//   whether or not EVpadOptStick is also given.
// Without EVpadOptSticks, or in portrait, iCount is EVpadDefaultCount.
// Portrait (aScreenH > aScreenW): EVpadOptSticks is ignored. The game picture
// takes the top yP = W*3/4 rows.
//   L1 at (m + sw, y1), L2 at (m + 3*sw + g, y1), R1 at (W - m - sw, y1),
//   R2 at (W - m - 3*sw - g, y1), with y1 = yP + m + sh
//   Select at (W/2 - rm - g - tw, H - m - th), Start at (W/2 + rm + g + tw, H - m - th)
//   Menu at (W/2, H - m - rm)
//   yMain = ((y1 + sh + g) + (H - m - 2*rm - g)) / 2
//   EVpadIxDir at (m + Rd, yMain), face centre F = (W - m - Rr - rb, yMain)
//   Kinds, shapes, sizes, groups and the offsets of the face buttons from F are
//   those of landscape.
// Buttons carry the matching single pad bit; the d-pad, stick and Menu have iBits 0.
//
// Fit. Scales s = aScalePct * (32 - i) / 32 are tried for i = 0, 1, ... 19 and the
// first one for which VpadLayoutFits is true is used. A scale that leaves any of
// the iCount controls without a size (iRx <= 0, or iRy <= 0 for a rect) does not
// fit, although VpadLayoutFits on its own ignores such controls.
int VpadDefaultLayout(TVpadLayout &aOut, int aScreenW, int aScreenH, int aUmPerPx,
	int aScalePct, unsigned aOptions);

// True when the layout can be shown on the screen: (a) the drawn box of every
// control (circle: centre +- iRx on both axes; rect: centre +- half sizes; the
// corner coordinates themselves belong to the box) lies within 0 <= x < aScreenW
// and 0 <= y < aScreenH; (b) the drawn boxes of two controls do not intersect
// unless they share a non-zero group (boxes that merely touch at an edge
// coordinate do intersect); (c) on a portrait screen no drawn box starts above
// yP = aScreenW*3/4.
int VpadLayoutFits(const TVpadLayout &aLayout, int aScreenW, int aScreenH);

// All pointers up, no bits, both sticks centred, no stick armed, nothing pending. A
// state belongs to the layout its pointers went down on: reset it whenever the
// layout changes.
void VpadReset(TVpadState &aState);

// --- Layouts the user arranges ------------------------------------------------
//
// A layout is plain data: an editor changes iCx, iCy, iRx, iRy of its controls with
// the two calls below and keeps the result. Either call changes the layout, so
// VpadReset the state afterwards. Overlap is the user's business: these
// calls do not prevent it, the capture rule resolves it, and VpadLayoutFits still
// tells whether a layout is free of it. Both calls do nothing when aIx is outside
// 0..count-1 (count as iCount is taken everywhere) or the control has no size.

// Index of the control a finger at (aX, aY) would capture with no other pointer
// down, by the capture rule of VpadPointerDown; -1 when there is none. The point is
// clamped like a pointer coordinate.
int VpadControlAt(const TVpadLayout &aLayout, int aX, int aY);

// Moves the centre of control aIx to (aCx, aCy) and pulls it back into the screen,
// each axis on its own: with r the half size on that axis (iRx on both axes for a
// circle, d-pad or stick) and n the screen size on it (taken as 1 when below 1),
// the centre is clamped to r .. n - 1 - r, and is n/2 when r > n - 1 - r: a
// control larger than the screen stays partly outside it, which VpadLayoutFits
// reports. The requested centre is clamped to -8192..8191 first. The screen must
// be no larger than 8191 pixels a side, here and in VpadResizeControl.
void VpadMoveControl(TVpadLayout &aLayout, int aIx, int aCx, int aCy,
	int aScreenW, int aScreenH);

// Sets the drawn size of control aIx: iRx = aRx for a circle, d-pad or stick (aRy
// is ignored and iRy left alone), half sizes (aRx, aRy) for a rect. Each value is
// first taken within KVpadMinSizePx..4095. The centre is then pulled back into the
// screen as in VpadMoveControl.
void VpadResizeControl(TVpadLayout &aLayout, int aIx, int aRx, int aRy,
	int aScreenW, int aScreenH);

// A finger went down. aId outside 0..KVpadMaxPointers-1 is ignored; a pointer that
// is already down is handled as VpadPointerMove.
//
// Capture: among all controls whose PRESS zone contains the point, the one with the
// smallest normalised distance is captured for the life of the touch. A d-pad or
// stick already captured by another pointer is skipped in this search, and so is a
// stick while another pointer holds a stick of the same side. With nothing
// to capture the touch stays inert until it is lifted: sliding onto a control later
// presses nothing.
//
// What a captured pointer presses, re-evaluated on every event:
//   Button: let S be the Button controls that share the captured control's group
//     (only the control itself when its group is 0) and whose press zone contains the
//     point. If S is not empty the pointer presses the OR of their iBits and S is
//     remembered. Otherwise, if the point is inside the LEASH zone of any remembered
//     control, the pointer keeps pressing what it pressed. Otherwise it presses
//     nothing and the remembered set is cleared; moving back presses again.
//   Dpad: with (dx, dy) from the centre and dist = isqrt(dx*dx + dy*dy): nothing
//     when dx == 0 and dy == 0, and nothing while dist < dead, where dead =
//     iRx*iDeadPct/100, or, when the pointer pressed something on the previous
//     event, that value*3/4. Otherwise hi = max(|dx|,|dy|),
//     lo = min(|dx|,|dy|), q = lo*1000/hi. The press is diagonal (both axis bits)
//     when iDiagPct >= 10, dist*100 >= iRx*iFourWayPct and q >= T, with T taken
//     from the table below at
//     iDiagPct/10 and then biased by the previous classification of this pointer:
//     T - 35 if it was diagonal, T + 35 if it was a single bit, T if it pressed
//     nothing. Not diagonal: only the axis with the larger |d|, the horizontal one
//     on a tie. dx > 0 is Right, dy > 0 is Down. There is no outer limit: a captured
//     d-pad keeps working when the finger slides beyond its rim.
//       iDiagPct/10:  0    1    2    3    4    5    6    7    8    9   10
//       T:         1000  924  854  788  727  668  613  560  510  461  414
//   Stick: (dx, dy) is measured from the origin (iOx, iOy), fixed at capture: the
//     control centre, or the capture point itself when iStickFloat is non-zero.
//     Centred when dx == 0 and dy == 0 and while dist < dead = iRx*iDeadPct/100
//     (no 3/4 hysteresis).
//     Otherwise the deflection grows from 0 at the dead rim to 127 at the drawn rim
//     and stays there beyond it: mag = min(dist, iRx) - dead, span = iRx - dead,
//     m = mag*127/span, x = 127 + dx*m/dist, y = 127 + dy*m/dist, every
//     division truncating towards zero; (x, y) goes to (iLx, iLy) for the left
//     stick and to (iRightX, iRightY) for the right one. When a LEFT stick is not
//     centred, iStickDigitalPct > 0 and dist*100 >= iRx*iStickDigitalPct, d-pad
//     bits are pressed as well, by the Dpad rule above without its dead-zone and
//     four-way tests; otherwise the stick presses no d-pad bits and its
//     classification is "nothing". The right stick never presses d-pad bits.
//   Action: presses nothing. See VpadPointerUp.
//
// Stick click. Each side (0 left, 1 right) is armed or not. When a pointer that
// captured a stick by VpadPointerDown is lifted, its side becomes armed, with the
// time of the lift, if the touch did not click, lasted no longer than
// KVpadTapMs (lift time - iDownMs, by the time rule at the top of this file) and
// its last position is no further than iRx*KVpadTapSlopPct/100 from the point it
// went down at ("dist <= r" as defined above; a quick flick is not a tap, a tap
// anywhere on the stick is);
// otherwise that side is disarmed. When VpadPointerDown captures a stick whose
// side is armed, iStickClickMs is above 0 and no more than iStickClickMs passed
// since the arming lift, the touch clicks: until it is lifted the pointer presses
// EVpadL3 (left) or EVpadR3 (right) on top of whatever the Stick rule gives, from
// this very call on. Every capture of a stick by VpadPointerDown disarms its side,
// click or not; nothing else does, so a tap stays armed across touches of other
// controls. A stick captured by second-finger inference never clicks, never
// arms and leaves the arming alone, also after pointer 0 takes it over: pointer 0
// then presses neither EVpadL3 nor EVpadR3, whatever it pressed before. The capture
// pointer 0 drops in that takeover is not a lift: it neither arms nor disarms.
// iDownMs of a pointer holding an inferred capture has no meaning. A consequence
// on a single-touch panel: a tap whose finger leaves while a second one is
// inferred ends as a takeover and does not arm.
//
// After every call iBits is the OR over all pointers, bits that turned on are added
// to iEdges, and the axes of a stick side are KVpadStickCentre unless a pointer
// holds a stick of that side. Two pointers can hold sticks of one side only through
// second-finger inference, whose search does not look at sides; the axes are then
// those of the higher-numbered pointer.
void VpadPointerDown(TVpadState &aState, const TVpadLayout &aLayout, int aId,
	int aX, int aY, unsigned aMs);

// The finger moved. Ignored for a pointer that is not down.
//
// Second-finger inference, only when aLayout.iDualJumpPx > 0 and aId == 0, for a
// resistive panel that reports one point: when a second finger lands, the reported
// point jumps to about the middle between the two. Slot 1 is used internally, so
// while iDualJumpPx > 0 all three pointer calls ignore every aId other than 0.
//   Not yet active, pointer 0 has captured a control: let L be its last position.
//     If the new point is at least iDualJumpPx from L and aMs - (last event time)
//     <= KVpadDualMaxMs, take P2 = (2*aX - L.x, 2*aY - L.y), each coordinate
//     clamped to -8192..8191. Among controls of kind
//     Dpad, Stick or Button that are not the captured control and do not share its
//     non-zero group, whose LEASH zone contains P2, pick the smallest normalised
//     distance (measured against the leash zone). If there is one, inference
//     becomes active: pointer 0 stays at L and keeps what it presses, (iDualX,
//     iDualY) = L, and slot 1 goes down capturing that control at P2. A Button
//     captured this way starts with its own iBits pressed and itself remembered,
//     and from the next event on follows the Button rule; a d-pad or stick is
//     evaluated at P2 at once. If there is none, this is an ordinary move.
//   Active, M = the new point, P2old = slot 1's position, r = iDualJumpPx/3 but
//   not less than 1:
//     dist(M, first finger) <= r: the second finger left. Slot 1 goes
//       up, inference ends, pointer 0 moves to M as an ordinary move.
//     else dist(M, P2old) <= r: the first finger left. Pointer 0 drops
//       its capture and takes over slot 1's capture, stick origin and remembered state; slot 1 is
//       freed; inference ends; pointer 0 then moves to M as an ordinary move.
//     else slot 1 moves to (2*M.x - iDualX, 2*M.y - iDualY), clamped as P2, as an
//       ordinary move; pointer 0 stays at (iDualX, iDualY).
void VpadPointerMove(TVpadState &aState, const TVpadLayout &aLayout, int aId,
	int aX, int aY, unsigned aMs);

// The finger left. Its bits are released. A pointer that captured an Action control
// sets bit iId in iActions if its last position is inside that control's leash
// zone. Lifting pointer 0 while inference is active also lifts slot 1.
void VpadPointerUp(TVpadState &aState, const TVpadLayout &aLayout, int aId, unsigned aMs);

// Return and clear the accumulated press edges (for tactile feedback) and actions.
unsigned VpadTakeEdges(TVpadState &aState);
unsigned VpadTakeActions(TVpadState &aState);

// Largest r with r*r <= aV, for aV >= 0.
int VpadIsqrt(int aV);

// ============================= Kinetic scroller ==============================
//
// One axis of a scrolling list. The caller feeds pointer events along that axis and
// calls KScrollStep once per drawn frame; KScrollPos is the offset of the view
// into the content. The same object animates jumps made from the keypad, so that
// touch and key navigation move the list the same way.

enum TKScrollState
	{
	EKsIdle = 0,
	EKsPressed = 1,     // finger down, has not moved beyond the slop yet
	EKsDragging = 2,
	EKsFlinging = 3,    // coasting after a flick
	EKsSettling = 4     // gliding to iTarget8: spring-back or KScrollTo
	};

enum TKScrollEvent { EKsNone = 0, EKsTap = 1, EKsLongPress = 2 };

enum
	{
	KKsSamples = 8,
	KKsVelocityWindowMs = 100,
	KKsLongPressMs = 500,
	KKsMaxStepMs = 250,
	KKsFrictionLossQ16 = 201, // speed lost per millisecond: 1 - exp(-1/325), in 1/65536
	KKsSettleShift = 6,       // per millisecond the gap to the target shrinks by 1/64
	KKsMaxOverPx = 8192       // the offset is never further than this beyond an end
	};

// Positions are 24.8 fixed point (1/256 px) so that a list tens of thousands of
// pixels long still fits in 32 bits; velocities are px/s in the same format.
struct TKScroll
	{
	int iView;              // visible length, px
	int iContent;           // content length, px
	int iPos8;              // offset, 24.8 px; outside [0, max] only while over-scrolled
	int iVel8;              // px per second, 24.8; positive moves towards larger offsets
	int iTarget8;           // EKsSettling: where to
	int iSlop;              // px a finger may wander and still be a tap
	int iMinFling;          // px/s; a slower release does not coast
	int iMaxFling;          // px/s; faster releases are clamped
	int iFrictionLossQ16;   // KKsFrictionLossQ16 unless tuned
	unsigned char iState;   // TKScrollState
	unsigned char iCaught;  // this press stopped a moving list: it cannot be a tap
	unsigned char iLongSent;
	unsigned char iCount;   // valid samples
	unsigned char iHead;    // index of the newest sample
	unsigned char iPad[3];
	int iDownCoord;         // finger coordinate at KScrollDown
	int iAnchorCoord;       // finger coordinate the drag is measured from
	int iAnchorPos8;        // raw (un-rubber-banded) offset at that moment
	unsigned iDownMs;
	unsigned iStepMs;       // time KScrollStep has advanced to
	int iSampleCoord[KKsSamples];
	unsigned iSampleMs[KKsSamples];
	};

// max offset = iContent - iView, or 0 when the content is shorter than the view.
//
// Idle at offset 0. iSlop = 2 mm, iMinFling = 30 mm/s, iMaxFling = 800 mm/s, each
// converted with aUmPerPx (micrometres per pixel) as (mm*1000 + aUmPerPx/2) /
// aUmPerPx and not less than 1. iFrictionLossQ16 = KKsFrictionLossQ16.
//
// Range. aUmPerPx below 20 is taken as 20. View and content sizes are clamped to
// 0..4000000 px here and in KScrollSetExtent; finger coordinates are clamped to
// -8192..8191 in KScrollDown and KScrollMove. The offset never leaves
// [-KKsMaxOverPx, max + KKsMaxOverPx] pixels: KScrollMove and KScrollSetExtent
// clamp it there. One drag that starts inside [0, max] cannot over-scroll that
// far, so the clamp shows only when drags pile up with no KScrollStep between
// them or the content shrinks under a list that is not idle. Fields a caller may
// tune are taken within: iMinFling 1..4000000, iMaxFling 0..4000000,
// iFrictionLossQ16 1..32768. With that everything fits 32 bits.
void KScrollInit(TKScroll &aS, int aViewPx, int aContentPx, int aUmPerPx);

// New sizes (rotation, list reloaded). An idle scroller is clamped into range at
// once. In any other state the offset is clamped to within KKsMaxOverPx of the
// new range, motion continues and ends in range by the usual rules, and the
// target of an EKsSettling scroller is clamped into the new range.
void KScrollSetExtent(TKScroll &aS, int aViewPx, int aContentPx);

// Finger down at aCoord. Any motion stops where it is; iCaught is set if the state
// was EKsFlinging or EKsSettling, cleared otherwise. State becomes EKsPressed. The
// sample history is restarted with this point.
void KScrollDown(TKScroll &aS, int aCoord, unsigned aMs);

// Finger moved. Ignored unless EKsPressed or EKsDragging. The point is appended to
// the sample history (the oldest of KKsSamples is overwritten).
//   EKsPressed: while |aCoord - iDownCoord| <= iSlop nothing moves. On the first
//     event beyond the slop the state becomes EKsDragging and the anchor is taken at
//     THIS event (anchor coordinate = aCoord, anchor position = the raw value of the
//     current offset), so the list does not jump by the slop.
//   EKsDragging: raw = anchor position + (anchor coordinate - aCoord) pixels.
//     Inside [0, max] the offset is raw. Beyond an end the excess is halved:
//     offset = raw/2 below 0 and max + (raw - max)/2 above max, computed in 24.8
//     with the division truncating towards zero, and then clamped to
//     [-KKsMaxOverPx, max + KKsMaxOverPx] pixels.
//   Raw value of an offset p: p inside [0, max]; 2*p below 0; max + 2*(p - max)
//     above max. This keeps the offset continuous when a drag starts while the
//     list is over-scrolled.
void KScrollMove(TKScroll &aS, int aCoord, unsigned aMs);

// Finger up. Returns EKsTap when the state was EKsPressed, the press was not
// iCaught, no long press was reported and aMs - iDownMs < KKsLongPressMs;
// otherwise EKsNone. Ignored (EKsNone) in states other than EKsPressed/EKsDragging.
//   From EKsPressed: becomes EKsIdle, or EKsSettling to the nearest end if the
//     offset is out of range.
//   From EKsDragging: if the offset is out of range, EKsSettling to the nearest end.
//     Otherwise the release velocity is taken from the history: let N be the newest
//     sample and O the oldest sample with N.ms - O.ms <= KKsVelocityWindowMs. If
//     O is N, or N.ms == O.ms, or aMs - N.ms > KKsVelocityWindowMs (the finger
//     rested before leaving), v = 0; else v = (O.coord - N.coord) * 1000 /
//     (N.ms - O.ms) px/s, clamped to +-iMaxFling. |v| >= iMinFling starts
//     EKsFlinging with iVel8 = v * 256, anything slower ends in EKsIdle.
// iStepMs is set to aMs so that coasting is timed from the release.
int KScrollUp(TKScroll &aS, unsigned aMs);

// Advances time to aMs and returns EKsLongPress once per press when the state is
// EKsPressed, not iCaught and aMs - iDownMs >= KKsLongPressMs; otherwise EKsNone.
// Motion is integrated one millisecond at a time for dt = aMs - iStepMs
// milliseconds (dt above KKsMaxStepMs is treated as KKsMaxStepMs). As long as no
// single gap exceeds that cap the result does not depend on how the interval is cut
// into calls: KScrollStep(t1) then KScrollStep(t2) leaves exactly the state of a
// single KScrollStep(t2).
//   EKsFlinging, per millisecond: iPos8 += iVel8 / 1000 (truncating towards zero);
//     then the speed drops by (|iVel8| * iFrictionLossQ16) >> 16 but not by less
//     than 1 (so that it always reaches the threshold below), the sign kept. If
//     the offset has left [0, max] it is set to that end, the velocity to 0 and the
//     state to EKsIdle. Otherwise, when |iVel8| < iMinFling * 64 (a quarter of the
//     fling threshold) the state becomes EKsIdle, the velocity 0, and iPos8 is
//     rounded to the nearest whole pixel, a half going up.
//   EKsSettling, per millisecond: d = iTarget8 - iPos8. If |d| < 128 then iPos8 =
//     iTarget8, the velocity is 0 and the state becomes EKsIdle; otherwise iPos8
//     moves towards the target by |d| >> KKsSettleShift.
// In every other state only iStepMs advances.
int KScrollStep(TKScroll &aS, unsigned aMs);

// Offset in whole pixels: floor((iPos8 + 128) / 256), also for negative offsets.
int KScrollPos(const TKScroll &aS);

// Non-zero in every state except EKsIdle and EKsPressed (redraw needed).
int KScrollActive(const TKScroll &aS);

// Go to aTargetPx (clamped to [0, max]). Ignored while a finger is down (EKsPressed
// or EKsDragging). With aAnimate the state becomes EKsSettling and iStepMs = aMs;
// without it the offset is set at once and the state is EKsIdle.
void KScrollTo(TKScroll &aS, int aTargetPx, int aAnimate, unsigned aMs);

// Scrolls the least distance that brings content rows [aTop, aBottom) into view;
// both are clamped to 0..4000000 on entry. The view is taken to start at base = iTarget8 / 256 when EKsSettling and at
// KScrollPos otherwise: aTop < base goes to aTop; else aBottom > base + iView goes
// to aBottom - iView, or to aTop when the span is taller than the view; else there
// is no effect. The move itself is KScrollTo, with its rules.
void KScrollEnsureVisible(TKScroll &aS, int aTop, int aBottom, int aAnimate, unsigned aMs);

#endif
