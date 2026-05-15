# Plan B — Make/Unmake with Incremental Piece Bitboards

**Target:** push `kiwipete perft 5 -nott` single-thread on AMD Ryzen 9 9950X3D
from **~75.3 ms** (post-E7 state) → **≤ 55 ms** (-27 %). Stretch: ≤ 50 ms.

**Conceptual goal:** eliminate per-leaf `DERIVE_PIECE_BITBOARDS` entirely.
The leaf reads pre-derived bitboards directly. The cost (per-make-move
update of derived state) is paid ONCE per move (depth-2 emit) instead of
ONCE per leaf (depth-1 enter).

---

## 1. Context

### 1.1 Why this approach

`DERIVE_PIECE_BITBOARDS` is 17 ALU ops to extract `allPieces, blackPieces,
whitePieces, allPawns, knights, bishopQueens, rookQueens, kings` from a
QBB. It runs at every leaf entry (~4 M times for perft 5).

**Cost accounting:**
- Per leaf: ~17 ALU ops → ~6 cycles with ILP → ~2 ns
- Total per perft 5: 4 M × 2 ns = **~8 ms** purely on derives

Plus the *implicit* derive cost in deeper plies. `enumerateMoves` itself
derives at every recursion level (depth 2, 3, 4, 5 each derive once).
For ~98 K depth-2 parents + ~2 K depth-3 + ... ≈ 100 K derives at non-leaf
levels = additional ~0.2 ms (small).

**Total derive cost: ~8-10 ms** if eliminated, plus follow-on wins from:
- Eliminating `kings & enemyPieces`, `bishopQueens & enemyPieces` etc. that
  appear inside leaf bodies (~5 ms additional)
- Eliminating the `kings & myPieces` step (~1 ms)
- Potentially eliminating findPinnedPieces' input derives

**Aggregate target: 12-15 ms savings = 16-20 % on perft 5.**

### 1.2 Why this approach is risky

Make/unmake on a heavily-templated perft engine touches every code path
that mutates board state. Each piece type × each special case
(capture/EP/promotion/castle) needs its own update logic. Bugs are subtle
because the perft count can be CORRECT for shallow depths and wrong only
at depth 6+.

This was attempted in many chess engines historically. Modern engines
(Stockfish, Ethereal) do incremental state via this approach because the
savings are real. **The path is well-trodden; the engineering is
high-volume but not novel.**

### 1.3 Standing measurement entering this plan

- E7 state: 75.20 / 75.30 ms perft 5.
- Profiler: countMovesPairCached + countMovesPair = 76 % of time. Inside
  those, derives + leaf body share roughly equally.
- Microbench countMovesDispatch: 14.33 ns / call. Of this, derive is
  estimated ~2-3 ns based on the structure.

### 1.4 What "make/unmake" means here

Two architectural changes are bundled:
1. **Carry derived state alongside QBB** across recursion. A new struct
   `BoardState` holds both QBB and 8 derived bitboards (~96 bytes total).
2. **make/unmake** updates derived state explicitly. After processing one
   child, we *unmake* to return to parent state — vs the current
   *copy-make* where the parent is preserved separately and the child is
   a fresh copy.

We can do (1) WITHOUT (2): keep copy-make, but compute child's derived
state from parent's derived state + move delta. This is what E1 tried and
failed (the per-emit store-buffer cost was 10 ms regression).

We can do (1) AND (2): truly incremental with explicit unmake. Lower
memory traffic (no fresh QBB copy per child) but more complex.

**This plan does (1) + (2).** The reason E1 failed was *combining* per-slot
store with per-child copy-make — both stores. With true make/unmake,
there's only ONE board (mutated and restored) at any depth, so the store
traffic is bounded by recursion depth not by move count.

---

## 2. Architecture

### 2.1 The `BoardState` struct

