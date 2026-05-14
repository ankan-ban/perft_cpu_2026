// CPU perft launchers + host transposition table management.

#include <math.h>
#include <thread>
#include <atomic>
#include <vector>
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

// Runtime TT toggle (default: enabled, disable with -nott CLI flag)
bool g_useTT = true;

// Number of CPU threads used at the root of CPU perft. Default 1 (single-threaded).
// Set via -mt N. Works with both `-nott` and the default TT path — the lossless
// host TT is thread-safe (atomic slot-bump + CAS chain prepend).
int g_numThreads = 1;

LosslessTT hostLosslessTTs[MAX_TT_DEPTH];

// Overridable host TT budget (set from CLI before calling initTT)
int g_hostTTBudgetMB = HOST_TT_BUDGET_MB;

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
    memset(hostLosslessTTs, 0, sizeof(hostLosslessTTs));
    if (!g_useTT) return;

    // Host TTs for depths 2..maxDepth (depth 1 doesn't recurse).
    int numHostTTs = 0;
    for (int d = 2; d <= maxDepth && d < MAX_TT_DEPTH; d++)
        numHostTTs++;
    if (numHostTTs == 0) return;

    // Auto-detect: 90% of total system RAM
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
            g_hostTTBudgetMB = 8192;  // fallback
        printf("Host TT budget: auto %d MB (90%% of %llu MB system RAM)\n",
               g_hostTTBudgetMB, (unsigned long long)(totalRAM / (1024 * 1024)));
    }
    uint64 budgetBytes = (uint64)g_hostTTBudgetMB * 1024 * 1024;

    // Budget split proportional to branchingFactor^(maxDepth - d). Shallow depths
    // (small d) get the lion's share since they hold many more unique entries.
    double weights[MAX_TT_DEPTH];
    memset(weights, 0, sizeof(weights));
    double totalWeight = 0;
    for (int d = 2; d <= maxDepth && d < MAX_TT_DEPTH; d++)
    {
        weights[d] = pow((double)branchingFactor, maxDepth - d);
        totalWeight += weights[d];
    }

    uint64 bytesPerSlot = sizeof(LosslessEntry) + sizeof(int32_t);  // pool entry + bucket head

    for (int d = 2; d <= maxDepth && d < MAX_TT_DEPTH; d++)
    {
        uint64 depthBytes = (uint64)(budgetBytes * weights[d] / totalWeight);
        uint64 numSlots = depthBytes / bytesPerSlot;
        numSlots = floorPow2(numSlots);
        if (numSlots < 4 * 1024 * 1024) numSlots = 4 * 1024 * 1024;
        if (numSlots > (1ull << 30)) numSlots = (1ull << 30);  // cap for int32_t safety

        int32_t poolCap = (int32_t)numSlots;
        uint64 numBuckets = numSlots;

        hostLosslessTTs[d].buckets = (int32_t *)malloc(numBuckets * sizeof(int32_t));
        hostLosslessTTs[d].pool = (LosslessEntry *)malloc((uint64)poolCap * sizeof(LosslessEntry));
        if (hostLosslessTTs[d].buckets && hostLosslessTTs[d].pool)
        {
            memset(hostLosslessTTs[d].buckets, 0xFF, numBuckets * sizeof(int32_t));  // -1 = empty
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
    for (int d = 0; d < MAX_TT_DEPTH; d++)
    {
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

    if (useTT)
    {
        uint64 ttCount;
        if (losslessProbe(hostLosslessTTs[depth], hash, &ttCount))
            return ttCount;
    }

    if (!useTT && depth == 2)
        return MoveGeneratorBitboard::countTwoLevelSubtree<chance>(pos, gs);
    if (!useTT && depth == 3)
        return MoveGeneratorBitboard::countThreeLevelSubtree<chance>(pos, gs);

    CMove moves[MAX_MOVES];
    int nMoves = MoveGeneratorBitboard::generateMoves<chance>(pos, gs, moves);

    uint64 count = 0;
    if (depth == 2)
    {
        // useTT path uses the explicit two-pass leaf so per-child state isn't needed.
        for (int i = 0; i < nMoves; i++)
        {
            QuadBitBoard childPos = *pos;
            GameState childGs = *gs;
            MoveGeneratorBitboard::makeMove<chance>(&childPos, &childGs, moves[i]);
            count += MoveGeneratorBitboard::countMoves<!chance>(&childPos, &childGs);
        }
    }
    else
    {
        for (int i = 0; i < nMoves; i++)
        {
            QuadBitBoard childPos = *pos;
            GameState childGs = *gs;

            if (useTT)
            {
                uint8 srcPiece = getPieceAt(pos, moves[i].getFrom());
                uint8 capPiece = getPieceAt(pos, moves[i].getTo());
                uint8 oldCastleRaw = gs->raw;
                uint8 oldEP = gs->enPassent;

                MoveGeneratorBitboard::makeMove<chance>(&childPos, &childGs, moves[i]);

                Hash128 childHash = updateHashAfterMove(hash, moves[i], chance,
                    srcPiece, capPiece, oldCastleRaw, childGs.raw, oldEP, childGs.enPassent);

                count += perft_cpu<!chance, true>(&childPos, &childGs, depth - 1, childHash);
            }
            else
            {
                MoveGeneratorBitboard::makeMove<chance>(&childPos, &childGs, moves[i]);
                count += perft_cpu<!chance, false>(&childPos, &childGs, depth - 1, hash);
            }
        }
    }

    if (useTT)
        losslessStore(hostLosslessTTs[depth], hash, count);

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
        losslessStore(hostLosslessTTs[depth], rootHash, result);
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
        losslessStore(hostLosslessTTs[depth], rootHash, result);
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

// MoveGeneratorBitboard::init() lives in move_gen_init.cpp.
void initMoveGen()
{
    MoveGeneratorBitboard::init();
}
