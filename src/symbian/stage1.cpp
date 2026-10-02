// Stage 1: load libpcsxcore with interpreter and compute frames.
// No on-screen display, no sound, no disc (HLE BIOS).
// Frontend contract and frame mechanism - docs/architecture/data-spec.md.

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
// CD drive: cdra_init/cdra_open (cdrom-async.c) and CheckCdrom (misc.c) are not in our
// headers. cdra_open calls ISOopen(GetIsoFile) - this is what opens the image.
int cdra_init(void);
int cdra_open(void);
int cdra_readTrack(const unsigned char *time);
void cdra_close(void);
int CheckCdrom(void);
int SaveState(const char *file);
int LoadState(const char *file);
}

// Outside extern "C": import interface is C++ (classes, references).
#include "imgconv.h"
#ifdef PS1_BENCH
#include "ocengine.h"
#endif

// How many frames to compute. Overridden by the first command-line argument:
// EKA2L1 passes it via --run "<exe>" "<args>". Log reaches host only
// when the process exits normally, so ability to quickly shorten a run is critical.
static const int KDefaultFrames = 600;
// Launch from device menu: 300 frames = 5 seconds of game time per CPU mode.
static const int KDeviceFrames = 300;
// Progress marker step in the frame loop.
static const int KProgressEvery = 50;

// Core builds path as "%s/%s" (psxmem.c:312) - forward slash in text is intentional,
// this tests whether ESTLIB accepts it instead of backslash.
static const char KBiosDir[]  = "E:\\PS1\\bios";
static const char KBiosFile[] = "scph7502.bin";
// Game disc on card: point to .bin (data image). cdriso finds .cue itself
// by replacing extension (cdriso.c:359 parsecue) for track layout.
static const char KDiscImg[]  = "E:\\PS1\\games\\gt.bin";

static FILE *g_log;
static TUint g_startTick;
static int g_eventId;

// Log in JSONL format: one object per event (CLAUDE.md, section 7).
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

	// Core messages contain quotes and newlines - escape them so JSON remains valid.
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

	// Duplicate to debug channel: EKA2L1 sends file to host only on close,
	// so an aborted run would otherwise leave no trace.
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

// Run conditions without which two timings cannot be compared. Clocks are read from the
// PRCM registers (HAL reports a wrong core speed) and the governor may sit at either OPP,
// so the pass logs them at its start and at its end. Release has no engine: says so.
static void logClocks(const char *aWhen)
	{
#ifdef PS1_BENCH
	const TOcProfile *prof = ProfileForThisDevice();
	if (!prof)
		{
		logEvent("clocks", "NO_PROFILE", "%s: known=0 reason=unknown-device", aWhen);
		return;
		}
	COcEngine *e = NULL;
	TRAPD(err, e = COcEngine::NewL());
	if (err != KErrNone || !e)
		{
		logEvent("clocks", "CLOCKS_FAIL", "%s: known=0 reason=engine-alloc rc=%d", aWhen, (int)err);
		return;
		}
	e->SetProfile(*prof);
	const TUint32 regs[4] = { KRegClkSel1Pll, KRegClkSel2Pll, KRegClkSelMpu, KRegClkSel1Core };
	TUint32 v[4] = { 0, 0, 0, 0 };
	TInt rc = e->Prepare();
	const TInt step = (TInt)e->FailedAt();
	if (rc == KErrNone)
		rc = e->ReadRegs(regs, v, 4);
	delete e;
	if (rc != KErrNone)
		{
		logEvent("clocks", "CLOCKS_FAIL", "%s: known=0 reason=read rc=%d step=%d", aWhen, (int)rc, (int)step);
		return;
		}
	TOcClocks c;
	c.Decode(v[0], v[1], v[2], v[3]);
	logEvent("clocks", NULL,
		"%s: known=1 sel1=%08x sel2=%08x mpu=%08x core=%08x mult=%d l3div=%d dss1div=%d coreKhz=%u mpuKhz=%u busKhz=%u",
		aWhen, (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3],
		(int)c.iMult, (int)c.iL3Div, (int)c.iDss1Div,
		(unsigned)c.CoreKhz(), (unsigned)c.MpuKhz(), (unsigned)c.L3Khz());
#else
	logEvent("clocks", NULL, "%s: known=0 reason=release-build", aWhen);
#endif
	}

static TUint32 crc32Update(TUint32 aCrc, const TUint8 *aData, TInt aLen)
	{
	// Nibble table: 16 entries instead of 256, the cost is irrelevant at startup.
	static const TUint32 KTab[16] =
		{
		0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u,
		0x4db26158u, 0x5005713cu, 0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
		0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
		};
	for (TInt i = 0; i < aLen; i++)
		{
		aCrc ^= aData[i];
		aCrc = (aCrc >> 4) ^ KTab[aCrc & 15];
		aCrc = (aCrc >> 4) ^ KTab[aCrc & 15];
		}
	return aCrc;
	}

// Which binary produced the numbers: size and CRC-32 of the EXE file. The system directory is closed
// to a process without AllFiles, so on refusal the loaded code is hashed instead (src=code);
// the SHA-256 of the pushed file is printed on the PC side by run.ps1.
static void logExeId()
	{
	TFileName name = RProcess().FileName();
	TBuf8<256> name8;
	name8.Copy(name.Left(name8.MaxLength() - 1));
	TUint32 crc = 0xffffffffu;
	TInt size = 0;
	RFs fs;
	RFile f;
	TInt err = fs.Connect();
	if (err == KErrNone)
		{
		err = f.Open(fs, name, EFileRead | EFileShareAny);
		if (err == KErrNone)
			{
			TBuf8<1024> buf;
			while (f.Read(buf) == KErrNone && buf.Length() > 0)
				{
				crc = crc32Update(crc, buf.Ptr(), buf.Length());
				size += buf.Length();
				}
			f.Close();
			}
		fs.Close();
		}
	if (err == KErrNone)
		{
		logEvent("exeid", NULL, "src=file path=%s size=%d crc32=%08x",
			(const char *)name8.PtrZ(), (int)size, (unsigned)(crc ^ 0xffffffffu));
		return;
		}
	TModuleMemoryInfo mi;
	const TInt mr = RProcess().GetMemoryInfo(mi);
	if (mr == KErrNone)
		crc = crc32Update(crc, (const TUint8 *)mi.iCodeBase, (TInt)mi.iCodeSize);
	logEvent("exeid", mr == KErrNone ? NULL : "EXEID_FAIL",
		"src=code path=%s fileErr=%d size=%u crc32=%08x", (const char *)name8.PtrZ(),
		(int)err, (unsigned)(mr == KErrNone ? mi.iCodeSize : 0),
		(unsigned)(mr == KErrNone ? crc ^ 0xffffffffu : 0));
	}

// --- Contract that libpcsxcore expects from frontend -----------------------
// List is determined from the library itself (nm: undefined minus defined).

// Collapsing repeats instead of capping count. Limit cut by arrival order:
// 2000 identical "game crash @f0001234" from interpreter consumed
// the quota, leaving zero core messages for dynarec run - including
// its own self-test. Moreover, each write costs fflush to flash, so
// repeated flood also skewed timing measurements.
static char g_lastCore[512];
static int g_coreRepeat;

// Call before events that must appear in log in correct order.
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

// Plugins are built-in, there is no dynamic loading on Symbian.
void *SysLoadLibrary(const char * /*lib*/)            { return NULL; }
void *SysLoadSym(void * /*lib*/, const char * /*sym*/) { return NULL; }
const char *SysLibError()                              { return NULL; }
void SysCloseLibrary(void * /*lib*/)                   { }

// Core calls this on every vertical blank via EmuUpdate().
// Incrementing stop terminates psxCpu->Execute() right at frame boundary.
unsigned g_vblanks;   // probe: whether vertical blank actually completes the frame

// Adaptive frameskip. gpulib can skip exactly as many as needed to keep up
// with real time, but the lag estimate itself must be provided by
// the shell: upstream does this in frontend/plugin_lib.c, whose directory we omit.
// Benefit over fixed frameskip is not in number of skipped frames, but in
// WHICH ones are skipped: decision is made when game flips buffer,
// not by parity of our counter (measured: fixed frameskip loses presented
// frames superlinearly, section 12.6 of report).
extern "C" void ps1GpuSetFrameskipAdvice(int aAdvice);
static TInt  g_fsFps;        // 0 - mode disabled
static TUint g_fsStart;      // timestamp from which expected frame is computed
static TUint g_fsFrames;     // vblanks since that timestamp
static TInt  g_fsAdvice;
static TInt  g_fsAdvised;    // probe: how many times skip was advised

// Expected time is computed from fixed timestamp, not by accumulating interval:
// 1000/60 in integers would give 3% drift per minute.
void fsAdaptiveStart(TInt aFps)
	{
	g_fsFps = aFps;
	g_fsStart = User::NTickCount();
	g_fsFrames = 0;
	g_fsAdvice = 0;
	g_fsAdvised = 0;
	}

void pl_frame_limit(void)
	{
	g_vblanks++;
	psxRegs.stop++;
	if (g_fsFps <= 0)
		return;
	const TUint now = User::NTickCount();
	g_fsFrames++;
	const TInt interval = 1000 / g_fsFps;
	const TInt diff = (TInt)(g_fsStart + (g_fsFrames * 1000u) / (TUint)g_fsFps - now);
	// Pause, long disc read or first frame after load: accumulated
	// lag here is not a sign of poor performance, and without reset it would cause
	// a burst of false skips.
	if (diff < -3 * interval || diff > 3 * interval)
		{
		g_fsStart = now;
		g_fsFrames = 0;
		}
	// Between -interval and 0 advice does not change: without this hysteresis it
	// would toggle every frame and skipping would occur every other frame regardless of need.
	if (diff < -interval)
		g_fsAdvice = 1;
	else if (diff >= 0)
		g_fsAdvice = 0;
	if (g_fsAdvice)
		g_fsAdvised++;
	ps1GpuSetFrameskipAdvice(g_fsAdvice);
	}

unsigned short in_keystate[8];

// Tilt reaches the pad here rather than through in_keystate: it drives the stick,
// which the key path knows nothing about.
int voutSymbianTiltAnalog(unsigned char *aLX, unsigned char *aLY,
	unsigned char *aRX, unsigned char *aRY);
unsigned short voutSymbianTiltPadBits(void);

// in_keystate[0] holds pressed buttons active-high (DKEY_* bits), populated
// every frame from device keys (voutSymbianPollPad). PS1 expects active-low, hence ~.
void PAD1_readPort(struct PadDataS *pad, int *is_multitap)
	{
	if (pad)
		{
		unsigned char lx = 127, ly = 127, rx = 127, ry = 127;
		const int tilt = voutSymbianTiltAnalog(&lx, &ly, &rx, &ry);
		// The core keeps DualShock state - analog and config mode - inside this
		// struct between polls, so wiping it here would undo the mode every frame.
		if (tilt)
			{
			pad->controllerType = PSE_PAD_TYPE_ANALOGPAD;
			// There is no ANALOG button to press on a phone, so the mode is held on:
			// a tilt that only moves a d-pad throws away everything it is good for.
			pad->ds.padMode = 1;
			pad->ds.userToggled = 1;
			}
		else
			pad->controllerType = PSE_PAD_TYPE_STANDARD;
		// The digital bits fire only at full stick, so a game that ignores the
		// stick still answers a hard tilt, and one that reads it sees nothing new.
		pad->buttonStatus = (unsigned short)~(in_keystate[0] | voutSymbianTiltPadBits());
		pad->leftJoyX = lx;
		pad->leftJoyY = ly;
		pad->rightJoyX = rx;
		pad->rightJoyY = ry;
		}
	if (is_multitap) *is_multitap = 0;
	}

void PAD2_readPort(struct PadDataS *pad, int *is_multitap)
	{
	if (pad) memset(pad, 0, sizeof(*pad));   // port 2 is empty
	if (is_multitap) *is_multitap = 0;
	}

void pl_gun_byte2(int /*port*/, unsigned char /*byte*/) { }
void plat_trigger_vibrate(int /*pad*/, int /*low*/, int /*high*/) { }

} // extern "C"

// --- Plugins ----------------------------------------------------------------
// LoadPlugins() loads .so via SysLoadLibrary, which does not exist on Symbian,
// so pointers are assigned directly. GPU is real gpu_unai via gpulib
// (Stage 2). Sound is muted for now, that is Stage 4.

// Wrappers from src/symbian/vout_symbian.cpp: gpulib names clash with TYPES
// in plugins.h, so they cannot be declared here directly.
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
static int g_tiltCli;
extern "C" int ps1TiltCli(void) { return g_tiltCli; }
static int g_keyProbeCli;
extern "C" int ps1KeyProbeCli(void) { return g_keyProbeCli; }
extern "C" void voutSymbianSetTilt(int aOn);
extern "C" int voutSymbianTiltActive(void);
extern "C" int voutSymbianTiltStats(int *aX, int *aY, int *aZeroX, int *aZeroY, int *aPad);
extern "C" TInt accelStart(void);
extern "C" void accelStop(void);
extern "C" TInt accelRead(TInt *aX, TInt *aY, TInt *aZ);
void voutSymbianPresent(void);
int voutSymbianFinish(int holdMs);
void voutSymbianGpuStats(unsigned *data, unsigned *dataMem, unsigned *dma, unsigned *status);
void voutSymbianBlitSplit(unsigned *fillMs, unsigned *presentMs);
void voutSymbianMemBench(char *aOut, int aOutLen);
void sndSymbianWsolaBench(char *aOut, int aOutLen);
int voutSymbianFbPixBytes(void);
int voutSymbianFbStrideB(void);
void voutSymbianSetBlitC(int on);
void gpuUnaiDrawStats(unsigned *out, int n);
void gpuUnaiSetSpanOff(int on);
void gpuUnaiSetTexFlat(int on);
void gpuUnaiSetDstFlat(int on);
int gpuUnaiCfEnabled(void);
void gpuUnaiCfReset(void);
int gpuUnaiCfGet(unsigned cf, int route, unsigned *out);
int gpuUnaiCfTimeEnabled(void);
void gpuUnaiCfTimeReset(void);
int gpuUnaiCfTimeGet(unsigned cf, unsigned long long *ticks,
	unsigned *primitives, unsigned *sampled);
void gpuUnaiCfTimeCalibration(unsigned long long *ticks, unsigned *calls);
#ifdef PS1_BENCH
int spuStageProfileEnabled(void);
void spuStageProfileReset(void);
int spuStageProfileGet(unsigned int stage, unsigned long long *ticks,
	unsigned int *calls, unsigned int *sampled, unsigned int *samples);
void spuStageProfileCalibration(unsigned long long *ticks, unsigned int *calls);
extern unsigned int g_ndrcLuiEmitted, g_ndrcLuiWouldLiteral, g_ndrcLuiTwoByte;
int pmuSymbianStart(int evt0, int evt1, unsigned *diag);
int pmuSymbianRead(unsigned *ccnt, unsigned *pmn0, unsigned *pmn1, unsigned *direct);
void pmuSymbianStop(void);
unsigned pmuSymbianHello(unsigned *sram);
void ps1SndSetDump(int aOn);
void voutSymbianSetSelfInterrupt(int aAt, int aFor);
void voutSymbianSelfInterruptTick(int aFrame);
#endif
void voutSymbianRowStats(unsigned *box, unsigned *plain, unsigned *scale, unsigned *rgb24, unsigned *other);
void voutSymbianRgb24Stats(unsigned *scaled, unsigned *b16, int *srcW, int *dstW);
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
void voutSymbianDsaDiag(int *covered, int *fbOk, int *rowCopy, int *rgnCount, int *rgnX,
	int *rgnY, int *rgnW, int *rgnH, int *scrW, int *scrH, int *recoverTries);
int voutSymbianReadBack(int mode, int *match, int *total, int *firstBad);
void voutSymbianFbPages(int *changes, int *seen, int *b0, int *b1);
int voutSymbianHalDump(const char *path);
int voutSymbianHalAddr(void);
int voutSymbianRawDump(const char *path, int bytes);
int voutSymbianGlBench(int frames);
// MBX rasterizer (glrender.cpp).
int  glrInit(void);
void glrShutdown(void);
void glrSetEnabled(int on);
int  glrEnabled(void);
void glrBeginFrame(void);
int  glrEndFrame(void);
void glrStats(int *verts, int *batches, int *over, int *hit, int *miss, int *evict);
void glrShot(const char *path);
void glrSetTexWhite(int on);
void glrSetSkip(int mask);
void glrSetDummyTex(int aSide);
void glrSetBgForce(int on);
void glrDepthProbe(void);
void glrSetSort(int on);
void glrSetZ(int on);
void glrSetIdx(int on);
void glrShotBig(void);
// Switch for gpu_unai software pass ("swoff" argument). Lives in gpulib_if.cpp.
extern int gpu_unai_sw_off;
// Dynarec block pre-compile on state load: crashes on this device, off by default.
extern "C" int ndrc_skip_load_blocks;
// Set when "swoff" CLI arg is used; menu must not override it.
static int g_swoffCli;
// Same for "glrender": a measurement run picks the rasteriser, not the menu.
static int g_glRenderCli;
int voutSymbianVisTest(int raw);
void voutSymbianSetOverlay(int on);
void voutSymbianSetNoUpdate(int on);
void voutSymbianSetSmooth(int on);
void voutSymbianSetDsaNoResume(int on);
void voutSymbianSetFxaa(int threshold, int max);
void voutSymbianSetStats(int fpsX10, int msX10);
void voutSymbianSetPumpAo(int on);
static TBool g_fbVis;

// Counters directly in gpulib: all core paths (interp/drc) converge there,
// so this is a reliable signal of whether BIOS actually sends draw commands.
extern unsigned int g_gpulibWriteData, g_gpulibWriteDataMem, g_gpulibDmaChain;
extern unsigned int g_gpulibDmaWords, g_gpulibWriteStatus, g_gpulibReadStatus;
extern unsigned int g_gpulibStatusLast;
// gpu_unai rasterization time (FastCounter ticks) - separates CPU vs GPU in "emu".
extern unsigned long long g_gpulibRenderTicks;
// SPU mixing time (FastCounter ticks) - separates sound from CPU inside "emu".
extern unsigned long long g_spuMixTicks;
#ifdef PS1_BENCH
extern unsigned long long g_spuCoreTicks;
extern unsigned int g_spuCoreCalls, g_spuCoreActive, g_spuCoreSamples;
#endif
// Sector read time from image (FastCounter ticks) including decompression if
// image is compressed, and read count - cost of image format inside "emu".
extern unsigned long long g_cdReadTicks;
extern unsigned int g_cdReadCount;
// MDEC decoding time (FastCounter ticks) - cost of video inside "emu".
extern unsigned long long g_mdecDecodeTicks;
extern unsigned int g_mdecWords;
unsigned int ndrc_symbian_fastfreq(void);
unsigned int ndrc_symbian_fastcount(void);
unsigned int ndrc_symbian_tcbase(void);
}

