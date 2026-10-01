// Game import: .cue parsing, disc image building into PBP (ECC zeroed), or copying
// as is. Track geometry deliberately mirrors parsecue() in libpcsxcore/cdriso.c -
// TOC in PBP must produce the exact same ti[] table, otherwise CD-DA audio games
// after conversion would look for tracks in the wrong places.

#include <e32base.h>
#include <e32math.h>
#include <f32file.h>
#include <badesca.h>
#include <hal.h>
#include <string.h>

#include "imgconv.h"
#include "zlib.h"

extern "C" void ps1Log(const char *aType, const char *aMsg);
extern "C" void ps1LogInt(const char *aType, const char *aMsg, int aVal);

const TInt KSector   = 2352;
const TInt KPbpBlk   = 16;                    // sectors per PBP block (block_shift=4)
const TInt KBlkBytes = KSector * KPbpBlk;     // 37632
const TInt KPsar     = 0x28;
const TInt KDataOffs = KPsar + 0x100000;
const TInt KMaxTrk   = 100;
// (0x100000-0x4000)/32 index entries - format hard ceiling, not ours.
const TInt KMaxIndex = (0x100000 - 0x4000) / 32;

// --- Volume: directory on disk ----------------------------------------------

class CFileSrc : public CBase, public MConvSrc
	{
public:
	static TInt New(RFs &aFs, const TDesC &aPath, MConvSrc *&aOut);
	~CFileSrc() { iFile.Close(); }
	TInt Read(TUint8 *aBuf, TInt aLen);
	TInt Skip(TInt aBytes);
	TInt Size() const { return iSize; }
private:
	RFile iFile;
	TInt iSize;
	};

TInt CFileSrc::New(RFs &aFs, const TDesC &aPath, MConvSrc *&aOut)
	{
	CFileSrc *self = new CFileSrc;
	if (!self)
		return KErrNoMemory;
	TInt r = self->iFile.Open(aFs, aPath, EFileRead | EFileShareReadersOnly);
	if (r != KErrNone)
		{
		delete self;
		return r;
		}
	self->iFile.Size(self->iSize);
	aOut = self;
	return KErrNone;
	}

TInt CFileSrc::Read(TUint8 *aBuf, TInt aLen)
	{
	TPtr8 p(aBuf, aLen);
	const TInt r = iFile.Read(p, aLen);
	return (r == KErrNone) ? p.Length() : r;
	}

TInt CFileSrc::Skip(TInt aBytes)
	{
	TInt pos = aBytes;
	return iFile.Seek(ESeekCurrent, pos);
	}

class CDirVol : public CBase, public MConvVol
	{
public:
	static TInt New(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);
	~CDirVol() { delete iNames; }
	const MDesCArray &Entries() const { return *iNames; }
	TInt Open(const TDesC &aName, MConvSrc *&aOut);
	TInt Size(const TDesC &aName, TInt &aOut);
private:
	TInt Full(const TDesC &aName, TDes &aOut) const;
	RFs *iFs;
	TFileName iDir;
	CDesCArray *iNames;
	};

TInt CDirVol::New(RFs &aFs, const TDesC &aPath, MConvVol *&aOut)
	{
	CDirVol *self = new CDirVol;
	if (!self)
		return KErrNoMemory;
	self->iFs = &aFs;
	const TInt s = aPath.LocateReverse('\\');
	self->iDir = aPath.Left(s + 1);
	self->iNames = new CDesCArrayFlat(2);
	if (!self->iNames)
		{
		delete self;
		return KErrNoMemory;
		}
	TRAPD(err, self->iNames->AppendL(aPath.Mid(s + 1)));
	if (err != KErrNone)
		{
		delete self;
		return err;
		}
	aOut = self;
	return KErrNone;
	}

TInt CDirVol::Full(const TDesC &aName, TDes &aOut) const
	{
	if (iDir.Length() + aName.Length() >= aOut.MaxLength())
		return KErrBadName;
	aOut = iDir;
	aOut.Append(aName);
	return KErrNone;
	}

TInt CDirVol::Open(const TDesC &aName, MConvSrc *&aOut)
	{
	TFileName p;
	const TInt r = Full(aName, p);
	return (r != KErrNone) ? r : CFileSrc::New(*iFs, p, aOut);
	}

TInt CDirVol::Size(const TDesC &aName, TInt &aOut)
	{
	TFileName p;
	TInt r = Full(aName, p);
	if (r != KErrNone)
		return r;
	TEntry e;
	r = iFs->Entry(p, e);
	if (r == KErrNone)
		aOut = e.iSize;
	return r;
	}

// --- Name recognition -------------------------------------------------------

static TBool extIs(const TDesC &aName, const TDesC &aExt)
	{
	if (aName.Length() < aExt.Length())
		return EFalse;
	TBuf<8> e;
	e.Copy(aName.Right(aExt.Length()));
	e.LowerCase();
	return e == aExt;
	}

TBool ConvIsArchiveName(const TDesC &aName)
	{
	return extIs(aName, _L(".zip")) || extIs(aName, _L(".7z")) ||
	       extIs(aName, _L(".rar"));
	}

static TBool isRawImageName(const TDesC &aName)
	{
	return extIs(aName, _L(".bin")) || extIs(aName, _L(".img")) ||
	       extIs(aName, _L(".iso"));
	}

// Creates volume. Archives are handled by archive.cpp, regular files by their parent directory.
TInt ConvOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut)
	{
	if (ConvIsArchiveName(aPath))
		return ArcOpenVol(aFs, aPath, aOut);
	return CDirVol::New(aFs, aPath, aOut);
	}

// --- .cue parsing -----------------------------------------------------------

struct TTrk
	{
	TInt iType;        // 0 - data, 1 - audio
	TInt iIdx0, iIdx1; // sectors within track file, -1 if absent
	TInt iPregap, iPostgap;
	TInt iSecSize;
	TInt iFile;        // filename index in file list
	TBool iFirstInFile;
	TInt iStart;       // absolute sector (MSF), like ti[].start
	TInt iLen;
	};

struct TCue
	{
	TInt iCount;
	TTrk iTrk[KMaxTrk];
	CDesCArray *iFiles;
	};

static TInt msf2sec(TInt m, TInt s, TInt f) { return (m * 60 + s) * 75 + f; }

