// glrender.cpp - растеризатор PS1 на MBX через OpenGL ES 1.1.
//
// Навіщо: розділ 14 звіту міряє, що той самий кадр Gran Turismo коштує тут
// 6,9 мс проти 14,4 у зв'язці "gpu_unai + блит". Числа з апарата, не з оцінок.
//
// Що цей файл робить і чого НЕ робить на цьому кроці. Він приймає ті самі
// пакети команд, які розбирає gpu_unai, і складає з них трикутники для GL.
// Текстур він поки не читає: текстурований примітив малюється своїм кольором.
// Це навмисно - спершу має бути правильною геометрія, порядок, відсікання і
// напівпрозорість, бо помилку в них не видно за текстурами.
//
// Ключове рішення про порядок: gpu_unai продовжує малювати ВСЕ у програмний
// VRAM, як і раніше, а GL малює паралельно й лише показує. Тому VRAM лишається
// правдивим для читань, копіювань і показу, і будь-якої миті можна вимкнути
// прапорець і побачити ту саму сцену старим шляхом. Швидкодії це поки не дає -
// вона з'явиться, коли програмний прохід можна буде вимкнути.
//
// Заміряні обмеження, від яких залежить будова (розділ 14):
//   - виклик малювання коштує 9,5-14 мкс, тому примітиви групуються;
//   - порядок подання міняти НЕ можна (перекриття на PS1 визначає саме він),
//     тому групуються лише сусідні примітиви з однаковим станом;
//   - зміна прив'язки текстури коштує стільки ж, скільки виклик.

#include <e32std.h>
#include <GLES/gl.h>

extern "C" void ps1LogFmt(const char *aType, const char *aFmt, ...);
extern "C" int  voutSymbianGlSwap(void);
extern "C" void voutSymbianGlSize(int *aW, int *aH);

// Стан, який gpu_unai передає разом із кожним пакетом. Порядок задає
// gpulib_if.cpp; тримати обидві сторони в одному порядку - обов'язок цього
// заголовка коментаря, бо типу тут немає навмисно (він був би спільним файлом
// між двома репозиторіями).
enum TGlrState
	{
	EStOfsX = 0, EStOfsY,
	EStClipX0, EStClipY0, EStClipX1, EStClipY1,
	EStBlendMode,     // 0: B/2+F/2  1: B+F  2: B-F  3: B+F/4
	EStTextMode,      // 32/64/96 - 4, 8, 16 біт на тексель
	EStTba,           // зсув текстурної сторінки у VRAM, півслів
	EStCba,           // індекс CLUT
	EStCount
	};

// Стеля на кадр. Заміряно 324 примітиви на кадр; беремо вчетверо з запасом,
// бо кадр із заставкою або меню може бути щільнішим.
static const TInt KMaxPrims = 1400;
static const TInt KMaxVerts = KMaxPrims * 6;
static const TInt KMaxBatch = 256;

struct TGlrBatch
	{
	TInt iFirst;      // перша вершина
	TInt iCount;      // скільки вершин
	TInt iBlend;      // -1 = непрозорий, інакше режим 0..3
	TInt iClip;       // індекс ділянки малювання, під якою подано пакет
	};

// За кадр ділянка малювання буває не одна: крім заднього буфера трапляються
// перехідні (на боті апарат показав 0,0..640,512). Проекцію будуємо за тією,
// під якою намальовано найбільше вершин, а не за першою і не за найбільшою за
// площею - обидва ті правила вже дали хибну картинку. Примітиви решти ділянок
// не малюємо взагалі: у чужій проекції вони лягли б навмання.
static const TInt KMaxClips = 4;

