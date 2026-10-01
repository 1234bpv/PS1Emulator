// Audio for Symbian: glue between core and dfsound (portable SPU branch).
//
// Separate file intentionally: libpcsxcore/plugins.h declares SPUinit, SPUopen, etc. as
// TYPES (typedefs), while in dfsound they are functions with the same names. Compiling both
// in the same translation unit is impossible (typedef/function conflict), so plugins.h is NOT
// included here, and unique ps1Spu* wrapper functions are exposed externally.
//
// Phase B: out-driver outputs PCM to speaker via CMdaAudioOutputStream. API
// is asynchronous (requires CActiveScheduler); frame loop is synchronous, so
// active objects are serviced non-blocking via CActiveScheduler::RunIfReady.

#include <e32base.h>
#include <f32file.h>
#include <mda/common/audio.h>
#include <mdaaudiooutputstream.h>

#include <stdio.h>
#include "compat_stdio.h"

extern "C" {
// Genuine dfsound exports (spu.c/registers.c/dma.c).
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
struct xa_decode;
void SPUplayADPCMchannel(struct xa_decode *, unsigned int, int);
int SPUplayCDDAchannel(short *, int, unsigned int, int);
void SPUsetCDvol(unsigned char, unsigned char, unsigned char, unsigned char, unsigned int);

#include "../../pcsx_rearmed/plugins/dfsound/spu_config.h"   // SPUConfig, spu_config
#include "../../pcsx_rearmed/plugins/dfsound/out.h"          // out_driver, out_current
}

// --- Speaker output: CMdaAudioOutputStream + ring buffer --------------------
// dfsound outputs 16-bit stereo PCM @44100 EMULATED time, while the hardware consumes
// 44100 per REAL second. When emulation falls behind, the difference causes buffer underruns
// and gaps, so Feed() resamples the stream to the actual pace (see UpdateRate).
// Pump() services active objects (RunIfReady) every frame and refills buffers while
// in-flight count is below KSndBufs.
static const TInt KSndRate  = 44100;
static const TInt KSndRing  = 128 * 1024;      // 0.74s at 44100 stereo 16-bit
static const TInt KSndChunk = 4 * 1024;        // ~23ms per WriteL
static const TInt KSndBufs  = 4;               // in-flight buffers: ~93ms headroom
static const TInt KSndFill  = 3 * KSndChunk;   // target ring fill level (~70ms)

// WSOLA time-scale constants (see CSymSnd::WsolaFeed below). A full two-channel
// search over a ~20ms window/range costs 4964-5006ms of CPU per second of audio,
// measured on N95 8GB - twice what the CPU has. Halving the window, correlating one
// channel, and the two-stage search in WsolaBestOffset are what make it affordable.
static const TInt KWsolaHop    = 220;              // synthesis/analysis half-window, frames (~5ms)
static const TInt KWsolaWin    = KWsolaHop * 2;    // full window cut per hop, frames (~10ms)
static const TInt KWsolaRange  = 441;              // search span, frames (~10ms)
static const TInt KWsolaBufCap = 4096;             // input staging capacity, frames

extern "C" unsigned int ndrc_symbian_fastcount(void);
extern "C" unsigned int ndrc_symbian_fastfreq(void);
extern "C" void ps1LogFmt(const char *aType, const char *aFmt, ...);

// Number of times the device had no data to refill, i.e. audible underruns.
static unsigned g_sndUnder;

// Stepped volume controlled from shell (* and # keys). We regulate STREAM
// volume rather than spu_config.iVolume: scaling in dfsound mixer costs
// CPU and impairs dynamic range, whereas here the audio subsystem handles it.
static const TInt KVolSteps   = 10;
static const TInt KVolDefault = 2;          // at full volume the device speaker overloads
static TInt g_volStep = KVolDefault;

// Time-scale algorithm: EFalse (default) keeps the linear stretch below, which is
// simplest but bends pitch by exactly the stretch ratio. ETrue switches to WSOLA,
// which resamples at the natural rate and hides the same time gap in overlap-add
// instead, at extra CPU cost. Set from the menu (restart-only, see ps1SndSetWsola);
// read into a new CSymSnd's iWsola at construction.
static TBool g_wsola;