```cpp
struct alignas(64) BoardState {
    // QBB still needed for "set piece at square" / generic queries.
    QuadBitBoard qbb;            // 32 bytes

    // Derived bitboards (always kept consistent with qbb).
    uint64 allPieces;            //  8
    uint64 whitePieces;          //  8
    uint64 blackPieces;          //  8
    uint64 allPawns;             //  8  (both colors)
    uint64 knights;              //  8
    uint64 bishopQueens;         //  8
    uint64 rookQueens;           //  8
    uint64 kings;                //  8

    GameState gs;                //  1 byte (whiteCastle:2, blackCastle:2, EP:4)
    uint8 _pad[31];              // pad to 128 bytes (2 cache lines)
};
static_assert(sizeof(BoardState) == 128);
```

**Total: 128 bytes = 2 cache lines.** Loads at the leaf are now hot reads
of one struct, not 4 reads of a QBB plus 17 ALU ops.

### 2.2 make / unmake API

```cpp
// Per-piece-type templated make. Returns enough info for unmake to reverse it.
template <uint8 chance, uint8 piece>
__forceinline UndoInfo makeIncr(BoardState &s, CMove move) noexcept;

template <uint8 chance, uint8 piece>
__forceinline void unmakeIncr(BoardState &s, CMove move, UndoInfo undo) noexcept;

struct UndoInfo {
    uint8 capturedPiece;     // 0=none, 1=PAWN,...,6=KING (NEVER 6 in legal play)
    uint8 oldCastleRaw;      // gs.raw before the move
    uint8 oldEP;             // gs.enPassent before
    uint8 promotedTo;        // for promotion only
    // No need to save more — the rest is reversible from `move` + above.
};
static_assert(sizeof(UndoInfo) == 4);
```

`UndoInfo` is 4 bytes — easy to keep on stack across the recursion.

### 2.3 The recursion now looks like

```cpp
template <uint8 chance>
uint64 perft_inc(BoardState &s, int depth) {
    if (depth == 1) {
        return countMovesIncr<chance>(s);   // uses pre-derived; no DERIVE step
    }
    if (depth == 2) {
        return countTwoLevelSubtreeIncr<chance>(s);
    }

    uint64 sum = 0;
    enumerateMovesIncr<chance>(s, [&](CMove move, uint8 piece) {
        UndoInfo undo = makeIncrSwitch<chance>(s, piece, move);
        sum += perft_inc<!chance>(s, depth - 1);
        unmakeIncrSwitch<chance>(s, piece, move, undo);
    });
    return sum;
}
```

Note the lambda is illustrative; in practice we keep the templated
Processor pattern. The mechanical thing is: ONE BoardState is mutated and
restored throughout one branch of the recursion. No copy-per-child.

### 2.4 Per-piece-type incremental updates

#### 2.4.1 Plain piece move (no capture, no special)

Source piece in `chance` color at FROM moves to TO. TO was empty.

```cpp
// Example: KNIGHT move, no capture.
const uint64 srcBit = BIT(move.from), dstBit = BIT(move.to);
const uint64 mask   = srcBit ^ dstBit;  // toggle both squares

s.qbb.bb[0] ^= (chance == BLACK) ? mask : 0;     // color (only if chance is black)
s.qbb.bb[2] ^= mask;                              // knight has bit 2 = 1
// bb[1], bb[3] unchanged (knight's bit 1 = 0, bit 3 = 0)

s.allPieces  ^= mask;                             // toggle both squares
s.whitePieces ^= (chance == WHITE) ? mask : 0;
s.blackPieces ^= (chance == BLACK) ? mask : 0;
s.knights    ^= mask;
// allPawns, bishopQueens, rookQueens, kings unchanged
```

Cost: ~4-6 ops for a typical non-capture move. Compare to the old copy +
derive: 4 QBB writes + 17 derive ops = 21 ops.

#### 2.4.2 Capture

Source piece moves to TO; TO has an enemy piece that must be removed.

