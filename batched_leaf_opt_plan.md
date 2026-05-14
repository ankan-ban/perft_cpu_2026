# Batched Depth-2 Leaf — Optimization Plan

**Target:** push `kiwipete perft 5 -nott` single-thread on Intel Core Ultra 7 270K from
~85 ms → ≤65 ms (-24 %). Stretch: ≤60 ms.

**Why this approach:** the current 4-tick sprint exhausted every cheap source-level lever (see `optimization_log.md`). The remaining accessible budget sits inside `countMoves` — ~14 ns per call × 4 M calls ≈ 56 ms — and the only way to shrink it is to process **multiple children in parallel** so the per-call fixed costs (function-call overhead, DERIVE_PIECE_BITBOARDS, popcount accumulation, non-slider attack shifts) get amortized across siblings, and slider magic lookups can ILP across children.

---

## 1. Context (read this first)

### 1.1 Standing measurement (entering this work)
- ST `perft 5 -nott` pos2: **84.5 ms min / 85.7 ms median** over 15 runs (PGO retrained on Intel).
- ST `perft 6 -nott` pos2: ~4.45 s.
- Counts: **193,690,690** for perft 5, **8,031,647,685** for perft 6.
- bench-leaf microbench: 14-17 ns/call.
- Build: `/O2 /Ob3 /Oi /Ot /fp:fast /Zi /GL` + `/LTCG /USEPROFILE`. No `/arch:AVX*`.

### 1.2 What we now know is dangerous (do NOT re-try unless you have new evidence)
| Mine | Lesson |
|---|---|
| `/arch:AVX2` globally | **-60 % cliff** (84 → 222 ms). YMM6-15 save/restore around `__declspec(noinline) countMoves`. |
| `/arch:AVX2` on an isolated leaf TU | **~10 ms overhead** vs SSE2, even with no PEXT use. The cliff is internal to AVX2-compiled code, not just at TU boundaries. |
| PEXT magic on Intel Lion Cove | **Slower per call than MUL+SHIFT** in microbench (20.6 ns vs 17.2 ns). Lion Cove's PEXT throughput on this access pattern is worse than scalar IMUL. |
| Changing `bitScan` return type uint8 → unsigned | **-35 % regression**. The 1-cycle `movzx eax, al` saved triggered cascading register-allocation damage. |
| Per-emit struct field loads (V1 / V3 designs) | Burn ~5-10 ms even on fast paths that fire <10 % of the time. |
| Fast-path heuristics on pos2 | Hit rate <10 % because 5 enemy sliders blanket the central board. |

### 1.3 Profile picture (perft 6 ST -nott, 60k samples)
- `countMoves<BLACK, noEP, castle>`: **63 %** of total time
  - line 1595 (`findAttackedSquares` call): **19.88 %**
  - line 1820 (unpinned rook table load): 4.26 %
  - line 1821 (popCount on rookMoves): 3.44 %
  - line 1592 (`findPinnedPieces`): 3.06 %
- `enumerateMoves<…FgmcCount2Processor>`: **16.5 %**
- `countMoves<BLACK, noEP, !castle>`: 14 %

`findAttackedSquares` + the unpinned slider loops + `findPinnedPieces` = ~31 % of perft. **That's the ~25 ms accessible budget**, and it's exactly the work that can be parallelized across siblings.

### 1.4 uArch facts that drive the design
- Lion Cove P-core: 64 KB L1i, 48 KB L1d, 3 MB L2 (per P-core), no SMT.
- AVX2 256-bit ops are available but the codegen cliff is real.
- SSE2 / `/arch:AVX` (VEX-encoded 128-bit) has **no cliff** — neutral vs default.
- 64-bit MUL/IMUL on YMM is **not available** without AVX-512. AVX-512 not on Arrow Lake. → Magic lookups stay scalar in any SIMD design.
- AVX2 `_mm256_i64gather_epi64`: 4 64-bit gathers in 1 instruction. Latency ~19 cycles, throughput ~5/8 cycles. Roughly the same total cycles as 4 sequential scalar loads but uses one µop slot.

