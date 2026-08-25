// RAR volume: RAR4 and RAR5 header parsing, reading entries stored WITHOUT
// compression.
//
// Compressed entries are deliberately not supported. The only RAR decompression implementation
// is unRAR by RARLAB, whose license forbids using the code to create a compatible
// archiver; this restriction is incompatible with GPL-2.0, under which
// libpcsxcore (linked with this application) is licensed. Thus, building such a binary
// is possible, but distributing it is not. For compressed archives, the application
// prompts repacking into .7z, which is fully supported.

#include <e32base.h>
#include <f32file.h>
#include <badesca.h>
#include <string.h>

#include "imgconv.h"

struct TRarEnt
	{
	TInt iPos;      // data offset in file
	TInt iSize;
	TBool iStored;
	};

class CRarSrc : public CBase, public MConvSrc
	{
public:
	static TInt New(RFs &aFs, const TDesC &aPath, const TRarEnt &aEnt, MConvSrc *&aOut);
	~CRarSrc() { iFile.Close(); }
	TInt Read(TUint8 *aBuf, TInt aLen);
	TInt Skip(TInt aBytes) { iPos += aBytes; return KErrNone; }
	TInt Size() const { return iEnt.iSize; }
private:
	RFile iFile;
	TRarEnt iEnt;
	TInt iPos;
	};

TInt CRarSrc::New(RFs &aFs, const TDesC &aPath, const TRarEnt &aEnt, MConvSrc *&aOut)
	{
	if (!aEnt.iStored)
		return KErrNotSupported;
	CRarSrc *self = new CRarSrc;
	if (!self)
		return KErrNoMemory;
	self->iEnt = aEnt;
	const TInt r = self->iFile.Open(aFs, aPath, EFileRead | EFileShareReadersOnly);
	if (r != KErrNone)
		{
		delete self;
		return r;
		}
	aOut = self;
	return KErrNone;
	}

TInt CRarSrc::Read(TUint8 *aBuf, TInt aLen)
	{
	const TInt left = iEnt.iSize - iPos;
	const TInt n = Min(aLen, left);
	if (n <= 0)
		return 0;
	TPtr8 p(aBuf, n);
	if (iFile.Read(iEnt.iPos + iPos, p, n) != KErrNone)
		return KErrCorrupt;
	iPos += p.Length();
	return p.Length();
	}

// --- Parsing -----------------------------------------------------------------

static TUint32 rd32(const TUint8 *p)
	{
	return (TUint32)p[0] | ((TUint32)p[1] << 8) | ((TUint32)p[2] << 16) |
	       ((TUint32)p[3] << 24);
	}
static TUint16 rd16(const TUint8 *p)
	{
	return (TUint16)((TUint16)p[0] | ((TUint16)p[1] << 8));
	}

// RAR5 variable-length integer: 7 bits per byte, MSB is continuation flag.
static TInt vint(const TUint8 *aBuf, TInt aLen, TInt &aPos, TUint32 &aOut)
	{
	aOut = 0;
	TInt shift = 0;
	while (aPos < aLen && shift < 32)
		{
		const TUint8 b = aBuf[aPos++];
		aOut |= ((TUint32)(b & 0x7f)) << shift;
		if (!(b & 0x80))
			return KErrNone;
		shift += 7;
		}
	return KErrCorrupt;
	}

class CRarVol : public CBase, public MConvVol
	{
public:
	static TInt New(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);
	~CRarVol() { delete iNames; iEnts.Close(); }
	const MDesCArray &Entries() const { return *iNames; }
	TInt Open(const TDesC &aName, MConvSrc *&aOut);
	TInt Size(const TDesC &aName, TInt &aOut);
private:
	TInt ScanL(RFs &aFs, const TDesC &aPath);
	TInt Scan4L(RFile &aFile, TInt aPos, TInt aSize);
	TInt Scan5L(RFile &aFile, TInt aPos, TInt aSize);
	TInt Find(const TDesC &aName) const;
	void AddL(const TDesC &aName, const TRarEnt &aEnt);
	RFs *iFs;
	TFileName iPath;
	CDesCArray *iNames;
	RArray<TRarEnt> iEnts;
	};

