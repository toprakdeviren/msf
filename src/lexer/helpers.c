/**
 * @file helpers.c
 * @brief Lexer helpers — lookup tables and small reusable functions.
 *
 * These are the building blocks used by the scanners and the core lexer.
 * Nothing here reads source code directly — they're pure utilities.
 *
 * Contents:
 *   MULTI_OPS           — multi-char operator lookup table
 *   make_string_tok()   — constructs a TOK_STRING_LIT token
 *   advance_past_token() — updates line/col after a multiline token
 */
#include "private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Multi-Character Operator Table
 *
 * Searched linearly by scan_symbol() for 2-3 character operators.
 * Longest operators come first so "===" is matched before "==".
 * Terminated by {NULL, 0, OP_NONE} sentinel.
 *
 * Lives in .rodata — zero runtime cost, no initialization needed.
 * ═══════════════════════════════════════════════════════════════════════════════ */

const MultiCharOp MULTI_OPS[] = {
    /* 3-char */
    {"...", 3, OP_RANGE_INCL}, {"..<", 3, OP_RANGE_EXCL},
    {"===", 3, OP_IDENTITY_EQ}, {"!==", 3, OP_IDENTITY_NEQ},
    /* 2-char: wrapping arithmetic */
    {"&+", 2, OP_WRAP_ADD}, {"&-", 2, OP_WRAP_SUB}, {"&*", 2, OP_WRAP_MUL},
    /* 2-char: arrows */
    {"->", 2, OP_ARROW}, {"=>", 2, OP_FAT_ARROW},
    /* 2-char: comparison */
    {"==", 2, OP_EQ}, {"!=", 2, OP_NEQ}, {"<=", 2, OP_LEQ}, {">=", 2, OP_GEQ},
    /* 2-char: logical */
    {"&&", 2, OP_AND}, {"||", 2, OP_OR}, {"??", 2, OP_NIL_COAL},
    /* 2-char: bitwise shift */
    {"<<", 2, OP_LSHIFT}, {">>", 2, OP_RSHIFT},
    /* 2-char: compound assignment */
    {"+=", 2, OP_ADD_ASSIGN}, {"-=", 2, OP_SUB_ASSIGN}, {"*=", 2, OP_MUL_ASSIGN},
    {"/=", 2, OP_DIV_ASSIGN}, {"%=", 2, OP_MOD_ASSIGN},
    {"&=", 2, OP_AND_ASSIGN}, {"|=", 2, OP_OR_ASSIGN}, {"^=", 2, OP_XOR_ASSIGN},
    /* sentinel */
    {NULL, 0, OP_NONE},
};

/**
 * @brief Constructs a TOK_STRING_LIT token with the given position info.
 *
 * Eliminates repeated 7-field compound literals across string scanners.
 * keyword and op_kind are always KW_NONE / OP_NONE for string tokens.
 *
 * @param sp  Start byte offset in source.
 * @param tl  Byte length of the token.
 * @param sl  1-based start line.
 * @param sc  1-based start column.
 * @return    A fully initialized Token.
 */
Token make_string_tok(uint32_t sp, uint32_t tl, uint32_t sl, uint32_t sc) {
  return (Token){TOK_STRING_LIT, sp, tl, sl, sc, KW_NONE, OP_NONE};
}

/**
 * @brief Updates lexer line/col after scanning a token that spans newlines.
 *
 * After a multiline string or block comment, the lexer's column must be
 * reset to the offset from the last newline inside the token.  This helper
 * is called by scan_string, scan_raw_string, and scan_block_comment.
 *
 * @param l            Lexer state to update.
 * @param s            Source bytes (needed to find the last newline).
 * @param tok_start    Byte offset where the token started.
 * @param tok_len      Total byte length of the token.
 * @param extra_lines  Number of newlines counted inside the token.
 *
 * @code
 *   // After scanning """hello\nworld""":
 *   advance_past_token(l, s, start, 18, 1);
 *   // l->line += 1, l->col = offset from last \n, l->pos = start + 18
 * @endcode
 */
void advance_past_token(Lexer *l, const uint8_t *s,
                        uint32_t tok_start, uint32_t tok_len,
                        uint32_t extra_lines) {
  l->line += extra_lines;
  if (extra_lines > 0) {
    uint32_t tok_end = tok_start + tok_len;
    uint32_t last_nl = tok_end - 1;
    while (last_nl > tok_start && s[last_nl] != '\n')
      last_nl--;
    l->col = tok_end - last_nl;
  } else {
    l->col += tok_len;
  }
  l->pos = tok_start + tok_len;
}
