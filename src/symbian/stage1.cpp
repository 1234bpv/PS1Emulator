// Етап 1: завантажити libpcsxcore з інтерпретатором і обчислити кадри.
// Без виведення на екран, без звуку, без диска (HLE BIOS).
// Контракт фронтенду та механізм кадру - docs/architecture/data-spec.md.

#include <e32base.h>
#include <e32std.h>
#include <e32debug.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "compat_stdio.h"

extern "C" {
#include "psxcommon.h"
#include "r3000a.h"
#include "plugins.h"
#include "psxcounters.h"
}

// Скільки кадрів рахувати. Перевизначається першим аргументом командного рядка:
// EKA2L1 передає його через --run "<exe>" "<args>". Лог потрапляє на хост лише
// коли процес завершився штатно, тож можливість швидко зменшити прогін критична.
static const int KDefaultFrames = 600;
// Запуск з меню апарата: 300 кадрів = 5 секунд ігрового часу на кожен режим ЦП.
static const int KDeviceFrames = 300;
// Крок маркерів прогресу в циклі кадрів.
static const int KProgressEvery = 50;

// Ядро складає шлях як "%s/%s" (psxmem.c:312) - пряма скісна в тексті навмисна,
// це перевірка, чи ESTLIB приймає її замість зворотної.
// Корінь картки E: замість C:\bios: демон розгортання (v1) не створює тек, а
// E: точно існує й читабельний (туди ж пишеться лог). Дамп заливається на
// E:\scph7502.bin.
static const char KBiosDir[]  = "E:";
static const char KBiosFile[] = "scph7502.bin";

static FILE *g_log;
static TUint g_startTick;
static int g_eventId;

// Лог у форматі JSONL: один об'єкт на подію (CLAUDE.md, розділ 7).
static void logEvent(const char *type, const char *errType, const char *fmt, ...)
	{
	char msg[512];
	msg[0] = 0;
	if (fmt)
		{
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(msg, sizeof(msg), fmt, ap);
		va_end(ap);
		}

	// Повідомлення ядра містять лапки і переводи рядка - екрануємо, щоб JSON лишався валідним.
	char esc[1024];
	unsigned d = 0;
	for (unsigned s = 0; msg[s] && d < sizeof(esc) - 7; s++)
		{
		unsigned char c = (unsigned char)msg[s];
		if (c == '"' || c == '\\')       { esc[d++] = '\\'; esc[d++] = c; }
		else if (c == '\n')              { esc[d++] = '\\'; esc[d++] = 'n'; }
		else if (c == '\r')              { esc[d++] = '\\'; esc[d++] = 'r'; }
		else if (c == '\t')              { esc[d++] = '\\'; esc[d++] = 't'; }
		else if (c < 0x20)               { d += snprintf(esc + d, 7, "\\u%04x", c); }
		else                             { esc[d++] = (char)c; }
		}
	esc[d] = 0;

	const TUint elapsed = User::NTickCount() - g_startTick;
	const int id = g_eventId++;

	// Дублюємо в канал налагодження: EKA2L1 віддає файл на хост лише при закритті,
	// тож обірваний прогін інакше не лишає жодного сліду.
	RDebug::Printf("[ps1] %d %ums %s %s %s", id, (unsigned)elapsed, type,
		errType ? errType : "-", esc);

	if (!g_log)
		return;

	fprintf(g_log, "{\"id\":%d,\"ms\":%u,\"event\":\"%s\",\"error\":%s%s%s,\"msg\":\"%s\"}\n",
		id, (unsigned)elapsed, type,
		errType ? "\"" : "null", errType ? errType : "", errType ? "\"" : "",
		esc);
	fflush(g_log);
	}

// --- Контракт, який libpcsxcore очікує від фронтенду -----------------------
// Перелік визначено з самої бібліотеки (nm: невизначені мінус визначені).

// Згортання повторів замість обмеження кількості. Ліміт різав за порядком
// надходження: 2000 однакових "game crash @f0001234" від інтерпретатора з'їдали
// квоту, і на прогін динареку не лишалось жодного повідомлення ядра - зокрема
// його власної самоперевірки. До того ж кожен запис коштує fflush на флеш, тож
// потік повторів спотворював і вимір часу.
static char g_lastCore[512];
static int g_coreRepeat;

