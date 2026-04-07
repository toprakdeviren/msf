/**
 * @file fast.c
 * @brief Performance-critical lexer scanners: keyword detect, identifier scan,
 *        number scan, and string scan.
 *
 * These functions are the lexer hot path — called for every token.
 * They use SWAR (8-byte word-at-a-time), bitmap lookups, and memchr
 * (which maps to NEON/AVX2/WASM128 SIMD on modern platforms).
 *
 * Separated from char_tables.h so the tables stay as pure data and the
 * scanner logic compiles as a regular translation unit.
 */
#include "../private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Keyword Detection
 *
 * FNV-1a hash + 2-probe open addressing + 16-byte memcmp.
 * Clang -O2: FIRST reject ~95%, memcmp = single vmovdqu + vpcmpeqb (SSE4.2).
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Detects whether a scanned identifier is a Swift keyword.
 *
 * Binary search over the sorted LEX_KEYWORDS[] table (67 entries).
 * 6-7 iterations max.  Each iteration: one strlen-free memcmp
 * (length pre-checked).
 *
 * @param str  Identifier bytes (not NUL-terminated).
 * @param len  Identifier length in bytes.
 * @return     KW_* id (> 0) if keyword, 0 if plain identifier.
 */
uint32_t keyword_detect(const uint8_t *str, uint32_t len) {
  if (len < 2u || len > 16u)
    return 0u;

  uint32_t lo = 0, hi = LEX_KEYWORD_COUNT;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const char *kw = LEX_KEYWORDS[mid].text;

    /* Compare first byte for fast reject, then full memcmp. */
    int cmp = (int)str[0] - (int)(unsigned char)kw[0];
    if (cmp == 0) {
      /* First byte matches — compare full string. */
      size_t kw_len = strlen(kw);
      if (len < kw_len)
        cmp = -1;
      else if (len > kw_len)
        cmp = 1;
      else
        cmp = memcmp(str, kw, len);
    }

    if (cmp < 0)
      hi = mid;
    else if (cmp > 0)
      lo = mid + 1;
    else
      return LEX_KEYWORDS[mid].id;
  }
  return 0u;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Identifier Scan — 8-byte SWAR + bitmap fast-path
 *
 * 256-bit bitmap: bit i is set if ASCII byte i is identifier-continue.
 *   bytes 0x00-0x3F: $, 0-9
 *   bytes 0x40-0x7F: A-Z, _, a-z
 *   bytes 0x80-0xFF: all zero (non-ASCII → Unicode path)
 * ═══════════════════════════════════════════════════════════════════════════════ */

static const uint64_t LEX_IDENT_BITMAP[2] = {
    0x03FF001000000000ULL,  /* 0x00-0x3F : $ (0x24), 0-9 (0x30-0x39) */
    0x07FFFFFE87FFFFFEULL,  /* 0x40-0x7F : A-Z (0x41-0x5A), _ (0x5F), a-z (0x61-0x7A) */
};

/* Single byte check — 1 table read + 1 shift + 1 AND */
#define IDENT_OK(b) ((LEX_IDENT_BITMAP[(uint8_t)(b) >> 6] >> ((uint8_t)(b) & 63u)) & 1u)

/**
 * @brief Scans an identifier and checks if it is a keyword.
 *
 * Two-phase scan:
 *   1. ASCII fast-path — reads 8 bytes at a time (SWAR), checks all 8
 *      against a 256-bit bitmap in a single operation.  Covers ~95% of
 *      identifiers without entering the byte loop.
 *   2. Tail — handles remaining ASCII bytes one-by-one, plus multi-byte
 *      UTF-8 sequences decoded and checked against Swift's Unicode
 *      identifier-head / identifier-continue range tables.
 *
 * After scanning, calls keyword_detect() to classify the result.
 *
 * @param src    Source bytes.
 * @param pos    Start position (at first identifier byte).
 * @param len    Source length.
 * @param kw_id  Receives KW_* id if keyword, 0 if plain identifier.
 * @return       Number of bytes consumed (0 if not an identifier).
 */
