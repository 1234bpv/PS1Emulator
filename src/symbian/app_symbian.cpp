// AVKON shell. The console-based stage1 had no CCoeEnv, and MBX-EGL creates a surface
// only on a window beneath it: eglCreateWindowSurface panicked on a "bare" RWindow
// (verified by tracing, see comment in vout_symbian.cpp). As a side effect the framework
// provides a proper application exit, a slot in the task list, and managed orientation.
//
// Emulation runs not as a continuous loop but one frame at a time via CIdle: otherwise
// the scheduler would never yield control to CONE, and neither keys nor exit would reach here.

#include <e32base.h>
#include <basched.h>          // KLeaveExit
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

#include "imgconv.h"
#include "i18n.h"

extern "C" {
int  SaveState(const char *file);
int  LoadState(const char *file);
int  ps1GuiSetup(void);
int  ps1GuiStep(void);
void ps1GuiShutdown(void);
void ps1GuiRequestExit(void);
void ps1GuiCloseGame(void);
void ps1GuiEndPass(void);
int  ps1GuiStartPass(void);
void ps1Log(const char *aType, const char *aMsg);
void ps1LogInt(const char *aType, const char *aMsg, int aVal);
void ps1LogFmt(const char *aType, const char *aFmt, ...);
int  ps1SndVolumeStep(int aDelta);
int  ps1SndVolumeSteps(void);
int  voutSymbianKeyEvent(int aScanCode, int aDown);
int  voutSymbianFinish(int aHoldMs);
int  voutSymbianGlActive(void);
void ps1ScreenShot(void);
void ps1CfgApply(const char *aDisc, const char *aBios, int aInterp, int aIlace,
	int aLight, int aBlend, int aPixSkip, int aGteNf, int aSound, int aGl,
	int aDirty, int aDsa, int aFps, int aGpuSkip, int aCycle, int aSmooth,
	int aFxaa, int aSwDraw);
// Subset that can be toggled while in-game (menu from pause).
void ps1CfgApplyLive(int aIlace, int aLight, int aBlend, int aPixSkip,
	int aFps, int aGpuSkip, int aCycle, int aSmooth, int aFxaa, int aDirty,
	int aSwDraw);
void voutSymbianSetKeyMap(const int *aScan, const int *aAlt, int aCount,
	unsigned int aToggle);
int  voutSymbianPadButtons(void);
int  voutSymbianDefaultKey(int aIdx, int aAlt);
int  voutSymbianPadBit(int aIdx);
void voutSymbianSuspend(int aOn);
}

// Output binding to the CONE window (vout_symbian.cpp). No extern "C": Symbian types.
void voutSymbianAttachWindow(RWsSession &aWs, CWsScreenDevice &aDev, RWindow &aWin,
                             CWindowGc &aGc);

// Same UID as in stage1.mmp and stage1_reg.rss.
static const TUid KUidPs1App = { 0xE0001002 };

// --- Settings ---------------------------------------------------------------

// We draw the menu ourselves rather than via CAknSettingItemList: we already have
// our own window and key handling, and the AVKON list would pull in resource
// definitions and control hierarchy rebuilding for the same result.
//
// Item order = display order. The item's section is given by KItemSection, so
// items can only be reordered together with it.
enum TCfgItem
	{
	// There is intentionally no DISPLAY skip ("Show every") here: it renders the
	// frame and discards it, i.e. pays for the expensive part. Measured -4.3% vs
	// -22.5% for ECfgGpuSkip. Kept as a command-line probe "showskip".
	ECfgStart = 0, ECfgGame, ECfgSaveState, ECfgLoadState, ECfgAdd, ECfgBios, ECfgProfile,
	ECfgLight, ECfgBlend, ECfgIlace, ECfgPixSkip, ECfgSmooth, ECfgFxaa,
	ECfgDirty, ECfgDsa, ECfgGl, ECfgSwDraw,
	ECfgCpu, ECfgGteNf, ECfgGpuSkip, ECfgCycle, ECfgFps,
	ECfgSound,
	ECfgKeys,
	ECfgLang,
	ECfgReset, ECfgExit,
	ECfgCount
	};

enum TCfgSection
	{
	ESecGame = 0, ESecVideo, ESecSpeed, ESecSound, ESecInput, ESecSystem, ESecCount
	};

static const TStrId KSecStrId[ESecCount] =
	{ EStrSecGame, EStrSecVideo, EStrSecSpeed, EStrSecSound, EStrSecControls, EStrSecSystem };

static const unsigned char KItemSection[ECfgCount] =
	{
	ESecGame, ESecGame, ESecGame, ESecGame, ESecGame, ESecGame, ESecGame,
	ESecVideo, ESecVideo, ESecVideo, ESecVideo, ESecVideo, ESecVideo,
	ESecVideo, ESecVideo, ESecVideo, ESecVideo,
	ESecSpeed, ESecSpeed, ESecSpeed, ESecSpeed, ESecSpeed,
	ESecSound,
	ESecInput,
	ESecSystem, ESecSystem, ESecSystem
	};

static const TStrId KItemLabelStrId[ECfgCount] =
	{
	EStrItemStart, EStrItemGame, EStrItemSaveState, EStrItemLoadState,
	EStrItemAdd, EStrItemBios, EStrItemProfile,
	EStrItemLight, EStrItemBlend, EStrItemIlace, EStrItemPixSkip,
	EStrItemSmooth, EStrItemFxaa, EStrItemDirty, EStrItemDsa,
	EStrItemGl, EStrItemSwDraw,
	EStrItemCpu, EStrItemGteNf, EStrItemGpuSkip, EStrItemCycle,
	EStrItemFps,
	EStrItemSound,
	EStrItemKeys,
	EStrItemLang,
	EStrItemReset, EStrItemExit
	};

// Key names in the settings file. NULL means a command item with no value.
// The reader looks up by name, so adding or reordering items is safe:
// the old file remains valid, missing keys get their default value.
static const char *const KCfgKey[ECfgCount] =
	{
	NULL, NULL, NULL, NULL, NULL, NULL, "profile",
	"light", "blend", "ilace", "pixskip", "smooth", "fxaa",
	"dirty", "dsa", "gl", "swdraw",
	"cpu", "gtenf", "gpuskip", "cycle", "fps",
	"sound",
	NULL,
	"lang",
	NULL, NULL
	};

// Require re-initialization of the core or output: do not take effect from
// the in-game menu, marked with an asterisk in the list.
static TBool restartOnly(TInt aItem)
	{
	return aItem == ECfgGame || aItem == ECfgBios || aItem == ECfgCpu ||
	       aItem == ECfgGteNf || aItem == ECfgSound || aItem == ECfgGl ||
	       aItem == ECfgDsa;
	}

static TBool isCommand(TInt aItem)
	{
	return aItem == ECfgStart || aItem == ECfgGame || aItem == ECfgSaveState ||
	       aItem == ECfgLoadState || aItem == ECfgAdd || aItem == ECfgBios ||
	       aItem == ECfgKeys || aItem == ECfgReset || aItem == ECfgExit;
	}

// PS1 cycle multiplier. Upstream default is CYCLE_MULT_DEFAULT (175); higher means
// "PS1 CPU is slower", i.e. less work for us and less smoothness for the game.
const TInt KCycleDefault = 175;
const TInt KCycleMin = 100;
const TInt KCycleMax = 300;
const TInt KCycleStep = 25;

// Generous upper bound on the number of buttons reported by vout: layout is defined there.
const TInt KPadMax = 16;

// Single emulator directory. Everything it reads and writes lives under it: otherwise
// the card root got cluttered with files, and which ones belong to which game was unclear.
_LIT(KCfgPath, "E:\\PS1\\cfg\\ps1_settings.txt");
_LIT(KProfDir, "E:\\PS1\\cfg\\");
_LIT(KImgDir,  "E:\\PS1\\games\\");
_LIT(KBiosDir, "E:\\PS1\\bios\\");
_LIT(KSavesDir, "E:\\PS1\\saves\\");

