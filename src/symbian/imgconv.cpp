// Імпорт ігор: розбір .cue, збирання образу в PBP (ECC обнулено) або перенесення
// як є. Геометрія доріжок навмисно повторює parsecue() з libpcsxcore/cdriso.c -
// TOC у PBP має давати рівно ту саму таблицю ti[], інакше гра з CD-аудіо після
// конвертації шукала б доріжки не там.

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
const TInt KPbpBlk   = 16;                    // секторів у блоці PBP (block_shift=4)
const TInt KBlkBytes = KSector * KPbpBlk;     // 37632
const TInt KPsar     = 0x28;
const TInt KDataOffs = KPsar + 0x100000;
const TInt KMaxTrk   = 100;
// (0x100000-0x4000)/32 елементів індексу - стеля, зашита у формат, не наша.
const TInt KMaxIndex = (0x100000 - 0x4000) / 32;

// --- Том: тека на диску -----------------------------------------------------

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

// --- Розпізнавання імен -----------------------------------------------------

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

// Створює том. Архів обслуговує archive.cpp, звичайний файл - тека поруч із ним.
TInt ConvOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut)
	{
	if (ConvIsArchiveName(aPath))
		return ArcOpenVol(aFs, aPath, aOut);
	return CDirVol::New(aFs, aPath, aOut);
	}

// --- Розбір .cue ------------------------------------------------------------

struct TTrk
	{
	TInt iType;        // 0 - дані, 1 - аудіо
	TInt iIdx0, iIdx1; // сектори всередині файлу доріжки, -1 якщо немає
	TInt iPregap, iPostgap;
	TInt iSecSize;
	TInt iFile;        // індекс імені у списку файлів
	TBool iFirstInFile;
	TInt iStart;       // абсолютний сектор (MSF), як ti[].start
	TInt iLen;
	};

struct TCue
	{
	TInt iCount;
	TTrk iTrk[KMaxTrk];
	CDesCArray *iFiles;
	};

static TInt msf2sec(TInt m, TInt s, TInt f) { return (m * 60 + s) * 75 + f; }

// "mm:ss:ff" -> сектори. Повертає -1, якщо це не час.
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

// Ім'я у лапках після FILE; без лапок - до кінця слова.
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
	// Нульовий елемент - не доріжка, а джерело postgap для першої: completeCue
	// читає iTrk[i-1] і на i==1 потрапляє сюди. Без цього рядка в пролог першої
	// доріжки йшло сміття з невиділеної пам'яті, iStart ставав величезним, і
	// конвертер починав добивати образ тишею, поки не скінчиться картка.
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
			// Шлях усередині .cue ігноруємо: файли доріжок лежать поруч із ним,
			// а абсолютний шлях із чужої машини на апараті все одно не існує.
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
			TTrk &t = aCue.iTrk[++aCue.iCount];
			t.iType = 0;
			t.iIdx0 = t.iIdx1 = -1;
			t.iPregap = t.iPostgap = 0;
			t.iSecSize = KSector;
			t.iFile = aCue.iFiles->MdcaCount() - 1;
			t.iFirstInFile = pendingFile;
			t.iStart = t.iLen = 0;
			pendingFile = EFalse;
			nextTok(line, lp, tok);          // номер
			nextTok(line, lp, tok);          // режим
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

// Добудова таблиці: та сама арифметика, що в parsecue (cdriso.c:530). Свій
// варіант тут не годиться - PBP має відтворювати рівно ті адреси, які ядро
// порахувало б із вихідного .cue, інакше аудіодоріжки поїдуть.
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
		// Проміжок, якого немає у файлах, доводиться дописувати тишею, тож
		// безглузде значення тут коштує місця на картці. Справжні прологи -
		// секунди; півтори хвилини вже означають, що .cue розібрано хибно.
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

// --- Обнулення ECC ----------------------------------------------------------

// ECC читає лише апаратура приводу, тож емулятор його не помічає, а рівний нуль
// DEFLATE стискає майже задарма (заміряно: 65,4% -> 58,5%). Незворотно.
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
		// MODE2/FORM2 (біт 0x20 у підзаголовку) ECC не має взагалі, а його дані
		// тягнуться до 2347 - зачистка там знищила б звук і відео.
		if (mode == 2 && (s[18] & 0x20))
			continue;
		if (mode != 1 && mode != 2)
			continue;
		memset(aBlk + off + 2076, 0, 276);
		n++;
		}
	return n;
	}