---

## 2. Approaches evaluated

| Approach | Win estimate | Risk | Engineering |
|---|---|---|---|
| **A. Pair-wise SSE2 (2 children)** | 8-15 ms | Low | 4-6 h |
| **B. 4-way AVX2 batched** | 16-25 ms gross, **6-15 ms net** after cliff | Medium | 8-12 h |
| **C. SIMD bulk-ops in single-child countMoves** | 3-6 ms | Low | 2-3 h |
| **D. Hybrid buffer + per-pair SSE2 + 4-way AVX2 on the deepest hot block** | 15-20 ms | Medium | 8-10 h |

**Recommended: A first, then B as stretch.** A is the safest path to a measurable win and lays the architecture (buffered emit, refactored countMoves) that B can build on without rewriting from scratch. If A clears 10 ms, we're at 75 ms — short of 65 but a real result. Then B targets the additional 10 ms.

---

## 3. Detailed design — Phase A (Pair-wise SSE2)

### 3.1 Concept

```
ENUMERATEMOVES (parent, processor)
  for each legal move m:
    processor.emit<piece>(from, to, flags)   // currently: makeMove + countMoves immediately
                                              // new:       buffer it
processor.flush()                             // new: process buffered entries in pairs
```

The processor accumulates emitted children in a small stack buffer. After enumerateMoves returns, it processes the buffer two children at a time with SSE2 128-bit ops where the data fits naturally (4 bb[] planes per QBB → 2 QBBs in 4 XMM registers).

### 3.2 Data flow

#### 3.2.1 Buffer layout
```cpp
struct LeafBufEntry {
    QuadBitBoard cp;     // 32 bytes
    GameState    cgs;    // 1 byte
    // pad to 64 (one cache line)
    uint8 _pad[31];
};
static_assert(sizeof(LeafBufEntry) == 64, "");

struct FgmcCount2Processor {
    QuadBitBoard parent;
    GameState parentGs;
    uint64 sum;
    int bufN;
    LeafBufEntry buf[256];  // max moves per position
};
```

256 entries × 64 bytes = **16 KB on the stack frame** per countTwoLevelSubtree call. Lives in L1d during the parent's enumerateMoves. Each parent (~98 K of them in perft 5) calls countTwoLevelSubtree, so this stack region is reused.

Concern: the recursion to depth-3 has multiple countTwoLevelSubtree frames live simultaneously (depth-3 emit → countTwoLevelSubtree per child). Each frame carries 16 KB. With recursion depth 2-3 active at once, ~32-48 KB on stack. Tight but fits.

If this is a concern: shrink buf to 96 entries (kiwipete typical max legal moves ≈ 60-80) — 6 KB per frame. Add an assert.

#### 3.2.2 Emit becomes a buffer-write
```cpp
template <uint8 piece>
CPU_FORCE_INLINE void emit(uint8 from, uint8 to, uint8 flags) {
    LeafBufEntry &e = buf[bufN++];
    // makeMoveTFromParent writes directly into e.cp / e.cgs
    if constexpr (piece == BISHOP) {
        // existing queen-probe + makeMoveT logic, output target is e.cp/e.cgs
        ...
    }
    ...
    // NO countMovesDispatch here.
}
```

The makeMoveT body is unchanged — it just writes into a buffer slot instead of a stack local. Total per-emit work is identical, plus 1 incremented counter.

#### 3.2.3 Flush — the heart of the work
```cpp
uint64 flush() {
    uint64 sum = 0;
    int i = 0;
    constexpr uint8 leafChance = parentChance ^ 1;
    for (; i + 1 < bufN; i += 2) {
        sum += countMovesPair<leafChance>(&buf[i], &buf[i+1]);
    }
    if (i < bufN) {
        sum += countMovesDispatch<leafChance>(&buf[i].cp, &buf[i].cgs);  // scalar tail
    }
    return sum;
}
```

