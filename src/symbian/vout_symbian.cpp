// Вивід кадру для Symbian. Замінює plugins/gpulib/vout_pl.c, який спирається на
// frontend/plugin_lib.h і libpicofe - шар, що під Symbian не портується.
//
// Етап 2 навмисно НЕ виводить на екран, а скидає VRAM у файл: так перевірка
// растеризації gpu_unai (робиться на хості) відокремлена від Direct Screen Access
// (окрема задача з власними помилками). Змішувати їх - шукати помилку рендерингу
// там, де насправді зламаний вивід.

#include <e32std.h>
#include <e32base.h>
#include <w32std.h>
#include <fbs.h>
#include <hal.h>
#include <gles/egl.h>
#include <gles/gl.h>

#include <stdio.h>
#include <string.h>
#include "compat_stdio.h"

// Шлях явний: у libpcsxcore є свій gpu.h, і за списком USERINCLUDE він знаходиться
// першим. Коротке "gpu.h" підхоплює саме його, а потрібен цей.
extern "C" {
#include "../../pcsx_rearmed/plugins/gpulib/gpu.h"
#include "../../pcsx_rearmed/frontend/plugin_lib.h"
}

// Останній стан вікна показу, який повідомив gpulib.
static struct {
	const void *vram;
	int offset;      // байтовий зсув початку вікна у VRAM
	int rgb24;
	int x, y, w, h;
	int updates;     // скільки разів gpulib просив показати кадр
	int blanks;
} g_vout;

// Лог оболонки (stage1.cpp): вивід сам JSONL не пише, але стан EGL/GL видно лише тут.
extern "C" void ps1LogFmt(const char *aType, const char *aFmt, ...);

extern "C" {

int vout_init(void)
	{
	memset(&g_vout, 0, sizeof(g_vout));
	return 0;
	}

int vout_finish(void)
	{
	return 0;
	}

int vout_update(struct psx_gpu *gpu, int src_x, int src_y)
	{
	int bpp = (gpu->status & PSX_GPU_STATUS_RGB24) ? 24 : 16;
	int x = gpu->screen.x;
	int y = gpu->screen.y;
	int w = gpu->screen.w;
	int h = gpu->screen.h;
	int src_x2 = 0;

	if (x < 0) { w += x; src_x2 = -x; x = 0; }
	if (y < 0) { h += y; src_y -= y;  y = 0; }
	if (w <= 0 || h <= 0)
		return 0;

	// gpu_unai пропускає непарні рядки при зменшенні
	if (h > 256 && gpu->state.downscale_enable && (src_y & 1))
		src_y++;

	g_vout.vram   = gpu->vram;
	g_vout.offset = (src_y * 1024 + src_x) * 2 + src_x2 * bpp / 8;
	g_vout.rgb24  = (bpp == 24);
	g_vout.x = x; g_vout.y = y; g_vout.w = w; g_vout.h = h;
	g_vout.updates++;

	gpu->state.dims_changed = 0;
	return 1;
	}

void vout_blank(struct psx_gpu * /*gpu*/)
	{
	g_vout.blanks++;
	}

void vout_set_config(const struct rearmed_cbs * /*cbs*/)
	{
	}

long GPUopen(unsigned long * /*disp*/, char * /*cap*/, char * /*cfg*/)
	{
	gpu.frameskip.active = 0;
	gpu.frameskip.frame_ready = 1;
	return 0;
	}

long GPUclose(void)
	{
	return 0;
	}

} // extern "C"

// --- Наше розширення: знімок для перевірки на хості --------------------------

// Формат навмисно найпростіший: текстовий заголовок, далі сира VRAM 1024x512x16.
// Розбирати на хості тривіально, а помилок формату, які можна сплутати з
// помилками растеризації, тут не виникає.
extern "C" int voutSymbianDump(const char *path)
	{
	if (!g_vout.vram)
		return -1;

	FILE *f = fopen(path, "wb");
	if (!f)
		return -2;

	char hdr[128];
	int n = snprintf(hdr, sizeof(hdr),
		"PS1VRAM 1024 512 16 win=%d,%d,%d,%d off=%d rgb24=%d upd=%d blank=%d\n",
		g_vout.x, g_vout.y, g_vout.w, g_vout.h,
		g_vout.offset, g_vout.rgb24, g_vout.updates, g_vout.blanks);
	fwrite(hdr, 1, n, f);
	fwrite(g_vout.vram, 1, 1024 * 512 * 2, f);
	fclose(f);
	return 0;
	}

extern "C" int voutSymbianUpdates(void)
	{
	return g_vout.updates;
	}

// --- Живий вивід кадру на екран (window server + BitBlt, Етап 2) ------------
// Вікно/контекст/бітмап створюються ОДИН раз (Init), кадр блитається ЩОКАДРУ
// (Present), наприкінці - тримання й прибирання (Finish). Так на екрані видно
// саму анімацію (завантаження BIOS, гру), а не єдиний кадр у кінці прогону -
// раніше показ у кінці ловив BIOS уже в простої (чорний екран).
//
// Present сам оновлює g_vout поточним кадром (vout_update), бо gpulib презентує
// рідко - лише за fb_dirty_display_area (тому в дампі був upd=1). Нам потрібен
// кожен кадр, тож викликаємо vout_update напряму й читаємо свіжий VRAM.
//
// DSA (CDirectScreenAccess) навмисно не беремо: це CActive із CActiveScheduler
// та обробкою AbortNow/Restart; синхронного BitBlt для нашого циклу достатньо.

static TBool g_wsReady;
static RWsSession g_ws;
static CWsScreenDevice *g_dev;
static RWindowGroup g_grp;
static RWindow g_win;
static CWindowGc *g_gc;
static CFbsBitmap *g_bmp;
static TSize g_scr;

// Вікно буває наше (консольна гілка створює g_ws/g_grp/g_win сама) або чуже - від
// CCoeControl в AVKON-оболонці, бо MBX-EGL вимагає вікна під CCoeEnv. Робота йде
// через покажчики; закриваємо лише те, що створили самі.
static TBool g_ownWindow = ETrue;
static RWsSession *g_wsp = &g_ws;
static RWindow *g_winp = &g_win;

// Кеш геометрії вписування: чорні поля (FillZ) чистимо лише коли вона змінилась,
// а не щокадру - решту бітмапа однаково повністю перезаписує копія кадру.
// Скидається в initL при (пере)створенні бітмапа.
static TInt g_blitOx = -1, g_blitOy, g_blitW, g_blitH;

