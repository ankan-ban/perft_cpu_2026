#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "Winmm.lib")
#endif
#include "utils.h"
#include "launcher.h"
#include "MoveGeneratorBitboard.h"

// -------------------------------------------------------------------
// Userland sampling profiler (no admin needed — uses SuspendThread).
// Samples the main thread's RIP at ~10 kHz, resolves to symbols via
// dbghelp at the end. Useful only for single-thread perft runs.
// -------------------------------------------------------------------
#ifdef _WIN32
namespace prof {

static std::vector<uintptr_t> g_samples;
static HANDLE                  g_targetThread = nullptr;
static std::atomic<bool>       g_stop{false};
static std::thread             g_samplerThread;

static void samplerFunc()
{
    timeBeginPeriod(1);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    LONGLONG ticksPer100us = freq.QuadPart / 10000;
    g_samples.reserve(1000000);

    while (!g_stop.load(std::memory_order_acquire))
    {
        LARGE_INTEGER s;
        QueryPerformanceCounter(&s);
        DWORD prev = SuspendThread(g_targetThread);
        if (prev != (DWORD)-1)
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(g_targetThread, &ctx))
                g_samples.push_back((uintptr_t)ctx.Rip);
            ResumeThread(g_targetThread);
        }
        LARGE_INTEGER e;
        do { QueryPerformanceCounter(&e); }
        while ((e.QuadPart - s.QuadPart) < ticksPer100us);
    }
    timeEndPeriod(1);
}

static void start()
{
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &dup,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, 0);
    g_targetThread = dup;
    g_stop.store(false);
    g_samplerThread = std::thread(samplerFunc);
}

static void stopAndPrint(int topN)
{
    g_stop.store(true);
    if (g_samplerThread.joinable()) g_samplerThread.join();
    if (g_targetThread) { CloseHandle(g_targetThread); g_targetThread = nullptr; }

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);

    std::map<std::string, int> hist;
    std::map<std::string, std::map<std::string, int>> hist_by_line;

    char symBuf[sizeof(SYMBOL_INFO) + 1024]{};
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symBuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 1023;

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    for (uintptr_t rip : g_samples)
    {
        DWORD64 disp = 0;
        std::string name = "<unresolved>";
        if (SymFromAddr(proc, (DWORD64)rip, &disp, sym))
            name = sym->Name;
        hist[name]++;

        DWORD ldisp = 0;
        if (SymGetLineFromAddr64(proc, (DWORD64)rip, &ldisp, &line))
        {
            const char *base = line.FileName;
            if (const char *slash = strrchr(base, '\\')) base = slash + 1;
            char loc[256];
            snprintf(loc, sizeof(loc), "%s:%lu", base, (unsigned long)line.LineNumber);
            hist_by_line[name][loc]++;
        }
    }

    std::vector<std::pair<int, std::string>> sorted;
    for (auto &kv : hist) sorted.push_back({kv.second, kv.first});
    std::sort(sorted.rbegin(), sorted.rend());

    int total = (int)g_samples.size();
    printf("\n=== CPU Profile: %d samples @ ~10 kHz ===\n", total);
    if (total == 0) { printf("  (no samples — profile target finished too fast?)\n"); SymCleanup(proc); return; }

    for (size_t i = 0; i < std::min((size_t)topN, sorted.size()); i++)
    {
        const auto &name = sorted[i].second;
        printf("%6d  %5.2f%%  %s\n",
               sorted[i].first, 100.0 * sorted[i].first / total, name.c_str());
        auto it = hist_by_line.find(name);
        if (it == hist_by_line.end()) continue;
        std::vector<std::pair<int, std::string>> lines;
        for (auto &kv : it->second) lines.push_back({kv.second, kv.first});
        std::sort(lines.rbegin(), lines.rend());
        for (size_t j = 0; j < std::min((size_t)5, lines.size()); j++)
            printf("              %5d  %5.2f%%   %s\n",
                   lines[j].first, 100.0 * lines[j].first / total, lines[j].second.c_str());
    }
    SymCleanup(proc);
}

} // namespace prof
#endif

// Microbench helper for `-bench-leaf`. noinline so the loop can't be hoisted
// past the entry point; the volatile load of pos->bb[0] forces re-reading
// each iteration so the optimizer can't compute the result once.
template <uint8 chance>
__declspec(noinline)
static uint64 benchLeafLoop(QuadBitBoard *pos, GameState *gs, int iters)
{
    uint64 sum = 0;
    volatile uint64 *p = &pos->bb[0];
    for (int j = 0; j < iters; j++)
    {
        (void)*p;  // forces re-read of pos so dispatch can't be hoisted
        sum += MoveGeneratorBitboard::countMovesDispatch<chance>(pos, gs);
    }
    return sum;
}

