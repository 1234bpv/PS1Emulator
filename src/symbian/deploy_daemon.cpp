// Демон розгортання: приймає файли й запускає застосунки з ПК по WiFi/TCP.
//
// Навіщо. Повний цикл розробки без дотику до екрана телефона: ПК заливає свіжий
// ps1stage1.exe, каже "запусти", забирає лог - усе по мережі. Це прибирає
// натискання на екран перед прогоном, яке псувало заміри (див. plans/progress.md:
// той самий код давав 9 326 і 22 314 мс залежно від фонової активності).
//
// Чому TCP, а не Bluetooth. Сокети на Symbian майже BSD-подібні, і TCP-код
// простіший за RFCOMM+SDP. Головне - WiFi (~1-2,5 МБ/с) на відміну від BT
// (~100-150 КБ/с) тягне й великі файли, зокрема образи ігор.
//
// Модель проста навмисно: одне з'єднання за раз, команди послідовні, кожна
// асинхронна операція сокета зроблена блокувальною через User::WaitForRequest.
// Тобто без CActiveScheduler і активних об'єктів - для демона це зайва складність.
//
// Протокол (рядок команди в ASCII, закінчення \n; двійкові дані йдуть сирими):
//   PUT <шлях> <розмір>\n <розмір байтів>   -> "OK <розмір>\n" | "ERR <код>\n"
//   GET <шлях>\n                            -> "OK <розмір>\n" <розмір байтів> | "ERR <код>\n"
//   RUN <exe> <аргументи>\n                 -> "EXIT <код>\n" | "ERR <код>\n"
//   STOP\n                                  -> "BYE\n", демон завершується
//   BYE\n                                   -> закрити з'єднання, чекати наступне

#include <e32base.h>
#include <e32std.h>
#include <e32debug.h>
#include <e32cons.h>
#include <f32file.h>
#include <in_sock.h>
#include <es_sock.h>
#include <es_enum.h>
#include <commdb.h>
#include <cdbcols.h>
#include <commdbconnpref.h>

static const TInt  KPort       = 2323;
static const TInt  KChunk      = 8192;
// Свій IP демон пише сюди, щоб ПК дізнався адресу один раз через накопичувач;
// DHCP може її змінювати між сесіями.
_LIT(KIpFile, "E:\\daemon_ip.txt");
// Стійкий лог на картці: читається через накопичувач навіть коли демон помер,
// тож видно, на якому кроці ініціалізації він упав (консоль зникає надто швидко).
_LIT(KLogFile, "E:\\ps1_daemon.log");

static RSocketServ g_ss;
static RConnection g_conn;
static RSocket     g_listener;
static RFs         g_fs;
static CConsoleBase *g_console;
static RFile       g_logFile;
static TBool       g_haveLog;
// Ознака єдиного екземпляра: існує, поки перший демон живий. Другий запуск бачить
// KErrAlreadyExists і виходить чисто, а не падає з -14 на Bind(зайнятий порт).
static RSemaphore  g_single;

// Буфер дочитування: RecvOneOrMore може захопити більше за рядок команди,
// прихопивши початок двійкового вантажу. Залишок тримається тут між викликами.
static TBuf8<KChunk> g_residual;
static TBuf8<KChunk> g_chunk;

static void say(const TDesC &aMsg)
    {
    if (g_console)
        {
        g_console->Write(aMsg);
        g_console->Write(_L("\n"));
        }
    TBuf8<256> m;
    m.Copy(aMsg.Left(m.MaxLength()));
    RDebug::RawPrint(m);
    // Стійкий слід із флешем на кожному рядку: якщо наступний крок панікує,
    // у файлі лишається саме те, що встигло записатися до падіння.
    if (g_haveLog)
        {
        TBuf8<300> line;
        line.Copy(m);
        line.Append(_L8("\r\n"));
        g_logFile.Write(line);
        g_logFile.Flush();
        }
    }

// --- блокувальні обгортки над асинхронними сокетами ------------------------

