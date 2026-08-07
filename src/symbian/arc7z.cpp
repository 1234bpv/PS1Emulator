// Том 7z для імпорту. Розпаковує потоково власним циклом, а не SzArEx_Extract:
// той віддає цілу теку архіву одним буфером у пам'яті, а тека тут - це образ
// диска на 700 МБ.
//
// Підтримується тека з одного кодера: LZMA, LZMA2 або без стиснення. Ланцюги з
// фільтрами (BCJ, BCJ2, дельта) відхиляються - на образах диска їх не буває, а
// потокова версія кожного з них коштувала б більше, ніж дає.

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

// Словник LZMA виділяється цілком і одразу. 64 МБ - стеля самого 7-Zip (режим
// "ультра"), тож більшого в архіві не буває; якщо стільки вільної пам'яті на
// апараті немає, User::Alloc відмовить сам, і це чесніше за KERN-EXEC.
const TInt KMaxDict = 64 * 1024 * 1024;

static void *szAlloc(ISzAllocPtr, size_t aSize) { return User::Alloc((TInt)aSize); }
static void szFree(ISzAllocPtr, void *aPtr) { User::Free(aPtr); }
static const ISzAlloc g_szAlloc = { szAlloc, szFree };

// --- Потік читання архіву для розбору заголовка ------------------------------

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

// --- Розпакувальник одного елемента ------------------------------------------

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
		const TInt dict = (aPropsSize < 5) ? 0
			: aProps[1] + (aProps[2] << 8) + (aProps[3] << 16) +
			  ((TInt)aProps[4] << 24);
		ConvLogVal("7z lzma dict", dict);
		if (aPropsSize < 5 || dict > KMaxDict)
			{
			delete self;
			return KErrTooBig;
			}
		LzmaDec_Construct(&self->iLzma);
		if (LzmaDec_Allocate(&self->iLzma, aProps, (unsigned)aPropsSize,
		                     &g_szAlloc) != SZ_OK)
			{
			delete self;
			ConvLogVal("7z dict oom", dict);
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
		// Байт властивостей LZMA2 кодує словник як 2|3 << (n/2 + 11).
		const TInt n = aProps[0] & 0x3f;
		const TInt dict = (n > 40) ? 0x7fffffff
			: (n == 40) ? 0xffffffff
			: ((2 | (n & 1)) << (n / 2 + 11));
		ConvLogVal("7z lzma2 dict", dict);
		if (dict > KMaxDict)
			{
			delete self;
			return KErrTooBig;
			}
		Lzma2Dec_Construct(&self->iLzma2);
		if (Lzma2Dec_Allocate(&self->iLzma2, aProps[0], &g_szAlloc) != SZ_OK)
			{
			delete self;
			ConvLogVal("7z dict oom", dict);
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

	// Файли всередині теки лежать упритул: до потрібного доводиться дочитатись.
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

// Наповнює вхідний буфер із запакованого потоку.
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

// --- Том ---------------------------------------------------------------------

struct T7zEnt
	{
	TInt iPackPos, iPackSize;
	TInt iMethod, iPropsOffs, iPropsSize;
	TInt iSkip;        // байтів від початку теки до цього файлу
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
	TUint8 *iProps;    // копія CodersData: db звільняється одразу після розбору
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

	// Опис кодерів переживає db: після Free покажчики в ньому стають чужими.
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
			continue;                     // ланцюг фільтрів - не наш випадок

		T7zEnt e;
		e.iMethod = (TInt)folder.Coders[0].MethodID;
		if (e.iMethod != K_LZMA && e.iMethod != K_LZMA2 && e.iMethod != K_COPY)
			continue;
		e.iPropsOffs = (TInt)folder.Coders[0].PropsOffset;
		e.iPropsSize = folder.Coders[0].PropsSize;

		// SzArEx_GetFolderStreamPos лише оголошено в 7z.h, а тіла в SDK немає -
		// рахуємо самі: початок даних плюс позиція запакованого потоку.
		const TUint32 pk = db.db.FoStartPackStreamIndex[fo];
		e.iPackPos  = (TInt)(db.dataPos + db.db.PackPositions[pk]);
		e.iPackSize = (TInt)(db.db.PackPositions[pk + 1] - db.db.PackPositions[pk]);
		e.iSize     = (TInt)SzArEx_GetFileSize(&db, i);
		e.iSkip     = (TInt)(db.UnpackPositions[i] -
		                     db.UnpackPositions[db.FolderToFile[fo]]);

		TFileName name;
		UInt16 *w = (UInt16 *)User::Alloc(512 * sizeof(UInt16));
		if (!w)
			break;
		const size_t len = SzArEx_GetFileNameUtf16(&db, i, w);
		if (len > 1 && len < 512)
			{
			// UTF-16 SDK збігається з TText на Symbian.
			name.Copy(TPtrC((const TText *)w, (TInt)len - 1).Left(name.MaxLength()));
			TInt s = name.LocateReverse('/');
			if (s < 0) s = name.LocateReverse('\\');
			if (s >= 0)
				name.Delete(0, s + 1);
			}
		User::Free(w);
		if (!name.Length())
			continue;

		iNames->AppendL(name);
		iEnts.AppendL(e);
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
