/**
 * @file private.h
 * @brief Lexer-internal shared declarations — NOT part of any module API.
 *
 * This header is included by all lexer .c files (lexer.c, helpers.c,
 * diag.c, token.c, and all scan/ files).  It is NOT included by parser or sema.
 *
 * WHAT THIS HEADER PROVIDES
 *
 *   Pattern macros   — IS_NEWLINE, IS_TRIPLE_QUOTE, IS_BLOCK_OPEN, ...
 *   Advance macros   — ADVANCE, ADVANCE_BY, NEWLINE_ADVANCE, SKIP_CRLF
 *   Token helpers    — MAKE_COMMENT_TOK, make_string_tok, advance_past_token
 *   Diagnostic msgs  — DIAG_UNTERM_STRING, DIAG_INVALID_ESCAPE, ...
 *   Inline helpers   — map_kw_id(), is_hex()
 *   Scanner protos   — scan_string, scan_symbol, scan_comment, ...
 *   Tables           — MULTI_OPS[], LEX_CHAR_CLASS[] (via char_tables.h)
 */
#ifndef LEXER_PRIVATE_H
#define LEXER_PRIVATE_H

#include "internal/lexer.h"
#include "internal/limits.h"
#include "char_tables.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* @generated — keyword hash table (scripts/codegen.py) */
static inline Keyword map_kw_id(uint32_t kid) {
  if (kid == 0 || kid >= KW__COUNT) return KW_NONE;
  return (Keyword)kid;
}

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  1. CHARACTER PATTERN MACROS                                           │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief True if c is a line terminator (LF or CR). */
#define IS_NEWLINE(c)           ((c) == '\n' || (c) == '\r')

/** @brief True if c is the raw string delimiter '#'. */
#define IS_RAW_STRING_START(c)  ((c) == '#')

/** @brief True if s[p..p+2] is a triple-quote sequence: \""" */
#define IS_TRIPLE_QUOTE(s, p)   ((s)[(p)] == '"' && (s)[(p)+1] == '"' && (s)[(p)+2] == '"')

/** @brief True if s[p..p+1] is the block comment opener: /\* */
#define IS_BLOCK_OPEN(s, p, len)   ((p) + 1 < (len) && (s)[(p)] == '/' && (s)[(p)+1] == '*')

/** @brief True if s[p..p+1] is the block comment closer: *\/ */
#define IS_BLOCK_CLOSE(s, p, len)  ((p) + 1 < (len) && (s)[(p)] == '*' && (s)[(p)+1] == '/')

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  2. LEXER STATE ADVANCE MACROS                                         │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  ADVANCE(l)           — 1 byte forward on the same line
 *  ADVANCE_BY(l, n)     — n bytes forward on the same line
 *  NEWLINE_ADVANCE(l)   — 1 byte forward, new line
 *  SKIP_CRLF(s,pos,len) — consume LF after CR (Windows line endings)
 */

#define ADVANCE(l)              do { (l)->pos++; (l)->col++; } while (0)
#define ADVANCE_BY(l, n)        do { (l)->pos += (n); (l)->col += (n); } while (0)
#define NEWLINE_ADVANCE(l)      do { (l)->pos++; (l)->line++; (l)->col = 1; } while (0)
#define SKIP_CRLF(s, pos, len)  \
  do { if ((s)[(pos)-1] == '\r' && (pos) < (len) && (s)[(pos)] == '\n') (pos)++; } while (0)

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  3. TOKEN CONSTRUCTORS                                                 │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief Constructs a TOK_COMMENT token spanning [sp, l->pos). */
#define MAKE_COMMENT_TOK(sp, l, sl, sc)  \
  ((Token){TOK_COMMENT, (sp), (uint32_t)((l)->pos - (sp)), (sl), (sc), KW_NONE, OP_NONE})

/**
 * @brief Constructs a TOK_STRING_LIT token with the given position info.
 *
 * Eliminates repeated 7-field compound literals across string scanners.
 * keyword and op_kind are always KW_NONE / OP_NONE for string tokens.
 */
Token make_string_tok(uint32_t start_pos, uint32_t tok_len,
                      uint32_t start_line, uint32_t start_col);

/**
 * @brief Updates lexer line/col after scanning a token that spans newlines.
 *
 * After a multiline string or block comment, the lexer's column must be
 * reset to the offset from the last newline inside the token.  Called by
 * scan_string, scan_raw_string, and scan_block_comment.
 */
void advance_past_token(Lexer *l, const uint8_t *s,
                        uint32_t tok_start, uint32_t tok_len,
                        uint32_t extra_lines);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  4. DIAGNOSTIC MESSAGES                                                │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  All scanner error strings in one place.  Change a message here,
 *  it updates everywhere.  Tests can strcmp against these constants.
 */

#define DIAG_SINGLE_QUOTE         "single-quoted string is not allowed; use double quotes"
#define DIAG_UNTERM_STRING        "unterminated string literal"
#define DIAG_UNTERM_TRIPLE        "unterminated triple-quoted string literal"
#define DIAG_UNTERM_RAW_STRING    "unterminated raw string literal"
#define DIAG_UNTERM_BLOCK_COMMENT "unterminated block comment"
#define DIAG_INVALID_ESCAPE       "invalid escape sequence in string literal"

