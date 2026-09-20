// Hardware performance counters of the ARM1136 (CP15 c15 c12: PMCR, CCNT,
// PMN0, PMN1).
//
// Why this file exists: the rasteriser spends four to six times more cycles
// than it issues instructions, which everywhere else in this project has meant
// waiting on memory - but three separate hypotheses about which memory failed
// to explain it. Counting cache misses directly is the only way left.
//
// ARMv6 has no user-access enable register, so every one of those registers is
// privileged. The way in is the one SymbianOverclock already uses on this
// device: a patched kernel function that jumps to a blob placed in the SRAM
// chunk. The blob runs in kernel mode and can touch CP15.
//
// Cost: each snapshot patches kernel code and unpatches it again, so it is
// taken twice per run, never per frame.
//
// The blobs are literal bytes rather than assembler symbols. An earlier version
// took them from an __asm__ block by label, and what reached SRAM was not what
// the labels described: the setup blob came back reading pc+4 where the pool
// sat at pc+8. These arrays are the ones the SymbianOverclock probe proved on
// this device.

// Bench-only instrumentation: nothing here is called outside PS1_BENCH, and
// a release build without the flag compiles this file down to nothing.
#ifdef PS1_BENCH

#include <e32std.h>
#include "ocengine.h"

// ldr r0,[pc,#8] / mcr p15,0,r0,c15,c12,0 / mov r0,#0 / bx lr / .word cfg
static const TUint8 KPmuSetup[] =
	{
	0x08, 0x00, 0x9f, 0xe5,
	0x1c, 0x0f, 0x0f, 0xee,
	0x00, 0x00, 0xa0, 0xe3,
	0x1e, 0xff, 0x2f, 0xe1,
	0x00, 0x00, 0x00, 0x00
	};
const TInt KPmuCfgOff = 16;

// mrc CCNT/PMN0/PMN1 / add r0,pc,#8 / stmia r0,{r1,r2,r3} / mov r0,r1 / bx lr,
// then the three words the store lands in.
static const TUint8 KPmuRead[] =
	{
	0x3c, 0x1f, 0x1f, 0xee,
	0x5c, 0x2f, 0x1f, 0xee,
	0x7c, 0x3f, 0x1f, 0xee,
	0x08, 0x00, 0x8f, 0xe2,
	0x0e, 0x00, 0x80, 0xe8,
	0x01, 0x00, 0xa0, 0xe1,
	0x1e, 0xff, 0x2f, 0xe1,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
	};
const TInt KPmuOutOff = 7 * 4;
// Where the engine puts a blob inside the SRAM chunk. Declared in ocengine.cpp
// rather than its header, so it is repeated here; the two must agree.
const TUint32 KOcSramSlot = 0x800u;

static COcEngine *g_eng;
static TInt g_prepRc = KErrNotReady;

// PMCR layout on the ARM1136: bit 0 enable, 1 reset the two event counters,
// 2 reset CCNT, 3 divide CCNT by 64. The divider matters: at 426 MHz an
// undivided CCNT wraps in ten seconds.
//
// Measured on this device, not taken from the manual: the event selected by
// bits [19:12] is the one read back through opcode2 3, and [27:20] is read
// through opcode2 2 - crosswise to the obvious reading of the register. Two
// runs settled it: with both fields set to 0x07 the two counters agreed to
// within one count, and with 0x07 and 0x0b only this pairing put the 15M
// instruction count on the instruction event.
static TUint32 pmcrCfg(TInt aEvt0, TInt aEvt1)
	{
	return ((TUint32)(aEvt1 & 0xff) << 20) | ((TUint32)(aEvt0 & 0xff) << 12) | 0x0f;
	}

static TInt runBlob(const TUint8 *aBytes, TInt aLen, TInt &aResult)
	{
	if (!g_eng)
		return KErrNotReady;
	TPtrC8 blob(aBytes, aLen);
	return g_eng->ExecCustomBlob(blob, aResult);
	}

// The known-answer blob of the SymbianOverclock probe: ldr r0,[pc,#0] / bx lr
// / .word 0x5a5a0001. If this does not come back it is the kernel entry that
// is not working here, not anything about the counters.
static const TUint8 KPmuHello[] =
	{
	0x00, 0x00, 0x9f, 0xe5,
	0x1e, 0xff, 0x2f, 0xe1,
	0x01, 0x00, 0x5a, 0x5a
	};

static TInt g_helloRc;

extern "C" unsigned pmuSymbianHello(unsigned *aSram)
	{
	if (aSram) *aSram = g_eng ? g_eng->SramAddress() : 0;
	TInt res = 0;
	g_helloRc = runBlob(KPmuHello, (TInt)sizeof(KPmuHello), res);
	if (g_helloRc != KErrNone)
		return 0;
	return (unsigned)res;
	}