```cpp
// Capture: figure out victim type from current state (BEFORE applying source move).
// Probe in priority order: pawn (most common), then knight/bishop/rook/queen.
uint8 victim = PAWN;
if      (s.allPawns      & dstBit) victim = PAWN;
else if (s.knights       & dstBit) victim = KNIGHT;
else if (s.bishopQueens  & dstBit && !(s.rookQueens & dstBit)) victim = BISHOP;
else if (s.rookQueens    & dstBit && !(s.bishopQueens & dstBit)) victim = ROOK;
else if (s.bishopQueens  & dstBit &&  (s.rookQueens & dstBit)) victim = QUEEN;
// king can't be captured in legal play; assertion-only

undo.capturedPiece = victim;

// Clear destination square from ALL bitboards (the victim's bitboard *and*
// any others — but in fact only one of allPawns/knights/bishopQueens/rookQueens
// will have the bit set; we can be precise).
switch (victim) {
    case PAWN:    s.allPawns     &= ~dstBit; break;
    case KNIGHT:  s.knights      &= ~dstBit; break;
    case BISHOP:  s.bishopQueens &= ~dstBit; break;
    case ROOK:    s.rookQueens   &= ~dstBit; break;
    case QUEEN:   s.bishopQueens &= ~dstBit; s.rookQueens &= ~dstBit; break;
}
// allPieces & enemyPieces also need updating
s.allPieces ^= srcBit;   // FROM goes empty; TO stays occupied (just changed owner)
if constexpr (chance == WHITE) {
    s.whitePieces ^= (srcBit | dstBit);   // source moves; victim's TO bit removed... wait
    s.blackPieces ^= dstBit;              // victim removed from black
} else { /* mirror */ }
// then proceed with the standard "place attacker at dst" updates
```

Capture is messier because we have to probe to find victim type. Cost:
3-5 probes (cheap AND + zero-test) + ~6-8 update ops. ~10-12 ops.

#### 2.4.3 Promotion

Pawn move where the destination is rank 1 or 8. The pawn is REMOVED from
allPawns and the promoted piece is ADDED to its bitboard.

```cpp
// Promotion: pawn doesn't end up at TO; instead, knight/bishop/rook/queen does.
uint8 promoTo = (move.flags & 3) + KNIGHT;   // 2..5

// Source square cleanup (pawn departed):
s.allPawns ^= srcBit;

// Destination square: place the promoted piece.
switch (promoTo) {
    case KNIGHT:  s.knights      |= dstBit; break;
    case BISHOP:  s.bishopQueens |= dstBit; break;
    case ROOK:    s.rookQueens   |= dstBit; break;
    case QUEEN:   s.bishopQueens |= dstBit; s.rookQueens |= dstBit; break;
}
// allPieces, white/blackPieces toggle for srcBit ^ dstBit as normal
// QBB bb[1..3] updates to match promoted piece encoding
```

#### 2.4.4 En passant capture

Pawn at FROM diagonal-moves to TO (an empty square); the captured enemy
pawn is at TO ± 8 (one rank back from the attacking-direction pawn).

```cpp
const uint64 epCapBit = (chance == WHITE) ? (dstBit >> 8) : (dstBit << 8);
s.allPawns    &= ~epCapBit;
s.allPieces   &= ~epCapBit;
if constexpr (chance == WHITE) s.blackPieces &= ~epCapBit;
else                            s.whitePieces &= ~epCapBit;
// QBB bb[0] (for color), bb[1] (for pawn): clear epCapBit
// Then standard pawn move from FROM to TO
```

#### 2.4.5 Castling

KING moves 2 squares; ROOK also moves. Both pieces toggled.

```cpp
// King-side castle, white: king e1→g1, rook h1→f1.
// Castling can never be a capture, never check, never EP, never promotion.
s.kings        ^= (BIT(E1) | BIT(G1));
s.rookQueens   ^= (BIT(H1) | BIT(F1));
s.whitePieces  ^= (BIT(E1) | BIT(G1) | BIT(H1) | BIT(F1));
s.allPieces    ^= (BIT(E1) | BIT(G1) | BIT(H1) | BIT(F1));
// QBB bb[2], bb[3] for king positions; bb[3] for rook positions
```

8 castle cases (W/B × KS/QS, with `chance` template making it 4 runtime
each). All similar shape.