TInt CRarVol::Find(const TDesC &aName) const
	{
	for (TInt i = 0; i < iNames->MdcaCount(); i++)
		if ((*iNames)[i].CompareF(aName) == 0)
			return i;
	return KErrNotFound;
	}

void CRarVol::AddL(const TDesC &aName, const TRarEnt &aEnt)
	{
	TFileName n = aName;
	TInt s = n.LocateReverse('\\');
	if (s < 0) s = n.LocateReverse('/');
	if (s >= 0)
		n.Delete(0, s + 1);
	if (!n.Length())
		return;
	iNames->AppendL(n);
	iEnts.AppendL(aEnt);
	}

TInt CRarVol::Scan4L(RFile &aFile, TInt aPos, TInt aSize)
	{
	TUint8 h[64];
	while (aPos + 7 <= aSize)
		{
		TPtr8 p(h, 7);
		if (aFile.Read(aPos, p, 7) != KErrNone || p.Length() < 7)
			break;
		const TInt type = h[2];
		const TInt flags = rd16(h + 3);
		TInt hsize = rd16(h + 5);
		if (hsize < 7)
			break;
		TInt dsize = 0;
		if (flags & 0x8000)
			{
			TUint8 d[4];
			TPtr8 pd(d, 4);
			if (aFile.Read(aPos + 7, pd, 4) != KErrNone || pd.Length() < 4)
				break;
			dsize = (TInt)rd32(d);
			}

		if (type == 0x74)               // file header
			{
			TUint8 *fh = (TUint8 *)User::Alloc(hsize);
			if (!fh)
				return KErrNoMemory;
			// AddL can leave; the header buffer would be stranded.
			CleanupStack::PushL(fh);
			TPtr8 pf(fh, hsize);
			if (aFile.Read(aPos, pf, hsize) == KErrNone && pf.Length() == hsize &&
			    hsize >= 32)
				{
				const TInt packSize = (TInt)rd32(fh + 7);
				const TInt unpSize  = (TInt)rd32(fh + 11);
				const TInt method   = fh[25];
				const TInt nameLen  = rd16(fh + 26);
				TInt off = 32;
				if (flags & 0x100)      // 64-bit sizes
					off += 8;
				if (off + nameLen <= hsize && nameLen > 0)
					{
					TFileName name;
					// RAR4 filename is single-byte unless the UNICODE flag is set;
					// in the unicode variant, the same ASCII name precedes it up
					// to the null byte, which is sufficient.
					TPtrC8 raw(fh + off, Min(nameLen, name.MaxLength()));
					const TInt z = raw.Locate('\0');
					name.Copy((z >= 0) ? raw.Left(z) : raw);
					TRarEnt e;
					e.iPos = aPos + hsize;
					e.iSize = unpSize;
					e.iStored = (method == 0x30) && (packSize == unpSize);
					// Directory in RAR4 is value 0xE0 in the dictionary size field
					// (flags bits 5-7), not a distinct block type.
					if ((flags & 0xE0) != 0xE0)
						AddL(name, e);
					}
				}
			CleanupStack::PopAndDestroy(fh);
			}

		const TInt step = hsize + dsize;
		if (step <= 0)
			break;
		aPos += step;
		}
	return KErrNone;
	}

