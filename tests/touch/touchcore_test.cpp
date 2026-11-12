// Перевірки touchcore.h: екранний геймпад (Vpad*) і кінетичне гортання (KScroll*).
// Очікувані значення виведено зі специфікації заголовка вручну; розрахунок наведено
// в коментарях. Посилання на заголовок — за назвами правил, без номерів рядків і сторінок.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "touchcore.h"

static int gChecks = 0;
static int gFails = 0;
static char gWhere[200] = "";
static const char *gTest = "";

#define CHECK(cond) \
	do { \
		++gChecks; \
		if (!(cond)) { ++gFails; printf("FAIL [%s] %s:%d: %s\n", gWhere, __FILE__, __LINE__, #cond); } \
	} while (0)

#define CHECK_EQ(actual, expected) \
	do { \
		long a_ = (long)(actual); \
		long e_ = (long)(expected); \
		++gChecks; \
		if (a_ != e_) { \
			++gFails; \
			printf("FAIL [%s] %s:%d: %s == %s: expected %ld, got %ld\n", \
				gWhere, __FILE__, __LINE__, #actual, #expected, e_, a_); \
		} \
	} while (0)

static void Run(const char *aName, void (*aFn)())
{
	gTest = aName;
	strcpy(gWhere, aName);
	aFn();
}

// Прапорці командного рядка: --list друкує імена тестів, --only NAME запускає один тест.
// Кожен тест виконується окремим процесом із межею часу, тож зависання в реалізації стає
// невдалим тестом з назвою, а не зупинкою всього прогону.
static const char *gOnly = 0;
static int gListOnly = 0;

#define RUN(fn) \
	do { \
		if (gListOnly) printf("TEST %s\n", #fn); \
		else if (gOnly == 0 || strcmp(gOnly, #fn) == 0) Run(#fn, fn); \
	} while (0)

// ---------------------------------------------------------------- допоміжні

static void SetRectCtl(TVpadControl &aC, int aCx, int aCy, int aHw, int aHh, int aGroup)
{
	memset(&aC, 0, sizeof aC);
	aC.iKind = (unsigned char)EVpadKindButton;
	aC.iShape = (unsigned char)EVpadShapeRect;
	aC.iGroup = (unsigned char)aGroup;
	aC.iBits = 1;
	aC.iCx = (short)aCx;
	aC.iCy = (short)aCy;
	aC.iRx = (short)aHw;
	aC.iRy = (short)aHh;
}

static void SetCircCtl(TVpadControl &aC, int aCx, int aCy, int aR, int aGroup, unsigned aBits)
{
	memset(&aC, 0, sizeof aC);
	aC.iKind = (unsigned char)EVpadKindButton;
	aC.iShape = (unsigned char)EVpadShapeCircle;
	aC.iGroup = (unsigned char)aGroup;
	aC.iBits = (unsigned short)aBits;
	aC.iCx = (short)aCx;
	aC.iCy = (short)aCy;
	aC.iRx = (short)aR;
}

// Параметри, які заголовок задає за замовчуванням.
static void LayoutInit(TVpadLayout &aL, int aCount)
{
	memset(&aL, 0, sizeof aL);
	aL.iCount = aCount;
	aL.iHitPct = 125;
	aL.iLeashPct = 175;
	aL.iDeadPct = 18;
	aL.iDiagPct = 100;
	aL.iFourWayPct = 40;
	aL.iStickDigitalPct = 80;
	aL.iStickFloat = 0;
	aL.iDualJumpPx = 0;
}

// Кількість елементів, яку береться в розрахунок (заголовок): від'ємне дає 0, більше
// за KVpadMaxControls дає KVpadMaxControls. Цикли тесту ходять лише по цій кількості, щоб не
// вийти за межі масиву iC навіть тоді, коли тест перевіряє саме це правило.
static int TakenCount(const TVpadLayout &aL)
{
	if (aL.iCount < 0) return 0;
	if (aL.iCount > KVpadMaxControls) return KVpadMaxControls;
	return aL.iCount;
}

static int SameLayout(const TVpadLayout &aA, const TVpadLayout &aB)
{
	int i;
	if (aA.iCount != aB.iCount) return 0;
	for (i = 0; i < TakenCount(aA); ++i) {
		if (memcmp(&aA.iC[i], &aB.iC[i], sizeof(TVpadControl)) != 0) return 0;
	}
	return aA.iHitPct == aB.iHitPct && aA.iLeashPct == aB.iLeashPct
		&& aA.iDeadPct == aB.iDeadPct && aA.iDiagPct == aB.iDiagPct
		&& aA.iFourWayPct == aB.iFourWayPct && aA.iStickDigitalPct == aB.iStickDigitalPct
		&& aA.iStickFloat == aB.iStickFloat && aA.iDualJumpPx == aB.iDualJumpPx;
}

// Перевіряє один елемент розкладки; iRy лише для прямокутників.
static void CheckCtl(const TVpadLayout &aL, int aI, int aKind, int aShape, int aGroup,
	unsigned aBits, int aCx, int aCy, int aRx, int aRy)
{
	const TVpadControl &c = aL.iC[aI];
	sprintf(gWhere, "%s ctl %d", gTest, aI);
	CHECK_EQ(c.iKind, aKind);
	CHECK_EQ(c.iShape, aShape);
	CHECK_EQ(c.iGroup, aGroup);
	CHECK_EQ(c.iBits, aBits);
	CHECK_EQ(c.iCx, aCx);
	CHECK_EQ(c.iCy, aCy);
	CHECK_EQ(c.iRx, aRx);
	if (aShape == EVpadShapeRect) CHECK_EQ(c.iRy, aRy);
	strcpy(gWhere, gTest);
}

// ---------------------------------------------------------------- VpadIsqrt

static void TestIsqrtSmallAndLargeValues()
{
	// Largest r with r*r <= v. Значення беруться з таблиці квадратів.
	CHECK_EQ(VpadIsqrt(0), 0);
	CHECK_EQ(VpadIsqrt(1), 1);
	CHECK_EQ(VpadIsqrt(2), 1);
	CHECK_EQ(VpadIsqrt(3), 1);
	CHECK_EQ(VpadIsqrt(4), 2);
	CHECK_EQ(VpadIsqrt(8), 2);
	CHECK_EQ(VpadIsqrt(9), 3);
	CHECK_EQ(VpadIsqrt(15), 3);
	CHECK_EQ(VpadIsqrt(16), 4);
	// 46340^2 = 2147395600; 46341^2 = 2147488281 > INT_MAX.
	CHECK_EQ(VpadIsqrt(2147395599), 46339);
	CHECK_EQ(VpadIsqrt(2147395600), 46340);
	CHECK_EQ(VpadIsqrt(2147483647), 46340);
}

static void TestIsqrtSquaresAndNeighbours()
{
	// Для кожного r: isqrt(r*r) == r, isqrt(r*r - 1) == r - 1, isqrt(r*r + 1) == r при r >= 1.
	int r;
	for (r = 0; r <= 46340; r += 7) {
		int v = r * r;
		CHECK_EQ(VpadIsqrt(v), r);
		if (r >= 1) CHECK_EQ(VpadIsqrt(v - 1), r - 1);
		if (r >= 1) CHECK_EQ(VpadIsqrt(v + 1), r);
	}
	CHECK_EQ(VpadIsqrt(46340 * 46340), 46340);
}

// ---------------------------------------------------------------- VpadDefaultLayout

static void TestDefaultLandscape640x360Scale100()
{
	// Розрахунок за правилами розмірів при px(t) = (t*100 + 138/2) / 138:
	// m=px(20)=14, g=px(10)=7, Rd=px(110)=80, rb=px(45)=33, Rr=px(75)=54,
	// tw=px(50)=36, th=px(35)=25, rm=25, sw=px(70)=51, sh=px(40)=29.
	// F = (640-14-54-33, 360-14-54-33) = (539, 259). Select x = 320-4-36 = 280,
	// Start x = 320+4+36 = 360 ((g+1)/2 = 4), обидва y = 360-14-25 = 321.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	CHECK_EQ(VpadDefaultLayout(L, 640, 360, 138, 100, 0), 100);
	CHECK_EQ(L.iCount, EVpadDefaultCount);
	CHECK_EQ(L.iHitPct, 125);
	CHECK_EQ(L.iLeashPct, 175);
	CHECK_EQ(L.iDeadPct, 18);
	CHECK_EQ(L.iDiagPct, 100);
	CHECK_EQ(L.iFourWayPct, 40);
	CHECK_EQ(L.iStickDigitalPct, 80);
	CHECK_EQ(L.iStickFloat, 0);
	CHECK_EQ(L.iStickClickMs, 300);
	CHECK_EQ(L.iDualJumpPx, 0);
	CheckCtl(L, EVpadIxDir, EVpadKindDpad, EVpadShapeCircle, 1, 0, 94, 266, 80, 0);
	CheckCtl(L, EVpadIxTriangle, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadTriangle, 539, 205, 33, 0);
	CheckCtl(L, EVpadIxCircle, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCircle, 593, 259, 33, 0);
	CheckCtl(L, EVpadIxCross, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCross, 539, 313, 33, 0);
	CheckCtl(L, EVpadIxSquare, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadSquare, 485, 259, 33, 0);
	CheckCtl(L, EVpadIxL1, EVpadKindButton, EVpadShapeRect, 3, 1 << EVpadL1, 65, 43, 51, 29);
	CheckCtl(L, EVpadIxL2, EVpadKindButton, EVpadShapeRect, 3, 1 << EVpadL2, 174, 43, 51, 29);
	CheckCtl(L, EVpadIxR1, EVpadKindButton, EVpadShapeRect, 4, 1 << EVpadR1, 575, 43, 51, 29);
	CheckCtl(L, EVpadIxR2, EVpadKindButton, EVpadShapeRect, 4, 1 << EVpadR2, 466, 43, 51, 29);
	CheckCtl(L, EVpadIxSelect, EVpadKindButton, EVpadShapeRect, 0, 1 << EVpadSelect, 280, 321, 36, 25);
	CheckCtl(L, EVpadIxStart, EVpadKindButton, EVpadShapeRect, 0, 1 << EVpadStart, 360, 321, 36, 25);
	CheckCtl(L, EVpadIxMenu, EVpadKindAction, EVpadShapeCircle, 0, 0, 320, 39, 25, 0);
	CHECK_EQ(L.iC[EVpadIxMenu].iId, 0);
	CHECK(VpadLayoutFits(L, 640, 360));
}

static void TestDefaultPortrait360x640ScaleFallsTo78()
{
	// 360x640, U=111, aScalePct=100. При s=100 L2 (x 109..191) перетинає R2 (x 169..251),
	// обидва в різних групах: масштаб 100 не вміщується.
	// Далі 2*(m+Rd+Rr+rb) < 360 є потрібною умовою, щоб Dir не перетинав Square
	// (обидва на одній висоті). Значення для s: 97..81 дають суми 2*(...)=
	// 96: 432, 93: 420, 90: 404, 87: 392, 84: 378, 81: 366, усі >= 360.
	// s=78: m=14, Rd=77, Rr=53, rb=32, 2*(14+77+53+32)=352 < 360, тож вміщується.
	// Перебір дає i=7, s = 100*(32-7)/32 = 78.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	CHECK_EQ(VpadDefaultLayout(L, 360, 640, 111, 100, 0), 78);
	// s=78: g=7, sw=rb=32, sh=28, tw=35, th=rm=25, yP=270, y1=270+14+28=312,
	// yMain=((312+28+7)+(640-14-50-7))/2=(347+569)/2=458.
	CheckCtl(L, EVpadIxDir, EVpadKindDpad, EVpadShapeCircle, 1, 0, 91, 458, 77, 0);
	CheckCtl(L, EVpadIxTriangle, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadTriangle, 261, 405, 32, 0);
	CheckCtl(L, EVpadIxCircle, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCircle, 314, 458, 32, 0);
	CheckCtl(L, EVpadIxCross, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCross, 261, 511, 32, 0);
	CheckCtl(L, EVpadIxSquare, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadSquare, 208, 458, 32, 0);
	CheckCtl(L, EVpadIxL1, EVpadKindButton, EVpadShapeRect, 3, 1 << EVpadL1, 46, 312, 32, 28);
	CheckCtl(L, EVpadIxL2, EVpadKindButton, EVpadShapeRect, 3, 1 << EVpadL2, 117, 312, 32, 28);
	CheckCtl(L, EVpadIxR1, EVpadKindButton, EVpadShapeRect, 4, 1 << EVpadR1, 314, 312, 32, 28);
	CheckCtl(L, EVpadIxR2, EVpadKindButton, EVpadShapeRect, 4, 1 << EVpadR2, 243, 312, 32, 28);
	CheckCtl(L, EVpadIxSelect, EVpadKindButton, EVpadShapeRect, 0, 1 << EVpadSelect, 113, 601, 35, 25);
	CheckCtl(L, EVpadIxStart, EVpadKindButton, EVpadShapeRect, 0, 1 << EVpadStart, 247, 601, 35, 25);
	CheckCtl(L, EVpadIxMenu, EVpadKindAction, EVpadShapeCircle, 0, 0, 180, 601, 25, 0);
	CHECK(VpadLayoutFits(L, 360, 640));
}

static void TestDefaultOptionStickReplacesDpad()
{
	// EVpadOptStick: лівий елемент — аналоговий стік, розміром і місцем як d-pad.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	CHECK_EQ(VpadDefaultLayout(L, 640, 360, 138, 100, EVpadOptStick), 100);
	CheckCtl(L, EVpadIxDir, EVpadKindStick, EVpadShapeCircle, 1, 0, 94, 266, 80, 0);
	CHECK_EQ(L.iStickFloat, 0);
	CHECK_EQ(L.iStickClickMs, 300);
	CHECK_EQ(L.iDualJumpPx, 0);
}

static void TestDefaultOptionResistiveDualJump()
{
	// EVpadOptResistiveDual: iDualJumpPx = px(120) при остаточному масштабі.
	// Ландшафт, s=100, U=138: (12000+69)/138 = 87. Портрет, s=78, U=111: (9360+55)/111 = 84.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	CHECK_EQ(VpadDefaultLayout(L, 640, 360, 138, 100, EVpadOptResistiveDual), 100);
	CHECK_EQ(L.iDualJumpPx, 87);
	memset(&L, 0, sizeof L);
	CHECK_EQ(VpadDefaultLayout(L, 360, 640, 111, 100, EVpadOptResistiveDual), 78);
	CHECK_EQ(L.iDualJumpPx, 84);
}

static void TestDefaultClampsOfUmPerPxAndScalePct()
{
	// aUmPerPx < 20 береться як 20; aScalePct обрізається до 25..400.
	TVpadLayout a, b;
	int ra, rb;
	ra = VpadDefaultLayout(a, 640, 360, 5, 100, 0);
	rb = VpadDefaultLayout(b, 640, 360, 20, 100, 0);
	CHECK_EQ(ra, rb);
	CHECK(SameLayout(a, b));
	ra = VpadDefaultLayout(a, 640, 360, 138, 1, 0);
	rb = VpadDefaultLayout(b, 640, 360, 138, 25, 0);
	CHECK_EQ(ra, rb);
	CHECK(SameLayout(a, b));
	ra = VpadDefaultLayout(a, 640, 360, 138, 1000, 0);
	rb = VpadDefaultLayout(b, 640, 360, 138, 400, 0);
	CHECK_EQ(ra, rb);
	CHECK(SameLayout(a, b));
	// 25% — перший кандидат і на цьому екрані вміщується (розрахунок: m=4, Rd=20,
	// лівий/правий плечі в межах), тож результат дорівнює 25.
	ra = VpadDefaultLayout(a, 640, 360, 138, 1, 0);
	CHECK_EQ(ra, 25);
	CHECK(VpadLayoutFits(a, 640, 360));
}

static void TestDefaultReturnsZeroWhenNothingFits()
{
	// 40x40 при s=25..10: найменший кандидат s=10, U=20 дає Rd=55,
	// тобто діаметр d-pad 110 px на екрані 40 px. Повертається 0.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	CHECK_EQ(VpadDefaultLayout(L, 40, 40, 20, 25, 0), 0);
	CHECK_EQ(VpadLayoutFits(L, 40, 40), 0);
}

static void TestDefaultOtherScreensFitWhenReturned()
{
	// Властивість: якщо повернуто не нуль, розкладка задовольняє VpadLayoutFits, а
	// масштаб лежить у 1..aScalePct.
	static const int W[6] = { 320, 240, 800, 640, 640, 360 };
	static const int H[6] = { 240, 320, 352, 360, 360, 640 };
	static const int U[6] = { 200, 200, 100, 138, 138, 111 };
	static const int P[6] = { 100, 100, 100, 100, 50, 100 };
	int i;
	for (i = 0; i < 6; ++i) {
		TVpadLayout L;
		int r;
		memset(&L, 0, sizeof L);
		r = VpadDefaultLayout(L, W[i], H[i], U[i], P[i], EVpadOptStick | EVpadOptResistiveDual);
		sprintf(gWhere, "%s screen %d", gTest, i);
		CHECK(r >= 0 && r <= P[i]);
		if (r != 0) CHECK(VpadLayoutFits(L, W[i], H[i]));
		strcpy(gWhere, gTest);
	}
}

// ---------------------------------------------------------------- VpadLayoutFits

static void TestFitsRuleA_BoxInsideScreen()
{
	// (а) 0 <= x < W, 0 <= y < H для коробки центр +- R. Екран 100x100
	// (квадрат — ландшафт, правило (в) не діє).
	TVpadLayout L;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 20, 50, 20, 0, 0);  CHECK_EQ(VpadLayoutFits(L, 100, 100), 1); // x 0..40
	SetCircCtl(L.iC[0], 19, 50, 20, 0, 0);  CHECK_EQ(VpadLayoutFits(L, 100, 100), 0); // x -1..39
	SetCircCtl(L.iC[0], 79, 50, 20, 0, 0);  CHECK_EQ(VpadLayoutFits(L, 100, 100), 1); // x 59..99
	SetCircCtl(L.iC[0], 80, 50, 20, 0, 0);  CHECK_EQ(VpadLayoutFits(L, 100, 100), 0); // x 60..100
	SetCircCtl(L.iC[0], 50, 20, 20, 0, 0);  CHECK_EQ(VpadLayoutFits(L, 100, 100), 1); // y 0..40
	SetCircCtl(L.iC[0], 50, 19, 20, 0, 0);  CHECK_EQ(VpadLayoutFits(L, 100, 100), 0); // y -1..39
	SetCircCtl(L.iC[0], 50, 79, 20, 0, 0);  CHECK_EQ(VpadLayoutFits(L, 100, 100), 1); // y 59..99
	SetCircCtl(L.iC[0], 50, 80, 20, 0, 0);  CHECK_EQ(VpadLayoutFits(L, 100, 100), 0); // y 60..100
	SetRectCtl(L.iC[0], 10, 50, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 100, 100), 1); // x 0..20
	SetRectCtl(L.iC[0], 9, 50, 10, 10, 0);  CHECK_EQ(VpadLayoutFits(L, 100, 100), 0); // x -1..19
	SetRectCtl(L.iC[0], 89, 50, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 100, 100), 1); // x 79..99
	SetRectCtl(L.iC[0], 90, 50, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 100, 100), 0); // x 80..100
}

static void TestFitsRuleB_OverlapAndSharedGroup()
{
	// (б) торкання краєм — перетин; спільна ненульова група — дозволено.
	TVpadLayout L;
	LayoutInit(L, 2);
	SetRectCtl(L.iC[0], 50, 50, 10, 10, 0);
	SetRectCtl(L.iC[1], 71, 50, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 200, 200), 1); // x 40..60 и 61..81, зазор
	SetRectCtl(L.iC[1], 70, 50, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 200, 200), 0); // x 60 торкається 60
	SetRectCtl(L.iC[1], 50, 71, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 200, 200), 1); // y зазор
	SetRectCtl(L.iC[1], 50, 70, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 200, 200), 0); // y торкання
	SetRectCtl(L.iC[0], 50, 50, 10, 10, 1);
	SetRectCtl(L.iC[1], 70, 50, 10, 10, 1); CHECK_EQ(VpadLayoutFits(L, 200, 200), 1); // спільна група 1
	SetRectCtl(L.iC[1], 70, 50, 10, 10, 2); CHECK_EQ(VpadLayoutFits(L, 200, 200), 0); // групи 1 і 2
	SetRectCtl(L.iC[0], 50, 50, 10, 10, 0);
	SetRectCtl(L.iC[1], 70, 50, 10, 10, 1); CHECK_EQ(VpadLayoutFits(L, 200, 200), 0); // група 0 не спільна
	SetRectCtl(L.iC[1], 70, 50, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 200, 200), 0); // обидві 0
}

static void TestFitsRuleC_PortraitTopLimit()
{
	// (в) на портреті жоден box не починається вище yP = W*3/4.
	// 360x640: yP = 270. Верхня межа 270 дозволена, 269 ні.
	TVpadLayout L;
	LayoutInit(L, 1);
	SetRectCtl(L.iC[0], 100, 280, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 360, 640), 1); // верх 270
	SetRectCtl(L.iC[0], 100, 279, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 360, 640), 0); // верх 269
	// Той самий box на ландшафті 640x360 верх 0: правило (в) не діє, лишається (а).
	SetRectCtl(L.iC[0], 100, 10, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 640, 360), 1);
	// Квадрат 300x300 — ландшафт (W >= H), верх 10 < 225 дозволено.
	SetRectCtl(L.iC[0], 100, 20, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 300, 300), 1);
	// Портрет 360x640 із верхом 0 — порушує (в).
	SetRectCtl(L.iC[0], 100, 10, 10, 10, 0); CHECK_EQ(VpadLayoutFits(L, 360, 640), 0);
}

// ---------------------------------------------------------------- натиск і кнопки

static void Layout2Buttons(TVpadLayout &aL, int aGroupA, int aGroupB)
{
	// A: (0,0) R40, Cross. B: (100,0) R40, Circle. Зона натиску R*125/100 = 50.
	LayoutInit(aL, 2);
	SetCircCtl(aL.iC[0], 0, 0, 40, aGroupA, 1 << EVpadCross);
	SetCircCtl(aL.iC[1], 100, 0, 40, aGroupB, 1 << EVpadCircle);
}

static void TestCaptureNearestByNormalisedDistance()
{
	// Радіус натиску 125 для обох кіл R=100. Точка (60,0): A = 60*1000/125 = 480, B = 40*1000/125 = 320.
	// Вибирається B.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 0, 0);
	L.iC[0].iRx = 100; L.iC[1].iRx = 100;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 60, 0, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
	CHECK_EQ(S.iP[0].iCtl, 1);
}

static void TestCaptureTieGoesToLowerIndex()
{
	// Точка (50,0): A = 400, B = 400. Нижчий індекс (A) перемагає.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 0, 0);
	L.iC[0].iRx = 100; L.iC[1].iRx = 100;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 50, 0, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	CHECK_EQ(S.iP[0].iCtl, 0);
}

static void TestCaptureOutsidePressZoneStaysInertUntilUp()
{
	// (300,0) лежить далі 125 від обох кіл: нічого не захоплено. Наїзд на кнопку
	// не натискає її.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 0, 0);
	L.iC[0].iRx = 100; L.iC[1].iRx = 100;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 300, 0, 1000);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iP[0].iCtl, -1);
	VpadPointerMove(S, L, 0, 100, 0, 1010);
	CHECK_EQ(S.iBits, 0);
	VpadPointerUp(S, L, 0, 1020);
	CHECK_EQ(S.iBits, 0);
}

static void TestButtonPressZoneBoundaryCircleAndRect()
{
	// Коло R=40: зона натиску 50. Точка (50,0) всередині, (51,0) ні.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 0, 0, 40, 0, 1 << EVpadCross);
	SetRectCtl(L.iC[1], 200, 0, 40, 20, 0);
	L.iC[1].iBits = 1 << EVpadSquare;
	// Прямокутник з півосями (40,20): зона (50,25).
	VpadReset(S);
	VpadPointerDown(S, L, 0, 50, 0, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 51, 0, 1000);
	CHECK_EQ(S.iBits, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 200 + 50, 25, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadSquare);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 200 + 51, 0, 1000);
	CHECK_EQ(S.iBits, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 200, 26, 1000);
	CHECK_EQ(S.iBits, 0);
}

static void TestButtonGlideAcrossSharedGroup()
{
	// Обидві кнопки в групі 5. Кнопка A: (0,0), B: (100,0). Ковзання від A до B натискає B,
	// на перетині зон натискаються обидві.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 5, 5);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	VpadPointerMove(S, L, 0, 100, 0, 1010);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
	VpadPointerMove(S, L, 0, 50, 0, 1020);
	CHECK_EQ(S.iBits, (1 << EVpadCross) | (1 << EVpadCircle));
	VpadPointerMove(S, L, 0, 150, 0, 1030);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
	CHECK_EQ(S.iP[0].iHeld, 1 << 1);
}

static void TestButtonLeashKeepsThenReleases()
{
	// Після B повідець кнопки B: 40*175/100 = 70. Точка (160,0): від B 60 ≤ 70, тримає Circle.
	// (171,0): 71 > 70, відпускає й очищає запам'ятане.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 5, 5);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	VpadPointerMove(S, L, 0, 100, 0, 1010);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
	VpadPointerMove(S, L, 0, 160, 0, 1020);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
	VpadPointerMove(S, L, 0, 171, 0, 1030);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iP[0].iHeld, 0);
	VpadPointerMove(S, L, 0, 100, 0, 1040);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
}

static void TestButtonDifferentGroupsNoGlide()
{
	// Групи 0: кнопки не поділяють ковзання. Від A до точки B натиску немає.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 0, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	VpadPointerMove(S, L, 0, 100, 0, 1010);
	CHECK_EQ(S.iBits, 0);
}

static void TestEdgesAccumulateAndTakeClears()
{
	// iEdges: лише переходи 0→1 загального слова; VpadTakeEdges повертає й очищає.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 0, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	CHECK_EQ(S.iEdges, 1 << EVpadCross);
	CHECK_EQ(VpadTakeEdges(S), 1 << EVpadCross);
	CHECK_EQ(VpadTakeEdges(S), 0);
	// Утримання без руху біта не створює нового фронту.
	VpadPointerMove(S, L, 0, 10, 0, 1010);
	CHECK_EQ(S.iEdges, 0);
	VpadPointerUp(S, L, 0, 1020);
	CHECK_EQ(S.iEdges, 0);
	// Відпускання не дає фронту; наступний натиск дає.
	VpadPointerDown(S, L, 0, 100, 0, 1030);
	CHECK_EQ(S.iEdges, 1 << EVpadCircle);
	CHECK_EQ(VpadTakeEdges(S), 1 << EVpadCircle);
}

static void TestEdgesSecondPointerAddsOnlyNewBits()
{
	// Палець 0 натискає Cross. Палець 1 натискає Circle: у iEdges лише новий біт Circle.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 0, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	VpadTakeEdges(S);
	VpadPointerDown(S, L, 1, 100, 0, 1005);
	CHECK_EQ(S.iEdges, 1 << EVpadCircle);
	CHECK_EQ(S.iBits, (1 << EVpadCross) | (1 << EVpadCircle));
}

static void TestBitsAreOrOverPointers()
{
	// iBits = OR по покажчиках; відпускання одного лишає біт іншого.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 0, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	VpadPointerDown(S, L, 1, 100, 0, 1005);
	CHECK_EQ(S.iBits, (1 << EVpadCross) | (1 << EVpadCircle));
	VpadPointerUp(S, L, 0, 1010);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
	VpadPointerUp(S, L, 1, 1020);
	CHECK_EQ(S.iBits, 0);
}