uint32_t scan_ident(const uint8_t *src, uint32_t pos,
                    uint32_t len, uint32_t *kw_id) {
  uint32_t start = pos;

  /* ASCII fast-path: 8-byte SWAR */
  while (pos + 8 <= len) {
    uint64_t word;
    __builtin_memcpy(&word, src + pos, 8);
    if (word & 0x8080808080808080ULL) break;
    unsigned ok =
        IDENT_OK(word      ) & IDENT_OK(word >>  8) &
        IDENT_OK(word >> 16) & IDENT_OK(word >> 24) &
        IDENT_OK(word >> 32) & IDENT_OK(word >> 40) &
        IDENT_OK(word >> 48) & IDENT_OK(word >> 56);
    if (!ok) break;
    pos += 8;
  }

  /* Tail: remaining ASCII bytes + Unicode multi-byte */
  while (pos < len) {
    uint8_t cc = src[pos];

    if (cc < 0x80) {
      if (!IDENT_OK(cc)) break;
      pos++;
    } else {
      /* Multi-byte UTF-8 → decode codepoint → Swift spec range check */
      uint32_t cp = 0;
      uint32_t seq_len = 0;
      if ((cc & 0xE0) == 0xC0)      { cp = cc & 0x1F; seq_len = 2; }
      else if ((cc & 0xF0) == 0xE0) { cp = cc & 0x0F; seq_len = 3; }
      else if ((cc & 0xF8) == 0xF0) { cp = cc & 0x07; seq_len = 4; }
      else { break; }

      if (pos + seq_len > len) break;

      for (uint32_t i = 1; i < seq_len; i++) {
        uint8_t cont = src[pos + i];
        if ((cont & 0xC0) != 0x80) { seq_len = 0; break; }
        cp = (cp << 6) | (cont & 0x3F);
      }
      if (seq_len == 0) break;

      int ok_cp;
      if (pos == start)
        ok_cp = unicode_is_ident_head(cp);
      else
        ok_cp = unicode_is_ident_continue(cp);

      if (!ok_cp) break;
      pos += seq_len;
    }
  }

  *kw_id = keyword_detect(src + start, pos - start);
  return pos - start;
}

#undef IDENT_OK

/* ═══════════════════════════════════════════════════════════════════════════════
 * Number Scan
 *
 * Handles decimal, hex (0x), octal (0o), binary (0b), underscore separators,
 * fractional parts, and exponents (e/E for decimal, p/P for hex float).
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Scans a numeric literal: decimal, hex (0x), octal (0o), binary (0b).
 *
 * Handles underscore separators (1_000), fractional parts (.5), and
 * exponents (e/E for decimal, p/P for hex float).  Sets *ttype to
 * TT_FLOAT_LITERAL or TT_INTEGER_LITERAL.
 *
 * @param src    Source bytes.
 * @param pos    Start position (at first digit).
 * @param len    Source length.
 * @param ttype  Receives TT_FLOAT_LITERAL or TT_INTEGER_LITERAL.
 * @return       Number of bytes consumed.
 */

/** @brief Consumes hex digits and underscores, returns new position. */
static uint32_t scan_hex_digits(const uint8_t *src, uint32_t pos, uint32_t len) {
  while (pos < len && (is_hex(src[pos]) || src[pos] == '_'))
    pos++;
  return pos;
}

/** @brief Consumes an optional sign (+/-), returns new position. */
static uint32_t scan_sign(const uint8_t *src, uint32_t pos, uint32_t len) {
  if (pos < len && (src[pos] == '+' || src[pos] == '-'))
    pos++;
  return pos;
}

/** @brief Consumes decimal digits, returns new position. */
static uint32_t scan_decimal_digits(const uint8_t *src, uint32_t pos, uint32_t len) {
  while (pos < len && src[pos] >= '0' && src[pos] <= '9')
    pos++;
  return pos;
}

