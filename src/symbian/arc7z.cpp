// 7z volume for import. Extracts as a stream via custom loop instead of SzArEx_Extract:
// the latter delivers an entire archive folder in a single memory buffer, where a folder
// here is a 700 MB disc image.
//
// Single-coder folders are supported: LZMA, LZMA2, or uncompressed. Filter chains
// (BCJ, BCJ2, delta) are rejected - disc images do not use them, and
// a streaming version of each would cost more than it yields.

#include <e32base.h>
#include <f32file.h>
#include <badesca.h>
#include <string.h>

#include "imgconv.h"

extern "C"
	{
#include "7z.h"
#include "7zCrc.h"
#include "LzmaDec.h"
#include "Lzma2Dec.h"
	}

#define K_COPY  0
#define K_LZMA2 0x21
#define K_LZMA  0x30101

// LZMA dictionary is allocated entirely and immediately. 64 MB is 7-Zip's own ceiling ("ultra"
// mode), so archives do not exceed this; if that much free RAM is unavailable
// on hardware, User::Alloc will fail gracefully, which is preferable to KERN-EXEC.
const TInt KMaxDict = 64 * 1024 * 1024;

static void *szAlloc(ISzAllocPtr, size_t aSize) { return User::Alloc((TInt)aSize); }
static void szFree(ISzAllocPtr, void *aPtr) { User::Free(aPtr); }
static const ISzAlloc g_szAlloc = { szAlloc, szFree };

// --- Archive read stream for header parsing ----------------------------------

struct TSzStream
	{
	ISeekInStream vt;
	RFile *iFile;
	TInt iPos;
	};

static SRes szRead(ISeekInStreamPtr pp, void *aBuf, size_t *aSize)
	{
	TSzStream *s = (TSzStream *)pp;
	if (!*aSize)
		return SZ_OK;
	TPtr8 p((TUint8 *)aBuf, (TInt)*aSize);
	if (s->iFile->Read(s->iPos, p, (TInt)*aSize) != KErrNone)
		return SZ_ERROR_READ;
	s->iPos += p.Length();
	*aSize = (size_t)p.Length();
	return SZ_OK;
	}

static SRes szSeek(ISeekInStreamPtr pp, Int64 *aPos, ESzSeek aOrigin)
	{
	TSzStream *s = (TSzStream *)pp;
	TInt size = 0;
	s->iFile->Size(size);
	TInt np = 0;
	switch (aOrigin)
		{
		case SZ_SEEK_SET: np = (TInt)*aPos; break;
		case SZ_SEEK_CUR: np = s->iPos + (TInt)*aPos; break;
		case SZ_SEEK_END: np = size + (TInt)*aPos; break;
		default: return SZ_ERROR_PARAM;
		}
	if (np < 0 || np > size)
		return SZ_ERROR_READ;
	s->iPos = np;
	*aPos = np;
	return SZ_OK;
	}

// --- Single-entry decompressor -----------------------------------------------

class C7zSrc : public CBase, public MConvSrc
	{
public:
	static TInt New(RFs &aFs, const TDesC &aPath, TInt aPackPos, TInt aPackSize,
	                TInt aMethod, const TUint8 *aProps, TInt aPropsSize,
	                TInt aSkip, TInt aSize, MConvSrc *&aOut);
	~C7zSrc();
	TInt Read(TUint8 *aBuf, TInt aLen);
	TInt Skip(TInt aBytes);
	TInt Size() const { return iSize; }
private:
	TInt Pull(TUint8 *aBuf, TInt aLen);
	RFile iFile;
	TInt iMethod;
	TInt iPackPos, iPackSize, iPackDone;
	TInt iSize;
	TUint8 *iIn;
	TInt iInFill, iInPos;
	CLzmaDec iLzma;
	CLzma2Dec iLzma2;
	TBool iInited;
	TBool iEof;
	};

const TInt KInBuf = 64 * 1024;