### 2.5 Game-state (castling rights, EP target)

```cpp
// gs.raw saves the whole packed byte; unmake restores it.
undo.oldCastleRaw = s.gs.raw;
undo.oldEP        = s.gs.enPassent;

// Update gs per move type:
// - King move: clear that side's castle rights (both KS and QS).
// - Rook move/capture from initial square: clear corresponding castle right.
// - Pawn double-push: set EP target.
// - Any other move: clear EP target.

// On unmake: s.gs.raw = undo.oldCastleRaw;   (the EP byte is included in raw)
```

### 2.6 The leaf — `countMovesIncr` reading pre-derived state

```cpp
template <uint8 chance, bool hasEP, bool hasCastle>
__declspec(noinline)
uint32 countMovesIncr(const BoardState &s) {
    // NO DERIVE_PIECE_BITBOARDS. All fields read directly.
    uint64 myPieces, enemyPieces;
    if constexpr (chance == WHITE) {
        myPieces    = s.whitePieces;
        enemyPieces = s.blackPieces;
    } else {
        myPieces    = s.blackPieces;
        enemyPieces = s.whitePieces;
    }

    uint64 enemyBishops = s.bishopQueens & enemyPieces;
    uint64 enemyRooks   = s.rookQueens   & enemyPieces;
    uint64 myKing       = s.kings        & myPieces;
    uint8  kingIdx      = bitScan(myKing);

    uint64 pinned     = findPinnedPieces(myKing, myPieces, enemyBishops, enemyRooks,
                                         s.allPieces, kingIdx);
    uint64 threatened = findAttackedSquares(~s.allPieces, enemyBishops, enemyRooks,
                                            s.allPawns & enemyPieces,
                                            s.knights  & enemyPieces,
                                            s.kings    & enemyPieces,
                                            myKing, !chance);

    if (threatened & myKing) [[unlikely]]
        return countMovesOutOfCheckIncr<chance>(s, myPieces, enemyPieces, pinned,
                                                threatened, kingIdx);

    return countMovesFromDerivedIncr<chance, hasEP, hasCastle>(s, myPieces, enemyPieces,
                                                               myKing, kingIdx,
                                                               pinned, threatened);
}
```

The body is mostly the same as today's `countMovesFromDerived` but reads
state from `s` instead of derived locals. The dead-code elimination should
still work because the templated parameters control which branches survive.

---

## 3. Phased implementation

### Phase 0 — Define `BoardState` and basic init (1 day)

**Steps:**
- Add `BoardState` struct in `chess.h`.
- Add `BoardState::fromQBB(const QBB&, GameState)` ctor that derives all 8
  bitboards.
- Add `BoardState::toQBB() const` that recovers a plain QBB (for legacy code
  paths that still use QBB).
- Add unit test (in `perft.cpp` or new TU): verify a few hand-crafted
  positions round-trip QBB → BoardState → QBB unchanged AND the derived
  fields match `DERIVE_PIECE_BITBOARDS`.

**Decision gate:** unit test passes, perft 1 (no recursion) on startpos
matches.

### Phase 1 — Skeleton of make/unmake (2-3 days)

Implement `makeIncr<chance, piece>` and `unmakeIncr<chance, piece>` for
the SIMPLEST move type: non-capture, non-promotion, non-castle, non-EP.
Cover all 6 piece types.

**Steps:**
- `makeIncr<chance, KNIGHT>` for plain knight move.
- `makeIncr<chance, BISHOP>`, `<ROOK>`, `<QUEEN>` plain.
- `makeIncr<chance, KING>` plain (no castle).
- `makeIncr<chance, PAWN>` plain (single push, no capture, no promotion).
- All 12 `unmake` mirrors.

**Test harness:** for each move type, hand-build a position, run a few
make+unmake cycles, verify position is unchanged. Use the `kiwipete` and
`startpos` positions on individual moves.

**Decision gate:** all 12 make/unmake round-trip cleanly. NO perft run yet
because special cases not done.

### Phase 2 — Capture handling (2 days)

