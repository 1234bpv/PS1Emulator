// Етап 1: завантажити libpcsxcore з інтерпретатором і обчислити кадри.
// Без виведення на екран, без звуку, без диска (HLE BIOS).
// Контракт фронтенду та механізм кадру - docs/architecture/data-spec.md.

#include <e32base.h>
#include <e32std.h>
#include <e32debug.h>
#include <f32file.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "compat_stdio.h"

extern "C" {
#include "psxcommon.h"
#include "r3000a.h"
#include "plugins.h"
#include "psxcounters.h"
#include "new_dynarec/new_dynarec.h"     // ndrc_g.hacks
#include "sio.h"                         // LoadMcds
// Привід CD: cdra_init/cdra_open (cdrom-async.c) і CheckCdrom (misc.c) не в наших
// заголовках. cdra_open робить ISOopen(GetIsoFile) - саме воно відкриває образ.
int cdra_init(void);
int cdra_open(void);
int cdra_readTrack(const unsigned char *time);
void cdra_close(void);
int CheckCdrom(void);
}

// Поза extern "C": інтерфейс імпорту - C++ (класи, посилання).
#include "imgconv.h"

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
static const char KBiosDir[]  = "E:\\PS1\\bios";
static const char KBiosFile[] = "scph7502.bin";
// Диск гри на картці: вказуємо на .bin (образ даних). cdriso сам знаходить .cue
// заміною розширення (cdriso.c:359 parsecue) для розкладки доріжок.
static const char KDiscImg[]  = "E:\\PS1\\games\\gt.bin";

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

// in_keystate[0] тримає натиснуті кнопки active-high (біти DKEY_*), заповнюється
// щокадру з клавіш апарата (voutSymbianPollPad). PS1 очікує active-low, звідси ~.
void PAD1_readPort(struct PadDataS *pad, int *is_multitap)
	{
	if (pad)
		{
		memset(pad, 0, sizeof(*pad));
		pad->controllerType = PSE_PAD_TYPE_STANDARD;
		pad->buttonStatus = (unsigned short)~in_keystate[0];
		}
	if (is_multitap) *is_multitap = 0;
	}

void PAD2_readPort(struct PadDataS *pad, int *is_multitap)
	{
	if (pad) memset(pad, 0, sizeof(*pad));   // порт 2 порожній
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
void ps1GpuSetCallbacks(unsigned int *frameCount, unsigned int *hcnt, int frameskip);

int voutSymbianDump(const char *path);
int voutSymbianScreenDump(const char *path);
int voutSymbianUpdates(void);
unsigned int voutSymbianPresents(void);
int voutSymbianGpulibUpdates(void);
int voutSymbianShow(int holdMs);
int voutSymbianInit(void);
unsigned short voutSymbianPollPad(void);
void voutSymbianPresent(void);
int voutSymbianFinish(int holdMs);
void voutSymbianGpuStats(unsigned *data, unsigned *dataMem, unsigned *dma, unsigned *status);
void voutSymbianBlitSplit(unsigned *fillMs, unsigned *presentMs);
void voutSymbianSetDsa(int on);
int voutSymbianDsaActive(void);
void voutSymbianSetGl(int on);
int voutSymbianGlActive(void);
void voutSymbianFbInfo(int *ok, int *xpix, int *ypix, int *lineBytes, int *bpp, int *firstOff, int *winW, int *winH);
int voutSymbianFbTest(void);
void voutSymbianSetIlaceOut(int on);
void voutSymbianSetDirtyOnly(int on);
void voutSymbianSetRowCopy(int on);
int voutSymbianFbFromPlatform(void);
void voutSymbianGeom(int *srcW, int *srcH, int *dstW, int *dstH, int *ox, int *oy);
void voutSymbianDbgState(int *fbDirty, int *fbDirtyArea, int *status, int *srcX,
	int *srcY, int *scrW, int *scrH, int *voutOffset, int *stale);
void voutSymbianScreenLayout(int *bits, int *stride, int *w, int *h, int *mode, int *rotation, int *halBase);
int voutSymbianReadBack(int mode, int *match, int *total, int *firstBad);
void voutSymbianFbPages(int *changes, int *seen, int *b0, int *b1);
int voutSymbianHalDump(const char *path);
int voutSymbianHalAddr(void);
int voutSymbianRawDump(const char *path, int bytes);
int voutSymbianVisTest(int raw);
void voutSymbianSetOverlay(int on);
void voutSymbianSetNoUpdate(int on);
void voutSymbianSetSmooth(int on);
void voutSymbianSetFxaa(int threshold, int max);
void voutSymbianSetStats(int fpsX10, int msX10);
void voutSymbianSetPumpAo(int on);
static TBool g_fbVis;

// Лічильники безпосередньо в gpulib: усі шляхи ядра (interp/drc) сходяться туди,
// тож це достовірний сигнал, чи BIOS реально шле команди малювання.
extern unsigned int g_gpulibWriteData, g_gpulibWriteDataMem, g_gpulibDmaChain;
extern unsigned int g_gpulibDmaWords, g_gpulibWriteStatus, g_gpulibReadStatus;
extern unsigned int g_gpulibStatusLast;
// Час растеризації gpu_unai (тики FastCounter) - розділяє CPU vs GPU в "emu".
extern unsigned long long g_gpulibRenderTicks;
// Час мікшування SPU (тики FastCounter) - відділяє звук від CPU всередині "emu".
extern unsigned long long g_spuMixTicks;
// Час читання сектора з образу (тики FastCounter) разом з розпакуванням, якщо
// образ стиснений, і кількість читань - ціна формату образу всередині "emu".
extern unsigned long long g_cdReadTicks;
extern unsigned int g_cdReadCount;
// Час декодування MDEC (тики FastCounter) - ціна відео всередині "emu".
extern unsigned long long g_mdecDecodeTicks;
extern unsigned int g_mdecWords;
unsigned int ndrc_symbian_fastfreq(void);
unsigned int ndrc_symbian_fastcount(void);
unsigned int ndrc_symbian_tcbase(void);
}

// Скільки тримати кадр на екрані, щоб людина встигла глянути (запуск headless
// по WiFi: дивимось на сам апарат під час прогону).
static const int KScreenHoldMs = 5000;

// Стан проходу й стадія кадру оголошені тут, а не біля самого циклу: їх читає
// сторожовий потік (чорна скринька нижче), який визначений раніше за цикл.
enum TFrameStage { EStageIdle = 0, EStagePoll, EStageExec, EStageBlit, EStageSnd };
static volatile TInt g_stage;

static struct
	{
	const char *mode;
	TInt frames;
	TBool showScreen;
	TBool haveWd;
	TBool stalled;
	TInt i;
	TUint tStart, tPoll, tEmu, tBlit, tSnd;
	u32 framesAtStart;
	TUint ticks;
	u32 produced;
	} g_pass;

// Захист від зависання. Цикл кадрів покладається на те, що psxCpu->Execute()
// повертається на межі кадру (pl_frame_limit виставляє psxRegs.stop на VBlank).
// Реальний BIOS може крутитися в ранній ініціалізації ДО першого VBlank - тоді
// Execute не повертається ніколи, а демон розгортання блокований у RUN без
// таймауту, тобто телефон знімається лише ребутом. Сторожовий потік через
// KWatchdogMs виставляє psxRegs.stop (інтерпретатор перевіряє його щоінструкції,
// while(!regs->stop)), і прохід завершується з частковим результатом у лозі.
static const TInt KWatchdogMs = 60000;

// Меню-запуск (alpha 1.0) грає гру, тож потрібні великий бюджет кадрів і довший
// сторож, щоб не обірвати сесію. Прохід усе одно самозавершується (не лишаємо
// вічний консольний процес, який важко зняти).
static const TInt KPlayFrames = 200000;
static const TInt KPlayWatchdogMs = 1200000;    // 20 хв
static TInt g_watchdogMs = KWatchdogMs;         // бенчмарк за замовчуванням

// Стеля тривалості сеансу, мс. Нуль - без обмеження (звичайна гра з меню).
// Аргумент "limit<N>" ставить її для прогонів через демон.
static TInt g_playLimitMs;

// Скільки кадрів прорахувати. Аргумент "frames<N>". Для замірів це надійніше за
// обмеження часом: за фіксований час швидша збірка встигає далі по грі й малює
// ІНШІ сцени, тож порівнювати нема чого. Однакова кількість кадрів із однаковим
// слідом вводу дає однакову роботу, і різниця в часі - це справді різниця коду.
static TInt g_playFrames;

// Аргумент "nosnd": прогін без звуку в динаміку.
static TBool g_noSound;

// Прискорювачі й якість растеризації. Усі за замовчуванням вимкнені, крім тих,
// де вимкнення саме по собі є втратою (див. light/blend нижче).
//   pixskip  - не малювати невидимі пікселі у високих горизонтальних режимах
//   ilace<N> - пропускати рядки (1 = кожен другий)
//   gtenf    - не рахувати прапорці GTE в динареку
//   light/blend - модуляція текстур освітленням і напівпрозорість; вимкнені
//                 вони були НЕ навмисно, а через нульовий gpu_unai_config_ext
//   cyclemul<N> - скільки тактів коштує інструкція гостя (175 = штатно)
static TInt g_hackPixelSkip;
static TInt g_hackIlace;
static TBool g_hackGteNoFlags;
static TInt g_gpuLighting;
static TInt g_gpuBlending;
static TInt g_cycleMul;

extern "C" void gpu_unai_set_config(int aPixelSkip, int aIlaceForce,
	int aLighting, int aFastLighting, int aBlending);
// Копіює конфіг у робочу структуру растеризатора вже після renderer_init.
extern "C" void gpu_unai_apply_config(void);

// Пропуск виведення: кожен (N+1)-й кадр показуємо, решту лише рахуємо. Емуляцію
// і растеризацію це НЕ пропускає - економить саме вивід (конвертація + вікно),
// а це 13 з 37 мс кадру. Чесніше називати це пропуском показу, а не кадрів.
static TInt g_frameskip;

// Пропуск РАСТЕРИЗАЦІЇ силами gpulib: скільки кадрів поспіль дозволено не малювати
// (0 - вимкнено). На відміну від g_frameskip вище, тут пропускається сама робота, а
// не лише її показ; gpulib ще й тримає власні охоронні перевірки (не пропускає,
// коли гра малює просто у видиму область).
static TInt g_gpuFrameskip;

// Зонди для замірів (лише командний рядок, у меню їх немає).
static TBool g_hackNoStalls;
// Кадр першого знімка екрана; 3300 - трасa, менші значення потрапляють у меню гри,
// де вона працює в 640x480 і видно роботу масштабування.
static TInt g_dumpAt = 3300;

// Накладка з метриками. Вікно усереднення ~0,5 с: за одним кадром число стрибало
// б так, що прочитати його з екрана неможливо.
static const TInt KFpsWindowMs = 500;
static TBool g_fpsOverlay;
static TUint g_fpsWinTick;
static TInt  g_fpsWinFrame;
static TUint g_fpsWinShown;   // лічильник показаних кадрів на початку вікна

// Образ диска, обраний у меню. Порожній - беремо KDiscImg.
static char g_discPath[256];
// Ім'я дампу BIOS у теці E:\PS1\bios (без шляху - його дає Config.BiosDir).
// Порожнє означає типовий KBiosFile; задається з меню, зокрема окремо на гру.
static char g_biosFile[64];

// Вікно належить контейнеру CCoeControl і прив'язується до виведення ще до setup,
// тож у GUI-режимі власного вікна не створюємо й не закриваємо. Оголошено тут,
// бо від нього залежить частота діагностичних записів у сторожі (нижче).
static TBool g_guiMode;

static volatile TInt g_wdRun;    // 1, поки прохід хоче, щоб сторож жив
static volatile TInt g_wdFired;  // 1, якщо сторож спрацював (спін довше ліміту)

// Діагностика: сторож семплює pc/cycle раз на ~2 с. Траєкторія показує, чи
// виконання стоїть на одній інструкції (хендлер висить), чи повзе - без цього
// повільний/завислий BIOS нічим не локалізувати на залізі.
static const TInt KWdSamples = 40;
static volatile TUint32 g_wdPc[KWdSamples];
static volatile TUint32 g_wdCyc[KWdSamples];
static volatile TInt g_wdSamples;

// Ознака життя циклу кадрів: інкремент на кожен крок.
static volatile TInt g_wdBeat;

// Ідентифікатор головного потоку: сторож читає його контекст (де саме він виконується).
static TThreadId g_mainThreadId;

// Завершується КООПЕРАТИВНО: перевіряє g_wdRun короткими кроками й виходить сам,
// коли прохід його вимкне. Так не треба RThread::Kill (паніка при вбивстві вже
// мертвого потоку) і немає гонки на завершенні.
static void crashHandler(TExcType aType);

// Знімок стека з обробника виключень. Ядро кладе збережений контекст збійного
// коду на стек потоку перед викликом обробника, тож адреса збійної інструкції
// лежить у цих словах - шукати серед них значення в межах кешу трансляцій або
// коду програми. Сам обробник у файл не пише: stdio з нього мовчав, а RFs звідти
// відкривати надто ризиковано (вторинний збій - і не лишиться нічого).
// Обробник не робить ЖОДНОГО системного виклику до публікації: минулий варіант
// брав StackInfo і чекав через User::After, і не записав нічого - блокувальний
// виклик з контексту виключення знімає потік мовчки. Вершина стека тому
// запам'ятовується заздалегідь (paintStack), а чекає обробник спін-циклом.
static const TInt KCrashWords = 64;
static volatile TInt g_crashType = -1;
static volatile TInt g_crashStep;
static volatile TUint32 g_crashSp;
static volatile TUint32 g_crashStack[KCrashWords];
static volatile TInt g_crashWords;
static volatile TInt g_crashPublished;
static TUint32 g_stackBase;

