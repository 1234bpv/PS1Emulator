// Перевірки cheatfile.h за його специфікацією: спільні текстові правила, три формати,
// перелік і перемикання читів, переповнення буфера, стійкість до випадкових байтів.
// Очікувані значення виведено з тексту заголовка вручну; розрахунок наведено біля тестів.
// У C-рядках керівні байти записано восьмковими ескейпами (\001, \200): шістнадцятковий
// ескейп поглинув би сусідні букви. Константи специфікації (63, 4194304) вписано в тести
// цифрами, а не взято з заголовка, щоб зміна заголовка була помітна.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "cheatfile.h"

static int gChecks = 0;
static int gFails = 0;
static char gWhere[200] = "";
static const char *gTest = "";
static const char *gOnly = 0;
static int gListOnly = 0;

static const int KBigCap = 65536;
static const int KGuard = 32;
static const unsigned char KCanary = 0xA5;
static const unsigned char KOutFill = 0x5A;

// Видимий вигляд байтів для повідомлень про помилку.
static std::string Esc(const std::string &aS)
{
	std::string r;
	int i;
	for (i = 0; i < (int)aS.size(); ++i) {
		unsigned char c = (unsigned char)aS[i];
		char b[8];
		if (c == '\n') r += "\\n";
		else if (c < 0x20 || c >= 0x7F) { sprintf(b, "\\%03o", (unsigned)c); r += b; }
		else r += (char)c;
	}
	return r;
}

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