TInt C7zSrc::New(RFs &aFs, const TDesC &aPath, TInt aPackPos, TInt aPackSize,
                 TInt aMethod, const TUint8 *aProps, TInt aPropsSize,
                 TInt aSkip, TInt aSize, MConvSrc *&aOut)
	{
	C7zSrc *self = new C7zSrc;
	if (!self)
		return KErrNoMemory;
	self->iMethod = aMethod;
	self->iPackPos = aPackPos;
	self->iPackSize = aPackSize;
	self->iSize = aSize;

	TInt r = self->iFile.Open(aFs, aPath, EFileRead | EFileShareReadersOnly);
	if (r != KErrNone)
		{
		delete self;
		return r;
		}
	self->iIn = (TUint8 *)User::Alloc(KInBuf);
	if (!self->iIn)
		{
		delete self;
		ConvLogVal("7z inbuf oom", KInBuf);
		return KErrNoMemory;
		}

	if (aMethod == K_LZMA)
		{
		// Unsigned: a signed shift of a byte >= 0x80 would go negative and pass the limit.
		const TUint32 dict = (aPropsSize < 5) ? 0
			: (TUint32)aProps[1] | ((TUint32)aProps[2] << 8) |
			  ((TUint32)aProps[3] << 16) | ((TUint32)aProps[4] << 24);
		ConvLogVal("7z lzma dict", (TInt)dict);
		if (aPropsSize < 5 || dict > (TUint32)KMaxDict)
			{
			delete self;
			return KErrTooBig;
			}
		LzmaDec_Construct(&self->iLzma);
		if (LzmaDec_Allocate(&self->iLzma, aProps, (unsigned)aPropsSize,
		                     &g_szAlloc) != SZ_OK)
			{
			delete self;
			ConvLogVal("7z dict oom", (TInt)dict);
			return KErrNoMemory;
			}
		LzmaDec_Init(&self->iLzma);
		self->iInited = ETrue;
		}
	else if (aMethod == K_LZMA2)
		{
		if (aPropsSize < 1)
			{
			delete self;
			return KErrCorrupt;
			}
		// LZMA2 properties byte encodes dictionary as 2|3 << (n/2 + 11).
		const TInt n = aProps[0] & 0x3f;
		const TUint32 dict = (n >= 40) ? 0xffffffffu
			: ((TUint32)(2 | (n & 1)) << (n / 2 + 11));
		ConvLogVal("7z lzma2 dict", (TInt)dict);
		if (dict > (TUint32)KMaxDict)
			{
			delete self;
			return KErrTooBig;
			}
		Lzma2Dec_Construct(&self->iLzma2);
		if (Lzma2Dec_Allocate(&self->iLzma2, aProps[0], &g_szAlloc) != SZ_OK)
			{
			delete self;
			ConvLogVal("7z dict oom", (TInt)dict);
			return KErrNoMemory;
			}
		Lzma2Dec_Init(&self->iLzma2);
		self->iInited = ETrue;
		}
	else if (aMethod != K_COPY)
		{
		delete self;
		return KErrNotSupported;
		}

	// Files within a folder are packed back-to-back: must read forward to reach target.
	if (aSkip > 0)
		{
		const TInt sr = self->Skip(aSkip);
		if (sr != KErrNone)
			{
			delete self;
			return sr;
			}
		}
	aOut = self;
	return KErrNone;
	}

C7zSrc::~C7zSrc()
	{
	if (iInited && iMethod == K_LZMA)
		LzmaDec_Free(&iLzma, &g_szAlloc);
	if (iInited && iMethod == K_LZMA2)
		Lzma2Dec_Free(&iLzma2, &g_szAlloc);
	User::Free(iIn);
	iFile.Close();
	}

// Fills input buffer from packed stream.
TInt C7zSrc::Pull(TUint8 *, TInt)
	{
	if (iInPos < iInFill)
		return iInFill - iInPos;
	const TInt left = iPackSize - iPackDone;
	if (left <= 0)
		return 0;
	TPtr8 p(iIn, KInBuf);
	const TInt want = Min(KInBuf, left);
	if (iFile.Read(iPackPos + iPackDone, p, want) != KErrNone)
		return KErrCorrupt;
	if (!p.Length())
		return 0;
	iPackDone += p.Length();
	iInFill = p.Length();
	iInPos = 0;
	return iInFill;
	}

TInt C7zSrc::Read(TUint8 *aBuf, TInt aLen)
	{
	if (iMethod == K_COPY)
		{
		const TInt left = iPackSize - iPackDone;
		const TInt n = Min(aLen, left);
		if (n <= 0)
			return 0;
		TPtr8 p(aBuf, n);
		if (iFile.Read(iPackPos + iPackDone, p, n) != KErrNone)
			return KErrCorrupt;
		iPackDone += p.Length();
		return p.Length();
		}

	TInt out = 0;
	while (out < aLen && !iEof)
		{
		const TInt avail = Pull(NULL, 0);
		if (avail < 0)
			return avail;

		SizeT dstLen = (SizeT)(aLen - out);
		SizeT srcLen = (SizeT)avail;
		ELzmaStatus st;
		SRes rc;
		if (iMethod == K_LZMA)
			rc = LzmaDec_DecodeToBuf(&iLzma, aBuf + out, &dstLen,
				iIn + iInPos, &srcLen, LZMA_FINISH_ANY, &st);
		else
			rc = Lzma2Dec_DecodeToBuf(&iLzma2, aBuf + out, &dstLen,
				iIn + iInPos, &srcLen, LZMA_FINISH_ANY, &st);
		if (rc != SZ_OK)
			return KErrCorrupt;
		iInPos += (TInt)srcLen;
		out += (TInt)dstLen;
		if (st == LZMA_STATUS_FINISHED_WITH_MARK ||
		    (!avail && !dstLen))
			{
			iEof = ETrue;
			break;
			}
		if (!srcLen && !dstLen)
			{
			iEof = ETrue;
			break;
			}
		}
	return out;
	}