Add capture probing + cleanup to all 6 `makeIncr` variants. The probe
order matters for performance — put the most common victim type first.

**Steps:**
- For PAWN capture: probe order PAWN > KNIGHT > BISHOP > ROOK > QUEEN.
- Add `UndoInfo::capturedPiece` field.
- `unmakeIncr` reads `capturedPiece` and replaces victim at TO.

**Test:** position 4 (talkchess promotion-heavy) at depth 2 — many
captures. Compare counts to known-good (perft 2 = 191).

**Decision gate:** test position 4 perft 2 / 3 / 4 match known-good values.

### Phase 3 — Promotion (1-2 days)

Add the promotion branch inside `makeIncr<chance, PAWN>`. Need to handle:
- Pawn moves to RANK 1/8 → promotion.
- Promotion can be a capture (pawn captures piece at RANK 1/8, then
  promotes).
- 4 promotion piece types (KNIGHT, BISHOP, ROOK, QUEEN).

**Test:** position 4 perft 4 / 5. Also dedicated promotion fenpaths.

**Decision gate:** position 4 perft 5 = 15,833,292 (known good).

### Phase 4 — En passant (1 day)

Add EP capture branch + EP target tracking.

**Steps:**
- `makeIncr<chance, PAWN>` with `flags == EP_CAPTURE`: clear the captured
  pawn at `dst ± 8`.
- `makeIncr<chance, PAWN>` with `flags == DOUBLE_PAWN_PUSH`: set
  `s.gs.enPassent = (move.from & 7) + 1`.
- All other moves: `s.gs.enPassent = 0`.
- `unmakeIncr` restores `gs.raw` (which includes EP byte).

**Test:** kiwipete (has EP) perft 4 = 4,085,603.

**Decision gate:** kiwipete perft 4 matches.

### Phase 5 — Castling (1 day)

Add the 4 castle cases (W-KS, W-QS, B-KS, B-QS) inside
`makeIncr<chance, KING>`.

**Steps:**
- Detect castle via `flags`.
- Move both king and rook explicitly.
- Clear that side's castle rights.
- Castle rights also need to clear on:
  - Any KING move (both rights cleared).
  - ROOK move FROM A1/H1/A8/H8 (corresponding right cleared).
  - Any piece CAPTURE on A1/H1/A8/H8 (opponent's corresponding right
    cleared).

**Test:** kiwipete (has castling) perft 5 = 193,690,690.

**Decision gate:** kiwipete perft 5 matches AND startpos perft 6 matches.

### Phase 6 — Wire up enumerateMoves with incremental Processor (2 days)

Modify `enumerateMoves<chance, Processor>` so the Processor's emit gets
called with `BoardState &s` instead of `QuadBitBoard *pos, GameState *gs`.
The Processor calls `makeIncr` in `emit`, recurses, calls `unmakeIncr` on
return.

**Critical:** the recursion must preserve `BoardState` correctness.
Every make MUST have a matching unmake. Easy to bug.

**Defensive testing:** assert-mode build that verifies `BoardState`
matches a freshly-derived `fromQBB` after each `unmake`. Crash on
mismatch with debug info.

**Decision gate:** kiwipete perft 5 = 193,690,690 in BOTH normal mode
AND assert mode.

### Phase 7 — Wire up `countTwoLevelSubtree` + `countMovesPair` for incremental (3 days)

Replace `FgmcCount2Processor` with `FgmcCount2ProcessorIncr` that:
- Stores `BoardState parent` (not just QBB).
- emit() applies `makeIncr`, stores child as `BoardState` not as `(QBB, GS)`.
- flush() reads children directly — no more derive.

Replace `countMovesPair` + `countMovesPairCached` with `countMovesPairIncr`
that reads pre-derived state directly from each child `BoardState`.

**Decision gate:** kiwipete perft 5 = 193,690,690. Performance: at minimum
no regression vs E7 (~75.3 ms); ideally ≤ 70 ms.

### Phase 8 — Eliminate redundant derives from leaf body (1-2 days)

