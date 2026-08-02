// Оболонка AVKON. Консольний stage1 не мав CCoeEnv, а MBX-EGL створює поверхню
// лише на вікні під ним: eglCreateWindowSurface панікувала на "голому" RWindow
// (перевірено трасуванням, див. коментар у vout_symbian.cpp). Побічно фреймворк
// дає штатний вихід із застосунку, місце в таск-листі й керовану орієнтацію.
//
// Емуляція йде не суцільним циклом, а по кадру з CIdle: інакше планувальник
// ніколи не віддав би керування CONE, і ні клавіші, ні вихід не дійшли б сюди.

#include <e32base.h>
#include <eikstart.h>
#include <eikenv.h>
#include <eikspane.h>
#include <eikbtgpc.h>
#include <eikon.hrh>
#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <avkon.hrh>
#include <coecntrl.h>
#include <akndef.h>
#include <w32std.h>
#include <apgcli.h>

extern "C" {
int  ps1GuiSetup(void);
int  ps1GuiStep(void);
void ps1GuiShutdown(void);
void ps1GuiRequestExit(void);
void ps1Log(const char *aType, const char *aMsg);
void ps1LogInt(const char *aType, const char *aMsg, int aVal);
int  ps1SndVolumeStep(int aDelta);
int  ps1SndVolumeSteps(void);
int  voutSymbianKeyEvent(int aScanCode, int aDown);
int  voutSymbianFinish(int aHoldMs);
}

// Прив'язка виведення до вікна CONE (vout_symbian.cpp). Без extern "C": типи Symbian.
void voutSymbianAttachWindow(RWsSession &aWs, CWsScreenDevice &aDev, RWindow &aWin,
                             CWindowGc &aGc);

// Той самий UID, що в stage1.mmp і stage1_reg.rss.
static const TUid KUidPs1App = { 0xE0001002 };

// --- Контейнер: вікно для кадру + клавіші -----------------------------------

class CPs1Container : public CCoeControl
	{
public:
	static CPs1Container *NewL(const TRect &aRect);
	TKeyResponse OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType);
	// CCoeControl::Window() захищений, а виведенню потрібне саме це вікно.
	RWindow &FrameWindow() { return Window(); }
private:
	void ConstructL(const TRect &aRect);
	void Draw(const TRect &aRect) const;
	};

CPs1Container *CPs1Container::NewL(const TRect &aRect)
	{
	CPs1Container *self = new (ELeave) CPs1Container;
	CleanupStack::PushL(self);
	self->ConstructL(aRect);
	CleanupStack::Pop(self);
	return self;
	}

void CPs1Container::ConstructL(const TRect &aRect)
	{
	CreateWindowL();
	SetRect(aRect);
	Window().SetBackgroundColor(KRgbBlack);
	ActivateL();
	}

// Порожній навмисно: кадр кладе блит емулятора поза парою BeginRedraw/EndRedraw,
// і актуального кадру фреймворк не має. Системне перемальовування з'їдає один
// кадр (фон), наступний його перекриває.
void CPs1Container::Draw(const TRect & /*aRect*/) const
	{
	}

TKeyResponse CPs1Container::OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKeyDown && aType != EEventKeyUp)
		return EKeyWasNotConsumed;

	// Права софт-клавіша - вихід. CBA сховано заради повного екрана, тож підпису
	// немає, але звичне місце лишається робочим; червона клавіша теж закриває.
	if (aType == EEventKeyDown && aKeyEvent.iScanCode == EStdKeyDevice1)
		{
		CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
		return EKeyWasConsumed;
		}

	// Гучність: бічні клавіші апарата плюс зірочка/решітка. Обидва набори кодів
	// для * і # - прошивки шлють то код клавіатури, то ASCII.
	if (aType == EEventKeyDown)
		{
		const TInt sc = aKeyEvent.iScanCode;
		TInt d = 0;
		if (sc == EStdKeyIncVolume || sc == EStdKeyHash || sc == '#')
			d = 1;
		else if (sc == EStdKeyDecVolume || sc == EStdKeyNkpAsterisk || sc == '*')
			d = -1;
		if (d)
			{
			ps1LogInt("vol", "step", ps1SndVolumeStep(d));
			return EKeyWasConsumed;
			}
		}

	return voutSymbianKeyEvent(aKeyEvent.iScanCode, aType == EEventKeyDown)
		? EKeyWasConsumed : EKeyWasNotConsumed;
	}

// --- AppUi: володіє контейнером і тіком емуляції -----------------------------

class CPs1AppUi : public CAknAppUi
	{
public:
	void ConstructL();
	~CPs1AppUi();
	void HandleCommandL(TInt aCommand);
	void HandleResourceChangeL(TInt aType);
private:
	static TInt TickCb(TAny *aSelf);
	TInt Tick();
	void AttachOutput();
	CPs1Container *iContainer;
	CIdle *iIdle;
	TBool iStarted;
	TBool iInTick;
	};

void CPs1AppUi::AttachOutput()
	{
	voutSymbianAttachWindow(iCoeEnv->WsSession(), *iCoeEnv->ScreenDevice(),
		iContainer->FrameWindow(), iCoeEnv->SystemGc());
	}

