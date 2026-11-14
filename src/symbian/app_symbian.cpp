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
#include <utf.h>
#include <hal.h>
#include <remconinterfaceselector.h>
#include <remconcoreapitarget.h>
#include <remconcoreapitargetobserver.h>

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
void ps1GuiHeartbeat(void);
int  ps1GuiStartPass(void);
void ps1Log(const char *aType, const char *aMsg);
void ps1LogInt(const char *aType, const char *aMsg, int aVal);
void ps1LogFmt(const char *aType, const char *aFmt, ...);
int  ps1SndVolumeStep(int aDelta);
int  ps1SndVolumeSteps(void);
void ps1SndResetRate(void);
int  voutSymbianKeyEvent(int aScanCode, int aDown);
int  voutSymbianAliasKey(int aScanCode);
int  voutSymbianFinish(int aHoldMs);
int  voutSymbianGlActive(void);
void ps1ScreenShot(void);
void ps1SetDiscFromCfg(const char *aDisc);
void ps1CfgApply(const char *aDisc, const char *aBios, int aInterp, int aIlace,
	int aLight, int aBlend, int aPixSkip, int aGteNf, int aSound, int aGl,
	int aDirty, int aDsa, int aFps, int aGpuSkip, int aCycle, int aSmooth,
	int aFxaa, int aSwDraw, int aWsola);
// Subset that can be toggled while in-game (menu from pause).
void ps1CfgApplyLive(int aIlace, int aLight, int aBlend, int aPixSkip,
	int aFps, int aGpuSkip, int aCycle, int aSmooth, int aFxaa, int aDirty,
	int aSwDraw);
extern "C" void voutSymbianSetTilt(int aOn);
extern "C" void voutSymbianTiltRezero(void);
extern "C" void ps1SetDebugLog(int aOn);
extern "C" void voutSymbianSetTiltMap(int aSide, int aPitch, int aDead, int aSensPct);
extern "C" int voutSymbianTiltActive(void);
extern "C" int ps1TiltCli(void);
extern "C" int ps1KeyProbeCli(void);
TBool ps1StateIoFailed();
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

// On-screen gamepad input (touchpad_symbian.cpp). The probe runs in ConstructL, before ActivateL.
TBool voutSymbianTouchProbe(RWindowBase &aWin);
void voutSymbianTouchOff(void);
void voutSymbianTouchOptions(TInt aSticks, TInt aAlphaPct);
TBool voutSymbianTouchEvent(const TPointerEvent &aEvent);

// Same UID as in stage1.mmp and stage1_reg.rss.
static const TUid KUidPs1App = { 0xE0001002 };

// --- Settings ---------------------------------------------------------------

// We draw the menu ourselves rather than via CAknSettingItemList: we already have
// our own window and key handling, and the AVKON list would pull in resource
// definitions and control hierarchy rebuilding for the same result.
//
// Item order = display order. The item's section is given by KItemSection, so
// items can only be reordered together with it.
// Sections form an accordion: a header row, then the section's rows, main ones first.
// Only the open section's rows are visible (ItemVisible), and only one is open at a time.
enum TCfgItem
	{
	// There is intentionally no DISPLAY skip ("Show every") here: it renders the
	// frame and discards it, i.e. pays for the expensive part. Measured -4.3% vs
	// -22.5% for ECfgGpuSkip. Kept as a command-line probe "showskip".
	ECfgStart = 0,
	ECfgHdrGame, ECfgGame, ECfgSaveState, ECfgLoadState, ECfgAdd,
	ECfgHdrVideo, ECfgSmooth, ECfgFxaa, ECfgFps,
	ECfgLight, ECfgBlend, ECfgDirty, ECfgDsa, ECfgGl, ECfgSwDraw, ECfgIlace, ECfgPixSkip,
	ECfgHdrSpeed, ECfgGpuSkip, ECfgCpu, ECfgGteNf, ECfgCycle,
	ECfgHdrSound, ECfgSound, ECfgWsola,
	ECfgHdrInput, ECfgPreset, ECfgKeyBase, ECfgKeys,
	ECfgTilt, ECfgTiltSide, ECfgTiltPitch, ECfgTiltDead, ECfgTiltSens, ECfgResetKeys,
	ECfgHdrSystem, ECfgLang, ECfgBios, ECfgProfile, ECfgDebug, ECfgReset,
	ECfgHelp, ECfgExit,
	ECfgCount
	};

enum TCfgSection
	{
	ESecGame = 0, ESecVideo, ESecSpeed, ESecSound, ESecInput, ESecSystem, ESecCount
	};

static const TStrId KSecStrId[ESecCount] =
	{ EStrSecGame, EStrSecVideo, EStrSecSpeed, EStrSecSound, EStrSecControls, EStrSecSystem };

// A section's header row.
static const unsigned char KSecHdr[ESecCount] =
	{ ECfgHdrGame, ECfgHdrVideo, ECfgHdrSpeed, ECfgHdrSound, ECfgHdrInput, ECfgHdrSystem };

// ESecCount in the table means no section: Start, Help and Exit stay on the top level.
static const unsigned char KItemSection[ECfgCount] =
	{
	ESecCount,
	ESecGame, ESecGame, ESecGame, ESecGame, ESecGame,
	ESecVideo, ESecVideo, ESecVideo, ESecVideo, ESecVideo, ESecVideo, ESecVideo,
	ESecVideo, ESecVideo, ESecVideo, ESecVideo, ESecVideo,
	ESecSpeed, ESecSpeed, ESecSpeed, ESecSpeed, ESecSpeed,
	ESecSound, ESecSound, ESecSound,
	ESecInput, ESecInput, ESecInput, ESecInput, ESecInput, ESecInput, ESecInput,
	ESecInput, ESecInput, ESecInput,
	ESecSystem, ESecSystem, ESecSystem, ESecSystem, ESecSystem, ESecSystem,
	ESecCount, ESecCount
	};

static const TStrId KItemLabelStrId[ECfgCount] =
	{
	EStrItemStart,
	EStrSecGame, EStrItemGame, EStrItemSaveState, EStrItemLoadState, EStrItemAdd,
	EStrSecVideo, EStrItemSmooth, EStrItemFxaa, EStrItemFps,
	EStrItemLight, EStrItemBlend, EStrItemDirty, EStrItemDsa, EStrItemGl,
	EStrItemSwDraw, EStrItemIlace, EStrItemPixSkip,
	EStrSecSpeed, EStrItemGpuSkip, EStrItemCpu, EStrItemGteNf, EStrItemCycle,
	EStrSecSound, EStrItemSound, EStrItemWsola,
	EStrSecControls, EStrItemPreset, EStrItemKeyBase, EStrItemKeys,
	EStrItemTilt, EStrItemTiltSide, EStrItemTiltPitch, EStrItemTiltDead,
	EStrItemTiltSens, EStrItemResetKeys,
	EStrSecSystem, EStrItemLang, EStrItemBios, EStrItemProfile, EStrItemDebug,
	EStrItemReset,
	EStrItemHelp, EStrItemExit
	};

// Key names in the settings file. NULL means a command item with no value.
// The reader looks up by name, so adding or reordering items is safe:
// the old file remains valid, missing keys get their default value.
static const char *const KCfgKey[ECfgCount] =
	{
	NULL,
	NULL, NULL, NULL, NULL, NULL,
	NULL, "smooth", "fxaa", "fps", "light", "blend", "dirty", "dsa", "gl",
	"swdraw", "ilace", "pixskip",
	NULL, "gpuskip", "cpu", "gtenf", "cycle",
	NULL, "sound", "wsola",
	NULL, NULL, NULL, NULL, "tilt", "tiltside", "tiltpitch", "tiltdead",
	"tiltsens", NULL,
	NULL, "lang", NULL, "profile", "debug", NULL,
	NULL, NULL
	};

// Stable ids of the rows in the help files ("hint.<id>"): they do not move with the
// order of the rows, so a translated file stays valid when the menu is rearranged.
static const char *const KItemHelpId[ECfgCount] =
	{
	"start",
	"sec.game", "game", "savestate", "loadstate", "add",
	"sec.video", "smooth", "fxaa", "fps", "light", "blend", "dirty",
	"dsa", "gl", "swdraw", "ilace", "pixskip",
	"sec.speed", "gpuskip", "cpu", "gtenf", "cycle",
	"sec.sound", "sound", "wsola",
	"sec.input", "preset", "keybase", "keys", "tilt", "tiltside",
	"tiltpitch", "tiltdead", "tiltsens", "resetkeys",
	"sec.system", "lang", "bios", "profile", "debug", "reset",
	"help", "exit"
	};

// Require re-initialization of the core or output: do not take effect from
// the in-game menu, marked with an asterisk in the list.
static TBool restartOnly(TInt aItem)
	{
	return aItem == ECfgGame || aItem == ECfgBios || aItem == ECfgCpu ||
	       aItem == ECfgGteNf || aItem == ECfgSound || aItem == ECfgWsola ||
	       aItem == ECfgGl ||
	       aItem == ECfgDsa;
	}

static TBool isHeader(TInt aItem)
	{
	return aItem == ECfgHdrGame || aItem == ECfgHdrVideo || aItem == ECfgHdrSpeed ||
		   aItem == ECfgHdrSound || aItem == ECfgHdrInput || aItem == ECfgHdrSystem;
	}

static TBool isCommand(TInt aItem)
	{
	return aItem == ECfgStart || aItem == ECfgGame || aItem == ECfgSaveState ||
		   aItem == ECfgLoadState || aItem == ECfgAdd || aItem == ECfgBios ||
		   aItem == ECfgKeys || aItem == ECfgResetKeys || aItem == ECfgReset ||
		   aItem == ECfgHelp ||
		   aItem == ECfgExit || isHeader(aItem);
	}

// PS1 cycle multiplier. Upstream default is CYCLE_MULT_DEFAULT (175); higher means
// "PS1 CPU is slower", i.e. less work for us and less smoothness for the game.
const TInt KCycleDefault = 175;
const TInt KCycleMin = 100;
const TInt KCycleMax = 300;
const TInt KCycleStep = 25;

// Generous upper bound on the number of buttons reported by vout: layout is defined there.
const TInt KPadMax = 16;

// --- Control presets (F3) ---------------------------------------------------

// Ready-made layouts offered from the CONTROLS menu. Default is not tabulated
// below: it is reproduced from voutSymbianDefaultKey, the same single source
// of truth TPs1Cfg::SetDefaults() already uses, so there is nowhere for the
// two to drift apart.
enum TControlPreset { EPresetDefault = 0, EPresetRacing, EPresetFighting, EPresetRpg, EPresetCount };

static const TStrId KPresetNameStrId[EPresetCount] =
	{ EStrValPresetDefault, EStrValPresetRacing, EStrValPresetFighting, EStrValPresetRpg };

// Row order matches KPadBit in vout_symbian.cpp: UP DOWN LEFT RIGHT CROSS
// CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 SELECT START. Sized to the button count
// as of this writing (14); ApplyPreset() clamps to it defensively.
const TInt KPresetBtnCount = 14;

// Racing: GT's own digital scheme is Cross = accelerate, Square = brake, so
// those two land on the keys easiest to hold with a thumb resting on the
// D-pad - the OK/5 pair, and the key right below it. Accelerate toggles by
// default so a race is not held down with a finger for its whole length.
// Steering is left on the D-pad's own Left/Right, unchanged from Default.
static const TInt KPresetScan[EPresetCount - 1][KPresetBtnCount] =
	{
		{ // Racing
		EStdKeyUpArrow, EStdKeyDownArrow, EStdKeyLeftArrow, EStdKeyRightArrow,
		EStdKeyDevice3, '0', '2', '8',
		'7', '9', '1', '3', '4', '6'
		},
		{ // Fighting: the four face buttons sit on the numeric keypad's own
		// plus shape (2/4/6/8 around 5) - the same diamond the PS1 pad already
		// draws them in, so one thumb rolls across it the way it would the pad.
		EStdKeyUpArrow, EStdKeyDownArrow, EStdKeyLeftArrow, EStdKeyRightArrow,
		'2', '6', '4', '8',
		'7', '9', '1', '3', '0', EStdKeyDevice3
		},
		{ // RPG: confirm/cancel on OK and C as asked; the rest keeps Default's
		// digits so the change from Default is only the two named keys.
		EStdKeyUpArrow, EStdKeyDownArrow, EStdKeyLeftArrow, EStdKeyRightArrow,
		EStdKeyDevice3, EStdKeyBackspace, '7', '9',
		'1', '3', '4', '6', '2', '8'
		}
	};
static const TInt KPresetAlt[EPresetCount - 1][KPresetBtnCount] =
	{
	{ -1, -1, -1, -1, '5', -1, -1, -1, -1, -1, -1, -1, -1, -1 },   // Racing
	{ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },    // Fighting
	{ -1, -1, -1, -1, '5', -1, -1, -1, -1, -1, -1, -1, -1, -1 }    // RPG
	};
static const TInt KPresetHold[EPresetCount - 1][KPresetBtnCount] =
	{
	{ 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },   // Racing: accelerate toggles
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },   // Fighting: no holds
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }    // RPG: no holds
	};

// Pseudo scan codes of the RemCon media operations. They sit above every TStdScanCode
// (the highest in the SDK is 0xE8) and below 0x8000, since the key map keeps codes in a short.
enum TRemConKey
	{
	ERemConKeyPlayPause = 0x1001, ERemConKeyPlay, ERemConKeyPause, ERemConKeyStop,
	ERemConKeyRewind, ERemConKeyFastFwd, ERemConKeyNext, ERemConKeyPrev
	};

// Base layout for the physical keyboard, independent of the game-type preset above:
// every one of these Nokias has a D-pad, and the N95 and C5 bases steer with it. N95 reuses the
// Default layout verbatim. C5-00.2 keeps the D-pad too but also answers to the numeric
// keypad's 2/4/6/8 as a second key for the four directions (the way the old numpad-only
// Symbian PS1 emulators did), with 5 on L2 and */# for Select/Start. Each button has
// one Alt: the directions take the digits, Select and Start take the named codes, and
// L1 and R1 take the volume keys. A bound volume key is consumed by its binding before
// DispatchKeyL reaches the volume step, so it does not change the volume as well.
enum TKeyboardBase { EBaseN95 = 0, EBaseC5, EBaseE90, EBaseSlider, EBaseCount };

static const TStrId KBaseNameStrId[EBaseCount] =
	{ EStrValBaseN95, EStrValBaseC5, EStrValBaseE90, EStrValBaseSlider };

static const TInt KBaseC5Scan[KPresetBtnCount] =
	{
	EStdKeyUpArrow, EStdKeyDownArrow, EStdKeyLeftArrow, EStdKeyRightArrow,
	EStdKeyDevice3, '0', '7', '9',
	'1', '3', '5', EStdKeyBackspace, '*', '#'
	};
// * and # each get their named scan code as the alt: firmwares are known to send
// either the ASCII character or the named code for these two (see the volume
// handler in OfferKeyEventL, which checks both for the same reason).
static const TInt KBaseC5Alt[KPresetBtnCount] =
	{ '2', '8', '4', '6', -1, -1, -1, -1, EStdKeyIncVolume, EStdKeyDecVolume,
	  -1, -1, EStdKeyNkpAsterisk, EStdKeyHash };

// E90 internal keyboard, two hands on two mirrored groups: W/A/S/D steer with Q/E as
// L1/L2 beside W, the face buttons sit one column right of the home row (O, K, L and the
// ;/Zh key) with P/I as R1/R2 beside O, and G/H in between are Select and Start. The
// D-pad is a second set of face buttons in the PlayStation diamond, its centre a second
// R1 and 0 a second R2. Layout as Pavlo set it on his E90 on 2026-10-24.
static const TInt KBaseE90Scan[KPresetBtnCount] =
	{
	'W', 'S', 'A', 'D',
	'L', EStdKeySemiColon, 'K', 'O',
	'Q', 'P', 'E', 'I', 'G', 'H'
	};