// How long to hold frame on screen so human can look (headless launch
// over WiFi: watching device itself during run).
static const int KScreenHoldMs = 5000;

// Pass state and frame stage are declared here, not near the loop itself: they are read
// by watchdog thread (black box below), which is defined before the loop.
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

// Hang protection. Frame loop relies on psxCpu->Execute()
// returning at frame boundary (pl_frame_limit sets psxRegs.stop on VBlank).
// Real BIOS may spin in early initialization BEFORE first VBlank - then
// Execute never returns, and deployment daemon is blocked in RUN without
// timeout, so phone can only be recovered via reboot. Watchdog thread sets
// psxRegs.stop after KWatchdogMs (interpreter checks it every instruction,
// while(!regs->stop)), and pass completes with partial result in log.
static const TInt KWatchdogMs = 60000;

// Menu launch (alpha 1.0) plays the game, so large frame budget and longer
// watchdog are needed to avoid cutting session short. Pass still self-terminates
// (avoids leaving eternal console process that is hard to kill).
static const TInt KPlayFrames = 200000;
// The menu game has no frame budget: running out of it used to close the app
// without a word after ~55 min of play. Only stand runs keep the finite KPlayFrames.
static const TInt KGuiFrames = 0x7fffffff;
static const TInt KPlayWatchdogMs = 1200000;    // 20 min
static TInt g_watchdogMs = KWatchdogMs;         // benchmark by default

// Session duration ceiling, ms. Zero - no limit (normal play from menu).
// "limit<N>" argument sets it for runs via daemon.
static TInt g_playLimitMs;

// How many frames to compute. "frames<N>" argument. For measurements this is more reliable
// than time limit: in fixed time faster build advances further in game and renders
// DIFFERENT scenes, so comparison is invalid. Identical frame count with identical
// input trace produces identical work, and timing difference reflects real code difference.
static TInt g_playFrames;

// "nosnd" argument: run without speaker sound.
static TBool g_noSound;

// Speedups and rasterization quality. All disabled by default, except those
// where disabling is itself a loss (see light/blend below).
//   pixskip  - skip invisible pixels in high horizontal resolution modes
//   ilace<N> - skip lines (1 = every other line)
//   gtenf    - do not calculate GTE flags in dynarec
//   light/blend - texture lighting modulation and blending; they were
//                 disabled UNINTENTIONALLY due to zeroed gpu_unai_config_ext
//   cyclemul<N> - how many cycles guest instruction costs (175 = stock)
static TInt g_hackPixelSkip;
static TInt g_hackIlace;
static TBool g_hackGteNoFlags;
static TInt g_gpuLighting;
static TInt g_gpuBlending;
static TInt g_cycleMul;

extern "C" void gpu_unai_set_config(int aPixelSkip, int aIlaceForce,
	int aLighting, int aFastLighting, int aBlending);
// Copies config into rasterizer working struct after renderer_init.
extern "C" void gpu_unai_apply_config(void);
// Present only in gpu_unai build with -DGPU_UNAI_PRIMSTATS; weak symbol so regular
// build links without it. Field order matches gpu_unai_primstats_t.
extern "C" int gpu_unai_get_primstats(unsigned *aOut, int aMax)
	__attribute__((weak));
// Rasterizer does not see frame boundary, and distinct texture counter per frame
// is meaningless without it; therefore called from here.
extern "C" void gpu_unai_primstats_frame(void)
	__attribute__((weak));
// VRAM operations bypassing rasterizer (gpulib): write, read, copy - in
// counts and pixels. For hardware renderer these are synchronization points.
extern "C" unsigned int gpulib_vram_stats[6];
extern "C" int g_gpuSpeedPct;

// Presentation skip: show every (N+1)-th frame, only compute the rest. Emulation
// and rasterization are NOT skipped - saves presentation itself (conversion + window),
// which is 13 of 37 ms per frame. More accurately called presentation skip rather than frameskip.
static TInt g_frameskip;

// RASTERIZATION skip via gpulib: how many consecutive frames are allowed to skip rendering
// (0 - disabled). Unlike g_frameskip above, here the work itself is skipped, not
// just its presentation; gpulib also maintains its own guard checks (does not skip
// when game draws directly into display area).
static TInt g_gpuFrameskip;

// Probes for measurements (command line only, not present in menu).
static TBool g_hackNoStalls;
static TBool g_memBench;
static TBool g_sndBench;
#ifdef PS1_BENCH
// Hardware counter pair to run over the pass, as two event numbers; -1 is off.
// Snapshotting patches kernel code, so it happens twice per pass and never
// inside the frame loop.
static TInt g_pmuEvt0 = -1, g_pmuEvt1 = -1;
static unsigned g_pmuBase[3];
#endif
// Frame of the first screenshot; 3300 - track, lower values hit game menu,
// where it runs in 640x480 and scaling behavior is visible.
static TInt g_dumpAt = 3300;

// Save and load state for benchmark harness.
static const char KBenchSavePath[] = "E:\\PS1\\saves\\bench.sav";
_LIT(KBenchSavePathDes, "E:\\PS1\\saves\\bench.sav");
static TInt g_saveStateAt;
static TInt g_loadStateAt;
static TBool g_savedState;

// Metrics overlay. Averaging window ~0.5 s: with single frame the number would jump
// so much that reading it from screen would be impossible.
static const TInt KFpsWindowMs = 500;
static TBool g_fpsOverlay;
static TUint g_fpsWinTick;
static TInt  g_fpsWinFrame;
static TUint g_fpsWinShown;   // presented frames counter at start of window

// Disc image selected in menu. Empty - use KDiscImg.
static char g_discPath[256];
// BIOS dump name in E:\PS1\bios folder (without path - provided by Config.BiosDir).
// Empty means default KBiosFile; set from menu, including per-game.
static char g_biosFile[64];

// Window belongs to CCoeControl container and attaches to output before setup,
// so in GUI mode we do not create or destroy own window. Declared here
// because diagnostic logging frequency in watchdog (below) depends on it.
static TBool g_guiMode;
// Launched with arguments (deploy stand). Stand-only diagnostics (screen and VRAM
// dumps, test pattern) run only then; a game started from the menu has none.
static TBool g_cliArgs;

static volatile TInt g_wdRun;    // 1 while pass wants watchdog alive
static volatile TInt g_wdFired;  // 1 if watchdog triggered (spin longer than limit)

// Diagnostics: watchdog samples pc/cycle once every ~2 s. Trajectory shows whether
// execution is stuck on single instruction (handler hung) or creeping - without this,
// slow/hung BIOS cannot be localized on hardware.
static const TInt KWdSamples = 40;
static volatile TUint32 g_wdPc[KWdSamples];
static volatile TUint32 g_wdCyc[KWdSamples];
static volatile TInt g_wdSamples;

// Heartbeat of frame loop: incremented on every step.
static volatile TInt g_wdBeat;

// Main thread ID: watchdog reads its context (where exactly it is executing).
static TThreadId g_mainThreadId;

#ifdef PS1_STAGE2_PROFILE
// Stage 2 profile: where the guest thread spends its time. The watchdog reads the
// main thread's PC every g_s2Ms milliseconds (arg "pcprof<ms>"; 0 = off, so one
// binary gives the with/without pair). Samples are kept raw in memory and folded
// into events only at teardown: s2prof, s2map, s2cls, s2hist, s2dyn, s2slow.
// Classes that need symbols are resolved on the PC (tools-patches/s2prof.ps1).
static const TInt KS2Max = 32768;
static TInt g_s2Ms;
static volatile TInt g_s2On;
static volatile TInt g_s2N;
static TUint32 g_s2Pc[KS2Max];
static TUint8  g_s2Stage[KS2Max];
static TInt g_s2Fail, g_s2Drop;
static TUint g_s2Tick0, g_s2Tick1, g_s2CtxTicks;
static TUint32 g_s2TcBase, g_s2TcSize;
extern "C" {
unsigned int ndrc_symbian_tcsize(void);
void *ndrc_symbian_alloc(unsigned int size);
extern unsigned int g_s2_rc_ticks, g_s2_rc_blocks, g_s2_rc_fail;
extern unsigned int g_s2_rc_pass[10], g_s2_rc_slen, g_s2_rc_repeat, g_s2_rc_uniq, g_s2_rc_bk[8][12];
extern unsigned int g_s2_ht_live, g_s2_ht_hit0, g_s2_ht_hit1, g_s2_ht_slow, g_s2_links;
extern unsigned int g_s2_slow[2][6];
extern int ndrc_s2_d1;
extern unsigned int g_s2ImbCalls, g_s2ImbTicks, g_s2ImbBytes;
}
// Dynarec counters at the start of the frame loop, so the loadstate warm-up is not counted.
static const TInt KS2Dyn = 22 + 8 * 12 + 1;
static const TInt KS2Slow = 12;
static TUint g_s2DynBase[KS2Dyn];
static TUint g_s2SlowBase[KS2Slow];

static void s2DynSnap(TUint *aDyn, TUint *aSlow)
	{
	aDyn[0] = g_s2_rc_ticks; aDyn[1] = g_s2_rc_blocks; aDyn[2] = g_s2_rc_fail;
	aDyn[3] = g_s2_ht_live; aDyn[4] = g_s2_ht_hit0; aDyn[5] = g_s2_ht_hit1;
	aDyn[6] = g_s2_ht_slow; aDyn[7] = g_s2_links;
	aDyn[8] = g_s2ImbCalls; aDyn[9] = g_s2ImbTicks; aDyn[10] = g_s2ImbBytes;
	aDyn[11] = g_s2_rc_slen; aDyn[12] = g_s2_rc_repeat;
	for (TInt k = 1; k < 10; k++) aDyn[12 + k] = g_s2_rc_pass[k];
	for (TInt b = 0; b < 8; b++)
		for (TInt k = 0; k < 12; k++) aDyn[22 + b * 12 + k] = g_s2_rc_bk[b][k];
	aDyn[22 + 8 * 12] = g_s2_rc_uniq;
	for (TInt k = 0; k < 6; k++)
		{ aSlow[k] = g_s2_slow[0][k]; aSlow[6 + k] = g_s2_slow[1][k]; }
	}

// Called where the pass clock starts.
static void s2Begin()
	{
	s2DynSnap(g_s2DynBase, g_s2SlowBase);
	g_s2N = 0; g_s2Fail = 0; g_s2Drop = 0; g_s2CtxTicks = 0;
	g_s2TcBase = (TUint32)ndrc_symbian_tcbase();
	g_s2TcSize = (TUint32)ndrc_symbian_tcsize();
	g_s2On = (g_s2Ms > 0) ? 1 : 0;
	}
#endif

// Terminates COOPERATIVELY: checks g_wdRun in short steps and exits on its own
// when pass disables it. Avoids RThread::Kill (panic when killing already
// dead thread) and avoids shutdown race conditions.
static void crashHandler(TExcType aType);

// Stack snapshot from exception handler. Core places saved faulting code
// context on thread stack before invoking handler, so faulting instruction
// address is among these words - look for values within translation cache or
// program code. Handler itself does not write to file: stdio from it produced nothing,
// and opening RFs from there is too risky (secondary fault leaves nothing).
// Handler makes NO system calls prior to publication: previous implementation
// took StackInfo and waited via User::After, and recorded nothing - blocking
// call from exception context kills thread silently. Stack base is therefore
// remembered in advance (paintStack), and handler waits via spin loop.
static const TInt KCrashWords = 64;
static volatile TInt g_crashType = -1;
static volatile TInt g_crashStep;
static volatile TUint32 g_crashSp;
static volatile TUint32 g_crashStack[KCrashWords];
static volatile TInt g_crashWords;
static volatile TInt g_crashPublished;
static TUint32 g_stackBase;

// Recompiler trace (new_dynarec.c): which guest block and pass was currently
// executing. Without this, faulting instruction address reveals nothing about the cause.
extern "C" {
extern unsigned int ndrc_dbg_addr, ndrc_dbg_pass, ndrc_dbg_i, ndrc_dbg_slen, ndrc_dbg_out;
}

