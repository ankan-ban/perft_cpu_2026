// Plan A — AVX2 isolated leaf-batch TU.
//
// This TU is compiled with /arch:AVX2 in isolation so that future SIMD work
// (4-lane derive, 4-lane non-slider attacks, gather LUT loads, etc.) can use
// the YMM register file.
//
// CRITICAL DESIGN RULE: this TU MUST NOT instantiate any template from
// MoveGeneratorBitboard.h. Doing so causes LTCG to merge the template COMDAT
// under /arch:AVX2 codegen, which triggers the documented -57% global-AVX2
// cliff (+~50 ms on kiwipete perft 5 — measured 2026-05-15). Instead, all
// scalar fallback work lives in a default-arch helper TU and is reached via
// the extern "C" forwarder ABI declared in chess.h.
//
// Phase 1 (current): the entry points are STUBS that just forward to the
// scalar pair-wise leaf loop in launcher.cpp. This validates the wiring,
// build setup, and decision gate (counts ✓, no significant regression vs
// the pre-Phase-1 baseline). Phases 2+ replace the bodies with real 4-lane
// SoA SIMD work.

#include "chess.h"
#include <stdint.h>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define PERFT_HAS_AVX2 1
#else
#define PERFT_HAS_AVX2 0
#endif

// Helpers in launcher.cpp (default-arch TU, NOT /arch:AVX2). They do the
// scalar pair-wise leaf loop using MoveGeneratorBitboard's templates.
extern "C" {
    uint64 leafFastLoopScalar_white(QuadBitBoard *bufCp, GameState *bufGs,
                                    int n, uint64 cachedNonSliderAtk) noexcept;
    uint64 leafFastLoopScalar_black(QuadBitBoard *bufCp, GameState *bufGs,
                                    int n, uint64 cachedNonSliderAtk) noexcept;
    uint64 leafSlowLoopScalar_white(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept;
    uint64 leafSlowLoopScalar_black(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept;
    // Phase 4 slow-path: scalar leaf body that consumes pre-computed enemy
    // non-slider attacks (one per child). Defined in launcher.cpp.
    uint64 leafSlowLoopScalarWithAtk4_white(QuadBitBoard *bufCp, GameState *bufGs,
                                            uint64 *enemyNonSliderAtk4, int n) noexcept;
    uint64 leafSlowLoopScalarWithAtk4_black(QuadBitBoard *bufCp, GameState *bufGs,
                                            uint64 *enemyNonSliderAtk4, int n) noexcept;
}

#if !PERFT_HAS_AVX2
// Non-x86 (ARM64, ARM64EC): no AVX2 — entry points forward straight to the
// scalar helpers. The Phase 4 SIMD non-slider precompute is x86-only, so the
// slow path here goes through the pre-Phase-4 scalar slow loop.
extern "C" __declspec(noinline)
uint64 countMovesBulkAVX2_fast_white(QuadBitBoard *bufCp, GameState *bufGs,
                                     int n, uint64 cachedNonSliderAtk) noexcept
{ return leafFastLoopScalar_white(bufCp, bufGs, n, cachedNonSliderAtk); }

extern "C" __declspec(noinline)
uint64 countMovesBulkAVX2_fast_black(QuadBitBoard *bufCp, GameState *bufGs,
                                     int n, uint64 cachedNonSliderAtk) noexcept
{ return leafFastLoopScalar_black(bufCp, bufGs, n, cachedNonSliderAtk); }

extern "C" __declspec(noinline)
uint64 countMovesBulkAVX2_slow_white(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept
{ return leafSlowLoopScalar_white(bufCp, bufGs, n); }

extern "C" __declspec(noinline)
uint64 countMovesBulkAVX2_slow_black(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept
{ return leafSlowLoopScalar_black(bufCp, bufGs, n); }
#else

// Phase 4: 4-lane SIMD computation of enemy non-slider attacks (pawn | knight
// | king) for 4 child boards in parallel. Each lane = one child.
//
// Inputs: 4 QBB pointers (the children).
// Output: an array of 4 uint64, one per child.
//
// chance = the leaf's side-to-move (NOT the enemy). The enemy is `!chance`.
//
// This function is in the AVX2 TU so it can use 256-bit YMM ops freely.
// It does NOT include MoveGeneratorBitboard.h — all the bitboard derivations
// and shift patterns are inlined here so no templates from that header are
// instantiated in this TU (which would otherwise trigger the global-AVX2
// cliff, see Phase 1 commentary above).
template <uint8 chance>
static __forceinline void enemyNonSliderAtk4_impl(
    const QuadBitBoard *p0, const QuadBitBoard *p1,
    const QuadBitBoard *p2, const QuadBitBoard *p3,
    uint64 *out4)
{
    // Pack each plane across the 4 children. The order in _mm256_set_epi64x
    // is (lane3, lane2, lane1, lane0), so lane 0 maps to child 0 etc.
    __m256i bb0 = _mm256_set_epi64x((long long)p3->bb[0], (long long)p2->bb[0],
                                    (long long)p1->bb[0], (long long)p0->bb[0]);
    __m256i bb1 = _mm256_set_epi64x((long long)p3->bb[1], (long long)p2->bb[1],
                                    (long long)p1->bb[1], (long long)p0->bb[1]);
    __m256i bb2 = _mm256_set_epi64x((long long)p3->bb[2], (long long)p2->bb[2],
                                    (long long)p1->bb[2], (long long)p0->bb[2]);
    __m256i bb3 = _mm256_set_epi64x((long long)p3->bb[3], (long long)p2->bb[3],
                                    (long long)p1->bb[3], (long long)p0->bb[3]);

    // Derive.
    __m256i allPieces   = _mm256_or_si256(_mm256_or_si256(bb1, bb2), bb3);
    __m256i blackPieces = bb0;
    __m256i enemyPieces = (chance == WHITE) ? blackPieces
                                            : _mm256_andnot_si256(blackPieces, allPieces);
    __m256i allPawns    = _mm256_andnot_si256(bb3, _mm256_andnot_si256(bb2, bb1));
    __m256i knights     = _mm256_andnot_si256(bb3, _mm256_andnot_si256(bb1, bb2));
    __m256i kings       = _mm256_andnot_si256(bb1, _mm256_and_si256(bb2, bb3));

    __m256i enemyPawns   = _mm256_and_si256(allPawns, enemyPieces);
    __m256i enemyKnights = _mm256_and_si256(knights, enemyPieces);
    __m256i enemyKing    = _mm256_and_si256(kings,   enemyPieces);

    // Constants used in shift+mask patterns.
    __m256i fileA  = _mm256_set1_epi64x((long long)0x0101010101010101ULL);
    __m256i fileH  = _mm256_set1_epi64x((long long)0x8080808080808080ULL);
    __m256i notFA  = _mm256_set1_epi64x((long long)~0x0101010101010101ULL);
    __m256i notFH  = _mm256_set1_epi64x((long long)~0x8080808080808080ULL);
    (void)fileA; (void)fileH;  // andnot uses notFA/notFH below; keep the names for clarity

    // Pawn attacks. enemy = !chance. White pawns attack NE/NW (up the board),
    // black pawns attack SE/SW (down the board). The shifts mirror the scalar
    // helpers (eastOne / westOne masking).
    __m256i pawnAtk;
    if constexpr (chance == BLACK) {
        // enemy = WHITE: pawns shift NE (<<9 & ~FILEA) and NW (<<7 & ~FILEH).
        __m256i ne = _mm256_and_si256(notFA, _mm256_slli_epi64(enemyPawns, 9));
        __m256i nw = _mm256_and_si256(notFH, _mm256_slli_epi64(enemyPawns, 7));
        pawnAtk = _mm256_or_si256(ne, nw);
    } else {
        // enemy = BLACK: pawns shift SE (>>7 & ~FILEA) and SW (>>9 & ~FILEH).
        __m256i se = _mm256_and_si256(notFA, _mm256_srli_epi64(enemyPawns, 7));
        __m256i sw = _mm256_and_si256(notFH, _mm256_srli_epi64(enemyPawns, 9));
        pawnAtk = _mm256_or_si256(se, sw);
    }

    // Knight attacks (bulk shift formula — mirrors scalar `knightAttacks`).
    __m256i m7f = _mm256_set1_epi64x((long long)0x7f7f7f7f7f7f7f7fULL);
    __m256i m3f = _mm256_set1_epi64x((long long)0x3f3f3f3f3f3f3f3fULL);
    __m256i mfe = _mm256_set1_epi64x((long long)0xfefefefefefefefeULL);
    __m256i mfc = _mm256_set1_epi64x((long long)0xfcfcfcfcfcfcfcfcULL);
    __m256i l1 = _mm256_and_si256(_mm256_srli_epi64(enemyKnights, 1), m7f);
    __m256i l2 = _mm256_and_si256(_mm256_srli_epi64(enemyKnights, 2), m3f);
    __m256i r1 = _mm256_and_si256(_mm256_slli_epi64(enemyKnights, 1), mfe);
    __m256i r2 = _mm256_and_si256(_mm256_slli_epi64(enemyKnights, 2), mfc);
    __m256i h1 = _mm256_or_si256(l1, r1);
    __m256i h2 = _mm256_or_si256(l2, r2);
    __m256i knightAtk = _mm256_or_si256(
        _mm256_or_si256(_mm256_slli_epi64(h1, 16), _mm256_srli_epi64(h1, 16)),
        _mm256_or_si256(_mm256_slli_epi64(h2,  8), _mm256_srli_epi64(h2,  8)));

    // King attacks (mirrors scalar `kingAttacks`).
    // attacks  = eastOne(K) | westOne(K)
    // kingSet |= attacks
    // attacks |= northOne(kingSet) | southOne(kingSet)
    __m256i kE  = _mm256_and_si256(notFA, _mm256_slli_epi64(enemyKing, 1));
    __m256i kW  = _mm256_and_si256(notFH, _mm256_srli_epi64(enemyKing, 1));
    __m256i kEW = _mm256_or_si256(kE, kW);
    __m256i kingExp = _mm256_or_si256(enemyKing, kEW);
    __m256i kingAtk = _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi64(kingExp, 8),
                                                       _mm256_srli_epi64(kingExp, 8)),
                                      kEW);

    __m256i atk = _mm256_or_si256(_mm256_or_si256(pawnAtk, knightAtk), kingAtk);
    _mm256_storeu_si256((__m256i*)out4, atk);
}

extern "C" {

__declspec(noinline)
uint64 countMovesBulkAVX2_fast_white(QuadBitBoard *bufCp, GameState *bufGs,
                                     int n, uint64 cachedNonSliderAtk) noexcept
{
    return leafFastLoopScalar_white(bufCp, bufGs, n, cachedNonSliderAtk);
}

__declspec(noinline)
uint64 countMovesBulkAVX2_fast_black(QuadBitBoard *bufCp, GameState *bufGs,
                                     int n, uint64 cachedNonSliderAtk) noexcept
{
    return leafFastLoopScalar_black(bufCp, bufGs, n, cachedNonSliderAtk);
}

// Slow-path slow-buffer entry point. Phase 4: process 4 children at a time,
// SIMD-computing their enemy non-slider attacks in this AVX2 TU and handing
// the precomputed map to a scalar helper for the slider+dispatch work.
// Tail (1-3 leaves) falls back to the pre-Phase-4 scalar slow loop.
__declspec(noinline)
uint64 countMovesBulkAVX2_slow_white(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept
{
    uint64 s = 0;
    int i = 0;
    if (n >= 4) {
        alignas(32) uint64 atk4[4];
        for (; i + 3 < n; i += 4) {
            enemyNonSliderAtk4_impl<WHITE>(&bufCp[i+0], &bufCp[i+1],
                                            &bufCp[i+2], &bufCp[i+3], atk4);
            s += leafSlowLoopScalarWithAtk4_white(&bufCp[i], &bufGs[i], atk4, 4);
        }
    }
    if (i < n)
        s += leafSlowLoopScalar_white(&bufCp[i], &bufGs[i], n - i);
    return s;
}

__declspec(noinline)
uint64 countMovesBulkAVX2_slow_black(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept
{
    uint64 s = 0;
    int i = 0;
    if (n >= 4) {
        alignas(32) uint64 atk4[4];
        for (; i + 3 < n; i += 4) {
            enemyNonSliderAtk4_impl<BLACK>(&bufCp[i+0], &bufCp[i+1],
                                            &bufCp[i+2], &bufCp[i+3], atk4);
            s += leafSlowLoopScalarWithAtk4_black(&bufCp[i], &bufGs[i], atk4, 4);
        }
    }
    if (i < n)
        s += leafSlowLoopScalar_black(&bufCp[i], &bufGs[i], n - i);
    return s;
}

}  // extern "C"
#endif  // PERFT_HAS_AVX2