`countMovesPair<chance>(LeafBufEntry*, LeafBufEntry*)` is the new SIMD'd function: counts legal moves for **both** child positions and returns their sum.

### 3.3 `countMovesPair` implementation strategy

#### 3.3.1 What is parallelized across the pair
Operations on 64-bit bitboards that have **no random-access memory dependency** parallelize naturally with SSE2 128-bit ops. Two 64-bit values in one XMM register, single instruction operates on both:

| Step in countMoves | Per-child cost | Pair'd cost | Savings (×2 calls amortized) |
|---|---|---|---|
| DERIVE_PIECE_BITBOARDS (8 bitboard derivations) | ~10 cycles | ~6 cycles (SSE2 ANDs/ORs) | ~4 cycles / 2 = **2 cycles/child** |
| Pawn attacks (4 shifts + 2 ORs) | ~5 cycles | ~3 cycles | ~1/child |
| Knight attacks bulk (~10 ALU ops) | ~7 cycles | ~5 cycles | ~1/child |
| King attacks (~4 ops) | ~3 cycles | ~2 cycles | ~0.5/child |
| popcount accumulation across pawn/king/castle | ~3 cycles | ~2 cycles | ~0.5/child |
| **Total amortized win** | | | **~5 cycles/child = ~1 ns/child** |

At 4M children: ~4 ms saved from bulk-ops alone. The bigger win is below.

#### 3.3.2 What stays scalar but gets ILP benefits
The slider magic lookups have a random-access table dependency (`table[sq][idx]`) that can't easily vectorize without AVX2 gather. **But interleaving the two children's slider loops in scalar form lets MSVC's OoO scheduler issue 2 independent magic chains in parallel.**

Per pair, current sequential code:
```
child A: bishop loop (3 iters × 7 cycles = 21 cycles)
child A: rook loop   (3 iters × 7 cycles = 21 cycles)
child A: findAttackedSquares slider work (4 iters × 7 = 28 cycles)
child B: same (total 70 cycles for B)
total: 140 cycles
```

Interleaved scalar (each iter for both children together):
```
for each slider iter:
   sq_A = ...; sq_B = ...     // independent
   idxA = ...;  idxB = ...    // independent
   atkA = tableA[idxA]; atkB = tableB[idxB]  // 2 outstanding loads
```

OoO can keep 2 loads in flight, halving the latency-bound stalls. Estimated saving: ~3 cycles per slider × 12 sliders per pair = **~36 cycles per pair = ~7 ns per pair = ~3.5 ns/child**.

At 4 M children: ~14 ms saved from slider ILP.

#### 3.3.3 The branchy parts
Each child can independently be:
- In check (→ countMovesOutOfCheck path, different code)
- Have EP available
- Have castle rights
- Have pinned pieces (numbers/squares differ per child)

The first 3 are rare. The 4th is universal but the pinned set differs per child.

Strategy:
1. Compute `pinned_A`, `pinned_B` in parallel (the findPinnedPieces work is small).
2. Compute `threatened_A`, `threatened_B` in parallel.
3. Check both `threatened & king` simultaneously: `((threatened_A_AND_king_A) | (threatened_B_AND_king_B)) != 0`. If true (rare), fall back to scalar for whichever is in check.
4. Compute castle / EP results per child — these are small and branchy; do them scalar after the bulk SIMD work.

### 3.4 Concrete pair-wise SIMD primitives needed

