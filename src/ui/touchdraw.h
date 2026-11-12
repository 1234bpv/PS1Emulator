// Panel drawing for the on-screen gamepad, portable half: the same rules as touchcore.h
// (C++98, no heap, no STL, no float, no platform headers). The look of a layout is
// prepared once as lists of coverage spans (VpadArtBuild); each frame only those spans
// are composited over the frame (VpadArtPaint). The buffers belong to the caller.
//
// Look: light strokes with a dark halo about one pixel wide on each side, so every stroke
// reads on white, black and busy backgrounds. Edges are anti-aliased: coverage 0..255 is
// mixed with the frame. Ink is only ever white or black.
#ifndef PS1_TOUCHDRAW_H
#define PS1_TOUCHDRAW_H

#include "touchcore.h"

// What a part of a control is. It decides how the pixels of the part react to the state.
enum TVpadPart
	{
	EVpadPartBody = 0,     // outline, glyph or label; pressed: black ink on a white fill
	EVpadPartUp = 1,       // d-pad arrows: 1 up, 2 right, 3 down, 4 left; held: black
	EVpadPartKnob = 5,     // stick head: moved by the axes, see VpadArtPaint
	EVpadPartFill = 6,     // pressed fill of a button or the menu button
	EVpadPartArm = 7       // pressed fill of a d-pad arm: 7 up, 8 right, 9 down, 10 left
	};

// One run of pixels: iLen coverage bytes starting at iOff in TVpadArt::iAlpha, left to
// right, all in the same row iY, starting at column iX.
struct TVpadSpan
	{
	short iX, iY;
	unsigned short iLen;
	unsigned iOff;
	unsigned char iPart;   // TVpadPart value
	};

// Spans of one layout. Control i owns the spans [iFirst[i], iFirst[i + 1]): first its
// fill spans, then its dark halo spans from iDark[i], then its light ink from iLight[i].
struct TVpadArt
	{
	int iCount;                      // controls prepared
	int iSpanCount;                  // spans in use
	int iAlphaCount;                 // coverage bytes in use
	int iSpanCap;                    // capacity of iSpan, in spans
	int iAlphaCap;                   // capacity of iAlpha, in bytes
	TVpadSpan *iSpan;                // caller-owned
	unsigned char *iAlpha;           // caller-owned
	int iFirst[KVpadMaxControls + 1];
	int iDark[KVpadMaxControls];
	int iLight[KVpadMaxControls];
	};

// The frame the panel is composited over. Every blit rewrites the game picture
// (iPicX, iPicY, iPicW, iPicH); the bars around it are not rewritten, so the panel's
// pixels there are redrawn only when the caller says they changed.
struct TVpadCanvas
	{
	unsigned char *iDst;             // frame bitmap
	int iStride;                     // bytes per row
	int iPixB;                       // 2 (RGB565) or 4 (XRGB8888)
	int iW, iH;                      // frame size in pixels
	int iPicX, iPicY, iPicW, iPicH;  // the game picture; the rest of the frame is bars
	};

// Pixels around a control's box that its ink can reach. VpadArtClearBars clears this box, so
// ink outside it would survive a repaint; the render test checks that the ink stays inside.
enum { KVpadArtMargin = 4 };

// Prepares the spans of aLayout for a pixel pitch of aUmPerPx micrometres. Returns 0 when
// aArt's buffers are too small, or when the ink box of a control is wider than 512 pixels
// (a control of about 250 px in radius or more); aArt is then not to be painted. Uses static
// scratch memory, so it is not reentrant.
int VpadArtBuild(TVpadArt &aArt, const TVpadLayout &aLayout, int aUmPerPx);

// Half sizes of the box that VpadArtClearBars clears for a control, margin included. Returns
// 0 for a control that is not drawn.
int VpadArtBox(const TVpadControl &aC, int &aHalfW, int &aHalfH);

// The buffer sizes VpadArtBuild needs for the same arguments, in spans and coverage bytes.
// Writes nothing; returns 0 under the same conditions as VpadArtBuild.
int VpadArtMeasure(const TVpadLayout &aLayout, int aUmPerPx, int &aSpans, int &aAlpha);

// Blackens the bars inside the boxes of the controls. Called before a VpadArtPaint whose
// aBarsToo is non-zero, since partly transparent ink must not build up over earlier ink.
void VpadArtClearBars(const TVpadLayout &aLayout, const TVpadCanvas &aCanvas);

// Composites the panel over the frame. aOpacityPct, 10..100, scales the ink of untouched
// parts; a pressed control, its held arrow or arm, and the fill under it are always
// drawn at full strength. The knob is placed from the stick axes of aState (127 is the
// centre). The game picture is always painted; the bars only when aBarsToo is non-zero.
void VpadArtPaint(const TVpadArt &aArt, const TVpadLayout &aLayout, const TVpadState &aState,
	const TVpadCanvas &aCanvas, int aOpacityPct, int aBarsToo);

#endif
