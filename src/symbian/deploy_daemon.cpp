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

static const TInt  KPort       = 2323;
static const TInt  KChunk      = 8192;
// Свій IP демон пише сюди, щоб ПК дізнався адресу один раз через накопичувач;
// DHCP може її змінювати між сесіями.
_LIT(KIpFile, "E:\\daemon_ip.txt");

static RSocketServ g_ss;
static RConnection g_conn;
static RSocket     g_listener;
static RFs         g_fs;
static CConsoleBase *g_console;

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

    RFile f;
    TInt r = f.Open(g_fs, path, EFileRead | EFileShareReadersOnly);
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

// Знаходить перший не-loopback IPv4 і пише його у файл + на екран. Це стандартний
// рецепт переліку інтерфейсів через опції UDP-сокета - offline-безпечний.
static void reportIp()
    {
    RSocket probe;
    if (probe.Open(g_ss, KAfInet, KSockDatagram, KProtocolInetUdp, g_conn) != KErrNone)
        return;
    probe.SetOpt(KSoInetEnumInterfaces, KSolInetIfCtrl);
    TPckgBuf<TSoInetInterfaceInfo> info;
    TBuf<40> ipText;
    while (probe.GetOpt(KSoInetNextInterface, KSolInetIfCtrl, info) == KErrNone)
        {
        TInetAddr addr(info().iAddress);
        if (addr.Address() != 0 && addr.Address() != INET_ADDR(127, 0, 0, 1))
            {
            addr.Output(ipText);
            break;
            }
        }
    probe.Close();

    if (ipText.Length())
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

static void mainL()
    {
    _LIT(KTitle, "PS1 deploy daemon");
    TRAP_IGNORE(g_console = Console::NewL(KTitle, TSize(KConsFullScreen, KConsFullScreen)));

    User::LeaveIfError(g_fs.Connect());
    User::LeaveIfError(g_ss.Connect());
    User::LeaveIfError(g_conn.Open(g_ss));

    // Може один раз спитати точку доступу; далі з'єднання тримається всю сесію,
    // тож окремий прогін лишається без дотику до екрана.
    say(_L("starting connection..."));
    TInt r = g_conn.Start();
    if (r != KErrNone)
        {
        TBuf<64> s; s.Format(_L("conn start err=%d"), r);
        say(s);
        User::Leave(r);
        }

    reportIp();

    User::LeaveIfError(
        g_listener.Open(g_ss, KAfInet, KSockStream, KProtocolInetTcp, g_conn));
    TInetAddr addr;
    addr.SetPort(KPort);
    addr.SetAddress(KInetAddrAny);
    User::LeaveIfError(g_listener.Bind(addr));
    User::LeaveIfError(g_listener.Listen(4));
    say(_L("listening. STOP command or task-kill to quit."));

    TBool keepGoing = ETrue;
    while (keepGoing)
        {
        RSocket sock;
        if (sock.Open(g_ss) != KErrNone)
            break;
        TRequestStatus st;
        g_listener.Accept(sock, st);
        User::WaitForRequest(st);
        if (st == KErrNone)
            {
            say(_L("client connected"));
            g_residual.Zero();
            keepGoing = serveOne(sock);
            say(_L("client done"));
            }
        sock.Close();
        }

    g_listener.Close();
    g_conn.Close();
    g_ss.Close();
    g_fs.Close();
    delete g_console;
    }

GLDEF_C TInt E32Main()
    {
    CTrapCleanup *cleanup = CTrapCleanup::New();
    if (!cleanup) return KErrNoMemory;
    TRAPD(err, mainL());
    delete cleanup;
    return err;
    }
