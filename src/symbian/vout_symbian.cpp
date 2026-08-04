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

// Дублювати парний рядок VRAM замість застарілого непарного (режим "Skip lines"
// растеризатора взагалі не переписує непарні рядки). Вмикає stage1 разом із хаком.
static int g_ilaceOut;
// Виставляється у vout_update, скидається після показу: чи є кадр, якого ще не показали.
// Одиниця на старті - перший кадр показати треба до будь-якого малювання.
static int g_voutPending = 1;
// Скільки разів САМЕ gpulib повідомив про новий кадр (Z2): частка від числа кадрів
// показує стелю виграшу від показу за латчем.
static int g_voutGpulib;
// Ознака нашого власного виклику vout_update, щоб не зводити ним латч.
static int g_voutSelfCall;
// Скільки кадрів поспіль показ пропущено через відсутність нового вмісту.
static int g_voutStale;
// Область, яку реально показали востаннє: зміна цих значень означає, що гра
// перемкнула буфер, навіть якщо нічого не малювала.
static int g_lastShownOffset = -1, g_lastShownW = -1, g_lastShownH = -1;
// Показувати лише за латчем замість безумовного показу щокадру (прапорець "dirty").
static TBool g_dirtyOnly;

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
	// Латч "є новий кадр": gpulib кличе vout_update лише коли область показу
	// справді змінилась (GPUupdateLace виходить достроково без fb_dirty_display_area).
	// Наш власний виклик із voutSymbianPresent латч не зводить і в лічильник не йде -
	// інакше Z2 показував би просто кількість кадрів.
	if (!g_voutSelfCall)
		{
		g_voutPending = 1;
		g_voutGpulib++;
		}

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


// Z2: скільки кадрів gpulib вважав новими. Менше за число кадрів рівно настільки,
// наскільки показ за латчем може зекономити.
extern "C" int voutSymbianGpulibUpdates(void)
	{
	return g_voutGpulib;
	}

// Діагностика застиглого екрана. Розрізняє два випадки, які ззовні однакові:
// (а) гра нічого не малює й не гортає буфери - завмирання КОРЕКТНЕ;
// (б) гра малює або міняє адресу початку кадру, а ми цього не показуємо - ВАДА.
// Для (б) вирішальні саме fb_dirty і поточні gpu.screen проти нашого g_vout.
extern "C" void voutSymbianDbgState(int *aFbDirty, int *aFbDirtyArea, int *aStatus,
                                    int *aSrcX, int *aSrcY, int *aScrW, int *aScrH,
                                    int *aVoutOffset, int *aStale)
	{
	if (aFbDirty)     *aFbDirty     = gpu.state.fb_dirty;
	if (aFbDirtyArea) *aFbDirtyArea = gpu.state.fb_dirty_display_area;
	if (aStatus)      *aStatus      = (int)gpu.status;
	if (aSrcX)        *aSrcX        = gpu.screen.src_x;
	if (aSrcY)        *aSrcY        = gpu.screen.src_y;
	if (aScrW)        *aScrW        = gpu.screen.w;
	if (aScrH)        *aScrH        = gpu.screen.h;
	if (aVoutOffset)  *aVoutOffset  = g_vout.offset;
	if (aStale)       *aStale       = g_voutStale;
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

// DSA (Direct Screen Access) + direct-FB: кадр масштабується й конвертується
// одразу в буфер екрана, тобто fill і present зливаються в один прохід.
// СТАТУС (2026-08-04): ПРАЦЮЄ, типово увімкнено, -0,93 мс на кадр (30,13 проти
// 31,06 з освітленням і напівпрозорістю). Вимикається пунктом меню "Direct draw"
// або відсутністю аргументу "dsa" в прогонах з командного рядка.
//
// Ламався він через ОДНЕ: sd->Update() без аргументу нічого не проштовхував.
// Ми пишемо повз bitgdi, тож його власна "брудна" область лишається порожньою.
// Оновлення треба просити з явним регіоном - беремо DrawingRegion() самого DSA.
//
// Діагноз 2026-08-01 ("HAL віддає повернуту геометрію", "WS композитить поверх")
// не підтвердився ЖОДНОЮ частиною: буфер один (halAddr == Bits() == 0xcb400000),
// сторінка одна (fbPages=0/1 за 1500 кадрів), а розкладка драйвера правильна -
// візерунок, покладений bitgdi, читається назад сирим покажчиком 64/64.
// Той зонд виконувався в консольній гілці, а гра йде в GUI - звідси хибний висновок.
//
// rgb24 у цей шлях не пускаємо (гілка пише 16-бітними словами й не перевірена),
// тож 24-бітні заставки й відео йдуть віконним шляхом. Саме через це вада колись
// читалась як "відео видно, а 2D - ні".
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
static TInt     g_fbStride;  // 16-бітних слів на рядок (сумісність зі старим кодом)
static TInt     g_fbStrideB; // байтів на рядок - єдина достовірна величина
static TInt     g_fbPixBytes;// байтів на піксель буфера екрана: 2 (565) або 4 (XRGB8888)
static TInt     g_fbMode;    // TDisplayMode від платформи, -1 = невідомо
static TInt     g_fbW, g_fbH;
// Сирі HAL-значення (для діагностики "двох вікон" - stride/розміри/буфер).
static TInt g_fbAddr, g_fbBpp, g_fbLineBytes, g_fbFirstOff, g_fbXpix, g_fbYpix;
// Базу й крок віддала платформа (CFbsScreenDevice), а не HAL.
static TBool g_fbFromPlatform;
// Present порядковим Mem::Copy у буфер екрана замість CWindowGc/CFbsBitGc::BitBlt.
// Кадр так само конвертується в бітмап (відомо коректний шлях), а на екран іде
// простою копією рядків - без механіки bitgdi, яка й коштує більшість present.
static TBool g_useRowCopy;

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
extern "C" int voutSymbianScreenDump(const char *aPath);   // визначений нижче

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
	g_fbBase     = (TUint16 *)((TUint8 *)g_fbAddr + g_fbFirstOff);
	g_fbStrideB  = g_fbLineBytes;
	g_fbPixBytes = 2;
	g_fbW = g_fbXpix; g_fbH = g_fbYpix;
	g_fbFromPlatform = EFalse;
	g_fbMode = -1;

	// Розкладку беремо ВІД ПЛАТФОРМИ, коли вона її віддає: Stride() - це
	// ScanLineBytes() драйвера, тобто те, чим малює сам bitgdi, а Bits() - початок
	// першого пікселя. HAL-значення лишаються запасним варіантом. Попередня спроба
	// direct-FB спиралася лише на HAL і давала перекошений кадр; це найдешевша
	// відповідь на питання, чи розкладки взагалі різні.
	CFbsScreenDevice *sd = g_dsa ? g_dsa->ScreenDevice() : NULL;
	if (sd)
		{
		const TUint32 *bits = sd->Bits();
		const TInt strideB  = sd->Stride();
		const TSize sz      = sd->SizeInPixels();
		const TDisplayMode dm = sd->DisplayMode();
		const TInt pix = (dm == EColor64K) ? 2 : ((dm == EColor16MU || dm == EColor16MA) ? 4 : 0);
		if (bits && pix && strideB >= sz.iWidth * pix && sz.iWidth > 0 && sz.iHeight > 0)
			{
			g_fbBase     = (TUint16 *)bits;
			g_fbStrideB  = strideB;
			g_fbPixBytes = pix;
			g_fbW = sz.iWidth; g_fbH = sz.iHeight;
			g_fbFromPlatform = ETrue;
			g_fbMode = (TInt)dm;
			}
		}
	g_fbStride = g_fbStrideB / 2;
	g_fbOk = ETrue;
	}