static void TestActionReleaseInsideLeashSetsBit()
{
	// Дія: коло R=25, iId=5. Повідець 25*175/100 = 43. Відпускання на 43 від центру — дія є;
	// на 44 — немає. Дія не натискає біт.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 100, 100, 25, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindAction;
	L.iC[0].iId = 5;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	CHECK_EQ(S.iBits, 0);
	VpadPointerMove(S, L, 0, 143, 100, 1010);
	VpadPointerUp(S, L, 0, 1020);
	CHECK_EQ(S.iActions, 1u << 5);
	CHECK_EQ(VpadTakeActions(S), 1u << 5);
	CHECK_EQ(VpadTakeActions(S), 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	VpadPointerMove(S, L, 0, 144, 100, 1010);
	VpadPointerUp(S, L, 0, 1020);
	CHECK_EQ(S.iActions, 0);
}

static void TestIdOutOfRangeIsIgnored()
{
	// aId поза 0..3 ігнорується.
	TVpadLayout L;
	TVpadState S;
	Layout2Buttons(L, 0, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 4, 0, 0, 1000);
	VpadPointerDown(S, L, -1, 0, 0, 1000);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iP[0].iDown, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	VpadPointerDown(S, L, 0, 0, 0, 1010);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	VpadPointerMove(S, L, 9, 100, 0, 1020);
	CHECK_EQ(S.iP[0].iX, 0);
	VpadPointerUp(S, L, 7, 1030);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
}

static void TestCapturedDpadNotCapturedByOtherPointer()
{
	// Хрестовина, захоплена пальцем 0, не захоплюється пальцем 1.
	// Палець 1 у зоні хрестовини безпечний: нічого не натискає.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadPointerDown(S, L, 1, 120, 100, 1005);
	CHECK_EQ(S.iP[1].iCtl, -1);
	CHECK_EQ(S.iP[1].iBits, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

// ---------------------------------------------------------------- хрестовина

static void LayoutDpad(TVpadLayout &aL, int aDiagPct, int aDeadPct)
{
	// Хрестовина (100,100) R=50. Зона натиску 62, повідець 87. Мертва зона 50*18/100 = 9.
	LayoutInit(aL, 1);
	SetCircCtl(aL.iC[0], 100, 100, 50, 0, 0);
	aL.iC[0].iKind = (unsigned char)EVpadKindDpad;
	aL.iDiagPct = aDiagPct;
	aL.iDeadPct = aDeadPct;
}

static void DpadAt(TVpadState &aS, const TVpadLayout &aL, int aDx, int aDy)
{
	VpadPointerMove(aS, aL, 0, 100 + aDx, 100 + aDy, 2000);
}

static void TestDpadFourDirectionsAndDiagonal()
{
	// Вісім напрямків: Right 32, Left 128, Down 64, Up 16; (40,40) при
	// попередньому одному біті: q=1000 >= T+35=449, dist 56 >= 20, тож діагональ Right|Down = 96.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 100, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	CHECK_EQ(S.iBits, 0);
	DpadAt(S, L, 40, 0);   CHECK_EQ(S.iBits, 1 << EVpadRight);
	DpadAt(S, L, -40, 0);  CHECK_EQ(S.iBits, 1 << EVpadLeft);
	DpadAt(S, L, 0, 40);   CHECK_EQ(S.iBits, 1 << EVpadDown);
	DpadAt(S, L, 0, -40);  CHECK_EQ(S.iBits, 1 << EVpadUp);
	DpadAt(S, L, 40, 40);  CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
	DpadAt(S, L, -40, -40); CHECK_EQ(S.iBits, (1 << EVpadLeft) | (1 << EVpadUp));
}

static void TestDpadDiagonalThresholdFromNothing()
{
	// T при iDiagPct=100 — індекс 10 таблиці = 414. Від "нічого" без зсуву:
	// (100,41): q=410 < 414, один біт Right. (100,42): q=420 >= 414, діагональ.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 100, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 100, 41);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 100, 42);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestDpadBiasAfterSingleBitRaisesThreshold()
{
	// Після одного біта T+35 = 449. (100,44): q=440 < 449, Right.
	// (100,45): q=450 >= 449, діагональ.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 100, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	DpadAt(S, L, 100, 44);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	DpadAt(S, L, 100, 45);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestDpadBiasAfterDiagonalLowersThreshold()
{
	// Після діагоналі T-35 = 379. (100,37): q=370 < 379, Right.
	// (100,38): q=380 >= 379, діагональ зберігається.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 100, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 140, 1000);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
	DpadAt(S, L, 100, 37);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 140, 1000);
	DpadAt(S, L, 100, 38);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestDpadDeadZoneBoundaryAndHysteresis()
{
	// Мертва зона 9. dist 8 — нічого, dist 9 — Right.
	// Після натиску dead = 9*3/4 = 6: dist 6 натискає, dist 5 — ні.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 100, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 8, 0);
	CHECK_EQ(S.iBits, 0);
	DpadAt(S, L, 9, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	DpadAt(S, L, 6, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	DpadAt(S, L, 5, 0);
	CHECK_EQ(S.iBits, 0);
}

static void TestDpadFourWayBoundaryAndTieGoesHorizontal()
{
	// iFourWayPct=40: при dist*100 < 50*40 = 2000 (dist < 20) діагоналі немає.
	// (12,12): dist 16, рівність |dx|=|dy| дає горизонталь. (14,14): dist 19 — ще четири напрямки.
	// (15,15): dist 21 — діагональ.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 100, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 12, 12);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, -12, 12);
	CHECK_EQ(S.iBits, 1 << EVpadLeft);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 14, 14);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 15, 15);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestDpadDiagPctZeroIsFourWayOnly()
{
	// iDiagPct=0: ніколи два біти разом.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 0, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 40, 40);  CHECK_EQ(S.iBits, 1 << EVpadRight);
	DpadAt(S, L, -30, 30); CHECK_EQ(S.iBits, 1 << EVpadLeft);
	DpadAt(S, L, 10, 40);  CHECK_EQ(S.iBits, 1 << EVpadDown);
}

static void TestDpadDiagPctBelowTenDisablesDiagonal()
{
	// Діагональ лише при iDiagPct >= 10: при 9 (40,40) дає один біт, при 10 — T=924 (індекс 1).
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 9, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 40, 40);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	LayoutDpad(L, 10, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 100, 92);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 100, 93);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestDpadTableIndexIsIntegerDivisionOfDiagPct()
{
	// iDiagPct=39: індекс 39/10 = 3, T = 788. (100,78): q=780 < 788; (100,79): q=790.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 39, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 100, 78);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 100, 79);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestDpadDeadPctAboveSixtyTreatedAsSixty()
{
	// iDeadPct=100 трактується як 60: dead = 50*60/100 = 30.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 100, 100);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 29, 0);
	CHECK_EQ(S.iBits, 0);
	DpadAt(S, L, 30, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestDpadDiagPctAboveHundredTreatedAsHundred()
{
	// iDiagPct=250 трактується як 100: (100,42) діагональ, (100,41) ні.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 250, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 100, 41);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 100, 42);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestDpadCentreGivesNothingAndRimIsNoLimit()
{
	// Точка в центрі — нічого. За межею обода хрестовина працює.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 100, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	CHECK_EQ(S.iBits, 0);
	DpadAt(S, L, 300, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

// ---------------------------------------------------------------- стік

static void LayoutStick(TVpadLayout &aL, int aFloat)
{
	// Стік (100,100) R=80. Мертва зона 80*18/100 = 14, span = 66. Цифровий поріг 80% = 64.
	LayoutInit(aL, 1);
	SetCircCtl(aL.iC[0], 100, 100, 80, 0, 0);
	aL.iC[0].iKind = (unsigned char)EVpadKindStick;
	aL.iStickFloat = aFloat;
}

static void TestStickDeflectionFormulaAtPoints()
{
	// m = mag*127/span, iLx = 127 + dx*m/dist.
	// (40,0): mag=26, m=26*127/66=50, iLx=127+40*50/40=177, iLy=127.
	// (-30,-40): dist 50, mag 36, m=36*127/66=69; iLx=127+(-2070/50)=127-41=86, iLy=127-55=72
	// (усічення до нуля). (30,40): iLx=168, iLy=182.
	TVpadLayout L;
	TVpadState S;
	LayoutStick(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
	VpadPointerMove(S, L, 0, 140, 100, 1010);
	CHECK_EQ(S.iLx, 177);
	CHECK_EQ(S.iLy, 127);
	VpadPointerMove(S, L, 0, 70, 60, 1020);
	CHECK_EQ(S.iLx, 86);
	CHECK_EQ(S.iLy, 72);
	VpadPointerMove(S, L, 0, 130, 140, 1030);
	CHECK_EQ(S.iLx, 168);
	CHECK_EQ(S.iLy, 182);
}

static void TestStickSaturatesAtRimAndAtZeroAnd254()
{
	// Насичення за ободом: 127+127=254, 127-127=0.
	TVpadLayout L;
	TVpadState S;
	LayoutStick(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	VpadPointerMove(S, L, 0, 300, 100, 1010);
	CHECK_EQ(S.iLx, 254);
	CHECK_EQ(S.iLy, 127);
	VpadPointerMove(S, L, 0, -100, 100, 1020);
	CHECK_EQ(S.iLx, 0);
	CHECK_EQ(S.iLy, 127);
}

static void TestStickDeadZoneBoundary()
{
	// dist 13 < 14 — центр; dist 15 — mag=1, m=127/66=1, iLx=128.
	TVpadLayout L;
	TVpadState S;
	LayoutStick(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	VpadPointerMove(S, L, 0, 113, 100, 1010);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
	VpadPointerMove(S, L, 0, 115, 100, 1020);
	CHECK_EQ(S.iLx, 128);
	CHECK_EQ(S.iLy, 127);
}

static void TestStickDigitalThresholdAndDiagonal()
{
	// Цифровий резерв при dist*100 >= 80*80 = 6400, тобто dist >= 64.
	// (64,0): Right, iLx=127+64*96/64=223. (63,0): нічого. (46,46): dist 65, діагональ 96.
	TVpadLayout L;
	TVpadState S;
	LayoutStick(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	VpadPointerMove(S, L, 0, 164, 100, 1010);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	CHECK_EQ(S.iLx, 223);
	VpadPointerMove(S, L, 0, 163, 100, 1020);
	CHECK_EQ(S.iBits, 0);
	VpadPointerMove(S, L, 0, 146, 146, 1030);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestStickFloatCentreAtTouchPoint()
{
	// iStickFloat != 0: центр — точка дотику. Той самий жест без
	// float дає 254 (dx=90 від центру кола, mag=min(90,80)-14=66 => 127+90*127/90).
	TVpadLayout L;
	TVpadState S;
	LayoutStick(L, 1);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 150, 100, 1000);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
	VpadPointerMove(S, L, 0, 190, 100, 1010);
	CHECK_EQ(S.iLx, 177);
	CHECK_EQ(S.iLy, 127);
	LayoutStick(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 150, 100, 1000);
	VpadPointerMove(S, L, 0, 190, 100, 1010);
	CHECK_EQ(S.iLx, 254);
}

static void TestStickReleaseCentres()
{
	// Після відпускання iLx = iLy = 127, iBits 0.
	TVpadLayout L;
	TVpadState S;
	LayoutStick(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 180, 100, 1000);
	CHECK_EQ(S.iLx, 254);
	VpadPointerUp(S, L, 0, 1010);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iBits, 0);
}

static void TestStickNotCapturedWhenHeldByOtherPointer()
{
	// Стік, захоплений пальцем 0, не захоплюється пальцем 1.
	TVpadLayout L;
	TVpadState S;
	LayoutStick(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	VpadPointerDown(S, L, 1, 100, 100, 1005);
	CHECK_EQ(S.iP[1].iCtl, -1);
	CHECK_EQ(S.iLx, 177);
}

// ---------------------------------------------------------------- другий палець

static void LayoutDual(TVpadLayout &aL, int aJump)
{
	// Хрестовина (100,100) R50, група 0; кнопка Cross (300,100) R40, група 0. Повідець
	// кнопки 70. Порогові значення: поріг стрибка aJump, час KVpadDualMaxMs = 60.
	LayoutInit(aL, 2);
	SetCircCtl(aL.iC[0], 100, 100, 50, 0, 0);
	aL.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(aL.iC[1], 300, 100, 40, 0, 1 << EVpadCross);
	aL.iDualJumpPx = aJump;
}

static void DualStart(TVpadState &aS, const TVpadLayout &aL)
{
	// Палець 0 падає на (140,100) (вправо від хрестовини) о 1000 мс, потім рух до (240,100)
	// о 1060 мс: від L=(140,100) крок 100 >= 60 за 60 мс, P2=(2*240-140, 100)=(340,100)
	// лежить у повідці кнопки (від (300,100) відстань 40 <= 70): інференція вмикається.
	VpadReset(aS);
	VpadPointerDown(aS, aL, 0, 140, 100, 1000);
	VpadPointerMove(aS, aL, 0, 240, 100, 1060);
}

static void TestDualInferenceActivatesOnJumpToLeashedControl()
{
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	DualStart(S, L);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iDualX, 140);
	CHECK_EQ(S.iDualY, 100);
	CHECK_EQ(S.iP[1].iDown, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iP[1].iHeld, 1 << 1);
	CHECK_EQ(S.iP[0].iX, 140);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadCross));
}

static void TestDualNoActivationBelowJumpThreshold()
{
	// Крок 59 < 60 = iDualJumpPx: звичайний рух, інференції немає.
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	VpadPointerMove(S, L, 0, 199, 100, 1010);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestDualNoActivationAfterSixtyMs()
{
	// Той самий стрибок за 61 мс — це ковзання, а не другий палець.
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	VpadPointerMove(S, L, 0, 240, 100, 1061);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestDualNoActivationWithoutCandidate()
{
	// Немає кнопки в повідці P2: звичайний рух.
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	L.iCount = 1;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	VpadPointerMove(S, L, 0, 240, 100, 1040);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestDualIgnoresNonZeroIdCalls()
{
	// При iDualJumpPx > 0 виклики з aId != 0 ігноруються.
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	DualStart(S, L);
	VpadPointerMove(S, L, 1, 400, 100, 1070);
	CHECK_EQ(S.iP[1].iX, 340);
	VpadPointerUp(S, L, 1, 1080);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iDown, 1);
}

static void TestDualSecondFingerLeavesReturnsToSingle()
{
	// Палець 0 у межах iDualJumpPx/3 = 20 від першого (140,100): другий пішов, інференцію
	// скасовано, палець 0 іде в M як звичайний рух.
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	DualStart(S, L);
	VpadPointerMove(S, L, 0, 150, 100, 1070);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iP[0].iX, 150);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestDualFirstFingerLeavesSecondTakesOver()
{
	// M=(335,100): далі 20 від першого (140,100), від P2old=(340,100) відстань 5 <= 20.
	// Палець 0 перейма захоплення слота 1 (кнопка Cross) у M, слот 1 звільнено.
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	DualStart(S, L);
	VpadPointerMove(S, L, 0, 335, 100, 1070);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iP[0].iCtl, 1);
	CHECK_EQ(S.iP[0].iX, 335);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
}

static void TestDualMirroredSlotMovesWhenNeitherFingerMatches()
{
	// M=(300,100): від першого 160, від P2old 40, обидва більше 20. Слот 1 іде в
	// (2*300-140, 2*100-100) = (460,100). Інференція лишається.
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	DualStart(S, L);
	VpadPointerMove(S, L, 0, 300, 100, 1070);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iX, 460);
	CHECK_EQ(S.iP[1].iY, 100);
	CHECK_EQ(S.iP[0].iX, 140);
}

static void TestDualLiftingFirstFingerLiftsSecond()
{
	// Відпускання покажчика 0 знімає все й слот 1.
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	DualStart(S, L);
	VpadPointerUp(S, L, 0, 1080);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
}

static void TestDualDpadCandidateEvaluatedAtSecondPointAtOnce()
{
	// Захоплена кнопка Cross (100,100) R40 при падінні (140,100) (зона 50, відстань 40).
	// Рух на (240,100) за 60 мс: P2=(340,100) у повідці хрестовини (300,100) R50, відстань 40 <= 87.
	// Хрестовина оцінюється в P2 одразу: Right. Кнопка палець 0 тримає далі.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 100, 100, 40, 0, 1 << EVpadCross);
	SetCircCtl(L.iC[1], 300, 100, 50, 0, 0);
	L.iC[1].iKind = (unsigned char)EVpadKindDpad;
	L.iDualJumpPx = 60;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	VpadPointerMove(S, L, 0, 240, 100, 1060);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iBits, (1 << EVpadCross) | (1 << EVpadRight));
}

static void TestDualCandidateSharingCapturedGroupIsExcluded()
{
	// Кандидат з тією самою ненульовою групою, що й захоплений елемент, виключається.
	// Та сама геометрія з групою 0 вмикає інференцію.
	TVpadLayout L;
	TVpadState S;
	int g;
	static const int kGroups[2] = { 7, 0 };
	static const int kExpectDual[2] = { 0, 1 };
	for (g = 0; g < 2; ++g) {
		LayoutInit(L, 2);
		SetCircCtl(L.iC[0], 100, 100, 40, kGroups[g], 1 << EVpadCross);
		SetCircCtl(L.iC[1], 300, 100, 40, kGroups[g], 1 << EVpadCircle);
		L.iDualJumpPx = 60;
		VpadReset(S);
		VpadPointerDown(S, L, 0, 140, 100, 1000);
		VpadPointerMove(S, L, 0, 240, 100, 1060);
		sprintf(gWhere, "%s group %d", gTest, kGroups[g]);
		CHECK_EQ(S.iDual, kExpectDual[g]);
		strcpy(gWhere, gTest);
	}
}

// ---------------------------------------------------------------- стійкість

static void TestCoordinatesClampedOnEntry()
{
	// Координати обрізаються до -8192..8191. Кнопка (8150,0) R100, зона 125:
	// точка 1000000 стає 8191, відстань 41 <= 125, натискає. Точка -1000000 стає -8192, далеко.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 8150, 0, 100, 0, 1 << EVpadCross);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 1000000, 0, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	VpadReset(S);
	VpadPointerDown(S, L, 0, -1000000, 0, 1000);
	CHECK_EQ(S.iBits, 0);
}

static void TestNonPositiveRadiusIgnored()
{
	// iRx <= 0 (коло) та iRy <= 0 (прямокутник) ігноруються всіма функціями.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 0, 0, 0, 0, 1 << EVpadCross);
	SetRectCtl(L.iC[1], 0, 0, 10, 0, 0);
	L.iC[1].iBits = 1 << EVpadSquare;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iP[0].iCtl, -1);
}

static void TestNegativeControlCountCapturesNothing()
{
	// iCount < 0 — немає елементів: Down нічого не захоплює.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, -5);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iP[0].iCtl, -1);
}

static void TestPadWordIsOrOfPointersAfterFuzzyUse()
{
	// Інваріанти після серії подій: iBits = OR по покажчиках; при всіх відпущених iBits = 0,
	// стік у центрі. Тут — з фіксованим прикладом.
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	VpadPointerDown(S, L, 1, 300, 100, 1005);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadCross));
	VpadPointerUp(S, L, 0, 1010);
	VpadPointerUp(S, L, 1, 1020);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
}

// ---------------------------------------------------------------- псевдовипадкові послідовності

struct TRng { unsigned iS; };

static unsigned Rnd(TRng &aR)
{
	aR.iS = aR.iS * 1103515245u + 12345u;
	return (aR.iS >> 16) & 0x7fff;
}

static int RndRange(TRng &aR, int aLo, int aHi)
{
	return aLo + (int)(Rnd(aR) % (unsigned)(aHi - aLo + 1));
}

static void CheckPadInvariants(const TVpadState &aS, const TVpadLayout &aL, unsigned aAllBits)
{
	int k;
	unsigned orBits = 0;
	int anyDown = 0;
	int holdL = 0;
	int holdR = 0;
	for (k = 0; k < KVpadMaxPointers; ++k) {
		orBits |= aS.iP[k].iBits;
		if (aS.iP[k].iDown) anyDown = 1;
		else CHECK_EQ(aS.iP[k].iBits, 0);
		// Сторона стіка визначається за iId: 1 — правий, будь-яке інше значення — лівий.
		if (aS.iP[k].iDown && aS.iP[k].iCtl >= 0 && aS.iP[k].iCtl < TakenCount(aL)
			&& aL.iC[aS.iP[k].iCtl].iKind == EVpadKindStick) {
			if (aL.iC[aS.iP[k].iCtl].iId == 1) holdR = 1;
			else holdL = 1;
		}
		// Заголовок: доки палець тримає клацання, він натискає L3 або R3.
		if (aS.iP[k].iDown && aS.iP[k].iClick) CHECK(aS.iP[k].iBits & ((1u << EVpadL3) | (1u << EVpadR3)));
	}
	CHECK_EQ(aS.iBits, orBits);
	CHECK_EQ(aS.iBits & ~aAllBits, 0);
	CHECK(aS.iLx <= 254);
	CHECK(aS.iLy <= 254);
	if (!holdL) {
		CHECK_EQ(aS.iLx, KVpadStickCentre);
		CHECK_EQ(aS.iLy, KVpadStickCentre);
	}
	if (!holdR) {
		CHECK_EQ(aS.iRightX, KVpadStickCentre);
		CHECK_EQ(aS.iRightY, KVpadStickCentre);
	}
	// iByDown: біт p стоїть лише для опущеного пальця, що тримає стік. Без опущених пальців поле нульове
	// (це випливає з першої перевірки). Чи стік захоплено натисканням, а не виведенням, зі стану не видно:
	// це перевіряють TestByDownIgnoresInferenceAndTakeover і TestByDownSetByStickDownClearedByLiftAndReset.
	{
		unsigned downMask = 0;
		for (k = 0; k < KVpadMaxPointers; ++k) {
			if (aS.iP[k].iDown) downMask |= 1u << k;
		}
		CHECK_EQ(aS.iByDown & ~downMask, 0);
		for (k = 0; k < KVpadMaxPointers; ++k) {
			if (aS.iByDown & (1u << k)) {
				CHECK(aS.iP[k].iDown);
				CHECK(aS.iP[k].iCtl >= 0 && aS.iP[k].iCtl < TakenCount(aL));
				if (aS.iP[k].iCtl >= 0 && aS.iP[k].iCtl < TakenCount(aL))
					CHECK_EQ(aL.iC[aS.iP[k].iCtl].iKind, EVpadKindStick);
			}
		}
	}
	if (!anyDown) CHECK_EQ(aS.iBits, 0);
}

static void RunRandomSequence(const TVpadLayout &aL, unsigned aSeed, int aSteps)
{
	TVpadState S;
	TRng R;
	int i;
	unsigned long t = 1000;
	unsigned allBits = 0xF0;
	int c;
	for (c = 0; c < TakenCount(aL); ++c) {
		if (aL.iC[c].iKind == EVpadKindButton) allBits |= aL.iC[c].iBits;
		if (aL.iC[c].iKind == EVpadKindStick) allBits |= 0xF0;
		if (aL.iC[c].iKind == EVpadKindStick && aL.iStickClickMs > 0) allBits |= (1u << EVpadL3) | (1u << EVpadR3);
	}
	R.iS = aSeed;
	VpadReset(S);
	for (i = 0; i < aSteps; ++i) {
		int op = RndRange(R, 0, 9);
		int id = RndRange(R, 0, 3);
		int x = RndRange(R, -20, 660);
		int y = RndRange(R, -20, 380);
		t += (unsigned)RndRange(R, 0, 20);
		if (op < 4) VpadPointerDown(S, aL, id, x, y, (unsigned)t);
		else if (op < 8) VpadPointerMove(S, aL, id, x, y, (unsigned)t);
		else if (op < 9) VpadPointerUp(S, aL, id, (unsigned)t);
		else (void)VpadTakeEdges(S);
		CheckPadInvariants(S, aL, allBits);
	}
}

static void TestRandomSequenceInvariantsLandscape()
{
	// 30000 подій із фіксованим зерном; інваріанти після кожного виклику.
	TVpadLayout L;
	VpadDefaultLayout(L, 640, 360, 138, 100, 0);
	RunRandomSequence(L, 20260101u, 30000);
}

static void TestRandomSequenceInvariantsDualPanel()
{
	// Резистивна панель: інференція другого пальця активна, зерно інше.
	TVpadLayout L;
	VpadDefaultLayout(L, 640, 360, 138, 100, EVpadOptResistiveDual);
	RunRandomSequence(L, 987654321u, 30000);
}

// ---------------------------------------------------------------- гортання: ініціалізація

// Список 300 px у вікні з 1000 px: максимум 700. При aUmPerPx=100 (1 px = 0.1 мм):
// slop = (2000+50)/100 = 20, minFling = (30000+50)/100 = 300, maxFling = (800000+50)/100 = 8000
static void ScrollInit100(TKScroll &aS)
{
	KScrollInit(aS, 300, 1000, 100);
}

static void TestScrollInitConversionsFromMicrometres()
{
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	CHECK_EQ(S.iSlop, 20);
	CHECK_EQ(S.iMinFling, 300);
	CHECK_EQ(S.iMaxFling, 8000);
	CHECK_EQ(S.iFrictionLossQ16, 201);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iPos8, 0);
	CHECK_EQ(S.iVel8, 0);
	CHECK_EQ(S.iView, 300);
	CHECK_EQ(S.iContent, 1000);
	// 138 мкм/px: slop=(2000+69)/138=14, minFling=(30000+69)/138=217, maxFling=(800000+69)/138=5797.
	KScrollInit(S, 300, 1000, 138);
	CHECK_EQ(S.iSlop, 14);
	CHECK_EQ(S.iMinFling, 217);
	CHECK_EQ(S.iMaxFling, 5797);
}

static void TestScrollInitMicrometresBelowTwentyTakenAsTwenty()
{
	// aUmPerPx < 20 береться як 20: slop=100, minFling=1500, maxFling=40000.
	TKScroll A, B;
	KScrollInit(A, 300, 1000, 5);
	KScrollInit(B, 300, 1000, 20);
	CHECK_EQ(A.iSlop, 100);
	CHECK_EQ(A.iMinFling, 1500);
	CHECK_EQ(A.iMaxFling, 40000);
	CHECK_EQ(A.iSlop, B.iSlop);
	CHECK_EQ(A.iMinFling, B.iMinFling);
	CHECK_EQ(A.iMaxFling, B.iMaxFling);
}

static void TestScrollInitHugeMicrometresNotLessThanOne()
{
	// aUmPerPx=1000000: slop=0, minFling=0, maxFling=1; обрізається до 1.
	TKScroll S;
	KScrollInit(S, 300, 1000, 1000000);
	CHECK_EQ(S.iSlop, 1);
	CHECK_EQ(S.iMinFling, 1);
	CHECK_EQ(S.iMaxFling, 1);
}

static void TestScrollSizesClampedToFourMillion()
{
	// Розміри обрізаються до 0..4000000.
	TKScroll S;
	KScrollInit(S, -10, 5000000, 100);
	CHECK_EQ(S.iView, 0);
	CHECK_EQ(S.iContent, 4000000);
	KScrollInit(S, 300, 100, 100);
	KScrollTo(S, 50, 0, 1000);
	CHECK_EQ(S.iPos8, 0);
}

// ---------------------------------------------------------------- дотик і довге натискання

static void TestTapWhenReleasedBeforeLongPress()
{
	// Відпускання за 499 мс — дотик; рівно 500 мс — ні.
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 500, 1000);
	CHECK_EQ(KScrollUp(S, 1499), EKsTap);
	CHECK_EQ(S.iState, EKsIdle);
	KScrollDown(S, 500, 1000);
	CHECK_EQ(KScrollUp(S, 1500), EKsNone);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestLongPressReportedExactlyOnceAt500()
{
	// EKsLongPress рівно раз на 500-й мс; після нього відпускання не дає дотику.
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 500, 1000);
	CHECK_EQ(KScrollStep(S, 1499), EKsNone);
	CHECK_EQ(KScrollStep(S, 1500), EKsLongPress);
	CHECK_EQ(KScrollStep(S, 1600), EKsNone);
	CHECK_EQ(KScrollUp(S, 1600), EKsNone);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestMotionInsideSlopKeepsTap()
{
	// Рух на 20 px (= iSlop) не скасовує дотик; 21 px скасовує.
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 500, 1000);
	KScrollMove(S, 520, 1005);
	CHECK_EQ(S.iState, EKsPressed);
	CHECK_EQ(KScrollUp(S, 1020), EKsTap);
	KScrollDown(S, 500, 1000);
	KScrollMove(S, 521, 1005);
	CHECK_EQ(S.iState, EKsDragging);
	CHECK_EQ(KScrollUp(S, 1020), EKsNone);
}

static void TestMoveIgnoredWhenNotPressedOrDragging()
{
	// KScrollMove в EKsIdle не змінює стан.
	TKScroll S;
	ScrollInit100(S);
	KScrollMove(S, 700, 1000);
	CHECK_EQ(S.iPos8, 0);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(KScrollUp(S, 1010), EKsNone);
}

// ---------------------------------------------------------------- перетягування

static void TestDragFirstBeyondSlopAnchorsWithoutJump()
{
	// Перша подія за межею 20 px не зсуває список: якір береться в ній.
	// Далі піксель у піксель: 500 -> 21 px, 480 -> 41 px (24.8: 5376, 10496).
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 500, 1000);
	KScrollMove(S, 521, 1010);
	CHECK_EQ(S.iState, EKsDragging);
	CHECK_EQ(S.iPos8, 0);
	KScrollMove(S, 500, 1020);
	CHECK_EQ(S.iPos8, 21 * 256);
	KScrollMove(S, 480, 1030);
	CHECK_EQ(S.iPos8, 41 * 256);
}

static void TestDragOverscrollBelowZeroHalves()
{
	// raw = 0 + (521-621) = -100 px. Нижче 0 половинний хід: -50 px = -12800 у 24.8.
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 500, 1000);
	KScrollMove(S, 521, 1010);
	KScrollMove(S, 621, 1020);
	CHECK_EQ(S.iPos8, -12800);
}

static void TestDragOverscrollAboveMaxHalvesExcess()
{
	// Max = 700. Старт з 700 (179200). Анкер 521 при p=700. Координата 479: raw = 700+42 = 742,
	// над max: 700 + (742-700)/2 = 721 px = 184576.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 700, 0, 1000);
	CHECK_EQ(S.iPos8, 700 * 256);
	KScrollDown(S, 500, 1100);
	KScrollMove(S, 521, 1110);
	CHECK_EQ(S.iPos8, 700 * 256);
	KScrollMove(S, 479, 1120);
	CHECK_EQ(S.iPos8, 721 * 256);
}

static void TestDragFromOverscrollKeepsPositionContinuous()
{
	// Перетягування з перегорнутого положення: p=721 (над max) дає raw = 700 + 2*21 = 742.
	// Тому перший рух на 0 від анкера лишає позицію 721 px без стрибка.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 700, 0, 1000);
	KScrollDown(S, 500, 1100);
	KScrollMove(S, 521, 1110);
	KScrollMove(S, 479, 1120);
	CHECK_EQ(S.iPos8, 721 * 256);
	KScrollMove(S, 479, 1130);
	CHECK_EQ(S.iPos8, 721 * 256);
}

// ---------------------------------------------------------------- відпускання і кидок

static void ScrollFlickUp(TKScroll &aS, unsigned aBase)
{
	// Вниз-вгору: 600 -> 579 (анкер) -> 560 -> 540 -> 520. N=520@+40, O=600@+0:
	// v = (600-520)*1000/40 = 2000 px/s, зсув 59 px від старту 0.
	KScrollInit(aS, 300, 1000, 100);
	KScrollDown(aS, 600, aBase);
	KScrollMove(aS, 579, aBase + 10);
	KScrollMove(aS, 560, aBase + 20);
	KScrollMove(aS, 540, aBase + 30);
	KScrollMove(aS, 520, aBase + 40);
}

static void TestReleaseVelocityFromWindowStartsFling()
{
	// v=2000 px/s -> iVel8 = 512000; стан EKsFlinging; відпускання без дотику.
	TKScroll S;
	ScrollFlickUp(S, 1000);
	CHECK_EQ(S.iState, EKsDragging);
	CHECK_EQ(KScrollUp(S, 1040), EKsNone);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, 2000 * 256);
	CHECK_EQ(S.iPos8, 59 * 256);
}

static void TestReleaseAfterRestingOverHundredMsGivesNoFling()
{
	// Палець стояв більше 100 мс перед відпусканням: aMs - N.ms = 101 > 100, v=0.
	TKScroll S;
	ScrollFlickUp(S, 1000);
	CHECK_EQ(KScrollUp(S, 1141), EKsNone);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iVel8, 0);
}

static void TestReleaseRestingExactlyHundredMsStillFlings()
{
	// Межа: затримка рівно 100 мс не перевищує KKsVelocityWindowMs, тож швидкість ще рахується.
	TKScroll S;
	ScrollFlickUp(S, 1000);
	CHECK_EQ(KScrollUp(S, 1140), EKsNone);
	CHECK_EQ(S.iState, EKsFlinging);
}

static void TestReleaseBelowMinFlingStops()
{
	// Вікно 600@1000 -> 589@1040: 11 px за 40 мс = 275 px/s < 300. Зупинка.
	// Старт з 100 px, щоб не вийти за межу.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	KScrollTo(S, 100, 0, 1000);
	KScrollDown(S, 600, 1000);
	KScrollMove(S, 579, 1010);
	KScrollMove(S, 589, 1040);
	CHECK_EQ(KScrollUp(S, 1040), EKsNone);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iVel8, 0);
}

static void TestReleaseAtMinFlingStarts()
{
	// 12 px за 40 мс = 300 px/s = iMinFling: |v| >= iMinFling починає кидок, iVel8 = 76800.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	KScrollTo(S, 100, 0, 1000);
	KScrollDown(S, 600, 1000);
	KScrollMove(S, 579, 1010);
	KScrollMove(S, 588, 1040);
	CHECK_EQ(KScrollUp(S, 1040), EKsNone);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, 300 * 256);
}

static void TestReleaseVelocityClampedToMaxFlingUpward()
{
	// 600 -> 200 за 40 мс: v = 400*1000/40 = 10000, обрізано до iMaxFling = 8000.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	KScrollDown(S, 600, 1000);
	KScrollMove(S, 579, 1010);
	KScrollMove(S, 200, 1040);
	CHECK_EQ(KScrollUp(S, 1040), EKsNone);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, 8000 * 256);
}

static void TestReleaseVelocityClampedToMaxFlingDownwardKeepsSign()
{
	// Старт 400 px, вниз: 500 -> 900 за 40 мс дає v=-10000, обрізано до -8000.
	// Позиція при відпусканні 400 + (521-900) = 21 px, у межах.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	KScrollTo(S, 400, 0, 1000);
	KScrollDown(S, 500, 1000);
	KScrollMove(S, 521, 1010);
	KScrollMove(S, 900, 1040);
	CHECK_EQ(KScrollUp(S, 1040), EKsNone);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, -8000 * 256);
	CHECK_EQ(S.iPos8, 21 * 256);
}

static void TestReleaseOutOfRangeSettlesToNearestEnd()
{
	// Позиція -50 px при відпусканні: EKsSettling до 0.
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 500, 1000);
	KScrollMove(S, 521, 1010);
	KScrollMove(S, 621, 1020);
	CHECK_EQ(KScrollUp(S, 1030), EKsNone);
	CHECK_EQ(S.iState, EKsSettling);
	CHECK_EQ(S.iTarget8, 0);
}

static void TestReleaseAboveMaxSettlesToMax()
{
	// Позиція 721 px при відпусканні: EKsSettling до max = 700.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 700, 0, 1000);
	KScrollDown(S, 500, 1100);
	KScrollMove(S, 521, 1110);
	KScrollMove(S, 479, 1120);
	CHECK_EQ(KScrollUp(S, 1130), EKsNone);
	CHECK_EQ(S.iState, EKsSettling);
	CHECK_EQ(S.iTarget8, 700 * 256);
}

// ---------------------------------------------------------------- кидок: динаміка

static void TestFlingFirstMillisecondExact()
{
	// Після відпускання: iStepMs=1040, pos=59 px = 15104, vel=512000. Один мілісекунд:
	// pos += 512000/1000 = 512 -> 15616. vel -= (512000*201)>>16 = 1570 -> 510430.
	TKScroll S;
	ScrollFlickUp(S, 1000);
	KScrollUp(S, 1040);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iStepMs, 1040);
	KScrollStep(S, 1041);
	CHECK_EQ(S.iPos8, 15616);
	CHECK_EQ(S.iVel8, 510430);
	CHECK_EQ(S.iState, EKsFlinging);
}

static void TestFlingDecaysMonotonicallyAndStopsWithinTenSeconds()
{
	// |vel| не зростає; зупинка до 10 с; кінцева позиція — ціле px у [0, max].
	TKScroll S;
	unsigned t;
	int prev;
	int mono = 1;
	ScrollFlickUp(S, 1000);
	KScrollUp(S, 1040);
	prev = S.iVel8 < 0 ? -S.iVel8 : S.iVel8;
	for (t = 1041; t <= 11040 && S.iState != EKsIdle; ++t) {
		int a;
		KScrollStep(S, t);
		a = S.iVel8 < 0 ? -S.iVel8 : S.iVel8;
		if (a > prev) mono = 0;
		prev = a;
	}
	CHECK_EQ(mono, 1);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iVel8, 0);
	CHECK_EQ(S.iPos8 % 256, 0);
	CHECK(S.iPos8 >= 0 && S.iPos8 <= 700 * 256);
}

