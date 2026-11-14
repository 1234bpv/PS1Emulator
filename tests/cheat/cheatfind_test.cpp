// Tests for cheatfind.h / cheatfind.cpp: candidate generation, priority order,
// format validation via CheatDetect, serial filtering for cheatpops.db, and
// active cheat file loading.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "cheatfind.h"

static int gChecks = 0;
static int gFails = 0;
static char gWhere[200] = "";
static const char *gTest = "";
static const char *gOnly = 0;
static int gListOnly = 0;

static std::string Esc(const std::string &aS)
{
	std::string r;
	for (size_t i = 0; i < aS.size(); ++i) {
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

// --- Mock filesystem for testing candidate priority and file access ---------

struct TMockEntry
{
	std::string iPath;
	std::string iContent;
	TMockEntry(const std::string &aPath, const std::string &aContent)
		: iPath(aPath), iContent(aContent) {}
};

struct TMockFs
{
	std::vector<TMockEntry> iFiles;
	std::string iLastWrittenPath;
	std::string iLastWrittenContent;
	std::string iLastLoadedPath;
};

static int MockExists(const char *aPath, void *aUserData)
{
	TMockFs *fs = (TMockFs *)aUserData;
	if (!fs || !aPath) return 0;
	for (size_t i = 0; i < fs->iFiles.size(); ++i) {
		if (fs->iFiles[i].iPath == aPath) return 1;
	}
	return 0;
}

static int MockRead(const char *aPath, char *aBuf, int aCap, void *aUserData)
{
	TMockFs *fs = (TMockFs *)aUserData;
	if (!fs || !aPath || !aBuf || aCap <= 0) return -1;
	for (size_t i = 0; i < fs->iFiles.size(); ++i) {
		if (fs->iFiles[i].iPath == aPath) {
			int len = (int)fs->iFiles[i].iContent.size();
			int toCopy = len < aCap ? len : aCap;
			memcpy(aBuf, fs->iFiles[i].iContent.data(), toCopy);
			return toCopy;
		}
	}
	return -1;
}

static int MockWrite(const char *aPath, const char *aBuf, int aLen, void *aUserData)
{
	TMockFs *fs = (TMockFs *)aUserData;
	if (!fs || !aPath) return -1;
	fs->iLastWrittenPath = aPath;
	fs->iLastWrittenContent.assign(aBuf ? aBuf : "", aLen > 0 ? aLen : 0);
	return aLen;
}

static std::string gLastLoadedPath;
static void TestLoader(const char *aPath)
{
	gLastLoadedPath = aPath ? aPath : "";
}

// pcsx core stub
extern "C" void LoadCheats(const char *filename)
{
	gLastLoadedPath = filename ? filename : "";
}

// ----------------------------------------------------------------------------
// Candidate generation tests
// ----------------------------------------------------------------------------

static void TestCandidateListStandard()
{
	TCheatCandidate cands[KCheatCandidateMax];
	int n = CheatBuildCandidates("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cheats\\", cands, KCheatCandidateMax);
	CHECK_EQ(n, 5);

	CHECK_EQ(cands[0].iRole, (int)ECheatCandGameSerial);
	CHECK_STR(cands[0].iPath, "E:\\PS1\\games\\SCES-00344.cht");

	CHECK_EQ(cands[1].iRole, (int)ECheatCandGameName);
	CHECK_STR(cands[1].iPath, "E:\\PS1\\games\\Crash.cht");

	CHECK_EQ(cands[2].iRole, (int)ECheatCandDirSerial);
	CHECK_STR(cands[2].iPath, "E:\\PS1\\cheats\\SCES-00344.cht");

	CHECK_EQ(cands[3].iRole, (int)ECheatCandGameTxt);
	CHECK_STR(cands[3].iPath, "E:\\PS1\\games\\CHEATS.TXT");

	CHECK_EQ(cands[4].iRole, (int)ECheatCandCheatpops);
	CHECK_STR(cands[4].iPath, "E:\\PS1\\cheats\\cheatpops.db");
}

static void TestCandidateListSlashSeparators()
{
	TCheatCandidate cands[KCheatCandidateMax];
	// Forward slashes in game path and cheats dir without trailing slash
	int n = CheatBuildCandidates("games/Crash.bin", "SCES-00344", "cheats", cands, KCheatCandidateMax);
	CHECK_EQ(n, 5);

	CHECK_EQ(cands[0].iRole, (int)ECheatCandGameSerial);
	CHECK_STR(cands[0].iPath, "games/SCES-00344.cht");

	CHECK_EQ(cands[1].iRole, (int)ECheatCandGameName);
	CHECK_STR(cands[1].iPath, "games/Crash.cht");

	CHECK_EQ(cands[2].iRole, (int)ECheatCandDirSerial);
	CHECK_STR(cands[2].iPath, "cheats/SCES-00344.cht");

	CHECK_EQ(cands[3].iRole, (int)ECheatCandGameTxt);
	CHECK_STR(cands[3].iPath, "games/CHEATS.TXT");

	CHECK_EQ(cands[4].iRole, (int)ECheatCandCheatpops);
	CHECK_STR(cands[4].iPath, "cheats/cheatpops.db");
}

static void TestCandidateListNoDir()
{
	TCheatCandidate cands[KCheatCandidateMax];
	int n = CheatBuildCandidates("Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", cands, KCheatCandidateMax);
	CHECK_EQ(n, 5);

	CHECK_STR(cands[0].iPath, "SCES-00344.cht");
	CHECK_STR(cands[1].iPath, "Crash.cht");
	CHECK_STR(cands[2].iPath, "E:\\PS1\\cfg\\SCES-00344.cht");
	CHECK_STR(cands[3].iPath, "CHEATS.TXT");
	CHECK_STR(cands[4].iPath, "E:\\PS1\\cfg\\cheatpops.db");
}

static void TestCandidateListNoExtension()
{
	TCheatCandidate cands[KCheatCandidateMax];
	int n = CheatBuildCandidates("E:\\PS1\\games\\Crash", "SCES-00344", "E:\\PS1\\cfg", cands, KCheatCandidateMax);
	CHECK_EQ(n, 5);
	CHECK_STR(cands[1].iPath, "E:\\PS1\\games\\Crash.cht");
}

static void TestCandidateListMultipleDots()
{
	TCheatCandidate cands[KCheatCandidateMax];
	int n = CheatBuildCandidates("E:\\PS1\\games\\Crash.Bandicoot.v1.0.bin", "SCES-00344", "E:\\PS1\\cfg\\", cands, KCheatCandidateMax);
	CHECK_EQ(n, 5);
	CHECK_STR(cands[1].iPath, "E:\\PS1\\games\\Crash.Bandicoot.v1.0.cht");
}

static void TestCandidateListMissingSerial()
{
	TCheatCandidate cands[KCheatCandidateMax];
	// When serial is null or has no letters/digits, rules 1 and 3 are omitted
	int n = CheatBuildCandidates("E:\\PS1\\games\\Crash.bin", 0, "E:\\PS1\\cfg\\", cands, KCheatCandidateMax);
	CHECK_EQ(n, 3);
	CHECK_EQ(cands[0].iRole, (int)ECheatCandGameName);
	CHECK_STR(cands[0].iPath, "E:\\PS1\\games\\Crash.cht");
	CHECK_EQ(cands[1].iRole, (int)ECheatCandGameTxt);
	CHECK_STR(cands[1].iPath, "E:\\PS1\\games\\CHEATS.TXT");
	CHECK_EQ(cands[2].iRole, (int)ECheatCandCheatpops);
	CHECK_STR(cands[2].iPath, "E:\\PS1\\cfg\\cheatpops.db");

	// Whitespace-only serial
	n = CheatBuildCandidates("E:\\PS1\\games\\Crash.bin", "   ", "E:\\PS1\\cfg\\", cands, KCheatCandidateMax);
	CHECK_EQ(n, 3);

	// Punctuation-only serial
	n = CheatBuildCandidates("E:\\PS1\\games\\Crash.bin", "---...", "E:\\PS1\\cfg\\", cands, KCheatCandidateMax);
	CHECK_EQ(n, 3);
}

static void TestCandidateListEmptyCheatsDir()
{
	TCheatCandidate cands[KCheatCandidateMax];
	// If cheats dir is null or empty, rules 3 and 5 are omitted
	int n = CheatBuildCandidates("E:\\PS1\\games\\Crash.bin", "SCES-00344", 0, cands, KCheatCandidateMax);
	CHECK_EQ(n, 3);
	CHECK_EQ(cands[0].iRole, (int)ECheatCandGameSerial);
	CHECK_STR(cands[0].iPath, "E:\\PS1\\games\\SCES-00344.cht");
	CHECK_EQ(cands[1].iRole, (int)ECheatCandGameName);
	CHECK_STR(cands[1].iPath, "E:\\PS1\\games\\Crash.cht");
	CHECK_EQ(cands[2].iRole, (int)ECheatCandGameTxt);
	CHECK_STR(cands[2].iPath, "E:\\PS1\\games\\CHEATS.TXT");
}

static void TestCandidateListMaxLimit()
{
	TCheatCandidate cands[2];
	int n = CheatBuildCandidates("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", cands, 2);
	CHECK_EQ(n, 2);
	CHECK_EQ(cands[0].iRole, (int)ECheatCandGameSerial);
	CHECK_EQ(cands[1].iRole, (int)ECheatCandGameName);

	n = CheatBuildCandidates("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", 0, 5);
	CHECK_EQ(n, 0);
}

// ----------------------------------------------------------------------------
// CheatCwHasSerial tests
// ----------------------------------------------------------------------------

static void TestCwHasSerialMatches()
{
	const char *db =
		"_S SCES-00344\n"
		"_G Crash Bandicoot\n"
		"_C0 Infinite Lives\n"
		"_L 800AAAAA 0009\n";
	int len = (int)strlen(db);

	// Exact match
	CHECK_EQ(CheatCwHasSerial(db, len, "SCES-00344", -1), 1);
	// Case-insensitive serial
	CHECK_EQ(CheatCwHasSerial(db, len, "sces-00344", -1), 1);
	// Serial with underscore instead of hyphen
	CHECK_EQ(CheatCwHasSerial(db, len, "SCES_00344", -1), 1);
	// Serial without punctuation
	CHECK_EQ(CheatCwHasSerial(db, len, "SCES00344", -1), 1);
	// Serial with dots
	CHECK_EQ(CheatCwHasSerial(db, len, "SCES.00344", -1), 1);
}

static void TestCwHasSerialDifferentGame()
{
	const char *db =
		"_S SLUS-01000\n"
		"_G Gran Turismo\n"
		"_C0 Max Cash\n"
		"_L 80011111 FFFF\n";
	int len = (int)strlen(db);

	CHECK_EQ(CheatCwHasSerial(db, len, "SCES-00344", -1), 0);
	CHECK_EQ(CheatCwHasSerial(db, len, "SLUS-01001", -1), 0);
	CHECK_EQ(CheatCwHasSerial(db, len, "SLUS-01000", -1), 1);
}

static void TestCwHasSerialMultipleGames()
{
	const char *db =
		"# cwcheat multi-game database\n"
		"\n"
		"_S SLUS-00001\n"
		"_G Game 1\n"
		"_C0 Cheat 1\n"
		"_L 80010000 0001\n"
		"\n"
		"_S SCES-00344\n"
		"_G Crash Bandicoot\n"
		"_C0 99 Lives\n"
		"_L 80020000 0063\n"
		"\n"
		"_S SLES-02000\n"
		"_G Game 3\n"
		"_C0 Cheat 3\n"
		"_L 80030000 0003\n";
	int len = (int)strlen(db);

	CHECK_EQ(CheatCwHasSerial(db, len, "SLUS-00001", -1), 1);
	CHECK_EQ(CheatCwHasSerial(db, len, "SCES-00344", -1), 1);
	CHECK_EQ(CheatCwHasSerial(db, len, "SLES-02000", -1), 1);
	CHECK_EQ(CheatCwHasSerial(db, len, "SLUS-99999", -1), 0);
}

static void TestCwHasSerialBOMAndComments()
{
	const char *db =
		"\xEF\xBB\xBF; UTF-8 cwcheat DB\n"
		"// Some comment line\n"
		"_s sces_00344\n"
		"_g Title\n";
	int len = (int)strlen(db);

	CHECK_EQ(CheatCwHasSerial(db, len, "SCES-00344", -1), 1);
}

static void TestCwHasSerialInvalidInputs()
{
	const char *db = "_S SCES-00344\n";
	int len = (int)strlen(db);

	CHECK_EQ(CheatCwHasSerial(0, len, "SCES-00344", -1), 0);
	CHECK_EQ(CheatCwHasSerial(db, 0, "SCES-00344", -1), 0);
	CHECK_EQ(CheatCwHasSerial(db, -1, "SCES-00344", -1), 0);
	CHECK_EQ(CheatCwHasSerial(db, len, 0, -1), 0);
	CHECK_EQ(CheatCwHasSerial(db, len, "", 0), 0);
	CHECK_EQ(CheatCwHasSerial(db, len, "---", 3), 0);

	// Malformed tag _Sxyz without blank
	const char *badTag = "_Sxyz SCES-00344\n";
	CHECK_EQ(CheatCwHasSerial(badTag, (int)strlen(badTag), "SCES-00344", -1), 0);
}

// ----------------------------------------------------------------------------
// Candidate priority and format validation tests (CheatFindFile)
// ----------------------------------------------------------------------------

static const char KPcsxSample[] =
	"[Infinite Lives]\n"
	"800AAAAA 0009\n"
	"[*Infinite Health]\n"
	"800BBBBB 03E7\n";

static const char KDollarSample[] =
	"// CHEATS.TXT wide screen and cheats\n"
	"#Infinite Lives\n"
	"$800AAAAA 0009\n";

static const char KCwSample[] =
	"_S SCES-00344\n"
	"_G Crash Bandicoot\n"
	"_C0 Infinite Lives\n"
	"_L 800AAAAA 0009\n";

static void TestPriorityCandidate1First()
{
	TMockFs fs;
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\SCES-00344.cht", KPcsxSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\Crash.cht", KPcsxSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\SCES-00344.cht", KPcsxSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\CHEATS.TXT", KDollarSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\cheatpops.db", KCwSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	TCheatFindResult res;
	int r = CheatFindFile("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", &ops, 0, 0, &res);

	CHECK_EQ(r, 1);
	CHECK_EQ(res.iRole, (int)ECheatCandGameSerial);
	CHECK_STR(res.iPath, "E:\\PS1\\games\\SCES-00344.cht");
	CHECK_EQ(res.iFormat, (int)ECheatFmtPcsx);
}

static void TestPriorityCandidate2Second()
{
	TMockFs fs;
	// Candidate 1 missing; Candidate 2 present
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\Crash.cht", KPcsxSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\SCES-00344.cht", KPcsxSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\CHEATS.TXT", KDollarSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\cheatpops.db", KCwSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	TCheatFindResult res;
	int r = CheatFindFile("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", &ops, 0, 0, &res);

	CHECK_EQ(r, 1);
	CHECK_EQ(res.iRole, (int)ECheatCandGameName);
	CHECK_STR(res.iPath, "E:\\PS1\\games\\Crash.cht");
	CHECK_EQ(res.iFormat, (int)ECheatFmtPcsx);
}

static void TestPriorityCandidate3Third()
{
	TMockFs fs;
	// Candidates 1 and 2 missing; Candidate 3 present
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\SCES-00344.cht", KPcsxSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\CHEATS.TXT", KDollarSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\cheatpops.db", KCwSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	TCheatFindResult res;
	int r = CheatFindFile("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", &ops, 0, 0, &res);

	CHECK_EQ(r, 1);
	CHECK_EQ(res.iRole, (int)ECheatCandDirSerial);
	CHECK_STR(res.iPath, "E:\\PS1\\cfg\\SCES-00344.cht");
	CHECK_EQ(res.iFormat, (int)ECheatFmtPcsx);
}

static void TestPriorityCandidate4Fourth()
{
	TMockFs fs;
	// Candidates 1, 2, 3 missing; Candidate 4 present
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\CHEATS.TXT", KDollarSample));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\cheatpops.db", KCwSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	TCheatFindResult res;
	int r = CheatFindFile("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", &ops, 0, 0, &res);

	CHECK_EQ(r, 1);
	CHECK_EQ(res.iRole, (int)ECheatCandGameTxt);
	CHECK_STR(res.iPath, "E:\\PS1\\games\\CHEATS.TXT");
	CHECK_EQ(res.iFormat, (int)ECheatFmtDollar);
}

static void TestPriorityCandidate5Fifth()
{
	TMockFs fs;
	// Candidates 1..4 missing; Candidate 5 (cheatpops.db) present with matching serial
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\cheatpops.db", KCwSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	TCheatFindResult res;
	int r = CheatFindFile("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", &ops, 0, 0, &res);

	CHECK_EQ(r, 1);
	CHECK_EQ(res.iRole, (int)ECheatCandCheatpops);
	CHECK_STR(res.iPath, "E:\\PS1\\cfg\\cheatpops.db");
	CHECK_EQ(res.iFormat, (int)ECheatFmtCw);
}

static void TestRejectsNonExistentFiles()
{
	TMockFs fs; // Empty filesystem
	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	TCheatFindResult res;
	int r = CheatFindFile("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", &ops, 0, 0, &res);
	CHECK_EQ(r, 0);
}

static void TestRejectsInvalidFormatCandidate1FallsBackTo2()
{
	TMockFs fs;
	// Candidate 1 exists but contains binary junk or random text (CheatDetect -> ECheatFmtUnknown)
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\SCES-00344.cht", "This is just a text file with no cheat codes!\nAnother line."));
	// Candidate 2 exists and has valid PCSX cheats
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\Crash.cht", KPcsxSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	TCheatFindResult res;
	int r = CheatFindFile("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", &ops, 0, 0, &res);

	// Candidate 1 must be skipped because format is unknown; Candidate 2 must be picked
	CHECK_EQ(r, 1);
	CHECK_EQ(res.iRole, (int)ECheatCandGameName);
	CHECK_STR(res.iPath, "E:\\PS1\\games\\Crash.cht");
	CHECK_EQ(res.iFormat, (int)ECheatFmtPcsx);
}

static void TestRejectsCheatpopsWithoutMatchingSerial()
{
	TMockFs fs;
	// cheatpops.db exists, but only has cheats for Gran Turismo (SLUS-01000)
	const char *gtDb =
		"_S SLUS-01000\n"
		"_G Gran Turismo\n"
		"_C0 Max Cash\n"
		"_L 80011111 FFFF\n";
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\cheatpops.db", gtDb));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	TCheatFindResult res;
	// We search for Crash Bandicoot (SCES-00344)
	int r = CheatFindFile("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", &ops, 0, 0, &res);

	// cheatpops.db does NOT have SCES-00344, so CheatFindFile must reject it and return 0
	CHECK_EQ(r, 0);
}

static void TestRejectsEmptyFile()
{
	TMockFs fs;
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\SCES-00344.cht", ""));
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\Crash.cht", KPcsxSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	TCheatFindResult res;
	int r = CheatFindFile("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\", &ops, 0, 0, &res);

	CHECK_EQ(r, 1);
	CHECK_EQ(res.iRole, (int)ECheatCandGameName);
}

// ----------------------------------------------------------------------------
// CheatLoadFile and CheatFindAndLoad tests
// ----------------------------------------------------------------------------

static void TestLoadFilePcsx()
{
	TMockFs fs;
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\SCES-00344.cht", KPcsxSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	gLastLoadedPath = "";
	CheatSetCoreLoader(TestLoader);

	char srcBuf[4096];
	char outBuf[4096];
	TCheatStats st;

	int r = CheatLoadFile("E:\\PS1\\games\\SCES-00344.cht", ECheatFmtPcsx, "SCES-00344",
		"E:\\PS1\\cfg\\active.cht", &ops, srcBuf, sizeof(srcBuf), outBuf, sizeof(outBuf), &st);

	CHECK_EQ(r, 1);
	CHECK_EQ(st.iCheats, 2);
	CHECK_EQ(st.iEnabled, 1);
	CHECK_EQ(st.iCodes, 2);
	CHECK_STR(fs.iLastWrittenPath, "E:\\PS1\\cfg\\active.cht");
	CHECK_STR(gLastLoadedPath, "E:\\PS1\\cfg\\active.cht");

	// Output in active.cht must be normalized PCSX text
	CHECK(fs.iLastWrittenContent.find("[Infinite Lives]\n800AAAAA 0009\n") != std::string::npos);
	CHECK(fs.iLastWrittenContent.find("[*Infinite Health]\n800BBBBB 03E7\n") != std::string::npos);

	CheatSetCoreLoader(0);
}

static void TestLoadFileDollar()
{
	TMockFs fs;
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\CHEATS.TXT", KDollarSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	gLastLoadedPath = "";
	CheatSetCoreLoader(TestLoader);

	char srcBuf[4096];
	char outBuf[4096];
	TCheatStats st;

	int r = CheatLoadFile("E:\\PS1\\games\\CHEATS.TXT", ECheatFmtDollar, "SCES-00344",
		"E:\\PS1\\cfg\\active.cht", &ops, srcBuf, sizeof(srcBuf), outBuf, sizeof(outBuf), &st);

	CHECK_EQ(r, 1);
	CHECK_EQ(st.iCheats, 1);
	CHECK_EQ(st.iEnabled, 1); // Dollar format cheats are enabled
	CHECK_EQ(st.iCodes, 1);
	CHECK_STR(fs.iLastWrittenPath, "E:\\PS1\\cfg\\active.cht");
	CHECK_STR(gLastLoadedPath, "E:\\PS1\\cfg\\active.cht");

	CheatSetCoreLoader(0);
}

static void TestLoadFileCheatpopsWithFilter()
{
	TMockFs fs;
	const char *multiDb =
		"_S SLUS-01000\n"
		"_G Gran Turismo\n"
		"_C0 Max Cash\n"
		"_L 80011111 FFFF\n"
		"\n"
		"_S SCES-00344\n"
		"_G Crash Bandicoot\n"
		"_C0 Infinite Lives\n"
		"_L 800AAAAA 0009\n"
		"_C1 Infinite Wumpa\n"
		"_L 800BBBBB 0063\n";
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\cheatpops.db", multiDb));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	gLastLoadedPath = "";
	CheatSetCoreLoader(TestLoader);

	char srcBuf[8192];
	char outBuf[8192];
	TCheatStats st;

	// Load with filter "SCES-00344": only Crash cheats should be extracted
	int r = CheatLoadFile("E:\\PS1\\cfg\\cheatpops.db", ECheatFmtCw, "SCES-00344",
		"E:\\PS1\\cfg\\active.cht", &ops, srcBuf, sizeof(srcBuf), outBuf, sizeof(outBuf), &st);

	CHECK_EQ(r, 1);
	CHECK_EQ(st.iGames, 2); // 2 _S lines seen in multiDb
	CHECK_EQ(st.iCheats, 2); // only 2 cheats for SCES-00344 written
	CHECK_EQ(st.iEnabled, 1);
	CHECK_EQ(st.iCodes, 2);

	// Check that Gran Turismo cheat is NOT in active.cht
	CHECK(fs.iLastWrittenContent.find("Max Cash") == std::string::npos);
	CHECK(fs.iLastWrittenContent.find("80011111") == std::string::npos);

	// Check that Crash cheats ARE in active.cht
	CHECK(fs.iLastWrittenContent.find("[Infinite Lives]\n800AAAAA 0009\n") != std::string::npos);
	CHECK(fs.iLastWrittenContent.find("[*Infinite Wumpa]\n800BBBBB 0063\n") != std::string::npos);

	CheatSetCoreLoader(0);
}

static void TestFindAndLoadEndToEnd()
{
	TMockFs fs;
	// Candidate 3 is present
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\SCES-00344.cht", KPcsxSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	gLastLoadedPath = "";
	CheatSetCoreLoader(TestLoader);

	char srcBuf[4096];
	char outBuf[4096];
	TCheatStats st;

	int r = CheatFindAndLoad("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\",
		"E:\\PS1\\cfg\\active.cht", &ops, srcBuf, sizeof(srcBuf), outBuf, sizeof(outBuf), &st);

	CHECK_EQ(r, 1);
	CHECK_EQ(st.iCheats, 2);
	CHECK_STR(fs.iLastWrittenPath, "E:\\PS1\\cfg\\active.cht");
	CHECK_STR(gLastLoadedPath, "E:\\PS1\\cfg\\active.cht");

	CheatSetCoreLoader(0);
}

static void TestFindAndLoadNoMatch()
{
	TMockFs fs; // Empty
	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	gLastLoadedPath = "";
	CheatSetCoreLoader(TestLoader);

	char srcBuf[4096];
	char outBuf[4096];
	TCheatStats st;
	st.iCheats = 123;

	int r = CheatFindAndLoad("E:\\PS1\\games\\Crash.bin", "SCES-00344", "E:\\PS1\\cfg\\",
		"E:\\PS1\\cfg\\active.cht", &ops, srcBuf, sizeof(srcBuf), outBuf, sizeof(outBuf), &st);

	CHECK_EQ(r, 0);
	CHECK_EQ(st.iCheats, 0);
	CHECK_STR(gLastLoadedPath, "");

	CheatSetCoreLoader(0);
}

static void TestFindAndLoadNullCheatsDir()
{
	TMockFs fs;
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\cfg\\SCES-00344.cht", KPcsxSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	gLastLoadedPath = "";
	CheatSetCoreLoader(TestLoader);

	char srcBuf[4096];
	char outBuf[4096];
	TCheatStats st;

	int r = CheatFindAndLoad("E:\\PS1\\games\\Crash.bin", "SCES-00344", 0,
		"E:\\PS1\\cfg\\active.cht", &ops, srcBuf, sizeof(srcBuf), outBuf, sizeof(outBuf), &st);

	CHECK_EQ(r, 1);
	CHECK_EQ(st.iCheats, 2);
	CHECK_STR(fs.iLastWrittenPath, "E:\\PS1\\cfg\\active.cht");

	CheatSetCoreLoader(0);
}

static void TestLoadFileBufferTooSmall()
{
	TMockFs fs;
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\Crash.cht", KPcsxSample));

	TCheatFileOps ops = { MockExists, MockRead, MockWrite, &fs };
	char srcBuf[4096];
	char outBuf[8];
	TCheatStats st;

	int r = CheatLoadFile("E:\\PS1\\games\\Crash.cht", ECheatFmtPcsx, "SCES-00344",
		"E:\\PS1\\cfg\\active.cht", &ops, srcBuf, sizeof(srcBuf), outBuf, sizeof(outBuf), &st);

	CHECK_EQ(r, -1);
}

static int MockFailWrite(const char *, const char *, int, void *)
{
	return -1;
}

static void TestLoadFileWriteFailed()
{
	TMockFs fs;
	fs.iFiles.push_back(TMockEntry("E:\\PS1\\games\\Crash.cht", KPcsxSample));

	TCheatFileOps ops = { MockExists, MockRead, MockFailWrite, &fs };
	char srcBuf[4096];
	char outBuf[4096];
	TCheatStats st;

	int r = CheatLoadFile("E:\\PS1\\games\\Crash.cht", ECheatFmtPcsx, "SCES-00344",
		"E:\\PS1\\cfg\\active.cht", &ops, srcBuf, sizeof(srcBuf), outBuf, sizeof(outBuf), &st);

	CHECK_EQ(r, -1);
}

// ----------------------------------------------------------------------------
// Main runner
// ----------------------------------------------------------------------------

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--list") == 0) gListOnly = 1;
		else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) gOnly = argv[++i];
	}

	RUN(TestCandidateListStandard);
	RUN(TestCandidateListSlashSeparators);
	RUN(TestCandidateListNoDir);
	RUN(TestCandidateListNoExtension);
	RUN(TestCandidateListMultipleDots);
	RUN(TestCandidateListMissingSerial);
	RUN(TestCandidateListEmptyCheatsDir);
	RUN(TestCandidateListMaxLimit);

	RUN(TestCwHasSerialMatches);
	RUN(TestCwHasSerialDifferentGame);
	RUN(TestCwHasSerialMultipleGames);
	RUN(TestCwHasSerialBOMAndComments);
	RUN(TestCwHasSerialInvalidInputs);

	RUN(TestPriorityCandidate1First);
	RUN(TestPriorityCandidate2Second);
	RUN(TestPriorityCandidate3Third);
	RUN(TestPriorityCandidate4Fourth);
	RUN(TestPriorityCandidate5Fifth);

	RUN(TestRejectsNonExistentFiles);
	RUN(TestRejectsInvalidFormatCandidate1FallsBackTo2);
	RUN(TestRejectsCheatpopsWithoutMatchingSerial);
	RUN(TestRejectsEmptyFile);

	RUN(TestLoadFilePcsx);
	RUN(TestLoadFileDollar);
	RUN(TestLoadFileCheatpopsWithFilter);
	RUN(TestFindAndLoadEndToEnd);
	RUN(TestFindAndLoadNoMatch);
	RUN(TestFindAndLoadNullCheatsDir);
	RUN(TestLoadFileBufferTooSmall);
	RUN(TestLoadFileWriteFailed);

	if (gListOnly) return 0;
	printf("cheatfind: %d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
}
