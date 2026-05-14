#pragma once

// Compile-time settings for the CPU perft.

// Host memory budget for transposition tables (MB). 0 = auto (90% of system RAM).
#define HOST_TT_BUDGET_MB 0

// Verbose diagnostics (progress reporting, per-depth stats).
#define VERBOSE_LOGGING 0

#ifndef CPU_FORCE_INLINE
#ifdef _MSC_VER
    #define CPU_FORCE_INLINE __forceinline
#else
    #define CPU_FORCE_INLINE inline __attribute__((always_inline))
#endif
#endif