// Bench bishopAttacks and rookAttacks magic lookups in isolation.
__declspec(noinline)
static uint64 benchMagicLoop(uint64 occ, int iters)
{
    uint64 sum = 0;
    volatile uint64 *po = &occ;
    for (int j = 0; j < iters; j++)
    {
        uint64 o = *po;
        // 8 bishop + 8 rook lookups spread across squares = 16 lookups per iter,
        // representative of `findAttackedSquares` + own-slider counting.
        sum += MoveGeneratorBitboard::bishopAttacks(BIT(2),  ~o);
        sum += MoveGeneratorBitboard::bishopAttacks(BIT(5),  ~o);
        sum += MoveGeneratorBitboard::bishopAttacks(BIT(18), ~o);
        sum += MoveGeneratorBitboard::bishopAttacks(BIT(27), ~o);
        sum += MoveGeneratorBitboard::bishopAttacks(BIT(36), ~o);
        sum += MoveGeneratorBitboard::bishopAttacks(BIT(45), ~o);
        sum += MoveGeneratorBitboard::bishopAttacks(BIT(58), ~o);
        sum += MoveGeneratorBitboard::bishopAttacks(BIT(61), ~o);
        sum += MoveGeneratorBitboard::rookAttacks  (BIT(0),  ~o);
        sum += MoveGeneratorBitboard::rookAttacks  (BIT(7),  ~o);
        sum += MoveGeneratorBitboard::rookAttacks  (BIT(20), ~o);
        sum += MoveGeneratorBitboard::rookAttacks  (BIT(27), ~o);
        sum += MoveGeneratorBitboard::rookAttacks  (BIT(36), ~o);
        sum += MoveGeneratorBitboard::rookAttacks  (BIT(43), ~o);
        sum += MoveGeneratorBitboard::rookAttacks  (BIT(56), ~o);
        sum += MoveGeneratorBitboard::rookAttacks  (BIT(63), ~o);
    }
    return sum;
}

int main(int argc, char *argv[])
{
    int benchLeafIters  = 0;
    int benchMagicIters = 0;
    bool doProfile      = false;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-nott") == 0)
        {
            g_useTT = false;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--; i--; continue;
        }
        if (strcmp(argv[i], "-htt") == 0 && i + 1 < argc)
        {
            g_hostTTBudgetMB = atoi(argv[i + 1]);
            for (int j = i; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2; i--; continue;
        }
        if (strcmp(argv[i], "-mt") == 0 && i + 1 < argc)
        {
            // -mt N : run with N threads at the root (0 = all hw threads).
            int n = atoi(argv[i + 1]);
            if (n <= 0) n = (int)std::thread::hardware_concurrency();
            if (n < 1)  n = 1;
            g_numThreads = n;
            for (int j = i; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2; i--; continue;
        }
        if (strcmp(argv[i], "-bench-leaf") == 0 && i + 1 < argc)
        {
            benchLeafIters = atoi(argv[i + 1]);
            for (int j = i; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2; i--; continue;
        }
        if (strcmp(argv[i], "-bench-magic") == 0 && i + 1 < argc)
        {
            benchMagicIters = atoi(argv[i + 1]);
            for (int j = i; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2; i--; continue;
        }
        if (strcmp(argv[i], "-profile") == 0)
        {
            doProfile = true;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--; i--; continue;
        }
    }

    char fen[1024] = "";
    int maxDepth = 7;

    if (argc >= 3)
    {
        strcpy(fen, argv[1]);
        maxDepth = atoi(argv[2]);
    }
    else
    {
        printf("Usage: perft_cpu <fen> <depth> [-nott] [-htt <MB>] [-mt <N>]\n");
        printf("  -nott      : disable transposition tables\n");
        printf("  -htt <MB>  : host TT budget in MB (default: 90%% of system RAM)\n");
        printf("  -mt <N>    : run with N threads at the root (0 = all hw threads).\n");
        printf("               Works with TT enabled — the host TT is lockless.\n\n");
        printf("As no parameters were provided... running default test\n");
    }

    initMoveGen();

    QuadBitBoard testBB;
    GameState testGS;
    uint8 rootColor;

    if (strlen(fen) > 5)
        readFENString(fen, &testBB, &testGS, &rootColor);
    else
        readFENString("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &testBB, &testGS, &rootColor);

    printf("CPU mode (threads: %d, TT: %s)\n", g_numThreads, g_useTT ? "on" : "off");
    fflush(stdout);

    if (benchLeafIters > 0)
    {
        Timer t;
        t.start();
        uint64 sum = (rootColor == WHITE)
            ? benchLeafLoop<WHITE>(&testBB, &testGS, benchLeafIters)
            : benchLeafLoop<BLACK>(&testBB, &testGS, benchLeafIters);
        t.stop();
        double secs = t.elapsed();
        double ns   = secs * 1e9 / benchLeafIters;
        printf("countMovesDispatch bench: %d iters, %.4f s, %.2f ns/call, sum=%llu\n",
            benchLeafIters, secs, ns, (unsigned long long)sum);
        return 0;
    }

    if (benchMagicIters > 0)
    {
        uint64 occ = testBB.bb[1] | testBB.bb[2] | testBB.bb[3];
        Timer t;
        t.start();
        uint64 sum = benchMagicLoop(occ, benchMagicIters);
        t.stop();
        double secs = t.elapsed();
        double nsPerCall = secs * 1e9 / ((double)benchMagicIters * 16.0);
        printf("magic bench: %d iters x 16 = %lld lookups, %.4f s, %.2f ns/lookup, sum=%llu\n",
            benchMagicIters, (long long)benchMagicIters * 16, secs, nsPerCall, (unsigned long long)sum);
        return 0;
    }

    initTT(maxDepth, 30.0f);

#ifdef _WIN32
    if (doProfile) prof::start();
#endif

    for (int depth = 1; depth <= maxDepth; depth++)
    {
        perftCPU(&testBB, &testGS, rootColor, depth);
        fflush(stdout);
    }

#ifdef _WIN32
    if (doProfile) prof::stopAndPrint(15);
#endif

    freeTT();
    return 0;
}