// Викликати перед подіями, які мають лягти в лог у правильному порядку.
static void flushCoreRepeat()
	{
	if (!g_coreRepeat)
		return;
	const int n = g_coreRepeat;
	g_coreRepeat = 0;
	g_lastCore[0] = 0;
	logEvent("core", NULL, "previous message repeated %d times", n);
	}

static void logCore(const char *type, const char *buf)
	{
	if (strcmp(buf, g_lastCore) == 0)
		{
		g_coreRepeat++;
		return;
		}
	flushCoreRepeat();
	strncpy(g_lastCore, buf, sizeof(g_lastCore) - 1);
	g_lastCore[sizeof(g_lastCore) - 1] = 0;
	logEvent(type, NULL, "%s", buf);
	}

extern "C" {

void SysPrintf(const char *fmt, ...)
	{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	logCore("core", buf);
	}

void SysMessage(const char *fmt, ...)
	{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	logCore("core_msg", buf);
	}

// Плагіни вбудовані, динамічного завантаження під Symbian немає.
void *SysLoadLibrary(const char * /*lib*/)            { return NULL; }
void *SysLoadSym(void * /*lib*/, const char * /*sym*/) { return NULL; }
const char *SysLibError()                              { return NULL; }
void SysCloseLibrary(void * /*lib*/)                   { }

// Ядро кличе це на кожній вертикальній розгортці через EmuUpdate().
// Інкремент stop завершує psxCpu->Execute() рівно на межі кадру.
void pl_frame_limit(void)
	{
	psxRegs.stop++;
	}

unsigned short in_keystate[8];

void PAD1_readPort(struct PadDataS *pad, int *is_multitap)
	{
	if (pad) memset(pad, 0, sizeof(*pad));
	if (is_multitap) *is_multitap = 0;
	}

void PAD2_readPort(struct PadDataS *pad, int *is_multitap)
	{
	if (pad) memset(pad, 0, sizeof(*pad));
	if (is_multitap) *is_multitap = 0;
	}

void pl_gun_byte2(int /*port*/, unsigned char /*byte*/) { }
void plat_trigger_vibrate(int /*pad*/, int /*low*/, int /*high*/) { }

} // extern "C"

// --- Плагіни ----------------------------------------------------------------
// LoadPlugins() вантажить .so через SysLoadLibrary, чого під Symbian немає,
// тож вказівники виставляються напряму. GPU - справжній gpu_unai через gpulib
// (Етап 2). Звук поки заглушений, це Етап 4.

// Обгортки з src/symbian/vout_symbian.cpp: імена gpulib збігаються з ТИПАМИ
// в plugins.h, тож напряму їх сюди не оголосити.
extern "C" {
long ps1GpuInit(void);
long ps1GpuShutdown(void);
long ps1GpuOpen(unsigned long *disp, char *cap, char *cfg);
long ps1GpuClose(void);
void ps1GpuWriteStatus(uint32_t data);
void ps1GpuWriteData(uint32_t data);
void ps1GpuWriteDataMem(uint32_t *mem, int count);
uint32_t ps1GpuReadStatus(void);
uint32_t ps1GpuReadData(void);
void ps1GpuReadDataMem(uint32_t *mem, int count);
long ps1GpuDmaChain(uint32_t *rambase, uint32_t start_addr, uint32_t *progress_addr, int32_t *cycles);
long ps1GpuFreeze(uint32_t type, GPUFreeze_t *freeze, uint16_t **vram_ptr);
void ps1GpuVBlank(int is_vblank, int lcf);
void ps1GpuGetScreenInfo(int *y, int *base_hres);
void ps1GpuSetCallbacks(unsigned int *frameCount, unsigned int *hcnt);

int voutSymbianDump(const char *path);
int voutSymbianUpdates(void);
int voutSymbianShow(int holdMs);
}

// Скільки тримати кадр на екрані, щоб людина встигла глянути (запуск headless
// по WiFi: дивимось на сам апарат під час прогону).
static const int KScreenHoldMs = 5000;

// Захист від зависання. Цикл кадрів покладається на те, що psxCpu->Execute()
// повертається на межі кадру (pl_frame_limit виставляє psxRegs.stop на VBlank).
// Реальний BIOS може крутитися в ранній ініціалізації ДО першого VBlank - тоді
// Execute не повертається ніколи, а демон розгортання блокований у RUN без
// таймауту, тобто телефон знімається лише ребутом. Сторожовий потік через
// KWatchdogMs виставляє psxRegs.stop (інтерпретатор перевіряє його щоінструкції,
// while(!regs->stop)), і прохід завершується з частковим результатом у лозі.
static const TInt KWatchdogMs = 60000;