// Поділ часу блиту: fill = цикл масштабування у бітмап, present = BitBlt+Flush у
// window server. Показує, що оптимізувати - цикл (кеш) чи презентацію (DSA/MBX).
static unsigned long long g_blitFillTicks, g_blitPresentTicks;
extern "C" unsigned int ndrc_symbian_fastfreq(void);

// DSA (Direct Screen Access) + direct-FB (запис прямо в LCD через HAL-адресу).
// СТАТУС: законсервований експеримент (типово off), глухий кут на N95 8GB -
// перевірено 2026-08-01 тест-патерном ("fbtest"):
//   1) WS композитить порожнє вікно поверх наших сирих HAL-записів (кадр блимав
//      частку секунди й зникав) - координований лише шлях через g_dsaGc->BitBlt,
//      а це та сама пропускна здатність, що й window;
//   2) HAL віддає ЛОГІЧНУ (повернуту) геометрію 320x240/640B, а сира пам'ять -
//      ФІЗИЧНА портретна 240x320/480B -> діагональний stride-зсув при автоповороті.
// Present упирається в bandwidth LCD; чисто його побити може лише MBX (EGL). Код
// лишено за прапорцем як основу/довідку. Вмикання: аргумент "dsa" (+"fbtest").
static TBool g_useDsa;
static CDirectScreenAccess *g_dsa;
static CFbsBitGc *g_dsaGc;
static TBool g_dsaActive;
static CActiveScheduler *g_dsaSched;   // ненульовий лише якщо його створив DSA

// Прямий запис у буфер LCD (HAL): масштабуємо кадр одразу в екран, зливаючи fill
// і present в один прохід (профіль показав: present - це bandwidth копії 150КБ,
// не оверхед). Лише коли DSA активний (володіє екраном) і формат = RGB565.
static TBool    g_fbOk;
static TUint16 *g_fbBase;    // адреса першого пікселя
static TInt     g_fbStride;  // 16-бітних слів на рядок
static TInt     g_fbW, g_fbH;
// Сирі HAL-значення (для діагностики "двох вікон" - stride/розміри/буфер).
static TInt g_fbAddr, g_fbBpp, g_fbLineBytes, g_fbFirstOff, g_fbXpix, g_fbYpix;

// EGL/GL ES 1.1 вивід (MBX): кадр PS1 -> текстура -> текстурований квад ->
// eglSwapBuffers. GPU робить масштабування і present. За прапорцем "gl", із
// відкатом на window-BitBlt. Колір: BGR555->RGB565 при заповненні текстури.
//
// СТАТУС (2026-08-02): ПРАЦЮЄ і дає коректну картинку під AVKON/CONE (на голому
// RWindow консольної версії eglCreateWindowSurface панікував - MBX-EGL спирається
// на CCoeEnv::Static()), але на N95 8GB вчетверо ПОВІЛЬНІШЕ за bitmap-шлях, тож
// лишається вимкненим за замовчуванням (прапорець "gl").
//
// Заміряно на GT, той самий слід вводу (мс на кадр):
//   bitmap: fill  8.0 + present  3.7 = 11.7  -> ~30 fps
//   GL:     fill 23.9 + present 22.3 = 46.2  -> ~15 fps
//
// Два обмеження, обидва виміряні й обидва не обходяться на цьому апараті:
//  1. Драйвер віддає ЛИШЕ 32-бітні конфіги (configs=4, have16=0). Поверхня тому
//     обов'язково 32-бітна, а панель 16-бітна - перетворення 32->16 неминуче:
//     або в EGL, або в композиторі. Звідси 22.3 мс на present, що саме по собі
//     вдвічі більше за ВЕСЬ bitmap-шлях.
//  2. MBX тримає текстури у twiddled-порядку, тож завантаження кадру коштує
//     23.9 мс (5.4 МБ/с) навіть після переходу на текстуру точно за розміром
//     джерела і повне glTexImage2D замість підобласті (спершу було 73 мс).
//
// Загальний висновок: GL виграє там, де GPU малює ГЕОМЕТРІЮ. Віддавати через
// нього готовий кадр, порахований процесором, на MBX Lite завжди дорожче за
// прямий blit. Реальний виграш дав би лише растеризатор PS1 на GPU замість
// gpu_unai - це окремий рендерер, а не налаштування.
static const TInt KGlTexW = 1024;   // POT, покриває ширину дисплея <=1024
static const TInt KGlTexH = 256;    // POT, покриває поле <=256 рядків
static TBool      g_useGl;
static TBool      g_glReady;
static EGLDisplay g_eglDpy = EGL_NO_DISPLAY;
static EGLSurface g_eglSurf = EGL_NO_SURFACE;
static EGLContext g_eglCtx = EGL_NO_CONTEXT;
static EGLConfig  g_eglCfg;
static GLuint     g_glTex;
static TInt       g_glW, g_glH;      // розміри EGL-поверхні (не вікна)
static TBool      g_glLogged;        // діагностику першого кадру пишемо один раз
static TUint16   *g_glBuf;           // буфер конвертації (rows*w, RGB565)
static TBool glInitOnWindow();       // визначені нижче, кличуться з initL
static void  glShutdown();

static void queryFramebuffer()
	{
	g_fbOk = EFalse;
	g_fbAddr = g_fbBpp = g_fbLineBytes = g_fbFirstOff = g_fbXpix = g_fbYpix = 0;
	HAL::Get(HALData::EDisplayMemoryAddress, g_fbAddr);
	HAL::Get(HALData::EDisplayBitsPerPixel, g_fbBpp);
	HAL::Get(HALData::EDisplayOffsetBetweenLines, g_fbLineBytes);
	HAL::Get(HALData::EDisplayOffsetToFirstPixel, g_fbFirstOff);
	HAL::Get(HALData::EDisplayXPixels, g_fbXpix);
	HAL::Get(HALData::EDisplayYPixels, g_fbYpix);
	if (g_fbAddr == 0 || g_fbBpp != 16 || g_fbLineBytes < 2 || g_fbXpix < 1 || g_fbYpix < 1)
		return;   // формат не підходить - лишаємось на window-шляху
	g_fbBase   = (TUint16 *)((TUint8 *)g_fbAddr + g_fbFirstOff);
	g_fbStride = g_fbLineBytes / 2;
	g_fbW = g_fbXpix; g_fbH = g_fbYpix;
	g_fbOk = ETrue;
	}