static TBool     g_on;            // рендерер увімкнено прапорцем
static TBool     g_ready;         // ініціалізовано і є контекст
static GLshort  *g_pos;           // x,y на вершину
static GLubyte  *g_col;           // r,g,b,a на вершину
static TInt      g_nVerts;
static TGlrBatch g_batch[KMaxBatch];
static TInt      g_nBatch;
static TInt      g_clip[4];       // ділянка малювання, за якою будуємо проекцію
static TInt      g_clips[KMaxClips][4];
static TInt      g_clipVerts[KMaxClips];
static TInt      g_nClips;
static TInt      g_curClip;       // ділянка поточного примітиву, -1 = не влізла
static TInt      g_vpW, g_vpH;
static TInt      g_over;          // скільки примітивів не вмістилось
static TInt      g_logged;      // скільки діагностичних кадрів уже записано

extern "C" int glrEnabled(void)
	{
	return (g_on && g_ready) ? 1 : 0;
	}

extern "C" void glrSetEnabled(int aOn)
	{
	g_on = aOn ? ETrue : EFalse;
	}

extern "C" int glrInit(void)
	{
	if (g_ready)
		return 0;
	g_pos = (GLshort *)User::Alloc(KMaxVerts * 2 * sizeof(GLshort));
	g_col = (GLubyte *)User::Alloc(KMaxVerts * 4);
	if (!g_pos || !g_col)
		{
		User::Free(g_pos); g_pos = NULL;
		User::Free(g_col); g_col = NULL;
		ps1LogFmt("glr", "init failed: out of memory");
		return -1;
		}
	voutSymbianGlSize(&g_vpW, &g_vpH);
	if (g_vpW <= 0 || g_vpH <= 0)
		{ g_vpW = 320; g_vpH = 240; }

	glViewport(0, 0, g_vpW, g_vpH);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glDisable(GL_DITHER);
	glShadeModel(GL_SMOOTH);
	glClearColor(0, 0, 0, 1);
	g_ready = ETrue;
	ps1LogFmt("glr", "init ok: viewport=%dx%d prims<=%d", g_vpW, g_vpH,
		(int)KMaxPrims);
	return 0;
	}

extern "C" void glrShutdown(void)
	{
	User::Free(g_pos); g_pos = NULL;
	User::Free(g_col); g_col = NULL;
	g_ready = EFalse;
	}

// --- складання вершин --------------------------------------------------------

// Відкриває новий пакет, якщо стан змінився. Порядок подання зберігається:
// об'єднуються лише СУСІДНІ примітиви, бо на PS1 пізніший примітив перекриває
// раніший, і переставляти їх не можна.
static void batchFor(TInt aBlend)
	{
	if (g_nBatch > 0 && g_batch[g_nBatch - 1].iBlend == aBlend
	                 && g_batch[g_nBatch - 1].iClip == g_curClip)
		return;
	if (g_nBatch >= KMaxBatch)
		return;
	g_batch[g_nBatch].iFirst = g_nVerts;
	g_batch[g_nBatch].iCount = 0;
	g_batch[g_nBatch].iBlend = aBlend;
	g_batch[g_nBatch].iClip = g_curClip;
	g_nBatch++;
	}

static inline void emitVertex(TInt aX, TInt aY, TUint32 aBgr, TInt aAlpha)
	{
	GLshort *p = g_pos + g_nVerts * 2;
	GLubyte *c = g_col + g_nVerts * 4;
	p[0] = (GLshort)aX;
	p[1] = (GLshort)aY;
	c[0] = (GLubyte)(aBgr & 0xFF);
	c[1] = (GLubyte)((aBgr >> 8) & 0xFF);
	c[2] = (GLubyte)((aBgr >> 16) & 0xFF);
	c[3] = (GLubyte)aAlpha;
	g_nVerts++;
	g_batch[g_nBatch - 1].iCount++;
	if (g_curClip >= 0)
		g_clipVerts[g_curClip]++;
	}

// Трикутник трьома вершинами; кольори окремі, бо Gouraud їх має різними.
static void emitTri(const TInt *aX, const TInt *aY, const TUint32 *aCol, TInt aAlpha)
	{
	if (g_nVerts + 3 > KMaxVerts) { g_over++; return; }
	emitVertex(aX[0], aY[0], aCol[0], aAlpha);
	emitVertex(aX[1], aY[1], aCol[1], aAlpha);
	emitVertex(aX[2], aY[2], aCol[2], aAlpha);
	}