In `countMovesFromDerived`, the body still computes some derived locals
(e.g., `myPawns = allPawns & myPieces`, `enemyBishops = bishopQueens &
enemyPieces`). Some of these can be moved into `BoardState` itself
(myPawns, myKnights etc., maintained per-color).

**Trade-off:** more BoardState fields = bigger struct (cache pressure).
Pick only the fields the leaf body uses most: maybe `whitePawns`,
`blackPawns`, `whiteKnights`, `blackKnights`. Adds 32 bytes (one cache
line) per BoardState.

**Decision gate:** ≤ 65 ms perft 5.

### Phase 9 — Compose with E7 (cached non-slider attacks) (1 day)

E7's cache is orthogonal to make/unmake — both can coexist. Re-implement
E7 on top of BoardState.

**Decision gate:** ≤ 60 ms perft 5.

### Phase 10 — PGO retrain + tuning (1 day)

Full retrain. The hot path has shifted; PGO data is stale.

**Decision gate:** final perft 5 ≤ 55 ms.

---

## 4. Validation strategy

### 4.1 Correctness — at EVERY phase

Same test positions as Plan A:
| Position | FEN | Depth | Expected |
|---|---|---|---|
| Kiwipete (pos 2) | `r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -` | 5 | 193,690,690 |
| Kiwipete | (same) | 6 | 8,031,647,685 |
| Startpos | `rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1` | 6 | 119,060,324 |
| Position 3 (sliders only) | `8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -` | 7 | 178,633,661 |
| Position 4 (promotions) | `r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1` | 5 | 15,833,292 |
| Position 5 (Edwards) | `rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8` | 5 | 89,941,194 |

**All 5 must pass after each phase.**

### 4.2 Defensive assertion mode

Add `#define BOARDSTATE_VERIFY` build flag that, after every `unmakeIncr`,
re-derives `BoardState` from QBB and asserts equality. This catches
make/unmake imbalance bugs early.

```cpp
template <uint8 chance, uint8 piece>
void unmakeIncr(BoardState &s, CMove move, UndoInfo undo) {
    // ... real unmake logic ...
#ifdef BOARDSTATE_VERIFY
    BoardState fresh = BoardState::fromQBB(s.qbb, s.gs);
    assert(s.allPieces    == fresh.allPieces);
    assert(s.whitePieces  == fresh.whitePieces);
    assert(s.blackPieces  == fresh.blackPieces);
    assert(s.allPawns     == fresh.allPawns);
    assert(s.knights      == fresh.knights);
    assert(s.bishopQueens == fresh.bishopQueens);
    assert(s.rookQueens   == fresh.rookQueens);
    assert(s.kings        == fresh.kings);
    assert(s.gs.raw       == fresh.gs.raw);
#endif
}
```

Run with the verify flag enabled during all of Phase 1-7. Disable for
perf benches in Phase 8+.

### 4.3 Per-phase pass criteria

| Phase | Pass criterion |
|---|---|
| 0 | BoardState round-trip test passes |
| 1 | All 12 make/unmake round-trip cleanly on hand-built positions |
| 2 | Pos 4 perft 2, 3, 4 match known-good |
| 3 | Pos 4 perft 5 = 15,833,292 |
| 4 | Kiwipete perft 4 = 4,085,603 |
| 5 | Kiwipete perft 5 = 193,690,690 AND startpos perft 6 = 119,060,324 |
| 6 | All 5 test positions match. With BOARDSTATE_VERIFY enabled. |
| 7 | Counts ✓, perft 5 ≤ 75 ms (no regression vs E7) |
| 8 | Counts ✓, perft 5 ≤ 65 ms |
| 9 | Counts ✓, perft 5 ≤ 60 ms |
| 10 | Counts ✓, perft 5 ≤ 55 ms |

---