static void TestFlingUpperEdgeStopsExactlyOnEdge()
{
	// Старт 640 px, відпускання на 699 px (в межах), швидкість 2000 px/s вгору. Наступна
	// мілісекунда дає 701 px > 700: зупинка рівно на краю, vel=0, Idle.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	KScrollTo(S, 640, 0, 1000);
	KScrollDown(S, 600, 1100);
	KScrollMove(S, 579, 1110);
	KScrollMove(S, 560, 1120);
	KScrollMove(S, 540, 1130);
	KScrollMove(S, 520, 1140);
	KScrollUp(S, 1140);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iPos8, 699 * 256);
	KScrollStep(S, 1141);
	CHECK_EQ(S.iPos8, 700 * 256);
	CHECK_EQ(S.iVel8, 0);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestFlingLowerEdgeStopsExactlyOnEdge()
{
	// Вниз: 100 px -> 41 px при відпусканні, v = (600-680)*1000/40 = -2000 px/s. Зупинка на 0.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	KScrollTo(S, 100, 0, 1000);
	KScrollDown(S, 600, 1100);
	KScrollMove(S, 621, 1110);
	KScrollMove(S, 640, 1120);
	KScrollMove(S, 660, 1130);
	KScrollMove(S, 680, 1140);
	KScrollUp(S, 1140);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, -2000 * 256);
	CHECK_EQ(S.iPos8, 41 * 256);
	KScrollStep(S, 1300);
	CHECK_EQ(S.iPos8, 0);
	CHECK_EQ(S.iVel8, 0);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestDownDuringFlingStopsWithoutTapOrLongPress()
{
	// Дотик на кидку зупиняє список: позиція не змінюється, iCaught, немає ні дотику, ні довгого натискання.
	TKScroll S;
	int before;
	ScrollFlickUp(S, 1000);
	KScrollUp(S, 1040);
	KScrollStep(S, 1100);
	before = S.iPos8;
	KScrollDown(S, 300, 1100);
	CHECK_EQ(S.iState, EKsPressed);
	CHECK_EQ(S.iCaught, 1);
	CHECK_EQ(S.iPos8, before);
	CHECK_EQ(KScrollStep(S, 1700), EKsNone);
	CHECK_EQ(KScrollUp(S, 1700), EKsNone);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestDownDuringSettlingIsCaught()
{
	// Дотик під час EKsSettling також "спіймав": iCaught=1, без дотику й довгого натискання.
	TKScroll S;
	int before;
	ScrollInit100(S);
	KScrollTo(S, 500, 1, 1000);
	KScrollStep(S, 1050);
	before = S.iPos8;
	KScrollDown(S, 300, 1050);
	CHECK_EQ(S.iCaught, 1);
	CHECK_EQ(S.iPos8, before);
	CHECK_EQ(KScrollStep(S, 1600), EKsNone);
	CHECK_EQ(KScrollUp(S, 1600), EKsNone);
}

static void TestTimeSliceNeverExceedsTwoFiftyMs()
{
	// Крок понад 250 мс рахується як 250.
	TKScroll A, B;
	ScrollFlickUp(A, 1000);
	KScrollUp(A, 1040);
	ScrollFlickUp(B, 1000);
	KScrollUp(B, 1040);
	KScrollStep(A, 1040 + 300);
	KScrollStep(B, 1040 + 250);
	CHECK_EQ(A.iPos8, B.iPos8);
	CHECK_EQ(A.iVel8, B.iVel8);
	CHECK_EQ(A.iState, B.iState);
}

// ---------------------------------------------------------------- доведення, переходи, позиція

// KScrollStep обмежує крок 250 мс; довші проміжки проходимо частинами.
static void StepTo(TKScroll &aS, unsigned aTo)
{
	unsigned now = aS.iStepMs;
	while (now < aTo) {
		unsigned n = (aTo - now > 250u) ? now + 250u : aTo;
		KScrollStep(aS, n);
		now = n;
	}
}

static void TestSettleAnimatedFromBelowAndAbove()
{
	// KScrollTo з анімацією: перший ms від 0 до 500 px: d=128000, >>6 = 2000.
	// Збіжність до цілі точно, стан Idle, vel=0.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 500, 1, 1000);
	CHECK_EQ(S.iState, EKsSettling);
	CHECK_EQ(S.iTarget8, 500 * 256);
	CHECK_EQ(S.iStepMs, 1000);
	KScrollStep(S, 1001);
	CHECK_EQ(S.iPos8, 2000);
	StepTo(S, 2000);
	CHECK_EQ(S.iPos8, 500 * 256);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iVel8, 0);
	KScrollTo(S, 700, 0, 2000);
	KScrollTo(S, 500, 1, 2000);
	StepTo(S, 4000);
	CHECK_EQ(S.iPos8, 500 * 256);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestScrollToInstantClampsAndIgnoresFinger()
{
	// Без анімації одразу, обрізання до [0, max]; під пальцем не діє.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 500, 0, 1000);
	CHECK_EQ(S.iPos8, 500 * 256);
	CHECK_EQ(S.iState, EKsIdle);
	KScrollTo(S, 2000, 0, 1000);
	CHECK_EQ(S.iPos8, 700 * 256);
	KScrollTo(S, -5, 0, 1000);
	CHECK_EQ(S.iPos8, 0);
	KScrollTo(S, 300, 0, 1000);
	KScrollDown(S, 500, 1000);
	KScrollTo(S, 200, 0, 1000);
	CHECK_EQ(S.iPos8, 300 * 256);
	CHECK_EQ(S.iState, EKsPressed);
}

static void TestEnsureVisibleAllBranchesFromIdle()
{
	// Гілки KScrollEnsureVisible; початок pos = 400 px.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 400, 0, 1000);
	KScrollEnsureVisible(S, 100, 150, 0, 1000);
	CHECK_EQ(S.iPos8, 100 * 256);
	KScrollTo(S, 400, 0, 1000);
	KScrollEnsureVisible(S, 650, 750, 0, 1000);
	CHECK_EQ(S.iPos8, 450 * 256);
	KScrollTo(S, 400, 0, 1000);
	KScrollEnsureVisible(S, 600, 1000, 0, 1000);
	CHECK_EQ(S.iPos8, 600 * 256);
	KScrollTo(S, 400, 0, 1000);
	KScrollEnsureVisible(S, 450, 500, 0, 1000);
	CHECK_EQ(S.iPos8, 400 * 256);
}

static void TestEnsureVisibleUsesSettlingTargetAsBase()
{
	// У EKsSettling база — iTarget8/256 = 100 px, а не поточна позиція 400.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 400, 0, 1000);
	KScrollTo(S, 100, 1, 1000);
	CHECK_EQ(S.iState, EKsSettling);
	KScrollEnsureVisible(S, 350, 360, 0, 1000);
	CHECK_EQ(S.iTarget8, 100 * 256);
	// Без анімації KScrollTo ставить зсув одразу й переводить стан в EKsIdle.
	KScrollEnsureVisible(S, 50, 60, 0, 1000);
	CHECK_EQ(S.iPos8, 50 * 256);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestEnsureVisibleAnimatedStartsSettling()
{
	// З анімацією стан стає EKsSettling з ціллю.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 400, 0, 1000);
	KScrollEnsureVisible(S, 100, 150, 1, 1000);
	CHECK_EQ(S.iState, EKsSettling);
	CHECK_EQ(S.iTarget8, 100 * 256);
	CHECK_EQ(S.iStepMs, 1000);
}

static void TestSetExtentClampsIdleAndSetsSizes()
{
	// Ненульовий стан Idle: ущільнюється до нового max=300 одразу.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 700, 0, 1000);
	KScrollSetExtent(S, 300, 600);
	CHECK_EQ(S.iView, 300);
	CHECK_EQ(S.iContent, 600);
	CHECK_EQ(S.iPos8, 300 * 256);
}

static void TestActiveFlagsByState()
{
	// KScrollActive: 0 для Idle і Pressed; ненульове для Dragging і Settling.
	TKScroll S;
	ScrollInit100(S);
	CHECK_EQ(KScrollActive(S), 0);
	KScrollDown(S, 500, 1000);
	CHECK_EQ(KScrollActive(S), 0);
	KScrollMove(S, 521, 1010);
	CHECK(KScrollActive(S) != 0);
	KScrollTo(S, 100, 1, 1010);
	CHECK(KScrollActive(S) != 0);
}

static void TestPosFloorForNegativeOffsets()
{
	// KScrollPos = floor((iPos8+128)/256), також для від'ємних.
	TKScroll S;
	ScrollInit100(S);
	S.iPos8 = 0;     CHECK_EQ(KScrollPos(S), 0);
	S.iPos8 = 127;   CHECK_EQ(KScrollPos(S), 0);
	S.iPos8 = 128;   CHECK_EQ(KScrollPos(S), 1);
	S.iPos8 = 256;   CHECK_EQ(KScrollPos(S), 1);
	S.iPos8 = -1;    CHECK_EQ(KScrollPos(S), 0);
	S.iPos8 = -128;  CHECK_EQ(KScrollPos(S), 0);
	S.iPos8 = -129;  CHECK_EQ(KScrollPos(S), -1);
	S.iPos8 = -256;  CHECK_EQ(KScrollPos(S), -1);
	S.iPos8 = -257;  CHECK_EQ(KScrollPos(S), -1);
	S.iPos8 = -384;  CHECK_EQ(KScrollPos(S), -1);
	S.iPos8 = -385;  CHECK_EQ(KScrollPos(S), -2);
}

// ---------------------------------------------------------------- незалежність від нарізки часу

static int gApos[3001], gAvel[3001], gAst[3001];

static void TimeSliceFling(TKScroll &aS, unsigned aBase)
{
	ScrollFlickUp(aS, aBase);
	KScrollUp(aS, aBase + 40);
}

static void TestTimeSlicingGivesSameStateAtCommonMoments()
{
	// Одна й та сама інтеграція, пройдена кроками 1, 7, 16, 33 і випадковими до 250 мс,
	// у спільні моменти дає побітово той самий стан.
	TKScroll A, B;
	TRng R;
	unsigned dt;
	unsigned done = 0;
	int bad = 0;
	int cutIdx = 0;
	static const int kFixed[4] = { 1, 7, 16, 33 };
	TimeSliceFling(A, 1000);
	for (dt = 1; dt <= 3000; ++dt) {
		KScrollStep(A, 1040 + dt);
		gApos[dt] = A.iPos8;
		gAvel[dt] = A.iVel8;
		gAst[dt] = A.iState;
	}
	TimeSliceFling(B, 1000);
	R.iS = 4242u;
	while (done < 3000) {
		unsigned cut;
		if (cutIdx < 4) cut = (unsigned)kFixed[cutIdx];
		else cut = 1u + (Rnd(R) % 250u);
		++cutIdx;
		if (done + cut > 3000) cut = 3000 - done;
		KScrollStep(B, 1040 + done + cut);
		done += cut;
		if (B.iPos8 != gApos[done] || B.iVel8 != gAvel[done] || B.iState != gAst[done]) ++bad;
	}
	CHECK_EQ(bad, 0);
}

static void TestWrappingClockGivesSameResults()
{
	// Годинник проходить через 0xFFFFFFFF -> 0: 0xFFFFFFE0 + 40 = 8.
	TKScroll W, N;
	unsigned k;
	int bad = 0;
	TimeSliceFling(W, 0xFFFFFFE0u);
	TimeSliceFling(N, 1000u);
	CHECK_EQ(W.iPos8, N.iPos8);
	CHECK_EQ(W.iVel8, N.iVel8);
	for (k = 1; k <= 2000; ++k) {
		KScrollStep(W, 0xFFFFFFE0u + 40u + k);
		KScrollStep(N, 1040u + k);
		if (W.iPos8 != N.iPos8 || W.iVel8 != N.iVel8 || W.iState != N.iState) ++bad;
	}
	CHECK_EQ(bad, 0);
	CHECK_EQ(W.iState, N.iState);
}

static void TestWrappingClockLongPressAcrossZero()
{
	// Довге натискання, що починається біля 0xFFFFFFF0 і переходить через 0, спрацьовує рівно на 500-й мс.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	KScrollDown(S, 500, 0xFFFFFFF0u);
	CHECK_EQ(KScrollStep(S, 0xFFFFFFF0u + 499u), EKsNone);
	CHECK_EQ(KScrollStep(S, 0xFFFFFFF0u + 500u), EKsLongPress);
}

// ---------------------------------------------------------------- уточнення заголовка

// Два прогони з однаковими подіями, але різними розкладками: мають збігатися біти, стік і фронти.
static int LockstepMismatches(const TVpadLayout &aA, const TVpadLayout &aB, unsigned aSeed, int aSteps)
{
	TVpadState SA, SB;
	TRng R;
	int i;
	int bad = 0;
	unsigned long t = 1000;
	R.iS = aSeed;
	VpadReset(SA);
	VpadReset(SB);
	for (i = 0; i < aSteps; ++i) {
		int op = RndRange(R, 0, 9);
		int id = RndRange(R, 0, 3);
		int x = RndRange(R, -20, 660);
		int y = RndRange(R, -20, 380);
		t += (unsigned)RndRange(R, 0, 20);
		if (op < 4) {
			VpadPointerDown(SA, aA, id, x, y, (unsigned)t);
			VpadPointerDown(SB, aB, id, x, y, (unsigned)t);
		} else if (op < 8) {
			VpadPointerMove(SA, aA, id, x, y, (unsigned)t);
			VpadPointerMove(SB, aB, id, x, y, (unsigned)t);
		} else if (op < 9) {
			VpadPointerUp(SA, aA, id, (unsigned)t);
			VpadPointerUp(SB, aB, id, (unsigned)t);
		} else {
			(void)VpadTakeEdges(SA);
			(void)VpadTakeEdges(SB);
		}
		if (SA.iBits != SB.iBits || SA.iLx != SB.iLx || SA.iLy != SB.iLy || SA.iRightX != SB.iRightX || SA.iRightY != SB.iRightY || SA.iEdges != SB.iEdges) ++bad;
	}
	return bad;
}

static void LayoutShapeVariant(TVpadLayout &aL, int aKind, int aShape, int aRy)
{
	// Хрестовина або стік (100,100) R=50. Заголовок ігнорує iShape для них; iRy тут
	// лише перевіряє, що поле справді не впливає.
	LayoutInit(aL, 1);
	SetCircCtl(aL.iC[0], 100, 100, 50, 0, 0);
	aL.iC[0].iKind = (unsigned char)aKind;
	aL.iC[0].iShape = (unsigned char)aShape;
	aL.iC[0].iRy = (short)aRy;
}

static void TestDpadRectShapeWithZeroRyActsAsCircle()
{
	// Хрестовина (100,100) R=50 з iShape=Rect, iRy=0 — як коло. Точка (100,140): dist 40 <= зона
	// натиску 62, захоплено; dy>0 дає Down (64). Рух на (140,100): Right (32). На (140,140): діагональ 96.
	TVpadLayout Lc, Lr;
	TVpadState S;
	LayoutShapeVariant(Lc, EVpadKindDpad, EVpadShapeCircle, 0);
	LayoutShapeVariant(Lr, EVpadKindDpad, EVpadShapeRect, 0);
	VpadReset(S);
	VpadPointerDown(S, Lr, 0, 100, 140, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadDown);
	VpadPointerMove(S, Lr, 0, 140, 100, 1010);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadPointerMove(S, Lr, 0, 140, 140, 1020);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestStickRectShapeWithZeroRyActsAsCircle()
{
	// Стік (100,100) R=50, точка (100,150): dist 50, зона 62, захоплено. dead=9, mag=41,
	// span=41, m=127; iLy=127+50*127/50=254; цифрово: 50*100=5000 >= 50*80=4000, Down (64).
	// Рух на (120,100): mag=11, m=11*127/41=34, iLx=127+20*34/20=161; цифрово 2000 < 4000, біт немає.
	TVpadLayout Lc, Lr;
	TVpadState S;
	LayoutShapeVariant(Lc, EVpadKindStick, EVpadShapeCircle, 0);
	LayoutShapeVariant(Lr, EVpadKindStick, EVpadShapeRect, 0);
	VpadReset(S);
	VpadPointerDown(S, Lr, 0, 100, 150, 1000);
	CHECK_EQ(S.iLy, 254);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iBits, 1 << EVpadDown);
	VpadPointerMove(S, Lr, 0, 120, 100, 1010);
	CHECK_EQ(S.iLx, 161);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iBits, 0);
}

static void TestDpadAndStickShapeIgnoredInLayoutFits()
{
	// Коробка хрестовини/стіка — центр +- iRx з обох боків при будь-якій формі.
	// 100x100, коло R=50 в (50,50): x 0..100, 100 не < W, тож 0. R=49: 1. Так само для Rect iRy=0.
	// 300x300: хрестовина (100,100) R=50 має коробку y 50..150; кнопка (100,160) з півосями (10,10)
	// має y 150..170: торкання краєм перетинає, тож 0 і для Rect iRy=0 (коробка не має стиснутися).
	static const int kKinds[2] = { EVpadKindDpad, EVpadKindStick };
	int k;
	for (k = 0; k < 2; ++k) {
		TVpadLayout L;
		LayoutInit(L, 1);
		SetCircCtl(L.iC[0], 50, 50, 50, 0, 0);
		L.iC[0].iKind = (unsigned char)kKinds[k];
		L.iC[0].iShape = (unsigned char)EVpadShapeRect;
		L.iC[0].iRy = 0;
		sprintf(gWhere, "%s kind %d", gTest, kKinds[k]);
		CHECK_EQ(VpadLayoutFits(L, 100, 100), 0);
		L.iC[0].iRx = 49;
		CHECK_EQ(VpadLayoutFits(L, 100, 100), 1);
		LayoutInit(L, 2);
		SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
		L.iC[0].iKind = (unsigned char)kKinds[k];
		L.iC[0].iShape = (unsigned char)EVpadShapeRect;
		L.iC[0].iRy = 0;
		SetRectCtl(L.iC[1], 100, 160, 10, 10, 0);
		CHECK_EQ(VpadLayoutFits(L, 300, 300), 0);
		strcpy(gWhere, gTest);
	}
}

static void TestShapeIgnoredLockstepOnDefaultLayouts()
{
	// Той самий потік подій для кола і для прямокутника з iRy=0 або 1: без жодної розбіжності.
	TVpadLayout Lc, Lr0, Lr1, Sc, Sr0;
	memset(&Lc, 0, sizeof Lc);
	memset(&Sc, 0, sizeof Sc);
	VpadDefaultLayout(Lc, 640, 360, 138, 100, 0);
	VpadDefaultLayout(Sc, 640, 360, 138, 100, EVpadOptStick);
	Lr0 = Lc; Lr0.iC[EVpadIxDir].iShape = (unsigned char)EVpadShapeRect; Lr0.iC[EVpadIxDir].iRy = 0;
	Lr1 = Lc; Lr1.iC[EVpadIxDir].iShape = (unsigned char)EVpadShapeRect; Lr1.iC[EVpadIxDir].iRy = 1;
	Sr0 = Sc; Sr0.iC[EVpadIxDir].iShape = (unsigned char)EVpadShapeRect; Sr0.iC[EVpadIxDir].iRy = 0;
	CHECK_EQ(LockstepMismatches(Lc, Lr0, 1111u, 20000), 0);
	CHECK_EQ(LockstepMismatches(Lc, Lr1, 2222u, 20000), 0);
	CHECK_EQ(LockstepMismatches(Sc, Sr0, 3333u, 20000), 0);
}

static void TestCountBeyondMaxTakenAsMax()
{
	// iCount > KVpadMaxControls береться як 16. Елементи 12..15 нульові (iRx=0) і ігноруються,
	// тож 12, 16, 17 і 1000 дають ті самі результати й той самий вердикт коробок.
	TVpadLayout A, B, C, D;
	memset(&A, 0, sizeof A);
	VpadDefaultLayout(A, 640, 360, 138, 100, 0);
	B = A; B.iCount = 16;
	C = A; C.iCount = 17;
	D = A; D.iCount = 1000;
	CHECK_EQ(LockstepMismatches(A, B, 77u, 20000), 0);
	CHECK_EQ(LockstepMismatches(A, C, 78u, 20000), 0);
	CHECK_EQ(LockstepMismatches(A, D, 79u, 20000), 0);
	CHECK_EQ(VpadLayoutFits(C, 640, 360), 1);
	CHECK_EQ(VpadLayoutFits(D, 640, 360), 1);
}

static void TestCountNegativeTakenAsZero()
{
	// Від'ємне iCount береться як 0: нічого не захоплюється, і коробок немає.
	TVpadLayout A, Z;
	TVpadState S;
	memset(&A, 0, sizeof A);
	VpadDefaultLayout(A, 640, 360, 138, 100, 0);
	A.iCount = -1;
	VpadReset(S);
	VpadPointerDown(S, A, 0, 94, 266, 1000);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iP[0].iCtl, -1);
	memset(&Z, 0, sizeof Z);
	Z.iCount = 0;
	CHECK_EQ(VpadLayoutFits(A, 640, 360), 1);
	CHECK_EQ(VpadLayoutFits(Z, 640, 360), 1);
}

static void TestDpadNegativeDiagPctTakenAsZero()
{
	// Від'ємне iDiagPct береться як 0: лише чотири напрямки. (40,40) — Right.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, -50, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	DpadAt(S, L, 40, 40);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestStickNegativeDeadPctTakenAsZero()
{
	// Від'ємне iDeadPct береться як 0. Стік R=80, dead=0, span=80.
	// (40,0): mag=40, m=40*127/80=63, iLx=127+63=190. (1,0): mag=1, m=127/80=1, iLx=128.
	// Якби від'ємний відсоток дав dead=-16: було б 201 і 149.
	TVpadLayout L;
	TVpadState S;
	LayoutStick(L, 0);
	L.iC[0].iRx = 80;
	L.iDeadPct = -20;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	CHECK_EQ(S.iLx, 127);
	VpadPointerMove(S, L, 0, 140, 100, 1010);
	CHECK_EQ(S.iLx, 190);
	CHECK_EQ(S.iLy, 127);
	VpadPointerMove(S, L, 0, 101, 100, 1020);
	CHECK_EQ(S.iLx, 128);
}

static void TestStickDigitalNotPressedWhileCentred()
{
	// Стік R=80, iDeadPct=30 (dead=24), iStickDigitalPct=10 (поріг 8). Точка на 16 (20 %): стік у центрі,
	// біти немає, хоча 16*100 = 1600 >= 80*10. На 23 — ще центр. На 24 = dead —
	// вже не в центрі, Right (32) при iLx=127 (mag=0). Без цифрових бітів (iStickDigitalPct=0) — нічого.
	TVpadLayout L;
	TVpadState S;
	LayoutStick(L, 0);
	L.iDeadPct = 30;
	L.iStickDigitalPct = 10;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	VpadPointerMove(S, L, 0, 116, 100, 1010);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
	VpadPointerMove(S, L, 0, 123, 100, 1020);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iLx, 127);
	VpadPointerMove(S, L, 0, 124, 100, 1030);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	CHECK_EQ(S.iLx, 127);
	L.iStickDigitalPct = 0;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	VpadPointerMove(S, L, 0, 150, 100, 1010);
	CHECK_EQ(S.iBits, 0);
}

static void TestDualP2ClampedToCoordinateRangePositive()
{
	// Палець 0 у (8100,100) захоплює хрестовину R=50. Рух на (8160,100): P2 = 2*8160-8100 = 8220,
	// обрізано до 8191. Кнопка (8189,100) R=2, повідець 3: 8191 у ній, 8220 ні.
	// Гніздо 1 з'являється в (8191,100): iX == 8191, iDual == 1.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 8100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(L.iC[1], 8189, 100, 2, 0, 1 << EVpadCross);
	L.iDualJumpPx = 60;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 8100, 100, 1000);
	VpadPointerMove(S, L, 0, 8160, 100, 1060);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iP[1].iX, 8191);
}

static void TestDualMirroredSlotClampedToCoordinateRangeNegative()
{
	// Після активації (L=(100,100), кнопка Cross в (220,100) з повідцем 70, P2=(220,100)).
	// Рух на (-5000,100): третя гілка, гніздо 1 іде в 2*(-5000)-100 = -10100, обрізано до -8192
	//. Палець 0 лишається в (iDualX, iDualY) = (100,100).
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(L.iC[1], 220, 100, 40, 0, 1 << EVpadCross);
	L.iDualJumpPx = 60;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	VpadPointerMove(S, L, 0, 160, 100, 1060);
	CHECK_EQ(S.iDual, 1);
	VpadPointerMove(S, L, 0, -5000, 100, 1070);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iX, -8192);
	CHECK_EQ(S.iP[0].iX, 100);
	CHECK_EQ(S.iP[0].iY, 100);
	CHECK_EQ(S.iDualX, 100);
	CHECK_EQ(S.iDualY, 100);
}

static void TestDualCandidatesNormalisedAgainstLeashNotRaw()
{
	// Хрестовина (100,100) R50 захоплена при (140,100). Рух на (240,100): P2=(340,100).
	// Кнопка Cross (300,100) R100, повідець 175: dist 40, нормована 40*1000/175 = 228.
	// Кнопка Circle (370,100) R20, повідець 35: dist 30, нормована 30*1000/35 = 857.
	// За нормованою Cross ближча; за сирою відстанню вибрали б Circle.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 3);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(L.iC[1], 300, 100, 100, 0, 1 << EVpadCross);
	SetCircCtl(L.iC[2], 370, 100, 20, 0, 1 << EVpadCircle);
	L.iDualJumpPx = 60;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	VpadPointerMove(S, L, 0, 240, 100, 1060);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadCross));
}

static void TestDualStickCandidateEvaluatedAtSecondPoint()
{
	// Стік (300,100) R80 як кандидат: повідець 140, P2=(340,100) у ньому (dist 40). Гніздо 1 оцінює
	// стік у P2 одразу: dx=40, mag=26, m=50, iLx=127+40*50/40=177, біти немає (4000 < 6400).
	// Палець 0 лишає хрестовину (Right, 32).
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(L.iC[1], 300, 100, 80, 0, 0);
	L.iC[1].iKind = (unsigned char)EVpadKindStick;
	L.iDualJumpPx = 60;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	VpadPointerMove(S, L, 0, 240, 100, 1060);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iLx, 177);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestCaptureRectAgainstCircleByNormalisedDistance()
{
	// Коло (0,0) R40 (зона 50) і прямокутник (100,0) з півосями (100,20) (зона (125,25)).
	// Точка (30,0): коло 30*1000/50 = 600; прямокутник 70*1000/125 = 560. Перемагає прямокутник
	// (сирою відстанню перемогло б коло, 30 < 70).
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 0, 0, 40, 0, 1 << EVpadCross);
	SetRectCtl(L.iC[1], 100, 0, 100, 20, 0);
	L.iC[1].iBits = 1 << EVpadCircle;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 30, 0, 1000);
	CHECK_EQ(S.iP[0].iCtl, 1);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
}

static void TestFlingStopRoundsToNearestPixelHalfUp()
{
	// Зупинка кидка: iPos8 округлюється до найближчого цілого, половина вгору.
	// Стан задано полями (структура відкрита). vel=100 < 19200: зупинка на першому мілісекунді.
	// 10.5 px -> 11 px; 10 px + 127/256 -> 10 px; для від'ємної швидкості 5.5 px -> 6 px.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	S.iState = EKsFlinging;
	S.iPos8 = 10 * 256 + 128;
	S.iVel8 = 100;
	S.iStepMs = 1000;
	KScrollStep(S, 1001);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iVel8, 0);
	CHECK_EQ(S.iPos8, 11 * 256);
	KScrollInit(S, 300, 1000, 100);
	S.iState = EKsFlinging;
	S.iPos8 = 10 * 256 + 127;
	S.iVel8 = 100;
	S.iStepMs = 1000;
	KScrollStep(S, 1001);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iPos8, 10 * 256);
	KScrollInit(S, 300, 1000, 100);
	S.iState = EKsFlinging;
	S.iPos8 = 5 * 256 + 128;
	S.iVel8 = -100;
	S.iStepMs = 1000;
	KScrollStep(S, 1001);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iPos8, 6 * 256);
}

static void TestVelocityIgnoresSamplesOlderThanWindow()
{
	// Зразки: 700@1000, 679@1010 (анкер), 600@1030, 575@1080, 565@1150. N=565@1150. O — найстаріший
	// з N.ms-O.ms <= 100, тобто 575@1080: v=(575-565)*1000/70=142 < 300, зупинка.
	// Якщо брати найстаріший зразок 700@1000 без вікна, v=135*1000/150=900 і був би кидок.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	KScrollDown(S, 700, 1000);
	KScrollMove(S, 679, 1010);
	KScrollMove(S, 600, 1030);
	KScrollMove(S, 575, 1080);
	KScrollMove(S, 565, 1150);
	CHECK_EQ(KScrollUp(S, 1150), EKsNone);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iVel8, 0);
}

static void TestSetExtentDuringFlingClampsOnNextMillisecond()
{
	// Кидок вгору з 59 px; нова довжина вмісту 250 < вікна 300: max=0. Кидок не обривається одразу
	//, але наступна мілісекунда залишає [0,max] і зупиняє на 0.
	TKScroll S;
	ScrollFlickUp(S, 1000);
	KScrollUp(S, 1040);
	CHECK_EQ(S.iState, EKsFlinging);
	const int before8 = S.iPos8;
	const int beforeVel = S.iVel8;
	KScrollSetExtent(S, 300, 250);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iPos8, before8);
	CHECK_EQ(S.iVel8, beforeVel);
	KScrollStep(S, 1041);
	CHECK_EQ(S.iPos8, 0);
	CHECK_EQ(S.iVel8, 0);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestSetExtentDuringSettleClampsTarget()
{
	// KScrollTo(500, анімація) з 0; нова довжина дає max=300. Ціль обрізається до 300 px.
	// Доведення сходиться до 76800 (= 300*256) і стає Idle.
	TKScroll S;
	ScrollInit100(S);
	KScrollTo(S, 500, 1, 1000);
	KScrollSetExtent(S, 300, 600);
	CHECK_EQ(S.iTarget8, 300 * 256);
	CHECK_EQ(S.iState, EKsSettling);
	StepTo(S, 3000);
	CHECK_EQ(S.iPos8, 300 * 256);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestCountBeyondMaxKeepsSixteenthControl()
{
	// Елемент 15 (шістнадцятий) береться, коли iCount дорівнює 16 або 1000, і не береться при 15
	//. Кнопка Circle в (600,300) R20: притиснення в її зоні дає її біт і індекс 15.
	TVpadLayout L;
	TVpadState S;
	memset(&L, 0, sizeof L);
	LayoutInit(L, 16);
	SetCircCtl(L.iC[15], 600, 300, 20, 0, 1 << EVpadCircle);
	L.iCount = 1000;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 600, 300, 1000);
	CHECK_EQ(S.iP[0].iCtl, 15);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
	L.iCount = 15;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 600, 300, 1000);
	CHECK_EQ(S.iBits, 0);
}

static void TestRandomSequenceInvariantsStickLayout()
{
	// Стік як лівий елемент: інваріанти, зокрема центр стіка, коли його ніхто не тримає.
	TVpadLayout L, M;
	memset(&L, 0, sizeof L);
	VpadDefaultLayout(L, 640, 360, 138, 100, EVpadOptStick);
	RunRandomSequence(L, 1357913579u, 30000);
	memset(&M, 0, sizeof M);
	VpadDefaultLayout(M, 640, 360, 138, 100, EVpadOptStick | EVpadOptResistiveDual);
	RunRandomSequence(M, 2468013579u, 30000);
}

// ---------------------------------------------------------------- третє коло: нові правила заголовка

// Один елемент потрібного виду; параметри за замовчуванням з LayoutInit.
static void OneDpad(TVpadLayout &aL, int aCx, int aCy, int aR)
{
	LayoutInit(aL, 1);
	SetCircCtl(aL.iC[0], aCx, aCy, aR, 0, 0);
	aL.iC[0].iKind = (unsigned char)EVpadKindDpad;
}

static void OneStick(TVpadLayout &aL, int aCx, int aCy, int aR)
{
	LayoutInit(aL, 1);
	SetCircCtl(aL.iC[0], aCx, aCy, aR, 0, 0);
	aL.iC[0].iKind = (unsigned char)EVpadKindStick;
}

