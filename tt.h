#pragma once

#include "chess.h"
#include "zobrist.h"

// -------------------------------------------------------------------------
// Lossy open-addressing TT (Hyatt/Crafty XOR-lockless scheme).
//   Store: verification = (hash.hi ^ hash.lo) ^ count
//   Probe: if ((verification ^ count) == hash.hi ^ hash.lo) → hit
// Multi-thread safety: torn reads / racing writes are detected by the XOR
// mismatch and reported as a miss (causes recompute, never a wrong count).
// Single 16-byte slot per bucket — no chain walk, no CAS.
// -------------------------------------------------------------------------

struct TTEntry
{
    uint64 verification;   // (hash.hi ^ hash.lo) ^ count
    uint64 count;
};

CT_ASSERT(sizeof(TTEntry) == 16);

struct TTTable
{
    TTEntry *entries;       // power-of-2-sized array
    uint64 mask;            // numEntries - 1
};

inline bool ttProbe(const TTTable &tt, Hash128 hash, uint64 *outCount)
{
    if (!tt.entries) return false;
    uint64 idx = hash.lo & tt.mask;
    uint64 storedCount = tt.entries[idx].count;
    uint64 storedVerif = tt.entries[idx].verification;
    uint64 expectedKey = hash.hi ^ hash.lo;
    if ((storedVerif ^ storedCount) == expectedKey)
    {
        *outCount = storedCount;
        return true;
    }
    return false;
}

inline void ttStore(const TTTable &tt, Hash128 hash, uint64 count)
{
    if (!tt.entries) return;
    uint64 idx = hash.lo & tt.mask;
    tt.entries[idx].count = count;
    tt.entries[idx].verification = (hash.hi ^ hash.lo) ^ count;
}

// -------------------------------------------------------------------------
// Shallow TT — 8-byte entry, used for TT[2] where the perft count fits in
// 24 bits (depth-2 max ≈ 218×218 ≈ 47K). Packs (high 40 bits of hash) | (24
// bits of perft) into a single uint64. With table size ≥ 2²⁴, the slot
// index implicitly verifies the low 24 hash bits, so the high-40 stored
// portion completes 64-bit hash verification — no collision detection lost
// vs the 16-byte TTEntry, at half the memory. Aligned-uint64 reads/writes
// are atomic on x86, so no torn-read concerns under MT.
// (Idea: ankan-ban/perft_gpu's ShallowHashEntry.)
// -------------------------------------------------------------------------

static const uint64 SHALLOW_PERFT_MASK = (1ULL << 24) - 1;        // 0x0000_0000_00FFFFFF
static const uint64 SHALLOW_VERIF_MASK = ~SHALLOW_PERFT_MASK;      // 0xFFFF_FFFFFF_000000

struct ShallowTT
{
    uint64 *entries;    // each = (hash.lo & VERIF_MASK) | (count & PERFT_MASK)
    uint64 mask;        // numEntries - 1; numEntries must be ≥ 2²⁴ and a power of 2
};

inline bool shallowProbe(const ShallowTT &tt, Hash128 hash, uint64 *outCount)
{
    if (!tt.entries) return false;
    uint64 idx   = hash.lo & tt.mask;
    uint64 entry = tt.entries[idx];
    if ((entry & SHALLOW_VERIF_MASK) == (hash.lo & SHALLOW_VERIF_MASK))
    {
        *outCount = entry & SHALLOW_PERFT_MASK;
        return true;
    }
    return false;
}

inline void shallowStore(const ShallowTT &tt, Hash128 hash, uint64 count)
{
    if (!tt.entries) return;
    uint64 idx = hash.lo & tt.mask;
    tt.entries[idx] = (hash.lo & SHALLOW_VERIF_MASK) | (count & SHALLOW_PERFT_MASK);
}

// -------------------------------------------------------------------------
// Legacy lossless chained TT (kept for reference / fallback experiments).
// No entry is ever evicted — 128-bit Zobrist makes collisions negligible.
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
    int32_t *buckets;
    LosslessEntry *pool;
    uint64 bucketMask;
    int32_t nextFree;
    int32_t poolCapacity;
};

inline bool losslessProbe(const LosslessTT &tt, Hash128 hash, uint64 *outCount)
{
    if (!tt.buckets) return false;
    uint64 bucket = hash.lo & tt.bucketMask;
    uint64 key = hash.hi ^ hash.lo;
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
#ifdef _MSC_VER
    long newIdx;
    long cur = *(volatile long *)&tt.nextFree;
    do {
        if (cur >= tt.poolCapacity) return;
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
// TT management
// -------------------------------------------------------------------------

#define MAX_TT_DEPTH 32

// Hybrid TT layout:
//   Depth 3..LOSSY_TT_MAX_DEPTH → lossy open-addressing (hostTTs)
//   Depth LOSSY_TT_MAX_DEPTH+1..MAX_TT_DEPTH-1 → lossless chained (hostLosslessTTs)
// Shallow remaining-depth tables hold millions of entries each visited many times
// (transpositions); collisions are cheap because recompute is a small FGMC subtree.
// Deep remaining-depth tables hold few but expensive entries (each represents a huge
// subtree); losing one would force enormous recompute, so we keep them lossless and
// sized to hold all expected entries.
#define LOSSY_TT_MAX_DEPTH 6

extern ShallowTT   hostShallowTT2;                 // TT[2] only (8-byte entries)
extern TTTable     hostTTs        [MAX_TT_DEPTH];  // TT[3..LOSSY_TT_MAX_DEPTH]
extern LosslessTT  hostLosslessTTs[MAX_TT_DEPTH];  // TT[LOSSY_TT_MAX_DEPTH+1..]

inline bool perftTTProbe(int depth, Hash128 hash, uint64 *outCount)
{
    if (depth == 2)
        return shallowProbe(hostShallowTT2, hash, outCount);
    if (depth <= LOSSY_TT_MAX_DEPTH)
        return ttProbe(hostTTs[depth], hash, outCount);
    return losslessProbe(hostLosslessTTs[depth], hash, outCount);
}

inline void perftTTStore(int depth, Hash128 hash, uint64 count)
{
    if (depth == 2)
        shallowStore(hostShallowTT2, hash, count);
    else if (depth <= LOSSY_TT_MAX_DEPTH)
        ttStore(hostTTs[depth], hash, count);
    else
        losslessStore(hostLosslessTTs[depth], hash, count);
}

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
inline void perftTTPrefetch(int depth, Hash128 hash)
{
    if (depth == 2 && hostShallowTT2.entries)
    {
        uint64 idx = hash.lo & hostShallowTT2.mask;
        _mm_prefetch((const char *)&hostShallowTT2.entries[idx], _MM_HINT_T0);
        return;
    }
    if (depth <= LOSSY_TT_MAX_DEPTH && hostTTs[depth].entries)
    {
        uint64 idx = hash.lo & hostTTs[depth].mask;
        _mm_prefetch((const char *)&hostTTs[depth].entries[idx], _MM_HINT_T0);
    }
}
#else
inline void perftTTPrefetch(int, Hash128) {}
#endif

// Initialize TTs for the given max depth and average branching factor.
void initTT(int maxDepth, float branchingFactor);

// Free all TT memory.
void freeTT();
