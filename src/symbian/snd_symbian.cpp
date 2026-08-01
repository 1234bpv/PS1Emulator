// Звук для Symbian: glue між ядром і dfsound (портабельна гілка SPU).
//
// Окремий файл навмисно: libpcsxcore/plugins.h оголошує SPUinit, SPUopen тощо як
// ТИПИ (typedef), а в dfsound це функції з тими самими іменами. Транслювати обидва
// в одному файлі не можна (конфлікт typedef/функція), тож тут plugins.h НЕ
// включаємо, а назовні віддаємо обгортки з унікальними іменами ps1Spu*.
//
// Фаза B: out-драйвер віддає PCM у динамік через CMdaAudioOutputStream. API
// асинхронний (потребує CActiveScheduler); наш цикл кадрів синхронний, тож
// обслуговуємо активні об'єкти неблокуюче через CActiveScheduler::RunIfReady.

#include <e32base.h>
#include <mda/common/audio.h>
#include <mdaaudiooutputstream.h>

extern "C" {
// Справжні експорти dfsound (spu.c/registers.c/dma.c).
long SPUinit(void);
long SPUshutdown(void);
long SPUopen(void);
long SPUclose(void);
void SPUconfigure(void);
void SPUwriteRegister(unsigned long, unsigned short, unsigned int);
unsigned short SPUreadRegister(unsigned long, unsigned int);
void SPUwriteDMAMem(unsigned short *, int, unsigned int);
void SPUreadDMAMem(unsigned short *, int, unsigned int);
void SPUasync(unsigned int, unsigned int);
void SPUregisterCallback(void (*cb)(int));
void SPUregisterScheduleCb(void (*cb)(unsigned int));

#include "../../pcsx_rearmed/plugins/dfsound/spu_config.h"   // SPUConfig, spu_config
#include "../../pcsx_rearmed/plugins/dfsound/out.h"          // out_driver, out_current
}

// --- Вивід у динамік: CMdaAudioOutputStream + кільцевий буфер --------------
// dfsound віддає 16-біт стерео PCM @44100. feed() кладе байти в кільце (той самий
// потік, що й генерація й Pump - без блокувань). Pump() щокадру обслуговує активні
// об'єкти (RunIfReady) і доливає наступний WriteL, коли попередній скопійовано.
static const TInt KSndRing  = 128 * 1024;   // ~0.37с при 44100 стерео 16-біт
static const TInt KSndChunk = 8 * 1024;     // ~46мс на один WriteL

class CSymSnd : public CBase, public MMdaAudioOutputStreamCallback
	{
public:
	static CSymSnd *NewL();
	~CSymSnd();
	void Feed(const void *aBuf, TInt aBytes);
	void Pump();
	TBool WaitOpen(TInt aTimeoutMs);
private:
	void ConstructL();
	void Refill();
	void MaoscOpenComplete(TInt aError);
	void MaoscBufferCopied(TInt aError, const TDesC8 &aBuffer);
	void MaoscPlayComplete(TInt aError);

	CMdaAudioOutputStream *iStream;
	TBool iOpen;
	TInt  iOpenErr;
	TBool iWriting;
	TUint8 *iRing;
	TInt iCap, iHead, iTail;   // кільце: iHead - запис, iTail - читання
	RBuf8 iSend;               // буфер, відданий WriteL (живе до MaoscBufferCopied)
	};

CSymSnd *CSymSnd::NewL()
	{
	CSymSnd *s = new (ELeave) CSymSnd;
	CleanupStack::PushL(s);
	s->ConstructL();
	CleanupStack::Pop(s);
	return s;
	}

void CSymSnd::ConstructL()
	{
	iCap = KSndRing;
	iRing = (TUint8 *)User::AllocL(iCap);
	iSend.CreateL(KSndChunk);
	iStream = CMdaAudioOutputStream::NewL(*this, EMdaPriorityNormal);
	iStream->Open(NULL);   // асинхронно -> MaoscOpenComplete
	}