static TInt watchdogThread(TAny *aArg)
	{
	// Exception handler is per-thread, not per-process: without this, fault in watchdog itself
	// would kill process silently, looking identical to fault in frame loop.
	User::SetExceptionHandler(crashHandler,
		KExceptionAbort | KExceptionKill | KExceptionUserInterrupt | KExceptionFpe |
		KExceptionFault | KExceptionInteger | KExceptionDebug);

	// Black box. KERN-EXEC 3 terminates process so quickly that exception handler
	// cannot write anything (verified: installed, but never called),
	// so state is written by NEIGHBORING thread - panic cannot erase data already on flash.
	// Native RFile rather than stdio: ESTLIB keeps C library state per-thread, and
	// fopen from here simply returned NULL (file was not created).
	// Main thread context: its PC indicates WHERE it executes - in translation
	// cache (generated code) or in program code (recompiler itself). This
	// divides search space in half, which stage counter alone cannot provide.
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
	// 5 ms step: after crash process survives only tens of milliseconds, and a coarser
	// step fails to capture the stall (verified - at 25 ms it was lost).
#ifdef PS1_STAGE2_PROFILE
	const TInt step = g_s2Ms > 0 ? g_s2Ms : 5;
#else
	const TInt step = 5;
#endif
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
#ifdef PS1_STAGE2_PROFILE
		if (g_s2On && haveMainT)
			{
			const TUint c0 = User::FastCounter();
			TBuf8<128> sctx;
			mainT.Context(sctx);
			const TBool ok = sctx.Length() >= 16 * 4;
			if (ok && g_s2N < KS2Max)
				{
				const TInt k = g_s2N;
				g_s2Pc[k] = ((const TUint32 *)sctx.Ptr())[15];
				g_s2Stage[k] = (TUint8)g_stage;
				g_s2N = k + 1;
				g_s2Tick1 = User::NTickCount();
				if (!k) g_s2Tick0 = g_s2Tick1;
				}
			else if (!ok)
				g_s2Fail++;
			else
				g_s2Drop++;
			g_s2CtxTicks += User::FastCounter() - c0;
			}
#endif
		if (haveBeat && elapsed >= nextBeatMs)
			{
			unsigned gpData = 0, gpDataMem = 0, gpDma = 0, gpStatus = 0;
			voutSymbianGpuStats(&gpData, &gpDataMem, &gpDma, &gpStatus);
			// Heap: dynarec metadata (blocks, jump lists, hash tables)
			// lives here and grows fastest when loading new code.
			// EPOCHEAPSIZE caps them, and failed malloc in dynarec
			// is not checked everywhere - then write occurs through null pointer.
			TInt heapBiggest = 0;
			const TInt heapAvail = User::Heap().Available(heapBiggest);

			// r13/r14/r15 of main thread. TArmRegSet is array of 32-bit words,
			// PC in it is 16th (index 15).
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
			// Each heartbeat is a write with Flush to memory card, i.e. a FAT
			// maintenance operation mid-frame. When hunting crashes (console), 50 ms
			// is justified: after crash process survives tens of milliseconds. In game this
			// is 20 flash memory accesses per second - precisely the periodic
			// micro-stutters visible in gameplay, so there the interval is one second.
			nextBeatMs += g_guiMode ? 1000 : 50;

			// Faulting instruction address without exception handler. That handler is not
			// invoked on this device, but core leaves faulting code context on
			// main thread stack - and watchdog runs in same process and reads
			// its memory directly. Stall indication: pc, sp AND guest cycle counter
			// have not changed between samples while stage is execution.
			if (!stuckDumped && g_stage == EStageExec && hpc == lastHpc &&
				hsp == lastHsp && (TUint32)psxRegs.cycle == lastCycle && hsp)
				{
				// Write snapshot of PREVIOUS sample rather than fresh one: at this step
				// main thread is already dead, its stack is unmapped, and
				// reading from it killed watchdog itself (verified - file was truncated).
				// Single write: process dies in milliseconds.
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
				// Rolling update on every sample while thread is alive: reading its stack
				// is safe only before crash, and state at crash moment is what is needed.
				// Window extends below sp: side on which kernel places faulting code
				// context is unspecified, and faulting function frame lies below.
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
		// Keep screen awake. This is not convenience, but comparability requirement: when
		// display sleeps, window server has nothing to compose, and present drops
		// from 12000 to 1700 ms per 9000 frames - larger than any effect measured
		// here. Once every 10 s is sufficient.
		if ((elapsed % 10000) < step)
			User::ResetInactivityTime();
		// Measure STALL, not pass duration: in GUI session lasts hours, and
		// total pass limit would cut game mid-play. Frame counter
		// stopping is the exact case watchdog exists for (Execute does not
		// return because BIOS spins before first VBlank).
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

// Sound: real SPU - dfsound via ps1Spu* wrappers in snd_symbian.cpp (plugins.h
// is not included there, so dfsound function names do not conflict with typedef).
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
void ps1SpuPlayADPCMchannel(xa_decode_t *, unsigned int, int);
int ps1SpuPlayCDDAchannel(short *, int, unsigned int, int);
void ps1SpuSetCDvol(unsigned char, unsigned char, unsigned char, unsigned char, unsigned int);
#ifdef PS1_BENCH
void ps1SpuCdStats(unsigned *xa, unsigned *cdda, unsigned *vol);
#endif
void ps1SndConfigure(void);
void ps1SndSelectOutput(void);
void ps1SndStats(unsigned *feeds, unsigned *samples, int *maxAmp,
                 unsigned *under, int *stepQ16);
int  ps1SndOpenOutput(void);
void ps1SndPump(void);
void ps1SndCloseOutput(void);
void ps1SndSetPumpAo(int aOn);
void ps1SndSetWsola(int aOn);
}

// SPU freeze is forwarded to dfsound so SaveState/LoadState do not get zero size and NULL.
extern "C" long DoFreeze(int, SPUFreeze_t *, unsigned short **, void *, unsigned int);

static long CALLBACK spuFreeze(int aMode, SPUFreeze_t *aHdr, unsigned short **aRam, void *aP2, unsigned int aCycles)
	{
	return DoFreeze(aMode, aHdr, aRam, aP2, aCycles);
	}

// --- Save and load state via native RFile (uncompressed) ---

// Flag enabled by "savefuncs" argument - see comment near hook.
static TBool g_saveFuncsOverride;
static const TInt KSaveBufferSize = 64 * 1024;

struct TSaveFile
	{
	RFs    iFs;
	RFile  iFile;
	TBool  iWriting;
	TInt   iBufPos;
	TUint8 iBuffer[KSaveBufferSize];
	};

static TInt saveFlushBuffer(TSaveFile *aFile)
	{
	if (!aFile || !aFile->iWriting)
		{
		return KErrNone;
		}

	if (aFile->iBufPos > 0)
		{
		TPtrC8 des(aFile->iBuffer, aFile->iBufPos);
		const TInt err = aFile->iFile.Write(des);
		if (err != KErrNone)
			{
			logEvent("save", "SAVE_WRITE_FAIL", "flush failed err=%d bytes=%d", err, aFile->iBufPos);
			return err;
			}
		aFile->iBufPos = 0;
		}
	return KErrNone;
	}

extern "C" {

static void *symbianSaveOpen(const char *name, const char *mode)
	{
	if (!name || !mode)
		{
		return NULL;
		}

	const TBool isWrite = (mode[0] == 'w');
	const TBool isRead  = (mode[0] == 'r');
	if (!isWrite && !isRead)
		{
		return NULL;
		}

	TSaveFile *f = new TSaveFile;
	if (!f)
		{
		logEvent("save", "SAVE_NOMEM", "alloc TSaveFile failed path=%s", name);
		return NULL;
		}

	f->iWriting = isWrite;
	f->iBufPos  = 0;

	TInt err = f->iFs.Connect();
	if (err != KErrNone)
		{
		logEvent("save", "SAVE_FS_FAIL", "RFs connect failed err=%d path=%s", err, name);
		delete f;
		return NULL;
		}

	const TInt nameLen = (TInt)strlen(name);
	TPtrC8 name8((const TUint8 *)name, nameLen);
	TFileName fileName;
	if (nameLen > fileName.MaxLength())
		{
		fileName.Copy(name8.Left(fileName.MaxLength()));
		}
	else
		{
		fileName.Copy(name8);
		}

	if (isWrite)
		{
		err = f->iFile.Replace(f->iFs, fileName, EFileWrite | EFileShareAny);
		}
	else
		{
		err = f->iFile.Open(f->iFs, fileName, EFileRead | EFileShareAny);
		}

	if (err != KErrNone)
		{
		logEvent("save", "SAVE_OPEN_FAIL", "open failed mode=%s path=%s err=%d", mode, name, err);
		f->iFs.Close();
		delete f;
		return NULL;
		}

	logEvent("save", NULL, "open ok mode=%s path=%s", mode, name);
	return (void *)f;
	}

static int symbianSaveRead(void *file, void *buf, u32 len)
	{
	TSaveFile *f = (TSaveFile *)file;
	if (!f || f->iWriting || !buf)
		{
		logEvent("save", "SAVE_READ_FAIL", "invalid read args file=%p writing=%d buf=%p len=%u",
			f, f ? (int)f->iWriting : 0, buf, (unsigned)len);
		return -1;
		}
	if (len == 0)
		{
		return 0;
		}
	// Guard against negative/excessive u32 values
	if ((TInt)len <= 0 || len > 0x10000000)
		{
		logEvent("save", "SAVE_READ_FAIL", "bad read length len=%u", (unsigned)len);
		return -1;
		}

	TInt curPos = 0;
	f->iFile.Seek(ESeekCurrent, curPos);
	logEvent("save", NULL, "read pos=%d len=%u", curPos, (unsigned)len);

	TUint8 *dst = (TUint8 *)buf;
	TInt totalRead = 0;
	TInt remaining = (TInt)len;

	while (remaining > 0)
		{
		const TInt chunk = (remaining > KSaveBufferSize) ? KSaveBufferSize : remaining;
		TPtr8 des(dst + totalRead, 0, chunk);
		const TInt err = f->iFile.Read(des, chunk);
		if (err != KErrNone)
			{
			logEvent("save", "SAVE_READ_FAIL", "read failed err=%d pos=%d totalRead=%d want=%u",
				err, curPos + totalRead, totalRead, (unsigned)len);
			return -1;
			}
		if (des.Length() == 0)
			{
			break;
			}
		totalRead += des.Length();
		remaining -= des.Length();
		}
	return totalRead;
	}

static int symbianSaveWrite(void *file, const void *buf, u32 len)
	{
	TSaveFile *f = (TSaveFile *)file;
	if (!f || !f->iWriting || !buf)
		{
		logEvent("save", "SAVE_WRITE_FAIL", "invalid write args file=%p writing=%d buf=%p len=%u",
			f, f ? (int)f->iWriting : 0, buf, (unsigned)len);
		return -1;
		}
	if (len == 0)
		{
		return 0;
		}
	// Guard against negative/excessive u32 values
	if ((TInt)len <= 0 || len > 0x10000000)
		{
		logEvent("save", "SAVE_WRITE_FAIL", "bad write length len=%u", (unsigned)len);
		return -1;
		}

	TInt curPos = 0;
	f->iFile.Seek(ESeekCurrent, curPos);
	logEvent("save", NULL, "write pos=%d len=%u bufPos=%d", curPos, (unsigned)len, f->iBufPos);

	const TUint8 *src = (const TUint8 *)buf;
	u32 remaining = len;

	while (remaining > 0)
		{
		const TInt avail = KSaveBufferSize - f->iBufPos;
		if ((u32)avail > remaining)
			{
			memcpy(f->iBuffer + f->iBufPos, src, remaining);
			f->iBufPos += (TInt)remaining;
			remaining = 0;
			}
		else
			{
			memcpy(f->iBuffer + f->iBufPos, src, avail);
			f->iBufPos += avail;
			src += avail;
			remaining -= avail;
			const TInt err = saveFlushBuffer(f);
			if (err != KErrNone)
				{
				return -1;
				}
			}
		}

	return (int)len;
	}

static long symbianSaveSeek(void *file, long offs, int whence)
	{
	TSaveFile *f = (TSaveFile *)file;
	if (!f)
		{
		return -1;
		}

	if (f->iWriting)
		{
		const TInt err = saveFlushBuffer(f);
		if (err != KErrNone)
			{
			return -1;
			}
		}

	TInt fileSize = 0;
	TInt err = f->iFile.Size(fileSize);
	if (err != KErrNone)
		{
		logEvent("save", "SAVE_SEEK_FAIL", "file size failed err=%d", err);
		return -1;
		}

	TInt curPos = 0;
	err = f->iFile.Seek(ESeekCurrent, curPos);
	if (err != KErrNone)
		{
		logEvent("save", "SAVE_SEEK_FAIL", "get pos failed err=%d", err);
		return -1;
		}

	logEvent("save", NULL, "seek offs=%ld whence=%d curPos=%d size=%d",
		offs, whence, curPos, fileSize);

	TInt targetPos = 0;
	if (whence == SEEK_SET)
		{
		targetPos = (TInt)offs;
		}
	else if (whence == SEEK_CUR)
		{
		targetPos = curPos + (TInt)offs;
		}
	else if (whence == SEEK_END)
		{
		targetPos = fileSize + (TInt)offs;
		}
	else
		{
		logEvent("save", "SAVE_SEEK_FAIL", "unknown whence=%d", whence);
		return -1;
		}

	if (targetPos < 0)
		{
		logEvent("save", "SAVE_SEEK_FAIL", "seek negative target=%d size=%d cur=%d offs=%ld whence=%d",
			targetPos, fileSize, curPos, offs, whence);
		return -1;
		}

	if (targetPos > fileSize)
		{
		if (f->iWriting)
			{
			// On write extend file because RFile::Seek clamps position to Size
			err = f->iFile.SetSize(targetPos);
			if (err != KErrNone)
				{
				logEvent("save", "SAVE_SEEK_FAIL", "SetSize failed err=%d target=%d", err, targetPos);
				return -1;
				}
			fileSize = targetPos;
			}
		else
			{
			logEvent("save", "SAVE_SEEK_FAIL", "seek past EOF target=%d size=%d cur=%d offs=%ld whence=%d",
				targetPos, fileSize, curPos, offs, whence);
			return -1;
			}
		}

	TInt newPos = targetPos;
	err = f->iFile.Seek(ESeekStart, newPos);
	if (err != KErrNone || newPos != targetPos)
		{
		logEvent("save", "SAVE_SEEK_FAIL", "seek failed err=%d newPos=%d target=%d", err, newPos, targetPos);
		return -1;
		}

	return (long)newPos;
	}

static void symbianSaveClose(void *file)
	{
	TSaveFile *f = (TSaveFile *)file;
	if (!f)
		{
		return;
		}

	logEvent("save", NULL, "close writing=%d bufPos=%d", (int)f->iWriting, f->iBufPos);

	if (f->iWriting)
		{
		const TInt err = saveFlushBuffer(f);
		if (err != KErrNone)
			{
			logEvent("save", "SAVE_FLUSH_FAIL", "close flush failed err=%d", err);
			}
		}

	f->iFile.Close();
	f->iFs.Close();
	delete f;
	}

} // extern "C"

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
	SPU_playADPCMchannel = ps1SpuPlayADPCMchannel;
	SPU_freeze           = spuFreeze;
	SPU_playCDDAchannel  = ps1SpuPlayCDDAchannel;
	SPU_setCDvol         = ps1SpuSetCDvol;

	// Custom state save path on top of RFile (uncompressed).
	if (g_saveFuncsOverride)
		{
		SaveFuncs.open  = symbianSaveOpen;
		SaveFuncs.read  = symbianSaveRead;
		SaveFuncs.write = symbianSaveWrite;
		SaveFuncs.seek  = symbianSaveSeek;
		SaveFuncs.close = symbianSaveClose;
		}
	}

// --- Entry point ------------------------------------------------------------

// Preserve one previous log: new launch overwrites current, and valuable run
// is lost as soon as application is launched again - got burned by this before.
static void rotateLog(const TDesC &aPath, const TDesC &aPrev)
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	fs.Delete(aPrev);
	fs.Rename(aPath, aPrev);
	fs.Close();
	}

// Emulator directory must exist before first fopen: ESTLIB does not create dirs,
// and without this both log and images would fail to open silently on clean device.
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
	// E: is mounted memory card, i.e. host folder: log is visible outside emulator.
	g_log = fopen("E:\\PS1\\logs\\ps1_stage1.jsonl", "w");
	// A previous instance that never died still holds that file, and the run
	// would then write no log at all - which from outside reads exactly like a
	// run that never started.
	if (!g_log)
		g_log = fopen("E:\\PS1\\logs\\ps1_stage1_alt.jsonl", "w");
	if (!g_log)
		g_log = fopen("C:\\ps1_stage1.jsonl", "w");
	}

// Command line: [frame count] [bios] [gputest] [drc]
// aHaveArgs == EFalse means launch from phone menu - there are no arguments,
// and both CPU modes are run in succession for comparison.
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

	// Dynarec is intentionally behind flag: in EKA2L1 its result proves nothing
	// (CLAUDE.md section 6), so interpreter remains default.
	_LIT(KDrcArg, "drc");
	if (cmd.FindF(KDrcArg) != KErrNotFound)
		aDrc = ETrue;

	// On-screen frame output (Stage 2). Disabled by default because it skews
	// clean headless benchmark runs: presentation holds process for extra KScreenHoldMs.
	_LIT(KScreenArg, "screen");
	if (cmd.FindF(KScreenArg) != KErrNotFound)
		aShowScreen = ETrue;

	// Game disc loading (E:\gt.cue). Requires real BIOS, so enables
	// that as well. CheckCdrom determines region itself (US->NTSC), so PsxType is not forced.
	_LIT(KDiscArg, "disc");
	if (cmd.FindF(KDiscArg) != KErrNotFound)
		{
		aUseDisc = ETrue;
		aUseBios = ETrue;
		}

	// DSA output (direct write to screen buffer). DISABLED by default - breaks 16-bit
	// game modes; enabled by explicit "dsa" to investigate cause.
	_LIT(KDsaArgC, "dsa");
	voutSymbianSetDsa(cmd.FindF(KDsaArgC) != KErrNotFound ? 1 : 0);
	_LIT(KNoDirtyArgC, "nodirty");
	voutSymbianSetDirtyOnly(cmd.FindF(KNoDirtyArgC) != KErrNotFound ? 0 : 1);
	_LIT(KMemBenchArgC, "membench");
	if (cmd.FindF(KMemBenchArgC) != KErrNotFound)
		g_memBench = ETrue;

	// GL ES output (MBX): GPU scales and presents via eglSwapBuffers.
	_LIT(KGlArg, "gl");
	if (cmd.FindF(KGlArg) != KErrNotFound)
		voutSymbianSetGl(1);

	// State save and load for test harness.
	_LIT(KSaveStateArgC, "savestate");
	const TInt ssp = cmd.FindF(KSaveStateArgC);
	if (ssp != KErrNotFound)
		{
		TLex lex(cmd.Mid(ssp + 9));
		TInt n = 0;
		if (lex.Val(n) == KErrNone && n > 0)
			g_saveStateAt = n;
		}

	_LIT(KSaveFuncsArg, "savefuncs");
	if (cmd.FindF(KSaveFuncsArg) != KErrNotFound)
		g_saveFuncsOverride = ETrue;

	_LIT(KLoadStateArgC, "loadstate");
	const TInt lsp = cmd.FindF(KLoadStateArgC);
	if (lsp != KErrNotFound)
		{
		TLex lex(cmd.Mid(lsp + 9));
		TInt n = 0;
		if (lex.Val(n) == KErrNone && n > 0)
			g_loadStateAt = n;
		}
	}

// Rasterization self-test: submit GP0 commands directly, without BIOS and without game.
// Only way to test gpu_unai while CPU spins in HLE trap.
// Command formats are standard PlayStation GP0.
static void runGpuSelfTest()
	{
	// Display window setup via GP1 (320x240 NTSC, 15bit). Does not affect VRAM
	// rasterization (GP0 writes to VRAM independently of display registers), so
	// dump remains unchanged. Goal is for gpulib to set screen.w/h=320/240, so
	// presentation uses real scaling branch (320x240 -> 240x180 letterbox),
	// rather than fallback (screen size). GP1 = (cmd<<24)|data.
	// hrange x1=0x200,x2=0xC00 -> (x2-x1)/hdiv(8)=320; vrange y1=0x10,y2=0x100 -> 240.
	ps1GpuWriteStatus(0x06000000u | (0xC00u << 12) | 0x200u); // GP1(06) hor range
	ps1GpuWriteStatus(0x07000000u | (0x100u << 10) | 0x010u); // GP1(07) ver range
	ps1GpuWriteStatus(0x08000001u);                           // GP1(08) mode: 320x240 NTSC 15bit
	ps1GpuWriteStatus(0x05000000u);                           // GP1(05) display start = (0,0)
	ps1GpuWriteStatus(0x03000000u);                           // GP1(03) display enable

	// 0x02 - rectangle fill in VRAM: color, then (y<<16|x), then (h<<16|w).
	// Color word is 0xCCBBGGRR, so 0x000080 is red, not blue.
	const uint32_t fill[] = {
		0x02FFFFFFu,          // white, like triangle: to compare color pipelines
		(20u << 16) | 10u,    // x=10, y=20
		(60u << 16) | 100u    // w=100, h=60
	};
	for (unsigned i = 0; i < sizeof(fill) / sizeof(fill[0]); i++)
		ps1GpuWriteData(fill[i]);

	// 0x20 - untextured solid triangle: color, then three vertices.
	// White: should produce 0x7fff in BGR555, testing entire color pipeline.
	const uint32_t tri[] = {
		0x20FFFFFFu,
		(200u << 16) | 200u,
		(200u << 16) | 300u,
		(260u << 16) | 250u
	};
	for (unsigned i = 0; i < sizeof(tri) / sizeof(tri[0]); i++)
		ps1GpuWriteData(tri[i]);
	}

// Single pass: core init, N frames, shutdown. Called twice
// (interpreter, then dynarec), so anything corrupting next pass must be
// cleaned up here - hence EmuShutdown at the end.
//
// Split into setup/step/teardown because GUI shell executes frames one by one from
// active object: continuous loop would not yield to scheduler, and
// CONE framework would not dispatch key events or handle exit in time. Console
// branch recombines the three parts into runPass, so its behavior is unchanged.
static RThread g_wdThread;
static TRequestStatus g_wdStat;

// Stack: measurement at top of loop is uninformative - depth grows inside
// psxCpu->Execute (block compilation), and is already unwound by time of logging.
// Therefore free stack portion is painted once with a pattern, and remaining untouched
// part is measured: this gives all-time high-water mark rather than instantaneous value.
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
	// Stack base to exception handler: StackInfo cannot be called there.
	g_stackBase = (TUint32)info.iBase;
	// Margin for current stack frame to avoid clobbering what is currently in use.
	g_paintFrom = (TUint32 *)info.iLimit;
	g_paintTo   = (TUint32 *)(((TLinAddr)&here - 256) & ~3u);
	for (TUint32 *p = g_paintFrom; p < g_paintTo; p++)
		*p = KStackPaint;
	}

// Number of stack bytes never touched. Approaching zero means overflow.
static TInt stackHighWater()
	{
	if (!g_paintFrom)
		return -1;
	const TUint32 *p = g_paintFrom;
	while (p < g_paintTo && *p == KStackPaint)
		p++;
	return (TInt)((TUint8 *)p - (TUint8 *)g_paintFrom);
	}

// Set by shell on user exit: frame loop finishes normally.
static volatile TInt g_guiExit;

// Both live below, next to button trace, because they operate on its arrays.
static void inputTraceOpen();
static void benchLoadState(const char *aMode);

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
#ifdef PS1_BENCH
	g_spuCoreTicks = 0;
	g_spuCoreCalls = g_spuCoreActive = g_spuCoreSamples = 0;
	g_ndrcLuiEmitted = g_ndrcLuiWouldLiteral = g_ndrcLuiTwoByte = 0;
#endif
	g_cdReadTicks = 0;
	g_cdReadCount = 0;
	g_mdecDecodeTicks = 0;
	g_mdecWords = 0;
	gpuUnaiCfReset();
	gpuUnaiCfTimeReset();
#ifdef PS1_BENCH
	spuStageProfileReset();
