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
// dfsound віддає 16-біт стерео PCM @44100 ЕМУЛЬОВАНОГО часу, а пристрій з'їдає
// 44100 на РЕАЛЬНУ секунду. Поки емуляція не встигає, різниця - це порожній буфер
// і розриви, тож Feed() перераховує потік під фактичний темп (див. UpdateRate).
// Pump() щокадру обслуговує активні об'єкти (RunIfReady) і доливає буфери, поки їх
// у польоті менше за KSndBufs.
static const TInt KSndRate  = 44100;
static const TInt KSndRing  = 128 * 1024;      // 0.74с при 44100 стерео 16-біт
static const TInt KSndChunk = 4 * 1024;        // ~23мс на один WriteL
static const TInt KSndBufs  = 4;               // буферів у польоті: ~93мс запасу
static const TInt KSndFill  = 3 * KSndChunk;   // цільове заповнення кільця (~70мс)

extern "C" unsigned int ndrc_symbian_fastcount(void);
extern "C" unsigned int ndrc_symbian_fastfreq(void);

// Скільки разів пристрою не було чим долити, тобто скільки чутних розривів.
static unsigned g_sndUnder;

// Гучність кроками, керується з оболонки (клавіші * і #). Регулюємо гучність
// ПОТОКУ, а не spu_config.iVolume: масштабування в мікшері dfsound однаково
// коштує CPU і псує динамічний діапазон, а тут це робить аудіопідсистема.
static const TInt KVolSteps   = 10;
static const TInt KVolDefault = 2;          // на повній динамік апарата перевантажується
static TInt g_volStep = KVolDefault;

class CSymSnd : public CBase, public MMdaAudioOutputStreamCallback
	{
public:
	static CSymSnd *NewL();
	~CSymSnd();
	void Feed(const void *aBuf, TInt aBytes);
	void Pump();
	TBool WaitOpen(TInt aTimeoutMs);
	void ApplyVolume();
	TInt Step() const { return iStep; }
private:
	void ConstructL();
	void Refill();
	void PushFrame(TInt aL, TInt aR);
	void UpdateRate(TInt aInFrames);
	void MaoscOpenComplete(TInt aError);
	void MaoscBufferCopied(TInt aError, const TDesC8 &aBuffer);
	void MaoscPlayComplete(TInt aError);

	CMdaAudioOutputStream *iStream;
	TBool iOpen;
	TInt  iOpenErr;
	TUint8 *iRing;
	TInt iCap, iHead, iTail;   // кільце: iHead - запис, iTail - читання
	RBuf8 iSend[KSndBufs];     // віддані WriteL (живуть до MaoscBufferCopied)
	TInt iSendNext;            // наступний вільний буфер, по колу
	TInt iInFlight;
	// Перерахунок темпу. iStep - скільки вхідних семплів припадає на один вихідний
	// у форматі 16.16; менше за 1.0 означає розтяг. iPos - дробова позиція в
	// поточному вхідному блоці, iPrev* - останній семпл попереднього (інтерполяція
	// має бути неперервною через межу блоків).
	TInt iStep, iPos;
	TInt iPrevL, iPrevR;
	TInt iRateFrames;          // вхідних кадрів від початку вікна вимірювання
	TUint iRateT0;
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
	for (TInt i = 0; i < KSndBufs; i++)
		iSend[i].CreateL(KSndChunk);
	iStep = 1 << 16;
	iRateT0 = ndrc_symbian_fastcount();
	iStream = CMdaAudioOutputStream::NewL(*this, EMdaPriorityNormal);
	iStream->Open(NULL);   // асинхронно -> MaoscOpenComplete
	}

CSymSnd::~CSymSnd()
	{
	if (iStream) { iStream->Stop(); delete iStream; }
	for (TInt i = 0; i < KSndBufs; i++)
		iSend[i].Close();
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
	iOpen = ETrue;
	// Вікно вимірювання темпу починається тоді, коли пристрій почав споживати, а
	// не при конструюванні: до відкриття споживача нема і вимір був би хибним.
	iRateFrames = 0;
	iRateT0 = ndrc_symbian_fastcount();
	ApplyVolume();
	Refill();
	}

