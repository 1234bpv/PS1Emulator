// Portable search and loading of cheat-code files.
// Platform-free: C++98, no STL, no exceptions, no RTTI, no heap, no platform headers.
#include "cheatfind.h"

#ifdef __cplusplus
extern "C" {
#endif
void LoadCheats(const char *filename);
#ifdef __cplusplus
}
#endif

static TCheatCoreLoaderFn g_customLoader = 0;

void CheatSetCoreLoader(TCheatCoreLoaderFn aFn)
	{
	g_customLoader = aFn;
	}

// --- Minimal string helpers without standard library headers ----------------

static int chfStrLen(const char *aStr)
	{
	if (!aStr)
		return 0;
	int n = 0;
	while (aStr[n])
		n++;
	return n;
	}

static const char *chfStrChr(const char *aStr, char aChar)
	{
	if (!aStr)
		return 0;
	while (*aStr)
		{
		if (*aStr == aChar)
			return aStr;
		aStr++;
		}
	return (aChar == '\0') ? aStr : 0;
	}

static void chfMemSet(void *aDst, int aVal, int aLen)
	{
	char *d = (char *)aDst;
	for (int i = 0; i < aLen; i++)
		d[i] = (char)aVal;
	}

static int HasAlphanumeric(const char *aStr)
	{
	if (!aStr)
		return 0;
	for (int i = 0; aStr[i]; i++)
		{
		const char c = aStr[i];
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
			return 1;
		}
	return 0;
	}

static int SafeCopy(char *aDst, int aCap, const char *aSrc, int aLen)
	{
	if (!aDst || !aSrc || aCap <= 0)
		return 0;
	int i = 0;
	while (i < aLen && i < aCap - 1 && aSrc[i])
		{
		aDst[i] = aSrc[i];
		i++;
		}
	aDst[i] = '\0';
	return i;
	}

static int SafeCat(char *aDst, int aCap, const char *aSrc, int aLen)
	{
	if (!aDst || !aSrc || aCap <= 0)
		return 0;
	int cur = chfStrLen(aDst);
	if (cur >= aCap - 1)
		return cur;
	int i = 0;
	while (i < aLen && (cur + i) < aCap - 1 && aSrc[i])
		{
		aDst[cur + i] = aSrc[i];
		i++;
		}
	aDst[cur + i] = '\0';
	return cur + i;
	}

// --- Candidate list generation ----------------------------------------------

