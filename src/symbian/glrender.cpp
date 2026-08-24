// glrender.cpp - растеризатор PS1 на MBX через OpenGL ES 1.1.
//
// Навіщо: розділ 14 звіту міряє, що той самий кадр Gran Turismo коштує тут
// 6,9 мс проти 14,4 у зв'язці "gpu_unai + блит". Числа з апарата, не з оцінок.
//
// Він приймає ті самі пакети команд, які розбирає gpu_unai, складає з них
// трикутники, розпаковує потрібні сторінки текстур із VRAM гостя і малює.
// Прапорцем swoff програмний прохід можна вимкнути зовсім, і тоді ця сцена -
// єдина; з увімкненим проходом VRAM лишається правдивим для читань і копіювань.
//
// Заміряні обмеження, від яких залежить будова (розділ 14):
//   - виклик малювання коштує 9,5-14 мкс, тому примітиви групуються;
//   - порядок подання міняти НЕ можна (перекриття на PS1 визначає саме він),
//     тому групуються лише СУСІДНІ примітиви з однаковим станом;
//   - зміна прив'язки текстури коштує стільки ж, скільки виклик.
//
// Текстурний атлас 2048x2048 пробували, щоб позбутися розривів пакетів від
// зміни сторінки, але заміри на апараті показали, що це невдале рішення:
// прив'язана текстура 256x256 не коштує нічого, а 2048x2048 коштує 20 мс на
// кадр, бо драйвер не тримає об'єкт на 8 мегабайт (прогін 313 с проти 178 с без
// атласа). Тому лишається одна текстура на сторінку, а виклики малювання
// групуються лише за сусідством.

#include <e32std.h>
#include <f32file.h>
#include <GLES/gl.h>

extern "C" void ps1LogFmt(const char *aType, const char *aFmt, ...);
extern "C" int  voutSymbianGlSwap(void);
extern "C" void voutSymbianGlSize(int *aW, int *aH);
extern "C" void voutSymbianDbgState(int *aFbDirty, int *aFbDirtyArea, int *aStatus,
	int *aSrcX, int *aSrcY, int *aScrW, int *aScrH, int *aVoutOffset, int *aStale);
extern "C" unsigned int ndrc_symbian_fastfreq(void);
extern "C" unsigned int ndrc_symbian_fastcount(void);

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
	EStTwU,           // маска текстурного вікна по U, текселів
	EStTwV,           // маска по V
	EStCount
	};

// Стеля на кадр. Заміряно 324 примітиви на кадр; беремо вчетверо з запасом,
// бо кадр із заставкою або меню може бути щільнішим.
static const TInt KMaxPrims = 1400;
static const TInt KMaxVerts = KMaxPrims * 6;
// Пакетів на кадр. 256 було мало: у Gran Turismo кожен чотирикутник іде зі
// своєю палітрою, тобто зі своїм пакетом, і на межі batchFor мовчки повертався,
// а emitVertex дописував вершини в ОСТАННІЙ пакет - усе після межі малювалося
// його текстурою. На екрані це виглядало як пів кадру, залитого травою.
static const TInt KMaxBatch = 1024;

struct TGlrVert
	{
	GLshort x, y;
	GLshort u, v;
	GLubyte r, g, b, a;
	};

struct TGlrBatch
	{
	TInt iFirst;      // перша вершина
	TInt iCount;      // скільки вершин
	TInt iBlend;      // -1 = непрозорий, інакше режим 0..3
	TInt iTex;        // індекс у кеші текстур, -1 = без текстури
	TInt iClip;       // індекс ділянки малювання, під якою подано пакет
	};

// За кадр ділянка малювання буває не одна: крім заднього буфера трапляються
// перехідні (на боті апарат показав 0,0..640,512). Проекцію будуємо за тією,
// під якою намальовано найбільше вершин, а не за першою і не за найбільшою за
// площею - обидва ті правила вже дали хибну картинку. Примітиви решти ділянок
// не малюємо взагалі: у чужій проекції вони лягли б навмання.
static const TInt KMaxClips = 8;

static TBool     g_on;            // рендерер увімкнено прапорцем
static TBool     g_ready;         // ініціалізовано і є контекст
static TGlrVert *g_v;             // interleaved vertex array (12 bytes per vertex)
static TInt      g_nVerts;
static TGlrBatch g_batch[KMaxBatch];
static TInt      g_nBatch;
static TInt      g_clip[4];       // ділянка малювання, за якою будуємо проекцію
static TInt      g_clips[KMaxClips][4];
static TInt      g_ofs[KMaxClips][2];   // зсув малювання групи: він і задає буфер
static TInt      g_clipVerts[KMaxClips];
static TInt      g_nClips;
static TInt      g_curClip;       // ділянка поточного примітиву, -1 = не влізла
static TInt      g_vpW, g_vpH;
static TInt      g_over;          // скільки примітивів не вмістилось
static TInt      g_logged;      // скільки діагностичних кадрів уже записано

// --- кеш текстур -------------------------------------------------------------
// Заміряно (розділ 14): за кадр гра вживає 12,7 різних пар "сторінка + CLUT" і
// лише 2,88 різних сторінок, а вміст VRAM міняє 0,7 раза на кадр. Тобто кеш на
// кілька десятків записів має влучати майже завжди, і саме на це він і
// розрахований. Три заміряні правила, які визначили будову:
//   - вантажити СТОРІНКАМИ цілком: у ціні завантаження є стала складова
//     0,7 мс, тож сто дрібних шматків коштували б сімдесят мілісекунд;
//   - НЕ писати в текстуру, з якої малює поточний кадр (окремий штраф MBX);
//   - палітровий формат драйвера не вигідний на сторінці, тож розпаковуємо самі.
static const TInt KTexSide   = 256;       // сторінка PS1 - завжди 256x256 текселів
static const TInt KMaxTex    = 64;