// Скільки разів драйвер віддав іншу адресу першого пікселя і які саме адреси.
// Якщо їх більше однієї - у драйвера дві сторінки, і запис у кешовану адресу
// лягає в ту, яку панель саме зараз не сканує. Це рівно та картина, яку описав
// власник апарата: частина старого кадру плюс артефакти в окремих ділянках.
static TInt   g_fbBaseChanges;
static TInt   g_fbBaseSeen;
static TUint32 g_fbBaseList[4];

// Перечитати розкладку в драйвера. Дешево (акцесори), тож можна щокадру.
static void refreshFbFromPlatform()
	{
	CFbsScreenDevice *sd = (g_dsa && g_dsaActive) ? g_dsa->ScreenDevice() : NULL;
	if (!sd)
		return;
	const TUint32 *bits = sd->Bits();
	if (!bits)
		return;
	const TUint32 b = (TUint32)bits;
	TInt i;
	for (i = 0; i < g_fbBaseSeen; i++)
		if (g_fbBaseList[i] == b)
			break;
	if (i == g_fbBaseSeen && g_fbBaseSeen < 4)
		g_fbBaseList[g_fbBaseSeen++] = b;
	if ((TUint32)g_fbBase != b)
		{
		g_fbBaseChanges++;
		g_fbBase    = (TUint16 *)bits;
		g_fbStrideB = sd->Stride();
		g_fbStride  = g_fbStrideB / 2;
		}
	}

extern "C" void voutSymbianFbPages(int *aChanges, int *aSeen, int *aB0, int *aB1)
	{
	if (aChanges) *aChanges = g_fbBaseChanges;
	if (aSeen)    *aSeen    = g_fbBaseSeen;
	if (aB0)      *aB0      = (int)(g_fbBaseSeen > 0 ? g_fbBaseList[0] : 0);
	if (aB1)      *aB1      = (int)(g_fbBaseSeen > 1 ? g_fbBaseList[1] : 0);
	}

static void queryFramebuffer();

// Чи віддав нам WS увесь екран. CFbsBitGc сам поважає регіон відсікання, а сирий
// запис і порядкова копія - ні: вони пишуть увесь прямокутник. Якщо поверх з'явився
// системний діалог, регіон звужується, і без цієї перевірки ми б його замалювали.
static TBool dsaCoversScreen()
	{
	if (!g_dsa)
		return EFalse;
	const RRegion *rgn = g_dsa->DrawingRegion();
	if (!rgn || rgn->Count() != 1)
		return EFalse;
	const TRect r = rgn->BoundingRect();
	return (r.iTl.iX == 0 && r.iTl.iY == 0 &&
	        r.Width() >= g_scr.iWidth && r.Height() >= g_scr.iHeight);
	}