// "mm:ss:ff" -> sectors. Returns -1 if not a timestamp.
static TInt parseMsf(const TDesC8 &aTok)
	{
	if (aTok.Length() < 8 || aTok[2] != ':' || aTok[5] != ':')
		return -1;
	TLex8 lm(aTok.Left(2)), ls(aTok.Mid(3, 2)), lf(aTok.Mid(6, 2));
	TInt m = 0, s = 0, f = 0;
	if (lm.Val(m) != KErrNone || ls.Val(s) != KErrNone || lf.Val(f) != KErrNone)
		return -1;
	return msf2sec(m, s, f);
	}

static void nextTok(const TDesC8 &aLine, TInt &aPos, TPtrC8 &aOut)
	{
	while (aPos < aLine.Length() && (aLine[aPos] == ' ' || aLine[aPos] == '\t'))
		aPos++;
	const TInt st = aPos;
	while (aPos < aLine.Length() && aLine[aPos] != ' ' && aLine[aPos] != '\t')
		aPos++;
	aOut.Set(aLine.Mid(st, aPos - st));
	}

// Quoted name after FILE; without quotes - up to word boundary.
static void quoted(const TDesC8 &aLine, TDes &aOut)
	{
	aOut.Zero();
	const TInt q1 = aLine.Locate('"');
	if (q1 >= 0)
		{
		const TInt q2 = aLine.Mid(q1 + 1).Locate('"');
		if (q2 > 0)
			aOut.Copy(aLine.Mid(q1 + 1, q2).Left(aOut.MaxLength()));
		return;
		}
	TInt pos = 0;
	TPtrC8 t;
	nextTok(aLine, pos, t);          // FILE
	nextTok(aLine, pos, t);
	aOut.Copy(t.Left(aOut.MaxLength()));
	}

static TInt parseCue(const TDesC8 &aTxt, TCue &aCue)
	{
	aCue.iCount = 0;
	// Element 0 is not a track, but a postgap source for track 1: completeCue
	// reads iTrk[i-1] and on i==1 hits this. Without this line, uninitialized garbage
	// went into the first track's pregap, iStart became huge, and
	// converter filled the image with silence until storage ran out.
	memset(&aCue.iTrk[0], 0, sizeof(aCue.iTrk[0]));
	TBool pendingFile = EFalse;
	TInt pos = 0;
	while (pos < aTxt.Length())
		{
		const TPtrC8 rest = aTxt.Mid(pos);
		const TInt nl = rest.Locate('\n');
		TPtrC8 line = (nl < 0) ? rest : rest.Left(nl);
		pos += (nl < 0) ? rest.Length() : nl + 1;
		while (line.Length() && (line[line.Length() - 1] == '\r' ||
		                         line[line.Length() - 1] == ' '))
			line.Set(line.Left(line.Length() - 1));

		TInt lp = 0;
		TPtrC8 tok;
		nextTok(line, lp, tok);
		if (!tok.Length())
			continue;
		TBuf8<16> kw;
		kw.Copy(tok.Left(16));
		kw.UpperCase();

		if (kw == _L8("FILE"))
			{
			TFileName name;
			quoted(line, name);
			if (!name.Length())
				continue;
			// Ignore paths inside .cue: track files reside alongside it,
			// and absolute paths from foreign machines do not exist on device anyway.
			TInt s = name.LocateReverse('\\');
			if (s < 0) s = name.LocateReverse('/');
			if (s >= 0)
				name.Delete(0, s + 1);
			TRAPD(err, aCue.iFiles->AppendL(name));
			if (err != KErrNone)
				return err;
			pendingFile = ETrue;
			}
		else if (kw == _L8("TRACK"))
			{
			if (aCue.iCount >= KMaxTrk - 1)
				return KErrOverflow;
			// A TRACK before any FILE would leave iFile at -1 and panic the
			// descriptor array later, in code that has no idea why.
			if (aCue.iFiles->MdcaCount() <= 0)
				return KErrCorrupt;
			TTrk &t = aCue.iTrk[++aCue.iCount];
			t.iType = 0;
			t.iIdx0 = t.iIdx1 = -1;
			t.iPregap = t.iPostgap = 0;
			t.iSecSize = KSector;
			t.iFile = aCue.iFiles->MdcaCount() - 1;
			t.iFirstInFile = pendingFile;
			t.iStart = t.iLen = 0;
			pendingFile = EFalse;
			nextTok(line, lp, tok);          // number
			nextTok(line, lp, tok);          // mode
			TBuf8<24> mode;
			mode.Copy(tok.Left(24));
			mode.UpperCase();
			if (mode.Find(_L8("AUDIO")) != KErrNotFound)
				t.iType = 1;
			else
				{
				const TInt sl = mode.Locate('/');
				if (sl > 0)
					{
					TLex8 lx(mode.Mid(sl + 1));
					TInt ss = 0;
					if (lx.Val(ss) == KErrNone && ss > 0)
						t.iSecSize = ss;
					}
				}
			}
		else if (kw == _L8("INDEX") && aCue.iCount)
			{
			nextTok(line, lp, tok);
			TLex8 li(tok);
			TInt idx = -1;
			li.Val(idx);
			nextTok(line, lp, tok);
			const TInt sec = parseMsf(tok);
			if (sec >= 0 && idx == 0) aCue.iTrk[aCue.iCount].iIdx0 = sec;
			if (sec >= 0 && idx == 1) aCue.iTrk[aCue.iCount].iIdx1 = sec;
			}
		else if (kw == _L8("PREGAP") && aCue.iCount)
			{
			nextTok(line, lp, tok);
			const TInt sec = parseMsf(tok);
			if (sec >= 0) aCue.iTrk[aCue.iCount].iPregap = sec;
			}
		else if (kw == _L8("POSTGAP") && aCue.iCount)
			{
			nextTok(line, lp, tok);
			const TInt sec = parseMsf(tok);
			if (sec >= 0) aCue.iTrk[aCue.iCount].iPostgap = sec;
			}
		}
	return aCue.iCount ? KErrNone : KErrCorrupt;
	}