static void OneButton(TVpadLayout &aL, int aCx, int aCy, int aR, unsigned aBits)
{
	LayoutInit(aL, 1);
	SetCircCtl(aL.iC[0], aCx, aCy, aR, 0, aBits);
}

// Хрестовина (4000,4000) R=1000: натискання в (4000,5000) дає Down; рух у (6829,6829) дає dist=4000,
// dx=dy=2829, q=1000. Діагональ тоді залежить від порогу 400%.
static void TestFourWayPctAboveFourHundredTakenAsFourHundred()
{
	// Dpad R=8000 у (4000,4000), iFourWayPct=300000 береться як 400. Точка (5414,5414):
	// dx=dy=1414, dist=isqrt(2*1414^2)=1999 (1999^2=3996001 <= 3998792 < 4000000). Поріг
	// R*400 = 3200000 > dist*100 = 199900, тож діагоналі немає; рівність |dx|=|dy| дає Right (32).
	// Без обрізання 8000*300000 переповнює int і поріг стає від'ємним: діагональ (96).
	TVpadLayout L;
	TVpadState S;
	OneDpad(L, 4000, 4000, 8000);
	L.iFourWayPct = 300000;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 5414, 5414, 1000);
	CHECK_EQ(S.iP[0].iCtl, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestFourWayPctFourHundredAndOneGiveSameDiagonal()
{
	// Dpad R=1000 у (4000,4000). Натискання (4000,5000): dist 1000, Down (64). Рух до (6829,6829):
	// dx=dy=2829, dist=isqrt(2*2829^2)=4000 (4000^2=16000000 <= 16000482 < 4001^2). Поріг при 400:
	// dist*100 = 400000 >= 1000*400 = 400000, тож діагональ дозволена; після одного біта T+35 = 449,
	// q=1000, результат Right|Down (96). При 401 без обрізання: 400000 < 401000, діагоналі немає (32).
	// Значення 1000 також береться як 400 (96).
	static const int kPct[3] = { 400, 401, 1000 };
	int i;
	TVpadLayout L;
	TVpadState S;
	for (i = 0; i < 3; ++i) {
		OneDpad(L, 4000, 4000, 1000);
		L.iFourWayPct = kPct[i];
		sprintf(gWhere, "%s pct %d", gTest, kPct[i]);
		VpadReset(S);
		VpadPointerDown(S, L, 0, 4000, 5000, 1000);
		CHECK_EQ(S.iBits, 1 << EVpadDown);
		VpadPointerMove(S, L, 0, 6829, 6829, 1010);
		CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
		strcpy(gWhere, gTest);
	}
}

static void TestFourWayPctNegativeTakenAsZero()
{
	// iFourWayPct=-50 береться як 0: поріг 0, діагональ можлива. Точка (4200,4200)
	// від (4000,4000): dist=isqrt(2*200^2)=282, після одного біта T+35=449, q=1000: діагональ (96).
	// Поріг 100% (якби від'ємне стало 100) дав би 282*100 < 1000*100 і лише Right (32).
	TVpadLayout L;
	TVpadState S;
	OneDpad(L, 4000, 4000, 1000);
	L.iFourWayPct = -50;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 4000, 5000, 1000);
	VpadPointerMove(S, L, 0, 4200, 4200, 1010);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
}

static void TestHitPctAboveFourHundredAndNegativeTakenAsZero()
{
	// Кнопка Cross (0,0) R=100. iHitPct=401 береться як 400: зона натиску 400, точка (400,1) має
	// dist^2=160001 > 160000 і поза зоною; (400,0) всередині. Від'ємне 'iHitPct' дає зону 0.
	TVpadLayout L;
	TVpadState S;
	OneButton(L, 0, 0, 100, 1 << EVpadCross);
	L.iHitPct = 401;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 400, 1, 1000);
	CHECK_EQ(S.iP[0].iCtl, -1);
	CHECK_EQ(S.iBits, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 400, 0, 1000);
	CHECK_EQ(S.iP[0].iCtl, 0);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	L.iHitPct = -50;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 1, 0, 1000);
	CHECK_EQ(S.iP[0].iCtl, -1);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	CHECK_EQ(S.iP[0].iCtl, 0);
}

static void TestLeashPctAboveFourHundredTakenAsFourHundred()
{
	// Кнопка Cross (0,0) R=100, iLeashPct=401 береться як 400: повідець 400. Після натискання в (0,0)
	// рух у (400,1) (dist^2=160001 > 160000) виходить з повідця і біт знімається; у (400,0) лишається.
	TVpadLayout L;
	TVpadState S;
	OneButton(L, 0, 0, 100, 1 << EVpadCross);
	L.iLeashPct = 401;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	VpadPointerMove(S, L, 0, 400, 1, 1010);
	CHECK_EQ(S.iBits, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	VpadPointerMove(S, L, 0, 400, 0, 1010);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
}

static void TestLeashPctNegativeTakenAsZero()
{
	// iLeashPct=-50 береться як 0. Заголовок вимагає iLeashPct >= iHitPct,
	// тож iHitPct=0 і повідець 0 лишають умову. Натискання в (0,0) захоплює кнопку з зоною 0, рух у
	// (130,0) виходить з повідця і біт знімається. Якби від'ємне стало 400, точка лишилася б у повідці.
	TVpadLayout L;
	TVpadState S;
	OneButton(L, 0, 0, 100, 1 << EVpadCross);
	L.iHitPct = 0;
	L.iLeashPct = -50;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 0, 0, 1000);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
	VpadPointerMove(S, L, 0, 130, 0, 1010);
	CHECK_EQ(S.iBits, 0);
}

static void TestStickDigitalPctAboveFourHundredTakenAsFourHundred()
{
	// Стік (4000,4000) R=100, iStickDigitalPct=401 береться як 400: поріг dist*100 >= 40000, dist>=400.
	// Точка (4399,4000): dist=399, 39900 < 40000, біта немає; iLx = 127 + 399*127/399 = 254.
	// Точка (4400,4000): dist=400, 40000 >= 40000, біт Right (32). Без обрізання 40100 біта б не дав.
	TVpadLayout L;
	TVpadState S;
	OneStick(L, 4000, 4000, 100);
	L.iStickDigitalPct = 401;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 4000, 4000, 1000);
	CHECK_EQ(S.iBits, 0);
	VpadPointerMove(S, L, 0, 4399, 4000, 1010);
	CHECK_EQ(S.iLx, 254);
	CHECK_EQ(S.iBits, 0);
	VpadPointerMove(S, L, 0, 4400, 4000, 1020);
	CHECK_EQ(S.iLx, 254);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestStickDigitalPctNegativeTakenAsZeroDisables()
{
	// iStickDigitalPct=-80 береться як 0, цифрові біти вимкнено: точка (4400,4000) біта не дає.
	TVpadLayout L;
	TVpadState S;
	OneStick(L, 4000, 4000, 100);
	L.iStickDigitalPct = -80;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 4000, 4000, 1000);
	VpadPointerMove(S, L, 0, 4400, 4000, 1010);
	CHECK_EQ(S.iLx, 254);
	CHECK_EQ(S.iBits, 0);
}

static void TestStickDigitalPctOverflowingProductClamped()
{
	// Стік R=8000 у (4000,4000), iStickDigitalPct=300000 -> 400. Відхилення 1500,
	// dead=8000*18/100=1440, 1500 >= 1440, не в центрі. mag=1500-1440=60, span=6560, m=60*127/6560=1,
	// iLx=127+1500*1/1500=128, iLy=127. Цифрово: 150000 < 8000*400=3200000, біта немає.
	// Без обрізання 8000*300000 переповнює int і біт з'являється.
	TVpadLayout L;
	TVpadState S;
	OneStick(L, 4000, 4000, 8000);
	L.iStickDigitalPct = 300000;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 4000, 4000, 1000);
	VpadPointerMove(S, L, 0, 5500, 4000, 1010);
	CHECK_EQ(S.iLx, 128);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iBits, 0);
}

static void TestHitAndLeashPctOverflowingProductClamped()
{
	// Кнопка Cross R=8000 у (4000,4000), iHitPct=iLeashPct=300000 -> 400: зона 32000. Точка (8191,4000)
	// на відстані 4191 всередині, захоплення відбувається.
	TVpadLayout L;
	TVpadState S;
	OneButton(L, 4000, 4000, 8000, 1 << EVpadCross);
	L.iHitPct = 300000;
	L.iLeashPct = 300000;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 8191, 4000, 1000);
	CHECK_EQ(S.iP[0].iCtl, 0);
	CHECK_EQ(S.iBits, 1 << EVpadCross);
}

// --- Розмір елементів при масштабі --------------------------------------------

static void TestDefaultZeroSizeScalesDoNotFit9001()
{
	// U=9001, екран 640x360, s=100: px(t)=(t*100+4500)/9001, px>=1 лише при t*100>=4501.
	// rb=px(45)=9000/9001=0, rm=th=px(35)=8000/9001=0, sh=px(40)=8500/9001=0, g=px(10)=5500/9001=0;
	// tw=px(50)=1. Для всіх двадцяти масштабів s<=100 g=0 (g>=1 лише при s>=450), тож Select і Start
	// торкаються в W/2, а за правилом перетину торкання перетинає (обидві групи 0): розкладка не
	// вміщується. Очікування 0.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	CHECK_EQ(VpadDefaultLayout(L, 640, 360, 9001, 100, 0), 0);
}

static void TestDefaultZeroSizeScalesDoNotFit20000()
{
	// U=20000, s=100: px(t)=(t*100 + 10000)/20000, ≥1 при t*s>=10000. rb=px(45)=0, th=rm=px(35)=0,
	// sh=px(40)=0, g=px(10)=0. Розкладка не вміщується на жодному масштабі; очікування 0.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	CHECK_EQ(VpadDefaultLayout(L, 640, 360, 20000, 100, 0), 0);
}

static void TestDefaultReturnedScaleHasAllSizesPositiveSweep()
{
	// Властивість: якщо повернуто не нуль, кожен з дванадцяти елементів має розмір (iRx>0 для кола,
	// iRy>0 для прямокутника), розкладка вміщується, масштаб є одним зі s = P*(32-i)/32, i=0..19.
	static const int kU[6] = { 138, 300, 1000, 3000, 9001, 20000 };
	static const int kP[3] = { 25, 100, 400 };
	int u, p, sc, k, i;
	int zeros = 0, nonzeros = 0;
	for (u = 0; u < 6; ++u) {
		for (p = 0; p < 3; ++p) {
			for (sc = 0; sc < 2; ++sc) {
				const int W = sc == 0 ? 640 : 360;
				const int H = sc == 0 ? 360 : 640;
				TVpadLayout L;
				int r;
				int ok = 0;
				memset(&L, 0, sizeof L);
				r = VpadDefaultLayout(L, W, H, kU[u], kP[p], 0);
				sprintf(gWhere, "%s U=%d P=%d screen %d", gTest, kU[u], kP[p], sc);
				CHECK(r >= 0 && r <= kP[p]);
				for (i = 0; i < 20; ++i) {
					if (r == kP[p] * (32 - i) / 32) ok = 1;
				}
				CHECK(r == 0 || ok);
				if (r == 0) {
					// Нуль означає: aOut тримає найменшу спробу, і вона не вміщується. Вміщується — це
					// VpadLayoutFits і жодного нульового розміру разом; VpadLayoutFits нульові елементи
					// ігнорує, тож одного з цих умов для невдачі достатньо.
					int zeroSize = 0;
					++zeros;
					for (k = 0; k < EVpadDefaultCount; ++k) {
						if (L.iC[k].iShape == EVpadShapeRect ? L.iC[k].iRy <= 0 : L.iC[k].iRx <= 0) zeroSize = 1;
					}
					CHECK(zeroSize || !VpadLayoutFits(L, W, H));
				} else {
					++nonzeros;
				}
				if (r != 0) {
					for (k = 0; k < EVpadDefaultCount; ++k) {
						if (L.iC[k].iShape == EVpadShapeRect) CHECK(L.iC[k].iRy > 0);
						else CHECK(L.iC[k].iRx > 0);
					}
					CHECK(VpadLayoutFits(L, W, H));
				}
				strcpy(gWhere, gTest);
			}
		}
	}
	// Сітка має містити обидва результати: інакше гілка «нічого не вміщується» не перевіряється.
	CHECK(zeros > 0);
	CHECK(nonzeros > 0);
}

static void TestFitsIgnoresZeroSizeControls()
{
	// Заголовок: VpadLayoutFits сам по собі ігнорує елементи з
	// нульовим розміром. Коло iRx=0 і прямокутник iRy=0 поза екраном не заважають; коло R=10 вміщується.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	LayoutInit(L, 3);
	SetCircCtl(L.iC[0], -50, -50, 0, 0, 0);
	SetRectCtl(L.iC[1], -10, -10, 5, 0, 0);
	SetCircCtl(L.iC[2], 50, 50, 10, 0, 0);
	CHECK_EQ(VpadLayoutFits(L, 100, 100), 1);
}

// --- Друга умова r = iDualJumpPx/3, не менше 1 ------------------------------

static void TestDualRadiusNotLessThanOneAtJumpTwo()
{
	// iDualJumpPx=2: r=max(2/3, 1)=1. Хрестовина (100,100) R50, Cross (200,100) R40 (повідець 70).
	// Палець 0 у (130,100) (Down), рух на (132,100): dist 2 >= 2, час 10 мс, P2=(134,100), у повідці
	// Cross (dist 66 <= 70): інференція активна, iDual=1, гніздо 1 захопило Cross.
	// А: M=(131,100), dist до першого пальця 1 <= r=1: другий пішов, iDual=0, слот 1 звільнено.
	// Б: M=(132,100), dist до першого 2 > 1 і до P2old 2 > 1: слот 1 іде в (134,100), iDual=1.
	// За r=0 у сценарії А було б iDual=1, тобто помилка.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(L.iC[1], 200, 100, 40, 0, 1 << EVpadCross);
	L.iDualJumpPx = 2;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 130, 100, 1000);
	VpadPointerMove(S, L, 0, 132, 100, 1010);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iP[1].iX, 134);
	VpadPointerMove(S, L, 0, 131, 100, 1020);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iP[0].iX, 131);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 130, 100, 1000);
	VpadPointerMove(S, L, 0, 132, 100, 1010);
	VpadPointerMove(S, L, 0, 132, 100, 1020);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iX, 134);
	CHECK_EQ(S.iP[0].iX, 130);
}

static void TestDualRadiusIsThirdOfJumpAtNine()
{
	// iDualJumpPx=9: r=3. Старт на (139,100) (dist 9, час 10 мс): P2=(148,100), повідець Cross (dist 52).
	// M=(134,100): dist до першого 4 > 3, до P2old 14 > 3: слот 1 у (2*134-130, 100)=(138,100), iDual=1.
	// M=(133,100): dist до першого 3 <= 3: другий пішов, iDual=0. За r=2 лишилося б iDual=1 на 133,
	// за r=4 пішло б на 134.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(L.iC[1], 200, 100, 40, 0, 1 << EVpadCross);
	L.iDualJumpPx = 9;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 130, 100, 1000);
	VpadPointerMove(S, L, 0, 139, 100, 1010);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iX, 148);
	VpadPointerMove(S, L, 0, 134, 100, 1020);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iX, 138);
	CHECK_EQ(S.iP[0].iX, 130);
	VpadPointerMove(S, L, 0, 133, 100, 1030);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
}

// --- Гортання: KKsMaxOverPx ---------------------------

// Один жест із 'Down(1000), Move(900), Move(aTo), Up' при сталому часі.
static void GestureTo(TKScroll &aS, int aTo, unsigned aMs)
{
	KScrollDown(aS, 1000, aMs);
	KScrollMove(aS, 900, aMs);
	KScrollMove(aS, aTo, aMs);
	(void)KScrollUp(aS, aMs);
}

static void TestOverscrollSaturatesAtOverPxFromZero()
{
	// KScrollInit(100, 0, 165): max=0, зсув 0. Жест 1: raw=0+(900+8192)=9092 px, зсув 0+9092/2=4546 px.
	// Жест 2: якір raw=2*4546=9092, raw=9092+9092=18184, зсув 9092, обрізано до KKsMaxOverPx=8192.
	// Далі 8192 назавжди. Перший хибний жест записується як результат (CHECK_EQ firstBad).
	TKScroll S;
	int g;
	int firstBad = 0;
	KScrollInit(S, 100, 0, 165);
	for (g = 1; g <= 2000; ++g) {
		int want = (g == 1) ? 4546 * 256 : 8192 * 256;
		GestureTo(S, -8192, 1000);
		if (S.iPos8 != want && firstBad == 0) firstBad = g;
	}
	CHECK_EQ(firstBad, 0);
	CHECK_EQ(S.iPos8, 8192 * 256);
}

static void TestOverscrollSaturatesAtOverPxBelowZero()
{
	// Від'ємний бік з нуля, max=0. Жест 1: Move(1100) як якір, Move(8191): raw=0+(1100-8191)=-7091 px,
	// зсув -7091/2 = -3545.5 px = -907648 у 24.8. Жест 2: якір raw=-7091 px, raw=-14182, зсув -7091 px.
	// Жест 3: якір raw=-14182, raw=-21273, зсув -10636.5 px, обрізано до -8192 px = -2097152.
	TKScroll S;
	int g;
	int firstBad = 0;
	int want;
	KScrollInit(S, 100, 0, 165);
	for (g = 1; g <= 2000; ++g) {
		if (g == 1) want = -907648;
		else if (g == 2) want = -1815296;
		else want = -2097152;
		KScrollDown(S, 1000, 1000);
		KScrollMove(S, 1100, 1000);
		KScrollMove(S, 8191, 1000);
		(void)KScrollUp(S, 1000);
		if (S.iPos8 != want && firstBad == 0) firstBad = g;
	}
	CHECK_EQ(firstBad, 0);
	CHECK_EQ(S.iPos8, -2097152);
}

static void TestOverscrollSaturatesOnLargestListBothSides()
{
	// Вміст 4000000, вигляд 100: max=3999900. Знизу ті самі числа, що й для малого списку (від нуля).
	// Зверху від max: жест 1 дає max+4546 = 4004446 px, жест 2 обрізано до max+8192 = 4008092 px.
	TKScroll S;
	int g;
	int firstBad = 0;
	int want;
	KScrollInit(S, 100, 4000000, 165);
	for (g = 1; g <= 2000; ++g) {
		if (g == 1) want = -907648;
		else if (g == 2) want = -1815296;
		else want = -2097152;
		KScrollDown(S, 1000, 1000);
		KScrollMove(S, 1100, 1000);
		KScrollMove(S, 8191, 1000);
		(void)KScrollUp(S, 1000);
		if (S.iPos8 != want && firstBad == 0) firstBad = g;
	}
	CHECK_EQ(firstBad, 0);
	CHECK_EQ(S.iPos8, -2097152);
	KScrollInit(S, 100, 4000000, 165);
	KScrollTo(S, 3999900, 0, 1000);
	CHECK_EQ(S.iPos8, 3999900 * 256);
	firstBad = 0;
	for (g = 1; g <= 2000; ++g) {
		want = (g == 1) ? 4004446 * 256 : 4008092 * 256;
		GestureTo(S, -8192, 1000);
		if (S.iPos8 != want && firstBad == 0) firstBad = g;
	}
	CHECK_EQ(firstBad, 0);
	CHECK_EQ(S.iPos8, 4008092 * 256);
}

static void TestSetExtentDuringDragClampsToNewOverPxRange()
{
	// Max 900. Жест 1 (з Up): зсув 900 + (9092-900)/2 = 4996 px. Жест 2 без Up: якір raw=900+2*4096=9092,
	// raw=18184, зсув 900+8642=9542, обрізано до 900+8192=9092. SetExtent(вміст 0): max=0, у неідельному
	// стані зсув обрізається до -8192..8192, тобто 8192 px. Стан лишається EKsDragging. Потім Up дає
	// EKsSettling до 0, і доведення закінчується в 0 з EKsIdle.
	TKScroll S;
	KScrollInit(S, 100, 1000, 165);
	GestureTo(S, -8192, 1000);
	CHECK_EQ(S.iPos8, 4996 * 256);
	KScrollDown(S, 1000, 1000);
	KScrollMove(S, 900, 1000);
	KScrollMove(S, -8192, 1000);
	CHECK_EQ(S.iPos8, 9092 * 256);
	CHECK_EQ(S.iState, EKsDragging);
	KScrollSetExtent(S, 100, 0);
	CHECK_EQ(S.iState, EKsDragging);
	CHECK_EQ(S.iPos8, 8192 * 256);
	KScrollUp(S, 1000);
	CHECK_EQ(S.iState, EKsSettling);
	CHECK_EQ(S.iTarget8, 0);
	StepTo(S, 4000);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iPos8, 0);
}

static void TestRandomScrollOffsetStaysWithinOverPxBounds()
{
	// Псевдовипадкові жести, зсуви, нові розміри й цілі. Після кожної події зсув лежить у
	// [-8192*256, (max+8192)*256], з max=max(0, вміст-вигляд), як у заголовку.
	TRng R;
	TKScroll S;
	int i;
	int bad = 0;
	int maxPx;
	unsigned long t = 1000;
	R.iS = 555777u;
	KScrollInit(S, 200, 1000, 100);
	for (i = 0; i < 20000; ++i) {
		int op = RndRange(R, 0, 11);
		int c = RndRange(R, -8192, 8191);
		t += (unsigned)RndRange(R, 0, 20);
		if (op < 3) KScrollDown(S, c, (unsigned)t);
		else if (op < 7) KScrollMove(S, c, (unsigned)t);
		else if (op < 8) (void)KScrollUp(S, (unsigned)t);
		else if (op < 9) (void)KScrollStep(S, (unsigned)t);
		else if (op < 10) KScrollSetExtent(S, RndRange(R, 50, 400), RndRange(R, 0, 2000));
		else KScrollTo(S, RndRange(R, -100, 2500), RndRange(R, 0, 1), (unsigned)t);
		maxPx = S.iContent - S.iView;
		if (maxPx < 0) maxPx = 0;
		if (S.iPos8 < -8192 * 256 || S.iPos8 > (maxPx + 8192) * 256) ++bad;
	}
	CHECK_EQ(bad, 0);
}

// --- Налаштовувані поля ------------------------------------------------------

static void FlickNoInit(TKScroll &aS, unsigned aBase)
{
	KScrollDown(aS, 600, aBase);
	KScrollMove(aS, 579, aBase + 10);
	KScrollMove(aS, 560, aBase + 20);
	KScrollMove(aS, 540, aBase + 30);
	KScrollMove(aS, 520, aBase + 40);
}

// Два кидки з однаковими подіями і різними налаштовуваними полями; повертає число розбіжностей
// за кожну мілісекунду (позиція, швидкість, стан). Цикл має стелю 10000 мс: зупинка не досягнута
// дає aBothIdle=0, а не зависання.
static int CompareFlings(int aFricA, int aMinA, int aMaxA, int aFricB, int aMinB, int aMaxB,
	int *aBothIdle)
{
	TKScroll A, B;
	int bad = 0;
	unsigned t;
	KScrollInit(A, 300, 1000, 100);
	KScrollInit(B, 300, 1000, 100);
	A.iFrictionLossQ16 = aFricA;
	A.iMinFling = aMinA;
	A.iMaxFling = aMaxA;
	B.iFrictionLossQ16 = aFricB;
	B.iMinFling = aMinB;
	B.iMaxFling = aMaxB;
	FlickNoInit(A, 1000);
	(void)KScrollUp(A, 1040);
	FlickNoInit(B, 1000);
	(void)KScrollUp(B, 1040);
	for (t = 1041; t <= 11040 && (A.iState != EKsIdle || B.iState != EKsIdle); ++t) {
		KScrollStep(A, t);
		KScrollStep(B, t);
		if (A.iPos8 != B.iPos8 || A.iVel8 != B.iVel8 || A.iState != B.iState) ++bad;
	}
	*aBothIdle = (A.iState == EKsIdle && B.iState == EKsIdle);
	return bad;
}

static void TestMinFlingZeroTakenAsOneStillStops()
{
	// iMinFling=0 береться як 1. Поріг зупинки iMinFling*64 дає 64 для
	// обох, траєкторії мають збігатися. Без взяття в межі поріг 0 не досягався б ніколи (|vel| < 0
	// хибне), і цикл з 10000 мс дав би aBothIdle=0.
	int idle = 0;
	int bad = CompareFlings(201, 0, 8000, 201, 1, 8000, &idle);
	CHECK_EQ(bad, 0);
	CHECK_EQ(idle, 1);
}

static void TestFrictionOutOfRangeTakenWithinRange()
{
	// iFrictionLossQ16 береться в 1..32768: 0 і -5 як 1, 100000 як 32768. Траєкторії збігаються.
	// Якщо 0 лишиться 0, кидок гальмуватиметься лише мінімумом 1 px/s за мс і розійдеться з 1.
	int idle = 0;
	int bad = CompareFlings(0, 300, 8000, 1, 300, 8000, &idle);
	CHECK_EQ(bad, 0);
	CHECK_EQ(idle, 1);
	bad = CompareFlings(-5, 300, 8000, 1, 300, 8000, &idle);
	CHECK_EQ(bad, 0);
	CHECK_EQ(idle, 1);
	bad = CompareFlings(100000, 300, 8000, 32768, 300, 8000, &idle);
	CHECK_EQ(bad, 0);
	CHECK_EQ(idle, 1);
}

static void TestMinFlingAboveFourMillionTakenAsFourMillion()
{
	// iMinFling=2000000000 береться як 4000000. Відпускання з v=4100000 (4100 px за 1 мс,
	// clamp iMaxFling=4000000) має |v|>=4000000, тож починається кидок з iVel8=4000000*256.
	// Без взяття в межі поріг 2e9 відкинув би кидок.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	S.iMaxFling = 4000000;
	S.iMinFling = 2000000000;
	KScrollDown(S, 4100, 1000);
	KScrollMove(S, 0, 1001);
	(void)KScrollUp(S, 1001);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, 4000000 * 256);
}

static void TestMaxFlingNegativeTakenAsZeroReleaseStops()
{
	// iMaxFling=-5 береться як 0: відпускання з v=2000 обрізається до 0, |0| < iMinFling=1,
	// тож Idle і без кидка. Без взяття в межі обрізання дало б |v|=5 >= 1 і кидок.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	S.iMinFling = 1;
	S.iMaxFling = -5;
	FlickNoInit(S, 1000);
	CHECK_EQ(KScrollUp(S, 1040), EKsNone);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iVel8, 0);
}

static void TestMaxFlingAboveFourMillionTakenAsFourMillion()
{
	// iMaxFling=1000000000 береться як 4000000: відпускання з v=4100000 дає iVel8=4000000*256, а не
	// 4100000*256 (1049600000), що було б без взяття в межі.
	TKScroll S;
	KScrollInit(S, 300, 1000, 100);
	S.iMaxFling = 1000000000;
	KScrollDown(S, 4100, 1000);
	KScrollMove(S, 0, 1001);
	(void)KScrollUp(S, 1001);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, 4000000 * 256);
}

// --- KScrollEnsureVisible: обидва кінці обрізаються на вході ------------------

static void TestEnsureVisibleNegativeTopClampedOnEntry()
{
	// Вигляд 100, вміст 1000, зсув 200. aTop=-50 обрізається до 0, зсув іде в 0. Цей результат однаковий
	// з обрізанням і без нього (KScrollTo все одно обрізає до [0,max]); тест лише фіксує поведінку.
	// Розрізняє обрізання входу лише переповнення в TestEnsureVisibleBothExtremesClampedOnEntry.
	TKScroll S;
	KScrollInit(S, 100, 1000, 100);
	KScrollTo(S, 200, 0, 1000);
	CHECK_EQ(S.iPos8, 200 * 256);
	KScrollEnsureVisible(S, -50, 50, 0, 1000);
	CHECK_EQ(KScrollPos(S), 0);
}

static void TestEnsureVisibleBottomNearIntMaxClampedOnEntry()
{
	// Вміст 4000000, вигляд 100, зсув 0. aTop=3000000, aBottom=INT_MAX обрізається до 4000000: проміжок
	// 1000000 > 100, тож зсув іде в aTop = 3000000.
	TKScroll S;
	KScrollInit(S, 100, 4000000, 100);
	KScrollEnsureVisible(S, 3000000, 2147483647, 0, 1000);
	CHECK_EQ(KScrollPos(S), 3000000);
}

static void TestEnsureVisibleBothExtremesClampedOnEntry()
{
	// aTop=-INT_MAX і aBottom=INT_MAX обрізаються до 0 і 4000000: проміжок 4000000 > 100, зсув у aTop=0.
	// Без обрізання проміжок переповнює int, гілка стає хибною, і зсув іде до max=3999900.
	TKScroll S;
	KScrollInit(S, 100, 4000000, 100);
	KScrollEnsureVisible(S, -2147483647, 2147483647, 0, 1000);
	CHECK_EQ(KScrollPos(S), 0);
	KScrollEnsureVisible(S, -2000000000, 2000000000, 0, 1000);
	CHECK_EQ(KScrollPos(S), 0);
}

// --- iStickFloat = 1 -----------------------------------------------

static void TestStickFloatOriginAtTouchPoint()
{
	// Стік (200,200) R=100, iStickFloat=1: точка відліку — точка дотику (230,200). Дотик у центрі.
	// Точка (280,200): dx=50, mag=50-18=32, m=32*127/82=49, iLx=127+50*49/50=176, біта немає (5000<8000).
	// Точка (320,200): dx=90, mag=72, m=111, iLx=127+90*111/90=238, Right (9000>=8000).
	// Для порівняння: iStickFloat=0 з відліком у центрі дає iLx=145 у точці (230,200).
	TVpadLayout L;
	TVpadState S;
	OneStick(L, 200, 200, 100);
	L.iStickFloat = 1;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 230, 200, 1000);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iBits, 0);
	VpadPointerMove(S, L, 0, 280, 200, 1010);
	CHECK_EQ(S.iLx, 176);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iBits, 0);
	VpadPointerMove(S, L, 0, 320, 200, 1020);
	CHECK_EQ(S.iLx, 238);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	OneStick(L, 200, 200, 100);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 230, 200, 1000);
	CHECK_EQ(S.iLx, 145);
}

static void TestStickFloatSecondFingerCentredAtLanding()
{
	// Хрестовина (100,100) R50 і стік (300,100) R80 з iStickFloat=1, як у TestDualStickCandidate...
	// Палець 0 у (140,100), рух до (240,100): P2=(340,100) у повідці стіка (dist 40 <= 140). Стік
	// захоплений у P2 з відліком у P2, тож центр: iLx=iLy=127, біта немає. Хрестовина дає Right (32).
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(L.iC[1], 300, 100, 80, 0, 0);
	L.iC[1].iKind = (unsigned char)EVpadKindStick;
	L.iDualJumpPx = 60;
	L.iStickFloat = 1;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	VpadPointerMove(S, L, 0, 240, 100, 1060);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

// --- Чотири вказівники без виведення другого пальця -----------------------------------------

static void TestFourPointersOnDistinctControlsAllPress()
{
	// Хрестовина (100,100) R50, Cross (250,100), Circle (350,100), Square (450,100) R30 кожна.
	// Вказівники 0..3 на хрестовині (130,100) і трьох кнопках. Біти: Right(32)+Cross(16384)+
	// Circle(8192)+Square(32768)=57376. Фронти в iEdges ті самі (57376), після VpadTakeEdges — 0.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 4);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(L.iC[1], 250, 100, 30, 0, 1 << EVpadCross);
	SetCircCtl(L.iC[2], 350, 100, 30, 0, 1 << EVpadCircle);
	SetCircCtl(L.iC[3], 450, 100, 30, 0, 1 << EVpadSquare);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 130, 100, 1000);
	VpadPointerDown(S, L, 1, 250, 100, 1000);
	VpadPointerDown(S, L, 2, 350, 100, 1000);
	VpadPointerDown(S, L, 3, 450, 100, 1000);
	CHECK_EQ(S.iP[0].iCtl, 0);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iP[2].iCtl, 2);
	CHECK_EQ(S.iP[3].iCtl, 3);
	CHECK_EQ(S.iBits, 57376);
	CHECK_EQ(VpadTakeEdges(S), 57376);
	CHECK_EQ(VpadTakeEdges(S), 0);
}