struct TGlrTex
	{
	// Стан, який ця розпакована сторінка відтворює. Тримаємо його цілком:
	// згортка в один ключ давала колізію 4 і 8 біт на тексель.
	GLuint  iTex;
	TInt    iTba, iCba, iMode, iMaskU, iMaskV;
	TInt    iFrame;      // останній кадр, у якому з неї малювали
	TBool   iValid;
	// Де ця сторінка лежить у VRAM - потрібне, щоб скидати лише те, що гра
	// справді переписала. Скидання всього кешу коштувало 4,5 мс на кадр:
	// заміряно, бо саме так було зроблено спершу. Прямокутник - саме вікна,
	// а не всіх 256x256: поза вікном сторінка не читається.
	TInt    iX, iY, iW, iH;  // у півсловах VRAM
	TInt    iCx, iCy, iCw;  // палітра: вона теж у VRAM і теж може змінитись
	TInt    iUseU0, iUseV0, iUseU1, iUseV1;
	};

static TGlrTex   g_tex[KMaxTex];
static TInt      g_nTex;
static TUint16  *g_decode;        // буфер розпакування сторінки
static const TUint16 *g_vram;     // VRAM гостя, 1024x512 півслів
static TInt      g_frame;         // лічильник кадрів для витіснення
static TInt      g_miss, g_hit, g_evict;
// Скільки разів текстури не знайшлося місця: усі записи вже зайняті цим самим
// кадром. Такий примітив малюється БЕЗ текстури, і на екрані це виглядає як
// заливка кольором вершини - тиха втрата, яку без лічильника не побачити.
static TInt      g_nofit;
// Скільки разів забракло пакета. Тихе обрізання тут коштувало пів кадру.
static TInt      g_batchOver;
// Чому пакет розривався (зміна текстури, режиму змішування або перехід між буферами).
static TInt      g_brkTex, g_brkBlend, g_brkClip, g_batchSum, g_frameSum;
// Скільки спрайтів вимагають обгортання текстури (u0+w або v0+h за 256).
// Від цього числа залежить, чи можливий текстурний атлас: у ньому обгортання
// пішло б у сусідню сторінку. У полігонів питання не стоїть - там U і V байти.
static TInt      g_spriteWrap, g_spriteAll;
static unsigned long long g_tSetup, g_tClear, g_tDraw, g_tSwap;
static unsigned long long g_useTexels, g_fullTexels, g_useSlots;


// Знімок того, що показав GL. Потрібен саме тут: із вимкненим програмним
// проходом VRAM більше не містить сцени, тож старий шлях знімка (дамп екрана
// через DSA) під GL нічого не бачить, а звірити картинку якось треба.
static char  g_shotPath[64];
static TBool g_shotWanted;

extern "C" void glrShot(const char *aPath)
	{
	if (!aPath)
		return;
	TInt n = 0;
	while (aPath[n] && n < (TInt)sizeof(g_shotPath) - 1)
		{
		g_shotPath[n] = aPath[n];
		n++;
		}
	g_shotPath[n] = 0;
	g_shotWanted = ETrue;
	}