#endif

	// Minimal equivalent of emu_core_preinit() from frontend/main.c:435.
	memset(&Config, 0, sizeof(Config));
	strcpy(Config.Bios[0], "HLE");
	Config.HLE = 1;
	Config.PsxAuto = 1;
	Config.Cpu = aUseDrc ? CPU_DYNAREC : CPU_INTERPRETER;

	// Defaults from emu_set_default_config() (frontend/main.c:108) cleared by memset.
	// CRITICAL: cycle_multiplier. 0 results in subCycleStep=0x10000*0/100=0, and
	// addCycle() (psxinterpreter.c:235) does not advance psxRegs.cycle - timers and VBlank
	// never tick, and BIOS wait loop spins forever (discovered via pc/cycle sampling:
	// pc at bfc04a48-8c, cycle frozen at 555). Remaining values are auto-sentinels (-1),
	// which core would otherwise confuse with explicit mode 0.
	// cyclemul<N> allows deviation from stock 175: higher - guest instruction costs
	// more cycles, executing fewer per frame (higher FPS, slower game logic).
	// This is a tradeoff, not an optimization.
	Config.cycle_multiplier = g_cycleMul > 0 ? g_cycleMul : CYCLE_MULT_DEFAULT;
	Config.GpuListWalking = -1;
	Config.FractionalFramerate = -1;
	Config.AlternativeFlip = -1;

	// Real BIOS enabled by second argument ("bios"). GPU is now real
	// (gpu_unai), so BIOS loads and executes (see plans/progress.md).
	if (aUseBios)
		{
		strcpy(Config.BiosDir, KBiosDir);
		// Same dump in all three slots: CheckCdrom determines region from
		// disc, and putting file only in European slot would mean BIOS
		// is silently skipped for Japanese games.
		const char *bios = g_biosFile[0] ? g_biosFile : KBiosFile;
		for (int rg = 0; rg < PSX_REGION_COUNT; rg++)
			strcpy(Config.Bios[rg], bios);
		Config.PsxRegion = PSX_REGION_EU;

		// SlowBoot=1: full boot with intro screen (SCE/PS logo). With 0 emulator
		// patches BIOS to skip animation - screen stays black until disc wait,
		// with nothing to display. Was previously 0 because GPU was a stub;
		// now enabled to see the logo.
		Config.SlowBoot = 1;

		// Without disc region auto-detection (misc.c) will not work - dump is European.
		// With disc CheckCdrom determines region below, so PsxType is not forced.
		if (!aUseDisc)
			Config.PsxType = PSX_TYPE_PAL;
		}

	// ndrc_g survives passes and the core never clears it: without this a restart
	// after switching a hack off in the menu would keep the old flag.
	ndrc_g.hacks &= ~(NDHACK_NO_STALLS | NDHACK_GTE_NO_FLAGS);

	// Dynarec compatibility hacks disabled: GT crashed right after "vsync @8008ab10"
	// (two consecutive runs, nothing else in log). This is do_vsync/vsync_hack_assemble -
	// dynarec detects "VSync: timeout" loop and embeds custom ARM code instead,
	// reading guest memory via ram_offset/ROREG. This exact branch is needed
	// by us and almost unused on Linux: there RAM sits at a fixed address
	// ("RAM is not directly mapped" in our log - ram_offset != 0). VSync-timeout
	// loop runs during track loading, which is where the crash occurs.
	// Cost of disabling is slower progress through this loop; PE2 hack is irrelevant to us.
	ndrc_g.hacks |= NDHACK_NO_COMPAT_HACKS;

	// Do NOT enable NDHACK_NO_SMC_CHECK: verified on hardware - without built-in
	// self-modifying code check BIOS does not boot at all (black screen). Invalidation
	// from C side does not replace it: BIOS writes its code via regular stores, not DMA.

	// nostalls: do not emulate R3000 pipeline stalls. Accuracy for speed -
	// compatibility drops, so only via explicit argument.
	if (g_hackNoStalls)
		ndrc_g.hacks |= NDHACK_NO_STALLS;

	// gtenf: do not compute GTE flags. Dynarec already skips them where liveness
	// analysis proved they are unread; this flag removes them everywhere.
	if (g_hackGteNoFlags)
		ndrc_g.hacks |= NDHACK_GTE_NO_FLAGS;
	logEvent("hacks", NULL, "%s: ndrc=%08x gtenf=%d cyclemul=%d", mode,
		(unsigned)ndrc_g.hacks, (int)g_hackGteNoFlags, (int)Config.cycle_multiplier);

	// Game disc (E:\gt.bin) or lack thereof. SetIsoFile before cdra_init - ISOopen
	// calls fopen(fname) as primary image, so path must point to .bin.
	SetIsoFile(aUseDisc ? (g_discPath[0] ? g_discPath : KDiscImg) : NULL);

	// Memory cards. Core does not load them on its own - frontend did that, which
	// we omit, so until now game saves silently went nowhere.
	strcpy(Config.Mcd1, "E:\\PS1\\saves\\card1.mcd");
	strcpy(Config.Mcd2, "E:\\PS1\\saves\\card2.mcd");

	if (EmuInit() == -1)
		{
		logEvent("fatal", "EMU_INIT_FAIL", "%s: EmuInit returned -1", mode);
		return KErrGeneral;
		}
	LoadMcds(Config.Mcd1, Config.Mcd2);

	// CD drive init: core calls cdra_init only in LoadPlugins, which we bypass.
	// CheckCdrom reads disc (region, boot record) as in frontend/main.c:684.
	if (aUseDisc)
		{
		const int cdInitRc = cdra_init();
		const int cdOpenRc = cdra_open();      // ISOopen(GetIsoFile) - opens image
		const int cdChkRc = CheckCdrom();
		logEvent("disc", (cdInitRc || cdOpenRc) ? "CDRA_FAIL" : NULL,
			"%s: cdra_init=%d cdra_open=%d checkcdrom=%d region=%d", mode,
			cdInitRc, cdOpenRc, cdChkRc, (int)Config.PsxRegion);
		// Stand runs keep going without a disc (BIOS shell); a game from the menu
		// cannot start, so the shell must be told instead of showing a black screen.
		if (cdOpenRc && g_guiMode && !g_cliArgs)
			{
			cdra_close();
			EmuShutdown();
			return KErrNotFound;
			}
		}

	// GPU_init() called once per process in runStage1 (like LoadPlugins in frontend);
	// here - only GPU_open() per pass (like OpenPlugins, frontend/main.c:210).
	// Empty gpuDisp: our display window backend does not take from it (vout_symbian).
	unsigned long gpuDisp = 0;
	const long gpuOpenRc = ps1GpuOpen(&gpuDisp, (char *)"PCSX", NULL);
	logEvent("gpu_open", gpuOpenRc ? "GPU_OPEN_FAIL" : NULL, "%s: open=%ld", mode, gpuOpenRc);

	// SPU_open calls SetupSound (out_current = none); redirect to our
	// driver (resets amplitude counters for this pass).
	const long spuOpenRc = SPU_open();
	ps1SndSelectOutput();
	logEvent("spu_open", spuOpenRc ? "SPU_OPEN_FAIL" : NULL, "%s: open=%ld", mode, spuOpenRc);

	// Speaker output only in interactive runs (screen): in benchmark sound
	// would skew measurements, and no window/active scheduler is present there.
	// "nosnd" argument: do not open speaker output. SPU still mixes, so
	// emulation timing and replay flow remain unchanged - only device is silent.
	if (aShowScreen && !g_noSound)
		{
		const int sndRc = ps1SndOpenOutput();
		logEvent("snd_open", sndRc ? "SND_OPEN_WARN" : NULL, "%s: rc=%d", mode, sndRc);
		}

	EmuReset();
	// Config.HLE == 0 - only reliable indication that dump was actually picked up.
	logEvent("reset", (aUseBios && Config.HLE) ? "BIOS_NOT_LOADED" : NULL,
		"%s: EmuReset ok, hle=%d type=%s", mode,
		(int)Config.HLE, Config.PsxType ? "PAL" : "NTSC");

	// Type is known only after EmuReset, and nominal frame duration
	// against which adaptive frameskip compares real duration depends on it.
	fsAdaptiveStart(g_gpuFrameskip == -1 ? (Config.PsxType ? 50 : 60) : 0);

	if (aGpuTest)
		{
		runGpuSelfTest();
		logEvent("gpu_selftest", NULL, "%s: GP0 fill + triangle submitted", mode);
		}

	// Per-frame logging is intentionally NOT done here: fflush costs more than
	// emulation itself (measured - 59 frames, 2274 ms vs 689 ms), and with per-frame
	// logging interpreter vs dynarec comparison would measure filesystem performance.
	// Single write before first Execute: if process dies on first block,
	// log must contain a trace of that rather than silence after gpu_selftest.
	flushCoreRepeat();
	logEvent("exec_begin", NULL, "%s: entering frame loop, %d frames", mode, aFrames);

	// Live presentation: window once before loop, frame - every frame inside. In GUI it
	// is already attached to CCoeControl container, so we do not create our own.
	if (aShowScreen)
		{
		const int initRc = g_guiMode ? KErrNone : voutSymbianInit();
		logEvent("screen_init", initRc ? "SHOW_FAIL" : NULL, "%s: rc=%d gui=%d", mode,
			initRc, (int)g_guiMode);
		// Direct-FB diagnostics: raw HAL buffer parameters vs WS window size.
		int fbOk = 0, xpix = 0, ypix = 0, lineB = 0, bpp = 0, firstOff = 0, winW = 0, winH = 0;
		voutSymbianFbInfo(&fbOk, &xpix, &ypix, &lineB, &bpp, &firstOff, &winW, &winH);
		logEvent("fb", NULL, "%s: ok=%d hal=%dx%d line=%dB bpp=%d first=%d win=%dx%d",
			mode, fbOk, xpix, ypix, lineB, bpp, firstOff, winW, winH);

		if (g_memBench)
			{
			char buf[256];
			voutSymbianMemBench(buf, sizeof(buf));
			logEvent("membench", NULL, "%s", buf);
			}

		if (g_sndBench)
			{
			char buf[256];
			sndSymbianWsolaBench(buf, sizeof(buf));
			logEvent("wsolabench", NULL, "%s", buf);
			}
		}

	// Input trace must be opened BEFORE loading state: rewinding replay
	// pointer reads the already loaded event array.
	inputTraceOpen();
	g_savedState = EFalse;
	benchLoadState(mode);

	// Arm watchdog for entire pass. Anonymous thread with small stack: large
	// EPOCSTACKSIZE is rejected by device (RThread::Create -> KErrTooBig, see Stage 3).
	g_wdRun = 1;
	g_wdFired = 0;
	g_wdSamples = 0;
	g_wdBeat = 0;
	g_mainThreadId = RThread().Id();   // before creating watchdog: it reads our context
	g_pass.haveWd =
		(g_wdThread.Create(KNullDesC, watchdogThread, 8192, NULL, (TAny *)g_watchdogMs) == KErrNone);
	if (g_pass.haveWd)
		{
		// Higher than main: after crash main spins in loop waiting for write,
		// and watchdog must preempt it rather than share timeslice.
		g_wdThread.SetPriority(EPriorityMore);
		g_wdThread.Logon(g_wdStat);
		g_wdThread.Resume();
		}

	// Before tStart, so the engine start-up (driver load, kernel patch) is not in the pass time.
	logClocks("start");
	g_pass.framesAtStart = frame_counter;
	// Pipeline profiling: cumulative time (ms) for each stage across entire pass.
	// NTickCount is ~1ms here; per-frame error averages out over hundreds of frames.
	g_pass.tStart = User::NTickCount();
#ifdef PS1_STAGE2_PROFILE
	s2Begin();
#endif
	// Overlay window opens along with pass, not upon menu toggle: between
	// menu and first frame lies core init (BIOS, disc image), which would
	// make first value three times lower.
	g_fpsWinTick = g_pass.tStart;
	g_fpsWinFrame = g_pass.i;
	g_fpsWinShown = voutSymbianPresents();   // counter persists across passes
#ifdef PS1_BENCH
	if (g_pmuEvt0 >= 0)
		{
		unsigned diag[5] = { 0, 0, 0, 0, 0 }, d = 0;
		const TInt rc = pmuSymbianStart(g_pmuEvt0, g_pmuEvt1, diag);
		unsigned sram = 0;
		const unsigned hello = pmuSymbianHello(&sram);
		pmuSymbianRead(&g_pmuBase[0], &g_pmuBase[1], &g_pmuBase[2], &d);
		logEvent("pmustart", rc ? "PMU_FAIL" : NULL,
			"%s: rc=%d setup=%08x hello=%08x sram=%08x diag=%08x,%d,%d,%d evt=%02x,%02x base=%u,%u,%u",
			mode, rc, diag[0], hello, sram, diag[1], (int)diag[2], (int)diag[3], (int)diag[4],
			(unsigned)g_pmuEvt0, (unsigned)g_pmuEvt1,
			g_pmuBase[0], g_pmuBase[1], g_pmuBase[2]);
		}
#endif
	return KErrNone;
	}

// Input recording and playback. Dynarec crash reproduces only after several
// minutes of manual play, and each hypothesis check costs a separate session.
// Button trace tied to FRAME NUMBER (not time) removes human from loop:
// exact same session is replayed as many times as needed.
//
// Format is intentionally not JSONL: file is parsed by device itself, and line
// parsing must be trivial. One line per button state CHANGE: <frame> <pad hex> <cycle>
// Cycle is needed to verify playback accuracy: if cycle counter diverges at
// the same frame, replay is inaccurate, and all conclusions from it are worthless.
enum TInputMode { EInputOff = 0, EInputRecord, EInputPlay };
static TInputMode g_inputMode;
_LIT(KInputPath, "E:\\PS1\\logs\\ps1_input.rec");

static RFs g_recFs;
static RFile g_recFile;
static TBool g_recOpen;
static TUint16 g_recLastPad;
static TBool g_recStarted;

// Replay kept in memory: trace is small (one change per dozens of frames),
// and reading flash every frame adds timing non-determinism.
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
	if (g_inputMode == EInputRecord && !g_recOpen) inputRecordOpen();
	else if (g_inputMode == EInputPlay && !g_playFrame) inputPlayLoad();
	}

