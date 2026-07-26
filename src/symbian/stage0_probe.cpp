// Етап 0: перевірка виконуваної пам'яті на фізичному апараті.
//
// Питання, на які має відповісти один запуск:
//   1. Чи дає RChunk::CreateLocalCode пам'ять, у яку можна і писати, і яку можна
//      виконувати, БЕЗ перемикання прав доступу (тобто чи можна лишити
//      NO_WRITE_EXEC невизначеним, як у new_dynarec.c:664).
//   2. Чи потрібні для цього capabilities (складається з CAPABILITY None).
//   3. Чи справді IMB_Range обов'язковий - тобто чи видно застарілий I-cache без нього.
//   4. Чи можна дорощувати закомічену пам'ять через Adjust (динарек росте по ходу).
//   5. Скільки коштує IMB_Range - потрібно для оцінки накладних витрат динареку.
//   6. Чи виділяються 16 МБ кешу трансляцій (TARGET_SIZE_2=24) на апараті з 128 МБ ОЗП.
//
// Лог скидається на диск після кожної події: якщо процес запанікує на виконанні
// згенерованого коду, з файлу буде видно, на якому саме кроці.

#include <e32base.h>
#include <e32std.h>
#include <e32debug.h>
#include <f32file.h>

// Кеш трансляцій ari64: TARGET_SIZE_2=24 в assem_arm.h:40.
static const TInt KCacheSize    = 16 * 1024 * 1024;
static const TInt KInitialCommit = 4096;
static const TInt KGrowCommit    = 1024 * 1024;
// Зсув для перевірки дорощеної пам'яті: свідомо за межами першого коміту.
static const TInt KFarOffset     = 512 * 1024;
static const TInt KImbIterations = 1000;

_LIT(KLogPath, "C:\\Data\\ps1_stage0.jsonl");
_LIT(KLogDir,  "C:\\Data\\");

// mov r0, #42 / mov r0, #43 / bx lr
static const TUint32 KMovR0_42 = 0xE3A0002AU;
static const TUint32 KMovR0_43 = 0xE3A0002BU;
static const TUint32 KBxLr     = 0xE12FFF1EU;

typedef TInt (*TProbeFn)();

static RFs  g_fs;
static RFile g_log;
static TBool g_logOpen;
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
        // Скидання після кожної події: паніка на виконанні коду не має з'їсти лог.
        g_log.Flush();
        }
    if (aError.Length()) g_failures++;
    }

static void logOk(const TDesC8 &aType, const TDesC8 &aMsg)
    {
    logEvent(aType, KNullDesC8, aMsg);
    }

// Записує "mov r0,#imm; bx lr" за вказаною адресою.
static void emitStub(TUint8 *aAt, TUint32 aMovInstr)
    {
    TUint32 *p = reinterpret_cast<TUint32*>(aAt);
    p[0] = aMovInstr;
    p[1] = KBxLr;
    }

static TInt callStub(TUint8 *aAt)
    {
    // Через ціле: ISO C++ забороняє пряме приведення object -> function pointer.
    TProbeFn fn = reinterpret_cast<TProbeFn>(reinterpret_cast<TUint32>(aAt));
    return fn();
    }

static void runProbe()
    {
    RChunk chunk;
    TBuf8<256> msg;

    // --- 1. Виділення ---
    TInt err = chunk.CreateLocalCode(KInitialCommit, KCacheSize);
    msg.Format(_L8("CreateLocalCode(commit=%d,max=%d) err=%d"),
               KInitialCommit, KCacheSize, err);
    if (err != KErrNone)
        {
        // KErrPermissionDenied тут означав би, що потрібна capability.
        logEvent(_L8("chunk_create"), _L8("MEM_ALLOC_FAIL"), msg);
        return;
        }
    logOk(_L8("chunk_create"), msg);
    CleanupClosePushL(chunk);

    TUint8 *base = chunk.Base();
    msg.Format(_L8("base=0x%08x size=%d maxsize=%d"),
               reinterpret_cast<TUint32>(base), chunk.Size(), chunk.MaxSize());
    logOk(_L8("chunk_info"), msg);

    // --- 2. Запис коду в свіжовиділену пам'ять ---
    emitStub(base, KMovR0_42);
    logOk(_L8("write"), _L8("stub v1 (return 42) written at base"));

    User::IMB_Range(base, base + 8);
    logOk(_L8("imb"), _L8("IMB_Range over 8 bytes"));

    // --- 3. Виконання. Якщо пам'ять не виконувана, тут буде KERN-EXEC 3
    //     і останнім рядком логу лишиться попередній запис. ---
    TInt r = callStub(base);
    msg.Format(_L8("returned %d, expected 42"), r);
    if (r != 42)
        logEvent(_L8("exec"), _L8("EXEC_WRONG_RESULT"), msg);
    else
        logOk(_L8("exec"), msg);

    // --- 4. Перезапис уже виконаного коду БЕЗ IMB_Range.
    //     Це не тест на успіх: результат 42 означає застарілий I-cache і
    //     доводить, що IMB_Range обов'язковий. Результат 43 означає лише, що
    //     на цьому апараті рядок не встиг закешуватись - не доказ протилежного. ---
    emitStub(base, KMovR0_43);
    r = callStub(base);
    msg.Format(_L8("returned %d (42=stale icache, 43=coherent)"), r);
    logOk(_L8("exec_no_imb"), msg);

    // --- 5. Той самий перезапис з IMB_Range: тут 43 обов'язковий. ---
    User::IMB_Range(base, base + 8);
    r = callStub(base);
    msg.Format(_L8("returned %d, expected 43"), r);
    if (r != 43)
        logEvent(_L8("exec_after_imb"), _L8("IMB_FAIL"), msg);
    else
        logOk(_L8("exec_after_imb"), msg);

    // --- 6. Вартість IMB_Range на 4 КБ ---
    const TUint t0 = User::NTickCount();
    for (TInt i = 0; i < KImbIterations; i++)
        User::IMB_Range(base, base + 4096);
    const TUint dt = User::NTickCount() - t0;
    msg.Format(_L8("%d iterations over 4096 bytes took %u ms"), KImbIterations, dt);
    logOk(_L8("imb_cost"), msg);

    // --- 7. Дорощування: динарек комітить пам'ять по ходу генерації ---
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

    // --- 8. Чи комітяться всі 16 МБ. Невдача тут не блокує порт - вона означає,
    //     що TARGET_SIZE_2 треба зменшувати (P2). ---
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
        g_fs.MkDirAll(KLogDir);
        if (g_log.Replace(g_fs, KLogPath, EFileWrite | EFileShareAny) == KErrNone)
            g_logOpen = ETrue;
        }

    logOk(_L8("start"), _L8("stage0 executable memory probe"));
    runProbe();

    TBuf8<64> msg;
    msg.Format(_L8("%d failure(s)"), g_failures);
    logOk(_L8("done"), msg);

    if (g_logOpen) g_log.Close();
    g_fs.Close();
    }

GLDEF_C TInt E32Main()
    {
    CTrapCleanup *cleanup = CTrapCleanup::New();
    if (!cleanup) return KErrNoMemory;
    TRAPD(err, mainL());
    delete cleanup;
    return err;
    }