static void dsaStartL()
	{
	g_dsa->StartL();
	g_dsaGc = g_dsa->Gc();
	g_dsaGc->SetClippingRegion(g_dsa->DrawingRegion());
	g_dsaActive = ETrue;
	}

// Спостерігач DSA: WS кличе AbortNow, коли забирає екран (діалог/сповіщення), і
// Restart, коли можна відновити. Між ними малюємо через window-шлях (fallback).
class TDsaObserver : public MDirectScreenAccess
	{
public:
	void AbortNow(RDirectScreenAccess::TTerminationReasons) { g_dsaActive = EFalse; }
	void Restart(RDirectScreenAccess::TTerminationReasons)
		{
		g_dsaActive = EFalse;
		if (g_dsa) { TRAP_IGNORE(dsaStartL()); }
		}
	};
static TDsaObserver g_dsaObs;

// Ввід: неблокуюче опитування подій RWsSession. g_evStat озброюється раз у initL,
// щокадру voutSymbianPollPad() виснажує чергу й тримає g_pad (біти DKEY_*, active-high).
static TRequestStatus g_evStat;
static unsigned short g_pad;

// Розкладка клавіатури N95 -> кнопки PS1 (біти DKEY_* з plugin_lib.h). Стрілки -
// хрестовина, центральна - Cross (підтвердити), цифри - лицьові кнопки.
static TInt mapScanToDkey(TInt aScan)
	{
	switch (aScan)
		{
		case EStdKeyUpArrow:    return DKEY_UP;
		case EStdKeyDownArrow:  return DKEY_DOWN;
		case EStdKeyLeftArrow:  return DKEY_LEFT;
		case EStdKeyRightArrow: return DKEY_RIGHT;
		case EStdKeyDevice3:                          // центральна навігаційна
		case EStdKeyEnter:                            // деякі прошивки шлють центр як Enter
		case '5':               return DKEY_CROSS;
		case '0':               return DKEY_CIRCLE;   // назад/скасувати
		case '7':               return DKEY_SQUARE;
		case '9':               return DKEY_TRIANGLE;
		case '1':               return DKEY_L1;
		case '3':               return DKEY_R1;
		case '4':               return DKEY_L2;
		case '6':               return DKEY_R2;
		case '2':               return DKEY_SELECT;
		case '8':               return DKEY_START;
		default:                return -1;
		}
	}

// PS1 VRAM - BGR555 (біт15 маска, 14-10 B, 9-5 G, 4-0 R); екран EColor64K -
// RGB565. Зелений розширюємо з 5 до 6 біт, щоб не втратити яскравість.
static inline TUint16 Bgr555ToRgb565(TUint16 aPix)
	{
	const TUint r = aPix & 0x1f;
	const TUint g = (aPix >> 5) & 0x1f;
	const TUint b = (aPix >> 10) & 0x1f;
	const TUint g6 = (g << 1) | (g >> 4);
	return (TUint16)((r << 11) | (g6 << 5) | b);
	}

static void initL()
	{
	g_ownWindow = ETrue;
	g_wsp = &g_ws;
	g_winp = &g_win;
	User::LeaveIfError(g_ws.Connect());
	g_dev = new (ELeave) CWsScreenDevice(g_ws);
	User::LeaveIfError(g_dev->Construct());
	g_grp = RWindowGroup(g_ws);
	User::LeaveIfError(g_grp.Construct((TUint32)&g_grp, ETrue));
	g_grp.SetOrdinalPosition(0);
	g_win = RWindow(g_ws);
	User::LeaveIfError(g_win.Construct(g_grp, (TUint32)&g_win));
	g_scr = g_dev->SizeInPixels();
	g_win.SetExtent(TPoint(0, 0), g_scr);
	g_win.SetBackgroundColor(KRgbBlack);
	g_win.Activate();
	g_gc = new (ELeave) CWindowGc(g_dev);
	User::LeaveIfError(g_gc->Construct());
	g_bmp = new (ELeave) CFbsBitmap;
	User::LeaveIfError(g_bmp->Create(g_scr, EColor64K));
	g_blitOx = -1;   // новий бітмап - примусити FillZ на першому кадрі
	g_blitFillTicks = g_blitPresentTicks = 0;

	// Ввід: група вже на передньому плані (ordinal 0), тож отримує клавіші.
	// Озброюємо чергу подій один раз; опитуємо неблокуюче щокадру.
	g_pad = 0;
	g_ws.EventReady(&g_evStat);

	// GL ES вивід (MBX): якщо ввімкнено й вдалось - present іде через GPU. Інакше
	// (нема 3D / помилка) лишаємось на window-BitBlt.
	g_glReady = EFalse;
	if (g_useGl)
		{
		TRAPD(ge, { if (!glInitOnWindow()) glShutdown(); });
		if (ge != KErrNone) { glShutdown(); }
		}

	// DSA: потребує CActiveScheduler (для подій abort/restart). Зазвичай його вже
	// створила аудіо-підсистема; якщо ні - робимо власний і прибираємо у Finish.
	g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; g_dsaSched = NULL;
	if (g_useDsa)
		{
		if (!CActiveScheduler::Current())
			{
			g_dsaSched = new CActiveScheduler;
			if (g_dsaSched) CActiveScheduler::Install(g_dsaSched);
			}
		TRAPD(e, {
			g_dsa = CDirectScreenAccess::NewL(g_ws, *g_dev, g_win, g_dsaObs);
			dsaStartL();
			});
		if (e != KErrNone)
			{ delete g_dsa; g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; }
		else
			queryFramebuffer();   // прямий запис у LCD, якщо формат підходить
		}

	g_wsReady = ETrue;
	}

