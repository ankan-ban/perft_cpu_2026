# Plan A — AVX2 Bulk-SIMD Leaf (4-wide structure-of-arrays)

> **2026-05-15 — EXECUTED, partially successful.** Final result: 74.55 ms p5 / 3.587 s p6
> (**-4.8 % / -5.4 %**), short of the ≤60 ms target. The core SoA 4-lane SIMD derive
> lever was **structurally blocked** by cross-TU marshaling cost (the AVX2 TU cannot
> instantiate `MoveGeneratorBitboard.h` templates without triggering the global-AVX2
> cliff, so derived state must marshal across the boundary, which exceeds the SIMD
> savings). See "Plan A retrospective" in `CLAUDE.md` and "Plan A — Phase 4" in
> `optimization_log.md` for the post-mortem. Next session: see
> `plan_make_unmake_incremental.md` (Plan B).

**Target:** push `kiwipete perft 5 -nott` single-thread on AMD Ryzen 9 9950X3D
from **~75.3 ms** (post-E7 state) → **≤ 60 ms** (-20 %). Stretch: ≤ 55 ms.

**Why this plan is the most aggressive of the three:** all scalar levers are
exhausted. We've spent a full session (E1..E12) confirming MSVC's PGO scalar
codegen for the leaf is near the throughput ceiling on Zen 5. The remaining
headroom is in *parallelism across siblings*, not within a single leaf.

---

## 1. Context (read this first)

### 1.1 Standing measurement entering this plan
- `kiwipete perft 5 -nott` ST: **75.20 ms min / 75.30 ms median** (E7 final, PGO-trained on perft 5+6).
- `bench-leaf` (countMovesDispatch on input FEN, 100M iters): 14.33 ns / call.
- `bench-magic` (16 lookups × 200M iters): 0.40 ns / lookup.
- Profiler attribution (perft 6 -nott, 43K samples):
  - `countMovesPairCached<1,0>` (slider fast path): **38.4 %**, dominated by
    `findSliderAttacksOnly` (~9 %) + `countMovesFromDerived` (~17 %).
  - `countMovesPair<1>` (slow path, PNK moves): **37.6 %**, similar shape.
  - `enumerateMoves` + `emit` chain: ~17 %.
- ~76 % of perft-5 time is in the depth-1 leaf path; the remaining 24 % is
  enumeration + makeMoveT + recursion bookkeeping.

### 1.2 The mines (do NOT re-step on without new evidence)
| Mine | Empirical evidence |
|---|---|
| `/arch:AVX2` globally | **-57 % ST** (sprint E3). YMM6-15 save/restore around the noinline `countMoves` boundary fires on every leaf call. |
| `/arch:AVX2` per isolated TU | **+10 ms boundary overhead** per cross-TU call on Lion Cove. Zen 5 not measured but expected similar; gradient is set by MSVC ABI not μarch. |
| Explicit SSE2 `__m128i` for paired A/B derives (E12) | **-2 ms regression** vs scalar. Pack/extract overhead beats 4-port scalar AND throughput on this size. |
| `__declspec(noinline)` removal on `countMovesPair` (E3) | Stack overflow. Inlined chain in the 5-deep recursion blows the default stack. |
| `alignas(32)` on QBB | -9.5 % (CLAUDE.md). Stack-frame growth in deeply-nested FGMC. |

### 1.3 Why "bulk SIMD with proper amortization" is the path

The AVX2 cliff is real but the *unit cost is fixed per TU boundary crossing*.
If we cross the boundary once per pair (~2 M crossings at perft 5), the cost
is intolerable (~10 ms × millions = catastrophic). If we cross once per
**depth-2 parent** (~98 K at perft 5), the cost is 98 K × ~10 cycles ≈
0.3 ms — a rounding error.

The wins come from:
- **4-lane bitboard derives** (8 ALU per child × 4 children = 32 scalar ops →
  ~8 AVX2 ops with full output extraction).
- **4-lane non-slider attacks** (pawn shifts, knight bulk, king attacks)
  using `_mm256_*` ops on `__m256i` × 4 lanes.
- **Scalar slider magic** stays scalar (no PVMULLQ on AVX2), but the 4
  children's slider iterations interleave naturally and OoO keeps multiple
  LUT loads in flight (3 load ports on Zen 5).