int CheatBuildCandidates(const char *aGamePath, const char *aSerial,
	const char *aCheatsDir, TCheatCandidate *aCandidates, int aMaxCandidates)
	{
	if (!aCandidates || aMaxCandidates <= 0)
		return 0;

	// Detect path separator
	char sep = '\\';
	if (aGamePath && chfStrChr(aGamePath, '/') != 0 && chfStrChr(aGamePath, '\\') == 0)
		sep = '/';
	else if (aCheatsDir && chfStrChr(aCheatsDir, '/') != 0 && chfStrChr(aCheatsDir, '\\') == 0)
		sep = '/';

	// Split aGamePath into image directory and base name
	char imgDir[KCheatPathMax];
	char baseName[KCheatPathMax];
	imgDir[0] = '\0';
	baseName[0] = '\0';

	if (aGamePath && aGamePath[0])
		{
		int lastSep = -1;
		for (int i = 0; aGamePath[i]; i++)
			{
			if (aGamePath[i] == '\\' || aGamePath[i] == '/')
				lastSep = i;
			}
		if (lastSep >= 0)
			SafeCopy(imgDir, sizeof(imgDir), aGamePath, lastSep + 1);

		const char *fn = aGamePath + (lastSep + 1);
		int lastDot = -1;
		for (int j = 0; fn[j]; j++)
			{
			if (fn[j] == '.')
				lastDot = j;
			}
		if (lastDot > 0)
			SafeCopy(baseName, sizeof(baseName), fn, lastDot);
		else if (lastDot == 0)
			SafeCopy(baseName, sizeof(baseName), fn, chfStrLen(fn));
		else
			SafeCopy(baseName, sizeof(baseName), fn, chfStrLen(fn));
		}

	// Prepare cheats directory (ensure trailing separator)
	char cDir[KCheatPathMax];
	cDir[0] = '\0';
	if (aCheatsDir && aCheatsDir[0])
		{
		SafeCopy(cDir, sizeof(cDir), aCheatsDir, chfStrLen(aCheatsDir));
		int cLen = chfStrLen(cDir);
		if (cLen > 0 && cDir[cLen - 1] != '\\' && cDir[cLen - 1] != '/')
			{
			char sStr[2];
			sStr[0] = sep;
			sStr[1] = '\0';
			SafeCat(cDir, sizeof(cDir), sStr, 1);
			}
		}

	// Clean trimmed serial
	char serial[KCheatPathMax];
	serial[0] = '\0';
	int hasSerial = 0;
	if (aSerial && HasAlphanumeric(aSerial))
		{
		int s = 0;
		int e = chfStrLen(aSerial);
		while (s < e && ((unsigned char)aSerial[s] <= ' '))
			s++;
		while (e > s && ((unsigned char)aSerial[e - 1] <= ' '))
			e--;
		if (e > s)
			{
			SafeCopy(serial, sizeof(serial), aSerial + s, e - s);
			hasSerial = HasAlphanumeric(serial);
			}
		}

	int count = 0;

	// 1. <image_dir>\<serial>.cht
	if (hasSerial && count < aMaxCandidates)
		{
		aCandidates[count].iRole = ECheatCandGameSerial;
		SafeCopy(aCandidates[count].iPath, KCheatPathMax, imgDir, chfStrLen(imgDir));
		SafeCat(aCandidates[count].iPath, KCheatPathMax, serial, chfStrLen(serial));
		SafeCat(aCandidates[count].iPath, KCheatPathMax, ".cht", 4);
		count++;
		}

	// 2. <image_dir>\<base_name>.cht
	if (baseName[0] && count < aMaxCandidates)
		{
		aCandidates[count].iRole = ECheatCandGameName;
		SafeCopy(aCandidates[count].iPath, KCheatPathMax, imgDir, chfStrLen(imgDir));
		SafeCat(aCandidates[count].iPath, KCheatPathMax, baseName, chfStrLen(baseName));
		SafeCat(aCandidates[count].iPath, KCheatPathMax, ".cht", 4);
		count++;
		}

	// 3. <cheats_dir>\<serial>.cht
	if (hasSerial && cDir[0] && count < aMaxCandidates)
		{
		aCandidates[count].iRole = ECheatCandDirSerial;
		SafeCopy(aCandidates[count].iPath, KCheatPathMax, cDir, chfStrLen(cDir));
		SafeCat(aCandidates[count].iPath, KCheatPathMax, serial, chfStrLen(serial));
		SafeCat(aCandidates[count].iPath, KCheatPathMax, ".cht", 4);
		count++;
		}

	// 4. <image_dir>\CHEATS.TXT
	if (count < aMaxCandidates)
		{
		aCandidates[count].iRole = ECheatCandGameTxt;
		SafeCopy(aCandidates[count].iPath, KCheatPathMax, imgDir, chfStrLen(imgDir));
		SafeCat(aCandidates[count].iPath, KCheatPathMax, "CHEATS.TXT", 10);
		count++;
		}

	// 5. <cheats_dir>\cheatpops.db
	if (cDir[0] && count < aMaxCandidates)
		{
		aCandidates[count].iRole = ECheatCandCheatpops;
		SafeCopy(aCandidates[count].iPath, KCheatPathMax, cDir, chfStrLen(cDir));
		SafeCat(aCandidates[count].iPath, KCheatPathMax, "cheatpops.db", 12);
		count++;
		}

	return count;
	}

// --- Serial check inside cwcheat text ---------------------------------------

int CheatCwHasSerial(const char *aText, int aLen, const char *aSerial, int aSerialLen)
	{
	if (!aText || aLen <= 0 || !aSerial)
		return 0;
	if (aSerialLen < 0)
		aSerialLen = chfStrLen(aSerial);
	if (!HasAlphanumeric(aSerial))
		return 0;

	int pos = 0;
	if (aLen >= 3 && (unsigned char)aText[0] == 0xEF &&
		(unsigned char)aText[1] == 0xBB && (unsigned char)aText[2] == 0xBF)
		pos = 3;

	while (pos < aLen)
		{
		int s = pos;
		while (pos < aLen && aText[pos] != '\r' && aText[pos] != '\n')
			pos++;
		int e = pos;
		if (pos < aLen && aText[pos] == '\r')
			pos++;
		if (pos < aLen && aText[pos] == '\n')
			pos++;

		while (s < e && (aText[s] == ' ' || aText[s] == '\t'))
			s++;
		while (e > s && (aText[e - 1] == ' ' || aText[e - 1] == '\t'))
			e--;

		if (e - s >= 2)
			{
			const char c0 = aText[s];
			const char c1 = aText[s + 1];
			if (c0 == '_' && (c1 == 's' || c1 == 'S'))
				{
				if (e - s == 2 || aText[s + 2] == ' ' || aText[s + 2] == '\t')
					{
					int serialStart = s + 2;
					while (serialStart < e && (aText[serialStart] == ' ' || aText[serialStart] == '\t'))
						serialStart++;
					if (CheatSerialEqual(aText + serialStart, e - serialStart, aSerial, aSerialLen))
						return 1;
					}
				}
			}
		}

	return 0;
	}

