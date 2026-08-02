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
#include <f32file.h>
#include <badesca.h>
#include <gdi.h>

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
int  voutSymbianGlActive(void);
void ps1CfgApply(const char *aDisc, int aInterp, int aFrameskip, int aIlace,
	int aLight, int aBlend, int aPixSkip, int aGteNf, int aSound, int aGl);
}

// Прив'язка виведення до вікна CONE (vout_symbian.cpp). Без extern "C": типи Symbian.
void voutSymbianAttachWindow(RWsSession &aWs, CWsScreenDevice &aDev, RWindow &aWin,
                             CWindowGc &aGc);

// Той самий UID, що в stage1.mmp і stage1_reg.rss.
static const TUid KUidPs1App = { 0xE0001002 };

// --- Налаштування -----------------------------------------------------------

// Меню малюємо самі, а не через CAknSettingItemList: власне вікно й обробка
// клавіш у нас уже є, а список AVKON потягнув би ресурсні визначення й
// перебудову ієрархії контролів заради того самого результату.
enum TCfgItem
	{
	ECfgGame = 0, ECfgCpu, ECfgSkip, ECfgIlace, ECfgLight, ECfgBlend,
	ECfgPixSkip, ECfgGteNf, ECfgSound, ECfgGl, ECfgCount
	};

_LIT(KCfgPath, "E:\\ps1_settings.txt");
_LIT(KImgDir, "E:\\");

class TPs1Cfg
	{
public:
	void SetDefaults();
	void Load();
	void Save() const;
	TInt &Value(TInt aItem);
	TInt iGame, iInterp, iSkip, iIlace, iLight, iBlend, iPixSkip, iGteNf, iSound, iGl;
	};

void TPs1Cfg::SetDefaults()
	{
	iGame = iInterp = iSkip = iIlace = iPixSkip = iGteNf = iGl = 0;
	// Освітлення й напівпрозорість типово УВІМКНЕНІ: без них текстури малюються
	// без затінення, і це не пришвидшення, а брак. Вимкнути можна тут же.
	iLight = iBlend = 1;
	iSound = 1;
	}

TInt &TPs1Cfg::Value(TInt aItem)
	{
	switch (aItem)
		{
		case ECfgCpu:     return iInterp;
		case ECfgSkip:    return iSkip;
		case ECfgIlace:   return iIlace;
		case ECfgLight:   return iLight;
		case ECfgBlend:   return iBlend;
		case ECfgPixSkip: return iPixSkip;
		case ECfgGteNf:   return iGteNf;
		case ECfgSound:   return iSound;
		case ECfgGl:      return iGl;
		default:          return iGame;
		}
	}

void TPs1Cfg::Load()
	{
	SetDefaults();
	RFs fs;
	if (fs.Connect() != KErrNone) return;
	RFile f;
	if (f.Open(fs, KCfgPath, EFileRead | EFileShareAny) == KErrNone)
		{
		TBuf8<128> b;
		f.Read(b);
		f.Close();
		TLex8 lex(b);
		for (TInt i = 0; i < ECfgCount; i++)
			{
			lex.SkipSpace();
			TInt v = 0;
			if (lex.Val(v) != KErrNone) break;
			Value(i) = v;
			}
		}
	fs.Close();
	}

void TPs1Cfg::Save() const
	{
	RFs fs;
	if (fs.Connect() != KErrNone) return;
	RFile f;
	if (f.Replace(fs, KCfgPath, EFileWrite | EFileShareAny) == KErrNone)
		{
		TBuf8<128> b;
		b.Format(_L8("%d %d %d %d %d %d %d %d %d %d\n"), iGame, iInterp, iSkip,
			iIlace, iLight, iBlend, iPixSkip, iGteNf, iSound, iGl);
		f.Write(b);
		f.Close();
		}
	fs.Close();
	}

// --- Контейнер: вікно для кадру + меню + клавіші -----------------------------

class CPs1Container : public CCoeControl
	{
public:
	static CPs1Container *NewL(const TRect &aRect);
	~CPs1Container();
	TKeyResponse OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType);
	// CCoeControl::Window() захищений, а виведенню потрібне саме це вікно.
	RWindow &FrameWindow() { return Window(); }
	TBool MenuActive() const { return iMenu; }
	void ApplyAndStart();
