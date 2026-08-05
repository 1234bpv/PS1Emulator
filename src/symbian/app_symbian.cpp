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
void ps1ScreenShot(void);
void ps1CfgApply(const char *aDisc, int aInterp, int aIlace,
	int aLight, int aBlend, int aPixSkip, int aGteNf, int aSound, int aGl,
	int aDirty, int aDsa, int aFps, int aGpuSkip, int aCycle, int aSmooth,
	int aFxaa);
// Підмножина, яку можна перемкнути прямо в грі (меню з паузи).
void ps1CfgApplyLive(int aIlace, int aLight, int aBlend, int aPixSkip,
	int aFps, int aGpuSkip, int aCycle, int aSmooth, int aFxaa, int aDirty);
void voutSymbianSetKeyMap(const int *aScan, const int *aAlt, int aCount,
	unsigned int aToggle);
int  voutSymbianPadButtons(void);
int  voutSymbianDefaultKey(int aIdx, int aAlt);
int  voutSymbianPadBit(int aIdx);
void voutSymbianSuspend(int aOn);
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
//
// Порядок пунктів = порядок показу. Розділ пункту задає KItemSection, тож
// переставляти пункти можна лише разом із нею.
enum TCfgItem
	{
	// Пропуску ПОКАЗУ ("Show every") тут навмисно немає: він малює кадр і викидає
	// його, тобто платить за дорогу частину. Заміряно -4,3% проти -22,5% у
	// ECfgGpuSkip. Лишився як зонд командного рядка "showskip".
	ECfgStart = 0, ECfgGame, ECfgProfile,
	ECfgLight, ECfgBlend, ECfgIlace, ECfgPixSkip, ECfgSmooth, ECfgFxaa,
	ECfgDirty, ECfgDsa, ECfgGl,
	ECfgCpu, ECfgGteNf, ECfgGpuSkip, ECfgCycle, ECfgFps,
	ECfgSound,
	ECfgKeys,
	ECfgReset, ECfgExit,
	ECfgCount
	};

enum TCfgSection
	{
	ESecGame = 0, ESecVideo, ESecSpeed, ESecSound, ESecInput, ESecSystem, ESecCount
	};

static const TText *const KSecName[ESecCount] =
	{ _S("GAME"), _S("VIDEO"), _S("SPEED"), _S("SOUND"), _S("CONTROLS"), _S("SYSTEM") };

static const unsigned char KItemSection[ECfgCount] =
	{
	ESecGame, ESecGame, ESecGame,
	ESecVideo, ESecVideo, ESecVideo, ESecVideo, ESecVideo, ESecVideo,
	ESecVideo, ESecVideo, ESecVideo,
	ESecSpeed, ESecSpeed, ESecSpeed, ESecSpeed, ESecSpeed,
	ESecSound,
	ESecInput,
	ESecSystem, ESecSystem
	};

static const TText *const KItemLabel[ECfgCount] =
	{
	_S("Start"), _S("Game"), _S("Per-game profile"),
	_S("Lighting"), _S("Transparency"), _S("Skip lines"), _S("Pixel skip"),
	_S("Smooth scale"), _S("Edge smooth"), _S("Skip repeats"), _S("Direct draw"),
	_S("GL output"),
	_S("CPU"), _S("GTE no flags"), _S("Draw every"), _S("PS1 cycles"),
	_S("FPS meter"),
	_S("Sound"),
	_S("Controls..."),
	_S("Reset settings"), _S("Exit")
	};

// Імена у файлі налаштувань. NULL - пункт-команда, значення в нього немає.
// Читач шукає за іменем, тож додавати й переставляти пункти можна вільно:
// старий файл лишається придатним, відсутні ключі беруть типове значення.
static const char *const KCfgKey[ECfgCount] =
	{
	NULL, NULL, "profile",
	"light", "blend", "ilace", "pixskip", "smooth", "fxaa",
	"dirty", "dsa", "gl",
	"cpu", "gtenf", "gpuskip", "cycle", "fps",
	"sound",
	NULL,
	NULL, NULL
	};

// Потребують повторної ініціалізації ядра або виводу: з меню в грі не діють,
// у списку позначені зірочкою.
static TBool restartOnly(TInt aItem)
	{
	return aItem == ECfgGame || aItem == ECfgCpu || aItem == ECfgGteNf ||
	       aItem == ECfgSound || aItem == ECfgGl || aItem == ECfgDsa;
	}

static TBool isCommand(TInt aItem)
	{
	return aItem == ECfgStart || aItem == ECfgGame || aItem == ECfgKeys ||
	       aItem == ECfgReset || aItem == ECfgExit;
	}

// Множник тактів PS1. Типове значення upstream - CYCLE_MULT_DEFAULT (175); більше
// означає "процесор PS1 повільніший", тобто менше роботи нам і менше плавності грі.
const TInt KCycleDefault = 175;
const TInt KCycleMin = 100;
const TInt KCycleMax = 300;
const TInt KCycleStep = 25;

// Із запасом до кількості кнопок, яку віддає vout: розкладка описана там.
const TInt KPadMax = 16;

_LIT(KCfgPath, "E:\\ps1_settings.txt");
_LIT(KProfDir, "E:\\ps1cfg\\");
_LIT(KImgDir, "E:\\");