- **Bulk popcount accumulation** with `_mm256_or_si256` reductions.

Theoretical model:

| Phase | Scalar cost (1 leaf) | 4-lane SIMD cost (4 leaves) | Per-leaf saved |
|---|---|---|---|
| DERIVE_PIECE_BITBOARDS | ~3 ns | ~4 ns | ~2 ns |
| Pawn attacks (when needed) | ~1 ns | ~1.5 ns | ~0.6 ns |
| Knight attacks | ~1.5 ns | ~2 ns | ~1 ns |
| King attacks | ~1 ns | ~1.5 ns | ~0.6 ns |
| Slider magic lookups | ~5 ns | ~5 ns (interleaved scalar, no SIMD) | ~0 |
| Per-leaf countMovesFromDerived body | ~5 ns | ~6 ns (split into bulk + scalar) | ~1 ns |
| **Total per leaf** | ~16.5 ns | ~5 ns / leaf within batch | **~5-6 ns/leaf gross** |

At 4 M leaves with ~50 % batched (slider moves where E7 already cached the
non-slider work): 4 M × 0.5 × 5 ns = **10 ms gross**. Minus the AVX2 boundary
amortization (~0.3 ms) and minus the bulk-LUT overhead (~1-2 ms for transpose
between scalar and SIMD halves) = **6-8 ms net**.

That's the **floor** estimate. With aggressive design (bulk pinned detection,
bulk king-move counting, fused threat+pin computation), the upside is
**10-15 ms** which would land us at **60-65 ms** for perft 5.

### 1.4 Hardware facts that drive the design

| Spec | Zen 5 (9950X3D) | Implication |
|---|---|---|
| L1d | 48 KB | Buffer for 4-lane SoA must fit ~10-20 KB |
| L1i | 32 KB | AVX2 TU code size budget; specializations multiply. Aim ≤ 16 KB for the AVX2 TU |
| AVX2 256-bit ops | Yes | 4-lane `__m256i` ops are native |
| AVX-512 VPMULLQ | No | 64-bit IMUL on YMM unavailable → magic stays scalar |
| AVX2 `_mm256_i64gather_epi64` | Yes; latency ~10c, throughput 1/4c | Worth trying for slider LUT loads if pipelining works |
| BMI2 PEXT | Yes, 1 cycle latency 1/c throughput | Could substitute for IMUL magic in scalar slider loop; needs `/arch:AVX2` |
| 3 load ports, 4 ALU ports | | Scalar can issue 3 LUT loads/cycle; SIMD has 2 256-bit vec ports |
| MSVC ABI YMM6-15 preserved | | This is the source of the cliff — caller in non-AVX2 code does NOT save these on call into AVX2 TU |

### 1.5 Why the cliff exists (mechanism)

When MSVC compiles a TU with `/arch:AVX2`, it considers YMM6-15 to be
nonvolatile (caller-preserved across function calls) in that TU. When such
code calls *out* to a non-AVX2 TU, MSVC inserts code to save/restore
YMM6-15 in case the callee clobbers them. Symmetrically, calling INTO an
AVX2 TU from a non-AVX2 TU goes through a prologue that may zero or save
YMM state because the caller didn't agree to preserve them.

The cost: typically 8 × 32-byte saves on prologue + 8 × 32-byte restores
on epilogue = ~10-15 cycles. Per cross-TU call. Amortizes over the work
done inside that call.

**Key design constraint:** the AVX2 TU must consume *batches* of work, not
single leaves. One call per parent (~50 leaves) ÷ ~10 cycles boundary = 0.2
cycles/leaf overhead. Acceptable.

---

## 2. Architecture