// Знак координати PS1: одинадцять біт, старші - розширення знаку.
static inline TInt expandSign(TUint32 aV)
	{
	return ((TInt)(aV << 21)) >> 21;
	}

// --- прийом команд -----------------------------------------------------------

// Один примітив. Викликається з do_cmd_list після того, як gpu_unai його вже
// намалював у програмний VRAM, тобто весь стан уже поточний.
extern "C" void glrPrim(unsigned aCmd, const unsigned *aPacket, const int *aState)
	{
	if (!g_on || !g_ready)
		return;
	// До якої ділянки малювання належить цей примітив. Вибір переможця -
	// наприкінці кадру, за кількістю вершин.
	g_curClip = -1;
	for (TInt i = 0; i < g_nClips; i++)
		if (g_clips[i][0] == aState[EStClipX0] && g_clips[i][1] == aState[EStClipY0] &&
		    g_clips[i][2] == aState[EStClipX1] && g_clips[i][3] == aState[EStClipY1])
			{ g_curClip = i; break; }
	if (g_curClip < 0 && g_nClips < KMaxClips)
		{
		g_curClip = g_nClips++;
		g_clips[g_curClip][0] = aState[EStClipX0];
		g_clips[g_curClip][1] = aState[EStClipY0];
		g_clips[g_curClip][2] = aState[EStClipX1];
		g_clips[g_curClip][3] = aState[EStClipY1];
		g_clipVerts[g_curClip] = 0;
		}

	const TInt ofsX = aState[EStOfsX], ofsY = aState[EStOfsY];
	// Напівпрозорість несе біт 1 коду команди; режим - у стані.
	const TInt blend = (aCmd & 2) ? aState[EStBlendMode] : -1;
	// Режим 3 (B+F/4) виражається через звичайне додавання, якщо самий колір
	// поділити на чотири; альфа тут і слугує тим дільником.
	const TInt alpha = (blend == 0) ? 128 : (blend == 3 ? 64 : 255);

	if (aCmd >= 0x20 && aCmd <= 0x3F)
		{
		const TBool tex = (aCmd & 4) != 0;
		const TBool gouraud = (aCmd & 0x10) != 0;
		const TBool quad = (aCmd & 8) != 0;
		const TInt stride = 1 + (tex ? 1 : 0) + (gouraud ? 1 : 0);
		const TInt nv = quad ? 4 : 3;
		TInt x[4], y[4];
		TUint32 col[4];
		// Текстуру ми ще не читаємо, тож текстурований примітив дістає свій
		// колір. Для "сирої" текстури (біт 0) кольору в пакеті немає взагалі -
		// беремо нейтральний, інакше вийшли б чорні плями.
		const TUint32 flat = (tex && (aCmd & 1)) ? 0x808080
		                                        : (aPacket[0] & 0xFFFFFF);
		for (TInt i = 0; i < nv; i++)
			{
			const TUint32 c = aPacket[1 + i * stride];
			x[i] = expandSign(c) + ofsX;
			y[i] = expandSign(c >> 16) + ofsY;
			col[i] = gouraud ? (aPacket[i * stride] & 0xFFFFFF) : flat;
			}
		batchFor(blend);
		TInt tx[3], ty[3]; TUint32 tc[3];
		tx[0]=x[0]; ty[0]=y[0]; tc[0]=col[0];
		tx[1]=x[1]; ty[1]=y[1]; tc[1]=col[1];
		tx[2]=x[2]; ty[2]=y[2]; tc[2]=col[2];
		emitTri(tx, ty, tc, alpha);
		if (quad)
			{
			tx[0]=x[1]; ty[0]=y[1]; tc[0]=col[1];
			tx[1]=x[2]; ty[1]=y[2]; tc[1]=col[2];
			tx[2]=x[3]; ty[2]=y[3]; tc[2]=col[3];
			emitTri(tx, ty, tc, alpha);
			}
		return;
		}

	if (aCmd >= 0x60 && aCmd <= 0x7F)
		{
		// Прямокутник. Розмір задають біти 3-4 коду: 0 - у пакеті, інакше
		// стала сторона 1, 8 або 16.
		const TBool tex = (aCmd & 4) != 0;
		const TUint32 xy = aPacket[1];
		const TInt x0 = expandSign(xy) + ofsX;
		const TInt y0 = expandSign(xy >> 16) + ofsY;
		TInt w = 0, h = 0;
		switch (aCmd & 0x18)
			{
			case 0x08: w = 1;  h = 1;  break;
			case 0x10: w = 8;  h = 8;  break;
			case 0x18: w = 16; h = 16; break;
			default:
				{
				const TUint32 wh = aPacket[tex ? 3 : 2];
				w = (TInt)(wh & 0x3FF);
				h = (TInt)((wh >> 16) & 0x1FF);
				} break;
			}
		if (w <= 0 || h <= 0)
			return;
		const TUint32 c = (tex && (aCmd & 1)) ? 0x808080 : (aPacket[0] & 0xFFFFFF);
		batchFor(blend);
		TInt tx[3], ty[3]; TUint32 tc[3];
		tc[0] = tc[1] = tc[2] = c;
		tx[0]=x0;   ty[0]=y0;   tx[1]=x0+w; ty[1]=y0;   tx[2]=x0; ty[2]=y0+h;
		emitTri(tx, ty, tc, alpha);
		tx[0]=x0+w; ty[0]=y0;   tx[1]=x0+w; ty[1]=y0+h; tx[2]=x0; ty[2]=y0+h;
		emitTri(tx, ty, tc, alpha);
		return;
		}

	if (aCmd == 0x02)
		{
		// Заливка. Вона єдина не зважає ні на зсув, ні на відсікання, і завжди
		// непрозора - це не примітив, а запис у VRAM.
		const TInt x0 = (TInt)(aPacket[1] & 0x3F0);
		const TInt y0 = (TInt)((aPacket[1] >> 16) & 0x1FF);
		const TInt w = (TInt)(((aPacket[2] & 0x3FF) + 15) & ~15);
		const TInt h = (TInt)((aPacket[2] >> 16) & 0x1FF);
		if (w <= 0 || h <= 0)
			return;
		const TUint32 c = aPacket[0] & 0xFFFFFF;
		batchFor(-1);
		TInt tx[3], ty[3]; TUint32 tc[3];
		tc[0] = tc[1] = tc[2] = c;
		tx[0]=x0;   ty[0]=y0;   tx[1]=x0+w; ty[1]=y0;   tx[2]=x0; ty[2]=y0+h;
		emitTri(tx, ty, tc, 255);
		tx[0]=x0+w; ty[0]=y0;   tx[1]=x0+w; ty[1]=y0+h; tx[2]=x0; ty[2]=y0+h;
		emitTri(tx, ty, tc, 255);
		return;
		}
	// Лінії (0x40-0x5F) поки не малюємо: їх 6,5 на кадр (розділ 13).
	}