static volatile TInt g_wdRun;    // 1, поки прохід хоче, щоб сторож жив
static volatile TInt g_wdFired;  // 1, якщо сторож спрацював (спін довше ліміту)

// Завершується КООПЕРАТИВНО: перевіряє g_wdRun короткими кроками й виходить сам,
// коли прохід його вимкне. Так не треба RThread::Kill (паніка при вбивстві вже
// мертвого потоку) і немає гонки на завершенні.
static TInt watchdogThread(TAny *aArg)
	{
	const TInt totalMs = (TInt)aArg;
	const TInt step = 200;
	TInt elapsed = 0;
	while (g_wdRun && elapsed < totalMs)
		{
		User::After(step * 1000);
		elapsed += step;
		}
	if (g_wdRun)
		{
		psxRegs.stop = 1;
		g_wdFired = 1;
		}
	return 0;
	}

static unsigned int g_gpuHcnt;

static long CALLBACK spuInit(void)                                            { return 0; }
static long CALLBACK spuShutdown(void)                                        { return 0; }
static long CALLBACK spuClose(void)                                           { return 0; }
static void CALLBACK spuWriteRegister(unsigned long, unsigned short, unsigned int) { }
static unsigned short CALLBACK spuReadRegister(unsigned long, unsigned int)   { return 0; }
static void CALLBACK spuWriteDMAMem(unsigned short *, int, unsigned int)      { }
static void CALLBACK spuReadDMAMem(unsigned short *, int, unsigned int)       { }
static void CALLBACK spuPlayADPCMchannel(xa_decode_t *, unsigned int, int)    { }
static long CALLBACK spuFreeze(int, SPUFreeze_t *, unsigned short **, void *, unsigned int) { return 0; }
static int  CALLBACK spuPlayCDDAchannel(short *, int, unsigned int, int)      { return 0; }
static void CALLBACK spuSetCDvol(unsigned char, unsigned char, unsigned char, unsigned char, unsigned int) { }
static void CALLBACK spuAsync(unsigned int, unsigned int)                     { }

static void bindStubPlugins()
	{
	GPU_init          = ps1GpuInit;
	GPU_shutdown      = ps1GpuShutdown;
	GPU_open          = ps1GpuOpen;
	GPU_close         = ps1GpuClose;
	GPU_writeStatus   = ps1GpuWriteStatus;
	GPU_writeData     = ps1GpuWriteData;
	GPU_writeDataMem  = ps1GpuWriteDataMem;
	GPU_readStatus    = ps1GpuReadStatus;
	GPU_readData      = ps1GpuReadData;
	GPU_readDataMem   = ps1GpuReadDataMem;
	GPU_dmaChain      = ps1GpuDmaChain;
	GPU_freeze        = ps1GpuFreeze;
	GPU_vBlank        = ps1GpuVBlank;
	GPU_getScreenInfo = ps1GpuGetScreenInfo;

	SPU_init             = spuInit;
	SPU_shutdown         = spuShutdown;
	SPU_close            = spuClose;
	SPU_writeRegister    = spuWriteRegister;
	SPU_readRegister     = spuReadRegister;
	SPU_writeDMAMem      = spuWriteDMAMem;
	SPU_readDMAMem       = spuReadDMAMem;
	SPU_playADPCMchannel = spuPlayADPCMchannel;
	SPU_freeze           = spuFreeze;
	SPU_playCDDAchannel  = spuPlayCDDAchannel;
	SPU_setCDvol         = spuSetCDvol;
	SPU_async            = spuAsync;
	}

// --- Точка входу ------------------------------------------------------------

static void openLog()
	{
	// E: - змонтована карта пам'яті, тобто тека хоста: лог видно ззовні емулятора.
	g_log = fopen("E:\\ps1_stage1.jsonl", "w");
	if (!g_log)
		g_log = fopen("C:\\ps1_stage1.jsonl", "w");
	}