// Table completion: exact same arithmetic as parsecue (cdriso.c:530). Custom
// variants are not suitable here - PBP must reproduce the exact addresses core
// would compute from source .cue, otherwise audio tracks shift.
static TInt completeCue(TCue &aCue, MConvVol &aVol)
	{
	for (TInt i = 1; i <= aCue.iCount; i++)
		{
		TTrk &t = aCue.iTrk[i];
		if (t.iIdx1 < 0)
			t.iIdx1 = (t.iIdx0 >= 0) ? t.iIdx0 + 2 * 75 : 0;
		if (t.iSecSize != KSector)
			return KErrNotSupported;
		}

	TInt toc = 2 * 75;
	for (TInt i = 1; i <= aCue.iCount; i++)
		{
		TTrk &t = aCue.iTrk[i];
		TInt pregap = t.iPregap + aCue.iTrk[i - 1].iPostgap;
		if (t.iFirstInFile && t.iIdx0 >= 0)
			pregap += t.iIdx1 - t.iIdx0;
		// Gaps missing from files must be padded with silence, so
		// nonsensical values here waste storage. Real pre-gaps are
		// seconds; a minute and a half already means .cue parsed incorrectly.
		if (pregap < 0 || pregap > 75 * 90)
			return KErrCorrupt;
		toc += pregap;
		t.iStart = toc;

		if (i + 1 <= aCue.iCount && !aCue.iTrk[i + 1].iFirstInFile)
			t.iLen = aCue.iTrk[i + 1].iIdx1 - t.iIdx1;
		else
			{
			TInt fsz = 0;
			const TInt r = aVol.Size(aCue.iFiles->MdcaPoint(t.iFile), fsz);
			if (r != KErrNone)
				return r;
			const TInt left = fsz - t.iIdx1 * KSector;
			t.iLen = (left > 0) ? left / KSector : 0;
			}
		if (t.iLen < 0)
			return KErrCorrupt;
		toc += t.iLen;
		}
	return KErrNone;
	}

// --- Zeroing ECC ------------------------------------------------------------

// ECC is only read by drive hardware, so the emulator ignores it, while all zeros
// are compressed by DEFLATE almost for free (measured: 65.4% -> 58.5%). Irreversible.
static TInt zeroEcc(TUint8 *aBlk, TInt aBytes)
	{
	TInt n = 0;
	for (TInt off = 0; off + KSector <= aBytes; off += KSector)
		{
		const TUint8 *s = aBlk + off;
		if (s[0] != 0x00 || s[11] != 0x00)
			continue;
		TBool sync = ETrue;
		for (TInt i = 1; i <= 10; i++)
			if (s[i] != 0xff) { sync = EFalse; break; }
		if (!sync)
			continue;
		const TInt mode = s[15];
		// MODE2/FORM2 (bit 0x20 in subheader) lacks ECC entirely, and its data
		// extends to 2347 - clearing there would destroy audio and video.
		if (mode == 2 && (s[18] & 0x20))
			continue;
		if (mode != 1 && mode != 2)
			continue;
		memset(aBlk + off + 2076, 0, 276);
		n++;
		}
	return n;
	}

// --- Worker thread log -------------------------------------------------------

// Dedicated file and RFs: file handles in Symbian cannot be transferred across
// threads, and main thread's ESTLIB FILE* is completely unusable here.
static RFs *g_logFs;
static RFile g_logFile;
static TBool g_logOk;

static void convLogOpen(RFs &aFs)
	{
	g_logFs = &aFs;
	if (g_logFile.Replace(aFs, _L("E:\\PS1\\logs\\ps1_import.jsonl"),
	                      EFileWrite | EFileShareAny) == KErrNone)
		g_logOk = ETrue;
	}

static void convLog(const char *aTag, TInt aVal)
	{
	if (!g_logOk)
		return;
	TBuf8<128> line;
	line.Copy(_L8("{\"at\":\""));
	line.Append(TPtrC8((const TUint8 *)aTag).Left(60));
	line.AppendFormat(_L8("\",\"v\":%d}\n"), aVal);
	g_logFile.Write(line);
	g_logFile.Flush();
	}

// Same log from other import files (arc7z): without it memory allocation failure
// inside the decompressor is indistinguishable from any other -4.
void ConvLogVal(const char *aTag, TInt aVal)
	{
	convLog(aTag, aVal);
	}

static void convLogClose()
	{
	if (g_logOk)
		g_logFile.Close();
	g_logOk = EFalse;
	}

// --- Progress state ---------------------------------------------------------

static RCriticalSection g_lock;
static TBool g_lockOk;
static TConvStat g_stat;
static volatile TInt g_cancel;
static RThread g_worker;
static TBool g_workerOk;

struct TConvJob
	{
	CDesCArray *iFiles;
	TConvMode iMode;
	TBool iDelete;
	TFileName iDst;
	};
static TConvJob g_job;

static void setStat(TInt aState, TInt aPercent, const TDesC *aName, const TDesC *aMsg)
	{
	if (g_lockOk) g_lock.Wait();
	g_stat.iState = aState;
	g_stat.iPercent = aPercent;
	if (aName) g_stat.iName.Copy(aName->Left(g_stat.iName.MaxLength()));
	if (aMsg)  g_stat.iMsg.Copy(aMsg->Left(g_stat.iMsg.MaxLength()));
	if (g_lockOk) g_lock.Signal();
	}

void ConvStat(TConvStat &aOut)
	{
	if (g_lockOk) g_lock.Wait();
	if (g_stat.iState == EConvRun && g_workerOk && g_worker.ExitType() != EExitPending)
		{
		g_stat.iState = EConvErr;
		g_stat.iMsg.Copy(_L("worker stopped"));
		}
	aOut = g_stat;
	if (g_lockOk) g_lock.Signal();
	}

void ConvCancel() { g_cancel = 1; }

// --- PBP writing ------------------------------------------------------------

static void put32(TUint8 *p, TUint32 v)
	{
	p[0] = (TUint8)v; p[1] = (TUint8)(v >> 8);
	p[2] = (TUint8)(v >> 16); p[3] = (TUint8)(v >> 24);
	}

static void msfBcd(TInt aSec, TUint8 *aOut)
	{
	TInt m = aSec / (75 * 60);
	TInt rest = aSec % (75 * 60);
	const TInt s = rest / 75, f = rest % 75;
	if (m > 99) m = 99;
	aOut[0] = (TUint8)(((m / 10) << 4) | (m % 10));
	aOut[1] = (TUint8)(((s / 10) << 4) | (s % 10));
	aOut[2] = (TUint8)(((f / 10) << 4) | (f % 10));
	}

// zlib in worker thread allocates directly from kernel: ESTLIB is uninitialized here.
static voidpf zAlloc(voidpf, uInt aItems, uInt aSize)
	{
	return (voidpf)User::Alloc((TInt)(aItems * aSize));
	}
static void zFree(voidpf, voidpf aPtr) { User::Free(aPtr); }

struct TPbpWriter
	{
	RFile iOut;
	TUint32 *iIndex;      // pairs of (offset, size)
	TInt iBlocks;
	TInt iOff;            // offset from cdimg_base
	TUint8 *iBlk;
	TInt iFill;           // bytes in iBlk
	z_stream iZ;
	TUint8 *iComp;
	TInt iSectors;
	};