void CSymSnd::ApplyVolume()
	{
	if (!iStream || !iOpen)
		return;
	const TInt max = iStream->MaxVolume();
	TInt v = max * g_volStep / KVolSteps;
	if (v < 0)   v = 0;
	if (v > max) v = max;
	iStream->SetVolume(v);
	}

void CSymSnd::MaoscBufferCopied(TInt /*aError*/, const TDesC8 & /*aBuffer*/)
	{
	if (iInFlight > 0)
		iInFlight--;
	Refill();
	}

void CSymSnd::MaoscPlayComplete(TInt /*aError*/) { }

// Кадр - 4 байти, iCap і KSndChunk кратні 4, тож кадр ніколи не лягає через межу
// кільця і копіювати його двома шматками не треба.
void CSymSnd::PushFrame(TInt aL, TInt aR)
	{
	TInt next = iHead + 4;
	if (next == iCap) next = 0;
	if (next == iTail)
		return;                     // кільце повне: надлишок відкидаємо
	TInt16 *p = (TInt16 *)(iRing + iHead);
	p[0] = (TInt16)aL;
	p[1] = (TInt16)aR;
	iHead = next;
	}

// Скільки семплів емуляція дала за реальне вікно - стільки їх і треба розтягнути
// на те саме вікно. Основний член - прямий вимір (сходиться за одне вікно і не
// коливається, на відміну від інтегратора по заповненню); заповнення кільця дає
// лише слабкий трим проти повільного дрейфу.
void CSymSnd::UpdateRate(TInt aInFrames)
	{
	iRateFrames += aInFrames;
	const TUint now = ndrc_symbian_fastcount();
	const TUint freq = ndrc_symbian_fastfreq();
	const TUint el = now - iRateT0;
	if (!freq || el < freq / 2)
		return;
	const TInt want = (TInt)((TInt64)KSndRate * el / freq);
	if (want > 0)
		{
		TInt step = (TInt)(((TInt64)iRateFrames << 16) / want);
		TInt avail = iHead - iTail;
		if (avail < 0) avail += iCap;
		step += (avail - KSndFill) / 64;
		if (step < (1 << 14)) step = 1 << 14;       // 0.25
		if (step > (2 << 16)) step = 2 << 16;
		iStep = step;
		}
	iRateFrames = 0;
	iRateT0 = now;
	}

void CSymSnd::Feed(const void *aBuf, TInt aBytes)
	{
	if (!iRing || aBytes <= 0)
		return;
	const TInt16 *src = (const TInt16 *)aBuf;
	const TInt n = aBytes / 4;
	if (n <= 0)
		return;
	while ((iPos >> 16) < n)
		{
		const TInt i = iPos >> 16;
		// Дріб на 14 бітах: різниця семплів займає 17 біт зі знаком, і на повних
		// 16 бітах дробу добуток вилазить за межу 32-бітного цілого.
		const TInt f = (iPos >> 2) & 0x3FFF;
		const TInt aL = i ? src[(i - 1) * 2]     : iPrevL;
		const TInt aR = i ? src[(i - 1) * 2 + 1] : iPrevR;
		PushFrame(aL + (((src[i * 2]     - aL) * f) >> 14),
		          aR + (((src[i * 2 + 1] - aR) * f) >> 14));
		iPos += iStep;
		}
	iPos -= n << 16;
	iPrevL = src[(n - 1) * 2];
	iPrevR = src[(n - 1) * 2 + 1];
	UpdateRate(n);
	}