```cpp
// Two 64-bit values packed in one __m128i. Order: { childA, childB } in lanes 0/1.
typedef __m128i bb2_t;

CPU_FORCE_INLINE bb2_t bb2_load(uint64 a, uint64 b) {
    return _mm_set_epi64x(b, a);   // careful: _mm_set lanes are reversed
}
CPU_FORCE_INLINE bb2_t bb2_and(bb2_t x, bb2_t y) { return _mm_and_si128(x, y); }
CPU_FORCE_INLINE bb2_t bb2_or (bb2_t x, bb2_t y) { return _mm_or_si128 (x, y); }
CPU_FORCE_INLINE bb2_t bb2_xor(bb2_t x, bb2_t y) { return _mm_xor_si128(x, y); }
CPU_FORCE_INLINE bb2_t bb2_andn(bb2_t y, bb2_t x) { return _mm_andnot_si128(y, x); } // x & ~y
CPU_FORCE_INLINE bb2_t bb2_shl(bb2_t x, int n)  { return _mm_slli_epi64(x, n); }
CPU_FORCE_INLINE bb2_t bb2_shr(bb2_t x, int n)  { return _mm_srli_epi64(x, n); }
CPU_FORCE_INLINE uint64 bb2_lane0(bb2_t x) { return _mm_cvtsi128_si64(x); }
CPU_FORCE_INLINE uint64 bb2_lane1(bb2_t x) { return (uint64)_mm_extract_epi64(x, 1); }
CPU_FORCE_INLINE int bb2_either_nonzero(bb2_t x) {
    // OR lanes, test nonzero — _mm_testz_si128 needs SSE4.1 (we have it under /arch:AVX)
    return !_mm_testz_si128(x, x);
}
```

Bulk operations like `pawnAttacksBulk` rewrite to 2-lane:
```cpp
template <uint8 color>
CPU_FORCE_INLINE bb2_t pawnAttacksBulk2(bb2_t pawns) {
    bb2_t fileA = _mm_set1_epi64x(FILEA);
    bb2_t fileH = _mm_set1_epi64x(FILEH);
    if constexpr (color == WHITE) {
        bb2_t ne = bb2_andn(fileA, bb2_shl(pawns, 9));   // (pawns<<9) & ~FILEA
        bb2_t nw = bb2_andn(fileH, bb2_shl(pawns, 7));
        return bb2_or(ne, nw);
    } else {
        bb2_t se = bb2_andn(fileA, bb2_shr(pawns, 7));
        bb2_t sw = bb2_andn(fileH, bb2_shr(pawns, 9));
        return bb2_or(se, sw);
    }
}
```

### 3.5 Slider loop interleaving (scalar but pair-wise)

```cpp
// Returns (attacks_A, attacks_B) packed in a bb2_t.
CPU_FORCE_INLINE bb2_t computeSliderAttacksPair(
    uint64 enemyBishops_A, uint64 enemyRooks_A, uint64 sliderOcc_A,
    uint64 enemyBishops_B, uint64 enemyRooks_B, uint64 sliderOcc_B)
{
    uint64 attA = 0, attB = 0;
    // Bishop loops, both children, interleaved:
    uint64 sA = enemyBishops_A, sB = enemyBishops_B;
    // Drive on whichever has bits left; interleave per iter.
    while (sA | sB) {
        if (sA) {
            uint8 sq = bitScan(sA);
            uint64 occ = sliderOcc_A & sqBishopAttacksMasked(sq);
            uint64 idx = (bishop_magic_factors[sq] * occ) >> 55;
            attA |= bishop_magic_tables[sq][idx];
            sA &= sA - 1;
        }
        if (sB) {
            uint8 sq = bitScan(sB);
            uint64 occ = sliderOcc_B & sqBishopAttacksMasked(sq);
            uint64 idx = (bishop_magic_factors[sq] * occ) >> 55;
            attB |= bishop_magic_tables[sq][idx];
            sB &= sB - 1;
        }
    }
    // Rook loops, same pattern
    ...
    return bb2_load(attA, attB);
}
```

