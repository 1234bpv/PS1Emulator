// Платформна заміна mmap(PROT_EXEC|PROT_WRITE) і __clear_cache для динареку ari64.
// new_dynarec.c - код на C, а RChunk і User::IMB_Range доступні лише з C++,
// тому вони живуть тут за C-зв'язуванням.
//
// Модель пам'яті свідомо проста: чанк комітиться повністю одразу, як це робить
// mmap в upstream. Дорощування через Adjust дало б менший слід у пам'яті, але
// динарек не повідомляє, скільки кешу він уже зайняв, тож ловити межу довелося б
// обробником виключень. Перевіряти це має сенс лише після замірів на апараті.
//
// NO_WRITE_EXEC НЕ визначаємо: CreateLocalCode дає пам'ять, у яку можна і писати,
// і яку можна виконувати, без перемикання прав. Це припущення перевіряє Етап 0
// (src/symbian/stage0_probe.cpp) - до його прогону на апараті воно недоведене.

#include <e32std.h>
#include <hal.h>

static RChunk g_ndrcChunk;
static TBool  g_ndrcOpen;

extern "C" void *ndrc_symbian_alloc(unsigned int size)
    {
    if (g_ndrcOpen) return NULL;

    const TInt bytes = static_cast<TInt>(size);
    if (g_ndrcChunk.CreateLocalCode(bytes, bytes) != KErrNone)
        return NULL;

    g_ndrcOpen = ETrue;
    return g_ndrcChunk.Base();
    }

extern "C" void ndrc_symbian_free(void *base)
    {
    (void)base;
    if (!g_ndrcOpen) return;
    g_ndrcChunk.Close();
    g_ndrcOpen = EFalse;
    }

extern "C" void ndrc_symbian_imb(void *start, void *end)
    {
    User::IMB_Range(start, end);
    }

// ESTLIB оголошує _SC_PAGESIZE, але sysconf не постачає.
extern "C" int ndrc_symbian_pgsize(void)
    {
    TInt size = 0;
    if (HAL::Get(HALData::EMemoryPageSize, size) != KErrNone || size < 1)
        size = 4096;
    return size;
    }

// Високороздільний лічильник для профілювання (розділити CPU/GPU в "emu").
// User::FastCounter доступний лише з C++; віддаємо його C-коду ядра/gpulib.
extern "C" unsigned int ndrc_symbian_fastcount(void)
    {
    return User::FastCounter();
    }

extern "C" unsigned int ndrc_symbian_fastfreq(void)
    {
    TInt f = 0;
    if (HAL::Get(HALData::EFastCounterFrequency, f) != KErrNone || f < 1)
        f = 1000000;
    return (unsigned int)f;
    }
