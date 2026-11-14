#include "cheatfile.h"

// Writer of CheatConvert. Bytes are counted even when they do not fit into aOut, so that
// the stats and the -1 result describe the whole input.
struct TChfWriter
	{
	char *iOut;
	int iCap;
	int iLen;
	int iOpen;      // a cheat is open: its codes go into it
	int iHeader;    // the open cheat's header is still to be written
	int iEnabled;   // the open cheat is written with "*"
	int iNameLen;
	char iName[KCheatNameMax];
	TCheatStats iSt;
	};

static const char KChfDefName[] = "Cheat";
static const char KChfHex[] = "0123456789ABCDEF";

// Bytes are read unsigned: plain char is signed on the host and unsigned on the phone.
static int chfByte(const char *aText, int aPos)
	{
	return (unsigned char)aText[aPos];
	}

static int chfIsBlank(int aC)
	{
	return aC == ' ' || aC == '\t';
	}

static int chfIsDigit(int aC)
	{
	return aC >= '0' && aC <= '9';
	}

static int chfIsLetter(int aC)
	{
	return (aC >= 'a' && aC <= 'z') || (aC >= 'A' && aC <= 'Z');
	}

static int chfIsHex(int aC)
	{
	return chfIsDigit(aC) || (aC >= 'a' && aC <= 'f') || (aC >= 'A' && aC <= 'F');
	}

static int chfLower(int aC)
	{
	return (aC >= 'A' && aC <= 'Z') ? aC + ('a' - 'A') : aC;
	}

static unsigned chfHexVal(int aC)
	{
	if (chfIsDigit(aC))
		return (unsigned)(aC - '0');
	if (aC >= 'a' && aC <= 'f')
		return (unsigned)(aC - 'a' + 10);
	return (unsigned)(aC - 'A' + 10);
	}

// Length of a text as given: a null pointer or a negative length is an empty text.
static int chfSpan(const char *aText, int aLen)
	{
	return (aText != 0 && aLen > 0) ? aLen : 0;
	}

// A text longer than KCheatTextMax is taken as its first KCheatTextMax bytes.
static int chfTextLen(const char *aText, int aLen)
	{
	const int n = chfSpan(aText, aLen);
	return n > KCheatTextMax ? KCheatTextMax : n;
	}

// The three bytes EF BB BF at the very start are a byte order mark and are skipped.
static int chfBodyStart(const char *aText, int aLen)
	{
	if (aLen >= 3 && chfByte(aText, 0) == 0xEF && chfByte(aText, 1) == 0xBB && chfByte(aText, 2) == 0xBF)
		return 3;
	return 0;
	}

// Next line from aPos on: [aS, aE) is the line without its terminator, and aPos moves past
// the terminator. Returns 0 at the end of the text.
static int chfLine(const char *aText, int aLen, int &aPos, int &aS, int &aE)
	{
	if (aPos >= aLen)
		return 0;
	aS = aPos;
	while (aPos < aLen && chfByte(aText, aPos) != '\n' && chfByte(aText, aPos) != '\r')
		aPos++;
	aE = aPos;
	if (aPos < aLen)
		{
		if (chfByte(aText, aPos) == '\r' && aPos + 1 < aLen && chfByte(aText, aPos + 1) == '\n')
			aPos++;
		aPos++;
		}
	return 1;
	}

// Shrinks [aS, aE) by blanks at both ends.
static void chfTrim(const char *aText, int &aS, int &aE)
	{
	while (aS < aE && chfIsBlank(chfByte(aText, aS)))
		aS++;
	while (aE > aS && chfIsBlank(chfByte(aText, aE - 1)))
		aE--;
	}

static int chfHasBlank(const char *aText, int aS, int aE)
	{
	for (int p = aS; p < aE; p++)
		if (chfIsBlank(chfByte(aText, p)))
			return 1;
	return 0;
	}

// Position of the last aC in [aS, aE), or -1.
static int chfLast(const char *aText, int aS, int aE, int aC)
	{
	for (int p = aE - 1; p >= aS; p--)
		if (chfByte(aText, p) == aC)
			return p;
	return -1;
	}