void CPs1AppUi::ConstructL()
	{
	BaseConstructL(EAknEnableSkin);
	ps1Log("gui", "appui constructed");

	// Повний екран: гра дістає весь дисплей, а не ClientRect без статусної панелі
	// та CBA. Ландшафт задаємо явно - інакше геометрія вікна змінюється посеред
	// гри за акселерометром.
	CEikStatusPane *sp = StatusPane();
	if (sp) sp->MakeVisible(EFalse);
	if (Cba()) Cba()->MakeVisible(EFalse);
	TRAP_IGNORE(SetOrientationL(EAppUiOrientationLandscape));

	iContainer = CPs1Container::NewL(ApplicationRect());
	AddToStackL(iContainer);

	AttachOutput();
	ps1Log("gui", "window attached");

	// Найнижчий пріоритет: події CONE (клавіші, перемальовування, вихід)
	// обслуговуються між кадрами, а не після кінця сесії.
	iIdle = CIdle::NewL(CActive::EPriorityIdle);
	iIdle->Start(TCallBack(TickCb, this));
	}

CPs1AppUi::~CPs1AppUi()
	{
	delete iIdle;
	// Порядок важливий: вивід відв'язується від вікна ДО знищення контейнера.
	ps1GuiShutdown();
	voutSymbianFinish(0);   // якщо вихід стався ще до старту ядра
	if (iContainer)
		{
		RemoveFromStack(iContainer);
		delete iContainer;
		}
	}

TInt CPs1AppUi::TickCb(TAny *aSelf)
	{
	return static_cast<CPs1AppUi *>(aSelf)->Tick();
	}

TInt CPs1AppUi::Tick()
	{
	// Захист від повторного входу: кадр докручує аудіо через планувальник, а CIdle
	// на той момент уже переозброєний - без прапорця крок емуляції рекурсивно
	// заходив би сам у себе, поки не скінчиться стек.
	if (iInTick)
		return ETrue;
	iInTick = ETrue;

	TInt more = ETrue;
	// Ініціалізація ядра - у першому тіку, а не в ConstructL: вона довга (BIOS,
	// образ диска, відкриття аудіо), а вікно має з'явитись до неї.
	if (!iStarted)
		{
		iStarted = ETrue;
		if (ps1GuiSetup() != KErrNone)
			{
			ps1Log("gui", "emu setup failed - exiting");
			more = EFalse;
			}
		}
	else if (!ps1GuiStep())
		{
		more = EFalse;
		}

	iInTick = EFalse;
	if (!more)
		Exit();
	return more;
	}

// Поворот екрана міняє розмір вікна, а бітмап кадру створено під попередній -
// перестворюємо його разом із геометрією контейнера.
void CPs1AppUi::HandleResourceChangeL(TInt aType)
	{
	CAknAppUi::HandleResourceChangeL(aType);
	if (aType == KEikDynamicLayoutVariantSwitch && iContainer)
		{
		iContainer->SetRect(ApplicationRect());
		AttachOutput();
		}
	}

void CPs1AppUi::HandleCommandL(TInt aCommand)
	{
	switch (aCommand)
		{
		case EEikCmdExit:
		case EAknSoftkeyExit:
			// Спершу просимо цикл кадрів завершитись, решту доробить деструктор.
			ps1GuiRequestExit();
			Exit();
			break;
		default:
			break;
		}
	}

// --- Документ і застосунок ---------------------------------------------------

class CPs1Document : public CAknDocument
	{
public:
	CPs1Document(CEikApplication &aApp) : CAknDocument(aApp) { }
private:
	CEikAppUi *CreateAppUiL() { return new (ELeave) CPs1AppUi; }
	};

class CPs1Application : public CAknApplication
	{
private:
	CApaDocument *CreateDocumentL() { return new (ELeave) CPs1Document(*this); }
	TUid AppDllUid() const { return KUidPs1App; }
	};

LOCAL_C CApaApplication *NewApplication()
	{
	return new CPs1Application;
	}

// Реєстрація в apparc - передумова запуску: EikStart резолвить застосунок через
// неї, і поки її не було, фреймворк тихо вмирав із KErrNotFound ще до нашого коду.
// Лишаємо як два рядки в лозі: заразом видно, ЯКИЙ саме бінарник запустився -
// деплой кладе свіжий поверх встановленого, і сплутати версії легко.
static void probeStartupEnv()
	{
	CTrapCleanup *cleanup = CTrapCleanup::New();
	if (!cleanup)
		return;

	RApaLsSession ls;
	if (ls.Connect() == KErrNone)
		{
		TApaAppInfo info;
		const TInt gi = ls.GetAppInfo(info, KUidPs1App);
		ps1LogInt("probe", "getappinfo", gi);
		if (gi == KErrNone)
			{
			TBuf8<256> n8;
			n8.Copy(info.iFullName.Left(200));
			ps1Log("probe", (const char *)n8.PtrZ());
			}
		ls.Close();
		}

	delete cleanup;
	}

// Точка входу гілки GUI. Не повертається, доки застосунок не закрито.
extern "C" int ps1AppRun(void)
	{
	probeStartupEnv();
	return EikStart::RunApplication(NewApplication);
	}