static TInt pbpFlushBlock(TPbpWriter &w)
	{
	if (!w.iFill)
		return KErrNone;
	if (w.iFill < KBlkBytes)
		memset(w.iBlk + w.iFill, 0, KBlkBytes - w.iFill);
	// One index slot stays reserved for the terminator written in pbpFinish.
	if (w.iBlocks + 1 >= KMaxIndex)
		return KErrOverflow;
	zeroEcc(w.iBlk, KBlkBytes);

	deflateReset(&w.iZ);
	w.iZ.next_in = w.iBlk;
	w.iZ.avail_in = KBlkBytes;
	w.iZ.next_out = w.iComp;
	w.iZ.avail_out = KBlkBytes + 4096;
	if (deflate(&w.iZ, Z_FINISH) != Z_STREAM_END)
		return KErrGeneral;
	const TInt clen = (KBlkBytes + 4096) - (TInt)w.iZ.avail_out;

	TPtrC8 p(w.iComp, clen);
	const TInt r = w.iOut.Write(p);
	if (r != KErrNone)
		return r;
	w.iIndex[w.iBlocks * 2]     = (TUint32)w.iOff;
	w.iIndex[w.iBlocks * 2 + 1] = (TUint32)clen;
	w.iBlocks++;
	w.iOff += clen;
	w.iFill = 0;
	return KErrNone;
	}

static TInt pbpAdd(TPbpWriter &w, const TUint8 *aData, TInt aLen)
	{
	while (aLen > 0)
		{
		const TInt n = Min(aLen, KBlkBytes - w.iFill);
		memcpy(w.iBlk + w.iFill, aData, n);
		w.iFill += n;
		aData += n;
		aLen -= n;
		if (w.iFill == KBlkBytes)
			{
			const TInt r = pbpFlushBlock(w);
			if (r != KErrNone)
				return r;
			}
		}
	return KErrNone;
	}

// Silence in inter-track gaps: sectors missing from source files (PREGAP).
static TInt pbpAddZeros(TPbpWriter &w, TInt aSectors)
	{
	TUint8 *z = (TUint8 *)User::Alloc(KSector);
	if (!z)
		return KErrNoMemory;
	memset(z, 0, KSector);
	TInt r = KErrNone;
	for (TInt i = 0; i < aSectors && r == KErrNone; i++)
		r = pbpAdd(w, z, KSector);
	User::Free(z);
	return r;
	}

static TInt pbpFinish(TPbpWriter &w, const TCue &aCue, TInt aTotalSectors)
	{
	TInt r = pbpFlushBlock(w);
	if (r != KErrNone)
		return r;

	TUint8 hdr[40];
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 0; hdr[1] = 'P'; hdr[2] = 'B'; hdr[3] = 'P';
	put32(hdr + 4, 0x10000);
	for (TInt i = 0; i < 8; i++)
		put32(hdr + 8 + i * 4, KPsar);
	TInt pos = 0;
	r = w.iOut.Seek(ESeekStart, pos);
	if (r != KErrNone) return r;
	r = w.iOut.Write(TPtrC8(hdr, sizeof(hdr)));
	if (r != KErrNone) return r;

	pos = KPsar;
	r = w.iOut.Seek(ESeekStart, pos);
	if (r != KErrNone) return r;
	r = w.iOut.Write(_L8("PSISOIMG0000"));
	if (r != KErrNone) return r;

	// TOC: entry is 10 bytes (all char fields, packed). First three are
	// header entries: dummy, track count, lead-out cd length.
	const TInt nent = 3 + aCue.iCount;
	TUint8 *toc = (TUint8 *)User::Alloc(nent * 10);
	if (!toc)
		return KErrNoMemory;
	memset(toc, 0, nent * 10);
	// Reader fetches both fields via btoi(), expecting BCD.
	toc[10 + 7] = (TUint8)(((aCue.iCount / 10) << 4) | (aCue.iCount % 10));
	msfBcd(aTotalSectors, toc + 20 + 7);                  // cd_length
	for (TInt i = 1; i <= aCue.iCount; i++)
		{
		TUint8 *e = toc + (2 + i) * 10;
		e[0] = (TUint8)(aCue.iTrk[i].iType == 1 ? 1 : 2);
		e[2] = (TUint8)(((i / 10) << 4) | (i % 10));
		msfBcd(aCue.iTrk[i].iStart - 2 * 75, e + 3);      // index0: position in image
		msfBcd(aCue.iTrk[i].iStart, e + 7);               // index1: address on disc
		}
	pos = KPsar + 0x800;
	r = w.iOut.Seek(ESeekStart, pos);
	if (r == KErrNone)
		r = w.iOut.Write(TPtrC8(toc, nent * 10));
	User::Free(toc);
	if (r != KErrNone)
		return r;

	// Block index: 32 bytes per entry, terminator with size==0 marks end of data.
	pos = KPsar + 0x4000;
	r = w.iOut.Seek(ESeekStart, pos);
	if (r != KErrNone)
		return r;
	TUint8 ent[32];
	memset(ent, 0, sizeof(ent));
	for (TInt b = 0; b < w.iBlocks; b++)
		{
		put32(ent + 0, w.iIndex[b * 2]);
		put32(ent + 4, w.iIndex[b * 2 + 1]);
		r = w.iOut.Write(TPtrC8(ent, sizeof(ent)));
		if (r != KErrNone)
			return r;
		}
	put32(ent + 0, (TUint32)w.iOff);
	put32(ent + 4, 0);
	return w.iOut.Write(TPtrC8(ent, sizeof(ent)));
	}

// --- Single conversion ------------------------------------------------------

// Destination name: source base name + .pbp in games directory.
static void dstName(const TDesC &aSrcName, const TDesC &aDir, const TDesC &aExt,
                    TDes &aOut)
	{
	TFileName n = aSrcName;
	TInt s = n.LocateReverse('\\');
	if (s < 0) s = n.LocateReverse('/');
	if (s >= 0)
		n.Delete(0, s + 1);
	const TInt d = n.LocateReverse('.');
	if (d > 0)
		n.SetLength(d);
	// Truncate only to fit path limit, not an arbitrary number: 40 chars cut
	// titles mid-word ("...of the Night (USA" missing parenthesis) even with ample room.
	const TInt room = aOut.MaxLength() - aDir.Length() - aExt.Length();
	// room can go negative on a deep destination directory, and then the name
	// was appended unclipped and panicked the descriptor.
	if (room <= 0)
		n.Zero();
	else if (n.Length() > room)
		n.SetLength(room);
	aOut = aDir.Left(aOut.MaxLength());
	if (aOut.Length() + n.Length() <= aOut.MaxLength())
		aOut.Append(n);
	if (aOut.Length() + aExt.Length() <= aOut.MaxLength())
		aOut.Append(aExt);
	}