// A tag ends at the end of its line or at a blank.
static int chfEndOrBlank(const char *aText, int aPos, int aE)
	{
	return aPos >= aE || chfIsBlank(chfByte(aText, aPos));
	}

// Skips "0x" or "0X" at aPos when both bytes lie before aE.
static int chfPrefix(const char *aText, int aPos, int aE)
	{
	if (aPos + 1 < aE && chfByte(aText, aPos) == '0' && (chfByte(aText, aPos + 1) == 'x' || chfByte(aText, aPos + 1) == 'X'))
		return aPos + 2;
	return aPos;
	}

// Code rule: after trimming, an optional "0x" or "0X", 1 to 8 hex digits, one or more blanks,
// an optional "0x" or "0X", 1 to 4 hex digits, and nothing else.
static int chfCode(const char *aText, int aS, int aE, unsigned &aAddr, unsigned &aVal)
	{
	int p = aS;
	int e = aE;
	chfTrim(aText, p, e);
	p = chfPrefix(aText, p, e);
	int n = 0;
	unsigned addr = 0;
	while (p < e && chfIsHex(chfByte(aText, p)))
		{
		if (++n > 8)
			return 0;
		addr = addr * 16 + chfHexVal(chfByte(aText, p));
		p++;
		}
	if (n == 0 || p >= e || !chfIsBlank(chfByte(aText, p)))
		return 0;
	while (p < e && chfIsBlank(chfByte(aText, p)))
		p++;
	p = chfPrefix(aText, p, e);
	n = 0;
	unsigned val = 0;
	while (p < e && chfIsHex(chfByte(aText, p)))
		{
		if (++n > 4)
			return 0;
		val = val * 16 + chfHexVal(chfByte(aText, p));
		p++;
		}
	if (n == 0 || p != e)
		return 0;
	aAddr = addr;
	aVal = val;
	return 1;
	}

// Name rule. The source [aS, aE) is trimmed first; the result goes into aName, at most
// KCheatNameMax bytes, and an empty result is KChfDefName.
static int chfName(const char *aText, int aS, int aE, char *aName)
	{
	chfTrim(aText, aS, aE);
	int n = 0;
	int lead = 1;
	for (int p = aS; p < aE && n < KCheatNameMax; p++)
		{
		int c = chfByte(aText, p);
		if (c < 0x20 || c == 0x7F)
			continue;
		if (c == '[')
			c = '(';
		else if (c == ']')
			c = ')';
		if (lead)
			{
			if (c == '*' || chfIsBlank(c))
				continue;
			lead = 0;
			}
		aName[n++] = (char)c;
		}
	while (n > 0 && chfIsBlank(chfByte(aName, n - 1)))
		n--;
	if (n == 0)
		for (n = 0; KChfDefName[n] != 0; n++)
			aName[n] = KChfDefName[n];
	return n;
	}

// Next letter or digit of a serial from aPos on, lower-cased; -1 at the end.
static int chfSerialNext(const char *aText, int aLen, int &aPos)
	{
	while (aPos < aLen)
		{
		const int c = chfByte(aText, aPos);
		aPos++;
		if (chfIsLetter(c) || chfIsDigit(c))
			return chfLower(c);
		}
	return -1;
	}

static int chfHasAlnum(const char *aText, int aLen)
	{
	int p = 0;
	return chfSerialNext(aText, aLen, p) >= 0;
	}

static void chfStatsZero(TCheatStats &aS)
	{
	aS.iCheats = 0;
	aS.iEnabled = 0;
	aS.iCodes = 0;
	aS.iBadLines = 0;
	aS.iDropped = 0;
	aS.iGames = 0;
	}

static void chfWriterInit(TChfWriter &aW, char *aOut, int aCap)
	{
	aW.iOut = aOut;
	aW.iCap = (aOut != 0 && aCap > 0) ? aCap : 0;
	aW.iLen = 0;
	aW.iOpen = 0;
	aW.iHeader = 0;
	aW.iEnabled = 0;
	aW.iNameLen = 0;
	chfStatsZero(aW.iSt);
	}