// Starting benchmark from saved state. Button trace is tied to frame number,
// so it is valid only for guest pace it was recorded with: changing
// cyclemul shifts inputs, game fails to navigate menu and stays in it
// (measured - 495 rendered frames instead of 2220). State removes this dependency.
static void benchLoadState(const char *aMode)
	{
	if (g_loadStateAt <= 0)
		return;
	const int lr = LoadState(KBenchSavePath);
	if (lr != 0)
		{
		logEvent("loadstate", "LOAD_FAIL", "%s: frame=%d rc=%d", aMode,
			(int)g_loadStateAt, lr);
		return;
		}
	g_pass.i = g_loadStateAt;
	TInt rewound = 0;
	if (g_inputMode == EInputPlay && g_playCount > 0)
		{
		TInt lastBefore = -1;
		while (g_playNext < g_playCount && g_playFrame[g_playNext] < g_loadStateAt)
			{
			lastBefore = g_playNext;
			g_playNext++;
			rewound++;
			}
		// Button held at save time would otherwise appear released.
		if (lastBefore >= 0)
			in_keystate[0] = g_playPad[lastBefore];
		}
	logEvent("loadstate", NULL, "%s: frame=%d rc=%d rewound=%d pc=%08x cycle=%u"
		" hsync=%u base=%u start=%u", aMode, (int)g_loadStateAt, lr, (int)rewound,
		(unsigned)psxRegs.pc, (unsigned)psxRegs.cycle,
		hSyncCount, (unsigned)rcnts[3].cycle, (unsigned)rcnts[3].cycleStart);
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

// Called every frame immediately after polling keys: records changes during recording,
// OVERWRITES in_keystate[0] with trace during playback.
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
		// Cycle counter divergence at the same frame means replay
		// took a different path (most likely - different disc read timing).
		// Report once: all subsequent deviations are derivative.
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

// Single frame. EFalse = pass finished (frames exhausted, watchdog or exit).
static TBool passStep()
	{
	if (g_pass.i >= g_pass.frames || g_guiExit)
		return EFalse;
	// Session ceiling. While output is being debugged, run must self-terminate:
	// daemon is blocked on child process and does not send keepalive, so long session
	// drops WiFi and forces reconnecting to device each time.
	if (g_playLimitMs > 0 && (TInt)(User::NTickCount() - g_pass.tStart) >= g_playLimitMs)
		{
		logEvent("timelimit", NULL, "%s: %d ms reached at frame %d",
			g_pass.mode, (int)g_playLimitMs, (int)g_pass.i);
		return EFalse;
		}

	const char *mode = g_pass.mode;

	// Input: fresh button state before Execute, since core reads pad inside frame.
	// Only when window is present (screen); no keys in headless benchmark.
	TUint t0 = User::NTickCount();
	g_stage = EStagePoll;
	if (g_pass.showScreen)
		in_keystate[0] = voutSymbianPollPad();
	// Which raw axis is which on screen cannot be settled without the phone in
	// hand, so a tilt run is traced and read back off the log afterwards.
	if (g_tiltCli && voutSymbianTiltActive() && (g_pass.i % 15) == 0)
		{
		int tx = 0, ty = 0, tzx = 0, tzy = 0, tp = 0;
		const int tn = voutSymbianTiltStats(&tx, &ty, &tzx, &tzy, &tp);
		unsigned char sx = 127, sy = 127;
		voutSymbianTiltAnalog(&sx, &sy, NULL, NULL);
		logEvent("tilttrace", NULL, "f=%d n=%d x=%d y=%d dx=%d dy=%d sx=%d sy=%d pad=%04x",
			(int)g_pass.i, tn, tx, ty, tx - tzx, ty - tzy, (int)sx, (int)sy, (unsigned)tp);
		}
	inputTrace(g_pass.i);

	TUint t1 = User::NTickCount(); g_pass.tPoll += t1 - t0;
	g_stage = EStageExec;
	psxRegs.stop = 0;
	// MBX rasterizer collects primitives throughout the frame, so frame
	// boundaries are here, wrapping Execute.
	glrBeginFrame();
	const TUint32 cyc0 = psxRegs.cycle;
	psxCpu->Execute(&psxRegs);
	// First frames after state load: how many cycles guest actually ran.
	// An empty frame here looks identical to a healthy one until inspecting this.
	if (g_loadStateAt > 0 && g_pass.i < g_loadStateAt + 3)
		logEvent("loadstate", NULL, "%s: frame=%d ran=%u pc=%08x vbl=%u hsync=%u"
			" base=%u start=%u cyc=%u", mode,
			(int)g_pass.i, (unsigned)(psxRegs.cycle - cyc0), (unsigned)psxRegs.pc,
			g_vblanks, hSyncCount, (unsigned)rcnts[3].cycle,
			(unsigned)rcnts[3].cycleStart, (unsigned)psxRegs.cycle);
	TUint t2 = User::NTickCount(); g_pass.tEmu += t2 - t1;
	g_wdBeat++;
	if (gpu_unai_primstats_frame)
		gpu_unai_primstats_frame();

	if (g_wdFired)
		{
		logEvent("watchdog", "WATCHDOG",
			"%s: stalled >%dms at frame %d - aborting pass", mode, (int)g_watchdogMs, g_pass.i);
		g_pass.stalled = ETrue;
		return EFalse;
		}

	// Live frame to screen. Adds overhead (conversion+BitBlt), skewing
	// measurements - enabled only with screen flag, not in benchmark.
	if (g_pass.showScreen)
		{
		g_stage = EStageBlit;
		// Presentation skip: emulation and rasterization run every frame,
		// every (skip+1)-th reaches screen. Audio is always pumped - otherwise buffer
		// empties on skipped frames.
		if (!g_frameskip || (g_pass.i % (g_frameskip + 1)) == 0)
			{
			// When MBX rasterizer is active, it presents the frame - otherwise swap
			// would happen twice, leaving on screen whichever came
			// second, making path comparison impossible.
			if (!glrEndFrame())
				voutSymbianPresent();
			}
		TUint t3 = User::NTickCount(); g_pass.tBlit += t3 - t2;
		g_stage = EStageSnd;
		ps1SndPump();   // service audio callbacks and refill buffer
		g_pass.tSnd += User::NTickCount() - t3;
		}

	g_stage = EStageIdle;
	g_pass.i++;
#ifdef PS1_BENCH
	voutSymbianSelfInterruptTick((int)(g_pass.i - g_loadStateAt));
#endif

	// Metrics for overlay. fps - PRESENTED frames: emulation rate depends on
	// PS1 cycle multiplier and increases where screen smoothness does not, meaning
	// it measures not what is visible. ms remains emulation time per frame - frame cost,
	// and matches measurements in log. Two divisions per half-second here.
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

	// Sparse markers: 6 writes per run do not skew measurement, but show
	// progress reached when process dies without any message. In GUI pass runs
	// for hours, so step is larger: each write costs fflush to flash.
	const TInt every = g_guiMode ? 1000 : KProgressEvery;
	if (g_pass.i % every == 0)
		{
		logEvent("progress", NULL, "%s: frame %d/%d ms=%u stackUnused=%d", mode,
			g_pass.i, g_pass.frames,
			(unsigned)(User::NTickCount() - g_pass.tStart), (int)stackHighWater());
		}

	// State save at specified frame (exactly once per pass).
	if (g_saveStateAt > 0 && !g_savedState && g_pass.i == g_saveStateAt)
		{
		g_savedState = ETrue;
		const int sr = SaveState(KBenchSavePath);
		TInt saveSize = -1;
		RFs fs;
		if (fs.Connect() == KErrNone)
			{
			RFile f;
			if (f.Open(fs, KBenchSavePathDes, EFileRead | EFileShareAny) == KErrNone)
				{
				f.Size(saveSize);
				f.Close();
				}
			fs.Close();
			}
		logEvent("savestate", (sr != 0 || saveSize <= 0) ? "SAVE_FAIL" : NULL,
			"frame=%d rc=%d size=%d cycle=%u hsync=%u base=%u start=%u",
			(int)g_pass.i, sr, (int)saveSize, (unsigned)psxRegs.cycle,
			hSyncCount, (unsigned)rcnts[3].cycle, (unsigned)rcnts[3].cycleStart);
		}

	// Z2+Z3: how many frames game actually updated and in what geometry. updates is
	// counter of vout_update calls; difference between menu (0-4000) and track phases
	// shows whether latch-based presentation gain is not a harness artifact.
	// Screenshots at representative points: game menu (2D) and track (3D). Comparing
	// these two files answers why 2D is not visible while video is.
	if (g_pass.showScreen && g_cliArgs && (g_pass.i == g_dumpAt || g_pass.i == 6000))
		{
		char p[32];
		snprintf(p, sizeof(p), "E:\\PS1\\logs\\ps1_scr%d.bin", (int)g_pass.i);
		const int sr = voutSymbianScreenDump(p);
		// Under GL screen dump is empty: GL presents instead of DSA. Screenshot is taken
		// from GL itself, next frame - readPixels is possible only before swap.
		if (glrEnabled())
			{
			char gp[40];
			snprintf(gp, sizeof(gp), "E:\\PS1\\logs\\ps1_gl%d.bin", (int)g_pass.i);
			glrShot(gp);
			}
		// Write address BEFORE reading: if physical, reading will kill thread.
		logEvent("screen_dump", sr ? "SCR_FAIL" : NULL, "frame=%d rc=%d halAddr=0x%x",
			g_pass.i, sr, (unsigned)voutSymbianHalAddr());
		// And software VRAM at the same frame: only available ground truth to
		// compare against GL output. End-of-pass dump will not do -
		// scene has already changed there.
		snprintf(p, sizeof(p), "E:\\PS1\\logs\\ps1_vram%d.bin", (int)g_pass.i);
		const int vr = voutSymbianDump(p);
		logEvent("vram_dump", vr ? "DUMP_FAIL" : NULL, "frame=%d rc=%d",
			g_pass.i, vr);
		// Same frame from buffer at HAL address: whether it reached scanned buffer.
		snprintf(p, sizeof(p), "E:\\PS1\\logs\\ps1_hal%d.bin", (int)g_pass.i);
		const int hr = voutSymbianHalDump(p);
		logEvent("hal_dump", hr ? "HAL_FAIL" : NULL, "frame=%d rc=%d", g_pass.i, hr);
		// Pattern via known working path + raw dump: screen buffer layout
		// (bytes per pixel, stride) is determined later on host.
		if (g_pass.i == g_dumpAt)
			{
			int m = 0, t = 0, bad[4];
			const int rb = voutSymbianReadBack(0, &m, &t, bad);
			const int rr = voutSymbianRawDump("E:\\PS1\\logs\\ps1_raw.bin", 0);
			logEvent("fb_pattern", NULL, "rb=%d match=%d/%d bad=%d,%d want=0x%x got=0x%x raw=%d",
				rb, m, t, bad[0], bad[1], (unsigned)bad[2], (unsigned)bad[3], rr);
			if (g_fbVis)
				{
				// First green cross via bitgdi, then red via raw write,
				// 5 s each. Whatever shows on panel is the answer.
				const int v0 = voutSymbianVisTest(0);
				User::After(5000000);
				const int v1 = voutSymbianVisTest(1);
				User::After(5000000);
				logEvent("fb_vis", NULL, "bitgdi=%d raw=%d", v0, v1);
				}
			}
		}

	// Same concern as with watchdog: every logEvent is fflush to card.
	// 100 frames is once every three game seconds, causing delay on that exact frame.
	if (g_pass.showScreen && (g_pass.i % (g_guiMode ? 3000 : 100)) == 0)
		{
		int srcW = 0, srcH = 0, dstW = 0, dstH = 0, ox = 0, oy = 0;
		voutSymbianGeom(&srcW, &srcH, &dstW, &dstH, &ox, &oy);
		int fbd = 0, fbda = 0, st = 0, sx = 0, sy = 0, sw = 0, sh = 0, voff = 0, stale = 0;
		voutSymbianDbgState(&fbd, &fbda, &st, &sx, &sy, &sw, &sh, &voff, &stale);
		// gp0/dma - draw command counters: if in frozen window they
		// GROW, game is drawing while we fail to present (bug). If frozen - game
		// actually does nothing, and freeze is expected.
		// fbPages: how many DISTINCT first-pixel addresses driver returned. >1 means
		// double buffering, so writes to cached address miss the panel.
		int fbChg = 0, fbSeen = 0, fbB0 = 0, fbB1 = 0;
		voutSymbianFbPages(&fbChg, &fbSeen, &fbB0, &fbB1);
		unsigned gp0Count = g_gpulibWriteData, dmaCount = g_gpulibDmaChain;
#ifndef PS1_BENCH
		unsigned dm = 0, stCount = 0;
		voutSymbianGpuStats(&gp0Count, &dm, &dmaCount, &stCount);
#endif
		logEvent("vout_geom", NULL,
			"frame=%d upd=%d gpulibUpd=%d src=%dx%d dst=%dx%d off=%d,%d "
			"gp0=%u dma=%u fbDirty=%d fbDirtyArea=%d status=0x%x scr=%dx%d src=%d,%d "
			"voutOff=%d stale=%d fbPages=%d/%d fb0=0x%x fb1=0x%x",
			g_pass.i, voutSymbianUpdates(), voutSymbianGpulibUpdates(),
			srcW, srcH, dstW, dstH, ox, oy,
			gp0Count, dmaCount, fbd, fbda, (unsigned)st,
			sw, sh, sx, sy, voff, stale, fbChg, fbSeen, (unsigned)fbB0, (unsigned)fbB1);
		}
	return ETrue;
	}

#ifdef PS1_STAGE2_PROFILE
static TUint32 g_s2Exe[KS2Max], g_s2Sys[KS2Max];
struct TS2Row { TUint32 key; TUint32 c[4]; TUint32 tot; };
static const TInt KS2Rows = 8192;
static TS2Row g_s2Rows[KS2Rows];

static int s2CmpU32(const void *a, const void *b)
	{
	const TUint32 x = *(const TUint32 *)a, y = *(const TUint32 *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
	}

static int s2CmpRow(const void *a, const void *b)
	{
	const TUint32 x = ((const TS2Row *)a)->tot, y = ((const TS2Row *)b)->tot;
	return x > y ? -1 : (x < y ? 1 : 0);
	}

// aPk holds (bucket << 2 | window). One row per bucket with the four per-window
// counts, hottest first; whatever does not fit into aMaxRows is summed in "rest".
static void s2Hist(const char *aMode, const char *aKind, TUint32 *aPk, TInt aN,
	TUint32 aBase, TInt aShift, TInt aMaxRows)
	{
	qsort(aPk, (size_t)aN, sizeof(TUint32), s2CmpU32);
	TInt rows = 0;
	TUint32 lost[4] = { 0, 0, 0, 0 };
	for (TInt i = 0; i < aN; i++)
		{
		const TUint32 key = aPk[i] >> 2;
		const TInt w = (TInt)(aPk[i] & 3);
		if (rows == 0 || g_s2Rows[rows - 1].key != key)
			{
			if (rows == KS2Rows)
				{ lost[w]++; continue; }
			TS2Row &r = g_s2Rows[rows++];
			r.key = key; r.c[0] = r.c[1] = r.c[2] = r.c[3] = 0; r.tot = 0;
			}
		g_s2Rows[rows - 1].c[w]++;
		g_s2Rows[rows - 1].tot++;
		}
	qsort(g_s2Rows, (size_t)rows, sizeof(TS2Row), s2CmpRow);
	const TInt shown = rows < aMaxRows ? rows : aMaxRows;
	for (TInt i = shown; i < rows; i++)
		for (TInt w = 0; w < 4; w++)
			lost[w] += g_s2Rows[i].c[w];
	char buf[460];
	TInt len = 0;
	for (TInt i = 0; i < shown; i++)
		{
		const TS2Row &r = g_s2Rows[i];
		len += snprintf(buf + len, sizeof(buf) - len, "%x:%u/%u/%u/%u ",
			(unsigned)(aBase + (r.key << aShift)), (unsigned)r.c[0], (unsigned)r.c[1],
			(unsigned)r.c[2], (unsigned)r.c[3]);
		if (len > 400 || i == shown - 1)
			{
			logEvent("s2hist", NULL, "%s: kind=%s shift=%d base=%x rows=%s", aMode, aKind,
				(int)aShift, (unsigned)aBase, buf);
			len = 0;
			}
		}
	logEvent("s2hist", NULL, "%s: kind=%s end buckets=%d shown=%d samples=%d rest=%u/%u/%u/%u",
		aMode, aKind, (int)rows, (int)shown, (int)aN,
		(unsigned)lost[0], (unsigned)lost[1], (unsigned)lost[2], (unsigned)lost[3]);
	}

static void s2Report(const char *aMode)
	{
	const TInt n = g_s2N;
	TUint dyn[KS2Dyn], slow[KS2Slow];
	s2DynSnap(dyn, slow);
	const TUint freq = 32768;
	for (TInt k = 0; k < KS2Dyn; k++) dyn[k] -= g_s2DynBase[k];
	for (TInt k = 0; k < KS2Slow; k++) slow[k] -= g_s2SlowBase[k];
	logEvent("s2dyn", NULL, "%s: rcMs=%u rcBlocks=%u rcFail=%u rcUsPerBlock=%u imbCalls=%u imbMs=%u imbKB=%u "
		"links=%u htLive=%u htHit0=%u htHit1=%u htSlow=%u",
		aMode, (unsigned)((unsigned long long)dyn[0] * 1000 / freq), dyn[1], dyn[2],
		dyn[1] ? (unsigned)((unsigned long long)dyn[0] * 1000000 / freq / dyn[1]) : 0,
		dyn[8], (unsigned)((unsigned long long)dyn[9] * 1000 / freq), dyn[10] / 1024,
		dyn[7], dyn[3], dyn[4], dyn[5], dyn[6]);
	// Pass clock is FastCounter ticks, 32768 Hz; the sum over blocks is unbiased.
	logEvent("s2rc", NULL, "%s: blocks=%u repeat=%u slen=%u uniq=%u rcMs=%u passMs=%u,%u,%u,%u,%u,%u,%u,%u,%u",
		aMode, dyn[1], dyn[12], dyn[11], dyn[22 + 8 * 12], (unsigned)((unsigned long long)dyn[0] * 1000 / freq),
		(unsigned)((unsigned long long)dyn[13] * 1000 / freq), (unsigned)((unsigned long long)dyn[14] * 1000 / freq),
		(unsigned)((unsigned long long)dyn[15] * 1000 / freq), (unsigned)((unsigned long long)dyn[16] * 1000 / freq),
		(unsigned)((unsigned long long)dyn[17] * 1000 / freq), (unsigned)((unsigned long long)dyn[18] * 1000 / freq),
		(unsigned)((unsigned long long)dyn[19] * 1000 / freq), (unsigned)((unsigned long long)dyn[20] * 1000 / freq),
		(unsigned)((unsigned long long)dyn[21] * 1000 / freq));
	for (TInt b = 0; b < 8; b++)
		{
		const TUint *q = dyn + 22 + b * 12;
		if (!q[0])
			continue;
		logEvent("s2rcb", NULL, "%s: slen<%d blocks=%u slen=%u us=%u passUs=%u,%u,%u,%u,%u,%u,%u,%u,%u", aMode,
			16 << b, q[0], q[1], (unsigned)((unsigned long long)q[2] * 1000000 / freq),
			(unsigned)((unsigned long long)q[3] * 1000000 / freq), (unsigned)((unsigned long long)q[4] * 1000000 / freq),
			(unsigned)((unsigned long long)q[5] * 1000000 / freq), (unsigned)((unsigned long long)q[6] * 1000000 / freq),
			(unsigned)((unsigned long long)q[7] * 1000000 / freq), (unsigned)((unsigned long long)q[8] * 1000000 / freq),
			(unsigned)((unsigned long long)q[9] * 1000000 / freq), (unsigned)((unsigned long long)q[10] * 1000000 / freq),
			(unsigned)((unsigned long long)q[11] * 1000000 / freq));
		}
	if (ndrc_s2_d1)
		logEvent("s2slow", NULL, "%s: d1=1 classes=scratch,scratchTail,io,bios,ram,other rd=%u,%u,%u,%u,%u,%u wr=%u,%u,%u,%u,%u,%u",
			aMode, slow[0], slow[1], slow[2], slow[3], slow[4], slow[5],
			slow[6], slow[7], slow[8], slow[9], slow[10], slow[11]);
	if (g_s2Ms <= 0)
		return;
	if (n <= 0)
		{
		logEvent("s2prof", "NO_SAMPLES", "%s: ms=%d fail=%d", aMode, (int)g_s2Ms, (int)g_s2Fail);
		return;
		}
	TModuleMemoryInfo mi;
	TUint32 codeBase = 0, codeSize = 0;
	if (RProcess().GetMemoryInfo(mi) == KErrNone)
		{ codeBase = mi.iCodeBase; codeSize = mi.iCodeSize; }
	const TUint span = g_s2Tick1 - g_s2Tick0;
	logEvent("s2prof", NULL, "%s: ms=%d n=%d fail=%d drop=%d spanMs=%u rateHz=%u ctxUs=%u",
		aMode, (int)g_s2Ms, (int)n, (int)g_s2Fail, (int)g_s2Drop, span,
		span ? (unsigned)((unsigned long long)n * 1000 / span) : 0,
		(unsigned)((unsigned long long)g_s2CtxTicks * 1000000 / freq / (n + g_s2Fail + g_s2Drop)));
	logEvent("s2map", NULL, "%s: code=%x,%u tc=%x,%u anchors=%x,%x,%x", aMode,
		(unsigned)codeBase, (unsigned)codeSize, (unsigned)g_s2TcBase, (unsigned)g_s2TcSize,
		(unsigned)(TUint32)&ndrc_symbian_alloc, (unsigned)(TUint32)&ps1SpuInit,
		(unsigned)(TUint32)&voutSymbianSetDsa);

	// Coarse classes need nothing from the linker. Only the frame stage "exec" is the
	// emulation proper; the other stages are counted so the split can be checked.
	TUint stageN[5] = { 0, 0, 0, 0, 0 };
	TInt execN = 0;
	for (TInt i = 0; i < n; i++)
		{
		const TInt st = g_s2Stage[i] < 5 ? g_s2Stage[i] : 0;
		stageN[st]++;
		if (st == EStageExec) execN++;
		}
	TUint win[4][3];
	for (TInt w = 0; w < 4; w++) win[w][0] = win[w][1] = win[w][2] = 0;
	TUint sysHigh = 0;
	TInt nExe = 0, nSys = 0, ei = 0;
	for (TInt i = 0; i < n; i++)
		{
		if (g_s2Stage[i] != EStageExec) continue;
		const TInt w = (ei * 4) / execN;
		ei++;
		const TUint32 pc = g_s2Pc[i];
		if (pc - g_s2TcBase < g_s2TcSize)
			win[w][0]++;
		else if (pc - codeBase < codeSize)
			{ win[w][1]++; g_s2Exe[nExe++] = (((pc - codeBase) >> 4) << 2) | (TUint32)w; }
		else
			{
			win[w][2]++;
			if (pc >= 0x80000000u) sysHigh++;
			g_s2Sys[nSys++] = ((pc >> 8) << 2) | (TUint32)w;
			}
		}
	logEvent("s2cls", NULL, "%s: stage=idle:%u,poll:%u,exec:%u,blit:%u,snd:%u win=tc/exe/sys "
		"w0=%u/%u/%u w1=%u/%u/%u w2=%u/%u/%u w3=%u/%u/%u sysRom=%u", aMode,
		stageN[0], stageN[1], stageN[2], stageN[3], stageN[4],
		win[0][0], win[0][1], win[0][2], win[1][0], win[1][1], win[1][2],
		win[2][0], win[2][1], win[2][2], win[3][0], win[3][1], win[3][2], sysHigh);
	if (nExe) s2Hist(aMode, "exe", g_s2Exe, nExe, 0, 4, 1500);
	if (nSys) s2Hist(aMode, "sys", g_s2Sys, nSys, 0, 8, 40);
	}
#endif

// Pass summary, core cleanup. Mirror of passSetup.
static void passTeardown()
	{
	const char *mode = g_pass.mode;
	const TBool aShowScreen = g_pass.showScreen;
	g_pass.ticks = User::NTickCount() - g_pass.tStart;
	g_pass.produced = frame_counter - g_pass.framesAtStart;
#ifdef PS1_BENCH
	TUint &tPoll = g_pass.tPoll, &tEmu = g_pass.tEmu, &tBlit = g_pass.tBlit, &tSnd = g_pass.tSnd;
#endif
	const TUint aTicks = g_pass.ticks;
	const u32 aProduced = g_pass.produced;
	const TBool stalled = g_pass.stalled;

	// After the ticks are taken; the processor is still loaded, so the OPP read is the working one.
	logClocks("end");

	inputTraceClose();

	// Disarm watchdog: request exit and wait for it to finish on its own.
#ifdef PS1_STAGE2_PROFILE
	g_s2On = 0;
#endif
	g_wdRun = 0;
	if (g_pass.haveWd) { User::WaitForRequest(g_wdStat); g_wdThread.Close(); g_pass.haveWd = EFalse; }

	// pc/cycle trajectory from watchdog: shows where execution stalled.
	for (TInt s = 0; s < g_wdSamples; s++)
		logEvent("wdsample", NULL, "%s: t=%ds pc=%08x cycle=%u", mode, (int)(s * 2),
			(unsigned)g_wdPc[s], (unsigned)g_wdCyc[s]);
#ifdef PS1_STAGE2_PROFILE
	s2Report(mode);
#endif

	flushCoreRepeat();
	logEvent("pass", stalled ? "STALLED" : (aProduced ? NULL : "NO_FRAMES"),
		"%s: frames=%u ms=%u pc=%08x cycle=%u vblanks=%u fsAdvised=%d", mode,
		(unsigned)aProduced, (unsigned)aTicks,
		(unsigned)psxRegs.pc, (unsigned)psxRegs.cycle,
		g_vblanks, (int)g_fsAdvised);

	// How many GPU commands BIOS actually sent: localizes black screen - command
	// delivery (all 0) vs rasterization (non-zero, but VRAM is empty).
	unsigned gpData = 0, gpDataMem = 0, gpDma = 0, gpStatus = 0;
	voutSymbianGpuStats(&gpData, &gpDataMem, &gpDma, &gpStatus);
	logEvent("gpustats", NULL, "%s: gp0=%u dataMem=%u dmaChain=%u gp1=%u", mode,
		gpData, gpDataMem, gpDma, gpStatus);

	// Ground truth from gpulib: if also zeros here - BIOS does not draw (stuck earlier);
	// if non-zero, but VRAM is empty - problem is in gpu_unai rasterization.
	#ifdef PS1_BENCH
	logEvent("gpulibstats", NULL,
		"%s: wData=%u wDataMem=%u dmaChain=%u dmaWords=%u wStatus=%u rStatus=%u statusLast=%08x",
		mode, g_gpulibWriteData, g_gpulibWriteDataMem, g_gpulibDmaChain,
		g_gpulibDmaWords, g_gpulibWriteStatus, g_gpulibReadStatus, g_gpulibStatusLast);
	#else
	logEvent("gpulibstats", NULL, "%s: disabled in release; gpustats remains active", mode);
	#endif

	// What game asks to draw: pixels and counts per primitive class.
	// Needed to compare gpu_unai against MBX hardware rasterizer, whose
	// cost is determined by area rather than call count.
	if (gpu_unai_get_primstats)
		{
		unsigned v[29];
		for (TInt i = 0; i < 29; i++) v[i] = 0;
		gpu_unai_get_primstats(v, 29);
		logEvent("primstats", NULL,
			"%s: pxF=%u pxFT=%u pxG=%u pxGT=%u pxSpr=%u pxFill=%u"
			" nF=%u nFT=%u nG=%u nGT=%u nSpr=%u nLine=%u nFill=%u"
			" area=%u,%u,%u,%u,%u,%u blend=%u,%u,%u,%u"
			" nTex=%u texChg=%u tpChg=%u texSum=%u tpSum=%u texMax=%u", mode,
			v[0], v[1], v[2], v[3], v[4], v[5],
			v[6], v[7], v[8], v[9], v[10], v[11], v[12],
			v[13], v[14], v[15], v[16], v[17], v[18],
			v[19], v[20], v[21], v[22],
			v[23], v[24], v[25], v[26], v[27], v[28]);
		}

	if (glrEnabled())
		{
		int gv = 0, gb = 0, go = 0, gh = 0, gm = 0, ge = 0;
		glrStats(&gv, &gb, &go, &gh, &gm, &ge);
		logEvent("glr", NULL,
			"%s: lastVerts=%d lastBatches=%d over=%d texHit=%d texMiss=%d evict=%d",
			mode, gv, gb, go, gh, gm, ge);
		}

	logEvent("vramstats", NULL,
		"%s: sysToVid=%u,%upx vidToSys=%u,%upx vidToVid=%u,%upx", mode,
		gpulib_vram_stats[0], gpulib_vram_stats[1],
		gpulib_vram_stats[2], gpulib_vram_stats[3],
		gpulib_vram_stats[4], gpulib_vram_stats[5]);

	// Sound (Phase A): how many samples SPU output and peak amplitude. maxAmp>0 =
	// SPU actually generates BIOS audio; speaker output is Phase B.
	unsigned sndFeeds = 0, sndSamples = 0, sndUnder = 0; int sndMax = 0, sndStep = 0;
	ps1SndStats(&sndFeeds, &sndSamples, &sndMax, &sndUnder, &sndStep);
	// under - speaker underrun count, step - stream stretch factor
	// for actual emulation speed (65536 = 1.0, lower = emulation lagging).
	logEvent("sndstats", NULL, "%s: feeds=%u samples=%u maxAmp=%d under=%u step=%d", mode,
		sndFeeds, sndSamples, sndMax, sndUnder, sndStep);
#ifdef PS1_BENCH
	unsigned xaCalls = 0, cddaCalls = 0, cdVolCalls = 0;
	ps1SpuCdStats(&xaCalls, &cddaCalls, &cdVolCalls);
	logEvent("cdsndstats", NULL, "%s: xa=%u cdda=%u volume=%u", mode,
		xaCalls, cddaCalls, cdVolCalls);
#endif

	if (voutSymbianTiltActive())
		{
		int tx = 0, ty = 0, tzx = 0, tzy = 0, tp = 0;
		const int tn = voutSymbianTiltStats(&tx, &ty, &tzx, &tzy, &tp);
		logEvent("tiltstats", NULL, "%s: n=%d x=%d y=%d zerox=%d zeroy=%d pad=%04x",
			mode, tn, tx, ty, tzx, tzy, (unsigned)tp);
		}

	// Pipeline profile: cumulative time per stage + share of total. Shows
	// what to optimize - emulation (drc) or presentation (blit)/sound.
	{
#ifdef PS1_BENCH
	const TUint tTot = tPoll + tEmu + tBlit + tSnd;
	const TUint den = tTot ? tTot : 1;
	// gpuRaster - emu portion spent on gpu_unai rasterization (ticks->ms). Rest of
	// emu is CPU (drc) + sound generation. Shows whether to optimize CPU or GPU next.
	const unsigned freq = ndrc_symbian_fastfreq();
	const unsigned gpuRasterMs = (unsigned)(g_gpulibRenderTicks * 1000ULL / (freq ? freq : 1));
	const unsigned spuMixMs = (unsigned)(g_spuMixTicks * 1000ULL / (freq ? freq : 1));
	const unsigned cdReadMs = (unsigned)(g_cdReadTicks * 1000ULL / (freq ? freq : 1));
	const unsigned mdecMs = (unsigned)(g_mdecDecodeTicks * 1000ULL / (freq ? freq : 1));
	unsigned blitFillMs = 0, blitPresentMs = 0;
	voutSymbianBlitSplit(&blitFillMs, &blitPresentMs);
	logEvent("profile", NULL,
		"%s: total=%ums poll=%ums(%u%%) emu=%ums(%u%%) blit=%ums(%u%%) snd=%ums(%u%%) gpuRaster=%ums(%u%% of emu) spuMix=%ums mdec=%ums(%u words) cdRead=%ums(%u reads) blitFill=%ums blitPresent=%ums shown=%u frames=%u",
		mode, (unsigned)tTot,
		(unsigned)tPoll, (unsigned)(tPoll * 100 / den),
		(unsigned)tEmu,  (unsigned)(tEmu * 100 / den),
		(unsigned)tBlit, (unsigned)(tBlit * 100 / den),
		(unsigned)tSnd,  (unsigned)(tSnd * 100 / den),
		gpuRasterMs, (unsigned)(tEmu ? gpuRasterMs * 100 / tEmu : 0),
		spuMixMs, mdecMs, g_mdecWords, cdReadMs, g_cdReadCount, blitFillMs, blitPresentMs,
		voutSymbianPresents(), (unsigned)aProduced);
	const unsigned spuCoreMs = (unsigned)(g_spuCoreTicks * 1000ULL / (freq ? freq : 1));
	logEvent("spucore", NULL, "%s: doSamples=%ums calls=%u active=%u samples=%u asyncInclusive=%ums",
		mode, spuCoreMs, g_spuCoreCalls, g_spuCoreActive, g_spuCoreSamples, spuMixMs);
	if (spuStageProfileEnabled())
		{
		const char *stageNames[6] = { "dec_adpcm", "dec_noise", "adsr", "mix_plain", "mix_rvb", "reverb" };
		unsigned long long emptyTicks = 0, sumEstTicks = 0;
		unsigned int emptyCalls = 0;
		spuStageProfileCalibration(&emptyTicks, &emptyCalls);
		logEvent("spucal", NULL, "%s: emptyTicks=%u emptyCalls=%u freq=%u",
			mode, (unsigned)emptyTicks, emptyCalls, freq);
		for (unsigned int s = 0; s < 6; s++)
			{
			unsigned long long ticks = 0;
			unsigned int calls = 0, sampled = 0, samples = 0;
			if (!spuStageProfileGet(s, &ticks, &calls, &sampled, &samples) || !calls)
				continue;
			const unsigned long long overhead = (emptyCalls && sampled) ?
				(emptyTicks * sampled + emptyCalls / 2) / emptyCalls : 0;
			const unsigned long long corrected = ticks > overhead ? ticks - overhead : 0;
			const unsigned long long estimated = sampled ? (corrected * calls / sampled) : 0;
			sumEstTicks += estimated;
			logEvent("spustage", NULL,
				"%s: stage=%s calls=%u sampled=%u samples=%u rawTicks=%u rawMs=%ums estMs=%ums",
				mode, stageNames[s], calls, sampled, samples,
				(unsigned)ticks,
				(unsigned)(ticks * 1000ULL / (freq ? freq : 1)),
				(unsigned)(estimated * 1000ULL / (freq ? freq : 1)));
			}
		logEvent("spustagesum", NULL, "%s: estTotalMs=%ums freq=%u",
			mode, (unsigned)(sumEstTicks * 1000ULL / (freq ? freq : 1)), freq);
		}
	logEvent("luiemit", NULL, "%s: emitted=%u wouldLiteral=%u twoByte=%u",
		mode, g_ndrcLuiEmitted, g_ndrcLuiWouldLiteral, g_ndrcLuiTwoByte);
	if (g_pmuEvt0 >= 0)
		{
		unsigned c[3] = { 0, 0, 0 };
		unsigned d = 0;
		const TInt rc = pmuSymbianRead(&c[0], &c[1], &c[2], &d);
		const unsigned hello = pmuSymbianHello(NULL);
		// CCNT counts every 64th cycle: an undivided one wraps in ten seconds.
		logEvent("pmu", (rc || hello != 0x5a5a0001u) ? "PMU_FAIL" : NULL,
			"%s: rc=%d hello=%08x evt=%02x,%02x cyc64=%u pmn0=%u pmn1=%u frames=%u raw=%u,%u,%u direct=%u",
			mode, rc, hello, (unsigned)g_pmuEvt0, (unsigned)g_pmuEvt1,
			c[0] - g_pmuBase[0], c[1] - g_pmuBase[1], c[2] - g_pmuBase[2],
			(unsigned)aProduced, c[0], c[1], c[2], d);
		pmuSymbianStop();
		}
#endif
	unsigned rowBox = 0, rowPlain = 0, rowScale = 0, rowRgb24 = 0, rowOther = 0;
	voutSymbianRowStats(&rowBox, &rowPlain, &rowScale, &rowRgb24, &rowOther);
	unsigned rgb24Scaled = 0, rgb24B16 = 0; int rgb24SrcW = 0, rgb24DstW = 0;
	voutSymbianRgb24Stats(&rgb24Scaled, &rgb24B16, &rgb24SrcW, &rgb24DstW);
	// covered/fbOk/rowCopy separate the three things that can block direct write
	// (F9), and rgn/scr show exactly what region WS handed back when it didn't.
	int dsaCovered = 0, dsaFbOk = 0, dsaRowCopy = 0, dsaRgnCount = 0, dsaRgnX = 0,
	    dsaRgnY = 0, dsaRgnW = 0, dsaRgnH = 0, dsaScrW = 0, dsaScrH = 0, dsaTries = 0;
	voutSymbianDsaDiag(&dsaCovered, &dsaFbOk, &dsaRowCopy, &dsaRgnCount, &dsaRgnX,
		&dsaRgnY, &dsaRgnW, &dsaRgnH, &dsaScrW, &dsaScrH, &dsaTries);
	logEvent("dsa", NULL, "%s: active=%d gl=%d fbPlatform=%d pixB=%d strideB=%d rows=%u/%u/%u/%u/%u "
		"covered=%d fbOk=%d rowCopy=%d rgn=%d,%d,%d,%d,%d scr=%d,%d tries=%d rgb24scaled=%u rgb24b16=%u rgb24src=%d rgb24dst=%d", mode,
		voutSymbianDsaActive(), voutSymbianGlActive(), voutSymbianFbFromPlatform(), voutSymbianFbPixBytes(), voutSymbianFbStrideB(), rowBox, rowPlain, rowScale, rowRgb24, rowOther,
		dsaCovered, dsaFbOk, dsaRowCopy, dsaRgnCount, dsaRgnX, dsaRgnY, dsaRgnW, dsaRgnH, dsaScrW, dsaScrH, dsaTries,
		rgb24Scaled, rgb24B16, rgb24SrcW, rgb24DstW);
	{
	// Rasteriser workload by kind: without pixel counts its time cannot be
	// turned into bytes per pixel, and memory and arithmetic need opposite fixes.
	unsigned dc[12];
	Mem::FillZ(dc, sizeof(dc));
	gpuUnaiDrawStats(dc, 12);
	if (gpuUnaiCfEnabled())
		{
		unsigned routeCalls[2] = { 0, 0 }, routePixels[2] = { 0, 0 };
		unsigned rows = 0;
		for (int route = 0; route < 2; route++)
			for (unsigned cf = 0; cf < 1024; cf++)
				{
				unsigned v[7] = { 0 };
				if (!gpuUnaiCfGet(cf, route, v) || !v[0]) continue;
				logEvent("cfspan", NULL,
					"%s: cf=%03x route=%s calls=%u pixels=%u len=%u,%u,%u,%u,%u",
					mode, cf, route ? "asm" : "cpp", v[0], v[1],
					v[2], v[3], v[4], v[5], v[6]);
				routeCalls[route] += v[0];
				routePixels[route] += v[1];
				rows++;
				}
		logEvent("cfsummary", NULL, "%s: rows=%u cpp=%u,%u asm=%u,%u",
			mode, rows, routeCalls[0], routePixels[0], routeCalls[1], routePixels[1]);
		}
#ifdef PS1_BENCH
	if (gpuUnaiCfTimeEnabled())
		{
		const unsigned targets[] = { 0x000, 0x02b, 0x033, 0x04b, 0x0a3, 0x0c1 };
		const unsigned freq = ndrc_symbian_fastfreq();
		unsigned long long emptyTicks = 0, sumEstTicks = 0;
		unsigned emptyCalls = 0;
		gpuUnaiCfTimeCalibration(&emptyTicks, &emptyCalls);
		logEvent("cfcal", NULL, "%s: emptyTicks=%u emptyCalls=%u freq=%u",
			mode, (unsigned)emptyTicks, emptyCalls, freq);
		for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
			{
			unsigned long long ticks = 0;
			unsigned primitives = 0, sampled = 0;
			if (!gpuUnaiCfTimeGet(targets[i], &ticks, &primitives, &sampled) || !sampled)
				continue;
			const unsigned long long overhead = emptyCalls ?
				(emptyTicks * sampled + emptyCalls / 2) / emptyCalls : 0;
			const unsigned long long corrected = ticks > overhead ? ticks - overhead : 0;
			const unsigned long long estimated = corrected * primitives / sampled;
			sumEstTicks += estimated;
			logEvent("cfprim", NULL,
				"%s: cf=%03x prims=%u sampled=%u raw=%ums estimated=%ums",
				mode, targets[i], primitives, sampled,
				(unsigned)(ticks * 1000ULL / (freq ? freq : 1)),
				(unsigned)(estimated * 1000ULL / (freq ? freq : 1)));
			}
		logEvent("cftime", NULL, "%s: selectedEstimated=%ums freq=%u",
			mode, (unsigned)(sumEstTicks * 1000ULL / (freq ? freq : 1)), freq);
		}
#endif
	logEvent("drawstats", NULL,
		"%s: polySpans=%u polyPix=%u polyTex=%u polyLit=%u polyBlend=%u"
		" spriteCalls=%u spritePix=%u spriteTex=%u tileCalls=%u tilePix=%u polyPrims=%u polyRows=%u",
		mode, dc[0], dc[1], dc[2], dc[3], dc[8], dc[4], dc[5], dc[9], dc[6], dc[7], dc[10], dc[11]);
	}
	}

	// Hold last frame (flushing audio), then tear down window and sound.
	// In GUI holding makes no sense - user decided to exit, and window belongs
	// to container anyway (voutSymbianFinish merely detaches there).
	if (aShowScreen)
		{
		const int holdMs = g_guiMode ? 0 : KScreenHoldMs;
		ps1SndPump();
		voutSymbianFinish(holdMs);
		ps1SndCloseOutput();
		logEvent("screen", NULL, "%s: presented %u frames, held %dms", mode,
			(unsigned)aProduced, holdMs);
		}

	// Mirror of passSetup in reverse order. Core does not do this: frontend
	// closes plugins and drive in ClosePlugins, which we bypass (no .so).
	// Key call here is cdra_close: ISOopen first line returns 0 if image is already
	// open ("it's already open"), so without closing next cdra_open silently
	// FAILS to open new disc. Game after disc change received previous image with
	// mismatched track table - hence the hang on logo.
	SPU_close();
	ps1GpuClose();
	cdra_close();

	EmuShutdown();
	}

// Console branch: three parts back into continuous pass.
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

// Hardware exception (KERN-EXEC 3 and similar) terminates process instantly: summary
// block is not written, leaving only abrupt end in log. Handler manages to record
// WHAT crashed - frame stage, frame number, guest pc, and GPU counters. After
// returning from here fault would recur, so exit explicitly.
static void crashHandler(TExcType aType)
	{
	// Type - first and in single write: if handler dies later, watchdog still
	// sees that exception was delivered at all.
	g_crashType = (TInt)aType;
	g_crashStep = 1;

	// Stack snapshot above own frame: kernel placed faulting code context
	// there, including address of faulting instruction.
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

	// Published by watchdog (separate thread, RFile): stdio from handler is silent.
	// Spin rather than User::After - blocking call never returns from here.
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

// Once per process, shared by both branches.
static TBool g_coreUp;
static void coreInitOnce()
	{
	// A retried Start after a failed setup comes through here again, and GPU_init
	// and SPU_init are not meant to run twice.
	if (g_coreUp)
		return;
	g_coreUp = ETrue;
	// Full mask: handler failed last time, and narrow mask was one of
	// two reasons (second was stack overflow when no stack was left to call it).
	const TInt excRc = User::SetExceptionHandler(crashHandler,
		KExceptionAbort | KExceptionKill | KExceptionUserInterrupt | KExceptionFpe |
		KExceptionFault | KExceptionInteger | KExceptionDebug);
	logEvent("exc_handler", excRc ? "EXC_INSTALL_FAIL" : NULL, "rc=%d", (int)excRc);
	paintStack();

	bindStubPlugins();

	// Rasterizer config - MUST be BEFORE GPU_init: renderer_init copies it
	// into working struct once. GPUrearmedCallbacks (below) is called with
	// zero struct, so without this line config remains zeroed, disabling
	// not only speedups but also lighting and blending.
	gpu_unai_set_config(g_hackPixelSkip, g_hackIlace, g_gpuLighting,
		g_gpuLighting /*fast*/, g_gpuBlending);
	// Output must know about line skipping: otherwise it displays odd VRAM lines
	// that rasterizer did not update, causing static overlays to leave ghost trails.
	voutSymbianSetIlaceOut(g_hackIlace ? 1 : 0);
	logEvent("gpu_cfg", NULL, "pixskip=%d ilace=%d light=%d blend=%d",
		(int)g_hackPixelSkip, (int)g_hackIlace, (int)g_gpuLighting, (int)g_gpuBlending);

	// GPU_init() once per process: allocates VRAM and executes do_reset() in gpulib
	// (gpu.status = 0x14802000, bit 28 = DMA ready). Frontend does this in
	// LoadPlugins() (plugins.c:328); we bypass LoadPlugins (no .so), so
	// call it ourselves. Both passes (interp/drc) share single VRAM.
	const long gpuInitRc = ps1GpuInit();
	logEvent("gpu_init", gpuInitRc ? "GPU_INIT_FAIL" : NULL, "init=%ld", gpuInitRc);

	// Callbacks - strictly AFTER GPU_init: GPUinit memsets gpu.state and
	// gpu.frameskip (gpu.c:355-359) and resets frame counter back to zero.
	// Placed earlier this was silently overwritten: gpulib always saw frame_count=0,
	// while frameskip.set=0 prevented rasterization skipping from working at all.
	ps1GpuSetCallbacks(&frame_counter, &g_gpuHcnt, g_gpuFrameskip);
	// And re-apply rasterizer config immediately: GPUrearmedCallbacks internally calls
	// renderer_set_config (gpu.c:1288), which unconditionally overwrites lighting,
	// blending and ilace_force with cbs->gpu_unai fields that we do not populate. Since
	// callbacks moved AFTER GPU_init, this was silently disabling lighting and
	// blending regardless of menu settings.
	gpu_unai_apply_config();
	logEvent("gpu_cbs", NULL, "frameskip=%d", (int)g_gpuFrameskip);

	// Sound: configure dfsound before SPU_init, then initialize SPU (once per process).
	ps1SndConfigure();
	const long spuInitRc = SPU_init();
	logEvent("spu_init", spuInitRc ? "SPU_INIT_FAIL" : NULL, "init=%ld", spuInitRc);
	}

// --- GUI branch (AVKON shell, app_symbian.cpp) ------------------------------
// Same steps as console pass, but tailored for active object: Setup once,
// Step per tick, Shutdown on exit. Single pass - game from disc.

static TBool g_guiUp;
// "interp" argument from deploy: same session, but on interpreter. Splits
// the major crash root cause branch - dynarec generated code vs everything
// else (gpu_unai rasterization, core, our output).
static TBool g_guiInterp;

// Frames per configuration in GL probe; 0 = probe not requested.
static TInt g_glBench;
// MBX rasterizer instead of VRAM presentation ("glrender" argument).
static TBool g_glRender;
static TInt  g_glBgForce;
static TInt  g_glDepthProbe;
static TInt  g_glSort;
static TInt  g_glZ;
static TInt  g_glIdx;
static TInt  g_glShotBig;
static TBool g_texWhite;
static TInt g_glrSkip;
static TInt g_dummyTexSide;   // probe: bind cost against texture size

extern "C" void ps1GuiUseInterpreter(void)
	{
	g_guiInterp = ETrue;
	}

extern "C" int ps1GuiSetup(void)
	{
	g_guiMode = ETrue;
	g_guiExit = 0;
	// GL probe runs instead of pass: core is not needed at all, while window and
	// context are already initialized (AttachOutput is called before us).
	if (g_glBench > 0)
		{
		logEvent("start", NULL, "stage1 begin (glbench), frames=%d", (int)g_glBench);
		voutSymbianGlBench(g_glBench);
		g_guiUp = EFalse;
		return KErrNone;
		}
	g_watchdogMs = KPlayWatchdogMs;
	ps1SndSetPumpAo(0);   // scheduler runs CIdle between frames
	voutSymbianSetPumpAo(0);
	coreInitOnce();
	const TInt frames = g_playFrames > 0 ? g_playFrames : (g_cliArgs ? KPlayFrames : KGuiFrames);
	logEvent("start", NULL, "stage1 begin (gui), frames=%d cpu=%s input=%d", (int)frames,
		g_guiInterp ? "interp" : "drc", (int)g_inputMode);
	const TInt rc = passSetup(!g_guiInterp, frames, ETrue /*bios*/, EFalse /*gputest*/,
		ETrue /*screen*/, ETrue /*disc*/);
	// After passSetup: trace is tied to frame number, and frame counter
	// starts right here.
	inputTraceOpen();
	// A menu request for GL is not proof that EGL came up (no 3D on the C5): with
	// the software pass off and no MBX rasteriser nothing would draw at all.
	if (rc == KErrNone && !voutSymbianGlActive())
		{
		if (!g_swoffCli)
			gpu_unai_sw_off = 0;
		if (!g_glRenderCli)
			g_glRender = EFalse;
		}
	// After passSetup: window and GL context are already active, so rasterizer has
	// resources to initialize.
	if (g_glRender && rc == KErrNone && voutSymbianGlActive())
		{
		const TInt grc = glrInit();
		glrSetEnabled(grc == 0 ? 1 : 0);
		glrSetTexWhite(g_texWhite ? 1 : 0);
		glrSetSkip(g_glrSkip);
		glrSetDummyTex(g_dummyTexSide);
		glrSetBgForce(g_glBgForce);
		glrSetSort(g_glSort);
		glrSetZ(g_glZ);
		glrSetIdx(g_glIdx);
		if (g_glShotBig)
			glrShotBig();
		if (g_glDepthProbe)
			glrDepthProbe();
		logEvent("glr", grc ? "GLR_INIT_FAIL" : NULL, "init rc=%d active=%d swoff=%d",
			(int)grc, (int)glrEnabled(), (int)gpu_unai_sw_off);
		}
	g_guiUp = (rc == KErrNone);
	return rc;
	}

// Called by the shell while the menu holds the frame loop: otherwise the watchdog
// reads a long pause or import as a stall and closes the game after the menu.
extern "C" void ps1GuiHeartbeat(void)
	{
	g_wdBeat++;
	}

extern "C" int ps1GuiStep(void)
	{
	if (!g_guiUp)
		return 0;
	return passStep() ? 1 : 0;
	}

// Pass restart in same process: different disc (or different CPU branch,
// BIOS, sound, GL, DSA) requires core reinitialization. Console branch works
// exactly like this - two consecutive passes - so teardown/setup pair is proven.
// VRAM, translation cache, and SPU persist from coreInitOnce: they are per-process.
//
// Two separate calls rather than one: between them shell must re-attach
// output. passTeardown detaches window (voutSymbianFinish), so attaching BEFORE
// it was lost - game computed frames while screen retained last frame of
// previous run. Call only from CIdle tick, never from key handler: internally contains
// both sound and DSA.
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
	const TInt frames = g_playFrames > 0 ? g_playFrames : (g_cliArgs ? KPlayFrames : KGuiFrames);
	const TInt rc = passSetup(!g_guiInterp, frames, ETrue /*bios*/, EFalse /*gputest*/,
		ETrue /*screen*/, ETrue /*disc*/);
	inputTraceOpen();
	g_guiUp = (rc == KErrNone);
	logEvent("game", rc ? "RESTART_FAIL" : NULL, "restart rc=%d disc=%s", (int)rc,
		g_discPath[0] ? g_discPath : KDiscImg);
	return rc;
	}

// Closing game mid-session: image currently read by core is about to be compressed,
// and converter must both read and delete it. Pass is torn down completely - cannot
// be restored back (single VRAM and translation cache per process), so shell leaves
// only exit in menu afterwards.
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

	// VRAM snapshot to verify rasterization on host (Stage 2); 1 MB written
	// to the card on every exit, so only for stand runs.
	if (g_cliArgs)
		{
		const int dumpRc = voutSymbianDump("E:\\PS1\\logs\\ps1_vram.bin");
		logEvent("vram_dump", dumpRc ? "DUMP_FAIL" : NULL, "rc=%d", dumpRc);
		}

	ps1GpuShutdown();   // paired with ps1GpuInit: frees VRAM after snapshot
	logEvent("shutdown", NULL, "gui: frames=%u ms=%u",
		(unsigned)g_pass.produced, (unsigned)g_pass.ticks);
	}

// Shell requests pass termination (user exited).
extern "C" void ps1GuiRequestExit(void)
	{
	g_guiExit = 1;
	}

// Single JSONL stream per process: shell writes through it rather than separate file.
extern "C" void ps1Log(const char *aType, const char *aMsg)
	{
	logEvent(aType, NULL, "%s", aMsg ? aMsg : "");
	}

extern "C" void ps1LogInt(const char *aType, const char *aMsg, int aVal)
	{
	logEvent(aType, NULL, "%s=%d", aMsg ? aMsg : "", aVal);
	}

// Level 1..5 from menu -> contrast threshold. Lower threshold captures more pixels:
// measured on track 10 -> 26% of frame, 6 -> 33%, 2 -> 60%. Do NOT set upper limit:
// with it filter skipped highest-contrast edges, which are the only ones noticeable
// on screen - verified on hardware, no effect was visible.
static void applyFxaaLevel(TInt aLevel)
	{
	static const TInt KFxaaThr[6] = { 0, 10, 8, 6, 4, 2 };
	voutSymbianSetFxaa(KFxaaThr[(aLevel >= 0 && aLevel <= 5) ? aLevel : 0], 31);
	}

// Disc from ps1_settings.txt when the command line names no img. That argument
// cuts the name at the first space, so eleven of the twelve images on the card
// cannot be named on a command line at all, and without this the run silently
// falls back to KDiscImg and reports cdra_open=-1.
extern "C" void ps1SetDiscFromCfg(const char *aDisc)
	{
	if (!aDisc || !aDisc[0])
		return;
	TInt i = 0;
	while (aDisc[i] && i < (TInt)sizeof(g_discPath) - 1)
		{ g_discPath[i] = aDisc[i]; i++; }
	g_discPath[i] = 0;
	}

// Settings from menu. Called BEFORE ps1GuiSetup, i.e. before any
// core and rasterizer initialization - otherwise some will not take effect.
extern "C" void ps1CfgApply(const char *aDisc, const char *aBios, int aInterp,
	int aIlace, int aLight, int aBlend, int aPixSkip, int aGteNf,
	int aSound, int aGl, int aDirty, int aDsa, int aFps, int aGpuSkip, int aCycle,
	int aSmooth, int aFxaa, int aSwDraw, int aWsola)
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
	ps1SndSetWsola(aWsola);
	voutSymbianSetGl(aGl);
	voutSymbianSetDirtyOnly(aDirty);
	// DSA initializes in voutSymbianAttachWindow, but menu delivers config AFTER it,
	// so attachment itself must be redone - otherwise flag takes effect only on next
	// launch. Handled by ps1GuiReattach() on first tick.
	voutSymbianSetDsa(aDsa);
	g_fpsOverlay = aFps ? ETrue : EFalse;
	voutSymbianSetOverlay(aFps);
	g_gpuFrameskip = (aGpuSkip > 0 || aGpuSkip == -1) ? aGpuSkip : 0;
	if (aCycle > 0)
		g_cycleMul = aCycle;
	voutSymbianSetSmooth(aSmooth);
	applyFxaaLevel(aFxaa);
	if (!g_swoffCli)
		gpu_unai_sw_off = (aGl && !aSwDraw) ? 1 : 0;
	// Switching the software pass off is how a user asks for the MBX rasteriser.
	// Without this the menu only turned gpu_unai off and left nobody drawing at
	// all, so the picture stopped changing.
	if (!g_glRenderCli)
		g_glRender = gpu_unai_sw_off ? ETrue : EFalse;
	logEvent("cfg_bios", NULL, "bios=%s", g_biosFile[0] ? g_biosFile : KBiosFile);
	logEvent("cfg", NULL, "disc=%s interp=%d ilace=%d light=%d blend=%d "
		"pixskip=%d gtenf=%d snd=%d gl=%d dirty=%d dsa=%d fps=%d gpuskip=%d cyclemul=%d "
		"smooth=%d fxaa=%d swdraw=%d swoff=%d wsola=%d",
		g_discPath[0] ? g_discPath : KDiscImg, (int)aInterp,
		(int)aIlace, (int)aLight, (int)aBlend, (int)aPixSkip, (int)aGteNf,
		(int)aSound, (int)aGl, (int)aDirty, (int)aDsa, (int)aFps, (int)aGpuSkip,
		(int)aCycle, (int)aSmooth, (int)aFxaa, (int)aSwDraw,
		(int)gpu_unai_sw_off, (int)aWsola);
	}