// Трасування з негайним флешем: переживає панік (TRAP не ловить), щоб знати, на
// якому EGL/GL-виклику падає ініціалізація. Читається з E:\gltrace.txt.
// EGL/GL ES ініціалізація на наявному вікні. Повертає EFalse -> відкат на bitmap.
// Вимагає вікна під CCoeEnv (AVKON), інакше eglCreateWindowSurface панікує.
// Що саме дав драйвер: розміри поверхні й РЕАЛЬНІ розрядності конфігу. Запит
// 5-6-5 не зобов'язує драйвер його віддати, а невідповідність формату вікна і
// поверхні виглядає саме як зіпсовані кольори, а не як помилка.
static void glLogState(const char *aWhere)
	{
	EGLint sw = 0, sh = 0, r = 0, g = 0, b = 0, a = 0, id = 0;
	eglQuerySurface(g_eglDpy, g_eglSurf, EGL_WIDTH, &sw);
	eglQuerySurface(g_eglDpy, g_eglSurf, EGL_HEIGHT, &sh);
	if (g_eglCfg)
		{
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_RED_SIZE, &r);
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_GREEN_SIZE, &g);
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_BLUE_SIZE, &b);
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_ALPHA_SIZE, &a);
		eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_CONFIG_ID, &id);
		}
	EGLint bs = 0;
	if (g_eglCfg) eglGetConfigAttrib(g_eglDpy, g_eglCfg, EGL_BUFFER_SIZE, &bs);
	ps1LogFmt("gl", "%s surf=%dx%d win=%dx%d cfg=%d rgba=%d%d%d%d bufsz=%d winmode=%d egl=0x%x glerr=0x%x",
		aWhere, (int)sw, (int)sh, (int)g_scr.iWidth, (int)g_scr.iHeight,
		(int)id, (int)r, (int)g, (int)b, (int)a, (int)bs,
		(int)(g_winp ? g_winp->DisplayMode() : 0),
		(unsigned)eglGetError(), (unsigned)glGetError());
	}

static TBool glInitOnWindow()
	{
	g_eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_eglDpy == EGL_NO_DISPLAY) return EFalse;
	if (!eglInitialize(g_eglDpy, NULL, NULL)) return EFalse;
	// Явний RGB565: конфіг має збігатися з форматом вікна.
	const EGLint attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_BUFFER_SIZE,  16,
		EGL_RED_SIZE,     5,
		EGL_GREEN_SIZE,   6,
		EGL_BLUE_SIZE,    5,
		EGL_DEPTH_SIZE,   0,
		EGL_NONE };
	// КЛЮЧОВЕ для не-ScreenPlay пристроїв (а N95/S60v3 саме такий). Symbian
	// Developer Library, "Symbian-Specific Behavior": режим відображення RWindow
	// МУСИТЬ збігатися з розрядністю буфера EGL-поверхні. Інакше вміст поверхні
	// читається як інший формат: 32-бітний буфер, прочитаний як 16-бітний, дає
	// і зсунуті кольори (чорний виглядає жовтим), і вдвічі ширшу картинку.
	//
	// eglChooseConfig до того ж трактує розрядності як МІНІМУМИ і на запит 5-6-5
	// повертає 8888, тож вибирати доводиться самим - і саме за розрядністю, яку
	// вікно погодилось прийняти.
	EGLConfig cfgs[32]; EGLint n = 0;
	if (!eglChooseConfig(g_eglDpy, attribs, cfgs, 32, &n) || n < 1) return EFalse;

	EGLint want16 = 0, want32 = 0;
	for (EGLint i = 0; i < n; i++)
		{
		EGLint bs = 0;
		eglGetConfigAttrib(g_eglDpy, cfgs[i], EGL_BUFFER_SIZE, &bs);
		if (bs == 16) want16 = 1;
		if (bs == 32) want32 = 1;
		}
	// Просимо режим, для якого конфіг узагалі існує; 16 біт дешевше, тож перше.
	TDisplayMode dm = EColor64K;
	if (!want16 && want32) dm = EColor16MU;
	g_winp->SetRequiredDisplayMode(dm);
	g_wsp->Flush();
	const TDisplayMode got = g_winp->DisplayMode();
	const EGLint gotBits = (got == EColor64K) ? 16 : (got == EColor16M ? 24 : 32);

	EGLConfig cfg = cfgs[0]; EGLint cfgBits = 0;
	for (EGLint i = 0; i < n; i++)
		{
		EGLint bs = 0;
		eglGetConfigAttrib(g_eglDpy, cfgs[i], EGL_BUFFER_SIZE, &bs);
		if (bs == gotBits) { cfg = cfgs[i]; cfgBits = bs; break; }
		}
	g_eglCfg = cfg;
	ps1LogFmt("gl", "configs=%d have16=%d have32=%d wantMode=%d gotMode=%d bits=%d cfgBits=%d",
		(int)n, (int)want16, (int)want32, (int)dm, (int)got, (int)gotBits, (int)cfgBits);
	g_eglSurf = eglCreateWindowSurface(g_eglDpy, cfg, (NativeWindowType)g_winp, NULL);
	if (g_eglSurf == EGL_NO_SURFACE) return EFalse;
	g_eglCtx = eglCreateContext(g_eglDpy, cfg, EGL_NO_CONTEXT, NULL);
	if (g_eglCtx == EGL_NO_CONTEXT) return EFalse;
	if (!eglMakeCurrent(g_eglDpy, g_eglSurf, g_eglSurf, g_eglCtx)) return EFalse;
	// Без очікування вертикальної синхронізації: емулятор і так не тримає 60 Гц,
	// а блокування у swap додається просто до часу кадру.
	eglSwapInterval(g_eglDpy, 0);

	g_glBuf = (TUint16 *)User::Alloc(KGlTexW * KGlTexH * 2);
	if (!g_glBuf) return EFalse;

	// Геометрія - з поверхні, а не з вікна: вікно автоповертається, і розбіжність
	// між ними дає саме "неправильний масштаб".
	EGLint sw = 0, sh = 0;
	eglQuerySurface(g_eglDpy, g_eglSurf, EGL_WIDTH, &sw);
	eglQuerySurface(g_eglDpy, g_eglSurf, EGL_HEIGHT, &sh);
	if (sw > 0 && sh > 0) { g_glW = sw; g_glH = sh; }
	else                  { g_glW = g_scr.iWidth; g_glH = g_scr.iHeight; }

	glViewport(0, 0, g_glW, g_glH);
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	glOrthof(0, (GLfloat)g_glW, (GLfloat)g_glH, 0, -1, 1);  // top-left origin
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glClearColor(0, 0, 0, 1);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
	glGenTextures(1, &g_glTex);
	glBindTexture(GL_TEXTURE_2D, g_glTex);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	// Нулями, а не NULL: у NULL-варіанті вміст текстури не визначений, і все, що
	// вибірка зачепить поза залитою ділянкою, показує сміття драйвера.
	Mem::FillZ(g_glBuf, KGlTexW * KGlTexH * 2);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, KGlTexW, KGlTexH, 0,
		GL_RGB, GL_UNSIGNED_SHORT_5_6_5, g_glBuf);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	g_glReady = ETrue;
	glLogState("init");
	return ETrue;
	}

