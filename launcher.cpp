// CPU perft launchers + host transposition table management.

#include <math.h>
#include <thread>
#include <atomic>
#include <vector>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
  #include <emmintrin.h>  // _mm_prefetch
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "MoveGeneratorBitboard.h"
#include "launcher.h"
#include "utils.h"
#include "zobrist.h"
#include "tt.h"

// -------------------------------------------------------------------------
// Plan A scalar leaf-batch helpers. These are extern "C" entry points called
// from the AVX2-isolated TU (`leaf_batch_avx2.cpp`). They live HERE rather
// than in the AVX2 TU so that the template instantiations of
// countMovesPair[Cached] / countMovesDispatch from MoveGeneratorBitboard.h
// land in a default-arch object. When the AVX2 TU was instantiating those
// templates, LTCG merged the COMDAT under /arch:AVX2 codegen and every
// caller paid the documented -57% global-AVX2 cliff (+~50 ms on kiwipete
// perft 5). Keeping the AVX2 TU thin-forwarder-only preserves the cliff.
// Phases 2+ will replace these forwarders with real SIMD work; that work
// must reach the scalar leaf body via THIS interface, never by including
// MoveGeneratorBitboard.h templates directly into the AVX2 TU.
// -------------------------------------------------------------------------

extern "C" {

uint64 leafFastLoopScalar_white(QuadBitBoard *bufCp, GameState *bufGs,
                                int n, uint64 cachedNonSliderAtk) noexcept
{
    uint64 s = 0;
    int i = 0;
    for (; i + 1 < n; i += 2) {
        s += MoveGeneratorBitboard::countMovesPairCached<WHITE, 0>(
            &bufCp[i], &bufGs[i], &bufCp[i + 1], &bufGs[i + 1], cachedNonSliderAtk);
    }
    if (i < n)
        s += MoveGeneratorBitboard::countMovesDispatch<WHITE>(&bufCp[i], &bufGs[i]);
    return s;
}

uint64 leafFastLoopScalar_black(QuadBitBoard *bufCp, GameState *bufGs,
                                int n, uint64 cachedNonSliderAtk) noexcept
{
    uint64 s = 0;
    int i = 0;
    for (; i + 1 < n; i += 2) {
        s += MoveGeneratorBitboard::countMovesPairCached<BLACK, 0>(
            &bufCp[i], &bufGs[i], &bufCp[i + 1], &bufGs[i + 1], cachedNonSliderAtk);
    }
    if (i < n)
        s += MoveGeneratorBitboard::countMovesDispatch<BLACK>(&bufCp[i], &bufGs[i]);
    return s;
}

uint64 leafSlowLoopScalar_white(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept
{
    uint64 s = 0;
    int i = 0;
    for (; i + 1 < n; i += 2) {
        s += MoveGeneratorBitboard::countMovesPair<WHITE>(
            &bufCp[i], &bufGs[i], &bufCp[i + 1], &bufGs[i + 1]);
    }
    if (i < n)
        s += MoveGeneratorBitboard::countMovesDispatch<WHITE>(&bufCp[i], &bufGs[i]);
    return s;
}

uint64 leafSlowLoopScalar_black(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept
{
    uint64 s = 0;
    int i = 0;
    for (; i + 1 < n; i += 2) {
        s += MoveGeneratorBitboard::countMovesPair<BLACK>(
            &bufCp[i], &bufGs[i], &bufCp[i + 1], &bufGs[i + 1]);
    }
    if (i < n)
        s += MoveGeneratorBitboard::countMovesDispatch<BLACK>(&bufCp[i], &bufGs[i]);
    return s;
}

// Phase 4: scalar slow-path entry that consumes a precomputed per-child enemy
// non-slider attack array (computed in the AVX2 TU via SIMD bulk shifts).
// `n` is always 4 in the current caller, but we keep the generic shape for
// future flexibility. Processes the 4 children as two pairs.
uint64 leafSlowLoopScalarWithAtk4_white(QuadBitBoard *bufCp, GameState *bufGs,
                                        uint64 *enemyNonSliderAtk4, int n) noexcept
{
    uint64 s = 0;
    int i = 0;
    for (; i + 1 < n; i += 2) {
        s += MoveGeneratorBitboard::countMovesPairWithAtk<WHITE>(
            &bufCp[i], &bufGs[i], &bufCp[i + 1], &bufGs[i + 1],
            enemyNonSliderAtk4[i], enemyNonSliderAtk4[i + 1]);
    }
    if (i < n)
        s += MoveGeneratorBitboard::countMovesDispatch<WHITE>(&bufCp[i], &bufGs[i]);
    return s;
}

uint64 leafSlowLoopScalarWithAtk4_black(QuadBitBoard *bufCp, GameState *bufGs,
                                        uint64 *enemyNonSliderAtk4, int n) noexcept
{
    uint64 s = 0;
    int i = 0;
    for (; i + 1 < n; i += 2) {
        s += MoveGeneratorBitboard::countMovesPairWithAtk<BLACK>(
            &bufCp[i], &bufGs[i], &bufCp[i + 1], &bufGs[i + 1],
            enemyNonSliderAtk4[i], enemyNonSliderAtk4[i + 1]);
    }
    if (i < n)
        s += MoveGeneratorBitboard::countMovesDispatch<BLACK>(&bufCp[i], &bufGs[i]);
    return s;
}

}  // extern "C"

// Runtime TT toggle (default: enabled, disable with -nott CLI flag)
bool g_useTT = true;

// Number of CPU threads used at the root of CPU perft. Default 1 (single-threaded).
// Set via -mt N. Works with both `-nott` and the default TT path — the lossless
// host TT is thread-safe (atomic slot-bump + CAS chain prepend).
int g_numThreads = 1;

ShallowTT   hostShallowTT2 = {};                  // TT[2] only (8-byte entries)
TTTable     hostTTs        [MAX_TT_DEPTH];
LosslessTT  hostLosslessTTs[MAX_TT_DEPTH];

// Overridable host TT budget (set from CLI before calling initTT)
int g_hostTTBudgetMB = HOST_TT_BUDGET_MB;

// TT[2] size in MB (-tt2 N). 0 disables TT[2]; default 2048 MB.
// Sweet-spot range is ~2–6 GB on the 24-thread Arrow Lake (32 GB RAM): below 1 GB
// per-thread working sets exceed the table; above 6-8 GB DRAM pressure cliffs.
// (TT[1] was tried and lost decisively — see git log; the FGMC depth-2 path is
// faster than any hash+probe loop can match at depth 1.)
int g_tt2EntriesMB = 2048;

// -------------------------------------------------------------------------
// Transposition table allocation
// -------------------------------------------------------------------------

static uint64 floorPow2(uint64 n)
{
    if (n == 0) return 0;
    n |= (n >> 1);
    n |= (n >> 2);
    n |= (n >> 4);
    n |= (n >> 8);
    n |= (n >> 16);
    n |= (n >> 32);
    return (n + 1) >> 1;
}

void initTT(int maxDepth, float branchingFactor)
{
    memset(hostTTs,         0, sizeof(hostTTs));
    memset(hostLosslessTTs, 0, sizeof(hostLosslessTTs));
    hostShallowTT2 = {};
    if (!g_useTT) return;

    // Host TTs for depths 3..maxDepth. Depth 1 doesn't recurse; depth 2 uses
    // the fused FGMC leaf path unconditionally (no TT lookup — see perft_cpu).
    int numHostTTs = 0;
    for (int d = 3; d <= maxDepth && d < MAX_TT_DEPTH; d++)
        numHostTTs++;
    if (numHostTTs == 0) return;

    // TT[2]: ShallowTT (8-byte entries). Half the memory of the previous 16-byte
    // lossy entries → double effective capacity at the same RAM. The entry stores
    // a 48-bit verifier taken from hash.hi while the slot index comes from hash.lo,
    // so verification strength (48 independent bits) does not depend on table size.
    if (g_tt2EntriesMB > 0)
    {
        uint64 numEntries = ((uint64)g_tt2EntriesMB * 1024 * 1024) / sizeof(uint64);
        uint64 pow2 = 1;
        while (pow2 < numEntries) pow2 <<= 1;
        if (pow2 > numEntries) pow2 >>= 1;
        numEntries = pow2;
        if (numEntries < (1ULL << 16))
        {
            printf("Warning: TT[2] size below 2^16 entries. Skipping.\n");
        }
        else
        {
            uint64 *entries = (uint64 *)malloc(numEntries * sizeof(uint64));
            if (entries)
            {
                memset(entries, 0, numEntries * sizeof(uint64));
                hostShallowTT2.entries = entries;
                hostShallowTT2.mask = numEntries - 1;
                uint64 totalMB = numEntries * sizeof(uint64) / (1024*1024);
                const char *unit = ""; uint64 disp = numEntries;
                if (numEntries >= 1024*1024) { unit = "M"; disp = numEntries / (1024*1024); }
                else if (numEntries >= 1024) { unit = "K"; disp = numEntries / 1024; }
                printf("Host TT[2] (shallow 8B): %llu%s entries (%llu MB)\n",
                       (unsigned long long)disp, unit, (unsigned long long)totalMB);
            }
        }
    }

    if (g_hostTTBudgetMB <= 0)
    {
        uint64 totalRAM = 0;
#ifdef _WIN32
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(memInfo);
        if (GlobalMemoryStatusEx(&memInfo))
            totalRAM = memInfo.ullTotalPhys;
#else
        long pages = sysconf(_SC_PHYS_PAGES);
        long pageSize = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && pageSize > 0)
            totalRAM = (uint64)pages * (uint64)pageSize;
#endif
        if (totalRAM > 0)
            g_hostTTBudgetMB = (int)((totalRAM * 9 / 10) / (1024 * 1024));
        else
            g_hostTTBudgetMB = 8192;
        printf("Host TT budget: auto %d MB (90%% of %llu MB system RAM)\n",
               g_hostTTBudgetMB, (unsigned long long)(totalRAM / (1024 * 1024)));
    }
    uint64 budgetBytes = (uint64)g_hostTTBudgetMB * 1024 * 1024;

    // Budget weights cover only the LOSSY band (3..LOSSY_TT_MAX_DEPTH). Lossless
    // tables for deeper remaining-depths are tiny and sized independently below.
    double weights[MAX_TT_DEPTH];
    memset(weights, 0, sizeof(weights));
    double totalWeight = 0;
    int lossyEnd = (LOSSY_TT_MAX_DEPTH <= maxDepth) ? LOSSY_TT_MAX_DEPTH : maxDepth;
    for (int d = 3; d <= lossyEnd && d < MAX_TT_DEPTH; d++)
    {
        weights[d] = pow((double)branchingFactor, lossyEnd - d);
        totalWeight += weights[d];
    }

    // Allocate lossy TTs for the dense shallow band. No per-depth caps — the
    // BF-weighted budget naturally gives TT[3] the bulk and scales for deeper
    // perfts; capping just shrinks the working set at higher maxDepth.
    for (int d = 3; d <= lossyEnd && d < MAX_TT_DEPTH; d++)
    {
        uint64 depthBytes = (uint64)(budgetBytes * weights[d] / totalWeight);
        uint64 numEntries = floorPow2(depthBytes / sizeof(TTEntry));
        if (numEntries < 1ull * 1024 * 1024) numEntries = 1ull * 1024 * 1024;  // 1M floor (16 MB)

        TTEntry *entries = (TTEntry *)malloc(numEntries * sizeof(TTEntry));
        if (entries)
        {
            memset(entries, 0, numEntries * sizeof(TTEntry));
            hostTTs[d].entries = entries;
            hostTTs[d].mask = numEntries - 1;

            uint64 totalMB = numEntries * sizeof(TTEntry) / (1024*1024);
            const char *unit = ""; uint64 dispEntries = numEntries;
            if (numEntries >= 1024*1024) { unit = "M"; dispEntries = numEntries / (1024*1024); }
            else if (numEntries >= 1024) { unit = "K"; dispEntries = numEntries / 1024; }
            printf("Host TT[%d] (lossy): %llu%s entries (%llu MB)\n",
                   d, (unsigned long long)dispEntries, unit, (unsigned long long)totalMB);
        }
        else
        {
            printf("Warning: failed to allocate host TT[%d] (%llu MB)\n",
                   d, (unsigned long long)(numEntries * sizeof(TTEntry) / (1024*1024)));
        }
    }

    // Allocate lossless chained TTs for the deep band. Few entries each but each
    // is enormously expensive to recompute, so we must size to hold them all.
    // Estimated max entries at depth d (caching subtree of remaining depth d) is
    // bounded by the perft count at distance (maxDepth - d) from the root —
    // each unique reachable position contributes at most one entry. For startpos:
    //   maxDepth=9: d=7 has ~400, d=8 has ~20, d=9 has 1.
    //   maxDepth=10: d=7 has ~4K, d=8 has ~200, d=9 has ~20, d=10 has 1.
    // We use perft growth^(maxDepth-d) where growth ≈ 30 is the raw branching
    // factor, capped to a reasonable max. Plenty of headroom for any feasible perft.
    for (int d = LOSSY_TT_MAX_DEPTH + 1; d <= maxDepth && d < MAX_TT_DEPTH; d++)
    {
        // Conservative upper bound on unique positions at distance (maxDepth-d) from root
        double distFromRoot = (double)(maxDepth - d);
        double est = pow(30.0, distFromRoot);
        if (est < 1.0) est = 1.0;
        uint64 numSlots = (uint64)(est * 4.0);  // 4x oversize for low load factor
        // Power-of-2 round up
        uint64 pow2 = 1;
        while (pow2 < numSlots) pow2 <<= 1;
        numSlots = pow2;
        if (numSlots < 1024) numSlots = 1024;
        if (numSlots > (1ull << 28)) numSlots = (1ull << 28);  // 256M cap

        int32_t poolCap = (int32_t)numSlots;
        uint64 numBuckets = numSlots;

        hostLosslessTTs[d].buckets = (int32_t *)malloc(numBuckets * sizeof(int32_t));
        hostLosslessTTs[d].pool = (LosslessEntry *)malloc((uint64)poolCap * sizeof(LosslessEntry));
        if (hostLosslessTTs[d].buckets && hostLosslessTTs[d].pool)
        {
            memset(hostLosslessTTs[d].buckets, 0xFF, numBuckets * sizeof(int32_t));
            hostLosslessTTs[d].bucketMask = numBuckets - 1;
            hostLosslessTTs[d].nextFree = 0;
            hostLosslessTTs[d].poolCapacity = poolCap;

            uint64 totalMB = (numBuckets * sizeof(int32_t) + (uint64)poolCap * sizeof(LosslessEntry)) / (1024*1024);
            const char *unit = ""; uint64 dispSlots = numSlots;
            if (numSlots >= 1024*1024) { unit = "M"; dispSlots = numSlots / (1024*1024); }
            else if (numSlots >= 1024) { unit = "K"; dispSlots = numSlots / 1024; }
            printf("Host TT[%d] (lossless): %llu%s entries (%llu MB)\n",
                   d, (unsigned long long)dispSlots, unit, (unsigned long long)totalMB);
        }
        else
        {
            free(hostLosslessTTs[d].buckets);
            free(hostLosslessTTs[d].pool);
            memset(&hostLosslessTTs[d], 0, sizeof(LosslessTT));
            printf("Warning: failed to allocate host TT[%d]\n", d);
        }
    }
    printf("\n");
    fflush(stdout);
}

void freeTT()
{
    free(hostShallowTT2.entries);
    hostShallowTT2 = {};
    for (int d = 0; d < MAX_TT_DEPTH; d++)
    {
        free(hostTTs[d].entries);
        memset(&hostTTs[d], 0, sizeof(TTTable));
        free(hostLosslessTTs[d].buckets);
        free(hostLosslessTTs[d].pool);
        memset(&hostLosslessTTs[d], 0, sizeof(LosslessTT));
    }
}

// -------------------------------------------------------------------------
// Template-optimized CPU perft
// -------------------------------------------------------------------------

// Templated on (chance, useTT). When useTT == false, the compiler eliminates
// all the Zobrist hash update plumbing — at depth 5 this is many millions of
// XORs and getPieceAt() probes that produce nothing.
//
// At depth == 2/3 (no TT) we fuse generate-make-count into a single walk so
// we avoid materialising the CMove array entirely.
template <uint8 chance, bool useTT>
static uint64 perft_cpu(QuadBitBoard *pos, GameState *gs, uint32 depth, Hash128 hash)
{
    if (depth == 1)
        return MoveGeneratorBitboard::countMoves<chance>(pos, gs);

    if (depth == 2)
    {
        if constexpr (useTT)
        {
            uint64 ttCount;
            if (perftTTProbe(2, hash, &ttCount))
                return ttCount;
            uint64 count = MoveGeneratorBitboard::countTwoLevelSubtree<chance>(pos, gs);
            perftTTStore(2, hash, count);
            return count;
        }
        return MoveGeneratorBitboard::countTwoLevelSubtree<chance>(pos, gs);
    }

    if (depth == 3)
    {
        if constexpr (useTT)
        {
            uint64 ttCount;
            if (perftTTProbe(3, hash, &ttCount))
                return ttCount;
            // useTT: drop FGMC at depth=3 so each depth-2 child has its own hash
            // and can probe TT[2]. Software-pipelined: pass 1 computes each
            // child's pos/gs/hash and prefetches its TT[2] bucket; pass 2 probes
            // and recurses. The probe at depth=2 is the single biggest profile
            // hotspot (~30 % of ST samples) because TT[2] is large (2 GB) and
            // most lookups miss the cache. Doing 30 prefetches up-front and then
            // 30 recursions lets the DRAM lines stream in during the work.
            CMove moves[MAX_MOVES];
            int nMoves = MoveGeneratorBitboard::generateMoves<chance>(pos, gs, moves);

            // Per-child buffers (kept in cache; nMoves ≤ MAX_MOVES so this is
            // ~13 KB of stack, fine for the 1-level-deep recursion at depth 3).
            QuadBitBoard childPos [MAX_MOVES];
            GameState    childGs  [MAX_MOVES];
            Hash128      childHash[MAX_MOVES];

            const ShallowTT &tt2 = hostShallowTT2;

            for (int i = 0; i < nMoves; i++)
            {
                childPos[i] = *pos;
                childGs [i] = *gs;
                uint8 srcPiece     = getPieceAt(pos, moves[i].getFrom());
                uint8 capPiece     = getPieceAt(pos, moves[i].getTo());
                uint8 oldCastleRaw = gs->raw;
                uint8 oldEP        = gs->enPassent;
                MoveGeneratorBitboard::makeMove<chance>(&childPos[i], &childGs[i], moves[i]);
                childHash[i] = updateHashAfterMove(hash, moves[i], chance,
                    srcPiece, capPiece, oldCastleRaw, childGs[i].raw, oldEP, childGs[i].enPassent);
                if (tt2.entries)
                {
                    uint64 idx = childHash[i].lo & tt2.mask;
                    _mm_prefetch((const char *)&tt2.entries[idx], _MM_HINT_T0);
                }
            }

            uint64 count = 0;
            for (int i = 0; i < nMoves; i++)
                count += perft_cpu<!chance, true>(&childPos[i], &childGs[i], 2, childHash[i]);

            perftTTStore(3, hash, count);
            return count;
        }
        return MoveGeneratorBitboard::countThreeLevelSubtree<chance>(pos, gs);
    }

    // Depth >= 4.
    if constexpr (useTT)
    {
        uint64 ttCount;
        if (perftTTProbe(depth, hash, &ttCount))
            return ttCount;
    }

    CMove moves[MAX_MOVES];
    int nMoves = MoveGeneratorBitboard::generateMoves<chance>(pos, gs, moves);

    uint64 count = 0;
    if constexpr (useTT)
    {
        // Same SoA + prefetch pattern as depth==3. The biggest gain comes at
        // depth==4 where the child probes TT[3] (a 16 GB lossy table that's
        // almost entirely in DRAM); deeper depths get progressively smaller
        // wins but the cost is uniform.
        QuadBitBoard childPos [MAX_MOVES];
        GameState    childGs  [MAX_MOVES];
        Hash128      childHash[MAX_MOVES];

        for (int i = 0; i < nMoves; i++)
        {
            childPos[i] = *pos;
            childGs [i] = *gs;
            uint8 srcPiece     = getPieceAt(pos, moves[i].getFrom());
            uint8 capPiece     = getPieceAt(pos, moves[i].getTo());
            uint8 oldCastleRaw = gs->raw;
            uint8 oldEP        = gs->enPassent;
            MoveGeneratorBitboard::makeMove<chance>(&childPos[i], &childGs[i], moves[i]);
            childHash[i] = updateHashAfterMove(hash, moves[i], chance,
                srcPiece, capPiece, oldCastleRaw, childGs[i].raw, oldEP, childGs[i].enPassent);
            perftTTPrefetch((int)depth - 1, childHash[i]);
        }
        for (int i = 0; i < nMoves; i++)
            count += perft_cpu<!chance, true>(&childPos[i], &childGs[i], depth - 1, childHash[i]);
        perftTTStore(depth, hash, count);
    }
    else
    {
        for (int i = 0; i < nMoves; i++)
        {
            QuadBitBoard childPos = *pos;
            GameState childGs = *gs;
            MoveGeneratorBitboard::makeMove<chance>(&childPos, &childGs, moves[i]);
            count += perft_cpu<!chance, false>(&childPos, &childGs, depth - 1, hash);
        }
    }

    return count;
}

// Two-level fan-out: distribute work across (root × ply-1) move pairs rather
// than just root moves. Kiwipete has ~48 root moves and ~46 average branching
// at ply 1 → ~2200 tasks vs. 48 with single-level distribution. With 32 threads
// the tail latency is bounded by the largest depth-(N-2) subtree, not the
// largest depth-(N-1) subtree, which closes most of the load-imbalance gap.
//
// Used only when depth >= 4 (otherwise the inner perft is trivial and serial
// setup dominates).
template <uint8 chance, bool useTT>
static uint64 perft_cpu_mt2(QuadBitBoard *pos, GameState *gs, uint32 depth, Hash128 rootHash)
{
    CMove rootMoves[MAX_MOVES];
    int nRootMoves = MoveGeneratorBitboard::generateMoves<chance>(pos, gs, rootMoves);

    struct Task {
        QuadBitBoard pos;
        GameState    gs;
        Hash128      hash;  // unused unless useTT
    };
    std::vector<Task> tasks;
    tasks.reserve((size_t)nRootMoves * 64);

    for (int i = 0; i < nRootMoves; i++)
    {
        QuadBitBoard childPos = *pos;
        GameState    childGs  = *gs;
        Hash128      childHash = rootHash;

        if constexpr (useTT)
        {
            uint8 srcPiece     = getPieceAt(pos, rootMoves[i].getFrom());
            uint8 capPiece     = getPieceAt(pos, rootMoves[i].getTo());
            uint8 oldCastleRaw = gs->raw;
            uint8 oldEP        = gs->enPassent;
            MoveGeneratorBitboard::makeMove<chance>(&childPos, &childGs, rootMoves[i]);
            childHash = updateHashAfterMove(rootHash, rootMoves[i], chance,
                srcPiece, capPiece, oldCastleRaw, childGs.raw, oldEP, childGs.enPassent);
        }
        else
        {
            MoveGeneratorBitboard::makeMove<chance>(&childPos, &childGs, rootMoves[i]);
        }

        CMove childMoves[MAX_MOVES];
        int nChildMoves = MoveGeneratorBitboard::generateMoves<!chance>(&childPos, &childGs, childMoves);

        for (int j = 0; j < nChildMoves; j++)
        {
            Task t;
            t.pos  = childPos;
            t.gs   = childGs;
            t.hash = childHash;

            if constexpr (useTT)
            {
                uint8 srcPiece     = getPieceAt(&childPos, childMoves[j].getFrom());
                uint8 capPiece     = getPieceAt(&childPos, childMoves[j].getTo());
                uint8 oldCastleRaw = childGs.raw;
                uint8 oldEP        = childGs.enPassent;
                MoveGeneratorBitboard::makeMove<!chance>(&t.pos, &t.gs, childMoves[j]);
                t.hash = updateHashAfterMove(childHash, childMoves[j], !chance,
                    srcPiece, capPiece, oldCastleRaw, t.gs.raw, oldEP, t.gs.enPassent);
            }
            else
            {
                MoveGeneratorBitboard::makeMove<!chance>(&t.pos, &t.gs, childMoves[j]);
            }
            tasks.push_back(t);
        }
    }

    const int nTasks = (int)tasks.size();
    std::atomic<int>    next(0);
    std::atomic<uint64> totalCount(0);
    int numWorkers = g_numThreads;
    if (numWorkers > nTasks) numWorkers = nTasks;
    if (numWorkers < 1)      numWorkers = 1;

    auto worker = [&]() {
        uint64 localCount = 0;
        while (true)
        {
            int i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= nTasks) break;

            uint64 c;
            if constexpr (useTT)
            {
                c = perft_cpu<chance, true>(&tasks[i].pos, &tasks[i].gs, depth - 2, tasks[i].hash);
            }
            else
            {
                Hash128 dummy{0, 0};
                c = perft_cpu<chance, false>(&tasks[i].pos, &tasks[i].gs, depth - 2, dummy);
            }
            localCount += c;
        }
        totalCount.fetch_add(localCount, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    threads.reserve(numWorkers - 1);
    for (int t = 1; t < numWorkers; t++) threads.emplace_back(worker);
    worker();
    for (auto &th : threads) th.join();

    uint64 result = totalCount.load(std::memory_order_relaxed);
    if constexpr (useTT)
        perftTTStore(depth, rootHash, result);
    return result;
}

// Multi-threaded root: generate root moves, dynamically distribute them across
// `g_numThreads` workers via an atomic counter. Each worker recurses into the
// single-threaded perft_cpu path for its assigned moves.
//
// TT-aware: the host TT is thread-safe (saturating CAS slot allocator + CAS chain
// prepend; read-only probe). Two threads racing on the same position may briefly
// miss each other's stores (causing a tiny amount of duplicate work) but never
// corrupt the table.
template <uint8 chance, bool useTT>
static uint64 perft_cpu_mt(QuadBitBoard *pos, GameState *gs, uint32 depth, Hash128 rootHash)
{
    if (depth == 1)
        return MoveGeneratorBitboard::countMoves<chance>(pos, gs);

    CMove moves[MAX_MOVES];
    int nMoves = MoveGeneratorBitboard::generateMoves<chance>(pos, gs, moves);

    std::atomic<int> nextMove(0);
    std::atomic<uint64> totalCount(0);
    int numWorkers = g_numThreads;
    if (numWorkers > nMoves) numWorkers = nMoves;
    if (numWorkers < 1)      numWorkers = 1;

    auto worker = [&]() {
        uint64 localCount = 0;
        while (true)
        {
            int i = nextMove.fetch_add(1, std::memory_order_relaxed);
            if (i >= nMoves) break;

            QuadBitBoard childPos = *pos;
            GameState    childGs  = *gs;

            uint64 c;
            if constexpr (useTT)
            {
                uint8 srcPiece = getPieceAt(pos, moves[i].getFrom());
                uint8 capPiece = getPieceAt(pos, moves[i].getTo());
                uint8 oldCastleRaw = gs->raw;
                uint8 oldEP = gs->enPassent;

                MoveGeneratorBitboard::makeMove<chance>(&childPos, &childGs, moves[i]);

                Hash128 childHash = updateHashAfterMove(rootHash, moves[i], chance,
                    srcPiece, capPiece, oldCastleRaw, childGs.raw, oldEP, childGs.enPassent);

                c = ((!chance) == WHITE)
                    ? perft_cpu<WHITE, true>(&childPos, &childGs, depth - 1, childHash)
                    : perft_cpu<BLACK, true>(&childPos, &childGs, depth - 1, childHash);
            }
            else
            {
                Hash128 dummy = {0, 0};
                MoveGeneratorBitboard::makeMove<chance>(&childPos, &childGs, moves[i]);
                c = ((!chance) == WHITE)
                    ? perft_cpu<WHITE, false>(&childPos, &childGs, depth - 1, dummy)
                    : perft_cpu<BLACK, false>(&childPos, &childGs, depth - 1, dummy);
            }
            localCount += c;
        }
        totalCount.fetch_add(localCount, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    threads.reserve(numWorkers - 1);
    for (int t = 1; t < numWorkers; t++) threads.emplace_back(worker);
    worker();
    for (auto &th : threads) th.join();

    uint64 result = totalCount.load(std::memory_order_relaxed);

    if constexpr (useTT)
        perftTTStore(depth, rootHash, result);
    return result;
}

uint64 perft_cpu_dispatch(QuadBitBoard *pos, GameState *gs, uint8 color, uint32 depth)
{
    if (depth == 0)
        return 1;

    if (g_useTT)
    {
        Hash128 hash = computeHash(pos, gs, color);
        if (g_numThreads > 1 && depth >= 4)
        {
            if (color == WHITE) return perft_cpu_mt2<WHITE, true>(pos, gs, depth, hash);
            else                return perft_cpu_mt2<BLACK, true>(pos, gs, depth, hash);
        }
        if (g_numThreads > 1 && depth >= 2)
        {
            if (color == WHITE) return perft_cpu_mt<WHITE, true>(pos, gs, depth, hash);
            else                return perft_cpu_mt<BLACK, true>(pos, gs, depth, hash);
        }
        if (color == WHITE) return perft_cpu<WHITE, true>(pos, gs, depth, hash);
        else                return perft_cpu<BLACK, true>(pos, gs, depth, hash);
    }

    Hash128 dummy = {0, 0};
    if (g_numThreads > 1 && depth >= 4)
    {
        if (color == WHITE) return perft_cpu_mt2<WHITE, false>(pos, gs, depth, dummy);
        else                return perft_cpu_mt2<BLACK, false>(pos, gs, depth, dummy);
    }
    if (g_numThreads > 1 && depth >= 2)
    {
        if (color == WHITE) return perft_cpu_mt<WHITE, false>(pos, gs, depth, dummy);
        else                return perft_cpu_mt<BLACK, false>(pos, gs, depth, dummy);
    }
    if (color == WHITE) return perft_cpu<WHITE, false>(pos, gs, depth, dummy);
    else                return perft_cpu<BLACK, false>(pos, gs, depth, dummy);
}

void perftCPU(QuadBitBoard *pos, GameState *gs, uint8 rootColor, uint32 depth)
{
    Timer timer;
    timer.start();
    uint64 result = perft_cpu_dispatch(pos, gs, rootColor, depth);
    timer.stop();
    double seconds = timer.elapsed();

    printf("\nPerft(%02d): %llu, time: %g seconds", depth, (unsigned long long)result, seconds);
    if (seconds > 0)
        printf(", nps: %llu", (unsigned long long)((double)result / seconds));
    printf("\n");
    fflush(stdout);
}

// Optional TT fill report. Scanning a lossy TT for populated slots is O(capacity)
// which can be huge (a 16 GB TT[3] = 1 billion entries) — call only on demand,
// not after every perft iteration.
void printTTFillReport()
{
    if (hostShallowTT2.entries)
    {
        uint64 cap = hostShallowTT2.mask + 1;
        uint64 used = 0;
        for (uint64 i = 0; i < cap; i++)
            if (hostShallowTT2.entries[i]) used++;
        double pct = 100.0 * (double)used / (double)cap;
        printf("  TT[2] (shallow):  used %llu / %llu (%.1f%%)\n",
               (unsigned long long)used, (unsigned long long)cap, pct);
    }
    for (int d = 3; d < MAX_TT_DEPTH; d++)
    {
        if (hostTTs[d].entries)
        {
            uint64 cap  = hostTTs[d].mask + 1;
            uint64 used = 0;
            for (uint64 i = 0; i < cap; i++)
                if (hostTTs[d].entries[i].verification | hostTTs[d].entries[i].count)
                    used++;
            double pct = 100.0 * (double)used / (double)cap;
            printf("  TT[%d] (lossy):    used %llu / %llu (%.1f%%)\n", d,
                   (unsigned long long)used, (unsigned long long)cap, pct);
        }
        else if (hostLosslessTTs[d].buckets)
        {
            int32_t used = hostLosslessTTs[d].nextFree;
            if (used > hostLosslessTTs[d].poolCapacity) used = hostLosslessTTs[d].poolCapacity;
            double pct = 100.0 * (double)used / (double)hostLosslessTTs[d].poolCapacity;
            printf("  TT[%d] (lossless): used %d / %d (%.1f%%)\n", d, used,
                   hostLosslessTTs[d].poolCapacity, pct);
        }
    }
    fflush(stdout);
}

// MoveGeneratorBitboard::init() lives in move_gen_init.cpp.
void initMoveGen()
{
    MoveGeneratorBitboard::init();
}