// Читає з сокета в g_residual хоча б один байт понад наявне. EFalse на розриві.
static TBool fill(RSocket &aSock)
    {
    if (g_residual.Length() == g_residual.MaxLength())
        return ETrue; // місця немає, але дані є
    TPtr8 tail((TUint8 *)g_residual.Ptr() + g_residual.Length(), 0,
               g_residual.MaxLength() - g_residual.Length());
    TSockXfrLength got;
    TRequestStatus st;
    aSock.RecvOneOrMore(tail, 0, st, got);
    User::WaitForRequest(st);
    if (st != KErrNone || got() == 0)
        return EFalse;
    g_residual.SetLength(g_residual.Length() + got());
    return ETrue;
    }

// Витягає один рядок (без \n) з потоку. EFalse на розриві до кінця рядка.
static TBool readLine(RSocket &aSock, TDes8 &aLine)
    {
    aLine.Zero();
    for (;;)
        {
        TInt nl = g_residual.Locate('\n');
        if (nl != KErrNotFound)
            {
            TPtrC8 l = g_residual.Left(nl);
            // прибрати можливий \r перед \n
            if (l.Length() && l[l.Length() - 1] == '\r')
                l.Set(l.Left(l.Length() - 1));
            aLine.Copy(l.Left(aLine.MaxLength()));
            g_residual.Delete(0, nl + 1);
            return ETrue;
            }
        if (g_residual.Length() == g_residual.MaxLength())
            return EFalse; // рядок довший за буфер - зіпсований потік
        if (!fill(aSock))
            return EFalse;
        }
    }

static TBool writeAll(RSocket &aSock, const TDesC8 &aData)
    {
    TRequestStatus st;
    aSock.Write(aData, st);
    User::WaitForRequest(st);
    return st == KErrNone;
    }

static void reply(RSocket &aSock, const TDesC8 &aLine)
    {
    writeAll(aSock, aLine);
    }

// Читає рівно aSize байтів із потоку у файл: спершу залишок, тоді з сокета.
static TInt recvToFile(RSocket &aSock, RFile &aFile, TInt aSize)
    {
    TInt remaining = aSize;
    while (remaining > 0)
        {
        if (g_residual.Length() == 0 && !fill(aSock))
            return KErrDisconnected;
        TInt take = Min(remaining, g_residual.Length());
        TPtrC8 part = g_residual.Left(take);
        TInt w = aFile.Write(part);
        if (w != KErrNone)
            return w;
        g_residual.Delete(0, take);
        remaining -= take;
        }
    return KErrNone;
    }

static TInt sendFromFile(RSocket &aSock, RFile &aFile, TInt aSize)
    {
    TInt remaining = aSize;
    while (remaining > 0)
        {
        TInt take = Min(remaining, KChunk);
        g_chunk.Zero();
        TInt r = aFile.Read(g_chunk, take);
        if (r != KErrNone)
            return r;
        if (g_chunk.Length() == 0)
            return KErrUnderflow; // файл коротший, ніж заявлено
        if (!writeAll(aSock, g_chunk))
            return KErrDisconnected;
        remaining -= g_chunk.Length();
        }
    return KErrNone;
    }

// --- команди ----------------------------------------------------------------

// "PUT <шлях> <розмір>"
static void cmdPut(RSocket &aSock, const TDesC8 &aArgs)
    {
    TInt sp = aArgs.LocateReverse(' ');
    if (sp == KErrNotFound) { reply(aSock, _L8("ERR badargs\n")); return; }

    TFileName path;
    path.Copy(aArgs.Left(sp));
    TLex8 lex(aArgs.Mid(sp + 1));
    TInt size;
    if (lex.Val(size) != KErrNone || size < 0)
        { reply(aSock, _L8("ERR badsize\n")); return; }

    // Створити батьківські теки: Replace їх не робить, а ціль на кшталт
    // C:\sys\hash може ще не існувати (KErrPathNotFound = -12).
    g_fs.MkDirAll(path);

    RFile f;
    TInt r = f.Replace(g_fs, path, EFileWrite | EFileShareExclusive);
    if (r != KErrNone)
        {
        TBuf8<64> e; e.Format(_L8("ERR open %d\n"), r);
        reply(aSock, e);
        return;
        }
    r = recvToFile(aSock, f, size);
    f.Close();
    if (r != KErrNone)
        {
        TBuf8<64> e; e.Format(_L8("ERR recv %d\n"), r);
        reply(aSock, e);
        return;
        }
    TBuf8<64> ok; ok.Format(_L8("OK %d\n"), size);
    reply(aSock, ok);
    TBuf<64> s; s.Format(_L("PUT %d bytes ok"), size);
    say(s);
    }