Two independent magic chains — MSVC + OoO should issue 2 outstanding loads on Lion Cove (it has 3 load ports, more than enough). The branches are well-predicted: sA and sB stay non-zero in lockstep typically.

### 3.6 Where the savings actually come from (cycle accounting)

| Source | Per-pair cycles saved | × 2M pairs |
|---|---|---|
| Pair'd DERIVE_PIECE_BITBOARDS (4 SSE2 ops vs 8 scalar) | ~4 | 8M cycles |
| Pair'd pawn/knight/king attacks (~half the scalar ops) | ~6 | 12M cycles |
| Pair'd popcount accumulation (popcnt is scalar but accumulator OR can be SSE2) | ~2 | 4M cycles |
| Slider lookups: 2 children's chains interleaved (ILP exploits 2 load ports) | ~10 | 20M cycles |
| Function-call overhead amortized over 2 children | ~5 | 10M cycles |
| **Total** | **~27 cycles/pair** | **~54M cycles ≈ 11 ms** |

Estimated landing: **85 → 74 ms** (-13 %).

### 3.7 Risks for Phase A

| Risk | Likelihood | Mitigation |
|---|---|---|
| Stack frame growth (16 KB buf) causes L1d evictions | Medium | Shrink buf to 96 entries; profile with VTune-equivalent. If still hot, allocate buf on a thread-local heap. |
| Buffering itself adds overhead that exceeds SIMD wins | Medium-low | Phase 1a does buffering-only as a perf check; if regression > 2 ms, redesign buf access pattern. |
| MSVC's PGO retrains worse on the new code structure | Medium | Retrain after each phase. Train on **both** perft 5 and perft 6 (the latter is the more stable signal). |
| Interleaved slider scalar loops don't actually get ILP | Low | Check disassembly. If MSVC serializes, manually unroll. |
| 2-lane `__m128i` triggers MSVC to use XMM6-15 (nonvolatile), forcing save/restore around countMoves | Low (CPU has plenty of XMM regs) | Use only XMM0-5 explicitly via intrinsic pinning if needed. |

---

## 4. Detailed design — Phase B (4-way AVX2, stretch)

Only attempt if Phase A lands ≤ 75 ms and we want to push to ≤ 65.

### 4.1 Concept
Same architecture as Phase A but pairs become quads. `__m256i` holds 4 × 64-bit lanes.

### 4.2 The cliff problem
`/arch:AVX2` globally regresses 60 %. Isolated TU regresses 10 ms. The expected gross win from 4-way is ~16-25 ms. **Net = 6-15 ms.** Tight but possibly worth it.

### 4.3 Cliff mitigation
- Compile *only the new TU* (`leaf_batch_avx2.cpp`) with `/arch:AVX2`. Everything else stays at default arch.
- The TU exposes a single C entry point `extern "C" uint64 countMovesQuad(LeafBufEntry*, LeafBufEntry*, LeafBufEntry*, LeafBufEntry*)`.
- Cross-TU boundary happens at the QUAD level (~1 M calls in perft 5 if we process 4 at a time, vs 4 M one-at-a-time) — so YMM save/restore at function entry is amortized over 4 children.
- **Inside the AVX2 TU, do NOT use `__declspec(noinline)`.** Inline everything into countMovesQuad. The save/restore happens once at the QUAD function boundary, not per countMoves.

### 4.4 Gather for slider lookups
`_mm256_i64gather_epi64(base, indices, scale)` — 4 64-bit gathers in 1 instruction.

Per slider iter, all 4 children compute their idx in parallel via AVX2 IMUL... wait, AVX2 has no 64-bit IMUL on YMM. **VPMULLQ is AVX-512 only.** So magic factor × occupancy must stay scalar.

Workaround: use 4 scalar IMULs sequenced, then gather. Or split into 32-bit halves with VPMULDQ (but magic factors are full 64-bit).

Net: gather might still win because gather hides latency better than 4 sequential scalar loads.