class TPs1Cfg
	{
public:
	void SetDefaults();
	TInt &Value(TInt aItem);
	TInt iInterp, iIlace, iLight, iBlend, iPixSkip, iGteNf, iSound, iGl;
	TInt iDirty, iDsa, iFps, iGpuSkip, iCycle, iSmooth, iFxaa, iProfile, iSwDraw;
	TInt iLang;
	TInt iKeyScan[KPadMax], iKeyAlt[KPadMax], iKeyHold[KPadMax];
	// BIOS dump name in E:\PS1\bios (without path). Empty means default scph7502.bin.
	// Stored in the game profile, so games released in only one region get their
	// own BIOS without manual switching.
	TFileName iBios;
	TInt iDummy;    // sink for command items that have no value
	};

void TPs1Cfg::SetDefaults()
	{
	iInterp = iIlace = iPixSkip = iGteNf = iGl = iFps = iSwDraw = 0;
	iGpuSkip = 0;
	iDummy = 0;
	iCycle = KCycleDefault;
	// Averaging during downscale is ON by default: without it in 640x* modes
	// the decimation eats thin font strokes, and that is an artifact, not a speedup.
	iSmooth = 1;
	// Edge anti-aliasing is OFF by default: measured -17% frames on trace, and on a
	// 320x240 screen the benefit is not visible in every game. Enable deliberately.
	iFxaa = 0;
	// Lighting and semi-transparency are ON by default: without them textures are drawn
	// without shading, and that is not a speedup but an artifact. Can be disabled here.
	iLight = iBlend = 1;
	iSound = 1;
	iLang = I18nDetectLanguage();
	I18nSetLanguage(iLang);
	iBios.Zero();
	// Per-game settings: key bindings and speed hacks needed in a racing game vs an
	// RPG are different, and flipping through them manually each time is the same menu twice.
	iProfile = 1;
	// Display by gpulib latch: measured -6.7 ms per frame with no image quality loss.
	iDirty = 1;
	// Direct screen buffer write: measured -0.93 ms per frame. It was broken because of
	// Update() without a region (bitgdi did not know about our write, so it pushed nothing);
	// with an explicit region the image is correct - verified on N95 8GB.
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
		case ECfgSwDraw:  return iSwDraw;
		case ECfgDirty:   return iDirty;
		case ECfgDsa:     return iDsa;
		case ECfgFps:     return iFps;
		case ECfgGpuSkip: return iGpuSkip;
		case ECfgCycle:   return iCycle;
		case ECfgSmooth:  return iSmooth;
		case ECfgFxaa:    return iFxaa;
		case ECfgProfile: return iProfile;
		case ECfgLang:    return iLang;
		default:          return iDummy;
		}
	}

// --- Keys: display names ----------------------------------------------------

static const TStrId KBtnStrId[14] =
	{
	EStrBtnUp, EStrBtnDown, EStrBtnLeft, EStrBtnRight,
	EStrBtnCross, EStrBtnCircle, EStrBtnSquare, EStrBtnTriangle,
	EStrBtnL1, EStrBtnR1, EStrBtnL2, EStrBtnR2, EStrBtnSelect, EStrBtnStart
	};

static void keyName(TInt aScan, TDes &aOut)
	{
	switch (aScan)
		{
		case EStdKeyUpArrow:     aOut.Copy(Tr(EStrKeyUp));       return;
		case EStdKeyDownArrow:   aOut.Copy(Tr(EStrKeyDown));     return;
		case EStdKeyLeftArrow:   aOut.Copy(Tr(EStrKeyLeft));     return;
		case EStdKeyRightArrow:  aOut.Copy(Tr(EStrKeyRight));    return;
		case EStdKeyDevice3:     aOut.Copy(Tr(EStrKeyOk));       return;
		case EStdKeyDevice0:     aOut.Copy(Tr(EStrKeyLsk));      return;
		case EStdKeyDevice1:     aOut.Copy(Tr(EStrKeyRsk));      return;
		case EStdKeyBackspace:   aOut.Copy(Tr(EStrKeyClear));    return;
		case EStdKeyHash:        aOut.Copy(_L("#"));             return;
		case EStdKeyNkpAsterisk: aOut.Copy(_L("*"));             return;
		case EStdKeyIncVolume:   aOut.Copy(Tr(EStrKeyVolPlus));  return;
		case EStdKeyDecVolume:   aOut.Copy(Tr(EStrKeyVolMinus)); return;
		case EStdKeyApplication0:aOut.Copy(Tr(EStrKeyMenu));     return;
		default: break;
		}
	if (aScan < 0)
		aOut.Copy(_L("-"));
	else if (aScan >= 32 && aScan < 127)
		aOut.Format(_L("%c"), aScan);
	else
		aOut.Format(_L("k%d"), aScan);
	}

// --- Container: window for the frame + menu + keys ---------------------------

enum TMenuPage
	{
	EPageNone = 0, EPageMain, EPageFiles, EPageKeys,
	EPageImport,      // browser with checkmarks for batch import
	EPageBios,        // BIOS dump selection (including from archives)
	EPageAsk,         // yes/no question before a long operation
	EPageProgress,
	EPageError
	};

// What to show in the browser. The browser is one, but the consumers differ:
// games come from disc images, import also from archives, BIOS from dumps.
enum TScanMode { EScanGames = 0, EScanImport, EScanBios };

// Question awaiting an answer on EPageAsk.
enum TAskWhat { EAskNone = 0, EAskMode, EAskDelete, EAskRestart };

class CPs1Container : public CCoeControl
	{
public:
	static CPs1Container *NewL(const TRect &aRect);
	~CPs1Container();
	TKeyResponse OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType);
	TKeyResponse DispatchKeyL(const TKeyEvent &aKeyEvent, TEventCode aType);
	// CCoeControl::Window() is protected, and the output needs exactly this window.
	RWindow &FrameWindow() { return Window(); }
	TBool MenuActive() const { return iPage != EPageNone; }
	void MenuTick();
	void ApplyAndStart();
	// Whether the user requested something that only takes effect with a new core
	// initialization. Read by the tick: restarting the pass from a key handler is
	// unsafe (sound, DSA, scheduler).
	TBool TakeRestart() { const TBool r = iRestartReq; iRestartReq = EFalse; return r; }
	void ShowError();