```
┌──────────────────────────────────────────────────────┐
│  countTwoLevelSubtree<chance> (non-AVX2 TU)          │
│  ├─ FgmcCount2Processor<chance> proc(pos, gs)         │
│  ├─ enumerateMoves<chance>(pos, gs, proc)             │
│  │    └─ emit<piece>(from, to, flags)                 │
│  │       └─ writes to bufCp_slider / bufCp_slow      │
│  └─ proc.flush() ──────────────────────┐              │
└─────────────────────────────────────────┼──────────────┘
                                          │ cross-TU call,
                                          │ pays YMM save/restore ONCE per parent
                                          ▼
┌──────────────────────────────────────────────────────┐
│  countMovesBulkAVX2<chance>(LeafBatchSoA*) (AVX2 TU) │
│  ├─ For 4-leaf chunks:                                │
│  │    transpose 4 QBBs → SoA (4×4 uint64)             │
│  │    4-lane SIMD derive                              │
│  │    4-lane SIMD non-slider attacks (if not cached)  │
│  │    Per-lane scalar slider attacks (interleaved)    │
│  │    Per-lane scalar findPinnedPieces                │
│  │    Per-lane bulk countMovesFromDerived             │
│  │    Sum per-lane nMoves into accumulator            │
│  └─ Returns total uint64 sum                          │
└──────────────────────────────────────────────────────┘
```

The non-AVX2 TU handles enumeration, buffer building, and flush dispatch.
The AVX2 TU is a pure consumer that batch-processes 4 leaves and returns the
sum. The boundary cost is 1 call per parent × ~10 cycles ≈ 1 ns / parent.

### 2.1 Data layout: SoA `LeafBatch`

```cpp
// Holds up to BATCH_CAP children of one parent, structure-of-arrays.
// Each "lane" array is 64-byte aligned so 4 lanes fit one __m256i load.
struct alignas(64) LeafBatch {
    static constexpr int BATCH_CAP = 128;  // tuned per kiwipete max ~80

    // AoS half — kept for scalar slider loops and for makeMoveT writes.
    alignas(64) QuadBitBoard cp [BATCH_CAP];  // 4 KB at cap 128
    GameState                gs [BATCH_CAP];  // 128 B

    // SoA half — transposed lazily before bulk SIMD work.
    // 4 lanes per __m256i. Each "plane" holds the same bitboard for 4 leaves.
    // The SoA is built from cp[] right before each 4-leaf SIMD pass.
    // Allocated as `alignas(32) uint64[4]` views; not a separate buffer.

    int n;                         // child count
    uint8 moveType[BATCH_CAP];     // SLIDER / PAWN / KNIGHT / KING / SPECIAL
};
```

**Stack budget per parent:** 4 KB cp + 128 B gs + 128 B moveType + overhead
≈ 4.5 KB. Two simultaneous batches (slider + slow) ≈ 9 KB. Comfortable.

Why keep AoS cp[] *and* transpose to SoA at SIMD time? Because makeMoveT in
`emit()` writes per-child (cp[i] is a single QBB). Transposing in the AVX2
TU is one `vpunpcklqdq`/`vpunpckhqdq` quartet, ~4 cycles for 4 children.

### 2.2 Cross-TU interface

```cpp
// Defined in leaf_batch_avx2.cpp (compiled with /arch:AVX2).
extern "C" {
    uint64 countMovesBulkAVX2_white(const LeafBatch *batch, uint64 cachedNonSliderAtk);
    uint64 countMovesBulkAVX2_black(const LeafBatch *batch, uint64 cachedNonSliderAtk);
}
```

Two entry points (one per `chance`) to keep the AVX2 TU code compact —
the inner SIMD doesn't need to runtime-switch on chance.

C linkage avoids MSVC name mangling complications across TU boundaries
and lets us link with simple `extern "C"`.

### 2.3 Inside the AVX2 TU

```
countMovesBulkAVX2<chance>(batch):
    uint64 sum = 0
    int i = 0
    while (i + 4 <= n):
        # 4-lane bulk path
        sum += process4<chance>(&batch->cp[i], &batch->gs[i], cachedNonSliderAtk)
        i += 4
    # Tail: process 1-3 leaves scalar (in the same AVX2 TU but tail loop)
    while (i < n):
        sum += processScalarInAvx2TU<chance>(&batch->cp[i], &batch->gs[i], cachedNonSliderAtk)
        i += 1
    return sum
```

The scalar tail inside the AVX2 TU is unavoidable; with BATCH_CAP=128 and
kiwipete typical ~25 sliders per parent, the tail is 1-3 leaves per parent.
Not on the critical path.

### 2.4 `process4<chance>` — the 4-lane SIMD leaf body

This is the heart of the plan. Pseudo-C:

