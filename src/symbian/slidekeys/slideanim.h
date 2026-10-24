// slideanim.h - window server anim DLL that renames the scan codes of the dual-slider media
// keys before the firmware's own raw event handler swallows them.
//
// The N95 media keys reach the window server as raw events, but a ROM handler registered
// ahead of every application consumes them, so neither CaptureKey nor a RemCon target
// sees them (keyprobe runs of 2026-10-24). Magic Keys gets past that handler by moving
// its own anim to the front of the window server's private handler list; this DLL does
// the same, and like Magic Keys' KeyMap it only rewrites the scan code in place and never
// consumes the event, so the renamed key goes on through the ordinary key path.
//
// The client protocol (opcodes and argument layout) is shared with app_symbian.cpp.

#ifndef SLIDEANIM_H
#define SLIDEANIM_H

#include <w32adll.h>
#include <e32keys.h>
#include <e32base.h>

enum TSlideCommand
	{
	// Start receiving raw events and move this handler to the front of the list.
	// Returns KErrNone, or KErrNotFound when the list was not found: the handler is then
	// left out of the list altogether, since at the back it would rename nothing.
	ESlideCmdAttach = 1,
	// Leave the handler list. Always KErrNone.
	ESlideCmdDetach = 2,
	// Replace the rename table: TInt32 count, then count pairs of TInt32 (from, to).
	// KErrArgument for a count outside 0..KSlideMapMax.
	ESlideCmdSetMap = 3,
	// Last raw key-down scan code seen while focused (before renaming) in the low 16 bits
	// and the number of such key-downs, modulo 0x8000, above them. Lets the client log
	// keys that never arrive as key events.
	ESlideCmdGetLast = 4
	};

const TInt KSlideMapMax = 8;

class CPs1SlideDll : public CAnimDll
	{
public:
	virtual CAnim *CreateInstanceL(TInt aType);
	};

class CPs1SlideAnim : public CWindowAnim
	{
public:
	CPs1SlideAnim();
	virtual ~CPs1SlideAnim();

	// From CWindowAnim
	virtual void ConstructL(TAny *aArgs, TBool aHasFocus);
	virtual void Redraw();
	virtual void FocusChanged(TBool aState);
	virtual void Animate(TDateTime *aDateTime);

	// From CAnim
	virtual void Command(TInt aOpcode, TAny *aArgs);
	virtual TInt CommandReplyL(TInt aOpcode, TAny *aArgs);

	// From MEventHandler
	virtual TBool OfferRawEvent(const TRawEvent &aRawEvent);

private:
	TInt Attach();
	void Detach();
	TInt SetMap(const TAny *aArgs);
	CArrayPtrFlat<MEventHandler> *FindHandlerList(TInt &aIndex) const;

private:
	TBool iAttached;
	TBool iHasFocus;
	TInt iMapCount;
	TInt iFrom[KSlideMapMax];
	TInt iTo[KSlideMapMax];
	// A key pressed while focused is renamed on its release too, wherever the focus is
	// by then: otherwise the client would see a press that never ends.
	TBool iDown[KSlideMapMax];
	TInt iLastScan;
	TInt iDowns;
	};

#endif // SLIDEANIM_H