static void TestSecondPointerOnCapturedDpadStaysInert()
{
	// Вказівник 0 захопив хрестовину (130,100). Вказівник 1 у (100,140), dist 40 в зоні натиску 62,
	// але хрестовина зайнята іншим вказівником, тож пошук її пропускає: вказівник 1 нічого не захоплює.
	// Вказівники 2 і 3 на Cross і Circle. Біти: 32+16384+8192=24608.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 4);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetCircCtl(L.iC[1], 250, 100, 30, 0, 1 << EVpadCross);
	SetCircCtl(L.iC[2], 350, 100, 30, 0, 1 << EVpadCircle);
	SetCircCtl(L.iC[3], 450, 100, 30, 0, 1 << EVpadSquare);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 130, 100, 1000);
	VpadPointerDown(S, L, 1, 100, 140, 1000);
	CHECK_EQ(S.iP[1].iCtl, -1);
	CHECK_EQ(S.iP[1].iBits, 0);
	VpadPointerDown(S, L, 2, 250, 100, 1000);
	VpadPointerDown(S, L, 3, 350, 100, 1000);
	CHECK_EQ(S.iBits, 24608);
}

static void TestEdgesCountSharedBitOnlyOnRisingWord()
{
	// Кнопки A (250,100) і B (500,100) обидві Cross, група 0. Фронт Cross рахується по загальному слову:
	// другий вказівник на B при вже піднятому біті фронту не дає; відпускання одного з двох біт не знімає.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 250, 100, 30, 0, 1 << EVpadCross);
	SetCircCtl(L.iC[1], 500, 100, 30, 0, 1 << EVpadCross);
	VpadReset(S);
	VpadPointerDown(S, L, 1, 250, 100, 1000);
	VpadPointerDown(S, L, 2, 500, 100, 1000);
	CHECK_EQ(VpadTakeEdges(S), 16384);
	CHECK_EQ(VpadTakeEdges(S), 0);
	VpadPointerUp(S, L, 1, 1010);
	CHECK_EQ(S.iBits, 16384);
	CHECK_EQ(VpadTakeEdges(S), 0);
	VpadPointerDown(S, L, 1, 250, 100, 1020);
	CHECK_EQ(VpadTakeEdges(S), 0);
	VpadPointerUp(S, L, 2, 1030);
	CHECK_EQ(S.iBits, 16384);
	VpadPointerUp(S, L, 1, 1040);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(VpadTakeEdges(S), 0);
	VpadPointerDown(S, L, 1, 250, 100, 1050);
	CHECK_EQ(VpadTakeEdges(S), 16384);
}

// ---------------------------------------------------------------- час, що йде назад, і межі

static void TestEarlierTimeDuringPressIsNoElapsedTime()
{
	// Натискання о 1000 мс. Час 999 раніший за натискання: 0 мс минуло, довге натискання не
	// спрацьовує. 1499 дає 499 мс (ні), 1500 дає 500 мс (так).
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 500, 1000);
	CHECK_EQ(KScrollStep(S, 999), EKsNone);
	CHECK_EQ(S.iState, EKsPressed);
	CHECK_EQ(KScrollStep(S, 1499), EKsNone);
	CHECK_EQ(KScrollStep(S, 1500), EKsLongPress);
}

static void TestStepEarlierThanLastStepKeepsStepTimeAndState()
{
	// Відпускання о 1040 мс (після ScrollFlickUp з 1000): pos 59 px = 15104, v=2000 px/s = 512000.
	// Крок о 1039 раніший за 1040: 0 мс, список не рухається, iStepMs лишається 1040.
	// Крок о 1041: рівно одна мілісекунда від 1040: pos += 512000/1000 = 512 -> 15616;
	// vel -= (512000*201)>>16 = 1570 -> 510430.
	TKScroll S;
	TimeSliceFling(S, 1000u);
	CHECK_EQ(S.iPos8, 15104);
	CHECK_EQ(S.iVel8, 512000);
	CHECK_EQ(S.iStepMs, 1040);
	CHECK_EQ(KScrollStep(S, 1039), EKsNone);
	CHECK_EQ(S.iPos8, 15104);
	CHECK_EQ(S.iVel8, 512000);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iStepMs, 1040);
	CHECK_EQ(KScrollStep(S, 1041), EKsNone);
	CHECK_EQ(S.iPos8, 15616);
	CHECK_EQ(S.iVel8, 510430);
	CHECK_EQ(S.iStepMs, 1041);
}

static void TestUpEarlierThanDownIsStillTap()
{
	// Відпускання о 990 мс при натисканні о 1000: 0 мс минуло, 0 < 500, дотик. Те саме при 999.
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 500, 1000);
	CHECK_EQ(KScrollUp(S, 990), EKsTap);
	CHECK_EQ(S.iState, EKsIdle);
	KScrollDown(S, 500, 1000);
	CHECK_EQ(KScrollUp(S, 999), EKsTap);
	CHECK_EQ(S.iState, EKsIdle);
}

static void TestReleaseEarlierThanNewestSampleStillFlings()
{
	// Остання подія о 1040 мс, відпускання о 1035: 0 мс минуло, палець не спочивав (не більше 100 мс).
	// v = (600-520)*1000/(1040-1000) = 2000 px/s, як при відпусканні о 1040 (TestStepEarlierThanLastStep...).
	// iStepMs = 1035, крок о 1036 інтегрує одну мілісекунду: 15104 + 512 = 15616.
	TKScroll S;
	ScrollFlickUp(S, 1000u);
	CHECK_EQ(KScrollUp(S, 1035), EKsNone);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, 512000);
	CHECK_EQ(S.iStepMs, 1035);
	KScrollStep(S, 1036);
	CHECK_EQ(S.iPos8, 15616);
	CHECK_EQ(S.iVel8, 510430);
}

static void TestSecondFingerInferenceEarlierThanLastEventIsInstant()
{
	// Палець 0 о 1000 мс на (140,100); рух на (240,100) о 990 мс: 0 мс від останньої події, 0 <= 60,
	// тож інференція вмикається так само, як за 60 мс (DualStart).
	TVpadLayout L;
	TVpadState S;
	LayoutDual(L, 60);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 140, 100, 1000);
	VpadPointerMove(S, L, 0, 240, 100, 990);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iDualX, 140);
	CHECK_EQ(S.iDualY, 100);
	CHECK_EQ(S.iP[1].iDown, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadCross));
}

static void TestWrapAroundTapBoundaryAt500Across0xFFFFFFFF()
{
	// Натискання о 0xFFFFFF00 (це -256 за модулем 2^32). Відпускання о 0xF3: 0xF3 + 0x100 = 499 мс,
	// дотик. О 0xF4: 500 мс, не дотик.
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 500, 0xFFFFFF00u);
	CHECK_EQ(KScrollUp(S, 0x000000F3u), EKsTap);
	KScrollDown(S, 500, 0xFFFFFF00u);
	CHECK_EQ(KScrollUp(S, 0x000000F4u), EKsNone);
	CHECK_EQ(S.iState, EKsIdle);
}

// Кидок довгим списком із iMinFling=1: від 500000 px, відпускання о 1030 мс, кроки по aStep мс до EKsIdle.
static int FlingLongList(TKScroll &aS, unsigned aStep, unsigned &aEndMs)
{
	int steps = 0;
	unsigned t = 1030u;
	KScrollInit(aS, 300, 1000000, 1000);
	aS.iMinFling = 1;
	KScrollTo(aS, 500000, 0, 0);
	KScrollDown(aS, 500, 1000u);
	KScrollMove(aS, 495, 1010u);
	KScrollMove(aS, 490, 1020u);
	(void)KScrollUp(aS, 1030u);
	while (aS.iState != EKsIdle && steps < 10000) {
		t += aStep;
		KScrollStep(aS, t);
		++steps;
	}
	aEndMs = t;
	return steps;
}

static void TestFlingWithMinFlingOneReachesIdleOnLongList()
{
	// Швидкість після відпускання: (500-490)*1000/20 = 500 px/s = 128000 одиниць 24.8. Тертя 201/65536 на мс
	// тримає експоненту, доки |v|*201 >= 65536, тобто до 326 одиниць: близько 1950 мс; далі −1 одиниця на мс
	// до порога 64. Очікування зупинки близько 2300 мс; межа 4000 мс. Шлях близько 163 px, тож позиція
	// лишається в (500005, 500005+400] px. Крок 16 мс і крок 1 мс дають однакову позицію.
	TKScroll A, B;
	unsigned endA = 0, endB = 0;
	int stepsA, stepsB;
	stepsA = FlingLongList(A, 16u, endA);
	stepsB = FlingLongList(B, 1u, endB);
	CHECK_EQ(A.iState, EKsIdle);
	CHECK_EQ(B.iState, EKsIdle);
	CHECK(stepsA < 10000);
	CHECK(stepsB < 10000);
	CHECK(endA - 1030u <= 4000u);
	CHECK_EQ(A.iVel8, 0);
	CHECK_EQ(A.iPos8 % 256, 0);
	CHECK(A.iPos8 > 128001280);
	CHECK(A.iPos8 < 128001280 + 400 * 256);
	CHECK_EQ(A.iPos8, B.iPos8);
}

static void TestFirstFingerLeftTakesOverStickOriginFromSecondFinger()
{
	// Палець 0 тримає кнопку Triangle (300,100) R40 (зона натиску 50). Інференція кладе палець 1 на стік
	// (100,100) R50 з iStickFloat=1 у P2=(110,100); r = 40/3 = 13. M=(205,100): P2=(2*205-300,100)=(110,100),
	// інференція вмикається. M=(123,100): дистанція до P2 13 <= 13, до першого пальця 177 > 13, тож перший
	// палець пішов. Палець 0 перейняв стік з початком P2: dx=13, dy=0, dist=13, dead=50*18/100=9,
	// mag=13-9=4, span=41, m=4*127/41=12, iLx=127+13*12/13=139, iLy=127. Від центру (100,100) було б dx=23, iLx=170.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 300, 100, 40, 0, 1 << EVpadTriangle);
	SetCircCtl(L.iC[1], 100, 100, 50, 0, 0);
	L.iC[1].iKind = (unsigned char)EVpadKindStick;
	L.iStickFloat = 1;
	L.iDualJumpPx = 40;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 300, 100, 1000);
	VpadPointerMove(S, L, 0, 205, 100, 1010);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iDown, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	VpadPointerMove(S, L, 0, 123, 100, 1020);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iP[0].iCtl, 1);
	CHECK_EQ(S.iLx, 139);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iBits, 0);
}

static void TestDpadDiagonalAtThresholdAndOneBelowInEachHysteresis()
{
	// iDiagPct=100 дає T = 414 (індекс 10). Без попереднього натискання: q=413 один біт, q=414 діагональ.
	// Після одного біта T+35 = 449: q=448 один біт, q=449 діагональ. Після діагоналі T-35 = 379: q=379
	// діагональ, q=378 один біт. Точка (100+1000, 100+q): hi=1000, lo=q, q=lo*1000/1000=q.
	TVpadLayout L;
	TVpadState S;
	LayoutDpad(L, 100, 18);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 2000);
	DpadAt(S, L, 1000, 413);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 2000);
	DpadAt(S, L, 1000, 414);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 2000);
	DpadAt(S, L, 1000, 0);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	DpadAt(S, L, 1000, 448);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	DpadAt(S, L, 1000, 449);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 2000);
	DpadAt(S, L, 1000, 414);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
	DpadAt(S, L, 1000, 379);
	CHECK_EQ(S.iBits, (1 << EVpadRight) | (1 << EVpadDown));
	DpadAt(S, L, 1000, 378);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
}

static void TestVelocityWindowIncludesSampleExactlyHundredMsOld()
{
	// Вибірки 600@1000, 575@1010, 570@1050, 540@1100. Найстаріша в вікні (<= 100 мс від 1100) — 600@1000:
	// v = (600-540)*1000/100 = 600 px/s = 153600. Якби межу взяли строго, опорною стала б 575@1010
	// (90 мс): v = 35000/90 = 388.
	TKScroll S;
	ScrollInit100(S);
	KScrollDown(S, 600, 1000);
	KScrollMove(S, 575, 1010);
	KScrollMove(S, 570, 1050);
	KScrollMove(S, 540, 1100);
	CHECK_EQ(KScrollUp(S, 1100), EKsNone);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, 600 * 256);
}

static void TestSettleStopsBelow128AndStepsFromOneTwentyEight()
{
	// Зазор d = iTarget8 - iPos8 у 24.8. |d| = 127 < 128: позиція стає на ціль, стан Idle.
	// |d| = 128: крок 128 >> 6 = 2 одиниці, стан лишається EKsSettling. Від'ємні — так само.
	TKScroll S;
	ScrollInit100(S);
	S.iState = (unsigned char)EKsSettling;
	S.iPos8 = 0;
	S.iTarget8 = 127;
	S.iStepMs = 1000;
	KScrollStep(S, 1001);
	CHECK_EQ(S.iPos8, 127);
	CHECK_EQ(S.iState, EKsIdle);
	CHECK_EQ(S.iVel8, 0);
	ScrollInit100(S);
	S.iState = (unsigned char)EKsSettling;
	S.iPos8 = 0;
	S.iTarget8 = 128;
	S.iStepMs = 1000;
	KScrollStep(S, 1001);
	CHECK_EQ(S.iPos8, 2);
	CHECK_EQ(S.iState, EKsSettling);
	ScrollInit100(S);
	S.iState = (unsigned char)EKsSettling;
	S.iPos8 = 0;
	S.iTarget8 = -127;
	S.iStepMs = 1000;
	KScrollStep(S, 1001);
	CHECK_EQ(S.iPos8, -127);
	CHECK_EQ(S.iState, EKsIdle);
	ScrollInit100(S);
	S.iState = (unsigned char)EKsSettling;
	S.iPos8 = 0;
	S.iTarget8 = -128;
	S.iStepMs = 1000;
	KScrollStep(S, 1001);
	CHECK_EQ(S.iPos8, -2);
	CHECK_EQ(S.iState, EKsSettling);
}

static void TestEnsureVisibleBottomAtViewEndHasNoEffectDuringFling()
{
	// Після ковзання база = KScrollPos = (15104+128)>>8 = 59. aTop = 59 не менше бази. aBottom = 59+300 =
	// база+iView: за правилом «aBottom > база+iView» жодного ефекту, ковзання не зупиняється. Контроль з
	// aBottom = 360 має запустити рух (EKsSettling). Порожній список у стані Idle теж не рухається.
	TKScroll S, C, I;
	TimeSliceFling(S, 1000u);
	CHECK_EQ(S.iState, EKsFlinging);
	C = S;
	KScrollEnsureVisible(S, 59, 359, 1, 1040);
	CHECK_EQ(S.iState, EKsFlinging);
	CHECK_EQ(S.iVel8, 512000);
	CHECK_EQ(S.iPos8, 15104);
	KScrollEnsureVisible(C, 59, 360, 1, 1040);
	CHECK_EQ(C.iState, EKsSettling);
	ScrollInit100(I);
	KScrollEnsureVisible(I, 0, 300, 1, 1000);
	CHECK_EQ(I.iState, EKsIdle);
	CHECK_EQ(I.iPos8, 0);
}

static void TestDefaultNothingFitsEndsWithNineteenthTry()
{
	// Поле 10x10 не вміщує жодного масштабу, тож повертається 0, а aOut тримає найменшу спробу, i=19.
	// Rd = px(110) = (110*s + u/2)/u при u=20. aScalePct=32: i=19 дає s=13, (1430+10)/20 = 72. Якщо б спробу
	// i=19 пропустили, s=14 дало б 77; якщо б взяли i=20, s=12 дало б 66. aScalePct=25: i=19 дає s=10,
	// (1100+10)/20 = 55; i=20 дало б s=9, (990+10)/20 = 50.
	TVpadLayout L;
	int r;
	memset(&L, 0, sizeof L);
	r = VpadDefaultLayout(L, 10, 10, 20, 32, 0);
	CHECK_EQ(r, 0);
	CHECK_EQ(L.iC[EVpadIxDir].iRx, 72);
	memset(&L, 0, sizeof L);
	r = VpadDefaultLayout(L, 10, 10, 20, 25, 0);
	CHECK_EQ(r, 0);
	CHECK_EQ(L.iC[EVpadIxDir].iRx, 55);
}

static void TestActionIdThirtyOneReportsBitThirtyTwoReportsNothing()
{
	// iId 31 дає біт 0x80000000. iId 32 не звітує ні про що: біт поза 32 не існує.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 100, 100, 40, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindAction;
	L.iC[0].iId = 31;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	VpadPointerUp(S, L, 0, 1010);
	CHECK(VpadTakeActions(S) == 0x80000000u);
	L.iC[0].iId = 32;
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 1000);
	VpadPointerUp(S, L, 0, 1010);
	CHECK_EQ(VpadTakeActions(S), 0);
	CHECK_EQ(S.iBits, 0);
}

// ---------------------------------------------------------------- друга редакція: стіки, клацання, розкладка
//
// Нові правила заголовка: лівий і правий стік (iId), клацання L3/R3 (Stick click), розкладка з
// EVpadOptSticks, редагування розкладки (VpadControlAt, VpadMoveControl, VpadResizeControl).
// Очікування — за формулами заголовка, розрахунок у коментарях. Розділ «Еталонна модель» рахує
// розкладку і вміщення окремо від реалізації, за текстом правил; з ним звіряються значення на
// кількох екранах і кроках пікселя.

static void SetStickCtl(TVpadControl &aC, int aCx, int aCy, int aR, int aGroup, int aId)
{
	memset(&aC, 0, sizeof aC);
	aC.iKind = (unsigned char)EVpadKindStick;
	aC.iShape = (unsigned char)EVpadShapeCircle;
	aC.iGroup = (unsigned char)aGroup;
	aC.iId = (unsigned char)aId;
	aC.iCx = (short)aCx;
	aC.iCy = (short)aCy;
	aC.iRx = (short)aR;
}

// Лівий стік (150,100) і правий (450,100), обидва R=80. Зони натиску R*125/100 = 100 не
// перетинаються: x 50..250 і 350..550.
static void LayoutTwoSticks(TVpadLayout &aL, int aClickMs)
{
	LayoutInit(aL, 2);
	SetStickCtl(aL.iC[0], 150, 100, 80, 0, 0);
	SetStickCtl(aL.iC[1], 450, 100, 80, 0, 1);
	aL.iStickClickMs = aClickMs;
}

// Лівий стік (100,100) R=80 з LayoutStick: мертва зона 14, span 66, цифровий поріг 64.
static void LayoutClick(TVpadLayout &aL, int aClickMs)
{
	LayoutStick(aL, 0);
	aL.iStickClickMs = aClickMs;
}

// Кнопка Triangle (300,100) R40 (ctl 0, зона 50) і лівий стік (100,100) R50 (ctl 1, зона 62,
// повідець 50*175/100 = 87). Другий палець: r = 40/3 = 13.
static void LayoutInferStick(TVpadLayout &aL)
{
	LayoutInit(aL, 2);
	SetCircCtl(aL.iC[0], 300, 100, 40, 0, 1 << EVpadTriangle);
	SetCircCtl(aL.iC[1], 100, 100, 50, 0, 0);
	aL.iC[1].iKind = (unsigned char)EVpadKindStick;
	aL.iDualJumpPx = 40;
	aL.iStickClickMs = 300;
}

// Два стіки R50: лівий (100,100) ctl 0, правий (300,100) ctl 1. Повідець правого 87.
static void LayoutInferTwoSticks(TVpadLayout &aL)
{
	LayoutInit(aL, 2);
	SetStickCtl(aL.iC[0], 100, 100, 50, 0, 0);
	SetStickCtl(aL.iC[1], 300, 100, 50, 0, 1);
	aL.iDualJumpPx = 40;
	aL.iStickClickMs = 300;
}

// Один палець: натискання в aDown, підняття в aUp.
static void TapStick(TVpadState &aS, const TVpadLayout &aL, int aId, int aX, int aY, unsigned aDown, unsigned aUp)
{
	VpadPointerDown(aS, aL, aId, aX, aY, aDown);
	VpadPointerUp(aS, aL, aId, aUp);
}

static int SameCtl(const TVpadControl &aA, const TVpadControl &aB)
{
	return aA.iKind == aB.iKind && aA.iShape == aB.iShape && aA.iGroup == aB.iGroup && aA.iId == aB.iId
		&& aA.iBits == aB.iBits && aA.iCx == aB.iCx && aA.iCy == aB.iCy && aA.iRx == aB.iRx && aA.iRy == aB.iRy;
}

// Порівняння елементів поле за полем (без байтів вирівнювання, які копія не гарантує).
static int SameAllCtl(const TVpadLayout &aA, const TVpadLayout &aB)
{
	int i;
	if (aA.iCount != aB.iCount) return 0;
	for (i = 0; i < TakenCount(aA); ++i) {
		if (!SameCtl(aA.iC[i], aB.iC[i])) return 0;
	}
	return 1;
}

static void Default14(TVpadLayout &aL)
{
	memset(&aL, 0, sizeof aL);
	VpadDefaultLayout(aL, 640, 360, 138, 100, EVpadOptSticks);
}

// ---------------------------------------------------------------- еталонна модель розкладки

// Написано за текстом правил заголовка, окремо від реалізації. Не викликає жодної функції Vpad*.

static int RefPx(int aT, int aS, int aU)
{
	return (aT * aS + aU / 2) / aU;
}

static void RefCirc(TVpadControl &aC, int aKind, int aCx, int aCy, int aR, int aGroup, unsigned aBits, int aId)
{
	memset(&aC, 0, sizeof aC);
	aC.iKind = (unsigned char)aKind;
	aC.iShape = (unsigned char)EVpadShapeCircle;
	aC.iGroup = (unsigned char)aGroup;
	aC.iBits = (unsigned short)aBits;
	aC.iId = (unsigned char)aId;
	aC.iCx = (short)aCx;
	aC.iCy = (short)aCy;
	aC.iRx = (short)aR;
}

static void RefRect(TVpadControl &aC, int aCx, int aCy, int aHw, int aHh, int aGroup, unsigned aBits)
{
	RefCirc(aC, EVpadKindButton, aCx, aCy, aHw, aGroup, aBits, 0);
	aC.iShape = (unsigned char)EVpadShapeRect;
	aC.iRy = (short)aHh;
}

// Чотири кнопки лиця навколо F (aFx, aFy) з відступом aRr, радіус aRb, група 2.
static void RefFace(TVpadLayout &aL, int aFx, int aFy, int aRr, int aRb)
{
	RefCirc(aL.iC[EVpadIxTriangle], EVpadKindButton, aFx, aFy - aRr, aRb, 2, 1u << EVpadTriangle, 0);
	RefCirc(aL.iC[EVpadIxCircle], EVpadKindButton, aFx + aRr, aFy, aRb, 2, 1u << EVpadCircle, 0);
	RefCirc(aL.iC[EVpadIxCross], EVpadKindButton, aFx, aFy + aRr, aRb, 2, 1u << EVpadCross, 0);
	RefCirc(aL.iC[EVpadIxSquare], EVpadKindButton, aFx - aRr, aFy, aRb, 2, 1u << EVpadSquare, 0);
}

// Розкладка за розділами «Sizes», «Landscape» і «Portrait». Ландшафт: aW >= aH. Розкладка з
// EVpadOptSticks тільки на ландшафті; елемент 0 при цьому — d-pad (обрано конкретніше правило).
static void RefBuild(TVpadLayout &aL, int aW, int aH, int aU, int aS, unsigned aOpt)
{
	const int land = aW >= aH;
	const int sticks = land && (aOpt & EVpadOptSticks) != 0;
	const int m = RefPx(20, aS, aU);
	const int g = RefPx(10, aS, aU);
	const int rd = RefPx(110, aS, aU);
	const int rb = RefPx(45, aS, aU);
	const int rr = RefPx(75, aS, aU);
	const int tw = RefPx(50, aS, aU);
	const int th = RefPx(35, aS, aU);
	const int rm = RefPx(35, aS, aU);
	const int sw = land ? RefPx(70, aS, aU) : RefPx(45, aS, aU);
	const int sh = RefPx(40, aS, aU);
	const int dirKind = (!sticks && (aOpt & EVpadOptStick)) ? EVpadKindStick : EVpadKindDpad;
	int fx = 0;
	int fy = 0;
	memset(&aL, 0, sizeof aL);
	aL.iCount = sticks ? EVpadSticksCount : EVpadDefaultCount;
	aL.iHitPct = 125;
	aL.iLeashPct = 175;
	aL.iDeadPct = 18;
	aL.iDiagPct = 100;
	aL.iFourWayPct = 40;
	aL.iStickDigitalPct = 80;
	aL.iStickFloat = 0;
	aL.iStickClickMs = 300;
	if (land) {
		fx = aW - m - rr - rb;
		fy = aH - m - rr - rb;
		RefCirc(aL.iC[EVpadIxDir], dirKind, m + rd, aH - m - rd, rd, 1, 0, 0);
		RefRect(aL.iC[EVpadIxL1], m + sw, m + sh, sw, sh, 3, 1u << EVpadL1);
		RefRect(aL.iC[EVpadIxL2], m + 3 * sw + g, m + sh, sw, sh, 3, 1u << EVpadL2);
		RefRect(aL.iC[EVpadIxR1], aW - m - sw, m + sh, sw, sh, 4, 1u << EVpadR1);
		RefRect(aL.iC[EVpadIxR2], aW - m - 3 * sw - g, m + sh, sw, sh, 4, 1u << EVpadR2);
		if (sticks) {
			const int tw2 = RefPx(43, aS, aU);
			const int th2 = RefPx(27, aS, aU);
			const int rs = RefPx(70, aS, aU);
			RefRect(aL.iC[EVpadIxSelect], aW / 2 - rm - g - tw2, m + rm, tw2, th2, 0, 1u << EVpadSelect);
			RefRect(aL.iC[EVpadIxStart], aW / 2 + rm + g + tw2, m + rm, tw2, th2, 0, 1u << EVpadStart);
			RefCirc(aL.iC[EVpadIxMenu], EVpadKindAction, aW / 2, m + rm, rm, 0, 0, 0);
			RefCirc(aL.iC[EVpadIxLStick], EVpadKindStick, aW / 2 - g - rs, aH - m - rs, rs, 0, 0, 0);
			RefCirc(aL.iC[EVpadIxRStick], EVpadKindStick, aW / 2 + g + rs, aH - m - rs, rs, 0, 0, 1);
		} else {
			RefRect(aL.iC[EVpadIxSelect], aW / 2 - (g + 1) / 2 - tw, aH - m - th, tw, th, 0, 1u << EVpadSelect);
			RefRect(aL.iC[EVpadIxStart], aW / 2 + (g + 1) / 2 + tw, aH - m - th, tw, th, 0, 1u << EVpadStart);
			RefCirc(aL.iC[EVpadIxMenu], EVpadKindAction, aW / 2, m + rm, rm, 0, 0, 0);
		}
	} else {
		const int yp = aW * 3 / 4;
		const int y1 = yp + m + sh;
		const int ym = ((y1 + sh + g) + (aH - m - 2 * rm - g)) / 2;
		RefCirc(aL.iC[EVpadIxDir], dirKind, m + rd, ym, rd, 1, 0, 0);
		RefRect(aL.iC[EVpadIxL1], m + sw, y1, sw, sh, 3, 1u << EVpadL1);
		RefRect(aL.iC[EVpadIxL2], m + 3 * sw + g, y1, sw, sh, 3, 1u << EVpadL2);
		RefRect(aL.iC[EVpadIxR1], aW - m - sw, y1, sw, sh, 4, 1u << EVpadR1);
		RefRect(aL.iC[EVpadIxR2], aW - m - 3 * sw - g, y1, sw, sh, 4, 1u << EVpadR2);
		RefRect(aL.iC[EVpadIxSelect], aW / 2 - rm - g - tw, aH - m - th, tw, th, 0, 1u << EVpadSelect);
		RefRect(aL.iC[EVpadIxStart], aW / 2 + rm + g + tw, aH - m - th, tw, th, 0, 1u << EVpadStart);
		RefCirc(aL.iC[EVpadIxMenu], EVpadKindAction, aW / 2, aH - m - rm, rm, 0, 0, 0);
		fx = aW - m - rr - rb;
		fy = ym;
	}
	RefFace(aL, fx, fy, rr, rb);
}

// Кожен елемент має розмір: коло iRx > 0, прямокутник iRx > 0 і iRy > 0.
static int RefSizesOk(const TVpadLayout &aL)
{
	int i;
	for (i = 0; i < aL.iCount; ++i) {
		const TVpadControl &c = aL.iC[i];
		if (c.iRx <= 0) return 0;
		if (c.iShape == EVpadShapeRect && c.iRy <= 0) return 0;
	}
	return 1;
}

// Вміщення за правилами (а), (б), (в) заголовка. Коло, d-pad і стік — коробка центр ± iRx.
static int RefFits(const TVpadLayout &aL, int aW, int aH)
{
	int x0[KVpadMaxControls];
	int y0[KVpadMaxControls];
	int x1[KVpadMaxControls];
	int y1[KVpadMaxControls];
	int use[KVpadMaxControls];
	const int n = TakenCount(aL);
	const int yp = aW * 3 / 4;
	int ok = 1;
	int i;
	int j;
	memset(x0, 0, sizeof x0);
	memset(y0, 0, sizeof y0);
	memset(x1, 0, sizeof x1);
	memset(y1, 0, sizeof y1);
	memset(use, 0, sizeof use);
	for (i = 0; i < n; ++i) {
		const TVpadControl &c = aL.iC[i];
		const int circle = c.iKind == EVpadKindDpad || c.iKind == EVpadKindStick || c.iShape == EVpadShapeCircle;
		const int hy = circle ? c.iRx : c.iRy;
		if (c.iRx <= 0 || hy <= 0) continue;
		use[i] = 1;
		x0[i] = c.iCx - c.iRx;
		x1[i] = c.iCx + c.iRx;
		y0[i] = c.iCy - hy;
		y1[i] = c.iCy + hy;
		if (x0[i] < 0 || x1[i] >= aW || y0[i] < 0 || y1[i] >= aH) ok = 0;
		if (aH > aW && y0[i] < yp) ok = 0;
	}
	for (i = 0; i < n; ++i) {
		for (j = i + 1; j < n; ++j) {
			if (!use[i] || !use[j]) continue;
			if (x1[i] < x0[j] || x1[j] < x0[i] || y1[i] < y0[j] || y1[j] < y0[i]) continue;
			if (aL.iC[i].iGroup != 0 && aL.iC[i].iGroup == aL.iC[j].iGroup) continue;
			ok = 0;
		}
	}
	return ok;
}

// Масштаби s = P*(32-i)/32, i = 0..19, перший, що вміщується, береться; інакше 0 і найменша спроба.
// iDualJumpPx = px(120) лише тоді, коли масштаб знайдено; при нулі заголовок не каже, який масштаб
// «остаточний», тож поле не звіряється (*aKnown = 0).
static int RefDefault(TVpadLayout &aL, int aW, int aH, int aU, int aP, unsigned aOpt, int *aKnown)
{
	TVpadLayout tmp;
	int i;
	int s = 0;
	if (aU < 20) aU = 20;
	if (aP < 25) aP = 25;
	if (aP > 400) aP = 400;
	for (i = 0; i < 20; ++i) {
		s = aP * (32 - i) / 32;
		RefBuild(tmp, aW, aH, aU, s, aOpt);
		if (RefSizesOk(tmp) && RefFits(tmp, aW, aH)) {
			if (aOpt & EVpadOptResistiveDual) tmp.iDualJumpPx = RefPx(120, s, aU);
			aL = tmp;
			*aKnown = 1;
			return s;
		}
	}
	aL = tmp;
	*aKnown = 0;
	return 0;
}

static int gReportLeft = 20;

static void Report(const char *aWhat, int aWant, int aGot)
{
	if (gReportLeft <= 0) return;
	--gReportLeft;
	printf("FAIL [%s] %s: expected %d, got %d\n", gWhere, aWhat, aWant, aGot);
}

static int Eq(const char *aWhat, int aGot, int aWant)
{
	if (aGot == aWant) return 0;
	Report(aWhat, aWant, aGot);
	return 1;
}

static int EqCtl(const char *aField, int aIx, int aGot, int aWant)
{
	char what[96];
	sprintf(what, "ctl %d %s", aIx, aField);
	return Eq(what, aGot, aWant);
}

