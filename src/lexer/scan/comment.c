/**
 * @file comment.c
 * @brief Comment scanners — line (//) and block comments.
 *
 * Swift supports two comment styles:
 *   - Line comments: // everything until end of line
 *   - Block comments: delimited by opening and closing markers, nestable
 *
 * Both scanners consume the entire comment and return a single TOK_COMMENT
 * token.  The lexer_tokenize() caller can filter these out with skip_ws=1.
 *
 * Block comments support nesting — each opening marker increments a depth
 * counter, each closing marker decrements it.  The scanner runs until
 * depth reaches 0 or EOF (in which case a diagnostic is recorded).
 */
#include "../private.h"

/**
 * @brief Scans a line comment: // ... until newline or EOF.
 *
 * Uses memchr for fast newline scanning (skips the comment body in one call).
 * Does not consume the newline itself — that becomes a separate TOK_NEWLINE.
 *
 * @param l   Lexer state (pos must be at the first '/').
 * @param s   Source bytes.
 * @param len Source length.
 * @param sp  Start byte offset.
 * @param sl  Start line (1-based).
 * @param sc  Start column (1-based).
 * @return    A TOK_COMMENT token spanning the entire comment.
 */
Token scan_line_comment(Lexer *l, const uint8_t *s, uint32_t len,
                        uint32_t sp, uint32_t sl, uint32_t sc) {
  l->pos += 2; /* skip "//" */
  const uint8_t *p = s + l->pos, *end = s + len;
  /* The comment ends at the first '\n' or '\r'.  Scan for '\n' first (memchr
   * stops at the first hit, so this is O(line)), then look for an earlier '\r'
   * only *within this line* — never past the newline.  Scanning '\r' over the
   * whole remaining file would be O(file) per comment, i.e. O(n^2) on sources
   * with a comment on every line (LF-only files have no '\r' to stop it). */
  const uint8_t *nl_n = memchr(p, '\n', (size_t)(end - p));
  const uint8_t *line_end = nl_n ? nl_n : end;
  const uint8_t *nl_r = memchr(p, '\r', (size_t)(line_end - p));
  const uint8_t *nl = nl_r ? nl_r : line_end;
  l->col += (uint32_t)(nl - p) + 2;
  l->pos = (uint32_t)(nl - s);
  return MAKE_COMMENT_TOK(sp, l, sl, sc);
}

/**
 * @brief Scans a block comment with nesting support.
 *
 * Tracks a depth counter: each nested opening increments, each closing
 * decrements.  Handles newlines for line/col tracking.  Records a
 * diagnostic if EOF is reached before the comment is closed.
 *
 * @param l   Lexer state (pos must be at the '/').
 * @param s   Source bytes.
 * @param len Source length.
 * @param sp  Start byte offset.
 * @param sl  Start line (1-based).
 * @param sc  Start column (1-based).
 * @return    A TOK_COMMENT token spanning the entire block comment.
 */
Token scan_block_comment(Lexer *l, const uint8_t *s, uint32_t len,
                         uint32_t sp, uint32_t sl, uint32_t sc) {
  ADVANCE_BY(l, 2); /* skip opening */
  for (uint32_t depth = 1; l->pos < len && depth > 0; ) {
    if (s[l->pos] == '\n')
      { NEWLINE_ADVANCE(l); }
    else if (IS_BLOCK_OPEN(s, l->pos, len))
      { depth++; ADVANCE_BY(l, 2); }
    else if (IS_BLOCK_CLOSE(s, l->pos, len))
      { depth--; ADVANCE_BY(l, 2); }
    else
      { ADVANCE(l); }
  }
  if (l->pos >= len)
    lexer_diag_record(l, sl, sc, DIAG_UNTERM_BLOCK_COMMENT);
  return MAKE_COMMENT_TOK(sp, l, sl, sc);
}
