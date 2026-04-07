/**
 * @file string.c
 * @brief String literal scanners — regular, triple-quoted, and raw strings.
 *
 * Swift has 4 string literal forms:
 *
 *   "hello"                — regular double-quoted
 *   \"""multi\nline\"""    — triple-quoted (multiline)
 *   #"no \escape"#         — raw string (# count must match)
 *   #\"""raw\nmulti\"""#   — raw triple-quoted
 *
 * This file handles all four.  Escape validation (\n, \t, \u{XXXX},
 * \(interpolation)) is a separate function called after scanning.
 *
 * On unterminated input, the scanner records a diagnostic and returns
 * a partial token covering everything up to EOF.  It never crashes.
 */
#include "../private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Escape Validation
 *
 * Called by scan_string() after the token boundaries are known.
 * Walks the string body (between quotes) and checks every backslash escape.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Validates escape sequences in s[start..end).
 *
 * Recognized escapes:
 *   \n \t \r \0 \" \' \\   — simple character escapes
 *   \(expr)                 — string interpolation (balanced parens skipped)
 *   \u{XXXX}               — Unicode escape (1+ hex digits)
 *
 * On first invalid escape, writes the error position to out_line/out_col
 * and returns 0.
 *
 * @param s          Source bytes.
 * @param start      First byte to check (after opening quote).
 * @param end        Past-the-end byte (before closing quote).
 * @param start_line Line of the first byte.
 * @param start_col  Column of the first byte.
 * @param out_line   Receives line of the invalid escape.
 * @param out_col    Receives column of the invalid escape.
 * @return           1 if all escapes valid, 0 on first invalid.
 */
/** @brief Cursor for walking string content — tracks byte position, line, column. */
typedef struct { uint32_t pos, line, col; } EscCursor;

/** @brief Returns 1 if c is a simple escape character: n t r 0 " ' \\ */
static int is_simple_escape(uint8_t c) {
  return c == 'n' || c == 't' || c == 'r' || c == '0' ||
         c == '"' || c == '\'' || c == '\\';
}

/** @brief Skips a \\(expr) interpolation by balancing parentheses. */
static void skip_interpolation(const uint8_t *s, uint32_t end, EscCursor *c) {
  c->pos += 2; c->col += 2; /* skip \( */
  for (int depth = 1; c->pos < end && depth > 0; c->pos++) {
    if (s[c->pos] == '(')      depth++;
    else if (s[c->pos] == ')') depth--;
    if (s[c->pos] == '\n') { c->line++; c->col = 1; } else { c->col++; }
  }
}

/** @brief Validates a \\u{XXXX} escape.  Returns 1 on success, 0 on invalid. */
static int validate_unicode_escape(const uint8_t *s, uint32_t end, EscCursor *c) {
  uint32_t j = c->pos + 3; /* past \u{ */
  while (j < end && is_hex(s[j])) j++;
  if (j == c->pos + 3 || j >= end || s[j] != '}')
    return 0;
  uint32_t len = (j + 1) - c->pos;
  c->pos += len; c->col += len;
  return 1;
}

/**
 * @brief Validates all escape sequences in a string body.
 *
 * Walks [start, end) checking every backslash.  On the first invalid
 * escape, writes the error position into out_line/out_col and returns 0.
 */