// Читання відбувається ДО обміну буферів: після нього вміст заднього буфера
// не визначений.
static void writeShot(void)
	{
	g_shotWanted = EFalse;
	const TInt bytes = g_vpW * g_vpH * 4;
	TUint8 *buf = (TUint8 *)User::Alloc(bytes);
	if (!buf)
		{
		ps1LogFmt("glr", "shot: alloc %d failed", bytes);
		return;
		}
	glReadPixels(0, 0, g_vpW, g_vpH, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	const GLenum err = glGetError();
	RFs fs;
	TInt rc = fs.Connect();
	if (rc == KErrNone)
		{
		TPtrC8 p8((const TUint8 *)g_shotPath);
		TBuf<64> path;
		path.Copy(p8);
		RFile f;
		rc = f.Replace(fs, path, EFileWrite);
		if (rc == KErrNone)
			{
			TPtrC8 data(buf, bytes);
			rc = f.Write(data);
			f.Close();
			}
		fs.Close();
		}
	User::Free(buf);
	ps1LogFmt("glr", "shot: %s %dx%d rgba rc=%d err=%u",
		g_shotPath, (int)g_vpW, (int)g_vpH, (int)rc, (unsigned)err);
	}

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
	g_v = (TGlrVert *)User::Alloc(KMaxVerts * sizeof(TGlrVert));
	g_decode = (TUint16 *)User::Alloc(KTexSide * KTexSide * 2);
	if (!g_v || !g_decode)
		{
		User::Free(g_v); g_v = NULL;
		User::Free(g_decode); g_decode = NULL;
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
	// Звичайна модуляція. Множник два, якого бракує до правила PS1, вкладено
	// в сам колір вершини (modColor), а не в GL_RGB_SCALE: це розширення
	// текстурного блоку, і драйвер MBX його не застосовував.
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	g_ready = ETrue;
	GLint maxTex = 0, maxUnits = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
	glGetIntegerv(GL_MAX_TEXTURE_UNITS, &maxUnits);
	ps1LogFmt("glr", "init ok: viewport=%dx%d prims<=%d texenv_err=%u"
		" maxTex=%d units=%d vendor=%s renderer=%s", g_vpW, g_vpH,
		(int)KMaxPrims, (unsigned)glGetError(), (int)maxTex, (int)maxUnits,
		(const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER));
	return 0;
	}

extern "C" void glrShutdown(void)
	{
	for (TInt i = 0; i < g_nTex; i++)
		glDeleteTextures(1, &g_tex[i].iTex);
	g_nTex = 0;
	User::Free(g_v); g_v = NULL;
	User::Free(g_decode); g_decode = NULL;
	g_ready = EFalse;
	}

// --- текстури ----------------------------------------------------------------

// Піксель VRAM (1 біт напівпрозорості + 5 B + 5 G + 5 R) у те, що приймає GL як
// GL_UNSIGNED_SHORT_5_5_5_1. Альфа тут означає не напівпрозорість PS1, а
// НАЯВНІСТЬ текселя. Діру задає саме ЗНАЧЕННЯ кольору 0x0000, а не індекс 0
// палітри: gpu_inner.h перевіряє "if (!uSrc)" уже після вибірки з CLUT, тож
// непрозорий чорний (0x8000) під індексом 0 малюється, а нульовий запис під
// будь-яким іншим індексом - ні.
static inline TUint16 texelToGl(TUint16 aV)
	{
	const TUint r = aV & 0x1F, g = (aV >> 5) & 0x1F, b = (aV >> 10) & 0x1F;
	return (TUint16)((r << 11) | (g << 6) | (b << 1) | (aV ? 1 : 0));
	}

// Розпаковує сторінку 256x256 у g_decode. Ширина сторінки у VRAM залежить від
// розрядності: 64 півслова на 4 біти, 128 на 8, 256 на 16.
//
// Маски текстурного вікна вкладаються ПРЯМО В СТОРІНКУ: тексель береться за
// (u & maskU, v & maskV), тобто вікно розмножується на всі 256x256. Так GL із
// GL_REPEAT дає рівно те, що gpu_unai рахує попіксельно (gpu_inner.h маскує
// u і v на кожній вибірці), і зникає потреба маскувати координати вершин -
// маскування самих лише кутів примітиву давало неправильну картинку скрізь, де
// примітив ширший за вікно. Заразом це прибирає читання поза VRAM: сторінка в
// нижній половині плюс зсув вікна давали рядок за межею 512.
static void decodePage(TUint32 aTba, TUint32 aCba, TInt aMode, TInt aMaskU, TInt aMaskV)
	{
	const TUint16 *clut = g_vram + (aCba << 4);
	TUint16 *out = g_decode;
	// Скільки текселів у півслові: 4, 2 або 1.
	const TInt tpw = (aMode == 32) ? 4 : (aMode == 64 ? 2 : 1);
	for (TInt y = 0; y < KTexSide; y++)
		{
		const TUint32 row = aTba + (TUint32)(y & aMaskV) * 1024;
		for (TInt x = 0; x < KTexSide; x++)
			{
			const TInt sx = x & aMaskU;
			const TUint32 off = row + (TUint32)(sx / tpw);
			// Сторінка може впритул підходити до краю VRAM; за межу не читаємо.
			if (off >= 1024u * 512u)
				{ out[x] = 0; continue; }
			const TUint16 w = g_vram[off];
			TUint16 t;
			if (aMode == 32)
				t = clut[(w >> ((sx & 3) << 2)) & 0xF];
			else if (aMode == 64)
				t = clut[(w >> ((sx & 1) << 3)) & 0xFF];
			else
				t = w;
			out[x] = texelToGl(t);
			}
		out += KTexSide;
		}
	}

// Індекс потрібної текстури в кеші; за потреби розпаковує й вантажить.
// Повертає -1, якщо взяти нізвідки (немає VRAM або кеш зайнятий цим кадром).
static TInt texFor(TUint32 aTba, TUint32 aCba, TInt aMode, TInt aMaskU, TInt aMaskV)
	{
	if (!g_vram || !g_decode)
		return -1;
	// Порівнюємо стан цілком, а не його згортку. Згортка тут була помилкою:
	// у "aMode << 27" при aMode 32 і 64 одиниця виходила за 32 біт, тобто
	// розрядність у ключ не потрапляла взагалі, і сторінка 4 біт підмінялася
	// сторінкою 8 біт із тією самою адресою.
	const TInt cba = (aMode == 96) ? 0 : (TInt)aCba;   // 16 біт палітри не читає
	for (TInt i = 0; i < g_nTex; i++)
		if (g_tex[i].iValid && g_tex[i].iTba == (TInt)aTba && g_tex[i].iCba == cba &&
		    g_tex[i].iMode == aMode && g_tex[i].iMaskU == aMaskU &&
		    g_tex[i].iMaskV == aMaskV)
			{
			g_tex[i].iFrame = g_frame;
			g_hit++;
			return i;
			}

	TInt slot = -1;
	if (g_nTex < KMaxTex)
		{
		slot = g_nTex++;
		glGenTextures(1, &g_tex[slot].iTex);
		}
	else
		{
		// Витісняємо найдавніше вжиту - і НІКОЛИ ту, з якої малює цей кадр:
		// запис у задіяну текстуру на MBX серіалізується зі сценою й коштує
		// вдвічі більше за звичайне завантаження (заміряно, розділ 14.5).
		TInt oldest = -1;
		for (TInt i = 0; i < g_nTex; i++)
			if (g_tex[i].iFrame != g_frame &&
			    (oldest < 0 || g_tex[i].iFrame < g_tex[oldest].iFrame))
				oldest = i;
		if (oldest < 0)
			{
			g_nofit++;
			return -1;
			}
		slot = oldest;
		g_evict++;
		}

	decodePage(aTba, aCba, aMode, aMaskU, aMaskV);
	glBindTexture(GL_TEXTURE_2D, g_tex[slot].iTex);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, KTexSide, KTexSide, 0,
		GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, g_decode);
	g_tex[slot].iTba = (TInt)aTba;
	g_tex[slot].iCba = cba;
	g_tex[slot].iMode = aMode;
	g_tex[slot].iMaskU = aMaskU;
	g_tex[slot].iMaskV = aMaskV;
	g_tex[slot].iFrame = g_frame;
	g_tex[slot].iValid = ETrue;
	// Прямокутник, який ця сторінка справді читає: вікно, а не всі 256x256.
	g_tex[slot].iX = (TInt)(aTba % 1024);
	g_tex[slot].iY = (TInt)(aTba / 1024);
	const TInt tpw = (aMode == 32) ? 4 : (aMode == 64 ? 2 : 1);
	g_tex[slot].iW = (aMaskU + 1 + tpw - 1) / tpw;
	g_tex[slot].iH = aMaskV + 1;
	const TUint32 cOff = aCba << 4;
	g_tex[slot].iCx = (TInt)(cOff % 1024);
	g_tex[slot].iCy = (TInt)(cOff / 1024);
	g_tex[slot].iCw = (aMode == 32) ? 16 : (aMode == 64 ? 256 : 0);
	g_miss++;
	return slot;
	}

