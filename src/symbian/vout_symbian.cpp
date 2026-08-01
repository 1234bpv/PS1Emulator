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
	else
		{
		g_win.Invalidate();
		g_win.BeginRedraw();
		g_gc->Activate(g_win);
		g_gc->BitBlt(TPoint(0, 0), g_bmp);
		g_gc->Deactivate();
		g_win.EndRedraw();
		g_ws.Flush();
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

// Неблокуюче опитування клавіш: виснажує чергу подій вікон, оновлює g_pad
// (біти DKEY_*, active-high) на натисканнях/відпусканнях, повертає поточний стан.
// Без CActiveScheduler: перевіряємо озброєний TRequestStatus напряму.
extern "C" unsigned short voutSymbianPollPad(void)
	{
	if (!g_wsReady)
		return 0;
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
	if (holdMs > 0)
		User::After(holdMs * 1000);
	g_ws.EventReadyCancel();   // знімаємо озброєний запит подій перед закриттям
	// DSA тримає вікно/пристрій - знімаємо його першим.
	if (g_dsa) { delete g_dsa; g_dsa = NULL; g_dsaGc = NULL; g_dsaActive = EFalse; }
	delete g_bmp; g_bmp = NULL;
	delete g_gc;  g_gc = NULL;
	g_win.Close();
	g_grp.Close();
	delete g_dev; g_dev = NULL;
	g_ws.Close();
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
