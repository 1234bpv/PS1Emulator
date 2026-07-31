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

	// Вписуємо джерело в екран зі збереженням пропорцій. PS1 - ландшафт
	// (320x240/640x480), екран N95 - портрет (240x320), тож кадр лягає з
	// чорними полями згори/знизу. Обертання під ландшафт - доведення (Етап 4).
	const TInt SW = g_scr.iWidth, SH = g_scr.iHeight;
	TInt dstW = SW;
	TInt dstH = sh * SW / sw;
	if (dstH > SH) { dstH = SH; dstW = sw * SH / sh; }
	if (dstW < 1) dstW = 1;
	if (dstH < 1) dstH = 1;
	const TInt ox = (SW - dstW) / 2;
	const TInt oy = (SH - dstH) / 2;

	g_bmp->LockHeap();
	TUint16 *dst = (TUint16 *)g_bmp->DataAddress();
	const TInt dstStride = g_bmp->DataStride() / 2;   // у 16-бітних словах
	Mem::FillZ(dst, dstStride * SH * 2);              // чорні поля
	for (TInt dy = 0; dy < dstH; dy++)
		{
		const TInt sy = dy * sh / dstH;
		TUint16 *drow = dst + (oy + dy) * dstStride + ox;
		if (rgb24)
			{
			// RGB24: 3 байти на піксель (R,G,B). Поки НЕ перевірено на залізі.
			const TUint8 *srow = vbytes + startByte + sy * 2048;
			for (TInt dx = 0; dx < dstW; dx++)
				{
				const TUint8 *p = srow + (dx * sw / dstW) * 3;
				drow[dx] = (TUint16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
				}
			}
		else
			{
			const TUint16 *srow = (const TUint16 *)(vbytes + startByte) + sy * 1024;
			for (TInt dx = 0; dx < dstW; dx++)
				drow[dx] = Bgr555ToRgb565(srow[dx * sw / dstW]);
			}
		}
	g_bmp->UnlockHeap();

	g_win.Invalidate();
	g_win.BeginRedraw();
	g_gc->Activate(g_win);
	g_gc->BitBlt(TPoint(0, 0), g_bmp);
	g_gc->Deactivate();
	g_win.EndRedraw();
	g_ws.Flush();
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
	delete g_bmp; g_bmp = NULL;
	delete g_gc;  g_gc = NULL;
	g_win.Close();
	g_grp.Close();
	delete g_dev; g_dev = NULL;
	g_ws.Close();
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
	return GPUinit();
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