class TPs1Cfg
	{
public:
	void SetDefaults();
	TInt &Value(TInt aItem);
	TInt iInterp, iIlace, iLight, iBlend, iPixSkip, iGteNf, iSound, iGl;
	TInt iDirty, iDsa, iFps, iGpuSkip, iCycle, iSmooth, iFxaa, iProfile;
	TInt iKeyScan[KPadMax], iKeyAlt[KPadMax], iKeyHold[KPadMax];
	TInt iDummy;    // приймач для пунктів-команд, у яких значення немає
	};

void TPs1Cfg::SetDefaults()
	{
	iInterp = iIlace = iPixSkip = iGteNf = iGl = iFps = 0;
	iGpuSkip = 0;
	iDummy = 0;
	iCycle = KCycleDefault;
	// Усереднення при зменшенні типово УВІМКНЕНЕ: без нього в режимах 640x*
	// проріджування з'їдає тонкі штрихи шрифту, і це брак, а не швидкодія.
	iSmooth = 1;
	// Згладжування країв типово ВИМКНЕНЕ: заміряно -17% кадрів на трасі, і на
	// екрані 320x240 виграш видно не в кожній грі. Вмикається свідомо.
	iFxaa = 0;
	// Освітлення й напівпрозорість типово УВІМКНЕНІ: без них текстури малюються
	// без затінення, і це не пришвидшення, а брак. Вимкнути можна тут же.
	iLight = iBlend = 1;
	iSound = 1;
	// Окремий набір налаштувань на гру: розкладка й швидкісні хаки в гонці та в
	// рпг потрібні різні, а перебирати їх щоразу вручну - те саме меню двічі.
	iProfile = 1;
	// Показ за латчем gpulib: заміряно -6,7 мс на кадр без втрати картинки.
	iDirty = 1;
	// Прямий запис у буфер екрана: заміряно -0,93 мс на кадр. Ламався він через
	// Update() без регіону (bitgdi не знав про наш запис, тож нічого не проштовхував);
	// з явним регіоном картинка коректна - перевірено на N95 8GB.
	iDsa = 1;
	for (TInt i = 0; i < KPadMax; i++)
		{
		iKeyScan[i] = voutSymbianDefaultKey(i, 0);
		iKeyAlt[i]  = voutSymbianDefaultKey(i, 1);
		iKeyHold[i] = 0;
		}
	}

TInt &TPs1Cfg::Value(TInt aItem)
	{
	switch (aItem)
		{
		case ECfgCpu:     return iInterp;
		case ECfgIlace:   return iIlace;
		case ECfgLight:   return iLight;
		case ECfgBlend:   return iBlend;
		case ECfgPixSkip: return iPixSkip;
		case ECfgGteNf:   return iGteNf;
		case ECfgSound:   return iSound;
		case ECfgGl:      return iGl;
		case ECfgDirty:   return iDirty;
		case ECfgDsa:     return iDsa;
		case ECfgFps:     return iFps;
		case ECfgGpuSkip: return iGpuSkip;
		case ECfgCycle:   return iCycle;
		case ECfgSmooth:  return iSmooth;
		case ECfgFxaa:    return iFxaa;
		case ECfgProfile: return iProfile;
		default:          return iDummy;
		}
	}

// --- Клавіші: імена для показу ----------------------------------------------

static const TText *const KBtnName[14] =
	{
	_S("Up"), _S("Down"), _S("Left"), _S("Right"),
	_S("Cross"), _S("Circle"), _S("Square"), _S("Triangle"),
	_S("L1"), _S("R1"), _S("L2"), _S("R2"), _S("Select"), _S("Start")
	};

static void keyName(TInt aScan, TDes &aOut)
	{
	switch (aScan)
		{
		case EStdKeyUpArrow:     aOut.Copy(_L("Up"));   return;
		case EStdKeyDownArrow:   aOut.Copy(_L("Down")); return;
		case EStdKeyLeftArrow:   aOut.Copy(_L("Left")); return;
		case EStdKeyRightArrow:  aOut.Copy(_L("Right"));return;
		case EStdKeyDevice3:     aOut.Copy(_L("OK"));   return;
		case EStdKeyDevice0:     aOut.Copy(_L("LSK"));  return;
		case EStdKeyDevice1:     aOut.Copy(_L("RSK"));  return;
		case EStdKeyBackspace:   aOut.Copy(_L("Clear"));return;
		case EStdKeyHash:        aOut.Copy(_L("#"));    return;
		case EStdKeyNkpAsterisk: aOut.Copy(_L("*"));    return;
		case EStdKeyIncVolume:   aOut.Copy(_L("Vol+")); return;
		case EStdKeyDecVolume:   aOut.Copy(_L("Vol-")); return;
		case EStdKeyApplication0:aOut.Copy(_L("Menu")); return;
		default: break;
		}
	if (aScan < 0)
		aOut.Copy(_L("-"));
	else if (aScan >= 32 && aScan < 127)
		aOut.Format(_L("%c"), aScan);
	else
		aOut.Format(_L("k%d"), aScan);
	}

// --- Контейнер: вікно для кадру + меню + клавіші -----------------------------

enum TMenuPage { EPageNone = 0, EPageMain, EPageFiles, EPageKeys };

class CPs1Container : public CCoeControl
	{
public:
	static CPs1Container *NewL(const TRect &aRect);
	~CPs1Container();
	TKeyResponse OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType);
	// CCoeControl::Window() захищений, а виведенню потрібне саме це вікно.
	RWindow &FrameWindow() { return Window(); }
	TBool MenuActive() const { return iPage != EPageNone; }
	void ApplyAndStart();
