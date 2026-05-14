# perft_cpu_2026

A heavily optimized, multi-threaded CPU chess [perft](https://www.chessprogramming.org/Perft) calculator. Single binary, cross-platform (x86-64 + ARM64), no GPU required.

This is the CPU-only fork of [perft_gpu](https://github.com/ankan-ban/perft_gpu_2026). The move generator is the same magic-bitboard implementation; the GPU BFS infrastructure has been removed. What remains are the CPU-side optimizations that grew out of tuning for Snapdragon X Oryon (ARM64) and that apply equally well to x86-64.

## Usage

```
perft_cpu <fen> <depth> [-nott] [-htt <MB>] [-mt <N>]
```

| Flag | Description |
|---|---|
| `<fen>` | FEN string for the position to analyze |
| `<depth>` | Maximum perft depth (computes 1..depth inclusive) |
| `-nott` | Disable transposition tables (raw move generation throughput) |
| `-htt <MB>` | Host TT memory budget in MB (default: auto, 90% of system RAM) |
| `-mt <N>` | Run with N threads at the root (0 = all hardware threads). Works with TT — the host TT is lockless. |

Transposition tables are enabled by default. Deep perft benefits massively from the TT; the `-nott` flag is useful when measuring raw move-generation throughput.

### Examples

```sh
# Starting position, depth 9, single-threaded with TT
perft_cpu "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1" 9

# Same, 8 worker threads
perft_cpu "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1" 9 -mt 8

# Kiwipete position, depth 7, no TT — pure move-generation throughput
perft_cpu "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -" 7 -nott
```

## Building

Requirements:
- CMake 3.18+
- C++20 compiler (MSVC 2022 / clang 14+ / gcc 11+)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

The binary lands in `build/Release/perft_cpu.exe` (MSVC) or `build/perft_cpu` (gcc/clang).

### Platform notes

- **MSVC ARM64** picks up `/arch:armv8.4` automatically to enable hardware popcount (CNT) and bit reverse (RBIT).
- **MSVC x64** uses the hardware POPCNT / BSF intrinsics directly.
- **gcc/clang on x86-64** gets `-mpopcnt -mbmi` added so `__builtin_popcountll` / `__builtin_ctzll` use single instructions.
- **gcc/clang on ARM64** uses the standard `__builtin_*` portable intrinsics — the toolchain already targets `armv8-a` by default which has CNT/RBIT.

## How it works

The perft tree is explored by a recursive, template-specialized count function. The hot path looks like:

1. `perft_cpu<chance, useTT>` — recursion dispatcher templated on side-to-move and TT mode so the optimizer eliminates dead code completely (e.g. all the Zobrist hash bookkeeping vanishes under `-nott`).
2. `MoveGeneratorBitboard::countMoves<chance>` — leaf counter at depth 1.
3. `countTwoLevelSubtree` / `countThreeLevelSubtree` — fused generate-make-count for depth 2/3 leaves under `-nott`. Avoids materialising the CMove array, saving roughly the cost of an entire stack frame's worth of bookkeeping per move.
4. Root-level parallelism via `-mt N`: workers grab moves from a `std::atomic<int>` counter and each recurses into the single-threaded `perft_cpu` for its assigned move.

The transposition table is a **lossless chained hash table** keyed on a 128-bit Zobrist hash:
- One TT per depth (so a position can be cached at multiple depths without aliasing).
- 128-bit keys mean collisions are effectively impossible, so chained entries are never evicted.
- Concurrent stores use a saturating CAS slot allocator + a seq-cst CAS chain prepend. Lookups are read-only. Two threads racing on the same position may briefly miss each other's stores (causing a tiny amount of duplicate work) but never corrupt the table.
- On ARM64, the bucket-head read uses a `volatile` load (which gives acquire semantics under MSVC's `/volatile:ms`) so newly-published entries are not seen via stale pool reads.

## File overview

| File | Description |
|---|---|
| `perft.cpp` | Entry point, CLI parsing |
| `launcher.cpp` / `launcher.h` | CPU perft launchers (single + multi-threaded), host TT allocation |
| `move_gen_init.cpp` | One-time initialization of move-gen LUTs and magic tables |
| `MoveGeneratorBitboard.h` | Bitboard-based legal move generation (~1900 lines) |
| `chess.h` | Core data structures (`QuadBitBoard`, `GameState`, `CMove`, magic entries) |
| `switches.h` | Compile-time flags (`HOST_TT_BUDGET_MB`, `VERBOSE_LOGGING`) |
| `tt.h` | Transposition table (`LosslessTT`, lockless probe/store) |
| `zobrist.h` / `zobrist.cpp` / `randoms.cpp` | 128-bit Zobrist hashing |
| `uint128.h` | 128-bit unsigned int (used only for very deep perft accumulation) |
| `utils.h` / `util.cpp` | FEN parsing, board display, Timer |
| `GlobalVars.cpp` | Pre-validated magic factors, attack-table storage |
| `Magics.cpp` | Magic-number search / verification at startup |
| `bench.ps1` | Build + benchmark harness (Windows; bypasses Defender file-lock by running from `%TEMP%`) |

## License

MIT
