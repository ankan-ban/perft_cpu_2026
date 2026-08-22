#pragma once

// Compile-time settings for the CPU perft.

// Host memory budget for transposition tables (MB). 0 = auto (90% of system RAM).
#define HOST_TT_BUDGET_MB 0

// Depth-2 "null move" count reuse: at a depth-2 parent, count the opponent's
// replies once on the unchanged board and reuse that count for every child move
// proven not to change the opponent's legal move set. See FgmcCount2Processor.
// Set to 0 to fall back to replaying every child.
#ifndef USE_NULL_COUNT_DEPTH2      // -DUSE_NULL_COUNT_DEPTH2=0 to A/B test it away
#define USE_NULL_COUNT_DEPTH2 1
#endif

// Verbose diagnostics (progress reporting, per-depth stats).
#define VERBOSE_LOGGING 0

#ifndef CPU_FORCE_INLINE
#ifdef _MSC_VER
    #define CPU_FORCE_INLINE __forceinline
#else
    #define CPU_FORCE_INLINE inline __attribute__((always_inline))
#endif
#endif
