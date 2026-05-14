#pragma once

#include "chess.h"
#include "zobrist.h"

// -------------------------------------------------------------------------
// Lossless chained transposition table for CPU perft.
//
// No entry is ever evicted (subject to pool capacity) — keys are 128-bit
// Zobrist so collisions are negligible, and each (position, depth) tuple
// gets its own pool entry.
//
// Thread-safety: losslessStore uses a saturating CAS allocator for slot
// reservation and a CAS loop for chain prepend; losslessProbe is read-only.
// Two threads racing on the same position may briefly miss each other's
// stores (causing a small amount of duplicate work) but never corrupt.
// -------------------------------------------------------------------------

struct LosslessEntry
{
    uint64 hashKey;     // hash.hi ^ hash.lo for verification
    uint64 count;       // perft value
    int32_t next;       // next entry index (-1 = end of chain)
    int32_t _pad;
};

CT_ASSERT(sizeof(LosslessEntry) == 24);

struct LosslessTT
{
    int32_t *buckets;           // bucket heads (-1 = empty)
    LosslessEntry *pool;        // entry pool
    uint64 bucketMask;          // numBuckets - 1
    int32_t nextFree;           // next free entry
    int32_t poolCapacity;       // max entries in pool
};

inline bool losslessProbe(const LosslessTT &tt, Hash128 hash, uint64 *outCount)
{
    if (!tt.buckets) return false;
    uint64 bucket = hash.lo & tt.bucketMask;
    uint64 key = hash.hi ^ hash.lo;
    // Acquire load on the bucket head: pairs with the seq_cst CAS in losslessStore.
    // Without this, a weakly-ordered reader (ARM64) can see the new head index but
    // speculatively read stale pool entries written before the CAS — causing many
    // spurious TT misses under MT and a lot of duplicate work at deep perft.
    // MSVC defaults to /volatile:ms on ARM/ARM64 which gives volatile loads
    // acquire semantics; on x86/x64 ordinary loads are already acquire.
    int32_t idx = *(volatile int32_t *)&tt.buckets[bucket];
    while (idx >= 0)
    {
        if (tt.pool[idx].hashKey == key)
        {
            *outCount = tt.pool[idx].count;
            return true;
        }
        idx = *(volatile int32_t *)&tt.pool[idx].next;
    }
    return false;
}

inline void losslessStore(LosslessTT &tt, Hash128 hash, uint64 count)
{
    if (!tt.buckets) return;
    // Saturating CAS bump allocator — safe for concurrent stores from multiple
    // threads. An unconditional InterlockedIncrement keeps growing nextFree even
    // when the table is full; with enough threads × stores that wraps int32
    // negative and then back to small positive values, which both writes OOB
    // and silently overwrites earlier entries (TT corruption). The CAS loop
    // below stops at poolCapacity exactly.
#ifdef _MSC_VER
    long newIdx;
    long cur = *(volatile long *)&tt.nextFree;
    do {
        if (cur >= tt.poolCapacity) return;  // table full, drop the store
        newIdx = cur;
    } while ((cur = _InterlockedCompareExchange((volatile long *)&tt.nextFree,
                                                newIdx + 1, newIdx)) != newIdx);
#else
    int32_t newIdx = __atomic_load_n(&tt.nextFree, __ATOMIC_RELAXED);
    do {
        if (newIdx >= tt.poolCapacity) return;
    } while (!__atomic_compare_exchange_n(&tt.nextFree, &newIdx, newIdx + 1,
                                          true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
#endif
    uint64 bucket = hash.lo & tt.bucketMask;
    tt.pool[newIdx].hashKey = hash.hi ^ hash.lo;
    tt.pool[newIdx].count = count;
    // CAS loop to prepend to bucket chain.
#ifdef _MSC_VER
    long oldHead;
    do {
        oldHead = *(volatile long *)&tt.buckets[bucket];
        tt.pool[newIdx].next = (int32_t)oldHead;
    } while (_InterlockedCompareExchange((volatile long *)&tt.buckets[bucket],
                                         (long)newIdx, oldHead) != oldHead);
#else
    int32_t oldHead;
    do {
        oldHead = __atomic_load_n(&tt.buckets[bucket], __ATOMIC_SEQ_CST);
        tt.pool[newIdx].next = oldHead;
    } while (!__atomic_compare_exchange_n(&tt.buckets[bucket], &oldHead, newIdx,
                                          true, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
#endif
}

// -------------------------------------------------------------------------
// TT management (allocation, deallocation)
// -------------------------------------------------------------------------

#define MAX_TT_DEPTH 32

// Global TT array (defined in launcher.cpp). Indexed by remaining depth.
extern LosslessTT hostLosslessTTs[MAX_TT_DEPTH];

// Initialize TTs for the given max depth and average branching factor.
// Memory is split across depths roughly proportional to branchingFactor^(maxDepth - d)
// so the densely-occupied shallow depths get the lion's share.
void initTT(int maxDepth, float branchingFactor);

// Free all TT memory.
void freeTT();
