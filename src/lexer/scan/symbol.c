/**
 * @file symbol.c
 * @brief Operator, regex literal, and punctuation scanners.
 *
 * When the main lexer loop encounters a byte that isn't whitespace,
 * identifier-start, digit, or quote, it lands here.  scan_symbol()
 * is the catch-all dispatcher:
 *
 *   1. Comments?  "/" followed by "/" or "*" → delegate to scan_comment.c.
 *   2. Multi-char operator?  Walk the MULTI_OPS table (longest first).
 *   3. Regex literal?  "/" followed by non-whitespace → try scan_regex_literal().
 *   4. Single-char operator or punctuation?  Use LEX_CHAR_TOKEN[] table.
 *
 * Regex literals are ambiguous with division — the heuristic here is:
 * if '/' is NOT followed by whitespace, closing bracket, comma, semicolon,
 * or colon, try to scan it as a regex.  If that fails (no closing '/'),
 * fall through to single-char '/' operator.
 */
#include "../private.h"

/**
 * @brief Attempts to scan a regex literal: /pattern/.
 *
 * Scans forward from the opening '/' looking for an unescaped closing '/'.
 * Backslash-escaped characters inside the regex are skipped.
 * Fails (returns 0) if a newline is hit before the closing slash — Swift
 * regex literals cannot span multiple lines.
 *
 * @param l    Lexer state (pos at the opening '/').
 * @param s    Source bytes.
 * @param len  Source length.
 * @param sp   Start byte offset.
 * @param sl   Start line (1-based).
 * @param sc   Start column (1-based).
 * @param out  Receives the token on success.
 * @return     1 if a regex literal was scanned, 0 otherwise.
 */
int scan_regex_literal(Lexer *l, const uint8_t *s, uint32_t len,
                       uint32_t sp, uint32_t sl, uint32_t sc, Token *out) {
  for (uint32_t p = l->pos + 1; p < len; p++) {
    if (s[p] == '\\' && p + 1 < len) { p++; continue; }
    if (s[p] == '/') {
      uint32_t tl = p - sp + 1;
      l->pos = p + 1; l->col += tl;
      *out = (Token){TOK_REGEX_LIT, sp, tl, sl, sc, KW_NONE, OP_NONE};
      return 1;
    }
    if (IS_NEWLINE(s[p])) return 0;
  }
  return 0;
}

/**
 * @brief Scans an operator, comment, regex, or punctuation token.
 *
 * This is the "everything else" dispatcher called by lexer_next() when
 * the current byte doesn't match whitespace, identifier, number, or string.
 *
 * Dispatch order (first match wins):
 *   1. "//" → line comment (scan_line_comment)
 *   2. "/" + "*" → block comment (scan_block_comment)
 *   3. Multi-char operators: walks MULTI_OPS[] longest-first ("===" before "==")
 *   4. "/" + non-delimiter → try regex literal
 *   5. Single-char: LEX_CHAR_TOKEN[c] → TOK_OPERATOR, TOK_PUNCT, or TOK_UNKNOWN
 *
 * @param l   Lexer state.
 * @param s   Source bytes.
 * @param len Source length.
 * @param c   Current byte (s[l->pos]).
 * @param sp  Start byte offset.
 * @param sl  Start line (1-based).
 * @param sc  Start column (1-based).
 * @return    The scanned token.
 */
Token scan_symbol(Lexer *l, const uint8_t *s, uint32_t len, uint8_t c,
                  uint32_t sp, uint32_t sl, uint32_t sc) {
  uint8_t nx = (l->pos + 1 < len) ? s[l->pos + 1] : 0;

  if (c == '/' && nx == '/') return scan_line_comment(l, s, len, sp, sl, sc);
  if (c == '/' && nx == '*') return scan_block_comment(l, s, len, sp, sl, sc);

  for (int i = 0; MULTI_OPS[i].op; i++) {
    uint8_t ml = MULTI_OPS[i].len;
    if (l->pos + ml <= len && memcmp(s + l->pos, MULTI_OPS[i].op, ml) == 0) {
      ADVANCE_BY(l, ml);
      return (Token){TOK_OPERATOR, sp, ml, sl, sc, KW_NONE, MULTI_OPS[i].kind};
    }
  }

  if (c == '/' && nx != ' ' && nx != '\t' && !IS_NEWLINE(nx) &&
      nx != ')' && nx != ']' && nx != '}' && nx != ',' && nx != ';' &&
      nx != ':' && nx != 0) {
    Token reg;
    if (scan_regex_literal(l, s, len, sp, sl, sc, &reg)) return reg;
  }

  uint8_t raw_tt = LEX_CHAR_TOKEN[c];
  TokenType tt = (raw_tt == TT_OPERATOR) ? TOK_OPERATOR
               : (raw_tt == TT_PUNCT)    ? TOK_PUNCT : TOK_UNKNOWN;
  ADVANCE(l);
  return (Token){tt, sp, 1, sl, sc, KW_NONE, OP_NONE};
}
