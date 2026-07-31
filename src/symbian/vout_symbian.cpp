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

// --- Виведення кадру на екран (DSA/CFbsBitmap, Етап 2) ----------------------
// Показуємо вміст VRAM на екрані апарата. Джерело - той самий g_vout, що й дамп
// (перевірений тракт): якщо gpulib задав вікно дисплея (справжня гра/BIOS),
// беремо його; інакше (шлях gputest, де регістри дисплея не програмуються) -
// лівий верхній кут VRAM у розмір екрана, там лежить заливка самоперевірки.
//
// Малюємо через window server + BitBlt, а не через CDirectScreenAccess: DSA -
// це CActive і потребує CActiveScheduler та обробки AbortNow/Restart, тоді як
// для показу одного кадру достатньо синхронного BitBlt. Швидкий шлях DSA -
// коли дійде до анімації (Етап 4), а не до перевірки виведення.

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

static void ShowL(TInt aHoldMs)
	{
	const TUint16 *vram = (const TUint16 *)g_vout.vram;
	if (!vram)
		User::Leave(KErrNotReady);

	RWsSession ws;
	User::LeaveIfError(ws.Connect());
	CleanupClosePushL(ws);

	CWsScreenDevice *dev = new (ELeave) CWsScreenDevice(ws);
	CleanupStack::PushL(dev);
	User::LeaveIfError(dev->Construct());

	RWindowGroup grp(ws);
	User::LeaveIfError(grp.Construct((TUint32)&grp, ETrue));
	CleanupClosePushL(grp);
	grp.SetOrdinalPosition(0);

	RWindow win(ws);
	User::LeaveIfError(win.Construct(grp, (TUint32)&win));
	CleanupClosePushL(win);

	const TSize scr = dev->SizeInPixels();
	win.SetExtent(TPoint(0, 0), scr);
	win.SetBackgroundColor(KRgbBlack);
	win.Activate();

	CWindowGc *gc = new (ELeave) CWindowGc(dev);
	CleanupStack::PushL(gc);
	User::LeaveIfError(gc->Construct());

	// Початок і розмір джерела у VRAM. g_vout.offset уже враховує src_x/src_y.
	const TUint16 *base = vram;
	TInt w = scr.iWidth, h = scr.iHeight;
	if (g_vout.w > 0 && g_vout.h > 0)
		{
		base = vram + g_vout.offset / 2;
		w = g_vout.w;
		h = g_vout.h;
		}
	if (w > scr.iWidth)  w = scr.iWidth;   // без масштабування на першому кроці
	if (h > scr.iHeight) h = scr.iHeight;

	CFbsBitmap *bmp = new (ELeave) CFbsBitmap;
	CleanupStack::PushL(bmp);
	User::LeaveIfError(bmp->Create(TSize(w, h), EColor64K));

	bmp->LockHeap();
	TUint16 *dst = (TUint16 *)bmp->DataAddress();
	const TInt dstStride = bmp->DataStride() / 2;   // у 16-бітних словах
	for (TInt row = 0; row < h; row++)
		{
		const TUint16 *s = base + row * 1024;        // крок рядка VRAM - 1024
		TUint16 *d = dst + row * dstStride;
		for (TInt col = 0; col < w; col++)
			d[col] = Bgr555ToRgb565(s[col]);
		}
	bmp->UnlockHeap();

	win.Invalidate();
	win.BeginRedraw();
	gc->Activate(win);
	gc->BitBlt(TPoint(0, 0), bmp);
	gc->Deactivate();
	win.EndRedraw();
	ws.Flush();

	User::After(aHoldMs * 1000);

	CleanupStack::PopAndDestroy(bmp);
	CleanupStack::PopAndDestroy(gc);
	CleanupStack::PopAndDestroy();   // win
	CleanupStack::PopAndDestroy();   // grp
	CleanupStack::PopAndDestroy(dev);
	CleanupStack::PopAndDestroy();   // ws
	}

// Тримаємо кадр aHoldMs мілісекунд, щоб людина встигла глянути на екран.
// Обгорнуто у TRAP: збій виведення не має валити прогін (числа з логу цінні
// самі по собі), тому повертаємо код помилки, а не лишаємо leave нагору.
extern "C" int voutSymbianShow(int holdMs)
	{
	if (holdMs < 0) holdMs = 0;
	if (holdMs > 30000) holdMs = 30000;
	TInt err;
	TRAP(err, ShowL(holdMs));
	return err;
	}

// --- Обгортки для libpcsxcore -----------------------------------------------
// libpcsxcore/plugins.h оголошує GPUinit, GPUopen тощо як ТИПИ, а в gpulib це
// функції з тими самими іменами. Транслювати обидва заголовки в одному файлі
// не можна, тож перейменовуємо тут, де plugins.h не потрібен.

extern "C" {

long ps1GpuInit(void)                                   { return GPUinit(); }
long ps1GpuShutdown(void)                               { return GPUshutdown(); }
long ps1GpuOpen(unsigned long *d, char *c, char *g)     { return GPUopen(d, c, g); }
long ps1GpuClose(void)                                  { return GPUclose(); }
void ps1GpuWriteStatus(uint32_t d)                      { GPUwriteStatus(d); }
void ps1GpuWriteData(uint32_t d)                        { GPUwriteData(d); }
void ps1GpuWriteDataMem(uint32_t *m, int c)             { GPUwriteDataMem(m, c); }
uint32_t ps1GpuReadStatus(void)                         { return GPUreadStatus(); }
uint32_t ps1GpuReadData(void)                           { return GPUreadData(); }
void ps1GpuReadDataMem(uint32_t *m, int c)              { GPUreadDataMem(m, c); }
long ps1GpuDmaChain(uint32_t *rb, uint32_t sa, uint32_t *pa, int32_t *cy)
                                                        { return GPUdmaChain(rb, sa, pa, cy); }
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
