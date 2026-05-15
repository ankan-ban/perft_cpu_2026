# Plan C — Incremental Attack & Threat Maps

> **2026-05-15.** Standing entry is now **74.55 ms p5 / 3.587 s p6**
> (post-Plan-A). Plan C still depends on Plan B substrate
> (`plan_make_unmake_incremental.md`); do Plan B first. Plan A's
> `enemyNonSliderAtk4_impl` SIMD helper in `leaf_batch_avx2.cpp` can
> coexist with this plan — it provides a fast slow-path before incremental
> attack maintenance kicks in.

**Target:** push `kiwipete perft 5 -nott` single-thread on AMD Ryzen 9 9950X3D
from **~75.3 ms** (post-E7 state) → **≤ 60 ms** (-20 %). Stretch: ≤ 50 ms
when combined with Plan B (make/unmake substrate).

**Conceptual goal:** eliminate `findAttackedSquares` from the leaf entirely.
The "squares attacked by enemy" bitboard is maintained as state across the
recursion, updated incrementally per move. The leaf reads it; never
computes it from scratch.

---

## 1. Context

### 1.1 Why this approach

`findAttackedSquares` is the **single biggest hot function** in the leaf,
per profiler:
- ~5-6 ns per call out of ~14 ns total leaf time = ~40 % of leaf work
- ~50 % of leaves go through E7's cached fast path (slider moves) where
  the non-slider portion is precomputed
- The slider portion is still ~3-4 ns per leaf (6 magic LUT loads on
  average)

If we maintain the full `threatened` bitboard incrementally, the leaf
becomes:
```
threatened = state.enemyAttacks;        // 1 load
if (threatened & myKing) [[unlikely]] { in-check path }
else { count moves; king moves use ~threatened mask }
```

**Eliminating findAttackedSquares: ~5-6 ms savings** at perft 5. If we
also eliminate `findPinnedPieces` (which is conceptually similar work)
we add another ~1-2 ms. **Aggregate target: 8-10 ms = ~10-13 %.**

### 1.2 Why this approach is the most complex

Incremental attack maps have several traps:
- **Slider rays change with occupancy.** A move that doesn't move a
  slider can still change ALL sliders' attack rays if the move's FROM
  or TO square sits on those rays.
- **Discovered attacks.** Moving a piece OFF a slider's ray can
  *uncover* a previously-blocked attack.
- **Blocked attacks.** Moving a piece ONTO a slider's ray can *block* a
  previously-open attack.
- **Symmetric maintenance.** We need BOTH sides' attack maps
  (state.whiteAttacks + state.blackAttacks) because every move is from
  one side's perspective and the leaf uses the *enemy's* attack map.

This plan is meaningfully harder than Plan B. Recommend doing **Plan B
first** (which gives the substrate — make/unmake on BoardState), then
Plan C bolted onto that substrate.

### 1.3 Standing measurement entering this plan

- E7 state: 75.20 / 75.30 ms perft 5.
- Profiler: `findSliderAttacksOnly` ~9 % of total perft time.
  `findAttackedSquares` (full) ~12 % in the slow-path leaves.
- Combined attack-computation work: ~10-15 ms across all 4 M leaves.

### 1.4 What state we maintain

Three new fields in `BoardState`:
```cpp
struct BoardState {
    // ... existing QBB + 8 derived bitboards from Plan B ...
    uint64 whiteAttacks;     //  8 bytes — squares attacked by white pieces
    uint64 blackAttacks;     //  8 bytes — squares attacked by black pieces
    uint64 pinnedByEnemy;    //  8 bytes — squares of MY pieces pinned by enemy sliders (computed lazily? maybe per-color)
};
```

Hmm, `pinnedByEnemy` is asymmetric (depends on STM). Better:
```cpp
    uint64 whitePinnedByBlack;  // white pieces pinned by black sliders
    uint64 blackPinnedByWhite;  // black pieces pinned by white sliders
```

Total: +32 bytes to BoardState. Combined with Plan B's 96 bytes (QBB + 8
derived), we're at 128 bytes — still fits 2 cache lines.

### 1.5 Why we maintain BOTH sides' attack maps

The leaf uses ENEMY's attack map (= the side that just moved, since we
flipped STM in the recurse). So at depth-N (chance=X), the leaf needs
`attacks[!X]`.