CSymSnd::~CSymSnd()
	{
	if (iStream) { iStream->Stop(); delete iStream; }
	iSend.Close();
	User::Free(iRing);
	}

void CSymSnd::MaoscOpenComplete(TInt aError)
	{
	iOpenErr = aError;
	if (aError != KErrNone)
		return;
	TRAP_IGNORE(iStream->SetAudioPropertiesL(
		TMdaAudioDataSettings::ESampleRate44100Hz,
		TMdaAudioDataSettings::EChannelsStereo));
	iStream->SetVolume(iStream->MaxVolume());
	iOpen = ETrue;
	Refill();
	}

void CSymSnd::MaoscBufferCopied(TInt /*aError*/, const TDesC8 & /*aBuffer*/)
	{
	iWriting = EFalse;
	Refill();
	}

void CSymSnd::MaoscPlayComplete(TInt /*aError*/) { }

void CSymSnd::Feed(const void *aBuf, TInt aBytes)
	{
	if (!iRing || aBytes <= 0)
		return;
	TInt freeSpace = iTail - iHead - 1;         // -1: повний != порожній
	if (freeSpace < 0) freeSpace += iCap;
	if (aBytes > freeSpace) aBytes = freeSpace; // переповнення -> відкидаємо надлишок
	const TUint8 *p = (const TUint8 *)aBuf;
	TInt first = iCap - iHead;
	if (first > aBytes) first = aBytes;
	Mem::Copy(iRing + iHead, p, first);
	if (aBytes > first)
		Mem::Copy(iRing, p + first, aBytes - first);
	iHead = (iHead + aBytes) % iCap;
	}

void CSymSnd::Refill()
	{
	if (!iOpen || iWriting)
		return;
	TInt avail = iHead - iTail;
	if (avail < 0) avail += iCap;
	if (avail < KSndChunk)
		return;                                 // недостатньо - чекаємо наступного Pump
	iSend.SetLength(0);
	TInt first = iCap - iTail;
	if (first > KSndChunk) first = KSndChunk;
	iSend.Append(iRing + iTail, first);
	if (KSndChunk > first)
		iSend.Append(iRing, KSndChunk - first);
	iTail = (iTail + KSndChunk) % iCap;
	iWriting = ETrue;
	TRAPD(err, iStream->WriteL(iSend));
	if (err != KErrNone) iWriting = EFalse;
	}

void CSymSnd::Pump()
	{
	TInt e;
	while (CActiveScheduler::RunIfReady(e, CActive::EPriorityIdle))
		{ }
	Refill();
	}

TBool CSymSnd::WaitOpen(TInt aTimeoutMs)
	{
	TInt waited = 0;
	while (!iOpen && iOpenErr == KErrNone && waited < aTimeoutMs)
		{
		TInt e;
		while (CActiveScheduler::RunIfReady(e, CActive::EPriorityIdle))
			{ }
		if (iOpen)
			break;
		User::After(10 * 1000);
		waited += 10;
		}
	return iOpen;
	}

static CSymSnd *g_snd;

// --- Out-драйвер dfsound: приймає 16-біт стерео PCM ------------------------
static unsigned g_sndFeeds, g_sndSamples;
static int g_sndMax;

static int  symOutInit(void)   { g_sndFeeds = g_sndSamples = 0; g_sndMax = 0; return 0; }
static void symOutFinish(void) { }
static int  symOutBusy(void)   { return 0; }          // 0 = порожньо: генеруй ще
static void symOutFeed(void *buf, int bytes)
	{
	g_sndFeeds++;
	const short *s = (const short *)buf;
	const int n = bytes / 2;
	g_sndSamples += (unsigned)n;
	for (int i = 0; i < n; i++)
		{
		int v = s[i];
		if (v < 0) v = -v;
		if (v > g_sndMax) g_sndMax = v;
		}
	if (g_snd)
		g_snd->Feed(buf, bytes);
	}