// --- Лог робочого потоку -----------------------------------------------------

// Власний файл і власна RFs: описувачі файлів у Symbian через потоки не
// переносяться, а FILE* із ESTLIB головного потоку тут узагалі непридатний.
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

// Той самий лог із інших файлів імпорту (arc7z): без нього відмова в пам'яті
// всередині розпакувальника не відрізняється від будь-якої іншої -4.
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

// --- Стан роботи ------------------------------------------------------------

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
	aOut = g_stat;
	if (g_lockOk) g_lock.Signal();
	}

void ConvCancel() { g_cancel = 1; }

// --- Запис PBP --------------------------------------------------------------

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

// zlib у робочому потоці бере пам'ять напряму в ядра: ESTLIB тут не ініціалізована.
static voidpf zAlloc(voidpf, uInt aItems, uInt aSize)
	{
	return (voidpf)User::Alloc((TInt)(aItems * aSize));
	}
static void zFree(voidpf, voidpf aPtr) { User::Free(aPtr); }

struct TPbpWriter
	{
	RFile iOut;
	TUint32 *iIndex;      // пари (offset, size)
	TInt iBlocks;
	TInt iOff;            // зміщення від cdimg_base
	TUint8 *iBlk;
	TInt iFill;           // байтів у iBlk
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
	if (w.iBlocks >= KMaxIndex)
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

// Тиша в проміжку між доріжками: сектори, яких у вихідних файлах немає (PREGAP).
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

	// TOC: елемент 10 байтів (усі поля char, без вирівнювання). Перші три
	// службові: пропуск, кількість доріжок, довжина диска.
	const TInt nent = 3 + aCue.iCount;
	TUint8 *toc = (TUint8 *)User::Alloc(nent * 10);
	if (!toc)
		return KErrNoMemory;
	memset(toc, 0, nent * 10);
	// Читач бере обидва поля через btoi(), тобто чекає BCD.
	toc[10 + 7] = (TUint8)(((aCue.iCount / 10) << 4) | (aCue.iCount % 10));
	msfBcd(aTotalSectors, toc + 20 + 7);                  // cd_length
	for (TInt i = 1; i <= aCue.iCount; i++)
		{
		TUint8 *e = toc + (2 + i) * 10;
		e[0] = (TUint8)(aCue.iTrk[i].iType == 1 ? 1 : 2);
		e[2] = (TUint8)(((i / 10) << 4) | (i % 10));
		msfBcd(aCue.iTrk[i].iStart - 2 * 75, e + 3);      // index0: місце в образі
		msfBcd(aCue.iTrk[i].iStart, e + 7);               // index1: адреса на диску
		}
	pos = KPsar + 0x800;
	r = w.iOut.Seek(ESeekStart, pos);
	if (r == KErrNone)
		r = w.iOut.Write(TPtrC8(toc, nent * 10));
	User::Free(toc);
	if (r != KErrNone)
		return r;

	// Індекс блоків: 32 байти на запис, завершувач із size==0 несе кінець даних.
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

// --- Одна конвертація -------------------------------------------------------

// Ім'я призначення: базове ім'я джерела + .pbp у теці ігор.
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
	// Обрізаємо лише під довжину шляху, а не під кругле число: 40 символів різало
	// назви посеред слова ("...of the Night (USA" без дужки), хоча місця вистачало.
	const TInt room = aOut.MaxLength() - aDir.Length() - aExt.Length();
	if (room > 0 && n.Length() > room)
		n.SetLength(room);
	aOut = aDir;
	aOut.Append(n);
	aOut.Append(aExt);
	}

// Вибирає в томі те, що варто конвертувати: .cue має перевагу над .bin, бо
// несе розкладку доріжок; інакше перший образ.
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

// Копіювання елемента (стиснути не вийшло або користувач так попросив).
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
	User::Free(buf);
	out.Close();
	delete src;
	if (g_cancel && r == KErrNone)
		r = KErrCancel;
	if (r != KErrNone)
		aFs.Delete(aDstPath);
	return r;
	}