// Число розбіжностей між реалізацією і еталоном; поля, які заголовок не фіксує, не звіряються.
static int CompareDefault(const TVpadLayout &aGot, const TVpadLayout &aRef, int aKnown)
{
	int bad = 0;
	int i;
	bad += Eq("iCount", aGot.iCount, aRef.iCount);
	bad += Eq("iHitPct", aGot.iHitPct, aRef.iHitPct);
	bad += Eq("iLeashPct", aGot.iLeashPct, aRef.iLeashPct);
	bad += Eq("iDeadPct", aGot.iDeadPct, aRef.iDeadPct);
	bad += Eq("iDiagPct", aGot.iDiagPct, aRef.iDiagPct);
	bad += Eq("iFourWayPct", aGot.iFourWayPct, aRef.iFourWayPct);
	bad += Eq("iStickDigitalPct", aGot.iStickDigitalPct, aRef.iStickDigitalPct);
	bad += Eq("iStickFloat", aGot.iStickFloat, aRef.iStickFloat);
	bad += Eq("iStickClickMs", aGot.iStickClickMs, aRef.iStickClickMs);
	if (aKnown) bad += Eq("iDualJumpPx", aGot.iDualJumpPx, aRef.iDualJumpPx);
	for (i = 0; i < TakenCount(aRef); ++i) {
		const TVpadControl &gc = aGot.iC[i];
		const TVpadControl &rc = aRef.iC[i];
		bad += EqCtl("iKind", i, gc.iKind, rc.iKind);
		bad += EqCtl("iShape", i, gc.iShape, rc.iShape);
		bad += EqCtl("iGroup", i, gc.iGroup, rc.iGroup);
		bad += EqCtl("iBits", i, gc.iBits, rc.iBits);
		bad += EqCtl("iCx", i, gc.iCx, rc.iCx);
		bad += EqCtl("iCy", i, gc.iCy, rc.iCy);
		bad += EqCtl("iRx", i, gc.iRx, rc.iRx);
		if (rc.iShape == EVpadShapeRect) bad += EqCtl("iRy", i, gc.iRy, rc.iRy);
		if (rc.iKind == EVpadKindAction || i == EVpadIxLStick || i == EVpadIxRStick) bad += EqCtl("iId", i, gc.iId, rc.iId);
	}
	return bad;
}

// ---------------------------------------------------------------- розкладка з EVpadOptSticks

static void TestSpecConstantsOfSticksAndLayout()
{
	// Числа зі специфікації: поріг тапу, мінімальний розмір, центр стіка, біти L3/R3, розміри розкладки.
	CHECK_EQ(KVpadTapMs, 250);
	CHECK_EQ(KVpadMinSizePx, 8);
	CHECK_EQ(KVpadStickCentre, 127);
	CHECK_EQ(EVpadL3, 1);
	CHECK_EQ(EVpadR3, 2);
	CHECK_EQ(EVpadOptSticks, 4);
	CHECK_EQ(EVpadDefaultCount, 12);
	CHECK_EQ(EVpadIxLStick, 12);
	CHECK_EQ(EVpadIxRStick, 13);
	CHECK_EQ(EVpadSticksCount, 14);
	CHECK_EQ(KVpadMaxControls, 16);
}

static void TestDefaultSticksLandscape640x360Scale100()
{
	// Як у TestDefaultLandscape640x360Scale100 для елементів 0..11, крім стиків і Select/Start/Menu.
	// Select і Start стають половинними: tw2 = px(43) = (4300+69)/138 = 31, th2 = px(27) = (2700+69)/138 = 20.
	// Вони стоять біля Menu на y = m + rm = 14+25 = 39: Select x = 320-25-7-31 = 257, Start x = 320+25+7+31 = 383.
	// Стіки: Rs = px(70) = 51, центри y = 360-14-51 = 295, лівий x = 320-7-51 = 262, правий x = 320+7+51 = 378.
	TVpadLayout L;
	Default14(L);
	CHECK_EQ(VpadDefaultLayout(L, 640, 360, 138, 100, EVpadOptSticks), 100);
	CHECK_EQ(L.iCount, EVpadSticksCount);
	CHECK_EQ(L.iStickClickMs, 300);
	CHECK_EQ(L.iDualJumpPx, 0);
	CheckCtl(L, EVpadIxDir, EVpadKindDpad, EVpadShapeCircle, 1, 0, 94, 266, 80, 0);
	CheckCtl(L, EVpadIxTriangle, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadTriangle, 539, 205, 33, 0);
	CheckCtl(L, EVpadIxCircle, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCircle, 593, 259, 33, 0);
	CheckCtl(L, EVpadIxCross, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCross, 539, 313, 33, 0);
	CheckCtl(L, EVpadIxSquare, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadSquare, 485, 259, 33, 0);
	CheckCtl(L, EVpadIxL1, EVpadKindButton, EVpadShapeRect, 3, 1 << EVpadL1, 65, 43, 51, 29);
	CheckCtl(L, EVpadIxL2, EVpadKindButton, EVpadShapeRect, 3, 1 << EVpadL2, 174, 43, 51, 29);
	CheckCtl(L, EVpadIxR1, EVpadKindButton, EVpadShapeRect, 4, 1 << EVpadR1, 575, 43, 51, 29);
	CheckCtl(L, EVpadIxR2, EVpadKindButton, EVpadShapeRect, 4, 1 << EVpadR2, 466, 43, 51, 29);
	CheckCtl(L, EVpadIxSelect, EVpadKindButton, EVpadShapeRect, 0, 1 << EVpadSelect, 257, 39, 31, 20);
	CheckCtl(L, EVpadIxStart, EVpadKindButton, EVpadShapeRect, 0, 1 << EVpadStart, 383, 39, 31, 20);
	CheckCtl(L, EVpadIxMenu, EVpadKindAction, EVpadShapeCircle, 0, 0, 320, 39, 25, 0);
	CheckCtl(L, EVpadIxLStick, EVpadKindStick, EVpadShapeCircle, 0, 0, 262, 295, 51, 0);
	CheckCtl(L, EVpadIxRStick, EVpadKindStick, EVpadShapeCircle, 0, 0, 378, 295, 51, 0);
	CHECK_EQ(L.iC[EVpadIxMenu].iId, 0);
	CHECK_EQ(L.iC[EVpadIxLStick].iId, 0);
	CHECK_EQ(L.iC[EVpadIxRStick].iId, 1);
	CHECK(VpadLayoutFits(L, 640, 360));
}

static void TestDefaultSticksPortraitIgnoredMatchesNoOption()
{
	// Портрет: EVpadOptSticks ігнорується. 360x640, U=111, s=100 дає 78 (як у TestDefaultPortrait...).
	TVpadLayout A;
	TVpadLayout B;
	int ra;
	int rb;
	memset(&A, 0, sizeof A);
	memset(&B, 0, sizeof B);
	ra = VpadDefaultLayout(A, 360, 640, 111, 100, 0);
	rb = VpadDefaultLayout(B, 360, 640, 111, 100, EVpadOptSticks);
	CHECK_EQ(ra, 78);
	CHECK_EQ(rb, 78);
	CHECK_EQ(B.iCount, EVpadDefaultCount);
	CHECK(SameAllCtl(A, B));
	CHECK_EQ(B.iStickClickMs, 300);
	CHECK_EQ(A.iStickClickMs, 300);
}

static void TestDefaultSticksSquareCountsAsLandscape()
{
	// W = H: за правилом aScreenW >= aScreenH це ландшафт, тож чотирнадцять елементів.
	// 640x640, U=138, s=100: F = (640-14-54-33, 640-14-54-33) = (539, 539); Dir (94, 640-14-80 = 546);
	// Tri (539, 485), Circle (593, 539), Cross (539, 593), Square (485, 539).
	// Стіки на y = 640-14-51 = 575, x = 262 і 378.
	TVpadLayout L;
	Default14(L);
	CHECK_EQ(VpadDefaultLayout(L, 640, 640, 138, 100, EVpadOptSticks), 100);
	CHECK_EQ(L.iCount, EVpadSticksCount);
	CheckCtl(L, EVpadIxDir, EVpadKindDpad, EVpadShapeCircle, 1, 0, 94, 546, 80, 0);
	CheckCtl(L, EVpadIxTriangle, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadTriangle, 539, 485, 33, 0);
	CheckCtl(L, EVpadIxCircle, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCircle, 593, 539, 33, 0);
	CheckCtl(L, EVpadIxCross, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadCross, 539, 593, 33, 0);
	CheckCtl(L, EVpadIxSquare, EVpadKindButton, EVpadShapeCircle, 2, 1 << EVpadSquare, 485, 539, 33, 0);
	CheckCtl(L, EVpadIxLStick, EVpadKindStick, EVpadShapeCircle, 0, 0, 262, 575, 51, 0);
	CheckCtl(L, EVpadIxRStick, EVpadKindStick, EVpadShapeCircle, 0, 0, 378, 575, 51, 0);
	CHECK(VpadLayoutFits(L, 640, 640));
}

static void TestDefaultSticksOptionStickChoosesDpadPerIndexRule()
{
	// Заголовок (коментарі EVpadIxDir і EVpadOptStick): з EVpadOptSticks у ландшафті елемент 0 — d-pad,
	// навіть разом з EVpadOptStick; EVpadOptStick не діє там, де діє EVpadOptSticks.
	// Окремий тест звіряє цю розкладку з розкладкою лише з EVpadOptSticks.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	CHECK_EQ(VpadDefaultLayout(L, 640, 360, 138, 100, EVpadOptStick | EVpadOptSticks), 100);
	CHECK_EQ(L.iCount, EVpadSticksCount);
	CHECK_EQ(L.iC[EVpadIxDir].iKind, EVpadKindDpad);
	CHECK_EQ(L.iC[EVpadIxLStick].iKind, EVpadKindStick);
	CHECK_EQ(L.iC[EVpadIxRStick].iKind, EVpadKindStick);
	CHECK_EQ(L.iC[EVpadIxLStick].iId, 0);
	CHECK_EQ(L.iC[EVpadIxRStick].iId, 1);
}

static void TestDefaultSticksDoNotFitShrinksScale()
{
	// 560x360, U=138, s=100. Дванадцять елементів вміщуються (перевірено за коробками).
	// З чотирнадцятьма лівий стік (центр x 280-7-51 = 222,
	// коробка 171..273 x 244..346) перетинає d-pad (коробка 14..174 x 186..346) у групах 0 і 1: при s=100
	// не вміщується, тож масштаб менший за 100.
	TVpadLayout A;
	TVpadLayout B;
	int r;
	memset(&A, 0, sizeof A);
	memset(&B, 0, sizeof B);
	CHECK_EQ(VpadDefaultLayout(A, 560, 360, 138, 100, 0), 100);
	CHECK(VpadLayoutFits(A, 560, 360));
	r = VpadDefaultLayout(B, 560, 360, 138, 100, EVpadOptSticks);
	CHECK(r > 0 && r < 100);
	CHECK_EQ(B.iCount, EVpadSticksCount);
	CHECK(VpadLayoutFits(B, 560, 360));
	// Стік при масштабі r: Rs = px(70), g = px(10), m = px(20); центр x = 280 - g - Rs, y = 360 - m - Rs.
	CHECK_EQ(B.iC[EVpadIxLStick].iRx, RefPx(70, r, 138));
	CHECK_EQ(B.iC[EVpadIxLStick].iCx, 280 - RefPx(10, r, 138) - RefPx(70, r, 138));
	CHECK_EQ(B.iC[EVpadIxLStick].iCy, 360 - RefPx(20, r, 138) - RefPx(70, r, 138));
}

static void TestDefaultLayoutMatchesReferenceAcrossScreensAndPitches()
{
	// Сітка: 11 екранів (ландшафт, портрет, квадрат, 40x40, яке нічого не вміщує), 6 кроків пікселя
	// (включно з нижньою межею 20), 5 масштабів (межі 25 і 400), 6 наборів параметрів. Усі поля, що
	// заголовок фіксує, звіряються з еталонною моделю. Здогадок про незафіксовані поля тут немає.
	static const int W[11] = { 640, 360, 640, 560, 480, 320, 240, 800, 400, 300, 40 };
	static const int H[11] = { 360, 640, 640, 360, 320, 240, 320, 352, 300, 400, 40 };
	static const int U[6] = { 20, 77, 111, 138, 200, 300 };
	static const int P[5] = { 25, 50, 100, 150, 400 };
	static const unsigned O[6] = { 0, EVpadOptStick, EVpadOptSticks, EVpadOptStick | EVpadOptSticks,
		EVpadOptResistiveDual, EVpadOptSticks | EVpadOptResistiveDual };
	int a;
	int b;
	int c;
	int d;
	int bad = 0;
	int zeros = 0;
	int sticks = 0;
	for (a = 0; a < 11; ++a) {
		for (b = 0; b < 6; ++b) {
			for (c = 0; c < 5; ++c) {
				for (d = 0; d < 6; ++d) {
					TVpadLayout got;
					TVpadLayout ref;
					int r;
					int rr;
					int known = 0;
					memset(&got, 0, sizeof got);
					sprintf(gWhere, "%s W=%d H=%d U=%d P=%d opt=%u", gTest, W[a], H[a], U[b], P[c], O[d]);
					r = VpadDefaultLayout(got, W[a], H[a], U[b], P[c], O[d]);
					rr = RefDefault(ref, W[a], H[a], U[b], P[c], O[d], &known);
					bad += Eq("return", r, rr);
					bad += CompareDefault(got, ref, known);
					if (rr == 0) ++zeros;
					else if (ref.iCount == EVpadSticksCount) ++sticks;
					strcpy(gWhere, gTest);
				}
			}
		}
	}
	CHECK_EQ(bad, 0);
	CHECK(zeros > 0);
	CHECK(sticks > 0);
}

static void TestFitsMatchesReferenceOnRandomLayouts()
{
	// Випадкові розкладки: нульові й від'ємні розміри, усі види і форми, групи, портрети. Вердикт
	// VpadLayoutFits має збігатися з еталонним за правилами (а), (б), (в).
	static const int W[4] = { 300, 200, 300, 120 };
	static const int H[4] = { 300, 300, 200, 160 };
	TRng R;
	int it;
	int bad = 0;
	int yes = 0;
	int no = 0;
	R.iS = 4242u;
	for (it = 0; it < 4000; ++it) {
		TVpadLayout L;
		const int s = it % 4;
		const int n = RndRange(R, 0, 16);
		int c;
		int got;
		int want;
		LayoutInit(L, n);
		for (c = 0; c < n; ++c) {
			TVpadControl &k = L.iC[c];
			memset(&k, 0, sizeof k);
			k.iKind = (unsigned char)RndRange(R, 0, 3);
			k.iShape = (unsigned char)RndRange(R, 0, 1);
			k.iGroup = (unsigned char)RndRange(R, 0, 2);
			k.iCx = (short)RndRange(R, -10, 330);
			k.iCy = (short)RndRange(R, -10, 330);
			k.iRx = (short)RndRange(R, -1, 120);
			k.iRy = (short)RndRange(R, -1, 120);
		}
		got = VpadLayoutFits(L, W[s], H[s]) != 0;
		want = RefFits(L, W[s], H[s]);
		if (got != want) {
			++bad;
			if (bad <= 5) printf("FAIL [%s] it %d (%dx%d, count %d): fits expected %d, got %d\n", gTest, it, W[s], H[s], n, want, got);
		}
		if (got) ++yes;
		else ++no;
	}
	CHECK_EQ(bad, 0);
	CHECK(yes > 0);
	CHECK(no > 0);
}

// ---------------------------------------------------------------- два стіки

static void TestTwoSticksSideFollowsIdAnyOtherIsLeft()
{
	// iId 1 — правий стік, будь-яке інше значення — лівий. Відхилення (140,100) від центра (100,100),
	// R=80: dx=40, mag=26, m=50, x=127+40*50/40=177.
	static const int kIds[4] = { 0, 1, 2, 255 };
	int k;
	for (k = 0; k < 4; ++k) {
		TVpadLayout L;
		TVpadState S;
		LayoutInit(L, 1);
		SetStickCtl(L.iC[0], 100, 100, 80, 0, kIds[k]);
		VpadReset(S);
		VpadPointerDown(S, L, 0, 100, 100, 1000);
		VpadPointerMove(S, L, 0, 140, 100, 1010);
		sprintf(gWhere, "%s iId %d", gTest, kIds[k]);
		if (kIds[k] == 1) {
			CHECK_EQ(S.iRightX, 177);
			CHECK_EQ(S.iLx, 127);
		} else {
			CHECK_EQ(S.iLx, 177);
			CHECK_EQ(S.iRightX, 127);
		}
		CHECK_EQ(S.iLy, 127);
		CHECK_EQ(S.iRightY, 127);
		strcpy(gWhere, gTest);
	}
}

static void TestRightStickDeflectsOnItsOwnAxesWithoutDpadBits()
{
	// Правий стік (450,100) R=80. Точка (514,100), dist 64: mag=50, m=96, x=127+64*96/64=223, біта немає
	// (правий стік d-pad-бітів не натискає, хоча 64*100 = 6400 >= 80*80). Рубіж (530,100), dist 80:
	// mag=66, m=127, x=254. Той самий відхил лівого (164,100) дає Right: контраст.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 450, 100, 1000);
	CHECK_EQ(S.iRightX, 127);
	VpadPointerMove(S, L, 0, 514, 100, 1010);
	CHECK_EQ(S.iRightX, 223);
	CHECK_EQ(S.iRightY, 127);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iLx, 127);
	VpadPointerMove(S, L, 0, 530, 100, 1020);
	CHECK_EQ(S.iRightX, 254);
	CHECK_EQ(S.iBits, 0);
	VpadPointerUp(S, L, 0, 1030);
	CHECK_EQ(S.iRightX, 127);
	VpadPointerDown(S, L, 1, 150, 100, 1040);
	VpadPointerMove(S, L, 1, 214, 100, 1050);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	CHECK_EQ(S.iLx, 223);
}

static void TestSecondPointerOnHeldLeftStickStaysInert()
{
	// Палець 0 тримає лівий стік (відхил 40, iLx=177). Палець 1 на тій самій стороні (150,100) — стік
	// пропускається, покажчик інертний. Рух до правого стіка (450,100) його не захоплює.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 190, 100, 1000);
	CHECK_EQ(S.iLx, 177);
	VpadPointerDown(S, L, 1, 150, 100, 1005);
	CHECK_EQ(S.iP[1].iCtl, -1);
	CHECK_EQ(S.iLx, 177);
	VpadPointerMove(S, L, 1, 450, 100, 1010);
	CHECK_EQ(S.iP[1].iCtl, -1);
	CHECK_EQ(S.iRightX, 127);
	CHECK_EQ(S.iBits, 0);
}

static void TestSecondPointerCapturesRightWhenLeftHeldAndZonesOverlap()
{
	// Зони стіків перетинаються: лівий (200,100), правий (260,100), обидва R=80, зона 100. Точка (230,100):
	// нормована відстань до обох 30*1000/100 = 300, перемагає нижчий індекс (лівий, ctl 0). Палець 1 у тій
	// самій точці: лівий тримається, тож пропускається, і захоплюється правий (ctl 1).
	// Лівий: dx=30, mag=16, m=30, x=127+30*30/30=157. Правий: dx=-30, x=127-30=97.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 2);
	SetStickCtl(L.iC[0], 200, 100, 80, 0, 0);
	SetStickCtl(L.iC[1], 260, 100, 80, 0, 1);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 230, 100, 1000);
	CHECK_EQ(S.iP[0].iCtl, 0);
	CHECK_EQ(S.iLx, 157);
	VpadPointerDown(S, L, 1, 230, 100, 1005);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iRightX, 97);
	CHECK_EQ(S.iRightY, 127);
	CHECK_EQ(S.iBits, 0);
}

static void TestDpadAndBothSticksWorkTogether()
{
	// Хрестовина (320,250) R50 — ctl 0; лівий стік (150,100) R80 — ctl 1; правий (450,100) R80 — ctl 2.
	// Палець 0 на (320,290): dist 40, хрестовина дає лише Down (q=0, тож не діагональ).
	// Палець 1 на (150,140): dist 40, лівий стік: y = 127+40*50/40 = 177, цифрового біта немає (40 < 64).
	// Палець 2 на (450,60): правий стік: y = 127-40*50/40 = 77.
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 3);
	SetCircCtl(L.iC[0], 320, 250, 50, 0, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	SetStickCtl(L.iC[1], 150, 100, 80, 0, 0);
	SetStickCtl(L.iC[2], 450, 100, 80, 0, 1);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 320, 290, 1000);
	VpadPointerDown(S, L, 1, 150, 140, 1001);
	VpadPointerDown(S, L, 2, 450, 60, 1002);
	CHECK_EQ(S.iP[0].iCtl, 0);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iP[2].iCtl, 2);
	CHECK_EQ(S.iBits, 1 << EVpadDown);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 177);
	CHECK_EQ(S.iRightX, 127);
	CHECK_EQ(S.iRightY, 77);
	VpadPointerUp(S, L, 0, 1010);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iLy, 177);
	CHECK_EQ(S.iRightY, 77);
}

static void TestBothSticksDeflectIndependentlyAndCentreOnRelease()
{
	// Два пальці на двох стіках одночасно: лівий 177, правий 77. Підняття лівого центрує лише лівий.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 0);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 190, 100, 1000);
	VpadPointerDown(S, L, 1, 410, 100, 1005);
	CHECK_EQ(S.iLx, 177);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iRightX, 77);
	CHECK_EQ(S.iRightY, 127);
	VpadPointerUp(S, L, 0, 1010);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iRightX, 77);
	VpadPointerUp(S, L, 1, 1020);
	CHECK_EQ(S.iRightX, 127);
	CHECK_EQ(S.iBits, 0);
}

static void TestVpadResetCentresBothSticksAndClearsPending()
{
	// Після відхилення обох стіків і цифрового біта VpadReset повертає все на початок, включно з
	// незабраними фронтами й діями («nothing pending»: це моє прочитання, див. звіт).
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 190, 100, 1000);
	VpadPointerMove(S, L, 0, 214, 100, 1002);
	VpadPointerDown(S, L, 1, 410, 100, 1005);
	CHECK_EQ(S.iBits, 1 << EVpadRight);
	VpadReset(S);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iEdges, 0);
	CHECK_EQ(S.iActions, 0);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iLy, 127);
	CHECK_EQ(S.iRightX, 127);
	CHECK_EQ(S.iRightY, 127);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[0].iDown, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iArmed[0], 0);
	CHECK_EQ(S.iArmed[1], 0);
}

// ---------------------------------------------------------------- клацання L3/R3

static void TestStickTapLimitIsInclusiveAt250Ms()
{
	// Дотик, відпущений через 250 мс після натискання (= KVpadTapMs), — тап, сторона зведена.
	// Через 251 мс — ні.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 1000, 1250);
	CHECK_EQ(S.iArmed[0], 1);
	CHECK_EQ(S.iArmMs[0], 1250);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 1000, 1251);
	CHECK_EQ(S.iArmed[0], 0);
}

static void TestStickClickWindowIsInclusiveAtIStickClickMs()
{
	// Зведення підняттям о 1000. Повторний дотик о 1300 (= iStickClickMs) клацає, о 1301 — ні.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1300);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1301);
	CHECK_EQ(S.iBits, 0);
}

static void TestStickClickPressesL3AtDownAndEdgeOnce()
{
	// Клацання: L3 уже в iBits цього ж виклику Down; фронт L3 один раз у iEdges; рух не дає нового
	// фронту; підняття знімає L3 і фронтів не додає.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	(void)VpadTakeEdges(S);
	VpadPointerDown(S, L, 0, 100, 100, 1100);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
	CHECK_EQ(VpadTakeEdges(S), 1 << EVpadL3);
	VpadPointerMove(S, L, 0, 110, 100, 1110);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
	CHECK_EQ(S.iEdges, 0);
	VpadPointerUp(S, L, 0, 1120);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iEdges, 0);
}

static void TestStickClickAddsDigitalBitsOfTheStick()
{
	// Клацання не замінює цифрових бітів: точка (164,100) при dist 64 дає Right; x = 223.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1100);
	VpadPointerMove(S, L, 0, 164, 100, 1110);
	CHECK_EQ(S.iBits, (1 << EVpadL3) | (1 << EVpadRight));
	CHECK_EQ(S.iLx, 223);
}

static void TestStickClickWindowZeroDisablesAndIsTakenWithinRange()
{
	// iStickClickMs 0 вимикає клацання; від'ємне береться як 0; 2500 береться як 2000: дотик через
	// 2000 мс клацає, через 2001 — ні.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 0);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1001);
	CHECK_EQ(S.iBits, 0);
	LayoutClick(L, -5);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1001);
	CHECK_EQ(S.iBits, 0);
	LayoutClick(L, 2500);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 3000);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 3001);
	CHECK_EQ(S.iBits, 0);
}

static void TestStickClickOnRightStickPressesR3()
{
	// Правий стік (iId 1) зведений дотиком, клацає R3 (біт 2) і тільки його.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 450, 100, 900, 1000);
	CHECK_EQ(S.iArmed[1], 1);
	CHECK_EQ(S.iArmed[0], 0);
	VpadPointerDown(S, L, 0, 450, 100, 1200);
	CHECK_EQ(S.iBits, 1 << EVpadR3);
	VpadPointerUp(S, L, 0, 1210);
	CHECK_EQ(S.iBits, 0);
}

static void TestStickIdOtherThanOneClicksAsLeft()
{
	// iId 5 — лівий стік: зведення і клацання йдуть у ліву сторону (L3, iArmed[0]).
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	L.iC[0].iId = 5;
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	CHECK_EQ(S.iArmed[0], 1);
	CHECK_EQ(S.iArmed[1], 0);
	VpadPointerDown(S, L, 0, 100, 100, 1100);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
}

static void TestClickTouchDisarmsItsSideAtLift()
{
	// Дотик, що клацнув, при підйомі (1160) роззброює свою сторону, тож дотик о 1200,
	// що був би в межах 300 мс, не клацає.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1100);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
	VpadPointerUp(S, L, 0, 1160);
	CHECK_EQ(S.iArmed[0], 0);
	CHECK_EQ(S.iBits, 0);
	VpadPointerDown(S, L, 0, 100, 100, 1200);
	CHECK_EQ(S.iBits, 0);
}

static void TestLongTouchDisarmsItsSide()
{
	// Дотик о 1400 не клацає (400 мс від зведення о 1000 > 300). Тривалість 1400..1700 = 300 > 250,
	// тож сторона не зводиться. Дотик о 1750 (50 мс від підняття) не клацає.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1400);
	CHECK_EQ(S.iBits, 0);
	VpadPointerUp(S, L, 0, 1700);
	CHECK_EQ(S.iArmed[0], 0);
	VpadPointerDown(S, L, 0, 100, 100, 1750);
	CHECK_EQ(S.iBits, 0);
}

static void TestTapArmsOnlyItsOwnSideAndWindowIsPerSide()
{
	// Лівий зведено о 1000. Правий дотик 1100..1150 (без клацання) зводить праву сторону о 1150.
	// Лівий дотик о 1200 клацає L3 і тільки L3: 1200-1000 = 200 <= 300.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 150, 100, 900, 1000);
	CHECK_EQ(S.iArmed[0], 1);
	VpadPointerDown(S, L, 0, 450, 100, 1100);
	CHECK_EQ(S.iBits, 0);
	VpadPointerUp(S, L, 0, 1150);
	CHECK_EQ(S.iArmed[1], 1);
	VpadPointerDown(S, L, 0, 150, 100, 1200);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
}

static void TestCaptureOfOneSideLeavesOtherSideArmed()
{
	// Правий зведено о 1000. Лівий дотик 1100..1150 (не клацає, зводить ліву) не чіпає праву:
	// правий дотик о 1200 (200 мс від зведення) клацає R3.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 450, 100, 900, 1000);
	TapStick(S, L, 0, 150, 100, 1100, 1150);
	VpadPointerDown(S, L, 0, 450, 100, 1200);
	CHECK_EQ(S.iBits, 1 << EVpadR3);
}

static void TestVpadResetClearsStickArming()
{
	// VpadReset знімає зведення обох сторін і центрує стіки: дотик у вікні після скидання не клацає.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	CHECK_EQ(S.iArmed[0], 1);
	VpadReset(S);
	CHECK_EQ(S.iArmed[0], 0);
	CHECK_EQ(S.iArmed[1], 0);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iRightX, 127);
	CHECK_EQ(S.iRightY, 127);
	VpadPointerDown(S, L, 0, 100, 100, 1100);
	CHECK_EQ(S.iBits, 0);
}

static void TestStickClockGoingBackCountsAsNoElapsedTime()
{
	// Час, що йде назад (різниця 2^31 і більше), — «часу не минуло».
	// (а) Підняття о 900 раніше натискання о 1000: тривалість 0, тап, сторона зведена на момент 900.
	// (б) Зведено о 1000; дотик о 900 (раніше зведення) — 0 мс пройшло, клацає.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 1000, 900);
	CHECK_EQ(S.iArmed[0], 1);
	CHECK_EQ(S.iArmMs[0], 900);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 900);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
}

static void TestStickClickAndTapAcrossClockWrap()
{
	// Лічильник переходить через 0xFFFFFFFF. Тап 0xFFFFFF00..0xFFFFFFF0 — 240 мс, зведення; клацання о
	// 0x00000004 — 0x14 = 20 мс від зведення. Тап через нуль 0xFFFFFF06..0x00000000 — рівно 250 (тап),
	// 0xFFFFFF05..0x00000000 — 251 (ні).
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 0xFFFFFF00u, 0xFFFFFFF0u);
	CHECK_EQ(S.iArmed[0], 1);
	VpadPointerDown(S, L, 0, 100, 100, 0x00000004u);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 0xFFFFFF06u, 0x00000000u);
	CHECK_EQ(S.iArmed[0], 1);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 0xFFFFFF05u, 0x00000000u);
	CHECK_EQ(S.iArmed[0], 0);
}

// ---------------------------------------------------------------- інференція і стіки

static void TestInferredStickNeverClicksAndLeavesArmingAlone()
{
	// Лівий стік зведено о 1000..1100. Палець 0 на кнопці (300,100) о 1150, рух на (220,100) о 1210:
	// 1210-1150 = 60 <= 60, відстань 80 >= 40, P2 = (140,100) — у зоні натиску стіка (40 <= 62) і в його
	// повіднці (40 <= 87): інференція вмикає стік. Стік другого пальця не клацає, хоча 1210-1100 = 110
	// <= 300, і зведення не чіпає. Підняття пальця 0 о 1260 знімає і другий палець. Дотик стіка о 1300
	// (200 мс від зведення) клацає, тож зведення живе.
	TVpadLayout L;
	TVpadState S;
	LayoutInferStick(L);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 1000, 1100);
	CHECK_EQ(S.iArmed[0], 1);
	VpadPointerDown(S, L, 0, 300, 100, 1150);
	CHECK_EQ(S.iP[0].iCtl, 0);
	VpadPointerMove(S, L, 0, 220, 100, 1210);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iP[1].iClick, 0);
	CHECK_EQ(S.iBits & (1 << EVpadL3), 0);
	CHECK_EQ(S.iArmed[0], 1);
	CHECK_EQ(S.iArmMs[0], 1100);
	VpadPointerUp(S, L, 0, 1260);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iArmed[0], 1);
	CHECK_EQ(S.iArmMs[0], 1100);
	VpadPointerDown(S, L, 0, 100, 100, 1300);
	CHECK_EQ(S.iBits & (1 << EVpadL3), 1 << EVpadL3);
}

static void TestInferredStickTakenOverByFirstFingerNeverArms()
{
	// Зведення раніше не було. Палець 0 на кнопці о 1000; (220,100) о 1060 вмикає інференцію, P2 = (140,100).
	// (143,100) о 1070: до першого пальця (300,100) далеко (157 > r = 13), до P2 близько (3 <= 13), тож
	// перший палець пішов: палець 0 перейняв стік. Після перейняття рух до M = (143,100) — звичайний:
	// dx=43, dead=9, mag=34, span=41, m=34*127/41=105, iLx=127+43*105/43=232. Підняття о 1100 стік не зводить.
	TVpadLayout L;
	TVpadState S;
	LayoutInferStick(L);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 300, 100, 1000);
	VpadPointerMove(S, L, 0, 220, 100, 1060);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	VpadPointerMove(S, L, 0, 143, 100, 1070);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[0].iCtl, 1);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iLx, 232);
	VpadPointerUp(S, L, 0, 1100);
	CHECK_EQ(S.iArmed[0], 0);
	CHECK_EQ(S.iArmed[1], 0);
	CHECK_EQ(S.iLx, 127);
}

