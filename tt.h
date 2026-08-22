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
// Shallow TT — 8-byte entry, used for TT[2].
//
// Layout: (high 48 bits of hash.hi) | (16-bit perft count)
//   slot index = hash.lo & mask     -> implicitly verifies log2(N) bits of hash.lo
//   stored verifier = hash.hi[63:16] -> 48 bits, fully INDEPENDENT of the index
//
// The independence matters. The previous layout stored hash.lo[63:24] as the
// verifier while indexing with hash.lo & mask, so index and verifier overlapped:
// a probe landing on an occupied slot had only 64 - log2(N) bits left to reject a
// foreign entry (36 bits at the default 2^28 entries). At deep-perft probe counts
// that yields false hits — the sister perft_gpu_2026 repo measured startpos perft
// 12 returning 62854969240506188 instead of 62854969236701747 with the equivalent
// scheme, and disabled it. Sourcing the verifier from the OTHER hash word instead
// gives a flat 48 rejection bits at any table size (4096x stronger here) for the
// same 8 bytes and the same single load/store.
//
// The count needs only 16 bits: a position has at most 218 legal moves, so
// perft(2) <= 218*218 = 47524 < 2^16. shallowStore guards the bound anyway.
//
// A zero word means "empty". A foreign probe whose hash.hi[63:16] is all zero
// therefore reads an empty slot as a hit with count 0; that is a 2^-48 event per
// probe (~1e-4 over a full perft 12) and is the only residual false-hit path.
// Aligned uint64 reads/writes are atomic, so no torn-read concerns under MT.
// -------------------------------------------------------------------------

static const uint64 SHALLOW_PERFT_MASK = (1ULL << 16) - 1;         // 0x0000_0000_0000_FFFF
static const uint64 SHALLOW_VERIF_MASK = ~SHALLOW_PERFT_MASK;      // 0xFFFF_FFFF_FFFF_0000

// verifier bits for a position: top 48 bits of hash.hi, aligned to bits [63:16]
static CPU_FORCE_INLINE uint64 shallowVerif(Hash128 hash)
{
    return hash.hi & SHALLOW_VERIF_MASK;
}

struct ShallowTT
{
    uint64 *entries;    // each = (hash.hi & VERIF_MASK) | (count & PERFT_MASK)
    uint64 mask;        // numEntries - 1 (power of 2; any size is verification-safe)
};

inline bool shallowProbe(const ShallowTT &tt, Hash128 hash, uint64 *outCount)
{
    if (!tt.entries) return false;
    uint64 idx   = hash.lo & tt.mask;
    uint64 entry = tt.entries[idx];
    if ((entry & SHALLOW_VERIF_MASK) == shallowVerif(hash))
    {
        *outCount = entry & SHALLOW_PERFT_MASK;
        return true;
    }
    return false;
}

inline void shallowStore(const ShallowTT &tt, Hash128 hash, uint64 count)
{
    if (!tt.entries) return;
    if (count > SHALLOW_PERFT_MASK) return;   // unreachable at depth 2 (max 47524)
    uint64 idx = hash.lo & tt.mask;
    tt.entries[idx] = shallowVerif(hash) | count;
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
