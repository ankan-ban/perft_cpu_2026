#pragma once

#include "chess.h"

#ifdef _MSC_VER
    #include <intrin.h>
    #ifndef CPU_FORCE_INLINE
        #define CPU_FORCE_INLINE __forceinline
    #endif
#else
    #ifndef CPU_FORCE_INLINE
        #define CPU_FORCE_INLINE inline
    #endif
#endif

#if defined(_M_ARM64) || defined(_M_ARM64EC) || defined(__ARM_NEON)
    #include <arm_neon.h>
#endif

#include <time.h>


// bit board constants
#define C64(constantU64) constantU64##ULL

// valid locations for pawns
#ifndef RANKS2TO7
#define RANKS2TO7 C64(0x00FFFFFFFFFFFF00)
#endif

#define RANK1     C64(0x00000000000000FF)
#define RANK2     C64(0x000000000000FF00)
#define RANK3     C64(0x0000000000FF0000)
#define RANK4     C64(0x00000000FF000000)
#define RANK5     C64(0x000000FF00000000)
#define RANK6     C64(0x0000FF0000000000)
#define RANK7     C64(0x00FF000000000000)
#define RANK8     C64(0xFF00000000000000)

#define FILEA     C64(0x0101010101010101)
#define FILEB     C64(0x0202020202020202)
#define FILEC     C64(0x0404040404040404)
#define FILED     C64(0x0808080808080808)
#define FILEE     C64(0x1010101010101010)
#define FILEF     C64(0x2020202020202020)
#define FILEG     C64(0x4040404040404040)
#define FILEH     C64(0x8080808080808080)

#define DIAGONAL_A1H8   C64(0x8040201008040201)
#define DIAGONAL_A8H1   C64(0x0102040810204080)

#define CENTRAL_SQUARES C64(0x007E7E7E7E7E7E00)

// used for castling checks
#define F1G1      C64(0x60)
#define C1D1      C64(0x0C)
#define B1D1      C64(0x0E)

// used for castling checks
#define F8G8      C64(0x6000000000000000)
#define C8D8      C64(0x0C00000000000000)
#define B8D8      C64(0x0E00000000000000)

// used to update castle flags
#define WHITE_KING_SIDE_ROOK   C64(0x0000000000000080)
#define WHITE_QUEEN_SIDE_ROOK  C64(0x0000000000000001)
#define BLACK_KING_SIDE_ROOK   C64(0x8000000000000000)
#define BLACK_QUEEN_SIDE_ROOK  C64(0x0100000000000000)
    

#define ALLSET    C64(0xFFFFFFFFFFFFFFFF)
#define BB_EMPTY  C64(0x0)

CPU_FORCE_INLINE uint8 popCount(uint64 x)
{
#if defined(__GNUC__) || defined(__clang__)
    // GCC/Clang map this directly to POPCNT (x86) or CNT.8B + ADDV (ARMv8).
    return (uint8)__builtin_popcountll(x);
#elif defined(_MSC_VER)
    #if defined(_M_X64) || defined(_M_IX86)
        return (uint8)__popcnt64(x);                    // POPCNT instruction.
    #elif defined(_M_ARM64) || defined(_M_ARM64EC) || defined(_M_ARM)
        return (uint8)_CountOneBits64(x);               // ARM CNT instruction.
    #else
        #error "Unsupported MSVC target architecture for popCount"
    #endif
#else
    #error "Unsupported compiler for popCount — add a hardware intrinsic here"
#endif
}


// return the index of first set LSB
CPU_FORCE_INLINE uint8 bitScan(uint64 x)
{
#if defined(__GNUC__) || defined(__clang__)
    // BSF on x86; RBIT + CLZ on ARMv8 — both hardware paths.
    assert(x != 0);
    return (uint8)__builtin_ctzll(x);
#elif defined(_MSC_VER)
    // _BitScanForward64 lowers to BSF on x64 and RBIT+CLZ on ARM64.
    unsigned long index;
    assert(x != 0);
    _BitScanForward64(&index, x);
    return (uint8)index;
#else
    #error "Unsupported compiler for bitScan — add a hardware intrinsic here"
#endif
}


// CPU copy of all the below global variables are defined in GlobalVars.cpp
// bit mask containing squares between two given squares
extern uint64 Between[64][64];

// bit mask containing squares in the same 'line' as two given squares
extern uint64 Line[64][64];

// squares a piece can attack in an empty board
extern uint64 RookAttacks    [64];
extern uint64 BishopAttacks  [64];
extern uint64 QueenAttacks   [64];
extern uint64 KingAttacks    [64];
extern uint64 KnightAttacks  [64];
// King-zone slider prefilter LUTs: for king square k, the set of squares from
// which a bishop/rook could reach k's 3x3 neighbourhood on an EMPTY board (a
// superset of any occupied board). Castle corridors are folded into E1/E8.
extern uint64 KingZoneDiag   [64];
extern uint64 KingZoneOrtho  [64];
extern uint64 pawnAttacks[2] [64];

// magic lookup tables
#define ROOK_MAGIC_BITS    12
#define BISHOP_MAGIC_BITS  9

// same as RookAttacks and BishopAttacks, but corner bits masked off
extern uint64 RookAttacksMasked   [64];
extern uint64 BishopAttacksMasked [64];

// fancy magic tables
extern uint64 fancy_magic_lookup_table[97264];
extern FancyMagicEntry bishop_magics_fancy[64];
extern FancyMagicEntry rook_magics_fancy[64];

// Packed CPU magic entry: (factor, table_ptr) — 16 bytes, single LDP load.
// `table_ptr` is precomputed as &fancy_magic_lookup_table[fancy_entry.position],
// so the per-call magic lookup becomes:
//    e = bishop_magics_fast[sq];          (1 LDP)
//    idx = (occ * e.factor) >> (64 - BITS);
//    result = e.table[idx];               (1 LDR)
// vs the current path which separately loads factor + position + adds them.
// (struct defined in chess.h)
extern FastMagicEntry bishop_magics_fast[64];
extern FastMagicEntry rook_magics_fast[64];

// E18 — same as above but split. Avoids the XMM round-trip MSVC emits when
// loading a packed FastMagicEntry into GPRs (see optimization_log.md E18).
extern uint64  bishop_magic_factors[64];
extern uint64 *bishop_magic_tables [64];
extern uint64  rook_magic_factors  [64];
extern uint64 *rook_magic_tables   [64];

uint64 findRookMagicForSquare  (int square, uint64 magicAttackTable[], uint64 magic = 0, uint64 *uniqueAttackTable = NULL, uint8 *byteIndices = NULL, int *numUniqueAttacks = 0);
uint64 findBishopMagicForSquare(int square, uint64 magicAttackTable[], uint64 magic = 0, uint64 *uniqueAttackTable = NULL, uint8 *byteIndices = NULL, int *numUniqueAttacks = 0);

// Compute EP target square bitboard from GameState enPassent field and color.
template<uint8 chance>
CPU_FORCE_INLINE uint64 getEpTarget(uint8 epField)
{
    if (!epField) return 0;
    return (chance == BLACK) ? (BIT(epField - 1) << (8 * 2))
                             : (BIT(epField - 1) << (8 * 5));
}

CPU_FORCE_INLINE uint64 sqsInBetweenLUT(uint8 sq1, uint8 sq2) { return Between[sq1][sq2]; }
CPU_FORCE_INLINE uint64 sqsInLineLUT   (uint8 sq1, uint8 sq2) { return Line   [sq1][sq2]; }
CPU_FORCE_INLINE uint64 sqKnightAttacks(uint8 sq)             { return KnightAttacks[sq]; }
CPU_FORCE_INLINE uint64 sqKingAttacks  (uint8 sq)             { return KingAttacks  [sq]; }
CPU_FORCE_INLINE uint64 sqRookAttacks  (uint8 sq)             { return RookAttacks  [sq]; }
CPU_FORCE_INLINE uint64 sqBishopAttacks(uint8 sq)             { return BishopAttacks[sq]; }
CPU_FORCE_INLINE uint64 sqBishopAttacksMasked(uint8 sq)       { return BishopAttacksMasked[sq]; }
CPU_FORCE_INLINE uint64 sqRookAttacksMasked  (uint8 sq)       { return RookAttacksMasked  [sq]; }
CPU_FORCE_INLINE uint64 sqKingZoneDiag (uint8 sq)             { return KingZoneDiag [sq]; }
CPU_FORCE_INLINE uint64 sqKingZoneOrtho(uint8 sq)             { return KingZoneOrtho[sq]; }