// Same, but from menu opened while already in-game. Includes only settings switchable
// between frames; disc selection, CPU branch, GL, DSA, sound, and GTE hacks
// require reinitialization and remain in ps1CfgApply. Call only
// during pause (CIdle tick outside frame): translation cache flush occurs below.
extern "C" void ps1CfgApplyLive(int aIlace, int aLight, int aBlend, int aPixSkip,
	int aFps, int aGpuSkip, int aCycle, int aSmooth, int aFxaa, int aDirty,
	int aSwDraw)
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
	if (!g_swoffCli)
		gpu_unai_sw_off = (voutSymbianGlActive() && !aSwDraw) ? 1 : 0;
	if (!g_glRenderCli)
		{
		// glrInit is idempotent, and the GL context is up by now, so the
		// rasteriser can be taken up and put down between frames.
		g_glRender = gpu_unai_sw_off ? ETrue : EFalse;
		const TInt grc = g_glRender ? glrInit() : 0;
		glrSetEnabled((g_glRender && grc == 0) ? 1 : 0);
		logEvent("glr", (g_glRender && grc) ? "GLR_INIT_FAIL" : NULL,
			"live rc=%d active=%d swoff=%d", (int)grc, (int)glrEnabled(),
			(int)gpu_unai_sw_off);
		}

	if (aGpuSkip != g_gpuFrameskip)
		{
		g_gpuFrameskip = (aGpuSkip > 0 || aGpuSkip == -1) ? aGpuSkip : 0;
		// Only way to set gpu.frameskip.set is re-registering callbacks, which again
		// overwrites rasterizer config (see comment in coreInitOnce).
		ps1GpuSetCallbacks(&frame_counter, &g_gpuHcnt, g_gpuFrameskip);
		gpu_unai_apply_config();
		fsAdaptiveStart(g_gpuFrameskip == -1 ? (Config.PsxType ? 50 : 60) : 0);
		}

	if (aCycle > 0 && aCycle != g_cycleMul)
		{
		g_cycleMul = aCycle;
		Config.cycle_multiplier = g_cycleMul;
		// ApplyConfig flushes translation cache itself when multiplier changes: blocks
		// compiled for old value would otherwise retain foreign timing.
		if (psxCpu && psxCpu->ApplyConfig)
			psxCpu->ApplyConfig();
		}

	logEvent("cfg_live", NULL, "ilace=%d light=%d blend=%d pixskip=%d fps=%d "
		"gpuskip=%d cyclemul=%d smooth=%d fxaa=%d dirty=%d swdraw=%d swoff=%d",
		(int)aIlace, (int)aLight, (int)aBlend, (int)aPixSkip, (int)aFps,
		(int)aGpuSkip, (int)aCycle, (int)aSmooth, (int)aFxaa, (int)aDirty,
		(int)aSwDraw, (int)gpu_unai_sw_off);
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

	// Direct-FB diagnostics: "fbtest" argument draws test pattern directly to LCD (no
	// emulation) and holds for 8s. Stride/buffer/orientation are identified from result.
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

		// Z1: layout FROM PLATFORM alongside HAL values. If stride or base
		// differ - this is the direct explanation for skewed frame on 2026-08-01.
		int bits = 0, stride = 0, sw = 0, sh = 0, mode = -1, rot = -1, halBase = 0;
		voutSymbianScreenLayout(&bits, &stride, &sw, &sh, &mode, &rot, &halBase);
		logEvent("screen_layout", NULL,
			"bits=0x%x halBase=0x%x stride=%dB size=%dx%d mode=%d rotation=%d "
			"halLine=%dB fromPlatform=%d",
			bits, halBase, stride, sw, sh, mode, rot, lb,
			voutSymbianFbFromPlatform());

		// Z1/M-RB: pattern via known working path, readback via raw pointer.
		int m = 0, t = 0, badA[4];
		const int rb0 = voutSymbianReadBack(0, &m, &t, badA);
		logEvent("readback", rb0 ? "RB_FAIL" : NULL,
			"pass=first rc=%d match=%d/%d firstBad=(%d,%d) want=0x%x got=0x%x",
			rb0, m, t, badA[0], badA[1], badA[2], badA[3]);

		// Repeat without redraw: if matches disappeared - double buffering.
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

	// No arguments = launch from device menu (alpha 1.0): playing GT from disc -
	// large frame budget and longer watchdog to avoid interrupting game. With arguments
	// (deploy/benchmark) legacy behavior applies.
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
		// alpha 1.0: immediately play GT - disc+screen+drc+sound. No dual-mode
		// benchmark (run with arguments via deploy for benchmarking).
		rc = runPass(ETrue /*drc*/, framesToRun, ETrue /*bios*/, EFalse /*gputest*/,
			ETrue /*screen*/, ETrue /*disc*/, drcTicks, drcFrames);
		(void)interpTicks; (void)interpFrames;
		}

	// VRAM snapshot to verify rasterization on host (Stage 2). On-screen display
	// is now live inside runPass (frame by frame), rather than one-shot here.
	const int dumpRc = voutSymbianDump("E:\\PS1\\logs\\ps1_vram.bin");
	logEvent("vram_dump", dumpRc ? "DUMP_FAIL" : NULL, "rc=%d", dumpRc);

	// Paired with ps1GpuInit(): frees VRAM after snapshot (here - after dump).
	ps1GpuShutdown();

	logEvent("shutdown", NULL, NULL);
	return rc;
	}