private:
	void ConstructL(const TRect &aRect);
	void Draw(const TRect &aRect) const;

	// малювання
	TRect DrawChrome(CWindowGc &gc, const TDesC &aTitle, const TDesC &aHint) const;
	void DrawMain(CWindowGc &gc, const TRect &aBody) const;
	void DrawKeys(CWindowGc &gc, const TRect &aBody) const;
	void DrawFiles(CWindowGc &gc, const TRect &aBody) const;
	void DrawRow(CWindowGc &gc, const TRect &aBody, TInt aY, TInt aLh,
	             const TDesC &aLeft, const TDesC &aRight, TBool aSel) const;
	void ItemLabel(TInt aItem, TDes &aOut) const;
	void ItemValue(TInt aItem, TDes &aOut) const;

	// сторінки
	TKeyResponse MainKeyL(TInt aScan);
	TKeyResponse FilesKeyL(TInt aScan);
	TKeyResponse KeysKeyL(TInt aScan);
	void OpenMenu();
	void CloseMenu();

	// файловий огляд
	void ScanDirL();
	void BrowseEnterL();
	void BrowseUpL();
	TBool AutoPickGameL();

	// збереження
	void ProfilePath(TDes &aOut) const;
	void LoadCfg(const TDesC &aPath);
	void ParseCfg(const TDesC8 &aTxt);
	void SaveCfg(const TDesC &aPath, TBool aWithGame) const;
	void SaveAll() const;
	void SetGameL(const TDesC &aPath);
	void ApplyKeyMap() const;

	CDesCArray *iBrowse;      // вміст поточного каталогу; каталоги з '\' у кінці
	TFileName iBrowsePath;    // порожній = список дисків
	TFileName iGamePath;
	TInt iPage;
	TInt iSel;                // вибраний пункт головної сторінки
	TInt iBrowseSel;
	TInt iKeySel;
	TInt iCapture;            // кнопка, для якої чекаємо натискання (-1 - ні)
	TInt iConfirm;            // пункт, що чекає підтвердження (-1 - ні)
	TInt iPadCount;
	TBool iStartedOnce;       // конфіг уже віддано ядру, далі лише живі зміни
	TBool iCliMode;           // запуск із аргументами: меню в грі не чіпаємо
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
	iBrowse = new (ELeave) CDesCArrayFlat(16);
	iPadCount = voutSymbianPadButtons();
	if (iPadCount > KPadMax) iPadCount = KPadMax;
	iCapture = -1;
	iConfirm = -1;

	iCfg.SetDefaults();
	LoadCfg(KCfgPath);
	// Профіль гри перекриває загальні налаштування - але тільки якщо він є.
	if (iCfg.iProfile && iGamePath.Length())
		{
		TFileName p;
		ProfilePath(p);
		LoadCfg(p);
		}
	if (!iGamePath.Length())
		AutoPickGameL();
	ApplyKeyMap();

	iBrowsePath = KImgDir;

	// Меню лише при запуску з апарата. Із аргументами (деплой, бенчмарк) параметри
	// задає командний рядок, і stage1 розбирає її сам - меню тут чекало б натискання,
	// якого в записаному сліді вводу немає, і прогін стояв би до вбивства демоном.
	TBuf<256> cl;
	User::CommandLine(cl);
	cl.Trim();
	iCliMode = (cl.Length() != 0);
	iPage = iCliMode ? EPageNone : EPageMain;
	ActivateL();
	}

CPs1Container::~CPs1Container()
	{
	delete iBrowse;
	}

// Перший образ у корені карти - щоб перший запуск не вимагав походу в огляд.
TBool CPs1Container::AutoPickGameL()
	{
	iBrowsePath = KImgDir;
	ScanDirL();
	for (TInt i = 0; i < iBrowse->Count(); i++)
		{
		const TPtrC n = (*iBrowse)[i];
		if (n.Right(1) != _L("\\"))
			{
			iGamePath = KImgDir;
			iGamePath.Append(n);
			return ETrue;
			}
		}
	return EFalse;
	}

// --- Файловий огляд ---------------------------------------------------------

// .cue не показуємо: cdriso виводить його з .bin за розширенням, і окремий пункт
// лише плутав би. Стиснуті образи ядро відкриває саме: .pbp блоками по 16 секторів,
// .cbn/.cbin (контейнер CISO) - посекторно. CHD не підтримано: HAVE_CHD не задано.
static TBool isImageName(const TDesC &aName)
	{
	if (aName.Length() < 5)
		return EFalse;
	TBuf<8> ext;
	ext.Copy(aName.Right(5));
	ext.LowerCase();
	if (ext == _L(".cbin"))
		return ETrue;
	ext.Copy(aName.Right(4));
	ext.LowerCase();
	return ext == _L(".bin") || ext == _L(".img") || ext == _L(".iso") ||
	       ext == _L(".pbp") || ext == _L(".cbn");
	}