static void chfPut(TChfWriter &aW, int aC)
	{
	if (aW.iLen < aW.iCap)
		aW.iOut[aW.iLen] = (char)aC;
	aW.iLen++;
	}

// Hex digits of aV, most significant first, zero-padded to aDigits.
static void chfPutHex(TChfWriter &aW, unsigned aV, int aDigits)
	{
	for (int s = (aDigits - 1) * 4; s >= 0; s -= 4)
		chfPut(aW, KChfHex[(aV >> s) & 15u]);
	}

// Opens a cheat named by the name rule over [aS, aE). Its header is written together with its
// first code, so a cheat without codes leaves no trace in the output or in the counters.
static void chfOpen(TChfWriter &aW, const char *aText, int aS, int aE, int aEnabled)
	{
	aW.iOpen = 1;
	aW.iHeader = 1;
	aW.iEnabled = aEnabled;
	aW.iNameLen = chfName(aText, aS, aE, aW.iName);
	}

static void chfEmitHeader(TChfWriter &aW)
	{
	chfPut(aW, '[');
	if (aW.iEnabled)
		chfPut(aW, '*');
	for (int i = 0; i < aW.iNameLen; i++)
		chfPut(aW, aW.iName[i]);
	chfPut(aW, ']');
	chfPut(aW, '\n');
	aW.iHeader = 0;
	aW.iSt.iCheats++;
	if (aW.iEnabled)
		aW.iSt.iEnabled++;
	}

// The caller has checked that a cheat is open.
static void chfEmitCode(TChfWriter &aW, unsigned aAddr, unsigned aVal)
	{
	if (aW.iHeader)
		chfEmitHeader(aW);
	chfPutHex(aW, aAddr, 8);
	chfPut(aW, ' ');
	chfPutHex(aW, aVal, 4);
	chfPut(aW, '\n');
	aW.iSt.iCodes++;
	}

// ECheatFmtPcsx rules of CheatConvert.
static void chfPcsx(const char *aText, int aPos, int aLen, TChfWriter &aW)
	{
	int s = 0;
	int e = 0;
	while (chfLine(aText, aLen, aPos, s, e))
		{
		chfTrim(aText, s, e);
		if (s == e)
			continue;
		const int c = chfByte(aText, s);
		if (c == '#' || c == ';' || c == '/' || c == '"')
			continue;
		if (c == '[')
			{
			const int close = chfLast(aText, s + 1, e, ']');
			if (close < 0)
				{
				aW.iSt.iBadLines++;
				continue;
				}
			int ns = s + 1;
			int ne = close;
			chfTrim(aText, ns, ne);
			chfOpen(aW, aText, s + 1, close, ns < ne && chfByte(aText, ns) == '*');
			continue;
			}
		unsigned addr = 0;
		unsigned val = 0;
		if (!chfCode(aText, s, e, addr, val))
			aW.iSt.iBadLines++;
		else if (aW.iOpen)
			chfEmitCode(aW, addr, val);
		else
			aW.iSt.iDropped++;
		}
	}

// ECheatFmtDollar rules of CheatConvert.
static void chfDollar(const char *aText, int aPos, int aLen, TChfWriter &aW)
	{
	int s = 0;
	int e = 0;
	while (chfLine(aText, aLen, aPos, s, e))
		{
		chfTrim(aText, s, e);
		if (s == e)
			continue;
		const int c = chfByte(aText, s);
		if (c == ';' || (c == '/' && e - s >= 2 && chfByte(aText, s + 1) == '/'))
			continue;
		if (c == '#')
			{
			chfOpen(aW, aText, s + 1, e, 1);
			continue;
			}
		unsigned addr = 0;
		unsigned val = 0;
		if (c == '$')
			{
			if (!chfCode(aText, s + 1, e, addr, val))
				{
				if (chfHasBlank(aText, s, e))
					aW.iSt.iBadLines++;
				else
					aW.iSt.iDropped++;
				}
			else
				{
				if (!aW.iOpen)
					chfOpen(aW, KChfDefName, 0, (int)sizeof(KChfDefName) - 1, 1);
				chfEmitCode(aW, addr, val);
				}
			continue;
			}
		if (chfCode(aText, s, e, addr, val))
			aW.iSt.iDropped++;
		else
			aW.iSt.iBadLines++;
		}
	}