// AVKON shell (app_symbian.cpp). Does not return until application is closed.
extern "C" int ps1AppRun(void);

// Sector read cost probe. Frame-based run is unsuitable for comparing image
// formats: cdRead is ~5 s out of 175, and day-to-day variance of hardware itself
// (same .bin produced 3950 and 5504 ms) exceeds format differences. No emulation
// here, measuring strictly read plus decompression.
// Stride 1 - sequential (block cache hits), large stride - every read in new
// block, i.e. worst-case boundary.
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

// Open image once per launch: repeated cdra_init/cdra_open on already
// opened image is rejected by ISOopen ("it's already open").
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
	g_cliArgs = (cl.Length() != 0);
	TBuf8<256> cl8;
	cl8.Copy(cl.Left(cl8.MaxLength() - 1));   // PtrZ below needs room for the terminator

	// No arguments = launch from device menu: full AVKON application. Needed
	// not only for exit and task list - MBX-EGL creates surface only
	// on window under CCoeEnv. Benchmark remains console-based (deploy arguments),
	// as neither window nor framework is needed, keeping measurements comparable.
	_LIT(KGuiArg, "gui");
	_LIT(KGlArg, "gl");
	const TBool gui = (cl.Length() == 0) || (cl.FindF(KGuiArg) != KErrNotFound);
	logEvent("boot", NULL, "stage1 starting, mode=%s args=\"%s\"",
		gui ? "gui" : "console", (const char *)cl8.PtrZ());
	// Hashing the 1.3 MB binary is for comparing stand runs, not for every player launch.
	if (cl.Length() != 0)
		logExeId();

	_LIT(KGpuSpeedArg, "gpuspeed");
	const TInt gspd = cl.FindF(KGpuSpeedArg);
	if (gspd != KErrNotFound)
		{
		TLex lex(cl.Mid(gspd + 8));
		TInt n = 0;
		if (lex.Val(n) == KErrNone)
			{
			if (n >= 10 && n <= 1000)
				{
				g_gpuSpeedPct = n;
				}
			else
				{
				logEvent("config", "BAD_GPUSPEED", "gpuspeed=%d out of range [10..1000]", (int)n);
				}
			}
		}
	logEvent("gpuspeed", NULL, "g_gpuSpeedPct=%d", (int)g_gpuSpeedPct);

	// Import probe: "conv img<file>" takes E:\PS1\games\<file> and builds
	// .pbp next to it. Testing conversion blindly via menu is impossible, whereas here
	// result can be pulled to host and verified with psxpack.py.
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
		// Console branch executes before cleanup stack creation in runStage1, and TRAP
		// without it panics with E32USER-CBase 69.
		CTrapCleanup *cs = CTrapCleanup::New();
		if (!cs)
			return KErrNoMemory;
		logEvent("conv", NULL, "probe entered");
		// Heap only: CDesCArrayFlat is a C-class, and they do not live on the stack.
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

	// Sensor probe: "accel" starts the accelerometer and logs what it delivers.
	_LIT(KAccelArg, "accel");
	if (cl.FindF(KAccelArg) != KErrNotFound)
		{
		// Console branch runs before runStage1 makes the cleanup stack; TRAP without it
		// panics with E32USER-CBase 69.
		CTrapCleanup *acs = CTrapCleanup::New();
		if (!acs)
			return KErrNoMemory;
		// The console path has no scheduler of its own, and the sensor delivers through one.
		CActiveScheduler *sched = NULL;
		if (!CActiveScheduler::Current())
			{
			sched = new CActiveScheduler;
			if (sched) CActiveScheduler::Install(sched);
			}
		const TInt ar = accelStart();
		logEvent("accel", ar == KErrNone ? NULL : "ACCEL_FAIL", "start=%d", (int)ar);
		if (ar == KErrNone)
			{
			for (TInt i = 0; i < 40; i++)
				{
				TInt x = 0, y = 0, z = 0;
				const TInt n = accelRead(&x, &y, &z);
				logEvent("accel", NULL, "n=%d x=%d y=%d z=%d", (int)n, (int)x, (int)y, (int)z);
				// The sensor delivers through the active scheduler, so it has to run.
				TInt e; while (CActiveScheduler::RunIfReady(e, CActive::EPriorityIdle)) { }
				User::After(250000);
				}
			accelStop();
			}
		if (sched) { CActiveScheduler::Install(NULL); delete sched; }
		delete acs;
		return KErrNone;
		}

	// "cdbench imgX cdbench imgY ..." is invalid: single image per launch, path from img.
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
		// Output flags set before framework start: EikStart does not return while
		// app is alive, and window is attached inside it.
		if (cl.FindF(KGlArg) != KErrNotFound)
			voutSymbianSetGl(1);
		// Tilt probe: the menu switch is the normal route, this only presets it.
		// Switching the sensor on here would panic - the cleanup stack belongs to
		// the framework and does not exist yet.
		_LIT(KTiltArg, "tiltcli");
		if (cl.FindF(KTiltArg) != KErrNotFound)
			g_tiltCli = 1;
		// GL overhead probe instead of game: same primitive count per frame
		// as Gran Turismo, but without emulation. Enables GL itself - nothing to
		// measure without it. "glbenchN" sets frame count per configuration.
		// MBX rasterizer. Enables GL itself, as there is nothing to draw with without context.
		// At this step it renders ALONGSIDE gpu_unai rather than replacing it:
		// presents GL, but VRAM remains software-computed, so clearing
		// flag to view same scene via legacy path is possible anytime.
		_LIT(KGlrArg, "glrender");
		if (cl.FindF(KGlrArg) != KErrNotFound)
			{
			g_glRender = ETrue;
			g_glRenderCli = 1;
			voutSymbianSetGl(1);
			}
		// "glrskipN" - probe: skip primitive class (1 polygons, 2
		// rectangles, 4 fill); bitmask combined.
		_LIT(KDummyTexArg, "dummytex");
		const TInt dtp = cl.FindF(KDummyTexArg);
		if (dtp != KErrNotFound)
			{
			TLex lex(cl.Mid(dtp + 8));
			TInt n = 0;
			g_dummyTexSide = (lex.Val(n) == KErrNone) ? n : 0;
			}
		_LIT(KGlrSkipArg, "glrskip");
		const TInt gskp = cl.FindF(KGlrSkipArg);
		if (gskp != KErrNotFound)
			{
			TLex lex(cl.Mid(gskp + 7));
			TInt n = 0;
			g_glrSkip = (lex.Val(n) == KErrNone) ? n : 0;
			}
		// "texwhite" - probe: textures without vertex color modulation.
		// "glbgforce" - probe: lay the VRAM picture under every rasterised frame.
		// "gldepthprobe" - ask the driver, once, whether a later primitive really
		// does cover an earlier one under our projection.
		// "glsort" - regroup opaque primitives by texture; off until the white menu
		// is understood. Also settable from the marker file, which is the only way
		// in when the emulator is started from its icon.
		// Halves of the same experiment, so the one that broke the picture can be
		// named: "glz" is the depth in the vertex, "glidx" the indexed draw calls.
		_LIT(KGlZArg, "glz");
		if (cl.FindF(KGlZArg) != KErrNotFound)
			g_glZ = 1;
		_LIT(KGlIdxArg, "glidx");
		if (cl.FindF(KGlIdxArg) != KErrNotFound)
			g_glIdx = 1;
		_LIT(KGlShotBigArg, "glshotbig");
		if (cl.FindF(KGlShotBigArg) != KErrNotFound)
			g_glShotBig = 1;
		_LIT(KGlSortArg, "glsort");
		if (cl.FindF(KGlSortArg) != KErrNotFound)
			g_glSort = 1;
		_LIT(KGlDepthArg, "gldepthprobe");
		if (cl.FindF(KGlDepthArg) != KErrNotFound)
			g_glDepthProbe = 1;
		_LIT(KGlBgArg, "glbgforce");
		if (cl.FindF(KGlBgArg) != KErrNotFound)
			g_glBgForce = 1;
		_LIT(KTexWhiteArg, "texwhite");
		if (cl.FindF(KTexWhiteArg) != KErrNotFound)
			g_texWhite = ETrue;
		// "swoff" disables software pass: scene is rendered exclusively by MBX. Meaningless
		// on its own - only in conjunction with "glrender".
		_LIT(KSwOffArg, "swoff");
		if (cl.FindF(KSwOffArg) != KErrNotFound)
			{ gpu_unai_sw_off = 1; g_swoffCli = 1; }
		_LIT(KGlBenchArg, "glbench");
		const TInt gbp = cl.FindF(KGlBenchArg);
		if (gbp != KErrNotFound)
			{
			TLex lex(cl.Mid(gbp + 7));
			TInt n = 0;
			g_glBench = (lex.Val(n) == KErrNone && n > 0) ? n : 200;
			voutSymbianSetGl(1);
			}
		// "dsa"/"rowcopy" are MANDATORY here: parseCommandLine parses them only in
		// console branch, while GUI window binds beforehand - flag would arrive
		// after DSA needed to initialize, silently falling back to windowed
		// path (log shows dsa active=0 when dsa was specified).
		// Latch-based presentation enabled by default; "nodirty" disables it.
		_LIT(KNoDirtyArg, "nodirty");
		voutSymbianSetDirtyOnly(cl.FindF(KNoDirtyArg) != KErrNotFound ? 0 : 1);
		// direct-FB is correct (root cause was Update() without region) and enabled
		// by default in menu. Kept disabled here and enabled by explicit "dsa":
		// benchmark must remain comparable to previously measured figures.
		_LIT(KDsaArgG, "dsa");
		voutSymbianSetDsa(cl.FindF(KDsaArgG) != KErrNotFound ? 1 : 0);
		// Visual A/B of output paths at frame 3300 (requires manual inspection).
		_LIT(KFbVisArg, "fbvis");
		g_fbVis = (cl.FindF(KFbVisArg) != KErrNotFound);
		// Metrics overlay. Disabled by default in benchmark: draws over frame.
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
		// Disc image for run: img<file> or img"file with spaces" under E:\PS1\games.
		// Menu does not reach here (ps1CfgApply is not called in console branch), and
		// comparing image formats with identical input trace is impossible otherwise.
		_LIT(KImgArg, "img");
		const TInt imp = cl.FindF(KImgArg);
		if (imp != KErrNotFound)
			{
			TPtrC nm = cl.Mid(imp + 3);
			if (nm.Length() && nm[0] == '"')
				{
				nm.Set(nm.Mid(1));
				const TInt end = nm.Locate('"');
				if (end >= 0) nm.Set(nm.Left(end));
				}
			else
				{
				const TInt sp = nm.Locate(' ');
				if (sp >= 0) nm.Set(nm.Left(sp));
				}
			TBuf<256> p(_L("E:\\PS1\\games\\"));
			p.Append(nm.Left(p.MaxLength() - p.Length()));
			TBuf8<256> p8;
			p8.Copy(p);
			const TInt n = Min(p8.Length(), (TInt)sizeof(g_discPath) - 1);
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
		// Benchmark probes: no pipeline stall emulation / no screen Update().
		_LIT(KNoStallsArg, "nostalls");
		g_hackNoStalls = (cl.FindF(KNoStallsArg) != KErrNotFound);
		_LIT(KSpanOffArg, "spanoff");
		{
		const TInt sop = cl.FindF(KSpanOffArg);
		TInt sov = 0;
		if (sop != KErrNotFound)
			{
			TLex lex(cl.Mid(sop + 7));
			TInt n = 0;
			sov = (lex.Val(n) == KErrNone && n > 0) ? n : 1;
			}
		gpuUnaiSetSpanOff(sov);
		}
#ifdef PS1_BENCH
		// Every texel read from the page base: the picture breaks, but the miss
		// counter then says how much of the cost is the texel fetch itself.
		_LIT(KTexFlatArg, "texflat");
		gpuUnaiSetTexFlat(cl.FindF(KTexFlatArg) != KErrNotFound ? 1 : 0);
		_LIT(KDstFlatArg, "dstflat");
		gpuUnaiSetDstFlat(cl.FindF(KDstFlatArg) != KErrNotFound ? 1 : 0);
#endif
		_LIT(KBlitCArg, "blitc");
		voutSymbianSetBlitC(cl.FindF(KBlitCArg) != KErrNotFound ? 1 : 0);
		_LIT(KNoUpdateArg, "noupdate");
		voutSymbianSetNoUpdate(cl.FindF(KNoUpdateArg) != KErrNotFound ? 1 : 0);
#ifdef PS1_BENCH
		// "pmu0b07": two ARM1136 event numbers as two hex digits each, the first
		// going to PMN0. 0x0b is a D-cache miss, 0x07 an executed instruction.
		_LIT(KPmuArg, "pmu");
		{
		const TInt pp = cl.FindF(KPmuArg);
		if (pp != KErrNotFound)
			{
			TPtrC pmuHex = cl.Mid(pp + 3);
			TLex lex(pmuHex.Left(4));
			TUint v = 0;
			if (lex.Val(v, EHex) == KErrNone)
				{ g_pmuEvt0 = (TInt)((v >> 8) & 0xff); g_pmuEvt1 = (TInt)(v & 0xff); }
			}
		}
#ifdef PS1_STAGE2_PROFILE
		// "pcprof<ms>": sample the guest PC every <ms> ms (3 if omitted). "s2d1": count
		// slow-path memory accesses by address class; heavy, so never together with pcprof
		// when the shares are what is being measured.
		_LIT(KPcProfArg, "pcprof");
		{
		const TInt pcp = cl.FindF(KPcProfArg);
		if (pcp != KErrNotFound)
			{
			TLex lex(cl.Mid(pcp + 6));
			TInt ms = 0;
			g_s2Ms = (lex.Val(ms) == KErrNone && ms > 0) ? ms : 3;
			}
		}
		_LIT(KS2D1Arg, "s2d1");
		if (cl.FindF(KS2D1Arg) != KErrNotFound)
			ndrc_s2_d1 = 1;
#endif
		_LIT(KMemBenchArg, "membench");
		if (cl.FindF(KMemBenchArg) != KErrNotFound)
			g_memBench = ETrue;
		_LIT(KSndBenchArg, "sndbench");
		if (cl.FindF(KSndBenchArg) != KErrNotFound)
			g_sndBench = ETrue;
		// Raw PCM dump of what actually reaches the speaker (see SndDumpFrame in
		// snd_symbian.cpp), for offline pitch/artifact comparison with and without wsola.
		_LIT(KSndDumpArg, "snddump");
		if (cl.FindF(KSndDumpArg) != KErrNotFound)
			ps1SndSetDump(1);
		// selfint<N>: go to the background at presented frame N and come back 90
		// frames later. The only way to interrupt a run: while a full-screen DSA
		// run holds the screen the phone answers nothing on the network, so the
		// interruption cannot come from the PC.
		_LIT(KSelfIntArg, "selfint");
		const TInt sip = cl.FindF(KSelfIntArg);
		if (sip != KErrNotFound)
			{
			TLex lex(cl.Mid(sip + 7));
			TInt n = 0;
			if (lex.Val(n) == KErrNone && n > 0)
				voutSymbianSetSelfInterrupt(n, 0);
			}
#endif
		_LIT(KNoSmoothArg, "nosmooth");
		voutSymbianSetSmooth(cl.FindF(KNoSmoothArg) != KErrNotFound ? 0 : 1);
		_LIT(KDsaNoResumeArg, "dsanoresume");
		voutSymbianSetDsaNoResume(cl.FindF(KDsaNoResumeArg) != KErrNotFound ? 1 : 0);
		// A command line does not apply the settings file, so the menu switch for
		// pitch preservation cannot be measured on the bench without its own argument.
		_LIT(KWsolaArg, "wsola");
		if (cl.FindF(KWsolaArg) != KErrNotFound)
			ps1SndSetWsola(1);
		_LIT(KKeyProbeArg, "keyprobe");
		if (cl.FindF(KKeyProbeArg) != KErrNotFound)
			g_keyProbeCli = 1;
		// Antialiasing prototype: "fxaa" uses lower threshold 3, "fxaa6" - six.
		// "fxmax14" sets upper threshold to avoid blurring high-contrast text. Disabled by default.
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
		// Stand arguments stay in the release build: the acceptance check dumps
		// VRAM at a frame number, and the C5 only runs the release flavour.
		_LIT(KDumpAtArg, "dumpat");
		const TInt dap = cl.FindF(KDumpAtArg);
		if (dap != KErrNotFound)
			{
			TLex lex(cl.Mid(dap + 6));
			TInt n = 0;
			if (lex.Val(n) == KErrNone && n > 0)
				g_dumpAt = n;
			}
		// PRESENTATION skip (frame rendered and discarded) - for comparison with gpuskip.
		_LIT(KShowSkipArg, "showskip");
		const TInt ssp = cl.FindF(KShowSkipArg);
		if (ssp != KErrNotFound)
			{
			TLex lex(cl.Mid(ssp + 8));
			TInt n = 0;
			g_frameskip = (lex.Val(n) == KErrNone && n > 0) ? n : 1;
			}
		// Rasterization skip in gpulib: "gpuskip1" - skip rendering every other frame.
		_LIT(KGpuSkipArg, "gpuskip");
		const TInt gsp = cl.FindF(KGpuSkipArg);
		if (gsp != KErrNotFound)
			{
			TLex lex(cl.Mid(gsp + 7));
			TInt n = 0;
			g_gpuFrameskip = (lex.Val(n) == KErrNone && (n > 0 || n == -1)) ? n : 1;
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
		_LIT(KSaveStateArg, "savestate");
		const TInt ssv = cl.FindF(KSaveStateArg);
		if (ssv != KErrNotFound)
			{
			TLex lex(cl.Mid(ssv + 9));
			TInt n = 0;
			if (lex.Val(n) == KErrNone && n > 0)
				g_saveStateAt = n;
			}
		// MANDATORY here: parseCommandLine parses this only in console branch,
		// while test runs execute in GUI, so flag never reached it and benchmark
		// silently measured legacy path via zlib.
		_LIT(KSaveFuncsArgG, "savefuncs");
		if (cl.FindF(KSaveFuncsArgG) != KErrNotFound)
			g_saveFuncsOverride = ETrue;
		_LIT(KBlkLoadArg, "blkload");
		if (cl.FindF(KBlkLoadArg) != KErrNotFound)
			ndrc_skip_load_blocks = 0;
		_LIT(KLoadStateArg, "loadstate");
		const TInt lsp = cl.FindF(KLoadStateArg);
		if (lsp != KErrNotFound)
			{
			TLex lex(cl.Mid(lsp + 9));
			TInt n = 0;
			if (lex.Val(n) == KErrNone && n > 0)
				g_loadStateAt = n;
			}

		// Same selection via marker file, since launch from menu has no arguments.
		// Long session via daemon is not an option: while waiting for child process,
		// its keepalive stalls, WiFi idles and connection drops.
		// By content rather than existence: daemon can write files but not delete them,
		// so toggling mode would otherwise work only in one direction.
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
				// Same marker controls input trace: "rec" records, "play" replays.
				_LIT8(KRec, "rec");
				_LIT8(KPlay, "play");
				if (m.Find(KPlay) != KErrNotFound)     g_inputMode = EInputPlay;
				else if (m.Find(KRec) != KErrNotFound) g_inputMode = EInputRecord;
				_LIT8(KGlSort, "glsort");
				if (m.Find(KGlSort) != KErrNotFound)
					g_glSort = 1;
				_LIT8(KGlZ, "glz");
				if (m.Find(KGlZ) != KErrNotFound)
					g_glZ = 1;
				_LIT8(KGlIdx, "glidx");
				if (m.Find(KGlIdx) != KErrNotFound)
					g_glIdx = 1;
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