void CPs1Container::ScanDirL()
	{
	iBrowse->Reset();
	iBrowseSel = 0;
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	CleanupClosePushL(fs);

	if (iBrowsePath.Length() == 0)
		{
		TDriveList dl;
		if (fs.DriveList(dl) == KErrNone)
			for (TInt d = 0; d < KMaxDrives; d++)
				{
				if (!dl[d])
					continue;
				// Порожній слот картки лишається в списку дисків, але каталогу не
				// має - без цієї перевірки огляд заводив у глухий кут.
				TVolumeInfo vi;
				if (fs.Volume(vi, d) != KErrNone)
					continue;
				TBuf<8> n;
				n.Copy(TDriveUnit(d).Name());
				n.Append('\\');
				iBrowse->AppendL(n);
				}
		}
	else
		{
		iBrowse->AppendL(_L("..\\"));
		CDir *dir = NULL;
		TFileName spec = iBrowsePath;
		if (spec.Length() + 1 < spec.MaxLength())
			spec.Append('*');
		if (fs.GetDir(spec, KEntryAttNormal | KEntryAttDir, ESortByName, dir) == KErrNone
		    && dir)
			{
			CleanupStack::PushL(dir);
			for (TInt i = 0; i < dir->Count(); i++)
				if ((*dir)[i].IsDir())
					{
					TFileName n = (*dir)[i].iName;
					if (n.Length() + 1 < n.MaxLength())
						n.Append('\\');
					iBrowse->AppendL(n);
					}
			for (TInt i = 0; i < dir->Count(); i++)
				if (!(*dir)[i].IsDir() && isImageName((*dir)[i].iName))
					iBrowse->AppendL((*dir)[i].iName);
			CleanupStack::PopAndDestroy(dir);
			}
		}
	CleanupStack::PopAndDestroy(&fs);
	}

void CPs1Container::BrowseUpL()
	{
	if (iBrowsePath.Length() <= 3)
		{
		iBrowsePath.Zero();     // вище кореня диска - список дисків
		ScanDirL();
		return;
		}
	const TInt p = iBrowsePath.Left(iBrowsePath.Length() - 1).LocateReverse('\\');
	iBrowsePath.SetLength(p + 1);
	ScanDirL();
	}

void CPs1Container::BrowseEnterL()
	{
	if (iBrowseSel < 0 || iBrowseSel >= iBrowse->Count())
		return;
	TFileName n = (*iBrowse)[iBrowseSel];
	if (n.Right(1) == _L("\\"))
		{
		if (n == _L("..\\"))
			BrowseUpL();
		else if (iBrowsePath.Length() == 0)
			{ iBrowsePath = n; ScanDirL(); }
		else if (iBrowsePath.Length() + n.Length() < iBrowsePath.MaxLength())
			{ iBrowsePath.Append(n); ScanDirL(); }
		return;
		}
	if (iBrowsePath.Length() + n.Length() >= iBrowsePath.MaxLength())
		return;
	TFileName full = iBrowsePath;
	full.Append(n);
	SetGameL(full);
	iPage = EPageMain;
	}

// Зміна гри при увімкнених профілях: спершу зберігаємо налаштування попередньої,
// інакше вони губляться мовчки, а вже потім підтягуємо профіль нової.
void CPs1Container::SetGameL(const TDesC &aPath)
	{
	if (iCfg.iProfile && iGamePath.Length())
		{
		TFileName p;
		ProfilePath(p);
		SaveCfg(p, EFalse);
		}
	iGamePath = aPath;
	if (iCfg.iProfile)
		{
		TFileName p;
		ProfilePath(p);
		LoadCfg(p);
		ApplyKeyMap();
		}
	}

// --- Збереження -------------------------------------------------------------

void CPs1Container::ProfilePath(TDes &aOut) const
	{
	TFileName n = iGamePath;
	const TInt s = n.LocateReverse('\\');
	if (s >= 0)
		n.Delete(0, s + 1);
	const TInt d = n.LocateReverse('.');
	if (d > 0)
		n.SetLength(d);
	if (n.Length() > 32)
		n.SetLength(32);
	aOut.Copy(KProfDir);
	for (TInt i = 0; i < n.Length(); i++)
		{
		const TChar c = n[i];
		aOut.Append(c.IsAlphaDigit() ? c : TChar('_'));
		}
	aOut.Append(_L(".txt"));
	}

void CPs1Container::LoadCfg(const TDesC &aPath)
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	RFile f;
	if (f.Open(fs, aPath, EFileRead | EFileShareAny) == KErrNone)
		{
		// На стеку тримати не можна: рядків тепер під сорок, а стек головного
		// потоку тут спільний із кадровим тіком.
		HBufC8 *buf = HBufC8::New(4096);
		if (buf)
			{
			TPtr8 p(buf->Des());
			if (f.Read(p) == KErrNone)
				ParseCfg(p);
			delete buf;
			}
		f.Close();
		}
	fs.Close();
	}