// Збирає образ у PBP. Джерело читається послідовно по доріжках; проміжки, яких
// у файлах немає, добиваються тишею, щоб місце сектора в образі дорівнювало
// його адресі на диску мінус 150 - лише так TOC лишається несуперечливим.
static TInt convertToPbp(RFs &aFs, MConvVol &aVol, const TDesC &aDstPath, TCue &aCue)
	{
	TPbpWriter w;
	memset(&w, 0, sizeof(w));
	TInt r = w.iOut.Replace(aFs, aDstPath, EFileWrite | EFileShareExclusive);
	if (r != KErrNone)
		return r;
	// Дані починаються рівно на psar+0x100000; зміщення в індексі рахуються від
	// цієї точки, тож iOff лишається нулем.
	//
	// SetSize перед Seek обов'язковий: Seek обрізає позицію до наявного розміру
	// файлу, тож у щойно створеному файлі перехід на 0x100028 мовчки лишав би
	// позицію на нулі, і заголовок згодом затирав би початок даних.
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
		// -15: сирий DEFLATE без заголовка zlib - рівно те, що чекає
		// uncompress2_pcsx() (inflateInit2(&z, -15) у cdriso.c).
		if (deflateInit2(&w.iZ, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
			r = KErrNoMemory;
		}

	TInt total = 0;
	for (TInt i = 1; i <= aCue.iCount && r == KErrNone; i++)
		total += aCue.iTrk[i].iLen;

	TInt curFile = -1;
	MConvSrc *src = NULL;
	TInt srcPos = 0;         // позиція в поточному файлі, сектори
	TInt written = 0;        // секторів в образі

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
			const TInt n = src->Read(sec, KSector);
			if (n < 0) { r = n; break; }
			if (n < KSector)
				memset(sec + (n > 0 ? n : 0), 0, KSector - (n > 0 ? n : 0));
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

// Готує TCue: із .cue поруч або синтетичну однодоріжкову для голого образу.
static TInt buildCue(MConvVol &aVol, const TDesC &aEntry, TCue &aCue)
	{
	aCue.iCount = 0;
	if (extIs(aEntry, _L(".cue")))
		{
		MConvSrc *src = NULL;
		TInt r = aVol.Open(aEntry, src);
		if (r != KErrNone)
			return r;
		const TInt sz = Min(src->Size(), 64 * 1024);
		TUint8 *buf = (TUint8 *)User::Alloc(sz + 1);
		if (!buf)
			{
			delete src;
			return KErrNoMemory;
			}
		const TInt n = src->Read(buf, sz);
		delete src;
		if (n <= 0)
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

	// Поруч може лежати .cue із тим самим іменем - саме так ядро й шукає його
	// (parsecue замінює розширення). Без цієї перевірки гра з CD-аудіо, вибрана
	// за .bin, втратила б усі доріжки, крім першої.
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

	// Голий образ: одна доріжка даних від нульового сектора.
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

// Видаляє джерело після вдалого імпорту: архів цілком або .cue разом із усіма
// файлами доріжок, на які він посилається.
static void deleteSource(RFs &aFs, const TDesC &aPath, const TCue &aCue)
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
		TFileName p(dir);
		if (p.Length() + aCue.iFiles->MdcaPoint(i).Length() < p.MaxLength())
			{
			p.Append(aCue.iFiles->MdcaPoint(i));
			aFs.Delete(p);
			}
		}
	aFs.Delete(aPath);
	// Користувач міг позначити .bin, а розкладку ми взяли із сусіднього .cue -
	// лишити його самого означало б лишити биту вказівку на видалений файл.
	TFileName cue = aPath;
	const TInt dot = cue.LocateReverse('.');
	if (dot > 0)
		{
		cue.SetLength(dot);
		cue.Append(_L(".cue"));
		aFs.Delete(cue);
		}
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

	// TCue - це масив на сто доріжок; на 16-кілобайтному стеку робочого потоку
	// йому не місце.
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
			// Майже завжди це словник LZMA: він виділяється цілком, до першого байта.
			aMsg.Copy(_L("archive needs too much RAM"));
		else if (r == KErrNotSupported)
			{
			// 2048-байтові сектори чи нестандартний .cue: PBP такого не виражає,
			// тож кладемо як є - гра лишається робочою, місця не заощаджуємо.
			aMsg.Copy(_L("stored as is"));
			dstName(entry, aDstDir, entry.Right(4), dst);
			TInt sz = 0;
			vol->Size(entry, sz);
			r = copyEntry(aFs, *vol, entry, dst, sz);
			}
		}
	else
		{
		// Із .cue переносимо і його самого, і всі файли доріжок: без будь-якого
		// з них розкладка губиться, а гра перестає читатись.
		r = buildCue(*vol, entry, *cue);
		if (r == KErrNone || r == KErrNotSupported)
			{
			r = KErrNone;
			if (extIs(entry, _L(".cue")))
				{
				TInt sz = 0;
				vol->Size(entry, sz);
				dstName(entry, aDstDir, _L(".cue"), dst);
				r = copyEntry(aFs, *vol, entry, dst, sz);
				}
			for (TInt i = 0; i < cue->iFiles->MdcaCount() && r == KErrNone; i++)
				{
				const TPtrC nm = cue->iFiles->MdcaPoint(i);
				TFileName d2(aDstDir);
				if (d2.Length() + nm.Length() >= d2.MaxLength())
					continue;
				d2.Append(nm);
				TInt s2 = 0;
				vol->Size(nm, s2);
				r = copyEntry(aFs, *vol, nm, d2, s2);
				}
			}
		}

	delete vol;
	if (r == KErrNone && aDelete)
		deleteSource(aFs, aPath, *cue);
	delete cue->iFiles;
	User::Free(cue);
	return r;
	}

// Чи є в списку запис про той самий диск: той самий каталог і те саме ім'я до
// крапки. Індекс збігу або -1.
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

// --- Робочий потік ----------------------------------------------------------

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
	// Словник LZMA виділяється цілком; скільки лишилось у системі - головне число,
	// коли розпакувальник відмовляє в пам'яті.
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
		// Джерела не стало - його забрало попереднє завдання черги (доріжка з
		// .cue, який уже оброблено разом із видаленням). Це не помилка.
		if (i > 0 && (rc == KErrNotFound || rc == KErrPathNotFound))
			{
			convLog("skip gone", i);
			continue;
			}
		if (rc != KErrNone && rc != KErrCancel)
			{
			// Черга йде далі: один архів, якому не вистачило пам'яті чи який побитий,
			// не привід кидати решту десяток. Причину показуємо одразу, підсумок -
			// у кінці.
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
	if (g_stat.iState == EConvRun)
		return KErrInUse;
	ConvRelease();

	if (!g_lockOk && g_lock.CreateLocal() == KErrNone)
		g_lockOk = ETrue;

	CDesCArray *copy = new CDesCArrayFlat(4);
	if (!copy)
		return KErrNoMemory;
	// Позначити .cue І його .bin - природний вибір користувача, але це той самий
	// диск: обидва завдання дали б один файл призначення, а після першого джерела
	// вже немає. Лишаємо один запис на диск, віддаючи перевагу .cue - у ньому
	// розкладка доріжок.
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
	g_stat.iState = EConvRun;
	g_stat.iErr = 0;
	g_stat.iPercent = 0;
	g_stat.iFileNo = 0;
	g_stat.iFileCount = aFiles.MdcaCount();
	g_stat.iMsg.Zero();
	g_stat.iName.Zero();

	// Власна купа: deflate тримає ~256 КБ, індекс блоків - ще 256 КБ, і брати їх
	// із купи головного потоку означало б конкурувати з динареком.
	// Стеля купи - це лише резервування адрес, сторінки беруться в міру потреби,
	// тож вона рахована під словник LZMA (KMaxDict у arc7z). Із 8 МБ будь-який
	// звичайний 7z (словник 16-64 МБ) відмовляв ще на відкритті з -4.
	// Ім'я з лічильником: щойно вбитий потік звільняє своє не миттєво, і другий
	// запуск підряд інакше падав би з KErrAlreadyExists.
	static TInt seq = 0;
	TName tn;
	tn.Format(_L("ps1conv%d"), ++seq);
	const TInt r = g_worker.Create(tn, workerMain,
		32 * 1024, 4 * 1024, 72 * 1024 * 1024, NULL);
	if (r != KErrNone)
		{
		g_stat.iState = EConvErr;
		g_stat.iMsg.Copy(_L("cannot start worker"));
		return r;
		}
	g_workerOk = ETrue;
	// Нижче головного: меню має лишатись чуйним, поки диск перетискається.
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

// --- Витяг одного файлу (BIOS) ----------------------------------------------

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
		// В архіві з дампами BIOS зазвичай лежить і readme; беремо перший, що
		// схожий на дамп, і лише як запасний варіант - перший-ліпший.
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
