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
 * @param lexer    Lexer state to initialize.
 * @param src  Source to scan (must outlive the lexer).
 */
void lexer_init(Lexer *lexer, const Source *src) {
  lexer->src  = src;
  lexer->pos  = 0;
  lexer->line = 1;
  lexer->col  = 1;
  lexer->diag = NULL;
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
 * @param lexer  Lexer state (pos/line/col are advanced past the returned token).
 * @return   The next token.
 */
Token lexer_next(Lexer *lexer) {
  const uint8_t *s = (const uint8_t *)lexer->src->data;
  const uint32_t len = lexer->src->len <= LEXER_MAX_SOURCE_LEN
                      ? (uint32_t)lexer->src->len : LEXER_MAX_SOURCE_LEN;

  if (lexer->pos >= len)
    return (Token){
      TOK_EOF,
      (uint32_t)lexer->pos,
      0,
      lexer->line,
      lexer->col,
      KW_NONE,
      OP_NONE,
      0
    };

  const uint32_t sp = lexer->pos, sl = lexer->line, sc = lexer->col;
  const uint8_t c   = s[lexer->pos];
  const uint8_t cls = LEX_CHAR_CLASS[c];
  const uint8_t act = LEX_ACTION[cls];

  /* Newline */
  if (cls == CC_NEWLINE) {
    lexer->pos++;
    SKIP_CRLF(s, lexer->pos, len);
    lexer->line++; lexer->col = 1;
    return (Token){
      TOK_NEWLINE,
      sp,
      (uint32_t)(lexer->pos - sp),
      sl,
      sc,
      KW_NONE,
      OP_NONE,
      0
    };
  }

  /* Whitespace */
  if (act == 0 && cls == CC_WHITESPACE) {
    while (lexer->pos < len && LEX_CHAR_CLASS[s[lexer->pos]] == CC_WHITESPACE) ADVANCE(lexer);
    return (Token){TOK_WHITESPACE, sp, (uint32_t)(lexer->pos - sp), sl, sc, KW_NONE, OP_NONE, 0};
  }

  /* Identifier / keyword */
  if (act == 1) {
    uint32_t kw_id = 0;
    const uint32_t tl = scan_ident(s, lexer->pos, len, &kw_id);

    if (tl == 0) {
      const uint8_t lead = s[lexer->pos];
      uint32_t skip = 1;
      if      ((lead & 0xE0) == 0xC0) skip = 2;
      else if ((lead & 0xF0) == 0xE0) skip = 3;
      else if ((lead & 0xF8) == 0xF0) skip = 4;
      if (lexer->pos + skip > len) skip = len - lexer->pos;
      lexer->pos += skip; lexer->col++;
      return (Token){
        TOK_UNKNOWN,
        sp,
        skip,
        sl,
        sc,
        KW_NONE,
        OP_NONE,
        0
      };
    }
    lexer->col += tl; lexer->pos += tl;
    Keyword kw = map_kw_id(kw_id);
    /* Contextual keywords: keywords only in specific positions, valid
     * identifiers everywhere else. Lex them as identifiers but keep the keyword
     * id so the relevant parser can still recognize them.
     *   - prefix/postfix/infix: keywords only before `operator`
     *     (try_parse_fixity_decl), e.g. `var prefix: Character?`.
     *   - some/any: keywords only at the start of a type (parse_type checks the
     *     preserved keyword id); valid names elsewhere — `case any`,
     *     `static let any`, `let some = …`. */
    int kw_is_contextual =
        (kw == KW_PREFIX || kw == KW_POSTFIX || kw == KW_INFIX ||
         kw == KW_SOME || kw == KW_ANY);
    TokenType tt = (kw && !kw_is_contextual) ? TOK_KEYWORD : TOK_IDENTIFIER;
    return (Token){tt, sp, tl, sl, sc, kw, OP_NONE, 0};
  }

  /* Escaped identifier: `default`, `class`, etc.  Store the logical token
   * range without the backticks so downstream name lookup sees the identifier
   * text, not the escape delimiters. */
  if (c == '`') {
    uint32_t end = lexer->pos + 1;
    while (end < len && s[end] != '`' && !IS_NEWLINE(s[end]))
      end++;
    if (end < len && s[end] == '`' && end > lexer->pos + 1) {
      uint32_t inner_len = end - lexer->pos - 1;
      lexer->pos = end + 1;
      lexer->col += inner_len + 2;
      return (Token){TOK_IDENTIFIER, sp + 1, inner_len, sl, sc + 1,
                     KW_NONE, OP_NONE, 0};
    }
  }

  /* Number */
  if (act == 2) {
    uint32_t ttype = TT_INTEGER_LITERAL, tl = scan_number(s, lexer->pos, len, &ttype);
    lexer->col += tl; lexer->pos += tl;
    return (Token){(ttype == TT_FLOAT_LITERAL) ? TOK_FLOAT_LIT : TOK_INTEGER_LIT,
                   sp, tl, sl, sc, KW_NONE, OP_NONE, 0};
  }

  /* String */
  if (act == 3) return scan_string(lexer, s, len, sp, sl, sc);

  /* Raw string */
  if (IS_RAW_STRING_START(c)) {
    Token r = scan_raw_string(lexer, s, len, sp, sl, sc);
    if (r.len > 0) return r;
  }

  /* Operator / comment / regex */
  return scan_symbol(lexer, s, len, c, sp, sl, sc);
}

/**
 * @brief Tokenizes an entire source file into a token stream.
 *
 * Calls lexer_next() in a loop until TOK_EOF.  When skip_ws is set,
 * whitespace, newline, and comment tokens are filtered out — the parser
 * typically doesn't need them.  The stream is always terminated by TOK_EOF
 * on success.
 *
 * Memory: pre-allocates an estimated token count (src->len / 4, clamped
 * to [512, 16M]) only if @p out is uninitialized (capacity == 0).  Callers
 * that already initialized the stream keep their allocation.  Shrinks the
 * array afterward if it's less than half used.  The caller must call
 * token_stream_free() when done.
 *
 * Safety: the loop is bounded at src->len * 4 + 1024 iterations to prevent
 * infinite loops from lexer bugs — in practice, each iteration consumes
 * at least one byte.  On allocation failure, the function returns -1; the
 * stream may be partial and MUST NOT be handed to the parser.
 *
 * @param src      Source to tokenize.
 * @param out      Receives the token stream (caller frees with token_stream_free).
 * @param skip_ws  If non-zero, filter whitespace/newline/comment tokens.
 * @param diag     Diagnostic accumulator (may be NULL to ignore errors).
 * @return         0 on success, -1 on out-of-memory.
 */
int lexer_tokenize(const Source *src, TokenStream *out, int skip_ws,
                   LexerDiagnostics *diag) {
  /* Only initialize if caller didn't already.  This prevents the double-init
   * leak when msf_analyze pre-allocates the stream. */
  if (out->capacity == 0 && out->tokens == NULL) {
    size_t est = src->len / 4;
    if (est < 512)    est = 512;
    if (est > 1u<<24) est = 1u << 24;
    token_stream_init(out, est);
    if (!out->tokens) return -1;
  }

  Lexer l;
  lexer_init(&l, src);
  l.diag = diag;

  /* When skip_ws drops TOK_NEWLINE tokens we still preserve the *fact* that
   * a newline occurred by setting has_leading_newline on the next retained
   * token.  Parser code that needs line-break sensitivity (generic-call
   * disambiguation, directive condition end, operator decl line scoping)
   * reads this flag instead of looking for a newline token that will never
   * appear in the filtered stream. */
  int pending_newline = 0;
  for (size_t n = (size_t)src->len * 4 + 1024, i = 0; i < n; i++) {
    Token t = lexer_next(&l);
    int is_trivia = skip_ws && (t.type == TOK_WHITESPACE ||
                                t.type == TOK_NEWLINE ||
                                t.type == TOK_COMMENT);
    if (is_trivia) {
      if (t.type == TOK_NEWLINE) pending_newline = 1;
    } else {
      if (pending_newline) {
        t.has_leading_newline = 1;
        pending_newline = 0;
      }
      if (token_stream_push(out, t) != 0) return -1;
    }
    if (t.type == TOK_EOF) break;
  }

  if (out->count == 0 || out->tokens[out->count - 1].type != TOK_EOF) {
    Token eof = {TOK_EOF, l.pos, 0, l.line, l.col, KW_NONE, OP_NONE, 0};
    if (pending_newline) eof.has_leading_newline = 1;
    if (token_stream_push(out, eof) != 0) return -1;
  }

  if (out->count > 0 && out->count < out->capacity / 2) {
    Token *shrink = realloc(out->tokens, out->count * sizeof(Token));
    if (shrink) { out->tokens = shrink; out->capacity = out->count; }
  }
  return 0;
}
