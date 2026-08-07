// Читання архівів як тому для імпорту: zip (DEFLATE через ту саму zlib, що й
// образи), 7z (LZMA SDK) і rar. Розпаковується потоково - образ на 700 МБ у
// пам'ять апарата не поміщається, а тимчасовий файл вимагав би вільного місця
// під розпакований образ ПЛЮС стиснений результат.

#include <e32base.h>
#include <f32file.h>
#include <badesca.h>
#include <string.h>

#include "imgconv.h"
#include "zlib.h"

// --- Спільне: файл із випадковим доступом ------------------------------------

class TArcFile
	{
public:
	TInt Open(RFs &aFs, const TDesC &aPath);
	void Close() { iFile.Close(); }
	TInt ReadAt(TInt aPos, TUint8 *aBuf, TInt aLen);
	TInt Size() const { return iSize; }
	RFile &File() { return iFile; }
private:
	RFile iFile;
	TInt iSize;
	};

TInt TArcFile::Open(RFs &aFs, const TDesC &aPath)
	{
	const TInt r = iFile.Open(aFs, aPath, EFileRead | EFileShareReadersOnly);
	if (r != KErrNone)
		return r;
	iFile.Size(iSize);
	return KErrNone;
	}

TInt TArcFile::ReadAt(TInt aPos, TUint8 *aBuf, TInt aLen)
	{
	TPtr8 p(aBuf, aLen);
	const TInt r = iFile.Read(aPos, p, aLen);
	return (r == KErrNone) ? p.Length() : r;
	}

static TUint32 rd32(const TUint8 *p)
	{
	return (TUint32)p[0] | ((TUint32)p[1] << 8) | ((TUint32)p[2] << 16) |
	       ((TUint32)p[3] << 24);
	}
static TUint16 rd16(const TUint8 *p)
	{
	return (TUint16)((TUint16)p[0] | ((TUint16)p[1] << 8));
	}

// --- ZIP ---------------------------------------------------------------------

struct TZipEnt
	{
	TInt iMethod;
	TInt iComp;
	TInt iSize;
	TInt iLocal;      // зміщення локального заголовка
	};

class CZipSrc : public CBase, public MConvSrc
	{
public:
	static TInt New(RFs &aFs, const TDesC &aPath, const TZipEnt &aEnt, MConvSrc *&aOut);
	~CZipSrc();
	TInt Read(TUint8 *aBuf, TInt aLen);
	TInt Skip(TInt aBytes);
	TInt Size() const { return iEnt.iSize; }
private:
	TArcFile iF;
	TZipEnt iEnt;
	TInt iPos;        // прочитано стисненого
	z_stream iZ;
	TBool iZOk;
	TUint8 *iBuf;
	};

const TInt KZipBuf = 32 * 1024;

static voidpf arcAlloc(voidpf, uInt aItems, uInt aSize)
	{
	return (voidpf)User::Alloc((TInt)(aItems * aSize));
	}
static void arcFree(voidpf, voidpf aPtr) { User::Free(aPtr); }

TInt CZipSrc::New(RFs &aFs, const TDesC &aPath, const TZipEnt &aEnt, MConvSrc *&aOut)
	{
	CZipSrc *self = new CZipSrc;
	if (!self)
		return KErrNoMemory;
	self->iEnt = aEnt;
	TInt r = self->iF.Open(aFs, aPath);
	if (r != KErrNone)
		{
		delete self;
		return r;
		}

	// Локальний заголовок повторює довжини імені й додаткового поля, і саме вони
	// задають, де насправді починаються дані: у центральному каталозі вони інші.
	TUint8 lh[30];
	if (self->iF.ReadAt(aEnt.iLocal, lh, sizeof(lh)) != (TInt)sizeof(lh) ||
	    rd32(lh) != 0x04034b50)
		{
		delete self;
		return KErrCorrupt;
		}
	self->iEnt.iLocal = aEnt.iLocal + 30 + rd16(lh + 26) + rd16(lh + 28);

	self->iBuf = (TUint8 *)User::Alloc(KZipBuf);
	if (!self->iBuf)
		{
		delete self;
		return KErrNoMemory;
		}
	if (aEnt.iMethod == 8)
		{
		self->iZ.zalloc = arcAlloc;
		self->iZ.zfree = arcFree;
		self->iZ.opaque = NULL;
		self->iZ.next_in = NULL;
		self->iZ.avail_in = 0;
		if (inflateInit2(&self->iZ, -15) != Z_OK)
			{
			delete self;
			return KErrNoMemory;
			}
		self->iZOk = ETrue;
		}
	else if (aEnt.iMethod != 0)
		{
		delete self;
		return KErrNotSupported;
		}
	aOut = self;
	return KErrNone;
	}