```cpp
template <uint8 chance>
__forceinline uint64 process4(const QuadBitBoard *cp4, const GameState *gs4,
                               uint64 cachedNonSliderAtk)
{
    // 1. Transpose 4 QBBs (4×4 uint64) into SoA __m256i planes.
    __m256i bb0 = load_lane4(&cp4[0].bb[0], &cp4[1].bb[0], &cp4[2].bb[0], &cp4[3].bb[0]);
    __m256i bb1 = load_lane4(&cp4[0].bb[1], &cp4[1].bb[1], &cp4[2].bb[1], &cp4[3].bb[1]);
    __m256i bb2 = load_lane4(&cp4[0].bb[2], &cp4[1].bb[2], &cp4[2].bb[2], &cp4[3].bb[2]);
    __m256i bb3 = load_lane4(&cp4[0].bb[3], &cp4[1].bb[3], &cp4[2].bb[3], &cp4[3].bb[3]);

    // 2. 4-lane derive (8 SIMD ops instead of 32 scalar).
    __m256i allPieces    = _mm256_or_si256(_mm256_or_si256(bb1, bb2), bb3);
    __m256i blackPieces  = bb0;
    __m256i whitePieces  = _mm256_andnot_si256(blackPieces, allPieces);
    __m256i allPawns     = _mm256_andnot_si256(bb3, _mm256_andnot_si256(bb2, bb1));
    __m256i knights      = _mm256_andnot_si256(bb3, _mm256_andnot_si256(bb1, bb2));
    __m256i bishopQueens = _mm256_and_si256(bb1, _mm256_xor_si256(bb2, bb3));
    __m256i rookQueens   = _mm256_andnot_si256(bb2, bb3);
    __m256i kings        = _mm256_andnot_si256(bb1, _mm256_and_si256(bb2, bb3));

    __m256i myPieces    = (chance == WHITE) ? whitePieces : blackPieces;
    __m256i enemyPieces = (chance == WHITE) ? blackPieces : whitePieces;
    __m256i myKing      = _mm256_and_si256(kings, myPieces);
    // ... etc

    // 3. Extract scalars for the scalar slider work + bitscans.
    alignas(32) uint64 myKing_s[4], allPieces_s[4], myPieces_s[4],
                       enemyPieces_s[4], bishopQueens_s[4], rookQueens_s[4],
                       allPawns_s[4], knights_s[4];
    _mm256_store_si256((__m256i*)myKing_s,       myKing);
    _mm256_store_si256((__m256i*)allPieces_s,    allPieces);
    _mm256_store_si256((__m256i*)myPieces_s,     myPieces);
    _mm256_store_si256((__m256i*)enemyPieces_s,  enemyPieces);
    _mm256_store_si256((__m256i*)bishopQueens_s, bishopQueens);
    _mm256_store_si256((__m256i*)rookQueens_s,   rookQueens);
    _mm256_store_si256((__m256i*)allPawns_s,     allPawns);
    _mm256_store_si256((__m256i*)knights_s,      knights);

    uint8 kingIdx_s[4] = { (uint8)bitScan(myKing_s[0]), ... };

    // 4. Per-lane scalar slider attacks, interleaved.
    uint64 threat_s[4];
    for (int lane = 0; lane < 4; ++lane) {
        uint64 enemyBishops = bishopQueens_s[lane] & enemyPieces_s[lane];
        uint64 enemyRooks   = rookQueens_s[lane]   & enemyPieces_s[lane];
        threat_s[lane] = cachedNonSliderAtk
                       | findSliderAttacksOnly(~allPieces_s[lane],
                                               enemyBishops, enemyRooks,
                                               myKing_s[lane]);
    }

    // 5. Per-lane scalar findPinnedPieces.
    uint64 pinned_s[4];
    for (int lane = 0; lane < 4; ++lane) {
        uint64 enemyBishops = bishopQueens_s[lane] & enemyPieces_s[lane];
        uint64 enemyRooks   = rookQueens_s[lane]   & enemyPieces_s[lane];
        pinned_s[lane] = findPinnedPieces(myKing_s[lane], myPieces_s[lane],
                                          enemyBishops, enemyRooks,
                                          allPieces_s[lane], kingIdx_s[lane]);
    }

    // 6. Per-lane scalar countMovesFromDerived dispatch (existing template).
    uint64 sum = 0;
    for (int lane = 0; lane < 4; ++lane) {
        const QuadBitBoard *pos = &cp4[lane];
        const GameState    *gs  = &gs4[lane];
        if (threat_s[lane] & myKing_s[lane]) [[unlikely]] {
            sum += countMovesOutOfCheck<chance>(...);
        } else {
            // dispatch on hasEP/hasCastle
            sum += countMovesFromDerivedTemplated(...);
        }
    }
    return sum;
}
```