// Командний рядок: [кількість кадрів] [bios] [gputest] [drc]
// aHaveArgs == EFalse означає запуск з меню телефона - там аргументів немає,
// і тоді проганяються обидва режими ЦП підряд для порівняння.
static void parseCommandLine(TInt &aFrames, TBool &aUseBios, TBool &aGpuTest, TBool &aDrc,
	TBool &aShowScreen, TBool &aHaveArgs)
	{
	aFrames = KDefaultFrames;
	aUseBios = EFalse;
	aGpuTest = EFalse;
	aDrc = EFalse;
	aShowScreen = EFalse;

	TBuf<256> cmd;
	User::CommandLine(cmd);
	cmd.Trim();
	aHaveArgs = cmd.Length() > 0;
	TLex lex(cmd);

	lex.SkipSpace();
	TInt n = 0;
	if (lex.Val(n) == KErrNone && n > 0)
		aFrames = n;

	_LIT(KBiosArg, "bios");
	if (cmd.FindF(KBiosArg) != KErrNotFound)
		aUseBios = ETrue;

	_LIT(KGpuTestArg, "gputest");
	if (cmd.FindF(KGpuTestArg) != KErrNotFound)
		aGpuTest = ETrue;

	// Динарек умисно за прапорцем: у EKA2L1 його результат нічого не доводить
	// (CLAUDE.md розділ 6), тож типовим лишається інтерпретатор.
	_LIT(KDrcArg, "drc");
	if (cmd.FindF(KDrcArg) != KErrNotFound)
		aDrc = ETrue;

	// Виведення кадру на екран (Етап 2). За замовчуванням вимкнено, бо псує
	// чистий headless-прогін замірів: показ тримає процес KScreenHoldMs зайвих.
	_LIT(KScreenArg, "screen");
	if (cmd.FindF(KScreenArg) != KErrNotFound)
		aShowScreen = ETrue;
	}

// Самоперевірка растеризації: подаємо команди GP0 напряму, без BIOS і без гри.
// Це єдиний спосіб перевірити gpu_unai поки процесор крутиться в пастці HLE.
// Формати команд - стандартні GP0 PlayStation.
static void runGpuSelfTest()
	{
	// Налаштування вікна дисплея через GP1 (320x240 NTSC, 15bit). На растеризацію
	// у VRAM це не впливає (GP0 пише в VRAM незалежно від регістрів дисплея), тож
	// дамп лишається тим самим. Мета - щоб gpulib виставив screen.w/h=320/240, і
	// показ пішов справжньою гілкою масштабування (320x240 -> 240x180 летербокс),
	// а не fallback'ом (розмір екрана). GP1 = (команда<<24)|дані.
	// hrange x1=0x200,x2=0xC00 -> (x2-x1)/hdiv(8)=320; vrange y1=0x10,y2=0x100 -> 240.
	ps1GpuWriteStatus(0x06000000u | (0xC00u << 12) | 0x200u); // GP1(06) hor range
	ps1GpuWriteStatus(0x07000000u | (0x100u << 10) | 0x010u); // GP1(07) ver range
	ps1GpuWriteStatus(0x08000001u);                           // GP1(08) mode: 320x240 NTSC 15bit
	ps1GpuWriteStatus(0x05000000u);                           // GP1(05) display start = (0,0)
	ps1GpuWriteStatus(0x03000000u);                           // GP1(03) display enable

	// 0x02 - заливка прямокутника у VRAM: колір, потім (y<<16|x), потім (h<<16|w).
	// Слово кольору - 0xCCBBGGRR, тож 0x000080 це червоний, а не синій.
	const uint32_t fill[] = {
		0x02FFFFFFu,          // білий, як і трикутник: щоб порівняти тракти кольору
		(20u << 16) | 10u,    // x=10, y=20
		(60u << 16) | 100u    // w=100, h=60
	};
	for (unsigned i = 0; i < sizeof(fill) / sizeof(fill[0]); i++)
		ps1GpuWriteData(fill[i]);

	// 0x20 - суцільний трикутник без текстури: колір, потім три вершини.
	// Білий: у BGR555 має вийти 0x7fff, і це перевіряє тракт кольору цілком.
	const uint32_t tri[] = {
		0x20FFFFFFu,
		(200u << 16) | 200u,
		(200u << 16) | 300u,
		(260u << 16) | 250u
	};
	for (unsigned i = 0; i < sizeof(tri) / sizeof(tri[0]); i++)
		ps1GpuWriteData(tri[i]);
	}