private:
	void ConstructL(const TRect &aRect);
	void Draw(const TRect &aRect) const;
	void ScanImagesL();
	void ItemText(TInt aItem, TDes &aOut) const;
	CDesCArray *iImages;    // знайдені образи дисків
	TBool iMenu;
	TInt iSel;
	TPs1Cfg iCfg;
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
	iImages = new (ELeave) CDesCArrayFlat(8);
	ScanImagesL();
	iCfg.Load();
	if (iCfg.iGame >= iImages->Count()) iCfg.iGame = 0;
	iMenu = ETrue;
	ActivateL();
	}

CPs1Container::~CPs1Container()
	{
	delete iImages;
	}

// Образи дисків у корені карти. .cue не показуємо: cdriso виводить його з .bin
// за розширенням, і окремий пункт лише плутав би.
void CPs1Container::ScanImagesL()
	{
	RFs fs;
	if (fs.Connect() != KErrNone) return;
	CDir *dir = NULL;
	if (fs.GetDir(KImgDir, KEntryAttNormal, ESortByName, dir) == KErrNone && dir)
		{
		for (TInt i = 0; i < dir->Count(); i++)
			{
			const TEntry &e = (*dir)[i];
			TFileName n = e.iName;
			n.LowerCase();
			if (n.Right(4) == _L(".bin") || n.Right(4) == _L(".img") ||
			    n.Right(4) == _L(".iso"))
				iImages->AppendL(e.iName);
			}
		}
	delete dir;
	fs.Close();
	}

void CPs1Container::ItemText(TInt aItem, TDes &aOut) const
	{
	_LIT(KOff, "off"); _LIT(KOn, "on");
	TPs1Cfg &c = const_cast<TPs1Cfg &>(iCfg);
	switch (aItem)
		{
		case ECfgGame:
			if (iImages->Count())
				aOut.Format(_L("Game:        %S"), &(*iImages)[c.iGame]);
			else
				aOut.Copy(_L("Game:        <no image on E:>"));
			break;
		case ECfgCpu:
			aOut.Format(_L("CPU:         %S"), c.iInterp ?
				&_L("interpreter") : &_L("dynarec"));
			break;
		case ECfgSkip:
			aOut.Format(_L("Show every:  %d frame(s)"), c.iSkip + 1);
			break;
		case ECfgIlace:
			aOut.Format(_L("Skip lines:  %S"), c.iIlace ? &KOn() : &KOff());
			break;
		case ECfgLight:
			aOut.Format(_L("Lighting:    %S"), c.iLight ? &KOn() : &KOff());
			break;
		case ECfgBlend:
			aOut.Format(_L("Transparency:%S"), c.iBlend ? &KOn() : &KOff());
			break;
		case ECfgPixSkip:
			aOut.Format(_L("Pixel skip:  %S"), c.iPixSkip ? &KOn() : &KOff());
			break;
		case ECfgGteNf:
			aOut.Format(_L("GTE no flags:%S"), c.iGteNf ? &KOn() : &KOff());
			break;
		case ECfgSound:
			aOut.Format(_L("Sound:       %S"), c.iSound ? &KOn() : &KOff());
			break;
		default:
			aOut.Format(_L("GL output:   %S"), c.iGl ? &KOn() : &KOff());
			break;
		}
	}

