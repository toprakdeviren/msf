/**
 * @file token.c
 * @brief Token utilities: type names, text extraction, stream management.
 *
 * These functions operate on tokens and the token stream — they don't
 * know about the lexer.  The lexer produces tokens; this file helps
 * you read and manage them.
 */
#include "internal/lexer.h"
#include "internal/limits.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════════
 * Token Type Names
 * ═══════════════════════════════════════════════════════════════════════════════ */

static const char *TOKEN_TYPE_NAMES[] = {
    [TOK_UNKNOWN]   = "unknown",     [TOK_IDENTIFIER]  = "identifier",
    [TOK_KEYWORD]   = "keyword",     [TOK_INTEGER_LIT] = "int_lit",
    [TOK_FLOAT_LIT] = "float_lit",   [TOK_STRING_LIT]  = "string_lit",
    [TOK_OPERATOR]  = "operator",    [TOK_PUNCT]       = "punct",
    [TOK_COMMENT]   = "comment",     [TOK_WHITESPACE]  = "whitespace",
    [TOK_NEWLINE]   = "newline",     [TOK_REGEX_LIT]   = "regex_lit",
};
#define TOKEN_TYPE_NAMES_LEN (sizeof(TOKEN_TYPE_NAMES) / sizeof(TOKEN_TYPE_NAMES[0]))

/**
 * @brief Returns a human-readable name for a token type.
 *
 * Maps TOK_IDENTIFIER → "identifier", TOK_KEYWORD → "keyword", etc.
 * Returns "eof" for TOK_EOF and "?" for unknown/out-of-range values.
 * The returned string is a static literal — never free it.
 *
 * @param t  Token type enum value.
 * @return   A NUL-terminated name string (static storage).
 */
const char *token_type_name(TokenType t) {
  if (t == TOK_EOF)
    return "eof";
  if (t < TOKEN_TYPE_NAMES_LEN && TOKEN_TYPE_NAMES[t])
    return TOKEN_TYPE_NAMES[t];
  return "?";
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Token Text Extraction
 * ═══════════════════════════════════════════════════════════════════════════════ */

static _Thread_local char _txt_buf[LEXER_TOKEN_TEXT_MAX];

/**
 * @brief Extracts a token's text as a NUL-terminated C string.
 *
 * Copies up to LEXER_TOKEN_TEXT_MAX-1 bytes from the source at the token's
 * position into a thread-local buffer.  Each call overwrites the previous
 * result — copy the string if you need to keep it.
 *
 * Not reentrant within the same thread (two calls in the same expression
 * will alias).  Safe across threads thanks to _Thread_local storage.
 *
 * @param src  Source that produced the token.
 * @param tok  Token whose text to extract.
 * @return     NUL-terminated string (thread-local static buffer, do not free).
 */
const char *token_text(const Source *src, const Token *tok) {
  size_t n = tok->len < (sizeof(_txt_buf) - 1) ? tok->len : (sizeof(_txt_buf) - 1);
  memcpy(_txt_buf, src->data + tok->pos, n);
  _txt_buf[n] = '\0';
  return _txt_buf;
}

/**
 * @brief Zero-copy view of a token's text — points into the source, no buffer,
 *        no allocation, fully reentrant (preferred over token_text()).
 */
MSFStringView msf_token_view(const Source *src, const Token *tok) {
  if (!src || !tok) return (MSFStringView){NULL, 0};
  return (MSFStringView){src->data + tok->pos, tok->len};
}

/** @brief First byte of the token's text within the source (NOT NUL-terminated;
 *  read exactly msf_token_length() bytes).  NULL if either argument is NULL. */
const char *msf_token_text(const Source *src, const Token *tok) {
  return (src && tok) ? src->data + tok->pos : NULL;
}

/** @brief Byte length of a token (0 if @p tok is NULL). */
size_t msf_token_length(const Token *tok) {
  return tok ? tok->len : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Token Stream Management
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Allocates storage for a token stream with initial capacity.
 *
 * On allocation failure, capacity is set to 0 and tokens to NULL —
 * subsequent token_stream_push() calls will attempt to grow from scratch.
 *
 * @param ts   Token stream to initialize.
 * @param cap  Initial capacity (number of tokens to pre-allocate).
 */
void token_stream_init(TokenStream *ts, size_t cap) {
  if (cap && cap <= SIZE_MAX / sizeof(Token)) {
    ts->tokens = malloc(cap * sizeof(Token));
  } else {
    ts->tokens = NULL;
  }
  ts->count = 0;
  ts->capacity = ts->tokens ? cap : 0;
}

/**
 * @brief Frees all memory owned by a token stream.
 *
 * After this call, the stream is zeroed out (count=0, capacity=0, tokens=NULL).
 * Safe to call on an already-freed or zero-initialized stream.
 *
 * @param ts  Token stream to free.
 */
void token_stream_free(TokenStream *ts) {
  free(ts->tokens);
  ts->tokens = NULL;
  ts->count = ts->capacity = 0;
}

/**
 * @brief Appends a token to the stream, growing the buffer if needed.
 *
 * Growth strategy: starts at 64 tokens, then grows by 50% each time
 * (amortized O(1) push).  Returns 0 on success, -1 on allocation
 * failure — the stream remains valid with its existing contents but
 * the caller MUST stop pushing further tokens; the parser relies on
 * an EOF terminator being reachable.
 *
 * @param ts   Token stream to append to.
 * @param tok  Token to append.
 * @return     0 on success, -1 on out-of-memory.
 */
int token_stream_push(TokenStream *ts, Token tok) {
  if (ts->count >= ts->capacity) {
    size_t new_cap = ts->capacity < 16 ? 64 : ts->capacity + (ts->capacity >> 1);
    /* Overflow guard for new_cap * sizeof(Token). */
    if (new_cap > SIZE_MAX / sizeof(Token))
      return -1;
    Token *new_ptr = realloc(ts->tokens, new_cap * sizeof(Token));
    if (!new_ptr)
      return -1;
    ts->capacity = new_cap;
    ts->tokens = new_ptr;
  }
  ts->tokens[ts->count++] = tok;
  return 0;
}
