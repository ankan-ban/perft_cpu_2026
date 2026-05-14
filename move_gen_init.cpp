// One-time initialization of the move generator: empty-board attack tables,
// Between/Line LUTs, magic occupancy masks, magic lookup tables, and Zobrist
// randoms.

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "MoveGeneratorBitboard.h"
#include "zobrist.h"

void MoveGeneratorBitboard::init()
{
    // Empty-board attack tables (sliding via Kogge-Stone)
    for (uint8 i = 0; i < 64; i++)
    {
        uint64 x = BIT(i);
        uint64 north = northAttacks(x, ALLSET);
        uint64 south = southAttacks(x, ALLSET);
        uint64 east  = eastAttacks (x, ALLSET);
        uint64 west  = westAttacks (x, ALLSET);
        uint64 ne    = northEastAttacks(x, ALLSET);
        uint64 nw    = northWestAttacks(x, ALLSET);
        uint64 se    = southEastAttacks(x, ALLSET);
        uint64 sw    = southWestAttacks(x, ALLSET);
        RookAttacks  [i] = north | south | east | west;
        BishopAttacks[i] = ne | nw | se | sw;
        QueenAttacks [i] = RookAttacks[i] | BishopAttacks[i];
        KnightAttacks[i] = knightAttacks(x);
        KingAttacks[i]   = kingAttacks(x);
    }

    // Between and Line LUTs (Between[i][j] only needs the upper triangle)
    for (uint8 i = 0; i < 64; i++)
        for (uint8 j = 0; j < 64; j++)
        {
            if (i <= j)
            {
                Between[i][j] = squaresInBetween(i, j);
                Between[j][i] = Between[i][j];
            }
            Line[i][j] = squaresInLine(i, j);
        }

    // Magic occupancy masks (excluding edge squares)
    for (int square = A1; square <= H8; square++)
    {
        uint64 thisSquare = BIT(square);
        uint64 mask = sqRookAttacks(square) & (~thisSquare);
        if ((thisSquare & RANK1) == 0) mask &= ~RANK1;
        if ((thisSquare & RANK8) == 0) mask &= ~RANK8;
        if ((thisSquare & FILEA) == 0) mask &= ~FILEA;
        if ((thisSquare & FILEH) == 0) mask &= ~FILEH;
        RookAttacksMasked[square] = mask;
        BishopAttacksMasked[square] = sqBishopAttacks(square) & (~thisSquare) & CENTRAL_SQUARES;
    }

    // Fancy magic lookup tables (uses the pre-validated factors from GlobalVars.cpp)
    srand((unsigned)time(nullptr));
    memset(fancy_magic_lookup_table, 0, sizeof(fancy_magic_lookup_table));
    for (int square = A1; square <= H8; square++)
    {
        uint64 rookMagic = findRookMagicForSquare(
            square, &fancy_magic_lookup_table[rook_magics_fancy[square].position],
            rook_magics_fancy[square].factor);
        (void)rookMagic;
        uint64 bishopMagic = findBishopMagicForSquare(
            square, &fancy_magic_lookup_table[bishop_magics_fancy[square].position],
            bishop_magics_fancy[square].factor);
        (void)bishopMagic;

        // Packed CPU fast-magic entries — pre-resolved table pointer saves a per-call add.
        bishop_magics_fast[square].factor = bishop_magics_fancy[square].factor;
        bishop_magics_fast[square].table  = &fancy_magic_lookup_table[bishop_magics_fancy[square].position];
        rook_magics_fast[square].factor   = rook_magics_fancy[square].factor;
        rook_magics_fast[square].table    = &fancy_magic_lookup_table[rook_magics_fancy[square].position];

        // E18: parallel split arrays — same data, but stored as two 64-bit GPRs
        // so MSVC doesn't pack into XMM and have to extract halves per lookup.
        bishop_magic_factors[square] = bishop_magics_fancy[square].factor;
        bishop_magic_tables [square] = &fancy_magic_lookup_table[bishop_magics_fancy[square].position];
        rook_magic_factors  [square] = rook_magics_fancy[square].factor;
        rook_magic_tables   [square] = &fancy_magic_lookup_table[rook_magics_fancy[square].position];
    }

    initZobrist();
}
