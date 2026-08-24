// Stage 0: executable memory probe on physical hardware.
//
// Questions a single run must answer:
//   1. Does RChunk::CreateLocalCode provide memory that can be both written and
//      executed WITHOUT permission switching (i.e. can NO_WRITE_EXEC remain
//      undefined, as in new_dynarec.c:664).
//   2. Are capabilities required for this (built with CAPABILITY None).
//   3. Is IMB_Range truly mandatory - i.e. is stale I-cache visible without it.
//   4. Can committed memory be grown via Adjust (dynarec grows dynamically).
//   5. How much does IMB_Range cost - needed to estimate dynarec overhead.
//   6. Can 16 MB of translation cache (TARGET_SIZE_2=24) be allocated on hardware with 128 MB RAM.
//
// Log is flushed to disk after every event: if the process panics executing
// generated code, the file will show the exact step.

#include <e32base.h>
#include <e32std.h>
#include <e32debug.h>
#include <e32cons.h>
#include <f32file.h>

// ari64 translation cache: TARGET_SIZE_2=24 in assem_arm.h:40.
static const TInt KCacheSize    = 16 * 1024 * 1024;
static const TInt KInitialCommit = 4096;
static const TInt KGrowCommit    = 1024 * 1024;
// Offset to test grown memory: deliberately beyond the initial commit.
static const TInt KFarOffset     = 512 * 1024;
static const TInt KImbIterations = 1000;

// E: first: on N95 8GB this is mass memory, and the only one visible over USB.
// C:\Data is fallback for devices without mass memory.
_LIT(KLogPathE, "E:\\ps1_stage0.jsonl");
_LIT(KLogPathC, "C:\\Data\\ps1_stage0.jsonl");
_LIT(KLogDirC,  "C:\\Data\\");

// mov r0, #42 / mov r0, #43 / bx lr
static const TUint32 KMovR0_42 = 0xE3A0002AU;
static const TUint32 KMovR0_43 = 0xE3A0002BU;
static const TUint32 KBxLr     = 0xE12FFF1EU;

typedef TInt (*TProbeFn)();

static RFs  g_fs;
static RFile g_log;
static TBool g_logOpen;
// On hardware RDebug writes nowhere without a debugger, and the file must be retrieved.
// The console is the only channel visible immediately on the phone screen. Created
// AFTER measurements: in EKA2L1 window server crashes on it, and the probe must not
// lose results because of that. Lines accumulate here until then.
static TBuf<4096> g_screen;
static TUint g_startTick;
static TInt  g_eventId;
static TInt  g_failures;

static void logEvent(const TDesC8 &aType, const TDesC8 &aError, const TDesC8 &aMsg)
    {
    const TUint elapsed = User::NTickCount() - g_startTick;
    const TInt id = g_eventId++;

    TBuf8<512> line;
    line.AppendFormat(_L8("{\"id\":%d,\"ms\":%u,\"event\":\"%S\",\"error\":"),
                      id, elapsed, &aType);
    if (aError.Length())
        line.AppendFormat(_L8("\"%S\""), &aError);
    else
        line.Append(_L8("null"));
    line.AppendFormat(_L8(",\"msg\":\"%S\"}\r\n"), &aMsg);

    RDebug::RawPrint(line);
    if (g_logOpen)
        {
        g_log.Write(line);
        // Flush after every event: panic executing code must not eat the log.
        g_log.Flush();
        }
    // The screen needs human-readable output, not JSON: event, error, essence.
    TBuf<256> screen;
    screen.Copy(aType);
    if (aError.Length())
        {
        TBuf<64> err;
        err.Copy(aError);
        screen.Append(_L(" ["));
        screen.Append(err);
        screen.Append(_L("]"));
        }
    screen.Append(_L(": "));
    TBuf<192> m;
    m.Copy(aMsg.Left(m.MaxLength()));
    screen.Append(m);
    screen.Append(_L("\n"));
    if (g_screen.Length() + screen.Length() <= g_screen.MaxLength())
        g_screen.Append(screen);

    if (aError.Length()) g_failures++;
    }

static void logOk(const TDesC8 &aType, const TDesC8 &aMsg)
    {
    logEvent(aType, KNullDesC8, aMsg);
    }

// Writes "mov r0,#imm; bx lr" at the specified address.
static void emitStub(TUint8 *aAt, TUint32 aMovInstr)
    {
    TUint32 *p = reinterpret_cast<TUint32*>(aAt);
    p[0] = aMovInstr;
    p[1] = KBxLr;
    }

static TInt callStub(TUint8 *aAt)
    {
    // Via integer: ISO C++ forbids direct cast object -> function pointer.
    TProbeFn fn = reinterpret_cast<TProbeFn>(reinterpret_cast<TUint32>(aAt));
    return fn();
    }

