// Platform replacement of mmap(PROT_EXEC|PROT_WRITE) and __clear_cache for ari64 dynarec.
// new_dynarec.c is C code, whereas RChunk and User::IMB_Range are only accessible from C++,
// so they live here under C linkage.
//
// The memory model is deliberately simple: the chunk is committed in full immediately,
// matching upstream mmap behavior. Growing it via Adjust would reduce the memory footprint, but
// the dynarec does not report how much cache it has used, so catching the boundary would require
// an exception handler. Investigating this makes sense only after hardware measurements.
//
// NO_WRITE_EXEC is NOT defined: CreateLocalCode provides memory that can be both written
// and executed without permissions switching. This assumption is checked by Stage 0
// (src/symbian/stage0_probe.cpp) - prior to running on hardware it is unverified.

#include <e32std.h>
#include <hal.h>

static RChunk g_ndrcChunk;
static TBool  g_ndrcOpen;
#ifdef PS1_STAGE2_PROFILE
static TInt   g_ndrcBytes;
// Cache flushes by the recompiler: count, FastCounter ticks (32768 Hz), bytes.
extern "C" { unsigned int g_s2ImbCalls, g_s2ImbTicks, g_s2ImbBytes; }
#endif

extern "C" void *ndrc_symbian_alloc(unsigned int size)
    {
    if (g_ndrcOpen) return NULL;

    const TInt bytes = static_cast<TInt>(size);
    if (g_ndrcChunk.CreateLocalCode(bytes, bytes) != KErrNone)
        return NULL;

    g_ndrcOpen = ETrue;
#ifdef PS1_STAGE2_PROFILE
    g_ndrcBytes = bytes;
#endif
    return g_ndrcChunk.Base();
    }

extern "C" void ndrc_symbian_free(void *base)
    {
    (void)base;
    if (!g_ndrcOpen) return;
    g_ndrcChunk.Close();
    g_ndrcOpen = EFalse;
    }

// Translation cache base - allowing diagnostics to determine if PC is in generated code.
extern "C" unsigned int ndrc_symbian_tcbase(void)
    {
    return g_ndrcOpen ? (unsigned int)g_ndrcChunk.Base() : 0;
    }

#ifdef PS1_STAGE2_PROFILE
extern "C" unsigned int ndrc_symbian_tcsize(void)
    {
    return g_ndrcOpen ? (unsigned int)g_ndrcBytes : 0;
    }
#endif

extern "C" void ndrc_symbian_imb(void *start, void *end)
    {
#ifdef PS1_STAGE2_PROFILE
    const TUint t0 = User::FastCounter();
    User::IMB_Range(start, end);
    g_s2ImbTicks += User::FastCounter() - t0;
    g_s2ImbCalls++;
    g_s2ImbBytes += (TUint)end - (TUint)start;
#else
    User::IMB_Range(start, end);
#endif
    }

// ESTLIB declares _SC_PAGESIZE, but does not provide sysconf.
extern "C" int ndrc_symbian_pgsize(void)
    {
    TInt size = 0;
    if (HAL::Get(HALData::EMemoryPageSize, size) != KErrNone || size < 1)
        size = 4096;
    return size;
    }

// High-resolution counter for profiling (separating CPU/GPU in "emu").
// User::FastCounter is accessible only from C++; exposed here to core/gpulib C code.
extern "C" unsigned int ndrc_symbian_fastcount(void)
    {
    return User::FastCounter();
    }

extern "C" unsigned int ndrc_symbian_fastfreq(void)
    {
    static unsigned int s_fastFreq = 0;
    if (s_fastFreq)
        return s_fastFreq;

    TInt f = 0;
    if (HAL::Get(HALData::EFastCounterFrequency, f) == KErrNone && f >= 1)
        {
        s_fastFreq = (unsigned int)f;
        return s_fastFreq;
        }

    return 1000000;
    }