uint32_t scan_number(const uint8_t *src, uint32_t pos,
                     uint32_t len, uint32_t *ttype) {
  uint32_t start = pos;
  int is_float = 0;

  /* Hex/octal/binary prefix: 0x 0o 0b */
  if (pos + 1 < len && src[pos] == '0') {
    uint8_t nx = src[pos + 1];

    /* 0x / 0X — hexadecimal */
    if (nx == 'x' || nx == 'X') {
      pos = scan_hex_digits(src, pos + 2, len);

      /* Hex fraction: 0x1.Ap3 */
      if (pos < len && src[pos] == '.') {
        uint8_t after = (pos + 1 < len) ? src[pos + 1] : 0;
        if (is_hex(after)) {
          is_float = 1;
          pos = scan_hex_digits(src, pos + 1, len);
        }
      }

      /* Binary exponent: p/P [+-] decimal_digits */
      if (pos < len && (src[pos] == 'p' || src[pos] == 'P')) {
        is_float = 1;
        pos = scan_decimal_digits(src, scan_sign(src, pos + 1, len), len);
      }

      *ttype = is_float ? TT_FLOAT_LITERAL : TT_INTEGER_LITERAL;
      return pos - start;
    }

    /* 0o — octal */
    if (nx == 'o') {
      pos += 2;
      while (pos < len && src[pos] >= '0' && src[pos] <= '7')
        pos++;
      *ttype = TT_INTEGER_LITERAL;
      return pos - start;
    }

    /* 0b — binary */
    if (nx == 'b') {
      pos += 2;
      while (pos < len && (src[pos] == '0' || src[pos] == '1' || src[pos] == '_'))
        pos++;
      *ttype = TT_INTEGER_LITERAL;
      return pos - start;
    }
  }

  /* Decimal digits (allow _ separator) */
  while (pos < len && (LEX_CHAR_CLASS[src[pos]] == CC_DIGIT || src[pos] == '_'))
    pos++;

  /* Fraction: 3.14 */
  if (pos < len && src[pos] == '.') {
    uint8_t nx = (pos + 1 < len) ? src[pos + 1] : 0;
    if (LEX_CHAR_CLASS[nx] == CC_DIGIT) {
      is_float = 1;
      pos++;
      while (pos < len && (LEX_CHAR_CLASS[src[pos]] == CC_DIGIT || src[pos] == '_'))
        pos++;
    }
  }

  /* Exponent: e/E [+-] digits */
  if (pos < len && (src[pos] == 'e' || src[pos] == 'E')) {
    is_float = 1;
    pos = scan_decimal_digits(src, scan_sign(src, pos + 1, len), len);
  }

  *ttype = is_float ? TT_FLOAT_LITERAL : TT_INTEGER_LITERAL;
  return pos - start;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * String Scan — memchr-based, SIMD-accelerated
 *
 * Strategy:
 *   1) two separate memchr for closing quote and '\' (both NEON/AVX2/WASM128)
 *   2) jump to whichever comes first — skips large string blocks in one pass
 *   3) if '\', skip the escaped char and repeat
 *   4) newline counting within segment via memchr('\n')
 *
 * For long escape-free strings, 2 memchr = practical single pass (~O(n/16)).
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Scans a string literal body using memchr (SIMD-accelerated).
 *
 * Strategy: two parallel memchr searches — one for the closing quote,
 * one for backslash.  Whichever comes first determines the segment end.
 * Large escape-free strings are skipped in essentially one pass (~O(n/16)).
 *
 * Newlines within each segment are counted via a third memchr('\n') call,
 * also SIMD-accelerated on NEON/AVX2/WASM128 platforms.
 *
 * @param src    Source bytes.
 * @param pos    Position of the opening quote character.
 * @param len    Source length.
 * @param lines  Receives the number of newlines inside the string.
 * @return       Number of bytes consumed (including both quotes).
 */
uint32_t scan_string_body(const uint8_t *src, uint32_t pos,
                          uint32_t len, uint32_t *lines) {
  uint32_t       start = pos;
  uint8_t        q     = src[pos++];
  const uint8_t *body  = src + pos;
  const uint8_t *eos   = src + len;

  for (;;) {
    const uint8_t *nq = (const uint8_t *)memchr(body, (int)q,    (size_t)(eos - body));
    const uint8_t *nb = (const uint8_t *)memchr(body, (int)'\\', (size_t)(eos - body));

    const uint8_t *seg = nq ? nq : eos;
    if (nb && nb < seg) seg = nb;

    /* Count '\n' within segment */
    const uint8_t *scan = body;
    while (scan < seg) {
      scan = (const uint8_t *)memchr(scan, (int)'\n', (size_t)(seg - scan));
      if (!scan) break;
      (*lines)++;
      scan++;
    }

    if (!nq || (nb && nb < nq && seg == eos)) {
      body = eos;
      break;
    }

    if (seg == nq) {
      body = nq + 1;
      break;
    }

    /* Backslash: skip '\' and the next char */
    body = nb + 2;
    if (body > eos) { body = eos; break; }
  }

  return (uint32_t)(body - src) - start;
}