private:
	void ConstructL(const TRect &aRect);
	void Draw(const TRect &aRect) const;

	// drawing
	TRect DrawChrome(CWindowGc &gc, const TDesC &aTitle, const TDesC &aHint) const;
	void DrawMain(CWindowGc &gc, const TRect &aBody) const;
	void DrawKeys(CWindowGc &gc, const TRect &aBody) const;
	void DrawFiles(CWindowGc &gc, const TRect &aBody) const;
	void DrawAsk(CWindowGc &gc, const TRect &aBody) const;
	void DrawError(CWindowGc &gc, const TRect &aBody) const;
	TBool RunningGamePicked() const;
	void DrawProgress(CWindowGc &gc, const TRect &aBody) const;
	void DrawRow(CWindowGc &gc, const TRect &aBody, TInt aY, TInt aLh,
	             const TDesC &aLeft, const TDesC &aRight, TBool aSel) const;
	void ItemLabel(TInt aItem, TDes &aOut) const;
	void ItemValue(TInt aItem, TDes &aOut) const;
	TBool ItemVisible(TInt aItem) const;

	// pages
	TKeyResponse MainKeyL(TInt aScan);
	TKeyResponse FilesKeyL(TInt aScan);
	TKeyResponse KeysKeyL(TInt aScan);
	TKeyResponse ImportKeyL(TInt aScan);
	TKeyResponse BiosKeyL(TInt aScan);
	TKeyResponse AskKeyL(TInt aScan);
	TKeyResponse ProgressKeyL(TInt aScan);
	TKeyResponse ErrorKeyL(TInt aScan);
	void OpenMenu();
	void CloseMenu();
	void DoCloseMenu(TBool aWithRestart);
	void RevertRestartSettings();

	// state save / load
	void StatePath(TDes &aOut, TInt aSlot) const;
	void DoSaveState();
	void DoLoadState();

	// file browser
	void MigrateOldLayout();
	void ScanDirL();
	void BrowseEnterL();
	void BrowseUpL();
	TBool AutoPickGameL();

	// import
	void CurrentEntry(TDes &aOut) const;
	TInt PickedIndex(const TDesC &aPath) const;
	void TogglePickL();
	void StartImportL();

	// persistence
	void ProfilePath(TDes &aOut) const;
	void LoadCfg(const TDesC &aPath);
	void ParseCfg(const TDesC8 &aTxt);
	void SaveCfg(const TDesC &aPath, TBool aWithGame) const;
	void SaveAll() const;
	void SetGameL(const TDesC &aPath);
	void ApplyKeyMap() const;
	void SnapRestart();
	TBool RestartNeeded() const;

	CDesCArray *iBrowse;      // current directory contents; directories end with '\'
	// Picked for import - stored as full paths, not row indices: otherwise
	// checkmarks would be lost on the first navigation to another directory.
	CDesCArray *iPicked;
	TInt iScanMode;
	TInt iTickDiv;            // tick divisor for progress redraw
	TInt iAsk;                // question awaiting an answer
	TInt iAskSel;
	TInt iConvMode;           // answer to EAskMode
	TFileName iBrowsePath;    // empty = drive list
	TFileName iGamePath;
	TInt iPage;
	TInt iSel;                // selected item on the main page
	TInt iBrowseSel;
	TInt iKeySel;
	TInt iCapture;            // button awaiting a key press (-1 = none)
	TInt iConfirm;            // item awaiting confirmation (-1 = none)
	TInt iStateSlot;          // state slot (1..5)
	TBuf<80> iStatusMsg;      // message shown in hint bar (e.g. state save/load result)
	TInt iPadCount;
	TBool iStartedOnce;       // config already sent to the core; only live changes from now on
	TBool iGameClosed;        // game was shut down for importing its own disc image
	TBool iRestartReq;        // exited menu with a change that requires a new pass
	// Values of asterisked items at the moment the pass was started: the difference
	// from the current values is the reason to restart emulation.
	TFileName iGameAtStart, iBiosAtStart;
	TInt iInterpAtStart, iGteNfAtStart, iSoundAtStart, iGlAtStart, iDsaAtStart;
	TBool iCliMode;           // launched with arguments: do not touch the in-game menu
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
	iPicked = new (ELeave) CDesCArrayFlat(8);
	iPadCount = voutSymbianPadButtons();
	if (iPadCount > KPadMax) iPadCount = KPadMax;
	iCapture = -1;
	iConfirm = -1;
	iStateSlot = 1;
	iStatusMsg.Zero();

	MigrateOldLayout();
	iCfg.SetDefaults();
	LoadCfg(KCfgPath);
	// Game profile overrides global settings - but only if it exists.
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

	// Menu only when launched from the device. With arguments (deploy, benchmark)
	// the command line sets the parameters, and stage1 parses it itself - the menu
	// here would wait for a key press absent from the recorded input trace, and
	// the run would stall until killed by the daemon.
	TBuf<256> cl;
	User::CommandLine(cl);
	cl.Trim();
	iCliMode = (cl.Length() != 0);
	iPage = iCliMode ? EPageNone : EPageMain;
	ActivateL();
	}

CPs1Container::~CPs1Container()
	{
	ConvCancel();
	ConvRelease();
	delete iBrowse;
	delete iPicked;
	}

void CPs1Container::ShowError()
	{
	iStartedOnce = EFalse;
	iPage = EPageError;
	voutSymbianSuspend(1);
	DrawNow();
	}

TBool CPs1Container::ItemVisible(TInt aItem) const
	{
	if (aItem == ECfgSaveState || aItem == ECfgLoadState)
		return iStartedOnce && !iGameClosed;
	return ETrue;
	}

// First image in the card root - so that the first launch does not require going to the browser.
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

// Layout before E:\PS1\ existed: settings in the card root, profiles in E:\ps1cfg\,
// BIOS dump next to them. We migrate by renaming (same volume, so no data is copied);
// disc images are left in place - they are large, and the browser can navigate anywhere.
void CPs1Container::MigrateOldLayout()
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	TEntry e;
	if (fs.Entry(KCfgPath, e) != KErrNone)
		fs.Rename(_L("E:\\ps1_settings.txt"), KCfgPath);

	CDir *dir = NULL;
	if (fs.GetDir(_L("E:\\ps1cfg\\*.txt"), KEntryAttNormal, ESortByName, dir) == KErrNone
	    && dir)
		{
		for (TInt i = 0; i < dir->Count(); i++)
			{
			TFileName from(_L("E:\\ps1cfg\\"));
			TFileName to(KProfDir);
			from.Append((*dir)[i].iName);
			to.Append((*dir)[i].iName);
			if (fs.Entry(to, e) != KErrNone)
				fs.Rename(from, to);
			}
		delete dir;
		dir = NULL;
		fs.RmDir(_L("E:\\ps1cfg\\"));
		}

	if (fs.GetDir(_L("E:\\scph*.bin"), KEntryAttNormal, ESortByName, dir) == KErrNone
	    && dir)
		{
		for (TInt i = 0; i < dir->Count(); i++)
			{
			TFileName from(_L("E:\\"));
			TFileName to(KBiosDir);
			from.Append((*dir)[i].iName);
			to.Append((*dir)[i].iName);
			if (fs.Entry(to, e) != KErrNone)
				fs.Rename(from, to);
			}
		delete dir;
		}
	fs.Close();
	}

// --- File browser -----------------------------------------------------------

// .cue is not shown: cdriso derives it from .bin by extension, and a separate entry
// would only cause confusion. Compressed images are opened by the core directly:
// .pbp in 16-sector blocks, .cbn/.cbin (CISO container) sector by sector.
// CHD is not supported: HAVE_CHD is not defined.
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

// For import we also show .cue (it carries the track layout, and it is the one
// that should be picked) and archives.
static TBool isImportName(const TDesC &aName)
	{
	if (isImageName(aName) || ConvIsArchiveName(aName))
		return ETrue;
	if (aName.Length() < 4)
		return EFalse;
	TBuf<8> ext;
	ext.Copy(aName.Right(4));
	ext.LowerCase();
	return ext == _L(".cue");
	}

// BIOS dumps: extensions in the wild vary, so we also accept archives.
static TBool isBiosName(const TDesC &aName)
	{
	if (ConvIsArchiveName(aName))
		return ETrue;
	if (aName.Length() < 4)
		return EFalse;
	TBuf<8> ext;
	ext.Copy(aName.Right(4));
	ext.LowerCase();
	return ext == _L(".bin") || ext == _L(".rom");
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
				// An empty card slot stays in the drive list but has no directory -
				// without this check the browser would lead to a dead end.
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
				{
				if ((*dir)[i].IsDir())
					continue;
				const TPtrC n = (*dir)[i].iName;
				const TBool ok = (iScanMode == EScanImport) ? isImportName(n)
					: (iScanMode == EScanBios) ? isBiosName(n)
					: isImageName(n);
				if (ok)
					iBrowse->AppendL(n);
				}
			CleanupStack::PopAndDestroy(dir);
			}
		}
	CleanupStack::PopAndDestroy(&fs);
	}

void CPs1Container::BrowseUpL()
	{
	if (iBrowsePath.Length() <= 3)
		{
		iBrowsePath.Zero();     // above the drive root - show the drive list
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
	// Cursor on the first item: the disc has been selected, and the next thing
	// the user wants is to launch it. Leaving the cursor on "Game" would mean
	// that OK opens the file browser again, with no obvious way to apply the choice.
	iSel = ECfgStart;
	}

// Changing the game with profiles enabled: first save the previous game's settings,
// otherwise they are silently lost, and only then load the new game's profile.
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

// --- Persistence ------------------------------------------------------------

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

void CPs1Container::StatePath(TDes &aOut, TInt aSlot) const
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
	aOut.Copy(KSavesDir);
	for (TInt i = 0; i < n.Length(); i++)
		{
		const TChar c = n[i];
		aOut.Append(c.IsAlphaDigit() ? c : TChar('_'));
		}
	aOut.AppendFormat(_L("_slot%d.sav"), aSlot);
	}