// Один прогін: ініціалізація ядра, N кадрів, вимкнення. Викликається двічі
// (інтерпретатор, потім динарек), тож усе, що псує наступний прогін, має тут
// прибиратися - звідси EmuShutdown у кінці.
static TInt runPass(TBool aUseDrc, TInt aFrames, TBool aUseBios, TBool aGpuTest,
	TUint &aTicks, u32 &aProduced)
	{
	const char *mode = aUseDrc ? "drc" : "interp";
	aTicks = 0;
	aProduced = 0;

	// Мінімальний еквівалент emu_core_preinit() з frontend/main.c:435.
	memset(&Config, 0, sizeof(Config));
	strcpy(Config.Bios[0], "HLE");
	Config.HLE = 1;
	Config.PsxAuto = 1;
	Config.Cpu = aUseDrc ? CPU_DYNAREC : CPU_INTERPRETER;

	// Справжній BIOS вмикається другим аргументом ("bios") і поки НЕ придатний для
	// роботи: він чекає на регістри GPU і SPU, а в нас там заглушки з константами,
	// тож перший же кадр не завершується (перевірено: 156 с ЦП без жодного кадру).
	// Вмикати після інтеграції gpu_unai - Етап 2. Завантаження самого дампу справне.
	if (aUseBios)
		{
		strcpy(Config.BiosDir, KBiosDir);
		strcpy(Config.Bios[PSX_REGION_EU], KBiosFile);
		Config.PsxRegion = PSX_REGION_EU;

		// Заставка теж вимагає GPU, тож поки обходимо її (r3000a.c:93).
		Config.SlowBoot = 0;

		// Диска немає, автовизначення (misc.c:483) не спрацює; дамп європейський.
		Config.PsxType = PSX_TYPE_PAL;
		}

	SetIsoFile(NULL);

	if (EmuInit() == -1)
		{
		logEvent("fatal", "EMU_INIT_FAIL", "%s: EmuInit returned -1", mode);
		return KErrGeneral;
		}

	EmuReset();
	// Config.HLE == 0 - єдине надійне свідчення, що дамп справді підхопився.
	logEvent("reset", (aUseBios && Config.HLE) ? "BIOS_NOT_LOADED" : NULL,
		"%s: EmuReset ok, hle=%d type=%s", mode,
		(int)Config.HLE, Config.PsxType ? "PAL" : "NTSC");

	if (aGpuTest)
		{
		runGpuSelfTest();
		logEvent("gpu_selftest", NULL, "%s: GP0 fill + triangle submitted", mode);
		}

	// Поперовий лог тут НЕ ведеться навмисно: fflush коштує більше за саму
	// емуляцію (виміряно - 59 кадрів, 2274 мс проти 689 мс), і при поперовому
	// логуванні порівняння інтерпретатора з динареком міряло б файлову систему.
	// Один запис перед першим Execute: якщо процес гине на першому ж блоці,
	// у лозі має лишитись слід саме про це, а не тиша після gpu_selftest.
	flushCoreRepeat();
	logEvent("exec_begin", NULL, "%s: entering frame loop, %d frames", mode, aFrames);

	// Озброїти сторож на весь прохід. Анонімний потік із малим стеком: великий
	// EPOCSTACKSIZE апарат відхиляє (RThread::Create -> KErrTooBig, див. Етап 3).
	g_wdRun = 1;
	g_wdFired = 0;
	RThread wd;
	TRequestStatus wdStat;
	const TBool haveWd =
		(wd.Create(KNullDesC, watchdogThread, 4096, NULL, (TAny *)KWatchdogMs) == KErrNone);
	if (haveWd) { wd.Logon(wdStat); wd.Resume(); }

	const u32 framesAtStart = frame_counter;
	const TUint tStart = User::NTickCount();
	TBool stalled = EFalse;
	for (int i = 0; i < aFrames; i++)
		{
		psxRegs.stop = 0;
		psxCpu->Execute(&psxRegs);

		if (g_wdFired)
			{
			logEvent("watchdog", "WATCHDOG",
				"%s: stalled >%dms at frame %d - aborting pass", mode, (int)KWatchdogMs, i);
			stalled = ETrue;
			break;
			}

		// Рідкісні маркери: 6 записів на прогін вимір не псують, зате показують,
		// доки дійшло, коли процес гине без жодного повідомлення.
		if ((i + 1) % KProgressEvery == 0)
			{
			logEvent("progress", NULL, "%s: frame %d/%d ms=%u", mode, i + 1, aFrames,
				(unsigned)(User::NTickCount() - tStart));
			}
		}
	aTicks = User::NTickCount() - tStart;
	aProduced = frame_counter - framesAtStart;

	// Роззброїти сторож: просимо вийти й чекаємо, поки він завершиться сам.
	g_wdRun = 0;
	if (haveWd) { User::WaitForRequest(wdStat); wd.Close(); }

	flushCoreRepeat();
	logEvent("pass", stalled ? "STALLED" : (aProduced ? NULL : "NO_FRAMES"),
		"%s: frames=%u ms=%u pc=%08x cycle=%u", mode,
		(unsigned)aProduced, (unsigned)aTicks,
		(unsigned)psxRegs.pc, (unsigned)psxRegs.cycle);

	EmuShutdown();
	return aProduced ? KErrNone : KErrGeneral;
	}