CZipSrc::~CZipSrc()
	{
	if (iZOk)
		inflateEnd(&iZ);
	User::Free(iBuf);
	iF.Close();
	}

TInt CZipSrc::Read(TUint8 *aBuf, TInt aLen)
	{
	if (iEnt.iMethod == 0)
		{
		const TInt left = iEnt.iComp - iPos;
		const TInt n = Min(aLen, left);
		if (n <= 0)
			return 0;
		const TInt r = iF.ReadAt(iEnt.iLocal + iPos, aBuf, n);
		if (r > 0)
			iPos += r;
		return r;
		}

	iZ.next_out = aBuf;
	iZ.avail_out = aLen;
	while (iZ.avail_out)
		{
		if (!iZ.avail_in)
			{
			const TInt left = iEnt.iComp - iPos;
			if (left <= 0)
				break;
			const TInt n = iF.ReadAt(iEnt.iLocal + iPos, iBuf, Min(KZipBuf, left));
			if (n <= 0)
				return (n < 0) ? n : KErrCorrupt;
			iPos += n;
			iZ.next_in = iBuf;
			iZ.avail_in = n;
			}
		const TInt zr = inflate(&iZ, Z_NO_FLUSH);
		if (zr == Z_STREAM_END)
			break;
		if (zr != Z_OK)
			return KErrCorrupt;
		}
	return aLen - (TInt)iZ.avail_out;
	}