void CPs1Container::ParseCfg(const TDesC8 &aTxt)
	{
	TInt pos = 0;
	while (pos < aTxt.Length())
		{
		const TPtrC8 rest = aTxt.Mid(pos);
		const TInt nl = rest.Locate('\n');
		TPtrC8 line = (nl < 0) ? rest : rest.Left(nl);
		pos += (nl < 0) ? rest.Length() : nl + 1;
		while (line.Length() && (line[line.Length() - 1] == '\r' ||
		                         line[line.Length() - 1] == ' '))
			line.Set(line.Left(line.Length() - 1));
		const TInt eq = line.Locate('=');
		if (eq <= 0)
			continue;
		const TPtrC8 key = line.Left(eq);
		const TPtrC8 val = line.Mid(eq + 1);

		if (key == _L8("game"))
			{
			iGamePath.Copy(val.Left(iGamePath.MaxLength()));
			continue;
			}
		if (key.Length() > 3 && key.Left(3) == _L8("btn"))
			{
			TLex8 lk(key.Mid(3));
			TInt b = -1;
			if (lk.Val(b) != KErrNone || b < 0 || b >= KPadMax)
				continue;
			TLex8 lv(val);
			TInt scan = -1, alt = -1, hold = 0;
			lv.Val(scan);
			lv.SkipSpace(); lv.Val(alt);
			lv.SkipSpace(); lv.Val(hold);
			iCfg.iKeyScan[b] = scan;
			iCfg.iKeyAlt[b]  = alt;
			iCfg.iKeyHold[b] = hold ? 1 : 0;
			continue;
			}
		for (TInt i = 0; i < ECfgCount; i++)
			if (KCfgKey[i] && key == TPtrC8((const TUint8 *)KCfgKey[i]))
				{
				TLex8 lv(val);
				TInt v = 0;
				if (lv.Val(v) == KErrNone)
					iCfg.Value(i) = v;
				break;
				}
		}
	}

void CPs1Container::SaveCfg(const TDesC &aPath, TBool aWithGame) const
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	// Каталог профілів може ще не існувати; помилку "вже є" ігноруємо.
	fs.MkDirAll(aPath);
	RFile f;
	if (f.Replace(fs, aPath, EFileWrite | EFileShareAny) == KErrNone)
		{
		TPs1Cfg &c = const_cast<TPs1Cfg &>(iCfg);
		TBuf8<KMaxFileName + 16> line;
		if (aWithGame && iGamePath.Length())
			{
			TBuf8<KMaxFileName> g;
			g.Copy(iGamePath);
			line.Format(_L8("game=%S\n"), &g);
			f.Write(line);
			}
		for (TInt i = 0; i < ECfgCount; i++)
			if (KCfgKey[i])
				{
				line.Format(_L8("%s=%d\n"), KCfgKey[i], c.Value(i));
				f.Write(line);
				}
		for (TInt b = 0; b < iPadCount; b++)
			{
			line.Format(_L8("btn%d=%d %d %d\n"), b, iCfg.iKeyScan[b],
				iCfg.iKeyAlt[b], iCfg.iKeyHold[b]);
			f.Write(line);
			}
		f.Close();
		}
	fs.Close();
	}

void CPs1Container::SaveAll() const
	{
	SaveCfg(KCfgPath, ETrue);
	if (iCfg.iProfile && iGamePath.Length())
		{
		TFileName p;
		ProfilePath(p);
		SaveCfg(p, EFalse);
		}
	}

void CPs1Container::ApplyKeyMap() const
	{
	unsigned int tog = 0;
	for (TInt i = 0; i < iPadCount; i++)
		if (iCfg.iKeyHold[i])
			{
			const TInt bit = voutSymbianPadBit(i);
			if (bit >= 0)
				tog |= (1u << bit);
			}
	voutSymbianSetKeyMap(iCfg.iKeyScan, iCfg.iKeyAlt, iPadCount, tog);
	}

// --- Тексти пунктів ---------------------------------------------------------

void CPs1Container::ItemLabel(TInt aItem, TDes &aOut) const
	{
	if (aItem == ECfgStart)
		{
		aOut.Copy(iStartedOnce ? _L("Resume") : _L("Start"));
		return;
		}
	aOut.Copy(TPtrC(KItemLabel[aItem]));
	// Зірочка лише в грі: до старту діє геть усе, і позначка збивала б з пантелику.
	if (iStartedOnce && restartOnly(aItem))
		aOut.Append(_L(" *"));
	}

void CPs1Container::ItemValue(TInt aItem, TDes &aOut) const
	{
	_LIT(KOff, "off"); _LIT(KOn, "on");
	TPs1Cfg &c = const_cast<TPs1Cfg &>(iCfg);
	aOut.Zero();
	switch (aItem)
		{
		case ECfgStart:
			break;
		case ECfgGame:
			if (iGamePath.Length())
				{
				const TInt s = iGamePath.LocateReverse('\\');
				aOut.Copy(iGamePath.Mid(s + 1).Left(aOut.MaxLength()));
				}
			else
				aOut.Copy(_L("<none>"));
			break;
		case ECfgCpu:
			aOut.Copy(c.iInterp ? _L("interpreter") : _L("dynarec"));
			break;
		// Пропуск САМОЇ растеризації: кожен (N+1)-й кадр малюємо, решту ні.
		case ECfgGpuSkip:
			if (c.iGpuSkip)
				aOut.Format(_L("%d frames"), c.iGpuSkip + 1);
			else
				aOut.Copy(_L("frame"));
			break;
		// Згладжування країв у 3D. Більше число - сильніше; всередині це нижчий
		// поріг контрасту, тобто фільтр бере дедалі слабші межі.
		case ECfgFxaa:
			if (c.iFxaa)
				aOut.Format(_L("%d"), c.iFxaa);
			else
				aOut.Copy(KOff);
			break;
		case ECfgCycle:
			if (c.iCycle == KCycleDefault)
				aOut.Format(_L("%d (default)"), c.iCycle);
			else
				aOut.Format(_L("%d"), c.iCycle);
			break;
		case ECfgKeys:
			aOut.Copy(_L("press OK"));
			break;
		case ECfgReset:
			aOut.Copy(iConfirm == ECfgReset ? _L("OK to confirm") : _L("press OK"));
			break;
		case ECfgExit:
			aOut.Copy(_L("press OK"));
			break;
		default:
			aOut.Copy(c.Value(aItem) ? KOn() : KOff());
			break;
		}
	}