static const TInt KBaseE90Alt[KPresetBtnCount] =
	{ -1, -1, -1, -1,
	  EStdKeyDownArrow, EStdKeyRightArrow, EStdKeyLeftArrow, EStdKeyUpArrow,
	  -1, EStdKeyDevice3, -1, '0', -1, -1 };

// Dual-slider handsets (N95, N95 8GB, N96, N85, N86, N81), held sideways with the media
// slider open: the D-pad steers under the right thumb, the four media keys under the left
// are the face buttons, the volume keys on the top edge are L1/L2, the camera and gallery
// keys R1/R2, the pencil and the green key are Select and Start, with C and the left
// soft key as second keys for handsets without a pencil. Layout as Pavlo set it on his
// N95 8GB on 2026-10-24, where 0xe2 is the camera key and 0xe6 the gallery key. The media
// keys take the RemCon pseudo codes, which the ps1slide anim and vout's alias produce
// whichever way a key arrives; the anim is loaded only while a binding uses one.
static const TInt KBaseSliderScan[KPresetBtnCount] =
	{
	EStdKeyUpArrow, EStdKeyDownArrow, EStdKeyLeftArrow, EStdKeyRightArrow,
	ERemConKeyPlay, ERemConKeyNext, ERemConKeyStop, ERemConKeyPrev,
	EStdKeyIncVolume, EStdKeyApplication19,
	EStdKeyDecVolume, EStdKeyApplication1D,
	EStdKeyLeftShift, EStdKeyYes
	};
static const TInt KBaseSliderAlt[KPresetBtnCount] =
	{
	-1, -1, -1, -1,
	-1, -1, -1, -1,
	-1, -1,
	-1, -1,
	EStdKeyBackspace, EStdKeyDevice0
	};

// What the ps1slide anim renames the raw media scan codes to: F21..F24, which no Nokia
// keyboard sends and no firmware handler takes, and which vout aliases to the RemCon
// pseudo codes. The raw codes are the SDK's EKeyPlay..EKeyRewind scan codes; the anim's
// GetLast, logged on the Controls page, is how a handset that sends others shows them.
static const TInt KSlideMap[] =
	{
	5,
	EStdKeyApplication2, EStdKeyF21,     // Play
	EStdKeyApplication3, EStdKeyF22,     // Stop
	EStdKeyApplication4, EStdKeyF23,     // Forward
	EStdKeyApplication5, EStdKeyF24,     // Rewind
	// The N95 8GB gallery key: bound, it still opened the gallery (Pavlo, 2026-10-24).
	// vout turns F20 back into 0xe6, so bindings keep the key's own code.
	EStdKeyApplication1D, EStdKeyF20
	};

// Single emulator directory. Everything it reads and writes lives under it: otherwise
// the card root got cluttered with files, and which ones belong to which game was unclear.
_LIT(KCfgPath, "E:\\PS1\\cfg\\ps1_settings.txt");
_LIT(KProfDir, "E:\\PS1\\cfg\\");
_LIT(KImgDir,  "E:\\PS1\\games\\");
_LIT(KBiosDir, "E:\\PS1\\bios\\");
_LIT(KSavesDir, "E:\\PS1\\saves\\");
_LIT(KHelpDir, "E:\\PS1\\help\\");
// The dump stage1.cpp falls back to when the profile names none.
_LIT(KBiosDefault, "scph7502.bin");

class TPs1Cfg
	{
public:
	void SetDefaults();
	void SetControlDefaults();
	void Clamp();
	TInt &Value(TInt aItem);
	TInt iInterp, iIlace, iLight, iBlend, iPixSkip, iGteNf, iSound, iGl, iWsola;
	TInt iDirty, iDsa, iFps, iGpuSkip, iCycle, iSmooth, iFxaa, iProfile, iSwDraw;
	TInt iLang;
	// Debug log: global only, like the language, so one switch covers every game.
	TInt iDebug;
	// First-run marker, kept in the global file only (intro=1): the setup page opens
	// at start until it is set. Not part of SetDefaults, so a reset does not clear it.
	TInt iIntro;
	TInt iTilt, iTiltSide, iTiltPitch, iTiltDead, iTiltSens;
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
	iIntro = 0;
	iCycle = KCycleDefault;
	SetControlDefaults();
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
	// Linear stretch is the long-standing default: WSOLA costs extra CPU, and a change
	// in emulated pitch should not appear silently for anyone not asking for it.
	iWsola = 0;
	iLang = I18nDetectLanguage();
	I18nSetLanguage(iLang);
	iBios.Zero();
	// Per-game settings: key bindings and speed hacks needed in a racing game vs an
	// RPG are different, and flipping through them manually each time is the same menu twice.
	iProfile = 1;
	iDebug = 0;
	// Display by gpulib latch: measured -6.7 ms per frame with no image quality loss.
	iDirty = 1;
	// Direct screen buffer write: measured -0.93 ms per frame. It was broken because of
	// Update() without a region (bitgdi did not know about our write, so it pushed nothing);
	// with an explicit region the image is correct - verified on N95 8GB.
	iDsa = 1;
	}

// The CONTROLS section alone, for its own reset: keys, holds, the tilt zero key and tilt.
void TPs1Cfg::SetControlDefaults()
	{
	// Tilt is off until asked for: it costs a sensor session and only one phone has one.
	iTilt = 0;
	// Sideways to the left stick X, forward-back to its Y: the binding measured on
	// the phone, where the stick follows the tilt as a marble on the screen would.
	iTiltSide = 1;
	iTiltPitch = 3;
	iTiltDead = 10;
	iTiltSens = 100;
	for (TInt i = 0; i < KPadMax; i++)
		{
		iKeyScan[i] = voutSymbianDefaultKey(i, 0);
		iKeyAlt[i]  = voutSymbianDefaultKey(i, 1);
		iKeyHold[i] = 0;
		}
	// The row after the PS1 buttons is the tilt zero key. C is free in the default
	// layout and in Racing and Fighting.
	const TInt tiltRow = voutSymbianPadButtons();
	if (tiltRow >= 0 && tiltRow < KPadMax)
		iKeyScan[tiltRow] = EStdKeyBackspace;
	}

// A hand-edited or truncated settings file must not reach the core or the menu
// arithmetic (cycle=0 divided by zero on the first redraw): bad values fall back.
void TPs1Cfg::Clamp()
	{
	if (iCycle < KCycleMin || iCycle > KCycleMax) iCycle = KCycleDefault;
	if (iGpuSkip < -1 || iGpuSkip > 3) iGpuSkip = 0;
	if (iFxaa < 0 || iFxaa > 5) iFxaa = 0;
	if (iTiltSide < 0 || iTiltSide > 8) iTiltSide = 1;
	if (iTiltPitch < 0 || iTiltPitch > 8) iTiltPitch = 3;
	if (iTiltDead < 0 || iTiltDead > 40) iTiltDead = 10;
	if (iTiltSens < 40 || iTiltSens > 300) iTiltSens = 100;
	iDebug = iDebug ? 1 : 0;
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
		case ECfgWsola:   return iWsola;
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
		case ECfgDebug:   return iDebug;
		case ECfgLang:    return iLang;
		case ECfgTilt:      return iTilt;
		case ECfgTiltSide:  return iTiltSide;
		case ECfgTiltPitch: return iTiltPitch;
		case ECfgTiltDead:  return iTiltDead;
		case ECfgTiltSens:  return iTiltSens;
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

// Lines of the action menu that OK opens on a row of the CONTROLS list.
enum TKeyAction { EKeyActMain = 0, EKeyActSecond, EKeyActClear, EKeyActCount };

static const TStrId KKeyActStrId[EKeyActCount] =
	{
	EStrKeyActMain, EStrKeyActSecond, EStrKeyActClear
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
		case EStdKeySemiColon:   aOut.Copy(_L(";"));             return;
		case EStdKeyIncVolume:   aOut.Copy(Tr(EStrKeyVolPlus));  return;
		case EStdKeyDecVolume:   aOut.Copy(Tr(EStrKeyVolMinus)); return;
		case EStdKeyApplication0:aOut.Copy(Tr(EStrKeyMenu));     return;
		case EStdKeyDevice7:     aOut.Copy(_L("Cam"));           return;
		case EStdKeyYes:         aOut.Copy(_L("Call"));          return;
		case ERemConKeyPlayPause:aOut.Copy(Tr(EStrKeyPlayPause)); return;
		case ERemConKeyPlay:     aOut.Copy(Tr(EStrKeyPlay));      return;
		case ERemConKeyPause:    aOut.Copy(Tr(EStrKeyPause));     return;
		case ERemConKeyStop:     aOut.Copy(Tr(EStrKeyStop));      return;
		case ERemConKeyRewind:   aOut.Copy(Tr(EStrKeyRewind));    return;
		case ERemConKeyFastFwd:  aOut.Copy(Tr(EStrKeyFastFwd));   return;
		case ERemConKeyNext:     aOut.Copy(Tr(EStrKeyNext));      return;
		case ERemConKeyPrev:     aOut.Copy(Tr(EStrKeyPrev));      return;
		default: break;
		}
	if (aScan < 0)
		aOut.Copy(_L("-"));
	else if (aScan >= 32 && aScan < 127)
		aOut.Format(_L("%c"), aScan);
	else
		aOut.Format(_L("k%d"), aScan);
	}

// --- Text helpers -----------------------------------------------------------

// Next line of aText that fits aWidth: breaks at the last space that fits, or inside a
// word wider than the line. Every '\n' ends a paragraph, and an empty paragraph comes
// out as an empty line.
static TBool wrapNext(const CFont *aFont, const TDesC &aText, TInt aWidth, TInt &aPos,
					  TPtrC &aLine)
	{
	if (aPos >= aText.Length())
		return EFalse;
	const TPtrC rest = aText.Mid(aPos);
	const TInt nl = rest.Locate('\n');
	const TPtrC para = (nl < 0) ? rest : rest.Left(nl);
	TInt take = para.Length();
	if (aFont->TextWidthInPixels(para) > aWidth)
		{
		take = aFont->TextCount(para, aWidth);
		if (take < 1)
			take = 1;
		if (take < para.Length() && para[take] != ' ')
			{
			const TInt sp = para.Left(take).LocateReverse(' ');
			if (sp > 0)
				take = sp;
			}
		}
	aLine.Set(para.Left(take));
	aPos += take;
	while (aPos < aText.Length() && aText[aPos] == ' ')
		aPos++;
	if (aPos < aText.Length() && aText[aPos] == '\n')
		aPos++;
	return ETrue;
	}

// Walks a help buffer. A section starts with "[key]" at the start of a line; aInline is
// the rest of that line (a title or a hint), aBody the lines up to the next section.
static TBool nextSection(const TDesC &aBuf, TInt &aPos, TPtrC &aKey, TPtrC &aInline,
						 TPtrC &aBody)
	{
	const TInt n = aBuf.Length();
	TInt p = aPos;
	for (;;)
		{
		while (p < n && aBuf[p] != '[')
			{
			const TInt nl = aBuf.Mid(p).Locate('\n');
			if (nl < 0)
				return EFalse;
			p += nl + 1;
			}
		if (p >= n)
			return EFalse;
		const TPtrC line = aBuf.Mid(p);
		const TInt eol = line.Locate('\n');
		const TInt close = line.Locate(']');
		if (close > 1 && (eol < 0 || close < eol))
			{
			const TInt lineEnd = (eol < 0) ? line.Length() : eol;
			aKey.Set(line.Mid(1, close - 1));
			TPtrC inl(line.Mid(close + 1, lineEnd - close - 1));
			while (inl.Length() && inl[0] == ' ')
				inl.Set(inl.Mid(1));
			while (inl.Length() && inl[inl.Length() - 1] == ' ')
				inl.Set(inl.Left(inl.Length() - 1));
			aInline.Set(inl);
			const TInt bodyStart = (eol < 0) ? n : p + eol + 1;
			TInt q = bodyStart;
			while (q < n && aBuf[q] != '[')
				{
				const TInt nl = aBuf.Mid(q).Locate('\n');
				if (nl < 0)
					{
					q = n;
					break;
					}
				q += nl + 1;
				}
			TPtrC body(aBuf.Mid(bodyStart, q - bodyStart));
			while (body.Length() && (body[body.Length() - 1] == '\n' ||
									 body[body.Length() - 1] == ' '))
				body.Set(body.Left(body.Length() - 1));
			aBody.Set(body);
			aPos = q;
			return ETrue;
			}
		p++;
		}
	}

// Help texts live on the card, not in the EXE: long text in every language does not
// fit the hex-array scheme of i18n.cpp, and a translation then needs no rebuild.
const TInt KHelpMax = 96 * 1024;

// UTF-8 file to UTF-16 with the CRs removed, so the parser only ever sees LF.
static HBufC *loadHelpFile(TInt aLang)
	{
	TFileName path(KHelpDir);
	TBuf<4> code;
	code.Copy(TPtrC8((const TUint8 *)I18nLanguageCode(aLang)));
	path.Append(code);
	path.Append(_L(".txt"));
	RFs fs;
	if (fs.Connect() != KErrNone)
		return NULL;
	HBufC *out = NULL;
	RFile f;
	TInt size = 0;
	if (f.Open(fs, path, EFileRead | EFileShareAny) == KErrNone)
		{
		if (f.Size(size) == KErrNone && size > 0 && size <= KHelpMax)
			{
			HBufC8 *raw = HBufC8::New(size);
			HBufC *uni = HBufC::New(size);
			if (raw && uni)
				{
				TPtr8 rp(raw->Des());
				if (f.Read(rp) == KErrNone && rp.Length() == size)
					{
					const TInt skip = (size >= 3 && rp[0] == 0xEF && rp[1] == 0xBB &&
									   rp[2] == 0xBF) ? 3 : 0;
					TPtr up(uni->Des());
					if (CnvUtfConverter::ConvertToUnicodeFromUtf8(up, rp.Mid(skip)) >= 0)
						{
						TInt w = 0;
						for (TInt r = 0; r < up.Length(); r++)
							if (up[r] != '\r')
								up[w++] = up[r];
						up.SetLength(w);
						out = uni;
						uni = NULL;
						}
					}
				}
			delete raw;
			delete uni;
			}
		f.Close();
		}
	fs.Close();
	return out;
	}

// --- Container: window for the frame + menu + keys ---------------------------

enum TMenuPage
	{
	EPageNone = 0, EPageMain, EPageFiles, EPageKeys,
	EPageImport,      // browser with checkmarks for batch import
	EPageBios,        // BIOS dump selection (including from archives)
	EPageAsk,         // yes/no question before a long operation
	EPageProgress,
	EPageError,
	EPageHelp,        // topic list and topic text, from the help files on the card
	EPageSetup        // first-run checklist
	};

// What to show in the browser. The browser is one, but the consumers differ:
// games come from disc images, import also from archives, BIOS from dumps.
enum TScanMode { EScanGames = 0, EScanImport, EScanBios };

// Question awaiting an answer on EPageAsk.
enum TAskWhat { EAskNone = 0, EAskMode, EAskDelete, EAskRestart,
                EAskNoGame, EAskNoBios };    // the last two guard Start

enum TRowStyle { ERowNormal = 0, ERowHeader, ERowWarn };

enum TSetupRow { ESetLang = 0, ESetKeys, ESetBios, ESetGames, ESetPreset, ESetStart,
                 ESetHelp, ESetCount };

static const TStrId KSetupLabel[ESetCount] =
	{ EStrItemLang, EStrItemKeyBase, EStrItemBios, EStrItemGames, EStrItemPreset,
	  EStrItemStart, EStrItemHelp };