static struct out_driver g_symOut = { "symbian", symOutInit, symOutFinish, symOutBusy, symOutFeed };

// --- Обгортки для оболонки (унікальні імена, без конфлікту з typedef) -------
extern "C" {

long ps1SpuInit(void)      { return SPUinit(); }
long ps1SpuShutdown(void)  { return SPUshutdown(); }
long ps1SpuOpen(void)      { return SPUopen(); }
long ps1SpuClose(void)     { return SPUclose(); }
void ps1SpuConfigure(void) { SPUconfigure(); }
void ps1SpuWriteRegister(unsigned long r, unsigned short v, unsigned int c) { SPUwriteRegister(r, v, c); }
unsigned short ps1SpuReadRegister(unsigned long r, unsigned int c)          { return SPUreadRegister(r, c); }
void ps1SpuWriteDMAMem(unsigned short *m, int s, unsigned int c) { SPUwriteDMAMem(m, s, c); }
void ps1SpuReadDMAMem(unsigned short *m, int s, unsigned int c)  { SPUreadDMAMem(m, s, c); }
void ps1SpuAsync(unsigned int a, unsigned int b)                { SPUasync(a, b); }
void ps1SpuRegisterCallback(void (*cb)(int))                    { SPUregisterCallback(cb); }
void ps1SpuRegisterScheduleCb(void (*cb)(unsigned int))         { SPUregisterScheduleCb(cb); }

// Конфіг dfsound до ps1SpuInit (SPUinit читає iVolume). Без потоку - генеруємо
// синхронно в циклі кадрів; реверб/інтерполяція вимкнені (зайвий CPU на ARM11).
void ps1SndConfigure(void)
	{
	spu_config.iVolume = 1024;
	spu_config.iUseThread = 0;
	spu_config.iUseReverb = 0;
	spu_config.iUseInterpolation = 0;
	spu_config.iXAPitch = 0;
	spu_config.iTempo = 0;
	}

// ps1SpuOpen викликає SetupSound (out_current = none); перенаправляємо на наш драйвер.
void ps1SndSelectOutput(void)
	{
	out_current = &g_symOut;
	symOutInit();
	}

void ps1SndStats(unsigned *aFeeds, unsigned *aSamples, int *aMaxAmp)
	{
	if (aFeeds)   *aFeeds = g_sndFeeds;
	if (aSamples) *aSamples = g_sndSamples;
	if (aMaxAmp)  *aMaxAmp = g_sndMax;
	}

// --- Керування виводом у динамік (Фаза B) ----------------------------------
static CActiveScheduler *g_sndSched;

// Відкрити аудіопристрій. Потребує CActiveScheduler на потоці (ставимо свій, якщо
// його ще нема); відкриття асинхронне, тож чекаємо MaoscOpenComplete до 2с.
int ps1SndOpenOutput(void)
	{
	if (g_snd)
		return 0;
	if (!CActiveScheduler::Current())
		{
		g_sndSched = new CActiveScheduler;
		if (!g_sndSched)
			return -1;
		CActiveScheduler::Install(g_sndSched);
		}
	CSymSnd *s = NULL;
	TRAPD(err, s = CSymSnd::NewL());
	if (err != KErrNone || !s)
		return err ? err : -1;
	g_snd = s;
	return g_snd->WaitOpen(2000) ? 0 : -2;   // -2: відкрилось не вчасно (звук може підхопитись пізніше)
	}

// Обслужити активні об'єкти й долити наступний буфер. Викликати щокадру.
void ps1SndPump(void)
	{
	if (g_snd)
		g_snd->Pump();
	}

void ps1SndCloseOutput(void)
	{
	if (g_snd)
		{
		delete g_snd;
		g_snd = NULL;
		}
	if (g_sndSched)
		{
		CActiveScheduler::Install(NULL);
		delete g_sndSched;
		g_sndSched = NULL;
		}
	}

} // extern "C"