### 4.5 Branch divergence with 4 lanes
With 4 lanes, the probability that **at least one** child triggers an unusual path (in-check, EP, castle) is higher than with 2 lanes. Need a "scalar-fallback for divergent lanes" strategy:
- Mask off divergent lanes
- Process them scalar after the SIMD path

Implementation complexity: significant.

### 4.6 Phase B estimated win
- Gross: ~16-25 ms
- Net after AVX2 cliff overhead: ~6-15 ms
- Combined with Phase A: ~17-30 ms total → **~55-68 ms perft 5**

This is the only realistic path to ≤ 65 ms in this codebase.

---

## 5. Phased implementation plan

### Phase 0 — Refactor countMoves to take pre-derived bitboards (1.5 h)
**Goal:** decouple DERIVE_PIECE_BITBOARDS from countMoves so the pair-wise code can do it in SIMD ahead of time.

- Add a new template `countMovesFromDerived<chance, hasEP, hasMyCastle>(QBB*, GS*, uint64 allPieces, uint64 myPieces, uint64 enemyPieces, ..., uint64 pinned, uint64 threatened)` that takes all derived state as parameters.
- Existing `countMoves<chance, hasEP, hasMyCastle>(QBB*, GS*)` becomes a 3-line wrapper: derive everything + call the new function.
- Bench: should be **neutral** — the wrapper inlines into the same code as before.

**Risk:** too many parameters trigger spill to stack. If so, group into a `PieceBoards` struct passed by pointer.

**Validation:** counts ✓, ST perft 5 median within ±1 ms of baseline.

### Phase 1 — Buffer + scalar flush (1 h)
**Goal:** validate buffering overhead is acceptable.

- Add `LeafBufEntry buf[256]` and `int bufN` to `FgmcCount2Processor`.
- emit() writes into `buf[bufN++]` instead of calling countMoves immediately.
- After enumerateMoves returns, scan `buf[0..bufN]` and call countMovesFromDerived (or just countMovesDispatch) on each, sequentially.

**Expected result:** ±2 ms vs baseline. The buffer adds memory writes/reads that didn't exist before, but reduces register pressure across the emit chain.

**Validation:** counts ✓, ST perft 5 median within ±2 ms.

**Decision point:** if Phase 1 regresses > 3 ms, the buffer-architecture is wrong — fall back, redesign with smaller buffer or different shape.

### Phase 2 — Pair-wise SSE2 flush (3 h)
**Goal:** primary win for Phase A.

- Write `countMovesPair<chance>(LeafBufEntry *a, LeafBufEntry *b)`.
- Inside: SIMD'd DERIVE_PIECE_BITBOARDS, non-slider attacks, popcount accumulation. Scalar interleaved slider loops. Scalar tail for castle/EP.
- Flush: process pairs, scalar tail for odd entry.

**Expected:** -8 to -15 ms on perft 5. Land at 70-77 ms.

**Validation:** counts ✓, ST perft 5 median improves by ≥ 5 ms or treat as fail.

### Phase 3 — PGO retrain + bench reflow (0.5 h)
- Delete `.pgd`, rebuild `/LTCG /GENPROFILE`, train on perft 5 + 6, merge, rebuild `/LTCG /USEPROFILE`.
- 15-run bench, median + min, confirm counts.

### Phase 4 — 4-way AVX2 (stretch, 4-6 h)
Only if Phase A landed clear win and we still want to push.

- New TU `leaf_batch_avx2.cpp`, `/arch:AVX2` only.
- Implement `countMovesQuad` with `__m256i` 4-lane operations.
- Scalar fallback for divergent lanes.
- Cross-TU boundary at the quad level.
- Phase 1's flush() chooses between `countMovesPair` (scalar quad-fallback path) and `countMovesQuad` based on a runtime arch-feature flag (or just hard-target Lion Cove).

**Expected:** additional -6 to -15 ms.

---

## 6. Validation plan

