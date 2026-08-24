// Game import into emulator folder: .cue parsing, image building into PBP with zeroed
// ECC, or simple transfer. Runs in a separate thread - converting a 700 MB
// disc takes minutes, while the menu must remain responsive and cancelable.
//
// Destination format is described in tools-patches/psxpack.py (pbpz mode); the rationale
// for why zeroing ECC yields the same 58.5% without modifying the reader is documented there.

#ifndef IMGCONV_H
#define IMGCONV_H

#include <e32base.h>
#include <f32file.h>
#include <badesca.h>

// Sequential byte source: file on card or archive member. Does not rewind -
// 7z/rar unpackers cannot do so without a second pass.
class MConvSrc
	{
public:
	virtual ~MConvSrc() { }
	// Reads up to aLen bytes. Returns bytes read (0 - end of stream) or error code < 0.
	virtual TInt Read(TUint8 *aBuf, TInt aLen) = 0;
	// Skips aBytes forward. For an archive this performs read-and-discard.
	virtual TInt Skip(TInt aBytes) = 0;
	virtual TInt Size() const = 0;
	};

// Collection of files from which tracks are taken: directory on disk or archive. .cue references
// .bin by name, so named access is required rather than by index.
class MConvVol
	{
public:
	virtual ~MConvVol() { }
	// Names of all members (archive contents for archive, single given file for directory).
	virtual const MDesCArray &Entries() const = 0;
	// Opens an entry; caller takes ownership.
	virtual TInt Open(const TDesC &aName, MConvSrc *&aOut) = 0;
	virtual TInt Size(const TDesC &aName, TInt &aOut) = 0;
	};

// Creates volume from path: archive (.zip/.7z/.rar) or directory with file. Archive
// implementation is in archive.cpp; directory in imgconv.cpp.
TInt ConvOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);
TInt ArcOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);
TInt SzOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);    // arc7z.cpp
TInt RarOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);   // arcrar.cpp

// Log line in worker thread log (E:\PS1\logs\ps1_import.jsonl) from other files.
void ConvLogVal(const char *aTag, TInt aVal);

enum TConvState { EConvIdle = 0, EConvRun, EConvDone, EConvErr };

enum TConvMode
	{
	EConvModePbp = 0,   // compress into .pbp with zeroed ECC (irreversible)
	EConvModeCopy       // copy as is
	};

struct TConvStat
	{
	TInt iState;
	TInt iFileNo;       // 1-based, index of file in queue
	TInt iFileCount;
	TInt iPercent;      // current file
	TInt iErr;
	TBuf<64> iName;
	TBuf<64> iMsg;      // text of last error or summary
	};

// Starts queue processing in a separate thread. aFiles are full paths.
TInt ConvStart(const MDesCArray &aFiles, TConvMode aMode, TBool aDeleteSrc,
               const TDesC &aDstDir);
void ConvStat(TConvStat &aOut);
void ConvCancel();
// Cleans up thread after EConvDone/EConvErr. Safe to call repeatedly.
void ConvRelease();

// Extraction of a single file (BIOS) from archive or copy to target folder.
// Synchronous: BIOS dump is 512 KB, a separate thread here would be gratuitous.
TInt ConvExtractOne(RFs &aFs, const TDesC &aPath, const TDesC &aMember,
                    const TDesC &aDstDir, TDes &aOutName);

TBool ConvIsArchiveName(const TDesC &aName);

#endif