// Plan A AVX2 leaf-batch entry points (defined in leaf_batch_avx2.cpp,
// compiled /arch:AVX2 /GL- in isolation). Phase 1: stubs that loop the
// scalar pair-wise leaf counter. Phases 2+ replace with SoA SIMD bodies.
extern "C" {
    uint64 countMovesBulkAVX2_fast_white(QuadBitBoard *bufCp, GameState *bufGs,
                                         int n, uint64 cachedNonSliderAtk) noexcept;
    uint64 countMovesBulkAVX2_fast_black(QuadBitBoard *bufCp, GameState *bufGs,
                                         int n, uint64 cachedNonSliderAtk) noexcept;
    uint64 countMovesBulkAVX2_slow_white(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept;
    uint64 countMovesBulkAVX2_slow_black(QuadBitBoard *bufCp, GameState *bufGs, int n) noexcept;
}

class MoveGeneratorBitboard
{
public:

    // move the bits in the bitboard one square in the required direction

    CPU_FORCE_INLINE static uint64 northOne(uint64 x)
    {
        return x << 8;
    }

    CPU_FORCE_INLINE static uint64 southOne(uint64 x)
    {
        return x >> 8;
    }

    CPU_FORCE_INLINE static uint64 eastOne(uint64 x)
    {
        return (x << 1) & (~FILEA);
    }

    CPU_FORCE_INLINE static uint64 westOne(uint64 x)
    {
        return (x >> 1) & (~FILEH);
    }

    CPU_FORCE_INLINE static uint64 northEastOne(uint64 x)
    {
        return (x << 9) & (~FILEA);
    }

    CPU_FORCE_INLINE static uint64 northWestOne(uint64 x)
    {
        return (x << 7) & (~FILEH);
    }

    CPU_FORCE_INLINE static uint64 southEastOne(uint64 x)
    {
        return (x >> 7) & (~FILEA);
    }

    CPU_FORCE_INLINE static uint64 southWestOne(uint64 x)
    {
        return (x >> 9) & (~FILEH);
    }


    // fill the board in the given direction
    // taken from http://chessprogramming.wikispaces.com/


    // gen - generator  : starting positions
    // pro - propogator : empty squares / squares not of current side

    // uses kogge-stone algorithm

    CPU_FORCE_INLINE static uint64 northFill(uint64 gen, uint64 pro)
    {
        gen |= (gen << 8) & pro;
        pro &= (pro << 8);
        gen |= (gen << 16) & pro;
        pro &= (pro << 16);
        gen |= (gen << 32) & pro;

        return gen;
    }

    CPU_FORCE_INLINE static uint64 southFill(uint64 gen, uint64 pro)
    {
        gen |= (gen >> 8) & pro;
        pro &= (pro >> 8);
        gen |= (gen >> 16) & pro;
        pro &= (pro >> 16);
        gen |= (gen >> 32) & pro;

        return gen;
    }

    CPU_FORCE_INLINE static uint64 eastFill(uint64 gen, uint64 pro)
    {
        pro &= ~FILEA;

        gen |= (gen << 1) & pro;
        pro &= (pro << 1);
        gen |= (gen << 2) & pro;
        pro &= (pro << 2);
        gen |= (gen << 3) & pro;

        return gen;
    }
    
    CPU_FORCE_INLINE static uint64 westFill(uint64 gen, uint64 pro)
    {
        pro &= ~FILEH;

        gen |= (gen >> 1) & pro;
        pro &= (pro >> 1);
        gen |= (gen >> 2) & pro;
        pro &= (pro >> 2);
        gen |= (gen >> 3) & pro;

        return gen;
    }


    CPU_FORCE_INLINE static uint64 northEastFill(uint64 gen, uint64 pro)
    {
        pro &= ~FILEA;

        gen |= (gen << 9) & pro;
        pro &= (pro << 9);
        gen |= (gen << 18) & pro;
        pro &= (pro << 18);
        gen |= (gen << 36) & pro;

        return gen;
    }

    CPU_FORCE_INLINE static uint64 northWestFill(uint64 gen, uint64 pro)
    {
        pro &= ~FILEH;

        gen |= (gen << 7) & pro;
        pro &= (pro << 7);
        gen |= (gen << 14) & pro;
        pro &= (pro << 14);
        gen |= (gen << 28) & pro;

        return gen;
    }

    CPU_FORCE_INLINE static uint64 southEastFill(uint64 gen, uint64 pro)
    {
        pro &= ~FILEA;

        gen |= (gen >> 7) & pro;
        pro &= (pro >> 7);
        gen |= (gen >> 14) & pro;
        pro &= (pro >> 14);
        gen |= (gen >> 28) & pro;

        return gen;
    }

    CPU_FORCE_INLINE static uint64 southWestFill(uint64 gen, uint64 pro)
    {
        pro &= ~FILEH;

        gen |= (gen >> 9) & pro;
        pro &= (pro >> 9);
        gen |= (gen >> 18) & pro;
        pro &= (pro >> 18);
        gen |= (gen >> 36) & pro;

        return gen;
    }


    // attacks in the given direction
    // need to OR with ~(pieces of side to move) to avoid killing own pieces

    CPU_FORCE_INLINE static uint64 northAttacks(uint64 gen, uint64 pro)
    {
        gen |= (gen << 8) & pro;
        pro &= (pro << 8);
        gen |= (gen << 16) & pro;
        pro &= (pro << 16);
        gen |= (gen << 32) & pro;

        return gen << 8;
    }

    CPU_FORCE_INLINE static uint64 southAttacks(uint64 gen, uint64 pro)
    {
        gen |= (gen >> 8) & pro;
        pro &= (pro >> 8);
        gen |= (gen >> 16) & pro;
        pro &= (pro >> 16);
        gen |= (gen >> 32) & pro;

        return gen >> 8;
    }

    CPU_FORCE_INLINE static uint64 eastAttacks(uint64 gen, uint64 pro)
    {
        pro &= ~FILEA;

        gen |= (gen << 1) & pro;
        pro &= (pro << 1);
        gen |= (gen << 2) & pro;
        pro &= (pro << 2);
        gen |= (gen << 3) & pro;

        return (gen << 1) & (~FILEA);
    }
    
    CPU_FORCE_INLINE static uint64 westAttacks(uint64 gen, uint64 pro)
    {
        pro &= ~FILEH;

        gen |= (gen >> 1) & pro;
        pro &= (pro >> 1);
        gen |= (gen >> 2) & pro;
        pro &= (pro >> 2);
        gen |= (gen >> 3) & pro;

        return (gen >> 1) & (~FILEH);
    }


    CPU_FORCE_INLINE static uint64 northEastAttacks(uint64 gen, uint64 pro)
    {
        pro &= ~FILEA;

        gen |= (gen << 9) & pro;
        pro &= (pro << 9);
        gen |= (gen << 18) & pro;
        pro &= (pro << 18);
        gen |= (gen << 36) & pro;

        return (gen << 9) & (~FILEA);
    }

    CPU_FORCE_INLINE static uint64 northWestAttacks(uint64 gen, uint64 pro)
    {
        pro &= ~FILEH;

        gen |= (gen << 7) & pro;
        pro &= (pro << 7);
        gen |= (gen << 14) & pro;
        pro &= (pro << 14);
        gen |= (gen << 28) & pro;

        return (gen << 7) & (~FILEH);
    }

    CPU_FORCE_INLINE static uint64 southEastAttacks(uint64 gen, uint64 pro)
    {
        pro &= ~FILEA;

        gen |= (gen >> 7) & pro;
        pro &= (pro >> 7);
        gen |= (gen >> 14) & pro;
        pro &= (pro >> 14);
        gen |= (gen >> 28) & pro;

        return (gen >> 7) & (~FILEA);
    }

    CPU_FORCE_INLINE static uint64 southWestAttacks(uint64 gen, uint64 pro)
    {
        pro &= ~FILEH;

        gen |= (gen >> 9) & pro;
        pro &= (pro >> 9);
        gen |= (gen >> 18) & pro;
        pro &= (pro >> 18);
        gen |= (gen >> 36) & pro;

        return (gen >> 9) & (~FILEH);
    }


    // attacks by pieces of given type
    // pro - empty squares

    CPU_FORCE_INLINE static uint64 bishopAttacksKoggeStone(uint64 bishops, uint64 pro)
    {
        return northEastAttacks(bishops, pro) |
               northWestAttacks(bishops, pro) |
               southEastAttacks(bishops, pro) |
               southWestAttacks(bishops, pro) ;
    }

    CPU_FORCE_INLINE static uint64 rookAttacksKoggeStone(uint64 rooks, uint64 pro)
    {
        return northAttacks(rooks, pro) |
               southAttacks(rooks, pro) |
               eastAttacks (rooks, pro) |
               westAttacks (rooks, pro) ;
    }


    // Magic-bitboard slider attack: 2 parallel scalar loads (factor + table_ptr)
    // + 1 indexed load. E18 — using split arrays so MSVC doesn't pack into XMM
    // and have to extract halves.
    CPU_FORCE_INLINE static uint64 bishopAttacks(uint64 bishop, uint64 pro)
    {
        uint8 square = bitScan(bishop);
        uint64 occ = (~pro) & sqBishopAttacksMasked(square);
        uint64 factor = bishop_magic_factors[square];
        uint64 *table = bishop_magic_tables [square];
        uint64 index = (factor * occ) >> (64 - BISHOP_MAGIC_BITS);
        return table[index];
    }

    CPU_FORCE_INLINE static uint64 rookAttacks(uint64 rook, uint64 pro)
    {
        uint8 square = bitScan(rook);
        uint64 occ = (~pro) & sqRookAttacksMasked(square);
        uint64 factor = rook_magic_factors[square];
        uint64 *table = rook_magic_tables [square];
        uint64 index = (factor * occ) >> (64 - ROOK_MAGIC_BITS);
        return table[index];
    }

    CPU_FORCE_INLINE static uint64 multiBishopAttacks(uint64 bishops, uint64 pro)
    {
        uint64 attacks = 0;
        while(bishops)
        {
            uint64 bishop = getOne(bishops);
            attacks |= bishopAttacks(bishop, pro);
            bishops ^= bishop;
        }

        return attacks;
    }

    CPU_FORCE_INLINE static uint64 multiRookAttacks(uint64 rooks, uint64 pro)
    {
        uint64 attacks = 0;
        while(rooks)
        {
            uint64 rook = getOne(rooks);
            attacks |= rookAttacks(rook, pro);
            rooks ^= rook;
        }

        return attacks;
    }

CPU_FORCE_INLINE static uint64 multiKnightAttacks(uint64 knights)
{
	uint64 attacks = 0;
	while(knights)
	{
		uint64 knight = getOne(knights);
		attacks |= sqKnightAttacks(bitScan(knight));
		knights ^= knight;
	}
	return attacks;
}


    CPU_FORCE_INLINE static uint64 kingAttacks(uint64 kingSet) 
    {
        uint64 attacks = eastOne(kingSet) | westOne(kingSet);
        kingSet       |= attacks;
        attacks       |= northOne(kingSet) | southOne(kingSet);
        return attacks;
    }

    // efficient knight attack generator
    // http://chessprogramming.wikispaces.com/Knight+Pattern
    CPU_FORCE_INLINE static uint64 knightAttacks(uint64 knights) {
        uint64 l1 = (knights >> 1) & C64(0x7f7f7f7f7f7f7f7f);
        uint64 l2 = (knights >> 2) & C64(0x3f3f3f3f3f3f3f3f);
        uint64 r1 = (knights << 1) & C64(0xfefefefefefefefe);
        uint64 r2 = (knights << 2) & C64(0xfcfcfcfcfcfcfcfc);
        uint64 h1 = l1 | r1;
        uint64 h2 = l2 | r2;
        return (h1<<16) | (h1>>16) | (h2<<8) | (h2>>8);
    }



    // gets one bit (the LSB) from a bitboard
    // returns a bitboard containing that bit
    CPU_FORCE_INLINE static uint64 getOne(uint64 x)
    {
        return x & (-x);
    }

    CPU_FORCE_INLINE static bool isMultiple(uint64 x)
    {
        return x ^ getOne(x);
    }

    CPU_FORCE_INLINE static bool isSingular(uint64 x)
    {
        return !isMultiple(x); 
    }


    // finds the squares in between the two given squares
    // taken from 
    // http://chessprogramming.wikispaces.com/Square+Attacked+By#Legality Test-In Between-Pure Calculation
    CPU_FORCE_INLINE static uint64 squaresInBetween(uint8 sq1, uint8 sq2)
    {
        const uint64 m1   = C64(0xFFFFFFFFFFFFFFFF);
        const uint64 a2a7 = C64(0x0001010101010100);
        const uint64 b2g7 = C64(0x0040201008040200);
        const uint64 h1b7 = C64(0x0002040810204080);
        uint64 btwn, line, rank, file;
     
        btwn  = (m1 << sq1) ^ (m1 << sq2);
        file  =   (sq2 & 7) - (sq1   & 7);
        rank  =  ((sq2 | 7) -  sq1) >> 3 ;
        line  =      (   (file  &  7) - 1) & a2a7; // a2a7 if same file
        line += 2 * ((   (rank  &  7) - 1) >> 58); // b1g1 if same rank
        line += (((rank - file) & 15) - 1) & b2g7; // b2g7 if same diagonal
        line += (((rank + file) & 15) - 1) & h1b7; // h1b7 if same antidiag
        line *= btwn & -btwn; // mul acts like shift by smaller square
        return line & btwn;   // return the bits on that line inbetween
    }

    // returns the 'line' containing all pieces in the same file/rank/diagonal or anti-diagonal containing sq1 and sq2
    CPU_FORCE_INLINE static uint64 squaresInLine(uint8 sq1, uint8 sq2)
    {
        // TODO: try to make it branchless?
        int fileDiff  =   (sq2 & 7) - (sq1 & 7);
        int rankDiff  =  ((sq2 | 7) -  sq1) >> 3 ;

        uint8 file = sq1 & 7;
        uint8 rank = sq1 >> 3;

        if (fileDiff == 0)  // same file
        {
            return FILEA << file;
        }
        if (rankDiff == 0)  // same rank
        {
            return RANK1 << (rank * 8);
        }
        if (fileDiff - rankDiff == 0)   // same diagonal (with slope equal to a1h8)
        {
            if (rank - file >= 0)
                return DIAGONAL_A1H8 << ((rank - file) * 8);
            else
                return DIAGONAL_A1H8 >> ((file - rank) * 8);
        }
        if (fileDiff + rankDiff == 0)  // same anti-diagonal (with slope equal to a8h1)
        {
            // for a8h1, rank + file = 7
            int shiftAmount = (rank + file - 7) * 8;
            if (shiftAmount >= 0)
                return DIAGONAL_A8H1 << shiftAmount;
            else
                return DIAGONAL_A8H1 >> (-shiftAmount);
        }

        // squares not on same line
        return 0;
    }


    CPU_FORCE_INLINE static uint64 sqsInBetween(uint8 sq1, uint8 sq2)
    {
        return sqsInBetweenLUT(sq1, sq2);
    }

    CPU_FORCE_INLINE static uint64 sqsInLine(uint8 sq1, uint8 sq2)
    {
        return sqsInLineLUT(sq1, sq2);
    }

    static void init();

    static CPU_FORCE_INLINE uint64 findPinnedPieces (uint64 myKing, uint64 myPieces, uint64 enemyBishops, uint64 enemyRooks, uint64 allPieces, uint8 kingIndex)
    {
        // E28
        __assume(myKing != 0);
        __assume(kingIndex < 64);
        // check for sliding attacks to the king's square

        // It doesn't matter if we process more attackers behind the first attackers
        // They will be taken care of when we check for no. of obstructing squares between king and the attacker
        uint64 b = sqBishopAttacks(kingIndex) & enemyBishops;
        uint64 r = sqRookAttacks  (kingIndex) & enemyRooks;

        uint64 attackers = b | r;

        // for every attacker we need to check if there is a single obstruction between
        // the attacker and the king, and if so - the obstructor is pinned
        uint64 pinned = BB_EMPTY;
        while (attackers)
        {
            uint64 attacker = getOne(attackers);
            uint8 attackerIndex = bitScan(attacker);

            uint64 squaresInBetween = sqsInBetween(attackerIndex, kingIndex);
            uint64 piecesInBetween = squaresInBetween & allPieces;
            if (isSingular(piecesInBetween))
                pinned |= piecesInBetween;

            attackers ^= attacker;
        }

        return pinned;
    }

    // Extended pin detector: returns pinned set AND the per-piece pin-direction line
    // (so the move-gen slider loop can AND with `sqsInLine` cheaply). Crucially, we
    // share a single attacker loop with findPinnedPieces — `b` (diagonal attackers)
    // get tagged in pinmaskD12, `r` (orthogonal) in pinmaskHV.
    CPU_FORCE_INLINE static uint64 findPinnedAndPinmask (
        uint64 /*myKing*/, uint64 myPieces, uint64 enemyBishops, uint64 enemyRooks,
        uint64 allPieces, uint8 kingIndex,
        uint64 &pinmaskD12, uint64 &pinmaskHV)
    {
        uint64 b = sqBishopAttacks(kingIndex) & enemyBishops;
        uint64 r = sqRookAttacks  (kingIndex) & enemyRooks;
        uint64 attackers = b | r;

        uint64 pinned = BB_EMPTY;
        pinmaskD12 = BB_EMPTY;
        pinmaskHV  = BB_EMPTY;

        while (attackers)
        {
            uint64 attacker = getOne(attackers);
            uint8  attackerSq = bitScan(attacker);
            uint64 between = sqsInBetween(attackerSq, kingIndex);
            uint64 piecesInBetween = between & allPieces;
            if (isSingular(piecesInBetween) && (piecesInBetween & myPieces))
            {
                pinned |= piecesInBetween;
                uint64 ray = between | attacker;
                if (attacker & b) pinmaskD12 |= ray;
                else              pinmaskHV  |= ray;
            }
            attackers ^= attacker;
        }
        return pinned;
    }

    // E7: slider-only variant of findAttackedSquares. Used when the caller has
    // precomputed the non-slider (pawn|knight|king) attack contribution and only
    // needs the slider piece contributions added. enemyBishops/enemyRooks refer
    // to leaf-state slider bitboards (they may have shifted by one square if the
    // parent move was a slider). The X-ray-through-my-king behaviour for sliders
    // is preserved.
    CPU_FORCE_INLINE static uint64 findSliderAttacksOnly(
        uint64 emptySquares, uint64 enemyBishops, uint64 enemyRooks, uint64 myKing,
        uint8 myKingIndex)
    {
        uint64 attacked = 0;
        uint64 sliderOcc = ~(emptySquares | myKing);

        // King-zone prefilter: see findAttackedSquares.
        uint64 sliders = enemyBishops & sqKingZoneDiag(myKingIndex);
        #pragma loop(ivdep)
        while (sliders)
        {
            uint8  sq = bitScan(sliders);
            uint64 occ = sliderOcc & sqBishopAttacksMasked(sq);
            uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
            attacked |= bishop_magic_tables[sq][idx];
            sliders &= sliders - 1;
        }

        sliders = enemyRooks & sqKingZoneOrtho(myKingIndex);
        #pragma loop(ivdep)
        while (sliders)
        {
            uint8  sq = bitScan(sliders);
            uint64 occ = sliderOcc & sqRookAttacksMasked(sq);
            uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
            attacked |= rook_magic_tables[sq][idx];
            sliders &= sliders - 1;
        }
        return attacked;
    }

    // Pair-wise interleaved version of findSliderAttacksOnly. Iterates the two
    // children's slider chains in lockstep so Zen 5's OoO can keep 2 magic-LUT
    // loads in flight at once (3 load ports on this μarch). NOT CURRENTLY
    // CALLED — sequential per-child `findSliderAttacksOnly` was faster under
    // PGO (E68: +0.7 ms regression). Kept as reference for the 4-way variant.
    static __forceinline void findSliderAttacksOnly2_UNUSED(
        uint64 emptyA, uint64 bishopsA, uint64 rooksA, uint64 myKingA,
        uint64 emptyB, uint64 bishopsB, uint64 rooksB, uint64 myKingB,
        uint64 &outA, uint64 &outB)
    {
        uint64 sliderOccA = ~(emptyA | myKingA);
        uint64 sliderOccB = ~(emptyB | myKingB);
        uint64 atkA = 0, atkB = 0;

        // Bishop chains, interleaved.
        uint64 sA = bishopsA;
        uint64 sB = bishopsB;
        while (sA | sB)
        {
            if (sA) {
                uint8  sq  = bitScan(sA);
                uint64 occ = sliderOccA & sqBishopAttacksMasked(sq);
                uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
                atkA |= bishop_magic_tables[sq][idx];
                sA &= sA - 1;
            }
            if (sB) {
                uint8  sq  = bitScan(sB);
                uint64 occ = sliderOccB & sqBishopAttacksMasked(sq);
                uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
                atkB |= bishop_magic_tables[sq][idx];
                sB &= sB - 1;
            }
        }

        // Rook chains, interleaved.
        sA = rooksA;
        sB = rooksB;
        while (sA | sB)
        {
            if (sA) {
                uint8  sq  = bitScan(sA);
                uint64 occ = sliderOccA & sqRookAttacksMasked(sq);
                uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
                atkA |= rook_magic_tables[sq][idx];
                sA &= sA - 1;
            }
            if (sB) {
                uint8  sq  = bitScan(sB);
                uint64 occ = sliderOccB & sqRookAttacksMasked(sq);
                uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
                atkB |= rook_magic_tables[sq][idx];
                sB &= sB - 1;
            }
        }

        outA = atkA;
        outB = atkB;
    }

    // 4-lane interleaved variant. NOT CURRENTLY CALLED — slow-path quad was
    // tried (E71) and regressed +2.4 ms vs Phase 4 pair-wise due to i-cache
    // pressure from 4 inlined countMovesFromDerived bodies.
    static __forceinline void findSliderAttacksOnly4_UNUSED(
        uint64 emptyA, uint64 bishopsA, uint64 rooksA, uint64 myKingA,
        uint64 emptyB, uint64 bishopsB, uint64 rooksB, uint64 myKingB,
        uint64 emptyC, uint64 bishopsC, uint64 rooksC, uint64 myKingC,
        uint64 emptyD, uint64 bishopsD, uint64 rooksD, uint64 myKingD,
        uint64 &outA, uint64 &outB, uint64 &outC, uint64 &outD)
    {
        uint64 occA = ~(emptyA | myKingA);
        uint64 occB = ~(emptyB | myKingB);
        uint64 occC = ~(emptyC | myKingC);
        uint64 occD = ~(emptyD | myKingD);
        uint64 atkA = 0, atkB = 0, atkC = 0, atkD = 0;

        // Bishop chains, 4-way interleaved.
        uint64 sA = bishopsA, sB = bishopsB, sC = bishopsC, sD = bishopsD;
        while (sA | sB | sC | sD)
        {
            if (sA) {
                uint8  sq  = bitScan(sA);
                uint64 occ = occA & sqBishopAttacksMasked(sq);
                uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
                atkA |= bishop_magic_tables[sq][idx];
                sA &= sA - 1;
            }
            if (sB) {
                uint8  sq  = bitScan(sB);
                uint64 occ = occB & sqBishopAttacksMasked(sq);
                uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
                atkB |= bishop_magic_tables[sq][idx];
                sB &= sB - 1;
            }
            if (sC) {
                uint8  sq  = bitScan(sC);
                uint64 occ = occC & sqBishopAttacksMasked(sq);
                uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
                atkC |= bishop_magic_tables[sq][idx];
                sC &= sC - 1;
            }
            if (sD) {
                uint8  sq  = bitScan(sD);
                uint64 occ = occD & sqBishopAttacksMasked(sq);
                uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
                atkD |= bishop_magic_tables[sq][idx];
                sD &= sD - 1;
            }
        }

        // Rook chains, 4-way interleaved.
        sA = rooksA; sB = rooksB; sC = rooksC; sD = rooksD;
        while (sA | sB | sC | sD)
        {
            if (sA) {
                uint8  sq  = bitScan(sA);
                uint64 occ = occA & sqRookAttacksMasked(sq);
                uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
                atkA |= rook_magic_tables[sq][idx];
                sA &= sA - 1;
            }
            if (sB) {
                uint8  sq  = bitScan(sB);
                uint64 occ = occB & sqRookAttacksMasked(sq);
                uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
                atkB |= rook_magic_tables[sq][idx];
                sB &= sB - 1;
            }
            if (sC) {
                uint8  sq  = bitScan(sC);
                uint64 occ = occC & sqRookAttacksMasked(sq);
                uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
                atkC |= rook_magic_tables[sq][idx];
                sC &= sC - 1;
            }
            if (sD) {
                uint8  sq  = bitScan(sD);
                uint64 occ = occD & sqRookAttacksMasked(sq);
                uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
                atkD |= rook_magic_tables[sq][idx];
                sD &= sD - 1;
            }
        }

        outA = atkA; outB = atkB; outC = atkC; outD = atkD;
    }

    // returns bitmask of squares in threat by enemy pieces
    // the king shouldn't ever attempt to move to a threatened square
    // TODO: maybe make this tempelated on color?
    CPU_FORCE_INLINE static uint64 findAttackedSquares(uint64 emptySquares, uint64 enemyBishops, uint64 enemyRooks,
                                      uint64 enemyPawns, uint64 enemyKnights, uint64 enemyKing,
                                      uint64 myKing, uint8 enemyColor, uint8 myKingIndex)
    {
        // E28: enemy king always exactly one bit; my king too. Helps MSVC
        // generate tighter slider iteration.
        __assume(myKing != 0);
        __assume(enemyKing != 0);
        uint64 attacked = 0;

        // 1. pawn attacks
        if (enemyColor == WHITE)
        {
            attacked |= northEastOne(enemyPawns);
            attacked |= northWestOne(enemyPawns);
        }
        else
        {
            attacked |= southEastOne(enemyPawns);
            attacked |= southWestOne(enemyPawns);
        }

        // 2. knight attacks
        attacked |= knightAttacks(enemyKnights);

        // 3+4. slider attacks — x-ray through my king (so king can't run along a check
        // ray). Compute the slider occupancy once and pass it down: the original
        // code called `bishopAttacks(b, pro)` which did `(~pro) & mask` inside —
        // identical algebraically but recomputed per call. MSVC's LICM was
        // reasonable but the explicit form lets the lookup loops use a single
        // already-negated occupancy. (E15)
        uint64 sliderOcc = ~(emptySquares | myKing);

        // King-zone prefilter (idea from sister repo perft_gpu_2026, cf33acd):
        // `attacked` is only ever consumed at king-local squares — the check test
        // (threatened & myKing), the king-ring filter (kingMoves &= ~threatened),
        // and the castle-corridor tests, whose squares are folded into the E1/E8
        // zone entries. A slider outside the zone cannot reach any of those squares
        // under ANY occupancy, because the zone is the empty-board superset of the
        // squares that attack the king's 3x3 neighbourhood. So skip its magic
        // lookup entirely.
        //
        // E20: bitScan(sliders) directly + BLSR clear — skips the
        // getOne (NEG+AND) per iter. The result is the index of the lowest
        // set bit either way.
        uint64 sliders = enemyBishops & sqKingZoneDiag(myKingIndex);
#pragma loop(ivdep)
        while (sliders)
        {
            uint8  sq = bitScan(sliders);
            uint64 occ = sliderOcc & sqBishopAttacksMasked(sq);
            uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
            attacked |= bishop_magic_tables[sq][idx];
            sliders &= sliders - 1;
        }

        sliders = enemyRooks & sqKingZoneOrtho(myKingIndex);
#pragma loop(ivdep)
        while (sliders)
        {
            uint8  sq = bitScan(sliders);
            uint64 occ = sliderOcc & sqRookAttacksMasked(sq);
            uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
            attacked |= rook_magic_tables[sq][idx];
            sliders &= sliders - 1;
        }

        // 5. King attacks
        attacked |= kingAttacks(enemyKing);

        return attacked;
    }

    CPU_FORCE_INLINE static void updateCastleFlag(GameState *gs, uint64 dst, uint8 chance)
    {
        if (chance == WHITE)
        {
            gs->blackCastle &= ~( ((dst & BLACK_KING_SIDE_ROOK ) >> H8)      |
                                   ((dst & BLACK_QUEEN_SIDE_ROOK) >> (A8-1))) ;
        }
        else
        {
            gs->whiteCastle &= ~( ((dst & WHITE_KING_SIDE_ROOK ) >> H1) |
                                   ((dst & WHITE_QUEEN_SIDE_ROOK) << 1)) ;
        }
    }

    // Generic templated move emission — Processor must expose:
    //   template <uint8 piece> void emit(uint8 from, uint8 to, uint8 flags);
    // Knowing the piece at emit time lets the FGMC processor pick a piece-specialised
    // makeMove; the array-store processor simply ignores the template arg.
    template <uint8 piece, typename Processor>
    CPU_FORCE_INLINE static void addCompactMove(Processor &p, uint8 from, uint8 to, uint8 flags)
    {
        p.template emit<piece>(from, to, flags);
    }

    template <typename Processor>
    CPU_FORCE_INLINE static void addCompactPawnMoves(Processor &p, uint8 from, uint64 dst, uint8 flags)
    {
        uint8 to = bitScan(dst);
        if (dst & (RANK1 | RANK8))
        {
            p.template emit<PAWN>(from, to, flags | CM_FLAG_KNIGHT_PROMOTION);
            p.template emit<PAWN>(from, to, flags | CM_FLAG_BISHOP_PROMOTION);
            p.template emit<PAWN>(from, to, flags | CM_FLAG_QUEEN_PROMOTION);
            p.template emit<PAWN>(from, to, flags | CM_FLAG_ROOK_PROMOTION);
        }
        else
        {
            p.template emit<PAWN>(from, to, flags);
        }
    }


    // Helper macro: derive piece bitboards from QuadBitBoard
    // After this macro, the following local variables are defined:
    //   allPawns, knights, bishopQueens, rookQueens, kings, allPieces, whitePieces, blackPieces
#define DERIVE_PIECE_BITBOARDS(pos) \
    uint64 allPieces    = (pos)->bb[1] | (pos)->bb[2] | (pos)->bb[3]; \
    uint64 blackPieces  = (pos)->bb[0]; \
    uint64 whitePieces  = allPieces & ~blackPieces; \
    uint64 allPawns     = (pos)->bb[1] & ~(pos)->bb[2] & ~(pos)->bb[3]; \
    uint64 knights      = (pos)->bb[2] & ~(pos)->bb[1] & ~(pos)->bb[3]; \
    uint64 bishopQueens = (pos)->bb[1] & ((pos)->bb[2] ^ (pos)->bb[3]); \
    uint64 rookQueens   = (pos)->bb[3] & ~(pos)->bb[2]; \
    uint64 kings        = (pos)->bb[2] & (pos)->bb[3] & ~(pos)->bb[1];


    template<uint8 chance, typename Processor>
    CPU_FORCE_INLINE static void enumerateMovesOutOfCheck (QuadBitBoard *pos, GameState *gs, Processor &p,
                                           uint64 allPawns, uint64 allPieces, uint64 myPieces,
                                           uint64 enemyPieces, uint64 pinned, uint64 threatened,
                                           uint8 kingIndex,
                                           uint64 knights_, uint64 bishopQueens_, uint64 rookQueens_, uint64 kings_)
    {
        uint64 king = kings_ & myPieces;

        // figure out the no. of attackers
        uint64 attackers = 0;

        // pawn attacks
        uint64 enemyPawns = allPawns & enemyPieces;
        attackers |= ((chance == WHITE) ? (northEastOne(king) | northWestOne(king)) :
                                          (southEastOne(king) | southWestOne(king)) ) & enemyPawns;

        // knight attackers — use LUT since king is a single piece (saves ~16 ALU ops vs bulk knightAttacks)
        uint64 enemyKnights = knights_ & enemyPieces;
        attackers |= sqKnightAttacks(kingIndex) & enemyKnights;

        // bishop attackers
        uint64 enemyBishops = bishopQueens_ & enemyPieces;
        attackers |= bishopAttacks(king, ~allPieces) & enemyBishops;

        // rook attackers
        uint64 enemyRooks = rookQueens_ & enemyPieces;
        attackers |= rookAttacks(king, ~allPieces) & enemyRooks;


        // A. Try king moves to get the king out of check
        uint64 kingMoves = sqKingAttacks(kingIndex);

        kingMoves &= ~(threatened | myPieces);  // king can't move to a square under threat or a square containing piece of same side
        while(kingMoves)
        {
            uint64 dst = getOne(kingMoves);

            // TODO: set capture flag correctly
            addCompactMove<KING>(p, kingIndex, bitScan(dst), 0);
            kingMoves ^= dst;
        }


        // B. try moves to kill/block attacking pieces
        if (isSingular(attackers))
        {
            // Find the safe squares - i.e, if a dst square of a move is any of the safe squares,
            // it will take king out of check

            // for pawn and knight attack, the only option is to kill the attacking piece
            // for bishops rooks and queens, it's the line between the attacker and the king, including the attacker
            uint64 safeSquares = attackers | sqsInBetween(kingIndex, bitScan(attackers));

            // pieces that are pinned don't have any hope of saving the king
            // TODO: Think more about it
            myPieces &= ~pinned;

            // 1. pawn moves
            uint64 myPawns = allPawns & myPieces;

            // checking rank for pawn double pushes
            uint64 checkingRankDoublePush = RANK3 << (chance * 24);           // rank 3 or rank 6

            uint64 enPassentTarget = getEpTarget<chance>(gs->enPassent);

            // en-passent can only save the king if the piece captured is the attacker
            uint64 enPassentCapturedPiece = (chance == WHITE) ? southOne(enPassentTarget) : northOne(enPassentTarget);
            if (enPassentCapturedPiece != attackers)
                enPassentTarget = 0;

            while (myPawns)
            {
                uint64 pawn = getOne(myPawns);

                // pawn push
                uint64 dst = ((chance == WHITE) ? northOne(pawn) : southOne(pawn)) & (~allPieces);
                if (dst)
                {
                    if (dst & safeSquares)
                    {
                        addCompactPawnMoves(p,bitScan(pawn), dst, 0);
                    }
                    else
                    {
                        // double push (only possible if single push was possible and single push didn't save the king)
                        dst = ((chance == WHITE) ? northOne(dst & checkingRankDoublePush):
                                                   southOne(dst & checkingRankDoublePush) ) & (safeSquares) &(~allPieces);

                        if (dst)
                        {
                            addCompactMove<PAWN>(p, bitScan(pawn), bitScan(dst), CM_FLAG_DOUBLE_PAWN_PUSH);
                        }
                    }
                }

                // captures (only one of the two captures will save the king.. if at all it does)
                uint64 westCapture = (chance == WHITE) ? northWestOne(pawn) : southWestOne(pawn);
                uint64 eastCapture = (chance == WHITE) ? northEastOne(pawn) : southEastOne(pawn);
                dst = (westCapture | eastCapture) & enemyPieces & safeSquares;
                if (dst)
                {
                    addCompactPawnMoves(p,bitScan(pawn), dst, CM_FLAG_CAPTURE);
                }

                // en-passent
                dst = (westCapture | eastCapture) & enPassentTarget;
                if (dst)
                {
                    addCompactMove<PAWN>(p, bitScan(pawn), bitScan(dst), CM_FLAG_EP_CAPTURE);
                }

                myPawns ^= pawn;
            }

            // 2. knight moves
            uint64 myKnights = (knights_ & myPieces);
            while (myKnights)
            {
                uint64 knight = getOne(myKnights);
                uint64 knightMoves = sqKnightAttacks(bitScan(knight)) & safeSquares;
                while (knightMoves)
                {
                    uint64 dst = getOne(knightMoves);
                    // TODO: set capture flag correctly
                    addCompactMove<KNIGHT>(p, bitScan(knight), bitScan(dst), 0);
                    knightMoves ^= dst;
                }
                myKnights ^= knight;
            }
            
            // 3. bishop moves
            uint64 bishops = bishopQueens_ & myPieces;
            while (bishops)
            {
                uint64 bishop = getOne(bishops);
                uint64 bishopMoves = bishopAttacks(bishop, ~allPieces) & safeSquares;

                while (bishopMoves)
                {
                    uint64 dst = getOne(bishopMoves);
                    addCompactMove<BISHOP>(p, bitScan(bishop), bitScan(dst), 0);
                    bishopMoves ^= dst;
                }
                bishops ^= bishop;
            }

            // 4. rook moves
            uint64 rooks = rookQueens_ & myPieces;
            while (rooks)
            {
                uint64 rook = getOne(rooks);
                uint64 rookMoves = rookAttacks(rook, ~allPieces) & safeSquares;

                while (rookMoves)
                {
                    uint64 dst = getOne(rookMoves);
                    // TODO: set capture flag correctly
                    addCompactMove<ROOK>(p, bitScan(rook), bitScan(dst), 0);
                    rookMoves ^= dst;
                }
                rooks ^= rook;
            }

        }   // end of if single attacker
        else
        {
            // multiple threats => only king moves possible
        }
    }


    // Templated move enumerator. The Processor must expose `void emit(from, to, flags)`.
    // The compiler inlines through the emit call, so for the array-store processor this is
    // exactly equivalent to the previous generateMoves(); for the FGMC processor each move
    // becomes an inline makeMove + countMoves dispatch.
    template <uint8 chance, typename Processor>
    static void enumerateMoves (QuadBitBoard *pos, GameState *gs, Processor &p)
    {
        DERIVE_PIECE_BITBOARDS(pos);

        uint64 myPieces     = (chance == WHITE) ? whitePieces : blackPieces;
        uint64 enemyPieces  = (chance == WHITE) ? blackPieces : whitePieces;

        uint64 enemyBishops = bishopQueens & enemyPieces;
        uint64 enemyRooks   = rookQueens & enemyPieces;

        uint64 myKing     = kings & myPieces;
        uint8  kingIndex  = bitScan(myKing);

        uint64 pinned     = findPinnedPieces(myKing, myPieces, enemyBishops, enemyRooks, allPieces, kingIndex);

        uint64 threatened = findAttackedSquares(~allPieces, enemyBishops, enemyRooks, allPawns & enemyPieces,
                                                knights & enemyPieces, kings & enemyPieces,
                                                myKing, !chance, kingIndex);

        // king is in check: call special generate function to generate only the moves that take king out of check
        if (threatened & myKing)
        {
            enumerateMovesOutOfCheck<chance>(pos, gs, p, allPawns, allPieces, myPieces, enemyPieces,
                                                              pinned, threatened, kingIndex,
                                                              knights, bishopQueens, rookQueens, kings);
            return;
        }


        // generate king moves
        uint64 kingMoves = sqKingAttacks(kingIndex);

        kingMoves &= ~(threatened | myPieces);  // king can't move to a square under threat or a square containing piece of same side
        // E22 pattern: bitScan-direct + BLSR in inner emit loops.
        while (kingMoves)
        {
            addCompactMove<KING>(p, kingIndex, bitScan(kingMoves), 0);
            kingMoves &= kingMoves - 1;
        }

        // generate knight moves (only non-pinned knights can move)
        uint64 myKnights = (knights & myPieces) & ~pinned;
        while (myKnights)
        {
            uint8 knightSq = bitScan(myKnights);
            uint64 knightMoves = sqKnightAttacks(knightSq) & ~myPieces;
            while (knightMoves)
            {
                addCompactMove<KNIGHT>(p, knightSq, bitScan(knightMoves), 0);
                knightMoves &= knightMoves - 1;
            }
            myKnights &= myKnights - 1;
        }



        // generate bishop (and queen) moves — combined loop, FGMC emit's runtime
        // bb[3] probe distinguishes queen vs bishop. (Queen-split was tried and
        // introduced a perft-7 mismatch; reverted while we investigate.)
        uint64 myBishops = bishopQueens & myPieces;
        uint64 bishops = myBishops & pinned;
        while (bishops)
        {
            uint64 bishop = getOne(bishops);
            uint64 bishopMoves = bishopAttacks(bishop, ~allPieces) & ~myPieces;
            bishopMoves &= sqsInLine(bitScan(bishop), kingIndex);
            while (bishopMoves)
            {
                uint64 dst = getOne(bishopMoves);
                addCompactMove<BISHOP>(p, bitScan(bishop), bitScan(dst), 0);
                bishopMoves ^= dst;
            }
            bishops ^= bishop;
        }
        // E19: hoist bitScan(bishop) — MSVC can't CSE _BitScanForward64 calls
        // because the intrinsic writes via an output pointer. Inline magic
        // lookup with split arrays (E18 pattern). E20-style bitScan-direct +
        // BLSR pattern was tried here and regressed (~+1.5% on perft 6),
        // probably because the outer loop is amortized over a heavy inner
        // emit loop so reducing outer iter overhead has less leverage.
        // E19: hoist bitScan(bishop). E20 bitScan-direct + BLSR on the outer
        // loop regresses here (the outer loop is amortized over a heavy inner
        // emit loop). Keep getOne/^=b in outer, E22 inner.
        {
            uint64 notMyP = ~myPieces;
            bishops = myBishops & ~pinned;
            while (bishops)
            {
                uint64 b  = getOne(bishops);
                uint8  sq = bitScan(b);
                uint64 occ = allPieces & sqBishopAttacksMasked(sq);
                uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
                uint64 bishopMoves = bishop_magic_tables[sq][idx] & notMyP;
                while (bishopMoves)
                {
                    addCompactMove<BISHOP>(p, sq, bitScan(bishopMoves), 0);
                    bishopMoves &= bishopMoves - 1;
                }
                bishops ^= b;
            }
        }

        // rook/queen moves
        uint64 myRooks = rookQueens & myPieces;
        uint64 rooks = myRooks & pinned;
        while (rooks)
        {
            uint64 rook = getOne(rooks);
            uint64 rookMoves = rookAttacks(rook, ~allPieces) & ~myPieces;
            rookMoves &= sqsInLine(bitScan(rook), kingIndex);
            while (rookMoves)
            {
                uint64 dst = getOne(rookMoves);
                addCompactMove<ROOK>(p, bitScan(rook), bitScan(dst), 0);
                rookMoves ^= dst;
            }
            rooks ^= rook;
        }
        // E19 + E22 inner-loop pattern. Outer kept in getOne/^=r form
        // (E20 outer regressed here per E25 retry).
        {
            uint64 notMyP = ~myPieces;
            rooks = myRooks & ~pinned;
            while (rooks)
            {
                uint64 r  = getOne(rooks);
                uint8  sq = bitScan(r);
                uint64 occ = allPieces & sqRookAttacksMasked(sq);
                uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
                uint64 rookMoves = rook_magic_tables[sq][idx] & notMyP;
                while (rookMoves)
                {
                    addCompactMove<ROOK>(p, sq, bitScan(rookMoves), 0);
                    rookMoves &= rookMoves - 1;
                }
                rooks ^= r;
            }
        }


        uint64 myPawns = allPawns & myPieces;

        // generate en-passent moves
        uint64 enPassentTarget = getEpTarget<chance>(gs->enPassent);

        if (enPassentTarget)
        {
            uint64 enPassentCapturedPiece = (chance == WHITE) ? southOne(enPassentTarget) : northOne(enPassentTarget);

            uint64 epSources = (eastOne(enPassentCapturedPiece) | westOne(enPassentCapturedPiece)) & myPawns;

            while (epSources)
            {
                uint64 pawn = getOne(epSources);
                if (pawn & pinned)
                {
                    uint64 line = sqsInLine(bitScan(pawn), kingIndex);
                    if (enPassentTarget & line)
                    {
                        addCompactMove<PAWN>(p, bitScan(pawn), bitScan(enPassentTarget), CM_FLAG_EP_CAPTURE);
                    }
                }
                else
                {
                    // Check if removing both pawns reveals a rook attack on king along the rank.
                    // Mask to king's rank only — vertical attacks are irrelevant here.
                    uint64 modifiedOcc = allPieces ^ enPassentCapturedPiece ^ pawn;
                    uint64 rankMask = RANK1 << ((kingIndex >> 3) * 8);
                    uint64 causesCheck = rookAttacks(myKing, ~modifiedOcc) & enemyRooks & rankMask;
                    if (!causesCheck)
                    {
                        addCompactMove<PAWN>(p, bitScan(pawn), bitScan(enPassentTarget), CM_FLAG_EP_CAPTURE);
                    }
                }
                epSources ^= pawn;
            }
        }

        // 1. pawn moves

        // checking rank for pawn double pushes
        uint64 checkingRankDoublePush = RANK3 << (chance * 24);           // rank 3 or rank 6

        // first deal with pinned pawns
        uint64 pinnedPawns = myPawns & pinned;

        while (pinnedPawns)
        {
            uint64 pawn = getOne(pinnedPawns);
            uint8 pawnIndex = bitScan(pawn);    // same as bitscan on pinnedPawns

            // the direction of the pin (mask containing all squares in the line joining the king and the current piece)
            uint64 line = sqsInLine(pawnIndex, kingIndex);

            // pawn push
            uint64 dst = ((chance == WHITE) ? northOne(pawn) : southOne(pawn)) & line & (~allPieces);
            if (dst) 
            {
                addCompactMove<PAWN>(p, pawnIndex, bitScan(dst), 0);

                // double push (only possible if single push was possible)
                dst = ((chance == WHITE) ? northOne(dst & checkingRankDoublePush): 
                                           southOne(dst & checkingRankDoublePush) ) & (~allPieces);
                if (dst) 
                {
                    addCompactMove<PAWN>(p, pawnIndex, bitScan(dst), CM_FLAG_DOUBLE_PAWN_PUSH);
                }
            }

            // captures
            // (either of them will be valid - if at all)
            dst  = ((chance == WHITE) ? northWestOne(pawn) : southWestOne(pawn)) & line;
            dst |= ((chance == WHITE) ? northEastOne(pawn) : southEastOne(pawn)) & line;
            
            if (dst & enemyPieces) 
            {
                addCompactPawnMoves(p,pawnIndex, dst, CM_FLAG_CAPTURE);
            }

            pinnedPawns ^= pawn;  // same as &= ~pawn (but only when we know that the first set contain the element we want to clear)
        }

        myPawns = myPawns & ~pinned;

        while (myPawns)
        {
            uint64 pawn = getOne(myPawns);

            // pawn push
            uint64 dst = ((chance == WHITE) ? northOne(pawn) : southOne(pawn)) & (~allPieces);
            if (dst) 
            {
                addCompactPawnMoves(p,bitScan(pawn), dst, 0);

                // double push (only possible if single push was possible)
                dst = ((chance == WHITE) ? northOne(dst & checkingRankDoublePush): 
                                           southOne(dst & checkingRankDoublePush) ) & (~allPieces);

                if (dst) addCompactPawnMoves(p,bitScan(pawn), dst, CM_FLAG_DOUBLE_PAWN_PUSH);
            }

            // captures
            uint64 westCapture = (chance == WHITE) ? northWestOne(pawn) : southWestOne(pawn);
            dst = westCapture & enemyPieces;
            if (dst) addCompactPawnMoves(p,bitScan(pawn), dst, CM_FLAG_CAPTURE);

            uint64 eastCapture = (chance == WHITE) ? northEastOne(pawn) : southEastOne(pawn);
            dst = eastCapture & enemyPieces;
            if (dst) addCompactPawnMoves(p,bitScan(pawn), dst, CM_FLAG_CAPTURE);

            myPawns ^= pawn;
        }

        // generate castling moves
        if (chance == WHITE)
        {
            if ((gs->whiteCastle & CASTLE_FLAG_KING_SIDE) &&
                !(F1G1 & allPieces) &&
                !(F1G1 & threatened))
            {
                addCompactMove<KING>(p, E1, G1, CM_FLAG_KING_CASTLE);
            }
            if ((gs->whiteCastle & CASTLE_FLAG_QUEEN_SIDE) &&
                !(B1D1 & allPieces) &&
                !(C1D1 & threatened))
            {
                addCompactMove<KING>(p, E1, C1, CM_FLAG_QUEEN_CASTLE);
            }
        }
        else
        {
            if ((gs->blackCastle & CASTLE_FLAG_KING_SIDE) &&
                !(F8G8 & allPieces) &&
                !(F8G8 & threatened))
            {
                addCompactMove<KING>(p, E8, G8, CM_FLAG_KING_CASTLE);
            }
            if ((gs->blackCastle & CASTLE_FLAG_QUEEN_SIDE) &&
                !(B8D8 & allPieces) &&
                !(C8D8 & threatened))
            {
                addCompactMove<KING>(p, E8, C8, CM_FLAG_QUEEN_CASTLE);
            }
        }
    }

    // Processor that stores moves to a CMove array — preserves the legacy generateMoves API.
    struct ArrayStoreProcessor
    {
        CMove *out;
        uint32 count;
        template <uint8 /*piece*/>
        CPU_FORCE_INLINE void emit(uint8 from, uint8 to, uint8 flags)
        {
            out[count++] = CMove(from, to, flags);
        }
    };

    // Processor that for each emitted move: makes it on a temporary, then sums countMoves
    // of the resulting child position. This is "fused generate-make-count" — for the
    // depth-2 leaf path it skips the entire CMove array round-trip. The piece template
    // arg routes to a piece-typed makeMoveT, eliminating the bb-probe + irrelevant
    // branches. For sliders the bishopQueens / rookQueens loops emit a single piece
    // tag (BISHOP / ROOK) and a single bb-probe disambiguates queen vs bishop/rook —
    // still cheaper than the generic makeMove that probes all three bb planes.
    // Dispatch the depth-1 countMoves with compile-time EP / castle flags so
    // the EP and castle blocks can be elided. ~95% of leaves have no EP.
    template <uint8 leafChance>
    CPU_FORCE_INLINE static uint64 countMovesDispatch(const QuadBitBoard *cp, const GameState *cgs)
    {
        bool hasEP = cgs->enPassent != 0;
        bool hasMyCastle = (leafChance == WHITE) ? (cgs->whiteCastle != 0) : (cgs->blackCastle != 0);
        if (!hasEP)
        {
            if (hasMyCastle) return countMoves<leafChance, false, true >(const_cast<QuadBitBoard*>(cp), const_cast<GameState*>(cgs));
            else             return countMoves<leafChance, false, false>(const_cast<QuadBitBoard*>(cp), const_cast<GameState*>(cgs));
        }
        else
        {
            if (hasMyCastle) return countMoves<leafChance, true,  true >(const_cast<QuadBitBoard*>(cp), const_cast<GameState*>(cgs));
            else             return countMoves<leafChance, true,  false>(const_cast<QuadBitBoard*>(cp), const_cast<GameState*>(cgs));
        }
    }

    // E7: split-buffer processor. Slider moves use cached non-slider attacks.
    // Other moves use full findAttackedSquares.
    template <uint8 parentChance>
    struct FgmcCount2Processor
    {
        static constexpr int BUF_CAP = 128;

        QuadBitBoard parent;
        GameState parentGs;
        int bufN_fast;
        int bufN_slow;
        uint64 partialSum;
        uint64 cachedNonSliderAtk;
#if USE_NULL_COUNT_DEPTH2
        // Depth-2 null-count reuse (see buildNullMasks).
        uint64 npOcc;          // squares whose occupancy can change O's own moves
        uint64 npLines;        // squares on a slider line to O's king ring (+ pin x-rays)
        uint64 npKnightPre;    // squares from which a knight attacks the ring
        uint64 npKingPre;      // ... a king
        uint64 npPawnPre;      // ... an M pawn
        uint32 npNullCount;    // O's reply count on the unchanged board
        bool   npEnabled;
#endif
        // E9: leaf's myKing is parent's opp-side king. parent never moves the
        // opp-side pieces, so this is invariant across the entire enumeration.
        // Hoist to avoid `kings = ...; myKing = kings & myPieces; bitScan(myKing)`
        // per leaf — replace by a single bitboard load + use.
        uint64 leafMyKing;
        alignas(64) QuadBitBoard bufCp_fast[BUF_CAP];
        GameState               bufGs_fast[BUF_CAP];
        alignas(64) QuadBitBoard bufCp_slow[BUF_CAP];
        GameState               bufGs_slow[BUF_CAP];

        CPU_FORCE_INLINE FgmcCount2Processor(const QuadBitBoard &p, const GameState &pgs) noexcept
            : parent(p), parentGs(pgs), bufN_fast(0), bufN_slow(0), partialSum(0)
        {
            uint64 all          = parent.bb[1] | parent.bb[2] | parent.bb[3];
            uint64 blackPieces  = parent.bb[0];
            uint64 whitePieces  = all & ~blackPieces;
            uint64 myPieces     = (parentChance == WHITE) ? whitePieces : blackPieces;
            uint64 oppPieces    = (parentChance == WHITE) ? blackPieces : whitePieces;
            uint64 allPawns     = parent.bb[1] & ~parent.bb[2] & ~parent.bb[3];
            uint64 knights      = parent.bb[2] & ~parent.bb[1] & ~parent.bb[3];
            uint64 kings        = parent.bb[2] &  parent.bb[3] & ~parent.bb[1];
            uint64 myPawns      = allPawns & myPieces;
            uint64 myKnights    = knights  & myPieces;
            uint64 myKing       = kings    & myPieces;
            leafMyKing          = kings    & oppPieces;   // leaf's myKing = parent's opp king
            uint64 pawnAtk;
            if constexpr (parentChance == WHITE)
                pawnAtk = northEastOne(myPawns) | northWestOne(myPawns);
            else
                pawnAtk = southEastOne(myPawns) | southWestOne(myPawns);
            cachedNonSliderAtk = pawnAtk | knightAttacks(myKnights) | kingAttacks(myKing);
#if USE_NULL_COUNT_DEPTH2
            buildNullMasks(all, oppPieces, allPawns, knights, myKing);
#endif
        }

#if USE_NULL_COUNT_DEPTH2
        static CPU_FORCE_INLINE uint64 bishopAtkOcc(uint8 sq, uint64 occ) noexcept
        {
            uint64 o = occ & sqBishopAttacksMasked(sq);
            return bishop_magic_tables[sq][(bishop_magic_factors[sq] * o) >> (64 - BISHOP_MAGIC_BITS)];
        }
        static CPU_FORCE_INLINE uint64 rookAtkOcc(uint8 sq, uint64 occ) noexcept
        {
            uint64 o = occ & sqRookAttacksMasked(sq);
            return rook_magic_tables[sq][(rook_magic_factors[sq] * o) >> (64 - ROOK_MAGIC_BITS)];
        }

        // Depth-2 null-move count reuse (idea: MPerft / chessbit via sister repo
        // perft_gpu_2026 commit 8f9bde2).
        //
        // Let M = the mover (parentChance) and O = the replier. The "null" count
        //     N = countMoves<O>(parent board, EP cleared)
        // is O's reply count if M could pass. For a child move f->t, the reply
        // count still equals N whenever the move provably leaves O's legal move
        // set untouched -- then the child needs no makeMove and no leaf count at
        // all, just `sum += N`.
        //
        // A move is harmless when it is a quiet move (flags == 0 rules out
        // captures-with-flags, promotions, castling, EP capture, and double pushes
        // which would hand O an EP capture) AND neither f nor t is in any of:
        //
        //   npOcc   occupancy-sensitive squares for O's OWN pieces:
        //             oppPieces  - t here means a capture: O loses a piece
        //             sl         - O's slider attack set: filling t truncates a ray,
        //                          vacating f extends one
        //             pw         - O's pawn push (1 and 2 step) and capture squares,
        //                          where occupancy flips a pawn move on or off
        //             ringExt    - O's king ring plus its castle corridors, where
        //                          occupancy decides king/castling moves
        //   npLines squares on a slider line to the ring: filling t blocks an M
        //           slider that bears on the ring, vacating f discovers one, and a
        //           moved slider landing on t may newly bear on the ring. Attack
        //           sets are symmetric under a fixed occupancy, so the set of
        //           squares from which a bishop attacks r is bishopAttacks(r, occ).
        //           Pins are relative to the king square alone, so one level of
        //           x-ray from there covers pins created and broken.
        //   npKnightPre / npKingPre / npPawnPre: the same "does the moved piece
        //           itself bear on the ring" test for the non-slider piece types.
        //
        // Guard: if M is in check, the null board would let O capture M's king, so
        // N is not a valid reply count. Disable the reuse for that parent.
        CPU_FORCE_INLINE void buildNullMasks(uint64 all, uint64 oppPieces, uint64 allPawns,
                                             uint64 knights, uint64 myKing) noexcept
        {
            uint64 bishopQueens = parent.bb[1] & (parent.bb[2] ^ parent.bb[3]);
            uint64 rookQueens   = parent.bb[3] & ~parent.bb[2];
            uint64 oPawns   = allPawns     & oppPieces;
            uint64 oKnights = knights      & oppPieces;
            uint64 oBishops = bishopQueens & oppPieces;
            uint64 oRooks   = rookQueens   & oppPieces;
            uint64 oKing    = leafMyKing;
            uint8  oKingIdx = bitScan(oKing);

            uint64 sl = 0, sliders = oBishops;
            while (sliders) { uint8 q = bitScan(sliders); sl |= bishopAtkOcc(q, all); sliders &= sliders - 1; }
            sliders = oRooks;
            while (sliders) { uint8 q = bitScan(sliders); sl |= rookAtkOcc(q, all);   sliders &= sliders - 1; }

            uint64 oPawnAtk, pw;
            if constexpr (parentChance == WHITE) {          // O is BLACK, pushes south
                uint64 p1 = southOne(oPawns);
                oPawnAtk = southEastOne(oPawns) | southWestOne(oPawns);
                pw = oPawnAtk | p1 | southOne(p1);
            } else {
                uint64 p1 = northOne(oPawns);
                oPawnAtk = northEastOne(oPawns) | northWestOne(oPawns);
                pw = oPawnAtk | p1 | northOne(p1);
            }

            uint64 oAtk = sl | oPawnAtk | knightAttacks(oKnights) | kingAttacks(oKing);
            npEnabled = (oAtk & myKing) == 0;
            if (!npEnabled) return;

            uint64 ringExt = sqKingAttacks(oKingIdx) | oKing;
            uint64 occExtra = 0;
            uint8 oCastle = (parentChance == WHITE) ? parentGs.blackCastle : parentGs.whiteCastle;
            if (oCastle) [[unlikely]]
            {
                if constexpr (parentChance == WHITE) {
                    if (oCastle & CASTLE_FLAG_KING_SIDE)  { ringExt |= BIT(F8) | BIT(G8); occExtra |= BIT(F8) | BIT(G8); }
                    if (oCastle & CASTLE_FLAG_QUEEN_SIDE) { ringExt |= BIT(C8) | BIT(D8); occExtra |= BIT(B8) | BIT(C8) | BIT(D8); }
                } else {
                    if (oCastle & CASTLE_FLAG_KING_SIDE)  { ringExt |= BIT(F1) | BIT(G1); occExtra |= BIT(F1) | BIT(G1); }
                    if (oCastle & CASTLE_FLAG_QUEEN_SIDE) { ringExt |= BIT(C1) | BIT(D1); occExtra |= BIT(B1) | BIT(C1) | BIT(D1); }
                }
            }
            npOcc = oppPieces | sl | pw | ringExt | occExtra;

            uint64 lines = 0, sqs = ringExt;
            while (sqs)
            {
                uint8 r = bitScan(sqs);
                lines |= bishopAtkOcc(r, all) | rookAtkOcc(r, all);
                sqs &= sqs - 1;
            }
            uint64 b  = bishopAtkOcc(oKingIdx, all);
            uint64 rk = rookAtkOcc  (oKingIdx, all);
            lines |= bishopAtkOcc(oKingIdx, all ^ (b  & all));   // pin x-ray, king square only
            lines |= rookAtkOcc  (oKingIdx, all ^ (rk & all));
            npLines = lines;

            npKnightPre = knightAttacks(ringExt);
            npKingPre   = kingAttacks(ringExt);
            if constexpr (parentChance == WHITE)
                npPawnPre = southEastOne(ringExt) | southWestOne(ringExt);
            else
                npPawnPre = northEastOne(ringExt) | northWestOne(ringExt);

            QuadBitBoard np = parent;
            GameState    ng = parentGs;
            ng.enPassent = 0;
            npNullCount = countMoves<(uint8)(parentChance ^ 1)>(&np, &ng);
        }
#endif

        // Drain a full buffer mid-enumeration. A position can have up to 218
        // legal moves, all of which may land in the same buffer, so BUF_CAP is
        // an optimization knob, not a correctness assumption.
        CPU_FORCE_INLINE void drainFast() noexcept
        {
            constexpr uint8 leafChance = (uint8)(parentChance ^ 1);
            if constexpr (leafChance == WHITE)
                partialSum += countMovesBulkAVX2_fast_white(bufCp_fast, bufGs_fast, bufN_fast, cachedNonSliderAtk);
            else
                partialSum += countMovesBulkAVX2_fast_black(bufCp_fast, bufGs_fast, bufN_fast, cachedNonSliderAtk);
            bufN_fast = 0;
        }

        CPU_FORCE_INLINE void drainSlow() noexcept
        {
            constexpr uint8 leafChance = (uint8)(parentChance ^ 1);
            if constexpr (leafChance == WHITE)
                partialSum += countMovesBulkAVX2_slow_white(bufCp_slow, bufGs_slow, bufN_slow);
            else
                partialSum += countMovesBulkAVX2_slow_black(bufCp_slow, bufGs_slow, bufN_slow);
            bufN_slow = 0;
        }

        template <uint8 piece>
        CPU_FORCE_INLINE void emit(uint8 from, uint8 to, uint8 flags)
        {
            constexpr bool isSliderEmit = (piece == BISHOP || piece == ROOK || piece == QUEEN);
#if USE_NULL_COUNT_DEPTH2
            if (npEnabled && flags == CM_FLAG_QUIET_MOVE)
            {
                uint64 mask = npOcc | npLines;
                if      constexpr (piece == KNIGHT) mask |= npKnightPre;
                else if constexpr (piece == KING)   mask |= npKingPre;
                else if constexpr (piece == PAWN)   mask |= npPawnPre;
                if (!((BIT(from) | BIT(to)) & mask))
                {
                    partialSum += npNullCount;   // reply set provably unchanged
                    return;
                }
            }
#endif
            QuadBitBoard *cp;
            GameState    *cgs;
            if constexpr (isSliderEmit) {
                if (bufN_fast == BUF_CAP) [[unlikely]] drainFast();
                int i = bufN_fast++;
                cp  = &bufCp_fast[i];
                cgs = &bufGs_fast[i];
            } else {
                if (bufN_slow == BUF_CAP) [[unlikely]] drainSlow();
                int i = bufN_slow++;
                cp  = &bufCp_slow[i];
                cgs = &bufGs_slow[i];
            }
            CMove move(from, to, flags);
            if constexpr (piece == BISHOP)
            {
                uint64 src = BIT(from);
                if (parent.bb[3] & src) [[unlikely]]
                    makeMoveTFromParent<parentChance, QUEEN>(cp, cgs, &parent, &parentGs, move);
                else
                    makeMoveTFromParent<parentChance, BISHOP>(cp, cgs, &parent, &parentGs, move);
            }
            else if constexpr (piece == ROOK)
            {
                uint64 src = BIT(from);
                if (parent.bb[1] & src) [[unlikely]]
                    makeMoveTFromParent<parentChance, QUEEN>(cp, cgs, &parent, &parentGs, move);
                else
                    makeMoveTFromParent<parentChance, ROOK>(cp, cgs, &parent, &parentGs, move);
            }
            else
            {
                makeMoveTFromParent<parentChance, piece>(cp, cgs, &parent, &parentGs, move);
            }
        }

        CPU_FORCE_INLINE uint64 flush()
        {
            constexpr uint8 leafChance = (uint8)(parentChance ^ 1);
            uint64 s = partialSum;
            if constexpr (leafChance == WHITE) {
                s += countMovesBulkAVX2_fast_white(bufCp_fast, bufGs_fast, bufN_fast, cachedNonSliderAtk);
                s += countMovesBulkAVX2_slow_white(bufCp_slow, bufGs_slow, bufN_slow);
            } else {
                s += countMovesBulkAVX2_fast_black(bufCp_fast, bufGs_fast, bufN_fast, cachedNonSliderAtk);
                s += countMovesBulkAVX2_slow_black(bufCp_slow, bufGs_slow, bufN_slow);
            }
            return s;
        }
    };

    // Legacy entry point — kept for callers (e.g. GPU code, perft_cpu_recurse).
    template <uint8 chance>
    CPU_FORCE_INLINE static uint32 generateMoves(QuadBitBoard *pos, GameState *gs, CMove *genMoves)
    {
        ArrayStoreProcessor proc{genMoves, 0};
        enumerateMoves<chance>(pos, gs, proc);
        return proc.count;
    }

    // Fused leaf entry: for a depth-2 subtree, sums countMoves over every legal child.
    // This avoids materialising the parent's move list into memory.
    template <uint8 chance>
    CPU_FORCE_INLINE static uint64 countTwoLevelSubtree(QuadBitBoard *pos, GameState *gs)
    {
        FgmcCount2Processor<chance> proc(*pos, *gs);
        enumerateMoves<chance>(pos, gs, proc);
        return proc.flush();
    }

    // Same idea one level up: at depth-3 we want to enumerate moves and, for each
    // child, call countTwoLevelSubtree. The CMove array round-trip is small at this
    // level (~4M emits in Kiwipete perft 5), so the win is modest, but it's free.
    template <uint8 parentChance>
    struct FgmcCount3Processor
    {
        QuadBitBoard parent;
        GameState parentGs;
        uint64 sum;
        template <uint8 piece>
        CPU_FORCE_INLINE void emit(uint8 from, uint8 to, uint8 flags)
        {
            QuadBitBoard cp;
            GameState cgs;
            CMove move(from, to, flags);
            if constexpr (piece == BISHOP)
            {
                uint64 src = BIT(from);
                if (parent.bb[3] & src) [[unlikely]] makeMoveTFromParent<parentChance, QUEEN >(&cp, &cgs, &parent, &parentGs, move);
                else                                 makeMoveTFromParent<parentChance, BISHOP>(&cp, &cgs, &parent, &parentGs, move);
            }
            else if constexpr (piece == ROOK)
            {
                uint64 src = BIT(from);
                if (parent.bb[1] & src) [[unlikely]] makeMoveTFromParent<parentChance, QUEEN>(&cp, &cgs, &parent, &parentGs, move);
                else                                 makeMoveTFromParent<parentChance, ROOK >(&cp, &cgs, &parent, &parentGs, move);
            }
            else
            {
                makeMoveTFromParent<parentChance, piece>(&cp, &cgs, &parent, &parentGs, move);
            }
            sum += countTwoLevelSubtree<(uint8)(parentChance ^ 1)>(&cp, &cgs);
        }
    };

    template <uint8 chance>
    CPU_FORCE_INLINE static uint64 countThreeLevelSubtree(QuadBitBoard *pos, GameState *gs)
    {
        FgmcCount3Processor<chance> proc{*pos, *gs, 0};
        enumerateMoves<chance>(pos, gs, proc);
        return proc.sum;
    }

    template<uint8 chance>
    CPU_FORCE_INLINE static uint32 countMovesOutOfCheck (QuadBitBoard *pos, GameState *gs,
                                           uint64 allPawns, uint64 allPieces, uint64 myPieces,
                                           uint64 enemyPieces, uint64 pinned, uint64 threatened,
                                           uint8 kingIndex,
                                           uint64 knights_, uint64 bishopQueens_, uint64 rookQueens_, uint64 kings_)
    {
        uint32 nMoves = 0;
        uint64 king = kings_ & myPieces;

        // figure out the no. of attackers
        uint64 attackers = 0;

        // pawn attacks
        uint64 enemyPawns = allPawns & enemyPieces;
        attackers |= ((chance == WHITE) ? (northEastOne(king) | northWestOne(king)) :
                                          (southEastOne(king) | southWestOne(king)) ) & enemyPawns;

        // knight attackers — use LUT since king is a single piece (saves ~16 ALU ops vs bulk knightAttacks)
        uint64 enemyKnights = knights_ & enemyPieces;
        attackers |= sqKnightAttacks(kingIndex) & enemyKnights;

        // bishop attackers
        uint64 enemyBishops = bishopQueens_ & enemyPieces;
        attackers |= bishopAttacks(king, ~allPieces) & enemyBishops;

        // rook attackers
        uint64 enemyRooks = rookQueens_ & enemyPieces;
        attackers |= rookAttacks(king, ~allPieces) & enemyRooks;


        // A. Try king moves to get the king out of check
        uint64 kingMoves = sqKingAttacks(kingIndex);

        kingMoves &= ~(threatened | myPieces);  // king can't move to a square under threat or a square containing piece of same side
        nMoves += popCount(kingMoves);

        // B. try moves to kill/block attacking pieces
        if (isSingular(attackers))
        {
            // Find the safe squares - i.e, if a dst square of a move is any of the safe squares,
            // it will take king out of check

            // for pawn and knight attack, the only option is to kill the attacking piece
            // for bishops rooks and queens, it's the line between the attacker and the king, including the attacker
            uint64 safeSquares = attackers | sqsInBetween(kingIndex, bitScan(attackers));

            // pieces that are pinned don't have any hope of saving the king
            // TODO: Think more about it
            myPieces &= ~pinned;

            // 1. pawn moves in bulk (no per-pawn loop)
            uint64 myPawns = allPawns & myPieces;
            uint64 emptySquares = ~allPieces;
            uint64 checkingRankDoublePush = RANK3 << (chance * 24);

            // Single pushes
            uint64 dsts = ((chance == WHITE) ? northOne(myPawns) : southOne(myPawns)) & emptySquares;
            uint64 safeDsts = dsts & safeSquares;
            uint64 promos = safeDsts & (RANK1 | RANK8);
            nMoves += popCount(safeDsts ^ promos) + (4 * popCount(promos));

            // Double pushes (only from pawns whose single push landed on checking rank)
            uint64 doubleDsts = ((chance == WHITE) ? northOne(dsts & checkingRankDoublePush) :
                                                     southOne(dsts & checkingRankDoublePush)) & emptySquares & safeSquares;
            nMoves += popCount(doubleDsts);

            // Captures (West and East)
            uint64 westCaps = ((chance == WHITE) ? northWestOne(myPawns) : southWestOne(myPawns)) & enemyPieces & safeSquares;
            promos = westCaps & (RANK1 | RANK8);
            nMoves += popCount(westCaps ^ promos) + (4 * popCount(promos));

            uint64 eastCaps = ((chance == WHITE) ? northEastOne(myPawns) : southEastOne(myPawns)) & enemyPieces & safeSquares;
            promos = eastCaps & (RANK1 | RANK8);
            nMoves += popCount(eastCaps ^ promos) + (4 * popCount(promos));

            // En-passent evasion (fast early exit — EP in check is extremely rare)
            uint64 enPassentTarget = getEpTarget<chance>(gs->enPassent);
            if (enPassentTarget)
            {
                uint64 enPassentCapturedPiece = (chance == WHITE) ? southOne(enPassentTarget) : northOne(enPassentTarget);
                if (enPassentCapturedPiece == attackers)
                {
                    uint64 epSources = ((chance == WHITE) ? northWestOne(myPawns) : southWestOne(myPawns)) & enPassentTarget;
                    epSources |= ((chance == WHITE) ? northEastOne(myPawns) : southEastOne(myPawns)) & enPassentTarget;
                    nMoves += popCount(epSources);
                }
            }

            // 2. knight moves
            uint64 myKnights = (knights_ & myPieces);
            while (myKnights)
            {
                uint64 knight = getOne(myKnights);
                uint64 knightMoves = sqKnightAttacks(bitScan(knight)) & safeSquares;
                nMoves += popCount(knightMoves);
                myKnights ^= knight;
            }
            
            // 3. bishop moves
            uint64 bishops = bishopQueens_ & myPieces;
            while (bishops)
            {
                uint64 bishop = getOne(bishops);
                uint64 bishopMoves = bishopAttacks(bishop, ~allPieces) & safeSquares;

                nMoves += popCount(bishopMoves);
                bishops ^= bishop;
            }

            // 4. rook moves
            uint64 rooks = rookQueens_ & myPieces;
            while (rooks)
            {
                uint64 rook = getOne(rooks);
                uint64 rookMoves = rookAttacks(rook, ~allPieces) & safeSquares;

                nMoves += popCount(rookMoves);
                rooks ^= rook;
            }

        }   // end of if single attacker
        else
        {
            // multiple threats => only king moves possible
        }

        return nMoves;
    }



    // Phase 0 split: leaf body that assumes the king is NOT in check and takes
    // all bitboards already derived from the position. The wrapper countMoves<>
    // below does the derivation + in-check fallback and then delegates here.
    // countMovesFromDerived is CPU_FORCE_INLINE so when called from the wrapper
    // it produces the same code as the original monolithic countMoves; the
    // separate entry point exists so the upcoming pair-wise SIMD flush can
    // SIMD-derive bitboards once across two children and call this body twice
    // with the per-child derived state.
    template <uint8 chance, bool hasEP, bool hasMyCastle>
    CPU_FORCE_INLINE static uint32 countMovesFromDerived(
        QuadBitBoard *pos, GameState *gs,
        uint64 allPieces, uint64 myPieces, uint64 enemyPieces,
        uint64 allPawns, uint64 knights, uint64 bishopQueens, uint64 rookQueens,
        uint64 myKing, uint8 kingIndex, uint64 pinned, uint64 threatened)
    {
        (void)pos;  // unused in the not-in-check path
        uint32 nMoves = 0;

        // E28: same single-bit-king invariants used in the wrapper. Restating them
        // here keeps the optimization when this function is invoked directly from
        // the pair-wise SIMD flush (Phase 2), where the wrapper isn't on the path.
        __assume(myKing != 0);
        __assume((myKing & (myKing - 1)) == 0);

        uint64 enemyBishops = bishopQueens & enemyPieces;
        uint64 enemyRooks   = rookQueens & enemyPieces;
        uint64 emptySquares = ~allPieces;

        uint64 myPawns = allPawns & myPieces;

        // 0. en-passent — block is compile-eliminated when hasEP=false.
        if constexpr (hasEP)
        {
        uint64 enPassentTarget = getEpTarget<chance>(gs->enPassent);

        if (enPassentTarget) [[unlikely]]
        {
            uint64 enPassentCapturedPiece = (chance == WHITE) ? southOne(enPassentTarget) : northOne(enPassentTarget);

            uint64 epSources = (eastOne(enPassentCapturedPiece) | westOne(enPassentCapturedPiece)) & myPawns;

            while (epSources)
            {
                uint64 pawn = getOne(epSources);
                if (pawn & pinned)
                {
                    uint64 line = sqsInLine(bitScan(pawn), kingIndex);
                    if (enPassentTarget & line)
                    {
                        nMoves++;
                    }
                }
                else
                {
                    // Check if removing both pawns reveals a rook attack on king along the rank.
                    // Use magic rook lookup from king's square with modified occupancy
                    // instead of Kogge-Stone east/west attacks (~14 ALU ops → 1 magic lookup).
                    // Mask to king's rank only — vertical attacks are irrelevant here.
                    uint64 modifiedOcc = allPieces ^ enPassentCapturedPiece ^ pawn;
                    uint64 rankMask = RANK1 << ((kingIndex >> 3) * 8);
                    uint64 causesCheck = rookAttacks(myKing, ~modifiedOcc) & enemyRooks & rankMask;
                    if (!causesCheck)
                    {
                        nMoves++;
                    }
                }
                epSources ^= pawn;
            }
        }
        }  // end if constexpr (hasEP)

        // 1. pawn moves

        // checking rank for pawn double pushes
        uint64 checkingRankDoublePush = RANK3 << (chance * 24);           // rank 3 or rank 6

        // first deal with pinned pawns
        uint64 pinnedPawns = myPawns & pinned;

        while (pinnedPawns)
        {
            uint64 pawn = getOne(pinnedPawns);
            uint8 pawnIndex = bitScan(pawn);    // same as bitscan on pinnedPawns

            // the direction of the pin (mask containing all squares in the line joining the king and the current piece)
            uint64 line = sqsInLine(pawnIndex, kingIndex);

            // pawn push
            uint64 dst = ((chance == WHITE) ? northOne(pawn) : southOne(pawn)) & line & emptySquares;
            if (dst)
            {
                nMoves++;

                // double push (only possible if single push was possible)
                dst = ((chance == WHITE) ? northOne(dst & checkingRankDoublePush):
                                           southOne(dst & checkingRankDoublePush) ) & emptySquares;
                if (dst) 
                {
                    nMoves++;
                }
            }

            // captures
            // (either of them will be valid - if at all)
            dst  = ((chance == WHITE) ? northWestOne(pawn) : southWestOne(pawn)) & line;
            dst |= ((chance == WHITE) ? northEastOne(pawn) : southEastOne(pawn)) & line;
            
            if (dst & enemyPieces) 
            {
                if (dst & (RANK1 | RANK8))
                    nMoves += 4;    // promotion
                else
                    nMoves++;
            }

            pinnedPawns ^= pawn;  // same as &= ~pawn (but only when we know that the first set contain the element we want to clear)
        }

        myPawns = myPawns & ~pinned;

        // pawn push (accumulate all promotions to save POPCs)
        uint64 allPromotions = 0;
        uint64 dsts = ((chance == WHITE) ? northOne(myPawns) : southOne(myPawns)) & emptySquares;
        uint64 promos = dsts & (RANK1 | RANK8);
        allPromotions = promos;
        nMoves += popCount(dsts ^ promos);  // non-promotion pushes

        // double push (never promotions — rank 4/5 destinations)
        dsts = ((chance == WHITE) ? northOne(dsts & checkingRankDoublePush):
                                    southOne(dsts & checkingRankDoublePush) ) & emptySquares;
        nMoves += popCount(dsts);

        // captures
        dsts = ((chance == WHITE) ? northWestOne(myPawns) : southWestOne(myPawns)) & enemyPieces;
        promos = dsts & (RANK1 | RANK8);
        allPromotions |= promos;
        nMoves += popCount(dsts ^ promos);  // non-promotion captures

        dsts = ((chance == WHITE) ? northEastOne(myPawns) : southEastOne(myPawns)) & enemyPieces;
        promos = dsts & (RANK1 | RANK8);
        allPromotions |= promos;
        nMoves += popCount(dsts ^ promos);  // non-promotion captures

        // all promotions: 4 moves each (N/B/R/Q)
        nMoves += 4 * popCount(allPromotions);

        // generate castling moves (skipped at compile time when hasMyCastle=false)
        if constexpr (hasMyCastle)
        {
            if (chance == WHITE)
            {
                if ((gs->whiteCastle & CASTLE_FLAG_KING_SIDE) &&
                    !(F1G1 & allPieces) && !(F1G1 & threatened))
                    nMoves++;
                if ((gs->whiteCastle & CASTLE_FLAG_QUEEN_SIDE) &&
                    !(B1D1 & allPieces) && !(C1D1 & threatened))
                    nMoves++;
            }
            else
            {
                if ((gs->blackCastle & CASTLE_FLAG_KING_SIDE) &&
                    !(F8G8 & allPieces) && !(F8G8 & threatened))
                    nMoves++;
                if ((gs->blackCastle & CASTLE_FLAG_QUEEN_SIDE) &&
                    !(B8D8 & allPieces) && !(C8D8 & threatened))
                    nMoves++;
            }
        }

        // generate king moves
        uint64 kingMoves = sqKingAttacks(kingIndex);

        kingMoves &= ~(threatened | myPieces);
        nMoves += popCount(kingMoves);

        // generate knight moves (only non-pinned knights can move) — E22 form
        uint64 myKnights = (knights & myPieces) & ~pinned;
        while (myKnights)
        {
            uint8  sq = bitScan(myKnights);
            uint64 knightMoves = sqKnightAttacks(sq) & ~myPieces;
            nMoves += popCount(knightMoves);
            myKnights &= myKnights - 1;
        }


        // generate bishop (and queen) moves
        uint64 myBishops = bishopQueens & myPieces;

        // first deal with pinned bishops (pinmask refactor was reverted —
        // pinmaskD12 union can spuriously match a HV-pinned bishop's diagonal
        // moves, causing perft drift at depth >= 7). Kept in the
        // bishopAttacks(bishop, emptySquares) form because the E16 hoist
        // pattern on this cold path bloats the function and hurts i-cache.
        uint64 bishops = myBishops & pinned;
        while (bishops)
        {
            uint64 bishop = getOne(bishops);
            uint64 bishopMoves = bishopAttacks(bishop, emptySquares) & ~myPieces;
            bishopMoves &= sqsInLine(bitScan(bishop), kingIndex);
            nMoves += popCount(bishopMoves);
            bishops ^= bishop;
        }

        // remaining bishops/queens — E16 hoist (~myPieces, allPieces) + E18 split
        // arrays + E20 bitScan-direct + BLSR clear + E34 ivdep hint.
        uint64 notMyPieces = ~myPieces;
        bishops = myBishops & ~pinned;
#pragma loop(ivdep)
        while (bishops)
        {
            uint8  sq = bitScan(bishops);
            uint64 occ = allPieces & sqBishopAttacksMasked(sq);
            uint64 idx = (bishop_magic_factors[sq] * occ) >> (64 - BISHOP_MAGIC_BITS);
            uint64 bishopMoves = bishop_magic_tables[sq][idx] & notMyPieces;
            nMoves += popCount(bishopMoves);
            bishops &= bishops - 1;
        }

        // rook/queen moves
        uint64 myRooks = rookQueens & myPieces;

        // first deal with pinned rooks (pinmaskHV refactor reverted same as bishops above)
        uint64 rooks = myRooks & pinned;
        while (rooks)
        {
            uint64 rook = getOne(rooks);
            uint64 rookMoves = rookAttacks(rook, emptySquares) & ~myPieces;
            rookMoves &= sqsInLine(bitScan(rook), kingIndex);
            nMoves += popCount(rookMoves);
            rooks ^= rook;
        }

        // remaining rooks/queens — E16+E18+E20+E34 combined form.
        rooks = myRooks & ~pinned;
#pragma loop(ivdep)
        while (rooks)
        {
            uint8  sq = bitScan(rooks);
            uint64 occ = allPieces & sqRookAttacksMasked(sq);
            uint64 idx = (rook_magic_factors[sq] * occ) >> (64 - ROOK_MAGIC_BITS);
            uint64 rookMoves = rook_magic_tables[sq][idx] & notMyPieces;
            nMoves += popCount(rookMoves);
            rooks &= rooks - 1;
        }

        return nMoves;
    }

    // count moves for the given board position
    // returns the no of moves generated.
    // hasEP / hasMyCastle are optional compile-time hints. When the caller knows the
    // child position can't have an EP target or castle rights for `chance`, passing
    // false skips the corresponding code blocks entirely.
    //
    // __declspec(noinline): WITHOUT LTCG, inlining this ~7KB function into
    // the 6-piece × 4-state emit specialisations explodes i-cache pressure
    // (measured: 14× slowdown). Stay out-of-line.
    template <uint8 chance, bool hasEP = true, bool hasMyCastle = true>
    __declspec(noinline)
    static uint32 countMoves (QuadBitBoard *pos, GameState *gs)
    {
        DERIVE_PIECE_BITBOARDS(pos);

        uint64 myPieces     = (chance == WHITE) ? whitePieces : blackPieces;
        uint64 enemyPieces  = (chance == WHITE) ? blackPieces : whitePieces;

        uint64 enemyBishops = bishopQueens & enemyPieces;
        uint64 enemyRooks   = rookQueens & enemyPieces;

        uint64 myKing     = kings & myPieces;
        // E28: tell MSVC that myKing is exactly one bit (always true in any
        // legal chess position). Helped ~1.6% on perft 6.
        __assume(myKing != 0);
        __assume((myKing & (myKing - 1)) == 0);
        uint8  kingIndex  = bitScan(myKing);

        uint64 pinned     = findPinnedPieces(myKing, myPieces, enemyBishops, enemyRooks,
                                             allPieces, kingIndex);

        uint64 threatened = findAttackedSquares(~allPieces, enemyBishops, enemyRooks, allPawns & enemyPieces,
                                                knights & enemyPieces, kings & enemyPieces,
                                                myKing, !chance, kingIndex);

        // king is in check (uncommon — most leaves are quiet positions)
        if (threatened & myKing) [[unlikely]]
        {
            return countMovesOutOfCheck<chance>(pos, gs, allPawns, allPieces, myPieces, enemyPieces,
                                                              pinned, threatened, kingIndex,
                                                              knights, bishopQueens, rookQueens, kings);
        }

        return countMovesFromDerived<chance, hasEP, hasMyCastle>(
            pos, gs,
            allPieces, myPieces, enemyPieces,
            allPawns, knights, bishopQueens, rookQueens,
            myKing, kingIndex, pinned, threatened);
    }

    // Pair-wise leaf counter. Called from FgmcCount2Processor::flush() in the
    // common (most leaves) path — for the in-check rare path each child still
    // falls back to countMovesOutOfCheck.
    //
    // What's actually pair-wise SIMD-equivalent:
    //   - findAttackedSquaresPair interleaves the two enemy-slider iteration
    //     chains for ILP — biggest single line of countMoves by profile.
    // What stays per-child:
    //   - DERIVE_PIECE_BITBOARDS (cheap, ~10 cycles)
    //   - findPinnedPieces (3% of profile)
    //   - the entire post-attack leaf body (pawn/knight/king/slider move counts)
    //
    // __declspec(noinline) on the pair function — same i-cache reasoning as
    // countMoves: each call inlines a substantial leaf body twice (one per
    // child), and we don't want that inlined into flush()'s caller chain.
    // E7/E8: templated on the moved piece type at parent (= piece type whose
    // attacks need recomputation at the leaf). `recomputedPiece` = 0 means
    // parent move was a slider — nothing to recompute; the cachedNonSliderAtk
    // is the full pawn|knight|king map. For PAWN/KNIGHT/KING, the leaf
    // recomputes that piece's enemy attacks from leaf state and ORs in.
    template <uint8 chance, uint8 recomputedPiece>
    __declspec(noinline)
    static uint64 countMovesPairCached(QuadBitBoard *posA, GameState *gsA,
                                       QuadBitBoard *posB, GameState *gsB,
                                       uint64 cachedNonSliderAtk)
    {
        // Derive A
        uint64 allPiecesA    = posA->bb[1] | posA->bb[2] | posA->bb[3];
        uint64 blackPiecesA  = posA->bb[0];
        uint64 whitePiecesA  = allPiecesA & ~blackPiecesA;
        uint64 allPawnsA     = posA->bb[1] & ~posA->bb[2] & ~posA->bb[3];
        uint64 knightsA      = posA->bb[2] & ~posA->bb[1] & ~posA->bb[3];
        uint64 bishopQueensA = posA->bb[1] & (posA->bb[2] ^ posA->bb[3]);
        uint64 rookQueensA   = posA->bb[3] & ~posA->bb[2];
        uint64 kingsA        = posA->bb[2] & posA->bb[3] & ~posA->bb[1];

        // Derive B
        uint64 allPiecesB    = posB->bb[1] | posB->bb[2] | posB->bb[3];
        uint64 blackPiecesB  = posB->bb[0];
        uint64 whitePiecesB  = allPiecesB & ~blackPiecesB;
        uint64 allPawnsB     = posB->bb[1] & ~posB->bb[2] & ~posB->bb[3];
        uint64 knightsB      = posB->bb[2] & ~posB->bb[1] & ~posB->bb[3];
        uint64 bishopQueensB = posB->bb[1] & (posB->bb[2] ^ posB->bb[3]);
        uint64 rookQueensB   = posB->bb[3] & ~posB->bb[2];
        uint64 kingsB        = posB->bb[2] & posB->bb[3] & ~posB->bb[1];

        uint64 myPiecesA    = (chance == WHITE) ? whitePiecesA : blackPiecesA;
        uint64 enemyPiecesA = (chance == WHITE) ? blackPiecesA : whitePiecesA;
        uint64 myPiecesB    = (chance == WHITE) ? whitePiecesB : blackPiecesB;
        uint64 enemyPiecesB = (chance == WHITE) ? blackPiecesB : whitePiecesB;

        uint64 enemyBishopsA = bishopQueensA & enemyPiecesA;
        uint64 enemyRooksA   = rookQueensA   & enemyPiecesA;
        uint64 enemyBishopsB = bishopQueensB & enemyPiecesB;
        uint64 enemyRooksB   = rookQueensB   & enemyPiecesB;

        uint64 myKingA = kingsA & myPiecesA;
        uint64 myKingB = kingsB & myPiecesB;
        __assume(myKingA != 0); __assume((myKingA & (myKingA - 1)) == 0);
        __assume(myKingB != 0); __assume((myKingB & (myKingB - 1)) == 0);
        uint8 kingIndexA = bitScan(myKingA);
        uint8 kingIndexB = bitScan(myKingB);

        // Recompute the moved piece type's attacks at leaf state, OR with cache.
        uint64 extraAtkA = 0, extraAtkB = 0;
        if constexpr (recomputedPiece == PAWN) {
            uint64 enemyPawnsA = allPawnsA & enemyPiecesA;
            uint64 enemyPawnsB = allPawnsB & enemyPiecesB;
            // leaf's enemy is !chance = parentChance. enemy pawn attacks shift
            // toward leaf's side: enemy=WHITE shifts up (NE/NW), enemy=BLACK shifts down (SE/SW).
            if constexpr (chance == BLACK) {  // enemy = WHITE
                extraAtkA = northEastOne(enemyPawnsA) | northWestOne(enemyPawnsA);
                extraAtkB = northEastOne(enemyPawnsB) | northWestOne(enemyPawnsB);
            } else {                          // enemy = BLACK
                extraAtkA = southEastOne(enemyPawnsA) | southWestOne(enemyPawnsA);
                extraAtkB = southEastOne(enemyPawnsB) | southWestOne(enemyPawnsB);
            }
        } else if constexpr (recomputedPiece == KNIGHT) {
            extraAtkA = knightAttacks(knightsA & enemyPiecesA);
            extraAtkB = knightAttacks(knightsB & enemyPiecesB);
        } else if constexpr (recomputedPiece == KING) {
            extraAtkA = kingAttacks(kingsA & enemyPiecesA);
            extraAtkB = kingAttacks(kingsB & enemyPiecesB);
        }
        // Slider-only attack computation, OR'd with the cached non-slider attacks
        // and the recomputed moved-piece-type's attacks.
        uint64 threatA = cachedNonSliderAtk | extraAtkA |
                         findSliderAttacksOnly(~allPiecesA, enemyBishopsA, enemyRooksA, myKingA, kingIndexA);
        uint64 threatB = cachedNonSliderAtk | extraAtkB |
                         findSliderAttacksOnly(~allPiecesB, enemyBishopsB, enemyRooksB, myKingB, kingIndexB);

        uint64 sum = 0;
        {
            uint64 pinnedA = findPinnedPieces(myKingA, myPiecesA, enemyBishopsA, enemyRooksA, allPiecesA, kingIndexA);
            if (threatA & myKingA) [[unlikely]]
            {
                sum += countMovesOutOfCheck<chance>(posA, gsA, allPawnsA, allPiecesA, myPiecesA, enemyPiecesA,
                                                   pinnedA, threatA, kingIndexA,
                                                   knightsA, bishopQueensA, rookQueensA, kingsA);
            }
            else
            {
                bool hasEP = gsA->enPassent != 0;
                bool hasCastle = (chance == WHITE) ? (gsA->whiteCastle != 0) : (gsA->blackCastle != 0);
                if (!hasEP)
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, false, true >(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                    else           sum += countMovesFromDerived<chance, false, false>(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                }
                else
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, true,  true >(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                    else           sum += countMovesFromDerived<chance, true,  false>(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                }
            }
        }
        {
            uint64 pinnedB = findPinnedPieces(myKingB, myPiecesB, enemyBishopsB, enemyRooksB, allPiecesB, kingIndexB);
            if (threatB & myKingB) [[unlikely]]
            {
                sum += countMovesOutOfCheck<chance>(posB, gsB, allPawnsB, allPiecesB, myPiecesB, enemyPiecesB,
                                                   pinnedB, threatB, kingIndexB,
                                                   knightsB, bishopQueensB, rookQueensB, kingsB);
            }
            else
            {
                bool hasEP = gsB->enPassent != 0;
                bool hasCastle = (chance == WHITE) ? (gsB->whiteCastle != 0) : (gsB->blackCastle != 0);
                if (!hasEP)
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, false, true >(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                    else           sum += countMovesFromDerived<chance, false, false>(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                }
                else
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, true,  true >(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                    else           sum += countMovesFromDerived<chance, true,  false>(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                }
            }
        }
        return sum;
    }

    // 4-wide quad version. NOT CALLED — fast-path quad regressed +2 ms with PGO
    // (E69: i-cache pressure + register pressure with 24+ live uint64 lanes).
    // Slow-path quad also regressed (E71). Retained as dead reference.
    template <uint8 chance>
    __declspec(noinline)
    static uint64 countMovesQuadCached_UNUSED(
        QuadBitBoard *posA, GameState *gsA,
        QuadBitBoard *posB, GameState *gsB,
        QuadBitBoard *posC, GameState *gsC,
        QuadBitBoard *posD, GameState *gsD,
        uint64 cachedNonSliderAtk)
    {
        // 4 derives. Stays scalar — SIMD-paired derive was tried (E12) and
        // regressed -2 ms due to XMM pack/extract overhead vs 4-port scalar.
        uint64 allPiecesA    = posA->bb[1] | posA->bb[2] | posA->bb[3];
        uint64 allPiecesB    = posB->bb[1] | posB->bb[2] | posB->bb[3];
        uint64 allPiecesC    = posC->bb[1] | posC->bb[2] | posC->bb[3];
        uint64 allPiecesD    = posD->bb[1] | posD->bb[2] | posD->bb[3];
        uint64 blackPiecesA  = posA->bb[0];
        uint64 blackPiecesB  = posB->bb[0];
        uint64 blackPiecesC  = posC->bb[0];
        uint64 blackPiecesD  = posD->bb[0];
        uint64 allPawnsA     = posA->bb[1] & ~posA->bb[2] & ~posA->bb[3];
        uint64 allPawnsB     = posB->bb[1] & ~posB->bb[2] & ~posB->bb[3];
        uint64 allPawnsC     = posC->bb[1] & ~posC->bb[2] & ~posC->bb[3];
        uint64 allPawnsD     = posD->bb[1] & ~posD->bb[2] & ~posD->bb[3];
        uint64 knightsA      = posA->bb[2] & ~posA->bb[1] & ~posA->bb[3];
        uint64 knightsB      = posB->bb[2] & ~posB->bb[1] & ~posB->bb[3];
        uint64 knightsC      = posC->bb[2] & ~posC->bb[1] & ~posC->bb[3];
        uint64 knightsD      = posD->bb[2] & ~posD->bb[1] & ~posD->bb[3];
        uint64 bishopQueensA = posA->bb[1] & (posA->bb[2] ^ posA->bb[3]);
        uint64 bishopQueensB = posB->bb[1] & (posB->bb[2] ^ posB->bb[3]);
        uint64 bishopQueensC = posC->bb[1] & (posC->bb[2] ^ posC->bb[3]);
        uint64 bishopQueensD = posD->bb[1] & (posD->bb[2] ^ posD->bb[3]);
        uint64 rookQueensA   = posA->bb[3] & ~posA->bb[2];
        uint64 rookQueensB   = posB->bb[3] & ~posB->bb[2];
        uint64 rookQueensC   = posC->bb[3] & ~posC->bb[2];
        uint64 rookQueensD   = posD->bb[3] & ~posD->bb[2];
        uint64 kingsA        = posA->bb[2] & posA->bb[3] & ~posA->bb[1];
        uint64 kingsB        = posB->bb[2] & posB->bb[3] & ~posB->bb[1];
        uint64 kingsC        = posC->bb[2] & posC->bb[3] & ~posC->bb[1];
        uint64 kingsD        = posD->bb[2] & posD->bb[3] & ~posD->bb[1];
        uint64 whitePiecesA = allPiecesA & ~blackPiecesA;
        uint64 whitePiecesB = allPiecesB & ~blackPiecesB;
        uint64 whitePiecesC = allPiecesC & ~blackPiecesC;
        uint64 whitePiecesD = allPiecesD & ~blackPiecesD;

        uint64 myPiecesA    = (chance == WHITE) ? whitePiecesA : blackPiecesA;
        uint64 enemyPiecesA = (chance == WHITE) ? blackPiecesA : whitePiecesA;
        uint64 myPiecesB    = (chance == WHITE) ? whitePiecesB : blackPiecesB;
        uint64 enemyPiecesB = (chance == WHITE) ? blackPiecesB : whitePiecesB;
        uint64 myPiecesC    = (chance == WHITE) ? whitePiecesC : blackPiecesC;
        uint64 enemyPiecesC = (chance == WHITE) ? blackPiecesC : whitePiecesC;
        uint64 myPiecesD    = (chance == WHITE) ? whitePiecesD : blackPiecesD;
        uint64 enemyPiecesD = (chance == WHITE) ? blackPiecesD : whitePiecesD;

        uint64 enemyBishopsA = bishopQueensA & enemyPiecesA;
        uint64 enemyRooksA   = rookQueensA   & enemyPiecesA;
        uint64 enemyBishopsB = bishopQueensB & enemyPiecesB;
        uint64 enemyRooksB   = rookQueensB   & enemyPiecesB;
        uint64 enemyBishopsC = bishopQueensC & enemyPiecesC;
        uint64 enemyRooksC   = rookQueensC   & enemyPiecesC;
        uint64 enemyBishopsD = bishopQueensD & enemyPiecesD;
        uint64 enemyRooksD   = rookQueensD   & enemyPiecesD;

        uint64 myKingA = kingsA & myPiecesA;
        uint64 myKingB = kingsB & myPiecesB;
        uint64 myKingC = kingsC & myPiecesC;
        uint64 myKingD = kingsD & myPiecesD;
        __assume(myKingA != 0); __assume((myKingA & (myKingA - 1)) == 0);
        __assume(myKingB != 0); __assume((myKingB & (myKingB - 1)) == 0);
        __assume(myKingC != 0); __assume((myKingC & (myKingC - 1)) == 0);
        __assume(myKingD != 0); __assume((myKingD & (myKingD - 1)) == 0);
        uint8 kingIndexA = bitScan(myKingA);
        uint8 kingIndexB = bitScan(myKingB);
        uint8 kingIndexC = bitScan(myKingC);
        uint8 kingIndexD = bitScan(myKingD);

        uint64 sliderAtkA, sliderAtkB, sliderAtkC, sliderAtkD;
        findSliderAttacksOnly4(
            ~allPiecesA, enemyBishopsA, enemyRooksA, myKingA,
            ~allPiecesB, enemyBishopsB, enemyRooksB, myKingB,
            ~allPiecesC, enemyBishopsC, enemyRooksC, myKingC,
            ~allPiecesD, enemyBishopsD, enemyRooksD, myKingD,
            sliderAtkA, sliderAtkB, sliderAtkC, sliderAtkD);

        uint64 threatA = cachedNonSliderAtk | sliderAtkA;
        uint64 threatB = cachedNonSliderAtk | sliderAtkB;
        uint64 threatC = cachedNonSliderAtk | sliderAtkC;
        uint64 threatD = cachedNonSliderAtk | sliderAtkD;

        uint64 pinnedA = findPinnedPieces(myKingA, myPiecesA, enemyBishopsA, enemyRooksA, allPiecesA, kingIndexA);
        uint64 pinnedB = findPinnedPieces(myKingB, myPiecesB, enemyBishopsB, enemyRooksB, allPiecesB, kingIndexB);
        uint64 pinnedC = findPinnedPieces(myKingC, myPiecesC, enemyBishopsC, enemyRooksC, allPiecesC, kingIndexC);
        uint64 pinnedD = findPinnedPieces(myKingD, myPiecesD, enemyBishopsD, enemyRooksD, allPiecesD, kingIndexD);

        uint64 sum = 0;

        #define QC_DISPATCH_LANE(L) do { \
            if (threat##L & myKing##L) [[unlikely]] { \
                sum += countMovesOutOfCheck<chance>(pos##L, gs##L, allPawns##L, allPieces##L, myPieces##L, enemyPieces##L, \
                                                    pinned##L, threat##L, kingIndex##L, \
                                                    knights##L, bishopQueens##L, rookQueens##L, kings##L); \
            } else { \
                bool hasEP = gs##L->enPassent != 0; \
                bool hasCastle = (chance == WHITE) ? (gs##L->whiteCastle != 0) : (gs##L->blackCastle != 0); \
                if (!hasEP) { \
                    if (hasCastle) sum += countMovesFromDerived<chance, false, true >(pos##L, gs##L, allPieces##L, myPieces##L, enemyPieces##L, allPawns##L, knights##L, bishopQueens##L, rookQueens##L, myKing##L, kingIndex##L, pinned##L, threat##L); \
                    else           sum += countMovesFromDerived<chance, false, false>(pos##L, gs##L, allPieces##L, myPieces##L, enemyPieces##L, allPawns##L, knights##L, bishopQueens##L, rookQueens##L, myKing##L, kingIndex##L, pinned##L, threat##L); \
                } else { \
                    if (hasCastle) sum += countMovesFromDerived<chance, true,  true >(pos##L, gs##L, allPieces##L, myPieces##L, enemyPieces##L, allPawns##L, knights##L, bishopQueens##L, rookQueens##L, myKing##L, kingIndex##L, pinned##L, threat##L); \
                    else           sum += countMovesFromDerived<chance, true,  false>(pos##L, gs##L, allPieces##L, myPieces##L, enemyPieces##L, allPawns##L, knights##L, bishopQueens##L, rookQueens##L, myKing##L, kingIndex##L, pinned##L, threat##L); \
                } \
            } \
        } while (0)

        QC_DISPATCH_LANE(A);
        QC_DISPATCH_LANE(B);
        QC_DISPATCH_LANE(C);
        QC_DISPATCH_LANE(D);
        #undef QC_DISPATCH_LANE

        return sum;
    }

    template <uint8 chance>
    __declspec(noinline)
    static uint64 countMovesPair(QuadBitBoard *posA, GameState *gsA, QuadBitBoard *posB, GameState *gsB)
    {

        // Derive A
        uint64 allPiecesA    = posA->bb[1] | posA->bb[2] | posA->bb[3];
        uint64 blackPiecesA  = posA->bb[0];
        uint64 whitePiecesA  = allPiecesA & ~blackPiecesA;
        uint64 allPawnsA     = posA->bb[1] & ~posA->bb[2] & ~posA->bb[3];
        uint64 knightsA      = posA->bb[2] & ~posA->bb[1] & ~posA->bb[3];
        uint64 bishopQueensA = posA->bb[1] & (posA->bb[2] ^ posA->bb[3]);
        uint64 rookQueensA   = posA->bb[3] & ~posA->bb[2];
        uint64 kingsA        = posA->bb[2] & posA->bb[3] & ~posA->bb[1];

        // Derive B
        uint64 allPiecesB    = posB->bb[1] | posB->bb[2] | posB->bb[3];
        uint64 blackPiecesB  = posB->bb[0];
        uint64 whitePiecesB  = allPiecesB & ~blackPiecesB;
        uint64 allPawnsB     = posB->bb[1] & ~posB->bb[2] & ~posB->bb[3];
        uint64 knightsB      = posB->bb[2] & ~posB->bb[1] & ~posB->bb[3];
        uint64 bishopQueensB = posB->bb[1] & (posB->bb[2] ^ posB->bb[3]);
        uint64 rookQueensB   = posB->bb[3] & ~posB->bb[2];
        uint64 kingsB        = posB->bb[2] & posB->bb[3] & ~posB->bb[1];

        uint64 myPiecesA    = (chance == WHITE) ? whitePiecesA : blackPiecesA;
        uint64 enemyPiecesA = (chance == WHITE) ? blackPiecesA : whitePiecesA;
        uint64 myPiecesB    = (chance == WHITE) ? whitePiecesB : blackPiecesB;
        uint64 enemyPiecesB = (chance == WHITE) ? blackPiecesB : whitePiecesB;

        uint64 enemyBishopsA = bishopQueensA & enemyPiecesA;
        uint64 enemyRooksA   = rookQueensA   & enemyPiecesA;
        uint64 enemyBishopsB = bishopQueensB & enemyPiecesB;
        uint64 enemyRooksB   = rookQueensB   & enemyPiecesB;

        uint64 myKingA = kingsA & myPiecesA;
        uint64 myKingB = kingsB & myPiecesB;
        __assume(myKingA != 0); __assume((myKingA & (myKingA - 1)) == 0);
        __assume(myKingB != 0); __assume((myKingB & (myKingB - 1)) == 0);
        uint8 kingIndexA = bitScan(myKingA);
        uint8 kingIndexB = bitScan(myKingB);

        // Two sequential findAttackedSquares calls inside the same function
        // body. A fused-loop version with per-iter `if (sA){} if (sB){}`
        // interleaving was tried but the per-iter branches plus extra register
        // pressure cost ~2 ms more than two clean back-to-back calls. The pair
        // function still wins from function-call amortization and from giving
        // MSVC's OoO scheduler a single hot frame instead of two.
        uint64 threatA = findAttackedSquares(~allPiecesA, enemyBishopsA, enemyRooksA, allPawnsA & enemyPiecesA, knightsA & enemyPiecesA, kingsA & enemyPiecesA, myKingA, !chance, kingIndexA);
        uint64 threatB = findAttackedSquares(~allPiecesB, enemyBishopsB, enemyRooksB, allPawnsB & enemyPiecesB, knightsB & enemyPiecesB, kingsB & enemyPiecesB, myKingB, !chance, kingIndexB);

        // Per-child pinned + dispatch.
        uint64 sum = 0;
        {
            uint64 pinnedA = findPinnedPieces(myKingA, myPiecesA, enemyBishopsA, enemyRooksA, allPiecesA, kingIndexA);
            if (threatA & myKingA) [[unlikely]]
            {
                sum += countMovesOutOfCheck<chance>(posA, gsA, allPawnsA, allPiecesA, myPiecesA, enemyPiecesA,
                                                   pinnedA, threatA, kingIndexA,
                                                   knightsA, bishopQueensA, rookQueensA, kingsA);
            }
            else
            {
                bool hasEP = gsA->enPassent != 0;
                bool hasCastle = (chance == WHITE) ? (gsA->whiteCastle != 0) : (gsA->blackCastle != 0);
                if (!hasEP)
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, false, true >(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                    else           sum += countMovesFromDerived<chance, false, false>(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                }
                else
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, true,  true >(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                    else           sum += countMovesFromDerived<chance, true,  false>(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                }
            }
        }
        {
            uint64 pinnedB = findPinnedPieces(myKingB, myPiecesB, enemyBishopsB, enemyRooksB, allPiecesB, kingIndexB);
            if (threatB & myKingB) [[unlikely]]
            {
                sum += countMovesOutOfCheck<chance>(posB, gsB, allPawnsB, allPiecesB, myPiecesB, enemyPiecesB,
                                                   pinnedB, threatB, kingIndexB,
                                                   knightsB, bishopQueensB, rookQueensB, kingsB);
            }
            else
            {
                bool hasEP = gsB->enPassent != 0;
                bool hasCastle = (chance == WHITE) ? (gsB->whiteCastle != 0) : (gsB->blackCastle != 0);
                if (!hasEP)
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, false, true >(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                    else           sum += countMovesFromDerived<chance, false, false>(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                }
                else
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, true,  true >(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                    else           sum += countMovesFromDerived<chance, true,  false>(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                }
            }
        }
        return sum;
    }

    // Phase 4: slow-path pair variant that takes pre-computed per-child enemy
    // non-slider attacks (pawn|knight|king) from the AVX2 SIMD batch helper.
    // Body is `countMovesPair` minus the inline `findAttackedSquares` calls
    // (replaced with `findSliderAttacksOnly | nonSliderAtk_X`).
    template <uint8 chance>
    __declspec(noinline)
    static uint64 countMovesPairWithAtk(
        QuadBitBoard *posA, GameState *gsA,
        QuadBitBoard *posB, GameState *gsB,
        uint64 nonSliderAtkA, uint64 nonSliderAtkB)
    {
        // Derive A
        uint64 allPiecesA    = posA->bb[1] | posA->bb[2] | posA->bb[3];
        uint64 blackPiecesA  = posA->bb[0];
        uint64 whitePiecesA  = allPiecesA & ~blackPiecesA;
        uint64 allPawnsA     = posA->bb[1] & ~posA->bb[2] & ~posA->bb[3];
        uint64 knightsA      = posA->bb[2] & ~posA->bb[1] & ~posA->bb[3];
        uint64 bishopQueensA = posA->bb[1] & (posA->bb[2] ^ posA->bb[3]);
        uint64 rookQueensA   = posA->bb[3] & ~posA->bb[2];
        uint64 kingsA        = posA->bb[2] & posA->bb[3] & ~posA->bb[1];

        // Derive B
        uint64 allPiecesB    = posB->bb[1] | posB->bb[2] | posB->bb[3];
        uint64 blackPiecesB  = posB->bb[0];
        uint64 whitePiecesB  = allPiecesB & ~blackPiecesB;
        uint64 allPawnsB     = posB->bb[1] & ~posB->bb[2] & ~posB->bb[3];
        uint64 knightsB      = posB->bb[2] & ~posB->bb[1] & ~posB->bb[3];
        uint64 bishopQueensB = posB->bb[1] & (posB->bb[2] ^ posB->bb[3]);
        uint64 rookQueensB   = posB->bb[3] & ~posB->bb[2];
        uint64 kingsB        = posB->bb[2] & posB->bb[3] & ~posB->bb[1];

        uint64 myPiecesA    = (chance == WHITE) ? whitePiecesA : blackPiecesA;
        uint64 enemyPiecesA = (chance == WHITE) ? blackPiecesA : whitePiecesA;
        uint64 myPiecesB    = (chance == WHITE) ? whitePiecesB : blackPiecesB;
        uint64 enemyPiecesB = (chance == WHITE) ? blackPiecesB : whitePiecesB;

        uint64 enemyBishopsA = bishopQueensA & enemyPiecesA;
        uint64 enemyRooksA   = rookQueensA   & enemyPiecesA;
        uint64 enemyBishopsB = bishopQueensB & enemyPiecesB;
        uint64 enemyRooksB   = rookQueensB   & enemyPiecesB;

        uint64 myKingA = kingsA & myPiecesA;
        uint64 myKingB = kingsB & myPiecesB;
        __assume(myKingA != 0); __assume((myKingA & (myKingA - 1)) == 0);
        __assume(myKingB != 0); __assume((myKingB & (myKingB - 1)) == 0);
        uint8 kingIndexA = bitScan(myKingA);
        uint8 kingIndexB = bitScan(myKingB);

        // Slider-only attacks, OR'd with the SIMD-precomputed non-slider attacks.
        uint64 threatA = nonSliderAtkA | findSliderAttacksOnly(~allPiecesA, enemyBishopsA, enemyRooksA, myKingA, kingIndexA);
        uint64 threatB = nonSliderAtkB | findSliderAttacksOnly(~allPiecesB, enemyBishopsB, enemyRooksB, myKingB, kingIndexB);

        uint64 sum = 0;
        {
            uint64 pinnedA = findPinnedPieces(myKingA, myPiecesA, enemyBishopsA, enemyRooksA, allPiecesA, kingIndexA);
            if (threatA & myKingA) [[unlikely]]
            {
                sum += countMovesOutOfCheck<chance>(posA, gsA, allPawnsA, allPiecesA, myPiecesA, enemyPiecesA,
                                                   pinnedA, threatA, kingIndexA,
                                                   knightsA, bishopQueensA, rookQueensA, kingsA);
            }
            else
            {
                bool hasEP = gsA->enPassent != 0;
                bool hasCastle = (chance == WHITE) ? (gsA->whiteCastle != 0) : (gsA->blackCastle != 0);
                if (!hasEP)
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, false, true >(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                    else           sum += countMovesFromDerived<chance, false, false>(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                }
                else
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, true,  true >(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                    else           sum += countMovesFromDerived<chance, true,  false>(posA, gsA, allPiecesA, myPiecesA, enemyPiecesA, allPawnsA, knightsA, bishopQueensA, rookQueensA, myKingA, kingIndexA, pinnedA, threatA);
                }
            }
        }
        {
            uint64 pinnedB = findPinnedPieces(myKingB, myPiecesB, enemyBishopsB, enemyRooksB, allPiecesB, kingIndexB);
            if (threatB & myKingB) [[unlikely]]
            {
                sum += countMovesOutOfCheck<chance>(posB, gsB, allPawnsB, allPiecesB, myPiecesB, enemyPiecesB,
                                                   pinnedB, threatB, kingIndexB,
                                                   knightsB, bishopQueensB, rookQueensB, kingsB);
            }
            else
            {
                bool hasEP = gsB->enPassent != 0;
                bool hasCastle = (chance == WHITE) ? (gsB->whiteCastle != 0) : (gsB->blackCastle != 0);
                if (!hasEP)
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, false, true >(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                    else           sum += countMovesFromDerived<chance, false, false>(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                }
                else
                {
                    if (hasCastle) sum += countMovesFromDerived<chance, true,  true >(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                    else           sum += countMovesFromDerived<chance, true,  false>(posB, gsB, allPiecesB, myPiecesB, enemyPiecesB, allPawnsB, knightsB, bishopQueensB, rookQueensB, myKingB, kingIndexB, pinnedB, threatB);
                }
            }
        }
        return sum;
    }

    // Phase 4+5 attempt: NOT CALLED — slow-path quad regressed +2.4 ms vs the
    // Phase 4 pair-wise slow path (E71: same i-cache mechanism as the fast quad).
    template <uint8 chance>
    __declspec(noinline)
    static uint64 countMovesQuadWithAtk_UNUSED(
        QuadBitBoard *posA, GameState *gsA,
        QuadBitBoard *posB, GameState *gsB,
        QuadBitBoard *posC, GameState *gsC,
        QuadBitBoard *posD, GameState *gsD,
        uint64 atkA, uint64 atkB, uint64 atkC, uint64 atkD)
    {
        // 4 derives (scalar, MSVC schedules across 4 ALU ports).
        uint64 allPiecesA    = posA->bb[1] | posA->bb[2] | posA->bb[3];
        uint64 allPiecesB    = posB->bb[1] | posB->bb[2] | posB->bb[3];
        uint64 allPiecesC    = posC->bb[1] | posC->bb[2] | posC->bb[3];
        uint64 allPiecesD    = posD->bb[1] | posD->bb[2] | posD->bb[3];
        uint64 allPawnsA     = posA->bb[1] & ~posA->bb[2] & ~posA->bb[3];
        uint64 allPawnsB     = posB->bb[1] & ~posB->bb[2] & ~posB->bb[3];
        uint64 allPawnsC     = posC->bb[1] & ~posC->bb[2] & ~posC->bb[3];
        uint64 allPawnsD     = posD->bb[1] & ~posD->bb[2] & ~posD->bb[3];
        uint64 knightsA      = posA->bb[2] & ~posA->bb[1] & ~posA->bb[3];
        uint64 knightsB      = posB->bb[2] & ~posB->bb[1] & ~posB->bb[3];
        uint64 knightsC      = posC->bb[2] & ~posC->bb[1] & ~posC->bb[3];
        uint64 knightsD      = posD->bb[2] & ~posD->bb[1] & ~posD->bb[3];
        uint64 bishopQueensA = posA->bb[1] & (posA->bb[2] ^ posA->bb[3]);
        uint64 bishopQueensB = posB->bb[1] & (posB->bb[2] ^ posB->bb[3]);
        uint64 bishopQueensC = posC->bb[1] & (posC->bb[2] ^ posC->bb[3]);
        uint64 bishopQueensD = posD->bb[1] & (posD->bb[2] ^ posD->bb[3]);
        uint64 rookQueensA   = posA->bb[3] & ~posA->bb[2];
        uint64 rookQueensB   = posB->bb[3] & ~posB->bb[2];
        uint64 rookQueensC   = posC->bb[3] & ~posC->bb[2];
        uint64 rookQueensD   = posD->bb[3] & ~posD->bb[2];
        uint64 kingsA        = posA->bb[2] & posA->bb[3] & ~posA->bb[1];
        uint64 kingsB        = posB->bb[2] & posB->bb[3] & ~posB->bb[1];
        uint64 kingsC        = posC->bb[2] & posC->bb[3] & ~posC->bb[1];
        uint64 kingsD        = posD->bb[2] & posD->bb[3] & ~posD->bb[1];
        uint64 blackPiecesA  = posA->bb[0];
        uint64 blackPiecesB  = posB->bb[0];
        uint64 blackPiecesC  = posC->bb[0];
        uint64 blackPiecesD  = posD->bb[0];
        uint64 whitePiecesA  = allPiecesA & ~blackPiecesA;
        uint64 whitePiecesB  = allPiecesB & ~blackPiecesB;
        uint64 whitePiecesC  = allPiecesC & ~blackPiecesC;
        uint64 whitePiecesD  = allPiecesD & ~blackPiecesD;

        uint64 myPiecesA    = (chance == WHITE) ? whitePiecesA : blackPiecesA;
        uint64 enemyPiecesA = (chance == WHITE) ? blackPiecesA : whitePiecesA;
        uint64 myPiecesB    = (chance == WHITE) ? whitePiecesB : blackPiecesB;
        uint64 enemyPiecesB = (chance == WHITE) ? blackPiecesB : whitePiecesB;
        uint64 myPiecesC    = (chance == WHITE) ? whitePiecesC : blackPiecesC;
        uint64 enemyPiecesC = (chance == WHITE) ? blackPiecesC : whitePiecesC;
        uint64 myPiecesD    = (chance == WHITE) ? whitePiecesD : blackPiecesD;
        uint64 enemyPiecesD = (chance == WHITE) ? blackPiecesD : whitePiecesD;

        uint64 enemyBishopsA = bishopQueensA & enemyPiecesA;
        uint64 enemyRooksA   = rookQueensA   & enemyPiecesA;
        uint64 enemyBishopsB = bishopQueensB & enemyPiecesB;
        uint64 enemyRooksB   = rookQueensB   & enemyPiecesB;
        uint64 enemyBishopsC = bishopQueensC & enemyPiecesC;
        uint64 enemyRooksC   = rookQueensC   & enemyPiecesC;
        uint64 enemyBishopsD = bishopQueensD & enemyPiecesD;
        uint64 enemyRooksD   = rookQueensD   & enemyPiecesD;

        uint64 myKingA = kingsA & myPiecesA;
        uint64 myKingB = kingsB & myPiecesB;
        uint64 myKingC = kingsC & myPiecesC;
        uint64 myKingD = kingsD & myPiecesD;
        __assume(myKingA != 0); __assume((myKingA & (myKingA - 1)) == 0);
        __assume(myKingB != 0); __assume((myKingB & (myKingB - 1)) == 0);
        __assume(myKingC != 0); __assume((myKingC & (myKingC - 1)) == 0);
        __assume(myKingD != 0); __assume((myKingD & (myKingD - 1)) == 0);
        uint8 kingIndexA = bitScan(myKingA);
        uint8 kingIndexB = bitScan(myKingB);
        uint8 kingIndexC = bitScan(myKingC);
        uint8 kingIndexD = bitScan(myKingD);

        // 4-way interleaved scalar slider attacks across all 4 children.
        // OoO can keep ~4 magic-LUT loads in flight simultaneously.
        uint64 sliderAtkA, sliderAtkB, sliderAtkC, sliderAtkD;
        findSliderAttacksOnly4(
            ~allPiecesA, enemyBishopsA, enemyRooksA, myKingA,
            ~allPiecesB, enemyBishopsB, enemyRooksB, myKingB,
            ~allPiecesC, enemyBishopsC, enemyRooksC, myKingC,
            ~allPiecesD, enemyBishopsD, enemyRooksD, myKingD,
            sliderAtkA, sliderAtkB, sliderAtkC, sliderAtkD);

        uint64 threatA = atkA | sliderAtkA;
        uint64 threatB = atkB | sliderAtkB;
        uint64 threatC = atkC | sliderAtkC;
        uint64 threatD = atkD | sliderAtkD;

        uint64 pinnedA = findPinnedPieces(myKingA, myPiecesA, enemyBishopsA, enemyRooksA, allPiecesA, kingIndexA);
        uint64 pinnedB = findPinnedPieces(myKingB, myPiecesB, enemyBishopsB, enemyRooksB, allPiecesB, kingIndexB);
        uint64 pinnedC = findPinnedPieces(myKingC, myPiecesC, enemyBishopsC, enemyRooksC, allPiecesC, kingIndexC);
        uint64 pinnedD = findPinnedPieces(myKingD, myPiecesD, enemyBishopsD, enemyRooksD, allPiecesD, kingIndexD);

        uint64 sum = 0;
        #define QWA_DISPATCH_LANE(L) do { \
            if (threat##L & myKing##L) [[unlikely]] { \
                sum += countMovesOutOfCheck<chance>(pos##L, gs##L, allPawns##L, allPieces##L, myPieces##L, enemyPieces##L, \
                                                    pinned##L, threat##L, kingIndex##L, \
                                                    knights##L, bishopQueens##L, rookQueens##L, kings##L); \
            } else { \
                bool hasEP = gs##L->enPassent != 0; \
                bool hasCastle = (chance == WHITE) ? (gs##L->whiteCastle != 0) : (gs##L->blackCastle != 0); \
                if (!hasEP) { \
                    if (hasCastle) sum += countMovesFromDerived<chance, false, true >(pos##L, gs##L, allPieces##L, myPieces##L, enemyPieces##L, allPawns##L, knights##L, bishopQueens##L, rookQueens##L, myKing##L, kingIndex##L, pinned##L, threat##L); \
                    else           sum += countMovesFromDerived<chance, false, false>(pos##L, gs##L, allPieces##L, myPieces##L, enemyPieces##L, allPawns##L, knights##L, bishopQueens##L, rookQueens##L, myKing##L, kingIndex##L, pinned##L, threat##L); \
                } else { \
                    if (hasCastle) sum += countMovesFromDerived<chance, true,  true >(pos##L, gs##L, allPieces##L, myPieces##L, enemyPieces##L, allPawns##L, knights##L, bishopQueens##L, rookQueens##L, myKing##L, kingIndex##L, pinned##L, threat##L); \
                    else           sum += countMovesFromDerived<chance, true,  false>(pos##L, gs##L, allPieces##L, myPieces##L, enemyPieces##L, allPawns##L, knights##L, bishopQueens##L, rookQueens##L, myKing##L, kingIndex##L, pinned##L, threat##L); \
                } \
            } \
        } while (0)
        QWA_DISPATCH_LANE(A);
        QWA_DISPATCH_LANE(B);
        QWA_DISPATCH_LANE(C);
        QWA_DISPATCH_LANE(D);
        #undef QWA_DISPATCH_LANE
        return sum;
    }

    // Piece-typed makeMove for the FGMC hot path. When the source piece is known at
    // compile time we skip the bb[1..3] probes that derive the piece from quad bits,
    // and we statically eliminate every branch that doesn't apply (promotion, EP,
    // castling, double-push, rook-castle-rights). For Kiwipete depth-2 leaves the
    // common cases (knight/bishop/rook/queen non-capture) shrink to just the
    // quad-bitboard XOR + a single castle-clear LUT call.
    template <uint8 chance, uint8 piece>
    CPU_FORCE_INLINE static void makeMoveT(QuadBitBoard *pos, GameState *gs, CMove move)
    {
        uint64 src = BIT(move.getFrom());
        uint64 dst = BIT(move.getTo());

        // Clear src + dst from every bitboard (handles captures implicitly).
        uint64 clearMask = ~(src | dst);
        pos->bb[0] &= clearMask;
        pos->bb[1] &= clearMask;
        pos->bb[2] &= clearMask;
        pos->bb[3] &= clearMask;

        if (piece == PAWN)
        {
            uint8 flags = move.getFlags();
            uint32 placed = PAWN;
            if (flags & CM_FLAG_PROMOTION)
                placed = (flags & 3) + KNIGHT;

            if (chance == BLACK)               pos->bb[0] |= dst;
            if (placed & 1)                    pos->bb[1] |= dst;
            if (placed & 2)                    pos->bb[2] |= dst;
            if (placed & 4)                    pos->bb[3] |= dst;

            // EP capture: clear the captured pawn (always opposite color, pawn-only).
            if (flags == CM_FLAG_EP_CAPTURE)
            {
                uint64 epCapture = (chance == WHITE) ? southOne(dst) : northOne(dst);
                pos->bb[0] &= ~epCapture;
                pos->bb[1] &= ~epCapture;
            }

            gs->enPassent = 0;
            if (flags == CM_FLAG_DOUBLE_PAWN_PUSH)
                gs->enPassent = (move.getFrom() & 7) + 1;

            // Any pawn move can capture a rook on its starting square → opponent castle update.
            updateCastleFlag(gs, dst, chance);
        }
        else if (piece == KING)
        {
            // King is encoded as 110.
            if (chance == BLACK)               pos->bb[0] |= dst;
            pos->bb[2] |= dst;
            pos->bb[3] |= dst;

            uint8 flags = move.getFlags();
            if (chance == WHITE)
            {
                if (flags == CM_FLAG_KING_CASTLE)        pos->bb[3] = (pos->bb[3] ^ BIT(H1)) | BIT(F1);
                else if (flags == CM_FLAG_QUEEN_CASTLE)  pos->bb[3] = (pos->bb[3] ^ BIT(A1)) | BIT(D1);
                gs->whiteCastle = 0;
            }
            else
            {
                if (flags == CM_FLAG_KING_CASTLE)
                {
                    pos->bb[3] = (pos->bb[3] ^ BIT(H8)) | BIT(F8);
                    pos->bb[0] = (pos->bb[0] ^ BIT(H8)) | BIT(F8);
                }
                else if (flags == CM_FLAG_QUEEN_CASTLE)
                {
                    pos->bb[3] = (pos->bb[3] ^ BIT(A8)) | BIT(D8);
                    pos->bb[0] = (pos->bb[0] ^ BIT(A8)) | BIT(D8);
                }
                gs->blackCastle = 0;
            }
            gs->enPassent = 0;
            updateCastleFlag(gs, dst, chance);  // captured an opposing rook on its starting square
        }
        else
        {
            // Non-pawn, non-king (KNIGHT=010, BISHOP=011, ROOK=100, QUEEN=101).
            if (chance == BLACK)               pos->bb[0] |= dst;
            if (piece & 1)                     pos->bb[1] |= dst;
            if (piece & 2)                     pos->bb[2] |= dst;
            if (piece & 4)                     pos->bb[3] |= dst;
            gs->enPassent = 0;

            updateCastleFlag(gs, dst, chance);
            if (piece == ROOK)
                updateCastleFlag(gs, src, !chance);
        }
    }

    // Variant that reads from `parent` and writes to `cp` in one pass.
    // The two-step pattern (`cp = parent; cp.bb[i] &= clearMask;`) leaves
    // 4 extra store-load forwarding chains the optimiser may or may not
    // eliminate. This form is unambiguous: each bb plane is exactly one
    // load + AND + store, with no intermediate copy.
    // On ARM64 we issue the 4 ANDs as 2 NEON uint64x2_t ops to halve the
    // store traffic.
    template <uint8 chance, uint8 piece>
    CPU_FORCE_INLINE static void makeMoveTFromParent(
        QuadBitBoard *cp, GameState *cgs,
        const QuadBitBoard *parent, const GameState *parentGs,
        CMove move)
    {
        uint64 src = BIT(move.getFrom());
        uint64 dst = BIT(move.getTo());
        uint64 clearMask = ~(src | dst);

        // Scalar 4 ANDs pipeline well on ARM64's wide scalar issue — beats NEON
        // 2-op version on Snapdragon X Oryon (measured: 0.142 vs 0.155). On
        // Intel Lion Cove an explicit __m128i (2 x 16B) variant was tried both
        // pre-SoA (E5) and post-SoA (E64) — both in-the-noise: perft 5 min
        // moved by <1 ms, perft 6 was flat-to-slightly-worse. Scalar wins
        // because MSVC schedules the 4 independent ANDs across 4 ports, while
        // the SSE2 version is store-port-bound on Lion Cove's 2 STA pipes.
        cp->bb[0] = parent->bb[0] & clearMask;
        cp->bb[1] = parent->bb[1] & clearMask;
        cp->bb[2] = parent->bb[2] & clearMask;
        cp->bb[3] = parent->bb[3] & clearMask;
        cgs->raw = parentGs->raw;

        if constexpr (piece == PAWN)
        {
            uint8 flags = move.getFlags();
            uint32 placed = PAWN;
            if (flags & CM_FLAG_PROMOTION) placed = (flags & 3) + KNIGHT;
            if (chance == BLACK) cp->bb[0] |= dst;
            if (placed & 1)      cp->bb[1] |= dst;
            if (placed & 2)      cp->bb[2] |= dst;
            if (placed & 4)      cp->bb[3] |= dst;
            if (flags == CM_FLAG_EP_CAPTURE)
            {
                uint64 epCapture = (chance == WHITE) ? southOne(dst) : northOne(dst);
                cp->bb[0] &= ~epCapture;
                cp->bb[1] &= ~epCapture;
            }
            cgs->enPassent = 0;
            if (flags == CM_FLAG_DOUBLE_PAWN_PUSH) cgs->enPassent = (move.getFrom() & 7) + 1;
            updateCastleFlag(cgs, dst, chance);
        }
        else if constexpr (piece == KING)
        {
            if (chance == BLACK) cp->bb[0] |= dst;
            cp->bb[2] |= dst;
            cp->bb[3] |= dst;
            uint8 flags = move.getFlags();
            if (chance == WHITE)
            {
                if (flags == CM_FLAG_KING_CASTLE)        cp->bb[3] = (cp->bb[3] ^ BIT(H1)) | BIT(F1);
                else if (flags == CM_FLAG_QUEEN_CASTLE)  cp->bb[3] = (cp->bb[3] ^ BIT(A1)) | BIT(D1);
                cgs->whiteCastle = 0;
            }
            else
            {
                if (flags == CM_FLAG_KING_CASTLE)
                {
                    cp->bb[3] = (cp->bb[3] ^ BIT(H8)) | BIT(F8);
                    cp->bb[0] = (cp->bb[0] ^ BIT(H8)) | BIT(F8);
                }
                else if (flags == CM_FLAG_QUEEN_CASTLE)
                {
                    cp->bb[3] = (cp->bb[3] ^ BIT(A8)) | BIT(D8);
                    cp->bb[0] = (cp->bb[0] ^ BIT(A8)) | BIT(D8);
                }
                cgs->blackCastle = 0;
            }
            cgs->enPassent = 0;
            updateCastleFlag(cgs, dst, chance);
        }
        else
        {
            // KNIGHT=010, BISHOP=011, ROOK=100, QUEEN=101
            if (chance == BLACK) cp->bb[0] |= dst;
            if constexpr (piece & 1) cp->bb[1] |= dst;
            if constexpr (piece & 2) cp->bb[2] |= dst;
            if constexpr (piece & 4) cp->bb[3] |= dst;
            cgs->enPassent = 0;
            updateCastleFlag(cgs, dst, chance);
            if constexpr (piece == ROOK) updateCastleFlag(cgs, src, !chance);
        }
    }

    template<uint8 chance>
    CPU_FORCE_INLINE static void makeMove (QuadBitBoard *pos, GameState *gs, CMove move)
    {
        uint64 src = BIT(move.getFrom());
        uint64 dst = BIT(move.getTo());

        // figure out the source piece from quad encoding
        // bb[1]=piece bit 0, bb[2]=piece bit 1, bb[3]=piece bit 2
        // PAWN=1(001), KNIGHT=2(010), BISHOP=3(011), ROOK=4(100), QUEEN=5(101), KING=6(110)
        uint32 piece = 0;
        if (pos->bb[1] & src) piece |= 1;
        if (pos->bb[2] & src) piece |= 2;
        if (pos->bb[3] & src) piece |= 4;

        // promote the pawn (if this was promotion move)
        // Branchless: promotions have bit 3 set (flags >= 8), piece type in bits [1:0]+2
        if (move.getFlags() & CM_FLAG_PROMOTION)
            piece = (move.getFlags() & 3) + KNIGHT;

        // clear source and destination from all bitboards (4 ops instead of 10)
        uint64 clearMask = ~(src | dst);
        pos->bb[0] &= clearMask;
        pos->bb[1] &= clearMask;
        pos->bb[2] &= clearMask;
        pos->bb[3] &= clearMask;

        // place piece at destination
        if (chance == BLACK) pos->bb[0] |= dst;   // color bit
        if (piece & 1) pos->bb[1] |= dst;
        if (piece & 2) pos->bb[2] |= dst;
        if (piece & 4) pos->bb[3] |= dst;

        // en-passent capture: clear the captured pawn
        if (move.getFlags() == CM_FLAG_EP_CAPTURE)
        {
            uint64 epCapture = (chance == WHITE) ? southOne(dst) : northOne(dst);
            // captured pawn: bb[0] set if enemy (always true), bb[1] set (pawn bit)
            // bb[2] and bb[3] guaranteed 0 for pawn
            pos->bb[0] &= ~epCapture;
            pos->bb[1] &= ~epCapture;
        }

        // castling: move the rook
        if (chance == WHITE)
        {
            if (move.getFlags() == CM_FLAG_KING_CASTLE)
            {
                // white king side castle: rook H1->F1, white rook = bb[3] only
                pos->bb[3] = (pos->bb[3] ^ BIT(H1)) | BIT(F1);
            }
            else if (move.getFlags() == CM_FLAG_QUEEN_CASTLE)
            {
                // white queen side castle: rook A1->D1
                pos->bb[3] = (pos->bb[3] ^ BIT(A1)) | BIT(D1);
            }
        }
        else
        {
            if (move.getFlags() == CM_FLAG_KING_CASTLE)
            {
                // black king side castle: rook H8->F8, black rook = bb[3] + bb[0]
                pos->bb[3] = (pos->bb[3] ^ BIT(H8)) | BIT(F8);
                pos->bb[0] = (pos->bb[0] ^ BIT(H8)) | BIT(F8);
            }
            else if (move.getFlags() == CM_FLAG_QUEEN_CASTLE)
            {
                // black queen side castle: rook A8->D8
                pos->bb[3] = (pos->bb[3] ^ BIT(A8)) | BIT(D8);
                pos->bb[0] = (pos->bb[0] ^ BIT(A8)) | BIT(D8);
            }
        }

        // update game state
        gs->enPassent = 0;

        // Castle flag updates:
        //   - dst: if we captured an opponent rook on its starting square, that side
        //     loses the corresponding castle right.
        //   - king move: lose all our castle rights.
        //   - rook move: if we moved a rook off its starting square, lose that side.
        updateCastleFlag(gs, dst, chance);
        if (piece == KING)
        {
            if (chance == WHITE) gs->whiteCastle = 0;
            else                 gs->blackCastle = 0;
        }
        if (piece == ROOK)
            updateCastleFlag(gs, src, !chance);

        if (move.getFlags() == CM_FLAG_DOUBLE_PAWN_PUSH)
        {
            // Always set the EP flag — countMoves will check if any EP source
            // pawns actually exist.
            gs->enPassent = (move.getFrom() & 7) + 1;
        }
    }

};

// Free function wrapper for generateMoves (dispatches on template chance)
CPU_FORCE_INLINE uint32 generateMoves(QuadBitBoard *pos, GameState *gs, uint8 color, CMove *genMoves)
{
    if (color == BLACK)
    {
        return MoveGeneratorBitboard::generateMoves<BLACK>(pos, gs, genMoves);
    }
    else
    {
        return MoveGeneratorBitboard::generateMoves<WHITE>(pos, gs, genMoves);
    }
}