static TInt runStage1()
	{
	TInt framesToRun;
	TBool useBios, gpuTest, useDrc, showScreen, haveArgs;
	parseCommandLine(framesToRun, useBios, gpuTest, useDrc, showScreen, haveArgs);

	bindStubPlugins();

	// gpulib читає лічильники кадрів і рядків напряму зі структури фронтенду
	// (gpu.c:1218-1264), тож підключаємо їх до справжніх змінних ядра.
	ps1GpuSetCallbacks(&frame_counter, &g_gpuHcnt);

	// Без аргументів (запуск з меню апарата) кількість кадрів менша: на 332 МГц
	// інтерпретатор помітно повільніший за хост, а чекати на телефон незручно.
	if (!haveArgs)
		framesToRun = KDeviceFrames;

	logEvent("start", NULL, "stage1 begin, frames=%d bios=%d gputest=%d args=%d",
		(int)framesToRun, (int)useBios, (int)gpuTest, (int)haveArgs);

	TInt rc;
	TUint interpTicks = 0, drcTicks = 0;
	u32 interpFrames = 0, drcFrames = 0;

	if (haveArgs)
		{
		rc = runPass(useDrc, framesToRun, useBios, gpuTest,
			useDrc ? drcTicks : interpTicks, useDrc ? drcFrames : interpFrames);
		}
	else
		{
		// Спершу інтерпретатор - він відомо працює, тож дає базу для порівняння
		// навіть якщо динарек упаде наступним.
		rc = runPass(EFalse, framesToRun, useBios, EFalse, interpTicks, interpFrames);
		const TInt rcDrc = runPass(ETrue, framesToRun, useBios, ETrue, drcTicks, drcFrames);
		if (rc == KErrNone)
			rc = rcDrc;

		// Частки на кадр у мкс: цілочисельно, бо ділення в мс губить різницю.
		const unsigned interpUs = interpFrames ? (unsigned)(interpTicks * 1000 / interpFrames) : 0;
		const unsigned drcUs = drcFrames ? (unsigned)(drcTicks * 1000 / drcFrames) : 0;
		logEvent("compare", NULL,
			"interp %ums/%uf=%uus_per_frame, drc %ums/%uf=%uus_per_frame",
			(unsigned)interpTicks, (unsigned)interpFrames, interpUs,
			(unsigned)drcTicks, (unsigned)drcFrames, drcUs);
		}

	// Знімок VRAM для перевірки растеризації на хості (Етап 2).
	const int dumpRc = voutSymbianDump("E:\\ps1_vram.bin");
	logEvent("vram_dump", dumpRc ? "DUMP_FAIL" : NULL, "rc=%d", dumpRc);

	// Виведення того самого кадру на екран апарата (Етап 2). Лог фіксує код
	// повернення: KErrNotReady (-18) означає, що vout_update жодного разу не
	// віддав кадр, решта від'ємних - збій window server / FBS.
	if (showScreen)
		{
		const int showRc = voutSymbianShow(KScreenHoldMs);
		logEvent("screen", showRc ? "SHOW_FAIL" : NULL, "rc=%d hold=%dms",
			showRc, KScreenHoldMs);
		}

	logEvent("shutdown", NULL, NULL);
	return rc;
	}

GLDEF_C TInt E32Main()
	{
	g_startTick = User::NTickCount();
	openLog();
	logEvent("boot", NULL, "stage1 starting");

	CTrapCleanup *cleanup = CTrapCleanup::New();
	if (!cleanup)
		return KErrNoMemory;

	TInt result = KErrNoMemory;
	TRAPD(err, result = runStage1());
	if (err != KErrNone)
		{
		logEvent("fatal", "LEAVE", "leave code %d", (int)err);
		result = err;
		}

	if (g_log)
		fclose(g_log);
	delete cleanup;
	return result;
	}
