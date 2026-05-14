#pragma once

#include "chess.h"

// -------------------------------------------------------------------------
// 128-bit Zobrist hash for transposition table keying
// lo: used as TT index (masked to table size)
// hi: used for verification (XOR'd with count in lockless scheme)
// -------------------------------------------------------------------------

struct Hash128
{
    uint64 lo;
    uint64 hi;
};

CT_ASSERT(sizeof(Hash128) == 16);

// Zobrist random number tables
struct ZobristRandoms
{
    uint64 pieces[2][6][64];    // [color][pieceType-1][square]
    uint64 castling[2][2];      // [color][0=kingside, 1=queenside]
    uint64 enPassant[8];        // [file 0-7]
    uint64 sideToMove;
};

extern ZobristRandoms zobrist1;
extern ZobristRandoms zobrist2;

#define ZOB1(field) zobrist1.field
#define ZOB2(field) zobrist2.field

// Initialize Zobrist tables (deterministic).
void initZobrist();

// -------------------------------------------------------------------------
// Extract piece type at a square from a QuadBitBoard
// Returns 0 if empty, PAWN(1)..KING(6) if occupied
// -------------------------------------------------------------------------

CPU_FORCE_INLINE uint8 getPieceAt(const QuadBitBoard *pos, uint8 sq)
{
    uint64 bit = BIT(sq);
    uint8 piece = 0;
    if (pos->bb[1] & bit) piece |= 1;
    if (pos->bb[2] & bit) piece |= 2;
    if (pos->bb[3] & bit) piece |= 4;
    return piece;
}

// -------------------------------------------------------------------------
// Compute hash from scratch (for root position and debug validation)
// -------------------------------------------------------------------------

CPU_FORCE_INLINE Hash128 computeHash(const QuadBitBoard *pos, const GameState *gs, uint8 color)
{
    Hash128 h = {0, 0};

    if (color == WHITE)
    {
        h.lo ^= ZOB1(sideToMove);
        h.hi ^= ZOB2(sideToMove);
    }

    if (gs->whiteCastle & CASTLE_FLAG_KING_SIDE)
    {
        h.lo ^= ZOB1(castling[WHITE][0]);
        h.hi ^= ZOB2(castling[WHITE][0]);
    }
    if (gs->whiteCastle & CASTLE_FLAG_QUEEN_SIDE)
    {
        h.lo ^= ZOB1(castling[WHITE][1]);
        h.hi ^= ZOB2(castling[WHITE][1]);
    }
    if (gs->blackCastle & CASTLE_FLAG_KING_SIDE)
    {
        h.lo ^= ZOB1(castling[BLACK][0]);
        h.hi ^= ZOB2(castling[BLACK][0]);
    }
    if (gs->blackCastle & CASTLE_FLAG_QUEEN_SIDE)
    {
        h.lo ^= ZOB1(castling[BLACK][1]);
        h.hi ^= ZOB2(castling[BLACK][1]);
    }

    if (gs->enPassent)
    {
        h.lo ^= ZOB1(enPassant[gs->enPassent - 1]);
        h.hi ^= ZOB2(enPassant[gs->enPassent - 1]);
    }

    for (uint8 sq = 0; sq < 64; sq++)
    {
        uint8 piece = getPieceAt(pos, sq);
        if (piece == 0) continue;
        uint8 pieceColor = (pos->bb[0] & BIT(sq)) ? BLACK : WHITE;
        h.lo ^= ZOB1(pieces[pieceColor][piece - 1][sq]);
        h.hi ^= ZOB2(pieces[pieceColor][piece - 1][sq]);
    }

    return h;
}

// -------------------------------------------------------------------------
// Incremental hash update after makeMove
//
// Usage pattern (caller does NOT modify makeMove itself):
//   uint8 srcPiece = getPieceAt(pos, move.getFrom());
//   uint8 capPiece = getPieceAt(pos, move.getTo());
//   uint8 oldCastleRaw = gs->raw;
//   uint8 oldEP = gs->enPassent;
//   makeMove(pos, gs, move, color);  // unchanged
//   hash = updateHashAfterMove(hash, move, color, srcPiece, capPiece,
//                              oldCastleRaw, gs->raw, oldEP, gs->enPassent);
// -------------------------------------------------------------------------