int validate_string_escapes(const uint8_t *s, uint32_t start, uint32_t end,
                            uint32_t start_line, uint32_t start_col,
                            uint32_t *out_line, uint32_t *out_col) {
  EscCursor c = { start, start_line, start_col };

  while (c.pos < end) {
    uint8_t ch = s[c.pos];

    if (ch == '\n') { c.line++; c.col = 1; c.pos++; continue; }
    if (ch != '\\') { c.col++; c.pos++; continue; }
    if (c.pos + 1 >= end) goto fail;

    uint8_t next = s[c.pos + 1];

    if (is_simple_escape(next))                              { c.pos += 2; c.col += 2; }
    else if (next == '(')                                    { skip_interpolation(s, end, &c); }
    else if (next == 'u' && c.pos + 2 < end && s[c.pos + 2] == '{') {
      if (!validate_unicode_escape(s, end, &c)) goto fail;
    }
    else goto fail;
  }
  return 1;

fail:
  *out_line = c.line;
  *out_col = c.col;
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Regular + Triple-Quoted Strings
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Scans a triple-quoted string: \"""...\""". */
static Token scan_triple_quoted(Lexer *l, const uint8_t *s, uint32_t len,
                                uint32_t sp, uint32_t sl, uint32_t sc) {
  uint32_t p = l->pos + 3, extra = 0;
  while (p < len) {
    if (s[p] == '\\' && p + 1 < len) { p += 2; continue; }
    if (p + 2 < len && IS_TRIPLE_QUOTE(s, p)) break;
    if (s[p] == '\n') extra++;
    p++;
  }
  if (p + 2 >= len || s[p] != '"') {
    lexer_diag_record(l, sl, sc, DIAG_UNTERM_TRIPLE);
    l->line += extra; l->pos = len; l->col = 1;
    return make_string_tok(sp, (uint32_t)(len - sp), sl, sc);
  }
  uint32_t tl = (p + 3) - l->pos;
  advance_past_token(l, s, l->pos, tl, extra);
  return make_string_tok(sp, tl, sl, sc);
}

/** @brief Scans a regular string: "...". */
static Token scan_regular_string(Lexer *l, const uint8_t *s, uint32_t len,
                                 uint32_t sp, uint32_t sl, uint32_t sc) {
  uint32_t extra = 0, tl = scan_string_body(s, l->pos, len, &extra);
  if (l->pos + tl > len) {
    lexer_diag_record(l, sl, sc, DIAG_UNTERM_STRING);
  } else if (tl > 2) {
    uint32_t el, ec;
    if (!validate_string_escapes(s, l->pos + 1, l->pos + tl - 1, sl, sc + 1, &el, &ec))
      lexer_diag_record(l, el, ec, DIAG_INVALID_ESCAPE);
  }
  advance_past_token(l, s, l->pos, tl, extra);
  return make_string_tok(sp, tl, sl, sc);
}

/**
 * @brief Scans a "..." or \"""...\""" string literal.
 *
 * Detects single-quote misuse ('hello') and reports a diagnostic.
 * For triple-quoted strings, counts newlines for multiline tracking.
 * After scanning, validates escape sequences inside the string body.
 *
 * @return A TOK_STRING_LIT token (or partial token on unterminated input).
 */
Token scan_string(Lexer *l, const uint8_t *s, uint32_t len,
                  uint32_t sp, uint32_t sl, uint32_t sc) {
  if (s[l->pos] == '\'')
    lexer_diag_record(l, sl, sc, DIAG_SINGLE_QUOTE);

  if (l->pos + 2 < len && IS_TRIPLE_QUOTE(s, l->pos))
    return scan_triple_quoted(l, s, len, sp, sl, sc);

  return scan_regular_string(l, s, len, sp, sl, sc);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Raw Strings: #"..."#
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Checks if the raw string closing delimiter starts at position hp.
 *
 * For regular raw: "###  →  closing is `"` followed by hc '#' characters.
 * For triple raw:  """### → closing is `"""` followed by hc '#' characters.
 *
 * @return Number of bytes consumed by the closing delimiter, or 0 if no match.
 */
static uint32_t match_raw_close(const uint8_t *s, uint32_t hp, uint32_t len,
                                uint32_t hc, int triple) {
  uint32_t quote_len = triple ? 3 : 1;
  /* Check quote(s) */
  if (triple) {
    if (hp + 2 >= len || !IS_TRIPLE_QUOTE(s, hp)) return 0;
  } else {
    if (s[hp] != '"') return 0;
  }
  /* Count trailing '#' — must match hc exactly */
  uint32_t h = 0;
  while (h < hc && hp + quote_len + h < len && s[hp + quote_len + h] == '#')
    h++;
  return (h == hc) ? quote_len + hc : 0;
}

/**
 * @brief Scans a raw string literal: #"..."#, ##"..."##, #\"""...\"""#, etc.
 *
 * Counts opening '#' characters to determine the closing delimiter.
 * No escape processing — backslashes are literal inside raw strings.
 *
 * Returns a zero-length TOK_UNKNOWN sentinel if the '#' sequence is not
 * followed by '"' (meaning this isn't a raw string — lexer_next tries
 * another scanner).
 *
 * @return TOK_STRING_LIT on success, TOK_UNKNOWN sentinel or error token otherwise.
 */
Token scan_raw_string(Lexer *l, const uint8_t *s, uint32_t len,
                      uint32_t sp, uint32_t sl, uint32_t sc) {
  /* Count opening '#' characters */
  uint32_t hc = 0, hp = l->pos;
  while (hp < len && s[hp] == '#') { hc++; hp++; }
  if (hp >= len || s[hp] != '"')
    return (Token){TOK_UNKNOWN, 0, 0, 0, 0, KW_NONE, OP_NONE};

  int triple = (hp + 2 < len && IS_TRIPLE_QUOTE(s, hp));
  hp += triple ? 3 : 1;
  uint32_t extra = 0;

  /* Scan body until closing delimiter */
  while (hp < len) {
    uint32_t close_len = match_raw_close(s, hp, len, hc, triple);
    if (close_len) {
      uint32_t tl = (hp + close_len) - l->pos;
      advance_past_token(l, s, l->pos, tl, extra);
      return make_string_tok(sp, tl, sl, sc);
    }
    if (s[hp] == '\n') extra++;
    hp++;
  }

  /* Unterminated */
  lexer_diag_record(l, sl, sc, DIAG_UNTERM_RAW_STRING);
  uint32_t el = hp - l->pos;
  advance_past_token(l, s, l->pos, el, extra);
  return (Token){TOK_UNKNOWN, sp, el, sl, sc, KW_NONE, OP_NONE};
}
