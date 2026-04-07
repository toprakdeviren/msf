/**
 * @file lexer.c
 * @brief Lexer entry points: init, next, tokenize.
 *
 * This is the file you read first.  It dispatches to scanners
 * in scan_comment.c, scan_string.c, and scan_symbol.c.
 *
 * Other lexer files:
 *   helpers.c      — tables, make_string_tok, advance_past_token
 *   diag.c         — diagnostic recording
 *   scan_comment.c — line + block comment scanners
 *   scan_string.c  — escape validation, string + raw string scanners
 *   scan_symbol.c  — regex, operator, punctuation scanners
 *   token.c        — token_text, token_type_name, stream management
 */
#include "private.h"

#define LEXER_MAX_SOURCE_LEN 0xFFFFFFFEu

/**
 * @brief Initializes lexer state for scanning a source file.
 *
 * Sets position to the start of the source, line/col to 1:1, and
 * clears the diagnostic pointer.  Caller must set l->diag after
 * init if diagnostics are desired (lexer_tokenize does this automatically).
 *
 * @param l    Lexer state to initialize.
 * @param src  Source to scan (must outlive the lexer).
 */
void lexer_init(Lexer *l, const Source *src) {
  l->src  = src;
  l->pos  = 0;
  l->line = 1;
  l->col  = 1;
  l->diag = NULL;
}

/**
 * @brief Produces the next token from the source.
 *
 * This is the core dispatch loop.  Each byte is classified in two steps:
 *
 *   1. LEX_CHAR_CLASS[byte] → character class (CC_WHITESPACE, CC_NEWLINE, etc.)
 *   2. LEX_ACTION[class]    → action code:
 *        0 = whitespace  — consume run of spaces/tabs
 *        1 = identifier  — scan_ident() + keyword lookup
 *        2 = number      — scan_number() (int or float)
 *        3 = string      — scan_string() (regular or triple-quoted)
 *        4 = symbol      — scan_symbol() (operators, comments, regex, punct)
 *
 *   Special cases checked first: '#' → raw string attempt, newlines (CR/LF).
 *
 * Returns TOK_EOF when the source is exhausted.  Never returns an error —
 * invalid input produces TOK_UNKNOWN tokens with diagnostics recorded.
 *
 * @param l  Lexer state (pos/line/col are advanced past the returned token).
 * @return   The next token.
 */