The 4-lane SIMD wins in steps 1-2-3 (transpose + derive + extract). Steps
4-6 are scalar but interleaved across 4 lanes, giving OoO the same
parallelism advantage that paired countMovesPair gets from 2 lanes.

### 2.5 What about bulk pinned + bulk countMovesFromDerived?

Stretch goals if Phase D delivers ≥ 5 ms:
- **Bulk pinned detection:** the slider iteration in findPinnedPieces is
  per-king. Can interleave 4 lanes' slider iterations using AVX2 `vpcmpeqq`
  for the singular-blocker test. ~2-3 ms additional.
- **Bulk king move counting:** kingAttacks(kingIdx) is an LUT load; 4 lanes
  ` = `_mm256_i64gather_epi64`. Worth trying — gather throughput is 1 per 4
  cycles, scalar would be 4 sequential loads. ~1 ms additional.

These are *optional* and should only be tackled after the base 4-lane derive
+ extract is proven a net win.

---

## 3. Phased implementation

Each phase has an explicit **decision gate**. If a phase fails its gate,
revert that phase and either revise or abandon the plan.

### Phase 0 — AVX2 cliff measurement (1 day)

**Goal:** measure the per-call boundary cost on Zen 5 with the CURRENT
build setup. Lion Cove showed +10 ms; Zen 5 not measured.

**Steps:**
- New TU `cliff_probe.cpp` with `/arch:AVX2`, exposing a single
  `extern "C" uint64 cliff_probe(uint64 *) noexcept` that does one ALU op
  on the pointer (no YMM use).
- In `perft.cpp` add `-bench-cliff N` that loops calling `cliff_probe()`
  N times, measures ns/call.
- Compare to baseline scalar function call cost (~1-2 cycles direct call).
- Repeat with the probe using 4× YMM ops internally, to bracket the cost
  range.

**Decision gate:**
| Cliff cost per call | Verdict |
|---|---|
| < 5 cycles | Plan is straightforward — can call per-pair if desired |
| 5-30 cycles | Plan as designed — call per-parent |
| > 30 cycles | Plan needs deeper amortization — call per depth-3 parent (~2K times) |
| > 200 cycles | Plan infeasible — abandon |

**Files added:** `cliff_probe.cpp`, modifications to `perft.cpp`,
`CMakeLists.txt` (per-source `/arch:AVX2`).

### Phase 1 — TU scaffolding (1 day)

**Goal:** establish the build infrastructure for an AVX2-isolated TU
without changing perft logic.

**Steps:**
- New file `leaf_batch_avx2.cpp` with `/arch:AVX2`, no `/GL` (LTCG-cross-TU
  with mixed `/arch` is problematic; verify in Phase 0 if `/GL` for both
  TUs works; otherwise drop `/GL` for the AVX2 TU only).
- Empty stub `extern "C" uint64 countMovesBulkAVX2_white(...)` that just
  iterates the AoS leaves with the existing `countMovesPairCached` —
  a behavioral no-op vs current code.
- Modify `FgmcCount2Processor::flush()` to call the stub for the slider
  sub-buffer instead of inline `countMovesPairCached`.

**Decision gate:** counts ✓, ST perft 5 within ±2 ms of E7 baseline
(75.3 ms). If regression > 5 ms, the cliff per-pair-call cost is real
and we MUST call once per parent, not once per pair. Restructure to
collect a full parent's slider batch into one cross-TU call.

### Phase 2 — SoA transpose + 4-lane derive (2 days)

**Goal:** prove the SIMD derive is faster than scalar inside the AVX2 TU.

**Steps:**
- Implement `process4<chance>` with steps 1-3 only (transpose + 4-lane
  derive + extract).
- Steps 4-6 fall back to the existing `findSliderAttacksOnly` +
  `findPinnedPieces` + `countMovesFromDerived` per lane scalar.
- Stub the 4-leaf loop with the 1-3 tail going to scalar fallback.

**Decision gate:** counts ✓, ST perft 5 ≤ 73 ms (-3 % gain target).
If neutral, the SIMD derive isn't winning over scalar — possibly Zen 5
schedules scalar derives across 4 ports better than AVX2 over 2 vec
ports. If so, abandon Phase 2 and try Phase 3 directly with scalar
derive.

### Phase 3 — Interleaved scalar slider with 4-way OoO (2 days)

**Goal:** the per-lane scalar slider work (step 4) should give OoO a
4× pipeline. Hand-write the loop to interleave 4 lanes' iterations.

**Steps:**
- Replace step 4 in `process4` with a manually-unrolled, 4-lane-interleaved
  slider iteration:
  ```cpp
  uint64 sA = enemyBishops_s[0], sB = enemyBishops_s[1],
         sC = enemyBishops_s[2], sD = enemyBishops_s[3];
  uint64 atA=0, atB=0, atC=0, atD=0;
  while (sA | sB | sC | sD) {
      if (sA) { uint8 sq = bitScan(sA); uint64 occ = sliderOcc_s[0] & sqBishopAttacksMasked(sq);
                uint64 idx = bishop_magic_factors[sq] * occ >> ...; atA |= bishop_magic_tables[sq][idx]; sA &= sA-1; }
      if (sB) { ... }
      if (sC) { ... }
      if (sD) { ... }
  }
  // repeat for rooks
  ```
- The `if (sX)` branches are well-predicted in the steady state (all 4
  have sliders); the only mispredict is at tail.
- Per-iter: 4 independent LUT loads can issue across Zen 5's 3 load ports
  (pipelined over cycles).

**Decision gate:** counts ✓, ST perft 5 improves by ≥ 2 ms vs Phase 2.
If neutral or regressed, the `if (sX)` branches mispredict more than
expected at tail; revert to sequential per-lane scalar slider loops
inside the AVX2 TU (still benefits from amortized TU boundary).

### Phase 4 — 4-lane non-slider attack SIMD (2 days)

**Goal:** for the SLOW sub-buffer (PNK moves where cachedNonSliderAtk is
NOT pre-computed), use 4-lane SIMD for pawn + knight + king attacks.

This requires a SECOND AVX2 entry point for slow-path batches OR a runtime
flag inside `process4`. Probably cleaner to have:
```cpp
extern "C" uint64 countMovesBulkAVX2_white_slow(const LeafBatch*);
extern "C" uint64 countMovesBulkAVX2_black_slow(const LeafBatch*);
```

The slow variant computes `nonSliderAtk` per lane inside the AVX2 TU
using SIMD shifts (pawn) + bulk knight + bulk king.

**Decision gate:** counts ✓, ST perft 5 improves by ≥ 1.5 ms.

### Phase 5 — Bulk king-move counting via gather (stretch, 2-3 days)

**Goal:** test if `_mm256_i64gather_epi64` for the per-lane
`sqKingAttacks[kingIdx]` LUT load wins.

**Steps:**
- In `process4`, instead of 4 sequential `sqKingAttacks[]` loads, use
  ```cpp
  __m256i indices = _mm256_load_si256((__m256i*)kingIdx_extended);  // 4 × 64-bit indices
  __m256i kingAtks = _mm256_i64gather_epi64((const long long*)KingAttacks, indices, 8);
  ```
- Same idea for `sqsInBetween`, `sqsInLine` LUTs.

**Decision gate:** counts ✓, ST perft 5 improves by ≥ 0.5 ms. Gather
on Zen 5 has reputation for being unpredictable; if neutral, drop.

### Phase 6 — Bulk pin detection (stretch, 3 days)

**Goal:** parallelize `findPinnedPieces` across 4 lanes.

The function structure:
```
For each enemy slider aiming at king's diag/orth line:
   blockers = sqsInBetween(slider, king) & allPieces
   if isSingular(blockers): pinned |= blockers