// "GET <шлях>"
static void cmdGet(RSocket &aSock, const TDesC8 &aArgs)
    {
    TFileName path;
    path.Copy(aArgs);
    path.TrimAll();

    // ShareAny: дозволяє забрати навіть файл, відкритий іншим (наш живий лог).
    RFile f;
    TInt r = f.Open(g_fs, path, EFileRead | EFileShareAny);
    if (r != KErrNone)
        {
        TBuf8<64> e; e.Format(_L8("ERR open %d\n"), r);
        reply(aSock, e);
        return;
        }
    TInt size = 0;
    f.Size(size);
    TBuf8<64> hdr; hdr.Format(_L8("OK %d\n"), size);
    if (!writeAll(aSock, hdr)) { f.Close(); return; }
    r = sendFromFile(aSock, f, size);
    f.Close();
    TBuf<64> s; s.Format(_L("GET %d bytes rc=%d"), size, r);
    say(s);
    }

// "RUN <exe> <аргументи>". Запускає процес і чекає завершення - ПК так дізнається,
// що прогін скінчився й лог готовий. Радіо під час обчислення простоює.
static void cmdRun(RSocket &aSock, const TDesC8 &aArgs)
    {
    TInt sp = aArgs.Locate(' ');
    TFileName exe;
    TBuf<256> args;
    if (sp == KErrNotFound)
        {
        exe.Copy(aArgs);
        }
    else
        {
        exe.Copy(aArgs.Left(sp));
        args.Copy(aArgs.Mid(sp + 1));
        }
    exe.TrimAll();

    RProcess p;
    TInt r = p.Create(exe, args);
    if (r != KErrNone)
        {
        TBuf8<64> e; e.Format(_L8("ERR create %d\n"), r);
        reply(aSock, e);
        return;
        }
    TRequestStatus st;
    p.Logon(st);
    p.Resume();
    User::WaitForRequest(st);
    TInt exitReason = p.ExitReason();
    TExitType et = p.ExitType();
    p.Close();

    // Код виходу для панічного завершення від'ємний за домовленістю тут.
    TInt code = (et == EExitPanic) ? -1000 - exitReason : exitReason;
    TBuf8<64> ok; ok.Format(_L8("EXIT %d\n"), code);
    reply(aSock, ok);
    TBuf<64> s; s.Format(_L("RUN exit=%d type=%d"), exitReason, (TInt)et);
    say(s);
    }

// Повертає ETrue, поки демон має жити.
static TBool serveOne(RSocket &aSock)
    {
    TBuf8<512> line;
    for (;;)
        {
        if (!readLine(aSock, line))
            return ETrue; // розрив - чекати наступне з'єднання

        TInt sp = line.Locate(' ');
        TPtrC8 cmd = (sp == KErrNotFound) ? TPtrC8(line) : line.Left(sp);
        TPtrC8 args = (sp == KErrNotFound) ? TPtrC8(KNullDesC8) : line.Mid(sp + 1);

        if (cmd == _L8("PUT"))       cmdPut(aSock, args);
        else if (cmd == _L8("GET"))  cmdGet(aSock, args);
        else if (cmd == _L8("RUN"))  cmdRun(aSock, args);
        else if (cmd == _L8("BYE"))  { reply(aSock, _L8("BYE\n")); return ETrue; }
        else if (cmd == _L8("STOP")) { reply(aSock, _L8("BYE\n")); return EFalse; }
        else                         reply(aSock, _L8("ERR unknown\n"));
        }
    }