// Слід рекомпілятора (new_dynarec.c): який гостьовий блок і який прохід саме
// виконувався. Без цього адреса збійної інструкції нічого не каже про причину.
extern "C" {
extern unsigned int ndrc_dbg_addr, ndrc_dbg_pass, ndrc_dbg_i, ndrc_dbg_slen, ndrc_dbg_out;
}

static TInt watchdogThread(TAny *aArg)
	{
	// Обробник виключень - на потік, а не на процес: без цього збій саме в сторожі
	// вбивав би процес мовчки, і виглядало б це так само, як збій у циклі кадрів.
	User::SetExceptionHandler(crashHandler,
		KExceptionAbort | KExceptionKill | KExceptionUserInterrupt | KExceptionFpe |
		KExceptionFault | KExceptionInteger | KExceptionDebug);

	// Чорна скринька. KERN-EXEC 3 знімає процес так, що обробник виключень не
	// встигає нічого записати (перевірено: встановлюється, але не викликається),
	// тож стан пише СУСІДНІЙ потік - те, що вже лягло на флеш, паніка не відбере.
	// Нативний RFile, а не stdio: ESTLIB тримає стан C-бібліотеки на потік, і
	// fopen звідси просто повертав NULL (файл не з'являвся).
	// Контекст головного потоку: його PC каже, ДЕ він виконується - у кеші
	// трансляцій (згенерований код) чи в коді програми (сам рекомпілятор). Це
	// розділяє простір пошуку навпіл, чого сам лічильник стадій не дає.
	RThread mainT;
	const TBool haveMainT = (mainT.Open(g_mainThreadId) == KErrNone);

	RFs beatFs;
	RFile beatFile;
	TBool haveBeat = EFalse;
	if (beatFs.Connect() == KErrNone)
		{
		_LIT(KBeatPath, "E:\\PS1\\logs\\ps1_beat.jsonl");
		_LIT(KBeatPrev, "E:\\PS1\\logs\\ps1_beat_prev.jsonl");
		beatFs.Delete(KBeatPrev);
		beatFs.Rename(KBeatPath, KBeatPrev);
		haveBeat = (beatFile.Replace(beatFs, KBeatPath, EFileWrite | EFileShareAny) == KErrNone);
		}

	const TInt limitMs = (TInt)aArg;
	// Крок 5 мс: після збою процес живе лічені десятки мілісекунд, і рідший
	// крок не встигає зафіксувати зупинку (перевірено - на 25 мс губилось).
	const TInt step = 5;
	TInt elapsed = 0;
	TInt idleMs = 0;
	TInt lastBeat = -1;
	TInt nextSampleMs = 0;
	TInt nextBeatMs = 0;
	TUint32 lastHpc = 0, lastHsp = 0, lastCycle = 0;
	TBool stuckDumped = EFalse;
	const TInt KSnapWords = 88;
	const TInt KSnapBelow = 32;
	TUint32 snap[KSnapWords];
	TUint32 snapSp = 0;
	TInt snapCount = 0;
	while (g_wdRun && idleMs < limitMs)
		{
		if (haveBeat && elapsed >= nextBeatMs)
			{
			unsigned gpData = 0, gpDataMem = 0, gpDma = 0, gpStatus = 0;
			voutSymbianGpuStats(&gpData, &gpDataMem, &gpDma, &gpStatus);
			// Купа: метадані динареку (блоки, списки переходів, хеш-таблиці)
			// живуть саме тут і ростуть найшвидше на завантаженні нового коду.
			// EPOCHEAPSIZE обмежує їх зверху, а невдалий malloc у динареку
			// перевіряється не скрізь - тоді запис іде за нульовим покажчиком.
			TInt heapBiggest = 0;
			const TInt heapAvail = User::Heap().Available(heapBiggest);

			// r13/r14/r15 головного потоку. TArmRegSet - масив 32-бітних слів,
			// PC у ньому 16-й (індекс 15).
			TUint32 hsp = 0, hlr = 0, hpc = 0;
			if (haveMainT)
				{
				TBuf8<128> ctx;
				mainT.Context(ctx);
				const TUint32 *r = (const TUint32 *)ctx.Ptr();
				if (ctx.Length() >= 16 * 4)
					{ hsp = r[13]; hlr = r[14]; hpc = r[15]; }
				}
			TBuf8<448> line;
			line.Format(_L8("{\"ms\":%u,\"stage\":%d,\"frame\":%d,\"pc\":\"%08x\","
				"\"cycle\":%u,\"gp0\":%u,\"dataMem\":%u,\"dma\":%u,\"gp1\":\"%08x\","
				"\"heapKb\":%d,\"heapFreeKb\":%d,"
				"\"hpc\":\"%08x\",\"hlr\":\"%08x\",\"hsp\":\"%08x\",\"tc\":\"%08x\","
				"\"blk\":\"%08x\",\"rp\":%u,\"ri\":%u,\"rn\":%u,\"out\":\"%08x\"}\n"),
				(TUint)(User::NTickCount() - g_startTick), (TInt)g_stage, (TInt)g_pass.i,
				(TUint)psxRegs.pc, (TUint)psxRegs.cycle,
				(TUint)gpData, (TUint)gpDataMem, (TUint)gpDma, (TUint)gpStatus,
				(TInt)(User::Heap().Size() / 1024), (TInt)(heapAvail / 1024),
				hpc, hlr, hsp, (TUint32)ndrc_symbian_tcbase(),
				(TUint32)ndrc_dbg_addr, (TUint)ndrc_dbg_pass, (TUint)ndrc_dbg_i,
				(TUint)ndrc_dbg_slen, (TUint32)ndrc_dbg_out);
			beatFile.Write(line);
			beatFile.Flush();
			// Кожен удар - це запис із Flush на картку пам'яті, тобто службова
			// операція FAT посеред кадру. У полюванні на збій (консоль) 50 мс
			// виправдані: після падіння процес живе десятки мілісекунд. У грі це
			// 20 звернень до флеш-пам'яті щосекунди - рівно ті регулярні
			// підфризування, які видно на геймплеї, тож там крок секундний.
			nextBeatMs += g_guiMode ? 1000 : 50;

			// Адреса збійної інструкції без обробника виключень. Той на цьому
			// апараті не викликається, зате ядро лишає контекст збійного коду на
			// стеку головного потоку - а сторож живе в тому ж процесі й читає
			// його пам'ять напряму. Ознака зупинки: pc, sp І лічильник тактів
			// гостя не змінилися між відліками, хоча стадія - виконання.
			if (!stuckDumped && g_stage == EStageExec && hpc == lastHpc &&
				hsp == lastHsp && (TUint32)psxRegs.cycle == lastCycle && hsp)
				{
				// Пишемо знімок ПОПЕРЕДНЬОГО відліку, а не свіжий: на цьому кроці
				// головний потік уже мертвий, його стек відображення не має, і
				// читання звідти вбивало сам сторож (перевірено - файл обривався).
				// Одним записом: процес помирає за мілісекунди.
				stuckDumped = ETrue;
				TBuf8<1200> rec;
				rec.Format(_L8("{\"stuck\":1,\"hpc\":\"%08x\",\"excsp\":\"%08x\","
					"\"hsp\":\"%08x\",\"words\":["), hpc, hsp, snapSp);
				for (TInt w = 0; w < snapCount; ++w)
					{
					if (w) rec.Append(',');
					rec.AppendFormat(_L8("\"%08x\""), snap[w]);
					}
				rec.Append(_L8("]}\n"));
				beatFile.Write(rec);
				beatFile.Flush();
				}
			else if (hsp)
				{
				// Котиться щовідліку, поки потік живий: читати його стек безпечно
				// лише до збою, а потрібен він саме за станом на момент збою.
				// Вікно і вниз від sp: невідомо, з якого боку ядро кладе контекст
				// збійного коду, а кадр збійної функції лежить саме нижче.
				TUint32 lo = hsp - KSnapBelow * 4;
				TUint32 top = lo + KSnapWords * 4;
				if (g_stackBase && top > g_stackBase) top = g_stackBase;
				const TUint32 *sp = (const TUint32 *)lo;
				snapCount = (top > lo) ? (TInt)((top - lo) / 4) : 0;
				if (snapCount > KSnapWords) snapCount = KSnapWords;
				for (TInt w = 0; w < snapCount; ++w)
					snap[w] = sp[w];
				snapSp = lo;
				}
			lastHpc = hpc; lastHsp = hsp; lastCycle = (TUint32)psxRegs.cycle;
			}
		if (haveBeat && g_crashType >= 0 && !g_crashPublished)
			{
			TBuf8<192> head;
			head.Format(_L8("{\"crash\":%d,\"step\":%d,\"sp\":\"%08x\","
				"\"blk\":\"%08x\",\"rp\":%u,\"ri\":%u,\"rn\":%u,\"out\":\"%08x\","
				"\"tc\":\"%08x\",\"stack\":["),
				(TInt)g_crashType, (TInt)g_crashStep, (TUint32)g_crashSp,
				(TUint32)ndrc_dbg_addr, (TUint)ndrc_dbg_pass, (TUint)ndrc_dbg_i,
				(TUint)ndrc_dbg_slen, (TUint32)ndrc_dbg_out,
				(TUint32)ndrc_symbian_tcbase());
			beatFile.Write(head);
			for (TInt w = 0; w < g_crashWords; ++w)
				{
				TBuf8<16> cell;
				cell.Format(_L8("%s\"%08x\""), w ? "," : "", (TUint32)g_crashStack[w]);
				beatFile.Write(cell);
				}
			beatFile.Write(_L8("]}\n"));
			beatFile.Flush();
			g_crashPublished = 1;
			}
		if (elapsed >= nextSampleMs && g_wdSamples < KWdSamples)
			{
			g_wdPc[g_wdSamples] = psxRegs.pc;
			g_wdCyc[g_wdSamples] = psxRegs.cycle;
			g_wdSamples++;
			nextSampleMs += 2000;
			}
		User::After(step * 1000);
		elapsed += step;
		// Не даємо екрану згаснути. Це не зручність, а умова порівнюваності: коли
		// дисплей гасне, віконному серверу нема чого компонувати, і present падає
		// з 12000 до 1700 мс на 9000 кадрів - більше за будь-який ефект, який ми
		// тут міряємо. Раз на 10 с достатньо.
		if ((elapsed % 10000) < step)
			User::ResetInactivityTime();
		// Міряємо ЗАСТІЙ, а не тривалість проходу: у GUI сесія триває годинами, і
		// ліміт на весь прохід обривав би гру посеред неї. Зупинка лічильника
		// кадрів - саме той випадок, заради якого сторож існує (Execute не
		// повертається, бо BIOS крутиться до першого VBlank).
		if (g_wdBeat != lastBeat) { lastBeat = g_wdBeat; idleMs = 0; }
		else                      { idleMs += step; }
		}
	if (g_wdRun)
		{
		psxRegs.stop = 1;
		g_wdFired = 1;
		}
	if (haveBeat)
		beatFile.Close();
	beatFs.Close();
	if (haveMainT)
		mainT.Close();
	return 0;
	}

static unsigned int g_gpuHcnt;

// Звук: справжній SPU - dfsound через обгортки ps1Spu* у snd_symbian.cpp (там
// plugins.h не включено, тож назви функцій dfsound не конфліктують із typedef).
extern "C" {
long ps1SpuInit(void);
long ps1SpuShutdown(void);
long ps1SpuOpen(void);
long ps1SpuClose(void);
void ps1SpuConfigure(void);
void ps1SpuWriteRegister(unsigned long, unsigned short, unsigned int);
unsigned short ps1SpuReadRegister(unsigned long, unsigned int);
void ps1SpuWriteDMAMem(unsigned short *, int, unsigned int);
void ps1SpuReadDMAMem(unsigned short *, int, unsigned int);
void ps1SpuAsync(unsigned int, unsigned int);
void ps1SpuRegisterCallback(void (*cb)(int));
void ps1SpuRegisterScheduleCb(void (*cb)(unsigned int));
void ps1SndConfigure(void);
void ps1SndSelectOutput(void);
void ps1SndStats(unsigned *feeds, unsigned *samples, int *maxAmp,
                 unsigned *under, int *stepQ16);
int  ps1SndOpenOutput(void);
void ps1SndPump(void);
void ps1SndCloseOutput(void);
void ps1SndSetPumpAo(int aOn);
}