const TInt KHintHoldMs = 2000;   // wait at both ends of a long hint
const TInt KHintPxPerSec = 40;   // smooth, evenly timed scroll
const TInt KRowGap = 2;          // row height = font height + this

_LIT(KTopicSetup, "topic.setup");

class CPs1RemCon;

// Client side of the ps1slide anim; opcodes are TSlideCommand in slidekeys/slideanim.h.
class RSlideAnim : public RAnim
	{
public:
	RSlideAnim(RAnimDll &aDll) : RAnim(aDll) {}
	TInt Construct(const RWindowBase &aDevice)
		{ return RAnim::Construct(aDevice, 0, KNullDesC8); }
	TInt Attach() { return CommandReply(1); }
	void Detach() { CommandReply(2); }
	TInt SetMap(const TInt *aMap)
		{
		const TPtrC8 args((const TUint8 *)aMap, (1 + 2 * aMap[0]) * sizeof(TInt));
		return CommandReply(3, args);
		}
	TInt GetLast() { return CommandReply(4); }
	};

class CPs1Container : public CCoeControl
	{
public:
	static CPs1Container *NewL(const TRect &aRect);
	~CPs1Container();
	TKeyResponse OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType);
	TKeyResponse DispatchKeyL(const TKeyEvent &aKeyEvent, TEventCode aType);
	void RemConKey(TInt aScan, TBool aDown);
	void HandlePointerEventL(const TPointerEvent &aPointerEvent);
	// CCoeControl::Window() is protected, and the output needs exactly this window.
	RWindow &FrameWindow() { return Window(); }
	TBool MenuActive() const { return iPage != EPageNone; }
	TBool OnProgress() const { return iPage == EPageProgress; }
	// Sleep of the menu loop: short only while a progress page or a moving hint needs it.
	TInt TickUs() const
		{ return iPage == EPageProgress ? 20000 : (iPage == EPageMain && iHintPhase == 1) ? 30000 : 50000; }
	// Leaving the foreground (call, task switch) pauses through the menu.
	void FocusLost();
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
	TRect DrawChrome(CWindowGc &gc, const TDesC &aTitle, const TDesC &aHint,
	                 TInt aHintPx = -1) const;
	void DrawMain(CWindowGc &gc, const TRect &aBody) const;
	void DrawKeys(CWindowGc &gc, const TRect &aBody) const;
	void DrawKeyMenu(CWindowGc &gc, const TRect &aBody, TInt aFirst) const;
	void DrawFiles(CWindowGc &gc, const TRect &aBody) const;
	void DrawAsk(CWindowGc &gc, const TRect &aBody) const;
	void DrawError(CWindowGc &gc, const TRect &aBody) const;
	TBool RunningGamePicked() const;
	void DrawProgress(CWindowGc &gc, const TRect &aBody) const;
	void DrawHelp(CWindowGc &gc, const TRect &aBody) const;
	void DrawSetup(CWindowGc &gc, const TRect &aBody) const;
	void DrawRow(CWindowGc &gc, const TRect &aBody, TInt aY, TInt aLh,
	             const TDesC &aLeft, const TDesC &aRight, TBool aSel,
	             TInt aIndent = 0, TInt aStyle = ERowNormal) const;
	TRect BodyRect() const;
	TRect BodyRectFor(const TRect &r) const;
	TInt BodyRows() const;
	TPtrC HintText() const;
	void HintTick();
	void ResetHint();
	void LogGeometry() const;
	void StepValue(TInt aItem, TInt aDelta);
	void ItemLabel(TInt aItem, TDes &aOut) const;
	void ItemValue(TInt aItem, TDes &aOut) const;
	TBool ItemVisible(TInt aItem) const;

	// pages
	TKeyResponse MainKeyL(TInt aScan);
	TKeyResponse FilesKeyL(TInt aScan);
	TKeyResponse KeysKeyL(TInt aScan);
	void StoreKey(TInt aRow, TBool aAlt, TInt aKey);
	void TakeKey(TInt aRow, TInt aKey);
	TInt KeyRowUsing(TInt aKey, TInt aExcept) const;
	TKeyResponse ImportKeyL(TInt aScan);
	TKeyResponse BiosKeyL(TInt aScan);
	TKeyResponse AskKeyL(TInt aScan);
	TKeyResponse ProgressKeyL(TInt aScan);
	TKeyResponse ErrorKeyL(TInt aScan);
	TKeyResponse HelpKeyL(TInt aScan);
	TKeyResponse SetupKeyL(TInt aScan);
	void OpenMenu();
	void CloseMenu();
	void DoCloseMenu(TBool aWithRestart);
	void RevertRestartSettings();
	void FixSel();
	TInt VisibleSel() const;
	void DoStart();
	TBool BiosThere() const;
	void LeaveBrowse();
	void OpenGameBrowse();
	void OpenImport();
	void OpenBiosBrowse();

	// help files and the pages built on them
	void LoadHelp();
	TBool HelpFind(const TDesC &aKey, TPtrC &aInline, TPtrC &aBody) const;
	TBool TopicAt(TInt aIdx, TPtrC &aKey) const;
	TInt TopicCount() const;
	TInt LayoutHelp(CWindowGc *aGc, const TRect &aBody, TInt aFrom) const;
	void OpenHelp(TInt aFrom);
	void OpenTopic(TInt aIdx);
	void OpenSetup(TInt aBack);
	void RefreshSetup();

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
	void BeginImport();

	// persistence
	void ProfilePath(TDes &aOut) const;
	void LoadCfg(const TDesC &aPath, TBool aWithGame);
	void ParseCfg(const TDesC8 &aTxt, TBool aWithGame);
	void SaveCfg(const TDesC &aPath, TBool aWithGame) const;
	void SaveAll() const;
	void SetGameL(const TDesC &aPath);
	void ApplyKeyMap();
	void SyncSideKeyCaptures(TBool aAll = EFalse);
	void SyncSlideKeys(TBool aAll);
	void SlideKeysStart(TBool aReport);
	void SlideKeysStop();
	void SlideKeysPoll();
	void ResetControls();
	void ApplyPreset(TInt aPreset);
	void ApplyBase(TInt aBase);
	void FreeTiltKey();
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
	TInt iOpenSec;            // section open on the main page (ESecCount = all closed)
	TInt iHintPx, iHintPhase; // pixel scroll of a hint wider than the status line, and its phase
	TUint32 iHintT0;          // NTickCount (ms) at which the phase began
	TInt iBackPage;           // where the file browsers return: main or the setup page
	HBufC *iHelp;             // help file of the interface language (UTF-16), NULL = none
	HBufC *iHelpEn;           // English fallback; NULL = none, or the language is English
	TInt iHelpFrom;           // page the help returns to
	TInt iHelpSel, iHelpTopic, iHelpScroll, iHelpLines;   // iHelpTopic -1 = topic list
	TInt iSetupBack, iSetupSel, iSetupGames;
	TBool iSetupBios;
	TInt iBrowseSel;
	TInt iKeySel;
	TInt iPresetSel;          // preset highlighted on the CONTROLS list (not persisted)
	TInt iBaseSel;            // keyboard base highlighted on the CONTROLS list (not persisted)
	TInt iCapture;            // button awaiting a key press (-1 = none)
	TBool iCaptureAlt;        // the capture sets the second key, not the main one
	TInt iKeyMenu;            // line of the action menu open on iKeySel (-1 = closed)
	TInt iClashKey;           // key already used by another row, waiting for OK (-1 = none)
	TInt iClashRow;           // the row that holds iClashKey
	TInt iConfirm;            // item awaiting confirmation (-1 = none)
	TInt iStateSlot;          // state slot (1..5)
	TBuf<80> iStatusMsg;      // message shown in hint bar (e.g. state save/load result)
	TInt iPadCount;
	// Rows of the key page: the PS1 buttons, then the tilt zero key.
	TInt iKeyRows;
	TBool iStartedOnce;       // config already sent to the core; only live changes from now on
	TBool iGameClosed;        // game was shut down for importing its own disc image
	TBool iRestartReq;        // exited menu with a change that requires a new pass
	// Values of asterisked items at the moment the pass was started: the difference
	// from the current values is the reason to restart emulation.
	TFileName iGameAtStart, iBiosAtStart;
	TInt iInterpAtStart, iGteNfAtStart, iSoundAtStart, iGlAtStart, iDsaAtStart, iWsolaAtStart;
	TBool iCliMode;           // launched with arguments: do not touch the in-game menu
	// The running game's settings as they were before another game's profile was
	// loaded over them, so "Discard changes" can put them back.
	TPs1Cfg iCfgPrev;
	TBool iCfgPrevValid;
	// The disc of the running game is being imported: the pass is torn down by the
	// tick (MenuTick), not by the key handler that asked for it.
	TBool iCloseReq;
	// keyprobe only: how many arriving keys have already been logged, so a held
	// side key cannot flood the log.
	TInt iKeyProbeSeen;
	TPs1Cfg iCfg;
	TInt32 iSideKeyCap[8];
	CPs1RemCon *iRemCon;      // RemCon target for the media keys; NULL when it did not open
	// ps1slide anim, held only while a binding uses a media key: NULL both when not.
	RAnimDll *iSlideDll;
	RSlideAnim *iSlideAnim;
	TInt iSlideLast;          // last GetLast value logged, so each new key is logged once
	TInt iSlideLogged;        // GetLast lines written by this run
	};

const TInt KRemConLogLines = 64;      // log lines per run, so a held key cannot flood the log
const TInt KRemConClickUs = 100000;   // a click holds its key this long

// RemCon target for the media keys. The firmware hands them to the music player's service,
// so the emulator opens a target to receive them. The one active object is both the
// observer and the owner of the single response in flight.
class CPs1RemCon : public CActive, public MRemConCoreApiTargetObserver
	{
public:
	static CPs1RemCon *NewL(CPs1Container &aOwner);
	~CPs1RemCon();
	void MrccatoCommand(TRemConCoreApiOperationId aOperationId,
		TRemConCoreApiButtonAction aButtonAct);
private:
	CPs1RemCon(CPs1Container &aOwner);
	void OpenTargetL();
	void Click(TInt aScan);
	static TInt ClickEnd(TAny *aSelf);
	void Respond(TRemConCoreApiOperationId aOperationId);
	void RunL();
	void DoCancel();
private:
	CPs1Container &iOwner;
	CRemConInterfaceSelector *iSelector;   // owns iTarget
	CRemConCoreApiTarget *iTarget;
	CPeriodic *iClick;
	TInt iClickScan;                       // key of a click whose up is still due, 0 when none
	TInt iLogged;                          // log lines written by this run
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
	// Multi-touch is requested before ActivateL below; the window has its size since SetRect.
	if (voutSymbianTouchProbe(Window()))
		EnableDragEvents();
	iBrowse = new (ELeave) CDesCArrayFlat(16);
	iPicked = new (ELeave) CDesCArrayFlat(8);
	iPadCount = voutSymbianPadButtons();
	if (iPadCount > KPadMax - 1) iPadCount = KPadMax - 1;
	iKeyRows = iPadCount + 1;
	iCapture = -1;
	iCaptureAlt = EFalse;
	iKeyMenu = -1;
	iClashKey = -1;
	iClashRow = -1;
	iRemCon = NULL;
	iSlideDll = NULL;
	iSlideAnim = NULL;
	iSlideLast = 0;
	iSlideLogged = 0;
	iConfirm = -1;
	iPresetSel = EPresetDefault;
	iBaseSel = EBaseN95;
	iStateSlot = 1;
	iOpenSec = ESecCount;
	ResetHint();
	iBackPage = EPageMain;
	iHelpTopic = -1;
	iSetupBack = EPageMain;
	iStatusMsg.Zero();
	for (TInt i = 0; i < 8; i++)
		iSideKeyCap[i] = 0;

	MigrateOldLayout();
	iCfg.SetDefaults();
	LoadCfg(KCfgPath, ETrue);
	// Game profile overrides global settings - but only if it exists.
	if (iCfg.iProfile && iGamePath.Length())
		{
		TFileName p;
		ProfilePath(p);
		LoadCfg(p, EFalse);
		}
	if (!iGamePath.Length())
		AutoPickGameL();
	LoadHelp();
	if (ps1TiltCli())
		iCfg.iTilt = 1;
	ApplyKeyMap();
	voutSymbianSetTilt(iCfg.iTilt);
	voutSymbianSetTiltMap(iCfg.iTiltSide, iCfg.iTiltPitch, iCfg.iTiltDead, iCfg.iTiltSens);
	if (iCfg.iTilt && !voutSymbianTiltActive()) iCfg.iTilt = 0;
	ps1LogInt("tilt", "active", voutSymbianTiltActive());

	iBrowsePath = KImgDir;

	// Menu only when launched from the device. With arguments (deploy, benchmark)
	// the command line sets the parameters, and stage1 parses it itself - the menu
	// here would wait for a key press absent from the recorded input trace, and
	// the run would stall until killed by the daemon.
	TBuf<256> cl;
	User::CommandLine(cl);
	cl.Trim();
	iCliMode = (cl.Length() != 0);
	// Command-line runs replay recorded input and are measured frame by frame; the panel
	// would add drawing to every frame of them.
	if (iCliMode && cl.FindF(_L("touchon")) == KErrNotFound)
		voutSymbianTouchOff();
	// Layout options of the on-screen gamepad, read next to touchon: "sticks" selects the
	// two-stick layout, "padalpha<N>" the opacity of the untouched panel in percent (default 100).
	TInt padAlpha = 100;
	const TInt padAt = cl.FindF(_L("padalpha"));
	if (padAt != KErrNotFound)
		{
		const TPtrC digits = cl.Mid(padAt + 8);
		TLex lex(digits);
		TInt value = 0;
		if (lex.Val(value) == KErrNone)
			padAlpha = value;
		else
			ps1LogFmt("touch", "padalpha is not a number, the default 100 is kept");
		}
	voutSymbianTouchOptions(cl.FindF(_L("sticks")) != KErrNotFound, padAlpha);
	// A command line without img would otherwise fall back to the built-in
	// gt.bin, so a game named in ps1_settings.txt - the only way to name one
	// whose file name holds a space - never reached the core.
	if (iCliMode && iGamePath.Length() && cl.FindF(_L("img")) == KErrNotFound)
		{
		TBuf8<256> g8;
		g8.Copy(iGamePath.Left(255));
		g8.ZeroTerminate();
		ps1SetDiscFromCfg((const char *)g8.Ptr());
		}
	if (iCliMode)
		iPage = EPageNone;
	else if (iCfg.iIntro)
		iPage = EPageMain;
	else
		OpenSetup(EPageMain);
	// One line per start, so the row budget of the main page can be read off a device:
	// the machine UID is for telling the handsets apart (keyboard autodetect needs it).
	TInt mach = 0;
	HAL::Get(HAL::EMachineUid, mach);
	ps1LogFmt("menu", "machine=0x%08x", (int)mach);
	LogGeometry();
	// Optional: the emulator runs without the media keys when RemCon refuses the target.
	TRAPD(re, iRemCon = CPs1RemCon::NewL(*this));
	ps1LogFmt("remcon", "open err=%d", (int)re);
	ActivateL();
	}