CPU_FORCE_INLINE Hash128 updateHashAfterMove(
    Hash128 h, CMove move, uint8 chance,
    uint8 srcPiece, uint8 capPiece,
    uint8 oldCastleRaw, uint8 newCastleRaw,
    uint8 oldEP, uint8 newEP)
{
    uint8 from  = move.getFrom();
    uint8 to    = move.getTo();
    uint8 flags = move.getFlags();

    h.lo ^= ZOB1(sideToMove);
    h.hi ^= ZOB2(sideToMove);

    h.lo ^= ZOB1(pieces[chance][srcPiece - 1][from]);
    h.hi ^= ZOB2(pieces[chance][srcPiece - 1][from]);

    uint8 placedPiece = srcPiece;
    if (flags & CM_FLAG_PROMOTION)
        placedPiece = (flags & 3) + KNIGHT;

    h.lo ^= ZOB1(pieces[chance][placedPiece - 1][to]);
    h.hi ^= ZOB2(pieces[chance][placedPiece - 1][to]);

    if (capPiece)
    {
        h.lo ^= ZOB1(pieces[!chance][capPiece - 1][to]);
        h.hi ^= ZOB2(pieces[!chance][capPiece - 1][to]);
    }

    if (flags == CM_FLAG_EP_CAPTURE)
    {
        uint8 epPawnSq = (chance == WHITE) ? (to - 8) : (to + 8);
        h.lo ^= ZOB1(pieces[!chance][PAWN - 1][epPawnSq]);
        h.hi ^= ZOB2(pieces[!chance][PAWN - 1][epPawnSq]);
    }

    if (flags == CM_FLAG_KING_CASTLE)
    {
        uint8 rookFrom = (chance == WHITE) ? H1 : H8;
        uint8 rookTo   = (chance == WHITE) ? F1 : F8;
        h.lo ^= ZOB1(pieces[chance][ROOK - 1][rookFrom]) ^ ZOB1(pieces[chance][ROOK - 1][rookTo]);
        h.hi ^= ZOB2(pieces[chance][ROOK - 1][rookFrom]) ^ ZOB2(pieces[chance][ROOK - 1][rookTo]);
    }
    else if (flags == CM_FLAG_QUEEN_CASTLE)
    {
        uint8 rookFrom = (chance == WHITE) ? A1 : A8;
        uint8 rookTo   = (chance == WHITE) ? D1 : D8;
        h.lo ^= ZOB1(pieces[chance][ROOK - 1][rookFrom]) ^ ZOB1(pieces[chance][ROOK - 1][rookTo]);
        h.hi ^= ZOB2(pieces[chance][ROOK - 1][rookFrom]) ^ ZOB2(pieces[chance][ROOK - 1][rookTo]);
    }

    // Castle rights: XOR only changed bits (oldRaw ^ newRaw)
    uint8 castleDelta = (oldCastleRaw ^ newCastleRaw) & 0x0F;
    if (castleDelta & 1) { h.lo ^= ZOB1(castling[WHITE][0]); h.hi ^= ZOB2(castling[WHITE][0]); }
    if (castleDelta & 2) { h.lo ^= ZOB1(castling[WHITE][1]); h.hi ^= ZOB2(castling[WHITE][1]); }
    if (castleDelta & 4) { h.lo ^= ZOB1(castling[BLACK][0]); h.hi ^= ZOB2(castling[BLACK][0]); }
    if (castleDelta & 8) { h.lo ^= ZOB1(castling[BLACK][1]); h.hi ^= ZOB2(castling[BLACK][1]); }

    if (oldEP)
    {
        h.lo ^= ZOB1(enPassant[oldEP - 1]);
        h.hi ^= ZOB2(enPassant[oldEP - 1]);
    }
    if (newEP)
    {
        h.lo ^= ZOB1(enPassant[newEP - 1]);
        h.hi ^= ZOB2(enPassant[newEP - 1]);
    }

    return h;
}