// XA/CD-аудіо та freeze для звуку BIOS не потрібні - лишаємо заглушками.
static void CALLBACK spuPlayADPCMchannel(xa_decode_t *, unsigned int, int)    { }
static long CALLBACK spuFreeze(int, SPUFreeze_t *, unsigned short **, void *, unsigned int) { return 0; }
static int  CALLBACK spuPlayCDDAchannel(short *, int, unsigned int, int)      { return 0; }
static void CALLBACK spuSetCDvol(unsigned char, unsigned char, unsigned char, unsigned char, unsigned int) { }

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

	SPU_init             = ps1SpuInit;
	SPU_shutdown         = ps1SpuShutdown;
	SPU_open             = ps1SpuOpen;
	SPU_close            = ps1SpuClose;
	SPU_configure        = ps1SpuConfigure;
	SPU_writeRegister    = ps1SpuWriteRegister;
	SPU_readRegister     = ps1SpuReadRegister;
	SPU_writeDMAMem      = ps1SpuWriteDMAMem;
	SPU_readDMAMem       = ps1SpuReadDMAMem;
	SPU_async            = ps1SpuAsync;
	SPU_registerCallback = ps1SpuRegisterCallback;
	SPU_registerScheduleCb = ps1SpuRegisterScheduleCb;
	SPU_playADPCMchannel = spuPlayADPCMchannel;
	SPU_freeze           = spuFreeze;
	SPU_playCDDAchannel  = spuPlayCDDAchannel;
	SPU_setCDvol         = spuSetCDvol;
	}

// --- Точка входу ------------------------------------------------------------

// Один попередній лог зберігаємо: запуск затирає поточний, і цікавий прогін
// пропадає, щойно застосунок піднімуть ще раз - на цьому вже раз згоріли.
static void rotateLog(const TDesC &aPath, const TDesC &aPrev)
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	fs.Delete(aPrev);
	fs.Rename(aPath, aPrev);
	fs.Close();
	}

// Тека емулятора має існувати до першого fopen: ESTLIB тек не створює, і без
// цього і лог, і образи мовчки не відкривались би на чистому апараті.
void ps1EnsureDirs()
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	fs.MkDirAll(_L("E:\\PS1\\games\\"));
	fs.MkDirAll(_L("E:\\PS1\\bios\\"));
	fs.MkDirAll(_L("E:\\PS1\\cfg\\"));
	fs.MkDirAll(_L("E:\\PS1\\saves\\"));
	fs.MkDirAll(_L("E:\\PS1\\logs\\"));
	fs.Close();
	}

static void openLog()
	{
	_LIT(KLogPath, "E:\\PS1\\logs\\ps1_stage1.jsonl");
	_LIT(KLogPrev, "E:\\PS1\\logs\\ps1_stage1_prev.jsonl");
	rotateLog(KLogPath, KLogPrev);
	// E: - змонтована карта пам'яті, тобто тека хоста: лог видно ззовні емулятора.
	g_log = fopen("E:\\PS1\\logs\\ps1_stage1.jsonl", "w");
	if (!g_log)
		g_log = fopen("C:\\ps1_stage1.jsonl", "w");
	}