## 5. Risks and mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Make/unmake imbalance bugs | **High** | Critical (wrong counts) | BOARDSTATE_VERIFY assertion mode during phases 1-7 |
| Capture victim probe is slow | Medium | Medium (perf) | Order probes by frequency (PAWN first); branchless via masked OR if profile shows it hot |
| BoardState size pressures L1d | Low-Medium | Medium | Phase 8 tunes which fields are added; keep struct ≤ 128 bytes |
| Special cases (EP + promotion + capture combined) buggy | Medium | High | Dedicate Phase 4 + tests; pos 4 has all of these |
| Recursion structure makes make/unmake awkward to wire | Medium | High | Phase 6 is the wiring; do it AFTER phases 1-5 are solid in isolation |
| Performance regresses despite eliminating derives (e.g., cache pressure from larger struct) | Medium | Critical | Phase 7 decision gate catches; if neutral, abandon plan or revert to "carry derived" only without make/unmake |
| MSVC's PGO can't optimize the new structure as well | Medium | Medium | Train on multiple positions in Phase 10 |
| Stack frame growth due to UndoInfo array in Processor | Low | Low | UndoInfo is 4 bytes; 256 of them = 1 KB |
| GameState `gs.raw` updates have hidden invariants | Medium | High | Audit `updateCastleFlag` and EP target setting carefully in Phase 5 |

---

## 6. Files that will change

| File | Change |
|---|---|
| `chess.h` | Add `BoardState` struct, `UndoInfo` struct, `BoardState::fromQBB` / `toQBB` helpers. |
| `MoveGeneratorBitboard.h` | Major: add `makeIncr<chance, piece>`, `unmakeIncr<chance, piece>` for all 6 pieces × all special cases. Add `enumerateMovesIncr`, `countMovesIncr`, `countMovesFromDerivedIncr`, `FgmcCount2ProcessorIncr`, `countMovesPairIncr`. |
| `launcher.cpp` | Replace `perft_cpu` recursion to use `BoardState` + incremental path. Keep legacy QBB path for fallback if needed. |
| `perft.cpp` | Add `-bench-incr N` microbench; possibly extend `-bench-leaf` to test the incremental leaf. |
| `optimization_log.md` | Append all experiments. |
| `CMakeLists.txt` | Add `BOARDSTATE_VERIFY` cmake option that flips a `#define`. |

Estimated LOC change: **~1500-2000 lines new code, ~500 lines modified or
deleted** from the legacy QBB-only path (which can stay for `make` not
`makeIncr`).

---

## 7. Open questions to resolve at session start

1. **Should we keep QBB inside BoardState?** YES — needed for legacy code,
   useful for the FROM-square piece probe (could be done via bit scans
   of derived state but QBB makes it cheap). The 32 bytes are not
   bottleneck.
2. **Should `myPawns`, `myKnights` (per-color) be in BoardState too?**
   Phase 8 decides based on profile. Initial design has only the
   color-agnostic versions.
3. **What's the right unmakeIncr signature — pass `UndoInfo` by value (4
   bytes, fits in 1 reg) or by reference (slower)?** Value. UndoInfo is
   small enough.
4. **Should `castle rights` update logic be inline in `makeIncr` or in a
   helper?** Helper (existing `updateCastleFlag`). Keeps makeIncr focused.
5. **Can we share code with the existing `makeMoveT` for the QBB updates?**
   YES — `makeIncr` can call `makeMoveTFromParent` for the QBB half then
   add the derived-state delta. But that wastes the opportunity to fuse
   the QBB and derived updates. Decision: write a fresh combined version.
6. **Will the depth-2 buffered path still benefit from a buffer?** With
   make/unmake, no copy-make per child, so buffering becomes pointless.
   Phase 7 design: drop the buffer for depth-2 in the incremental path;
   just process each emit immediately. Saves stack frame.
7. **What about the depth-3 and depth-4 paths?** Same story: drop
   FgmcCount3Processor; use direct recursion with make/unmake.

---

## 8. What success looks like

| Final perft 5 | Verdict |
|---|---|
| ≤ 50 ms | **Stretch hit.** |
| 50-55 ms | **Target hit.** Plan worked. |
| 55-60 ms | **Substantial.** Either Phase 8 stretch didn't materialize or PGO was hard to tune. Accept. |
| 60-67 ms | **Real but partial.** Make/unmake worked but the leaf-body wins are limited. |
| 67-75 ms | **Marginal.** Some derive savings but not enough to justify the engineering. |
| > 75 ms or counts wrong | **Failed.** Revert. Document what broke for future tries. |