// Тримаємо в польоті KSndBufs буферів, а не один: із єдиним буфером уся глибина
// конвеєра дорівнює його тривалості, і будь-який кадр, довший за неї, чути як
// розрив. Слоти беруться по колу, а MaoscBufferCopied приходить у порядку віддачі,
// тож лічильника в польоті досить, щоб не перезаписати ще не скопійований буфер.
void CSymSnd::Refill()
	{
	if (!iOpen)
		return;
	while (iInFlight < KSndBufs)
		{
		TInt avail = iHead - iTail;
		if (avail < 0) avail += iCap;
		// Поки нічого не грає - не стартувати з майже порожнього кільця: воно
		// одразу спорожніє знову. Той самий поріг служить і первинним запасом.
		const TInt need = iInFlight ? KSndChunk : KSndFill;
		if (avail < need)
			{
			if (iInFlight == 0)
				g_sndUnder++;       // скільки разів Pump застав динамік без даних
			return;
			}
		RBuf8 &b = iSend[iSendNext];
		b.SetLength(0);
		TInt first = iCap - iTail;
		if (first > KSndChunk) first = KSndChunk;
		b.Append(iRing + iTail, first);
		if (KSndChunk > first)
			b.Append(iRing, KSndChunk - first);
		iTail = (iTail + KSndChunk) % iCap;
		TRAPD(err, iStream->WriteL(b));
		if (err != KErrNone)
			return;
		iSendNext = (iSendNext + 1) % KSndBufs;
		iInFlight++;
		}
	}

// У GUI планувальник крутить сама оболонка: CIdle віддає йому керування після
// КОЖНОГО кадру, тож активні об'єкти й так обслуговуються. Вкладений RunIfReady
// там лише поглиблював би стек рівно в найглибшій точці - усередині кроку
// емуляції - і давав би реентрантність. У консольній гілці планувальника не
// крутить ніхто, тому там він лишається.
static TBool g_pumpAo = ETrue;

void CSymSnd::Pump()
	{
	if (g_pumpAo)
		{
		TInt e;
		while (CActiveScheduler::RunIfReady(e, CActive::EPriorityIdle))
			{ }
		}
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

static int  symOutInit(void)   { g_sndFeeds = g_sndSamples = 0; g_sndMax = 0; g_sndUnder = 0; return 0; }
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
// Мікшування SPU - частина "emu", і досі вона була невіддільна від CPU. Хук той
// самий, що для растеризації: без нього не сказати, чи звук узагалі помітний у
// кадрі. Аргумент "nosnd" лише не відкриває динамік, мікшер працює далі.
extern "C" unsigned int ndrc_symbian_fastcount(void);
unsigned long long g_spuMixTicks;

void ps1SpuAsync(unsigned int a, unsigned int b)
	{
	const unsigned int t0 = ndrc_symbian_fastcount();
	SPUasync(a, b);
	g_spuMixTicks += (unsigned int)(ndrc_symbian_fastcount() - t0);
	}
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

void ps1SndStats(unsigned *aFeeds, unsigned *aSamples, int *aMaxAmp,
                 unsigned *aUnder, int *aStepQ16)
	{
	if (aFeeds)   *aFeeds = g_sndFeeds;
	if (aSamples) *aSamples = g_sndSamples;
	if (aMaxAmp)  *aMaxAmp = g_sndMax;
	if (aUnder)   *aUnder = g_sndUnder;
	if (aStepQ16) *aStepQ16 = g_snd ? g_snd->Step() : 0;
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

// Гучність на крок вгору/вниз. Повертає новий рівень (0..KVolSteps). Працює й до
// відкриття потоку - рівень запам'ятовується й застосується в MaoscOpenComplete.
int ps1SndVolumeStep(int aDelta)
	{
	g_volStep += aDelta;
	if (g_volStep < 0)         g_volStep = 0;
	if (g_volStep > KVolSteps) g_volStep = KVolSteps;
	if (g_snd)
		g_snd->ApplyVolume();
	return g_volStep;
	}

int ps1SndVolumeSteps(void)
	{
	return KVolSteps;
	}

void ps1SndSetPumpAo(int aOn)
	{
	g_pumpAo = aOn ? ETrue : EFalse;
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
