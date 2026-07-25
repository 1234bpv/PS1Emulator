// Етап 1: завантажити libpcsxcore з інтерпретатором і обчислити кадри.
// Без виведення на екран, без звуку, без диска (HLE BIOS).
// Контракт фронтенду та механізм кадру - docs/architecture/data-spec.md.

#include <e32base.h>
#include <e32std.h>

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

static const int KFramesToRun = 60;

static FILE *g_log;
static TUint g_startTick;
static int g_eventId;

// Лог у форматі JSONL: один об'єкт на подію (CLAUDE.md, розділ 7).
static void logEvent(const char *type, const char *errType, const char *fmt, ...)
	{
	if (!g_log)
		return;

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
	fprintf(g_log, "{\"id\":%d,\"ms\":%u,\"event\":\"%s\",\"error\":%s%s%s,\"msg\":\"%s\"}\n",
		g_eventId++, (unsigned)elapsed, type,
		errType ? "\"" : "null", errType ? errType : "", errType ? "\"" : "",
		esc);
	fflush(g_log);
	}

// --- Контракт, який libpcsxcore очікує від фронтенду -----------------------
// Перелік визначено з самої бібліотеки (nm: невизначені мінус визначені).

extern "C" {

void SysPrintf(const char *fmt, ...)
	{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	logEvent("core", NULL, "%s", buf);
	}

void SysMessage(const char *fmt, ...)
	{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	logEvent("core_msg", NULL, "%s", buf);
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

// --- Заглушки плагінів ------------------------------------------------------
// LoadPlugins() вантажить .so через SysLoadLibrary, чого під Symbian немає,
// тож вказівники виставляються напряму. Справжні gpu_unai і dfsound - Етапи 2 і 4.

static long   CALLBACK gpuInit(void)                                          { return 0; }
static long   CALLBACK gpuShutdown(void)                                      { return 0; }
static long   CALLBACK gpuClose(void)                                         { return 0; }
static void   CALLBACK gpuWriteStatus(uint32_t)                               { }
static void   CALLBACK gpuWriteData(uint32_t)                                 { }
static void   CALLBACK gpuWriteDataMem(uint32_t *, int)                       { }
static uint32_t CALLBACK gpuReadStatus(void)                                  { return 0x14802000; }
static uint32_t CALLBACK gpuReadData(void)                                    { return 0; }
static void   CALLBACK gpuReadDataMem(uint32_t *, int)                        { }
static long   CALLBACK gpuDmaChain(uint32_t *, uint32_t, uint32_t *, int32_t *) { return 0; }
static long   CALLBACK gpuFreeze(uint32_t, GPUFreeze_t *, uint16_t **)        { return 0; }
static void   CALLBACK gpuVBlank(int, int)                                    { }
static void   CALLBACK gpuGetScreenInfo(int *w, int *h)                       { if (w) *w = 320; if (h) *h = 240; }

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
	GPU_init          = gpuInit;
	GPU_shutdown      = gpuShutdown;
	GPU_close         = gpuClose;
	GPU_writeStatus   = gpuWriteStatus;
	GPU_writeData     = gpuWriteData;
	GPU_writeDataMem  = gpuWriteDataMem;
	GPU_readStatus    = gpuReadStatus;
	GPU_readData      = gpuReadData;
	GPU_readDataMem   = gpuReadDataMem;
	GPU_dmaChain      = gpuDmaChain;
	GPU_freeze        = gpuFreeze;
	GPU_vBlank        = gpuVBlank;
	GPU_getScreenInfo = gpuGetScreenInfo;

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

static TInt runStage1()
	{
	logEvent("start", NULL, "stage1 begin, frames=%d", KFramesToRun);

	bindStubPlugins();

	// Мінімальний еквівалент emu_core_preinit() з frontend/main.c:435.
	memset(&Config, 0, sizeof(Config));
	strcpy(Config.Bios[0], "HLE");
	Config.HLE = 1;
	Config.PsxAuto = 1;
	Config.Cpu = CPU_INTERPRETER;
	SetIsoFile(NULL);

	if (EmuInit() == -1)
		{
		logEvent("fatal", "EMU_INIT_FAIL", "EmuInit returned -1");
		return KErrGeneral;
		}
	logEvent("init", NULL, "EmuInit ok");

	EmuReset();
	logEvent("reset", NULL, "EmuReset ok, frame_counter=%u", (unsigned)frame_counter);

	const u32 framesAtStart = frame_counter;
	for (int i = 0; i < KFramesToRun; i++)
		{
		const TUint t0 = User::NTickCount();
		const u32 before = frame_counter;

		psxRegs.stop = 0;
		psxCpu->Execute(&psxRegs);

		logEvent("frame", NULL, "n=%d ticks=%u pc=%08x cycle=%u frames=%u",
			i, (unsigned)(User::NTickCount() - t0),
			(unsigned)psxRegs.pc, (unsigned)psxRegs.cycle,
			(unsigned)(frame_counter - before));
		}

	const u32 produced = frame_counter - framesAtStart;
	logEvent("done", produced ? NULL : "NO_FRAMES",
		"frames_produced=%u pc=%08x cycle=%u", (unsigned)produced,
		(unsigned)psxRegs.pc, (unsigned)psxRegs.cycle);

	EmuShutdown();
	logEvent("shutdown", NULL, NULL);
	return produced ? KErrNone : KErrGeneral;
	}

GLDEF_C TInt E32Main()
	{
	g_startTick = User::NTickCount();
	openLog();

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