// Two-stage single-channel correlation search used by WSOLA below: a coarse pass
// over every 4th offset across [0, aRange), then a refinement at full resolution
// within +-4 of the best coarse offset. Left channel only (stride 2) - correlating
// both channels at full resolution is the naive search sndSymbianWsolaBench measures,
// and it is twice too slow on its own. Samples are shifted right by 6 bits before
// multiplying so a KWsolaHop-long 32-bit accumulation cannot overflow.
static TInt WsolaBestOffset(const TInt16 *aTmpl, const TInt16 *aSearch, TInt aLen, TInt aRange)
	{
	TInt bestOff = 0;
	TInt32 bestSum = 0;
	TBool have = EFalse;
	for (TInt off = 0; off < aRange; off += 4)
		{
		const TInt16 *cand = aSearch + off * 2;
		TInt32 sum = 0;
		for (TInt i = 0; i < aLen; i++)
			sum += (TInt32)(aTmpl[i * 2] >> 6) * (TInt32)(cand[i * 2] >> 6);
		if (!have || sum > bestSum)
			{ bestSum = sum; bestOff = off; have = ETrue; }
		}
	TInt lo = bestOff - 4; if (lo < 0) lo = 0;
	TInt hi = bestOff + 4; if (hi > aRange - 1) hi = aRange - 1;
	for (TInt off = lo; off <= hi; off++)
		{
		if ((off & 3) == 0)
			continue;              // already scored in the coarse pass
		const TInt16 *cand = aSearch + off * 2;
		TInt32 sum = 0;
		for (TInt i = 0; i < aLen; i++)
			sum += (TInt32)(aTmpl[i * 2] >> 6) * (TInt32)(cand[i * 2] >> 6);
		if (sum > bestSum)
			{ bestSum = sum; bestOff = off; }
		}
	return bestOff;
	}

class CSymSnd : public CBase, public MMdaAudioOutputStreamCallback
	{
public:
	static CSymSnd *NewL();
	~CSymSnd();
	void Feed(const void *aBuf, TInt aBytes);
	void Pump();
	TBool WaitOpen(TInt aTimeoutMs);
	void ApplyVolume();
	void ResetRate();
	TInt Step() const { return iStep; }
private:
	void ConstructL();
	void Reopen();
	void Refill();
	void PushFrame(TInt aL, TInt aR);
	void UpdateRate(TInt aInFrames);
	void WsolaFeed(const TInt16 *aSrc, TInt aFrames);
	void MaoscOpenComplete(TInt aError);
	void MaoscBufferCopied(TInt aError, const TDesC8 &aBuffer);
	void MaoscPlayComplete(TInt aError);

	CMdaAudioOutputStream *iStream;
	TBool iOpen;
	TInt  iOpenErr;
	TUint8 *iRing;
	TInt iCap, iHead, iTail;   // ring: iHead - write, iTail - read
	RBuf8 iSend[KSndBufs];     // submitted to WriteL (live until MaoscBufferCopied)
	TInt iSendNext;            // next free buffer in circular rotation
	TInt iInFlight;
	// Rate resampling. iStep - input samples per single output sample
	// in 16.16 format; less than 1.0 indicates stretching. iPos - fractional position
	// in current input block, iPrev* - last sample of previous block (interpolation
	// must be continuous across block boundaries).
	TInt iStep, iPos;
	TInt iPrevL, iPrevR;
	TInt iRateFrames;          // input frames since start of measurement window
	TUint iRateT0;
	// Stream lost to a higher-priority client (call, alarm) or failed to open: Pump
	// reopens it, at most once a second while the device is still taken.
	TBool iFailed;
	TUint iRetryT0;
	// WSOLA state (used instead of iPos/iPrev* above when iWsola is set). Incoming
	// frames accumulate in iWsBuf until a full search window is available, then hops
	// are cut off the front and the remainder compacted down, so index 0 always holds
	// the next nominal analysis position. iWsAnaFrac carries the fractional part of
	// the analysis hop (KWsolaHop*iStep, 16.16) the same way iPos carries fractional
	// position above. iWsTail holds the second half of the last emitted window, kept
	// for the next hop's overlap-add and waveform-similarity search.
	TBool iWsola;
	TInt16 *iWsBuf;
	TInt iWsLen;
	TInt iWsAnaFrac;
	TBool iWsPrimed;
	TInt16 iWsTail[KWsolaHop * 2];
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
	iWsola = g_wsola;
	iWsBuf = (TInt16 *)User::AllocL(KWsolaBufCap * 2 * sizeof(TInt16));
	iRateT0 = ndrc_symbian_fastcount();
	iStream = CMdaAudioOutputStream::NewL(*this, EMdaPriorityNormal);
	iStream->Open(NULL);   // asynchronous -> MaoscOpenComplete
	}