// --- Малювання ---------------------------------------------------------------

// Шапка, тіло списку й рядок підказки. Повертає прямокутник під список.
TRect CPs1Container::DrawChrome(CWindowGc &gc, const TDesC &aTitle,
                                const TDesC &aHint) const
	{
	const TRect r = Rect();
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + 3;
	const TInt asc = font->AscentInPixels();

	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushColor(TRgb(0x08, 0x0c, 0x14));
	gc.DrawRect(r);
	gc.SetBrushColor(TRgb(0x16, 0x2c, 0x50));
	gc.DrawRect(TRect(r.iTl.iX, r.iTl.iY, r.iBr.iX, r.iTl.iY + lh + 4));

	gc.UseFont(font);
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(TRgb(0xd8, 0xe8, 0xff));
	gc.DrawText(aTitle, TPoint(r.iTl.iX + 6, r.iTl.iY + asc + 2));
	gc.SetPenColor(TRgb(0x88, 0x90, 0xa0));
	gc.DrawText(aHint, TPoint(r.iTl.iX + 6, r.iBr.iY - 4));

	return TRect(r.iTl.iX, r.iTl.iY + lh + 8, r.iBr.iX, r.iBr.iY - lh - 4);
	}

void CPs1Container::DrawRow(CWindowGc &gc, const TRect &aBody, TInt aY, TInt aLh,
                            const TDesC &aLeft, const TDesC &aRight, TBool aSel) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt asc = font->AscentInPixels();
	if (aSel)
		{
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetBrushColor(TRgb(0x20, 0x48, 0x88));
		gc.SetPenStyle(CGraphicsContext::ENullPen);
		gc.DrawRect(TRect(aBody.iTl.iX + 2, aY - asc - 1, aBody.iBr.iX - 2,
			aY - asc - 1 + aLh));
		gc.SetPenStyle(CGraphicsContext::ESolidPen);
		}
	gc.SetPenColor(aSel ? KRgbWhite : TRgb(0xc8, 0xc8, 0xd0));
	gc.DrawText(aLeft, TPoint(aBody.iTl.iX + 10, aY));
	if (aRight.Length())
		{
		gc.SetPenColor(aSel ? TRgb(0xff, 0xdc, 0x88) : TRgb(0x8c, 0xb0, 0xd8));
		const TInt w = font->TextWidthInPixels(aRight);
		gc.DrawText(aRight, TPoint(aBody.iBr.iX - 10 - w, aY));
		}
	}

void CPs1Container::DrawMain(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + 3;
	TInt rows = aBody.Height() / lh;
	if (rows < 1) rows = 1;

	// Заголовки розділів беруть участь у прокрутці як звичайні рядки, інакше
	// вибраний пункт біля межі вікна виїжджав би за екран.
	const TInt total = ECfgCount + ESecCount;
	const TInt selRow = iSel + KItemSection[iSel] + 1;
	TInt first = selRow - rows / 2;
	if (first > total - rows) first = total - rows;
	if (first < 0) first = 0;

	TInt y = aBody.iTl.iY + font->AscentInPixels();
	TInt row = 0;
	for (TInt sec = 0; sec < ESecCount; sec++)
		{
		if (row >= first && row < first + rows)
			{
			gc.SetPenColor(TRgb(0x58, 0x9c, 0xe0));
			gc.DrawText(TPtrC(KSecName[sec]), TPoint(aBody.iTl.iX + 4, y));
			y += lh;
			}
		row++;
		for (TInt i = 0; i < ECfgCount; i++)
			{
			if (KItemSection[i] != sec)
				continue;
			if (row >= first && row < first + rows)
				{
				TBuf<40> label;
				TBuf<48> value;
				ItemLabel(i, label);
				ItemValue(i, value);
				DrawRow(gc, aBody, y, lh, label, value, i == iSel);
				y += lh;
				}
			row++;
			}
		}
	}

void CPs1Container::DrawKeys(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + 3;
	TInt rows = aBody.Height() / lh;
	if (rows < 1) rows = 1;
	TInt first = iKeySel - rows / 2;
	if (first > iPadCount - rows) first = iPadCount - rows;
	if (first < 0) first = 0;

	TInt y = aBody.iTl.iY + font->AscentInPixels();
	for (TInt i = first; i < first + rows && i < iPadCount; i++)
		{
		TBuf<24> name;
		name.Copy(TPtrC(KBtnName[i]));
		TBuf<40> value;
		if (iCapture == i)
			value.Copy(_L("press a key"));
		else
			{
			keyName(iCfg.iKeyScan[i], value);
			if (iCfg.iKeyAlt[i] >= 0)
				{
				TBuf<16> alt;
				keyName(iCfg.iKeyAlt[i], alt);
				value.Append(_L(" / "));
				value.Append(alt);
				}
			if (iCfg.iKeyHold[i])
				value.Append(_L("   hold"));
			}
		DrawRow(gc, aBody, y, lh, name, value, i == iKeySel);
		y += lh;
		}
	}