// VRAM гостя віддає gpu_unai один раз, з renderer_init.
extern "C" void glrSetVram(const void *aVram)
	{
	g_vram = (const TUint16 *)aVram;
	}

// Гра переписала прямокутник VRAM. Скидаємо лише ті сторінки, яких він
// торкнувся: спершу тут стояло скидання всього кешу, і воно коштувало 4,5 мс на
// кадр, бо після кожного запису доводилось наново розпакувати весь робочий
// набір. Прямокутник - у півсловах, як і все у VRAM.
extern "C" void glrInvalidateRect(int aX, int aY, int aW, int aH)
	{
	for (TInt i = 0; i < g_nTex; i++)
		{
		if (!g_tex[i].iValid)
			continue;
		const TBool hitPage =
			!(aX >= g_tex[i].iX + g_tex[i].iW || aX + aW <= g_tex[i].iX) &&
			!(aY >= g_tex[i].iY + g_tex[i].iH || aY + aH <= g_tex[i].iY);
		// Палітра 8 біт - це 256 півслів, і вона може не вміститися в рядок
		// VRAM. Коли переходить на наступний, перевірку по X опускаємо: зайве
		// скидання коштує одне розпакування, пропущене - чужу палітру на екрані.
		const TInt cWrap = (g_tex[i].iCx + g_tex[i].iCw > 1024) ? 1 : 0;
		const TBool hitClut = g_tex[i].iCw > 0 &&
			!(aY >= g_tex[i].iCy + 1 + cWrap || aY + aH <= g_tex[i].iCy) &&
			(cWrap ||
			 !(aX >= g_tex[i].iCx + g_tex[i].iCw || aX + aW <= g_tex[i].iCx));
		if (hitPage || hitClut)
			g_tex[i].iValid = EFalse;
		}
	}

// Палітру теж треба перечитати, а де вона лежить, ми в записі не тримаємо -
// тому зміна стану (перезавантаження регістрів) скидає все.
extern "C" void glrInvalidate(void)
	{
	for (TInt i = 0; i < g_nTex; i++)
		g_tex[i].iValid = EFalse;
	}

// --- складання вершин --------------------------------------------------------

// Відкриває новий пакет, якщо стан змінився. Порядок подання зберігається:
// об'єднуються лише СУСІДНІ примітиви, бо на PS1 пізніший примітив перекриває
// раніший, і переставляти їх не можна.
static void batchFor(TInt aBlend, TInt aTex)
	{
	if (g_nBatch > 0 && g_batch[g_nBatch - 1].iBlend == aBlend
	                 && g_batch[g_nBatch - 1].iTex == aTex
	                 && g_batch[g_nBatch - 1].iClip == g_curClip)
		return;
	if (g_nBatch > 0)
		{
		const TGlrBatch &b = g_batch[g_nBatch - 1];
		if (b.iTex != aTex)          g_brkTex++;
		else if (b.iBlend != aBlend) g_brkBlend++;
		else                         g_brkClip++;
		}
	if (g_nBatch >= KMaxBatch)
		{
		g_batchOver++;
		return;
		}
	g_batch[g_nBatch].iFirst = g_nVerts;
	g_batch[g_nBatch].iCount = 0;
	g_batch[g_nBatch].iBlend = aBlend;
	g_batch[g_nBatch].iTex   = aTex;
	g_batch[g_nBatch].iClip  = g_curClip;
	g_nBatch++;
	}

static inline void emitVertex(TInt aX, TInt aY, TUint32 aBgr, TInt aAlpha,
	TInt aU, TInt aV)
	{
	TGlrVert *v = g_v + g_nVerts;
	v->x = (GLshort)aX;
	v->y = (GLshort)aY;
	v->u = (GLshort)aU;
	v->v = (GLshort)aV;
	v->r = (GLubyte)(aBgr & 0xFF);
	v->g = (GLubyte)((aBgr >> 8) & 0xFF);
	v->b = (GLubyte)((aBgr >> 16) & 0xFF);
	v->a = (GLubyte)aAlpha;
	g_nVerts++;
	g_batch[g_nBatch - 1].iCount++;
	if (g_curClip >= 0)
		g_clipVerts[g_curClip]++;
	}

// Трикутник трьома вершинами; кольори й координати текстури окремі, бо Gouraud
// має різні кольори, а текстурований примітив - різні тексельні координати.
static void emitTri(const TInt *aX, const TInt *aY, const TUint32 *aCol,
	TInt aAlpha, const TInt *aU, const TInt *aV)
	{
	if (g_nVerts + 3 > KMaxVerts) { g_over++; return; }
	emitVertex(aX[0], aY[0], aCol[0], aAlpha, aU[0], aV[0]);
	emitVertex(aX[1], aY[1], aCol[1], aAlpha, aU[1], aV[1]);
	emitVertex(aX[2], aY[2], aCol[2], aAlpha, aU[2], aV[2]);
	}