// Один кадр через GL: конвертуємо поле у буфер (BGR555->RGB565), вантажимо у
// текстуру, малюємо летербоксований квад, свопаємо. Масштаб і present - на GPU.
static void glPresentFrame(const TUint8 *vbytes, TInt startByte,
                           TInt sw, TInt sh, TBool field, TInt srcRows)
	{
	// Ті самі два лічильники, що й на bitmap-шляху, щоб цифри були зіставні:
	// "fill" = конвертація + завантаження текстури, "present" = малювання + swap.
	const TUint _gc0 = User::FastCounter();
	TInt hf = field ? srcRows : sh;
	if (hf > KGlTexH) hf = KGlTexH;
	TInt w = sw; if (w > KGlTexW) w = KGlTexW;

	// Текстура рівно під джерело (найменший POT), а не постійні 1024x256: MBX
	// зберігає текстури у twiddled-порядку, тож драйвер перекладає ВСЮ область
	// на кожному завантаженні, і зайві мегабайти оплачуються щокадру.
	TInt potW = 16; while (potW < w)  potW <<= 1;
	TInt potH = 16; while (potH < hf) potH <<= 1;

	for (TInt y = 0; y < hf; y++)
		{
		const TInt ssy = field ? y * 2 : y;
		const TUint16 *srow = (const TUint16 *)(vbytes + startByte) + ssy * 1024;
		TUint16 *drow = g_glBuf + y * potW;
		for (TInt x = 0; x < w; x++)
			drow[x] = Bgr555ToRgb565(srow[x]);
		}
	glBindTexture(GL_TEXTURE_2D, g_glTex);
	// Повне перевизначення рівня замість підобласті: часткове завантаження в
	// twiddled-текстуру - це read-modify-write усього рівня.
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, potW, potH, 0,
		GL_RGB, GL_UNSIGNED_SHORT_5_6_5, g_glBuf);
	const TUint _gc1 = User::FastCounter();
	g_blitFillTicks += (unsigned)(_gc1 - _gc0);

	// Летербокс зі збереженням пропорцій (аспект за повною висотою sh).
	const TInt SW = g_glW, SH = g_glH;
	TInt dstW = SW, dstH = sh * SW / sw;
	if (dstH > SH) { dstH = SH; dstW = sw * SH / sh; }
	const TInt ox = (SW - dstW) / 2, oy = (SH - dstH) / 2;
	const GLfloat u = (GLfloat)w / potW, v = (GLfloat)hf / potH;
	const GLfloat x0 = (GLfloat)ox, y0 = (GLfloat)oy, x1 = (GLfloat)(ox + dstW), y1 = (GLfloat)(oy + dstH);
	const GLfloat verts[8] = { x0, y0,  x1, y0,  x0, y1,  x1, y1 };
	const GLfloat texc[8]  = { 0, 0,   u, 0,    0, v,    u, v };

	glClear(GL_COLOR_BUFFER_BIT);
	glVertexPointer(2, GL_FLOAT, 0, verts);
	glTexCoordPointer(2, GL_FLOAT, 0, texc);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	eglSwapBuffers(g_eglDpy, g_eglSurf);
	g_blitPresentTicks += (unsigned)(User::FastCounter() - _gc1);

	// Один раз після першого кадру: помилка GL тут означає, що завантаження
	// текстури або малювання не відбулося, хоча ініціалізація пройшла.
	if (!g_glLogged)
		{
		g_glLogged = ETrue;
		ps1LogFmt("gl", "frame1 src=%dx%d field=%d tex=%dx%d dst=%dx%d+%d+%d",
			(int)sw, (int)sh, (int)field, (int)w, (int)hf,
			(int)dstW, (int)dstH, (int)ox, (int)oy);
		glLogState("frame1");
		}
	}