void CPs1Container::DrawFiles(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + 3;
	TInt rows = aBody.Height() / lh;
	if (rows < 1) rows = 1;
	const TInt n = iBrowse->Count();
	TInt first = iBrowseSel - rows / 2;
	if (first > n - rows) first = n - rows;
	if (first < 0) first = 0;

	TInt y = aBody.iTl.iY + font->AscentInPixels();
	if (!n)
		{
		gc.SetPenColor(TRgb(0x90, 0x90, 0x98));
		gc.DrawText(_L("(empty)"), TPoint(aBody.iTl.iX + 10, y));
		return;
		}
	for (TInt i = first; i < first + rows && i < n; i++)
		{
		const TPtrC e = (*iBrowse)[i];
		const TBool dir = (e.Right(1) == _L("\\"));
		TBuf<64> name;
		name.Copy(e.Left(name.MaxLength() - 2));
		DrawRow(gc, aBody, y, lh, name, dir ? _L("dir") : _L(""), i == iBrowseSel);
		y += lh;
		}
	}

// У режимі меню малюємо самі; у грі - порожньо: кадр кладе блит емулятора поза
// парою BeginRedraw/EndRedraw, і актуального кадру фреймворк не має.
void CPs1Container::Draw(const TRect & /*aRect*/) const
	{
	if (iPage == EPageNone)
		return;

	CWindowGc &gc = SystemGc();
	if (iPage == EPageFiles)
		{
		TBuf<KMaxFileName> title;
		title.Copy(_L("Select game: "));
		title.Append(iBrowsePath.Length() ? TPtrC(iBrowsePath) : TPtrC(_S("drives")));
		const TRect body = DrawChrome(gc, title.Left(46),
			_L("OK open, Left up, C cancel"));
		DrawFiles(gc, body);
		}
	else if (iPage == EPageKeys)
		{
		const TRect body = DrawChrome(gc, _L("Controls"),
			iCapture >= 0 ? _L("press a key, RSK cancels")
			              : _L("OK bind, L/R hold, C back"));
		DrawKeys(gc, body);
		}
	else
		{
		const TRect body = DrawChrome(gc, _L("PS1Emulator"),
			iStartedOnce ? _L("L/R change, OK resume, * needs restart")
			             : _L("L/R change, OK start, C exit"));
		DrawMain(gc, body);
		}
	gc.DiscardFont();
	}

// --- Керування меню ---------------------------------------------------------

void CPs1Container::OpenMenu()
	{
	if (iPage != EPageNone)
		return;
	voutSymbianSuspend(1);
	iPage = EPageMain;
	iConfirm = -1;
	DrawNow();
	}

void CPs1Container::CloseMenu()
	{
	SaveAll();
	ApplyKeyMap();
	ps1CfgApplyLive(iCfg.iIlace, iCfg.iLight, iCfg.iBlend, iCfg.iPixSkip,
		iCfg.iFps, iCfg.iGpuSkip, iCfg.iCycle, iCfg.iSmooth, iCfg.iFxaa, iCfg.iDirty);
	iPage = EPageNone;
	voutSymbianSuspend(0);
	}

// Передає вибране в ядро й віддає керування емулятору.
void CPs1Container::ApplyAndStart()
	{
	SaveAll();
	ApplyKeyMap();
	TBuf8<KMaxFileName> path8;
	path8.Copy(iGamePath);
	ps1CfgApply((const char *)path8.PtrZ(), iCfg.iInterp, iCfg.iIlace,
		iCfg.iLight, iCfg.iBlend, iCfg.iPixSkip, iCfg.iGteNf, iCfg.iSound, iCfg.iGl,
		iCfg.iDirty, iCfg.iDsa, iCfg.iFps, iCfg.iGpuSkip, iCfg.iCycle, iCfg.iSmooth,
		iCfg.iFxaa);
	iStartedOnce = ETrue;
	iPage = EPageNone;
	}