// --- кадр --------------------------------------------------------------------

extern "C" void glrBeginFrame(void)
	{
	if (!g_on || !g_ready)
		return;
	g_nVerts = 0;
	g_nBatch = 0;
	g_nClips = 0;
	g_curClip = -1;
	g_over = 0;
	}

// Режими напівпрозорості PS1 у термінах GL. Режим 2 (B-F) відповідника не має;
// заміряно, що гра вживає його 35 разів за 6099 кадрів (розділ 13), тож беремо
// затемнення як найближче за змістом і позначаємо це тут, а не ховаємо.
static void applyBlend(TInt aMode)
	{
	if (aMode < 0)
		{
		glDisable(GL_BLEND);
		return;
		}
	glEnable(GL_BLEND);
	switch (aMode)
		{
		case 0:  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
		case 1:  glBlendFunc(GL_ONE, GL_ONE); break;
		case 2:  glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); break;
		default: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;   // B + F/4 через альфу
		}
	}

extern "C" int glrEndFrame(void)
	{
	if (!g_on || !g_ready)
		return 0;
	// Кадр наш навіть тоді, коли малювати нічого: віддати його шляхові показу
	// з VRAM означало б показати сцену, порахунку якої ми щойно не робили.
	// Порожній кадр просто лишає на екрані попередній.
	if (g_nBatch == 0 || g_nClips == 0)
		return 1;

	// Ділянка, під якою намальовано найбільше вершин, і є задній буфер.
	TInt best = 0;
	for (TInt i = 1; i < g_nClips; i++)
		if (g_clipVerts[i] > g_clipVerts[best])
			best = i;
	g_clip[0] = g_clips[best][0]; g_clip[1] = g_clips[best][1];
	g_clip[2] = g_clips[best][2]; g_clip[3] = g_clips[best][3];
	if (g_clip[2] <= g_clip[0] || g_clip[3] <= g_clip[1])
		return 1;

	// Ділянка малювання PS1 лягає на все вікно. Вісь Y донизу, як у VRAM.
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof((GLfloat)g_clip[0], (GLfloat)(g_clip[2] + 1),
	         (GLfloat)(g_clip[3] + 1), (GLfloat)g_clip[1], -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// Стан масивів виставляємо ЩОРАЗУ, а не один раз при ініціалізації. Шлях
	// показу у vout_symbian.cpp вмикає GL_TEXTURE_COORD_ARRAY і лишає
	// glTexCoordPointer на масив, який живе в його стековому кадрі. Якщо цей
	// масив лишити ввімкненим, наш виклик малювання читає з нього по координаті
	// на кожну вершину - тобто з чужого, уже звільненого стека.
	glDisable(GL_TEXTURE_2D);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	glClear(GL_COLOR_BUFFER_BIT);
	glVertexPointer(2, GL_SHORT, 0, g_pos);
	glColorPointer(4, GL_UNSIGNED_BYTE, 0, g_col);
	for (TInt b = 0; b < g_nBatch; b++)
		{
		// Примітиви інших ділянок пропускаємо: у цій проекції вони лягли б
		// навмання, а показати їх усе одно нема де.
		if (g_batch[b].iCount <= 0 || g_batch[b].iClip != best)
			continue;
		applyBlend(g_batch[b].iBlend);
		glDrawArrays(GL_TRIANGLES, g_batch[b].iFirst, g_batch[b].iCount);
		}
	glDisable(GL_BLEND);
	// Повертаємо стан, якого чекає шлях показу vout, щоб перемикання прапорця
	// посеред сеансу не лишало його з вимкненими масивами.
	glDisableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnable(GL_TEXTURE_2D);
	voutSymbianGlSwap();

	// Два записи: перший кадр (чи взагалі дійшло) і перша справжня сцена (чи
	// правильні ділянка й склад). Більше не пишемо - лог тут не поперовий.
	if (g_logged < 2 && (g_logged == 0 || g_nVerts > 300))
		{
		ps1LogFmt("glr", "frame%d: clip=%d,%d..%d,%d verts=%d/%d batches=%d clips=%d"
			" over=%d err=%u",
			(int)g_logged, g_clip[0], g_clip[1], g_clip[2], g_clip[3],
			(int)g_clipVerts[best], (int)g_nVerts, (int)g_nBatch, (int)g_nClips,
			(int)g_over, (unsigned)glGetError());
		g_logged++;
		}
	return 1;
	}

// Скільки коштував кадр за складом: для звіту наприкінці прогону.
extern "C" void glrStats(int *aVerts, int *aBatches, int *aOver)
	{
	if (aVerts)   *aVerts = g_nVerts;
	if (aBatches) *aBatches = g_nBatch;
	if (aOver)    *aOver = g_over;
	}