// Командний рядок: [кількість кадрів] [bios] [gputest] [drc]
// aHaveArgs == EFalse означає запуск з меню телефона - там аргументів немає,
// і тоді проганяються обидва режими ЦП підряд для порівняння.
static void parseCommandLine(TInt &aFrames, TBool &aUseBios, TBool &aGpuTest, TBool &aDrc,
	TBool &aShowScreen, TBool &aUseDisc, TBool &aHaveArgs)
	{
	aFrames = KDefaultFrames;
	aUseBios = EFalse;
	aGpuTest = EFalse;
	aDrc = EFalse;
	aShowScreen = EFalse;
	aUseDisc = EFalse;

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

	// Завантаження гри з диска (E:\gt.cue). Потребує справжнього BIOS, тож вмикає
	// і його. CheckCdrom сам визначить регіон (US->NTSC), тож PsxType не форсуємо.
	_LIT(KDiscArg, "disc");
	if (cmd.FindF(KDiscArg) != KErrNotFound)
		{
		aUseDisc = ETrue;
		aUseBios = ETrue;
		}

	// DSA-вивід (прямий запис у буфер екрана). Типово ВИМКНЕНИЙ - ламає 16-бітні
	// режими гри; вмикається явним "dsa" для пошуку причини.
	_LIT(KDsaArgC, "dsa");
	voutSymbianSetDsa(cmd.FindF(KDsaArgC) != KErrNotFound ? 1 : 0);
	_LIT(KNoDirtyArgC, "nodirty");
	voutSymbianSetDirtyOnly(cmd.FindF(KNoDirtyArgC) != KErrNotFound ? 0 : 1);

	// GL ES вивід (MBX): GPU масштабує й презентує через eglSwapBuffers.
	_LIT(KGlArg, "gl");
	if (cmd.FindF(KGlArg) != KErrNotFound)
		voutSymbianSetGl(1);
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
//
// Розділений на setup/step/teardown, бо GUI-оболонка виконує кадри по одному з
// активного об'єкта: суцільний цикл не віддавав би керування планувальнику, і
// фреймворк CONE не встигав би ні роздати клавіші, ні обробити вихід. Консольна
// гілка складає три частини назад у runPass, тож її поведінка не змінилась.
static RThread g_wdThread;
static TRequestStatus g_wdStat;

// Стек: замір на вершині циклу нічого не дає - глибина набирається всередині
// psxCpu->Execute (компіляція блоку), і на момент запису вже розкручена назад.
// Тому фарбуємо вільну частину взірцем один раз, а далі рахуємо, скільки його
// лишилось незачепленим: це найглибша точка ЗА ВЕСЬ ЧАС, а не миттєва.
static const TUint32 KStackPaint = 0xA5A5A5A5;
static TUint32 *g_paintFrom;
static TUint32 *g_paintTo;

static void paintStack()
	{
	TThreadStackInfo info;
	RThread me;
	if (me.StackInfo(info) != KErrNone)
		return;
	TUint8 here;
	// Вершина стека - обробникові виключень: там StackInfo викликати вже не можна.
	g_stackBase = (TUint32)info.iBase;
	// Запас під поточний кадр виклику, щоб не затерти те, чим користуємось зараз.
	g_paintFrom = (TUint32 *)info.iLimit;
	g_paintTo   = (TUint32 *)(((TLinAddr)&here - 256) & ~3u);
	for (TUint32 *p = g_paintFrom; p < g_paintTo; p++)
		*p = KStackPaint;
	}

// Скільки байтів стека жодного разу не використали. Прямує до нуля - переповнення.
static TInt stackHighWater()
	{
	if (!g_paintFrom)
		return -1;
	const TUint32 *p = g_paintFrom;
	while (p < g_paintTo && *p == KStackPaint)
		p++;
	return (TInt)((TUint8 *)p - (TUint8 *)g_paintFrom);
	}

// Виставляє оболонка на виході користувача: цикл кадрів завершується штатно.
static volatile TInt g_guiExit;

static TInt passSetup(TBool aUseDrc, TInt aFrames, TBool aUseBios, TBool aGpuTest,
	TBool aShowScreen, TBool aUseDisc)
	{
	const char *mode = aUseDrc ? "drc" : "interp";
	memset(&g_pass, 0, sizeof(g_pass));
	g_pass.mode = mode;
	g_pass.frames = aFrames;
	g_pass.showScreen = aShowScreen;

	g_gpulibWriteData = g_gpulibWriteDataMem = g_gpulibDmaChain = 0;
	g_gpulibDmaWords = g_gpulibWriteStatus = g_gpulibReadStatus = 0;
	g_gpulibStatusLast = 0;
	g_gpulibRenderTicks = 0;
	g_spuMixTicks = 0;
	g_cdReadTicks = 0;
	g_cdReadCount = 0;
	g_mdecDecodeTicks = 0;
	g_mdecWords = 0;

	// Мінімальний еквівалент emu_core_preinit() з frontend/main.c:435.
	memset(&Config, 0, sizeof(Config));
	strcpy(Config.Bios[0], "HLE");
	Config.HLE = 1;
	Config.PsxAuto = 1;
	Config.Cpu = aUseDrc ? CPU_DYNAREC : CPU_INTERPRETER;

	// Дефолти з emu_set_default_config() (frontend/main.c:108), які memset обнуляє.
	// КРИТИЧНО: cycle_multiplier. Із 0 виходить subCycleStep=0x10000*0/100=0, і
	// addCycle() (psxinterpreter.c:235) не нарощує psxRegs.cycle - таймери й VBlank
	// ніколи не цокають, і петля-очікування в BIOS крутиться вічно (виявлено
	// семплюванням pc/cycle: pc у bfc04a48-8c, cycle застиг на 555). Решта -
	// авто-сентинели (-1), які ядро інакше плутає з явним режимом 0.
	// cyclemul<N> дозволяє відхилитись від штатних 175: більше - інструкція
	// гостя коштує більше тактів, тож на кадр їх виконується менше (кадрів на
	// секунду більше, ігрова логіка повільніша). Це компроміс, не оптимізація.
	Config.cycle_multiplier = g_cycleMul > 0 ? g_cycleMul : CYCLE_MULT_DEFAULT;
	Config.GpuListWalking = -1;
	Config.FractionalFramerate = -1;
	Config.AlternativeFlip = -1;

	// Справжній BIOS вмикається другим аргументом ("bios"). Тепер GPU справжній
	// (gpu_unai), тож BIOS завантажується й виконується (див. plans/progress.md).
	if (aUseBios)
		{
		strcpy(Config.BiosDir, KBiosDir);
		// Один і той самий дамп у всі три слоти: регіон визначає CheckCdrom за
		// диском, і класти файл лише в європейський слот означало б, що для
		// японської гри BIOS мовчки не підхопиться.
		const char *bios = g_biosFile[0] ? g_biosFile : KBiosFile;
		for (int rg = 0; rg < PSX_REGION_COUNT; rg++)
			strcpy(Config.Bios[rg], bios);
		Config.PsxRegion = PSX_REGION_EU;

		// SlowBoot=1: повний бут із заставкою (логотип SCE/PS). З 0 емулятор
		// патчить BIOS, щоб пропустити анімацію - тоді екран лишається чорним аж
		// до очікування диска, і показувати нічого. Раніше було 0, бо GPU був
		// заглушкою; тепер вмикаємо, щоб побачити логотип.
		Config.SlowBoot = 1;

		// Без диска автовизначення регіону (misc.c) не спрацює - дамп європейський.
		// З диском регіон визначить CheckCdrom нижче, тож PsxType не форсуємо.
		if (!aUseDisc)
			Config.PsxType = PSX_TYPE_PAL;
		}

	// Хаки сумісності динареку вимкнено: GT падав рівно після "vsync @8008ab10"
	// (два прогони поспіль, більш нічого в лозі). Це do_vsync/vsync_hack_assemble -
	// динарек упізнає петлю "VSync: timeout" і вбудовує замість неї свій ARM-код,
	// який читає гостьову пам'ять через ram_offset/ROREG. Саме ця гілка потрібна
	// нам і майже не потрібна на Linux: там RAM лягає за фіксованою адресою
	// ("RAM is not directly mapped" у нашому лозі - ram_offset != 0). Петля
	// VSync-timeout крутиться якраз на завантаженні траси, де падіння й ловиться.
	// Ціна вимкнення - повільніше проходження цієї петлі; PE2-хак нам байдужий.
	ndrc_g.hacks |= NDHACK_NO_COMPAT_HACKS;

	// NDHACK_NO_SMC_CHECK НЕ вмикати: перевірено на апараті - без вбудованої
	// перевірки самомодифікації BIOS не стартує взагалі (чорний екран). Скидання
	// з боку C його не заміняє: BIOS пише свій код звичайними записами, а не DMA.

	// nostalls: не емулювати затримки конвеєра R3000. Точність за швидкодію -
	// сумісність падає, тож лише явним аргументом.
	if (g_hackNoStalls)
		ndrc_g.hacks |= NDHACK_NO_STALLS;

	// gtenf: не рахувати прапорці GTE. Динарек і так пропускає їх там, де аналіз
	// живучості довів, що вони не читаються; цей прапорець прибирає їх скрізь.
	if (g_hackGteNoFlags)
		ndrc_g.hacks |= NDHACK_GTE_NO_FLAGS;
	logEvent("hacks", NULL, "%s: ndrc=%08x gtenf=%d cyclemul=%d", mode,
		(unsigned)ndrc_g.hacks, (int)g_hackGteNoFlags, (int)Config.cycle_multiplier);

	// Диск гри (E:\gt.bin) або його відсутність. SetIsoFile до cdra_init - ISOopen
	// робить fopen(fname) як основний образ, тож шлях має вказувати на .bin.
	SetIsoFile(aUseDisc ? (g_discPath[0] ? g_discPath : KDiscImg) : NULL);

	// Карти пам'яті. Ядро вантажить їх не саме - це робив frontend, який ми не
	// беремо, тож досі збереження ігор мовчки нікуди не потрапляли.
	strcpy(Config.Mcd1, "E:\\PS1\\saves\\card1.mcd");
	strcpy(Config.Mcd2, "E:\\PS1\\saves\\card2.mcd");

	if (EmuInit() == -1)
		{
		logEvent("fatal", "EMU_INIT_FAIL", "%s: EmuInit returned -1", mode);
		return KErrGeneral;
		}
	LoadMcds(Config.Mcd1, Config.Mcd2);

	// Ініціалізація приводу CD: ядро кличе cdra_init лише в LoadPlugins, який ми
	// обходимо. CheckCdrom читає диск (регіон, boot-запис), як у frontend/main.c:684.
	if (aUseDisc)
		{
		const int cdInitRc = cdra_init();
		const int cdOpenRc = cdra_open();      // ISOopen(GetIsoFile) - відкриває образ
		const int cdChkRc = CheckCdrom();
		logEvent("disc", (cdInitRc || cdOpenRc) ? "CDRA_FAIL" : NULL,
			"%s: cdra_init=%d cdra_open=%d checkcdrom=%d region=%d", mode,
			cdInitRc, cdOpenRc, cdChkRc, (int)Config.PsxRegion);
		}

	// GPU_init() робиться раз на процес у runStage1 (як LoadPlugins у фронтенді);
	// тут - лише GPU_open() на кожен прохід (як OpenPlugins, frontend/main.c:210).
	// Порожній gpuDisp: наш backend вікна дисплея з нього не бере (vout_symbian).
	unsigned long gpuDisp = 0;
	const long gpuOpenRc = ps1GpuOpen(&gpuDisp, (char *)"PCSX", NULL);
	logEvent("gpu_open", gpuOpenRc ? "GPU_OPEN_FAIL" : NULL, "%s: open=%ld", mode, gpuOpenRc);

	// SPU_open викликає SetupSound (out_current = none); перенаправляємо на наш
	// драйвер (скидає лічильники амплітуди на цей прохід).
	const long spuOpenRc = SPU_open();
	ps1SndSelectOutput();
	logEvent("spu_open", spuOpenRc ? "SPU_OPEN_FAIL" : NULL, "%s: open=%ld", mode, spuOpenRc);

	// Вивід у динамік лише в інтерактивних прогонах (screen): у бенчмарку звук
	// псував би заміри, а вікна/акт.планувальника там нема.
	// Аргумент "nosnd": не відкривати вивід у динамік. SPU далі мікшує, тож
	// таймінги емуляції й хід повтору не змінюються - мовчить лише апарат.
	if (aShowScreen && !g_noSound)
		{
		const int sndRc = ps1SndOpenOutput();
		logEvent("snd_open", sndRc ? "SND_OPEN_WARN" : NULL, "%s: rc=%d", mode, sndRc);
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

	// Живий вивід: вікно один раз до циклу, кадр - щокадру всередині. У GUI воно
	// вже прив'язане до контейнера CCoeControl, тож свого не створюємо.
	if (aShowScreen)
		{
		const int initRc = g_guiMode ? KErrNone : voutSymbianInit();
		logEvent("screen_init", initRc ? "SHOW_FAIL" : NULL, "%s: rc=%d gui=%d", mode,
			initRc, (int)g_guiMode);
		// Діагностика direct-FB: сирі HAL-параметри буфера проти розміру вікна WS.
		int fbOk = 0, xpix = 0, ypix = 0, lineB = 0, bpp = 0, firstOff = 0, winW = 0, winH = 0;
		voutSymbianFbInfo(&fbOk, &xpix, &ypix, &lineB, &bpp, &firstOff, &winW, &winH);
		logEvent("fb", NULL, "%s: ok=%d hal=%dx%d line=%dB bpp=%d first=%d win=%dx%d",
			mode, fbOk, xpix, ypix, lineB, bpp, firstOff, winW, winH);
		}

	// Озброїти сторож на весь прохід. Анонімний потік із малим стеком: великий
	// EPOCSTACKSIZE апарат відхиляє (RThread::Create -> KErrTooBig, див. Етап 3).
	g_wdRun = 1;
	g_wdFired = 0;
	g_wdSamples = 0;
	g_wdBeat = 0;
	g_mainThreadId = RThread().Id();   // до створення сторожа: він читає наш контекст
	g_pass.haveWd =
		(g_wdThread.Create(KNullDesC, watchdogThread, 8192, NULL, (TAny *)g_watchdogMs) == KErrNone);
	if (g_pass.haveWd)
		{
		// Вище за головний: після збою той крутиться в спін-циклі, чекаючи запису,
		// і сторож мусить його витіснити, а не ділити з ним квант.
		g_wdThread.SetPriority(EPriorityMore);
		g_wdThread.Logon(g_wdStat);
		g_wdThread.Resume();
		}

	g_pass.framesAtStart = frame_counter;
	// Профілювання пайплайну: сумарний час (мс) на кожен етап за весь прохід.
	// NTickCount тут ~1мс; пер-кадрова похибка усереднюється на сотнях кадрів.
	g_pass.tStart = User::NTickCount();
	// Вікно накладки відкриваємо разом із проходом, а не при вмиканні в меню: між
	// меню і першим кадром лежить ініціалізація ядра (BIOS, образ диска), і перше
	// значення вийшло б утричі заниженим.
	g_fpsWinTick = g_pass.tStart;
	g_fpsWinFrame = 0;
	g_fpsWinShown = voutSymbianPresents();   // лічильник живе через проходи
	return KErrNone;
	}

// Запис і відтворення вводу. Падіння в динареку відтворюється лише через
// кількахвилинну гру руками, і кожна перевірка гіпотези коштує окремого сеансу.
// Слід кнопок, прив'язаний до НОМЕРА КАДРУ (а не до часу), знімає людину з циклу:
// той самий сеанс проганяється стільки разів, скільки треба.
//
// Формат навмисно не JSONL: файл читає сам апарат, і розбір рядка тут має бути
// тривіальним. Один рядок на ЗМІНУ стану кнопок:  <кадр> <пад hex> <цикл>
// Цикл потрібен для перевірки самого відтворення: якщо на тому ж кадрі лічильник
// розійшовся, повтор не точний, і всі висновки з нього нічого не варті.
enum TInputMode { EInputOff = 0, EInputRecord, EInputPlay };
static TInputMode g_inputMode;
_LIT(KInputPath, "E:\\PS1\\logs\\ps1_input.rec");

static RFs g_recFs;
static RFile g_recFile;
static TBool g_recOpen;
static TUint16 g_recLastPad;
static TBool g_recStarted;

// Відтворення тримається в пам'яті: слід малий (одна зміна на десятки кадрів),
// а читати з флеші щокадру - додаткова недетермінованість у таймінгах.
static const TInt KMaxPlayEvents = 8192;
static TInt *g_playFrame;
static TUint16 *g_playPad;
static TUint32 *g_playCycle;
static TInt g_playCount;
static TInt g_playNext;
static TBool g_playDiverged;

static void inputRecordOpen()
	{
	if (g_recFs.Connect() != KErrNone)
		return;
	_LIT(KInputPrev, "E:\\PS1\\logs\\ps1_input_prev.rec");
	g_recFs.Delete(KInputPrev);
	g_recFs.Rename(KInputPath, KInputPrev);
	g_recOpen = (g_recFile.Replace(g_recFs, KInputPath, EFileWrite | EFileShareAny) == KErrNone);
	if (!g_recOpen)
		g_recFs.Close();
	logEvent("input", g_recOpen ? NULL : "INPUT_OPEN_FAIL", "record open=%d", (int)g_recOpen);
	}

static void inputPlayLoad()
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	RFile f;
	TInt rc = f.Open(fs, KInputPath, EFileRead | EFileShareAny);
	if (rc != KErrNone)
		{
		fs.Close();
		logEvent("input", "INPUT_OPEN_FAIL", "replay open rc=%d", (int)rc);
		return;
		}
	TInt size = 0;
	f.Size(size);
	HBufC8 *buf = HBufC8::New(size + 1);
	if (buf)
		{
		TPtr8 p(buf->Des());
		f.Read(p);
		}
	f.Close();
	fs.Close();
	if (!buf)
		return;

	g_playFrame = new TInt[KMaxPlayEvents];
	g_playPad   = new TUint16[KMaxPlayEvents];
	g_playCycle = new TUint32[KMaxPlayEvents];
	if (g_playFrame && g_playPad && g_playCycle)
		{
		TLex8 lex(*buf);
		while (!lex.Eos() && g_playCount < KMaxPlayEvents)
			{
			TInt fr = 0; TUint32 pad = 0; TUint32 cyc = 0;
			lex.SkipSpace();
			if (lex.Val(fr) != KErrNone) break;
			lex.SkipSpace();
			if (lex.Val(pad, EHex) != KErrNone) break;
			lex.SkipSpace();
			if (lex.Val(cyc, EDecimal) != KErrNone) break;
			g_playFrame[g_playCount] = fr;
			g_playPad[g_playCount]   = (TUint16)pad;
			g_playCycle[g_playCount] = (TUint32)cyc;
			g_playCount++;
			}
		}
	delete buf;
	logEvent("input", NULL, "replay events=%d", (int)g_playCount);
	}

static void inputTraceOpen()
	{
	if (g_inputMode == EInputRecord) inputRecordOpen();
	else if (g_inputMode == EInputPlay) inputPlayLoad();
	}

static void inputTraceClose()
	{
	if (g_recOpen)
		{
		g_recFile.Close();
		g_recFs.Close();
		g_recOpen = EFalse;
		}
	delete[] g_playFrame; g_playFrame = NULL;
	delete[] g_playPad;   g_playPad = NULL;
	delete[] g_playCycle; g_playCycle = NULL;
	g_playCount = g_playNext = 0;
	}

// Викликається щокадру одразу після опитування клавіш: у записі фіксує зміни,
// у відтворенні ПЕРЕЗАПИСУЄ in_keystate[0] слідом.
static void inputTrace(TInt aFrame)
	{
	if (g_inputMode == EInputRecord)
		{
		if (!g_recOpen) return;
		const TUint16 pad = in_keystate[0];
		if (g_recStarted && pad == g_recLastPad) return;
		g_recStarted = ETrue;
		g_recLastPad = pad;
		TBuf8<48> line;
		line.Format(_L8("%d %04x %u\n"), aFrame, (TUint)pad, (TUint)psxRegs.cycle);
		g_recFile.Write(line);
		g_recFile.Flush();
		return;
		}
	if (g_inputMode != EInputPlay || g_playCount == 0)
		return;

	while (g_playNext < g_playCount && g_playFrame[g_playNext] <= aFrame)
		{
		// Розбіжність лічильника циклів на тому самому кадрі означає, що повтор
		// пішов іншим шляхом (найімовірніше - інший таймінг читання з диска).
		// Кажемо про це один раз: далі всі відхилення вже похідні.
		if (!g_playDiverged && g_playFrame[g_playNext] == aFrame &&
			g_playCycle[g_playNext] != (TUint32)psxRegs.cycle)
			{
			g_playDiverged = ETrue;
			logEvent("input", "REPLAY_DIVERGED",
				"frame %d: cycle %u, recorded %u (delta %d)",
				(int)aFrame, (unsigned)psxRegs.cycle, (unsigned)g_playCycle[g_playNext],
				(int)((TInt32)psxRegs.cycle - (TInt32)g_playCycle[g_playNext]));
			}
		in_keystate[0] = g_playPad[g_playNext];
		g_playNext++;
		}
	}

// Один кадр. EFalse = прохід завершено (кадри вичерпано, сторож або вихід).
static TBool passStep()
	{
	if (g_pass.i >= g_pass.frames || g_guiExit)
		return EFalse;
	// Стеля на сеанс. Поки йде налагодження виводу, прогін має завершуватись сам:
	// демон блокований на дочірньому процесі й не шле keepalive, тож довга сесія
	// щоразу коштує розриву WiFi і повторного пошуку апарата в мережі.
	if (g_playLimitMs > 0 && (TInt)(User::NTickCount() - g_pass.tStart) >= g_playLimitMs)
		{
		logEvent("timelimit", NULL, "%s: %d ms reached at frame %d",
			g_pass.mode, (int)g_playLimitMs, (int)g_pass.i);
		return EFalse;
		}

	const char *mode = g_pass.mode;

	// Ввід: свіжий стан кнопок до Execute, бо ядро читає пад усередині кадру.
	// Лише за наявності вікна (screen); у headless-бенчмарку клавіш нема.
	TUint t0 = User::NTickCount();
	g_stage = EStagePoll;
	if (g_pass.showScreen)
		in_keystate[0] = voutSymbianPollPad();
	inputTrace(g_pass.i);

	TUint t1 = User::NTickCount(); g_pass.tPoll += t1 - t0;
	g_stage = EStageExec;
	psxRegs.stop = 0;
	psxCpu->Execute(&psxRegs);
	TUint t2 = User::NTickCount(); g_pass.tEmu += t2 - t1;
	g_wdBeat++;

	if (g_wdFired)
		{
		logEvent("watchdog", "WATCHDOG",
			"%s: stalled >%dms at frame %d - aborting pass", mode, (int)g_watchdogMs, g_pass.i);
		g_pass.stalled = ETrue;
		return EFalse;
		}

	// Живий кадр на екран. Додає накладні (конвертація+BitBlt), тож псує
	// заміри - вмикається лише за прапорцем screen, не в бенчмарку.
	if (g_pass.showScreen)
		{
		g_stage = EStageBlit;
		// Пропуск показу: емуляція й растеризація йдуть щокадру, на екран
		// потрапляє кожен (skip+1)-й. Звук пампимо завжди - інакше буфер
		// спорожніє саме на пропущених кадрах.
		if (!g_frameskip || (g_pass.i % (g_frameskip + 1)) == 0)
			voutSymbianPresent();
		TUint t3 = User::NTickCount(); g_pass.tBlit += t3 - t2;
		g_stage = EStageSnd;
		ps1SndPump();   // обслужити аудіо-callback'и й долити буфер
		g_pass.tSnd += User::NTickCount() - t3;
		}

	g_stage = EStageIdle;
	g_pass.i++;

	// Метрики для накладки. fps - ПОКАЗАНІ кадри: емуляційний темп залежить від
	// множника тактів PS1 і росте там, де на екрані плавності не додається, тобто
	// міряє не те, що видно. ms лишається часом на кадр емуляції - це ціна кадру,
	// і саме з нею зіставні заміри в лозі. Ділення тут два на пів секунди.
	if (g_fpsOverlay && g_pass.showScreen)
		{
		const TUint now = User::NTickCount();
		const TUint dt = now - g_fpsWinTick;
		const TInt  df = g_pass.i - g_fpsWinFrame;
		const TUint shown = voutSymbianPresents();
		if (df > 0 && dt >= (TUint)KFpsWindowMs)
			{
			const TUint ds = shown - g_fpsWinShown;
			voutSymbianSetStats((TInt)((ds * 10000 + dt / 2) / dt),
			                    (TInt)((dt * 10 + (TUint)df / 2) / (TUint)df));
			g_fpsWinTick = now;
			g_fpsWinFrame = g_pass.i;
			g_fpsWinShown = shown;
			}
		}

	// Рідкісні маркери: 6 записів на прогін вимір не псують, зате показують,
	// доки дійшло, коли процес гине без жодного повідомлення. У GUI прохід іде
	// годинами, тож крок більший: кожен запис коштує fflush на флеш.
	const TInt every = g_guiMode ? 1000 : KProgressEvery;
	if (g_pass.i % every == 0)
		{
		logEvent("progress", NULL, "%s: frame %d/%d ms=%u stackUnused=%d", mode,
			g_pass.i, g_pass.frames,
			(unsigned)(User::NTickCount() - g_pass.tStart), (int)stackHighWater());
		}

	// Z2+Z3: скільки кадрів гра справді оновила і в якій геометрії. updates - це
	// лічильник викликів vout_update; різниця між фазами меню (0-4000) і траси
	// показує, чи виграш від показу за латчем не є артефактом стенду.
	// Знімки екрана в характерних точках: меню гри (2D) і траса (3D). Порівняння
	// цих двох файлів відповідає на питання, чому 2D не видно, а відео видно.
	if (g_pass.showScreen && (g_pass.i == g_dumpAt || g_pass.i == 6000))
		{
		char p[32];
		snprintf(p, sizeof(p), "E:\\PS1\\logs\\ps1_scr%d.bin", (int)g_pass.i);
		const int sr = voutSymbianScreenDump(p);
		// Адресу пишемо ДО читання: якщо вона фізична, читання вб'є потік.
		logEvent("screen_dump", sr ? "SCR_FAIL" : NULL, "frame=%d rc=%d halAddr=0x%x",
			g_pass.i, sr, (unsigned)voutSymbianHalAddr());
		// Той самий кадр із буфера за HAL-адресою: чи доїхав він до сканованого.
		snprintf(p, sizeof(p), "E:\\PS1\\logs\\ps1_hal%d.bin", (int)g_pass.i);
		const int hr = voutSymbianHalDump(p);
		logEvent("hal_dump", hr ? "HAL_FAIL" : NULL, "frame=%d rc=%d", g_pass.i, hr);
		// Візерунок відомо робочим шляхом + сирий дамп: розкладку буфера екрана
		// (байтів на піксель, крок) визначаємо потім на хості.
		if (g_pass.i == g_dumpAt)
			{
			int m = 0, t = 0, bad[4];
			const int rb = voutSymbianReadBack(0, &m, &t, bad);
			const int rr = voutSymbianRawDump("E:\\PS1\\logs\\ps1_raw.bin", 0);
			logEvent("fb_pattern", NULL, "rb=%d match=%d/%d bad=%d,%d want=0x%x got=0x%x raw=%d",
				rb, m, t, bad[0], bad[1], (unsigned)bad[2], (unsigned)bad[3], rr);
			if (g_fbVis)
				{
				// Спершу зелений хрест через bitgdi, потім червоний сирим записом,
				// по 5 с кожен. Що з них видно на панелі - те й відповідь.
				const int v0 = voutSymbianVisTest(0);
				User::After(5000000);
				const int v1 = voutSymbianVisTest(1);
				User::After(5000000);
				logEvent("fb_vis", NULL, "bitgdi=%d raw=%d", v0, v1);
				}
			}
		}

	// Той самий рахунок, що й для сторожа: кожен logEvent - це fflush на картку.
	// Сто кадрів - це раз на три секунди гри, і саме на цьому кадрі йде затримка.
	if (g_pass.showScreen && (g_pass.i % (g_guiMode ? 3000 : 100)) == 0)
		{
		int srcW = 0, srcH = 0, dstW = 0, dstH = 0, ox = 0, oy = 0;
		voutSymbianGeom(&srcW, &srcH, &dstW, &dstH, &ox, &oy);
		int fbd = 0, fbda = 0, st = 0, sx = 0, sy = 0, sw = 0, sh = 0, voff = 0, stale = 0;
		voutSymbianDbgState(&fbd, &fbda, &st, &sx, &sy, &sw, &sh, &voff, &stale);
		// gp0/dma - лічильники команд малювання: якщо в застиглому вікні вони
		// РОСТУТЬ, гра малює, а ми не показуємо (вада). Якщо стоять - гра справді
		// нічого не робить, і завмирання коректне.
		// fbPages: скільки РІЗНИХ адрес першого пікселя віддав драйвер. >1 означає
		// подвійну буферизацію, тобто запис у кешовану адресу лягає повз панель.
		int fbChg = 0, fbSeen = 0, fbB0 = 0, fbB1 = 0;
		voutSymbianFbPages(&fbChg, &fbSeen, &fbB0, &fbB1);
		logEvent("vout_geom", NULL,
			"frame=%d upd=%d gpulibUpd=%d src=%dx%d dst=%dx%d off=%d,%d "
			"gp0=%u dma=%u fbDirty=%d fbDirtyArea=%d status=0x%x scr=%dx%d src=%d,%d "
			"voutOff=%d stale=%d fbPages=%d/%d fb0=0x%x fb1=0x%x",
			g_pass.i, voutSymbianUpdates(), voutSymbianGpulibUpdates(),
			srcW, srcH, dstW, dstH, ox, oy,
			g_gpulibWriteData, g_gpulibDmaChain, fbd, fbda, (unsigned)st,
			sw, sh, sx, sy, voff, stale, fbChg, fbSeen, (unsigned)fbB0, (unsigned)fbB1);
		}
	return ETrue;
	}

// Підсумки проходу, звільнення ядра. Дзеркало passSetup.
static void passTeardown()
	{
	const char *mode = g_pass.mode;
	const TBool aShowScreen = g_pass.showScreen;
	g_pass.ticks = User::NTickCount() - g_pass.tStart;
	g_pass.produced = frame_counter - g_pass.framesAtStart;
	TUint &tPoll = g_pass.tPoll, &tEmu = g_pass.tEmu, &tBlit = g_pass.tBlit, &tSnd = g_pass.tSnd;
	const TUint aTicks = g_pass.ticks;
	const u32 aProduced = g_pass.produced;
	const TBool stalled = g_pass.stalled;

	inputTraceClose();

	// Роззброїти сторож: просимо вийти й чекаємо, поки він завершиться сам.
	g_wdRun = 0;
	if (g_pass.haveWd) { User::WaitForRequest(g_wdStat); g_wdThread.Close(); g_pass.haveWd = EFalse; }

	// Траєкторія pc/cycle зі сторожа: показує, де саме застрягло виконання.
	for (TInt s = 0; s < g_wdSamples; s++)
		logEvent("wdsample", NULL, "%s: t=%ds pc=%08x cycle=%u", mode, (int)(s * 2),
			(unsigned)g_wdPc[s], (unsigned)g_wdCyc[s]);

	flushCoreRepeat();
	logEvent("pass", stalled ? "STALLED" : (aProduced ? NULL : "NO_FRAMES"),
		"%s: frames=%u ms=%u pc=%08x cycle=%u", mode,
		(unsigned)aProduced, (unsigned)aTicks,
		(unsigned)psxRegs.pc, (unsigned)psxRegs.cycle);

	// Скільки GPU-команд BIOS реально надіслав: локалізує чорний екран - доставка
	// команд (усе 0) проти растеризації (ненульові, а VRAM порожній).
	unsigned gpData = 0, gpDataMem = 0, gpDma = 0, gpStatus = 0;
	voutSymbianGpuStats(&gpData, &gpDataMem, &gpDma, &gpStatus);
	logEvent("gpustats", NULL, "%s: gp0=%u dataMem=%u dmaChain=%u gp1=%u", mode,
		gpData, gpDataMem, gpDma, gpStatus);

	// Ground truth з gpulib: якщо тут теж нулі - BIOS не малює (застряг раніше);
	// якщо ненульові, а VRAM порожній - проблема в растеризації gpu_unai.
	logEvent("gpulibstats", NULL,
		"%s: wData=%u wDataMem=%u dmaChain=%u dmaWords=%u wStatus=%u rStatus=%u statusLast=%08x",
		mode, g_gpulibWriteData, g_gpulibWriteDataMem, g_gpulibDmaChain,
		g_gpulibDmaWords, g_gpulibWriteStatus, g_gpulibReadStatus, g_gpulibStatusLast);

	// Звук (Фаза A): скільки семплів SPU видав і пікова амплітуда. maxAmp>0 =
	// SPU реально генерує звук BIOS; вивід у динамік - Фаза B.
	unsigned sndFeeds = 0, sndSamples = 0, sndUnder = 0; int sndMax = 0, sndStep = 0;
	ps1SndStats(&sndFeeds, &sndSamples, &sndMax, &sndUnder, &sndStep);
	// under - число розривів у динаміку, step - у скільки разів потік розтягнуто
	// під фактичну швидкість емуляції (65536 = 1.0, менше = емуляція відстає).
	logEvent("sndstats", NULL, "%s: feeds=%u samples=%u maxAmp=%d under=%u step=%d", mode,
		sndFeeds, sndSamples, sndMax, sndUnder, sndStep);

	// Профіль пайплайну: сумарний час на етап + частка від загального. Показує,
	// що оптимізувати - емуляцію (drc) чи вивід (блит)/звук.
	{
	const TUint tTot = tPoll + tEmu + tBlit + tSnd;
	const TUint den = tTot ? tTot : 1;
	// gpuRaster - частина emu, витрачена на растеризацію gpu_unai (тики->мс). Решта
	// emu - це CPU (drc) + генерація звуку. Показує, CPU чи GPU оптимізувати далі.
	const unsigned freq = ndrc_symbian_fastfreq();
	const unsigned gpuRasterMs = (unsigned)(g_gpulibRenderTicks * 1000ULL / (freq ? freq : 1));
	const unsigned spuMixMs = (unsigned)(g_spuMixTicks * 1000ULL / (freq ? freq : 1));
	const unsigned cdReadMs = (unsigned)(g_cdReadTicks * 1000ULL / (freq ? freq : 1));
	const unsigned mdecMs = (unsigned)(g_mdecDecodeTicks * 1000ULL / (freq ? freq : 1));
	unsigned blitFillMs = 0, blitPresentMs = 0;
	voutSymbianBlitSplit(&blitFillMs, &blitPresentMs);
	logEvent("profile", NULL,
		"%s: total=%ums poll=%ums(%u%%) emu=%ums(%u%%) blit=%ums(%u%%) snd=%ums(%u%%) gpuRaster=%ums(%u%% of emu) spuMix=%ums mdec=%ums(%u words) cdRead=%ums(%u reads) blitFill=%ums blitPresent=%ums frames=%u",
		mode, (unsigned)tTot,
		(unsigned)tPoll, (unsigned)(tPoll * 100 / den),
		(unsigned)tEmu,  (unsigned)(tEmu * 100 / den),
		(unsigned)tBlit, (unsigned)(tBlit * 100 / den),
		(unsigned)tSnd,  (unsigned)(tSnd * 100 / den),
		gpuRasterMs, (unsigned)(tEmu ? gpuRasterMs * 100 / tEmu : 0),
		spuMixMs, mdecMs, g_mdecWords, cdReadMs, g_cdReadCount, blitFillMs, blitPresentMs,
		(unsigned)aProduced);
	logEvent("dsa", NULL, "%s: active=%d gl=%d fbPlatform=%d", mode,
		voutSymbianDsaActive(), voutSymbianGlActive(), voutSymbianFbFromPlatform());
	}

	// Тримаємо останній кадр (докручуючи аудіо), тоді прибираємо вікно й звук.
	// У GUI тримати нема сенсу - користувач сам вирішив вийти, і вікно однаково
	// належить контейнеру (voutSymbianFinish там лише відв'язується).
	if (aShowScreen)
		{
		const int holdMs = g_guiMode ? 0 : KScreenHoldMs;
		ps1SndPump();
		voutSymbianFinish(holdMs);
		ps1SndCloseOutput();
		logEvent("screen", NULL, "%s: presented %u frames, held %dms", mode,
			(unsigned)aProduced, holdMs);
		}

	// Дзеркало passSetup у зворотному порядку. Ядро цього не робить: у фронтенді
	// плагіни й привід закриває ClosePlugins, який ми обходимо (нема .so).
	// Ключове тут - cdra_close: ISOopen першим рядком повертає 0, якщо образ уже
	// відкрито ("it's already open"), тож без закриття наступний cdra_open мовчки
	// НЕ відкриває новий диск. Гра після зміни диска діставала попередній образ із
	// чужою таблицею доріжок - звідси й зависання на логотипі.
	SPU_close();
	ps1GpuClose();
	cdra_close();

	EmuShutdown();
	}

// Консольна гілка: три частини назад у суцільний прохід.
static TInt runPass(TBool aUseDrc, TInt aFrames, TBool aUseBios, TBool aGpuTest,
	TBool aShowScreen, TBool aUseDisc, TUint &aTicks, u32 &aProduced)
	{
	aTicks = 0;
	aProduced = 0;
	const TInt rc = passSetup(aUseDrc, aFrames, aUseBios, aGpuTest, aShowScreen, aUseDisc);
	if (rc != KErrNone)
		return rc;
	while (passStep())
		{ }
	passTeardown();
	aTicks = g_pass.ticks;
	aProduced = g_pass.produced;
	return aProduced ? KErrNone : KErrGeneral;
	}

// Апаратне виключення (KERN-EXEC 3 і подібні) знімає процес миттєво: підсумковий
// блок не пишеться, і в лозі лишається сам лише обрив. Обробник встигає записати,
// ЩО впало - стадія кадру, його номер, гостьовий pc і лічильники GPU. Після
// повернення звідси помилка виникла б знову, тож виходимо самі.
static void crashHandler(TExcType aType)
	{
	// Тип - найперше й одним записом: якщо далі обробник загине, сторож усе одно
	// побачить, що виключення взагалі доставлено.
	g_crashType = (TInt)aType;
	g_crashStep = 1;

	// Знімок стека вгору від власного кадру: саме там ядро лишило контекст
	// збійного коду, включно з адресою збійної інструкції.
	TUint8 here;
	TUint32 sp = ((TUint32)&here) & ~3u;
	TUint32 top = sp + KCrashWords * 4;
	if (g_stackBase && top > g_stackBase)
		top = g_stackBase;
	TInt n = (top > sp) ? (TInt)((top - sp) / 4) : 0;
	if (n > KCrashWords) n = KCrashWords;
	const TUint32 *p = (const TUint32 *)sp;
	for (TInt i = 0; i < n; ++i)
		g_crashStack[i] = p[i];
	g_crashSp = sp;
	g_crashWords = n;
	g_crashStep = 2;

	// Публікує сторож (окремий потік, RFile): з обробника stdio мовчить.
	// Спін, а не User::After - блокувальний виклик звідси не повертається.
	for (volatile TInt w = 0; w < 40000000 && !g_crashPublished; ++w)
		{ }
	g_crashStep = 3;

	unsigned gpData = 0, gpDataMem = 0, gpDma = 0, gpStatus = 0;
	voutSymbianGpuStats(&gpData, &gpDataMem, &gpDma, &gpStatus);
	logEvent("crash", "EXCEPTION",
		"type=%d stage=%d frame=%d pc=%08x cycle=%u gp0=%u dataMem=%u dma=%u gp1=%u",
		(int)aType, (int)g_stage, (int)g_pass.i,
		(unsigned)psxRegs.pc, (unsigned)psxRegs.cycle,
		gpData, gpDataMem, gpDma, gpStatus);
	if (g_log)
		{ fclose(g_log); g_log = NULL; }
	User::Exit(KErrAbort);
	}

// Раз на процес, спільне для обох гілок.
static void coreInitOnce()
	{
	// Повна маска: минулого разу обробник не спрацював, і звужена маска - одна з
	// двох причин (друга - переповнення стека, коли викликати його нема на чому).
	const TInt excRc = User::SetExceptionHandler(crashHandler,
		KExceptionAbort | KExceptionKill | KExceptionUserInterrupt | KExceptionFpe |
		KExceptionFault | KExceptionInteger | KExceptionDebug);
	logEvent("exc_handler", excRc ? "EXC_INSTALL_FAIL" : NULL, "rc=%d", (int)excRc);
	paintStack();

	bindStubPlugins();

	// Конфіг растеризатора - обов'язково ДО GPU_init: renderer_init копіює його
	// у робочу структуру один раз. GPUrearmedCallbacks (нижче) викликаємо з
	// нульовою структурою, тож без цього рядка конфіг лишається нульовим, і
	// вимкненими виявляються не лише прискорювачі, а й освітлення з напівпрозорістю.
	gpu_unai_set_config(g_hackPixelSkip, g_hackIlace, g_gpuLighting,
		g_gpuLighting /*fast*/, g_gpuBlending);
	// Вивід мусить знати про пропуск рядків: інакше він показує непарні рядки VRAM,
	// яких растеризатор не переписував, і статичні накладки лишають слід на екрані.
	voutSymbianSetIlaceOut(g_hackIlace ? 1 : 0);
	logEvent("gpu_cfg", NULL, "pixskip=%d ilace=%d light=%d blend=%d",
		(int)g_hackPixelSkip, (int)g_hackIlace, (int)g_gpuLighting, (int)g_gpuBlending);

	// GPU_init() раз на процес: виділяє VRAM і виконує do_reset() у gpulib
	// (gpu.status = 0x14802000, біт 28 = готовність до DMA). Фронтенд робить це в
	// LoadPlugins() (plugins.c:328); ми обходимо LoadPlugins (нема .so), тож
	// викликаємо самі. Обидва проходи (interp/drc) поділяють один VRAM.
	const long gpuInitRc = ps1GpuInit();
	logEvent("gpu_init", gpuInitRc ? "GPU_INIT_FAIL" : NULL, "init=%ld", gpuInitRc);

	// Колбеки - суворо ПІСЛЯ GPU_init: GPUinit робить memset на gpu.state і
	// gpu.frameskip (gpu.c:355-359) і повертає лічильник кадрів на власний нуль.
	// Стояло це вище й тихо скасовувалось: gpulib увесь час бачив frame_count=0,
	// а frameskip.set=0 не давав пропуску растеризації спрацювати взагалі.
	ps1GpuSetCallbacks(&frame_counter, &g_gpuHcnt, g_gpuFrameskip);
	// І одразу назад конфіг растеризатора: GPUrearmedCallbacks усередині кличе
	// renderer_set_config (gpu.c:1288), а той безумовно перезаписує lighting,
	// blending та ilace_force полями cbs->gpu_unai, яких ми не заповнюємо. Відколи
	// колбеки переїхали ПІСЛЯ GPU_init, це мовчки вимикало освітлення й
	// напівпрозорість незалежно від меню.
	gpu_unai_apply_config();
	logEvent("gpu_cbs", NULL, "frameskip=%d", (int)g_gpuFrameskip);

	// Звук: конфіг dfsound до SPU_init, тоді ініціалізація SPU (раз на процес).
	ps1SndConfigure();
	const long spuInitRc = SPU_init();
	logEvent("spu_init", spuInitRc ? "SPU_INIT_FAIL" : NULL, "init=%ld", spuInitRc);
	}

// --- Гілка GUI (оболонка AVKON, app_symbian.cpp) ----------------------------
// Ті самі кроки, що й консольний прохід, але під активний об'єкт: Setup один раз,
// Step на кожен тік, Shutdown при виході. Прохід один - гра з диска.

static TBool g_guiUp;
// Аргумент "interp" з деплою: той самий сеанс, але на інтерпретаторі. Розділяє
// найбільшу розвилку причин падіння - код, згенерований динареком, проти всього
// іншого (растеризація gpu_unai, ядро, наш вивід).
static TBool g_guiInterp;

extern "C" void ps1GuiUseInterpreter(void)
	{
	g_guiInterp = ETrue;
	}

extern "C" int ps1GuiSetup(void)
	{
	g_guiMode = ETrue;
	g_guiExit = 0;
	g_watchdogMs = KPlayWatchdogMs;
	ps1SndSetPumpAo(0);   // планувальник крутить CIdle між кадрами
	voutSymbianSetPumpAo(0);
	coreInitOnce();
	const TInt frames = g_playFrames > 0 ? g_playFrames : KPlayFrames;
	logEvent("start", NULL, "stage1 begin (gui), frames=%d cpu=%s input=%d", (int)frames,
		g_guiInterp ? "interp" : "drc", (int)g_inputMode);
	const TInt rc = passSetup(!g_guiInterp, frames, ETrue /*bios*/, EFalse /*gputest*/,
		ETrue /*screen*/, ETrue /*disc*/);
	// Після passSetup: слід прив'язаний до номера кадру, а лічильник кадрів
	// починається саме тут.
	inputTraceOpen();
	g_guiUp = (rc == KErrNone);
	return rc;
	}

extern "C" int ps1GuiStep(void)
	{
	if (!g_guiUp)
		return 0;
	return passStep() ? 1 : 0;
	}

// Перезапуск проходу в тому ж процесі: інший диск (або інша гілка процесора,
// BIOS, звук, GL, DSA) вимагає повторної ініціалізації ядра. Саме так працює
// консольна гілка - два проходи підряд, - тож пара teardown/setup перевірена.
// VRAM, кеш трансляцій і SPU лишаються від coreInitOnce: вони на процес.
//
// Два окремі виклики, а не один: між ними оболонка мусить наново прив'язати
// вивід. passTeardown відв'язує вікно (voutSymbianFinish), тож прив'язка ДО
// нього пропадала - гра рахувала кадри, а на екрані лишався останній кадр
// попередньої. Кликати лише з тіку CIdle, не з обробника клавіші: усередині є
// і звук, і DSA.
extern "C" void ps1GuiEndPass(void)
	{
	if (!g_guiUp)
		return;
	g_guiUp = EFalse;
	passTeardown();
	}

extern "C" int ps1GuiStartPass(void)
	{
	g_guiExit = 0;
	const TInt frames = g_playFrames > 0 ? g_playFrames : KPlayFrames;
	const TInt rc = passSetup(!g_guiInterp, frames, ETrue /*bios*/, EFalse /*gputest*/,
		ETrue /*screen*/, ETrue /*disc*/);
	inputTraceOpen();
	g_guiUp = (rc == KErrNone);
	logEvent("game", rc ? "RESTART_FAIL" : NULL, "restart rc=%d disc=%s", (int)rc,
		g_discPath[0] ? g_discPath : KDiscImg);
	return rc;
	}

// Закриття гри посеред сеансу: образ, який зараз читає ядро, збираються стиснути,
// а конвертер має його і прочитати, і видалити. Прохід згортаємо повністю - назад
// його вже не підняти (VRAM і кеш трансляцій одні на процес), тож оболонка після
// цього лишає в меню тільки вихід.
extern "C" void ps1GuiCloseGame(void)
	{
	if (!g_guiUp)
		return;
	g_guiUp = EFalse;
	g_guiExit = 1;
	passTeardown();
	logEvent("game", NULL, "closed: disc goes to import");
	}

extern "C" void ps1GuiShutdown(void)
	{
	if (!g_guiUp)
		return;
	g_guiUp = EFalse;
	passTeardown();

	// Знімок VRAM для перевірки растеризації на хості (Етап 2).
	const int dumpRc = voutSymbianDump("E:\\PS1\\logs\\ps1_vram.bin");
	logEvent("vram_dump", dumpRc ? "DUMP_FAIL" : NULL, "rc=%d", dumpRc);

	ps1GpuShutdown();   // парний до ps1GpuInit: звільняє VRAM після знімка
	logEvent("shutdown", NULL, "gui: frames=%u ms=%u",
		(unsigned)g_pass.produced, (unsigned)g_pass.ticks);
	}

// Оболонка просить завершити прохід (користувач вийшов).
extern "C" void ps1GuiRequestExit(void)
	{
	g_guiExit = 1;
	}

// Один потік JSONL на процес: оболонка пише через нього, а не окремим файлом.
extern "C" void ps1Log(const char *aType, const char *aMsg)
	{
	logEvent(aType, NULL, "%s", aMsg ? aMsg : "");
	}

extern "C" void ps1LogInt(const char *aType, const char *aMsg, int aVal)
	{
	logEvent(aType, NULL, "%s=%d", aMsg ? aMsg : "", aVal);
	}

// Рівень 1..5 з меню -> поріг контрасту. Менший поріг захоплює більше пікселів:
// заміряно на трасі 10 -> 26% кадру, 6 -> 33%, 2 -> 60%. Верхньої межі НЕ ставимо:
// з нею фільтр пропускав саме найконтрастніші межі, тобто ті, які єдині й помітні
// на екрані - перевірено на апараті, ефекту не було видно.
static void applyFxaaLevel(TInt aLevel)
	{
	static const TInt KFxaaThr[6] = { 0, 10, 8, 6, 4, 2 };
	voutSymbianSetFxaa(KFxaaThr[(aLevel >= 0 && aLevel <= 5) ? aLevel : 0], 31);
	}

// Налаштування з меню. Викликається ДО ps1GuiSetup, тобто до будь-якої
// ініціалізації ядра й растеризатора - інакше частина з них не подіє.
extern "C" void ps1CfgApply(const char *aDisc, const char *aBios, int aInterp,
	int aIlace, int aLight, int aBlend, int aPixSkip, int aGteNf,
	int aSound, int aGl, int aDirty, int aDsa, int aFps, int aGpuSkip, int aCycle,
	int aSmooth, int aFxaa)
	{
	g_discPath[0] = 0;
	if (aDisc && aDisc[0])
		{
		TInt i = 0;
		while (aDisc[i] && i < (TInt)sizeof(g_discPath) - 1)
			{ g_discPath[i] = aDisc[i]; i++; }
		g_discPath[i] = 0;
		}
	g_biosFile[0] = 0;
	if (aBios && aBios[0])
		{
		TInt i = 0;
		while (aBios[i] && i < (TInt)sizeof(g_biosFile) - 1)
			{ g_biosFile[i] = aBios[i]; i++; }
		g_biosFile[i] = 0;
		}
	g_guiInterp      = aInterp ? ETrue : EFalse;
	g_hackIlace      = aIlace;
	g_gpuLighting    = aLight;
	g_gpuBlending    = aBlend;
	g_hackPixelSkip  = aPixSkip;
	g_hackGteNoFlags = aGteNf ? ETrue : EFalse;
	g_noSound        = aSound ? EFalse : ETrue;
	voutSymbianSetGl(aGl);
	voutSymbianSetDirtyOnly(aDirty);
	// DSA піднімається у voutSymbianAttachWindow, а меню віддає конфіг ПІСЛЯ нього,
	// тож саму прив'язку треба переробити - інакше прапорець подіє лише з наступного
	// запуску. Це робить ps1GuiReattach() у першому тіку.
	voutSymbianSetDsa(aDsa);
	g_fpsOverlay = aFps ? ETrue : EFalse;
	voutSymbianSetOverlay(aFps);
	g_gpuFrameskip = aGpuSkip > 0 ? aGpuSkip : 0;
	if (aCycle > 0)
		g_cycleMul = aCycle;
	voutSymbianSetSmooth(aSmooth);
	applyFxaaLevel(aFxaa);
	logEvent("cfg_bios", NULL, "bios=%s", g_biosFile[0] ? g_biosFile : KBiosFile);
	logEvent("cfg", NULL, "disc=%s interp=%d ilace=%d light=%d blend=%d "
		"pixskip=%d gtenf=%d snd=%d gl=%d dirty=%d dsa=%d fps=%d gpuskip=%d cyclemul=%d "
		"smooth=%d fxaa=%d",
		g_discPath[0] ? g_discPath : KDiscImg, (int)aInterp,
		(int)aIlace, (int)aLight, (int)aBlend, (int)aPixSkip, (int)aGteNf,
		(int)aSound, (int)aGl, (int)aDirty, (int)aDsa, (int)aFps, (int)aGpuSkip,
		(int)aCycle, (int)aSmooth, (int)aFxaa);
	}

// Те саме, але з меню, відкритого вже в грі. Сюди потрапляє лише те, що можна
// перемкнути між кадрами; вибір диска, гілка процесора, GL, DSA, звук і хаки GTE
// потребують повторної ініціалізації й лишаються за ps1CfgApply. Викликати можна
// тільки з паузи (тік CIdle не в кадрі): нижче є скидання кеша трансляцій.
extern "C" void ps1CfgApplyLive(int aIlace, int aLight, int aBlend, int aPixSkip,
	int aFps, int aGpuSkip, int aCycle, int aSmooth, int aFxaa, int aDirty)
	{
	g_hackIlace     = aIlace;
	g_gpuLighting   = aLight;
	g_gpuBlending   = aBlend;
	g_hackPixelSkip = aPixSkip;
	gpu_unai_set_config(g_hackPixelSkip, g_hackIlace, g_gpuLighting,
		g_gpuLighting /*fast*/, g_gpuBlending);
	gpu_unai_apply_config();
	voutSymbianSetIlaceOut(g_hackIlace ? 1 : 0);

	g_fpsOverlay = aFps ? ETrue : EFalse;
	voutSymbianSetOverlay(aFps);
	voutSymbianSetSmooth(aSmooth);
	applyFxaaLevel(aFxaa);
	voutSymbianSetDirtyOnly(aDirty);

	if (aGpuSkip != g_gpuFrameskip)
		{
		g_gpuFrameskip = aGpuSkip > 0 ? aGpuSkip : 0;
		// Єдиний шлях до gpu.frameskip.set - повторні колбеки, а вони знову
		// перезаписують конфіг растеризатора (див. коментар у coreInitOnce).
		ps1GpuSetCallbacks(&frame_counter, &g_gpuHcnt, g_gpuFrameskip);
		gpu_unai_apply_config();
		}

	if (aCycle > 0 && aCycle != g_cycleMul)
		{
		g_cycleMul = aCycle;
		Config.cycle_multiplier = g_cycleMul;
		// ApplyConfig сам скидає кеш трансляцій, коли множник змінився: блоки,
		// зібрані під старе значення, інакше лишились би з чужим таймінгом.
		if (psxCpu && psxCpu->ApplyConfig)
			psxCpu->ApplyConfig();
		}

	logEvent("cfg_live", NULL, "ilace=%d light=%d blend=%d pixskip=%d fps=%d "
		"gpuskip=%d cyclemul=%d smooth=%d fxaa=%d dirty=%d",
		(int)aIlace, (int)aLight, (int)aBlend, (int)aPixSkip, (int)aFps,
		(int)aGpuSkip, (int)aCycle, (int)aSmooth, (int)aFxaa, (int)aDirty);
	}

extern "C" void ps1LogFmt(const char *aType, const char *aFmt, ...)
	{
	char buf[256];
	va_list ap;
	va_start(ap, aFmt);
	vsnprintf(buf, sizeof(buf), aFmt, ap);
	va_end(ap);
	logEvent(aType, NULL, "%s", buf);
	}

static TInt runStage1()
	{
	TInt framesToRun;
	TBool useBios, gpuTest, useDrc, showScreen, useDisc, haveArgs;
	parseCommandLine(framesToRun, useBios, gpuTest, useDrc, showScreen, useDisc, haveArgs);

	// Діагностика direct-FB: аргумент "fbtest" малює тест-патерн прямо в LCD (без
	// емуляції) і тримає 8с. За побаченим визначаємо stride/буфер/орієнтацію.
	TBuf<256> cl; User::CommandLine(cl); cl.Trim();
	_LIT(KFbTest, "fbtest");
	if (cl.FindF(KFbTest) != KErrNotFound)
		{
		voutSymbianSetDsa(1);
		const int ir = voutSymbianInit();
		int fbOk = 0, xp = 0, yp = 0, lb = 0, bpp = 0, fo = 0, ww = 0, wh = 0;
		voutSymbianFbInfo(&fbOk, &xp, &yp, &lb, &bpp, &fo, &ww, &wh);
		logEvent("fb", NULL, "init=%d ok=%d hal=%dx%d line=%dB bpp=%d first=%d win=%dx%d",
			ir, fbOk, xp, yp, lb, bpp, fo, ww, wh);

		// Z1: розкладка ВІД ПЛАТФОРМИ поряд із HAL-значеннями. Якщо stride або база
		// різняться - це і є пряме пояснення перекошеного кадру 2026-08-01.
		int bits = 0, stride = 0, sw = 0, sh = 0, mode = -1, rot = -1, halBase = 0;
		voutSymbianScreenLayout(&bits, &stride, &sw, &sh, &mode, &rot, &halBase);
		logEvent("screen_layout", NULL,
			"bits=0x%x halBase=0x%x stride=%dB size=%dx%d mode=%d rotation=%d "
			"halLine=%dB fromPlatform=%d",
			bits, halBase, stride, sw, sh, mode, rot, lb,
			voutSymbianFbFromPlatform());

		// Z1/M-RB: візерунок відомо робочим шляхом, читання назад сирим покажчиком.
		int m = 0, t = 0, badA[4];
		const int rb0 = voutSymbianReadBack(0, &m, &t, badA);
		logEvent("readback", rb0 ? "RB_FAIL" : NULL,
			"pass=first rc=%d match=%d/%d firstBad=(%d,%d) want=0x%x got=0x%x",
			rb0, m, t, badA[0], badA[1], badA[2], badA[3]);

		// Повтор без нового малювання: якщо збіги зникли - подвійна буферизація.
		User::After(100000);
		int m2 = 0, t2 = 0, badB[4];
		const int rb1 = voutSymbianReadBack(1, &m2, &t2, badB);
		logEvent("readback", rb1 ? "RB_FAIL" : NULL,
			"pass=repeat rc=%d match=%d/%d firstBad=(%d,%d) want=0x%x got=0x%x",
			rb1, m2, t2, badB[0], badB[1], badB[2], badB[3]);

		const int tr = voutSymbianFbTest();
		logEvent("fbtest", tr ? "NO_FB" : NULL, "rc=%d - pattern held 8s", tr);
		voutSymbianFinish(8000);
		return KErrNone;
		}

	coreInitOnce();

	// Без аргументів = запуск з меню апарата (alpha 1.0): граємо GT з диска -
	// великий бюджет кадрів і довший сторож, щоб не обірвати гру. З аргументами
	// (деплой/бенчмарк) поведінка стара.
	if (!haveArgs)
		{
		framesToRun = KPlayFrames;
		g_watchdogMs = KPlayWatchdogMs;
		}

	logEvent("start", NULL, "stage1 begin, frames=%d bios=%d gputest=%d args=%d",
		(int)framesToRun, (int)useBios, (int)gpuTest, (int)haveArgs);

	TInt rc;
	TUint interpTicks = 0, drcTicks = 0;
	u32 interpFrames = 0, drcFrames = 0;

	if (haveArgs)
		{
		rc = runPass(useDrc, framesToRun, useBios, gpuTest, showScreen, useDisc,
			useDrc ? drcTicks : interpTicks, useDrc ? drcFrames : interpFrames);
		}
	else
		{
		// alpha 1.0: одразу граємо GT - диск+екран+drc+звук. Без бенчмарку двох
		// режимів (для замірів запускати з аргументами через деплой).
		rc = runPass(ETrue /*drc*/, framesToRun, ETrue /*bios*/, EFalse /*gputest*/,
			ETrue /*screen*/, ETrue /*disc*/, drcTicks, drcFrames);
		(void)interpTicks; (void)interpFrames;
		}

	// Знімок VRAM для перевірки растеризації на хості (Етап 2). Вивід на екран
	// тепер живий, усередині runPass (кадр за кадром), а не одноразово тут.
	const int dumpRc = voutSymbianDump("E:\\PS1\\logs\\ps1_vram.bin");
	logEvent("vram_dump", dumpRc ? "DUMP_FAIL" : NULL, "rc=%d", dumpRc);

	// Парний до ps1GpuInit(): звільняє VRAM після знімка (звідси - після дампу).
	ps1GpuShutdown();

	logEvent("shutdown", NULL, NULL);
	return rc;
	}

// Оболонка AVKON (app_symbian.cpp). Не повертається, доки застосунок не закрито.
extern "C" int ps1AppRun(void);

// Зонд вартості читання сектора. Кадровий прогін для порівняння форматів образу
// не годиться: cdRead там ~5 с із 175, і денний розкид самого апарата (той самий
// .bin давав 3950 і 5504 мс) більший за різницю між форматами. Тут емуляції немає
// зовсім, тож міряється рівно читання плюс розпакування.
// Крок 1 - послідовно (кеш блока влучає), великий крок - кожне читання в новий
// блок, тобто гірша межа.
static void cdBenchPass(const char *aPath, TInt aCount, TInt aStride)
	{
	g_cdReadTicks = 0;
	g_cdReadCount = 0;
	TInt ok = 0, fail = 0;
	const unsigned int t0 = ndrc_symbian_fastcount();
	for (TInt i = 0; i < aCount; i++)
		{
		const TInt s = i * aStride + 150;
		unsigned char msf[3];
		msf[0] = (unsigned char)(s / (60 * 75));
		msf[1] = (unsigned char)((s / 75) % 60);
		msf[2] = (unsigned char)(s % 75);
		if (cdra_readTrack(msf) < 0)
			fail++;
		else
			ok++;
		}
	const unsigned int dt = ndrc_symbian_fastcount() - t0;
	const unsigned freq = ndrc_symbian_fastfreq();
	const unsigned d = freq ? freq : 1;

	logEvent("cdbench", fail ? "READ_FAIL" : NULL,
		"%s stride=%d: ok=%d fail=%d wall=%ums us/read=%u inner=%ums(%u reads)",
		aPath, (int)aStride, ok, fail,
		(unsigned)((TUint64)dt * 1000 / d),
		(unsigned)(ok ? (TUint64)dt * 1000000 / d / ok : 0),
		(unsigned)(g_cdReadTicks * 1000ULL / d), g_cdReadCount);
	}

// Відкриваємо образ один раз на запуск: повторний cdra_init/cdra_open на вже
// відкритому образі ISOopen відхиляє ("it's already open").
static void cdBench(const char *aPath)
	{
	SetIsoFile(aPath);
	if (cdra_init() != 0)
		{ logEvent("cdbench", "INIT_FAIL", "%s", aPath); return; }
	if (cdra_open() != 0)
		{ logEvent("cdbench", "OPEN_FAIL", "%s", aPath); return; }
	cdBenchPass(aPath, 20000, 1);
	cdBenchPass(aPath, 2000, 37);
	cdra_close();
	}

GLDEF_C TInt E32Main()
	{
	g_startTick = User::NTickCount();
	ps1EnsureDirs();
	openLog();

	TBuf<256> cl;
	User::CommandLine(cl);
	cl.Trim();
	TBuf8<256> cl8;
	cl8.Copy(cl);

	// Без аргументів = запуск із меню апарата: повноцінний застосунок AVKON. Він
	// потрібен не лише заради виходу й таск-листа - MBX-EGL створює поверхню лише
	// на вікні під CCoeEnv. Бенчмарк лишається консольним (аргументи з деплою), бо
	// там ні вікна, ні фреймворку не треба, а заміри мають лишитись порівнюваними.
	_LIT(KGuiArg, "gui");
	_LIT(KGlArg, "gl");
	const TBool gui = (cl.Length() == 0) || (cl.FindF(KGuiArg) != KErrNotFound);
	logEvent("boot", NULL, "stage1 starting, mode=%s args=\"%s\"",
		gui ? "gui" : "console", (const char *)cl8.PtrZ());

	// Зонд імпорту: "conv img<файл>" бере E:\PS1\games\<файл> і збирає з нього
	// .pbp поруч. Перевіряти конвертацію через меню наосліп неможливо, а тут
	// результат можна забрати на хост і звірити psxpack.py.
	_LIT(KConvArg, "conv");
	if (cl.FindF(KConvArg) != KErrNotFound)
		{
		_LIT(KImgC, "img");
		const TInt ip = cl.FindF(KImgC);
		TFileName p(_L("E:\\PS1\\games\\"));
		if (ip != KErrNotFound)
			{
			TPtrC nm = cl.Mid(ip + 3);
			const TInt sp2 = nm.Locate(' ');
			if (sp2 >= 0)
				nm.Set(nm.Left(sp2));
			p.Append(nm.Left(p.MaxLength() - p.Length()));
			}
		_LIT(KCopyArg, "storeas");
		const TConvMode mode = (cl.FindF(KCopyArg) != KErrNotFound)
			? EConvModeCopy : EConvModePbp;
		// Консольна гілка йде до створення cleanup stack у runStage1, а TRAP без
		// нього - паніка E32USER-CBase 69.
		CTrapCleanup *cs = CTrapCleanup::New();
		if (!cs)
			return KErrNoMemory;
		logEvent("conv", NULL, "probe entered");
		// Тільки в купі: CDesCArrayFlat - C-клас, а вони на стеку не живуть.
		CDesCArrayFlat *list = new CDesCArrayFlat(1);
		if (!list)
			{
			delete cs;
			return KErrNoMemory;
			}
		TRAPD(err, list->AppendL(p));
		if (err != KErrNone)
			{
			delete list;
			delete cs;
			return err;
			}
		const TUint t0 = User::NTickCount();
		TBuf8<KMaxFileName> p8;
		p8.Copy(p);
		logEvent("conv", NULL, "src=%s mode=%d", (const char *)p8.PtrZ(), (int)mode);
		const TInt sr = ConvStart(*list, mode, EFalse, _L("E:\\PS1\\games\\"));
		logEvent("conv", NULL, "start=%d", (int)sr);
		TConvStat st;
		st.iState = EConvRun;
		while (sr == KErrNone && st.iState == EConvRun)
			{
			User::After(200000);
			ConvStat(st);
			}
		ConvRelease();
		TBuf8<64> msg8;
		msg8.Copy(st.iMsg);
		logEvent("conv", st.iState == EConvErr ? "CONV_FAIL" : NULL,
			"state=%d pct=%d ms=%u msg=%s", (int)st.iState, (int)st.iPercent,
			(unsigned)(User::NTickCount() - t0), (const char *)msg8.PtrZ());
		delete list;
		delete cs;
		if (g_log)
			fclose(g_log);
		return KErrNone;
		}

	// "cdbench imgX cdbench imgY ..." не буває: один образ на запуск, шлях з img.
	_LIT(KCdBenchArg, "cdbench");
	if (cl.FindF(KCdBenchArg) != KErrNotFound)
		{
		_LIT(KImgA, "img");
		const TInt ip = cl.FindF(KImgA);
		TBuf<64> p(_L("E:\\PS1\\games\\"));
		if (ip != KErrNotFound)
			{
			TPtrC nm = cl.Mid(ip + 3);
			const TInt sp2 = nm.Locate(' ');
			if (sp2 >= 0)
				nm.Set(nm.Left(sp2));
			p.Append(nm.Left(p.MaxLength() - p.Length()));
			}
		else
			p.Copy(_L("E:\\PS1\\games\\gt.bin"));
		TBuf8<64> p8;
		p8.Copy(p);
		cdBench((const char *)p8.PtrZ());
		return KErrNone;
		}

	if (gui)
		{
		// Прапорці виводу - до старту фреймворку: EikStart не повертається, доки
		// застосунок живий, а вікно прив'язується вже всередині нього.
		if (cl.FindF(KGlArg) != KErrNotFound)
			voutSymbianSetGl(1);
		// "dsa"/"rowcopy" тут ОБОВ'ЯЗКОВО: parseCommandLine розбирає їх лише в
		// консольній гілці, а вікно в GUI прив'язується до неї - прапорець приходив
		// би після того, як DSA треба було підняти, і прогін мовчки йшов віконним
		// шляхом (у логу dsa active=0 при заданому dsa).
		// Показ за латчем типово увімкнений; "nodirty" вимикає.
		_LIT(KNoDirtyArg, "nodirty");
		voutSymbianSetDirtyOnly(cl.FindF(KNoDirtyArg) != KErrNotFound ? 0 : 1);
		// direct-FB коректний (причина була в Update() без регіону) і в меню
		// увімкнений типово. Тут лишається вимкненим і вмикається явним "dsa":
		// бенчмарк має лишатись зіставним із раніше знятими числами.
		_LIT(KDsaArgG, "dsa");
		voutSymbianSetDsa(cl.FindF(KDsaArgG) != KErrNotFound ? 1 : 0);
		// Візуальний A/B шляхів виводу на кадрі 3300 (потрібні очі).
		_LIT(KFbVisArg, "fbvis");
		g_fbVis = (cl.FindF(KFbVisArg) != KErrNotFound);
		// Накладка з метриками. У бенчмарку типово вимкнена: вона малює по кадру.
		_LIT(KFpsArg, "fps");
		g_fpsOverlay = (cl.FindF(KFpsArg) != KErrNotFound);
		voutSymbianSetOverlay(g_fpsOverlay ? 1 : 0);
		_LIT(KRowCopyArgG, "rowcopy");
		if (cl.FindF(KRowCopyArgG) != KErrNotFound)
			voutSymbianSetRowCopy(1);
		_LIT(KInterpArg, "interp");
		if (cl.FindF(KInterpArg) != KErrNotFound)
			ps1GuiUseInterpreter();
		_LIT(KRecArg, "rec");
		_LIT(KPlayArg, "play");
		if (cl.FindF(KPlayArg) != KErrNotFound)     g_inputMode = EInputPlay;
		else if (cl.FindF(KRecArg) != KErrNotFound) g_inputMode = EInputRecord;
		_LIT(KLimitArg, "limit");
		const TInt lp = cl.FindF(KLimitArg);
		if (lp != KErrNotFound)
			{
			TLex lex(cl.Mid(lp + 5));
			TInt secs = 0;
			if (lex.Val(secs) == KErrNone && secs > 0)
				g_playLimitMs = secs * 1000;
			}
		_LIT(KNoSndArg, "nosnd");
		if (cl.FindF(KNoSndArg) != KErrNotFound)
			g_noSound = ETrue;
		// Образ диска для прогону: "img<файл>" бере E:\<файл> замість KDiscImg.
		// Меню сюди не дістає (ps1CfgApply у консольній гілці не викликається), а
		// порівняння форматів образу одним слідом вводу інакше не зняти.
		_LIT(KImgArg, "img");
		const TInt imp = cl.FindF(KImgArg);
		if (imp != KErrNotFound)
			{
			TPtrC nm = cl.Mid(imp + 3);
			const TInt sp = nm.Locate(' ');
			if (sp >= 0)
				nm.Set(nm.Left(sp));
			TBuf<64> p(_L("E:\\PS1\\games\\"));
			p.Append(nm.Left(p.MaxLength() - p.Length()));
			TBuf8<64> p8;
			p8.Copy(p);
			const TInt n = p8.Length();
			for (TInt i = 0; i < n; i++)
				g_discPath[i] = (char)p8[i];
			g_discPath[n] = 0;
			}
		_LIT(KPixSkipArg, "pixskip");
		if (cl.FindF(KPixSkipArg) != KErrNotFound)
			g_hackPixelSkip = 1;
		_LIT(KGteNfArg, "gtenf");
		if (cl.FindF(KGteNfArg) != KErrNotFound)
			g_hackGteNoFlags = ETrue;
		_LIT(KLightArg, "light");
		if (cl.FindF(KLightArg) != KErrNotFound)
			g_gpuLighting = 1;
		_LIT(KBlendArg, "blend");
		if (cl.FindF(KBlendArg) != KErrNotFound)
			g_gpuBlending = 1;
		_LIT(KIlaceArg, "ilace");
		const TInt ip = cl.FindF(KIlaceArg);
		if (ip != KErrNotFound)
			{
			TLex lex(cl.Mid(ip + 5));
			TInt n = 0;
			g_hackIlace = (lex.Val(n) == KErrNone && n > 0) ? n : 1;
			}
		// Замірні зонди: без емуляції затримок конвеєра / без Update() екрана.
		_LIT(KNoStallsArg, "nostalls");
		g_hackNoStalls = (cl.FindF(KNoStallsArg) != KErrNotFound);
		_LIT(KNoUpdateArg, "noupdate");
		voutSymbianSetNoUpdate(cl.FindF(KNoUpdateArg) != KErrNotFound ? 1 : 0);
		_LIT(KNoSmoothArg, "nosmooth");
		voutSymbianSetSmooth(cl.FindF(KNoSmoothArg) != KErrNotFound ? 0 : 1);
		// Прототип згладжування: "fxaa" бере нижній поріг 3, "fxaa6" - шість.
		// "fxmax14" ставить верхній, щоб не чіпати контрастний текст. Типово вимкнено.
		_LIT(KFxMaxArg, "fxmax");
		TInt fxMax = 31;
		const TInt fmp = cl.FindF(KFxMaxArg);
		if (fmp != KErrNotFound)
			{
			TLex lex(cl.Mid(fmp + 5));
			TInt n = 0;
			if (lex.Val(n) == KErrNone && n > 0)
				fxMax = n;
			}
		_LIT(KFxaaArg, "fxaa");
		const TInt fxp = cl.FindF(KFxaaArg);
		if (fxp != KErrNotFound)
			{
			TLex lex(cl.Mid(fxp + 4));
			TInt n = 0;
			voutSymbianSetFxaa((lex.Val(n) == KErrNone && n > 0) ? n : 3, fxMax);
			}
		_LIT(KDumpAtArg, "dumpat");
		const TInt dap = cl.FindF(KDumpAtArg);
		if (dap != KErrNotFound)
			{
			TLex lex(cl.Mid(dap + 6));
			TInt n = 0;
			if (lex.Val(n) == KErrNone && n > 0)
				g_dumpAt = n;
			}
		// Пропуск ПОКАЗУ (кадр малюється й викидається) - для порівняння з gpuskip.
		_LIT(KShowSkipArg, "showskip");
		const TInt ssp = cl.FindF(KShowSkipArg);
		if (ssp != KErrNotFound)
			{
			TLex lex(cl.Mid(ssp + 8));
			TInt n = 0;
			g_frameskip = (lex.Val(n) == KErrNone && n > 0) ? n : 1;
			}
		// Пропуск растеризації в gpulib: "gpuskip1" - не малювати кожен другий кадр.
		_LIT(KGpuSkipArg, "gpuskip");
		const TInt gsp = cl.FindF(KGpuSkipArg);
		if (gsp != KErrNotFound)
			{
			TLex lex(cl.Mid(gsp + 7));
			TInt n = 0;
			g_gpuFrameskip = (lex.Val(n) == KErrNone && n > 0) ? n : 1;
			}
		_LIT(KCycleMulArg, "cyclemul");
		const TInt cp = cl.FindF(KCycleMulArg);
		if (cp != KErrNotFound)
			{
			TLex lex(cl.Mid(cp + 8));
			TInt n = 0;
			if (lex.Val(n) == KErrNone && n > 0)
				g_cycleMul = n;
			}
		_LIT(KFramesArg, "frames");
		const TInt fp = cl.FindF(KFramesArg);
		if (fp != KErrNotFound)
			{
			TLex lex(cl.Mid(fp + 6));
			TInt n = 0;
			if (lex.Val(n) == KErrNone && n > 0)
				g_playFrames = n;
			}

		// Той самий вибір файлом-маркером, бо запуск із меню аргументів не має.
		// Довгий сеанс через демон не варіант: поки він чекає на дочірній процес,
		// його keepalive не працює, WiFi паркується і з'єднання рветься.
		// За вмістом, а не за наявністю: демон уміє класти файли, але не видаляти,
		// тож перемикати режим інакше можна було б лише в один бік.
		_LIT(KModeMark, "E:\\PS1\\logs\\ps1_interp.txt");
		RFs markFs;
		if (markFs.Connect() == KErrNone)
			{
			RFile mf;
			if (mf.Open(markFs, KModeMark, EFileRead | EFileShareAny) == KErrNone)
				{
				TBuf8<32> m;
				mf.Read(m);
				mf.Close();
				_LIT8(KInterp, "interp");
				if (m.Find(KInterp) != KErrNotFound)
					ps1GuiUseInterpreter();
				// Той самий маркер керує слідом вводу: "rec" пише, "play" відтворює.
				_LIT8(KRec, "rec");
				_LIT8(KPlay, "play");
				if (m.Find(KPlay) != KErrNotFound)     g_inputMode = EInputPlay;
				else if (m.Find(KRec) != KErrNotFound) g_inputMode = EInputRecord;
				_LIT8(KGl, "gl");
				if (m.Find(KGl) != KErrNotFound)
					voutSymbianSetGl(1);
				}
			markFs.Close();
			}
		const TInt r = ps1AppRun();
		logEvent("exit", NULL, "gui returned %d", (int)r);
		if (g_log)
			fclose(g_log);
		return r;
		}

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