static void engineClose()
	{
	if (g_eng) { g_eng->EndPatchSession(); delete g_eng; g_eng = NULL; }
	g_prepRc = KErrNotReady;
	}

static TInt engineOpen()
	{
	if (g_eng)
		return g_prepRc;
	TRAPD(err, g_eng = COcEngine::NewL());
	if (err != KErrNone || !g_eng)
		{ g_prepRc = err ? err : KErrNoMemory; return g_prepRc; }
	g_prepRc = g_eng->Prepare();
	// A session that died with the kernel page still shadowed leaves the read
	// patch live, and then every exec blob silently returns its own first word
	// instead of running. Cost of being wrong about this is one unshadow.
	if (g_prepRc == KErrNone)
		g_eng->EndPatchSession();
	return g_prepRc;
	}

// Returns KErrNone when the counters are running. A run that cannot answer the
// known-answer blob must say so: the failure mode is not an error code but
// counters that never move, which reads exactly like a workload that costs
// nothing. Reopening the engine has recovered it in practice.
extern "C" int pmuSymbianStart(int aEvt0, int aEvt1, unsigned *aDiag)
	{
	// Repeating the exec cycle without reopening is what recovers it: Prepare
	// itself runs a read-patch session, so closing and reopening only puts the
	// read patch back into the same instruction-cache lines. Each ExecCustomBlob
	// ends with an unshadow, and the cycle after one of those behaves.
	for (TInt attempt = 0; attempt < 6; attempt++)
		{
		if (engineOpen() != KErrNone)
			{
			if (aDiag) { aDiag[2] = (unsigned)g_prepRc; aDiag[4] = (unsigned)attempt; }
			return g_prepRc;
			}
		const unsigned h = pmuSymbianHello(NULL);
		if (aDiag)
			{ aDiag[1] = h; aDiag[2] = (unsigned)g_helloRc; aDiag[3] = (unsigned)g_eng->FailedAt(); aDiag[4] = (unsigned)attempt; }
		if (h == 0x5a5a0001u)
			break;
		if (attempt == 5)
			{ engineClose(); return KErrNotReady; }
		}

	// The configuration word is patched into the blob's own pool, because the
	// kernel entry point passes no argument: its only parameter has to equal
	// the jump target itself.
	TUint8 buf[sizeof(KPmuSetup)];
	Mem::Copy(buf, KPmuSetup, sizeof(KPmuSetup));
	*(TUint32 *)(buf + KPmuCfgOff) = pmcrCfg(aEvt0, aEvt1);
	TInt res = -1;
	const TInt rc = runBlob(buf, (TInt)sizeof(buf), res);
	// The setup blob returns a literal zero, so a non-zero here means the jump
	// never happened and ReadOrCall merely read the words back.
	if (aDiag) aDiag[0] = (unsigned)res;
	return rc;
	}

// Takes a snapshot. The blob leaves all three counters in its own pool inside
// SRAM, because only one word can travel back in r0.
extern "C" int pmuSymbianRead(unsigned *aCcnt, unsigned *aPmn0, unsigned *aPmn1, unsigned *aDirect)
	{
	if (aCcnt) *aCcnt = 0;
	if (aPmn0) *aPmn0 = 0;
	if (aPmn1) *aPmn1 = 0;
	if (aDirect) *aDirect = 0;
	if (!g_eng || g_prepRc != KErrNone)
		return g_prepRc;

	TInt res = 0;
	const TInt rc = runBlob(KPmuRead, (TInt)sizeof(KPmuRead), res);
	// The blob returns CCNT in r0 as well as leaving it in its pool. The two
	// paths disagreeing is the only way to tell a blob that did not run from a
	// readback that came from the wrong address.
	if (aDirect) *aDirect = (unsigned)res;
	if (rc != KErrNone)
		return rc;

	const TUint32 base = g_eng->SramAddress() + KOcSramSlot + KPmuOutOff;
	TUint32 out[3] = { 0, 0, 0 };
	const TUint32 addr[3] = { base, base + 4, base + 8 };
	const TInt rr = g_eng->ReadRegs(addr, out, 3);
	if (rr != KErrNone)
		return rr;
	if (aCcnt) *aCcnt = out[0];
	// Crosswise, see the note at pmcrCfg: the blob stores opcode2 2 then 3, and
	// the event of the first field comes back in the second word.
	if (aPmn0) *aPmn0 = out[2];
	if (aPmn1) *aPmn1 = out[1];
	return KErrNone;
	}

extern "C" void pmuSymbianStop(void)
	{
	engineClose();
	}

#endif // PS1_BENCH