// Discs in a volume, counted the way pickEntry chooses: by .cue files when there
// are any (their tracks belong to them), otherwise by standalone images.
static TInt discCount(MConvVol &aVol)
	{
	const MDesCArray &e = aVol.Entries();
	TInt cues = 0, images = 0;
	for (TInt i = 0; i < e.MdcaCount(); i++)
		{
		const TPtrC n = e.MdcaPoint(i);
		if (extIs(n, _L(".cue")))
			cues++;
		else if (isRawImageName(n) || extIs(n, _L(".pbp")) || extIs(n, _L(".cbn")))
			images++;
		}
	return cues ? cues : images;
	}

// Selects appropriate entry to convert: .cue takes precedence over .bin
// as it carries track layout; otherwise first disc image.
static TInt pickEntry(MConvVol &aVol, TDes &aOut)
	{
	const MDesCArray &e = aVol.Entries();
	for (TInt i = 0; i < e.MdcaCount(); i++)
		if (extIs(e.MdcaPoint(i), _L(".cue")))
			{ aOut = e.MdcaPoint(i); return KErrNone; }
	for (TInt i = 0; i < e.MdcaCount(); i++)
		if (isRawImageName(e.MdcaPoint(i)))
			{ aOut = e.MdcaPoint(i); return KErrNone; }
	for (TInt i = 0; i < e.MdcaCount(); i++)
		if (extIs(e.MdcaPoint(i), _L(".pbp")) || extIs(e.MdcaPoint(i), _L(".cbn")))
			{ aOut = e.MdcaPoint(i); return KErrNone; }
	return KErrNotFound;
	}

// Sources may return short counts mid-stream (archive decoders), so a short
// result here means the stream really ended.
static TInt readFull(MConvSrc &aSrc, TUint8 *aBuf, TInt aLen)
	{
	TInt got = 0;
	while (got < aLen)
		{
		const TInt n = aSrc.Read(aBuf + got, aLen - got);
		if (n < 0)
			return n;
		if (!n)
			break;
		got += n;
		}
	return got;
	}

// Copy entry (compression failed or requested by user).
static TInt copyEntry(RFs &aFs, MConvVol &aVol, const TDesC &aName,
                      const TDesC &aDstPath, TInt aTotal)
	{
	MConvSrc *src = NULL;
	TInt r = aVol.Open(aName, src);
	if (r != KErrNone)
		return r;
	RFile out;
	r = out.Replace(aFs, aDstPath, EFileWrite | EFileShareExclusive);
	if (r != KErrNone)
		{
		delete src;
		return r;
		}
	const TInt KBuf = 64 * 1024;
	TUint8 *buf = (TUint8 *)User::Alloc(KBuf);
	TInt done = 0;
	while (buf && r == KErrNone && !g_cancel)
		{
		const TInt n = src->Read(buf, KBuf);
		if (n < 0) { r = n; break; }
		if (!n) break;
		r = out.Write(TPtrC8(buf, n));
		done += n;
		if (aTotal > 0)
			setStat(EConvRun, (TInt)((TInt64)done * 100 / aTotal), NULL, NULL);
		}
	if (!buf)
		r = KErrNoMemory;
	// A truncated source ends the stream quietly; the caller deletes it on success.
	if (r == KErrNone && !g_cancel && aTotal > 0 && done != aTotal)
		r = KErrCorrupt;
	User::Free(buf);
	out.Close();
	delete src;
	if (g_cancel && r == KErrNone)
		r = KErrCancel;
	if (r != KErrNone)
		aFs.Delete(aDstPath);
	return r;
	}