Token lexer_next(Lexer *l) {
  const uint8_t *s = (const uint8_t *)l->src->data;
  uint32_t len = (l->src->len <= LEXER_MAX_SOURCE_LEN)
                     ? (uint32_t)l->src->len : LEXER_MAX_SOURCE_LEN;

  if (l->pos >= len)
    return (Token){TOK_EOF, (uint32_t)l->pos, 0, l->line, l->col, KW_NONE, OP_NONE};

  uint32_t sp = l->pos, sl = l->line, sc = l->col;
  uint8_t c   = s[l->pos];
  uint8_t cls = LEX_CHAR_CLASS[c];
  uint8_t act = LEX_ACTION[cls];

  /* Newline */
  if (cls == CC_NEWLINE) {
    l->pos++;
    SKIP_CRLF(s, l->pos, len);
    l->line++; l->col = 1;
    return (Token){TOK_NEWLINE, sp, (uint32_t)(l->pos - sp), sl, sc, KW_NONE, OP_NONE};
  }

  /* Whitespace */
  if (act == 0 && cls == CC_WHITESPACE) {
    while (l->pos < len && LEX_CHAR_CLASS[s[l->pos]] == CC_WHITESPACE) ADVANCE(l);
    return (Token){TOK_WHITESPACE, sp, (uint32_t)(l->pos - sp), sl, sc, KW_NONE, OP_NONE};
  }

  /* Identifier / keyword */
  if (act == 1) {
    uint32_t kw_id = 0, tl = scan_ident(s, l->pos, len, &kw_id);
    if (tl == 0) {
      uint8_t lead = s[l->pos];
      uint32_t skip = 1;
      if      ((lead & 0xE0) == 0xC0) skip = 2;
      else if ((lead & 0xF0) == 0xE0) skip = 3;
      else if ((lead & 0xF8) == 0xF0) skip = 4;
      if (l->pos + skip > len) skip = len - l->pos;
      l->pos += skip; l->col++;
      return (Token){TOK_UNKNOWN, sp, skip, sl, sc, KW_NONE, OP_NONE};
    }
    l->col += tl; l->pos += tl;
    Keyword kw = map_kw_id(kw_id);
    return (Token){kw ? TOK_KEYWORD : TOK_IDENTIFIER, sp, tl, sl, sc, kw, OP_NONE};
  }

  /* Number */
  if (act == 2) {
    uint32_t ttype = TT_INTEGER_LITERAL, tl = scan_number(s, l->pos, len, &ttype);
    l->col += tl; l->pos += tl;
    return (Token){(ttype == TT_FLOAT_LITERAL) ? TOK_FLOAT_LIT : TOK_INTEGER_LIT,
                   sp, tl, sl, sc, KW_NONE, OP_NONE};
  }

  /* String */
  if (act == 3) return scan_string(l, s, len, sp, sl, sc);

  /* Raw string */
  if (IS_RAW_STRING_START(c)) { 
    Token r = scan_raw_string(l, s, len, sp, sl, sc); 
    if (r.len > 0) return r; 
  }

  /* Operator / comment / regex */
  return scan_symbol(l, s, len, c, sp, sl, sc);
}

/**
 * @brief Tokenizes an entire source file into a token stream.
 *
 * Calls lexer_next() in a loop until TOK_EOF.  When skip_ws is set,
 * whitespace, newline, and comment tokens are filtered out — the parser
 * typically doesn't need them.  The stream is always terminated by TOK_EOF.
 *
 * Memory: pre-allocates an estimated token count (src->len / 4, clamped
 * to [512, 16M]) and shrinks the array afterward if it's less than half used.
 * The caller must call token_stream_free() when done.
 *
 * Safety: the loop is bounded at src->len * 4 + 1024 iterations to prevent
 * infinite loops from lexer bugs — in practice, each iteration consumes
 * at least one byte.
 *
 * @param src      Source to tokenize.
 * @param out      Receives the token stream (caller frees with token_stream_free).
 * @param skip_ws  If non-zero, filter whitespace/newline/comment tokens.
 * @param diag     Diagnostic accumulator (may be NULL to ignore errors).
 * @return         0 on success (always succeeds — errors are recorded in diag).
 */
int lexer_tokenize(const Source *src, TokenStream *out, int skip_ws,
                   LexerDiagnostics *diag) {
  size_t est = src->len / 4;
  if (est < 512)    est = 512;
  if (est > 1u<<24) est = 1u << 24;

  token_stream_init(out, est);
  Lexer l;
  lexer_init(&l, src);
  l.diag = diag;

  for (size_t n = (size_t)src->len * 4 + 1024, i = 0; i < n; i++) {
    Token t = lexer_next(&l);
    if (!(skip_ws && (t.type == TOK_WHITESPACE || t.type == TOK_NEWLINE || t.type == TOK_COMMENT)))
      token_stream_push(out, t);
    if (t.type == TOK_EOF) break;
  }

  if (out->count == 0 || out->tokens[out->count - 1].type != TOK_EOF)
    token_stream_push(out, (Token){TOK_EOF, l.pos, 0, l.line, l.col, KW_NONE, OP_NONE});

  if (out->count > 0 && out->count < out->capacity / 2) {
    Token *shrink = realloc(out->tokens, out->count * sizeof(Token));
    if (shrink) { out->tokens = shrink; out->capacity = out->count; }
  }
  return 0;
}
