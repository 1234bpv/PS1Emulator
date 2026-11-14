// Portable reader of cheat-code files: turns the three text formats users actually have
// into the one the emulator core loads (PCSX .cht), and lets a menu list and switch the
// cheats of such a text.
//
// Platform-free like touchcore: C++98, no STL, no exceptions, no RTTI, no heap, no
// floating point, no platform headers, no file access. The caller reads the file into
// memory and gives the bytes; every result goes into a caller-owned buffer. Texts are
// byte strings with an explicit length; they need not end with a zero and a zero byte
// inside counts as an ordinary character. Nothing here interprets what a code does.
#ifndef PS1_CHEATFILE_H
#define PS1_CHEATFILE_H

enum TCheatFormat
	{
	ECheatFmtUnknown = 0,
	ECheatFmtPcsx = 1,     // [name] / [*name] headers, "AAAAAAAA VVVV" lines
	ECheatFmtDollar = 2,   // CHEATS.TXT of POPS-style loaders: #name, $AAAAAAAA VVVV
	ECheatFmtCw = 3        // cwcheat cheatpops.db: _S serial, _G title, _C0 name, _L code
	};

enum
	{
	KCheatNameMax = 63,    // bytes of a cheat name kept; the rest of a longer name is dropped
	KCheatTextMax = 4194304 // limits input text only: a text longer than this is taken as its first 4194304 bytes
	};

struct TCheatStats
	{
	int iCheats;      // cheats written to the output
	int iEnabled;     // of them, written as enabled
	int iCodes;       // code lines written
	int iBadLines;    // lines that should have been a code or a header and were not
	int iDropped;     // well-formed things left out on purpose (see each format)
	int iGames;       // ECheatFmtCw: _S lines seen; 0 for the other formats
	};

// --- Rules shared by everything below ------------------------------------------
//
// Lines. A text is cut into lines at "\r\n", "\n" or a lone "\r"; the last line needs
// no terminator. The three bytes EF BB BF at the very start of the text are skipped.
// Every line is trimmed: spaces and tabs are removed from both ends. A blank line is
// one that is empty after trimming. "Starts with" below refers to the trimmed line.
//
// Hex digit: 0-9, a-f, A-F. Blank: a space or a tab.
//
// Code. A string is a code when, after trimming, it is: an optional "0x" or "0X", 1
// to 8 hex digits (the address), one or more blanks, an optional "0x" or "0X", 1 to 4
// hex digits (the value), and nothing else. A code is written out as the address in
// exactly 8 upper-case hex digits (zero-padded on the left), one space, the value in
// exactly 4 upper-case hex digits, and "\n".
//
// Name. Taken from the source after trimming, then: bytes below 0x20 and the byte 0x7F
// are removed; '[' becomes '(' and ']' becomes ')'; leading '*' and blank bytes are
// removed together for as long as they run; the result is cut to its first
// KCheatNameMax bytes (and trimmed at the end, in case the cut left a blank). An empty
// result is replaced by "Cheat". Bytes of 0x80 and above pass unchanged (the file's
// own encoding).
//
// Output. Normalised PCSX text: for every cheat a header line "[name]\n", or
// "[*name]\n" when it is enabled, followed by its code lines. No blank lines, no
// comments, "\n" only. A cheat with no code lines is not written at all and is not
// counted in any counter (neither iCheats, iEnabled, iBadLines, nor iDropped).
//
// Serial. Two game serials are equal when they match after removing every byte that
// is not an ASCII letter or a digit and ignoring letter case ("SLUS-012.22", "slus_01222"
// and "SLUS01222" are all equal; "letter" means ASCII letter only). A serial with no
// letter or digit equals nothing, not even itself.

// Format of a text, decided by its first line that starts with one of: "[" (Pcsx),
// "$" (Dollar), "_S", "_G", "_C" or "_L" (Cw; the letter in either case; no following
// blank required, so e.g. "_Sxyz" gives ECheatFmtCw, while CheatConvert counts it as a
// bad line). Every other line is skipped, so a file may open with comments in any
// style. No such line: ECheatFmtUnknown.
int CheatDetect(const char *aText, int aLen);

// Non-zero when the two serials are equal by the Serial rule. A null pointer is an
// empty serial.
int CheatSerialEqual(const char *aA, int aLenA, const char *aB, int aLenB);