/**
 * @brief Records a diagnostic with printf-style formatting.
 *
 * Called by scanners when they encounter a recoverable error (unterminated
 * string, invalid escape, etc.).  No-op if l->diag is NULL — callers
 * that don't need diagnostics simply pass NULL at init time.
 */
void lexer_diag_record(Lexer *l, uint32_t line, uint32_t col,
                       const char *fmt, ...);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  5. TABLES & HELPERS                                                   │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief Multi-character operator lookup entry (longest-first order). */
typedef struct {
  const char *op;
  uint8_t     len;
  OpKind      kind;
} MultiCharOp;
extern const MultiCharOp MULTI_OPS[];

/** @brief Returns 1 if c is a hexadecimal digit. */
static inline int is_hex(uint8_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  6. SCANNER PROTOTYPES                                                 │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Each scanner handles one token family.  Called by lexer_next()
 *  (in lexer.c) or by scan_symbol() (cross-scanner dispatch).
 *
 *  All scanners share the same signature pattern:
 *    (Lexer *l, const uint8_t *s, uint32_t len, uint32_t sp, sl, sc)
 *  where sp/sl/sc = start position/line/col of the token being scanned.
 */

/* — String scanners (scan/string.c) --------------------------------------- */

/**
 * @brief Validates escape sequences in s[start..end).
 *
 * Walks the string body checking every backslash: \n \t \r \0 \" \' \\,
 * \(interpolation), \u{XXXX}.  On first invalid escape, writes the error
 * position into out_line/out_col and returns 0.
 *
 * @return 1 if all escapes are valid, 0 on first invalid escape.
 */
int   validate_string_escapes(const uint8_t *s, uint32_t start, uint32_t end,
                              uint32_t start_line, uint32_t start_col,
                              uint32_t *out_line, uint32_t *out_col);

/**
 * @brief Scans a regular "..." or triple-quoted \"""...\""" string literal.
 *
 * Also detects single-quote misuse ('hello') and emits a diagnostic.
 * After scanning, validates escape sequences inside the string body.
 *
 * @return TOK_STRING_LIT token (or partial token on unterminated input).
 */
Token scan_string(Lexer *l, const uint8_t *s, uint32_t len,
                  uint32_t sp, uint32_t sl, uint32_t sc);

/**
 * @brief Scans a raw string literal: #"..."#, ##"..."##, or #\"""...\"""#.
 *
 * Counts opening '#' characters to determine the closing delimiter.
 * No escape processing — backslashes are literal inside raw strings.
 * Returns a zero-length sentinel if '#' is not followed by '"',
 * signaling lexer_next() to try another scanner.
 *
 * @return TOK_STRING_LIT on success, zero-length TOK_UNKNOWN sentinel otherwise.
 */
Token scan_raw_string(Lexer *l, const uint8_t *s, uint32_t len,
                      uint32_t sp, uint32_t sl, uint32_t sc);

/* — Comment scanners (scan/comment.c) ------------------------------------ */

/**
 * @brief Scans a line comment: // ... until newline or EOF.
 *
 * Uses memchr for fast newline scanning.  Does not consume the newline
 * itself — that becomes a separate TOK_NEWLINE token.
 *
 * @return TOK_COMMENT token spanning the entire comment.
 */
Token scan_line_comment(Lexer *l, const uint8_t *s, uint32_t len,
                        uint32_t sp, uint32_t sl, uint32_t sc);

/**
 * @brief Scans a block comment with nesting support.
 *
 * Tracks a depth counter: each nested opening increments, each closing
 * decrements.  Records a diagnostic if EOF is reached before the comment
 * is closed.
 *
 * @return TOK_COMMENT token spanning the entire block comment.
 */
Token scan_block_comment(Lexer *l, const uint8_t *s, uint32_t len,
                         uint32_t sp, uint32_t sl, uint32_t sc);

/* — Symbol scanner (scan/symbol.c) ---------------------------------------- */

/**
 * @brief Attempts to scan a regex literal: /pattern/.
 *
 * Scans forward from the opening '/' looking for an unescaped closing '/'.
 * Fails (returns 0) if a newline is hit — Swift regex literals cannot
 * span multiple lines.
 *
 * @param out  Receives the token on success.
 * @return     1 if a regex literal was scanned, 0 otherwise.
 */
int   scan_regex_literal(Lexer *l, const uint8_t *s, uint32_t len,
                         uint32_t sp, uint32_t sl, uint32_t sc, Token *out);

/**
 * @brief Scans an operator, comment, regex, or punctuation token.
 *
 * Catch-all dispatcher called by lexer_next() when the current byte
 * doesn't match whitespace, identifier, number, or string.
 *
 * Dispatch order (first match wins):
 *   1. "//" → line comment
 *   2. "/" + "*" → block comment
 *   3. Multi-char operator (MULTI_OPS[] table, longest first)
 *   4. "/" + non-delimiter → regex literal attempt
 *   5. Single-char → LEX_CHAR_TOKEN[c] lookup
 *
 * @param c  Current byte (s[l->pos]).
 * @return   The scanned token.
 */
Token scan_symbol(Lexer *l, const uint8_t *s, uint32_t len, uint8_t c,
                  uint32_t sp, uint32_t sl, uint32_t sc);

#endif /* LEXER_PRIVATE_H */
