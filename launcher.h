#pragma once

#include "chess.h"

// Runtime TT toggle (default: enabled, disable with -nott CLI flag)
extern bool g_useTT;

// Number of threads for CPU perft root-level parallelism (default 1).
extern int g_numThreads;

// Host TT budget in MB (0 = auto: 90% of system RAM).
extern int g_hostTTBudgetMB;

void initMoveGen();

void perftCPU(QuadBitBoard *pos, GameState *gs, uint8 rootColor, uint32 depth);
uint64 perft_cpu_dispatch(QuadBitBoard *pos, GameState *gs, uint8 color, uint32 depth);

void initTT(int maxDepth, float branchingFactor);
void freeTT();