// Converts aText, which is in aFormat, to normalised PCSX text in aOut. Returns the
// number of bytes written, 0 when no cheat came out, or -1 when the result does not
// fit in aCap bytes (aOut is then unspecified, aStats still describes the whole
// input). aFormat ECheatFmtUnknown, or any other value not listed, returns 0 with
// all counters 0. aStats may be null. aText and aOut must not overlap. A null aOut
// is equivalent to capacity 0; negative aLen or aCap is taken as 0; no terminating
// zero is written. A bad line does not close or change the currently open cheat.
//
// ECheatFmtPcsx. A line starting with '#', ';', '/' or '"' is a comment. A line
// starting with '[' is a header if it also holds a ']' later; the name is what lies
// between the '[' and the LAST ']' of the line, and the cheat is enabled when that
// text, trimmed, starts with '*'. A '[' line without ']' is a bad line and does not
// start a cheat (codes after it still belong to the previous cheat). Any other
// non-blank line must be a code: if it is not, it is a bad line; if it is, it joins
// the cheat started by the last header, or is dropped (iDropped) when no header came
// before it.
//
// ECheatFmtDollar. A line starting with "//" or ';' is a comment. A line starting
// with '#' sets the name for the codes that follow (the text after the '#'); it does
// not by itself make a cheat. A line starting with '$': if the rest of the line is a
// code, it joins the current cheat, which is the one opened by the nearest '#' line
// above, or one named "Cheat" when there is none; all the '$' codes between two '#'
// lines form one cheat. If the rest is not a code (for example "$SAFEMODE", or a lone
// '$' without a code) the line is dropped (iDropped) when it holds no blank after
// trimming, and is a bad line otherwise. Any other non-blank line: dropped when it is
// a code (a code without '$' is one the user switched off), bad otherwise. Every cheat
// written is enabled.
//
// ECheatFmtCw. Tags are matched in either letter case and must be followed by a
// blank or the end of the line. "_S" starts a game: the rest of the line is its
// serial. "_G" (the game title) is ignored. "_C" followed by one digit starts a
// cheat named by the rest of the line; digit 0 is disabled, any other digit
// enabled. "_L" must be followed by a code, which joins the current cheat; a "_L"
// line that is not a code is a bad line; a "_L" code before any "_C" of its game is
// dropped. A line starting with '#', ';' or "//" is a comment; any other non-blank
// line is a bad line. Games are filtered by aSerial: when aSerial has at least one
// letter or digit, only games whose serial equals it are written and everything
// inside other games is skipped without counting (iBadLines and iDropped count only
// inside matching games; lines before the first "_S" belong to no game and are
// skipped the same way). When aSerial is null or has no letter or digit, every game
// is written. If two cheats written have the same name, both are kept.
int CheatConvert(const char *aText, int aLen, int aFormat, const char *aSerial,
	int aSerialLen, char *aOut, int aCap, TCheatStats *aStats);

// --- Listing and switching, on normalised PCSX text ------------------------------
//
// These take text as CheatConvert writes it. On any other text they must not read or
// write outside the buffer; what they return is then unspecified.

// Number of header lines.
int CheatCount(const char *aText, int aLen);

// Describes cheat aIndex (0-based, in file order). Copies its name from the header
// as is (without re-normalising it), omitting the enclosing brackets and any
// leading '*', into aName, at most aNameCap - 1 bytes, and ends it with a zero
// (nothing is written when aNameCap <= 0). *aEnabled becomes 1 or 0 and *aCodes
// the number of code lines; either pointer may be null. Returns the full length of
// the name, or -1 when there is no such cheat (the outputs are then untouched).
int CheatInfo(const char *aText, int aLen, int aIndex, char *aName, int aNameCap,
	int *aEnabled, int *aCodes);

// Switches cheat aIndex on (aOn non-zero) or off by adding or removing the '*' of its
// header, moving the rest of the text. Returns the new length of the text. Returns
// aLen unchanged when the cheat already is in that state, and -1, with the text
// untouched, when aLen > aCap, there is no such cheat or switching on needs more
// than aCap bytes.
int CheatSetEnabled(char *aText, int aLen, int aCap, int aIndex, int aOn);

#endif