void CPs1Container::DoSaveState()
	{
	if (!iStartedOnce || iGameClosed)
		return;
	RFs fs;
	if (fs.Connect() == KErrNone)
		{
		fs.MkDirAll(KSavesDir);
		fs.Close();
		}
	TFileName path;
	StatePath(path, iStateSlot);
	TBuf8<KMaxFileName> path8;
	path8.Copy(path);
	const int r = SaveState((const char *)path8.PtrZ());
	if (r == 0)
		{
		iStatusMsg.Format(Tr(EStrStateSaveOk), iStateSlot);
		ps1LogInt("state", "saved slot", iStateSlot);
		}
	else
		{
		iStatusMsg.Copy(Tr(EStrStateSaveErr));
		ps1LogInt("state", "save failed", r);
		}
	}

void CPs1Container::DoLoadState()
	{
	if (!iStartedOnce || iGameClosed)
		return;
	TFileName path;
	StatePath(path, iStateSlot);
	TBuf8<KMaxFileName> path8;
	path8.Copy(path);
	const int r = LoadState((const char *)path8.PtrZ());
	if (r == 0)
		{
		iStatusMsg.Format(Tr(EStrStateLoadOk), iStateSlot);
		ps1LogInt("state", "loaded slot", iStateSlot);
		}
	else
		{
		iStatusMsg.Copy(Tr(EStrStateLoadErr));
		ps1LogInt("state", "load failed", r);
		}
	}

void CPs1Container::LoadCfg(const TDesC &aPath)
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	RFile f;
	if (f.Open(fs, aPath, EFileRead | EFileShareAny) == KErrNone)
		{
		// Cannot keep on the stack: there are about forty lines now, and the
		// main thread stack here is shared with the frame tick.
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
		if (key == _L8("bios"))
			{
			iCfg.iBios.Copy(val.Left(iCfg.iBios.MaxLength()));
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
					{
					iCfg.Value(i) = v;
					if (i == ECfgLang)
						I18nSetLanguage(v);
					}
				break;
				}
		}
	}

void CPs1Container::SaveCfg(const TDesC &aPath, TBool aWithGame) const
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	// The profiles directory may not exist yet; we ignore the "already exists" error.
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
		// BIOS is written to both files: in the profile it is per-game, in the
		// global file it is the default for games that have no profile yet.
		if (iCfg.iBios.Length())
			{
			TBuf8<KMaxFileName> b;
			b.Copy(iCfg.iBios);
			line.Format(_L8("bios=%S\n"), &b);
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

// --- Item labels ------------------------------------------------------------

void CPs1Container::ItemLabel(TInt aItem, TDes &aOut) const
	{
	if (aItem == ECfgStart)
		{
		// After importing the game's own disc there is nowhere to return: the pass
		// was shut down. "Restart game" instead of "Resume" is the only indication
		// that a disc selection (or another asterisked item) has been made and awaits
		// application.
		aOut.Copy(Tr(iGameClosed ? EStrItemExit
			: !iStartedOnce ? EStrItemStart
			: RestartNeeded() ? EStrItemRestart : EStrItemResume));
		return;
		}
	aOut.Copy(Tr(KItemLabelStrId[aItem]));
	// Asterisk only in-game: before the start everything takes effect, and the mark
	// would be misleading.
	if (iStartedOnce && restartOnly(aItem))
		aOut.Append(_L(" *"));
	}

void CPs1Container::ItemValue(TInt aItem, TDes &aOut) const
	{
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
				aOut.Copy(Tr(EStrValNone));
			break;
		case ECfgSaveState:
		case ECfgLoadState:
			aOut.Format(Tr(EStrValSlotFmt), iStateSlot);
			break;
		case ECfgAdd:
			aOut.Copy(Tr(EStrValPressOk));
			break;
		case ECfgBios:
			aOut.Copy(c.iBios.Length() ? TPtrC(c.iBios) : Tr(EStrValDefault));
			break;
		case ECfgCpu:
			aOut.Copy(Tr(c.iInterp ? EStrValInterp : EStrValDynarec));
			break;
		// Skip of the rasterization itself: every (N+1)th frame is rendered, the rest are not.
		case ECfgGpuSkip:
			if (c.iGpuSkip < 0)
				aOut.Copy(Tr(EStrValAuto));
			else if (c.iGpuSkip > 0)
				aOut.Format(_L("%d"), c.iGpuSkip);
			else
				aOut.Copy(Tr(EStrValOff));
			break;
		// 3D edge anti-aliasing. Higher value means stronger effect; internally it is
		// a lower contrast threshold, i.e. the filter picks up progressively weaker edges.
		case ECfgFxaa:
			if (c.iFxaa)
				aOut.Format(_L("%d"), c.iFxaa);
			else
				aOut.Copy(Tr(EStrValOff));
			break;
		case ECfgCycle:
			if (c.iCycle == KCycleDefault)
				aOut.Format(Tr(EStrValCycleDefFmt), c.iCycle);
			else
				aOut.Format(_L("%d"), c.iCycle);
			break;
		case ECfgKeys:
			aOut.Copy(Tr(EStrValPressOk));
			break;
		case ECfgLang:
			aOut.Copy(I18nLanguageName(c.iLang));
			break;
		case ECfgReset:
			aOut.Copy(Tr(iConfirm == ECfgReset ? EStrValConfirm : EStrValPressOk));
			break;
		case ECfgExit:
			aOut.Copy(Tr(EStrValPressOk));
			break;
		default:
			aOut.Copy(Tr(c.Value(aItem) ? EStrValOn : EStrValOff));
			break;
		}
	}

// --- Drawing ----------------------------------------------------------------