### 6.1 Correctness
After **every** phase:
- `perft_cpu <kiwipete-fen> 5 -nott` must print `Perft(05): 193690690`.
- `perft_cpu <kiwipete-fen> 6 -nott` must print `Perft(06): 8031647685`.
- (Optional, for confidence) `perft_cpu <startpos-fen> 6 -nott` must print `119060324`.

If counts mismatch, **bisect via depth**: which depth's count first diverged? Then bisect via the buf-flush index — does scalar countMovesDispatch on entry N produce the same result as countMovesPair on entries N/N+1?

### 6.2 Performance
After every phase:
- 15-run bench (`perft 5 -nott`), drop first, report **min and median**.
- Compare against the pre-phase median.
- If a phase regresses, revert and analyze before proceeding.

### 6.3 Per-phase decision gate
| Phase | Pass criterion | Fail action |
|---|---|---|
| 0 | Median within ±1 ms of baseline (84.5 / 85.7) | Reshape param passing (struct pointer); retry |
| 1 | Median within ±2 ms | Buffer architecture wrong; redesign or abandon |
| 2 | Median ≤ 77 ms | Don't proceed to AVX2; investigate why pair-wise didn't win |
| 4 | Median ≤ 70 ms | Same — investigate, possibly accept Phase A as result |

---

## 7. Test FENs and counts (reference)
| Position | FEN | Depth | Expected |
|---|---|---|---|
| Pos2 / kiwipete | `r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -` | 5 | 193,690,690 |
|  | (same) | 6 | 8,031,647,685 |
| Startpos | `rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1` | 6 | 119,060,324 |
| Pos3 (sliders only) | `8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -` | 7 | 178,633,661 |

Run a quick perft-6 startpos before / after Phase 2 just to confirm the optimization generalizes.

---

## 8. Build / bench commands

### Clean rebuild + bench
```powershell
cmake --build C:\personal\perft_cpu_2026\build --config Release --clean-first
$src = "C:\personal\perft_cpu_2026\build\Release\perft_cpu.exe"
$run = Join-Path $env:TEMP ("perft_" + [Guid]::NewGuid().ToString("N").Substring(0,8) + ".exe")
Copy-Item $src $run -Force
$fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -"
$times = @()
for ($i = 0; $i -lt 16; $i++) {
    $line = (& $run $fen 5 -nott | Select-String "Perft\(05\):").Line
    $t = [regex]::Match($line, "time:\s+([\d\.eE\+\-]+)").Groups[1].Value
    $times += [double]$t
}
Remove-Item $run -Force
$sorted = ($times | Select-Object -Skip 1 | Sort-Object)
"min=$([math]::Round($sorted[0]*1000,2)) ms  median=$([math]::Round($sorted[[math]::Floor($sorted.Count/2)]*1000,2)) ms"
```

### PGO retrain workflow
```powershell
$buildRel = "C:\personal\perft_cpu_2026\build\Release"
Remove-Item "$buildRel\*.pgc","$buildRel\*.pgd" -Force -ErrorAction SilentlyContinue
$cmake = "C:\personal\perft_cpu_2026\CMakeLists.txt"
(Get-Content $cmake -Raw) -replace "/LTCG /USEPROFILE","/LTCG /GENPROFILE" | Set-Content $cmake -NoNewline
cmake --build C:\personal\perft_cpu_2026\build --config Release --clean-first

# Train
$pgort = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\pgort140.dll"
$run = Join-Path $env:TEMP "perft_train.exe"
Copy-Item "$buildRel\perft_cpu.exe" $run -Force
Copy-Item $pgort (Split-Path $run) -Force
& $run $fen 6 -nott | Out-Null
& $run $fen 5 -nott | Out-Null   # train both depths
Get-ChildItem $env:TEMP -Filter "perft_cpu!*.pgc" | ForEach-Object { Copy-Item $_.FullName $buildRel -Force; Remove-Item $_.FullName -Force }
$pgomgr = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\pgomgr.exe"
& $pgomgr /merge "$buildRel\perft_cpu.pgd"

# Use profile
(Get-Content $cmake -Raw) -replace "/LTCG /GENPROFILE","/LTCG /USEPROFILE" | Set-Content $cmake -NoNewline
cmake --build C:\personal\perft_cpu_2026\build --config Release
Remove-Item $run -Force
```