// У режимі меню малюємо самі; у грі - порожньо: кадр кладе блит емулятора поза
// парою BeginRedraw/EndRedraw, і актуального кадру фреймворк не має.
void CPs1Container::Draw(const TRect & /*aRect*/) const
	{
	if (!iMenu)
		return;

	CWindowGc &gc = SystemGc();
	const TRect r = Rect();
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbBlack);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.DrawRect(r);

	const CFont *font = iCoeEnv->NormalFont();
	gc.UseFont(font);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);

	const TInt lh = font->HeightInPixels() + 2;
	TInt y = r.iTl.iY + lh;
	gc.SetPenColor(TRgb(0x80, 0xc0, 0xff));
	gc.DrawText(_L("PS1Emulator"), TPoint(r.iTl.iX + 6, y));
	y += lh + 2;

	for (TInt i = 0; i < ECfgCount; i++)
		{
		TBuf<80> line;
		ItemText(i, line);
		if (i == iSel)
			{
			gc.SetBrushColor(TRgb(0x20, 0x40, 0x80));
			gc.SetPenStyle(CGraphicsContext::ENullPen);
			gc.DrawRect(TRect(r.iTl.iX + 2, y - font->AscentInPixels() - 1,
				r.iBr.iX - 2, y + 2));
			gc.SetPenStyle(CGraphicsContext::ESolidPen);
			}
		gc.SetPenColor(i == iSel ? KRgbWhite : TRgb(0xc0, 0xc0, 0xc0));
		gc.DrawText(line, TPoint(r.iTl.iX + 6, y));
		y += lh;
		}

	y += 4;
	gc.SetPenColor(TRgb(0x90, 0x90, 0x90));
	gc.DrawText(_L("left/right change, OK starts, C exits"), TPoint(r.iTl.iX + 6, y));
	gc.DiscardFont();
	}

// Передає вибране в ядро й віддає керування емулятору.
void CPs1Container::ApplyAndStart()
	{
	iCfg.Save();
	TBuf<256> path;
	if (iImages->Count())
		{
		path.Copy(KImgDir);
		path.Append((*iImages)[iCfg.iGame]);
		}
	TBuf8<256> path8;
	path8.Copy(path);
	ps1CfgApply((const char *)path8.PtrZ(), iCfg.iInterp, iCfg.iSkip, iCfg.iIlace,
		iCfg.iLight, iCfg.iBlend, iCfg.iPixSkip, iCfg.iGteNf, iCfg.iSound, iCfg.iGl);
	iMenu = EFalse;
	}

TKeyResponse CPs1Container::OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKeyDown && aType != EEventKeyUp)
		return EKeyWasNotConsumed;

	// Меню забирає клавіші цілком, поки не почалась гра.
	if (iMenu)
		{
		if (aType != EEventKeyDown)
			return EKeyWasConsumed;
		const TInt sc = aKeyEvent.iScanCode;
		// Вихід мусить працювати і звідси, інакше з меню нема виходу взагалі:
		// нижче гілка поглинає геть усі клавіші.
		if (sc == EStdKeyDevice1 || sc == EStdKeyBackspace || sc == EStdKeyEscape)
			{
			CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
			return EKeyWasConsumed;
			}
		TInt d = 0;
		if (sc == EStdKeyLeftArrow)  d = -1;
		if (sc == EStdKeyRightArrow) d = 1;
		if (sc == EStdKeyUpArrow)    { iSel = (iSel + ECfgCount - 1) % ECfgCount; DrawNow(); }
		if (sc == EStdKeyDownArrow)  { iSel = (iSel + 1) % ECfgCount; DrawNow(); }
		if (d)
			{
			TInt &v = iCfg.Value(iSel);
			if (iSel == ECfgGame)
				{
				const TInt n = iImages->Count();
				if (n) v = (v + n + d) % n;
				}
			else if (iSel == ECfgSkip)
				{
				v += d;
				if (v < 0) v = 3;
				if (v > 3) v = 0;
				}
			else
				v = v ? 0 : 1;
			DrawNow();
			}
		if (sc == EStdKeyDevice3 || sc == EStdKeyEnter || sc == EStdKeyNkpEnter)
			ApplyAndStart();
		return EKeyWasConsumed;
		}

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
	// Чи піднявся EGL, видно лише звідси: далі відкат на BitBlt мовчазний, і без
	// цього рядка "gl не дав приросту" не відрізнити від "gl не увімкнувся".
	ps1LogInt("gui", "window attached, gl=", voutSymbianGlActive());

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

	// Поки відкрите меню, ядро не чіпаємо: вибір диска й режимів має подіяти до
	// ініціалізації. CIdle крутиться далі, щоб CONE обслуговував клавіші.
	if (iContainer && iContainer->MenuActive())
		{
		iInTick = EFalse;
		return ETrue;
		}

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