static void TestTakeoverDropsFirstFingerSideAndNeverArms()
{
	// Лівий стік (100,100) — ctl 0 — зведено о 900..1000; клацання о 1100 (L3 на пальці 0, тобто
	// проба 1100-1000 = 100 <= 300). Інференція: M = (190,100) о 1150 (50 мс від 1100, відстань 90),
	// P2 = (280,100) у зоні правого стіка (300,100). Перший палець (100,100) пішов: M = (283,100) о 1160,
	// палець 0 перейняв правий стік; лівий захоплення втратив.
	// Правий: dx=-17, dead=9, mag=8, m=24, iRightX=127-17*24/17=103. Правий не зводиться і ніколи не клацає.
	// Заголовок (абзац «Stick click»): після перехоплення пойнтер 0 не тисне L3 і R3, хоч би що тиснув раніше;
	// очікування: iBits 0 і iClick 0. Попереднє буквальне прочитання «until it is lifted» вимагало L3, і заголовок це вже уточнив.
	TVpadLayout L;
	TVpadState S;
	LayoutInferTwoSticks(L);
	VpadReset(S);
	TapStick(S, L, 0, 100, 100, 900, 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1100);
	CHECK_EQ(S.iBits & (1 << EVpadL3), 1 << EVpadL3);
	VpadPointerMove(S, L, 0, 190, 100, 1150);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	VpadPointerMove(S, L, 0, 283, 100, 1160);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[0].iCtl, 1);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iRightX, 103);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iP[0].iClick, 0);
	VpadPointerUp(S, L, 0, 1170);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iArmed[0], 0);
	CHECK_EQ(S.iArmed[1], 0);
}

static void TestRandomSequenceTwoSticksLandscape()
{
	// 30000 подій на розкладці з двома стіками; інваріанти після кожного виклику, у тому числі правий стік
	// і клацання.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	VpadDefaultLayout(L, 640, 360, 138, 100, EVpadOptSticks);
	RunRandomSequence(L, 1020304050u, 30000);
}

static void TestRandomSequenceTwoSticksDualPanel()
{
	// Те саме з резистивною панеллю: інференція другого пальця і стіки разом.
	TVpadLayout L;
	memset(&L, 0, sizeof L);
	VpadDefaultLayout(L, 640, 360, 138, 100, EVpadOptSticks | EVpadOptResistiveDual);
	RunRandomSequence(L, 556677889u, 30000);
}

// ---------------------------------------------------------------- редагування розкладки

static void TestControlAtCentreOfEachControlOfSticksLayout()
{
	// Центр кожного з чотирнадцяти елементів: нормована відстань 0, тож захоплює сам елемент.
	TVpadLayout L;
	int k;
	Default14(L);
	for (k = 0; k < EVpadSticksCount; ++k) {
		sprintf(gWhere, "%s ctl %d", gTest, k);
		CHECK_EQ(VpadControlAt(L, L.iC[k].iCx, L.iC[k].iCy), k);
		strcpy(gWhere, gTest);
	}
}

static void TestControlAtOutsideAllZonesIsMinusOne()
{
	// Порожнє місце посередині (320,200) і точка далеко за екраном: жодна зона натиску не містить її.
	TVpadLayout L;
	Default14(L);
	CHECK_EQ(VpadControlAt(L, 320, 200), -1);
	CHECK_EQ(VpadControlAt(L, -300, -300), -1);
}

static void TestControlAtNearestByNormalisedDistanceAcrossShapes()
{
	// Коло (0,0) R40 (зона 50) і прямокутник (100,0) з півосями (100,20) (зона 125 x 25). Точка (30,0):
	// коло 30*1000/50 = 600, прямокутник 70*1000/125 = 560. Сира відстань дала б коло; перемагає (1).
	TVpadLayout L;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 0, 0, 40, 0, 0);
	SetRectCtl(L.iC[1], 100, 0, 100, 20, 0);
	CHECK_EQ(VpadControlAt(L, 30, 0), 1);
}

static void TestControlAtTieGoesToLowerIndex()
{
	// Обидва кола R40 (зона 50): точка (50,0) має нормовану відстань 1000 до кожного. Нижчий індекс.
	// Друга частина: нижчий індекс стоїть правіше, рішення все одно за індексом.
	TVpadLayout L;
	LayoutInit(L, 2);
	SetCircCtl(L.iC[0], 0, 0, 40, 0, 0);
	SetCircCtl(L.iC[1], 100, 0, 40, 0, 0);
	CHECK_EQ(VpadControlAt(L, 50, 0), 0);
	SetCircCtl(L.iC[0], 100, 0, 40, 0, 0);
	SetCircCtl(L.iC[1], 0, 0, 40, 0, 0);
	CHECK_EQ(VpadControlAt(L, 50, 0), 0);
}

static void TestControlAtCountAndSizeRules()
{
	// Від'ємне iCount — 0, більше 16 — 16. Елемент без розміру ігнорується; прямокутник з iRy=0 теж;
	// d-pad з прямокутною формою і iRy=0 діє як коло.
	TVpadLayout L;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 50, 50, 20, 0, 0);
	CHECK_EQ(VpadControlAt(L, 50, 50), 0);
	L.iCount = 0;
	CHECK_EQ(VpadControlAt(L, 50, 50), -1);
	L.iCount = -3;
	CHECK_EQ(VpadControlAt(L, 50, 50), -1);
	L.iCount = 17;
	CHECK_EQ(VpadControlAt(L, 50, 50), 0);
	L.iCount = 1;
	L.iC[0].iRx = 0;
	CHECK_EQ(VpadControlAt(L, 50, 50), -1);
	SetRectCtl(L.iC[0], 50, 50, 20, 0, 0);
	CHECK_EQ(VpadControlAt(L, 50, 50), -1);
	SetRectCtl(L.iC[0], 50, 50, 20, 20, 0);
	L.iC[0].iKind = (unsigned char)EVpadKindDpad;
	L.iC[0].iRy = 0;
	CHECK_EQ(VpadControlAt(L, 50, 50), 0);
}

static void TestControlAtClampsPointLikePointer()
{
	// Координата точки обрізається до -8192..8191: (10000,100) стає (8191,100), а прямокутник на (8190,100)
	// з півосями (1,1) містить її: зона дорівнює 1 (125% від 1), dx = 1. Без обрізання точка далеко.
	TVpadLayout L;
	LayoutInit(L, 1);
	SetRectCtl(L.iC[0], 8190, 100, 1, 1, 0);
	CHECK_EQ(VpadControlAt(L, 10000, 100), 0);
	SetRectCtl(L.iC[0], 0, 100, 1, 1, 0);
	CHECK_EQ(VpadControlAt(L, -10000, 100), -1);
}

static void TestMoveControlFreeAndAxisClamps()
{
	// Вільний рух (200,150) у межах: r=80 на 640x360 дає x 80..559 і y 80..279. Потім (-50,500) → (80,279).
	// Прямокутник L1 (r 51 x 29): (700,0) → x 51..588 → 588, y 29..330 → 29. Інші елементи не змінюються.
	TVpadLayout L;
	TVpadLayout before;
	Default14(L);
	before = L;
	VpadMoveControl(L, 0, 200, 150, 640, 360);
	CHECK_EQ(L.iC[0].iCx, 200);
	CHECK_EQ(L.iC[0].iCy, 150);
	CHECK_EQ(L.iC[0].iRx, 80);
	VpadMoveControl(L, 0, -50, 500, 640, 360);
	CHECK_EQ(L.iC[0].iCx, 80);
	CHECK_EQ(L.iC[0].iCy, 279);
	VpadMoveControl(L, EVpadIxL1, 700, 0, 640, 360);
	CHECK_EQ(L.iC[EVpadIxL1].iCx, 588);
	CHECK_EQ(L.iC[EVpadIxL1].iCy, 29);
	CHECK_EQ(L.iC[EVpadIxL1].iRx, 51);
	CHECK_EQ(L.iC[EVpadIxL1].iRy, 29);
	CHECK_EQ(L.iC[1].iCx, before.iC[1].iCx);
	CHECK_EQ(L.iC[1].iCy, before.iC[1].iCy);
}

static void TestMoveControlRadiusLargerThanRangeGivesHalfScreen()
{
	// Коло R=200 на 300x100: по x 300-1-200 = 99 < 200, центр 300/2 = 150; по y 100-1-200 < 200, центр 100/2 = 50.
	// Прямокутник (200,10) на тому самому екрані: по x центр 150, по y діапазон 10..89, запит 5 → 10.
	TVpadLayout L;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 0, 0, 200, 0, 0);
	VpadMoveControl(L, 0, 0, 0, 300, 100);
	CHECK_EQ(L.iC[0].iCx, 150);
	CHECK_EQ(L.iC[0].iCy, 50);
	CHECK_EQ(L.iC[0].iRx, 200);
	SetRectCtl(L.iC[0], 0, 0, 200, 10, 0);
	VpadMoveControl(L, 0, 0, 5, 300, 100);
	CHECK_EQ(L.iC[0].iCx, 150);
	CHECK_EQ(L.iC[0].iCy, 10);
	CHECK_EQ(L.iC[0].iRx, 200);
	CHECK_EQ(L.iC[0].iRy, 10);
}

static void TestMoveControlScreenBelowOneTakenAsOne()
{
	// Розміри екрана менше 1 беруться як 1: n=1, r=5 > 1-1-5, центр n/2 = 0 на обох осях.
	TVpadLayout L;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 50, 50, 5, 0, 0);
	VpadMoveControl(L, 0, 100, 100, 0, -4);
	CHECK_EQ(L.iC[0].iCx, 0);
	CHECK_EQ(L.iC[0].iCy, 0);
}

static void TestMoveControlIgnoredForBadIndexAndNoSize()
{
	// Індекс поза 0..iCount-1 (-1, 14 і 15 при iCount 14) і елемент без розміру до виклику: без змін.
	TVpadLayout L;
	TVpadLayout before;
	Default14(L);
	before = L;
	VpadMoveControl(L, -1, 100, 100, 640, 360);
	VpadMoveControl(L, EVpadSticksCount, 100, 100, 640, 360);
	VpadMoveControl(L, 15, 100, 100, 640, 360);
	CHECK(SameAllCtl(before, L));
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 50, 50, 0, 0, 0);
	VpadMoveControl(L, 0, 100, 100, 640, 360);
	CHECK_EQ(L.iC[0].iCx, 50);
	CHECK_EQ(L.iC[0].iCy, 50);
	SetRectCtl(L.iC[0], 50, 50, 20, 0, 0);
	VpadMoveControl(L, 0, 100, 100, 640, 360);
	CHECK_EQ(L.iC[0].iCx, 50);
	CHECK_EQ(L.iC[0].iCy, 50);
}

static void TestResizeControlCircleKeepsRyAndCentreInside()
{
	// Коло d-pad (94,266) на 640x360: R=60, центр у межах 60..579 і 60..299 — лишається.
	// Для кола aRy ігнорується, iRy лишається як був (7).
	TVpadLayout L;
	Default14(L);
	L.iC[0].iRy = 7;
	VpadResizeControl(L, 0, 60, 99, 640, 360);
	CHECK_EQ(L.iC[0].iRx, 60);
	CHECK_EQ(L.iC[0].iRy, 7);
	CHECK_EQ(L.iC[0].iCx, 94);
	CHECK_EQ(L.iC[0].iCy, 266);
}

static void TestResizeControlRectSizesAndCentre()
{
	// Прямокутник L1 (65,43), півосі (51,29) → (40,20): центр у межах 40..599 і 20..339, лишається.
	TVpadLayout L;
	Default14(L);
	VpadResizeControl(L, EVpadIxL1, 40, 20, 640, 360);
	CHECK_EQ(L.iC[EVpadIxL1].iRx, 40);
	CHECK_EQ(L.iC[EVpadIxL1].iRy, 20);
	CHECK_EQ(L.iC[EVpadIxL1].iCx, 65);
	CHECK_EQ(L.iC[EVpadIxL1].iCy, 43);
}

static void TestResizeControlClampsToMinAndMax()
{
	// Значення беруться в KVpadMinSizePx..4095: 3 → 8 для кола; (4,5) → (8,8) і (-3,0) → (8,8) для
	// прямокутника. 5000 → 4095; на екрані 8191x8191 центр притиснутий до 4095 (= 8191-1-4095).
	TVpadLayout L;
	Default14(L);
	VpadResizeControl(L, 0, 3, 0, 640, 360);
	CHECK_EQ(L.iC[0].iRx, KVpadMinSizePx);
	VpadResizeControl(L, EVpadIxL1, 4, 5, 640, 360);
	CHECK_EQ(L.iC[EVpadIxL1].iRx, 8);
	CHECK_EQ(L.iC[EVpadIxL1].iRy, 8);
	VpadResizeControl(L, EVpadIxL1, -3, 0, 640, 360);
	CHECK_EQ(L.iC[EVpadIxL1].iRx, 8);
	CHECK_EQ(L.iC[EVpadIxL1].iRy, 8);
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 100, 100, 50, 0, 0);
	VpadResizeControl(L, 0, 5000, 0, 8191, 8191);
	CHECK_EQ(L.iC[0].iRx, 4095);
	CHECK_EQ(L.iC[0].iCx, 4095);
	CHECK_EQ(L.iC[0].iCy, 4095);
}

static void TestResizeControlPullsCentreIntoScreen()
{
	// Коло (10,10) R50 на 640x360, новий R=100: центр тягнеться до 100..539 і 100..259 → (100,100).
	// На 100x100 з R=60: 100-1-60 = 39 < 60, центр n/2 = 50 на обох осях.
	TVpadLayout L;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 10, 10, 50, 0, 0);
	VpadResizeControl(L, 0, 100, 0, 640, 360);
	CHECK_EQ(L.iC[0].iRx, 100);
	CHECK_EQ(L.iC[0].iCx, 100);
	CHECK_EQ(L.iC[0].iCy, 100);
	SetCircCtl(L.iC[0], 10, 10, 10, 0, 0);
	VpadResizeControl(L, 0, 60, 0, 100, 100);
	CHECK_EQ(L.iC[0].iRx, 60);
	CHECK_EQ(L.iC[0].iCx, 50);
	CHECK_EQ(L.iC[0].iCy, 50);
}

static void TestResizeControlIgnoredForBadIndexAndNoSize()
{
	// Індекс поза межами — без змін. Елемент, що вже без розміру до виклику, теж не змінюється: так
	// прочитано буквально («the control has no size»); це одне з місць, де заголовок може мати інше
	// прочитання (див. звіт).
	TVpadLayout L;
	TVpadLayout before;
	Default14(L);
	before = L;
	VpadResizeControl(L, -1, 30, 30, 640, 360);
	VpadResizeControl(L, EVpadSticksCount, 30, 30, 640, 360);
	CHECK(SameAllCtl(before, L));
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 50, 50, 0, 0, 0);
	VpadResizeControl(L, 0, 30, 0, 100, 100);
	CHECK_EQ(L.iC[0].iRx, 0);
	CHECK_EQ(L.iC[0].iCx, 50);
}

static void TestResizeStickKeepsRy()
{
	// Лівий стік (262,295), R=51: Resize задає iRx=60, iRy стіка (коло) лишається як був. Центр у межах
	// 60..579 і 60..299: 295 лишається.
	TVpadLayout L;
	int ry0;
	Default14(L);
	ry0 = L.iC[EVpadIxLStick].iRy;
	VpadResizeControl(L, EVpadIxLStick, 60, 30, 640, 360);
	CHECK_EQ(L.iC[EVpadIxLStick].iKind, EVpadKindStick);
	CHECK_EQ(L.iC[EVpadIxLStick].iRx, 60);
	CHECK_EQ(L.iC[EVpadIxLStick].iRy, ry0);
	CHECK_EQ(L.iC[EVpadIxLStick].iCx, 262);
	CHECK_EQ(L.iC[EVpadIxLStick].iCy, 295);
}

// ---------------------------------------------------------------- друга хвиля: один бік, виведення, iByDown

// Вісь бока: 0 — лівий (iLx), 1 — правий (iRightX).
static int Axis(const TVpadState &aS, int aSide)
{
	return aSide == 0 ? aS.iLx : aS.iRightX;
}

// Два різні стіки одного боку (обидва з iId aId), зони не перекриваються: зона натиску першого 0..200,
// другого 220..420 по x (R=80, зона 100).
static void LayoutSameSide(TVpadLayout &aL, int aId)
{
	LayoutInit(aL, 2);
	SetStickCtl(aL.iC[0], 100, 100, 80, 0, aId);
	SetStickCtl(aL.iC[1], 320, 100, 80, 0, aId);
}

static void TestSameSideSecondStickStaysInertWhileFirstHeld()
{
	// Стік не захоплюється, якщо інший вказівник тримає стік того самого боку: дотик інертний до підняття.
	// Після підняття першого новий дотик захоплює другий стік. Точка (340,100) від центра (320,100):
	// dx=20, dead=14, mag=6, m=6*127/66=11, відхил 127+11 = 138.
	static const int kSide[2] = { 0, 1 };
	int k;
	for (k = 0; k < 2; ++k) {
		TVpadLayout L;
		TVpadState S;
		LayoutSameSide(L, kSide[k]);
		VpadReset(S);
		sprintf(gWhere, "%s side %d", gTest, kSide[k]);
		VpadPointerDown(S, L, 0, 100, 100, 1000);
		CHECK_EQ(S.iP[0].iCtl, 0);
		VpadPointerDown(S, L, 1, 320, 100, 1005);
		CHECK_EQ(S.iP[1].iDown, 1);
		CHECK_EQ(S.iP[1].iCtl, -1);
		VpadPointerMove(S, L, 1, 340, 100, 1010);
		CHECK_EQ(S.iP[1].iCtl, -1);
		CHECK_EQ(Axis(S, kSide[k]), 127);
		CHECK_EQ(S.iBits, 0);
		VpadPointerUp(S, L, 0, 1020);
		CHECK_EQ(Axis(S, kSide[k]), 127);
		VpadPointerDown(S, L, 2, 320, 100, 1030);
		CHECK_EQ(S.iP[2].iCtl, 1);
		VpadPointerMove(S, L, 2, 340, 100, 1040);
		CHECK_EQ(Axis(S, kSide[k]), 138);
		CHECK_EQ(S.iP[1].iCtl, -1);
		strcpy(gWhere, gTest);
	}
}

static void TestSameSideStickSkippedSoButtonUnderPointIsCaptured()
{
	// Стік одного боку пропускається, тож під точкою захоплюється кнопка. Точка (300,100): стік (320,100) R80
	// дає нормовану відстань 20*1000/100 = 200, кнопка Triangle (330,100) R40 — 30*1000/50 = 600. Без пропуску
	// ближчий стік захопив би другий вказівник. Після підняття кнопка не тисне.
	static const int kSide[2] = { 0, 1 };
	int k;
	for (k = 0; k < 2; ++k) {
		TVpadLayout L;
		TVpadState S;
		LayoutInit(L, 3);
		SetStickCtl(L.iC[0], 100, 100, 80, 0, kSide[k]);
		SetStickCtl(L.iC[1], 320, 100, 80, 0, kSide[k]);
		SetCircCtl(L.iC[2], 330, 100, 40, 0, 1u << EVpadTriangle);
		VpadReset(S);
		sprintf(gWhere, "%s side %d", gTest, kSide[k]);
		VpadPointerDown(S, L, 0, 100, 100, 1000);
		VpadPointerDown(S, L, 1, 300, 100, 1005);
		CHECK_EQ(S.iP[1].iCtl, 2);
		CHECK_EQ(S.iBits, 1 << EVpadTriangle);
		VpadPointerUp(S, L, 1, 1010);
		CHECK_EQ(S.iBits, 0);
		strcpy(gWhere, gTest);
	}
}

static void TestOppositeSideStickCapturedWhileOtherSideHeld()
{
	// Стік протилежного боку за тих самих умов захоплюється: перший вказівник тримає стік одного боку,
	// другий опускається на стік іншого. Точка (340,100) від (320,100): відхил 138, як у попередньому тесті.
	static const int kFirst[2] = { 0, 1 };
	int k;
	for (k = 0; k < 2; ++k) {
		TVpadLayout L;
		TVpadState S;
		const int f = kFirst[k];
		const int s = 1 - f;
		LayoutInit(L, 2);
		SetStickCtl(L.iC[0], 100, 100, 80, 0, f);
		SetStickCtl(L.iC[1], 320, 100, 80, 0, s);
		VpadReset(S);
		sprintf(gWhere, "%s first side %d", gTest, f);
		VpadPointerDown(S, L, 0, 100, 100, 1000);
		VpadPointerDown(S, L, 1, 320, 100, 1005);
		CHECK_EQ(S.iP[1].iCtl, 1);
		VpadPointerMove(S, L, 1, 340, 100, 1010);
		CHECK_EQ(Axis(S, s), 138);
		CHECK_EQ(Axis(S, f), 127);
		CHECK_EQ(S.iBits, 0);
		strcpy(gWhere, gTest);
	}
}

static void TestTakeoverKeepsArmedOtherSideAndDoesNotArmDroppedOne()
{
	// Правий стік зведено дотиком 900..1000 (пальцем 0). Пальцем 0 береться лівий стік у 1100 (без клацання,
	// його сторона не зводиться).Виведення захоплює правий стік у P2 = (280,100): стік виведення
	// не клацає і не чіпає зведення. Перехоплення пальцем 0 кидає лівий захват: це не підняття, тож лівий
	// не зводиться (його перший дотик тривав 60 мс і міг би звестися, якби кидання трактувалось як підняття),
	// а правий зведення зберігає. Тоді дотик правого о 1180 (180 мс від підняття о 1000) клацає R3.
	// Перевірено: iRightX у P2: dx=-20, dead=9, mag=11, m=34, 127-34=93; після перехоплення dx=-17,
	// mag=8, m=24, 103.
	TVpadLayout L;
	TVpadState S;
	LayoutInferTwoSticks(L);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 300, 100, 900);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[1], 1);
	CHECK_EQ(S.iArmMs[1], 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1100);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iByDown, 1);
	CHECK_EQ(S.iArmed[0], 0);
	VpadPointerMove(S, L, 0, 190, 100, 1150);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 1);
	CHECK_EQ(S.iP[1].iClick, 0);
	CHECK_EQ(S.iArmed[1], 1);
	CHECK_EQ(S.iArmMs[1], 1000);
	CHECK_EQ(S.iByDown, 1);
	CHECK_EQ(S.iRightX, 93);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iBits, 0);
	VpadPointerMove(S, L, 0, 283, 100, 1160);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[0].iCtl, 1);
	CHECK_EQ(S.iP[1].iDown, 0);
	CHECK_EQ(S.iRightX, 103);
	CHECK_EQ(S.iLx, 127);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iByDown, 0);
	CHECK_EQ(S.iArmed[0], 0);
	CHECK_EQ(S.iArmed[1], 1);
	CHECK_EQ(S.iArmMs[1], 1000);
	VpadPointerUp(S, L, 0, 1170);
	CHECK_EQ(S.iArmed[0], 0);
	CHECK_EQ(S.iArmed[1], 1);
	CHECK_EQ(S.iRightX, 127);
	CHECK_EQ(S.iBits, 0);
	VpadPointerDown(S, L, 0, 300, 100, 1180);
	CHECK_EQ(S.iBits, 1 << EVpadR3);
	CHECK_EQ(S.iArmed[1], 0);
	VpadPointerUp(S, L, 0, 1190);
	CHECK_EQ(S.iBits, 0);
}

static void TestTakeoverAfterRightClickPressesNoR3()
{
	// Пальцем 0 зведено правий стік і клацнуто по ньому (R3 тисне). Виведення захоплює лівий стік у
	// P2 = (120,100), перехоплення (117,100) передає лівий стік пальцю 0. Тоді пальці не тиснуть ні L3, ні R3,
	// клацання більше не тисне, а права сторона роззброєна клацанням: захоплення Down її роззброїло, і
	// зведення не відновлюється ні перехопленням, ні підняттям. Лівий стік: dx=17, mag=8, m=24, відхил 127+24=151.
	TVpadLayout L;
	TVpadState S;
	LayoutInferTwoSticks(L);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 300, 100, 900);
	VpadPointerUp(S, L, 0, 1000);
	VpadPointerDown(S, L, 0, 300, 100, 1100);
	CHECK_EQ(S.iBits, 1 << EVpadR3);
	CHECK_EQ(S.iP[0].iClick, 1);
	VpadPointerMove(S, L, 0, 210, 100, 1150);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iP[1].iCtl, 0);
	CHECK_EQ(S.iP[1].iClick, 0);
	VpadPointerMove(S, L, 0, 117, 100, 1160);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[0].iCtl, 0);
	CHECK_EQ(S.iLx, 151);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iP[0].iClick, 0);
	CHECK_EQ(S.iByDown, 0);
	CHECK_EQ(S.iArmed[0], 0);
	CHECK_EQ(S.iArmed[1], 0);
	VpadPointerUp(S, L, 0, 1170);
	CHECK_EQ(S.iBits, 0);
	VpadPointerDown(S, L, 0, 300, 100, 1180);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iP[0].iClick, 0);
}

static void TestTwoSticksOfOneSideThroughInferenceAxesBelongToHigherPointer()
{
	// Два вказівники тримають стіки одного боку лише через виведення. Осі — вказівника з більшим номером.
	// Стіки A (100,100) і B (290,100), R=50, зони 38..162 і 228..352 не перекриваються. Палець 0 на A (1000),
	// рух до (180,100) о 1050: P2 = (260,100), у повіднці B (30 <= 87). B у P2: dx=-30, dead=9, mag=21,
	// m=21*127/41=65, відхил 127-65 = 62. Лівий стік пальця 0 стоїть в центрі, тож 127 на осі пальця 0.
	// Цифрових бітів немає: 30*100 = 3000 < 50*80 = 4000.
	static const int kSide[2] = { 0, 1 };
	int k;
	for (k = 0; k < 2; ++k) {
		TVpadLayout L;
		TVpadState S;
		LayoutInit(L, 2);
		SetStickCtl(L.iC[0], 100, 100, 50, 0, kSide[k]);
		SetStickCtl(L.iC[1], 290, 100, 50, 0, kSide[k]);
		L.iDualJumpPx = 40;
		VpadReset(S);
		sprintf(gWhere, "%s side %d", gTest, kSide[k]);
		VpadPointerDown(S, L, 0, 100, 100, 1000);
		CHECK_EQ(S.iByDown, 1);
		VpadPointerMove(S, L, 0, 180, 100, 1050);
		CHECK_EQ(S.iDual, 1);
		CHECK_EQ(S.iP[0].iCtl, 0);
		CHECK_EQ(S.iP[1].iCtl, 1);
		CHECK_EQ(Axis(S, kSide[k]), 62);
		CHECK_EQ(Axis(S, 1 - kSide[k]), 127);
		CHECK_EQ(S.iBits, 0);
		CHECK_EQ(S.iByDown, 1);
		VpadPointerUp(S, L, 0, 1100);
		CHECK_EQ(S.iDual, 0);
		CHECK_EQ(Axis(S, kSide[k]), 127);
		CHECK_EQ(S.iByDown, 0);
		strcpy(gWhere, gTest);
	}
}

static void TestByDownSetByStickDownClearedByLiftAndReset()
{
	// Біт p: вказівник p тримає стік, захоплений саме VpadPointerDown. Дотик по стіку ставить біт, підняття
	// знімає, дотик по кнопці чи порожнечі біт не ставить, рух на стік його не ставить, VpadReset обнуляє.
	// Розкладка: лівий стік (150,100) і правий (450,100), R=80.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 1, 150, 100, 1000);
	CHECK_EQ(S.iByDown, 1 << 1);
	VpadPointerUp(S, L, 1, 1010);
	CHECK_EQ(S.iByDown, 0);
	VpadPointerDown(S, L, 0, 450, 100, 1500);
	CHECK_EQ(S.iByDown, 1 << 0);
	VpadPointerDown(S, L, 2, 150, 100, 2000);
	CHECK_EQ(S.iByDown, (1 << 0) | (1 << 2));
	VpadPointerUp(S, L, 0, 2100);
	CHECK_EQ(S.iByDown, 1 << 2);
	VpadPointerDown(S, L, 3, 320, 300, 2200);
	CHECK_EQ(S.iP[3].iCtl, -1);
	CHECK_EQ(S.iByDown, 1 << 2);
	VpadPointerMove(S, L, 3, 450, 100, 2210);
	CHECK_EQ(S.iP[3].iCtl, -1);
	CHECK_EQ(S.iByDown, 1 << 2);
	VpadReset(S);
	CHECK_EQ(S.iByDown, 0);
}

static void TestByDownIgnoresInferenceAndTakeover()
{
	// Стік, захоплений виведенням другого пальця, біт не ставить; після перехоплення пальцем 0 цей стік
	// теж без біта. Розкладка LayoutInferStick: кнопка Triangle (300,100) і лівий стік (100,100).
	TVpadLayout L;
	TVpadState S;
	LayoutInferStick(L);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 300, 100, 1000);
	CHECK_EQ(S.iByDown, 0);
	VpadPointerMove(S, L, 0, 220, 100, 1060);
	CHECK_EQ(S.iDual, 1);
	CHECK_EQ(S.iByDown, 0);
	VpadPointerMove(S, L, 0, 143, 100, 1070);
	CHECK_EQ(S.iDual, 0);
	CHECK_EQ(S.iP[0].iCtl, 1);
	CHECK_EQ(S.iByDown, 0);
	VpadPointerUp(S, L, 0, 1100);
	CHECK_EQ(S.iByDown, 0);
}

static void TestDefaultOptionStickHasNoEffectWhereOptionSticksIsInEffect()
{
	// EVpadOptStick не діє там, де діє EVpadOptSticks (ландшафт, включно з квадратом і меншим масштабом):
	// розкладка з обома опціями повністю збігається з розкладкою лише з EVpadOptSticks. У портреті
	// EVpadOptSticks ігнорується, тож розкладка з обома опціями збігається з розкладкою з EVpadOptStick.
	static const int W[3] = { 640, 640, 560 };
	static const int H[3] = { 360, 640, 360 };
	int k;
	for (k = 0; k < 3; ++k) {
		TVpadLayout A;
		TVpadLayout B;
		int ra;
		int rb;
		memset(&A, 0, sizeof A);
		memset(&B, 0, sizeof B);
		sprintf(gWhere, "%s screen %dx%d", gTest, W[k], H[k]);
		ra = VpadDefaultLayout(A, W[k], H[k], 138, 100, EVpadOptStick | EVpadOptSticks);
		rb = VpadDefaultLayout(B, W[k], H[k], 138, 100, EVpadOptSticks);
		CHECK_EQ(ra, rb);
		CHECK_EQ(A.iCount, EVpadSticksCount);
		CHECK(SameAllCtl(A, B));
		CHECK_EQ(A.iStickClickMs, B.iStickClickMs);
		CHECK_EQ(A.iStickFloat, B.iStickFloat);
		CHECK_EQ(A.iDualJumpPx, B.iDualJumpPx);
		strcpy(gWhere, gTest);
	}
	{
		TVpadLayout C;
		TVpadLayout D;
		int rc;
		int rd;
		memset(&C, 0, sizeof C);
		memset(&D, 0, sizeof D);
		sprintf(gWhere, "%s portrait", gTest);
		rc = VpadDefaultLayout(C, 360, 640, 111, 100, EVpadOptStick | EVpadOptSticks);
		rd = VpadDefaultLayout(D, 360, 640, 111, 100, EVpadOptStick);
		CHECK_EQ(rc, 78);
		CHECK_EQ(rd, 78);
		CHECK_EQ(C.iCount, EVpadDefaultCount);
		CHECK_EQ(C.iC[EVpadIxDir].iKind, EVpadKindStick);
		CHECK(SameAllCtl(C, D));
		strcpy(gWhere, gTest);
	}
}


static void TestOptStickDoubleTapPressesL3InDefaultLayout()
{
	// EVpadOptStick без EVpadOptSticks: d-pad заміщено стіком (iId 0). iStickClickMs 300 діє й на нього:
	// дотик у центрі зводить, повторний у вікні клацає L3, через 301 мс не клацає.
	TVpadLayout L;
	TVpadState S;
	int i;
	int idx = -1;
	int cx;
	int cy;
	memset(&L, 0, sizeof L);
	VpadDefaultLayout(L, 640, 360, 138, 100, EVpadOptStick);
	CHECK_EQ(L.iStickClickMs, 300);
	for (i = 0; i < TakenCount(L); ++i) {
		if (L.iC[i].iKind == EVpadKindStick && L.iC[i].iId == 0) idx = i;
	}
	CHECK(idx >= 0);
	if (idx < 0) return;
	cx = L.iC[idx].iCx;
	cy = L.iC[idx].iCy;
	VpadReset(S);
	TapStick(S, L, 0, cx, cy, 900, 1000);
	CHECK_EQ(S.iArmed[0], 1);
	VpadPointerDown(S, L, 0, cx, cy, 1300);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
	VpadReset(S);
	TapStick(S, L, 0, cx, cy, 900, 1000);
	VpadPointerDown(S, L, 0, cx, cy, 1301);
	CHECK_EQ(S.iBits, 0);
}