### Profile run (Windows userland sampler — already in source)
```powershell
$run = Join-Path $env:TEMP "perft_prof.exe"
Copy-Item "$buildRel\perft_cpu.exe" $run -Force
Copy-Item "$buildRel\perft_cpu.pdb" (Split-Path $run) -Force
& $run $fen 6 -nott -profile
```

---

## 9. Files that will change

| File | Change |
|---|---|
| `MoveGeneratorBitboard.h` | Add `countMovesFromDerived` template. `FgmcCount2Processor` gains `buf[]` + `bufN`. emit() becomes buffer-write. Add `countMovesPair`. Add SSE2 helper functions (or new header `simd_bb2.h`). |
| `chess.h` | Possibly `LeafBufEntry` if shared, else keep in MoveGeneratorBitboard.h. |
| `CMakeLists.txt` | (Phase 4 only) Per-source `/arch:AVX2` on `leaf_batch_avx2.cpp`. Add the new TU to PERFT_SOURCES. |
| `leaf_batch_avx2.cpp` | (Phase 4 only) New file with `countMovesQuad`. |
| `optimization_log.md` | Append every experiment, kept or reverted. |

No changes to `perft.cpp`, `launcher.cpp` (the API stays the same — `countTwoLevelSubtree` still returns a uint64).

---

## 10. Open questions to resolve at session start

1. **Buffer size 256 vs 96 vs dynamic.** Decide after profiling buf-access patterns in Phase 1.
2. **`bb2_t` representation.** `__m128i` with `_mm_set_epi64x(b, a)` is standard but lane ordering is reverse-intuitive. Worth wrapping in a thin type to keep call sites readable.
3. **Should `countMovesPair` handle the in-check (rare) path SIMD'd or fall back to scalar?** Start with scalar fallback; revisit if profile shows it's hot.
4. **Phase 4 only?** If Phase A delivers e.g. 76 ms, accepting it as the result is reasonable. Phase 4's AVX2 risk is real.
5. **Cross-validate on pos3?** Sliders-only position, very different distribution. Useful sanity check that we didn't overfit pos2.

---

## 11. What success looks like

| Outcome | Phase | Verdict |
|---|---|---|
| ≤ 65 ms median | A + B both win | **Hit target.** |
| 66-72 ms median | A wins, B marginal/skipped | **Substantial improvement, accept.** |
| 73-78 ms median | A wins, no B | **Real win but short.** Document and decide. |
| 79-83 ms median | A marginal | **Below noise floor.** Revert, write retrospective. |
| ≥ 84 ms or counts wrong | Failed | **Revert everything,** keep PGO. |

---

## 12. Pre-session checklist

- [ ] Read `optimization_log.md` end-to-end (especially "Dangerous mines" + the 4 tick retrospectives).
- [ ] Verify baseline: build clean, retrain PGO, 15-run bench → median should be ~85 ms ± 1.
- [ ] Confirm counts at baseline (perft 5 = 193,690,690, perft 6 = 8,031,647,685).
- [ ] Decide on buffer size (3.2.1) based on a quick `max_legal_moves` scan.
- [ ] Open `MoveGeneratorBitboard.h` line 1576 (countMoves) and line 1331 (FgmcCount2Processor) as the two primary edit sites.

When all checks pass, begin Phase 0.

---

*Plan written end of sprint tick #4. Standing baseline: 84.5 ms min / 85.7 ms median. Counts ✓.*