---

## 9. Estimated effort

| Phase | Days | Cumulative |
|---|---|---|
| 0 (BoardState scaffolding) | 1 | 1 |
| 1 (make/unmake skeleton — 6 piece types, no specials) | 2-3 | 3-4 |
| 2 (captures) | 2 | 5-6 |
| 3 (promotion) | 1-2 | 6-8 |
| 4 (EP) | 1 | 7-9 |
| 5 (castling) | 1 | 8-10 |
| 6 (wire enumerateMoves) | 2 | 10-12 |
| 7 (wire countTwoLevelSubtree + countMovesPair) | 3 | 13-15 |
| 8 (additional BoardState fields per profile) | 1-2 | 14-17 |
| 9 (compose with E7) | 1 | 15-18 |
| 10 (PGO retrain + tuning) | 1 | 16-19 |

**Total: ~3-4 work-weeks.** The first 5 phases (~10 days) are pure
correctness work with no perf payoff — gates exist but performance isn't
measured until Phase 7. Risk concentration is high in the first 10 days.

If Phase 1-5 take longer than budgeted (likely on first attempt), this
plan stretches to 5-6 weeks. The 4-byte UndoInfo discipline + assertion
mode are critical to bounded debugging time.

---

## 10. Comparison with Plan A (AVX2 SIMD)

| Dimension | Plan A (AVX2 SIMD) | Plan B (Make/Unmake) |
|---|---|---|
| Target perft 5 | ≤ 60 ms (-20 %) | ≤ 55 ms (-27 %) |
| Engineering effort | ~3 weeks | ~3-4 weeks |
| Risk profile | Cliff measurement upfront; if cliff ≤ 30c, low-medium risk | Make/unmake correctness; high risk concentrated in first 2 weeks |
| Reversibility | Easy — drop the AVX2 TU | Hard — fundamental architecture change |
| Composes with E7? | Yes (E7 is the slider-fast cache the AVX2 TU consumes) | Yes (Phase 9 explicitly composes) |
| Composes with Plan C (incremental attacks)? | Independently — both can be active | Yes, naturally — make/unmake is the substrate for incremental attacks |
| Generalizability | AVX2 only; ARM64 needs separate work | All architectures benefit |
| Win source | Parallelism across siblings | Eliminate per-leaf derive work |

**If you can only do one:** Plan B has a deeper architectural payoff and
generalizes to ARM64 (Snapdragon X), but it carries more correctness risk.
Plan A is more circumscribed and faster to validate (the Phase 0-2 spike
tells you in a week whether it'll work).

**If you do both:** B first, then A on top. The make/unmake substrate
makes the AVX2 TU simpler (it consumes BoardState arrays, not QBB+derive).

---

## 11. Pre-session checklist

- [ ] Read `optimization_log.md`, especially E1 (where pre-derived state
      regressed by 10 %). Understand why per-emit stores regressed — Plan B
      avoids this because there are no per-emit stores; make/unmake mutates
      ONE board, not buffers.
- [ ] Verify E7 baseline: perft 5 = 75.20 ms min / 75.30 ms median.
- [ ] Confirm counts at baseline for all 6 test positions.
- [ ] Open `MoveGeneratorBitboard.h` at line 2120 (makeMoveTFromParent),
      line 1672 (countMovesFromDerived), line 1418 (countTwoLevelSubtree).
- [ ] Decide on `BOARDSTATE_VERIFY` build mode and ensure CMake plumbing
      is ready.

When all check, begin Phase 0.

---

*Plan written 2026-05-15. Building on E7's 3.4 % win and the 12-experiment
exploration that established the scalar wall. Make/unmake is the
historically-proven path used by Stockfish-class engines; the effort is
large but the win is bounded by well-understood mechanics.*