// --- підняття мережі --------------------------------------------------------

// Поточний не-loopback IPv4, якщо є. ETrue = мережа піднята. Стандартний рецепт
// переліку інтерфейсів через опції UDP-сокета - offline-безпечний.
static TBool currentIp(TDes &aOut)
    {
    aOut.Zero();
    RSocket probe;
    if (probe.Open(g_ss, KAfInet, KSockDatagram, KProtocolInetUdp, g_conn) != KErrNone)
        return EFalse;
    probe.SetOpt(KSoInetEnumInterfaces, KSolInetIfCtrl);
    TPckgBuf<TSoInetInterfaceInfo> info;
    while (probe.GetOpt(KSoInetNextInterface, KSolInetIfCtrl, info) == KErrNone)
        {
        TInetAddr addr(info().iAddress);
        if (addr.Address() != 0 && addr.Address() != INET_ADDR(127, 0, 0, 1))
            {
            addr.Output(aOut);
            break;
            }
        }
    probe.Close();
    return aOut.Length() > 0;
    }

// Чи піднята мережа зараз (для перевірки здоров'я з'єднання в циклі прийому).
static TBool haveIp()
    {
    TBuf<40> ip;
    return currentIp(ip);
    }

static void reportIp()
    {
    TBuf<40> ipText;
    if (currentIp(ipText))
        {
        TBuf<80> s; s.Format(_L("IP %S port %d"), &ipText, KPort);
        say(s);
        RFile f;
        if (f.Replace(g_fs, KIpFile, EFileWrite) == KErrNone)
            {
            TBuf8<64> out; out.Format(_L8("%S:%d\r\n"), &ipText, KPort);
            f.Write(out);
            f.Close();
            }
        }
    else
        {
        say(_L("no IPv4 interface - is WiFi connected?"));
        }
    }

// Ім'я точки доступу (IAP), до якої під'єднуватись без запиту. WiFi-мережі на
// S60v3 налаштовані як IAP; ім'я IAP зазвичай збігається з SSID.
_LIT(KApName, "Netis");

// Знаходить id IAP на ім'я KApName у CommDb. 0, якщо немає. Може лишити
// (KErrPermissionDenied тощо), викликач обгортає TRAP.
static TUint32 findApIapL()
    {
    TUint32 iapId = 0;
    CCommsDatabase *db = CCommsDatabase::NewL();
    CleanupStack::PushL(db);
    CCommsDbTableView *view = db->OpenTableLC(TPtrC(IAP));
    for (TInt rc = view->GotoFirstRecord(); rc == KErrNone; rc = view->GotoNextRecord())
        {
        TBuf<64> name;
        view->ReadTextL(TPtrC(COMMDB_NAME), name);
        if (name.CompareF(KApName) == 0)
            {
            view->ReadUintL(TPtrC(COMMDB_ID), iapId);
            break;
            }
        }
    CleanupStack::PopAndDestroy(2); // view, db
    return iapId;
    }

// Одна спроба мовчазного підняття через IAP "Netis".
static TInt startConnOnce()
    {
    say(_L("reading CommDb for Netis IAP..."));
    TUint32 iapId = 0;
    TRAPD(err, iapId = findApIapL());
    if (err != KErrNone)
        { TBuf<64> s; s.Format(_L("CommDb read err=%d"), err); say(s); return err; }
    if (iapId == 0)
        { say(_L("Netis IAP not found")); return KErrNotFound; }
    TCommDbConnPref pref;
    pref.SetIapId(iapId);
    pref.SetDialogPreference(ECommDbDialogPrefDoNotPrompt);
    pref.SetDirection(ECommDbConnectionDirectionOutgoing);
    TBuf<64> s; s.Format(_L("Netis iap=%u, connecting..."), (unsigned)iapId); say(s);
    return g_conn.Start(pref);
    }