But the depth-(N-1) parent's countMoves at THAT depth also needed
`attacks[X]` (the enemy of N-1's STM). So at every depth we need attacks
for *whichever side is the enemy of the current STM*.

Maintaining BOTH means we always have what we need without recomputation.
The cost: every move updates BOTH attack maps (the mover's because their
piece moved; the opponent's because their slider rays may have
changed due to occupancy delta).

---

## 2. Architecture

### 2.1 The state invariant

After every successful `makeIncr` and `unmakeIncr`:
```
state.whiteAttacks  ==  attacks_by_white(state.boardPosition)
state.blackAttacks  ==  attacks_by_black(state.boardPosition)
```

Where `attacks_by_X(pos)` is the canonical full computation
(`findAttackedSquares` from X's perspective).

The maintenance must preserve this invariant. **The hardest part of this
plan is getting the incremental updates correct.**

### 2.2 What does "delta" mean for a move?

Consider a single move by side X: piece P moves from F to T (possibly
captures victim V).

**`attacks_by_X` delta:**
- LOST: P's attacks from F (computed against old occupancy at F).
- GAINED: P's attacks from T (computed against new occupancy at T).
- Plus: ANY X slider whose attack ray passed through F gains additional
  squares (F is now empty, so the ray extends).
- Plus: ANY X slider whose attack ray now passes through T loses squares
  (T is now blocked, so the ray stops at T).

**`attacks_by_!X` (opponent's attacks) delta:**
- IF V was captured: V's attacks lost (V no longer on board).
- ANY !X slider whose attack ray passed through F: ray now extends (F empty).
- ANY !X slider whose attack ray now passes through T: ray stops at T.

So the move affects BOTH sides' attack maps, primarily via:
1. The mover's own piece-attack delta (large change, well-localized).
2. Slider rays through F and T (small-to-medium change, requires
   inspecting all sliders to know which rays are affected).

### 2.3 The slider-ray-delta sub-problem

For each side, find all SLIDERS whose attack rays pass through square F
or T (or both). For each such slider:
1. Recompute its attacks with the NEW occupancy (using existing magic LUT).
2. XOR the difference into that side's attack map.

The set of sliders whose rays pass through square X is:
```
slidersAimingAtX_bishop = sqBishopAttacks(X) & (allBishopQueens)
slidersAimingAtX_rook   = sqRookAttacks(X)   & (allRookQueens)
```

`sqBishopAttacks(X)` is the LUT giving all squares a bishop AT X attacks
on an empty board — i.e., its diagonal mask. This is a single LUT load.

So per move, for each side, we have:
- Determine sliders aiming at F: 2 LUT loads + 2 ANDs.
- Determine sliders aiming at T: 2 LUT loads + 2 ANDs.
- For each (typically 0-4 sliders): recompute its attack pattern.

Plus the mover's own piece-attack delta.

Total per move: roughly 4-8 magic LUT loads on average (vs ~6 in
findAttackedSquares but with the savings being amortized across many
leaves).

### 2.4 Performance estimate

The maintenance cost per move:
- Mover's piece-attack delta: 1-2 LUT loads (knight = bulk shift; bishop
  = 1 magic LUT; etc.).
- Slider-ray-through-F delta: 0-2 LUTs per side.
- Slider-ray-through-T delta: 0-2 LUTs per side.

**Total per make: ~4-8 LUT loads + ~20 ALU ops ≈ 8-15 cycles ≈ 3-5 ns.**

Per unmake: same cost (mirror).

So **per move pair (make + unmake): ~6-10 ns.**

Old cost (the leaf): `findAttackedSquares` is ~5-6 ns per leaf
invocation. Per move (not leaf) the cost is hit ONCE per leaf at the
deepest level. So at perft 5, ~4 M leaves × 5 ns = 20 ms.

New cost: 4 M makes + 4 M unmakes × 4 ns = 32 ms. WORSE.

**Wait — this suggests Plan C might not pay off.** Let me re-analyze.

Hmm — the move/unmake pair happens at EVERY level of the recursion, not
just depth-1. So:
- depth 5: 1 root, 0 moves
- depth 4: 48 moves
- depth 3: ~2 K moves
- depth 2: ~98 K moves
- depth 1: ~4 M moves (the leaves themselves don't make further moves)

Total moves at all depths: ~98 K + 2 K + 48 + 1 ≈ 100 K.

But each LEAF invocation needs the attack map of the depth-1 position
(the leaf's "current" state). At depth-1 we ALSO call make for each move
when enumerating leaves' children — wait, depth-1 is the LEAF, no further
recursion. The leaf just COUNTS moves; doesn't make them.

So move/unmake happens at depths 2-5. Total: ~100 K make + ~100 K unmake
= ~200 K updates. Per update ~4-8 LUT loads × 3 cycles = ~15 cycles = ~5 ns.
Total: **~1 ms cost for incremental attack maintenance.**

vs OLD cost: 4 M leaf invocations × 5 ns per findAttackedSquares = ~20 ms.

**Net savings: ~19 ms.** Cool — this IS worth it.

Wait I made an error. Let me recount.

In the original architecture, `findAttackedSquares` is called at the LEAF
(depth 1). One call per leaf. There are 4 M leaves. So 4 M findAttackedSquares
calls. ~5 ns each = ~20 ms.

In the incremental architecture, `findAttackedSquares` is computed ONCE
at the root then maintained incrementally per move. ~100 K make + 100 K
unmake = ~200 K incremental updates. Each ~5 ns = ~1 ms.

**Savings: ~19 ms on perft 5.** Very promising IF correctness can be
preserved.

But wait — at the depth-2 parent, we DO buffer children. We make each
child, count its moves, "unmake" implicitly by overwriting in the
buffer slot. With incremental attacks, the buffer needs to ALSO store
the attack maps per child (or we restore from delta).

This gets complex with the buffered architecture. Cleaner if we have
Plan B's true make/unmake first.

### 2.6 The leaf's `findPinnedPieces` work

Same opportunity. `findPinnedPieces` iterates sliders aiming at MY king
and detects single-blockers. ~2 ns per call × 4 M = ~8 ms.

If we maintain "pinned by enemy" incrementally too, that's another ~8 ms
saved at perft 5.

But the maintenance is harder because:
- Pinned status changes if the piece BETWEEN slider and king moves.
- Pinned status changes if a NEW slider appears on king's diagonal/orthog.

Approach: maintain `whitePinnedByBlack` and `blackPinnedByWhite`
bitboards. Update them whenever:
- A KING moves (the entire pinned set re-determined relative to new king position).
- A SLIDER moves (its potential pins recomputed).
- ANY piece moves on/off the diagonal/orthog rays from king (might block/unblock pins).

**Bottom line:** pin maintenance is harder than attack maintenance because
it depends on TWO pieces (slider + blocker), not one. Easier to recompute
from scratch at the leaf using existing `findPinnedPieces` — only 2 LUT
loads + a slider iteration. Skip pin incremental for now; only do attack
incremental.

So **revised target: ~19 ms savings, no pin maintenance**.

---

## 3. Phased implementation

### Phase 0 — Add `whiteAttacks` / `blackAttacks` to BoardState (1 day)

**Prerequisite:** Plan B Phase 0-7 complete (BoardState exists, make/unmake
works on QBB + derived bitboards).

**Steps:**
- Extend `BoardState` with `whiteAttacks` and `blackAttacks` fields.
- `BoardState::fromQBB(qbb, gs)` computes both via existing
  `findAttackedSquares` (for both colors).
- BOARDSTATE_VERIFY also checks the attack invariants.

**Decision gate:** unit test: round-trip QBB → BoardState → check both
attack fields match `findAttackedSquares` from each perspective.

### Phase 1 — Naive incremental: recompute both attack maps on every make (2 days)

This is the "lazy" implementation. After every makeIncr, call
`findAttackedSquares` twice (once per color) and store the result.

**Why this phase?** It validates the WIRING is correct before optimizing.
With this in place:
- `makeIncr` ends with: `s.whiteAttacks = findAttackedSquares(...white perspective...); s.blackAttacks = findAttackedSquares(...black perspective...);`
- Leaf reads `s.whiteAttacks` / `s.blackAttacks` directly; never recomputes.

**Expected perf:** WORSE than baseline (doing 2× findAttackedSquares per
move instead of 1× per leaf). But: enables phase 2-5 to optimize the
update path one slot at a time, knowing the wiring works.

**Decision gate:** counts ✓ for all 5 test positions. Perf is expected
to regress here — this phase is correctness-only.

### Phase 2 — Incremental for KNIGHT moves only (2 days)

Knight moves are the SIMPLEST incremental case:
- Knight's own attacks change: easy — `knightAttacks(BIT(F))` lost,
  `knightAttacks(BIT(T))` gained.
- Other knights of the mover's color: unchanged.
- Slider attacks of the mover's color: change only because occupancy
  (F empty, T occupied) — recompute affected sliders.
- Opponent's piece attacks (pawn/knight/king): unchanged (their pieces
  didn't move).
- Opponent's slider attacks: change because occupancy delta.

**Algorithm for `updateAttacksKnight<chance>(s, from, to, capture)`:**
```cpp
const uint64 fBit = BIT(from), tBit = BIT(to);

// 1. Mover's own knight delta.
uint64 attacksMover = s.attacksOf(chance);
attacksMover ^= sqKnightAttacks(from);
attacksMover ^= sqKnightAttacks(to);
// If capture: don't change opponent's knight attacks here; we handle below.

// 2. Mover's slider rays through F/T (occupancy delta).
//    F is now empty (one less blocker), T is now occupied (one more blocker).
uint64 sliderOccBefore = s.allPiecesBefore (we have to know this... ugh)
```

Wait — by the time `updateAttacks` is called inside `makeIncr`, the state
fields have likely been updated. We need to be careful about WHEN we
compute the delta vs WHEN we apply it.

**Cleaner protocol:** call `updateAttacks` BEFORE the QBB/derived
updates. That way `s.allPieces` still reflects the OLD occupancy.
```cpp
template <uint8 chance, uint8 piece>
void makeIncr(BoardState &s, CMove move) {
    const uint64 fBit = BIT(move.from), tBit = BIT(move.to);
    
    // 1. Compute the new attack maps (mover's + opponent's) based on the
    //    move. Uses OLD state.
    updateAttacks<chance, piece>(s, move);
    
    // 2. Update QBB, derived bitboards, gs. (existing Plan B logic)
    applyMoveQBBAndDerived<chance, piece>(s, move);
}
```

Actually, this isn't quite right either because `updateAttacks` needs the
new occupancy to compute "what sliders see WITH F empty / T occupied".

The clean protocol is **interleaved**: update mover's piece attack
contribution first, then update occupancy, then update slider deltas.

Let me sketch the KNIGHT case fully:

```cpp
template <uint8 chance>
void updateAttacksKnight(BoardState &s, uint8 from, uint8 to, uint8 capturedPiece) {
    const uint64 fBit = BIT(from), tBit = BIT(to);

    uint64 &moverAtks = (chance == WHITE) ? s.whiteAttacks : s.blackAttacks;
    uint64 &oppAtks   = (chance == WHITE) ? s.blackAttacks : s.whiteAttacks;

    // -- Mover's piece-attack contribution delta --
    // The knight at FROM was contributing sqKnightAttacks(from) to moverAtks.
    // The knight at TO will contribute sqKnightAttacks(to) to moverAtks.
    // But careful: if another mover-knight ALSO has these squares in its
    // pattern, the XOR isn't right (overlap). We need to recompute the
    // moverAtks knight contribution as `knightAttacks(s.knights & moverPieces)`.
    
    // Simpler approach: recompute the knight contribution to moverAtks from
    // scratch each time. Bulk shift form costs ~14 ALU ops, no LUT load.
    // The other parts of moverAtks (pawn/king/slider) are handled below.
    //
    // Hmm — but we don't have moverAtks decomposed into pieces. We have
    // ONE bitboard. We can't subtract just "knight contribution" without
    // recomputing.
    //
    // ALTERNATIVE: maintain SUB-BITBOARDS in BoardState:
    //   whiteAtksPawn, whiteAtksKnight, whiteAtksSliderBishop, whiteAtksSliderRook, whiteAtksKing
    //   blackAtks{Pawn,Knight,SliderBishop,SliderRook,King}
    //
    // That's 10 extra uint64 = 80 bytes. BoardState swells to ~208 bytes
    // = 3.25 cache lines. Painful.
}
```

OK this is getting hard. Let me reconsider the design.

**Insight**: we don't actually need to maintain attack sub-bitboards. We
can recompute the knight-attack contribution from `knights & moverPieces`
when needed (it's a bulk shift = ~14 ops, no LUT). Same for pawn (4
shifts + 2 ORs), king (1 LUT load). Sliders are the expensive part.

**Revised storage:** maintain just `whiteAttacks` and `blackAttacks` (the
full bitboards). When we need to "subtract knight contribution":
```
knight_atks_white = knightAttacks(s.knights & s.whitePieces);
```
And use bitwise math: `whiteAtks_minus_knight = whiteAttacks_old XOR knight_atks_white_old`. But this assumes no OVERLAP between knight attacks and other piece attacks of the same color. Overlap exists.

The problem: bitboard attack maps are ORed, not summed. We can't subtract
overlaps cleanly. So XOR doesn't reliably "remove" a piece's contribution
from the union.

**This is fundamental.** Incremental attack maintenance via XOR is wrong
when contributions overlap.

**Correct approach:** decompose by piece type. For each color, maintain:
- `atksPawn[color]`
- `atksKnight[color]`
- `atksKing[color]`
- `atksSlider[color]`  (combined bishop+rook+queen attacks)

Each piece type's attacks can be XOR-updated because pieces of the same
type don't overlap (each piece sits on one square).

Wait — two knights of the same color CAN attack the same square. So
`knightAttacks(twoKnights)` is the OR of their patterns. Removing one
knight requires recomputing the bulk.

`knightAttacks(knights)` is a bulk SHIFT formula on the whole knights
bitboard. It's not "knight A's attacks | knight B's attacks" computed
separately. So when one knight moves, we re-run the bulk formula on the
new knights bitboard. That's cheap (~14 ops).

OK so for non-slider piece types, just recompute the bulk attack on the
updated piece bitboard. Cheap.

**For sliders**, recomputing the bulk means iterating each slider with
a magic LUT — that's what we're trying to AVOID.

**Slider-specific incremental:** for the moved slider, swap its old
contribution for its new contribution. For OTHER sliders, recompute if
the move affected their ray (F or T on their ray); else leave alone.

But again, we can't "swap" a single slider's contribution without
overlap concerns. Unless we maintain PER-SLIDER attack maps? That's too
much state.

**Real solution:** decompose `slider_attacks` by maintaining the slider
attack contribution as a SUM over sliders (not XOR), using `popcount`-based
encoding. Or maintain attack contribution per SQUARE.

The classic approach: maintain `attack_count[64]` array — number of
times each square is attacked. To compute attack map: `(attack_count >
0)` per square. This supports clean increment/decrement.

`uint8 attackCount[64]` × 2 colors = 128 bytes. BoardState swells
significantly. But updates become arithmetic-clean.

Hmm but `attack_count > 0` per square requires conversion at every read.

**Practical compromise:** maintain `attacks` as a bitboard but use a
"safe-XOR" protocol: maintain a `count[64]` array per color, derive the
bitboard from the count whenever needed (or once per leaf, which is per
make/unmake — the cost is at recompute time, not at maintenance time).

OK let me give up on attack-count and think about a SIMPLER approach.

### Phase 2 (revised) — Restricted incremental: only handle the SAFE delta case (1 day)

For moves where the delta is unambiguous, do incremental. Otherwise,
recompute from scratch.

**SAFE deltas** (where XOR works correctly):
- The moved piece's OWN attack contribution doesn't overlap with same-color
  pieces' attacks for the new position. (This is hard to guarantee a
  priori.)

OK this is getting nowhere. Let me revise the plan.

---

## 2 (revised) — Simpler architecture: piece-typed attack sub-bitboards

```cpp
struct BoardState {
    // ... QBB + 8 derived from Plan B ...
    
    // 8 attack sub-bitboards: per color, per piece-type group.
    uint64 attacksPawn[2];        //  16 bytes — pawn attacks of each color
    uint64 attacksKnight[2];      //  16 bytes
    uint64 attacksKing[2];        //  16 bytes
    uint64 attacksSlider[2];      //  16 bytes — combined bishop+queen + rook+queen
    
    // Caller computes full attacks lazily:
    uint64 attacksOf(uint8 color) const {
        return attacksPawn[color] | attacksKnight[color] | attacksKing[color] | attacksSlider[color];
    }
};
```

BoardState size: 96 + 64 = 160 bytes. Round to 192 (3 cache lines). Painful
but tolerable on a 48 KB L1d.

Now updates per move type:

#### KNIGHT move (no capture, no special)

- `attacksKnight[mover]`: recompute via bulk shift on new knight set. ~14 ops.
- `attacksSlider[mover]`: occupancy changed; need to recompute affected
  sliders. (See below.)
- `attacksPawn[mover]`, `attacksKing[mover]`: unchanged.
- `attacksSlider[!mover]`: occupancy changed; recompute affected sliders.
- `attacksPawn[!mover]`, `attacksKnight[!mover]`, `attacksKing[!mover]`:
  unchanged.

For the slider deltas: any !mover-color (or mover-color) slider whose
attack ray passes through F or T may need recomputation.

```cpp
uint64 affectedSliders = (sqBishopAttacks(from) | sqBishopAttacks(to)) & (mover_bishopQueens)
                      | (sqRookAttacks(from)   | sqRookAttacks(to))   & (mover_rookQueens);
// Recompute the contribution of these sliders to attacksSlider[mover]
// by iterating each and OR-ing into a fresh accumulator, then update.
```

But again, we can't cleanly "subtract" some sliders from `attacksSlider`
without knowing which squares they contributed.

**Solution:** recompute attacksSlider[mover] from scratch. That's one
slider iteration per side. ~3-6 LUTs. ~5-10 cycles per side.

`updateAttacksKnight<chance>`:
- attacksKnight[mover] = `knightAttacks(s.knights & moverPieces)` (14 ops).
- attacksSlider[mover] = `findSliderAttacksOnly(...occupancy..., bishopsM, rooksM, kingM)` (3-6 LUTs).
- attacksSlider[!mover] = `findSliderAttacksOnly(...same occupancy..., bishopsOpp, rooksOpp, kingOpp)` (3-6 LUTs).

**Wait — slider attacks of !mover side also change!** Because occupancy
changed (F empty, T occupied), opponent sliders' rays through F/T change.

So we're recomputing BOTH `attacksSlider[mover]` AND `attacksSlider[!mover]`
from scratch on every move. That's the full slider work × 2.

**Performance check:**
- Old (leaf): `findSliderAttacksOnly` once per leaf at depth-1. ~5 ns × 4M = 20 ms.
- New (incremental): 2× findSliderAttacksOnly per move. ~100 K moves × 2 × 5 ns = 1 ms.

Wait the leaf is at depth-1 ONLY counted; no makes at depth-1. Moves happen
at depths 2-5. ~100 K moves total. Each move recomputes slider attacks twice.
~1 ms total maintenance.

vs OLD: 4 M leaf calls × ~5 ns each = ~20 ms.

**Savings: ~19 ms.** Same as before.

OK so the architecture IS workable, with **bulk-recompute slider attacks
per move** (2 sides per move).

But: we'd be doing slider attack computation 200 K times (~100 K moves × 2 sides)
vs 4 M times today. Net 20× reduction. ~19 ms savings.

Conditional path: if neither F nor T is on any !mover slider's attack pattern,
opponent slider attacks DON'T change. We can detect this and skip:
```
if ((sqBishopAttacks(from) | sqBishopAttacks(to)) & oppBishopQueens == 0 &&
    (sqRookAttacks(from)   | sqRookAttacks(to))   & oppRookQueens   == 0) {
    // !mover sliders unaffected; skip recompute
}
```

This skip is common for moves that aren't on any slider's ray. Probably
~30 % of moves. Savings: 30 % × 200 K × 5 ns = 0.3 ms additional.

Small but cheap.

### Phase 3 — Per-piece-type makeIncr-with-attack-update (5-6 days)

Repeat the KNIGHT logic for the other 5 piece types:

| Piece | Mover's own atk delta | Slider deltas |
|---|---|---|
| KNIGHT | recompute `attacksKnight[mover]` bulk | recompute both sides' sliders if rays affected |
| BISHOP/ROOK/QUEEN | recompute `attacksSlider[mover]` (moving slider changes its own attacks) | recompute opp sliders if rays affected |
| PAWN | recompute `attacksPawn[mover]` bulk; if EP or capture, may also affect opp |  same as knight |
| KING | recompute `attacksKing[mover]` (1 LUT load); castling: also moves a rook → recompute slider attacks | same |

PROMOTION: changes piece type. Recompute `attacksPawn[mover]` AND the
promoted piece's attack sub-bitboard.

CAPTURE: removes a piece. Recompute the captured piece's color's
corresponding sub-bitboard.

### Phase 4 — unmakeIncr-with-attack-update (3-4 days)

Mirror of Phase 3. Restore the previous attack sub-bitboards. Two options:
1. Compute the inverse delta (clean math but error-prone).
2. Save the previous attack sub-bitboards in UndoInfo (simple but
   UndoInfo grows from 4 bytes to ~70 bytes).

Recommend option 2 for sanity. UndoInfo at 64-128 bytes is still fine
on stack (the recursion is bounded to ~5-6 active makes).

### Phase 5 — Leaf reads pre-computed attacks (1 day)

The leaf no longer calls `findAttackedSquares`. It reads:
```cpp
uint64 threatened = s.attacksOf(!chance);  // 4 ORs of cached sub-bitboards
```

Or even better: lazily computed in BoardState — `attacksOf(color)`
inlines to 3 ORs.

**Decision gate:** counts ✓, perft 5 ≤ 65 ms.

### Phase 6 — Skip slider re-computation when rays unaffected (1-2 days)

Add the optimization from Phase 2: detect when F and T don't intersect any
slider's diagonal/orthog mask, skip the recompute.

**Decision gate:** counts ✓, perft 5 ≤ 60 ms.

### Phase 7 — PGO retrain + tuning (1 day)

**Decision gate:** final perft 5 ≤ 60 ms.

---

## 3. Phased plan summary

| Phase | Days | What | Pass criterion |
|---|---|---|---|
| 0 | 1 | BoardState fields + lazy init | unit test |
| 1 | 2 | Naive recompute-after-make (validates wiring) | counts ✓ |
| 2 | 1 | Restricted-incremental for KNIGHT only | counts ✓, perft 5 ≤ 72 ms |
| 3 | 5-6 | All 6 piece types with attack delta | counts ✓, perft 5 ≤ 68 ms |
| 4 | 3-4 | unmakeIncr with attack delta | counts ✓ |
| 5 | 1 | Leaf reads pre-computed attacks | counts ✓, perft 5 ≤ 65 ms |
| 6 | 1-2 | Skip slider recompute when rays unaffected | counts ✓, perft 5 ≤ 60 ms |
| 7 | 1 | PGO retrain | perft 5 ≤ 60 ms |

**Total: ~15-18 days = 3-4 work-weeks.** Heavily front-loaded in Phase 3.

---

## 4. Validation strategy

### 4.1 Correctness

Same 5 test positions as Plans A/B. Must pass after each phase.

### 4.2 Defensive assertion mode

Extend `BOARDSTATE_VERIFY` (from Plan B) to also check the attack
sub-bitboards:
```cpp
#ifdef BOARDSTATE_VERIFY
    BoardState fresh = BoardState::fromQBB(s.qbb, s.gs);
    assert(s.attacksPawn[0]   == fresh.attacksPawn[0]);
    assert(s.attacksKnight[0] == fresh.attacksKnight[0]);
    // ... etc for all 8 sub-bitboards ...
#endif
```

Run with verify enabled during all of Phase 3-4. Disable for perf benches.

### 4.3 Per-phase pass criteria

| Phase | Pass | Performance |
|---|---|---|
| 0 | Unit test passes | N/A |
| 1 | All 5 test positions counts ✓ | Expected regression to ~150 ms (2× findAttackedSquares per move) |
| 2 | Counts ✓ | ≤ 72 ms (KNIGHT moves now incremental) |
| 3 | Counts ✓ | ≤ 68 ms |
| 4 | Counts ✓ (with verify mode) | ≤ 67 ms |
| 5 | Counts ✓ | ≤ 65 ms |
| 6 | Counts ✓ | ≤ 60 ms |
| 7 | Counts ✓ | ≤ 60 ms |

---

## 5. Risks and mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Attack delta math is bug-prone (especially captures, EP, promotion, castle) | **Very High** | Critical | BOARDSTATE_VERIFY mode + extensive testing on all 5 test positions at each phase |
| BoardState 192-byte size causes cache pressure | Medium | Medium | Profile L1d misses; consider packing or removing per-color attack maps that aren't used |
| UndoInfo growing to ~70 bytes per move stresses stack | Low | Low | 5-6 active makes × 70 = 420 bytes. Fine. |
| Slider re-recompute on every move is more expensive than estimated | Medium | High | Phase 1 measurement bounds this; if 200 K × cost is too high, optimize slider re-recompute (e.g., decompose slider per square — but back to attack_count[64] mess) |
| Phase 6 optimization (skip slider when rays unaffected) adds branchy code that hurts more than it helps | Low | Medium | Bench Phase 6 carefully; revert if neutral |
| Composing with E7 doesn't help (since E7 caches non-slider attacks but with incremental we have everything cached) | Low | Low | Drop E7's split-buffer design once incremental works |
| Plan B make/unmake substrate isn't in place | High (prerequisite) | Critical | Don't attempt Plan C without Plan B done first |
| Bulk-recompute of slider attacks dominates and there's no good way to reduce it | Medium | Medium | Worst case: keep slider attacks computed-per-leaf (old way) and ONLY incrementalize non-slider attacks. ~30 % of the savings; still useful. |

---

## 6. Files that will change

| File | Change |
|---|---|
| `chess.h` | Extend `BoardState` with attack sub-bitboard fields. Extend `UndoInfo` with saved attack maps (for unmake). |
| `MoveGeneratorBitboard.h` | Add `updateAttacks<chance, piece>(s, move, captured)` helpers per piece type. Modify all `makeIncr` to call them. Same for `unmakeIncr`. Modify leaf to read `s.attacksOf(!chance)` instead of `findAttackedSquares`. |
| `launcher.cpp` | Possibly: root-level compute of initial attack maps when creating root BoardState. |
| `optimization_log.md` | Append. |
| `CMakeLists.txt` | Existing `BOARDSTATE_VERIFY` flag now checks more invariants. |

Estimated LOC: **~1000-1500 lines new code, ~500 lines modified**.

---

## 7. Open questions

1. **Should we maintain per-color or per-piece-type sub-bitboards?**
   Per-piece-type is cleaner for incremental updates (clean XOR semantics
   within a type). Per-color is more cache-efficient. Recommend
   per-piece-type-per-color (8 sub-bitboards).
2. **Should `findPinnedPieces` also become incremental?** Recommend NO —
   pin updates are complex and the savings are smaller. Keep `findPinnedPieces`
   computed per leaf using the existing function. ~1.5 ns per leaf × 4 M ≈ 6 ms,
   which we accept.
3. **What's the right way to handle EP / promotion / castle in updateAttacks?**
   For these, just RECOMPUTE the affected attack sub-bitboards from scratch.
   They're rare; correctness > speed.
4. **Can UndoInfo be smaller?** If we recompute the previous attack maps
   instead of storing them: cleaner stack but error-prone. Recommend
   storing for correctness.
5. **Does the FgmcCount2Processor buffer architecture still make sense?**
   With make/unmake + incremental attacks, the buffer adds nothing (we
   modify state, recurse, restore). DROP THE BUFFER for the incremental
   architecture. Saves ~8 KB stack per depth-2 frame.

---

## 8. What success looks like

| Final perft 5 | Verdict |
|---|---|
| ≤ 50 ms | **Stretch hit.** Plans B+C combined hit the 2× target zone. |
| 50-55 ms | **Strong.** Major architectural wins materialized. |
| 55-60 ms | **Target.** Plan C delivered its independent share on top of Plan B. |
| 60-65 ms | **Partial.** Slider re-recompute was more expensive than estimated. |
| 65-72 ms | **Marginal.** Incremental wins offset by maintenance overhead. |
| > 72 ms or counts wrong | **Failed.** Revert. |

---

## 9. Estimated effort

| Phase | Days | Cumulative |
|---|---|---|
| 0 (BoardState fields) | 1 | 1 |
| 1 (naive recompute) | 2 | 3 |
| 2 (KNIGHT only) | 1 | 4 |
| 3 (all 6 piece types) | 5-6 | 9-10 |
| 4 (unmake mirror) | 3-4 | 12-14 |
| 5 (leaf reads attacks) | 1 | 13-15 |
| 6 (skip-recompute opt) | 1-2 | 14-17 |
| 7 (PGO retrain) | 1 | 15-18 |

**Total: 3-4 work-weeks** on top of Plan B's prerequisite.

---

## 10. Strategic positioning

This is the **highest-payoff plan in absolute terms** but the **highest
correctness risk**. Recommend:

1. **Do Plan A first** (3 weeks, lower risk, ~20 % win). This validates
   the AVX2 cliff regime and gets a real win quickly. Demonstrable
   progress.

2. **Do Plan B second** (3-4 weeks, medium risk, ~20-25 % additional).
   Establishes make/unmake substrate. Composes with Plan A trivially.

3. **Do Plan C third** (3-4 weeks, high risk, ~10-15 % additional).
   The compound effect of B+C is the path to the 2× target.

**If only one plan is done:** Plan B is the best ROI — substrate value +
direct perf gains. Plan A is the fastest to deliver but caps at ~20 %.
Plan C standalone (without B's make/unmake) is impractical due to the
buffer architecture incompatibility.

**Aggregate target (all 3 plans applied):**
- E7 baseline: 75.3 ms
- Plan A win: -15 ms → ~60 ms
- Plan B win on top: -10 ms → ~50 ms
- Plan C win on top: -7 ms → ~43 ms

That's **2.05× from the 91 ms pre-sprint baseline**, near the 2× goal
originally documented in `optimization_log.md`.

---

## 11. Pre-session checklist

- [ ] Plan B Phase 0-7 complete and working (perft 5 ≤ 70 ms via Plan B).
- [ ] BoardState struct exists and `BOARDSTATE_VERIFY` mode works.
- [ ] All 5 test positions pass.
- [ ] Profile data shows where current attack-compute time is spent (the
      `-profile` data confirms ~12 % attribution to findAttackedSquares).
- [ ] Open `MoveGeneratorBitboard.h` at `findAttackedSquares` (line 745
      currently) and `findSliderAttacksOnly` (line 712 — added in E7).

When all check, begin Phase 0.

---

*Plan written 2026-05-15. Plan C is the deepest and most risky of the
three follow-on plans. It assumes Plan B's make/unmake architecture is
in place; without it, the design is much more complex due to the
buffered architecture's per-child copy semantics. Estimated payoff of
10-15 % on top of the prerequisites for ~3-4 weeks of careful work,
predominantly correctness-engineering.*