CSymSnd::~CSymSnd()
	{
	if (iStream) { iStream->Stop(); delete iStream; }
	for (TInt i = 0; i < KSndBufs; i++)
		iSend[i].Close();
	User::Free(iRing);
	User::Free(iWsBuf);
	}

void CSymSnd::MaoscOpenComplete(TInt aError)
	{
	iOpenErr = aError;
	if (aError != KErrNone)
		{
		iFailed = ETrue;
		return;
		}
	TRAPD(propErr, iStream->SetAudioPropertiesL(
		TMdaAudioDataSettings::ESampleRate44100Hz,
		TMdaAudioDataSettings::EChannelsStereo));
	if (propErr != KErrNone)
		ps1LogFmt("snd", "SetAudioProperties failed err=%d", (int)propErr);
	iOpen = ETrue;
	// The rate measurement window starts when device begins consuming data, not
	// at construction: before opening there is no consumer and measurement would be bogus.
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

// Only these mean the device went to someone else. KErrUnderflow is the ordinary end
// of data (a menu pause, a slow frame): the stream resumes on the next WriteL, and
// reopening on it would only add a glitch.
static TBool deviceTaken(TInt aError)
	{
	return aError == KErrInUse || aError == KErrDied || aError == KErrAccessDenied;
	}

void CSymSnd::MaoscBufferCopied(TInt aError, const TDesC8 & /*aBuffer*/)
	{
	if (iInFlight > 0)
		iInFlight--;
	if (deviceTaken(aError))
		iFailed = ETrue;
	Refill();
	}

void CSymSnd::MaoscPlayComplete(TInt aError)
	{
	if (deviceTaken(aError))
		iFailed = ETrue;
	}

void CSymSnd::Reopen()
	{
	const TUint now = ndrc_symbian_fastcount();
	const TUint freq = ndrc_symbian_fastfreq();
	if (freq && now - iRetryT0 < freq)
		return;
	iRetryT0 = now;
	ps1LogFmt("snd", "stream lost, reopening err=%d", (int)iOpenErr);
	iStream->Stop();
	iOpen = EFalse;
	iInFlight = 0;
	iFailed = EFalse;
	iOpenErr = KErrNone;
	iStream->Open(NULL);   // asynchronous -> MaoscOpenComplete
	}

// After a menu pause the measurement window holds the pause itself, which would read
// as a near-zero emulation speed and stretch the stream fourfold; start over instead.
void CSymSnd::ResetRate()
	{
	iRateFrames = 0;
	iRateT0 = ndrc_symbian_fastcount();
	iStep = 1 << 16;
	iPos = 0;
	iHead = iTail = 0;
	iWsLen = 0;
	iWsAnaFrac = 0;
	iWsPrimed = EFalse;
	}

#ifdef PS1_BENCH
// Bench-only raw PCM dump of the samples actually pushed to the playback ring, i.e.
// after whichever time-scale path (linear resample or WSOLA) the run selected, so a
// dump made with and without "wsola" is directly comparable pitch-for-pitch on a PC.
// Rate and layout match the output stream (KSndRate, 16-bit signed stereo). Capped to
// a few seconds so the run cannot fill the card, and buffered through RFile::Write in
// KSndDumpBufFrames chunks so posting one sample at a time does not tax the frame budget.

static const TInt KSndDumpSecs      = 6;
static const TInt KSndDumpMaxFrames = KSndDumpSecs * KSndRate;
static const TInt KSndDumpBufFrames = KSndChunk / 4;

static TBool g_sndDumpWanted;
static TBool g_sndDumpOpen;
static TBool g_sndDumpDone;
static RFs   g_sndDumpFs;
static RFile g_sndDumpFile;
static TInt16 g_sndDumpBuf[KSndDumpBufFrames * 2];
static TInt  g_sndDumpBufLen;
static TInt  g_sndDumpFramesLeft;

_LIT(KSndDumpPath, "E:\\PS1\\logs\\ps1_snd_dump.pcm");

static void SndDumpFlush()
	{
	if (g_sndDumpBufLen <= 0)
		return;
	TPtrC8 p((const TUint8 *)g_sndDumpBuf, g_sndDumpBufLen * 4);
	g_sndDumpFile.Write(p);
	g_sndDumpBufLen = 0;
	}

static void SndDumpClose()
	{
	SndDumpFlush();
	g_sndDumpFile.Close();
	g_sndDumpFs.Close();
	g_sndDumpDone = ETrue;
	ps1LogFmt("snddump", "closed frames=%d", (int)(KSndDumpMaxFrames - g_sndDumpFramesLeft));
	}

static void SndDumpFrame(TInt aL, TInt aR)
	{
	if (g_sndDumpDone || !g_sndDumpWanted)
		return;
	if (!g_sndDumpOpen)
		{
		if (g_sndDumpFs.Connect() != KErrNone)
			{ g_sndDumpDone = ETrue; return; }
		const TInt rc = g_sndDumpFile.Replace(g_sndDumpFs, KSndDumpPath, EFileWrite | EFileShareAny);
		if (rc != KErrNone)
			{
			g_sndDumpFs.Close();
			g_sndDumpDone = ETrue;
			ps1LogFmt("snddump", "open failed rc=%d", (int)rc);
			return;
			}
		g_sndDumpOpen = ETrue;
		g_sndDumpFramesLeft = KSndDumpMaxFrames;
		ps1LogFmt("snddump", "opened rate=%d", (int)KSndRate);
		}
	TInt16 *p = g_sndDumpBuf + g_sndDumpBufLen * 2;
	p[0] = (TInt16)aL;
	p[1] = (TInt16)aR;
	g_sndDumpBufLen++;
	g_sndDumpFramesLeft--;
	if (g_sndDumpBufLen >= KSndDumpBufFrames)
		SndDumpFlush();
	if (g_sndDumpFramesLeft <= 0)
		SndDumpClose();
	}
#endif // PS1_BENCH

// Frame is 4 bytes, iCap and KSndChunk are multiples of 4, so frames never cross the ring
// boundary and split-copying is not required.
void CSymSnd::PushFrame(TInt aL, TInt aR)
	{
#ifdef PS1_BENCH
	SndDumpFrame(aL, aR);
#endif
	TInt next = iHead + 4;
	if (next == iCap) next = 0;
	if (next == iTail)
		return;                     // ring is full: discard excess
	TInt16 *p = (TInt16 *)(iRing + iHead);
	p[0] = (TInt16)aL;
	p[1] = (TInt16)aR;
	iHead = next;
	}

// However many samples emulation provided during real time window - that many must be stretched
// over that same window. Primary term is direct measurement (converges in one window without
// oscillating, unlike a fill-based integrator); ring fill level only provides
// weak trim against slow drift.
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
	if (iWsola)
		{
		WsolaFeed(src, n);
		}
	else
		{
		while ((iPos >> 16) < n)
			{
			const TInt i = iPos >> 16;
			// 14-bit fraction: sample difference takes 17 signed bits, and with a full
			// 16-bit fraction the product overflows 32-bit signed integer.
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
		}
	UpdateRate(n);
	}

// Time-scale stretch that keeps pitch. Unlike the linear resampler above, the input
// is never resampled point-by-point: it is read at its natural rate, cut into
// KWsolaWin-frame windows, and overlap-added at a fixed KWsolaHop synthesis stride.
// The nominal analysis position drifts from the synthesis position by exactly the
// ratio iStep already carries (a hop of KWsolaHop output frames consumes
// KWsolaHop*iStep input frames, the same 16.16 accounting iPos does above), so the
// average time-scale ratio - and with it the fix for the pitch bug - comes from the
// same measured iStep as the linear path. WsolaBestOffset then looks up to
// KWsolaRange frames ahead of that nominal position for the offset whose left
// channel best continues the waveform saved in iWsTail, so successive windows
// overlap-add without a click; the offset perturbs only where the window is read
// from, never how far the nominal position advances, so it cannot drift the rate.
void CSymSnd::WsolaFeed(const TInt16 *aSrc, TInt aFrames)
	{
	if (aFrames > KWsolaBufCap)
		{
		aSrc += (aFrames - KWsolaBufCap) * 2;
		aFrames = KWsolaBufCap;
		}
	if (iWsLen + aFrames > KWsolaBufCap)
		{
		// Should not happen at normal chunk sizes and stretch ratios; keep the most
		// recent input rather than overflow the staging buffer.
		TInt drop = iWsLen + aFrames - KWsolaBufCap;
		if (drop > iWsLen) drop = iWsLen;
		Mem::Move(iWsBuf, iWsBuf + drop * 2, (iWsLen - drop) * 2 * sizeof(TInt16));
		iWsLen -= drop;
		}
	Mem::Copy(iWsBuf + iWsLen * 2, aSrc, aFrames * 2 * sizeof(TInt16));
	iWsLen += aFrames;

	for (;;)
		{
		if (!iWsPrimed)
			{
			if (iWsLen < KWsolaWin)
				break;
			// First window ever: there is nothing to crossfade against yet, so its
			// first half goes straight to output and its second half seeds iWsTail.
			for (TInt i = 0; i < KWsolaHop; i++)
				PushFrame(iWsBuf[i * 2], iWsBuf[i * 2 + 1]);
			Mem::Copy(iWsTail, iWsBuf + KWsolaHop * 2, KWsolaHop * 2 * sizeof(TInt16));
			Mem::Move(iWsBuf, iWsBuf + KWsolaWin * 2, (iWsLen - KWsolaWin) * 2 * sizeof(TInt16));
			iWsLen -= KWsolaWin;
			iWsPrimed = ETrue;
			continue;
			}

		// Tentative advance: only committed to iWsAnaFrac once the hop below actually
		// runs, so a break here (not enough lookahead yet) loses nothing and the same
		// hop is retried whole on the next call once more input has arrived.
		const TInt frac = iWsAnaFrac + KWsolaHop * iStep;
		const TInt ss = frac >> 16;
		if (iWsLen < ss + KWsolaWin + KWsolaRange)
			break;
		iWsAnaFrac = frac - (ss << 16);

		const TInt16 *search = iWsBuf + ss * 2;
		const TInt off = WsolaBestOffset(iWsTail, search, KWsolaHop, KWsolaRange);
		const TInt16 *win = search + off * 2;

		// Overlap-add: crossfade the saved tail with the first half of the new window
		// on a linear ramp, emit it, and save the window's second half as the next tail.
		for (TInt i = 0; i < KWsolaHop; i++)
			{
			const TInt wL = win[i * 2], wR = win[i * 2 + 1];
			const TInt tL = iWsTail[i * 2], tR = iWsTail[i * 2 + 1];
			PushFrame(tL + ((wL - tL) * i) / KWsolaHop,
			          tR + ((wR - tR) * i) / KWsolaHop);
			}
		Mem::Copy(iWsTail, win + KWsolaHop * 2, KWsolaHop * 2 * sizeof(TInt16));

		Mem::Move(iWsBuf, iWsBuf + ss * 2, (iWsLen - ss) * 2 * sizeof(TInt16));
		iWsLen -= ss;
		}
	}

// Keep KSndBufs buffers in-flight rather than just one: with a single buffer the entire pipeline
// depth equals its duration, and any frame taking longer results in an audible
// gap. Slots rotate circularly and MaoscBufferCopied arrives in submission order,
// so an in-flight counter suffices to avoid overwriting uncopied buffers.
void CSymSnd::Refill()
	{
	if (!iOpen)
		return;
	while (iInFlight < KSndBufs)
		{
		TInt avail = iHead - iTail;
		if (avail < 0) avail += iCap;
		// While nothing is playing - avoid starting from an almost empty ring: it will
		// immediately drain again. The same threshold also serves as initial buffer.
		const TInt need = iInFlight ? KSndChunk : KSndFill;
		if (avail < need)
			{
			if (iInFlight == 0)
				g_sndUnder++;       // how many times Pump found speaker without data
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

// In GUI the scheduler is driven by the shell itself: CIdle yields control to it after
// EVERY frame, so active objects are already serviced. Nested RunIfReady
// there would only deepen the stack at its deepest point - inside the emulation
// step - and introduce reentrancy. In the console branch nobody drives the scheduler,
// so it is retained there.
static TBool g_pumpAo = ETrue;

void CSymSnd::Pump()
	{
	if (iFailed)
		Reopen();
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

// --- dfsound out-driver: accepts 16-bit stereo PCM -------------------------
static unsigned g_sndFeeds, g_sndSamples;
static int g_sndMax;

static int  symOutInit(void)   { g_sndFeeds = g_sndSamples = 0; g_sndMax = 0; g_sndUnder = 0; return 0; }
static void symOutFinish(void) { }
static int  symOutBusy(void)   { return 0; }          // 0 = empty: generate more
static void symOutFeed(void *buf, int bytes)
	{
	g_sndFeeds++;
	const int n = bytes / 2;
	g_sndSamples += (unsigned)n;
#ifdef PS1_BENCH
	const short *s = (const short *)buf;
	for (int i = 0; i < n; i++)
		{
		int v = s[i];
		if (v < 0) v = -v;
		if (v > g_sndMax) g_sndMax = v;
		}
#endif
	if (g_snd)
		g_snd->Feed(buf, bytes);
	}
static struct out_driver g_symOut = { "symbian", symOutInit, symOutFinish, symOutBusy, symOutFeed };

// --- Shell wrappers (unique names, avoiding typedef conflicts) -------------
extern "C" {

#ifdef PS1_BENCH
static unsigned g_xaCalls, g_cddaCalls, g_cdVolCalls;
#endif

long ps1SpuInit(void)
	{
#ifdef PS1_BENCH
	g_xaCalls = g_cddaCalls = g_cdVolCalls = 0;
#endif
	return SPUinit();
	}
long ps1SpuShutdown(void)  { return SPUshutdown(); }
long ps1SpuOpen(void)      { return SPUopen(); }
long ps1SpuClose(void)     { return SPUclose(); }
void ps1SpuConfigure(void) { SPUconfigure(); }
void ps1SpuWriteRegister(unsigned long r, unsigned short v, unsigned int c) { SPUwriteRegister(r, v, c); }
unsigned short ps1SpuReadRegister(unsigned long r, unsigned int c)          { return SPUreadRegister(r, c); }
void ps1SpuWriteDMAMem(unsigned short *m, int s, unsigned int c) { SPUwriteDMAMem(m, s, c); }
void ps1SpuReadDMAMem(unsigned short *m, int s, unsigned int c)  { SPUreadDMAMem(m, s, c); }
// SPU mixing is part of "emu", and previously inseparable from CPU. The hook matches
// the one used for rasterization: without it one cannot tell if audio is noticeable in
// frame time. The "nosnd" argument merely suppresses opening the speaker; mixer continues running.
extern "C" unsigned int ndrc_symbian_fastcount(void);
unsigned long long g_spuMixTicks;

void ps1SpuAsync(unsigned int a, unsigned int b)
	{
#ifdef PS1_BENCH
	const unsigned int t0 = ndrc_symbian_fastcount();
#endif
	SPUasync(a, b);
#ifdef PS1_BENCH
	g_spuMixTicks += (unsigned int)(ndrc_symbian_fastcount() - t0);
#endif
	}
void ps1SpuRegisterCallback(void (*cb)(int))                    { SPUregisterCallback(cb); }
void ps1SpuRegisterScheduleCb(void (*cb)(unsigned int))         { SPUregisterScheduleCb(cb); }
void ps1SpuPlayADPCMchannel(struct xa_decode *xa, unsigned int cycle, int isStart)
	{
#ifdef PS1_BENCH
	g_xaCalls++;
#endif
	SPUplayADPCMchannel(xa, cycle, isStart);
	}
int ps1SpuPlayCDDAchannel(short *pcm, int bytes, unsigned int cycle, int unused)
	{
#ifdef PS1_BENCH
	g_cddaCalls++;
#endif
	return SPUplayCDDAchannel(pcm, bytes, cycle, unused);
	}
void ps1SpuSetCDvol(unsigned char ll, unsigned char lr, unsigned char rl,
	unsigned char rr, unsigned int cycle)
	{
#ifdef PS1_BENCH
	g_cdVolCalls++;
#endif
	SPUsetCDvol(ll, lr, rl, rr, cycle);
	}
#ifdef PS1_BENCH
void ps1SpuCdStats(unsigned *xa, unsigned *cdda, unsigned *vol)
	{
	if (xa) *xa = g_xaCalls;
	if (cdda) *cdda = g_cddaCalls;
	if (vol) *vol = g_cdVolCalls;
	}
#endif

// dfsound config before ps1SpuInit (SPUinit reads iVolume). Threadless - generated
// synchronously in frame loop; reverb/interpolation disabled (excess CPU on ARM11).
void ps1SndConfigure(void)
	{
	spu_config.iVolume = 1024;
	spu_config.iUseThread = 0;
	spu_config.iUseReverb = 0;
	spu_config.iUseInterpolation = 0;
	spu_config.iXAPitch = 0;
	spu_config.iTempo = 0;
	}

// ps1SpuOpen calls SetupSound (out_current = none); redirect to our driver.
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
	if (aMaxAmp)
		{
#ifdef PS1_BENCH
		*aMaxAmp = g_sndMax;
#else
		*aMaxAmp = -1; // peak scan is disabled in release
#endif
		}
	if (aUnder)   *aUnder = g_sndUnder;
	if (aStepQ16) *aStepQ16 = g_snd ? g_snd->Step() : 0;
	}

// --- Speaker output control (Phase B) --------------------------------------
static CActiveScheduler *g_sndSched;

// Open audio device. Requires CActiveScheduler on the thread (install our own if
// not yet present); open is async, so wait for MaoscOpenComplete up to 2s.
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
	return g_snd->WaitOpen(2000) ? 0 : -2;   // -2: did not open in time (audio may catch up later)
	}

// Volume step up/down. Returns new level (0..KVolSteps). Works before opening
// the stream too - level is remembered and applied in MaoscOpenComplete.
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

void ps1SndResetRate(void)
	{
	if (g_snd)
		g_snd->ResetRate();
	}

void ps1SndSetPumpAo(int aOn)
	{
	g_pumpAo = aOn ? ETrue : EFalse;
	}

// Restart-only (see ECfgWsola in app_symbian.cpp): read into iWsola when the next
// CSymSnd is constructed, same as the stream itself only opening on (re)start.
void ps1SndSetWsola(int aOn)
	{
	g_wsola = aOn ? ETrue : EFalse;
	}

// Bench-only: arm/disarm the raw PCM dump (see SndDumpFrame above). Declared and
// called only under PS1_BENCH (stage1.cpp), so a normal build carries none of it.
#ifdef PS1_BENCH
void ps1SndSetDump(int aOn)
	{
	g_sndDumpWanted = aOn ? ETrue : EFalse;
	}
#endif

// Service active objects and refill next buffer. Call every frame.
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

// --- WSOLA benchmark probe --------------------------------------------------
#ifdef PS1_BENCH
static volatile TInt g_wsolaBenchSink;
#endif

extern "C" void sndSymbianWsolaBench(char *aOut, int aOutLen)
	{
#ifdef PS1_BENCH
	if (!aOut || aOutLen <= 0)
		return;

	const TUint f = ndrc_symbian_fastfreq();
	const TUint freq = f ? f : 1;
	const TUint minTicks = (TUint)(((TUint64)freq * 200ULL) / 1000ULL);

	const TInt KWinFrames = 882;
	const TInt KOffsets   = 882;
	const TInt KTotalFrames = KOffsets + KWinFrames + KWinFrames;
	const TInt KTotalSamples = KTotalFrames * 2;
	const TInt KTotalBytes = KTotalSamples * (TInt)sizeof(TInt16);
	const TInt KWinSamples = KWinFrames * 2;

	TInt16 *buf = (TInt16 *)User::Alloc(KTotalBytes);
	if (!buf)
		{
		snprintf(aOut, (size_t)aOutLen, "wsola: alloc failed");
		return;
		}

	// Pseudo-random samples keep correlation non-trivial across offsets.
	TUint32 seed = 0x12345678;
	for (TInt i = 0; i < KTotalSamples; i++)
		{
		seed = seed * 1103515245u + 12345u;
		buf[i] = (TInt16)(seed >> 16);
		}

	const TInt16 *searchBuf = buf;
	const TInt16 *win = buf + (KOffsets + KWinFrames) * 2;

	TUint iters = 0;
	const TUint t0 = ndrc_symbian_fastcount();
	do
		{
		TInt bestOff = -1;
		TInt64 bestSum = 0;
		for (TInt off = 0; off < KOffsets; off++)
			{
			const TInt16 *cand = searchBuf + off * 2;
			TInt64 sum = 0;
			for (TInt i = 0; i < KWinSamples; i++)
				{
				sum += (TInt64)win[i] * cand[i];
				}
			if (off == 0 || sum > bestSum)
				{
				bestSum = sum;
				bestOff = off;
				}
			}
		g_wsolaBenchSink += bestOff;
		iters++;
		} while ((TUint)(ndrc_symbian_fastcount() - t0) < minTicks);
	const TUint dt = (TUint)(ndrc_symbian_fastcount() - t0);
	const TUint totalDt = dt ? dt : 1;

	User::Free(buf);

	const TUint totalMs = (TUint)(((TUint64)totalDt * 1000ULL) / freq);
	const TUint perSearchUs = (TUint)(((TUint64)totalDt * 1000000ULL) / ((TUint64)freq * iters));
	const TUint msPerAudioSec = (TUint)(((TUint64)totalDt * 50000ULL) / ((TUint64)freq * iters));

	const int len1 = snprintf(aOut, (size_t)aOutLen,
		"wsola: searches=%d totalMs=%u perSearchUs=%u msPerAudioSec=%u",
		(int)iters, totalMs, perSearchUs, msPerAudioSec);

	// Second measurement: the actual two-stage single-channel search WsolaFeed uses,
	// at its real window/range (KWsolaHop/KWsolaRange), same method and units as
	// above so the two lines can be placed side by side. One search happens per
	// synthesis hop of KWsolaHop output frames, hence the KSndRate/KWsolaHop scaling
	// below instead of the fixed 50/s the naive search above assumes.
	if (len1 >= 0 && len1 < aOutLen)
		{
		const TInt KBenchTotalFrames2 = KWsolaRange + KWsolaHop + KWsolaHop;
		const TInt KBenchTotalSamples2 = KBenchTotalFrames2 * 2;
		const TInt KBenchTotalBytes2 = KBenchTotalSamples2 * (TInt)sizeof(TInt16);

		TInt16 *buf2 = (TInt16 *)User::Alloc(KBenchTotalBytes2);
		if (buf2)
			{
			TUint32 seed2 = 0x87654321u;
			for (TInt i = 0; i < KBenchTotalSamples2; i++)
				{
				seed2 = seed2 * 1103515245u + 12345u;
				buf2[i] = (TInt16)(seed2 >> 16);
				}

			const TInt16 *searchBuf2 = buf2;
			const TInt16 *tmpl2 = buf2 + (KWsolaRange + KWsolaHop) * 2;

			TUint iters2 = 0;
			const TUint t0b = ndrc_symbian_fastcount();
			do
				{
				g_wsolaBenchSink += WsolaBestOffset(tmpl2, searchBuf2, KWsolaHop, KWsolaRange);
				iters2++;
				} while ((TUint)(ndrc_symbian_fastcount() - t0b) < minTicks);
			const TUint dt2 = (TUint)(ndrc_symbian_fastcount() - t0b);
			const TUint totalDt2 = dt2 ? dt2 : 1;

			User::Free(buf2);

			const TUint totalMs2 = (TUint)(((TUint64)totalDt2 * 1000ULL) / freq);
			const TUint perSearchUs2 = (TUint)(((TUint64)totalDt2 * 1000000ULL) / ((TUint64)freq * iters2));
			const TUint msPerAudioSec2 = (TUint)(((TUint64)totalDt2 * 1000ULL * (TUint64)KSndRate) /
				((TUint64)freq * iters2 * (TUint64)KWsolaHop));

			snprintf(aOut + len1, (size_t)(aOutLen - len1),
				"\nwsola2: searches=%d totalMs=%u perSearchUs=%u msPerAudioSec=%u",
				(int)iters2, totalMs2, perSearchUs2, msPerAudioSec2);
			}
		}
#else
	if (aOut && aOutLen > 0)
		aOut[0] = 0;
#endif
	}
