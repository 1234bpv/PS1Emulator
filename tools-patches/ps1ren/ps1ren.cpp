// ps1ren.cpp - renames files on the phone's card from a list, because the deploy daemon
// has no rename and moving disc images there and back over WiFi takes hours.
//
// E:\PS1\ren.txt holds one rename per line: old path, a tab, new path (ASCII).
// Each result goes to E:\PS1\ren.log as "<error code> <old> -> <new>"; 0 is success.
#include <e32base.h>
#include <f32file.h>

_LIT(KList, "E:\\PS1\\ren.txt");
_LIT(KLog, "E:\\PS1\\ren.log");

static void RunL()
	{
	RFs fs;
	User::LeaveIfError(fs.Connect());
	CleanupClosePushL(fs);
	RFile in;
	User::LeaveIfError(in.Open(fs, KList, EFileRead | EFileShareReadersOnly));
	CleanupClosePushL(in);
	TInt size = 0;
	User::LeaveIfError(in.Size(size));
	HBufC8 *buf = HBufC8::NewLC(size);
	TPtr8 p = buf->Des();
	User::LeaveIfError(in.Read(p));
	RFile log;
	User::LeaveIfError(log.Replace(fs, KLog, EFileWrite));
	CleanupClosePushL(log);

	TPtrC8 rest(p);
	while (rest.Length() > 0)
		{
		TInt nl = rest.Locate('\n');
		TPtrC8 line = (nl >= 0) ? rest.Left(nl) : rest;
		rest.Set((nl >= 0) ? rest.Mid(nl + 1) : TPtrC8());
		if (line.Length() && line[line.Length() - 1] == '\r')
			line.Set(line.Left(line.Length() - 1));
		const TInt tab = line.Locate('\t');
		if (tab <= 0)
			continue;
		TFileName from, to;
		from.Copy(line.Left(tab));
		to.Copy(line.Mid(tab + 1));
		const TInt err = fs.Rename(from, to);
		TBuf8<600> out;
		out.AppendNum(err);
		out.Append(' ');
		out.Append(line.Left(tab));
		out.Append(_L8(" -> "));
		out.Append(line.Mid(tab + 1));
		out.Append(_L8("\r\n"));
		log.Write(out);
		}
	CleanupStack::PopAndDestroy(4, &fs);
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup *cleanup = CTrapCleanup::New();
	TRAPD(err, RunL());
	delete cleanup;
	return err;
	}