#define CHECK_STR(actual, expected) \
	do { \
		std::string a_ = (actual); \
		std::string e_ = (expected); \
		++gChecks; \
		if (a_ != e_) { \
			++gFails; \
			printf("FAIL [%s] %s:%d: %s\n\texpected \"%s\"\n\tgot      \"%s\"\n", \
				gWhere, __FILE__, __LINE__, #actual, Esc(e_).c_str(), Esc(a_).c_str()); \
		} \
	} while (0)

static void ResetWhere()
{
	strcpy(gWhere, gTest);
}

static void SetWhere(const char *aLabel)
{
	strcpy(gWhere, gTest);
	strncat(gWhere, " | ", 4);
	strncat(gWhere, aLabel, 100);
}

static void Run(const char *aName, void (*aFn)())
{
	gTest = aName;
	ResetWhere();
	aFn();
}

#define RUN(fn) \
	do { \
		if (gListOnly) printf("TEST %s\n", #fn); \
		else if (gOnly == 0 || strcmp(gOnly, #fn) == 0) Run(#fn, fn); \
	} while (0)

// ---------------------------------------------------------------- допоміжні

static TCheatStats Unset()
{
	TCheatStats s;
	s.iCheats = -999;
	s.iEnabled = -999;
	s.iCodes = -999;
	s.iBadLines = -999;
	s.iDropped = -999;
	s.iGames = -999;
	return s;
}

struct Res
{
	int rc;
	std::string out;
	TCheatStats st;
};

// Перетворення з буфером достатнього розміру. aSer == 0 — нульовий вказівник серійного номера.
static Res ConvSer(const std::string &aText, int aFormat, const char *aSer)
{
	Res r;
	std::vector<char> buf(KBigCap, 0);
	int serLen = aSer ? (int)strlen(aSer) : 0;
	r.st = Unset();
	r.rc = CheatConvert(aText.data(), (int)aText.size(), aFormat, aSer, serLen, &buf[0], KBigCap, &r.st);
	if (r.rc > 0 && r.rc <= KBigCap) r.out.assign(&buf[0], (size_t)r.rc);
	return r;
}

static Res Conv(const std::string &aText, int aFormat)
{
	return ConvSer(aText, aFormat, 0);
}

// Перетворення у вихід aCap байтів між двома зонами канарок. aOk = 0, якщо будь-який байт
// поза виходом змінився (запис за aCap або перед початком виходу). aBytes — записане, якщо
// 0 < rc <= aCap; інакше порожнє.
static int ConvGuarded(const std::string &aText, int aFormat, const char *aSer, int aCap,
	TCheatStats *aSt, std::string *aBytes, int *aOk)
{
	int n = aCap > 0 ? aCap : 0;
	std::vector<char> mem(2 * KGuard + n, (char)KCanary);
	int i;
	for (i = 0; i < n; ++i) mem[KGuard + i] = (char)KOutFill;
	int serLen = aSer ? (int)strlen(aSer) : 0;
	int rc = CheatConvert(aText.data(), (int)aText.size(), aFormat, aSer, serLen,
		&mem[KGuard], aCap, aSt);
	*aOk = 1;
	for (i = 0; i < (int)mem.size(); ++i) {
		int inside = i >= KGuard && i < KGuard + n;
		if (!inside && mem[i] != (char)KCanary) *aOk = 0;
	}
	aBytes->clear();
	if (rc > 0 && rc <= n) aBytes->assign(&mem[KGuard], (size_t)rc);
	return rc;
}

static void CheckStats(const TCheatStats &aSt, int aCheats, int aEnabled, int aCodes,
	int aBad, int aDropped, int aGames)
{
	CHECK_EQ(aSt.iCheats, aCheats);
	CHECK_EQ(aSt.iEnabled, aEnabled);
	CHECK_EQ(aSt.iCodes, aCodes);
	CHECK_EQ(aSt.iBadLines, aBad);
	CHECK_EQ(aSt.iDropped, aDropped);
	CHECK_EQ(aSt.iGames, aGames);
}

// Перетворення з перевіркою виводу, лічильників і того, що rc дорівнює довжині виводу.
static void CheckConv(const char *aLabel, const std::string &aText, int aFormat, const char *aSer,
	const std::string &aOut, int aCheats, int aEnabled, int aCodes, int aBad, int aDropped, int aGames)
{
	Res r = ConvSer(aText, aFormat, aSer);
	SetWhere(aLabel);
	CHECK_EQ(r.rc, (int)aOut.size());
	CHECK_STR(r.out, aOut);
	CheckStats(r.st, aCheats, aEnabled, aCodes, aBad, aDropped, aGames);
	ResetWhere();
}

static int EqS(const char *aA, const char *aB)
{
	int la = aA ? (int)strlen(aA) : 0;
	int lb = aB ? (int)strlen(aB) : 0;
	return CheatSerialEqual(aA, la, aB, lb) != 0;
}

// Рядок коду в заголовку [h]: має перетворитися на aExpect і бути один раз зарахований.
static void CheckCodeAccepted(const char *aLine, const char *aExpect)
{
	SetWhere(aLine);
	Res r = Conv(std::string("[h]\n") + aLine + "\n", 1);
	CHECK_STR(r.out, std::string("[h]\n") + aExpect + "\n");
	CheckStats(r.st, 1, 0, 1, 0, 0, 0);
	ResetWhere();
}

// Рядок, що не є кодом: після заголовка дає одну погану рядок і нічого не записує.
static void CheckCodeRejected(const char *aLine)
{
	SetWhere(aLine);
	Res r = Conv(std::string("[h]\n") + aLine + "\n", 1);
	CHECK_STR(r.out, std::string(""));
	CheckStats(r.st, 0, 0, 0, 1, 0, 0);
	ResetWhere();
}

// Ім'я за правилом Name: aSource — текст між '[' і ']' заголовка Pcsx.
static void CheckName(const std::string &aSource, int aEnabled, const std::string &aName)
{
	SetWhere(Esc(aSource).c_str());
	Res r = Conv("[" + aSource + "]\n00000001 0001\n", 1);
	std::string want = std::string("[") + (aEnabled ? "*" : "") + aName + "]\n00000001 0001\n";
	CHECK_STR(r.out, want);
	CheckStats(r.st, 1, aEnabled, 1, 0, 0, 0);
	ResetWhere();
}

// ---------------------------------------------------------------- константи й перелічення

static void TestConstantsMatchSpec()
{
	CHECK_EQ(KCheatNameMax, 63);
	CHECK_EQ(KCheatTextMax, 4194304);
}

static void TestFormatValuesMatchSpec()
{
	CHECK_EQ(ECheatFmtUnknown, 0);
	CHECK_EQ(ECheatFmtPcsx, 1);
	CHECK_EQ(ECheatFmtDollar, 2);
	CHECK_EQ(ECheatFmtCw, 3);
}

// ---------------------------------------------------------------- Lines

// Розбиття за "\r\n", "\n" і одинокому "\r"; останній рядок без кінця теж береться.
static void TestLinesSplitAtCrLfLfCr()
{
	CheckConv("CRLF", "[a]\r\n00000001 0001\r\n", 1, 0, "[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	CheckConv("LF", "[a]\n00000001 0001\n", 1, 0, "[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	CheckConv("lone CR", "[a]\r00000001 0001\r", 1, 0, "[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	// "\r" і "\r\n" підряд: два розриви, між ними порожній рядок.
	CheckConv("CR then CRLF", "[a]\r\r\n00000001 0001", 1, 0, "[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	CheckConv("mixed with empty lines", "[a]\r\n\r00000001 0001\n\r\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
}

static void TestLinesLastLineWithoutTerminator()
{
	CheckConv("no final break", "[a]\n00000001 0001", 1, 0, "[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
}

// BOM EF BB BF пропускається лише на самому початку тексту, і лише один раз.
static void TestLinesBomSkippedOnlyAtStart()
{
	CheckConv("BOM at start", "\357\273\277[a]\n00000001 0001\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	// BOM у середині: рядок починається з байтів BOM, тож це не код і не заголовок.
	CheckConv("BOM mid-text", "[a]\n00000001 0001\n\357\273\27700000002 0002\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 1, 0, 0);
	// Другий BOM не є "самим початком": перший рядок "\357\273\277[a]" не заголовок.
	CheckConv("second BOM", "\357\273\277\357\273\277[a]\n00000001 0001\n", 1, 0,
		"", 0, 0, 0, 1, 1, 0);
	CheckConv("BOM only", "\357\273\277", 1, 0, "", 0, 0, 0, 0, 0, 0);
}

static void TestLinesTrimSpacesAndTabs()
{
	CheckConv("trim", "  \t[ a ]\t  \n\t 00000001 \t 0001 \t\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
}

static void TestLinesBlankLinesIgnored()
{
	CheckConv("blank lines", "\n \t \n[a]\n\n\r\n00000001 0001\n\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
}

// ---------------------------------------------------------------- Code

// Допустимі записи коду. Ліва колонка — рядок, права — нормований вигляд: адреса з 8 цифр,
// пробіл, значення з 4 цифр, верхній регістр. "0x" дозволено перед адресою і значенням.
static void TestCodeAcceptedShapes()
{
	CheckCodeAccepted("00000001 0001", "00000001 0001");
	CheckCodeAccepted("1 2", "00000001 0002");
	CheckCodeAccepted("0x1234abcd 0xABCD", "1234ABCD ABCD");
	CheckCodeAccepted("0X0 0X0", "00000000 0000");
	CheckCodeAccepted("ffffffff ffff", "FFFFFFFF FFFF");
	CheckCodeAccepted("0xFFFFFFFF 0xffff", "FFFFFFFF FFFF");
	CheckCodeAccepted("12345678 1", "12345678 0001");
	CheckCodeAccepted("1234\t\t5678", "00001234 5678");
	CheckCodeAccepted("1234    \t  5678", "00001234 5678");
	CheckCodeAccepted("  1234 5678  ", "00001234 5678");
	CheckCodeAccepted("1234 5678\t", "00001234 5678");
	CheckCodeAccepted("0 0000", "00000000 0000");
	CheckCodeAccepted("1234 0x12", "00001234 0012");
	CheckCodeAccepted("0x1 1234", "00000001 1234");
	CheckCodeAccepted("0x12345678 0x0", "12345678 0000");
	CheckCodeAccepted("00001234 00AB", "00001234 00AB");
	CheckCodeAccepted("1234 0x0012", "00001234 0012");
}

// Недопустимі рядки: кожен дає рівно одну погану рядок і жодного коду.
static void TestCodeRejectedShapes()
{
	CheckCodeRejected("1234");
	CheckCodeRejected("1234 ");
	CheckCodeRejected("00000001");
	CheckCodeRejected("0x");
	CheckCodeRejected("123456789 1234");
	CheckCodeRejected("0x123456789 1234");
	CheckCodeRejected("1234 12345");
	CheckCodeRejected("1234 0x12345");
	CheckCodeRejected("0x 1234");
	CheckCodeRejected("0x 0x");
	CheckCodeRejected("1234 0x");
	CheckCodeRejected("1234 ZZZZ");
	CheckCodeRejected("1234 5678 9");
	CheckCodeRejected("1234 5678 ;c");
	CheckCodeRejected("1234,5678");
	CheckCodeRejected("1234 5678x");
	CheckCodeRejected("0x0x12 1234");
	CheckCodeRejected("x1234 1234");
	CheckCodeRejected("-1234 5678");
	CheckCodeRejected("1234 -5678");
	CheckCodeRejected("12 34 56");
}

// ---------------------------------------------------------------- Name

// Керівні байти прибираються (до видалення зірочок), а не замінюються пробілом.
static void TestNameControlBytesRemoved()
{
	CheckName("a\001b\177c d", 0, "abc d");
	CheckName("\037x\037", 0, "x");
	CheckName("a\001 \002b", 0, "a b");
}

static void TestNameTabRemovedNotReplaced()
{
	CheckName("a\tb", 0, "ab");
	CheckName("\ta\t", 0, "a");
}

// '[' стає '(' і ']' стає ')'. Для "[a]b]" ім'я береться до ОСТАННЬОЇ ']'.
static void TestNameBracketsReplaced()
{
	CheckName("a[b", 0, "a(b");
	CheckName("a]b", 0, "a)b");
	CheckName("(x)", 0, "(x)");
	CheckName("[]", 0, "()");
}

// Зірочки: ведучі видаляються; ім'я починається з '*' — заголовок увімкнений.
// "*\001*": за переліком правила керівні байти прибираються раніше зірочок, тож
// після них лишаються дві ведучі зірочки і ім'я порожнє. Прочитання за переліком.
static void TestNameLeadingStarsRemoved()
{
	CheckName("**a*b*", 1, "a*b*");
	CheckName(" * a", 1, "a");
	CheckName("x*", 0, "x*");
	CheckName("*", 1, "Cheat");
	CheckName("*\001*", 1, "Cheat");
}

// Обрізання пробілів після видалення керівних байтів.
static void TestNameTrimAfterRemoval()
{
	CheckName("\001 a", 0, "a");
	CheckName(" \001", 0, "Cheat");
	CheckName("a \001", 0, "a");
}

// Порожній результат замінюється на "Cheat".
static void TestNameEmptyBecomesCheat()
{
	CheckName("", 0, "Cheat");
	CheckName("   ", 0, "Cheat");
	CheckName("\001\002", 0, "Cheat");
	CheckName("**", 1, "Cheat");
}

// Обрізання до 63 байтів (після всіх видалень), потім обрізання пробілів на межі.
static void TestNameCutAt63()
{
	CheckName(std::string(63, 'a'), 0, std::string(63, 'a'));
	CheckName(std::string(64, 'a'), 0, std::string(63, 'a'));
	// 62 a + " bc" = 65 байтів; межа 63 дає 62 a і пробіл, пробіл обрізається.
	CheckName(std::string(62, 'a') + " bc", 0, std::string(62, 'a'));
	// 62 a + " b" = 64 байти; межа 63 дає 62 a і пробіл.
	CheckName(std::string(62, 'a') + " b", 0, std::string(62, 'a'));
	// Пробіл всередині, що не на межі, лишається.
	CheckName(std::string(60, 'a') + " " + std::string(10, 'b'), 0,
		std::string(60, 'a') + " bb");
	// 61 a + "  b" = 64 байти; межа дає 61 a і два пробіли, обрізаються обидва.
	CheckName(std::string(61, 'a') + "  b", 0, std::string(61, 'a'));
	// Керівні байти, прибрані до обрізання: 40 пар "a\001" дають 40 байтів, усе лишається.
	std::string src;
	int i;
	for (i = 0; i < 40; ++i) src += "a\001";
	CheckName(src, 0, std::string(40, 'a'));
}

// Обрізання байтами: багатобайтовий символ, розрізаний на межі, дає неповну послідовність.
static void TestNameCutMidMultibyte()
{
	CheckName(std::string(62, 'a') + "\303\251", 0, std::string(62, 'a') + "\303");
	CheckName(std::string(63, 'a') + "\303\251", 0, std::string(63, 'a'));
	CheckName("\303\251x", 0, "\303\251x");
}

// Байти 0x80 і вище проходять без змін; 0x7F прибирається, 0xFF лишається.
static void TestNameHighBytesPass()
{
	CheckName("\200\001\201", 0, "\200\201");
	CheckName("\377", 0, "\377");
}

// ---------------------------------------------------------------- Serial

// Рівність: після прибирання не-літер/цифр і без урахування регістру. Серійний номер без
// жодної літери чи цифри не дорівнює нічому, навіть собі.
static void TestSerialEqualRules()
{
	CHECK_EQ(EqS("SLUS-012.22", "slus_01222"), 1);
	CHECK_EQ(EqS("SLUS01222", "SLUS01222"), 1);
	CHECK_EQ(EqS("SLUS01222", "SLUS01223"), 0);
	CHECK_EQ(EqS("SLUS0122", "SLUS01222"), 0);
	CHECK_EQ(EqS("SLUS 012 22", "slus-01222"), 1);
	CHECK_EQ(EqS("SLUS-01222\001", "SLUS01222"), 1);
	CHECK_EQ(EqS("01222", "0-1-2-2-2"), 1);
	CHECK_EQ(EqS("SLUS", "slus"), 1);
	CHECK_EQ(EqS("slus01222", "SLUS-01222"), 1);
	CHECK_EQ(EqS("SLUS-01222", "slus01222"), 1);
	CHECK_EQ(EqS("---", "---"), 0);
	CHECK_EQ(EqS("-", "SLUS"), 0);
	CHECK_EQ(EqS("", ""), 0);
	CHECK_EQ(EqS("", "SLUS"), 0);
	// Байти не-ASCII трактовано як не літери: "letter" у заголовку не уточнено.
	CHECK_EQ(EqS("\303\251", "\303\251"), 0);
}

// Нульовий вказівник — порожній серійний номер, який не дорівнює нічому.
static void TestSerialNullIsEmpty()
{
	CHECK_EQ(CheatSerialEqual(0, 0, 0, 0), 0);
	CHECK_EQ(EqS(0, "SLUS"), 0);
	CHECK_EQ(EqS("SLUS", 0), 0);
	CHECK_EQ(EqS(0, 0), 0);
}

// ---------------------------------------------------------------- CheatDetect

static int DetectS(const std::string &aText)
{
	return CheatDetect(aText.data(), (int)aText.size());
}

static void TestDetectPcsxFirstLine()
{
	CHECK_EQ(DetectS("[a]"), ECheatFmtPcsx);
	CHECK_EQ(DetectS("   \t[a]"), ECheatFmtPcsx);
	CHECK_EQ(DetectS("["), ECheatFmtPcsx);
	CHECK_EQ(DetectS("x\r[a]"), ECheatFmtPcsx);
}

static void TestDetectDollarFirstLine()
{
	CHECK_EQ(DetectS("$00000001 0001"), ECheatFmtDollar);
	CHECK_EQ(DetectS("$"), ECheatFmtDollar);
	CHECK_EQ(DetectS("  $x"), ECheatFmtDollar);
}

// Теги Cw: регістр літери байдужий. "_Sx" визначається за префіксом "_S" (буквальне
// "starts with"), хоча як тег такий рядок не дійсний; прочитання обрано навмисно.
static void TestDetectCwTags()
{
	CHECK_EQ(DetectS("_S SLUS-01222"), ECheatFmtCw);
	CHECK_EQ(DetectS("_s x"), ECheatFmtCw);
	CHECK_EQ(DetectS("_G title"), ECheatFmtCw);
	CHECK_EQ(DetectS("_g"), ECheatFmtCw);
	CHECK_EQ(DetectS("_C1 name"), ECheatFmtCw);
	CHECK_EQ(DetectS("_c0 x"), ECheatFmtCw);
	CHECK_EQ(DetectS("_L 00000001 0001"), ECheatFmtCw);
	CHECK_EQ(DetectS("_l"), ECheatFmtCw);
	CHECK_EQ(DetectS("_Sx"), ECheatFmtCw);
}

// Рядки коментарів будь-якого стилю пропускаються, перший значущий визначає формат.
static void TestDetectSkipsCommentsAndBlanks()
{
	CHECK_EQ(DetectS("# [x]\n; $x\n// _S\n[a]\n"), ECheatFmtPcsx);
	CHECK_EQ(DetectS("\"x\n\n   \n_S A"), ECheatFmtCw);
	// "/ [x]" починається з одного '/': це не коментар, але й не тригер, тож пропускається.
	CHECK_EQ(DetectS("/ [x]\n$1 2"), ECheatFmtDollar);
	CHECK_EQ(DetectS("#name\n$00000001 0001"), ECheatFmtDollar);
	CHECK_EQ(DetectS("#name\n[a]"), ECheatFmtPcsx);
	CHECK_EQ(DetectS("; c\n//c\n#d\n_G t"), ECheatFmtCw);
}

static void TestDetectUnknown()
{
	CHECK_EQ(DetectS(""), ECheatFmtUnknown);
	CHECK_EQ(DetectS("#name\n00000001 0001\n"), ECheatFmtUnknown);
	CHECK_EQ(DetectS("00000001 0001"), ECheatFmtUnknown);
	CHECK_EQ(DetectS("\n\r\n  \t\n"), ECheatFmtUnknown);
	CHECK_EQ(DetectS("__S x"), ECheatFmtUnknown);
	CHECK_EQ(DetectS("_ S"), ECheatFmtUnknown);
	CHECK_EQ(DetectS("_"), ECheatFmtUnknown);
	CHECK_EQ(DetectS("x[a]"), ECheatFmtUnknown);
	CHECK_EQ(DetectS("#\n;\n//"), ECheatFmtUnknown);
	// Один BOM пропускається; другий BOM — частина рядка "\357\273\277[a]", не тригер.
	CHECK_EQ(DetectS("\357\273\277"), ECheatFmtUnknown);
	CHECK_EQ(DetectS("\357\273\277\357\273\277[a]"), ECheatFmtUnknown);
	// Довжина 0 при непорожньому буфері: текст порожній.
	CHECK_EQ(CheatDetect("[a]", 0), ECheatFmtUnknown);
}

// Тригер визначає перший рядок, що з нього починається; BOM на початку не заважає.
static void TestDetectFirstTriggerWins()
{
	CHECK_EQ(DetectS("_S A\n[a]"), ECheatFmtCw);
	CHECK_EQ(DetectS("$1 2\n[a]"), ECheatFmtDollar);
	CHECK_EQ(DetectS("[a]\n_S A"), ECheatFmtPcsx);
	CHECK_EQ(DetectS("#x\n_L 00000001 0001\n$1 2"), ECheatFmtCw);
	CHECK_EQ(DetectS("_X\n[a]"), ECheatFmtPcsx);
	CHECK_EQ(DetectS("\357\273\277[a]"), ECheatFmtPcsx);
}

// ---------------------------------------------------------------- Pcsx

static void TestPcsxBasicOutput()
{
	CheckConv("basic", "[alpha]\n00000001 0001\n00000002 0002\n[*beta]\n0000000a 000a\n", 1, 0,
		"[alpha]\n00000001 0001\n00000002 0002\n[*beta]\n0000000A 000A\n", 2, 1, 3, 0, 0, 0);
}

// Коментарі: '#', ';', '/' і '"' — будь-який рядок, що починається з них.
static void TestPcsxCommentsSkipped()
{
	CheckConv("comments", "# c\n; c\n/ c\n\" c\n//c\n[a]\n00000001 0001\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
}

// Заголовок без ']' — погана рядок; коди після нього належать попередньому читу.
static void TestPcsxUnclosedBracketIsBad()
{
	CheckConv("unclosed after cheat", "[a]\n00000001 0001\n[broken\n00000002 0002\n", 1, 0,
		"[a]\n00000001 0001\n00000002 0002\n", 1, 0, 2, 1, 0, 0);
	// Без попереднього чита: "[broken" погана, код після нього відкинуто.
	CheckConv("unclosed before any cheat", "[broken\n00000001 0001\n", 1, 0, "", 0, 0, 0, 1, 1, 0);
}

// Код до першого заголовка відкидається (iDropped), не є поганим рядком.
static void TestPcsxCodeBeforeHeaderDropped()
{
	CheckConv("code first", "00000001 0001\n[a]\n00000002 0002\n", 1, 0,
		"[a]\n00000002 0002\n", 1, 0, 1, 0, 1, 0);
}

// Непорожній рядок, що не є кодом, — погана рядок, навіть до першого заголовка.
static void TestPcsxNonCodeIsBad()
{
	CheckConv("non-code", "[a]\nhello\n00000001 0001\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 1, 0, 0);
	CheckConv("non-code before header", "hello\n[a]\n00000001 0001\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 1, 0, 0);
}

// Чит без кодів не записується і не рахується в iCheats; iDropped для нього не зростає.
static void TestPcsxEmptyHeaderNotWritten()
{
	CheckConv("empty first", "[empty]\n[full]\n00000001 0001\n", 1, 0,
		"[full]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	CheckConv("empty last", "[a]\n00000001 0001\n[empty]\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	CheckConv("two empty", "[a]\n[b]\n00000001 0001\n", 1, 0,
		"[b]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
}

// Ім'я береться між '[' і ОСТАННІМ ']' рядка; текст після ']' ігнорується.
static void TestPcsxNameUsesLastBracket()
{
	CheckConv("last bracket", "[a]b]\n00000001 0001\n", 1, 0,
		"[a)b]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	CheckConv("trailing text", "[a] trailing\n00000001 0001\n", 1, 0,
		"[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	CheckConv("empty name", "[]\n00000001 0001\n", 1, 0,
		"[Cheat]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
}

// Увімкнений чит: після обрізання пробілів текст у дужках починається з '*'.
static void TestPcsxEnabledMarker()
{
	CheckConv("spaced marker", "[ *x ]\n00000001 0001\n", 1, 0,
		"[*x]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
	CheckConv("star at end", "[x*]\n00000001 0001\n", 1, 0,
		"[x*]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
	CheckConv("leading space", "[ x]\n00000001 0001\n", 1, 0,
		"[x]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
}

static void TestPcsxDuplicateNamesKept()
{
	CheckConv("duplicates", "[a]\n00000001 0001\n[a]\n00000002 0002\n", 1, 0,
		"[a]\n00000001 0001\n[a]\n00000002 0002\n", 2, 0, 2, 0, 0, 0);
}

// Заголовок, що не починається з '[' (стоїть "x[a]"), не заголовок; код після нього відкинуто.
static void TestPcsxBracketNotAtStartIsBad()
{
	CheckConv("x[a]", "x[a]\n00000001 0001\n", 1, 0, "", 0, 0, 0, 1, 1, 0);
}

static void TestPcsxEmptyInputs()
{
	CheckConv("empty text", "", 1, 0, "", 0, 0, 0, 0, 0, 0);
	CheckConv("comments only", "# only\n\n", 1, 0, "", 0, 0, 0, 0, 0, 0);
}

// Кожен лічильник окремо: сценарій з відомим підрахунком.
// 00000009 до заголовка — відкинуто (iDropped=1). "bad" і "[broken" — погані (iBadLines=2).
// [*d] увімкнено, його код 00000003 0003 — iEnabled=1. Записано 3 чити, 3 коди.
static void TestPcsxStatsEachCounter()
{
	CheckConv("counters",
		"00000009 0009\n[a]\n00000001 0001\n[*b]\nbad\n[c]\n00000002 0002\n[*d]\n00000003 0003\n[broken\n",
		1, 0,
		"[a]\n00000001 0001\n[c]\n00000002 0002\n[*d]\n00000003 0003\n",
		3, 1, 3, 2, 1, 0);
}

// ---------------------------------------------------------------- Dollar

// "#" задає ім'я наступним кодам; "$"-коди між двома "#" утворюють один чит; без "#" —
// чит "Cheat". Усі записані чити увімкнені.
static void TestDollarBasic()
{
	CheckConv("basic", "#Alpha\n$00000001 0001\n$00000002 0002\n#Beta\n$0000000a 000a\n", 2, 0,
		"[*Alpha]\n00000001 0001\n00000002 0002\n[*Beta]\n0000000A 000A\n", 2, 2, 3, 0, 0, 0);
}

static void TestDollarCodesBeforeHashUseCheat()
{
	CheckConv("before hash", "$00000001 0001\n#Named\n$00000002 0002\n", 2, 0,
		"[*Cheat]\n00000001 0001\n[*Named]\n00000002 0002\n", 2, 2, 2, 0, 0, 0);
	CheckConv("only dollar", "$00000001 0001\n$00000002 0002\n", 2, 0,
		"[*Cheat]\n00000001 0001\n00000002 0002\n", 1, 1, 2, 0, 0, 0);
}

// "#" без кодів не дає чита і не відкидається як "dropped" (за переліком формату).
static void TestDollarHashWithoutCodesNotWritten()
{
	CheckConv("two hashes", "#A\n#B\n$00000001 0001\n", 2, 0,
		"[*B]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
	CheckConv("trailing hash", "#A\n$00000001 0001\n#C\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
	CheckConv("hashes only", "#A\n#B\n", 2, 0, "", 0, 0, 0, 0, 0, 0);
}

static void TestDollarCodesBetweenHashesOneCheat()
{
	CheckConv("between hashes", "#A\n$00000001 0001\n$00000002 0002\n#B\n$00000003 0003\n$00000004 0004\n",
		2, 0, "[*A]\n00000001 0001\n00000002 0002\n[*B]\n00000003 0003\n00000004 0004\n",
		2, 2, 4, 0, 0, 0);
}

static void TestDollarSameNameKept()
{
	CheckConv("same name", "#A\n$00000001 0001\n#A\n$00000002 0002\n", 2, 0,
		"[*A]\n00000001 0001\n[*A]\n00000002 0002\n", 2, 2, 2, 0, 0, 0);
}

// "$" з рештою без пробілів, що не є кодом, — відкинуто (iDropped), у тому числі "$" сам по собі.
static void TestDollarNoBlankNonCodeDropped()
{
	CheckConv("SAFEMODE", "#A\n$SAFEMODE\n$00000001 0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 0, 1, 0);
	CheckConv("no blank 1234", "#A\n$1234\n$00000001 0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 0, 1, 0);
	CheckConv("lone dollar", "#A\n$\n$00000001 0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 0, 1, 0);
	CheckConv("dollar and blanks", "#A\n$   \n$00000001 0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 0, 1, 0);
}

// "$" з пробілом у решті, що не є кодом, — погана рядок.
static void TestDollarBlankNonCodeIsBad()
{
	CheckConv("SAFE MODE", "#A\n$SAFE MODE\n$00000001 0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 1, 0, 0);
	CheckConv("two bad", "#A\n$SAFE MODE\n$00000002 00001\n$00000001 0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 2, 0, 0);
}

// Рест після '$' обрізається: "$ 00000001 0001" — код. Прочитання за правилом Code.
static void TestDollarRestIsTrimmed()
{
	CheckConv("space after dollar", "#A\n$ 00000001 0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
	CheckConv("tab separator", "#A\n$00000001\t0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
}

// Рядок без '$' що є кодом — відкинуто (iDropped), не погана рядок; некод без '$' — погана.
static void TestDollarPlainLines()
{
	CheckConv("plain code dropped", "#A\n00000001 0001\n$00000002 0002\n", 2, 0,
		"[*A]\n00000002 0002\n", 1, 1, 1, 0, 1, 0);
	CheckConv("plain non-code bad", "#A\nhello\n$00000002 0002\n[x]\n", 2, 0,
		"[*A]\n00000002 0002\n", 1, 1, 1, 2, 0, 0);
}

// Коментарі Dollar: "//" і ';'. Одинокий '/' коментарем не є.
static void TestDollarCommentStyles()
{
	CheckConv("double slash and semicolon", "// c\n; c\n#A\n$00000001 0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
	CheckConv("single slash is bad", "/ c\n#A\n$00000001 0001\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 1, 0, 0);
}

// Правило імені для "#": обрізання, керівні байти, дужки, зірочки, порожнє → "Cheat".
static void TestDollarNameRules()
{
	CheckConv("brackets and control", "#[x]\001y\n$00000001 0001\n", 2, 0,
		"[*(x)y]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
	CheckConv("trimmed", "#   Name  \t \n$00000001 0001\n", 2, 0,
		"[*Name]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
	CheckConv("empty name", "#\n$00000001 0001\n", 2, 0,
		"[*Cheat]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
	CheckConv("leading star", "#*x\n$00000001 0001\n", 2, 0,
		"[*x]\n00000001 0001\n", 1, 1, 1, 0, 0, 0);
}

// Підрахунок змішаного входу: "$FOO" — відкинуто; "$BAR BAZ", "xyz" — погані.
static void TestDollarMixedStats()
{
	CheckConv("mixed", "#A\n$00000001 0001\n$FOO\n$BAR BAZ\nxyz\n", 2, 0,
		"[*A]\n00000001 0001\n", 1, 1, 1, 2, 1, 0);
}

static void TestDollarEmptyInput()
{
	CheckConv("empty", "", 2, 0, "", 0, 0, 0, 0, 0, 0);
}

// ---------------------------------------------------------------- Cw

// _C0 — вимкнений, _C1 — увімкнений; _L приєднує код до поточного чита гри.
static void TestCwBasic()
{
	CheckConv("basic", "_S SLUS-01222\n_G Title\n_C0 Off cheat\n_L 00000001 0001\n"
		"_C1 On cheat\n_L 00000002 0002\n_L 00000003 0003\n", 3, 0,
		"[Off cheat]\n00000001 0001\n[*On cheat]\n00000002 0002\n00000003 0003\n",
		2, 1, 3, 0, 0, 1);
}

// Будь-яка цифра після "_C", крім 0, вмикає чит.
static void TestCwDigitZeroDisabledOthersEnabled()
{
	CheckConv("digits", "_S A\n_C0 a\n_L 00000001 0001\n_C5 b\n_L 00000002 0002\n_C9 c\n_L 00000003 0003\n",
		3, 0, "[a]\n00000001 0001\n[*b]\n00000002 0002\n[*c]\n00000003 0003\n", 3, 2, 3, 0, 0, 1);
}

// Теги без урахування регістру літер; тег має бути за пробілом або кінцем рядка.
static void TestCwTagCaseInsensitive()
{
	CheckConv("lower case", "_s slus\n_g t\n_c5 x\n_l 00000001 0001\n_C1 y\n_L 00000002 0002\n",
		3, 0, "[*x]\n00000001 0001\n[*y]\n00000002 0002\n", 2, 2, 2, 0, 0, 1);
}

// Прочитання, яке обрано (тег — "_S", "_G", "_C"+цифра, "_L", за яким має йти пробіл або
// кінець рядка). "_Gx", "_C1name", "_SLUS-01222" як теги не дійсні: погані рядки.
static void TestCwTagMustBeFollowedByBlank()
{
	// "_Gx" і "_C1name" погані; "_L" без поточного чита відкинуто.
	CheckConv("glued tags", "_S A\n_Gx\n_C1name\n_L 00000001 0001\n", 3, 0,
		"", 0, 0, 0, 2, 1, 1);
	// "_SLUS-01222" всередині гри — погана рядок; "_C1 x" далі працює.
	CheckConv("glued serial in game", "_S A\n_SLUS-01222\n_C1 x\n_L 00000001 0001\n", 3, 0,
		"[*x]\n00000001 0001\n", 1, 1, 1, 1, 0, 1);
}

// Назва з "_G" ігнорується; "_G" без тексту теж дозволено.
static void TestCwTitleIgnored()
{
	CheckConv("title", "_S A\n_G anything at all\n_C1 x\n_L 00000001 0001\n", 3, 0,
		"[*x]\n00000001 0001\n", 1, 1, 1, 0, 0, 1);
	CheckConv("bare title", "_S A\n_G\n_C1 x\n_L 00000001 0001\n", 3, 0,
		"[*x]\n00000001 0001\n", 1, 1, 1, 0, 0, 1);
}

// "_L" до першого "_C" гри відкидається (iDropped). "_S" скидає поточний чит.
static void TestCwCodeBeforeCheatDropped()
{
	CheckConv("L before C", "_S A\n_L 00000001 0001\n_C1 x\n_L 00000002 0002\n", 3, 0,
		"[*x]\n00000002 0002\n", 1, 1, 1, 0, 1, 1);
	CheckConv("new game resets", "_S A\n_C1 x\n_S B\n_L 00000001 0001\n_C1 y\n_L 00000002 0002\n",
		3, 0, "[*y]\n00000002 0002\n", 1, 1, 1, 0, 1, 2);
}

// "_L" без коду або з не-кодом — погана рядок; код після неї все одно приєднується.
static void TestCwLNotCodeIsBad()
{
	CheckConv("L variants", "_S A\n_C1 x\n_L\n_L hello\n_L 00000001 0001\n", 3, 0,
		"[*x]\n00000001 0001\n", 1, 1, 1, 2, 0, 1);
	CheckConv("bad line keeps cheat", "_S A\n_C1 x\nbad\n_L 00000001 0001\n", 3, 0,
		"[*x]\n00000001 0001\n", 1, 1, 1, 1, 0, 1);
}

// Коментарі Cw: '#', ';' і "//". Одинокий '/' — погана рядок.
static void TestCwCommentStyles()
{
	CheckConv("comments", "# c\n; c\n// c\n_S A\n_C1 x\n_L 00000001 0001\n", 3, 0,
		"[*x]\n00000001 0001\n", 1, 1, 1, 0, 0, 1);
	CheckConv("single slash", "_S A\n/ c\n_C1 x\n_L 00000001 0001\n", 3, 0,
		"[*x]\n00000001 0001\n", 1, 1, 1, 1, 0, 1);
}

// Рядки перед першим "_S" не належать жодній грі: пропускаються без підрахунку.
// Прочитання прийнято для обох варіантів фільтра (з фільтром це сказано прямо).
static void TestCwLinesBeforeFirstSerialSkipped()
{
	CheckConv("orphans", "_C1 orphan\n_L 00000009 0009\nbad\n_S A\n_C1 x\n_L 00000001 0001\n",
		3, 0, "[*x]\n00000001 0001\n", 1, 1, 1, 0, 0, 1);
	CheckConv("orphans with filter", "bad\n_L 00000009 0009\n_S SLUS01222\n_C1 A\n_L 00000001 0001\n",
		3, "SLUS01222", "[*A]\n00000001 0001\n", 1, 1, 1, 0, 0, 1);
}

static void TestCwNoGameNoOutput()
{
	CheckConv("no game", "_C1 x\n_L 00000001 0001\n", 3, 0, "", 0, 0, 0, 0, 0, 0);
}

// Чит без кодів не записується і не рахується; "_C0 y" з кодом пишеться як вимкнений.
static void TestCwCheatWithoutCodesNotWritten()
{
	CheckConv("empty cheat", "_S A\n_C1 x\n_C0 y\n_L 00000001 0001\n_C1 z\n", 3, 0,
		"[y]\n00000001 0001\n", 1, 0, 1, 0, 0, 1);
}

static void TestCwDuplicateNamesKept()
{
	CheckConv("duplicates", "_S A\n_C1 x\n_L 00000001 0001\n_C1 x\n_L 00000002 0002\n", 3, 0,
		"[*x]\n00000001 0001\n[*x]\n00000002 0002\n", 2, 2, 2, 0, 0, 1);
}

// Правило імені для "_C": ім'я — залишок рядка після обрізання, з правилом Name.
static void TestCwCheatNameRules()
{
	CheckConv("name rules",
		"_S A\n_C1 *[a]\t\n_L 00000001 0001\n_C0\n_L 00000002 0002\n_C1   \n_L 00000003 0003\n",
		3, 0, "[*(a)]\n00000001 0001\n[Cheat]\n00000002 0002\n[*Cheat]\n00000003 0003\n",
		3, 2, 3, 0, 0, 1);
}

// "_C10 x": тег "_C1" за ним "0", не пробіл, тож тегом не є (прочитання як у TestCwTagMustBeFollowedByBlank).
// Далі "_L" без поточного чита відкинуто.
static void TestCwTwoDigitCheatTagIsBad()
{
	CheckConv("two digits", "_S A\n_C10 x\n_L 00000001 0001\n", 3, 0, "", 0, 0, 0, 1, 1, 1);
}

// Фільтр серійного номера: збіг за правилом Serial, тобто без пунктуації й регістру.
static void TestCwSerialFilterMatchesIgnoringPunctuation()
{
	std::string t = "_S SLUS-01222\n_C1 A\n_L 00000001 0001\n_S SLUS-99999\n_C1 B\n_L 00000002 0002\n"
		"_S slus_01222\n_C0 C\n_L 00000003 0003\n";
	CheckConv("filter SLUS01222", t, 3, "SLUS01222", "[*A]\n00000001 0001\n[C]\n00000003 0003\n",
		2, 1, 2, 0, 0, 3);
	CheckConv("filter slus-012.22", t, 3, "slus-012.22", "[*A]\n00000001 0001\n[C]\n00000003 0003\n",
		2, 1, 2, 0, 0, 3);
}

// Ігри, що не збіглися з фільтром, не рахуються: їх погані рядки і відкинуті коди теж.
static void TestCwSerialFilterSkipsOtherGamesSilently()
{
	CheckConv("other game skipped",
		"_S OTHER\nbad line\n_L 00000009 0009\n_C1 q\n_L xx\n_S SLUS01222\n_C1 A\n_L 00000001 0001\n",
		3, "SLUS01222", "[*A]\n00000001 0001\n", 1, 1, 1, 0, 0, 2);
}

// Усередині збіглої гри погані рядки і відкинуті коди рахуються.
static void TestCwSerialFilterCountsInsideMatch()
{
	CheckConv("inside match",
		"_S SLUS01222\n_L 00000000 0000\n_C1 A\n_L 00000001 0001\nbad\n",
		3, "SLUS01222", "[*A]\n00000001 0001\n", 1, 1, 1, 1, 1, 1);
}

// Порожній серійний номер гри не збігається з жодним фільтром, навіть порожнім-по-змісту.
static void TestCwSerialFilterEmptyGameNeverMatches()
{
	CheckConv("empty game serial", "_S\n_C1 A\n_L 00000001 0001\n_S SLUS01222\n_C1 B\n_L 00000002 0002\n",
		3, "SLUS01222", "[*B]\n00000002 0002\n", 1, 1, 1, 0, 0, 2);
}

// Фільтр без літер і цифр (нуль, порожній, "-", пробіл): пишуться всі ігри.
static void TestCwNoFilterWritesAllGames()
{
	std::string t = "_S A\n_C1 x\n_L 00000001 0001\n_S B\n_C0 y\n_L 00000002 0002\n"
		"_S\n_C1 z\n_L 00000003 0003\n";
	std::string want = "[*x]\n00000001 0001\n[y]\n00000002 0002\n[*z]\n00000003 0003\n";
	CheckConv("null serial", t, 3, 0, want, 3, 2, 3, 0, 0, 3);
	CheckConv("empty serial", t, 3, "", want, 3, 2, 3, 0, 0, 3);
	CheckConv("dash serial", t, 3, "-", want, 3, 2, 3, 0, 0, 3);
	CheckConv("space serial", t, 3, " ", want, 3, 2, 3, 0, 0, 3);
}

// ---------------------------------------------------------------- нульові й від'ємні аргументи

static void TestUnknownFormatIsZero()
{
	const char *text = "[a]\n00000001 0001\n";
	int fmts[5];
	int i;
	fmts[0] = 0;
	fmts[1] = 4;
	fmts[2] = -1;
	fmts[3] = 7;
	fmts[4] = 99;
	for (i = 0; i < 5; ++i) {
		char lab[64];
		sprintf(lab, "format %d", fmts[i]);
		SetWhere(lab);
		Res r = Conv(text, fmts[i]);
		CHECK_EQ(r.rc, 0);
		CHECK_STR(r.out, std::string(""));
		CheckStats(r.st, 0, 0, 0, 0, 0, 0);
		ResetWhere();
	}
}

// Від'ємна довжина тексту береться як 0: порожній вхід, нульові лічильники.
static void TestNegativeLengthIsEmpty()
{
	std::vector<char> buf(64, 0);
	TCheatStats st = Unset();
	int rc = CheatConvert("[a]\n00000001 0001\n", -1, 1, 0, 0, &buf[0], 64, &st);
	SetWhere("aLen -1");
	CHECK_EQ(rc, 0);
	CheckStats(st, 0, 0, 0, 0, 0, 0);
	ResetWhere();
}

// Від'ємна ємність — як 0: вихід, що не вміщується, дає -1 без запису; порожній вихід — 0.
static void TestNegativeCapIsZero()
{
	TCheatStats st = Unset();
	std::string bytes;
	int ok = 0;
	int rc = ConvGuarded("[a]\n00000001 0001\n", 1, 0, -1, &st, &bytes, &ok);
	SetWhere("aCap -1, output 18 bytes");
	CHECK_EQ(rc, -1);
	CHECK_EQ(ok, 1);
	CheckStats(st, 1, 0, 1, 0, 0, 0);
	ResetWhere();

	rc = ConvGuarded("# c\n", 1, 0, -1, &st, &bytes, &ok);
	SetWhere("aCap -1, empty output");
	CHECK_EQ(rc, 0);
	CHECK_EQ(ok, 1);
	ResetWhere();

	rc = ConvGuarded("# c\n", 1, 0, 0, &st, &bytes, &ok);
	SetWhere("aCap 0, empty output");
	CHECK_EQ(rc, 0);
	CHECK_EQ(ok, 1);
	ResetWhere();
}

// Нульовий вказівник на лічильники дозволено.
static void TestNullStatsAccepted()
{
	std::vector<char> buf(64, 0);
	int rc = CheatConvert("[a]\n00000001 0001\n", 18, 1, 0, 0, &buf[0], 64, 0);
	SetWhere("null stats");
	CHECK_EQ(rc, 18);
	CHECK_STR(std::string(&buf[0], (size_t)(rc > 0 ? rc : 0)), std::string("[a]\n00000001 0001\n"));
	ResetWhere();
}

// ---------------------------------------------------------------- переповнення буфера

// Для кожного aCap від 0 до розміру виходу: -1, канарки цілі, лічильники як при великому
// буфері. При aCap, що вміщує вихід (рівно або з запасом), — точний вихід.
static void CheckCapSweep(const std::string &aText, int aFormat, const char *aSer, const std::string &aOut)
{
	Res full = ConvSer(aText, aFormat, aSer);
	int need = (int)aOut.size();
	int cap;
	for (cap = 0; cap < need; ++cap) {
		TCheatStats st = Unset();
		std::string bytes;
		int ok = 0;
		int rc = ConvGuarded(aText, aFormat, aSer, cap, &st, &bytes, &ok);
		char lab[80];
		sprintf(lab, "cap %d of %d", cap, need);
		SetWhere(lab);
		CHECK_EQ(rc, -1);
		CHECK_EQ(ok, 1);
		CheckStats(st, full.st.iCheats, full.st.iEnabled, full.st.iCodes,
			full.st.iBadLines, full.st.iDropped, full.st.iGames);
		ResetWhere();
	}
	int caps[3];
	caps[0] = need;
	caps[1] = need + 1;
	caps[2] = need + 7;
	for (cap = 0; cap < 3; ++cap) {
		TCheatStats st = Unset();
		std::string bytes;
		int ok = 0;
		int rc = ConvGuarded(aText, aFormat, aSer, caps[cap], &st, &bytes, &ok);
		char lab[80];
		sprintf(lab, "cap %d (output %d)", caps[cap], need);
		SetWhere(lab);
		CHECK_EQ(rc, need);
		CHECK_EQ(ok, 1);
		CHECK_STR(bytes, aOut);
		CheckStats(st, full.st.iCheats, full.st.iEnabled, full.st.iCodes,
			full.st.iBadLines, full.st.iDropped, full.st.iGames);
		ResetWhere();
	}
}

static void TestCapSweepPcsx()
{
	CheckCapSweep("[alpha]\n00000001 0001\n[*b]\n00000002 0002\n", 1, 0,
		"[alpha]\n00000001 0001\n[*b]\n00000002 0002\n");
}

static void TestCapSweepDollar()
{
	CheckCapSweep("#A\n$00000001 0001\n#B\n$00000002 0002\n", 2, 0,
		"[*A]\n00000001 0001\n[*B]\n00000002 0002\n");
}

static void TestCapSweepCw()
{
	CheckCapSweep("_S A\n_C0 x\n_L 00000001 0001\n_C1 yy\n_L 00000002 0002\n", 3, 0,
		"[x]\n00000001 0001\n[*yy]\n00000002 0002\n");
	CheckCapSweep("_S SLUS-01222\n_C1 q\n_L 00000001 0001\n", 3, "slus01222",
		"[*q]\n00000001 0001\n");
}

// Багато читів із довгими іменами: ім'я обрізається до 63 байтів у кожному.
// Для кожного чита "*" у вході вмикає його; вихід — ім'я з 63 'm'.
static void TestCapSweepManyCheats()
{
	std::string input;
	std::string want;
	int i;
	for (i = 0; i < 6; ++i) {
		char code[32];
		sprintf(code, "%08X %04X\n", (unsigned)(i + 1), (unsigned)(i + 1));
		std::string star = (i % 2) ? "*" : "";
		input += "[" + star + std::string(70, 'm') + "]\n" + code;
		want += "[" + star + std::string(63, 'm') + "]\n" + code;
	}
	CheckCapSweep(input, 1, 0, want);
}

// Вихід нульової довжини: aCap 0 дає 0, не -1.
static void TestCapZeroWithEmptyOutput()
{
	TCheatStats st = Unset();
	std::string bytes;
	int ok = 0;
	int rc = ConvGuarded("# c\n", 1, 0, 0, &st, &bytes, &ok);
	SetWhere("cap 0, empty");
	CHECK_EQ(rc, 0);
	CHECK_EQ(ok, 1);
	ResetWhere();
}

// ---------------------------------------------------------------- KCheatTextMax

// Текст довший за 4194304 байти береться як перші 4194304: "[late]" за межею ігнорується.
static void TestTextMaxPrefixOnly()
{
	std::string t = "[a]\n00000001 0001\n";
	t.append((size_t)(4194304 - 18), '\n');
	t += "[late]\n00000002 0002\n";
	CheckConv("beyond limit", t, 1, 0, "[a]\n00000001 0001\n", 1, 0, 1, 0, 0, 0);
}

// Межа розрізає рядок "[late]" посередині: "[la" в межах — погана рядок (без ']').
static void TestTextMaxCutMidLine()
{
	std::string t = "[a]\n00000001 0001\n";
	t.append((size_t)(4194304 - 21), '\n');
	t += "[late]\n00000002 0002\n";
	CheckConv("cut mid line", t, 1, 0, "[a]\n00000001 0001\n", 1, 0, 1, 1, 0, 0);
}

// Текст рівно 4194304 байти береться повністю: останній рядок не відкидається.
static void TestTextMaxExactlyLimitKept()
{
	std::string t;
	t.append((size_t)(4194304 - 18), '\n');
	t += "[z]\n00000003 0003\n";
	CheckConv("exact limit", t, 1, 0, "[z]\n00000003 0003\n", 1, 0, 1, 0, 0, 0);
}

// ---------------------------------------------------------------- перелік і перемикання

// Нормований текст із трьома читами: alpha (вимкнений, 1 код), beta (увімкнений, 2 коди),
// gamma (вимкнений, 1 код). Довжина 80 байтів (перевіряє TestCountHeaders).
static std::string Sample()
{
	return "[alpha]\n00000001 0001\n[*beta]\n00000002 0002\n00000003 0003\n[gamma]\n0000000A 000A\n";
}

static int CountS(const std::string &aText)
{
	return CheatCount(aText.data(), (int)aText.size());
}

// Опис чита з буфером імені 256 байтів; ім'я, увімкнення й коди повертаються для rc >= 0.
static int InfoOf(const std::string &aText, int aIndex, std::string *aName, int *aEnabled, int *aCodes)
{
	std::vector<char> nb(256, 'Z');
	int en = -999;
	int co = -999;
	int rc = CheatInfo(aText.data(), (int)aText.size(), aIndex, &nb[0], 256, &en, &co);
	aName->clear();
	if (rc >= 0) {
		int n = 0;
		while (n < 255 && nb[n] != 0) ++n;
		aName->assign(&nb[0], (size_t)n);
	}
	*aEnabled = en;
	*aCodes = co;
	return rc;
}

// Перемикання на копії тексту з буфером aCap. Пам'ять за aCap заповнена 'Z': запис за aCap
// видно як aOk = 0. aOutText — текст після виклику (при rc < 0 — незмінений).
static int SetOn(const std::string &aText, int aIndex, int aOn, int aCap, std::string *aOutText, int *aOk)
{
	int len = (int)aText.size();
	int size = len + (aCap > 0 ? aCap : 0) + 64;
	std::vector<char> buf((size_t)size, 'Z');
	int i;
	for (i = 0; i < len; ++i) buf[i] = aText[i];
	int rc = CheatSetEnabled(&buf[0], len, aCap, aIndex, aOn);
	*aOk = 1;
	for (i = (aCap > 0 ? aCap : 0); i < size; ++i) {
		if (buf[i] != 'Z') *aOk = 0;
	}
	if (rc >= 0 && rc <= aCap) aOutText->assign(&buf[0], (size_t)rc);
	else aOutText->assign(&buf[0], (size_t)len);
	return rc;
}

static void TestCountHeaders()
{
	CHECK_EQ((int)Sample().size(), 80);
	CHECK_EQ(CountS(Sample()), 3);
	CHECK_EQ(CountS(""), 0);
	CHECK_EQ(CountS("[a]\n"), 1);
	CHECK_EQ(CountS("[a*b]\n00000001 0001\n"), 1);
	CHECK_EQ(CheatCount(Sample().data(), 8), 1);
}

static void TestInfoDescribesEachCheat()
{
	std::string name;
	int en = -1;
	int co = -1;
	int rc = InfoOf(Sample(), 0, &name, &en, &co);
	SetWhere("index 0");
	CHECK_EQ(rc, 5);
	CHECK_STR(name, std::string("alpha"));
	CHECK_EQ(en, 0);
	CHECK_EQ(co, 1);
	ResetWhere();

	rc = InfoOf(Sample(), 1, &name, &en, &co);
	SetWhere("index 1");
	CHECK_EQ(rc, 4);
	CHECK_STR(name, std::string("beta"));
	CHECK_EQ(en, 1);
	CHECK_EQ(co, 2);
	ResetWhere();

	rc = InfoOf(Sample(), 2, &name, &en, &co);
	SetWhere("index 2");
	CHECK_EQ(rc, 5);
	CHECK_STR(name, std::string("gamma"));
	CHECK_EQ(en, 0);
	CHECK_EQ(co, 1);
	ResetWhere();
}

// Вихід імені: aNameCap - 1 байтів імені, далі нуль; байти поза aNameCap не чіпаються.
// aNameCap <= 0 — нічого не пишеться, але повна довжина повертається.
static void CheckInfoCap(const std::string &aText, int aIndex, const std::string &aName, int aCap)
{
	std::vector<char> nb(32, 'Z');
	int en = -999;
	int co = -999;
	int rc = CheatInfo(aText.data(), (int)aText.size(), aIndex, &nb[0], aCap, &en, &co);
	char lab[64];
	sprintf(lab, "aNameCap %d, name len %d", aCap, (int)aName.size());
	SetWhere(lab);
	CHECK_EQ(rc, (int)aName.size());
	int take = 0;
	if (aCap > 0) take = (aCap - 1 < (int)aName.size()) ? aCap - 1 : (int)aName.size();
	int i;
	int bytesOk = 1;
	for (i = 0; i < take; ++i) {
		if (nb[i] != aName[i]) bytesOk = 0;
	}
	CHECK(bytesOk == 1);
	if (aCap > 0) CHECK_EQ(nb[take], 0);
	int restOk = 1;
	for (i = (aCap > 0 ? take + 1 : 0); i < 32; ++i) {
		if (nb[i] != 'Z') restOk = 0;
	}
	CHECK(restOk == 1);
	CHECK_EQ(en, aName == "alpha" ? 0 : (aName == "beta" ? 1 : 0));
	ResetWhere();
}

static void TestInfoNameCapBehaviour()
{
	CheckInfoCap(Sample(), 0, "alpha", 0);
	CheckInfoCap(Sample(), 0, "alpha", -4);
	CheckInfoCap(Sample(), 0, "alpha", 1);
	CheckInfoCap(Sample(), 0, "alpha", 3);
	CheckInfoCap(Sample(), 0, "alpha", 5);
	CheckInfoCap(Sample(), 0, "alpha", 6);
	CheckInfoCap(Sample(), 0, "alpha", 7);
	CheckInfoCap(Sample(), 1, "beta", 4);
	CheckInfoCap(Sample(), 1, "beta", 5);
}

// Довжина імені — повна, навіть коли вихід обрізано.
static void TestInfoReturnsFullLengthWhenTruncated()
{
	std::string t = "[" + std::string(63, 'k') + "]\n00000001 0001\n";
	std::vector<char> nb(32, 'Z');
	int rc = CheatInfo(t.data(), (int)t.size(), 0, &nb[0], 10, 0, 0);
	SetWhere("name 63, cap 10");
	CHECK_EQ(rc, 63);
	CHECK_EQ(nb[8], 'k');
	CHECK_EQ(nb[9], 0);
	CHECK_EQ(nb[10], 'Z');
	ResetWhere();
}

// Байти 0x80 і вище в імені лишаються; зірочка-позначка вмикання не входить до імені.
static void TestInfoHighBytesAndEnabledMark()
{
	std::string name;
	int en = -1;
	int co = -1;
	int rc = InfoOf("[*\303\251]\n00000001 0001\n", 0, &name, &en, &co);
	SetWhere("high bytes");
	CHECK_EQ(rc, 2);
	CHECK_STR(name, std::string("\303\251"));
	CHECK_EQ(en, 1);
	CHECK_EQ(co, 1);
	ResetWhere();
}

// Індекс поза межами: -1, при порожньому тексті теж. Вихідні параметри не змінюються.
static void TestInfoIndexOutOfRange()
{
	int idx[4];
	idx[0] = -1;
	idx[1] = 3;
	idx[2] = 100;
	idx[3] = 0;
	int i;
	for (i = 0; i < 4; ++i) {
		std::vector<char> nb(32, 'Z');
		int en = -77;
		int co = -77;
		const std::string &src = (i == 3) ? std::string("") : Sample();
		int rc = CheatInfo(src.data(), (int)src.size(), idx[i], &nb[0], 32, &en, &co);
		char lab[40];
		sprintf(lab, "index %d", idx[i]);
		SetWhere(lab);
		CHECK_EQ(rc, -1);
		CHECK_EQ(en, -77);
		CHECK_EQ(co, -77);
		int untouched = 1;
		int k;
		for (k = 0; k < 32; ++k) {
			if (nb[k] != 'Z') untouched = 0;
		}
		CHECK(untouched == 1);
		ResetWhere();
	}
}

// Нульові вказівники на увімкнення й коди дозволено.
static void TestInfoNullOutputPointers()
{
	std::vector<char> nb(32, 'Z');
	int rc = CheatInfo(Sample().data(), 80, 1, &nb[0], 32, 0, 0);
	SetWhere("null enabled and codes");
	CHECK_EQ(rc, 4);
	CHECK_EQ(nb[0], 'b');
	CHECK_EQ(nb[4], 0);
	ResetWhere();
}

static void TestSetEnabledTurnsOn()
{
	std::string want = "[*alpha]\n00000001 0001\n[*beta]\n00000002 0002\n00000003 0003\n[gamma]\n0000000A 000A\n";
	std::string out;
	int ok = 0;
	int rc = SetOn(Sample(), 0, 1, 200, &out, &ok);
	SetWhere("turn on index 0");
	CHECK_EQ(rc, 81);
	CHECK_EQ(ok, 1);
	CHECK_STR(out, want);
	ResetWhere();
}

// Ємність: вмикання потребує рівно +1 байт. Вимкнення не потребує ємності. Уже ввімкнений
// чит повертає довжину без перевірки ємності.
static void TestSetEnabledCapBoundary()
{
	std::string out;
	int ok = 0;
	std::string onAlpha = "[*alpha]\n00000001 0001\n[*beta]\n00000002 0002\n00000003 0003\n[gamma]\n0000000A 000A\n";
	std::string offBeta = "[alpha]\n00000001 0001\n[beta]\n00000002 0002\n00000003 0003\n[gamma]\n0000000A 000A\n";

	int rc = SetOn(Sample(), 0, 1, 81, &out, &ok);
	SetWhere("on, cap 81 (exact)");
	CHECK_EQ(rc, 81);
	CHECK_STR(out, onAlpha);
	CHECK_EQ(ok, 1);
	ResetWhere();

	rc = SetOn(Sample(), 0, 1, 80, &out, &ok);
	SetWhere("on, cap 80 (one short)");
	CHECK_EQ(rc, -1);
	CHECK_STR(out, Sample());
	CHECK_EQ(ok, 1);
	ResetWhere();

	rc = SetOn(Sample(), 1, 0, 80, &out, &ok);
	SetWhere("off, cap 80 (equal to length)");
	CHECK_EQ(rc, 79);
	CHECK_STR(out, offBeta);
	CHECK_EQ(ok, 1);
	ResetWhere();

	rc = SetOn(Sample(), 1, 1, 80, &out, &ok);
	SetWhere("already on, cap 80");
	CHECK_EQ(rc, 80);
	CHECK_STR(out, Sample());
	CHECK_EQ(ok, 1);
	ResetWhere();
}

static void TestSetEnabledTurnsOffRemovesStar()
{
	std::string out;
	int ok = 0;
	int rc = SetOn(Sample(), 1, 0, 200, &out, &ok);
	SetWhere("turn off index 1");
	CHECK_EQ(rc, 79);
	CHECK_STR(out, std::string("[alpha]\n00000001 0001\n[beta]\n00000002 0002\n00000003 0003\n[gamma]\n0000000A 000A\n"));
	CHECK_EQ(ok, 1);
	ResetWhere();
}

// Уже в потрібному стані: повертається довжина, текст не змінюється.
static void TestSetEnabledAlreadyInState()
{
	std::string out;
	int ok = 0;
	int rc = SetOn(Sample(), 0, 0, 200, &out, &ok);
	SetWhere("off already off");
	CHECK_EQ(rc, 80);
	CHECK_STR(out, Sample());
	ResetWhere();

	rc = SetOn(Sample(), 1, 1, 200, &out, &ok);
	SetWhere("on already on");
	CHECK_EQ(rc, 80);
	CHECK_STR(out, Sample());
	ResetWhere();
}

// Перший, середній і останній чит: решта тексту лишається побайтово цілою.
static void TestSetEnabledFirstMiddleLast()
{
	std::string out;
	int ok = 0;
	std::string onGamma = "[alpha]\n00000001 0001\n[*beta]\n00000002 0002\n00000003 0003\n[*gamma]\n0000000A 000A\n";
	std::string onGammaOffBeta = "[alpha]\n00000001 0001\n[beta]\n00000002 0002\n00000003 0003\n[*gamma]\n0000000A 000A\n";
	std::string onAllFirst = "[*alpha]\n00000001 0001\n[*beta]\n00000002 0002\n00000003 0003\n[gamma]\n0000000A 000A\n";

	int rc = SetOn(Sample(), 2, 1, 200, &out, &ok);
	SetWhere("last on");
	CHECK_EQ(rc, 81);
	CHECK_STR(out, onGamma);
	ResetWhere();

	rc = SetOn(onGamma, 1, 0, 200, &out, &ok);
	SetWhere("middle off after last on");
	CHECK_EQ(rc, 80);
	CHECK_STR(out, onGammaOffBeta);
	ResetWhere();

	rc = SetOn(onGammaOffBeta, 0, 1, 200, &out, &ok);
	SetWhere("first on");
	CHECK_EQ(rc, 81);
	CHECK_STR(out, std::string("[*alpha]\n00000001 0001\n[beta]\n00000002 0002\n00000003 0003\n[*gamma]\n0000000A 000A\n"));
	ResetWhere();

	rc = SetOn(onAllFirst, 0, 0, 200, &out, &ok);
	SetWhere("first off");
	CHECK_EQ(rc, 80);
	CHECK_STR(out, std::string("[alpha]\n00000001 0001\n[*beta]\n00000002 0002\n00000003 0003\n[gamma]\n0000000A 000A\n"));
	ResetWhere();
}

// Повтори: на → на (без змін) → вимкнути → вимкнути (без змін).
static void TestSetEnabledRepeatedSameState()
{
	std::string out;
	int ok = 0;
	std::string onAlpha = "[*alpha]\n00000001 0001\n[*beta]\n00000002 0002\n00000003 0003\n[gamma]\n0000000A 000A\n";
	int rc = SetOn(Sample(), 0, 1, 200, &out, &ok);
	SetWhere("repeat step 1");
	CHECK_EQ(rc, 81);
	CHECK_STR(out, onAlpha);
	ResetWhere();

	rc = SetOn(out, 0, 1, 200, &out, &ok);
	SetWhere("repeat step 2");
	CHECK_EQ(rc, 81);
	CHECK_STR(out, onAlpha);
	ResetWhere();

	rc = SetOn(out, 0, 0, 200, &out, &ok);
	SetWhere("repeat step 3");
	CHECK_EQ(rc, 80);
	CHECK_STR(out, Sample());
	ResetWhere();

	rc = SetOn(out, 0, 0, 200, &out, &ok);
	SetWhere("repeat step 4");
	CHECK_EQ(rc, 80);
	CHECK_STR(out, Sample());
	ResetWhere();
}

// Індекс поза межами: -1 і текст не змінено.
static void TestSetEnabledOutOfRange()
{
	std::string out;
	int ok = 0;
	int idx[4];
	idx[0] = -1;
	idx[1] = 3;
	idx[2] = 4;
	idx[3] = -1;
	int i;
	for (i = 0; i < 4; ++i) {
		int rc = SetOn(Sample(), idx[i], (i % 2) == 0 ? 1 : 0, 200, &out, &ok);
		char lab[40];
		sprintf(lab, "index %d", idx[i]);
		SetWhere(lab);
		CHECK_EQ(rc, -1);
		CHECK_STR(out, Sample());
		ResetWhere();
	}
}

static void TestSetEnabledEmptyText()
{
	std::string out;
	int ok = 0;
	int rc = SetOn("", 0, 1, 16, &out, &ok);
	SetWhere("empty text");
	CHECK_EQ(rc, -1);
	CHECK_EQ(CountS(""), 0);
	ResetWhere();
}

// aOn — будь-яке ненульове значення вмикає.
static void TestSetEnabledAOnNonZeroMeansOn()
{
	std::string out;
	int ok = 0;
	std::string onAlpha = "[*alpha]\n00000001 0001\n[*beta]\n00000002 0002\n00000003 0003\n[gamma]\n0000000A 000A\n";
	int rc = SetOn(Sample(), 0, 2, 200, &out, &ok);
	SetWhere("aOn 2");
	CHECK_EQ(rc, 81);
	CHECK_STR(out, onAlpha);
	ResetWhere();

	rc = SetOn(onAlpha, 0, -1, 200, &out, &ok);
	SetWhere("aOn -1 on already on");
	CHECK_EQ(rc, 81);
	CHECK_STR(out, onAlpha);
	ResetWhere();
}

// Кругова перевірка: нормований текст → перелік → перемикання → Pcsx дає той самий текст.
// Для кожного чита: перемикання міняє лише його стан, назва і коди ті самі, повторне
// перетворення як Pcsx дає той самий текст, лічильники відповідають.
static int EnabledCount(const std::string &aText)
{
	int n = CountS(aText);
	int i;
	int en = 0;
	for (i = 0; i < n; ++i) {
		std::string nm;
		int e;
		int c;
		InfoOf(aText, i, &nm, &e, &c);
		en += e;
	}
	return en;
}

static int CodeTotal(const std::string &aText)
{
	int n = CountS(aText);
	int i;
	int total = 0;
	for (i = 0; i < n; ++i) {
		std::string nm;
		int e;
		int c;
		InfoOf(aText, i, &nm, &e, &c);
		total += c;
	}
	return total;
}

static void CheckRoundTrip(const std::string &aNorm)
{
	int count = CountS(aNorm);
	int i;
	for (i = 0; i < count; ++i) {
		std::string name;
		int en = -1;
		int co = -1;
		int rc = InfoOf(aNorm, i, &name, &en, &co);
		char lab[48];
		sprintf(lab, "round trip index %d", i);
		SetWhere(lab);
		CHECK(rc >= 0);
		std::string toggled;
		int ok = 0;
		int rc2 = SetOn(aNorm, i, en ? 0 : 1, (int)aNorm.size() + 1, &toggled, &ok);
		CHECK_EQ(rc2, (int)aNorm.size() + (en ? -1 : 1));
		CHECK_EQ(ok, 1);
		std::string name2;
		int en2 = -1;
		int co2 = -1;
		InfoOf(toggled, i, &name2, &en2, &co2);
		CHECK_STR(name2, name);
		CHECK_EQ(en2, en ? 0 : 1);
		CHECK_EQ(co2, co);
		Res again = Conv(toggled, 1);
		CHECK_STR(again.out, toggled);
		CheckStats(again.st, count, EnabledCount(toggled), CodeTotal(toggled), 0, 0, 0);
		ResetWhere();
	}
}

// Вихід трьох форматів, нормований, переходить у перелік і перемикання.
static void TestRoundTripFromEachFormat()
{
	Res cw = Conv("_S A\n_C0 Off one\n_L 00000001 0001\n_C1 On two\n_L 00000002 0002\n_L 00000003 0003\n", 3);
	SetWhere("cw normalised");
	CHECK_STR(cw.out, std::string("[Off one]\n00000001 0001\n[*On two]\n00000002 0002\n00000003 0003\n"));
	ResetWhere();
	CheckRoundTrip(cw.out);

	Res dl = Conv("#A\n$00000001 0001\n#B\n$00000002 0002\n$00000003 0003\n", 2);
	SetWhere("dollar normalised");
	CHECK_STR(dl.out, std::string("[*A]\n00000001 0001\n[*B]\n00000002 0002\n00000003 0003\n"));
	ResetWhere();
	CheckRoundTrip(dl.out);

	CheckRoundTrip(Sample());
}

// ---------------------------------------------------------------- стійкість (фіксовані зерна)

static unsigned gRng = 1;

static unsigned Rnd()
{
	gRng ^= gRng << 13;
	gRng ^= gRng >> 17;
	gRng ^= gRng << 5;
	return gRng;
}

static int RndN(int aN)
{
	return (int)(Rnd() % (unsigned)aN);
}

// Фрагменти для випадкового тексту: теги, коментарі, коди, керівні й не-ASCII байти.
// Усі байти записано трицифровими восьмковими ескейпами, щоб сусідні цифри не змінили значення.
static const char *kPieces[] = {
	"[", "]", "[a]", "[*b]", "[ x ]", "*", "#", "#name", ";", "//", "/", "\"", "$", "$1234",
	"$00000001 0001", "_S", "_S SLUS-01222", "_s slus01222", "_G", "_G title", "_C", "_C0",
	"_C1 name", "_c5 x", "_C10", "_L", "_L 00000001 0001", "_l 0000000a 000B", "00000001 0001",
	"0x12 0x3", "1234", "ZZ", " ", "\t", "\r", "\n", "\r\n", "a", "Z", "9", "\001", "\177",
	"\200", "\377", "\357\273\277", "\303\251", "SAFEMODE", "x", "*[]", "0", "FFFFFFFF FFFF",
	"_Sx", "$SAFE MODE", "[]", "#A\n", "$00000002 0002\n", "_C0 q\n", "_L 00000003 0003\n",
	"_S SLUS01222\n", "[*c]\n", "\n#x\n"
};
static const int KPieceCount = (int)(sizeof(kPieces) / sizeof(kPieces[0]));

static std::string RandomText()
{
	std::string t;
	int n = 1 + RndN(40);
	int i;
	for (i = 0; i < n; ++i) {
		if (RndN(5) == 0) {
			char code[40];
			sprintf(code, "%08X %04X\n", Rnd(), Rnd() & 0xFFFFu);
			t += code;
		} else {
			t += kPieces[RndN(KPieceCount)];
		}
	}
	return t;
}

static int IsHexUpper(char aC)
{
	return (aC >= '0' && aC <= '9') || (aC >= 'A' && aC <= 'F');
}

// Рядок виводу: заголовок "[name]" або "[*name]" з ім'ям 1..63 байти без керівних і дужок,
// або код з 8 цифр, пробілом і 4 цифр у верхньому регістрі.
static int ValidOutLine(const std::string &aL)
{
	if (aL.size() >= 3 && aL[0] == '[' && aL[aL.size() - 1] == ']') {
		std::string n = aL.substr(1, aL.size() - 2);
		if (!n.empty() && n[0] == '*') n = n.substr(1);
		if (n.empty() || n.size() > 63) return 0;
		if (n[0] == '*') return 0;
		size_t i;
		for (i = 0; i < n.size(); ++i) {
			unsigned char c = (unsigned char)n[i];
			if (c < 0x20 || c == 0x7F || c == '[' || c == ']') return 0;
		}
		return 1;
	}
	if (aL.size() == 13 && aL[8] == ' ') {
		int i;
		for (i = 0; i < 13; ++i) {
			if (i != 8 && !IsHexUpper(aL[i])) return 0;
		}
		return 1;
	}
	return 0;
}

static int ValidOutput(const std::string &aOut)
{
	if (aOut.empty()) return 1;
	if (aOut[aOut.size() - 1] != '\n') return 0;
	size_t start = 0;
	size_t i;
	for (i = 0; i < aOut.size(); ++i) {
		if (aOut[i] == '\n') {
			if (!ValidOutLine(aOut.substr(start, i - start))) return 0;
			start = i + 1;
		}
	}
	return 1;
}

static int CodesIn(const std::string &aText)
{
	return CodeTotal(aText);
}

// Тисячі випадкових входів: канарки цілі, rc у межах aCap або -1, при -1 лічильники такі
// самі, як при великому буфері; вихід коректний, лічильники узгоджені з виходом, повторне
// перетворення як Pcsx дає той самий текст (ідемпотентність).
static void TestRandomConvertInvariants()
{
	const char *sers[4];
	sers[0] = 0;
	sers[1] = "SLUS01222";
	sers[2] = "-";
	sers[3] = "";
	gRng = 12345;
	int iter;
	for (iter = 0; iter < 3000; ++iter) {
		std::string text = RandomText();
		int fmt = 1 + RndN(3);
		const char *ser = sers[RndN(4)];
		int cap = RndN(2049);
		char lab[96];
		sprintf(lab, "iter %d fmt %d cap %d", iter, fmt, cap);
		SetWhere(lab);

		TCheatStats st = Unset();
		std::string out;
		int ok = 0;
		int rc = ConvGuarded(text, fmt, ser, cap, &st, &out, &ok);
		CHECK_EQ(ok, 1);
		CHECK(rc == -1 || (rc >= 0 && rc <= cap));

		Res big = ConvSer(text, fmt, ser);
		if (rc == -1) {
			CHECK(big.rc > cap);
		} else {
			CHECK_EQ(rc, big.rc);
			CHECK_STR(out, big.out);
		}
		CheckStats(st, big.st.iCheats, big.st.iEnabled, big.st.iCodes,
			big.st.iBadLines, big.st.iDropped, big.st.iGames);

		CHECK(big.st.iCheats >= 0 && big.st.iEnabled >= 0 && big.st.iCodes >= 0);
		CHECK(big.st.iEnabled <= big.st.iCheats);
		CHECK(big.st.iCodes >= big.st.iCheats);
		CHECK(big.st.iBadLines >= 0 && big.st.iDropped >= 0);
		CHECK(fmt == 3 || big.st.iGames == 0);
		CHECK(ValidOutput(big.out));
		CHECK_EQ(CountS(big.out), big.st.iCheats);
		CHECK_EQ(EnabledCount(big.out), big.st.iEnabled);
		CHECK_EQ(CodesIn(big.out), big.st.iCodes);

		Res again = Conv(big.out, 1);
		CHECK_STR(again.out, big.out);
		ResetWhere();
	}
}

// Невідомий або неправильний формат: нульовий результат і нульові лічильники, канарки цілі.
static void TestRandomUnknownFormatZero()
{
	const int fmts[4] = { 0, 4, -1, 9 };
	gRng = 777;
	int iter;
	for (iter = 0; iter < 1000; ++iter) {
		std::string text = RandomText();
		int fmt = fmts[RndN(4)];
		TCheatStats st = Unset();
		std::string out;
		int ok = 0;
		int rc = ConvGuarded(text, fmt, 0, 256, &st, &out, &ok);
		char lab[64];
		sprintf(lab, "iter %d fmt %d", iter, fmt);
		SetWhere(lab);
		CHECK_EQ(rc, 0);
		CHECK_EQ(ok, 1);
		CheckStats(st, 0, 0, 0, 0, 0, 0);
		ResetWhere();
	}
}

// Визначення формату завжди дає одне з чотирьох значень перелічення.
static void TestRandomDetectRange()
{
	gRng = 4242;
	int iter;
	for (iter = 0; iter < 3000; ++iter) {
		std::string text = RandomText();
		int r = DetectS(text);
		char lab[48];
		sprintf(lab, "iter %d", iter);
		SetWhere(lab);
		CHECK(r >= 0 && r <= 3);
		ResetWhere();
	}
}

// Перелік і перемикання на випадкових (ненормованих) текстах не пишуть за межі буфера;
// на нормованих текстах rc узгоджений зі станом: після перемикання чит має задане
// увімкнення, решта читів не змінені.
static void TestRandomListingCanaries()
{
	gRng = 9001;
	int iter;
	for (iter = 0; iter < 2000; ++iter) {
		std::string text = RandomText();
		int idx = RndN(7) - 1;
		int cap = RndN(42) - 2;
		std::vector<char> nb(64, 'Z');
		int en = 0;
		int co = 0;
		CheatInfo(text.data(), (int)text.size(), idx, &nb[0], cap, &en, &co);
		int untouched = 1;
		int k;
		for (k = (cap > 0 ? cap : 0); k < 64; ++k) {
			if (nb[k] != 'Z') untouched = 0;
		}
		char lab[64];
		sprintf(lab, "info iter %d", iter);
		SetWhere(lab);
		CHECK(untouched == 1);
		ResetWhere();

		std::string out;
		int ok = 0;
		int cap2 = (int)text.size() + RndN(8);
		int aOn = RndN(2);
		CheatCount(text.data(), (int)text.size());
		SetOn(text, idx, aOn, cap2, &out, &ok);
		sprintf(lab, "set iter %d", iter);
		SetWhere(lab);
		CHECK_EQ(ok, 1);
		ResetWhere();
	}

	// Нормовані тексти: перемикання дає rc або -1, і стан чита відповідає aOn.
	gRng = 31337;
	for (iter = 0; iter < 1500; ++iter) {
		Res base = Conv(RandomText(), 1 + RndN(3));
		int count = CountS(base.out);
		int idx = RndN(count + 3) - 1;
		int aOn = RndN(2);
		std::string out;
		int ok = 0;
		int cap = (int)base.out.size() + RndN(4);
		int rc = SetOn(base.out, idx, aOn, cap, &out, &ok);
		char lab[64];
		sprintf(lab, "norm iter %d idx %d", iter, idx);
		SetWhere(lab);
		CHECK_EQ(ok, 1);
		if (idx < 0 || idx >= count) {
			CHECK_EQ(rc, -1);
		} else {
			std::string nm0;
			int e0 = -1;
			int c0 = -1;
			InfoOf(base.out, idx, &nm0, &e0, &c0);
			// -1 законний лише як нестача ємності для вмикання вимкненого чита.
			int legit = aOn && !e0 && cap < (int)base.out.size() + 1;
			if (rc == -1) {
				CHECK(legit == 1);
				CHECK_STR(out, base.out);
			} else {
				CHECK(rc >= 0 && rc <= cap);
				std::string nm;
				int e = -1;
				int c = -1;
				InfoOf(out, idx, &nm, &e, &c);
				CHECK_EQ(e, aOn ? 1 : 0);
				CHECK_EQ(CountS(out), count);
			}
		}
		ResetWhere();
	}
}

// ---------------------------------------------------------------- запуск

int main(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--list") == 0) gListOnly = 1;
		else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) gOnly = argv[++i];
	}
	RUN(TestConstantsMatchSpec);
	RUN(TestFormatValuesMatchSpec);
	RUN(TestLinesSplitAtCrLfLfCr);
	RUN(TestLinesLastLineWithoutTerminator);
	RUN(TestLinesBomSkippedOnlyAtStart);
	RUN(TestLinesTrimSpacesAndTabs);
	RUN(TestLinesBlankLinesIgnored);
	RUN(TestCodeAcceptedShapes);
	RUN(TestCodeRejectedShapes);
	RUN(TestNameControlBytesRemoved);
	RUN(TestNameTabRemovedNotReplaced);
	RUN(TestNameBracketsReplaced);
	RUN(TestNameLeadingStarsRemoved);
	RUN(TestNameTrimAfterRemoval);
	RUN(TestNameEmptyBecomesCheat);
	RUN(TestNameCutAt63);
	RUN(TestNameCutMidMultibyte);
	RUN(TestNameHighBytesPass);
	RUN(TestSerialEqualRules);
	RUN(TestSerialNullIsEmpty);
	RUN(TestDetectPcsxFirstLine);
	RUN(TestDetectDollarFirstLine);
	RUN(TestDetectCwTags);
	RUN(TestDetectSkipsCommentsAndBlanks);
	RUN(TestDetectUnknown);
	RUN(TestDetectFirstTriggerWins);
	RUN(TestPcsxBasicOutput);
	RUN(TestPcsxCommentsSkipped);
	RUN(TestPcsxUnclosedBracketIsBad);
	RUN(TestPcsxCodeBeforeHeaderDropped);
	RUN(TestPcsxNonCodeIsBad);
	RUN(TestPcsxEmptyHeaderNotWritten);
	RUN(TestPcsxNameUsesLastBracket);
	RUN(TestPcsxEnabledMarker);
	RUN(TestPcsxDuplicateNamesKept);
	RUN(TestPcsxBracketNotAtStartIsBad);
	RUN(TestPcsxEmptyInputs);
	RUN(TestPcsxStatsEachCounter);
	RUN(TestDollarBasic);
	RUN(TestDollarCodesBeforeHashUseCheat);
	RUN(TestDollarHashWithoutCodesNotWritten);
	RUN(TestDollarCodesBetweenHashesOneCheat);
	RUN(TestDollarSameNameKept);
	RUN(TestDollarNoBlankNonCodeDropped);
	RUN(TestDollarBlankNonCodeIsBad);
	RUN(TestDollarRestIsTrimmed);
	RUN(TestDollarPlainLines);
	RUN(TestDollarCommentStyles);
	RUN(TestDollarNameRules);
	RUN(TestDollarMixedStats);
	RUN(TestDollarEmptyInput);
	RUN(TestCwBasic);
	RUN(TestCwDigitZeroDisabledOthersEnabled);
	RUN(TestCwTagCaseInsensitive);
	RUN(TestCwTagMustBeFollowedByBlank);
	RUN(TestCwTitleIgnored);
	RUN(TestCwCodeBeforeCheatDropped);
	RUN(TestCwLNotCodeIsBad);
	RUN(TestCwCommentStyles);
	RUN(TestCwLinesBeforeFirstSerialSkipped);
	RUN(TestCwNoGameNoOutput);
	RUN(TestCwCheatWithoutCodesNotWritten);
	RUN(TestCwDuplicateNamesKept);
	RUN(TestCwCheatNameRules);
	RUN(TestCwTwoDigitCheatTagIsBad);
	RUN(TestCwSerialFilterMatchesIgnoringPunctuation);
	RUN(TestCwSerialFilterSkipsOtherGamesSilently);
	RUN(TestCwSerialFilterCountsInsideMatch);
	RUN(TestCwSerialFilterEmptyGameNeverMatches);
	RUN(TestCwNoFilterWritesAllGames);
	RUN(TestUnknownFormatIsZero);
	RUN(TestNegativeLengthIsEmpty);
	RUN(TestNegativeCapIsZero);
	RUN(TestNullStatsAccepted);
	RUN(TestCapSweepPcsx);
	RUN(TestCapSweepDollar);
	RUN(TestCapSweepCw);
	RUN(TestCapSweepManyCheats);
	RUN(TestCapZeroWithEmptyOutput);
	RUN(TestTextMaxPrefixOnly);
	RUN(TestTextMaxCutMidLine);
	RUN(TestTextMaxExactlyLimitKept);
	RUN(TestCountHeaders);
	RUN(TestInfoDescribesEachCheat);
	RUN(TestInfoNameCapBehaviour);
	RUN(TestInfoReturnsFullLengthWhenTruncated);
	RUN(TestInfoHighBytesAndEnabledMark);
	RUN(TestInfoIndexOutOfRange);
	RUN(TestInfoNullOutputPointers);
	RUN(TestSetEnabledTurnsOn);
	RUN(TestSetEnabledCapBoundary);
	RUN(TestSetEnabledTurnsOffRemovesStar);
	RUN(TestSetEnabledAlreadyInState);
	RUN(TestSetEnabledFirstMiddleLast);
	RUN(TestSetEnabledRepeatedSameState);
	RUN(TestSetEnabledOutOfRange);
	RUN(TestSetEnabledEmptyText);
	RUN(TestSetEnabledAOnNonZeroMeansOn);
	RUN(TestRoundTripFromEachFormat);
	RUN(TestRandomConvertInvariants);
	RUN(TestRandomUnknownFormatZero);
	RUN(TestRandomDetectRange);
	RUN(TestRandomListingCanaries);
	if (gListOnly) return 0;
	printf("cheatfile: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
}