TKeyResponse CPs1Container::MainKeyL(TInt aScan)
	{
	// Вихід мусить працювати і звідси, інакше з меню до старту нема виходу взагалі.
	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		if (iStartedOnce)
			CloseMenu();
		else
			CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
		return EKeyWasConsumed;
		}

	if (aScan == EStdKeyUpArrow || aScan == EStdKeyDownArrow)
		{
		const TInt d = (aScan == EStdKeyUpArrow) ? ECfgCount - 1 : 1;
		iSel = (iSel + d) % ECfgCount;
		iConfirm = -1;      // підтвердження діє лише поки не зійшли з пункта
		DrawNow();
		return EKeyWasConsumed;
		}

	TInt d = 0;
	if (aScan == EStdKeyLeftArrow)  d = -1;
	if (aScan == EStdKeyRightArrow) d = 1;
	if (d && !isCommand(iSel))
		{
		TInt &v = iCfg.Value(iSel);
		if (iSel == ECfgGpuSkip)
			{
			v += d;
			if (v < 0) v = 3;
			if (v > 3) v = 0;
			}
		else if (iSel == ECfgFxaa)
			{
			v += d;
			if (v < 0) v = 5;
			if (v > 5) v = 0;
			}
		else if (iSel == ECfgCycle)
			{
			v += d * KCycleStep;
			if (v < KCycleMin) v = KCycleMax;
			if (v > KCycleMax) v = KCycleMin;
			}
		else
			v = v ? 0 : 1;
		DrawNow();
		return EKeyWasConsumed;
		}

	if (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
		{
		switch (iSel)
			{
			case ECfgGame:
				// Огляд починаємо з каталогу поточної гри - так найчастіше й треба.
				if (iGamePath.Length())
					{
					const TInt s = iGamePath.LocateReverse('\\');
					iBrowsePath = iGamePath.Left(s + 1);
					}
				ScanDirL();
				iPage = EPageFiles;
				break;
			case ECfgKeys:
				iKeySel = 0;
				iCapture = -1;
				iPage = EPageKeys;
				break;
			case ECfgReset:
				// Два натискання: розкладка й підібрані хаки - надто дорога втрата
				// для випадкового OK.
				if (iConfirm == ECfgReset)
					{
					iCfg.SetDefaults();
					ApplyKeyMap();
					iConfirm = -1;
					}
				else
					iConfirm = ECfgReset;
				break;
			case ECfgExit:
				CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
				return EKeyWasConsumed;
			default:
				if (iStartedOnce)
					CloseMenu();
				else
					ApplyAndStart();
				return EKeyWasConsumed;
			}
		DrawNow();
		return EKeyWasConsumed;
		}
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::FilesKeyL(TInt aScan)
	{
	const TInt n = iBrowse->Count();
	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		iPage = EPageMain;
		DrawNow();
		return EKeyWasConsumed;
		}
	if (aScan == EStdKeyUpArrow && n)
		iBrowseSel = (iBrowseSel + n - 1) % n;
	else if (aScan == EStdKeyDownArrow && n)
		iBrowseSel = (iBrowseSel + 1) % n;
	else if (aScan == EStdKeyLeftArrow)
		BrowseUpL();
	else if (aScan == EStdKeyRightArrow || aScan == EStdKeyDevice3 ||
	         aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
		BrowseEnterL();
	DrawNow();
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::KeysKeyL(TInt aScan)
	{
	// Захоплення: наступна клавіша стає прив'язкою. RSK лишаємо собі - нею
	// скасовують, і саме вона відкриває меню в грі, тож призначати її не можна.
	if (iCapture >= 0)
		{
		if (aScan != EStdKeyDevice1)
			{
			iCfg.iKeyScan[iCapture] = aScan;
			iCfg.iKeyAlt[iCapture] = -1;    // запасний код був частиною типової розкладки
			}
		iCapture = -1;
		DrawNow();
		return EKeyWasConsumed;
		}

	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		ApplyKeyMap();
		iPage = EPageMain;
		DrawNow();
		return EKeyWasConsumed;
		}
	if (aScan == EStdKeyUpArrow)
		iKeySel = (iKeySel + iPadCount - 1) % iPadCount;
	else if (aScan == EStdKeyDownArrow)
		iKeySel = (iKeySel + 1) % iPadCount;
	else if (aScan == EStdKeyLeftArrow || aScan == EStdKeyRightArrow)
		iCfg.iKeyHold[iKeySel] = iCfg.iKeyHold[iKeySel] ? 0 : 1;
	else if (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
		iCapture = iKeySel;
	DrawNow();
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKeyDown && aType != EEventKeyUp)
		return EKeyWasNotConsumed;

	// Меню забирає клавіші цілком.
	if (iPage != EPageNone)
		{
		if (aType != EEventKeyDown)
			return EKeyWasConsumed;
		const TInt sc = aKeyEvent.iScanCode;
		if (iPage == EPageFiles) return FilesKeyL(sc);
		if (iPage == EPageKeys)  return KeysKeyL(sc);
		return MainKeyL(sc);
		}

	// Права софт-клавіша в грі відкриває меню. CBA сховано заради повного екрана,
	// тож підпису немає, але звичне місце лишається робочим. Виняток - запуск із
	// командного рядка: там натискань немає, а поведінку бенчмарку міняти не можна.
	if (aType == EEventKeyDown && aKeyEvent.iScanCode == EStdKeyDevice1)
		{
		if (iCliMode)
			CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
		else
			OpenMenu();
		return EKeyWasConsumed;
		}

	// Розкладка користувача йде ПЕРШОЮ, поперед службових клавіш: інакше кнопка,
	// призначена на зірочку чи ліву софт-клавішу, мовчки не діяла б - їх раніше
	// поглинали знімок екрана й гучність. Типова розкладка їх не займає, тож для
	// незміненого конфігу порядок нічого не міняє.
	if (voutSymbianKeyEvent(aKeyEvent.iScanCode, aType == EEventKeyDown))
		return EKeyWasConsumed;

	// Ліва софт-клавіша - знімок екрана на вимогу.
	if (aType == EEventKeyDown && aKeyEvent.iScanCode == EStdKeyDevice0)
		{
		ps1ScreenShot();
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

	return EKeyWasNotConsumed;
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

	// Поки відкрите меню, ядро не чіпаємо: до старту вибір диска й режимів має
	// подіяти до ініціалізації, у грі це просто пауза. CIdle крутиться далі, щоб
	// CONE обслуговував клавіші.
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
		// Перечіпляємо вивід: DSA піднімається саме тут, а прапорець із меню
		// приходить у ps1CfgApply уже ПІСЛЯ первинної прив'язки в ConstructL.
		// Без цього "Direct draw: on" подіяв би лише з наступного запуску.
		AttachOutput();
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