// Піднімає з'єднання. Автозапуск при завантаженні стартує ДО того, як WiFi
// асоціюється, тож перша спроба зазвичай падає - повторюємо мовчки з паузами.
// Лише коли всі спроби вичерпано, відкат на діалог вибору точки (ручний запуск).
static TInt startConn()
    {
    const TInt kTries = 12;
    for (TInt i = 0; i < kTries; i++)
        {
        TInt r = startConnOnce();
        if (r == KErrNone) { say(_L("connected to Netis")); return KErrNone; }
        TBuf<80> s; s.Format(_L("conn attempt %d/%d r=%d, retry in 5s"), i + 1, kTries, r);
        say(s);
        User::After(5 * 1000000);
        }
    say(_L("Netis failed, falling back to AP dialog"));
    return g_conn.Start();
    }

// Обслуговує з'єднання, паралельно раз на 15 с перевіряючи, чи жива мережа.
// Повертає EFalse на STOP (демон завершується), ETrue на втраті мережі
// (потрібне перепідключення). Health-check прибирає зомбі-стан: раніше при
// зникненні WiFi демон висів у Accept живим, тримав порт, і перезапуск не
// вдавався (захист екземпляра) - лишався тільки ребут.
static TBool acceptLoop()
    {
    RTimer timer;
    if (timer.CreateLocal() != KErrNone)
        return ETrue;
    TBool reconnect = ETrue;
    for (;;)
        {
        RSocket sock;
        if (sock.Open(g_ss) != KErrNone)
            break;
        TRequestStatus astat, tstat;
        g_listener.Accept(sock, astat);
        timer.After(tstat, 15 * 1000000);
        User::WaitForRequest(astat, tstat);
        if (astat != KRequestPending)
            {
            timer.Cancel();
            User::WaitForRequest(tstat);
            if (astat == KErrNone)
                {
                say(_L("client connected"));
                g_residual.Zero();
                if (!serveOne(sock)) { sock.Close(); reconnect = EFalse; break; }
                say(_L("client done"));
                }
            sock.Close();
            continue;
            }
        // Спрацював таймер (простій): тримаємо апарат активним, інакше він засинає,
        // WiFi гасне, а приспаний процес не самовідновлюється - і рятував лише ребут.
        User::ResetInactivityTime();
        // Скасувати Accept і перевірити мережу.
        g_listener.CancelAccept();
        User::WaitForRequest(astat);
        sock.Close();
        if (!haveIp())
            {
            say(_L("network lost - reconnecting"));
            break;
            }
        }
    timer.Close();
    return reconnect;
    }

// Завершити інші екземпляри демона. Консольний exe диспетчер завдань не знімає, а
// завислий екземпляр (напр. після сну WiFi) тримав сокет/семафор і блокував новий
// запуск - раніше рятував лише ребут. Тепер новий запуск сам замінює старого.
// Потребує capability PowerMgmt (вбивство чужого процесу).
static void killOtherInstances()
    {
    const TProcessId myId = RProcess().Id();
    TFindProcess finder(_L("ps1daemon*"));
    TFullName name;
    TInt killed = 0;
    while (finder.Next(name) == KErrNone)
        {
        RProcess p;
        if (p.Open(name) != KErrNone)
            continue;
        if (p.Id() != myId && p.ExitType() == EExitPending)
            {
            p.Kill(0);
            killed++;
            }
        p.Close();
        }
    if (killed)
        {
        TBuf<64> s; s.Format(_L("killed %d stale instance(s)"), killed); say(s);
        User::After(500 * 1000);   // дати ядру звільнити сокет/семафор
        }
    }