// Header, list body, and hint line. Returns the rectangle for the list.
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
	if (iStatusMsg.Length())
		{
		gc.SetPenColor(TRgb(0xff, 0xdc, 0x88));
		gc.DrawText(iStatusMsg, TPoint(r.iTl.iX + 6, r.iBr.iY - 4));
		}
	else
		{
		gc.SetPenColor(TRgb(0x88, 0x90, 0xa0));
		gc.DrawText(aHint, TPoint(r.iTl.iX + 6, r.iBr.iY - 4));
		}

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

	// Section headers participate in scrolling as regular rows; count only visible items.
	TInt total = 0;
	TInt selRow = 0;
	for (TInt sec = 0; sec < ESecCount; sec++)
		{
		total++;
		for (TInt i = 0; i < ECfgCount; i++)
			{
			if (KItemSection[i] != sec || !ItemVisible(i))
				continue;
			if (i == iSel)
				selRow = total;
			total++;
			}
		}

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
			gc.DrawText(Tr(KSecStrId[sec]), TPoint(aBody.iTl.iX + 4, y));
			y += lh;
			}
		row++;
		for (TInt i = 0; i < ECfgCount; i++)
			{
			if (KItemSection[i] != sec || !ItemVisible(i))
				continue;
			if (row >= first && row < first + rows)
				{
				TBuf<64> label;
				TBuf<64> value;
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
		TBuf<40> name;
		name.Copy(Tr(KBtnStrId[i]));
		TBuf<64> value;
		if (iCapture == i)
			value.Copy(Tr(EStrPressKey));
		else
			{
			keyName(iCfg.iKeyScan[i], value);
			if (iCfg.iKeyAlt[i] >= 0)
				{
				TBuf<32> alt;
				keyName(iCfg.iKeyAlt[i], alt);
				value.Append(_L(" / "));
				value.Append(alt);
				}
			if (iCfg.iKeyHold[i])
				{
				value.Append(_L("   "));
				value.Append(Tr(EStrHold));
				}
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
		gc.DrawText(Tr(EStrEmpty), TPoint(aBody.iTl.iX + 10, y));
		return;
		}
	for (TInt i = first; i < first + rows && i < n; i++)
		{
		const TPtrC e = (*iBrowse)[i];
		const TBool dir = (e.Right(1) == _L("\\"));
		TBuf<96> name;
		if (iPage == EPageImport && !dir)
			{
			TFileName full = iBrowsePath;
			if (full.Length() + e.Length() < full.MaxLength())
				full.Append(e);
			name.Copy(PickedIndex(full) >= 0 ? _L("[x] ") : _L("[ ] "));
			}
		name.Append(e.Left(name.MaxLength() - name.Length()));
		DrawRow(gc, aBody, y, lh, name, dir ? Tr(EStrDir) : KNullDesC, i == iBrowseSel);
		y += lh;
		}
	}

// Question before a long operation. Drawn with the same list as everything else:
// a separate AVKON dialog would require resources and its own key handling.
// Whether any of the picked items is the disc image of the currently running game.
// Compared by name up to the dot: the game is opened as .bin, but for compression
// the natural choice is the .cue file next to it.
TBool CPs1Container::RunningGamePicked() const
	{
	if (!iStartedOnce || !iGamePath.Length())
		return EFalse;
	const TInt gd = iGamePath.LocateReverse('.');
	const TPtrC gstem = (gd > 0) ? iGamePath.Left(gd) : TPtrC(iGamePath);
	for (TInt i = 0; i < iPicked->Count(); i++)
		{
		const TPtrC p = (*iPicked)[i];
		const TInt d = p.LocateReverse('.');
		const TPtrC s = (d > 0) ? p.Left(d) : TPtrC(p);
		if (s.CompareF(gstem) == 0)
			return ETrue;
		}
	return EFalse;
	}

void CPs1Container::DrawAsk(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + 3;
	TInt y = aBody.iTl.iY + font->AscentInPixels() + lh;

	const TDesC &o0 = Tr(iAsk == EAskMode ? EStrAskCompress
		: iAsk == EAskDelete ? EStrAskDeleteYes
		: EStrAskRestartYes);
	const TDesC &o1 = Tr(iAsk == EAskMode ? EStrAskStore
		: iAsk == EAskDelete ? EStrAskDeleteNo
		: EStrAskRestartNo);
	DrawRow(gc, aBody, y, lh, o0, KNullDesC, iAskSel == 0);
	y += lh;
	DrawRow(gc, aBody, y, lh, o1, KNullDesC, iAskSel == 1);
	y += lh * 2;

	gc.SetPenColor(TRgb(0x90, 0x90, 0x98));
	if (iAsk == EAskMode)
		{
		gc.DrawText(Tr(EStrAskEccWarn1),
			TPoint(aBody.iTl.iX + 10, y));
		gc.DrawText(Tr(EStrAskEccWarn2),
			TPoint(aBody.iTl.iX + 10, y + lh));
		y += lh * 2;
		}
	else if (iAsk == EAskDelete)
		{
		TBuf<80> n;
		n.Format(Tr(EStrAskFilesFmt), iPicked->Count());
		gc.DrawText(n, TPoint(aBody.iTl.iX + 10, y));
		y += lh;
		}
	else if (iAsk == EAskRestart)
		{
		gc.SetPenColor(TRgb(0xff, 0xdc, 0x88));
		gc.DrawText(Tr(EStrAskRestartWarn1),
			TPoint(aBody.iTl.iX + 10, y));
		gc.DrawText(Tr(EStrAskRestartWarn2),
			TPoint(aBody.iTl.iX + 10, y + lh));
		}

	// Game-close warning - shown on both questions: the decision about the disc
	// currently being read by the core is made here, not after the start.
	if (RunningGamePicked())
		{
		gc.SetPenColor(TRgb(0xff, 0xdc, 0x88));
		gc.DrawText(Tr(EStrAskRunWarn1),
			TPoint(aBody.iTl.iX + 10, y + lh));
		gc.DrawText(Tr(EStrAskRunWarn2),
			TPoint(aBody.iTl.iX + 10, y + lh * 2));
		gc.DrawText(Tr(EStrAskRunWarn3),
			TPoint(aBody.iTl.iX + 10, y + lh * 3));
		}
	}

void CPs1Container::DrawError(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + 3;
	TInt y = aBody.iTl.iY + font->AscentInPixels() + lh;

	gc.SetPenColor(TRgb(0xff, 0x90, 0x90));
	gc.DrawText(Tr(EStrErrDiscOrBios1), TPoint(aBody.iTl.iX + 10, y));
	y += lh * 2;

	gc.SetPenColor(TRgb(0xc8, 0xc8, 0xd0));
	gc.DrawText(Tr(EStrErrDiscOrBios2), TPoint(aBody.iTl.iX + 10, y));
	y += lh;
	gc.DrawText(Tr(EStrErrDiscOrBios3), TPoint(aBody.iTl.iX + 10, y));
	}

void CPs1Container::DrawProgress(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + 3;
	TInt y = aBody.iTl.iY + font->AscentInPixels() + lh;

	TConvStat st;
	ConvStat(st);

	TBuf<80> line;
	line.Format(Tr(EStrProgFileFmt), st.iFileNo, st.iFileCount);
	gc.SetPenColor(TRgb(0xc8, 0xc8, 0xd0));
	gc.DrawText(line, TPoint(aBody.iTl.iX + 10, y));
	y += lh;
	gc.SetPenColor(KRgbWhite);
	gc.DrawText(st.iName.Left(40), TPoint(aBody.iTl.iX + 10, y));
	y += lh + 6;

	// Progress bar: during disc conversion this is the only sign that the process is alive.
	const TInt bw = aBody.Width() - 20;
	const TInt bx = aBody.iTl.iX + 10;
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushColor(TRgb(0x20, 0x24, 0x30));
	gc.DrawRect(TRect(bx, y, bx + bw, y + lh));
	gc.SetBrushColor(TRgb(0x20, 0x48, 0x88));
	TInt pc = st.iPercent;
	if (pc < 0) pc = 0;
	if (pc > 100) pc = 100;
	gc.DrawRect(TRect(bx, y, bx + bw * pc / 100, y + lh));
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	y += lh * 2 + 6;

	line.Format(_L("%d%%"), pc);
	gc.SetPenColor(TRgb(0xff, 0xdc, 0x88));
	gc.DrawText(line, TPoint(bx, y));
	y += lh;
	if (st.iMsg.Length())
		{
		gc.SetPenColor(st.iState == EConvErr ? TRgb(0xff, 0x90, 0x90)
		                                     : TRgb(0x90, 0xd0, 0x90));
		gc.DrawText(st.iMsg, TPoint(bx, y));
		}
	}

// In menu mode we draw ourselves; in-game it is empty: the frame is blitted by the
// emulator outside the BeginRedraw/EndRedraw pair, and the framework has no current frame.
void CPs1Container::Draw(const TRect & /*aRect*/) const
	{
	if (iPage == EPageNone)
		return;

	CWindowGc &gc = SystemGc();
	if (iPage == EPageFiles || iPage == EPageImport || iPage == EPageBios)
		{
		TBuf<KMaxFileName> title;
		title.Copy(Tr(iPage == EPageImport ? EStrTitleAddGames
			: iPage == EPageBios ? EStrTitleSelectBios
			: EStrTitleSelectGame));
		title.Append(iBrowsePath.Length() ? TPtrC(iBrowsePath) : Tr(EStrDrives));
		const TRect body = DrawChrome(gc, title.Left(46),
			Tr(iPage == EPageImport ? EStrHintImport : EStrHintBrowse));
		DrawFiles(gc, body);
		}
	else if (iPage == EPageAsk)
		{
		const TRect body = DrawChrome(gc,
			Tr(iAsk == EAskMode ? EStrTitleStore
				: iAsk == EAskDelete ? EStrTitleDelete
				: EStrTitleRestart),
			Tr(EStrHintAsk));
		DrawAsk(gc, body);
		}
	else if (iPage == EPageError)
		{
		const TRect body = DrawChrome(gc, Tr(EStrTitleError), Tr(EStrHintErrorBack));
		DrawError(gc, body);
		}
	else if (iPage == EPageProgress)
		{
		TConvStat st;
		ConvStat(st);
		const TRect body = DrawChrome(gc, Tr(EStrTitleImporting),
			Tr(st.iState == EConvRun ? EStrHintCancels : EStrHintCloses));
		DrawProgress(gc, body);
		}
	else if (iPage == EPageKeys)
		{
		const TRect body = DrawChrome(gc, Tr(EStrTitleControls),
			Tr(iCapture >= 0 ? EStrHintCapture : EStrHintKeys));
		DrawKeys(gc, body);
		}
	else
		{
		const TRect body = DrawChrome(gc, _L("PS1Emulator"),
			Tr(iGameClosed  ? EStrHintGameClosed
			: iStartedOnce ? EStrHintMainResume
			               : EStrHintMainStart));
		DrawMain(gc, body);
		}
	gc.DiscardFont();
	}

// --- Menu control -----------------------------------------------------------

void CPs1Container::OpenMenu()
	{
	if (iPage != EPageNone)
		return;
	ps1Log("menu", "open");
	voutSymbianSuspend(1);
	iPage = EPageMain;
	iConfirm = -1;
	iStatusMsg.Zero();
	DrawNow();
	}

void CPs1Container::DoCloseMenu(TBool aWithRestart)
	{
	SaveAll();
	ApplyKeyMap();
	// A different disc (or BIOS, CPU branch, sound, GL, DSA) cannot be switched on the
	// fly: the pass must be torn down and brought up again. This is done by the tick,
	// not by this branch - we got here from a key handler.
	if (aWithRestart && RestartNeeded())
		iRestartReq = ETrue;
	ps1CfgApplyLive(iCfg.iIlace, iCfg.iLight, iCfg.iBlend, iCfg.iPixSkip,
		iCfg.iFps, iCfg.iGpuSkip, iCfg.iCycle, iCfg.iSmooth, iCfg.iFxaa, iCfg.iDirty,
		iCfg.iSwDraw);
	iPage = EPageNone;
	voutSymbianSuspend(0);
	ps1Log("menu", "close");
	}

void CPs1Container::CloseMenu()
	{
	DoCloseMenu(ETrue);
	}

void CPs1Container::RevertRestartSettings()
	{
	iGamePath = iGameAtStart;
	iCfg.iBios = iBiosAtStart;
	iCfg.iInterp = iInterpAtStart;
	iCfg.iGteNf = iGteNfAtStart;
	iCfg.iSound = iSoundAtStart;
	iCfg.iGl = iGlAtStart;
	iCfg.iDsa = iDsaAtStart;
	}

// Progress page redraw: it receives no key events, and its state is changed by another
// thread, so the only way to update it is the application tick.
void CPs1Container::MenuTick()
	{
	if (iPage != EPageProgress)
		return;
	TConvStat st;
	ConvStat(st);
	// Every ~0.4 s: more often would be unnecessary drawing on top of converter work.
	if (++iTickDiv < 20)
		return;
	iTickDiv = 0;
	DrawNow();
	}

// Passes the selection to the core and yields control to the emulator.
void CPs1Container::ApplyAndStart()
	{
	if (!iGamePath.Length())
		return;
	SaveAll();
	ApplyKeyMap();
	TBuf8<KMaxFileName> path8;
	path8.Copy(iGamePath);
	TBuf8<KMaxFileName> bios8;
	bios8.Copy(iCfg.iBios);
	ps1CfgApply((const char *)path8.PtrZ(), (const char *)bios8.PtrZ(),
		iCfg.iInterp, iCfg.iIlace,
		iCfg.iLight, iCfg.iBlend, iCfg.iPixSkip, iCfg.iGteNf, iCfg.iSound, iCfg.iGl,
		iCfg.iDirty, iCfg.iDsa, iCfg.iFps, iCfg.iGpuSkip, iCfg.iCycle, iCfg.iSmooth,
		iCfg.iFxaa, iCfg.iSwDraw);
	iStartedOnce = ETrue;
	iRestartReq = EFalse;
	SnapRestart();
	iPage = EPageNone;
	}

void CPs1Container::SnapRestart()
	{
	iGameAtStart = iGamePath;
	iBiosAtStart = iCfg.iBios;
	iInterpAtStart = iCfg.iInterp;
	iGteNfAtStart  = iCfg.iGteNf;
	iSoundAtStart  = iCfg.iSound;
	iGlAtStart     = iCfg.iGl;
	iDsaAtStart    = iCfg.iDsa;
	}

TBool CPs1Container::RestartNeeded() const
	{
	return iGamePath.CompareF(iGameAtStart) != 0 ||
	       iCfg.iBios.CompareF(iBiosAtStart) != 0 ||
	       iCfg.iInterp != iInterpAtStart || iCfg.iGteNf != iGteNfAtStart ||
	       iCfg.iSound  != iSoundAtStart  || iCfg.iGl    != iGlAtStart ||
	       iCfg.iDsa    != iDsaAtStart;
	}

TKeyResponse CPs1Container::MainKeyL(TInt aScan)
	{
	// Exit must work from here too; otherwise there is no way to exit from the menu before start.
	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		if (iStartedOnce && !iGameClosed)
			{
			if (RestartNeeded())
				{
				iAsk = EAskRestart;
				iAskSel = 0;
				iPage = EPageAsk;
				DrawNow();
				return EKeyWasConsumed;
				}
			DoCloseMenu(EFalse);
			}
		else
			CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
		return EKeyWasConsumed;
		}

	if (aScan == EStdKeyUpArrow || aScan == EStdKeyDownArrow)
		{
		const TInt d = (aScan == EStdKeyUpArrow) ? -1 : 1;
		do
			{
			iSel = (iSel + d + ECfgCount) % ECfgCount;
			}
		while (!ItemVisible(iSel));
		iConfirm = -1;      // confirmation applies only while staying on the item
		iStatusMsg.Zero();
		DrawNow();
		return EKeyWasConsumed;
		}

	TInt d = 0;
	if (aScan == EStdKeyLeftArrow)  d = -1;
	if (aScan == EStdKeyRightArrow) d = 1;
	if (d)
		{
		if (iSel == ECfgSaveState || iSel == ECfgLoadState)
			{
			iStateSlot += d;
			if (iStateSlot < 1) iStateSlot = 5;
			if (iStateSlot > 5) iStateSlot = 1;
			iStatusMsg.Zero();
			DrawNow();
			return EKeyWasConsumed;
			}
		if (!isCommand(iSel))
			{
			TInt &v = iCfg.Value(iSel);
			if (iSel == ECfgGpuSkip)
				{
				// -1 is the adaptive mode of gpulib: it decides per-frame and is the one
				// that synchronizes the skip with the moment the game flips the buffer.
				v += d;
				if (v < -1) v = 3;
				if (v > 3) v = -1;
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
			else if (iSel == ECfgLang)
				{
				const TInt count = I18nLanguageCount();
				v = (v + d + count) % count;
				I18nSetLanguage(v);
				}
			else
				v = v ? 0 : 1;
			iStatusMsg.Zero();
			DrawNow();
			return EKeyWasConsumed;
			}
		}

	if (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
		{
		switch (iSel)
			{
			case ECfgStart:
				if (iGameClosed)
					CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
				else if (!iGamePath.Length())
					{
					iBrowsePath = KImgDir;
					iScanMode = EScanGames;
					ScanDirL();
					iPage = EPageFiles;
					}
				else if (iStartedOnce)
					{
					if (RestartNeeded())
						{
						iAsk = EAskRestart;
						iAskSel = 0;
						iPage = EPageAsk;
						}
					else
						DoCloseMenu(EFalse);
					}
				else
					ApplyAndStart();
				break;
			case ECfgGame:
				// Start browsing from the current game's directory - this is the most common need.
				if (iGamePath.Length())
					{
					const TInt s = iGamePath.LocateReverse('\\');
					iBrowsePath = iGamePath.Left(s + 1);
					}
				else
					iBrowsePath = KImgDir;
				iScanMode = EScanGames;
				ScanDirL();
				iPage = EPageFiles;
				break;
			case ECfgSaveState:
				DoSaveState();
				break;
			case ECfgLoadState:
				DoLoadState();
				break;
			case ECfgAdd:
				// Start from the card root: what is being imported was just copied
				// there from a PC, not placed into the emulator directory.
				iBrowsePath = _L("E:\\");
				iScanMode = EScanImport;
				iPicked->Reset();
				ScanDirL();
				iPage = EPageImport;
				break;
			case ECfgBios:
				iBrowsePath = KBiosDir;
				iScanMode = EScanBios;
				ScanDirL();
				iPage = EPageBios;
				break;
			case ECfgKeys:
				iKeySel = 0;
				iCapture = -1;
				iPage = EPageKeys;
				break;
			case ECfgReset:
				// Two presses required: the key bindings and tuned speed hacks are too
				// expensive a loss for an accidental OK.
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
				if (iGameClosed)
					CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
				else if (!iGamePath.Length() && !iStartedOnce)
					{
					iBrowsePath = KImgDir;
					iScanMode = EScanGames;
					ScanDirL();
					iPage = EPageFiles;
					}
				else if (iStartedOnce)
					{
					if (RestartNeeded())
						{
						iAsk = EAskRestart;
						iAskSel = 0;
						iPage = EPageAsk;
						}
					else
						DoCloseMenu(EFalse);
					}
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

// --- Import -----------------------------------------------------------------

void CPs1Container::CurrentEntry(TDes &aOut) const
	{
	aOut.Zero();
	if (iBrowseSel < 0 || iBrowseSel >= iBrowse->Count())
		return;
	const TPtrC n = (*iBrowse)[iBrowseSel];
	if (n.Right(1) == _L("\\"))
		return;
	if (iBrowsePath.Length() + n.Length() >= aOut.MaxLength())
		return;
	aOut = iBrowsePath;
	aOut.Append(n);
	}

TInt CPs1Container::PickedIndex(const TDesC &aPath) const
	{
	for (TInt i = 0; i < iPicked->Count(); i++)
		if ((*iPicked)[i].CompareF(aPath) == 0)
			return i;
	return KErrNotFound;
	}

void CPs1Container::TogglePickL()
	{
	TFileName full;
	CurrentEntry(full);
	if (!full.Length())
		return;
	const TInt at = PickedIndex(full);
	if (at >= 0)
		iPicked->Delete(at);
	else
		iPicked->AppendL(full);
	}

void CPs1Container::StartImportL()
	{
	if (!iPicked->Count())
		return;
	// The core holds the disc image open and reads from it every frame, while the
	// converter needs to read it and (with consent) delete it. Shut down the pass
	// BEFORE starting the queue.
	if (RunningGamePicked())
		{
		ps1GuiCloseGame();
		iGameClosed = ETrue;
		}
	const TInt r = ConvStart(*iPicked, (TConvMode)iConvMode,
		iAskSel == 0 /*delete*/, KImgDir);
	iPage = EPageProgress;
	if (r != KErrNone)
		ps1LogInt("import", "start failed", r);
	}

TKeyResponse CPs1Container::ImportKeyL(TInt aScan)
	{
	const TInt n = iBrowse->Count();
	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		iPage = EPageMain;
		DrawNow();
		return EKeyWasConsumed;
		}
	// Left soft key = "start". In-game it takes a screenshot, but the menu captures
	// keys entirely, so there is no conflict.
	if (aScan == EStdKeyDevice0)
		{
		if (iPicked->Count())
			{
			iAsk = EAskMode;
			iAskSel = 0;
			iPage = EPageAsk;
			}
		DrawNow();
		return EKeyWasConsumed;
		}
	if (aScan == EStdKeyUpArrow && n)
		iBrowseSel = (iBrowseSel + n - 1) % n;
	else if (aScan == EStdKeyDownArrow && n)
		iBrowseSel = (iBrowseSel + 1) % n;
	else if (aScan == EStdKeyLeftArrow)
		BrowseUpL();
	else if (aScan == EStdKeyRightArrow)
		{
		// Enter a directory with the arrow key, while OK is reserved for toggling
		// the checkmark: otherwise the same key would mean different things on a
		// file vs a directory.
		if (n && (*iBrowse)[iBrowseSel].Right(1) == _L("\\"))
			BrowseEnterL();
		}
	else if (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
		{
		if (n && (*iBrowse)[iBrowseSel].Right(1) == _L("\\"))
			BrowseEnterL();
		else
			TogglePickL();
		}
	DrawNow();
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::BiosKeyL(TInt aScan)
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
		{
		if (n && (*iBrowse)[iBrowseSel].Right(1) == _L("\\"))
			BrowseEnterL();
		else
			{
			TFileName full;
			CurrentEntry(full);
			if (full.Length())
				{
				const TInt s = full.LocateReverse('\\');
				const TPtrC dir = full.Left(s + 1);
				if (!ConvIsArchiveName(full) && dir.CompareF(KBiosDir) == 0)
					{
					// File is already in the BIOS directory: cannot copy it into
					// itself - Replace on a file open for reading would fail.
					iCfg.iBios = full.Mid(s + 1);
					}
				else
					{
					RFs fs;
					if (fs.Connect() == KErrNone)
						{
						TFileName name;
						// Extract in place: the BIOS is half a megabyte, a separate
						// thread and a progress bar here would be overkill.
						const TInt r = ConvExtractOne(fs, full, KNullDesC,
							KBiosDir, name);
						fs.Close();
						if (r == KErrNone)
							iCfg.iBios = name;
						ps1LogInt("bios", "pick", r);
						}
					}
				iPage = EPageMain;
				}
			}
		}
	DrawNow();
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::AskKeyL(TInt aScan)
	{
	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		iPage = (iAsk == EAskRestart) ? EPageMain : EPageImport;
		DrawNow();
		return EKeyWasConsumed;
		}
	if (aScan == EStdKeyUpArrow || aScan == EStdKeyDownArrow)
		iAskSel = iAskSel ? 0 : 1;
	else if (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
		{
		if (iAsk == EAskMode)
			{
			iConvMode = (iAskSel == 0) ? EConvModePbp : EConvModeCopy;
			iAsk = EAskDelete;
			// Default is delete: card space is exactly what it is, and keeping two
			// copies of the same disc makes no sense.
			iAskSel = 0;
			}
		else if (iAsk == EAskDelete)
			{
			StartImportL();
			}
		else if (iAsk == EAskRestart)
			{
			if (iAskSel == 0)
				{
				DoCloseMenu(ETrue);
				}
			else
				{
				RevertRestartSettings();
				DoCloseMenu(EFalse);
				}
			}
		}
	DrawNow();
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::ErrorKeyL(TInt /*aScan*/)
	{
	iPage = EPageMain;
	DrawNow();
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::ProgressKeyL(TInt aScan)
	{
	TConvStat st;
	ConvStat(st);
	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		if (st.iState == EConvRun)
			ConvCancel();
		else
			{
			ConvRelease();
			iPicked->Reset();
			// Freshly imported games should appear in the list immediately.
			iScanMode = EScanGames;
			iBrowsePath = KImgDir;
			iPage = EPageMain;
			}
		DrawNow();
		}
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::KeysKeyL(TInt aScan)
	{
	// Capture mode: the next key press becomes the binding. RSK is reserved for us -
	// it cancels, and it is the key that opens the menu in-game, so assigning it is not allowed.
	if (iCapture >= 0)
		{
		if (aScan != EStdKeyDevice1)
			{
			iCfg.iKeyScan[iCapture] = aScan;
			iCfg.iKeyAlt[iCapture] = -1;    // alt code was part of the default layout
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

// A leave from here costs ALL window events: CONE re-arms the request for the next event
// at the end of its RunL, and if control bypasses that line, keys and focus never arrive
// again even though the application keeps counting frames. That is exactly how the game
// went deaf after exiting the menu. We trap here rather than hoping nothing below leaves.
TKeyResponse CPs1Container::OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType)
	{
	TKeyResponse r = EKeyWasNotConsumed;
	TRAPD(e, r = DispatchKeyL(aKeyEvent, aType));
	if (e != KErrNone)
		ps1LogFmt("key", "leave=%d sc=%d page=%d sel=%d", (int)e,
			(int)aKeyEvent.iScanCode, (int)iPage, (int)iSel);
	// KLeaveExit is not an error but the way CEikAppUi::Exit unwinds the stack. If we
	// swallow it here, "Exit" in the menu simply does nothing: the application stays
	// alive, and the user sees a frozen frame with a live menu.
	if (e == KLeaveExit)
		User::Leave(e);
	return r;
	}

TKeyResponse CPs1Container::DispatchKeyL(const TKeyEvent &aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKeyDown && aType != EEventKeyUp)
		return EKeyWasNotConsumed;

	// The menu captures keys entirely.
	if (iPage != EPageNone)
		{
		if (aType != EEventKeyDown)
			return EKeyWasConsumed;
		const TInt sc = aKeyEvent.iScanCode;
		if (iPage == EPageFiles)    return FilesKeyL(sc);
		if (iPage == EPageKeys)     return KeysKeyL(sc);
		if (iPage == EPageImport)   return ImportKeyL(sc);
		if (iPage == EPageBios)     return BiosKeyL(sc);
		if (iPage == EPageAsk)      return AskKeyL(sc);
		if (iPage == EPageProgress) return ProgressKeyL(sc);
		if (iPage == EPageError)    return ErrorKeyL(sc);
		return MainKeyL(sc);
		}

	// Right soft key in-game opens the menu. The CBA is hidden for full-screen, so
	// there is no label, but the usual position remains functional. Exception: launch
	// from the command line, where there are no key presses, and benchmark behavior
	// must not be altered.
	if (aType == EEventKeyDown && aKeyEvent.iScanCode == EStdKeyDevice1)
		{
		if (iCliMode)
			CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
		else
			OpenMenu();
		return EKeyWasConsumed;
		}

	// User key bindings go FIRST, before system keys: otherwise a button assigned to
	// the asterisk or the left soft key would silently not work - screenshot and volume
	// would consume them earlier. The default layout does not use them, so for an
	// unmodified config the order changes nothing.
	if (voutSymbianKeyEvent(aKeyEvent.iScanCode, aType == EEventKeyDown))
		return EKeyWasConsumed;

	// Left soft key = on-demand screenshot.
	if (aType == EEventKeyDown && aKeyEvent.iScanCode == EStdKeyDevice0)
		{
		ps1ScreenShot();
		return EKeyWasConsumed;
		}

	// Volume: side keys of the device plus asterisk/hash. Both sets of keycodes for
	// * and # are handled - firmwares send either the keypad code or the ASCII code.
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

// --- AppUi: owns the container and the emulation tick ------------------------

class CPs1AppUi : public CAknAppUi
	{
public:
	void ConstructL();
	~CPs1AppUi();
	void HandleCommandL(TInt aCommand);
	void HandleResourceChangeL(TInt aType);
	void HandleForegroundEventL(TBool aForeground);
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

	// Full screen: the game gets the entire display, not ClientRect without the status
	// pane and CBA. Landscape is set explicitly - otherwise the window geometry changes
	// mid-game following the accelerometer.
	CEikStatusPane *sp = StatusPane();
	if (sp) sp->MakeVisible(EFalse);
	if (Cba()) Cba()->MakeVisible(EFalse);
	TRAP_IGNORE(SetOrientationL(EAppUiOrientationLandscape));

	iContainer = CPs1Container::NewL(ApplicationRect());
	AddToStackL(iContainer);

	AttachOutput();
	// Whether EGL came up is only visible from here: further on the fallback to BitBlt
	// is silent, and without this line "gl gave no speedup" is indistinguishable from
	// "gl did not activate".
	ps1LogInt("gui", "window attached, gl=", voutSymbianGlActive());

	// Lowest priority: CONE events (keys, redraws, exit) are serviced between frames,
	// not after the session ends.
	iIdle = CIdle::NewL(CActive::EPriorityIdle);
	iIdle->Start(TCallBack(TickCb, this));
	}

CPs1AppUi::~CPs1AppUi()
	{
	delete iIdle;
	// Order matters: output is detached from the window BEFORE the container is destroyed.
	ps1GuiShutdown();
	voutSymbianFinish(0);   // in case exit happened before the core was started
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
	// Reentrancy guard: the frame ticks audio via the scheduler, and CIdle is already
	// re-armed by that point - without this flag the emulation step would recursively
	// enter itself until the stack is exhausted.
	if (iInTick)
		return ETrue;
	iInTick = ETrue;

	// While the menu is open, do not touch the core: before start the disc and mode
	// selection must take effect before initialization; in-game this is simply a pause.
	// CIdle keeps spinning so that CONE services key events.
	if (iContainer && iContainer->MenuActive())
		{
		iContainer->MenuTick();
		// Without a pause the main thread would spin CIdle at full speed and starve
		// the conversion worker thread (and outside conversion just heat the device).
		User::After(20000);
		iInTick = EFalse;
		return ETrue;
		}

	TInt more = ETrue;
	// Core initialization happens in the first tick, not in ConstructL: it is long
	// (BIOS, disc image, audio open), and the window must appear before it.
	if (!iStarted)
		{
		iStarted = ETrue;
		// Re-attach output: DSA is brought up right here, and the flag from the menu
		// arrives in ps1CfgApply AFTER the initial binding in ConstructL.
		// Without this, "Direct draw: on" would only take effect from the next launch.
		AttachOutput();
		if (ps1GuiSetup() != KErrNone)
			{
			ps1Log("gui", "emu setup failed");
			iStarted = EFalse;
			if (iContainer)
				iContainer->ShowError();
			}
		}
	// Pass restart after the menu: different disc or different core branch. Done here,
	// outside the key handler - inside there is audio teardown and DSA re-initialization.
	else if (iContainer && iContainer->TakeRestart())
		{
		// Order matters: the old pass is torn down FIRST, because it is the one that
		// detaches output, and only then the window is given to the new one. Reversed,
		// the game would render frames into the void while the screen shows the last
		// frame of the previous game.
		ps1GuiEndPass();
		iContainer->ApplyAndStart();
		AttachOutput();
		if (ps1GuiStartPass() != KErrNone)
			{
			ps1Log("gui", "emu restart failed");
			iStarted = EFalse;
			if (iContainer)
				iContainer->ShowError();
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

// A screen rotation changes the window size, but the frame bitmap was created for the
// previous one - we recreate it along with the container geometry.
void CPs1AppUi::HandleResourceChangeL(TInt aType)
	{
	CAknAppUi::HandleResourceChangeL(aType);
	if (aType == KEikDynamicLayoutVariantSwitch && iContainer)
		{
		iContainer->SetRect(ApplicationRect());
		AttachOutput();
		}
	}

// A focus loss would explain "keys do not work" with no crash on our side: DSA draws
// the frame on top of everything, but events go to a different window.
void CPs1AppUi::HandleForegroundEventL(TBool aForeground)
	{
	CAknAppUi::HandleForegroundEventL(aForeground);
	ps1LogInt("focus", "fore", (int)aForeground);
	}

void CPs1AppUi::HandleCommandL(TInt aCommand)
	{
	switch (aCommand)
		{
		case EEikCmdExit:
		case EAknSoftkeyExit:
			// First ask the frame loop to finish; the destructor does the rest.
			ps1GuiRequestExit();
			Exit();
			break;
		default:
			break;
		}
	}

// --- Document and application -----------------------------------------------

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

// Registration in apparc - a prerequisite for launching: EikStart resolves the
// application through it, and until it existed the framework silently died with
// KErrNotFound before reaching our code.
// Kept as two lines in the log: also shows WHICH binary was actually launched -
// deploy puts the fresh one on top of the installed one, and mixing up versions is easy.
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

// GUI branch entry point. Does not return until the application is closed.
extern "C" int ps1AppRun(void)
	{
	probeStartupEnv();
	return EikStart::RunApplication(NewApplication);
	}
