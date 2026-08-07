// Імпорт ігор у теку емулятора: розбір .cue, збирання образу в PBP з обнуленим
// ECC або просте перенесення. Працює в окремому потоці - конвертація диска на
// 700 МБ триває хвилини, а меню має лишатись живим і скасовуваним.
//
// Формат призначення описаний у tools-patches/psxpack.py (режим pbpz); там же
// обґрунтування, чому обнулення ECC дає ті самі 58,5% без жодної зміни в читачі.

#ifndef IMGCONV_H
#define IMGCONV_H

#include <e32base.h>
#include <f32file.h>
#include <badesca.h>

// Послідовне джерело байтів: файл на картці або елемент архіву. Назад не
// перемотує - розпакувальники 7z/rar цього й не вміють без повторного проходу.
class MConvSrc
	{
public:
	virtual ~MConvSrc() { }
	// Читає до aLen байтів. Повертає прочитане (0 - кінець) або код помилки < 0.
	virtual TInt Read(TUint8 *aBuf, TInt aLen) = 0;
	// Пропускає aBytes уперед. Для архіву це читання з викиданням.
	virtual TInt Skip(TInt aBytes) = 0;
	virtual TInt Size() const = 0;
	};

// Набір файлів, з якого беруть доріжки: тека на диску або архів. .cue посилається
// на .bin за іменем, тож потрібен саме доступ за іменем, а не за номером.
class MConvVol
	{
public:
	virtual ~MConvVol() { }
	// Імена всіх елементів (для архіву - вміст, для теки - один заданий файл).
	virtual const MDesCArray &Entries() const = 0;
	// Відкриває елемент; власником стає той, хто викликав.
	virtual TInt Open(const TDesC &aName, MConvSrc *&aOut) = 0;
	virtual TInt Size(const TDesC &aName, TInt &aOut) = 0;
	};

// Створює том за шляхом: архів (.zip/.7z/.rar) або тека з файлом. Реалізація
// архівів - archive.cpp; тека - imgconv.cpp.
TInt ConvOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);
TInt ArcOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);
TInt SzOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);    // arc7z.cpp
TInt RarOpenVol(RFs &aFs, const TDesC &aPath, MConvVol *&aOut);   // arcrar.cpp

// Рядок у лозі робочого потоку (E:\PS1\logs\ps1_import.jsonl) з інших файлів.
void ConvLogVal(const char *aTag, TInt aVal);

enum TConvState { EConvIdle = 0, EConvRun, EConvDone, EConvErr };

enum TConvMode
	{
	EConvModePbp = 0,   // стиснути в .pbp з обнуленим ECC (незворотно)
	EConvModeCopy       // перенести як є
	};

struct TConvStat
	{
	TInt iState;
	TInt iFileNo;       // 1-based, який файл у черзі
	TInt iFileCount;
	TInt iPercent;      // поточний файл
	TInt iErr;
	TBuf<64> iName;
	TBuf<64> iMsg;      // текст останньої помилки або підсумку
	};

// Запускає обробку черги в окремому потоці. aFiles - повні шляхи.
TInt ConvStart(const MDesCArray &aFiles, TConvMode aMode, TBool aDeleteSrc,
               const TDesC &aDstDir);
void ConvStat(TConvStat &aOut);
void ConvCancel();
// Прибирає потік після EConvDone/EConvErr. Безпечно кликати повторно.
void ConvRelease();

// Розпакування одного файлу (BIOS) з архіву або копіювання в теку призначення.
// Синхронне: дамп BIOS - 512 КБ, окремий потік тут був би самоціллю.
TInt ConvExtractOne(RFs &aFs, const TDesC &aPath, const TDesC &aMember,
                    const TDesC &aDstDir, TDes &aOutName);

TBool ConvIsArchiveName(const TDesC &aName);

#endif
