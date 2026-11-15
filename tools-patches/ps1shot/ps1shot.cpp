// ps1shot.cpp - screen capture through the window server, for phones where the
// deploy daemon's frame buffer screenshot comes back blank (Belle composes on the GPU).
//
// Writes E:\tester\ps1shot.bin: "PS1SHOT <w> <h>\n" padded to 32 bytes, then
// w*h pixels of 4 bytes each (B, G, R, unused), top row first.
#include <e32base.h>
#include <f32file.h>
#include <w32std.h>
#include <fbs.h>

_LIT(KOut, "E:\\tester\\ps1shot.bin");

static void RunL()
	{
	RWsSession ws;
	User::LeaveIfError(ws.Connect());
	CleanupClosePushL(ws);
	CWsScreenDevice *dev = new (ELeave) CWsScreenDevice(ws);
	CleanupStack::PushL(dev);
	User::LeaveIfError(dev->Construct());
	const TSize size = dev->SizeInPixels();
	CFbsBitmap *bmp = new (ELeave) CFbsBitmap;
	CleanupStack::PushL(bmp);
	User::LeaveIfError(bmp->Create(size, EColor16MU));
	User::LeaveIfError(dev->CopyScreenToBitmap(bmp));

	RFs fs;
	User::LeaveIfError(fs.Connect());
	CleanupClosePushL(fs);
	RFile out;
	User::LeaveIfError(out.Replace(fs, KOut, EFileWrite));
	CleanupClosePushL(out);
	TBuf8<32> head;
	head.Format(_L8("PS1SHOT %d %d\n"), size.iWidth, size.iHeight);
	head.AppendFill(' ', 32 - head.Length());
	User::LeaveIfError(out.Write(head));
	bmp->LockHeap();
	const TUint8 *base = (const TUint8 *)bmp->DataAddress();
	const TInt stride = bmp->DataStride();
	TInt err = KErrNone;
	for (TInt y = 0; y < size.iHeight && err == KErrNone; y++)
		err = out.Write(TPtrC8(base + y * stride, size.iWidth * 4));
	bmp->UnlockHeap();
	User::LeaveIfError(err);
	CleanupStack::PopAndDestroy(5);
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup *cleanup = CTrapCleanup::New();
	if (!cleanup)
		return KErrNoMemory;
	TRAPD(err, RunL());
	delete cleanup;
	return err;
	}
