/**
 * @file diag.c
 * @brief Lexer diagnostic recording.
 *
 * When the lexer encounters something wrong (unterminated string, invalid
 * escape, single-quoted string), it doesn't stop — it records the problem
 * and keeps scanning.  This file provides the recording mechanism.
 *
 * Two functions write diagnostics:
 *
 *   lexer_diag_record() — called by scanners (internal, takes Lexer*)
 *   lexer_diag_push()   — called by parser for post-lex warnings (public, takes LexerDiagnostics*)
 *
 * Both write into the same LexerDiagnostics struct: an array of
 * (message, line, col) triples, capped at LEXER_DIAG_MAX entries.
 * When the array is full, additional diagnostics are silently dropped.
 *
 * Usage by scanners:
 *   lexer_diag_record(l, line, col, "unterminated string literal");
 *
 * Usage by consumer:
 *   LexerDiagnostics diag;
 *   lexer_diag_init(&diag);
 *   lexer_tokenize(&src, &ts, 1, &diag);
 *   for (size_t i = 0; i < diag.count; i++)
 *       printf("%u:%u: %s\n", diag.line[i], diag.col[i], diag.message[i]);
 */
#include "private.h"

/**
 * @brief Resets the diagnostic accumulator to empty.
 *
 * Call before lexer_tokenize() if you're reusing a LexerDiagnostics struct.
 *
 * @param d  Diagnostic accumulator to reset (may be NULL — no-op).
 */
void lexer_diag_init(LexerDiagnostics *d) {
  if (d) d->count = 0;
}

/**
 * @brief Records a diagnostic from inside a scanner.
 *
 * Called by scan_string, scan_block_comment, etc. when they encounter
 * a recoverable error.  Writes into l->diag if present.
 *
 * @param l     Lexer state (l->diag may be NULL — silently ignored).
 * @param line  1-based line number where the issue was found.
 * @param col   1-based column number.
 * @param fmt   printf-style format string.
 */
void lexer_diag_record(Lexer *l, uint32_t line, uint32_t col,
                       const char *fmt, ...) {
  if (!l->diag || l->diag->count >= LEXER_DIAG_MAX) return;
  size_t i = l->diag->count++;
  l->diag->line[i] = line;
  l->diag->col[i] = col;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(l->diag->message[i], sizeof(l->diag->message[i]), fmt, ap);
  va_end(ap);
}

/**
 * @brief Records a diagnostic from outside the lexer (public API).
 *
 * Used by the parser to add post-lex warnings (e.g. "expected expression
 * after operator") into the same diagnostic stream.
 *
 * @param d     Diagnostic accumulator (may be NULL — silently ignored).
 * @param line  1-based line number.
 * @param col   1-based column number.
 * @param fmt   printf-style format string.
 */
void lexer_diag_push(LexerDiagnostics *d, uint32_t line, uint32_t col,
                     const char *fmt, ...) {
  if (!d || d->count >= LEXER_DIAG_MAX) return;
  size_t i = d->count++;
  d->line[i] = line;
  d->col[i] = col;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(d->message[i], sizeof(d->message[i]), fmt, ap);
  va_end(ap);
}