CPs1Container::~CPs1Container()
	{
	SlideKeysStop();
	delete iRemCon;
	for (TInt i = 0; i < 8; i++)
		if (iSideKeyCap[i])
			{
			iCoeEnv->RootWin().CancelCaptureKeyUpAndDowns(iSideKeyCap[i]);
			iSideKeyCap[i] = 0;
			}
	ConvCancel();
	ConvRelease();
	delete iBrowse;
	delete iPicked;
	delete iHelp;
	delete iHelpEn;
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
	// Only the open section shows its rows.
	const TInt sec = KItemSection[aItem];
	if (sec < ESecCount && !isHeader(aItem) && sec != iOpenSec)
		return EFalse;
	// Slots are named after iGamePath, which already points at the next game while the
	// core still runs the old one.
	if (aItem == ECfgSaveState || aItem == ECfgLoadState)
		return iStartedOnce && !iGameClosed && iGamePath.CompareF(iGameAtStart) == 0;
	// The switch only has an effect alongside GL; without it the software path always runs.
	if (aItem == ECfgSwDraw)
		return iCfg.iGl;
	// Tuning rows for a sensor that is switched off would be four dead lines.
	if (aItem == ECfgTiltSide || aItem == ECfgTiltPitch ||
		aItem == ECfgTiltDead || aItem == ECfgTiltSens)
		return iCfg.iTilt;
	return ETrue;
	}

static TBool fileThere(const TDesC &aPath)
	{
	RFs fs;
	TBool have = EFalse;
	if (fs.Connect() == KErrNone)
		{
		TEntry e;
		have = (fs.Entry(aPath, e) == KErrNone);
		fs.Close();
		}
	return have;
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
	// RFs::Rename does not create the destination directory.
	fs.MkDirAll(KProfDir);
	fs.MkDirAll(KBiosDir);
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
	// Cursor on the first item: the disc has been selected, and the next thing
	// the user wants is to launch it. Leaving the cursor on "Game" would mean
	// that OK opens the file browser again, with no obvious way to apply the choice.
	if (iBackPage == EPageMain)
		iSel = ECfgStart;
	LeaveBrowse();
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
	if (iStartedOnce && iCfg.iProfile && !iCfgPrevValid)
		{
		iCfgPrev = iCfg;
		iCfgPrevValid = ETrue;
		}
	iGamePath = aPath;
	if (iCfg.iProfile)
		{
		TFileName p;
		ProfilePath(p);
		LoadCfg(p, EFalse);
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
	if (!iStartedOnce || iGameClosed || iGamePath.CompareF(iGameAtStart) != 0)
		return;
	RFs fs;
	if (fs.Connect() == KErrNone)
		{
		fs.MkDirAll(KSavesDir);
		fs.Close();
		}
	// Written beside the slot and renamed over it only when complete, so a full
	// card or a failed write keeps the previous state instead of truncating it.
	TFileName path;
	StatePath(path, iStateSlot);
	TFileName tmp(path);
	tmp.Append(_L(".new"));
	TBuf8<KMaxFileName + 1> path8;
	path8.Copy(tmp);
	ps1StateIoFailed();
	int r = SaveState((const char *)path8.PtrZ());
	if (r == 0 && ps1StateIoFailed())
		r = KErrWrite;
	if (fs.Connect() == KErrNone)
		{
		if (r == 0)
			r = fs.Replace(tmp, path);
		if (r != 0)
			fs.Delete(tmp);
		fs.Close();
		}
	else if (r == 0)
		r = KErrGeneral;
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
	if (!iStartedOnce || iGameClosed || iGamePath.CompareF(iGameAtStart) != 0)
		return;
	TFileName path;
	StatePath(path, iStateSlot);
	TBuf8<KMaxFileName + 1> path8;
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

void CPs1Container::LoadCfg(const TDesC &aPath, TBool aWithGame)
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
			// A profile without bios= means the default dump; without this it would
			// inherit the previous game's.
			if (!aWithGame)
				iCfg.iBios.Zero();
			if (f.Read(p) == KErrNone)
				ParseCfg(p, aWithGame);
			delete buf;
			}
		f.Close();
		}
	fs.Close();
	}

void CPs1Container::ParseCfg(const TDesC8 &aTxt, TBool aWithGame)
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
		// Older builds skip a key they do not know, so the file stays readable by them.
		if (key == _L8("intro"))
			{
			TLex8 li(val);
			TInt v = 0;
			if (aWithGame && li.Val(v) == KErrNone)
				iCfg.iIntro = v ? 1 : 0;
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
					// Language is global: a profile written under another language
					// must not switch it back.
					if (i == ECfgLang && (!aWithGame || v < 0 || v >= I18nLanguageCount()))
						break;
					if (i == ECfgDebug && !aWithGame)
						break;
					iCfg.Value(i) = v;
					if (i == ECfgLang)
						I18nSetLanguage(v);
					}
				break;
				}
		}
	iCfg.Clamp();
	}

void CPs1Container::SaveCfg(const TDesC &aPath, TBool aWithGame) const
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	// The profiles directory may not exist yet; we ignore the "already exists" error.
	fs.MkDirAll(aPath);
	// Written aside and renamed over the target: a card that fills up mid-write
	// would otherwise leave the settings file truncated.
	TFileName tmp(aPath);
	if (tmp.Length() + 4 > tmp.MaxLength())
		{
		fs.Close();
		return;
		}
	tmp.Append(_L(".new"));
	RFile f;
	if (f.Replace(fs, tmp, EFileWrite | EFileShareAny) == KErrNone)
		{
		TInt werr = KErrNone;
		TPs1Cfg &c = const_cast<TPs1Cfg &>(iCfg);
		TBuf8<KMaxFileName + 16> line;
		if (aWithGame && iGamePath.Length())
			{
			TBuf8<KMaxFileName> g;
			g.Copy(iGamePath);
			line.Format(_L8("game=%S\n"), &g);
			if (werr == KErrNone) werr = f.Write(line);
			}
		if (aWithGame && iCfg.iIntro)
			{
			if (werr == KErrNone) werr = f.Write(_L8("intro=1\n"));
			}
		// BIOS is written to both files: in the profile it is per-game, in the
		// global file it is the default for games that have no profile yet.
		// Always written, even when empty: a profile that omits it would take over
		// the previous game's dump on load.
		{
		TBuf8<KMaxFileName> b;
		b.Copy(iCfg.iBios);
		line.Format(_L8("bios=%S\n"), &b);
		if (werr == KErrNone) werr = f.Write(line);
		}
		for (TInt i = 0; i < ECfgCount; i++)
			if (KCfgKey[i] && (aWithGame || (i != ECfgLang && i != ECfgDebug)))
				{
				line.Format(_L8("%s=%d\n"), KCfgKey[i], c.Value(i));
				if (werr == KErrNone) werr = f.Write(line);
				}
		for (TInt b = 0; b < iKeyRows; b++)
			{
			line.Format(_L8("btn%d=%d %d %d\n"), b, iCfg.iKeyScan[b],
				iCfg.iKeyAlt[b], iCfg.iKeyHold[b]);
			if (werr == KErrNone) werr = f.Write(line);
			}
		f.Close();
		if (werr != KErrNone || fs.Replace(tmp, aPath) != KErrNone)
			fs.Delete(tmp);
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

void CPs1Container::ApplyKeyMap()
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
	SyncSideKeyCaptures();
	}

// Side keys are captured only while bound so system retains volume and camera otherwise.
void CPs1Container::SyncSideKeyCaptures(TBool aAll)
	{
	// The first six are what an N95 8GB actually sends, read from the keyprobe log
	// on 2026-09-22; the guessed Application2..5 never appeared and are gone. The
	// last two are kept unmeasured for handsets nobody has probed yet.
	static const TInt KSideKey[8] =
		{
		EStdKeyIncVolume, EStdKeyDecVolume, EStdKeyDevice5, EStdKeyDevice8,
		EStdKeyApplication19, EStdKeyApplication1D, EStdKeyDevice6, EStdKeyDevice7
		};
	TBool inUse[8];
	for (TInt k = 0; k < 8; k++)
		inUse[k] = aAll;
	for (TInt i = 0; i < iKeyRows; i++)
		{
		const TInt sc = iCfg.iKeyScan[i];
		const TInt alt = iCfg.iKeyAlt[i];
		for (TInt k = 0; k < 8; k++)
			if (sc == KSideKey[k] || alt == KSideKey[k])
				inUse[k] = ETrue;
		}
	RWindowGroup &root = iCoeEnv->RootWin();
	for (TInt i = 0; i < 8; i++)
		{
		if (inUse[i] && !iSideKeyCap[i])
			{
			const TInt32 h = root.CaptureKeyUpAndDowns(KSideKey[i], 0, 0);
			iSideKeyCap[i] = (h > 0) ? h : 0;
			// A refusal here is -46, meaning the process lacks SwEvent; the release
			// build has carried it since 2026-09-22. Without this line a refused
			// capture would look like a dead key with no visible cause.
			if (h <= 0)
				ps1LogFmt("key", "capture refused scan=0x%02x err=%d", (int)KSideKey[i], (int)h);
			}
		else if (!inUse[i] && iSideKeyCap[i])
			{
			root.CancelCaptureKeyUpAndDowns(iSideKeyCap[i]);
			iSideKeyCap[i] = 0;
			}
		}
	SyncSlideKeys(aAll);
	}

// A preset is a one-shot starting point, not a mode: it overwrites every
// button's key, alt key and hold flag (never leaving a stale hold behind,
// per the same reasoning as voutSymbianSetKeyMap's own g_pad reset), then
// applies and saves exactly as leaving the CONTROLS menu already does for a
// hand-remapped key. The user is free to re-map individual keys afterwards.
void CPs1Container::ApplyPreset(TInt aPreset)
	{
	for (TInt i = 0; i < iPadCount; i++)
		{
		if (aPreset == EPresetDefault)
			{
			iCfg.iKeyScan[i] = voutSymbianDefaultKey(i, 0);
			iCfg.iKeyAlt[i]  = voutSymbianDefaultKey(i, 1);
			iCfg.iKeyHold[i] = 0;
			}
		else if (i < KPresetBtnCount)
			{
			const TInt p = aPreset - 1;    // KPreset* tables start at Racing
			iCfg.iKeyScan[i] = KPresetScan[p][i];
			iCfg.iKeyAlt[i]  = KPresetAlt[p][i];
			iCfg.iKeyHold[i] = KPresetHold[p][i];
			}
		else
			{
			// A future button beyond this preset's table: leave it unmapped
			// rather than read past the table's end.
			iCfg.iKeyScan[i] = -1;
			iCfg.iKeyAlt[i]  = -1;
			iCfg.iKeyHold[i] = 0;
			}
		}
	FreeTiltKey();
	ApplyKeyMap();
	SaveAll();
	}

// Unlike the full reset, this leaves the language, video, speed and sound alone. The tilt
// settings take effect when the menu closes, as after any change to them.
void CPs1Container::ResetControls()
	{
	iCfg.SetControlDefaults();
	iPresetSel = EPresetDefault;
	iBaseSel = EBaseN95;
	ApplyKeyMap();
	SaveAll();
	ps1Log("menu", "controls reset");
	}

// A preset or a base can take the tilt zero key for a PS1 button (RPG and C5 both use
// C): move it to the first key the new layout leaves free, so one press does not do two
// things the player did not ask for together.
void CPs1Container::FreeTiltKey()
	{
	static const TInt KTiltCandidates[5] = { EStdKeyBackspace, '0', '5', '*', '#' };
	const TInt row = iPadCount;
	for (TInt c = -1; c < 5; c++)
		{
		const TInt k = (c < 0) ? iCfg.iKeyScan[row] : KTiltCandidates[c];
		if (k < 0)
			continue;
		TBool used = EFalse;
		for (TInt i = 0; i < iPadCount && !used; i++)
			used = (iCfg.iKeyScan[i] == k || iCfg.iKeyAlt[i] == k);
		if (!used)
			{
			iCfg.iKeyScan[row] = k;
			iCfg.iKeyAlt[row] = -1;
			return;
			}
		}
	iCfg.iKeyScan[row] = -1;
	iCfg.iKeyAlt[row] = -1;
	}

// A base picks the handset's keyboard, not the game type: same one-shot overwrite
// as ApplyPreset, and left for the user to touch up afterwards the same way.
void CPs1Container::ApplyBase(TInt aBase)
	{
	for (TInt i = 0; i < iPadCount; i++)
		{
		if (aBase == EBaseN95 || i >= KPresetBtnCount)
			{
			iCfg.iKeyScan[i] = voutSymbianDefaultKey(i, 0);
			iCfg.iKeyAlt[i]  = voutSymbianDefaultKey(i, 1);
			}
		else if (aBase == EBaseE90)
			{
			iCfg.iKeyScan[i] = KBaseE90Scan[i];
			iCfg.iKeyAlt[i]  = KBaseE90Alt[i];
			}
		else if (aBase == EBaseSlider)
			{
			iCfg.iKeyScan[i] = KBaseSliderScan[i];
			iCfg.iKeyAlt[i]  = KBaseSliderAlt[i];
			}
		else
			{
			iCfg.iKeyScan[i] = KBaseC5Scan[i];
			iCfg.iKeyAlt[i]  = KBaseC5Alt[i];
			}
		iCfg.iKeyHold[i] = 0;
		}
	FreeTiltKey();
	ApplyKeyMap();
	SaveAll();
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
	if (isHeader(aItem))
		{
		aOut.Copy(iOpenSec == KItemSection[aItem] ? _L("- ") : _L("+ "));
		aOut.Append(Tr(KItemLabelStrId[aItem]));
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
		case ECfgHdrVideo:
		case ECfgHdrSpeed:
		case ECfgHdrInput:
		case ECfgHdrSystem:
		case ECfgHelp:
			break;
		case ECfgHdrSound:
			aOut.Copy(Tr(c.iSound ? EStrValOn : EStrValOff));
			break;
		case ECfgHdrGame:
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
			{
			aOut.Format(Tr(EStrValSlotFmt), iStateSlot);
			// A symbol rather than a word: the marker needs no translation.
			TFileName p;
			StatePath(p, iStateSlot);
			TEntry e;
			const TBool have = (iCoeEnv->FsSession().Entry(p, e) == KErrNone);
			aOut.Append(have ? _L(" [*]") : _L(" [ ]"));
			}
			break;
		case ECfgAdd:
			aOut.Copy(Tr(EStrValPressOk));
			break;
		case ECfgBios:
			aOut.Copy((c.iBios.Length() ? TPtrC(c.iBios) : Tr(EStrValDefault)).Left(aOut.MaxLength()));
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
			{
			// Shown as speed rather than as the cycle multiplier it is, so that a
			// larger number means a faster PS1 processor.
			const TInt pct = (KCycleDefault * 100 + c.iCycle / 2) / c.iCycle;
			if (c.iCycle == KCycleDefault)
				aOut.Format(Tr(EStrValCycleDefFmt), pct);
			else
				aOut.Format(_L("%d%%"), pct);
			}
			break;
		case ECfgTiltSide:
		case ECfgTiltPitch:
			{
			// Stick and axis in three characters, with a leading minus for the reversed
			// direction: no wording to translate and none to misread.
			const TInt m = (aItem == ECfgTiltSide) ? c.iTiltSide : c.iTiltPitch;
			if (m <= 0)
				aOut.Copy(Tr(EStrValOff));
			else
				{
				const TInt t = (m - 1) / 2;
				aOut.Zero();
				if ((m - 1) & 1) aOut.Append(TChar(0x2D));
				aOut.Append(TChar((t < 2) ? 0x4C : 0x52));
				aOut.Append(TChar((t & 1) ? 0x59 : 0x58));
				}
			}
			break;
		case ECfgTiltDead:
			aOut.Format(_L("%d"), c.iTiltDead);
			break;
		case ECfgTiltSens:
			aOut.Format(_L("%d%%"), c.iTiltSens);
			break;

		case ECfgKeys:
			aOut.Copy(Tr(EStrValPressOk));
			break;
		case ECfgKeyBase:
			aOut.Copy(Tr(KBaseNameStrId[iBaseSel]));
			break;
		case ECfgPreset:
			aOut.Copy(Tr(KPresetNameStrId[iPresetSel]));
			break;
		case ECfgLang:
			aOut.Copy(I18nLanguageName(c.iLang));
			break;
		case ECfgResetKeys:
			aOut.Copy(Tr(iConfirm == ECfgResetKeys ? EStrValConfirm : EStrValPressOk));
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

// One line per menu open so the row budget can be read off a device, in the current
// orientation and in the other one.
void CPs1Container::LogGeometry() const
	{
	const TRect r = Rect();
	const TRect rot(r.iTl, TPoint(r.iTl.iX + r.Height(), r.iTl.iY + r.Width()));
	const TInt lh = iCoeEnv->NormalFont()->HeightInPixels() + KRowGap;
	ps1LogFmt("menu", "geometry screen=%dx%d lh=%d rows=%d rows_rotated=%d",
		(int)r.Width(), (int)r.Height(), (int)lh, (int)BodyRows(),
		(int)(BodyRectFor(rot).Height() / lh));
	}

// The list area between the title bar and the hint line. The rows are packed against
// the hint line, so what is left over sits under the title bar and not as a gap above
// the hint.
TRect CPs1Container::BodyRectFor(const TRect &r) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + KRowGap;
	const TInt top = r.iTl.iY + lh + 5;
	const TInt bottom = r.iBr.iY - 4 - font->AscentInPixels() - 1;
	TInt rows = (bottom - top) / lh;
	if (rows < 1) rows = 1;
	return TRect(r.iTl.iX, bottom - rows * lh, r.iBr.iX, bottom);
	}

TRect CPs1Container::BodyRect() const
	{
	return BodyRectFor(Rect());
	}

TInt CPs1Container::BodyRows() const
	{
	const TInt lh = iCoeEnv->NormalFont()->HeightInPixels() + KRowGap;
	const TInt rows = BodyRect().Height() / lh;
	return (rows < 1) ? 1 : rows;
	}

// Header, list body, and hint line. Returns the rectangle for the list.
TRect CPs1Container::DrawChrome(CWindowGc &gc, const TDesC &aTitle,
								const TDesC &aHint, TInt aHintPx) const
	{
	const TRect r = Rect();
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + KRowGap;
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
		// Cut at the screen edge on a character boundary rather than mid-glyph.
		TPtrC hint(aHint);
		const TInt avail = r.Width() - 12;
		const TInt w = font->TextWidthInPixels(hint);
		if (w > avail && aHintPx >= 0)
			{
			// Scrolling hint: the whole text, shifted left and clipped to the line.
			const TInt px = (aHintPx < w - avail) ? aHintPx : w - avail;
			gc.SetClippingRect(TRect(r.iTl.iX + 6, r.iBr.iY - 5 - asc, r.iBr.iX - 6, r.iBr.iY));
			gc.DrawText(hint, TPoint(r.iTl.iX + 6 - px, r.iBr.iY - 4));
			gc.CancelClippingRect();
			}
		else
			{
			if (w > avail)
				hint.Set(hint.Left(font->TextCount(hint, avail)));
			gc.DrawText(hint, TPoint(r.iTl.iX + 6, r.iBr.iY - 4));
			}
		}

	return BodyRect();
	}

