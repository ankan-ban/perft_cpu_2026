// Phase 0 cliff probe for Plan A.
//
// Compiled with /arch:AVX2 in isolation. Calls measure cross-TU boundary cost
// when a non-AVX2 caller invokes an AVX2-compiled function.
//
// On MSVC x64, YMM6-15 are nonvolatile inside an /arch:AVX2 TU. When a non-AVX2
// caller (default arch) calls into this TU, the caller hasn't agreed to
// preserve those YMM halves, so MSVC emits a prologue/epilogue inside the AVX2
// function that vzeroupper / save / restore YMM state. That overhead is the
// "cliff" we are measuring.
//
// Three probes are exposed:
//   cliff_probe_min  — no YMM use at all; baseline call overhead.
//   cliff_probe_4yc  — 4 YMM ALU ops; representative light kernel.
//   cliff_probe_16yc — 16 YMM ALU ops + a sink; representative heavy kernel.
//
// All three return a sum derived from the input pointer's first uint64 so the
// optimizer can't fold the call. They are declared `extern "C"` to keep the
// ABI mangle-free and predictable across the TU boundary.

#include <stdint.h>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define PERFT_HAS_AVX2 1
#else
#define PERFT_HAS_AVX2 0
#endif

extern "C" __declspec(noinline)
uint64_t cliff_probe_min(uint64_t *p)
{
    return *p + 1u;
}

#if PERFT_HAS_AVX2
extern "C" __declspec(noinline)
uint64_t cliff_probe_4yc(uint64_t *p)
{
    // 4 YMM ops + extract. The YMM use forces MSVC to honor the AVX2
    // calling convention for this TU.
    uint64_t x = *p;
    __m256i a = _mm256_set1_epi64x((long long)x);
    __m256i b = _mm256_set1_epi64x((long long)(x ^ 0x55aaULL));
    __m256i c = _mm256_xor_si256(a, b);
    __m256i d = _mm256_and_si256(c, a);
    __m256i e = _mm256_or_si256(d, b);
    // Extract lane 0 (single 64-bit value).
    return (uint64_t)_mm256_extract_epi64(e, 0);
}

extern "C" __declspec(noinline)
uint64_t cliff_probe_16yc(uint64_t *p)
{
    uint64_t x = *p;
    __m256i a = _mm256_set1_epi64x((long long)x);
    __m256i b = _mm256_set1_epi64x((long long)(x ^ 0x55aaULL));
    __m256i acc = _mm256_setzero_si256();
    // 16 chained YMM ops — large enough to fill nonvolatile YMM regs and force
    // the compiler to save/restore them around the call.
    for (int i = 0; i < 4; ++i) {
        __m256i t = _mm256_xor_si256(a, b);
        t = _mm256_and_si256(t, a);
        t = _mm256_or_si256(t, b);
        acc = _mm256_xor_si256(acc, t);
        a = _mm256_add_epi64(a, b);
        b = _mm256_sub_epi64(b, a);
    }
    return (uint64_t)_mm256_extract_epi64(acc, 0)
         + (uint64_t)_mm256_extract_epi64(acc, 1)
         + (uint64_t)_mm256_extract_epi64(acc, 2)
         + (uint64_t)_mm256_extract_epi64(acc, 3);
}
#else
// Non-x86: stub the AVX2 microbench probes. They're only used by -bench-cliff,
// which doesn't run on ARM. Returning *p keeps the call non-trivial so MSVC
// can't fold it, matching the perf characteristic of cliff_probe_min.
extern "C" __declspec(noinline) uint64_t cliff_probe_4yc (uint64_t *p) { return *p ^ 0x55aaULL; }
extern "C" __declspec(noinline) uint64_t cliff_probe_16yc(uint64_t *p) { return *p ^ 0xaa55ULL; }
#endif