static void TestResizeLargerThanScreenLeavesPartOutsideAndFitsFails()
{
	// Круг R200 на екрані 320x240: VpadResizeControl центрує його (x: 200 > 319-200, тож n/2 = 160;
	// y: 200 > 239-200, тож n/2 = 120). Бокс -40..360 виходить за екран, VpadLayoutFits дає 0.
	// Круг R50 у тих самих координатах влазить: 1.
	TVpadLayout L;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 160, 120, 40, 0, 1);
	VpadResizeControl(L, 0, 200, 0, 320, 240);
	CHECK_EQ(L.iC[0].iRx, 200);
	CHECK_EQ(L.iC[0].iCx, 160);
	CHECK_EQ(L.iC[0].iCy, 120);
	CHECK_EQ(VpadLayoutFits(L, 320, 240), 0);
	VpadResizeControl(L, 0, 50, 0, 320, 240);
	CHECK_EQ(VpadLayoutFits(L, 320, 240), 1);
}

static void TestMoveLargerThanScreenCentresOnScreenAndFitsFails()
{
	// Круг R200 просимо в (0,0) на екрані 320x240: по кожній осі центр затискається в r..n-1-r, а при
	// r > n-1-r дає n/2, тож (160,120), а не кут. Бокс виходить за екран: VpadLayoutFits дає 0.
	TVpadLayout L;
	LayoutInit(L, 1);
	SetCircCtl(L.iC[0], 160, 120, 200, 0, 1);
	VpadMoveControl(L, 0, 0, 0, 320, 240);
	CHECK_EQ(L.iC[0].iCx, 160);
	CHECK_EQ(L.iC[0].iCy, 120);
	CHECK_EQ(VpadLayoutFits(L, 320, 240), 0);
}

// ---------------------------------------------------------------- зведення: тап від точки опускання

// Лівий стік (100,100) з радіусом R (без iStickFloat), клацання увімкнено.
static void LayoutTapSlop(TVpadLayout &aL, int aR)
{
	LayoutInit(aL, 1);
	SetStickCtl(aL.iC[0], 100, 100, aR, 0, 0);
	aL.iStickClickMs = 300;
}

static void TestSideTapOnNonFloatStickArmsAndRepeatClicksLeft()
{
	// Лівий стік (100,100) R80, iStickFloat 0. Палець опустився за третину радіуса від центру, в (127,100),
	// стояв нерухомо і піднявся: тап від точки опускання, зводить. Повторний дотик у вікні клацає L3 і
	// тільки L3: відхил 27 нижчий за цифровий поріг 64.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 127, 100, 900);
	CHECK(S.iLx != KVpadStickCentre);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[0], 1);
	CHECK_EQ(S.iArmMs[0], 1000);
	VpadPointerDown(S, L, 0, 127, 100, 1100);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
}

static void TestSideTapOnNonFloatStickArmsAndRepeatClicksRight()
{
	// Те саме на правому стіку (450,100): палець у (477,100), тап зводить праву сторону, ліва не чіпається,
	// повторний дотик дає R3 і тільки R3.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 477, 100, 900);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[1], 1);
	CHECK_EQ(S.iArmed[0], 0);
	VpadPointerDown(S, L, 0, 477, 100, 1100);
	CHECK_EQ(S.iBits, 1 << EVpadR3);
}

static void TestSideTapAtPressRimOfStickArms()
{
	// Межа зони натиску: 80*125/100 = 100 від центру, точка (200,100) належить стіку (dist 100 включно).
	// Тап по ній зводить сторону: «тап будь-де на стіку».
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 200, 100, 900);
	CHECK_EQ(S.iP[0].iCtl, 0);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[0], 1);
}

static void TestTapSlopBoundaryAxisAndDiagonalR85()
{
	// Стік R85: r = 85*30/100 = 25 (25.5 округлено вниз). Палець опускається в (110,100), не в центрі, тож
	// відлік тапу — точка опускання. Випадки: (0,0), (13,0) і (25,0) зводять; на межі по діагоналі (15,20) і
	// (24,7) (dx*dx+dy*dy == 625) зводять; за межею (26,0), (16,20) (656) і (25,1) (626) не зводять; (25,4)
	// дає 641 і не зводить: при r = 25.5 без округлення зводив би (641 <= 650.25), тож округлення перевірене.
	static const int kCase[9][3] = { {0, 0, 1}, {13, 0, 1}, {25, 0, 1}, {26, 0, 0}, {15, 20, 1}, {16, 20, 0}, {24, 7, 1}, {25, 1, 0}, {25, 4, 0} };
	int k;
	TVpadLayout L;
	TVpadState S;
	LayoutTapSlop(L, 85);
	for (k = 0; k < 9; ++k) {
		sprintf(gWhere, "%s case %d (dx %d, dy %d)", gTest, k, kCase[k][0], kCase[k][1]);
		VpadReset(S);
		VpadPointerDown(S, L, 0, 110, 100, 900);
		VpadPointerMove(S, L, 0, 110 + kCase[k][0], 100 + kCase[k][1], 950);
		VpadPointerUp(S, L, 0, 1000);
		CHECK_EQ(S.iArmed[0], kCase[k][2]);
	}
	strcpy(gWhere, gTest);
}

static void TestTapSlopIntegerDivisionWithIRx83()
{
	// Стік R83: r = 83*30/100 = 24 (24.9 округлено вниз). Зсув по осі на 24 зводить, на 25 ні.
	TVpadLayout L;
	TVpadState S;
	LayoutTapSlop(L, 83);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 900);
	VpadPointerMove(S, L, 0, 124, 100, 950);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[0], 1);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 900);
	VpadPointerMove(S, L, 0, 125, 100, 950);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[0], 0);
}

static void TestTapSlopBoundaryOnRightStick()
{
	// Правий стік (450,100) R80: r = 24. Зсув на 24 по осі зводить праву сторону, на 25 ні.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 450, 100, 900);
	VpadPointerMove(S, L, 0, 474, 100, 950);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[1], 1);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 450, 100, 900);
	VpadPointerMove(S, L, 0, 475, 100, 950);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[1], 0);
}

static void TestFlickToRimWithoutReturnDoesNotArmLeft()
{
	// Різкий рух до краю без повернення: натискання 900 в (100,100), відхил до (180,100) (dist 80 > r = 24),
	// підняття 1000. Стік не повернувся до точки опускання, сторона не зводиться. Дотик 1100 (у вікні 300 мс)
	// не клацає.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 900);
	VpadPointerMove(S, L, 0, 180, 100, 950);
	CHECK_EQ(S.iLx, 254);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[0], 0);
	VpadPointerDown(S, L, 0, 100, 100, 1100);
	CHECK_EQ(S.iBits, 0);
}

static void TestFlickToRimWithoutReturnDoesNotArmRight()
{
	// Те саме на правому стіку (450,100): відхил до (530,100), підняття 1000, дотик 1100 не клацає.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 450, 100, 900);
	VpadPointerMove(S, L, 0, 530, 100, 950);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[1], 0);
	VpadPointerDown(S, L, 0, 450, 100, 1100);
	CHECK_EQ(S.iBits, 0);
}

static void TestReturnToDownPointBeforeLiftArmsLeft()
{
	// Далекий рух: відхил до (170,100) о 930, повернення до точки опускання (100,100) о 960, підняття 1000.
	// Важить лише остання позиція: вона в точці опускання, сторона зведена о 1000, дотик 1100 клацає L3.
	TVpadLayout L;
	TVpadState S;
	LayoutClick(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 900);
	VpadPointerMove(S, L, 0, 170, 100, 930);
	CHECK(S.iLx != KVpadStickCentre);
	VpadPointerMove(S, L, 0, 100, 100, 960);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[0], 1);
	CHECK_EQ(S.iArmMs[0], 1000);
	VpadPointerDown(S, L, 0, 100, 100, 1100);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
}

static void TestReturnToDownPointBeforeLiftArmsRight()
{
	// Те саме на правому стіку: відхил до (470,100), повернення до (450,100), підняття 1000; дотик 1100 клацає R3.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 450, 100, 900);
	VpadPointerMove(S, L, 0, 470, 100, 930);
	VpadPointerMove(S, L, 0, 450, 100, 960);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[1], 1);
	CHECK_EQ(S.iArmed[0], 0);
	VpadPointerDown(S, L, 0, 450, 100, 1100);
	CHECK_EQ(S.iBits, 1 << EVpadR3);
}

static void TestDeadPctAndFloatDoNotChangeTapArming()
{
	// Тап міряється від точки опускання: мертва зона, центр стіка й iStickFloat на зведення не впливають.
	// Для кожної комбінації iDeadPct {0,18,60} і iStickFloat {0,1}: відхід на r = 24 зводить, на 25 ні.
	static const int kDead[3] = { 0, 18, 60 };
	int d;
	int f;
	TVpadLayout L;
	TVpadState S;
	for (d = 0; d < 3; ++d) {
		for (f = 0; f < 2; ++f) {
			LayoutClick(L, 300);
			L.iDeadPct = kDead[d];
			L.iStickFloat = f;
			sprintf(gWhere, "%s deadPct %d float %d", gTest, kDead[d], f);
			VpadReset(S);
			VpadPointerDown(S, L, 0, 120, 100, 900);
			VpadPointerMove(S, L, 0, 144, 100, 950);
			VpadPointerUp(S, L, 0, 1000);
			CHECK_EQ(S.iArmed[0], 1);
			VpadReset(S);
			VpadPointerDown(S, L, 0, 120, 100, 900);
			VpadPointerMove(S, L, 0, 145, 100, 950);
			VpadPointerUp(S, L, 0, 1000);
			CHECK_EQ(S.iArmed[0], 0);
		}
	}
	strcpy(gWhere, gTest);
}

static void TestSideArmingSurvivesTouchesOfOtherControls()
{
	// Зведення скидає лише захоплення стіка тієї самої сторони, а інші дотики не скидають: ні тап по правому
	// стіку, ні натиск і відпускання кнопки. Лівий стік (100,100), кнопка Circle (300,100) R40, правий стік
	// (500,100); зони натиску не перетинаються (0..200, 250..350, 400..600).
	TVpadLayout L;
	TVpadState S;
	LayoutInit(L, 3);
	L.iStickClickMs = 300;
	SetStickCtl(L.iC[0], 100, 100, 80, 0, 0);
	SetCircCtl(L.iC[1], 300, 100, 40, 0, 1u << EVpadCircle);
	SetStickCtl(L.iC[2], 500, 100, 80, 0, 1);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 100, 100, 900);
	VpadPointerUp(S, L, 0, 1000);
	CHECK_EQ(S.iArmed[0], 1);
	VpadPointerDown(S, L, 1, 300, 100, 1020);
	CHECK_EQ(S.iBits, 1 << EVpadCircle);
	VpadPointerUp(S, L, 1, 1040);
	CHECK_EQ(S.iBits, 0);
	CHECK_EQ(S.iArmed[0], 1);
	CHECK_EQ(S.iArmMs[0], 1000);
	VpadPointerDown(S, L, 2, 500, 100, 1060);
	VpadPointerUp(S, L, 2, 1080);
	CHECK_EQ(S.iArmed[1], 1);
	CHECK_EQ(S.iArmed[0], 1);
	VpadPointerDown(S, L, 0, 100, 100, 1200);
	CHECK_EQ(S.iBits, 1 << EVpadL3);
	CHECK_EQ(S.iArmed[0], 0);
	VpadPointerDown(S, L, 3, 500, 100, 1250);
	CHECK_EQ(S.iBits, (1 << EVpadL3) | (1 << EVpadR3));
}

static void TestHigherPointerAxesWinWhenFirstPointerIsDeflected()
{
	// Два вказівники тримають стіки одного боку лише через виведення; осі боку — вказівника з більшим номером.
	// Перша частина без виведення: один палець на стіку A (центр (100,100), R50) тягнеться до (180,100), dx 80,
	// вісь 254. Друга частина: той самий палець 0 на A, виведений палець 1 стоїть у центрі стіка B (260,100),
	// R50; у TestTwoSticksOfOneSideThroughInferenceAxesBelongToHigherPointer той самий рух дає P2 = (260,100).
	// Осі боку мають бути 127. Правило «перший нецентрований виграє» дало б 254.
	static const int kSide[2] = { 0, 1 };
	int k;
	for (k = 0; k < 2; ++k) {
		TVpadLayout L;
		TVpadState S;
		sprintf(gWhere, "%s side %d", gTest, kSide[k]);
		LayoutInit(L, 1);
		SetStickCtl(L.iC[0], 100, 100, 50, 0, kSide[k]);
		VpadReset(S);
		VpadPointerDown(S, L, 0, 100, 100, 1000);
		VpadPointerMove(S, L, 0, 180, 100, 1050);
		CHECK_EQ(Axis(S, kSide[k]), 254);
		LayoutInit(L, 2);
		SetStickCtl(L.iC[0], 100, 100, 50, 0, kSide[k]);
		SetStickCtl(L.iC[1], 260, 100, 50, 0, kSide[k]);
		L.iDualJumpPx = 40;
		VpadReset(S);
		VpadPointerDown(S, L, 0, 100, 100, 1000);
		VpadPointerMove(S, L, 0, 180, 100, 1050);
		CHECK_EQ(S.iDual, 1);
		CHECK_EQ(S.iP[0].iCtl, 0);
		CHECK_EQ(S.iP[1].iCtl, 1);
		CHECK_EQ(Axis(S, kSide[k]), 127);
	}
	strcpy(gWhere, gTest);
}

static void TestDownPointIsRecordedAndKeptThroughMoves()
{
	// iDownX/iDownY — де палець опустився: задано натисканням і не змінюється рухом. Інших гарантій заголовок
	// не дає (зокрема про нулі після VpadReset), тож їх тут не перевіряють.
	TVpadLayout L;
	TVpadState S;
	LayoutTwoSticks(L, 300);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 120, 95, 900);
	CHECK_EQ(S.iP[0].iDownX, 120);
	CHECK_EQ(S.iP[0].iDownY, 95);
	VpadPointerMove(S, L, 0, 150, 140, 950);
	CHECK_EQ(S.iP[0].iDownX, 120);
	CHECK_EQ(S.iP[0].iDownY, 95);
	VpadPointerDown(S, L, 1, 450, 100, 960);
	CHECK_EQ(S.iP[1].iDownX, 450);
	CHECK_EQ(S.iP[1].iDownY, 100);
	VpadPointerUp(S, L, 0, 1000);
	VpadPointerUp(S, L, 1, 1010);
	VpadReset(S);
	VpadPointerDown(S, L, 0, 130, 90, 1100);
	CHECK_EQ(S.iP[0].iDownX, 130);
	CHECK_EQ(S.iP[0].iDownY, 90);
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "--list") == 0) gListOnly = 1;
	if (argc >= 3 && strcmp(argv[1], "--only") == 0) gOnly = argv[2];
	RUN(TestIsqrtSmallAndLargeValues);
	RUN(TestIsqrtSquaresAndNeighbours);
	RUN(TestDefaultLandscape640x360Scale100);
	RUN(TestDefaultPortrait360x640ScaleFallsTo78);
	RUN(TestDefaultOptionStickReplacesDpad);
	RUN(TestDefaultOptionResistiveDualJump);
	RUN(TestDefaultClampsOfUmPerPxAndScalePct);
	RUN(TestDefaultReturnsZeroWhenNothingFits);
	RUN(TestDefaultOtherScreensFitWhenReturned);
	RUN(TestFitsRuleA_BoxInsideScreen);
	RUN(TestFitsRuleB_OverlapAndSharedGroup);
	RUN(TestFitsRuleC_PortraitTopLimit);
	RUN(TestCaptureNearestByNormalisedDistance);
	RUN(TestCaptureTieGoesToLowerIndex);
	RUN(TestCaptureOutsidePressZoneStaysInertUntilUp);
	RUN(TestButtonPressZoneBoundaryCircleAndRect);
	RUN(TestButtonGlideAcrossSharedGroup);
	RUN(TestButtonLeashKeepsThenReleases);
	RUN(TestButtonDifferentGroupsNoGlide);
	RUN(TestEdgesAccumulateAndTakeClears);
	RUN(TestEdgesSecondPointerAddsOnlyNewBits);
	RUN(TestBitsAreOrOverPointers);
	RUN(TestActionReleaseInsideLeashSetsBit);
	RUN(TestIdOutOfRangeIsIgnored);
	RUN(TestCapturedDpadNotCapturedByOtherPointer);
	RUN(TestDpadFourDirectionsAndDiagonal);
	RUN(TestDpadDiagonalThresholdFromNothing);
	RUN(TestDpadBiasAfterSingleBitRaisesThreshold);
	RUN(TestDpadBiasAfterDiagonalLowersThreshold);
	RUN(TestDpadDeadZoneBoundaryAndHysteresis);
	RUN(TestDpadFourWayBoundaryAndTieGoesHorizontal);
	RUN(TestDpadDiagPctZeroIsFourWayOnly);
	RUN(TestDpadDiagPctBelowTenDisablesDiagonal);
	RUN(TestDpadTableIndexIsIntegerDivisionOfDiagPct);
	RUN(TestDpadDeadPctAboveSixtyTreatedAsSixty);
	RUN(TestDpadDiagPctAboveHundredTreatedAsHundred);
	RUN(TestDpadCentreGivesNothingAndRimIsNoLimit);
	RUN(TestStickDeflectionFormulaAtPoints);
	RUN(TestStickSaturatesAtRimAndAtZeroAnd254);
	RUN(TestStickDeadZoneBoundary);
	RUN(TestStickDigitalThresholdAndDiagonal);
	RUN(TestStickFloatCentreAtTouchPoint);
	RUN(TestStickReleaseCentres);
	RUN(TestStickNotCapturedWhenHeldByOtherPointer);
	RUN(TestDualInferenceActivatesOnJumpToLeashedControl);
	RUN(TestDualNoActivationBelowJumpThreshold);
	RUN(TestDualNoActivationAfterSixtyMs);
	RUN(TestDualNoActivationWithoutCandidate);
	RUN(TestDualIgnoresNonZeroIdCalls);
	RUN(TestDualSecondFingerLeavesReturnsToSingle);
	RUN(TestDualFirstFingerLeavesSecondTakesOver);
	RUN(TestDualMirroredSlotMovesWhenNeitherFingerMatches);
	RUN(TestDualLiftingFirstFingerLiftsSecond);
	RUN(TestDualDpadCandidateEvaluatedAtSecondPointAtOnce);
	RUN(TestDualCandidateSharingCapturedGroupIsExcluded);
	RUN(TestCoordinatesClampedOnEntry);
	RUN(TestNonPositiveRadiusIgnored);
	RUN(TestNegativeControlCountCapturesNothing);
	RUN(TestPadWordIsOrOfPointersAfterFuzzyUse);
	RUN(TestRandomSequenceInvariantsLandscape);
	RUN(TestRandomSequenceInvariantsDualPanel);
	RUN(TestScrollInitConversionsFromMicrometres);
	RUN(TestScrollInitMicrometresBelowTwentyTakenAsTwenty);
	RUN(TestScrollInitHugeMicrometresNotLessThanOne);
	RUN(TestScrollSizesClampedToFourMillion);
	RUN(TestTapWhenReleasedBeforeLongPress);
	RUN(TestLongPressReportedExactlyOnceAt500);
	RUN(TestMotionInsideSlopKeepsTap);
	RUN(TestMoveIgnoredWhenNotPressedOrDragging);
	RUN(TestDragFirstBeyondSlopAnchorsWithoutJump);
	RUN(TestDragOverscrollBelowZeroHalves);
	RUN(TestDragOverscrollAboveMaxHalvesExcess);
	RUN(TestDragFromOverscrollKeepsPositionContinuous);
	RUN(TestReleaseVelocityFromWindowStartsFling);
	RUN(TestReleaseAfterRestingOverHundredMsGivesNoFling);
	RUN(TestReleaseRestingExactlyHundredMsStillFlings);
	RUN(TestReleaseBelowMinFlingStops);
	RUN(TestReleaseAtMinFlingStarts);
	RUN(TestReleaseVelocityClampedToMaxFlingUpward);
	RUN(TestReleaseVelocityClampedToMaxFlingDownwardKeepsSign);
	RUN(TestReleaseOutOfRangeSettlesToNearestEnd);
	RUN(TestReleaseAboveMaxSettlesToMax);
	RUN(TestFlingFirstMillisecondExact);
	RUN(TestFlingDecaysMonotonicallyAndStopsWithinTenSeconds);
	RUN(TestFlingUpperEdgeStopsExactlyOnEdge);
	RUN(TestFlingLowerEdgeStopsExactlyOnEdge);
	RUN(TestDownDuringFlingStopsWithoutTapOrLongPress);
	RUN(TestDownDuringSettlingIsCaught);
	RUN(TestTimeSliceNeverExceedsTwoFiftyMs);
	RUN(TestSettleAnimatedFromBelowAndAbove);
	RUN(TestScrollToInstantClampsAndIgnoresFinger);
	RUN(TestEnsureVisibleAllBranchesFromIdle);
	RUN(TestEnsureVisibleUsesSettlingTargetAsBase);
	RUN(TestEnsureVisibleAnimatedStartsSettling);
	RUN(TestSetExtentClampsIdleAndSetsSizes);
	RUN(TestActiveFlagsByState);
	RUN(TestPosFloorForNegativeOffsets);
	RUN(TestTimeSlicingGivesSameStateAtCommonMoments);
	RUN(TestWrappingClockGivesSameResults);
	RUN(TestWrappingClockLongPressAcrossZero);
	RUN(TestDpadRectShapeWithZeroRyActsAsCircle);
	RUN(TestStickRectShapeWithZeroRyActsAsCircle);
	RUN(TestDpadAndStickShapeIgnoredInLayoutFits);
	RUN(TestShapeIgnoredLockstepOnDefaultLayouts);
	RUN(TestCountBeyondMaxTakenAsMax);
	RUN(TestCountNegativeTakenAsZero);
	RUN(TestDpadNegativeDiagPctTakenAsZero);
	RUN(TestStickNegativeDeadPctTakenAsZero);
	RUN(TestStickDigitalNotPressedWhileCentred);
	RUN(TestDualP2ClampedToCoordinateRangePositive);
	RUN(TestDualMirroredSlotClampedToCoordinateRangeNegative);
	RUN(TestDualCandidatesNormalisedAgainstLeashNotRaw);
	RUN(TestDualStickCandidateEvaluatedAtSecondPoint);
	RUN(TestCaptureRectAgainstCircleByNormalisedDistance);
	RUN(TestFlingStopRoundsToNearestPixelHalfUp);
	RUN(TestVelocityIgnoresSamplesOlderThanWindow);
	RUN(TestSetExtentDuringFlingClampsOnNextMillisecond);
	RUN(TestSetExtentDuringSettleClampsTarget);
	RUN(TestCountBeyondMaxKeepsSixteenthControl);
	RUN(TestRandomSequenceInvariantsStickLayout);
	RUN(TestFourWayPctAboveFourHundredTakenAsFourHundred);
	RUN(TestFourWayPctFourHundredAndOneGiveSameDiagonal);
	RUN(TestFourWayPctNegativeTakenAsZero);
	RUN(TestHitPctAboveFourHundredAndNegativeTakenAsZero);
	RUN(TestLeashPctAboveFourHundredTakenAsFourHundred);
	RUN(TestLeashPctNegativeTakenAsZero);
	RUN(TestStickDigitalPctAboveFourHundredTakenAsFourHundred);
	RUN(TestStickDigitalPctNegativeTakenAsZeroDisables);
	RUN(TestStickDigitalPctOverflowingProductClamped);
	RUN(TestHitAndLeashPctOverflowingProductClamped);
	RUN(TestDefaultZeroSizeScalesDoNotFit9001);
	RUN(TestDefaultZeroSizeScalesDoNotFit20000);
	RUN(TestDefaultReturnedScaleHasAllSizesPositiveSweep);
	RUN(TestFitsIgnoresZeroSizeControls);
	RUN(TestDualRadiusNotLessThanOneAtJumpTwo);
	RUN(TestDualRadiusIsThirdOfJumpAtNine);
	RUN(TestOverscrollSaturatesAtOverPxFromZero);
	RUN(TestOverscrollSaturatesAtOverPxBelowZero);
	RUN(TestOverscrollSaturatesOnLargestListBothSides);
	RUN(TestSetExtentDuringDragClampsToNewOverPxRange);
	RUN(TestRandomScrollOffsetStaysWithinOverPxBounds);
	RUN(TestMinFlingZeroTakenAsOneStillStops);
	RUN(TestFrictionOutOfRangeTakenWithinRange);
	RUN(TestMinFlingAboveFourMillionTakenAsFourMillion);
	RUN(TestMaxFlingNegativeTakenAsZeroReleaseStops);
	RUN(TestMaxFlingAboveFourMillionTakenAsFourMillion);
	RUN(TestEnsureVisibleNegativeTopClampedOnEntry);
	RUN(TestEnsureVisibleBottomNearIntMaxClampedOnEntry);
	RUN(TestEnsureVisibleBothExtremesClampedOnEntry);
	RUN(TestStickFloatOriginAtTouchPoint);
	RUN(TestStickFloatSecondFingerCentredAtLanding);
	RUN(TestFourPointersOnDistinctControlsAllPress);
	RUN(TestSecondPointerOnCapturedDpadStaysInert);
	RUN(TestEdgesCountSharedBitOnlyOnRisingWord);
	RUN(TestEarlierTimeDuringPressIsNoElapsedTime);
	RUN(TestStepEarlierThanLastStepKeepsStepTimeAndState);
	RUN(TestUpEarlierThanDownIsStillTap);
	RUN(TestReleaseEarlierThanNewestSampleStillFlings);
	RUN(TestSecondFingerInferenceEarlierThanLastEventIsInstant);
	RUN(TestWrapAroundTapBoundaryAt500Across0xFFFFFFFF);
	RUN(TestFlingWithMinFlingOneReachesIdleOnLongList);
	RUN(TestFirstFingerLeftTakesOverStickOriginFromSecondFinger);
	RUN(TestDpadDiagonalAtThresholdAndOneBelowInEachHysteresis);
	RUN(TestVelocityWindowIncludesSampleExactlyHundredMsOld);
	RUN(TestSettleStopsBelow128AndStepsFromOneTwentyEight);
	RUN(TestEnsureVisibleBottomAtViewEndHasNoEffectDuringFling);
	RUN(TestDefaultNothingFitsEndsWithNineteenthTry);
	RUN(TestActionIdThirtyOneReportsBitThirtyTwoReportsNothing);
	RUN(TestSpecConstantsOfSticksAndLayout);
	RUN(TestDefaultSticksLandscape640x360Scale100);
	RUN(TestDefaultSticksPortraitIgnoredMatchesNoOption);
	RUN(TestDefaultSticksSquareCountsAsLandscape);
	RUN(TestDefaultSticksOptionStickChoosesDpadPerIndexRule);
	RUN(TestDefaultSticksDoNotFitShrinksScale);
	RUN(TestDefaultLayoutMatchesReferenceAcrossScreensAndPitches);
	RUN(TestFitsMatchesReferenceOnRandomLayouts);
	RUN(TestTwoSticksSideFollowsIdAnyOtherIsLeft);
	RUN(TestRightStickDeflectsOnItsOwnAxesWithoutDpadBits);
	RUN(TestSecondPointerOnHeldLeftStickStaysInert);
	RUN(TestSecondPointerCapturesRightWhenLeftHeldAndZonesOverlap);
	RUN(TestDpadAndBothSticksWorkTogether);
	RUN(TestBothSticksDeflectIndependentlyAndCentreOnRelease);
	RUN(TestVpadResetCentresBothSticksAndClearsPending);
	RUN(TestStickTapLimitIsInclusiveAt250Ms);
	RUN(TestStickClickWindowIsInclusiveAtIStickClickMs);
	RUN(TestStickClickPressesL3AtDownAndEdgeOnce);
	RUN(TestStickClickAddsDigitalBitsOfTheStick);
	RUN(TestStickClickWindowZeroDisablesAndIsTakenWithinRange);
	RUN(TestStickClickOnRightStickPressesR3);
	RUN(TestStickIdOtherThanOneClicksAsLeft);
	RUN(TestClickTouchDisarmsItsSideAtLift);
	RUN(TestLongTouchDisarmsItsSide);
	RUN(TestTapArmsOnlyItsOwnSideAndWindowIsPerSide);
	RUN(TestCaptureOfOneSideLeavesOtherSideArmed);
	RUN(TestVpadResetClearsStickArming);
	RUN(TestStickClockGoingBackCountsAsNoElapsedTime);
	RUN(TestStickClickAndTapAcrossClockWrap);
	RUN(TestInferredStickNeverClicksAndLeavesArmingAlone);
	RUN(TestInferredStickTakenOverByFirstFingerNeverArms);
	RUN(TestTakeoverDropsFirstFingerSideAndNeverArms);
	RUN(TestRandomSequenceTwoSticksLandscape);
	RUN(TestRandomSequenceTwoSticksDualPanel);
	RUN(TestControlAtCentreOfEachControlOfSticksLayout);
	RUN(TestControlAtOutsideAllZonesIsMinusOne);
	RUN(TestControlAtNearestByNormalisedDistanceAcrossShapes);
	RUN(TestControlAtTieGoesToLowerIndex);
	RUN(TestControlAtCountAndSizeRules);
	RUN(TestControlAtClampsPointLikePointer);
	RUN(TestMoveControlFreeAndAxisClamps);
	RUN(TestMoveControlRadiusLargerThanRangeGivesHalfScreen);
	RUN(TestMoveControlScreenBelowOneTakenAsOne);
	RUN(TestMoveControlIgnoredForBadIndexAndNoSize);
	RUN(TestResizeControlCircleKeepsRyAndCentreInside);
	RUN(TestResizeControlRectSizesAndCentre);
	RUN(TestResizeControlClampsToMinAndMax);
	RUN(TestResizeControlPullsCentreIntoScreen);
	RUN(TestResizeControlIgnoredForBadIndexAndNoSize);
	RUN(TestResizeStickKeepsRy);
	RUN(TestSameSideSecondStickStaysInertWhileFirstHeld);
	RUN(TestSameSideStickSkippedSoButtonUnderPointIsCaptured);
	RUN(TestOppositeSideStickCapturedWhileOtherSideHeld);
	RUN(TestTakeoverKeepsArmedOtherSideAndDoesNotArmDroppedOne);
	RUN(TestTakeoverAfterRightClickPressesNoR3);
	RUN(TestTwoSticksOfOneSideThroughInferenceAxesBelongToHigherPointer);
	RUN(TestByDownSetByStickDownClearedByLiftAndReset);
	RUN(TestByDownIgnoresInferenceAndTakeover);
	RUN(TestDefaultOptionStickHasNoEffectWhereOptionSticksIsInEffect);
	RUN(TestOptStickDoubleTapPressesL3InDefaultLayout);
	RUN(TestResizeLargerThanScreenLeavesPartOutsideAndFitsFails);
	RUN(TestMoveLargerThanScreenCentresOnScreenAndFitsFails);
	RUN(TestSideTapOnNonFloatStickArmsAndRepeatClicksLeft);
	RUN(TestSideTapOnNonFloatStickArmsAndRepeatClicksRight);
	RUN(TestSideTapAtPressRimOfStickArms);
	RUN(TestTapSlopBoundaryAxisAndDiagonalR85);
	RUN(TestTapSlopIntegerDivisionWithIRx83);
	RUN(TestTapSlopBoundaryOnRightStick);
	RUN(TestFlickToRimWithoutReturnDoesNotArmLeft);
	RUN(TestFlickToRimWithoutReturnDoesNotArmRight);
	RUN(TestReturnToDownPointBeforeLiftArmsLeft);
	RUN(TestReturnToDownPointBeforeLiftArmsRight);
	RUN(TestDeadPctAndFloatDoNotChangeTapArming);
	RUN(TestSideArmingSurvivesTouchesOfOtherControls);
	RUN(TestHigherPointerAxesWinWhenFirstPointerIsDeflected);
	RUN(TestDownPointIsRecordedAndKeptThroughMoves);
	if (gListOnly) return 0;
	printf("touchcore: %d checks, %d failed\n", gChecks, gFails);
	return gFails == 0 ? 0 : 1;
}