void CPs1Container::DrawRow(CWindowGc &gc, const TRect &aBody, TInt aY, TInt aLh,
							const TDesC &aLeft, const TDesC &aRight, TBool aSel,
							TInt aIndent, TInt aStyle) const
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
	// A long value (a game file name) must not push the label out altogether.
	TPtrC right(aRight);
	const TInt maxRight = (aBody.Width() - 20) * 3 / 5;
	if (font->TextWidthInPixels(right) > maxRight)
		right.Set(right.Left(font->TextCount(right, maxRight)));
	const TInt rw = right.Length() ? font->TextWidthInPixels(right) : 0;
	// Translated labels are longer than the English ones and would print under the value.
	TInt avail = aBody.Width() - 20 - aIndent - (rw ? rw + 8 : 0);
	if (avail < 0) avail = 0;
	TPtrC left(aLeft);
	if (font->TextWidthInPixels(left) > avail)
		left.Set(left.Left(font->TextCount(left, avail)));
	gc.SetPenColor(aSel ? KRgbWhite
		: aStyle == ERowHeader ? TRgb(0x58, 0x9c, 0xe0) : TRgb(0xc8, 0xc8, 0xd0));
	gc.DrawText(left, TPoint(aBody.iTl.iX + 10 + aIndent, aY));
	if (rw)
		{
		gc.SetPenColor(aStyle == ERowWarn ? TRgb(0xff, 0x90, 0x90)
			: aSel ? TRgb(0xff, 0xdc, 0x88) : TRgb(0x8c, 0xb0, 0xd8));
		gc.DrawText(right, TPoint(aBody.iBr.iX - 10 - rw, aY));
		}
	}

void CPs1Container::DrawMain(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + KRowGap;
	TInt rows = aBody.Height() / lh;
	if (rows < 1) rows = 1;

	// Scrolling counts only the rows that are visible now.
	const TInt sel = VisibleSel();
	TInt total = 0;
	TInt selRow = 0;
	for (TInt i = 0; i < ECfgCount; i++)
		{
		if (!ItemVisible(i))
			continue;
		if (i == sel)
			selRow = total;
		total++;
		}

	TInt first = selRow - rows / 2;
	if (first > total - rows) first = total - rows;
	if (first < 0) first = 0;

	TInt y = aBody.iTl.iY + font->AscentInPixels();
	TInt row = 0;
	for (TInt i = 0; i < ECfgCount; i++)
		{
		if (!ItemVisible(i))
			continue;
		if (row >= first && row < first + rows)
			{
			TBuf<64> label;
			TBuf<64> value;
			ItemLabel(i, label);
			ItemValue(i, value);
			// Rows hang under their header.
			const TInt indent = (KItemSection[i] == ESecCount || isHeader(i)) ? 0 : 10;
			DrawRow(gc, aBody, y, lh, label, value, i == sel, indent,
				isHeader(i) ? ERowHeader : ERowNormal);
			y += lh;
			}
		row++;
		}
	}

void CPs1Container::DrawKeys(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + KRowGap;
	TInt rows = aBody.Height() / lh;
	if (rows < 1) rows = 1;
	TInt first = iKeySel - rows / 2;
	if (first > iKeyRows - rows) first = iKeyRows - rows;
	if (first < 0) first = 0;

	TInt y = aBody.iTl.iY + font->AscentInPixels();
	for (TInt i = first; i < first + rows && i < iKeyRows; i++)
		{
		TBuf<40> name;
		name.Copy(Tr(i < iPadCount ? KBtnStrId[i] : EStrBtnTiltZero));
		TBuf<64> value;
		if (iCapture == i && iClashKey < 0)
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
	if (iKeyMenu >= 0)
		DrawKeyMenu(gc, aBody, first);
	}

// Three lines in a box under the row they act on, or above it when the list runs out below.
// The box is drawn last, so it covers the rows it lands on.
void CPs1Container::DrawKeyMenu(CWindowGc &gc, const TRect &aBody, TInt aFirst) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + KRowGap;
	const TInt asc = font->AscentInPixels();
	const TInt cell = aBody.iTl.iY + (iKeySel - aFirst) * lh - 1;    // top of the row's highlight
	TInt top = cell + lh;
	if (top + 3 * lh + 2 > aBody.iBr.iY)
		top = cell - 3 * lh - 2;
	const TRect box(aBody.iTl.iX + 6, top, aBody.iBr.iX - 6, top + 3 * lh + 2);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(TRgb(0x08, 0x0c, 0x14));
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(TRgb(0x58, 0x9c, 0xe0));
	gc.DrawRect(box);
	for (TInt i = 0; i < EKeyActCount; i++)
		DrawRow(gc, box, top + 2 + asc + i * lh, lh, Tr(KKeyActStrId[i]), KNullDesC, i == iKeyMenu);
	}