static void mainL()
    {
    _LIT(KTitle, "PS1 deploy daemon");
    TRAP_IGNORE(g_console = Console::NewL(KTitle, TSize(KConsFullScreen, KConsFullScreen)));

    User::LeaveIfError(g_fs.Connect());
    // Відкрити стійкий лог якнайраніше (одразу після ФС), щоб покрити весь старт.
    // APPEND, не Replace: інакше ручний запуск затирає слід спроби автозапуску.
    g_haveLog = (g_logFile.Open(g_fs, KLogFile, EFileWrite | EFileShareAny) == KErrNone);
    if (!g_haveLog)
        g_haveLog = (g_logFile.Create(g_fs, KLogFile, EFileWrite | EFileShareAny) == KErrNone);
    if (g_haveLog) { TInt endPos = 0; g_logFile.Seek(ESeekEnd, endPos); }
    // NTickCount росте від завантаження: малий тік = запуск одразу після boot
    // (тобто автозапуск спрацював), великий = ручний запуск.
    TBuf<64> startMsg; startMsg.Format(_L("=== daemon start (tick=%u) ==="),
        (unsigned)User::NTickCount());
    say(startMsg);

    // Новий запуск замінює старий: спершу знімаємо будь-які інші екземпляри
    // (у т.ч. завислі), тоді беремо семафор. Раніше тут був відмовний вихід, що й
    // робило демон неперезапускним без ребуту.
    killOtherInstances();
    _LIT(KSingleName, "ps1daemon_single");
    if (g_single.CreateGlobal(KSingleName, 0) == KErrAlreadyExists)
        {
        // Старий не звільнив семафор миттєво - зачекаємо ще трохи й продовжимо;
        // самовідновний цикл нижче все одно повторюватиме Bind, доки сокет вільний.
        say(_L("single-instance busy after kill - proceeding anyway"));
        }

    User::LeaveIfError(g_ss.Connect());
    say(_L("socket server ok"));

    // Самовідновлення: підключитись -> слухати -> при втраті мережі
    // перепідключитись. Так засинання WiFi більше не робить демон зомбі; вихід
    // лише за командою STOP.
    for (;;)
        {
        User::LeaveIfError(g_conn.Open(g_ss));
        say(_L("connection opened, starting..."));
        TInt r = startConn();
        if (r != KErrNone)
            {
            TBuf<64> s; s.Format(_L("conn start err=%d, retry in 10s"), r); say(s);
            g_conn.Close();
            User::After(10 * 1000000);
            continue;
            }
        say(_L("connection up"));
        reportIp();

        TInt lr = g_listener.Open(g_ss, KAfInet, KSockStream, KProtocolInetTcp, g_conn);
        if (lr == KErrNone)
            {
            TInetAddr addr;
            addr.SetPort(KPort);
            addr.SetAddress(KInetAddrAny);
            lr = g_listener.Bind(addr);
            }
        if (lr == KErrNone)
            lr = g_listener.Listen(4);
        if (lr != KErrNone)
            {
            TBuf<64> s; s.Format(_L("listen err=%d, reconnecting"), lr); say(s);
            g_listener.Close();
            g_conn.Close();
            User::After(5 * 1000000);
            continue;
            }
        say(_L("listening. STOP to quit."));

        const TBool reconnect = acceptLoop();
        g_listener.Close();
        g_conn.Close();
        if (!reconnect)
            break;               // STOP
        say(_L("reconnecting..."));
        User::After(3 * 1000000);
        }

    g_ss.Close();
    say(_L("=== daemon stop ==="));
    if (g_haveLog) { g_logFile.Close(); g_haveLog = EFalse; }  // до закриття ФС
    g_fs.Close();
    delete g_console;
    }

GLDEF_C TInt E32Main()
    {
    CTrapCleanup *cleanup = CTrapCleanup::New();
    if (!cleanup) return KErrNoMemory;
    TRAPD(err, mainL());
    // Шлях падіння: mainL лишила до нормального прибирання, лог іще відкритий -
    // записати код leave, щоб було видно причину виходу.
    if (err != KErrNone && g_haveLog)
        {
        TBuf8<64> s; s.Format(_L8("mainL left with %d\r\n"), err);
        g_logFile.Write(s);
        g_logFile.Flush();
        }
    if (g_haveLog) { g_logFile.Close(); g_haveLog = EFalse; }
    delete cleanup;
    return err;
    }
