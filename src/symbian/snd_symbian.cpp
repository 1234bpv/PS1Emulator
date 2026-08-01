// Звук для Symbian: glue між ядром і dfsound (портабельна гілка SPU).
//
// Окремий файл навмисно: libpcsxcore/plugins.h оголошує SPUinit, SPUopen тощо як
// ТИПИ (typedef), а в dfsound це функції з тими самими іменами. Транслювати обидва
// в одному файлі не можна (конфлікт typedef/функція), тож тут plugins.h НЕ
// включаємо, а назовні віддаємо обгортки з унікальними іменами ps1Spu*.
//
// Фаза A: out-драйвер лише міряє амплітуду (доказ, що SPU генерує звук BIOS).
// Вивід у динамік (CMdaAudioOutputStream) - Фаза B.

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

} // extern "C"
