// Portable search and loading of cheat-code files: finds candidate cheat files
// for a running game by priority, verifies file format via CheatDetect, filters
// cheatpops.db by serial, converts to normalized PCSX format, writes to active.cht,
// and invokes LoadCheats.
//
// Platform-free like cheatfile: C++98, no STL, no exceptions, no RTTI, no heap,
// no floating point, no platform headers, no direct file access.
// File I/O is abstracted via TCheatFileOps, all buffers are caller-owned.
#ifndef PS1_CHEATFIND_H
#define PS1_CHEATFIND_H

#include "cheatfile.h"

enum
	{
	KCheatPathMax = 260,
	KCheatCandidateMax = 5
	};

enum TCheatCandidateRole
	{
	ECheatCandGameSerial = 1, // <image_dir>\<serial>.cht
	ECheatCandGameName   = 2, // <image_dir>\<game_name>.cht
	ECheatCandDirSerial  = 3, // <cheats_dir>\<serial>.cht
	ECheatCandGameTxt    = 4, // <image_dir>\CHEATS.TXT
	ECheatCandCheatpops  = 5  // <cheats_dir>\cheatpops.db
	};

#define KCheatActivePathDefault "E:\\PS1\\cfg\\active.cht"
#define KCheatDirDefault        "E:\\PS1\\cfg\\"

struct TCheatCandidate
	{
	char iPath[KCheatPathMax];
	int iRole; // TCheatCandidateRole
	};

struct TCheatFindResult
	{
	char iPath[KCheatPathMax];
	int iFormat; // ECheatFmtPcsx, ECheatFmtDollar, ECheatFmtCw
	int iRole;   // TCheatCandidateRole
	};

// File operations abstraction for testing and platform adaptation.
struct TCheatFileOps
	{
	// Non-zero if file exists, 0 otherwise. If null, iRead returning >= 0 is used.
	int (*iExists)(const char *aPath, void *aUserData);

	// Reads up to aCap bytes into aBuf. Returns bytes read (>= 0), or -1 on error.
	int (*iRead)(const char *aPath, char *aBuf, int aCap, void *aUserData);

	// Writes aLen bytes from aBuf into aPath. Returns bytes written (>= 0), or -1 on error.
	int (*iWrite)(const char *aPath, const char *aBuf, int aLen, void *aUserData);

	void *iUserData;
	};

// Builds candidate list for a given game path, serial (CdromId), and cheats directory.
// Candidates are placed in priority order 1..5:
//   1. <image_dir>\<serial>.cht
//   2. <image_dir>\<base_name>.cht
//   3. <cheats_dir>\<serial>.cht
//   4. <image_dir>\CHEATS.TXT
//   5. <cheats_dir>\cheatpops.db
// If aSerial is empty / has no alphanumeric characters, rules 1 and 3 are omitted.
// Returns number of candidates written (0 to 5).
int CheatBuildCandidates(const char *aGamePath, const char *aSerial,
	const char *aCheatsDir, TCheatCandidate *aCandidates, int aMaxCandidates);

// Checks if aText (of length aLen) contains a cwcheat game header (_S ...)
// matching aSerial via CheatSerialEqual.
// Returns 1 if found, 0 if not found.
int CheatCwHasSerial(const char *aText, int aLen, const char *aSerial, int aSerialLen);

// Finds the best matching cheat file among candidates.
// Checks existence and validates format via CheatDetect. For cwcheat databases
// (cheatpops.db), also checks for the game serial via CheatCwHasSerial.
// aBuf / aCap: optional buffer for reading candidate data. If aBuf is null or
// aCap <= 0, an internal stack probe buffer (1024 bytes) is used.
// If a matching candidate is found, populates aResult and returns 1.
// Returns 0 if no matching candidate found, or negative on invalid arguments.
int CheatFindFile(const char *aGamePath, const char *aSerial,
	const char *aCheatsDir, const TCheatFileOps *aOps,
	char *aBuf, int aCap,
	TCheatFindResult *aResult);

// Optional custom core loader hook. If null, LoadCheats() is called.
typedef void (*TCheatCoreLoaderFn)(const char *aActivePath);
void CheatSetCoreLoader(TCheatCoreLoaderFn aFn);

// Loads and converts the cheat file at aFilePath into aActivePath, then calls LoadCheats.
// aSrcBuf / aSrcCap: buffer for reading source file.
// aOutBuf / aOutCap: buffer for normalized PCSX text.
// Filters by aSerial for ECheatFmtCw, NULL for other formats.
// Returns 1 if cheats were converted and loaded, 0 if 0 cheats converted,
// or negative on error. If aStats is non-null, it receives the conversion stats.
int CheatLoadFile(const char *aFilePath, int aFormat, const char *aSerial,
	const char *aActivePath, const TCheatFileOps *aOps,
	char *aSrcBuf, int aSrcCap, char *aOutBuf, int aOutCap,
	TCheatStats *aStats);

// High-level: finds cheat file via CheatFindFile and loads it via CheatLoadFile.
// If aActivePath is null, defaults to KCheatActivePathDefault ("E:\\PS1\\cfg\\active.cht").
// If aCheatsDir is null, defaults to KCheatDirDefault ("E:\\PS1\\cfg\\").
// Returns 1 if cheats loaded, 0 if no candidate found or 0 cheats converted,
// or negative on error.
int CheatFindAndLoad(const char *aGamePath, const char *aSerial,
	const char *aCheatsDir, const char *aActivePath,
	const TCheatFileOps *aOps,
	char *aSrcBuf, int aSrcCap, char *aOutBuf, int aOutCap,
	TCheatStats *aStats);

#endif