TInt CZipSrc::Skip(TInt aBytes)
	{
	if (iEnt.iMethod == 0)
		{
		iPos += aBytes;
		return KErrNone;
		}
	// Стиснений потік назад не мотається: пропуск - це читання з викиданням.
	TUint8 *tmp = (TUint8 *)User::Alloc(KZipBuf);
	if (!tmp)
		return KErrNoMemory;
	TInt r = KErrNone;
	while (aBytes > 0)
		{
		const TInt n = Read(tmp, Min(KZipBuf, aBytes));
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

class CZipVol : public CBase, public MConvVol
	{
public:
	static TInt New(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);
	~CZipVol();
	const MDesCArray &Entries() const { return *iNames; }
	TInt Open(const TDesC &aName, MConvSrc *&aOut);
	TInt Size(const TDesC &aName, TInt &aOut);
private:
	TInt ScanL(RFs &aFs, const TDesC &aPath);
	TInt Find(const TDesC &aName) const;
	RFs *iFs;
	TFileName iPath;
	CDesCArray *iNames;
	RArray<TZipEnt> iEnts;
	};

CZipVol::~CZipVol()
	{
	delete iNames;
	iEnts.Close();
	}

TInt CZipVol::Find(const TDesC &aName) const
	{
	for (TInt i = 0; i < iNames->MdcaCount(); i++)
		if ((*iNames)[i].CompareF(aName) == 0)
			return i;
	return KErrNotFound;
	}

TInt CZipVol::ScanL(RFs &aFs, const TDesC &aPath)
	{
	TArcFile f;
	TInt r = f.Open(aFs, aPath);
	if (r != KErrNone)
		return r;

	// Кінцевий запис каталогу лежить у хвості й має змінну довжину коментаря,
	// тож його шукають скануванням назад.
	const TInt KTail = 66 * 1024;
	const TInt tail = Min(KTail, f.Size());
	TUint8 *buf = (TUint8 *)User::Alloc(tail);
	if (!buf)
		{
		f.Close();
		return KErrNoMemory;
		}
	const TInt base = f.Size() - tail;
	TInt got = f.ReadAt(base, buf, tail);
	TInt eocd = -1;
	for (TInt i = got - 22; i >= 0; i--)
		if (rd32(buf + i) == 0x06054b50)
			{ eocd = i; break; }
	if (eocd < 0)
		{
		User::Free(buf);
		f.Close();
		return KErrCorrupt;
		}
	const TInt count = rd16(buf + eocd + 10);
	const TInt cdSize = (TInt)rd32(buf + eocd + 12);
	const TInt cdOff  = (TInt)rd32(buf + eocd + 16);
	User::Free(buf);

	if (cdSize <= 0 || cdOff < 0 || cdSize > 8 * 1024 * 1024)
		{
		f.Close();
		return KErrCorrupt;
		}
	TUint8 *cd = (TUint8 *)User::Alloc(cdSize);
	if (!cd)
		{
		f.Close();
		return KErrNoMemory;
		}
	got = f.ReadAt(cdOff, cd, cdSize);
	f.Close();
	if (got != cdSize)
		{
		User::Free(cd);
		return KErrCorrupt;
		}

	TInt p = 0;
	for (TInt i = 0; i < count && p + 46 <= cdSize; i++)
		{
		if (rd32(cd + p) != 0x02014b50)
			break;
		const TInt nlen = rd16(cd + p + 28);
		const TInt elen = rd16(cd + p + 30);
		const TInt clen = rd16(cd + p + 32);
		TZipEnt e;
		e.iMethod = rd16(cd + p + 10);
		e.iComp   = (TInt)rd32(cd + p + 20);
		e.iSize   = (TInt)rd32(cd + p + 24);
		e.iLocal  = (TInt)rd32(cd + p + 42);
		TFileName name;
		TPtrC8 raw(cd + p + 46, Min(nlen, name.MaxLength()));
		name.Copy(raw);
		// Тек усередині архіву не розгортаємо: беремо базове ім'я, бо .cue
		// посилається на сусідні файли теж без шляху.
		TInt s = name.LocateReverse('/');
		if (s < 0) s = name.LocateReverse('\\');
		if (s >= 0)
			name.Delete(0, s + 1);
		if (name.Length() && e.iSize >= 0)
			{
			iNames->AppendL(name);
			iEnts.AppendL(e);
			}
		p += 46 + nlen + elen + clen;
		}
	User::Free(cd);
	return iNames->MdcaCount() ? KErrNone : KErrCorrupt;
	}

TInt CZipVol::New(RFs &aFs, const TDesC &aPath, MConvVol *&aOut)
	{
	CZipVol *self = new CZipVol;
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

TInt CZipVol::Open(const TDesC &aName, MConvSrc *&aOut)
	{
	const TInt i = Find(aName);
	if (i < 0)
		return KErrNotFound;
	return CZipSrc::New(*iFs, iPath, iEnts[i], aOut);
	}

TInt CZipVol::Size(const TDesC &aName, TInt &aOut)
	{
	const TInt i = Find(aName);
	if (i < 0)
		return KErrNotFound;
	aOut = iEnts[i].iSize;
	return KErrNone;
	}

// --- Вибір формату -----------------------------------------------------------

static TBool endsWith(const TDesC &aName, const TDesC &aExt)
	{
	if (aName.Length() < aExt.Length())
		return EFalse;
	TBuf<8> e;
	e.Copy(aName.Right(aExt.Length()));
	e.LowerCase();
	return e == aExt;
	}

TInt ArcOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut)
	{
	if (endsWith(aPath, _L(".zip")))
		return CZipVol::New(aFs, aPath, aOut);
	if (endsWith(aPath, _L(".7z")))
		return SzOpenVol(aFs, aPath, aOut);
	if (endsWith(aPath, _L(".rar")))
		return RarOpenVol(aFs, aPath, aOut);
	return KErrNotSupported;
	}