TInt C7zSrc::Skip(TInt aBytes)
	{
	if (iMethod == K_COPY)
		{
		iPackDone += aBytes;
		return KErrNone;
		}
	const TInt KTmp = 64 * 1024;
	TUint8 *tmp = (TUint8 *)User::Alloc(KTmp);
	if (!tmp)
		return KErrNoMemory;
	TInt r = KErrNone;
	while (aBytes > 0)
		{
		const TInt n = Read(tmp, Min(KTmp, aBytes));
		if (n <= 0)
			{
			r = (n < 0) ? n : KErrEof;
			break;
			}
		aBytes -= n;
		}
	User::Free(tmp);
	return r;
	}

// --- Volume ------------------------------------------------------------------

struct T7zEnt
	{
	TInt iPackPos, iPackSize;
	TInt iMethod, iPropsOffs, iPropsSize;
	TInt iSkip;        // bytes from start of folder to this file
	TInt iSize;
	};

class C7zVol : public CBase, public MConvVol
	{
public:
	static TInt New(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);
	~C7zVol();
	const MDesCArray &Entries() const { return *iNames; }
	TInt Open(const TDesC &aName, MConvSrc *&aOut);
	TInt Size(const TDesC &aName, TInt &aOut);
private:
	TInt ScanL(RFs &aFs, const TDesC &aPath);
	TInt Find(const TDesC &aName) const;
	RFs *iFs;
	TFileName iPath;
	CDesCArray *iNames;
	RArray<T7zEnt> iEnts;
	TUint8 *iProps;    // copy of CodersData: db is freed immediately after parsing
	TInt iPropsLen;
	};

C7zVol::~C7zVol()
	{
	delete iNames;
	iEnts.Close();
	User::Free(iProps);
	}

TInt C7zVol::Find(const TDesC &aName) const
	{
	for (TInt i = 0; i < iNames->MdcaCount(); i++)
		if ((*iNames)[i].CompareF(aName) == 0)
			return i;
	return KErrNotFound;
	}