void CPs1Container::DrawFiles(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + KRowGap;
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
	const TInt lh = font->HeightInPixels() + KRowGap;
	TInt y = aBody.iTl.iY + font->AscentInPixels() + lh;

	const TDesC &o0 = Tr(iAsk == EAskMode ? EStrAskCompress
		: iAsk == EAskDelete ? EStrAskDeleteYes
		: iAsk == EAskNoGame ? EStrAskPickGame
		: iAsk == EAskNoBios ? EStrAskContinue
		: EStrAskRestartYes);
	const TDesC &o1 = Tr(iAsk == EAskMode ? EStrAskStore
		: iAsk == EAskDelete ? EStrAskDeleteNo
		: iAsk == EAskNoGame ? EStrItemAdd
		: iAsk == EAskNoBios ? EStrAskPickBios
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
	else if (iAsk == EAskNoGame || iAsk == EAskNoBios)
		{
		const TDesC &msg = Tr(iAsk == EAskNoGame ? EStrAskNoGameWarn : EStrAskNoBiosWarn);
		gc.SetPenColor(TRgb(0xff, 0xdc, 0x88));
		TInt pos = 0;
		TPtrC line;
		while (wrapNext(font, msg, aBody.Width() - 20, pos, line))
			{
			gc.DrawText(line, TPoint(aBody.iTl.iX + 10, y));
			y += lh;
			}
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
	const TInt lh = font->HeightInPixels() + KRowGap;
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
	const TInt lh = font->HeightInPixels() + KRowGap;
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

void CPs1Container::DrawHelp(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + KRowGap;
	const TInt rows = BodyRows();
	TInt y = aBody.iTl.iY + font->AscentInPixels();
	if (iHelpTopic < 0)
		{
		const TInt n = TopicCount();
		if (!n)
			{
			gc.SetPenColor(TRgb(0x90, 0x90, 0x98));
			gc.DrawText(Tr(EStrEmpty), TPoint(aBody.iTl.iX + 10, y));
			return;
			}
		TInt first = iHelpSel - rows / 2;
		if (first > n - rows) first = n - rows;
		if (first < 0) first = 0;
		for (TInt i = first; i < first + rows && i < n; i++)
			{
			TPtrC key, inl, text;
			TPtrC title;
			if (TopicAt(i, key))
				title.Set(HelpFind(key, inl, text) ? inl : key);
			DrawRow(gc, aBody, y, lh, title, KNullDesC, i == iHelpSel);
			y += lh;
			}
		return;
		}
	gc.SetPenColor(TRgb(0xc8, 0xc8, 0xd0));
	const TInt total = LayoutHelp(&gc, aBody, iHelpScroll);
	if (total > rows)
		{
		const TInt th = aBody.Height();
		TInt bh = th * rows / total;
		if (bh < 6) bh = 6;
		const TInt by = aBody.iTl.iY + (th - bh) * iHelpScroll / (total - rows);
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetPenStyle(CGraphicsContext::ENullPen);
		gc.SetBrushColor(TRgb(0x20, 0x24, 0x30));
		gc.DrawRect(TRect(aBody.iBr.iX - 5, aBody.iTl.iY, aBody.iBr.iX - 2, aBody.iBr.iY));
		gc.SetBrushColor(TRgb(0x58, 0x9c, 0xe0));
		gc.DrawRect(TRect(aBody.iBr.iX - 5, by, aBody.iBr.iX - 2, by + bh));
		gc.SetPenStyle(CGraphicsContext::ESolidPen);
		}
	}

void CPs1Container::DrawSetup(CWindowGc &gc, const TRect &aBody) const
	{
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + KRowGap;
	TInt y = aBody.iTl.iY + font->AscentInPixels();
	for (TInt i = 0; i < ESetCount; i++)
		{
		TBuf<64> value;
		TInt style = ERowNormal;
		switch (i)
			{
			case ESetLang:
				value.Format(_L("< %S >"), &I18nLanguageName(iCfg.iLang));
				break;
			case ESetKeys:
				value.Format(_L("< %S >"), &Tr(KBaseNameStrId[iBaseSel]));
				break;
			case ESetBios:
				if (iSetupBios)
					{
					const TPtrC nm = iCfg.iBios.Length() ? TPtrC(iCfg.iBios) : TPtrC(KBiosDefault);
					value.Copy(Tr(EStrValFound));
					value.Append(_L(": "));
					value.Append(nm.Left(value.MaxLength() - value.Length()));
					}
				else
					{
					value.Copy(Tr(EStrValMissing));
					style = ERowWarn;
					}
				break;
			case ESetGames:
				if (iSetupGames)
					value.Format(_L("%d"), iSetupGames);
				else
					{
					value.Copy(Tr(EStrValMissing));
					style = ERowWarn;
					}
				break;
			case ESetPreset:
				value.Format(_L("< %S >"), &Tr(KPresetNameStrId[iPresetSel]));
				break;
			default:
				break;
			}
		DrawRow(gc, aBody, y, lh, Tr(KSetupLabel[i]), value, i == iSetupSel, 0, style);
		y += lh;
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
		const TPtrC where = iBrowsePath.Length() ? TPtrC(iBrowsePath) : Tr(EStrDrives);
		title.Append(where.Left(title.MaxLength() - title.Length()));
		const TRect body = DrawChrome(gc, title.Left(46),
			Tr(iPage == EPageImport ? EStrHintImport : EStrHintBrowse));
		DrawFiles(gc, body);
		}
	else if (iPage == EPageAsk)
		{
		const TRect body = DrawChrome(gc,
			Tr(iAsk == EAskMode ? EStrTitleStore
				: iAsk == EAskDelete ? EStrTitleDelete
				: iAsk == EAskNoGame ? EStrTitleNoGame
				: iAsk == EAskNoBios ? EStrTitleNoBios
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
		// While a key taken from another row waits for OK, the hint line names that row.
		TBuf<80> hint;
		if (iClashKey >= 0)
			hint.Format(Tr(EStrKeyTakenFmt), &Tr(iClashRow < iPadCount ? KBtnStrId[iClashRow] : EStrBtnTiltZero));
		else
			hint.Copy(Tr(iKeyMenu >= 0 ? EStrHintKeyMenu : iCapture >= 0 ? EStrHintCapture : EStrHintKeys));
		const TRect body = DrawChrome(gc, Tr(EStrTitleControls), hint);
		DrawKeys(gc, body);
		}
	else if (iPage == EPageHelp)
		{
		TPtrC title(Tr(EStrItemHelp));
		if (iHelpTopic >= 0)
			{
			TPtrC key, inl, text;
			if (TopicAt(iHelpTopic, key) && HelpFind(key, inl, text))
				title.Set(inl);
			}
		const TRect body = DrawChrome(gc, title,
			Tr(iHelpTopic >= 0 ? EStrHintHelpText : EStrHintHelpList));
		DrawHelp(gc, body);
		}
	else if (iPage == EPageSetup)
		{
		const TRect body = DrawChrome(gc, Tr(EStrTitleSetup), Tr(EStrHintSetup));
		DrawSetup(gc, body);
		}
	else
		{
		// The selected row's own hint replaces the general one when the help file has it.
		TPtrC hint = HintText();
		const TBool own = hint.Length() > 0;
		if (!own)
			hint.Set(Tr(iGameClosed  ? EStrHintGameClosed
				: iStartedOnce ? EStrHintMainResume
							   : EStrHintMainStart));
		const TRect body = DrawChrome(gc, _L("PS1Emulator"), hint, own ? iHintPx : -1);
		DrawMain(gc, body);
		}
	gc.DiscardFont();
	}

// --- Start checks, help and first-run page ----------------------------------

// The cursor must stand on a visible row: a section can close (or a game end) under it.
TInt CPs1Container::VisibleSel() const
	{
	if (iSel >= 0 && iSel < ECfgCount && ItemVisible(iSel))
		return iSel;
	const TInt sec = (iSel >= 0 && iSel < ECfgCount) ? KItemSection[iSel] : ESecCount;
	return (sec < ESecCount) ? KSecHdr[sec] : ECfgStart;
	}

void CPs1Container::FixSel()
	{
	iSel = VisibleSel();
	}

TBool CPs1Container::BiosThere() const
	{
	TFileName p(KBiosDir);
	if (iCfg.iBios.Length())
		{
		if (p.Length() + iCfg.iBios.Length() > p.MaxLength())
			return EFalse;
		p.Append(iCfg.iBios);
		}
	else
		p.Append(KBiosDefault);
	return fileThere(p);
	}

// What Start does once the checks have passed.
void CPs1Container::DoStart()
	{
	if (iStartedOnce)
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
	}

// The browsers return to the page that opened them: the main page, or the setup page.
void CPs1Container::LeaveBrowse()
	{
	iPage = iBackPage;
	iBackPage = EPageMain;
	if (iPage == EPageSetup)
		RefreshSetup();
	}

void CPs1Container::OpenGameBrowse()
	{
	// Start browsing from the current game's directory - this is the most common need.
	// A game whose file is gone starts from the games directory instead.
	if (iGamePath.Length() && fileThere(iGamePath))
		{
		const TInt s = iGamePath.LocateReverse('\\');
		iBrowsePath = iGamePath.Left(s + 1);
		}
	else
		iBrowsePath = KImgDir;
	iScanMode = EScanGames;
	ScanDirL();
	iPage = EPageFiles;
	}

void CPs1Container::OpenImport()
	{
	// Start from the card root: what is being imported was just copied
	// there from a PC, not placed into the emulator directory.
	iBrowsePath = _L("E:\\");
	iScanMode = EScanImport;
	iPicked->Reset();
	ScanDirL();
	iPage = EPageImport;
	}

void CPs1Container::OpenBiosBrowse()
	{
	iBrowsePath = KBiosDir;
	iScanMode = EScanBios;
	ScanDirL();
	iPage = EPageBios;
	}

// Only the language file is replaced on a language change: the English one never changes.
void CPs1Container::LoadHelp()
	{
	TInt lang = iCfg.iLang;
	if (lang < 0 || lang >= ELangIdCount)
		lang = ELangIdEnglish;
	delete iHelp;
	iHelp = loadHelpFile(lang);
	if (lang == ELangIdEnglish)
		{
		delete iHelpEn;
		iHelpEn = NULL;
		}
	else if (!iHelpEn)
		iHelpEn = loadHelpFile(ELangIdEnglish);
	}

// A key missing from the language file is taken from the English one.
TBool CPs1Container::HelpFind(const TDesC &aKey, TPtrC &aInline, TPtrC &aBody) const
	{
	for (TInt pass = 0; pass < 2; pass++)
		{
		const HBufC *b = pass ? iHelpEn : iHelp;
		if (!b)
			continue;
		TInt pos = 0;
		TPtrC key, inl, body;
		while (nextSection(*b, pos, key, inl, body))
			if (key == aKey)
				{
				aInline.Set(inl);
				aBody.Set(body);
				return ETrue;
				}
		}
	return EFalse;
	}

// The topic list comes from the English file, so a partly translated file still shows
// every topic; the titles and texts are looked up per key.
TBool CPs1Container::TopicAt(TInt aIdx, TPtrC &aKey) const
	{
	const HBufC *b = iHelpEn ? iHelpEn : iHelp;
	if (!b)
		return EFalse;
	TInt pos = 0, n = 0;
	TPtrC key, inl, body;
	while (nextSection(*b, pos, key, inl, body))
		if (key.Length() > 6 && key.Left(6) == _L("topic."))
			{
			if (n == aIdx)
				{
				aKey.Set(key);
				return ETrue;
				}
			n++;
			}
	return EFalse;
	}

TInt CPs1Container::TopicCount() const
	{
	TPtrC key;
	TInt n = 0;
	while (TopicAt(n, key))
		n++;
	return n;
	}

// Lays out the open topic. Draws the lines from aFrom on when a context is given;
// returns the number of lines in all.
TInt CPs1Container::LayoutHelp(CWindowGc *aGc, const TRect &aBody, TInt aFrom) const
	{
	TPtrC key, inl, body;
	if (!TopicAt(iHelpTopic, key) || !HelpFind(key, inl, body))
		return 0;
	const CFont *font = iCoeEnv->NormalFont();
	const TInt lh = font->HeightInPixels() + KRowGap;
	const TInt rows = BodyRows();
	TInt y = aBody.iTl.iY + font->AscentInPixels();
	TInt pos = 0, n = 0;
	TPtrC line;
	// 8 px of margin on the left, and room on the right for the scroll bar.
	while (wrapNext(font, body, aBody.Width() - 20, pos, line))
		{
		if (aGc && n >= aFrom && n < aFrom + rows)
			{
			aGc->DrawText(line, TPoint(aBody.iTl.iX + 8, y));
			y += lh;
			}
		n++;
		}
	return n;
	}

void CPs1Container::OpenHelp(TInt aFrom)
	{
	iHelpFrom = aFrom;
	iHelpTopic = -1;
	iPage = EPageHelp;
	}

void CPs1Container::OpenTopic(TInt aIdx)
	{
	TPtrC key;
	if (!TopicAt(aIdx, key))
		return;
	if (key.Compare(KTopicSetup) == 0)
		{
		OpenSetup(EPageHelp);
		return;
		}
	iHelpTopic = aIdx;
	iHelpScroll = 0;
	iHelpLines = LayoutHelp(NULL, BodyRect(), 0);
	}

void CPs1Container::OpenSetup(TInt aBack)
	{
	iSetupBack = aBack;
	iSetupSel = 0;
	RefreshSetup();
	iPage = EPageSetup;
	}

// What the setup page shows is looked up here, not on every redraw.
void CPs1Container::RefreshSetup()
	{
	iSetupBios = BiosThere();
	iSetupGames = 0;
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	CDir *dir = NULL;
	TFileName spec(KImgDir);
	spec.Append('*');
	if (fs.GetDir(spec, KEntryAttNormal, ESortNone, dir) == KErrNone && dir)
		{
		for (TInt i = 0; i < dir->Count(); i++)
			if (isImageName((*dir)[i].iName))
				iSetupGames++;
		delete dir;
		}
	fs.Close();
	}

// The hint of the selected row, or empty when the help files have none.
TPtrC CPs1Container::HintText() const
	{
	if (iSel < 0 || iSel >= ECfgCount)
		return TPtrC();
	const char *id = KItemHelpId[iSel];
	if (iSel == ECfgStart)
		id = iGameClosed ? "exit" : !iStartedOnce ? "start"
			: RestartNeeded() ? "restart" : "resume";
	TBuf<48> key(_L("hint."));
	TBuf<32> name;
	name.Copy(TPtrC8((const TUint8 *)id));
	key.Append(name);
	TPtrC inl, body;
	if (HelpFind(key, inl, body))
		return inl;
	return TPtrC();
	}

void CPs1Container::ResetHint()
	{
	iHintPx = 0;
	iHintPhase = 0;
	iHintT0 = User::NTickCount();
	}

// A hint wider than the status line waits at its start, scrolls pixel by pixel at a
// fixed speed (timed by the clock, so it is even whatever the tick jitter), waits at
// its end and starts over. Phase 0 = waiting at the start, 1 = moving, 2 = at the end.
void CPs1Container::HintTick()
	{
	if (iStatusMsg.Length())
		return;
	const TPtrC hint = HintText();
	const CFont *font = iCoeEnv->NormalFont();
	const TInt over = hint.Length()
		? font->TextWidthInPixels(hint) - (Rect().Width() - 12) : 0;
	if (over <= 0)
		{
		iHintPx = 0;
		iHintPhase = 0;
		return;
		}
	const TUint32 now = User::NTickCount();
	const TInt ms = (TInt)(now - iHintT0);
	if (iHintPhase == 0)
		{
		if (ms >= KHintHoldMs)
			{
			iHintPhase = 1;
			iHintT0 = now;
			}
		}
	else if (iHintPhase == 1)
		{
		TInt px = ms * KHintPxPerSec / 1000;
		if (px >= over)
			{
			px = over;
			iHintPhase = 2;
			iHintT0 = now;
			}
		if (px != iHintPx)
			{
			iHintPx = px;
			DrawNow();
			}
		}
	else if (ms >= KHintHoldMs)
		{
		iHintPx = 0;
		iHintPhase = 0;
		iHintT0 = now;
		DrawNow();
		}
	}

// --- Menu control -----------------------------------------------------------

void CPs1Container::OpenMenu()
	{
	if (iPage != EPageNone)
		return;
	ps1Log("menu", "open");
	voutSymbianSuspend(1);
	iPage = EPageMain;
	FixSel();
	ResetHint();
	LogGeometry();
	iConfirm = -1;
	iStatusMsg.Zero();
	DrawNow();
	}

void CPs1Container::DoCloseMenu(TBool aWithRestart)
	{
	SaveAll();
	ApplyKeyMap();
	voutSymbianSetTilt(iCfg.iTilt);
	voutSymbianSetTiltMap(iCfg.iTiltSide, iCfg.iTiltPitch, iCfg.iTiltDead, iCfg.iTiltSens);
	// No sensor means the switch would sit on "on" and do nothing.
	if (iCfg.iTilt && !voutSymbianTiltActive()) iCfg.iTilt = 0;
	// A different disc (or BIOS, CPU branch, sound, GL, DSA) cannot be switched on the
	// fly: the pass must be torn down and brought up again. This is done by the tick,
	// not by this branch - we got here from a key handler.
	if (aWithRestart && RestartNeeded())
		iRestartReq = ETrue;
	ps1CfgApplyLive(iCfg.iIlace, iCfg.iLight, iCfg.iBlend, iCfg.iPixSkip,
		iCfg.iFps, iCfg.iGpuSkip, iCfg.iCycle, iCfg.iSmooth, iCfg.iFxaa, iCfg.iDirty,
		iCfg.iSwDraw);
	ps1SndResetRate();
	ps1SetDebugLog(iCfg.iDebug);
	iPage = EPageNone;
	voutSymbianSuspend(0);
	ps1Log("menu", "close");
	}

void CPs1Container::FocusLost()
	{
	if (iStartedOnce && !iGameClosed && !iCliMode)
		OpenMenu();
	}

void CPs1Container::HandlePointerEventL(const TPointerEvent &aPointerEvent)
	{
	// The menu pages have no pointer handling yet; only the game takes taps.
	if (MenuActive())
		return;
	if (voutSymbianTouchEvent(aPointerEvent))
		OpenMenu();
	}

void CPs1Container::CloseMenu()
	{
	DoCloseMenu(ETrue);
	}

void CPs1Container::RevertRestartSettings()
	{
	if (iCfgPrevValid)
		{
		const TInt lang = iCfg.iLang;
		const TInt debug = iCfg.iDebug;
		iCfg = iCfgPrev;
		iCfg.iLang = lang;
		iCfg.iDebug = debug;
		iCfgPrevValid = EFalse;
		}
	iGamePath = iGameAtStart;
	iCfg.iBios = iBiosAtStart;
	iCfg.iInterp = iInterpAtStart;
	iCfg.iGteNf = iGteNfAtStart;
	iCfg.iSound = iSoundAtStart;
	iCfg.iWsola = iWsolaAtStart;
	iCfg.iGl = iGlAtStart;
	iCfg.iDsa = iDsaAtStart;
	}

// Progress page redraw: it receives no key events, and its state is changed by another
// thread, so the only way to update it is the application tick.
void CPs1Container::MenuTick()
	{
	if (iCloseReq)
		{
		iCloseReq = EFalse;
		ps1GuiCloseGame();
		BeginImport();
		DrawNow();
		return;
		}
	if (iPage == EPageMain)
		HintTick();
	if (iPage == EPageKeys)
		SlideKeysPoll();
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
	voutSymbianSetTilt(iCfg.iTilt);
	voutSymbianSetTiltMap(iCfg.iTiltSide, iCfg.iTiltPitch, iCfg.iTiltDead, iCfg.iTiltSens);
	// No sensor means the switch would sit on "on" and do nothing.
	if (iCfg.iTilt && !voutSymbianTiltActive()) iCfg.iTilt = 0;
	TBuf8<KMaxFileName + 1> path8;
	path8.Copy(iGamePath);
	TBuf8<KMaxFileName + 1> bios8;
	bios8.Copy(iCfg.iBios);
	ps1CfgApply((const char *)path8.PtrZ(), (const char *)bios8.PtrZ(),
		iCfg.iInterp, iCfg.iIlace,
		iCfg.iLight, iCfg.iBlend, iCfg.iPixSkip, iCfg.iGteNf, iCfg.iSound, iCfg.iGl,
		iCfg.iDirty, iCfg.iDsa, iCfg.iFps, iCfg.iGpuSkip, iCfg.iCycle, iCfg.iSmooth,
		iCfg.iFxaa, iCfg.iSwDraw, iCfg.iWsola);
	ps1SetDebugLog(iCfg.iDebug);
	iStartedOnce = ETrue;
	iRestartReq = EFalse;
	SnapRestart();
	iPage = EPageNone;
	}

void CPs1Container::SnapRestart()
	{
	iCfgPrevValid = EFalse;
	iGameAtStart = iGamePath;
	iBiosAtStart = iCfg.iBios;
	iInterpAtStart = iCfg.iInterp;
	iGteNfAtStart  = iCfg.iGteNf;
	iSoundAtStart  = iCfg.iSound;
	iWsolaAtStart  = iCfg.iWsola;
	iGlAtStart     = iCfg.iGl;
	iDsaAtStart    = iCfg.iDsa;
	}

TBool CPs1Container::RestartNeeded() const
	{
	return iGamePath.CompareF(iGameAtStart) != 0 ||
	       iCfg.iBios.CompareF(iBiosAtStart) != 0 ||
	       iCfg.iInterp != iInterpAtStart || iCfg.iGteNf != iGteNfAtStart ||
	       iCfg.iSound  != iSoundAtStart  || iCfg.iGl    != iGlAtStart ||
	       iCfg.iDsa    != iDsaAtStart    || iCfg.iWsola != iWsolaAtStart;
	}

void CPs1Container::StepValue(TInt aItem, TInt d)
	{
	TInt &v = iCfg.Value(aItem);
	if (aItem == ECfgGpuSkip)
		{
		// -1 is the adaptive mode of gpulib: it decides per-frame and is the one
		// that synchronizes the skip with the moment the game flips the buffer.
		v += d;
		if (v < -1) v = 3;
		if (v > 3) v = -1;
		}
	else if (aItem == ECfgFxaa)
		{
		v += d;
		if (v < 0) v = 5;
		if (v > 5) v = 0;
		}
	else if (aItem == ECfgCycle)
		{
		// The shown number is speed, so right must lower the multiplier.
		v -= d * KCycleStep;
		if (v < KCycleMin) v = KCycleMax;
		if (v > KCycleMax) v = KCycleMin;
		}
	else if (aItem == ECfgTiltSide || aItem == ECfgTiltPitch)
		{
		v = (v + d + 9) % 9;
		}
	else if (aItem == ECfgTiltDead)
		{
		v += d * 2;
		if (v < 0) v = 40;
		if (v > 40) v = 0;
		}
	else if (aItem == ECfgTiltSens)
		{
		v += d * 10;
		if (v < 40) v = 300;
		if (v > 300) v = 40;
		}
	else if (aItem == ECfgLang)
		{
		const TInt count = I18nLanguageCount();
		v = (v + d + count) % count;
		I18nSetLanguage(v);
		LoadHelp();
		}
	else if (aItem == ECfgPreset)
		{
		// Only highlights a name here; OK applies it (see below) - unlike
		// language, applying a preset overwrites fourteen keys at once and
		// should not happen on every arrow press while browsing the list.
		iPresetSel = (iPresetSel + d + EPresetCount) % EPresetCount;
		}
	else if (aItem == ECfgKeyBase)
		{
		// Same reasoning as ECfgPreset: highlight only, OK applies it.
		iBaseSel = (iBaseSel + d + EBaseCount) % EBaseCount;
		}
	else
		v = v ? 0 : 1;
	}

TKeyResponse CPs1Container::MainKeyL(TInt aScan)
	{
	FixSel();
	// C inside an open section closes it and leaves the cursor on its header; with no
	// section around the cursor it keeps its old meaning below.
	if ((aScan == EStdKeyBackspace || aScan == EStdKeyEscape) &&
		KItemSection[iSel] < ESecCount && KItemSection[iSel] == iOpenSec)
		{
		iSel = KSecHdr[iOpenSec];
		iOpenSec = ESecCount;
		ResetHint();
		iConfirm = -1;
		iStatusMsg.Zero();
		DrawNow();
		return EKeyWasConsumed;
		}
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
			{
			SaveAll();
			CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
			}
		return EKeyWasConsumed;
		}

	if (aScan == EStdKeyUpArrow || aScan == EStdKeyDownArrow)
		{
		const TInt d = (aScan == EStdKeyUpArrow) ? -1 : 1;
		FixSel();
		do
			{
			iSel = (iSel + d + ECfgCount) % ECfgCount;
			}
		while (!ItemVisible(iSel));
		ResetHint();
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
		if (isHeader(iSel))
			{
			// Right opens a closed section and Left closes an open one.
			const TInt sec = KItemSection[iSel];
			const TBool open = (iOpenSec == sec);
			if ((d > 0 && !open) || (d < 0 && open))
				{
				iOpenSec = open ? ESecCount : sec;
				ResetHint();
				}
			DrawNow();
			return EKeyWasConsumed;
			}
		if (!isCommand(iSel))
			{
			StepValue(iSel, d);
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
				{
				// Resuming a running game needs neither the disc file nor the BIOS on the
				// card; starting or restarting one needs both, so they are checked here.
				const TBool launching = !iStartedOnce || RestartNeeded();
				if (iGameClosed)
					{
					SaveAll();
					CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
					}
				else if (launching && (!iGamePath.Length() || !fileThere(iGamePath)))
					{
					iAsk = EAskNoGame;
					iAskSel = 0;
					iPage = EPageAsk;
					}
				else if (launching && !BiosThere())
					{
					iAsk = EAskNoBios;
					iAskSel = 0;
					iPage = EPageAsk;
					}
				else
					DoStart();
				}
				break;
			case ECfgGame:
				OpenGameBrowse();
				break;
			case ECfgSaveState:
				DoSaveState();
				break;
			case ECfgLoadState:
				DoLoadState();
				break;
			case ECfgAdd:
				OpenImport();
				break;
			case ECfgBios:
				OpenBiosBrowse();
				break;
			case ECfgKeys:
				iKeySel = 0;
				iCapture = -1;
				iKeyMenu = -1;
				iClashKey = -1;
				// A key the application never receives cannot be assigned, so all the
				// side keys are held while this page is open; leaving it calls
				// ApplyKeyMap, which trims them back to the ones actually bound.
				SyncSideKeyCaptures(ETrue);
				iPage = EPageKeys;
				break;
			case ECfgKeyBase:
				ApplyBase(iBaseSel);
				break;
			case ECfgPreset:
				ApplyPreset(iPresetSel);
				break;
			case ECfgResetKeys:
				// Two presses, as for the full reset below.
				if (iConfirm == ECfgResetKeys)
					{
					ResetControls();
					iConfirm = -1;
					}
				else
					iConfirm = ECfgResetKeys;
				break;
			case ECfgReset:
				// Two presses required: the key bindings and tuned speed hacks are too
				// expensive a loss for an accidental OK.
				if (iConfirm == ECfgReset)
					{
					// A reset is not a first run: the marker survives it.
					const TInt intro = iCfg.iIntro;
					iCfg.SetDefaults();
					iCfg.iIntro = intro;
					LoadHelp();
					ApplyKeyMap();
					iConfirm = -1;
					}
				else
					iConfirm = ECfgReset;
				break;
			case ECfgExit:
				SaveAll();
				CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
				return EKeyWasConsumed;
			case ECfgHdrGame:
			case ECfgHdrVideo:
			case ECfgHdrSpeed:
			case ECfgHdrSound:
			case ECfgHdrInput:
			case ECfgHdrSystem:
				{
				// One section open at a time: opening another closes this one.
				const TInt sec = KItemSection[iSel];
				iOpenSec = (iOpenSec == sec) ? ESecCount : sec;
				ResetHint();
				}
				break;
			case ECfgHelp:
				OpenHelp(EPageMain);
				break;
			default:
				// A value row steps like Right does, with the same limits.
				if (!isCommand(iSel))
					{
					StepValue(iSel, 1);
					iStatusMsg.Zero();
					}
				break;
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
		LeaveBrowse();
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
	iPage = EPageProgress;
	// The core holds the disc image open and reads from it every frame, while the
	// converter needs to read it and (with consent) delete it. Shut down the pass
	// BEFORE starting the queue - from the tick, since tearing it down here would
	// close sound and DSA inside a key handler.
	if (RunningGamePicked())
		{
		iGameClosed = ETrue;
		iCloseReq = ETrue;
		return;
		}
	BeginImport();
	}

void CPs1Container::BeginImport()
	{
	const TInt r = ConvStart(*iPicked, (TConvMode)iConvMode,
		iAskSel == 0 /*delete*/, KImgDir);
	if (r != KErrNone)
		ps1LogInt("import", "start failed", r);
	}

TKeyResponse CPs1Container::ImportKeyL(TInt aScan)
	{
	const TInt n = iBrowse->Count();
	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		LeaveBrowse();
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
		LeaveBrowse();
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
						else
							iStatusMsg.Copy(Tr(EStrBiosErr));
						ps1LogInt("bios", "pick", r);
						}
					}
				LeaveBrowse();
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
		iPage = (iAsk == EAskMode || iAsk == EAskDelete) ? EPageImport : EPageMain;
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
			// Keep is the default, even though the card is small: deleting is
			// irreversible, and two OK presses in a row are a habit on this phone.
			iAskSel = 1;
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
		else if (iAsk == EAskNoGame)
			{
			if (iAskSel == 0)
				OpenGameBrowse();
			else
				OpenImport();
			}
		else if (iAsk == EAskNoBios)
			{
			if (iAskSel == 0)
				DoStart();
			else
				OpenBiosBrowse();
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
	if (iCloseReq)
		return EKeyWasConsumed;
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
			// The import may have compressed or deleted the picked game's image.
			// When it was the running game, the pass is already closed and nothing
			// is left to fall back to.
			if (iGamePath.Length() && !fileThere(iGamePath))
				{
				if (iStartedOnce && !iGameClosed && fileThere(iGameAtStart))
					iGamePath = iGameAtStart;
				else
					iGamePath.Zero();
				}
			if (!iGamePath.Length()) AutoPickGameL();
			LeaveBrowse();
			}
		DrawNow();
		}
	return EKeyWasConsumed;
	}

// Setting the main key keeps the second key, unless the new main key is that same key.
void CPs1Container::StoreKey(TInt aRow, TBool aAlt, TInt aKey)
	{
	if (aAlt)
		iCfg.iKeyAlt[aRow] = aKey;
	else
		{
		iCfg.iKeyScan[aRow] = aKey;
		if (iCfg.iKeyAlt[aRow] == aKey)
			iCfg.iKeyAlt[aRow] = -1;
		}
	}

// Losing the main key promotes the second key, so the row keeps a key to press.
void CPs1Container::TakeKey(TInt aRow, TInt aKey)
	{
	if (iCfg.iKeyAlt[aRow] == aKey)
		iCfg.iKeyAlt[aRow] = -1;
	if (iCfg.iKeyScan[aRow] == aKey)
		{
		iCfg.iKeyScan[aRow] = iCfg.iKeyAlt[aRow];
		iCfg.iKeyAlt[aRow] = -1;
		}
	}

// The row other than aExcept that already uses aKey as its main or second key, -1 if none.
TInt CPs1Container::KeyRowUsing(TInt aKey, TInt aExcept) const
	{
	for (TInt i = 0; i < iKeyRows; i++)
		if (i != aExcept && (iCfg.iKeyScan[i] == aKey || iCfg.iKeyAlt[i] == aKey))
			return i;
	return -1;
	}

TKeyResponse CPs1Container::KeysKeyL(TInt aScan)
	{
	// A key taken from another row waits here for OK. Any other key, RSK included, drops it
	// and changes nothing: the other row loses the key only once the user has agreed.
	if (iClashKey >= 0)
		{
		if (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
			{
			TakeKey(iClashRow, iClashKey);
			StoreKey(iCapture, iCaptureAlt, iClashKey);
			}
		iClashKey = -1;
		iCapture = -1;
		DrawNow();
		return EKeyWasConsumed;
		}

	// Capture mode: the next key press becomes the binding. RSK is reserved for us -
	// it cancels, and it is the key that opens the menu in-game, so assigning it is not allowed.
	if (iCapture >= 0)
		{
		if (aScan != EStdKeyDevice1)
			{
			const TInt k = voutSymbianAliasKey(aScan);
			// A second key equal to the main one would be no second key: nothing changes.
			if (!iCaptureAlt || k != iCfg.iKeyScan[iCapture])
				{
				const TInt other = KeyRowUsing(k, iCapture);
				if (other >= 0)
					{
					iClashKey = k;
					iClashRow = other;
					// The hint line must show this prompt, not an earlier status message.
					iStatusMsg.Zero();
					DrawNow();
					return EKeyWasConsumed;
					}
				StoreKey(iCapture, iCaptureAlt, k);
				}
			}
		iCapture = -1;
		DrawNow();
		return EKeyWasConsumed;
		}

	// The action menu of the row: Up/Down pick a line, OK runs it, RSK, C or Escape closes it.
	if (iKeyMenu >= 0)
		{
		if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
			iKeyMenu = -1;
		else if (aScan == EStdKeyUpArrow)
			iKeyMenu = (iKeyMenu + EKeyActCount - 1) % EKeyActCount;
		else if (aScan == EStdKeyDownArrow)
			iKeyMenu = (iKeyMenu + 1) % EKeyActCount;
		else if (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
			{
			const TInt act = iKeyMenu;
			iKeyMenu = -1;
			if (act == EKeyActClear)
				{
				iCfg.iKeyScan[iKeySel] = -1;
				iCfg.iKeyAlt[iKeySel] = -1;
				}
			else
				{
				iCapture = iKeySel;
				iCaptureAlt = (act == EKeyActSecond);
				}
			}
		DrawNow();
		return EKeyWasConsumed;
		}

	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		ApplyKeyMap();
		SaveAll();
		iPage = EPageMain;
		DrawNow();
		return EKeyWasConsumed;
		}
	if (aScan == EStdKeyUpArrow)
		iKeySel = (iKeySel + iKeyRows - 1) % iKeyRows;
	else if (aScan == EStdKeyDownArrow)
		iKeySel = (iKeySel + 1) % iKeyRows;
	// Hold means nothing for the tilt zero key: it acts once per press.
	else if ((aScan == EStdKeyLeftArrow || aScan == EStdKeyRightArrow) && iKeySel < iPadCount)
		iCfg.iKeyHold[iKeySel] = iCfg.iKeyHold[iKeySel] ? 0 : 1;
	else if (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
		{
		// The menu's hint line must not be hidden by an earlier status message.
		iStatusMsg.Zero();
		iKeyMenu = EKeyActMain;
		}
	DrawNow();
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::HelpKeyL(TInt aScan)
	{
	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		if (iHelpTopic >= 0)
			iHelpTopic = -1;
		else
			iPage = iHelpFrom;
		DrawNow();
		return EKeyWasConsumed;
		}
	if (iHelpTopic < 0)
		{
		const TInt n = TopicCount();
		if (aScan == EStdKeyUpArrow && n)
			iHelpSel = (iHelpSel + n - 1) % n;
		else if (aScan == EStdKeyDownArrow && n)
			iHelpSel = (iHelpSel + 1) % n;
		else if (n && (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter ||
					   aScan == EStdKeyNkpEnter))
			OpenTopic(iHelpSel);
		}
	else
		{
		const TInt rows = BodyRows();
		TInt maxScroll = iHelpLines - rows;
		if (maxScroll < 0) maxScroll = 0;
		if (aScan == EStdKeyUpArrow)
			iHelpScroll -= 1;
		else if (aScan == EStdKeyDownArrow)
			iHelpScroll += 1;
		else if (aScan == EStdKeyLeftArrow)
			iHelpScroll -= rows - 1;
		else if (aScan == EStdKeyRightArrow)
			iHelpScroll += rows - 1;
		if (iHelpScroll > maxScroll) iHelpScroll = maxScroll;
		if (iHelpScroll < 0) iHelpScroll = 0;
		}
	DrawNow();
	return EKeyWasConsumed;
	}

TKeyResponse CPs1Container::SetupKeyL(TInt aScan)
	{
	if (aScan == EStdKeyDevice1 || aScan == EStdKeyBackspace || aScan == EStdKeyEscape)
		{
		iPage = iSetupBack;
		DrawNow();
		return EKeyWasConsumed;
		}
	if (aScan == EStdKeyUpArrow)
		iSetupSel = (iSetupSel + ESetCount - 1) % ESetCount;
	else if (aScan == EStdKeyDownArrow)
		iSetupSel = (iSetupSel + 1) % ESetCount;
	else if (aScan == EStdKeyLeftArrow || aScan == EStdKeyRightArrow)
		{
		// All three values apply at once: a first-time user has nothing to lose, and
		// the shown value is then always the one in force.
		const TInt d = (aScan == EStdKeyLeftArrow) ? -1 : 1;
		if (iSetupSel == ESetLang)
			{
			const TInt count = I18nLanguageCount();
			iCfg.iLang = (iCfg.iLang + d + count) % count;
			I18nSetLanguage(iCfg.iLang);
			LoadHelp();
			}
		else if (iSetupSel == ESetKeys)
			{
			iBaseSel = (iBaseSel + d + EBaseCount) % EBaseCount;
			ApplyBase(iBaseSel);
			}
		else if (iSetupSel == ESetPreset)
			{
			iPresetSel = (iPresetSel + d + EPresetCount) % EPresetCount;
			ApplyPreset(iPresetSel);
			}
		}
	else if (aScan == EStdKeyDevice3 || aScan == EStdKeyEnter || aScan == EStdKeyNkpEnter)
		{
		switch (iSetupSel)
			{
			case ESetBios:
				iBackPage = EPageSetup;
				OpenBiosBrowse();
				break;
			case ESetGames:
				// Games present: choose one. None yet: import them.
				iBackPage = EPageSetup;
				if (iSetupGames)
					OpenGameBrowse();
				else
					OpenImport();
				break;
			case ESetStart:
				iCfg.iIntro = 1;
				SaveAll();
				iPage = EPageMain;
				iSel = ECfgStart;
				break;
			case ESetHelp:
				OpenHelp(EPageSetup);
				break;
			default:
				break;
			}
		}
	DrawNow();
	return EKeyWasConsumed;
	}

// A leave from here costs ALL window events: CONE re-arms the request for the next event
// at the end of its RunL, and if control bypasses that line, keys and focus never arrive
// again even though the application keeps counting frames. That is exactly how the game
// went deaf after exiting the menu. We trap here rather than hoping nothing below leaves.
TKeyResponse CPs1Container::OfferKeyEventL(const TKeyEvent &aKeyEvent, TEventCode aType)
	{
	// The keyprobe asks whether the side key reaches the application, and in the
	// GUI branch voutSymbianPollPad cannot answer it: CONE dispatches the events
	// there, so that loop returns before it ever looks at one.
	if (ps1KeyProbeCli() && aType == EEventKey && iKeyProbeSeen < 32 &&
		aKeyEvent.iCode >= EKeyApplication2 && aKeyEvent.iCode <= EKeyApplication5)
		{
		++iKeyProbeSeen;
		ps1LogFmt("keyprobe", "media key scan=0x%02x code=0x%04x", (int)aKeyEvent.iScanCode,
			(int)aKeyEvent.iCode);
		}
	if (ps1KeyProbeCli() && aType == EEventKeyDown && iKeyProbeSeen < 32)
		{
		++iKeyProbeSeen;
		ps1LogFmt("keyprobe", "arrived scan=0x%02x code=0x%04x", (int)aKeyEvent.iScanCode,
			(int)aKeyEvent.iCode);
		}
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
		if (iPage == EPageHelp)     return HelpKeyL(sc);
		if (iPage == EPageSetup)    return SetupKeyL(sc);
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
	// Tilt zero key: the way the phone is held at this moment becomes the neutral
	// position. A key that is also bound to a PS1 button goes on pressing it below.
	const TInt sc0 = voutSymbianAliasKey(aKeyEvent.iScanCode);
	const TBool tiltKey = iCfg.iTilt && sc0 >= 0 &&
		(sc0 == iCfg.iKeyScan[iPadCount] || sc0 == iCfg.iKeyAlt[iPadCount]);
	if (tiltKey && aType == EEventKeyDown)
		{
		voutSymbianTiltRezero();
		ps1Log("tilt", "rezero by key");
		}
	if (voutSymbianKeyEvent(aKeyEvent.iScanCode, aType == EEventKeyDown))
		return EKeyWasConsumed;
	if (tiltKey)
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

// --- RemCon: media keys -----------------------------------------------------------

// Pseudo key of each RemCon operation, 0 for those that are not keys. Volume is left out
// because its keys already reach the application as key events.
static TInt remConScan(TRemConCoreApiOperationId aOperationId)
	{
	switch (aOperationId)
		{
		case ERemConCoreApiPausePlayFunction: return ERemConKeyPlayPause;
		case ERemConCoreApiPlay:              return ERemConKeyPlay;
		case ERemConCoreApiPause:             return ERemConKeyPause;
		case ERemConCoreApiStop:              return ERemConKeyStop;
		case ERemConCoreApiRewind:            return ERemConKeyRewind;
		case ERemConCoreApiFastForward:       return ERemConKeyFastFwd;
		case ERemConCoreApiForward:           return ERemConKeyNext;
		case ERemConCoreApiBackward:          return ERemConKeyPrev;
		default:                              return 0;
		}
	}

CPs1RemCon::CPs1RemCon(CPs1Container &aOwner) : CActive(EPriorityStandard), iOwner(aOwner)
	{
	iSelector = NULL;
	iTarget = NULL;
	iClick = NULL;
	iClickScan = 0;
	iLogged = 0;
	}

CPs1RemCon *CPs1RemCon::NewL(CPs1Container &aOwner)
	{
	CPs1RemCon *self = new (ELeave) CPs1RemCon(aOwner);
	CleanupStack::PushL(self);
	CActiveScheduler::Add(self);
	self->OpenTargetL();
	CleanupStack::Pop(self);
	return self;
	}

CPs1RemCon::~CPs1RemCon()
	{
	Cancel();
	// A click still holding its key would otherwise leave that key pressed.
	if (iClickScan)
		voutSymbianKeyEvent(iClickScan, 0);
	delete iClick;
	delete iSelector;    // the selector owns iTarget
	}

void CPs1RemCon::OpenTargetL()
	{
	iClick = CPeriodic::NewL(EPriorityStandard);
	iSelector = CRemConInterfaceSelector::NewL();
	iTarget = CRemConCoreApiTarget::NewL(*iSelector, *this);
	iSelector->OpenTargetL();
	}

void CPs1RemCon::MrccatoCommand(TRemConCoreApiOperationId aOperationId,
	TRemConCoreApiButtonAction aButtonAct)
	{
	if (iLogged < KRemConLogLines)
		{
		iLogged++;
		ps1LogFmt("remcon", "op=%d act=%d", (int)aOperationId, (int)aButtonAct);
		}
	const TInt scan = remConScan(aOperationId);
	if (scan)
		{
		if (aButtonAct == ERemConCoreApiButtonClick)
			Click(scan);
		else
			iOwner.RemConKey(scan, aButtonAct == ERemConCoreApiButtonPress);
		}
	Respond(aOperationId);
	}

// A click is a press now and a release KRemConClickUs later. A click that comes while
// the previous one is still held releases that one first.
void CPs1RemCon::Click(TInt aScan)
	{
	if (iClickScan)
		ClickEnd(this);
	iClickScan = aScan;
	iOwner.RemConKey(aScan, ETrue);
	iClick->Start(KRemConClickUs, KRemConClickUs, TCallBack(ClickEnd, this));
	}

TInt CPs1RemCon::ClickEnd(TAny *aSelf)
	{
	CPs1RemCon *self = static_cast<CPs1RemCon *>(aSelf);
	self->iClick->Cancel();
	const TInt scan = self->iClickScan;
	self->iClickScan = 0;
	self->iOwner.RemConKey(scan, EFalse);
	return KErrNone;
	}

// The request status is single, so a response sent while one is in flight is dropped.
void CPs1RemCon::Respond(TRemConCoreApiOperationId aOperationId)
	{
	if (IsActive())
		return;
	iTarget->SendResponse(iStatus, aOperationId, KErrNone);
	SetActive();
	}

void CPs1RemCon::RunL()
	{
	if (iStatus.Int() != KErrNone && iLogged < KRemConLogLines)
		{
		iLogged++;
		ps1LogFmt("remcon", "response err=%d", iStatus.Int());
		}
	}

void CPs1RemCon::DoCancel()
	{
	iSelector->SendCancel(ERemConResponse);
	}

// A media key from RemCon, as the pseudo key it stands for. In game it takes the path of a
// physical key; on the Controls page a press assigns it; other menu pages ignore it.
void CPs1Container::RemConKey(TInt aScan, TBool aDown)
	{
	if (ps1KeyProbeCli() && aDown && iKeyProbeSeen < 32)
		{
		++iKeyProbeSeen;
		ps1LogFmt("keyprobe", "arrived scan=0x%02x code=0x%04x", (int)aScan, 0);
		}
	if (iPage == EPageNone)
		voutSymbianKeyEvent(aScan, aDown);
	else if (iPage == EPageKeys && aDown)
		{
		TRAPD(e, KeysKeyL(aScan));
		if (e != KErrNone)
			ps1LogFmt("remcon", "keys leave=%d", (int)e);
		}
	}

// The anim is held while a binding uses a media key, plus the whole time the Controls
// page is open (aAll) so that a media key can be assigned there: elsewhere the media
// keys stay with the music player, as they were before the Slider base existed.
void CPs1Container::SyncSlideKeys(TBool aAll)
	{
	TBool bound = EFalse;
	for (TInt i = 0; i < iKeyRows && !bound; i++)
		{
		const TInt sc = iCfg.iKeyScan[i];
		const TInt alt = iCfg.iKeyAlt[i];
		bound = (sc >= ERemConKeyPlayPause && sc <= ERemConKeyPrev) ||
		        (alt >= ERemConKeyPlayPause && alt <= ERemConKeyPrev);
		}
	const TBool want = bound || aAll;
	// Only a binding that cannot work is worth a message: the Controls page loads the
	// anim on every phone, and on most it has no media keys to rename anyway.
	if (want && !iSlideAnim)
		SlideKeysStart(bound);
	else if (!want && iSlideAnim)
		SlideKeysStop();
	}

// Any failure leaves the media keys to RemCon, which does not get them on an N95 but may
// on another handset; the rest of the controls work either way. The usual failure is a
// phone that cannot load the DLL: a capability or certificate error on an unhacked phone.
void CPs1Container::SlideKeysStart(TBool aReport)
	{
	RAnimDll *dll = new RAnimDll(iCoeEnv->WsSession());
	RSlideAnim *anim = dll ? new RSlideAnim(*dll) : NULL;
	TInt err = anim ? KErrNone : KErrNoMemory;
	const char *step = "alloc";
	TBool loaded = EFalse;
	if (err == KErrNone)
		{
		step = "load";
		err = dll->Load(_L("ps1slide.dll"));
		loaded = (err == KErrNone);
		}
	TBool constructed = EFalse;
	if (err == KErrNone)
		{
		step = "construct";
		err = anim->Construct(Window());
		constructed = (err == KErrNone);
		}
	if (err == KErrNone)
		{
		step = "map";
		err = anim->SetMap(KSlideMap);
		}
	if (err == KErrNone)
		{
		step = "attach";
		err = anim->Attach();
		}
	ps1LogFmt("slidekeys", "start %s err=%d", step, (int)err);
	if (err == KErrNone)
		{
		iSlideDll = dll;
		iSlideAnim = anim;
		iSlideLast = anim->GetLast();
		return;
		}
	if (constructed)
		anim->Destroy();
	else
		delete anim;
	// A handle that never loaded is not closed: only a loaded one has anything to free.
	if (loaded)
		dll->Close();
	delete dll;
	if (aReport && iPage != EPageNone)
		iStatusMsg.Copy(Tr(EStrSlideKeysErr));
	}

void CPs1Container::SlideKeysStop()
	{
	if (!iSlideAnim)
		return;
	SlideKeysPoll();
	iSlideAnim->Detach();
	iSlideAnim->Destroy();        // closes and deletes the handle
	iSlideAnim = NULL;
	iSlideDll->Close();
	delete iSlideDll;
	iSlideDll = NULL;
	ps1Log("slidekeys", "stop");
	}

// Logs each new raw key-down the anim saw, before renaming. A media key whose raw code is
// not in KSlideMap never reaches the application, and this line is the only place its
// code shows: press it on the Controls page and read the log.
void CPs1Container::SlideKeysPoll()
	{
	if (!iSlideAnim || iSlideLogged >= KRemConLogLines)
		return;
	const TInt last = iSlideAnim->GetLast();
	if (last == iSlideLast)
		return;
	iSlideLast = last;
	const TInt sc = last & 0xffff;
	// The keys that drive this page itself say nothing about the media keys.
	if ((sc >= EStdKeyLeftArrow && sc <= EStdKeyDownArrow) || sc == EStdKeyDevice3 ||
		sc == EStdKeyDevice0 || sc == EStdKeyDevice1 || sc == EStdKeyBackspace)
		return;
	iSlideLogged++;
	ps1LogFmt("slidekeys", "raw scan=0x%02x downs=%d", last & 0xffff, (last >> 16) & 0x7fff);
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
	// Layout changed while the menu was up: output is re-attached after it closes.
	TBool iLayoutPending;
	// Capture handles held for the whole run by the keyprobe: released at once
	// on the way out. 0 = that key was not captured.
	TInt32 iKeyProbeCap[8];
	// Media key captures at top priority: scan codes up/down first, then key codes.
	TInt32 iKeyProbeMedia[8];
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

	if (ps1KeyProbeCli())
		{
		// The captures are kept for the whole run, not taken and given straight
		// back. The claim they once carried - that an uncaptured key never reaches
		// this application - is false: on 2026-09-22 four side scan codes arrived
		// here without ever being in the list. Capture is about taking the key away
		// from the system, not about receiving it.
		static const TInt KProbeKey[8] =
			{
			EStdKeyIncVolume, EStdKeyDecVolume, EStdKeyDevice5, EStdKeyDevice8,
			EStdKeyApplication19, EStdKeyApplication1D, EStdKeyDevice6, EStdKeyDevice7
			};
		RWindowGroup &root = iCoeEnv->RootWin();
		for (TInt i = 0; i < 8; i++)
			{
			const TInt32 h = root.CaptureKeyUpAndDowns(KProbeKey[i], 0, 0);
			iKeyProbeCap[i] = (h > 0) ? h : 0;
			ps1LogFmt("keyprobe", "scan=0x%02x capture=%d", (int)KProbeKey[i], (int)h);
			}
		// The N95 media keys open the music player even with a RemCon target open
		// (2026-10-24), so ask whether a top-priority capture takes them first.
		for (TInt i = 0; i < 4; i++)
			{
			const TInt32 hs = root.CaptureKeyUpAndDowns(EStdKeyApplication2 + i, 0, 0, KMaxTInt);
			const TInt32 hc = root.CaptureKey(EKeyApplication2 + i, 0, 0, KMaxTInt);
			iKeyProbeMedia[i] = (hs > 0) ? hs : 0;
			iKeyProbeMedia[4 + i] = (hc > 0) ? hc : 0;
			ps1LogFmt("keyprobe", "media scan=0x%02x capture=%d code=0x%04x capture=%d",
				(int)(EStdKeyApplication2 + i), (int)hs, (int)(EKeyApplication2 + i), (int)hc);
			}
		}

	// Lowest priority: CONE events (keys, redraws, exit) are serviced between frames,
	// not after the session ends.
	iIdle = CIdle::NewL(CActive::EPriorityIdle);
	iIdle->Start(TCallBack(TickCb, this));
	}

CPs1AppUi::~CPs1AppUi()
	{
	// Give the side keys back to the system, or volume stays deaf in whatever
	// runs next until the window group dies.
	TInt released = 0;
	for (TInt i = 0; i < 8; i++)
		if (iKeyProbeCap[i])
			{
			iCoeEnv->RootWin().CancelCaptureKeyUpAndDowns(iKeyProbeCap[i]);
			iKeyProbeCap[i] = 0;
			++released;
			}
	for (TInt i = 0; i < 8; i++)
		if (iKeyProbeMedia[i])
			{
			if (i < 4)
				iCoeEnv->RootWin().CancelCaptureKeyUpAndDowns(iKeyProbeMedia[i]);
			else
				iCoeEnv->RootWin().CancelCaptureKey(iKeyProbeMedia[i]);
			iKeyProbeMedia[i] = 0;
			++released;
			}
	if (released)
		ps1LogInt("keyprobe", "released", released);
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
		// The watchdog counts frames, and none run while the menu is up.
		ps1GuiHeartbeat();
		// Without a pause the main thread would spin CIdle at full speed and starve
		// the conversion worker thread (and outside conversion just heat the device).
		// Only the progress page and a moving hint need the short step; elsewhere it just
		// wakes the CPU.
		User::After(iContainer->TickUs());
		iInTick = EFalse;
		return ETrue;
		}

	if (iLayoutPending)
		{
		iLayoutPending = EFalse;
		if (iStarted)
			AttachOutput();
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
	else
		{
		// Hypothesis 5.1: execute frames in a loop without returning to active scheduler
		// while there are no completed asynchronous requests pending on the thread.
		RThread thisThread;
		int frameBurst = 0;
		while (more)
			{
			if (!ps1GuiStep())
				{
				more = EFalse;
				break;
				}
			if (++frameBurst >= 4 || thisThread.RequestCount() > 0)
				break;
			}
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
		// Attaching under an open menu would bring DSA up over it, or onto a closed game.
		if (iContainer->MenuActive())
			iLayoutPending = ETrue;
		else
			AttachOutput();
		}
	}

// A focus loss would explain "keys do not work" with no crash on our side: DSA draws
// the frame on top of everything, but events go to a different window.
void CPs1AppUi::HandleForegroundEventL(TBool aForeground)
	{
	CAknAppUi::HandleForegroundEventL(aForeground);
	ps1LogInt("focus", "fore", (int)aForeground);
	// Nobody is playing: pause, so the game neither runs nor sounds over a call.
	if (!aForeground && iContainer)
		iContainer->FocusLost();
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