```

Hard to vectorize because:
- The set of "aimed sliders" varies per lane.
- The blockers-singular test (`x && !(x & (x-1))`) maps poorly to SIMD.

Approach: skip SIMD here. Keep scalar but interleave like Phase 3's
slider loop. ~1 ms incremental.

**Decision gate:** counts ✓, ST perft 5 improves by ≥ 0.5 ms.

### Phase 7 — PGO retraining + final tuning (1 day)

The new TU layout will invalidate the PGO `.pgd`. Retrain on perft 5+6+startpos
(broader training set helps because the AVX2 TU has different hotspots).

**Decision gate:** final ST perft 5 ≤ 60 ms (-20 % vs E7).

---

## 4. Validation strategy

### 4.1 Correctness — at EVERY phase
| Test | Expected |
|---|---|
| `kiwipete -nott 5` | 193,690,690 |
| `kiwipete -nott 6` | 8,031,647,685 |
| `startpos -nott 6` | 119,060,324 |
| Position 3 (sliders-only) `8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -` depth 7 | 178,633,661 |
| Position 4 (promotions-heavy) `r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1` depth 5 | 15,833,292 |
| Position 5 (talkchess "Steven Edwards") `rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8` depth 5 | 89,941,194 |

**Required:** all 5 positions × correct counts before any perf measurement.

### 4.2 Performance bench
- 15-run perft 5, drop first 3, report min/median. Use `bench_p5.ps1`.
- 4-run perft 6, drop first, report min/median (use sparingly due to ~3.9 s
  per run; perft 5 is the primary signal).
- For PGO retrain: the GENPROFILE/USEPROFILE cycle exists in `bench_pgo.ps1`.

### 4.3 Per-phase pass criteria
| Phase | Pass criterion |
|---|---|
| 0 | Cliff cost measured and documented |
| 1 | Counts ✓, ≤ 77 ms (no regression from TU split) |
| 2 | Counts ✓, ≤ 73 ms |
| 3 | Counts ✓, ≤ 71 ms |
| 4 | Counts ✓, ≤ 69 ms |
| 5 | Counts ✓, ≤ 68 ms |
| 6 | Counts ✓, ≤ 67 ms |
| 7 | Counts ✓, ≤ 60 ms |

### 4.4 Bisect plan if counts break
If counts diverge at any phase:
1. Run all 5 test positions. Note which fail.
2. If only one fails, reduce-position to a smaller perft depth that still
   diverges. Find smallest depth where any position diverges.
3. Compare per-leaf output of bulk path vs scalar path on a specific
   parent position (use a printf in the bulk path).
4. Verify SoA transpose by printing `cp[0..3]` BEFORE and AFTER transpose
   (should round-trip identically).

---

## 5. Risks and mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| AVX2 cliff > expected | Medium | Critical | Phase 0 measures it; if catastrophic, restructure to call per depth-3 parent or per depth-4 parent for deeper amortization |
| MSVC LTCG breaks across mixed `/arch` TUs | Medium | High | Drop `/GL` for the AVX2 TU only; verify in Phase 1 |
| SoA transpose overhead exceeds derive savings | Low | Medium | Phase 2 gate catches; if neutral, drop transpose and feed scalar derives 4-at-a-time directly to the interleaved scalar slider loop |
| Interleaved scalar slider mispredicts | Medium | Medium | Phase 3 gate catches; fall back to sequential scalar |
| Stack frame bloat in AVX2 TU triggers Windows stack overflow | Low | High | Keep `process4` non-inline; use `_alloca` only if needed; monitor via `/Bv` MSVC switch |
| PGO retraining produces worse layout | Medium | Medium | Train on multiple positions; verify with at least 2 startpos depths and 2 kiwipete depths |
| Cross-TU calling convention quirks (varargs, register clobbers) | Low | High | Use `extern "C"` with fixed signatures; no varargs; no vec args (pass `LeafBatch*` not `__m256i`) |
| Code size in AVX2 TU exceeds L1i | Medium | Medium | Single `process4<WHITE>` + single `process4<BLACK>` specializations; do NOT explode countMovesFromDerived's hasEP/hasCastle inside the TU (call out to existing scalar specializations) |

---

## 6. Files that will change

| File | Change |
|---|---|
| `CMakeLists.txt` | Per-source `/arch:AVX2` on `leaf_batch_avx2.cpp` and `cliff_probe.cpp`. Conditionally disable `/GL` for those TUs if LTCG cross-TU breaks. |
| `cliff_probe.cpp` (new) | Phase 0 only; can delete after measurement done. |
| `leaf_batch_avx2.cpp` (new) | All `process4`, `countMovesBulkAVX2_{white,black}{,_slow}` entry points. |
| `MoveGeneratorBitboard.h` | `FgmcCount2Processor::flush()` switches to call into AVX2 TU for slider sub-buffer (Phase 1+) and slow sub-buffer (Phase 4+). Add `LeafBatch` struct. |
| `perft.cpp` | `-bench-cliff N` and any other new microbenches. |
| `chess.h` | Possibly add `LeafBatch` here if shared between TUs (likely yes since AVX2 TU needs to know its layout). |
| `optimization_log.md` | Append every experiment with attached number. |

---

## 7. Open questions to resolve at session start

1. **Cliff cost on Zen 5.** Phase 0 deliverable. Drives BATCH_CAP and where
   in the recursion to fan out.
2. **Can `/GL` + `/LTCG` coexist with mixed `/arch:AVX2` TUs?** MSVC 19.44
   docs are ambiguous. Test in Phase 1.
3. **Should the slow path (PNK moves) also go through the AVX2 TU?**
   Probably yes (Phase 4), but only if it doesn't blow the AVX2 TU's L1i
   budget.
4. **PEXT magic in the AVX2 TU.** Since we're paying the cliff anyway,
   PEXT is "free" inside the TU. Worth trying as a Phase 3 micro-experiment:
   `_pext_u64(occ, mask)` vs `factor * occ >> shift`. On Zen 5 PEXT is 1
   cycle / 1 per cycle. IMUL is 3 cycles / 1 per cycle. Same throughput
   but lower latency → more pipelined.
5. **Are there positions where `BATCH_CAP=128` overflows?** Test on
   talkchess "max moves" positions (`r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/...`
   has 218 moves at one ply). Bump to 256 if needed; cost is 4 KB stack.

---

## 8. What success looks like

| Final perft 5 | Verdict |
|---|---|
| ≤ 55 ms | **Stretch hit.** Plan exceeded expectations. |
| 55-60 ms | **Target hit.** Plan worked end-to-end. |
| 60-65 ms | **Substantial.** Phase 5/6 stretch didn't materialize but the core SIMD path works. |
| 65-70 ms | **Real but partial.** AVX2 cliff was higher than estimated OR scalar Zen 5 codegen was harder to beat than expected. Document and accept. |
| 70-75 ms | **Below target.** Suggests SIMD on this leaf doesn't extract enough parallelism over Zen 5's wide scalar. Pivot to Plan B (make/unmake) or Plan C (incremental attacks). |
| > 75 ms or counts wrong | **Failed.** Revert everything, retrospective. |

---

## 9. Estimated effort

| Phase | Days | Cumulative |
|---|---|---|
| 0 (cliff measure) | 1 | 1 |
| 1 (TU scaffold) | 1 | 2 |
| 2 (4-lane derive) | 2 | 4 |
| 3 (interleaved sliders) | 2 | 6 |
| 4 (4-lane non-slider attacks) | 2 | 8 |
| 5 (gather king LUT) | 2-3 | 10-11 |
| 6 (bulk pin) | 3 | 13-14 |
| 7 (PGO retrain + final tuning) | 1 | 14-15 |

**Total: ~3 work-weeks for an experienced systems engineer**, more if
unfamiliar with MSVC's AVX2 quirks. The first 6 days (through Phase 2)
are the most important — if Phase 2's decision gate fails, the rest is
moot. Treat days 1-6 as a feasibility spike, days 7-15 as conditional
follow-on.

---

## 10. Pre-session checklist

- [ ] Read `optimization_log.md` end-to-end, especially the AVX2 sprint
      mines (E3, sprint E15-E34).
- [ ] Verify E7 baseline: 75.20 ms min / 75.30 ms median over 15 runs.
- [ ] Confirm counts at baseline for all 5 test positions.
- [ ] Have `MoveGeneratorBitboard.h` open at line 1485 (countTwoLevelSubtree)
      and line 1983 (countMovesPairCached) as the primary edit sites.
- [ ] CMake configure verified for adding per-source compile flags.

When all check, begin Phase 0.

---

*Plan written 2026-05-15 after a session of 12 scalar experiments (E1-E12)
yielding the E7 winning idea (-3.4 % on perft 5). The next gain on this
hardware requires a different regime: bulk SIMD with batch-level
amortization. This plan is the most aggressive of the three follow-on
options, with the highest engineering cost and the highest upside.*
