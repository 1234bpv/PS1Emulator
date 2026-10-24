// slideanim.cpp - see slideanim.h. Runs inside the window server: a fault here restarts
// the phone, so nothing below dereferences memory it has not range-checked first.
#include "slideanim.h"

CAnim *CPs1SlideDll::CreateInstanceL(TInt /* aType */)
	{
	return new (ELeave) CPs1SlideAnim();
	}

EXPORT_C CAnimDll *CreateCAnimDllL()
	{
	return new (ELeave) CPs1SlideDll();
	}

CPs1SlideAnim::CPs1SlideAnim()
	{
	// CBase zero-fills: detached, empty table, nothing pressed.
	}

CPs1SlideAnim::~CPs1SlideAnim()
	{
	Detach();
	}

void CPs1SlideAnim::ConstructL(TAny * /* aArgs */, TBool aHasFocus)
	{
	iHasFocus = aHasFocus;
	}

void CPs1SlideAnim::Redraw()
	{
	}

// Renaming follows the focus of the emulator's window: in the background the media keys
// belong to the music player again.
void CPs1SlideAnim::FocusChanged(TBool aState)
	{
	iHasFocus = aState;
	}

void CPs1SlideAnim::Animate(TDateTime * /* aDateTime */)
	{
	}

void CPs1SlideAnim::Command(TInt aOpcode, TAny *aArgs)
	{
	TRAP_IGNORE(CommandReplyL(aOpcode, aArgs));
	}

TInt CPs1SlideAnim::CommandReplyL(TInt aOpcode, TAny *aArgs)
	{
	switch (aOpcode)
		{
		case ESlideCmdAttach:
			return Attach();
		case ESlideCmdDetach:
			Detach();
			return KErrNone;
		case ESlideCmdSetMap:
			return SetMap(aArgs);
		case ESlideCmdGetLast:
			return ((iDowns & 0x7fff) << 16) | (iLastScan & 0xffff);
		default:
			return KErrNotSupported;
		}
	}

// The window server keeps its raw event handlers in a CArrayPtrFlat<MEventHandler> on its
// own heap (Magic Keys finds it by RTTI name; GetRawEvents has just appended this anim to
// it). The array is recognised by its fields alone, and its CBufFlat by its fields too,
// before anything is called through either: a false match must not reach a virtual call.
//   CArrayFixBase: vptr, iCount, iGranularity, iLength, iCreateRep, iBase
//   CBufFlat:      vptr, iSize, iExpandSize, iMaxSize, iPtr
CArrayPtrFlat<MEventHandler> *CPs1SlideAnim::FindHandlerList(TInt &aIndex) const
	{
	RHeap &heap = User::Heap();
	const TUint32 lo = (TUint32)heap.Base();
	const TUint32 hi = lo + (TUint32)heap.Size();
	const TUint32 me = (TUint32)static_cast<const MEventHandler *>(this);
	if (hi <= lo + 64)
		return NULL;

	// Bounds are written as hi - n, never x + n, so a wild value cannot wrap past them.
	for (TUint32 a = (lo + 3) & ~3u; a <= hi - 24; a += 4)
		{
		const TUint32 *arr = (const TUint32 *)a;
		const TInt count = (TInt)arr[1];
		const TInt gran = (TInt)arr[2];
		if (arr[3] != sizeof(TAny *) || count < 1 || count > 64 || gran < 1 || gran > 64)
			continue;
		const TUint32 buf = arr[5];
		if ((buf & 3) || buf < lo || buf > hi - 20)
			continue;
		const TUint32 *b = (const TUint32 *)buf;
		if (b[1] != (TUint32)count * 4 || b[2] != (TUint32)gran * 4 || b[3] < b[1])
			continue;
		const TUint32 data = b[4];
		if ((data & 3) || data < lo || b[1] > hi - lo || data > hi - b[1])
			continue;
		const TUint32 *p = (const TUint32 *)data;
		for (TInt i = 0; i < count; i++)
			if (p[i] == me)
				{
				aIndex = i;
				return (CArrayPtrFlat<MEventHandler> *)a;
				}
		}
	return NULL;
	}

TInt CPs1SlideAnim::Attach()
	{
	if (iAttached)
		return KErrNone;
	iFunctions->GetRawEvents(ETrue);
	TInt idx = -1;
	CArrayPtrFlat<MEventHandler> *list = FindHandlerList(idx);
	if (!list)
		{
		iFunctions->GetRawEvents(EFalse);
		return KErrNotFound;
		}
	if (idx > 0)
		{
		// Insert before deleting: if the insert cannot allocate, the handler is still in
		// the list once, where the window server will look for it on removal.
		MEventHandler *self = static_cast<MEventHandler *>(this);
		TRAPD(err, list->InsertL(0, self));
		if (err != KErrNone)
			{
			iFunctions->GetRawEvents(EFalse);
			return err;
			}
		list->Delete(idx + 1);
		}
	iAttached = ETrue;
	return KErrNone;
	}

// The window server removes a handler by pointer, so having moved it does not matter;
// a copy it somehow left behind is taken out by hand, since it would outlive this object.
void CPs1SlideAnim::Detach()
	{
	if (!iAttached)
		return;
	iAttached = EFalse;
	iFunctions->GetRawEvents(EFalse);
	TInt idx = -1;
	CArrayPtrFlat<MEventHandler> *list = FindHandlerList(idx);
	if (list)
		list->Delete(idx);
	for (TInt i = 0; i < KSlideMapMax; i++)
		iDown[i] = EFalse;
	}

TInt CPs1SlideAnim::SetMap(const TAny *aArgs)
	{
	if (!aArgs)
		return KErrArgument;
	const TInt32 *a = (const TInt32 *)aArgs;
	const TInt n = a[0];
	if (n < 0 || n > KSlideMapMax)
		return KErrArgument;
	for (TInt i = 0; i < n; i++)
		{
		iFrom[i] = a[1 + 2 * i];
		iTo[i] = a[2 + 2 * i];
		iDown[i] = EFalse;
		}
	iMapCount = n;
	return KErrNone;
	}

TBool CPs1SlideAnim::OfferRawEvent(const TRawEvent &aRawEvent)
	{
	const TRawEvent::TType type = aRawEvent.Type();
	if (type != TRawEvent::EKeyDown && type != TRawEvent::EKeyUp)
		return EFalse;
	const TInt sc = aRawEvent.ScanCode();
	// Only while focused: this sees every application's keys, PIN digits included.
	if (type == TRawEvent::EKeyDown && iHasFocus)
		{
		iLastScan = sc;
		iDowns++;
		}
	for (TInt i = 0; i < iMapCount; i++)
		{
		if (iFrom[i] != sc)
			continue;
		const TBool down = (type == TRawEvent::EKeyDown);
		if (down ? iHasFocus : iDown[i])
			{
			iDown[i] = down;
			// The event the window server goes on to process is this very object, which
			// is how KeyMap renames keys too; the const is only the interface's promise.
			const_cast<TRawEvent &>(aRawEvent).Set(type, iTo[i]);
			}
		break;
		}
	return EFalse;
	}