static void runProbe()
    {
    RChunk chunk;
    TBuf8<256> msg;

    // --- 1. Allocation ---
    TInt err = chunk.CreateLocalCode(KInitialCommit, KCacheSize);
    msg.Format(_L8("CreateLocalCode(commit=%d,max=%d) err=%d"),
               KInitialCommit, KCacheSize, err);
    if (err != KErrNone)
        {
        // KErrPermissionDenied here would indicate a capability is required.
        logEvent(_L8("chunk_create"), _L8("MEM_ALLOC_FAIL"), msg);
        return;
        }
    logOk(_L8("chunk_create"), msg);
    CleanupClosePushL(chunk);

    TUint8 *base = chunk.Base();
    msg.Format(_L8("base=0x%08x size=%d maxsize=%d"),
               reinterpret_cast<TUint32>(base), chunk.Size(), chunk.MaxSize());
    logOk(_L8("chunk_info"), msg);

    // --- 2. Write code into freshly allocated memory ---
    emitStub(base, KMovR0_42);
    logOk(_L8("write"), _L8("stub v1 (return 42) written at base"));

    User::IMB_Range(base, base + 8);
    logOk(_L8("imb"), _L8("IMB_Range over 8 bytes"));

    // --- 3. Execution. If memory is non-executable, KERN-EXEC 3 will happen
    //     and the last log line will remain the previous write. ---
    TInt r = callStub(base);
    msg.Format(_L8("returned %d, expected 42"), r);
    if (r != 42)
        logEvent(_L8("exec"), _L8("EXEC_WRONG_RESULT"), msg);
    else
        logOk(_L8("exec"), msg);

    // --- 4. Overwrite previously executed code WITHOUT IMB_Range.
    //     This is not a pass/fail test: result 42 indicates stale I-cache and
    //     proves IMB_Range is mandatory. Result 43 only means the line was not
    //     cached yet on this hardware - not proof to the contrary. ---
    emitStub(base, KMovR0_43);
    r = callStub(base);
    msg.Format(_L8("returned %d (42=stale icache, 43=coherent)"), r);
    logOk(_L8("exec_no_imb"), msg);

    // --- 5. Same overwrite with IMB_Range: here 43 is mandatory. ---
    User::IMB_Range(base, base + 8);
    r = callStub(base);
    msg.Format(_L8("returned %d, expected 43"), r);
    if (r != 43)
        logEvent(_L8("exec_after_imb"), _L8("IMB_FAIL"), msg);
    else
        logOk(_L8("exec_after_imb"), msg);

    // --- 6. Cost of IMB_Range over 4 KB ---
    const TUint t0 = User::NTickCount();
    for (TInt i = 0; i < KImbIterations; i++)
        User::IMB_Range(base, base + 4096);
    const TUint dt = User::NTickCount() - t0;
    msg.Format(_L8("%d iterations over 4096 bytes took %u ms"), KImbIterations, dt);
    logOk(_L8("imb_cost"), msg);

    // --- 7. Growing: dynarec commits memory dynamically as code is generated ---
    err = chunk.Adjust(KGrowCommit);
    msg.Format(_L8("Adjust(%d) err=%d size=%d"), KGrowCommit, err, chunk.Size());
    if (err != KErrNone)
        {
        logEvent(_L8("grow"), _L8("MEM_ALLOC_FAIL"), msg);
        }
    else
        {
        logOk(_L8("grow"), msg);
        TUint8 *far = base + KFarOffset;
        emitStub(far, KMovR0_42);
        User::IMB_Range(far, far + 8);
        r = callStub(far);
        msg.Format(_L8("at +%d returned %d, expected 42"), KFarOffset, r);
        if (r != 42)
            logEvent(_L8("exec_grown"), _L8("EXEC_WRONG_RESULT"), msg);
        else
            logOk(_L8("exec_grown"), msg);
        }

    // --- 8. Check if full 16 MB commits. Failure here does not block the port - it means
    //     TARGET_SIZE_2 must be reduced (P2). ---
    err = chunk.Adjust(KCacheSize);
    msg.Format(_L8("Adjust(%d) err=%d size=%d"), KCacheSize, err, chunk.Size());
    if (err != KErrNone)
        logEvent(_L8("commit_full"), _L8("MEM_ALLOC_FAIL"), msg);
    else
        logOk(_L8("commit_full"), msg);

    TMemoryInfoV1Buf mem;
    UserHal::MemoryInfo(mem);
    msg.Format(_L8("total=%d free=%d"), mem().iTotalRamInBytes, mem().iFreeRamInBytes);
    logOk(_L8("ram"), msg);

    CleanupStack::PopAndDestroy(&chunk);
    }

static void mainL()
    {
    g_startTick = User::NTickCount();

    if (g_fs.Connect() == KErrNone)
        {
        if (g_log.Replace(g_fs, KLogPathE, EFileWrite | EFileShareAny) == KErrNone)
            g_logOpen = ETrue;
        else
            {
            g_fs.MkDirAll(KLogDirC);
            if (g_log.Replace(g_fs, KLogPathC, EFileWrite | EFileShareAny) == KErrNone)
                g_logOpen = ETrue;
            }
        }

    logOk(_L8("start"), _L8("stage0 executable memory probe"));
    if (g_logOpen)
        logOk(_L8("log"), _L8("written to E:\\ps1_stage0.jsonl or C:\\Data"));
    else
        logEvent(_L8("log"), _L8("LOG_OPEN_FAIL"), _L8("screen only"));

    runProbe();

    TBuf8<64> msg;
    msg.Format(_L8("%d failure(s)"), g_failures);
    logOk(_L8("done"), msg);

    if (g_logOpen) g_log.Close();
    g_fs.Close();

    // Measurements are complete and recorded; display only from here on.
    _LIT(KTitle, "PS1 stage0");
    CConsoleBase *console = NULL;
    TRAPD(err, console = Console::NewL(KTitle, TSize(KConsFullScreen, KConsFullScreen)));
    if (err == KErrNone && console)
        {
        console->Write(g_screen);
        console->Write(_L("\nPress any key to exit.\n"));
        console->Getch();
        delete console;
        }
    }

GLDEF_C TInt E32Main()
    {
    CTrapCleanup *cleanup = CTrapCleanup::New();
    if (!cleanup) return KErrNoMemory;
    TRAPD(err, mainL());
    delete cleanup;
    return err;
    }