static void dsaStartL()
	{
	g_dsa->StartL();
	g_dsaGc = g_dsa->Gc();
	g_dsaGc->SetClippingRegion(g_dsa->DrawingRegion());
	// Геометрію перезапитуємо на КОЖЕН старт, не раз у initL: після
	// ETerminateRotation режим екрана інший, а стара база/крок дали б перекошений
	// кадр - самостійна причина "зсуву", яку 2026-08-01 не відокремили.
	queryFramebuffer();
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

// Гарячий цикл конвертації: два пікселі за 32-бітне слово. Винесений в окрему
// функцію НАВМИСНО - усередині blitCurrent забагато живих змінних, і GCCE 3.4.3
// витісняв маски на стек, перезавантажуючи їх на кожній ітерації (15 інструкцій
// замість 11). Тут розподільник регістрів має вільні руки.
//
// Порожня asm-вставка робить константи непрозорими для оптимізатора: інакше
// компілятор синтезує кожну 32-бітну маску ланцюжком із чотирьох BIC прямо в
// циклі - 16 із 28 інструкцій на два пікселі (перевірено дизасемблером).
//
// __builtin_prefetch відкинуто в ЦЬОМУ розміщенні: одне PLD на два пікселі - це
// вісім звернень на лінію кешу. Заміряні +3,7% дорівнюють вартості однієї зайвої
// інструкції на 28, тобто користі не було. Один PLD на 32 байти не перевірявся.
// Форма з підкресленнями обов'язкова: у заголовках уже є макрос noinline.
static void __attribute__((__noinline__))
blitRow565(TUint32 *aDst, const TUint32 *aSrc, TInt aPairs)
	{
	TUint32 cRB = 0x001f001fu, cG54 = 0x07c007c0u, cG0 = 0x00200020u;
	__asm__ __volatile__ ("" : "+r"(cRB), "+r"(cG54), "+r"(cG0));
	do
		{
		const TUint32 p = *aSrc++;
		*aDst++ = ((p & cRB) << 11) | ((p << 1) & cG54)
		        | ((p >> 4) & cG0) | ((p >> 10) & cRB);
		} while (--aPairs);
	}

// Те саме для 32-бітного буфера екрана (EColor16MU/16MA, XRGB8888).
//
// ЗАМІРЯНО зондом 2026-08-03: платформа віддає stride=1280B при 320 пікселях і
// DisplayMode=EColor16MU, тобто буфер екрана 32-бітний, тоді як HAL повідомляє
// bpp=16 і line=640B. Саме на HAL-числа спиралась спроба direct-FB 2026-08-01 -
// звідси й був "зсув": ми писали 16-бітні пікселі з кроком 640 у 32-бітний буфер
// з кроком 1280. Тут конвертуємо одразу в цільовий формат, тож зникає і проміжний
// бітмап, і перетворення 565->8888 усередині bitgdi (а воно і є більшість present).
//
// 5 біт -> 8 робимо як (v<<3)|(v>>2): це множення на 255/31 із точністю до одиниці,
// без таблиць і без ділення.
static void __attribute__((__noinline__))
blitRow8888(TUint32 *aDst, const TUint16 *aSrc, TInt aPixels)
	{
	TUint32 m5 = 0x1fu;
	__asm__ __volatile__ ("" : "+r"(m5));
	do
		{
		const TUint32 p = *aSrc++;
		const TUint32 r = p & m5;
		const TUint32 g = (p >> 5) & m5;
		const TUint32 b = (p >> 10) & m5;
		*aDst++ = 0xff000000u
		        | (((r << 3) | (r >> 2)) << 16)
		        | (((g << 3) | (g >> 2)) << 8)
		        |  ((b << 3) | (b >> 2));
		} while (--aPixels);
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

static inline TUint32 Bgr555ToXrgb8888(TUint16 aPix)
	{
	const TUint32 r = aPix & 0x1f;
	const TUint32 g = (aPix >> 5) & 0x1f;
	const TUint32 b = (aPix >> 10) & 0x1f;
	return 0xff000000u | (((r << 3) | (r >> 2)) << 16)
	                   | (((g << 3) | (g >> 2)) << 8)
	                   |  ((b << 3) | (b >> 2));
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
	// Під DSA/direct-FB заливка фону і redraw store - це право WS перемалювати наше
	// вікно поверх кадру, який ми поклали в екран повз нього. SetBackgroundColor()
	// БЕЗ аргументу означає "не чистити"; це виправлення коректності, не швидкодії
	// (заміряно, що оверхед WS тут нульовий).
	if (g_useDsa)
		{
		g_win.SetBackgroundColor();
		g_win.EnableRedrawStore(EFalse);
		g_win.SetNonFading(ETrue);
		}
	else
		g_win.SetBackgroundColor(KRgbBlack);
	g_win.Activate();
	if (g_useDsa)
		{
		// Валідувати вікно ОДИН раз до захоплення екрана: Activate лишає вікно
		// недійсним, і відкладений server-initiated redraw спрацював би вже після
		// StartL, затерши перший кадр.
		g_win.Invalidate();
		g_win.BeginRedraw();
		g_win.EndRedraw();
		g_ws.Flush();
		}
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
		// Геометрію під direct-FB запитує сам dsaStartL - і на кожному рестарті.
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

// --- Накладка з лічильником кадрів -------------------------------------------

// Шрифт 5x7, лише ті знаки, що трапляються в рядку метрик. Малюємо самі, а не
// текстом bitgdi: під direct-FB ми пишемо в буфер екрана повз нього, тож його
// текст ліг би в інший буфер, а розділити ці два шляхи довелось би однаково.
static const char KOvlChars[] = "0123456789. fpsm";
static const TUint8 KOvlFont[16][7] =
	{
	{ 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E },   // 0
	{ 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E },   // 1
	{ 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F },   // 2
	{ 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E },   // 3
	{ 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 },   // 4
	{ 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E },   // 5
	{ 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E },   // 6
	{ 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 },   // 7
	{ 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E },   // 8
	{ 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C },   // 9
	{ 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C },   // .
	{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00 },   // пробіл
	{ 0x06,0x09,0x08,0x1C,0x08,0x08,0x08 },   // f
	{ 0x00,0x00,0x1E,0x11,0x1E,0x10,0x10 },   // p
	{ 0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E },   // s
	{ 0x00,0x00,0x1A,0x15,0x15,0x15,0x15 }    // m
	};

static const TInt KOvlScale = 2;    // 5x7 на 320x240 без збільшення не читається
static const TInt KOvlCellW = 6 * KOvlScale;
static const TInt KOvlCellH = 8 * KOvlScale;
static const TInt KOvlCols  = 9;    // "999.9 fps" - найдовший з рядків
static const TInt KOvlPad   = 2;

static TBool g_noUpdate;    // зонд: пропустити Update() екрана (див. сетер нижче)

static TInt g_ovlOn;
static TInt g_ovlFpsX10 = -1;
static TInt g_ovlMsX10;

// Показувати накладку. Значення дає stage1: він рахує КАДРИ ЕМУЛЯЦІЇ, а сюди
// доходить лише показаний кадр, тож лічити тут означало б міряти інше число.
extern "C" void voutSymbianSetOverlay(int aOn)
	{
	g_ovlOn = aOn ? 1 : 0;
	}

extern "C" void voutSymbianSetStats(int aFpsX10, int aMsX10)
	{
	g_ovlFpsX10 = aFpsX10;
	g_ovlMsX10 = aMsX10;
	}

static void ovlFillRect(TUint8 *aDst, TInt aStrideB, TInt aPixB, TInt aX, TInt aY,
                        TInt aW, TInt aH, TUint32 aCol)
	{
	for (TInt y = 0; y < aH; y++)
		{
		TUint8 *row = aDst + (aY + y) * aStrideB + aX * aPixB;
		if (aPixB == 4)
			{
			TUint32 *d = (TUint32 *)row;
			for (TInt x = 0; x < aW; x++) d[x] = aCol;
			}
		else
			{
			TUint16 *d = (TUint16 *)row;
			for (TInt x = 0; x < aW; x++) d[x] = (TUint16)aCol;
			}
		}
	}

static void ovlText(TUint8 *aDst, TInt aStrideB, TInt aPixB, TInt aX, TInt aY,
                    const char *aStr, TUint32 aCol)
	{
	for (; *aStr; aStr++, aX += KOvlCellW)
		{
		const char *p = strchr(KOvlChars, *aStr);
		if (!p)
			continue;   // невідомий знак лишає порожню клітинку
		const TUint8 *g = KOvlFont[p - KOvlChars];
		for (TInt gy = 0; gy < 7; gy++)
			{
			const TUint8 bits = g[gy];
			if (!bits)
				continue;
			for (TInt gx = 0; gx < 5; gx++)
				if (bits & (0x10 >> gx))
					ovlFillRect(aDst, aStrideB, aPixB, aX + gx * KOvlScale,
						aY + gy * KOvlScale, KOvlScale, KOvlScale, aCol);
			}
		}
	}

// Рядок метрик у лівому верхньому куті приймача. Кліп по розміру екрана
// обов'язковий: під direct-FB приймач - буфер LCD, і запис повз нього пішов би
// у чужу пам'ять. Підкладка суцільна, бо чорні поля перемальовуються лише при
// зміні геометрії - без неї попередні цифри лишались би на місці.
static void drawOverlay(TUint8 *aDst, TInt aStrideB, TInt aPixB, TInt aSW, TInt aSH)
	{
	if (!g_ovlOn || g_ovlFpsX10 < 0)
		return;
	const TInt boxW = KOvlCols * KOvlCellW + 2 * KOvlPad;
	const TInt boxH = 2 * KOvlCellH + 2 * KOvlPad;
	if (aSW < boxW || aSH < boxH)
		return;

	char l1[16], l2[16];
	snprintf(l1, sizeof(l1), "%d.%d fps", g_ovlFpsX10 / 10, g_ovlFpsX10 % 10);
	snprintf(l2, sizeof(l2), "%d.%d ms", g_ovlMsX10 / 10, g_ovlMsX10 % 10);

	const TUint32 white = (aPixB == 4) ? 0x00ffffffu : 0xffffu;
	ovlFillRect(aDst, aStrideB, aPixB, 0, 0, boxW, boxH, 0);
	ovlText(aDst, aStrideB, aPixB, KOvlPad, KOvlPad, l1, white);
	ovlText(aDst, aStrideB, aPixB, KOvlPad, KOvlPad + KOvlCellH, l2, white);
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
	// rgb24 у прямий буфер не пускаємо: та гілка пише 16-бітними словами й не
	// перевірена на залізі, а помилка тут - запис повз буфер екрана.
	const TBool directFb = (g_useDsa && g_dsaActive && g_fbOk && !g_useRowCopy &&
	                        !rgb24 && dsaCoversScreen());

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
	// Накладки тут немає: приймач - текстура GL, а не лінійний буфер. Шлях і так
	// вчетверо повільніший за блит, тож дублювати заради нього шрифт нема сенсу.
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
	// Адресуємо рядки в БАЙТАХ: буфер екрана може бути 32-бітним (заміряно:
	// EColor16MU, stride 1280B при 320 пікселях), а бітмап у нас 16-бітний.
	TUint8 *dstB;
	TInt dstStrideB, pixB;
	if (directFb)
		{
		refreshFbFromPlatform();    // драйвер може міняти сторінку між кадрами
		dstB = (TUint8 *)g_fbBase;  // прямо в LCD - зливаємо fill і present в один прохід
		dstStrideB = g_fbStrideB;
		pixB = g_fbPixBytes;
		}
	else
		{
		g_bmp->LockHeap();
		dstB = (TUint8 *)g_bmp->DataAddress();
		dstStrideB = g_bmp->DataStride();
		pixB = 2;
		}
	// Чорні поля лише коли геометрія змінилась: інакше вони вже чорні з минулого
	// кадру, а зображення однаково перезаписується. Прибирає memset ~150КБ/кадр.
	if (ox != g_blitOx || oy != g_blitOy || dstW != g_blitW || dstH != g_blitH)
		{
		Mem::FillZ(dstB, dstStrideB * SH);
		g_blitOx = ox; g_blitOy = oy; g_blitW = dstW; g_blitH = dstH;
		}

	// Nearest-neighbor через 16.16 фіксед-поінт: акумулятор замість ділення на
	// кожен піксель. ARM11 не має швидкого апаратного ділення, а профіль показав,
	// що блит з per-pixel діленням з'їдав ~половину часу кадру.
	const TInt srcH = field ? srcRows : sh;           // рядків-джерел для кроку
	const TUint sxStep = ((TUint)sw << 16) / (TUint)dstW;
	const TUint syStep = ((TUint)srcH << 16) / (TUint)dstH;

	// Попередній рядок-джерело і куди його поклали: коли наступний рядок приймача
	// читає те саме джерело (масштабування вгору або пропуск рядків), копія готового
	// рядка дешевша за повторну конвертацію.
	TInt prevSsy = -1;
	const TUint16 *prevDrow = NULL;

	for (TInt dy = 0; dy < dstH; dy++)
		{
		const TInt srcLine = (TInt)((syStep * (TUint)dy) >> 16);
		TInt ssy = field ? srcLine * 2 : srcLine;
		// Растеризатор із "Skip lines" не переписує непарні рядки VRAM ніколи, тож
		// вони тримають вміст попередніх кадрів (статичні накладки лишали слід).
		// Беремо парний рядок замість застарілого непарного.
		if (g_ilaceOut && !field)
			ssy &= ~1;
		TUint16 *drow = (TUint16 *)(dstB + (oy + dy) * dstStrideB + ox * pixB);
		// Копіюємо готовий рядок лише в бітмап. При direct-FB приймач - буфер екрана,
		// і читання з нього не заміряне: заміряно тільки лінійний ЗАПИС. Некешоване
		// читання коштувало б повну латентність пам'яті, тож там конвертуємо заново.
		if (ssy == prevSsy && prevDrow && !directFb)
			{
			Mem::Copy(drow, prevDrow, dstW * pixB);
			continue;
			}
		prevSsy = ssy; prevDrow = drow;
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
			// 32-бітний буфер екрана: конвертуємо прямо в XRGB8888, без проміжного
			// 16-бітного бітмапа і без перетворення 565->8888 у bitgdi.
			if (pixB == 4)
				{
				TUint32 *d32 = (TUint32 *)drow;
				if (sxStep == 0x10000u)
					blitRow8888(d32, srow, dstW);
				else
					{
					for (TInt dx = 0; dx < dstW; dx++)
						{
						d32[dx] = Bgr555ToXrgb8888(srow[sxf >> 16]);
						sxf += sxStep;
						}
					}
				}
			// Масштаб 1:1 по горизонталі - переважний випадок: вихід PS1 (256 або
			// 320) після вписування в 320x240 дає рівно крок 0x10000. Тоді ні
			// акумулятор, ні зсув не потрібні, і два пікселі конвертуються разом
			// у 32-бітному регістрі: удвічі менше звернень до пам'яті і вдвічі
			// менше операцій на піксель.
			else if (sxStep == 0x10000u &&
				!(((TUint)srow | (TUint)drow) & 3) && !(dstW & 1))
				{
				// dstW парний і >=2 (умова гілки), тож лічильник гарантовано >=1.
				blitRow565((TUint32 *)drow, (const TUint32 *)srow, dstW >> 1);
				}
			else if (sxStep == 0x10000u)
				{
				for (TInt dx = 0; dx < dstW; dx++)
					drow[dx] = Bgr555ToRgb565(srow[dx]);
				}
			else
				{
				for (TInt dx = 0; dx < dstW; dx++)
					{
					drow[dx] = Bgr555ToRgb565(srow[sxf >> 16]);
					sxf += sxStep;
					}
				}
			}
		}
	// Після конвертації, до present: накладка має лягти поверх кадру, а не бути
	// затертою ним. Ціна входить у виміряний час блиту навмисно - вона реальна.
	drawOverlay(dstB, dstStrideB, pixB, SW, SH);
	if (!directFb)
		g_bmp->UnlockHeap();
	const TUint _fc1 = User::FastCounter();
	g_blitFillTicks += (unsigned)(_fc1 - _fc0);

	if (directFb)
		{
		// Кадр уже в буфері екрана - present-копія не потрібна. Оновлення просимо
		// з ЯВНИМ регіоном: ми писали повз bitgdi, тож його власна "брудна" область
		// лишається порожньою і Update() без аргументу може не проштовхнути нічого.
		CFbsScreenDevice *sd = g_noUpdate ? NULL : (g_dsa ? g_dsa->ScreenDevice() : NULL);
		if (sd)
			{
			const RRegion *rgn = g_dsa->DrawingRegion();
			if (rgn && !rgn->CheckError()) sd->Update(*rgn);
			else                           sd->Update();
			}
		}
	// Порядкова копія має сенс лише коли формат бітмапа й екрана збігається.
	// На цьому апараті екран 32-бітний, тож шлях неактивний - лишається як варіант
	// для апаратів із 16-бітним буфером.
	else if (g_useRowCopy && g_dsaActive && g_fbOk && g_fbPixBytes == 2 &&
	         dsaCoversScreen())
		{
		// Порядкова копія бітмапа в буфер екрана. Заміряно, що клієнт-серверна
		// механіка WS коштує ~нуль (наш власний bitgdi-блит без IPC вийшов ДОРОЖЧИМ
		// за віконний шлях), тобто present - це вартість самої процедури копіювання
		// в bitgdi. Плоский Mem::Copy робить ту саму роботу без неї.
		g_bmp->LockHeap();
		const TUint8 *src = (const TUint8 *)g_bmp->DataAddress();
		const TInt sstride = g_bmp->DataStride();
		const TInt rows  = SH < g_fbH ? SH : g_fbH;
		const TInt bytes = (SW < g_fbW ? SW : g_fbW) * 2;
		TUint8 *dstp = (TUint8 *)g_fbBase;
		for (TInt y = 0; y < rows; y++)
			Mem::Copy(dstp + y * g_fbStride * 2, src + y * sstride, bytes);
		g_bmp->UnlockHeap();
		CFbsScreenDevice *sd = g_dsa->ScreenDevice();
		if (sd) sd->Update();
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

	// DSA у GUI-гілці. Раніше він піднімався лише в initL (консольний шлях), тож
	// прогони "gui ... dsa" мовчки лишались на віконному BitBlt: у логу dsa active=0.
	// Викликається і на поворот екрана, тому спершу знімаємо попередній.
	if (g_dsa) { delete g_dsa; g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; }
	if (g_useDsa)
		{
		// Заливка фону й redraw store - це право WS перемалювати вікно поверх кадру,
		// який ми кладемо в екран повз нього. Аргументу немає = "не чистити".
		aWin.SetBackgroundColor();
		aWin.EnableRedrawStore(EFalse);
		aWin.SetNonFading(ETrue);
		if (!CActiveScheduler::Current())
			{
			g_dsaSched = new CActiveScheduler;
			if (g_dsaSched) CActiveScheduler::Install(g_dsaSched);
			}
		TRAPD(e, {
			g_dsa = CDirectScreenAccess::NewL(aWs, aDev, aWin, g_dsaObs);
			dsaStartL();
			});
		if (e != KErrNone)
			{ delete g_dsa; g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; }
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
	// g_vout тримаємо синхронним із поточною областю показу ЗАВЖДИ, навіть коли
	// показувати не збираємось. Інакше при зміні адреси початку кадру ми показали б
	// стару область - тобто чужий буфер, а не застиглий кадр.
	g_voutSelfCall = 1;
	vout_update(&gpu, gpu.screen.src_x, gpu.screen.src_y);
	g_voutSelfCall = 0;

	// Пропуск растеризації в gpulib: показувати можна ЛИШЕ кадри, які він справді
	// намалював. Ознака - латч, виставлений його ж викликом vout_update
	// (GPUupdateLace виходить раніше для пропущених, gpu.c:1181-1185). Причини (2)
	// і (3) нижче тут не діють: гра гортає буфери й для непромальованих кадрів, тож
	// вони показали б порожній буфер - це й був чорний екран при frameskip.
	if (gpu.frameskip.set && !g_voutPending)
		return;

	if (g_dirtyOnly)
		{
		// Три незалежні причини показати кадр:
		//  1) gpulib повідомив про новий вміст видимої області;
		//  2) змінилась сама область показу (гра гортає готові буфери, нічого не
		//     малюючи - тоді fb_dirty_display_area не виставляється, і сигналу немає);
		//  3) стеля несвіжості - страховка від будь-якого пропущеного сигналу
		//     і від чужого малювання поверх нашого кадру під DSA.
		// Заміряно на GT: у гоночній фазі оновлюється 50% кадрів; у меню бували
		// вікна по 300 кадрів, де гра малювала лише в невидимий буфер і не гортала -
		// там показувати справді нема чого, і (3) не дає екрану виглядати мертвим.
		const TBool areaChanged = (g_vout.offset != g_lastShownOffset ||
		                           g_vout.w != g_lastShownW ||
		                           g_vout.h != g_lastShownH);
		if (!g_voutPending && !areaChanged && ++g_voutStale < 10)
			return;
		}
	g_lastShownOffset = g_vout.offset;
	g_lastShownW = g_vout.w;
	g_lastShownH = g_vout.h;
	g_voutPending = 0;
	g_voutStale = 0;
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

// Зонд: не викликати Update() екрана взагалі. Під direct-FB пікселі вже в буфері
// LCD, тож питання лише в тому, скільки коштує сам виклик - і чи він блокується на
// кадровій розгортці. Не для звичайного вжитку: без Update() частина апаратів
// нічого не покаже.
extern "C" void voutSymbianSetNoUpdate(int aOn)
	{
	g_noUpdate = aOn ? ETrue : EFalse;
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

// Растеризатор працює в режимі "Skip lines": непарні рядки VRAM застарілі.
extern "C" void voutSymbianSetIlaceOut(int aOn)
	{
	g_ilaceOut = aOn ? 1 : 0;
	}

// Показувати лише кадри, для яких gpulib повідомив про зміну.
extern "C" void voutSymbianSetDirtyOnly(int aOn)
	{
	g_dirtyOnly = aOn ? ETrue : EFalse;
	}

// Present порядковою копією в буфер екрана замість BitBlt. Потребує DSA.
extern "C" void voutSymbianSetRowCopy(int aOn)
	{
	g_useRowCopy = aOn ? ETrue : EFalse;
	if (aOn)
		g_useDsa = ETrue;   // копія в екран без захоплення екрана некоректна
	}

// Звідки взято базу й крок буфера екрана: 1 = від платформи, 0 = з HAL.
extern "C" int voutSymbianFbFromPlatform(void)
	{
	return g_fbFromPlatform ? 1 : 0;
	}

// Геометрія кадру після вписування (Z3): що саме віддає гра і куди воно лягає.
extern "C" void voutSymbianGeom(int *aSrcW, int *aSrcH, int *aDstW, int *aDstH,
                                int *aOx, int *aOy)
	{
	if (aSrcW) *aSrcW = g_vout.w;
	if (aSrcH) *aSrcH = g_vout.h;
	if (aDstW) *aDstW = g_blitW;
	if (aDstH) *aDstH = g_blitH;
	if (aOx)   *aOx   = g_blitOx;
	if (aOy)   *aOy   = g_blitOy;
	}

// Z1: розкладка екрана ВІД ПЛАТФОРМИ, а не з HAL. CFbsScreenDevice::Bits()/Stride()
// публічні (bitdev.h:145-146) і експортуються з bitgdi.dso; Stride() - це
// ScanLineBytes() драйвера, тобто те, чим малює сама платформа. Bits() може
// повернути NULL, якщо драйвер не реалізує MFastBlit2 - це теж відповідь.
// Плюс орієнтація режиму екрана: якщо не Normal, поворот застосовує GC, і сирий
// лінійний запис за HAL-адресою дає перекіс - саме те, що бачили 2026-08-01.
extern "C" void voutSymbianScreenLayout(int *aBits, int *aStride, int *aW, int *aH,
                                        int *aMode, int *aRotation, int *aHalBase)
	{
	if (aBits)     *aBits     = 0;
	if (aStride)   *aStride   = 0;
	if (aW)        *aW        = 0;
	if (aH)        *aH        = 0;
	if (aMode)     *aMode     = -1;
	if (aRotation) *aRotation = -1;
	if (aHalBase)  *aHalBase  = (int)g_fbBase;

	CFbsScreenDevice *sd = (g_dsa && g_dsaActive) ? g_dsa->ScreenDevice() : NULL;
	if (sd)
		{
		if (aBits)   *aBits   = (int)sd->Bits();
		if (aStride) *aStride = sd->Stride();
		const TSize sz = sd->SizeInPixels();
		if (aW)      *aW      = sz.iWidth;
		if (aH)      *aH      = sz.iHeight;
		if (aMode)   *aMode   = (int)sd->DisplayMode();
		}
	if (g_dev && aRotation)
		{
		TPixelsAndRotation par;
		TRAPD(e, g_dev->GetScreenModeSizeAndRotation(g_dev->CurrentScreenMode(), par));
		if (e == KErrNone)
			*aRotation = (int)par.iRotation;
		}
	}

// Z1/M-RB: чи доходять сирі записи за HAL-адресою до сканованого буфера і в якій
// розкладці. Малюємо візерунок ВІДОМО РОБОЧИМ шляхом (DSA BitBlt), читаємо назад
// сирим покажчиком і рахуємо збіги. Розшифровка: збіглось усе - HAL-адреса є
// видимим буфером; f(y,x) замість f(x,y) - поворот; сталий зсув - інший крок;
// нічого - подвійна буферизація або чужий буфер.
// aMode 0 = намалювати візерунок і перевірити, 1 = лише перевірити (через кадри).
extern "C" int voutSymbianReadBack(int aMode, int *aMatch, int *aTotal, int *aFirstBad)
	{
	if (aMatch)   *aMatch   = 0;
	if (aTotal)   *aTotal   = 0;
	if (aFirstBad) { aFirstBad[0] = aFirstBad[1] = aFirstBad[2] = aFirstBad[3] = -1; }
	if (!g_fbOk || !g_fbBase || !g_bmp)
		return -1;

	const TInt W = g_fbW < g_scr.iWidth ? g_fbW : g_scr.iWidth;
	const TInt H = g_fbH < g_scr.iHeight ? g_fbH : g_scr.iHeight;
	if (W < 8 || H < 8)
		return -2;

	if (aMode == 0)
		{
		g_bmp->LockHeap();
		TUint16 *bits = (TUint16 *)g_bmp->DataAddress();
		const TInt bstride = g_bmp->DataStride() / 2;
		for (TInt y = 0; y < H; y++)
			for (TInt x = 0; x < W; x++)
				bits[y * bstride + x] = (TUint16)(x * 7 + y * 13 + 1);
		g_bmp->UnlockHeap();
		if (!g_dsaGc)
			return -3;
		g_dsaGc->BitBlt(TPoint(0, 0), g_bmp);
		CFbsScreenDevice *sd = g_dsa ? g_dsa->ScreenDevice() : NULL;
		if (sd) sd->Update();
		}

	TInt match = 0, total = 0, bad = 0;
	for (TInt gy = 0; gy < 8; gy++)
		for (TInt gx = 0; gx < 8; gx++)
			{
			const TInt x = gx * (W / 8) + 3;
			const TInt y = gy * (H / 8) + 3;
			if (x >= W || y >= H)
				continue;
			// Порівнюємо в 16 бітах: бітмап заповнено RGB565, а буфер екрана може
			// бути 32-бітним - тоді беремо той самий піксель у своєму форматі й
			// зводимо назад до 565, інакше "розбіжність" означала б лише формат.
			const TUint16 want = (TUint16)(x * 7 + y * 13 + 1);
			const TUint8 *rowp = (const TUint8 *)g_fbBase + y * g_fbStrideB;
			TUint16 got;
			if (g_fbPixBytes == 4)
				{
				const TUint32 v = ((const TUint32 *)rowp)[x];
				got = (TUint16)((((v >> 16) & 0xff) >> 3) << 11 |
				                (((v >> 8) & 0xff) >> 2) << 5 |
				                 ((v & 0xff) >> 3));
				}
			else
				got = ((const TUint16 *)rowp)[x];
			total++;
			if (want == got)
				match++;
			else if (!bad && aFirstBad)
				{
				aFirstBad[0] = x; aFirstBad[1] = y;
				aFirstBad[2] = want; aFirstBad[3] = got;
				bad = 1;
				}
			}
	if (aMatch) *aMatch = match;
	if (aTotal) *aTotal = total;
	return 0;
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
	const TInt W = g_fbW, H = g_fbH;
	TUint8 *fb = (TUint8 *)g_fbBase;
	// Кольори задаємо в BGR555 і конвертуємо у формат буфера: він може бути і
	// 16-бітним, і 32-бітним (на N95 8GB заміряно EColor16MU).
	const TUint16 BLUE = 0x7c00, WHITE = 0x7fff, RED = 0x001f,
	              GREEN = 0x03e0, YELLOW = 0x03ff, CYAN = 0x7fe0;
	for (TInt y = 0; y < H; y++)
		{
		TUint8 *row = fb + y * g_fbStrideB;
		for (TInt x = 0; x < W; x++)
			{
			TUint16 c = BLUE;
			if (x < 2 || x >= W - 2 || y < 2 || y >= H - 2) c = WHITE;      // рамка
			else if (x < 40 && y < 40) c = RED;                            // ЛВ кут
			else if (x >= W - 40 && y < 40) c = GREEN;                     // ПВ кут
			else if (x < 40 && y >= H - 40) c = CYAN;                      // ЛН кут
			else if (x >= W - 40 && y >= H - 40) c = YELLOW;               // ПН кут
			else if (x == y) c = WHITE;                                    // діагональ
			if (g_fbPixBytes == 4) ((TUint32 *)row)[x] = Bgr555ToXrgb8888(c);
			else                   ((TUint16 *)row)[x] = Bgr555ToRgb565(c);
			}
		}
	if (g_dsa && g_dsaActive)
		{ CFbsScreenDevice *sd = g_dsa->ScreenDevice(); if (sd) sd->Update(); }
	return 0;
	}

// Візуальний A/B для direct-FB: та сама картинка двома шляхами. aRaw=0 - через
// bitgdi (DSA BitBlt), тобто шлях, який точно показує; aRaw=1 - сирим записом у
// той самий буфер. Кольори різні, щоб їх не сплутати: зелене поле = bitgdi,
// червоне = сирий запис. Що з'явилось на панелі, те й відповідає на питання,
// чи доходить сирий запис до показу взагалі.
extern "C" int voutSymbianVisTest(int aRaw)
	{
	if (!g_fbOk || !g_fbBase || !g_bmp)
		return -1;
	const TInt W = aRaw ? g_fbW : g_scr.iWidth;
	const TInt H = aRaw ? g_fbH : g_scr.iHeight;
	if (W < 16 || H < 16)
		return -2;
	const TUint16 bg   = aRaw ? 0x001f : 0x03e0;   // BGR555: червоний / зелений
	const TUint16 fg   = 0x7fff;                   // білий

	if (!aRaw)
		g_bmp->LockHeap();
	TUint8 *dst = aRaw ? (TUint8 *)g_fbBase : (TUint8 *)g_bmp->DataAddress();
	const TInt stride = aRaw ? g_fbStrideB : g_bmp->DataStride();
	const TInt pix    = aRaw ? g_fbPixBytes : 2;
	for (TInt y = 0; y < H; y++)
		{
		TUint8 *row = dst + y * stride;
		for (TInt x = 0; x < W; x++)
			{
			// Хрест по центру плюс рамка: за ними видно і зсув, і поворот.
			const TBool cross = (x >= W / 2 - 3 && x <= W / 2 + 3) ||
			                    (y >= H / 2 - 3 && y <= H / 2 + 3);
			const TBool frame = (x < 3 || x >= W - 3 || y < 3 || y >= H - 3);
			const TUint16 c = (cross || frame) ? fg : bg;
			if (pix == 4) ((TUint32 *)row)[x] = Bgr555ToXrgb8888(c);
			else          ((TUint16 *)row)[x] = Bgr555ToRgb565(c);
			}
		}
	if (!aRaw)
		{
		g_bmp->UnlockHeap();
		if (!g_dsaGc)
			return -3;
		g_dsaGc->BitBlt(TPoint(0, 0), g_bmp);
		}
	CFbsScreenDevice *sd = g_dsa ? g_dsa->ScreenDevice() : NULL;
	if (sd)
		{
		const RRegion *rgn = g_dsa->DrawingRegion();
		if (rgn && !rgn->CheckError()) sd->Update(*rgn);
		else                           sd->Update();
		}
	return 0;
	}

// Знімок того, що РЕАЛЬНО лежить у буфері екрана. Дамп VRAM показує вхід
// конвертації, тобто не відрізняє "гра не намалювала" від "ми не показали".
// Тут читаємо назад саме те, що бачить панель.
extern "C" int voutSymbianScreenDump(const char *aPath)
	{
	if (!g_fbOk || !g_fbBase)
		return -1;
	FILE *f = fopen(aPath, "wb");
	if (!f)
		return -2;
	char hdr[160];
	const int n = snprintf(hdr, sizeof(hdr),
		"PS1SCR %d %d %d %d vout=%d,%d,%d,%d off=%d rgb24=%d\n",
		g_fbW, g_fbH, g_fbStrideB, g_fbPixBytes,
		g_vout.x, g_vout.y, g_vout.w, g_vout.h, g_vout.offset, g_vout.rgb24);
	fwrite(hdr, 1, n, f);
	for (TInt y = 0; y < g_fbH; y++)
		fwrite((const TUint8 *)g_fbBase + y * g_fbStrideB, 1, g_fbW * g_fbPixBytes, f);
	fclose(f);
	return 0;
	}

// Знімок на вимогу: що бачить панель + що намалювала гра + стан шляху виводу.
// Різниця між двома файлами локалізує ваду: правильна VRAM і зіпсований екран
// означають помилку виводу, зіпсована VRAM - помилку растеризації.
extern "C" void ps1ScreenShot(void)
	{
	const int sr = voutSymbianScreenDump("E:\\ps1_scrkey.bin");
	const int vr = voutSymbianDump("E:\\ps1_vramkey.bin");
	ps1LogFmt("screen_dump",
		"manual: scr=%d vram=%d vout=%d,%d,%d,%d off=%d rgb24=%d dsa=%d fbPlat=%d "
		"fb=%dx%d stride=%dB pix=%d mode=%d",
		sr, vr, g_vout.x, g_vout.y, g_vout.w, g_vout.h, g_vout.offset, g_vout.rgb24,
		(int)(g_useDsa && g_dsaActive), (int)g_fbFromPlatform,
		g_fbW, g_fbH, g_fbStrideB, g_fbPixBytes, g_fbMode);
	}

// Знімок буфера за HAL-адресою - того, який (за замірами 2026-08-01) справді
// сканується панеллю. Буфер драйвера (Bits()) і цей - різні описи: драйвер каже
// 1280 Б/рядок і 32 біти, HAL - 640 Б і 16. Порівняння двох дампів відповідає на
// питання, чи доїжджає наш кадр із буфера драйвера до сканованого.
// Сирий дамп буфера екрана БЕЗ припущень про розкладку. Разом із візерунком
// x*7+y*13+1, покладеним відомо робочим шляхом (DSA BitBlt), дозволяє визначити
// байтів на піксель і крок рядка вже на хості, без нових прогонів на апараті.
// aBytes обмежено тим, що ми й так пишемо щокадру, тобто свідомо відображеним.
extern "C" int voutSymbianRawDump(const char *aPath, int aBytes)
	{
	if (!g_fbOk || !g_fbBase)
		return -1;
	const int maxB = g_fbH * g_fbStrideB;
	if (aBytes <= 0 || aBytes > maxB)
		aBytes = maxB;
	FILE *f = fopen(aPath, "wb");
	if (!f)
		return -2;
	char hdr[160];
	const int n = snprintf(hdr, sizeof(hdr),
		"PS1RAW base=0x%x bytes=%d platStride=%d platPix=%d %dx%d hal=0x%x halLine=%d halBpp=%d %dx%d\n",
		(unsigned)g_fbBase, aBytes, g_fbStrideB, g_fbPixBytes, g_fbW, g_fbH,
		(unsigned)g_fbAddr, g_fbLineBytes, g_fbBpp, g_fbXpix, g_fbYpix);
	fwrite(hdr, 1, n, f);
	fwrite((const TUint8 *)g_fbBase, 1, aBytes, f);
	fclose(f);
	return 0;
	}

// Адреса окремо від дампа: якщо вона фізична, читання вб'є потік, і в логу має
// лишитись хоча б саме число.
extern "C" int voutSymbianHalAddr(void)
	{
	return g_fbAddr;
	}

extern "C" int voutSymbianHalDump(const char *aPath)
	{
	if (!g_fbAddr || g_fbLineBytes < 2 || g_fbYpix < 1)
		return -1;
	FILE *f = fopen(aPath, "wb");
	if (!f)
		return -2;
	char hdr[160];
	const int n = snprintf(hdr, sizeof(hdr),
		"PS1HAL addr=0x%x bpp=%d line=%d first=%d %dx%d plat=0x%x platStride=%d platPix=%d\n",
		(unsigned)g_fbAddr, g_fbBpp, g_fbLineBytes, g_fbFirstOff, g_fbXpix, g_fbYpix,
		(unsigned)g_fbBase, g_fbStrideB, g_fbPixBytes);
	fwrite(hdr, 1, n, f);
	// Рівно стільки, скільки описав HAL: читати далі - вихід за відображення.
	fwrite((const TUint8 *)g_fbAddr + g_fbFirstOff, 1, g_fbLineBytes * g_fbYpix, f);
	fclose(f);
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
// cbs МУСИТЬ пережити виклик: gpulib лишає в себе вказівники на fskip_advice /
// fskip_force / fskip_dirty усередині неї, а не копії.
void ps1GpuSetCallbacks(unsigned int *frameCount, unsigned int *hcnt, int aFrameskip)
	{
	static struct rearmed_cbs cbs;
	memset(&cbs, 0, sizeof(cbs));
	cbs.gpu_frame_count = frameCount;
	cbs.gpu_hcnt = hcnt;
	cbs.thread_rendering = 0;
	// Пропуск растеризації силами gpulib: скільки кадрів поспіль дозволено НЕ
	// малювати. Копіюється в gpu.frameskip.set саме тут, тож значення має бути
	// відоме до виклику.
	cbs.frameskip = aFrameskip;
	GPUrearmedCallbacks(&cbs);
	}

} // extern "C"