TInt CRarVol::Scan5L(RFile &aFile, TInt aPos, TInt aSize)
	{
	while (aPos + 8 <= aSize)
		{
		TUint8 hdr[512];
		TPtr8 p(hdr, sizeof(hdr));
		const TInt want = Min((TInt)sizeof(hdr), aSize - aPos);
		if (aFile.Read(aPos, p, want) != KErrNone || p.Length() < 8)
			break;

		TInt q = 4;                     // header CRC32
		TUint32 hsize = 0, type = 0, flags = 0, extra = 0, dsize = 0;
		if (vint(hdr, p.Length(), q, hsize) != KErrNone)
			break;
		const TInt hstart = q;
		if (vint(hdr, p.Length(), q, type) != KErrNone ||
		    vint(hdr, p.Length(), q, flags) != KErrNone)
			break;
		if ((flags & 1) && vint(hdr, p.Length(), q, extra) != KErrNone)
			break;
		if ((flags & 2) && vint(hdr, p.Length(), q, dsize) != KErrNone)
			break;

		if (type == 2)                  // file
			{
			TUint32 fflags = 0, unpSize = 0, attr = 0, cinfo = 0, host = 0, nlen = 0;
			TInt r = vint(hdr, p.Length(), q, fflags);
			if (r == KErrNone) r = vint(hdr, p.Length(), q, unpSize);
			if (r == KErrNone) r = vint(hdr, p.Length(), q, attr);
			if (r == KErrNone && (fflags & 2)) q += 4;          // mtime
			if (r == KErrNone && (fflags & 4)) q += 4;          // crc32
			if (r == KErrNone) r = vint(hdr, p.Length(), q, cinfo);
			if (r == KErrNone) r = vint(hdr, p.Length(), q, host);
			if (r == KErrNone) r = vint(hdr, p.Length(), q, nlen);
			if (r == KErrNone && nlen > 0 && q + (TInt)nlen <= p.Length())
				{
				TFileName name;
				// Filenames in RAR5 are UTF-8; ASCII identical for Latin, and
				// Cyrillic in disc image names is not needed anyway.
				name.Copy(TPtrC8(hdr + q, Min((TInt)nlen,
					name.MaxLength())));
				TRarEnt e;
				e.iPos = aPos + hstart + (TInt)hsize;
				e.iSize = (TInt)unpSize;
				// Bits 7-9 are method; 0 means "uncompressed".
				e.iStored = (((cinfo >> 7) & 7) == 0);
				if (!(fflags & 1))       // not a directory
					AddL(name, e);
				}
			}

		const TInt step = hstart + (TInt)hsize + (TInt)dsize;
		if (step <= 0)
			break;
		aPos += step;
		}
	return KErrNone;
	}

TInt CRarVol::ScanL(RFs &aFs, const TDesC &aPath)
	{
	RFile file;
	TInt r = file.Open(aFs, aPath, EFileRead | EFileShareReadersOnly);
	if (r != KErrNone)
		return r;
	TInt size = 0;
	file.Size(size);

	TUint8 sig[8];
	TPtr8 p(sig, sizeof(sig));
	if (file.Read(0, p, sizeof(sig)) != KErrNone || p.Length() < 8)
		{
		file.Close();
		return KErrCorrupt;
		}
	const TBool rar = (sig[0] == 0x52 && sig[1] == 0x61 && sig[2] == 0x72 &&
	                   sig[3] == 0x21 && sig[4] == 0x1a && sig[5] == 0x07);
	if (!rar)
		{
		file.Close();
		return KErrCorrupt;
		}

	if (sig[6] == 0x01 && sig[7] == 0x00)
		r = Scan5L(file, 8, size);
	else if (sig[6] == 0x00)
		r = Scan4L(file, 7, size);
	else
		r = KErrNotSupported;

	file.Close();
	if (r != KErrNone)
		return r;
	return iNames->MdcaCount() ? KErrNone : KErrNotSupported;
	}

TInt CRarVol::New(RFs &aFs, const TDesC &aPath, MConvVol *&aOut)
	{
	CRarVol *self = new CRarVol;
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

TInt CRarVol::Open(const TDesC &aName, MConvSrc *&aOut)
	{
	const TInt i = Find(aName);
	if (i < 0)
		return KErrNotFound;
	return CRarSrc::New(*iFs, iPath, iEnts[i], aOut);
	}

TInt CRarVol::Size(const TDesC &aName, TInt &aOut)
	{
	const TInt i = Find(aName);
	if (i < 0)
		return KErrNotFound;
	aOut = iEnts[i].iSize;
	return KErrNone;
	}

TInt RarOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut)
	{
	return CRarVol::New(aFs, aPath, aOut);
	}