// Знак координати PS1: одинадцять біт, старші - розширення знаку.
// Зонд "texwhite": текстурований примітив малюється білим кольором вершини,
// тобто на екран іде сама текстура без модуляції. Відділяє хибу кольору від
// хиби розпакування чи завантаження сторінки - інакше їх не розрізнити.
static TBool g_texWhite;
// Бітова маска класів, які НЕ малювати: 1 - полігони, 2 - прямокутники,
// 4 - заливка. Зонд для пошуку того, що залило кадр.
static TInt g_skipMask;

extern "C" void glrSetTexWhite(int aOn)
	{
	g_texWhite = aOn ? ETrue : EFalse;
	}

extern "C" void glrSetSkip(int aMask)
	{
	g_skipMask = aMask;
	}

// PS1 модулює як (tex*col)/128, тобто 0x80 - нейтральний колір; GL множить на
// col/255, тож бракує рівно множника два. Задавати його через GL_RGB_SCALE не
// вийшло: це розширення текстурного блоку, і драйвер MBX його мовчки не
// застосовує - уся текстурована сцена виходила вдвічі темнішою за програмну.
// Тому подвоюємо сам колір вершини, з насиченням, як і на PS1.
static inline TUint32 modColor(TUint32 aC)
	{
	TUint r = (aC & 0xFF) << 1, g = ((aC >> 8) & 0xFF) << 1, b = ((aC >> 16) & 0xFF) << 1;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return r | (g << 8) | (b << 16);
	}
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
	// До якого заднього буфера належить цей примітив. Ділянки самої мало:
	// Gran Turismo тримає обидва буфери під однією ділянкою 0,0..640,480 і
	// розрізняє їх лише зсувом малювання, тож групуємо за парою.
	g_curClip = -1;
	for (TInt i = 0; i < g_nClips; i++)
		if (g_clips[i][0] == aState[EStClipX0] && g_clips[i][1] == aState[EStClipY0] &&
		    g_clips[i][2] == aState[EStClipX1] && g_clips[i][3] == aState[EStClipY1] &&
		    g_ofs[i][0] == aState[EStOfsX] && g_ofs[i][1] == aState[EStOfsY])
			{ g_curClip = i; break; }
	if (g_curClip < 0 && g_nClips < KMaxClips)
		{
		g_curClip = g_nClips++;
		g_clips[g_curClip][0] = aState[EStClipX0];
		g_clips[g_curClip][1] = aState[EStClipY0];
		g_clips[g_curClip][2] = aState[EStClipX1];
		g_clips[g_curClip][3] = aState[EStClipY1];
		g_ofs[g_curClip][0] = aState[EStOfsX];
		g_ofs[g_curClip][1] = aState[EStOfsY];
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
		if (g_skipMask & 1)
			return;
		const TBool tex = (aCmd & 4) != 0;
		const TBool gouraud = (aCmd & 0x10) != 0;
		const TBool quad = (aCmd & 8) != 0;
		const TInt stride = 1 + (tex ? 1 : 0) + (gouraud ? 1 : 0);
		const TInt nv = quad ? 4 : 3;
		TInt x[4], y[4], u[4], v[4];
		TUint32 col[4];
		// "Сира" текстура (біт 0) кольору в пакеті не має - беремо нейтральний
		// для модуляції, інакше вийшли б чорні плями.
		const TUint32 flat = (tex && (aCmd & 1)) ? 0x808080
		                                        : (aPacket[0] & 0xFFFFFF);
		for (TInt i = 0; i < nv; i++)
			{
			const TUint32 c = aPacket[1 + i * stride];
			x[i] = expandSign(c) + ofsX;
			y[i] = expandSign(c >> 16) + ofsY;
			col[i] = gouraud ? (aPacket[i * stride] & 0xFFFFFF) : flat;
			if (tex)
				col[i] = g_texWhite ? 0xFFFFFF : modColor(col[i]);
			if (tex)
				{
				const TUint32 t = aPacket[2 + i * stride];
				// Маски вікна вже вкладені в розпаковану сторінку, тут не чіпаємо:
				// маскування самих лише кутів ламало все, що ширше за вікно.
				u[i] = (TInt)(t & 0xFF);
				v[i] = (TInt)((t >> 8) & 0xFF);
				}
			else
				{ u[i] = 0; v[i] = 0; }
			}
		const TInt ti = tex ? texFor((TUint32)aState[EStTba], (TUint32)aState[EStCba],
			aState[EStTextMode], aState[EStTwU], aState[EStTwV]) : -1;
		if (ti < 0)
			{
			for (TInt i = 0; i < nv; i++)
				{ u[i] = 0; v[i] = 0; }
			}
		else if (tex)
			{
			for (TInt i = 0; i < nv; i++)
				{
				if (u[i] < g_tex[ti].iUseU0) g_tex[ti].iUseU0 = u[i];
				if (u[i] > g_tex[ti].iUseU1) g_tex[ti].iUseU1 = u[i];
				if (v[i] < g_tex[ti].iUseV0) g_tex[ti].iUseV0 = v[i];
				if (v[i] > g_tex[ti].iUseV1) g_tex[ti].iUseV1 = v[i];
				}
			}
		batchFor(blend, ti);
		TInt tx[3], ty[3], tu[3], tv[3]; TUint32 tc[3];
		for (TInt k = 0; k < 3; k++)
			{ tx[k]=x[k]; ty[k]=y[k]; tc[k]=col[k]; tu[k]=u[k]; tv[k]=v[k]; }
		emitTri(tx, ty, tc, alpha, tu, tv);
		if (quad)
			{
			for (TInt k = 0; k < 3; k++)
				{
				const TInt s = k + 1;
				tx[k]=x[s]; ty[k]=y[s]; tc[k]=col[s]; tu[k]=u[s]; tv[k]=v[s];
				}
			emitTri(tx, ty, tc, alpha, tu, tv);
			}
		return;
		}

	if (aCmd >= 0x60 && aCmd <= 0x7F)
		{
		if (g_skipMask & 2)
			return;
		// Прямокутник. Розмір задають біти 3-4 коду: 0 - у пакеті, інакше
		// стала сторона 1, 8 або 16.
		const TBool tex = (aCmd & 4) != 0;
		const TUint32 xy = aPacket[1];
		// У прямокутника gpu_unai розширює знак ВІД СУМИ з зсувом
		// (gpu_raster_sprite.h), а не від самої координати.
		const TInt x0 = expandSign((xy & 0xFFFF) + (TUint32)ofsX);
		const TInt y0 = expandSign((xy >> 16) + (TUint32)ofsY);
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
		TUint32 c = (tex && (aCmd & 1)) ? 0x808080 : (aPacket[0] & 0xFFFFFF);
		if (tex)
			c = g_texWhite ? 0xFFFFFF : modColor(c);
		// У прямокутника координата текстури задана лише для лівого верхнього
		// кута, решта - його розміром.
		TInt u0 = 0, v0 = 0, uw = 0, vh = 0;
		if (tex)
			{
			u0 = (TInt)(aPacket[2] & 0xFF);
			v0 = (TInt)((aPacket[2] >> 8) & 0xFF);
			uw = w;
			vh = h;
			g_spriteAll++;
			if (u0 + w > 256 || v0 + h > 256)
				g_spriteWrap++;
			}
		const TInt ti = tex ? texFor((TUint32)aState[EStTba], (TUint32)aState[EStCba],
			aState[EStTextMode], aState[EStTwU], aState[EStTwV]) : -1;
		if (ti < 0)
			{
			u0 = 0; v0 = 0; uw = 0; vh = 0;
			}
		else if (tex)
			{
			TInt u1 = u0 + w - 1;
			TInt v1 = v0 + h - 1;
			if (u1 > 255) u1 = 255;
			if (v1 > 255) v1 = 255;
			if (u0 < g_tex[ti].iUseU0) g_tex[ti].iUseU0 = u0;
			if (u1 > g_tex[ti].iUseU1) g_tex[ti].iUseU1 = u1;
			if (v0 < g_tex[ti].iUseV0) g_tex[ti].iUseV0 = v0;
			if (v1 > g_tex[ti].iUseV1) g_tex[ti].iUseV1 = v1;
			}
		batchFor(blend, ti);
		TInt tx[3], ty[3], tu[3], tv[3]; TUint32 tc[3];
		tc[0] = tc[1] = tc[2] = c;
		tx[0]=x0;   ty[0]=y0;   tu[0]=u0;    tv[0]=v0;
		tx[1]=x0+w; ty[1]=y0;   tu[1]=u0+uw; tv[1]=v0;
		tx[2]=x0;   ty[2]=y0+h; tu[2]=u0;    tv[2]=v0+vh;
		emitTri(tx, ty, tc, alpha, tu, tv);
		tx[0]=x0+w; ty[0]=y0;   tu[0]=u0+uw; tv[0]=v0;
		tx[1]=x0+w; ty[1]=y0+h; tu[1]=u0+uw; tv[1]=v0+vh;
		tx[2]=x0;   ty[2]=y0+h; tu[2]=u0;    tv[2]=v0+vh;
		emitTri(tx, ty, tc, alpha, tu, tv);
		return;
		}

	if (aCmd == 0x02)
		{
		if (g_skipMask & 4)
			return;
		// Заливка. Вона єдина не зважає ні на зсув, ні на відсікання, і завжди
		// непрозора - це не примітив, а запис у VRAM.
		const TInt x0 = (TInt)(aPacket[1] & 0x3F0);
		const TInt y0 = (TInt)((aPacket[1] >> 16) & 0x1FF);
		const TInt w = (TInt)(((aPacket[2] & 0x3FF) + 15) & ~15);
		const TInt h = (TInt)((aPacket[2] >> 16) & 0x1FF);
		if (w <= 0 || h <= 0)
			return;
		const TUint32 c = aPacket[0] & 0xFFFFFF;
		batchFor(-1, -1);
		TInt tx[3], ty[3]; TUint32 tc[3];
		const TInt z[3] = { 0, 0, 0 };
		tc[0] = tc[1] = tc[2] = c;
		tx[0]=x0;   ty[0]=y0;   tx[1]=x0+w; ty[1]=y0;   tx[2]=x0; ty[2]=y0+h;
		emitTri(tx, ty, tc, 255, z, z);
		tx[0]=x0+w; ty[0]=y0;   tx[1]=x0+w; ty[1]=y0+h; tx[2]=x0; ty[2]=y0+h;
		emitTri(tx, ty, tc, 255, z, z);
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
	g_frame++;
	for (TInt i = 0; i < KMaxTex; i++)
		{
		g_tex[i].iUseU0 = 0x7FFFFFFF;
		g_tex[i].iUseV0 = 0x7FFFFFFF;
		g_tex[i].iUseU1 = -1;
		g_tex[i].iUseV1 = -1;
		}
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
	// Задній буфер - група з найбільшою кількістю вершин.
	TInt best = 0;
	for (TInt i = 1; i < g_nClips; i++)
		if (g_clipVerts[i] > g_clipVerts[best])
			best = i;
	// Рамка кадру - НЕ ділянка малювання. Gran Turismo тримає ділянку
	// 0,0..640,480 на обидва буфери 320x240, тож проекція за нею клала сцену в
	// чверть екрана, а другий буфер - у сусідню чверть. Правильна рамка - зсув
	// малювання цієї групи (початок буфера) плюс розмір того, що GPU показує.
	TInt dw = 0, dh = 0;
	voutSymbianDbgState(NULL, NULL, NULL, NULL, NULL, &dw, &dh, NULL, NULL);
	if (dw <= 0 || dh <= 0)
		{ dw = 320; dh = 240; }
	g_clip[0] = g_ofs[best][0];
	g_clip[1] = g_ofs[best][1];
	g_clip[2] = g_clip[0] + dw;
	g_clip[3] = g_clip[1] + dh;

	// Вікно на цьому апараті обертається саме собою, а поле зору ми виставили
	// один раз при ініціалізації. Після повороту воно лишалося від старого
	// розміру - саме той збій, що вже траплявся в Hexen 2.
	TInt vw = 0, vh = 0;
	voutSymbianGlSize(&vw, &vh);
	if (vw > 0 && vh > 0 && (vw != g_vpW || vh != g_vpH))
		{
		g_vpW = vw; g_vpH = vh;
		glViewport(0, 0, g_vpW, g_vpH);
		ps1LogFmt("glr", "viewport changed: %dx%d", (int)g_vpW, (int)g_vpH);
		}

	const unsigned int tSetup0 = ndrc_symbian_fastcount();
	// Задній буфер PS1 лягає на все вікно. Вісь Y донизу, як у VRAM.
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof((GLfloat)g_clip[0], (GLfloat)g_clip[2],
	         (GLfloat)g_clip[3], (GLfloat)g_clip[1], -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// Стан масивів і текстури виставляємо ЩОРАЗУ, а не один раз при ініціалізації.
	// Шлях показу у vout_symbian.cpp вмикає GL_TEXTURE_COORD_ARRAY і лишає
	// glTexCoordPointer на масив, який живе в його стековому кадрі.
	glDisable(GL_TEXTURE_2D);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	// Індекс 0 палітри на PS1 - діра, а не колір. У розпакованій текстурі він
	// дістав альфу 0, тож відсікаємо його перевіркою, а не змішуванням: змішати
	// довелося б і там, де примітив непрозорий.
	//
	// Поріг мусить лежати НИЖЧЕ за найменший коефіцієнт змішування. GL_MODULATE
	// множить і альфу теж, а альфа вершини в нас несе сам коефіцієнт: 128 для
	// режиму B/2+F/2 і 64 для B+F/4. При порозі 0,5 режим B+F/4 відсікався б
	// цілком, а B/2+F/2 проходив із запасом в один крок квантування. Відсікати
	// треба саме нуль, тому поріг близький до нуля.
	glAlphaFunc(GL_GREATER, 0.05f);
	const unsigned int tSetup1 = ndrc_symbian_fastcount();
	g_tSetup += (unsigned int)(tSetup1 - tSetup0);

	const unsigned int tClear0 = ndrc_symbian_fastcount();
	glClear(GL_COLOR_BUFFER_BIT);
	const unsigned int tClear1 = ndrc_symbian_fastcount();
	g_tClear += (unsigned int)(tClear1 - tClear0);

	const unsigned int tSetup2 = ndrc_symbian_fastcount();
	// Texture coords are raw PS1 texels; the divide by 256 is done by the texture
	// matrix. It must be set here and cleared below: the full-screen present path
	// in vout_symbian.cpp shares this context and passes normalised coords.
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glScalef(1.0f / 256.0f, 1.0f / 256.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glVertexPointer(2, GL_SHORT, sizeof(TGlrVert), &g_v[0].x);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(TGlrVert), &g_v[0].r);
	glTexCoordPointer(2, GL_SHORT, sizeof(TGlrVert), &g_v[0].u);
	const unsigned int tSetup3 = ndrc_symbian_fastcount();
	g_tSetup += (unsigned int)(tSetup3 - tSetup2);

	// Зонди для розподілу вартості між масивами вершин: 32 - без кольору,
	// 64 - без текстури. Картинка при цьому неправильна, це саме зонд.
	if (g_skipMask & 32)
		glDisableClientState(GL_COLOR_ARRAY);
	if (g_skipMask & 64)
		{
		glDisable(GL_TEXTURE_2D);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
	// Зонди для розділення двох причин вартості малювання. Біт 8 лишає ту саму
	// геометрію, але відсікає її до одного пікселя: якщо час не впав, платимо за
	// обробку вершин, а не за заповнення. Біт 16 не малює зовсім - лишається
	// чиста ціна обміну буферів.
	if (g_skipMask & 8)
		{
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, 0, 1, 1);
		}
	const unsigned int tDraw0 = ndrc_symbian_fastcount();
	TInt curTex = -2;
	TInt curBlend = -2;
	for (TInt b = 0; (g_skipMask & 16) == 0 && b < g_nBatch; b++)
		{
		// Примітиви інших ділянок пропускаємо: у цій проекції вони лягли б
		// навмання, а показати їх усе одно нема де.
		if (g_batch[b].iCount <= 0 || g_batch[b].iClip != best)
			continue;
		if (g_batch[b].iTex != curTex)
			{
			curTex = g_batch[b].iTex;
			if (curTex >= 0 && !(g_skipMask & 64))
				{
				glEnable(GL_TEXTURE_2D);
				glEnable(GL_ALPHA_TEST);
				glBindTexture(GL_TEXTURE_2D, g_tex[curTex].iTex);
				}
			else
				{
				glDisable(GL_TEXTURE_2D);
				glDisable(GL_ALPHA_TEST);
				}
			}
		if (g_batch[b].iBlend != curBlend)
			{
			curBlend = g_batch[b].iBlend;
			applyBlend(curBlend);
			}
		glDrawArrays(GL_TRIANGLES, g_batch[b].iFirst, g_batch[b].iCount);
		}
	const unsigned int tDraw1 = ndrc_symbian_fastcount();
	g_tDraw += (unsigned int)(tDraw1 - tDraw0);
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	if (g_skipMask & 8)
		glDisable(GL_SCISSOR_TEST);

	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	// Повертаємо стан, якого чекає шлях показу vout, щоб перемикання прапорця
	// посеред сеансу не лишало його з вимкненими масивами.
	glDisableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnable(GL_TEXTURE_2D);
	// Разом зі знімком - опис усіх текстур, з яких цей кадр малював. Маючи їх
	// і дамп VRAM того самого кадру, ту саму сторінку можна розпакувати на
	// хості й звірити: інакше не відрізнити хибу розпакування від хиби
	// координат чи від того, що текстура справді така.
	if (g_shotWanted)
		{
		for (TInt i = 0; i < g_nTex; i++)
			if (g_tex[i].iValid && g_tex[i].iFrame == g_frame)
				ps1LogFmt("glrtex", "slot=%d tba=%d cba=%d mode=%d mu=%d mv=%d"
					" x=%d y=%d w=%d h=%d",
					(int)i, g_tex[i].iTba, g_tex[i].iCba, g_tex[i].iMode,
					g_tex[i].iMaskU, g_tex[i].iMaskV,
					g_tex[i].iX, g_tex[i].iY, g_tex[i].iW, g_tex[i].iH);
		for (TInt b = 0; b < g_nBatch && b < 12; b++)
			ps1LogFmt("glrbatch", "b=%d n=%d tex=%d blend=%d clip=%d",
				(int)b, g_batch[b].iCount, (int)g_batch[b].iTex,
				g_batch[b].iBlend, g_batch[b].iClip);
		}
	for (TInt i = 0; i < KMaxTex; i++)
		{
		if (g_tex[i].iUseU1 >= g_tex[i].iUseU0 && g_tex[i].iUseV1 >= g_tex[i].iUseV0)
			{
			const unsigned long long tw = (unsigned long long)(g_tex[i].iUseU1 - g_tex[i].iUseU0 + 1);
			const unsigned long long th = (unsigned long long)(g_tex[i].iUseV1 - g_tex[i].iUseV0 + 1);
			g_useTexels += tw * th;
			g_fullTexels += 65536ULL;
			g_useSlots++;
			}
		}
	g_batchSum += g_nBatch;
	g_frameSum++;
	if (g_shotWanted)
		writeShot();

	const unsigned int tSwap0 = ndrc_symbian_fastcount();
	voutSymbianGlSwap();
	const unsigned int tSwap1 = ndrc_symbian_fastcount();
	g_tSwap += (unsigned int)(tSwap1 - tSwap0);

	// Два записи: перший кадр (чи взагалі дійшло) і перша справжня сцена (чи
	// правильні ділянка й склад). Більше не пишемо - лог тут не поперовий.
	if (g_logged < 4 && (g_logged == 0 || g_nVerts > 300))
		{
		ps1LogFmt("glr", "frame%d: frame=%d,%d..%d,%d verts=%d/%d batches=%d clips=%d"
			" tex=%d hit=%d miss=%d evict=%d nofit=%d bover=%d over=%d vp=%dx%d err=%u",
			(int)g_logged, g_clip[0], g_clip[1], g_clip[2], g_clip[3],
			(int)g_clipVerts[best], (int)g_nVerts, (int)g_nBatch, (int)g_nClips,
			(int)g_nTex, (int)g_hit, (int)g_miss, (int)g_evict, (int)g_nofit, (int)g_batchOver,
			(int)g_over, (int)g_vpW, (int)g_vpH, (unsigned)glGetError());
		g_logged++;
		}
	return 1;
	}

// Скільки коштував кадр за складом: для звіту наприкінці прогону.
extern "C" void glrStats(int *aVerts, int *aBatches, int *aOver,
	int *aHit, int *aMiss, int *aEvict)
	{
	if (aVerts)   *aVerts = g_nVerts;
	if (aBatches) *aBatches = g_nBatch;
	if (aOver)    *aOver = g_over;
	if (aHit)     *aHit = g_hit;
	if (aMiss)    *aMiss = g_miss;
	if (aEvict)   *aEvict = g_evict;
	if (aOver)    *aOver = g_nofit + g_batchOver;   // тихі втрати обох видів
	ps1LogFmt("glrbrk", "batches=%d frames=%d brkTex=%d brkBlend=%d brkClip=%d"
		" spriteWrap=%d/%d",
		(int)g_batchSum, (int)g_frameSum, (int)g_brkTex, (int)g_brkBlend,
		(int)g_brkClip, (int)g_spriteWrap, (int)g_spriteAll);
	const unsigned int freq = ndrc_symbian_fastfreq();
	const unsigned int f = freq ? freq : 1;
	ps1LogFmt("glrtime", "setup=%ums clear=%ums draw=%ums swap=%ums frames=%d",
		(unsigned int)(g_tSetup * 1000ULL / f),
		(unsigned int)(g_tClear * 1000ULL / f),
		(unsigned int)(g_tDraw * 1000ULL / f),
		(unsigned int)(g_tSwap * 1000ULL / f),
		(int)g_frameSum);
	ps1LogFmt("glruv", "use=%uK full=%uK slots=%d frames=%d",
		(unsigned int)(g_useTexels / 1024ULL),
		(unsigned int)(g_fullTexels / 1024ULL),
		(int)(g_useSlots / (g_frameSum ? g_frameSum : 1)),
		(int)g_frameSum);
	}