// ECheatFmtCw rules of CheatConvert.
static void chfCw(const char *aText, int aPos, int aLen, const char *aSerial, int aSerialLen, TChfWriter &aW)
	{
	const int filter = chfHasAlnum(aSerial, chfSpan(aSerial, aSerialLen));
	int gameOn = 0;
	int s = 0;
	int e = 0;
	while (chfLine(aText, aLen, aPos, s, e))
		{
		chfTrim(aText, s, e);
		if (s == e)
			continue;
		const int c = chfByte(aText, s);
		const int tag = (c == '_' && e - s >= 2) ? chfLower(chfByte(aText, s + 1)) : 0;
		if (tag == 's' && chfEndOrBlank(aText, s + 2, e))
			{
			aW.iSt.iGames++;
			aW.iOpen = 0;
			gameOn = !filter || CheatSerialEqual(aText + s + 2, e - s - 2, aSerial, aSerialLen);
			continue;
			}
		if (!gameOn)
			continue;
		if (c == '#' || c == ';' || (c == '/' && e - s >= 2 && chfByte(aText, s + 1) == '/'))
			continue;
		if (tag == 'g' && chfEndOrBlank(aText, s + 2, e))
			continue;
		if (tag == 'c' && s + 2 < e && chfIsDigit(chfByte(aText, s + 2)) && chfEndOrBlank(aText, s + 3, e))
			{
			chfOpen(aW, aText, s + 3, e, chfByte(aText, s + 2) != '0');
			continue;
			}
		if (tag == 'l' && chfEndOrBlank(aText, s + 2, e))
			{
			unsigned addr = 0;
			unsigned val = 0;
			if (!chfCode(aText, s + 2, e, addr, val))
				aW.iSt.iBadLines++;
			else if (aW.iOpen)
				chfEmitCode(aW, addr, val);
			else
				aW.iSt.iDropped++;
			continue;
			}
		aW.iSt.iBadLines++;
		}
	}

// Finds header aIndex of normalised PCSX text. Sets aHs and aHe to the trimmed bounds of the
// header line and aPos to the line after it.
static int chfHeader(const char *aText, int aLen, int aIndex, int &aHs, int &aHe, int &aPos)
	{
	int k = -1;
	int s = 0;
	int e = 0;
	aPos = chfBodyStart(aText, aLen);
	while (chfLine(aText, aLen, aPos, s, e))
		{
		chfTrim(aText, s, e);
		if (s < e && chfByte(aText, s) == '[')
			{
			k++;
			if (k == aIndex)
				{
				aHs = s;
				aHe = e;
				return 1;
				}
			}
		}
	return 0;
	}

int CheatDetect(const char *aText, int aLen)
	{
	const int len = chfTextLen(aText, aLen);
	int pos = chfBodyStart(aText, len);
	int s = 0;
	int e = 0;
	while (chfLine(aText, len, pos, s, e))
		{
		chfTrim(aText, s, e);
		if (s == e)
			continue;
		const int c = chfByte(aText, s);
		if (c == '[')
			return ECheatFmtPcsx;
		if (c == '$')
			return ECheatFmtDollar;
		if (c == '_' && e - s >= 2)
			{
			const int t = chfLower(chfByte(aText, s + 1));
			if (t == 's' || t == 'g' || t == 'c' || t == 'l')
				return ECheatFmtCw;
			}
		}
	return ECheatFmtUnknown;
	}