static void glShutdown()
	{
	if (g_eglDpy != EGL_NO_DISPLAY)
		{
		eglMakeCurrent(g_eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (g_eglCtx != EGL_NO_CONTEXT)  eglDestroyContext(g_eglDpy, g_eglCtx);
		if (g_eglSurf != EGL_NO_SURFACE) eglDestroySurface(g_eglDpy, g_eglSurf);
		eglTerminate(g_eglDpy);
		}
	g_eglDpy = EGL_NO_DISPLAY; g_eglSurf = EGL_NO_SURFACE; g_eglCtx = EGL_NO_CONTEXT;
	g_eglCfg = NULL;
	User::Free(g_glBuf); g_glBuf = NULL;
	g_glReady = EFalse;
	// Повертаємо 16-бітний режим вікна: під GL ми піднімали його до 32 біт, і
	// лишити так означало б платити за перетворення ще й на bitmap-шляху, куди
	// відкат і веде.
	if (g_winp)
		{
		g_winp->SetRequiredDisplayMode(EColor64K);
		if (g_wsp) g_wsp->Flush();
		}
	}

// Масштабує поточний g_vout у g_bmp (nearest-neighbor, збереження пропорцій,
// чорні поля) і блитає у вікно.
static void blitCurrent()
	{
	const TUint8 *vbytes = (const TUint8 *)g_vout.vram;
	if (!vbytes)
		return;

	TInt startByte = 0;
	TInt sw = g_scr.iWidth, sh = g_scr.iHeight;
	const TBool rgb24 = g_vout.rgb24;
	if (g_vout.w > 0 && g_vout.h > 0)
		{
		startByte = g_vout.offset;
		sw = g_vout.w;
		sh = g_vout.h;
		}
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;
	const TInt maxW = rgb24 ? 682 : 1024;          // 2048 байтів на рядок VRAM
	if (sw > maxW) sw = maxW;
	const TInt startRow = startByte / 2048;
	if (sh > 512 - startRow) sh = 512 - startRow;  // не читати за межі VRAM 1024x512
	if (sh < 1) sh = 1;

	// Пампимо планувальник (DSA abort/restart) до вибору цілі, щоб не писати в
	// екран, який WS уже забрав.
	if (g_useDsa && CActiveScheduler::Current())
		{ TInt _e; while (CActiveScheduler::RunIfReady(_e, CActive::EPriorityIdle)) { } }
	const TBool directFb = (g_useDsa && g_dsaActive && g_fbOk);

	// Вписуємо джерело в екран зі збереженням пропорцій. Орієнтацію НЕ обертаємо
	// самі: апарат робить автоповорот вікна за акселерометром. У directFb цілимось
	// прямо у фізичний буфер LCD (розміри з HAL).
	const TInt SW = directFb ? g_fbW : g_scr.iWidth;
	const TInt SH = directFb ? g_fbH : g_scr.iHeight;

	// Черезрядкові режими (hi-res, sh>256) кладуть поля в сусідні рядки VRAM, тож
	// лінійне читання дає "гребінку". Беремо одне поле (парні рядки); пропорцію
	// рахуємо за повною висотою, бо на залізі поле розтягується на весь кадр.
	const TBool field = (sh > 256);
	const TInt srcRows = field ? sh / 2 : sh;

	// GL ES шлях (MBX): GPU масштабує й презентує (eglSwapBuffers). Замінює весь
	// bitmap+present нижче. rgb24 поки лишаємо на bitmap-шляху (не перевірено).
	if (g_glReady && !rgb24)
		{
		glPresentFrame(vbytes, startByte, sw, sh, field, srcRows);
		return;
		}

	TInt dstW = SW;
	TInt dstH = sh * SW / sw;
	if (dstH > SH) { dstH = SH; dstW = sw * SH / sh; }
	if (dstW < 1) dstW = 1;
	if (dstH < 1) dstH = 1;
	const TInt ox = (SW - dstW) / 2;
	const TInt oy = (SH - dstH) / 2;

	const TUint _fc0 = User::FastCounter();
	TUint16 *dst;
	TInt dstStride;
	if (directFb)
		{
		dst = g_fbBase;             // прямо в LCD - зливаємо fill і present в один прохід
		dstStride = g_fbStride;
		}
	else
		{
		g_bmp->LockHeap();
		dst = (TUint16 *)g_bmp->DataAddress();
		dstStride = g_bmp->DataStride() / 2;   // у 16-бітних словах
		}
	// Чорні поля лише коли геометрія змінилась: інакше вони вже чорні з минулого
	// кадру, а зображення однаково перезаписується. Прибирає memset ~150КБ/кадр.
	if (ox != g_blitOx || oy != g_blitOy || dstW != g_blitW || dstH != g_blitH)
		{
		Mem::FillZ(dst, dstStride * SH * 2);
		g_blitOx = ox; g_blitOy = oy; g_blitW = dstW; g_blitH = dstH;
		}

	// Nearest-neighbor через 16.16 фіксед-поінт: акумулятор замість ділення на
	// кожен піксель. ARM11 не має швидкого апаратного ділення, а профіль показав,
	// що блит з per-pixel діленням з'їдав ~половину часу кадру.
	const TInt srcH = field ? srcRows : sh;           // рядків-джерел для кроку
	const TUint sxStep = ((TUint)sw << 16) / (TUint)dstW;
	const TUint syStep = ((TUint)srcH << 16) / (TUint)dstH;
	for (TInt dy = 0; dy < dstH; dy++)
		{
		const TInt srcLine = (TInt)((syStep * (TUint)dy) >> 16);
		const TInt ssy = field ? srcLine * 2 : srcLine;
		TUint16 *drow = dst + (oy + dy) * dstStride + ox;
		TUint sxf = 0;
		if (rgb24)
			{
			// RGB24: 3 байти на піксель (R,G,B). Поки НЕ перевірено на залізі.
			const TUint8 *srow = vbytes + startByte + ssy * 2048;
			for (TInt dx = 0; dx < dstW; dx++)
				{
				const TUint8 *p = srow + (sxf >> 16) * 3;
				drow[dx] = (TUint16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
				sxf += sxStep;
				}
			}
		else
			{
			const TUint16 *srow = (const TUint16 *)(vbytes + startByte) + ssy * 1024;
			for (TInt dx = 0; dx < dstW; dx++)
				{
				drow[dx] = Bgr555ToRgb565(srow[sxf >> 16]);
				sxf += sxStep;
				}
			}
		}
	if (!directFb)
		g_bmp->UnlockHeap();
	const TUint _fc1 = User::FastCounter();
	g_blitFillTicks += (unsigned)(_fc1 - _fc0);

	if (directFb)
		{
		// Кадр уже в буфері екрана - present-копія не потрібна.
		}
	else if (g_useDsa && g_dsaActive && g_dsaGc)
		{
		g_dsaGc->BitBlt(TPoint(0, 0), g_bmp);
		CFbsScreenDevice *sd = g_dsa->ScreenDevice();
		if (sd) sd->Update();           // проштовхнути в буфер екрана
		}
	else if (g_ownWindow)
		{
		g_winp->Invalidate();
		g_winp->BeginRedraw();
		g_gc->Activate(*g_winp);
		g_gc->BitBlt(TPoint(0, 0), g_bmp);
		g_gc->Deactivate();
		g_winp->EndRedraw();
		g_wsp->Flush();
		}
	else
		{
		// Вікно CONE: малюємо поза парою BeginRedraw/EndRedraw. З Invalidate
		// фреймворк відповів би власним перемальовуванням (Draw() у контейнера
		// порожній) і чистив би щойно покладений кадр кольором фону.
		g_gc->Activate(*g_winp);
		g_gc->BitBlt(TPoint(0, 0), g_bmp);
		g_gc->Deactivate();
		g_wsp->Flush();
		}
	g_blitPresentTicks += (unsigned)(User::FastCounter() - _fc1);
	}

// Init/Present/Finish обгорнуті у TRAP: збій виведення не має валити прогін
// (числа з логу цінні самі по собі).
extern "C" int voutSymbianInit(void)
	{
	if (g_wsReady)
		return KErrNone;
	TInt err;
	TRAP(err, initL());
	return err;
	}

// Прив'язка до чужого вікна (AVKON-оболонка): малюємо у вікно CCoeControl і
// беремо сесію/пристрій/контекст у CCoeEnv. Заміна initL для GUI-гілки - власних
// RWsSession/RWindowGroup/RWindow не створюємо, тож і не закриваємо їх у Finish.
void voutSymbianAttachWindow(RWsSession &aWs, CWsScreenDevice &aDev, RWindow &aWin,
                             CWindowGc &aGc)
	{
	// Викликається ще й на зміну орієнтації, тож спершу знімаємо попередню прив'язку.
	if (g_eglDpy != EGL_NO_DISPLAY)
		glShutdown();

	g_ownWindow = EFalse;
	g_wsp = &aWs;
	g_winp = &aWin;
	g_dev = &aDev;
	g_gc = &aGc;
	g_scr = aWin.Size();

	delete g_bmp; g_bmp = NULL;
	g_bmp = new CFbsBitmap;
	if (!g_bmp || g_bmp->Create(g_scr, EColor64K) != KErrNone)
		{ delete g_bmp; g_bmp = NULL; return; }
	g_blitOx = -1;
	g_blitFillTicks = g_blitPresentTicks = 0;

	// Ввід приходить від CONE (OfferKeyEventL -> voutSymbianKeyEvent), тож черга
	// подій тут не озброюється - інакше два читачі одної сесії ділили б події.
	g_pad = 0;

	g_glReady = EFalse;
	if (g_useGl)
		{
		TRAPD(ge, { if (!glInitOnWindow()) glShutdown(); });
		if (ge != KErrNone) { glShutdown(); }
		}

	g_wsReady = ETrue;
	}

// Клавіша від CONE: оновлює g_pad (біти DKEY_*, active-high). Повертає 1, якщо
// код розкладено на кнопку PS1 (тоді контейнер поглинає подію).
extern "C" int voutSymbianKeyEvent(int aScanCode, int aDown)
	{
	const TInt bit = mapScanToDkey(aScanCode);
	if (bit < 0)
		return 0;
	if (aDown) g_pad |= (unsigned short)(1 << bit);
	else       g_pad &= (unsigned short)~(1 << bit);
	return 1;
	}

// Неблокуюче опитування клавіш: виснажує чергу подій вікон, оновлює g_pad
// (біти DKEY_*, active-high) на натисканнях/відпусканнях, повертає поточний стан.
// Без CActiveScheduler: перевіряємо озброєний TRequestStatus напряму.
extern "C" unsigned short voutSymbianPollPad(void)
	{
	if (!g_wsReady)
		return 0;
	if (!g_ownWindow)
		return g_pad;   // події роздає CONE, черга тут не озброєна
	while (g_evStat != KRequestPending)
		{
		TWsEvent ev;
		g_ws.GetEvent(ev);
		const TInt t = ev.Type();
		if (t == EEventKeyDown || t == EEventKeyUp)
			{
			const TInt bit = mapScanToDkey(ev.Key()->iScanCode);
			if (bit >= 0)
				{
				if (t == EEventKeyDown) g_pad |= (unsigned short)(1 << bit);
				else                    g_pad &= (unsigned short)~(1 << bit);
				}
			}
		g_ws.EventReady(&g_evStat);
		}
	return g_pad;
	}

extern "C" void voutSymbianPresent(void)
	{
	if (!g_wsReady)
		return;
	// Оновити g_vout поточним кадром напряму (gpulib робить це рідко), тоді блит.
	vout_update(&gpu, gpu.screen.src_x, gpu.screen.src_y);
	TRAP_IGNORE(blitCurrent());
	}

extern "C" int voutSymbianFinish(int holdMs)
	{
	if (!g_wsReady)
		return KErrNotReady;
	if (holdMs < 0) holdMs = 0;
	if (holdMs > 30000) holdMs = 30000;
	if (holdMs > 0 && g_ownWindow)
		User::After(holdMs * 1000);
	if (g_ownWindow)
		g_ws.EventReadyCancel();   // знімаємо озброєний запит подій перед закриттям
	// GL тримає EGL-поверхню на вікні - знімаємо перед закриттям вікна.
	if (g_eglDpy != EGL_NO_DISPLAY) glShutdown();
	// DSA тримає вікно/пристрій - знімаємо його першим.
	if (g_dsa) { delete g_dsa; g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; }
	delete g_bmp; g_bmp = NULL;
	if (g_ownWindow)
		{
		delete g_gc;  g_gc = NULL;
		g_win.Close();
		g_grp.Close();
		delete g_dev; g_dev = NULL;
		g_ws.Close();
		}
	else
		{
		// Сесія, пристрій, контекст і вікно належать CCoeEnv/контейнеру - лише
		// відпускаємо покажчики й повертаємось до власної (консольної) розкладки.
		g_gc = NULL; g_dev = NULL;
		g_ownWindow = ETrue;
		g_wsp = &g_ws;
		g_winp = &g_win;
		}
	// Планувальник прибираємо лише якщо його створив саме DSA (інакше він аудіо-,
	// і його зніме ps1SndCloseOutput).
	if (g_dsaSched) { CActiveScheduler::Install(NULL); delete g_dsaSched; g_dsaSched = NULL; }
	g_wsReady = EFalse;
	return KErrNone;
	}

// Одноразовий показ (шлях gputest: статична картинка): init + один present + hold.
extern "C" int voutSymbianShow(int holdMs)
	{
	TInt r = voutSymbianInit();
	if (r != KErrNone)
		return r;
	voutSymbianPresent();
	return voutSymbianFinish(holdMs);
	}

// Увімкнути DSA (прямий доступ до екрана) для present. Викликати ДО voutSymbianInit.
extern "C" void voutSymbianSetDsa(int aOn)
	{
	g_useDsa = aOn ? ETrue : EFalse;
	}

// Увімкнути GL ES вивід (MBX). Викликати ДО voutSymbianInit.
extern "C" void voutSymbianSetGl(int aOn)
	{
	g_useGl = aOn ? ETrue : EFalse;
	}

// Чи справді GL активний (для логу): 1 = present іде через GPU/EGL.
extern "C" int voutSymbianGlActive(void)
	{
	return g_glReady ? 1 : 0;
	}

// Чи справді DSA активний (для логу): 1 = present іде через DSA.
extern "C" int voutSymbianDsaActive(void)
	{
	return (g_useDsa && g_dsaActive) ? 1 : 0;
	}

// Діагностичний тест-патерн прямо в буфер LCD: синій фон, білі рамки, різнокольорові
// кути (ЧЕР=ЛВ, ЗЕЛ=ПВ, СИН-ярк=ЛН, ЖОВ=ПН) і діагональ. За тим, що видно, точно
// визначаємо проблему direct-FB: зсув=stride, порожньо=подвійний буфер, дзеркало=орієнтація.
extern "C" int voutSymbianFbTest(void)
	{
	if (!g_fbOk || !g_fbBase) return -1;
	const TInt W = g_fbW, H = g_fbH, S = g_fbStride;
	TUint16 *fb = g_fbBase;
	const TUint16 BLUE = 0x001f, WHITE = 0xffff, RED = 0xf800, GREEN = 0x07e0, YELLOW = 0xffe0, CYAN = 0x07ff;
	for (TInt y = 0; y < H; y++)
		{
		TUint16 *row = fb + y * S;
		for (TInt x = 0; x < W; x++)
			{
			TUint16 c = BLUE;
			if (x < 2 || x >= W - 2 || y < 2 || y >= H - 2) c = WHITE;      // рамка
			else if (x < 40 && y < 40) c = RED;                            // ЛВ кут
			else if (x >= W - 40 && y < 40) c = GREEN;                     // ПВ кут
			else if (x < 40 && y >= H - 40) c = CYAN;                      // ЛН кут
			else if (x >= W - 40 && y >= H - 40) c = YELLOW;               // ПН кут
			else if (x == y) c = WHITE;                                    // діагональ
			row[x] = c;
			}
		}
	if (g_dsa && g_dsaActive)
		{ CFbsScreenDevice *sd = g_dsa->ScreenDevice(); if (sd) sd->Update(); }
	return 0;
	}

// Сирі HAL-параметри буфера екрана + розмір вікна з WS (для діагностики direct-FB).
extern "C" void voutSymbianFbInfo(int *aOk, int *aXpix, int *aYpix, int *aLineBytes,
                                  int *aBpp, int *aFirstOff, int *aWinW, int *aWinH)
	{
	if (aOk)        *aOk        = g_fbOk ? 1 : 0;
	if (aXpix)      *aXpix      = g_fbXpix;
	if (aYpix)      *aYpix      = g_fbYpix;
	if (aLineBytes) *aLineBytes = g_fbLineBytes;
	if (aBpp)       *aBpp       = g_fbBpp;
	if (aFirstOff)  *aFirstOff  = g_fbFirstOff;
	if (aWinW)      *aWinW      = g_scr.iWidth;
	if (aWinH)      *aWinH      = g_scr.iHeight;
	}

// Поділ часу блиту в мс: fill = цикл масштабування, present = BitBlt+Flush.
extern "C" void voutSymbianBlitSplit(unsigned *aFillMs, unsigned *aPresentMs)
	{
	const unsigned f = ndrc_symbian_fastfreq();
	const unsigned freq = f ? f : 1;
	if (aFillMs)    *aFillMs    = (unsigned)(g_blitFillTicks * 1000ULL / freq);
	if (aPresentMs) *aPresentMs = (unsigned)(g_blitPresentTicks * 1000ULL / freq);
	}

// --- Обгортки для libpcsxcore -----------------------------------------------
// libpcsxcore/plugins.h оголошує GPUinit, GPUopen тощо як ТИПИ, а в gpulib це
// функції з тими самими іменами. Транслювати обидва заголовки в одному файлі
// не можна, тож перейменовуємо тут, де plugins.h не потрібен.

extern "C" {

// Діагностика GPU-тракту: скільки команд BIOS реально шле. Показує, чи проблема
// в доставці команд (лічильники нульові) чи в растеризації (ненульові, але VRAM
// чорний). Скидаються в ps1GpuInit (раз на прохід).
static unsigned g_gpData, g_gpDataMem, g_gpDma, g_gpStatus;

long ps1GpuInit(void)
	{
	g_gpData = g_gpDataMem = g_gpDma = g_gpStatus = 0;
	const long r = GPUinit();
	// Пропуск непарних ліній при растеризації черезрядкового (hi-res) контенту:
	// blitCurrent і так показує лише парне поле, тож малювати непарні - марно.
	// Умовно на PSX_GPU_STATUS_INTERLACE (gpulib_if.cpp:116), тож 240p не страждає.
	// Ставимо ПІСЛЯ GPUinit: він робить memset(&gpu.state).
	gpu.state.downscale_enable = 1;
	return r;
	}
long ps1GpuShutdown(void)                               { return GPUshutdown(); }
long ps1GpuOpen(unsigned long *d, char *c, char *g)     { return GPUopen(d, c, g); }
long ps1GpuClose(void)                                  { return GPUclose(); }
void ps1GpuWriteStatus(uint32_t d)                      { g_gpStatus++; GPUwriteStatus(d); }
void ps1GpuWriteData(uint32_t d)                        { g_gpData++; GPUwriteData(d); }
void ps1GpuWriteDataMem(uint32_t *m, int c)             { g_gpDataMem++; GPUwriteDataMem(m, c); }
uint32_t ps1GpuReadStatus(void)                         { return GPUreadStatus(); }
uint32_t ps1GpuReadData(void)                           { return GPUreadData(); }
void ps1GpuReadDataMem(uint32_t *m, int c)              { GPUreadDataMem(m, c); }
long ps1GpuDmaChain(uint32_t *rb, uint32_t sa, uint32_t *pa, int32_t *cy)
                                                        { g_gpDma++; return GPUdmaChain(rb, sa, pa, cy); }

void voutSymbianGpuStats(unsigned *aData, unsigned *aDataMem, unsigned *aDma, unsigned *aStatus)
	{
	if (aData)    *aData = g_gpData;
	if (aDataMem) *aDataMem = g_gpDataMem;
	if (aDma)     *aDma = g_gpDma;
	if (aStatus)  *aStatus = g_gpStatus;
	}
long ps1GpuFreeze(uint32_t t, struct GPUFreeze *f, uint16_t **v) { return GPUfreeze(t, f, v); }
void ps1GpuVBlank(int v, int lcf)                       { GPUvBlank(v, lcf); }
void ps1GpuGetScreenInfo(int *y, int *hres)             { GPUgetScreenInfo(y, hres); }

// Мінімальний набір: gpulib читає лічильники напряму (gpu.c:1218-1264), решта
// полів - нулі. thread_rendering = 0, бо USE_ASYNC_GPU не задано.
void ps1GpuSetCallbacks(unsigned int *frameCount, unsigned int *hcnt)
	{
	static struct rearmed_cbs cbs;
	memset(&cbs, 0, sizeof(cbs));
	cbs.gpu_frame_count = frameCount;
	cbs.gpu_hcnt = hcnt;
	cbs.thread_rendering = 0;
	GPUrearmedCallbacks(&cbs);
	}

} // extern "C"
