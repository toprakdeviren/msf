/**
 * @file lexer.h
 * @brief Lexer module API — tokenization, diagnostics, incremental scanning.
 *
 * NOT part of the public API.  This header is the internal contract
 * between the lexer module and its consumers (parser, msf.c, tests).
 *
 * WHAT THIS MODULE PROVIDES
 *
 *   Tokenize     — lexer_tokenize() converts source text into a token stream
 *   Incremental  — lexer_init() + lexer_next() for one-at-a-time scanning
 *   Diagnostics  — LexerDiagnostics accumulates warnings/errors during scan
 *   Stream ops   — token_stream_init/free/push for dynamic token arrays
 *
 * USAGE (batch — most common)
 *
 *   TokenStream ts;
 *   token_stream_init(&ts, 512);
 *   lexer_tokenize(&src, &ts, 1, NULL);   // skip_ws=1, no diagnostics
 *   // ... use ts.tokens[0..ts.count-1] ...
 *   token_stream_free(&ts);
 *
 * USAGE (incremental)
 *
 *   Lexer l;
 *   lexer_init(&l, &src);
 *   for (Token t; (t = lexer_next(&l)).type != TOK_EOF; )
 *       process(t);
 *
 * OWNERSHIP
 *
 *   TokenStream owns its token array.  Free with token_stream_free().
 *   Lexer and LexerDiagnostics are stack-allocated, no cleanup needed.
 */
#ifndef MSF_LEXER_INTERNAL_H
#define MSF_LEXER_INTERNAL_H

#include <msf.h>  /* public types: Source, Token, TokenStream, TokenType */
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  1. DIAGNOSTICS — warnings and errors during lexing                    │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  LexerDiagnostics diag;
 *  lexer_diag_init(&diag);
 *  lexer_tokenize(&src, &ts, 1, &diag);
 *
 *  for (size_t i = 0; i < diag.count; i++)
 *      printf("%u:%u: %s\n", diag.line[i], diag.col[i], diag.message[i]);
 */

#define LEXER_DIAG_MAX 16

struct LexerDiagnostics {
  char     message[LEXER_DIAG_MAX][200];  /**< Error/warning messages.  */
  uint32_t line[LEXER_DIAG_MAX];          /**< 1-based line numbers.    */
  uint32_t col[LEXER_DIAG_MAX];           /**< 1-based column numbers.  */
  size_t   count;                         /**< Number of diagnostics.   */
};

/** @brief Zeroes the diagnostic accumulator. */
void lexer_diag_init(LexerDiagnostics *d);

/** @brief Records a diagnostic (printf-style).  Parser can also push here. */
void lexer_diag_push(LexerDiagnostics *d, uint32_t line, uint32_t col,
                     const char *fmt, ...);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  2. INCREMENTAL LEXER — one token at a time                            │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief Lexer state for incremental scanning.  Stack-allocated. */
typedef struct {
  const Source     *src;   /**< Source being scanned (not owned).  */
  size_t            pos;   /**< Current byte offset.               */
  uint32_t          line;  /**< Current line (1-based).            */
  uint32_t          col;   /**< Current column (1-based).          */
  LexerDiagnostics *diag;  /**< Diagnostic sink (may be NULL).    */
} Lexer;

/** @brief Initializes lexer state: pos=0, line=1, col=1. */
void lexer_init(Lexer *l, const Source *src);

/** @brief Produces the next token.  Returns TOK_EOF at end of source. */
Token lexer_next(Lexer *l);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  3. TOKEN STREAM — dynamic array of tokens                             │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief Allocates storage for a token stream. */
void token_stream_init(TokenStream *ts, size_t initial_capacity);

/** @brief Frees all memory owned by a token stream. */
void token_stream_free(TokenStream *ts);

/** @brief Appends a token (grows buffer if needed, silently drops on OOM). */
void token_stream_push(TokenStream *ts, Token tok);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  4. BATCH TOKENIZATION                                                 │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/**
 * @brief Tokenizes an entire source file into a token stream.
 *
 * @param src      Source to scan.
 * @param out      Pre-initialized stream (receives tokens).
 * @param skip_ws  Non-zero to filter whitespace/newline/comment tokens.
 * @param diag     Diagnostic sink, or NULL.
 * @return         0 on success (always — errors go into diag).
 */
int lexer_tokenize(const Source *src, TokenStream *out, int skip_ws,
                   LexerDiagnostics *diag);

#ifdef __cplusplus
}
#endif
#endif /* MSF_LEXER_INTERNAL_H */