int CheatSerialEqual(const char *aA, int aLenA, const char *aB, int aLenB)
	{
	const int na = chfSpan(aA, aLenA);
	const int nb = chfSpan(aB, aLenB);
	int pa = 0;
	int pb = 0;
	int ca = chfSerialNext(aA, na, pa);
	int cb = chfSerialNext(aB, nb, pb);
	if (ca < 0 || cb < 0)
		return 0;
	while (ca >= 0 && cb >= 0 && ca == cb)
		{
		ca = chfSerialNext(aA, na, pa);
		cb = chfSerialNext(aB, nb, pb);
		}
	return ca < 0 && cb < 0;
	}

int CheatConvert(const char *aText, int aLen, int aFormat, const char *aSerial, int aSerialLen, char *aOut, int aCap, TCheatStats *aStats)
	{
	TChfWriter w;
	chfWriterInit(w, aOut, aCap);
	const int len = chfTextLen(aText, aLen);
	const int start = chfBodyStart(aText, len);
	if (aFormat == ECheatFmtPcsx)
		chfPcsx(aText, start, len, w);
	else if (aFormat == ECheatFmtDollar)
		chfDollar(aText, start, len, w);
	else if (aFormat == ECheatFmtCw)
		chfCw(aText, start, len, aSerial, aSerialLen, w);
	if (aStats)
		*aStats = w.iSt;
	return w.iLen > w.iCap ? -1 : w.iLen;
	}

int CheatCount(const char *aText, int aLen)
	{
	const int len = chfSpan(aText, aLen);
	int pos = chfBodyStart(aText, len);
	int n = 0;
	int s = 0;
	int e = 0;
	while (chfLine(aText, len, pos, s, e))
		{
		chfTrim(aText, s, e);
		if (s < e && chfByte(aText, s) == '[')
			n++;
		}
	return n;
	}

int CheatInfo(const char *aText, int aLen, int aIndex, char *aName, int aNameCap, int *aEnabled, int *aCodes)
	{
	const int len = chfSpan(aText, aLen);
	int hs = 0;
	int he = 0;
	int pos = 0;
	if (!chfHeader(aText, len, aIndex, hs, he, pos))
		return -1;
	const int on = (hs + 1 < he && chfByte(aText, hs + 1) == '*') ? 1 : 0;
	const int ns = hs + 1 + on;
	int ne = chfLast(aText, hs, he, ']');
	if (ne < 0)
		ne = he;
	if (ne < ns)
		ne = ns;
	int codes = 0;
	int s = 0;
	int e = 0;
	while (chfLine(aText, len, pos, s, e))
		{
		chfTrim(aText, s, e);
		if (s == e)
			continue;
		if (chfByte(aText, s) == '[')
			break;
		unsigned addr = 0;
		unsigned val = 0;
		if (chfCode(aText, s, e, addr, val))
			codes++;
		}
	if (aEnabled)
		*aEnabled = on;
	if (aCodes)
		*aCodes = codes;
	if (aName != 0 && aNameCap > 0)
		{
		const int keep = ne - ns < aNameCap ? ne - ns : aNameCap - 1;
		for (int i = 0; i < keep; i++)
			aName[i] = aText[ns + i];
		aName[keep] = 0;
		}
	return ne - ns;
	}

int CheatSetEnabled(char *aText, int aLen, int aCap, int aIndex, int aOn)
	{
	const int cap = aCap > 0 ? aCap : 0;
	const int len = chfSpan(aText, aLen);
	if (len > cap)
		return -1;
	int hs = 0;
	int he = 0;
	int pos = 0;
	if (!chfHeader(aText, len, aIndex, hs, he, pos))
		return -1;
	const int on = (hs + 1 < he && chfByte(aText, hs + 1) == '*') ? 1 : 0;
	if (on == (aOn != 0 ? 1 : 0))
		return len;
	if (aOn == 0)
		{
		for (int p = hs + 1; p + 1 < len; p++)
			aText[p] = aText[p + 1];
		return len - 1;
		}
	if (len + 1 > cap)
		return -1;
	for (int p = len; p > hs + 1; p--)
		aText[p] = aText[p - 1];
	aText[hs + 1] = '*';
	return len + 1;
	}