// --- Candidate verification and search --------------------------------------

int CheatFindFile(const char *aGamePath, const char *aSerial,
	const char *aCheatsDir, const TCheatFileOps *aOps,
	char *aBuf, int aCap,
	TCheatFindResult *aResult)
	{
	if (!aOps || !aOps->iRead)
		return -1;

	TCheatCandidate candidates[KCheatCandidateMax];
	const int count = CheatBuildCandidates(aGamePath, aSerial, aCheatsDir, candidates, KCheatCandidateMax);
	if (count <= 0)
		return 0;

	char stackProbe[1024];
	char *readBuf = (aBuf && aCap > 0) ? aBuf : stackProbe;
	int readCap = (aBuf && aCap > 0) ? aCap : (int)sizeof(stackProbe);

	for (int i = 0; i < count; i++)
		{
		const char *path = candidates[i].iPath;

		if (aOps->iExists)
			{
			if (!aOps->iExists(path, aOps->iUserData))
				continue;
			}

		int n = aOps->iRead(path, readBuf, readCap, aOps->iUserData);
		if (n <= 0)
			continue;

		const int fmt = CheatDetect(readBuf, n);
		if (fmt == ECheatFmtUnknown)
			continue;

		// For cwcheat databases (cheatpops.db or .cht in cw format), verify game serial
		if (fmt == ECheatFmtCw)
			{
			if (!aSerial || !HasAlphanumeric(aSerial))
				continue;
			if (!CheatCwHasSerial(readBuf, n, aSerial, -1))
				continue;
			}

		if (aResult)
			{
			SafeCopy(aResult->iPath, KCheatPathMax, path, chfStrLen(path));
			aResult->iFormat = fmt;
			aResult->iRole = candidates[i].iRole;
			}
		return 1;
		}

	return 0;
	}

// --- Loading and conversion -------------------------------------------------

int CheatLoadFile(const char *aFilePath, int aFormat, const char *aSerial,
	const char *aActivePath, const TCheatFileOps *aOps,
	char *aSrcBuf, int aSrcCap, char *aOutBuf, int aOutCap,
	TCheatStats *aStats)
	{
	if (!aFilePath || !aFilePath[0] || aFormat == ECheatFmtUnknown)
		return -1;
	if (!aOps || !aOps->iRead || !aOps->iWrite)
		return -1;
	if (!aSrcBuf || aSrcCap <= 0 || !aOutBuf || aOutCap <= 0)
		return -1;

	int srcLen = aOps->iRead(aFilePath, aSrcBuf, aSrcCap, aOps->iUserData);
	if (srcLen <= 0)
		return -1;

	const char *filterSerial = (aFormat == ECheatFmtCw) ? aSerial : 0;
	const int filterLen = filterSerial ? chfStrLen(filterSerial) : 0;

	TCheatStats st;
	int outLen = CheatConvert(aSrcBuf, srcLen, aFormat, filterSerial, filterLen, aOutBuf, aOutCap, &st);

	if (aStats)
		*aStats = st;

	if (outLen < 0)
		return -1;
	if (outLen == 0)
		return 0;

	const char *active = (aActivePath && aActivePath[0]) ? aActivePath : KCheatActivePathDefault;
	int writeRc = aOps->iWrite(active, aOutBuf, outLen, aOps->iUserData);
	if (writeRc != outLen)
		return -1;

	if (g_customLoader)
		g_customLoader(active);
	else
		LoadCheats(active);

	return 1;
	}

int CheatFindAndLoad(const char *aGamePath, const char *aSerial,
	const char *aCheatsDir, const char *aActivePath,
	const TCheatFileOps *aOps,
	char *aSrcBuf, int aSrcCap, char *aOutBuf, int aOutCap,
	TCheatStats *aStats)
	{
	const char *cheatsDir = (aCheatsDir && aCheatsDir[0]) ? aCheatsDir : KCheatDirDefault;
	TCheatFindResult found;
	int r = CheatFindFile(aGamePath, aSerial, cheatsDir, aOps, aSrcBuf, aSrcCap, &found);
	if (r < 0)
		{
		if (aStats)
			chfMemSet(aStats, 0, sizeof(*aStats));
		return r;
		}
	if (r == 0)
		{
		if (aStats)
			chfMemSet(aStats, 0, sizeof(*aStats));
		return 0;
		}

	return CheatLoadFile(found.iPath, found.iFormat, aSerial, aActivePath, aOps,
		aSrcBuf, aSrcCap, aOutBuf, aOutCap, aStats);
	}