TInt C7zVol::ScanL(RFs &aFs, const TDesC &aPath)
	{
	RFile file;
	TInt r = file.Open(aFs, aPath, EFileRead | EFileShareReadersOnly);
	if (r != KErrNone)
		return r;

	TSzStream ss;
	ss.vt.Read = szRead;
	ss.vt.Seek = szSeek;
	ss.iFile = &file;
	ss.iPos = 0;

	CLookToRead2 look;
	LookToRead2_CreateVTable(&look, False);
	look.buf = (Byte *)User::Alloc(1 << 15);
	if (!look.buf)
		{
		file.Close();
		return KErrNoMemory;
		}
	look.bufSize = 1 << 15;
	look.realStream = &ss.vt;
	LookToRead2_INIT(&look)

	CrcGenerateTable();
	CSzArEx db;
	SzArEx_Init(&db);
	const SRes res = SzArEx_Open(&db, &look.vt, &g_szAlloc, &g_szAlloc);
	if (res != SZ_OK)
		{
		SzArEx_Free(&db, &g_szAlloc);
		User::Free(look.buf);
		file.Close();
		return (res == SZ_ERROR_MEM) ? KErrNoMemory : KErrCorrupt;
		}

	// Coder descriptors must outlive db: after Free its pointers become dangling.
	iPropsLen = (TInt)db.db.FoCodersOffsets[db.db.NumFolders];
	iProps = (TUint8 *)User::Alloc(iPropsLen ? iPropsLen : 1);
	if (iProps)
		memcpy(iProps, db.db.CodersData, iPropsLen);

	for (TUint32 i = 0; i < db.NumFiles && iProps; i++)
		{
		if (SzArEx_IsDir(&db, i))
			continue;
		const TUint32 fo = db.FileToFolder[i];
		if (fo == (TUint32)-1)
			continue;

		CSzFolder folder;
		CSzData sd;
		sd.Data = db.db.CodersData + db.db.FoCodersOffsets[fo];
		sd.Size = db.db.FoCodersOffsets[(size_t)fo + 1] - db.db.FoCodersOffsets[fo];
		if (SzGetNextFolderItem(&folder, &sd) != SZ_OK)
			continue;
		if (folder.NumCoders != 1 || folder.NumPackStreams != 1)
			continue;                     // filter chain - not our case

		T7zEnt e;
		e.iMethod = (TInt)folder.Coders[0].MethodID;
		if (e.iMethod != K_LZMA && e.iMethod != K_LZMA2 && e.iMethod != K_COPY)
			continue;
		// PropsOffset counts from this folder's coder record, iProps holds them all.
		e.iPropsOffs = (TInt)(db.db.FoCodersOffsets[fo] + folder.Coders[0].PropsOffset);
		e.iPropsSize = folder.Coders[0].PropsSize;

		// SzArEx_GetFolderStreamPos is only declared in 7z.h with no body in SDK -
		// compute manually: start of data plus packed stream position.
		const TUint32 pk = db.db.FoStartPackStreamIndex[fo];
		const UInt64 packPos  = db.dataPos + db.db.PackPositions[pk];
		const UInt64 packSize = db.db.PackPositions[pk + 1] - db.db.PackPositions[pk];
		const UInt64 fileSize = SzArEx_GetFileSize(&db, i);
		const UInt64 skip     = db.UnpackPositions[i] -
		                        db.UnpackPositions[db.FolderToFile[fo]];
		// Offsets and sizes are TInt downstream: 2 GB and up would turn negative.
		const UInt64 KMaxSz = 0x7fffffff;
		if (packPos > KMaxSz || packSize > KMaxSz || fileSize > KMaxSz ||
		    skip > KMaxSz || packPos + packSize > KMaxSz)
			continue;
		e.iPackPos  = (TInt)packPos;
		e.iPackSize = (TInt)packSize;
		e.iSize     = (TInt)fileSize;
		e.iSkip     = (TInt)skip;

		TFileName name;
		// Ask for the length FIRST: given a destination the SDK writes the whole
		// name and takes no buffer size, so a fixed buffer is a heap overflow
		// waiting for an archive with a long path.
		const size_t len = SzArEx_GetFileNameUtf16(&db, i, NULL);
		if (len < 2 || len > 8192)
			continue;
		UInt16 *w = (UInt16 *)User::Alloc(len * sizeof(UInt16));
		if (!w)
			break;
		SzArEx_GetFileNameUtf16(&db, i, w);
		// SDK UTF-16 matches Symbian TText.
		name.Copy(TPtrC((const TText *)w, (TInt)len - 1).Left(name.MaxLength()));
		TInt s = name.LocateReverse('/');
		if (s < 0) s = name.LocateReverse('\\');
		if (s >= 0)
			name.Delete(0, s + 1);
		User::Free(w);
		if (!name.Length())
			continue;

		// Trapped: leaving here would strand look.buf and the archive database.
		TRAPD(err, iNames->AppendL(name));
		if (err == KErrNone) err = iEnts.Append(e);
		if (err != KErrNone)
			{
			SzArEx_Free(&db, &g_szAlloc);
			User::Free(look.buf);
			file.Close();
			return err;
			}
		}

	SzArEx_Free(&db, &g_szAlloc);
	User::Free(look.buf);
	file.Close();
	return iNames->MdcaCount() ? KErrNone : KErrNotSupported;
	}

TInt C7zVol::New(RFs &aFs, const TDesC &aPath, MConvVol *&aOut)
	{
	C7zVol *self = new C7zVol;
	if (!self)
		return KErrNoMemory;
	self->iFs = &aFs;
	self->iPath = aPath;
	self->iNames = new CDesCArrayFlat(4);
	if (!self->iNames)
		{
		delete self;
		return KErrNoMemory;
		}
	TInt r = KErrNone;
	TRAPD(err, r = self->ScanL(aFs, aPath));
	if (err != KErrNone)
		r = err;
	if (r != KErrNone)
		{
		delete self;
		return r;
		}
	aOut = self;
	return KErrNone;
	}

TInt C7zVol::Open(const TDesC &aName, MConvSrc *&aOut)
	{
	const TInt i = Find(aName);
	if (i < 0)
		return KErrNotFound;
	const T7zEnt &e = iEnts[i];
	return C7zSrc::New(*iFs, iPath, e.iPackPos, e.iPackSize, e.iMethod,
		iProps + e.iPropsOffs, e.iPropsSize, e.iSkip, e.iSize, aOut);
	}

TInt C7zVol::Size(const TDesC &aName, TInt &aOut)
	{
	const TInt i = Find(aName);
	if (i < 0)
		return KErrNotFound;
	aOut = iEnts[i].iSize;
	return KErrNone;
	}

TInt SzOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut)
	{
	return C7zVol::New(aFs, aPath, aOut);
	}