// Builds disc image into PBP. Source is read sequentially by track; missing gaps
// are padded with silence so a sector's position in image equals
// its disc address minus 150 - this is required for a consistent TOC.
static TInt convertToPbp(RFs &aFs, MConvVol &aVol, const TDesC &aDstPath, TCue &aCue)
	{
	TPbpWriter w;
	memset(&w, 0, sizeof(w));
	TInt r = w.iOut.Replace(aFs, aDstPath, EFileWrite | EFileShareExclusive);
	if (r != KErrNone)
		return r;
	// Data starts exactly at psar+0x100000; index offsets are relative to
	// this point, so iOff remains zero.
	//
	// SetSize before Seek is mandatory: Seek clamps position to existing file
	// size, so in a newly created file seeking to 0x100028 would silently stay
	// at zero, causing headers to overwrite the start of data later.
	TInt startPos = KDataOffs;
	r = w.iOut.SetSize(KDataOffs);
	if (r == KErrNone)
		r = w.iOut.Seek(ESeekStart, startPos);
	if (r != KErrNone || startPos != KDataOffs)
		{
		w.iOut.Close();
		aFs.Delete(aDstPath);
		return (r != KErrNone) ? r : KErrGeneral;
		}

	w.iIndex = (TUint32 *)User::Alloc(KMaxIndex * 2 * sizeof(TUint32));
	w.iBlk   = (TUint8 *)User::Alloc(KBlkBytes);
	w.iComp  = (TUint8 *)User::Alloc(KBlkBytes + 4096);
	if (!w.iIndex || !w.iBlk || !w.iComp)
		r = KErrNoMemory;

	if (r == KErrNone)
		{
		w.iZ.zalloc = zAlloc;
		w.iZ.zfree = zFree;
		w.iZ.opaque = NULL;
		// -15: raw DEFLATE without zlib header - exactly what
		// uncompress2_pcsx() expects (inflateInit2(&z, -15) in cdriso.c).
		if (deflateInit2(&w.iZ, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
			r = KErrNoMemory;
		}

	TInt total = 0;
	for (TInt i = 1; i <= aCue.iCount && r == KErrNone; i++)
		total += aCue.iTrk[i].iLen;

	TInt curFile = -1;
	MConvSrc *src = NULL;
	TInt srcPos = 0;         // position in current file, sectors
	TInt written = 0;        // sectors written to image

	for (TInt i = 1; i <= aCue.iCount && r == KErrNone; i++)
		{
		TTrk &t = aCue.iTrk[i];
		const TInt want = t.iStart - 2 * 75;
		if (want > written)
			{
			r = pbpAddZeros(w, want - written);
			written = want;
			}
		if (r != KErrNone)
			break;

		if (t.iFile != curFile)
			{
			delete src;
			src = NULL;
			r = aVol.Open(aCue.iFiles->MdcaPoint(t.iFile), src);
			if (r != KErrNone)
				break;
			curFile = t.iFile;
			srcPos = 0;
			}
		if (t.iIdx1 > srcPos)
			{
			r = src->Skip((t.iIdx1 - srcPos) * KSector);
			srcPos = t.iIdx1;
			}
		if (r != KErrNone)
			break;

		for (TInt s = 0; s < t.iLen && r == KErrNone; s++)
			{
			TUint8 sec[KSector];
			const TInt n = readFull(*src, sec, KSector);
			if (n < 0) { r = n; break; }
			if (n < KSector)
				{
				// Only the unfinished sector at the very end of the file is legitimate;
				// anything else is a truncated or damaged source.
				if (n == 0 || (TInt64)srcPos * KSector + n != (TInt64)src->Size())
					{ r = KErrCorrupt; break; }
				memset(sec + n, 0, KSector - n);
				}
			r = pbpAdd(w, sec, KSector);
			srcPos++;
			written++;
			if ((written & 1023) == 0)
				{
				if (g_cancel) { r = KErrCancel; break; }
				setStat(EConvRun, total ? (TInt)((TInt64)written * 100 / total) : 0,
					NULL, NULL);
				}
			}
		}

	delete src;
	if (r == KErrNone)
		r = pbpFinish(w, aCue, written);

	deflateEnd(&w.iZ);
	User::Free(w.iIndex);
	User::Free(w.iBlk);
	User::Free(w.iComp);
	w.iOut.Close();
	if (r != KErrNone)
		aFs.Delete(aDstPath);
	return r;
	}

// Prepares TCue: from neighboring .cue or synthetic single-track for raw image.
static TInt buildCue(MConvVol &aVol, const TDesC &aEntry, TCue &aCue)
	{
	aCue.iCount = 0;
	if (extIs(aEntry, _L(".cue")))
		{
		MConvSrc *src = NULL;
		TInt r = aVol.Open(aEntry, src);
		if (r != KErrNone)
			return r;
		// A cue cut at 64 KB would silently drop tracks.
		const TInt sz = src->Size();
		if (sz <= 0 || sz > 64 * 1024)
			{
			delete src;
			return KErrCorrupt;
			}
		TUint8 *buf = (TUint8 *)User::Alloc(sz);
		if (!buf)
			{
			delete src;
			return KErrNoMemory;
			}
		const TInt n = readFull(*src, buf, sz);
		delete src;
		if (n != sz)
			{
			User::Free(buf);
			return KErrCorrupt;
			}
		r = parseCue(TPtrC8(buf, n), aCue);
		User::Free(buf);
		if (r != KErrNone)
			return r;
		return completeCue(aCue, aVol);
		}

	// A neighboring .cue with matching stem may exist - this is how core searches
	// (parsecue replaces extension). Without this check, a CD-DA game selected
	// via .bin would lose all tracks except track 1.
	TFileName sibling = aEntry;
	const TInt dot = sibling.LocateReverse('.');
	if (dot > 0)
		{
		sibling.SetLength(dot);
		sibling.Append(_L(".cue"));
		TInt tmp = 0;
		if (aVol.Size(sibling, tmp) == KErrNone && tmp > 0)
			return buildCue(aVol, sibling, aCue);
		}

	// Raw image: single data track starting at sector 0.
	TInt fsz = 0;
	TInt r = aVol.Size(aEntry, fsz);
	if (r != KErrNone)
		return r;
	if (fsz <= 0 || (fsz % KSector) != 0)
		return KErrNotSupported;
	TRAP(r, aCue.iFiles->AppendL(aEntry));
	if (r != KErrNone)
		return r;
	aCue.iCount = 1;
	TTrk &t = aCue.iTrk[1];
	t.iType = 0;
	t.iIdx0 = -1;
	t.iIdx1 = 0;
	t.iPregap = t.iPostgap = 0;
	t.iSecSize = KSector;
	t.iFile = 0;
	t.iFirstInFile = ETrue;
	t.iStart = 2 * 75;
	t.iLen = fsz / KSector;
	return KErrNone;
	}

// Track file that conversion actually read (a name in .cue with no sectors to
// take may be someone else's file).
static TBool trackFileUsed(const TCue &aCue, TInt aFile)
	{
	for (TInt i = 1; i <= aCue.iCount; i++)
		if (aCue.iTrk[i].iFile == aFile && aCue.iTrk[i].iLen > 0)
			return ETrue;
	return EFalse;
	}

// Deletes source after successful import: whole archive or .cue together with the
// track files that were used (aOnlyUsed) or all that were copied.
static void deleteSource(RFs &aFs, const TDesC &aPath, const TCue &aCue, TBool aOnlyUsed)
	{
	if (ConvIsArchiveName(aPath))
		{
		aFs.Delete(aPath);
		return;
		}
	const TInt s = aPath.LocateReverse('\\');
	const TPtrC dir = aPath.Left(s + 1);
	for (TInt i = 0; aCue.iFiles && i < aCue.iFiles->MdcaCount(); i++)
		{
		if (aOnlyUsed && !trackFileUsed(aCue, i))
			continue;
		TFileName p(dir);
		if (p.Length() + aCue.iFiles->MdcaPoint(i).Length() < p.MaxLength())
			{
			p.Append(aCue.iFiles->MdcaPoint(i));
			aFs.Delete(p);
			}
		}
	aFs.Delete(aPath);
	// User may have selected .bin while layout came from neighboring .cue -
	// leaving .cue alone would leave a broken reference to the deleted file.
	TFileName cue = aPath;
	const TInt dot = cue.LocateReverse('.');
	if (dot > 0)
		{
		cue.SetLength(dot);
		cue.Append(_L(".cue"));
		aFs.Delete(cue);
		}
	}

// Stores the disc without conversion: the .cue (when it is the entry) and every
// file it names. Anything left behind is an error, because the caller deletes the
// source on success.
static TInt copyDisc(RFs &aFs, MConvVol &aVol, const TDesC &aEntry, const TCue &aCue,
                     const TDesC &aDstDir)
	{
	TInt r = KErrNone;
	// A .bin picked next to its .cue took the layout from that .cue, and deleteSource
	// removes the .cue as well: it has to travel with the tracks.
	TFileName cueName(aEntry);
	if (!extIs(aEntry, _L(".cue")))
		{
		const TInt dot = cueName.LocateReverse('.');
		TInt csz = 0;
		if (dot > 0)
			{
			cueName.SetLength(dot);
			cueName.Append(_L(".cue"));
			}
		if (dot <= 0 || aVol.Size(cueName, csz) != KErrNone || csz <= 0)
			cueName.Zero();
		}
	if (cueName.Length() || !aCue.iFiles->MdcaCount())
		{
		const TDesC &src = cueName.Length() ? (const TDesC &)cueName : aEntry;
		TFileName dst;
		TInt sz = 0;
		aVol.Size(src, sz);
		dstName(src, aDstDir, src.Right(4), dst);
		r = copyEntry(aFs, aVol, src, dst, sz);
		}
	for (TInt i = 0; i < aCue.iFiles->MdcaCount() && r == KErrNone; i++)
		{
		const TPtrC nm = aCue.iFiles->MdcaPoint(i);
		TFileName d2(aDstDir);
		if (d2.Length() + nm.Length() >= d2.MaxLength())
			{
			r = KErrBadName;
			break;
			}
		d2.Append(nm);
		TInt s2 = 0;
		aVol.Size(nm, s2);
		r = copyEntry(aFs, aVol, nm, d2, s2);
		}
	return r;
	}

static TInt convertOne(RFs &aFs, const TDesC &aPath, TConvMode aMode,
                       TBool aDelete, const TDesC &aDstDir, TDes &aMsg)
	{
	MConvVol *vol = NULL;
	TInt r = ConvOpenVol(aFs, aPath, vol);
	convLog("openvol", r);
	if (r != KErrNone)
		{
		aMsg.Copy(_L("cannot open"));
		return r;
		}

	TFileName entry;
	r = pickEntry(*vol, entry);
	convLog("pickentry", r);
	if (r != KErrNone)
		{
		aMsg.Copy(_L("no image inside"));
		delete vol;
		return r;
		}

	// TCue is a 100-track array; it does not belong on the worker thread's
	// 16 KB stack.
	TCue *cue = (TCue *)User::Alloc(sizeof(TCue));
	if (!cue)
		{
		delete vol;
		return KErrNoMemory;
		}
	cue->iCount = 0;
	cue->iFiles = new CDesCArrayFlat(4);
	if (!cue->iFiles)
		{
		User::Free(cue);
		delete vol;
		return KErrNoMemory;
		}

	TFileName dst;
	// PBP conversion reads only tracks with sectors; a plain copy takes every file.
	TBool onlyUsed = ETrue;
	if (aMode == EConvModePbp)
		{
		r = buildCue(*vol, entry, *cue);
		convLog("buildcue", r);
		convLog("tracks", cue->iCount);
		if (r == KErrNone)
			{
			dstName(entry, aDstDir, _L(".pbp"), dst);
			r = convertToPbp(aFs, *vol, dst, *cue);
			convLog("topbp", r);
			if (r != KErrNone && r != KErrCancel)
				aMsg.Copy(_L("convert failed"));
			}
		else if (r == KErrNotFound || r == KErrPathNotFound)
			aMsg.Copy(_L("file not readable"));
		else if (r == KErrCorrupt)
			aMsg.Copy(_L("bad cue or image"));
		else if (r == KErrNoMemory || r == KErrTooBig)
			// Almost always LZMA dictionary: allocated in full before the first byte.
			aMsg.Copy(_L("archive needs too much RAM"));
		else if (r == KErrNotSupported)
			{
			// 2048-byte sectors or non-standard .cue: PBP cannot represent this,
			// so store as is - game remains playable without saving space.
			aMsg.Copy(_L("stored as is"));
			onlyUsed = EFalse;
			r = copyDisc(aFs, *vol, entry, *cue, aDstDir);
			}
		}
	else
		{
		// For .cue, transfer both the cue file and all track files: missing any
		// of them loses track layout and prevents game from loading.
		r = buildCue(*vol, entry, *cue);
		onlyUsed = EFalse;
		if (r == KErrNone || r == KErrNotSupported)
			r = copyDisc(aFs, *vol, entry, *cue, aDstDir);
		}

	// Only one disc is taken from an archive; deleting one that holds more would
	// lose the rest.
	const TBool keepArchive = ConvIsArchiveName(aPath) && discCount(*vol) > 1;
	delete vol;
	if (r == KErrNone && aDelete && !keepArchive)
		deleteSource(aFs, aPath, *cue, onlyUsed);
	delete cue->iFiles;
	User::Free(cue);
	return r;
	}

// Checks if list contains an entry for the same disc: same directory and stem.
// Returns matching index or -1.
static TInt sameDiscIndex(const MDesCArray &aList, const TDesC &aPath)
	{
	const TInt dot = aPath.LocateReverse('.');
	const TPtrC stem = (dot > 0) ? aPath.Left(dot) : TPtrC(aPath);
	for (TInt i = 0; i < aList.MdcaCount(); i++)
		{
		const TPtrC p = aList.MdcaPoint(i);
		const TInt d = p.LocateReverse('.');
		const TPtrC s = (d > 0) ? p.Left(d) : TPtrC(p);
		if (s.CompareF(stem) == 0)
			return i;
		}
	return -1;
	}

// --- Worker thread ----------------------------------------------------------

static TInt workerMain(TAny *)
	{
	CTrapCleanup *cleanup = CTrapCleanup::New();
	if (!cleanup)
		return KErrNoMemory;
	RFs fs;
	TInt r = fs.Connect();
	if (r != KErrNone)
		{
		delete cleanup;
		TBuf<64> m(_L("no file server"));
		setStat(EConvErr, 0, NULL, &m);
		return r;
		}

	convLogOpen(fs);
	const TInt n = g_job.iFiles->MdcaCount();
	convLog("worker start", n);
	// LZMA dictionary is allocated in full; remaining system RAM is the primary metric
	// when decompressor fails with out-of-memory.
	TInt ramFree = 0;
	HAL::Get(HALData::EMemoryRAMFree, ramFree);
	convLog("ram free", ramFree);
	TInt failed = 0;
	TBuf<64> lastFail;
	for (TInt i = 0; i < n && !g_cancel; i++)
		{
		const TPtrC path = g_job.iFiles->MdcaPoint(i);
		TInt s = path.LocateReverse('\\');
		const TPtrC nm = path.Mid(s + 1);

		if (g_lockOk) g_lock.Wait();
		g_stat.iFileNo = i + 1;
		g_stat.iFileCount = n;
		g_stat.iName.Copy(nm.Left(g_stat.iName.MaxLength()));
		g_stat.iState = EConvRun;
		g_stat.iPercent = 0;
		if (g_lockOk) g_lock.Signal();

		TBuf<64> msg;
		const TInt rc = convertOne(fs, path, g_job.iMode, g_job.iDelete,
			g_job.iDst, msg);
		// Source vanished - consumed by a previous queue task (track file from
		// .cue already processed with deletion enabled). This is not an error.
		if (i > 0 && (rc == KErrNotFound || rc == KErrPathNotFound))
			{
			convLog("skip gone", i);
			continue;
			}
		if (rc != KErrNone && rc != KErrCancel)
			{
			// Queue continues: a single archive that ran out of memory or was corrupted
			// is no reason to abandon dozens of remaining files. Show cause immediately, summary
			// at the end.
			if (!msg.Length())
				msg.Copy(_L("failed"));
			msg.AppendFormat(_L(" (%d)"), rc);
			setStat(EConvRun, 0, NULL, &msg);
			convLog("file failed", rc);
			failed++;
			lastFail.Copy(msg);
			continue;
			}
		}

	convLog("worker done", failed);
	convLogClose();
	fs.Close();
	delete cleanup;
	TBuf<64> done;
	if (g_cancel)
		done.Copy(_L("cancelled"));
	else if (failed)
		done.Format(_L("%d of %d failed: "), failed, n);
	else
		done.Copy(_L("done"));
	if (failed && !g_cancel)
		done.Append(lastFail.Left(done.MaxLength() - done.Length()));
	setStat(failed && !g_cancel ? EConvErr : EConvDone, 100, NULL, &done);
	return KErrNone;
	}

TInt ConvStart(const MDesCArray &aFiles, TConvMode aMode, TBool aDeleteSrc,
               const TDesC &aDstDir)
	{
	// A worker that died without its final setStat leaves the state at EConvRun.
	if (g_stat.iState == EConvRun && (!g_workerOk || g_worker.ExitType() == EExitPending))
		return KErrInUse;
	ConvRelease();

	if (!g_lockOk && g_lock.CreateLocal() == KErrNone)
		g_lockOk = ETrue;

	CDesCArray *copy = new CDesCArrayFlat(4);
	if (!copy)
		return KErrNoMemory;
	// Selecting .cue AND its .bin is a natural user action, but represents the same
	// disc: both jobs would produce the same output file, and after the first the source
	// is gone. Retain one entry per disc, favoring .cue for its
	// track layout.
	TRAPD(err, {
		for (TInt i = 0; i < aFiles.MdcaCount(); i++)
			{
			const TPtrC p = aFiles.MdcaPoint(i);
			const TInt at = sameDiscIndex(*copy, p);
			if (at < 0)
				copy->AppendL(p);
			else if (extIs(p, _L(".cue")) && !extIs(copy->MdcaPoint(at), _L(".cue")))
				{
				copy->Delete(at);
				copy->InsertL(at, p);
				}
			}
		});
	if (err != KErrNone)
		{
		delete copy;
		return err;
		}

	delete g_job.iFiles;
	g_job.iFiles = copy;
	g_job.iMode = aMode;
	g_job.iDelete = aDeleteSrc;
	g_job.iDst = aDstDir;
	g_cancel = 0;
	if (g_lockOk) g_lock.Wait();
	g_stat.iState = EConvRun;
	g_stat.iErr = 0;
	g_stat.iPercent = 0;
	g_stat.iFileNo = 0;
	g_stat.iFileCount = aFiles.MdcaCount();
	g_stat.iMsg.Zero();
	g_stat.iName.Zero();
	if (g_lockOk) g_lock.Signal();

	// Dedicated heap: deflate keeps ~256 KB, block index another 256 KB; taking them
	// from the main thread heap would compete with dynarec.
	// Heap ceiling is address space reservation only, pages are committed on demand,
	// sized for LZMA dictionary (KMaxDict in arc7z). With 8 MB any typical
	// 7z (16-64 MB dictionary) failed during open with -4.
	// Counted thread name: a terminated thread cleans up asynchronously, so a quick
	// subsequent launch would otherwise fail with KErrAlreadyExists.
	static TInt seq = 0;
	TName tn;
	tn.Format(_L("ps1conv%d"), ++seq);
	const TInt r = g_worker.Create(tn, workerMain,
		32 * 1024, 4 * 1024, 72 * 1024 * 1024, NULL);
	if (r != KErrNone)
		{
		TBuf<64> m(_L("cannot start worker"));
		setStat(EConvErr, 0, NULL, &m);
		return r;
		}
	g_workerOk = ETrue;
	// Below main: menu must remain responsive while disc is being compressed.
	g_worker.SetPriority(EPriorityLess);
	g_worker.Resume();
	return KErrNone;
	}

void ConvRelease()
	{
	if (g_workerOk)
		{
		g_worker.Close();
		g_workerOk = EFalse;
		}
	}

// --- Single file extraction (BIOS) ------------------------------------------

TInt ConvExtractOne(RFs &aFs, const TDesC &aPath, const TDesC &aMember,
                    const TDesC &aDstDir, TDes &aOutName)
	{
	MConvVol *vol = NULL;
	TInt r = ConvOpenVol(aFs, aPath, vol);
	if (r != KErrNone)
		return r;

	TFileName name = aMember;
	if (!name.Length())
		{
		const MDesCArray &e = vol->Entries();
		if (!e.MdcaCount())
			{
			delete vol;
			return KErrNotFound;
			}
		// BIOS dump archives usually include a readme; pick the first entry that
		// looks like a dump, falling back to the first entry if none match.
		name = e.MdcaPoint(0);
		for (TInt i = 0; i < e.MdcaCount(); i++)
			if (extIs(e.MdcaPoint(i), _L(".bin")) || extIs(e.MdcaPoint(i), _L(".rom")))
				{ name = e.MdcaPoint(i); break; }
		}

	TInt s = name.LocateReverse('\\');
	if (s < 0) s = name.LocateReverse('/');
	const TPtrC base = name.Mid(s + 1);
	TFileName dst(aDstDir);
	if (dst.Length() + base.Length() >= dst.MaxLength())
		{
		delete vol;
		return KErrBadName;
		}
	dst.Append(base);

	TInt sz = 0;
	vol->Size(name, sz);
	r = copyEntry(aFs, *vol, name, dst, sz);
	delete vol;
	if (r == KErrNone)
		aOutName.Copy(base.Left(aOutName.MaxLength()));
	return r;
	}
