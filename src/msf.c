/**
 * @file msf.c
 * @brief Library entry point — ties the pipeline together.
 *
 * Provides the public API declared in msf.h:
 *
 *   msf_version()       — library version
 *   msf_analyze()       — tokenize + parse + sema in one call
 *   msf_result_free()   — free all resources
 *   msf_root/source/tokens/token_count() — result accessors
 *   msf_error_count/message/line/col()   — error accessors
 *   msf_dump_text/json/sexpr()           — AST serialization
 */

#include "internal/msf.h"
#include "internal/lexer.h"

#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════════
 * MSFResult — full definition (opaque in public header)
 * ═══════════════════════════════════════════════════════════════════════════════ */

struct MSFResult {
  Source           src;
  /* Heap-allocated copy of the input source text.  Owned by the result so
   * the caller can free their buffer immediately after msf_analyze returns
   * without invalidating tokens, AST source ranges, or diagnostics. */
  char            *src_buf;
  /* Heap-allocated copy of the filename (or NULL if caller passed NULL). */
  char            *src_filename;
  TokenStream      ts;
  LexerDiagnostics lex_diag;
  ASTNode         *root;
  ASTArena         ast_arena;
  TypeArena        type_arena;
  Parser          *parser;
  SemaContext     *sema;

  /* Token streams owned by sub-expression re-parses (msf_parse_expression).
   * Each TokenStream is heap-allocated; released alongside the result. */
  TokenStream **sub_ts;
  size_t        sub_ts_cnt;
  size_t        sub_ts_cap;
};

/* ═══════════════════════════════════════════════════════════════════════════════
 * Version
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns the library version string. */
const char *msf_version(void) {
  return MSF_VERSION_STRING;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Analyze — full pipeline in one call
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Tokenizes, parses, and resolves types in one call.
 *
 * Returns NULL only on allocation failure.  Syntax and semantic errors
 * are recorded and accessible via msf_error_*().  The AST is always
 * produced (best-effort recovery).
 */
MSFResult *msf_analyze(const char *code, const char *filename) {
  if (!code) return NULL;

  MSFResult *r = calloc(1, sizeof(MSFResult));
  if (!r) return NULL;

  /* Copy the source so the result owns its backing memory.  Tokens, AST
   * nodes, and diagnostics all reference into this buffer; the caller may
   * free their input immediately after we return. */
  size_t code_len = strlen(code);
  r->src_buf = malloc(code_len + 1);
  if (!r->src_buf) { msf_result_free(r); return NULL; }
  memcpy(r->src_buf, code, code_len + 1);

  const char *fname = filename ? filename : "<input>";
  size_t fname_len = strlen(fname);
  r->src_filename = malloc(fname_len + 1);
  if (!r->src_filename) { msf_result_free(r); return NULL; }
  memcpy(r->src_filename, fname, fname_len + 1);

  r->src.data = r->src_buf;
  r->src.len = code_len;
  r->src.filename = r->src_filename;

  /* 1. Tokenize.  Pre-allocate so lexer_tokenize reuses our buffer; on OOM
   * the stream may be missing TOK_EOF and feeding it to the parser would
   * cause out-of-bounds reads. */
  token_stream_init(&r->ts, r->src.len / 4 + 64);
  if (!r->ts.tokens) { msf_result_free(r); return NULL; }
  lexer_diag_init(&r->lex_diag);
  if (lexer_tokenize(&r->src, &r->ts, 1, &r->lex_diag) != 0) {
    msf_result_free(r);
    return NULL;
  }

  /* 2. Parse */
  ast_arena_init(&r->ast_arena, 0);
  r->parser = parser_init(&r->src, &r->ts, &r->ast_arena);
  r->root = parse_source_file(r->parser);

  /* 3. Sema */
  type_arena_init(&r->type_arena, 0);
  type_builtins_init(&r->type_arena);
  r->sema = sema_init(&r->src, r->ts.tokens, &r->ast_arena, &r->type_arena);
  sema_analyze(r->sema, r->root);

  return r;
}

/**
 * @brief Frees all resources held by an analysis result.
 *
 * Destruction order mirrors generation order in reverse: sema → parser →
 * type arena → AST arena → token stream → result struct.
 */
void msf_result_free(MSFResult *r) {
  if (!r) return;
  sema_destroy(r->sema);
  parser_destroy(r->parser);
  type_arena_free(&r->type_arena);
  ast_arena_free(&r->ast_arena);
  /* Release sub-expression token streams owned by msf_parse_expression. */
  for (size_t i = 0; i < r->sub_ts_cnt; i++) {
    token_stream_free(r->sub_ts[i]);
    free(r->sub_ts[i]);
  }
  free(r->sub_ts);
  token_stream_free(&r->ts);
  free(r->src_buf);
  free(r->src_filename);
  free(r);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Bare Expression Parsing — §16 backend ABI
 * ═══════════════════════════════════════════════════════════════════════════════ */

const ASTNode *msf_parse_expression(MSFResult *r, const char *expr_text,
                                    const Token **out_tokens) {
  if (!r || !expr_text) return NULL;

  TokenStream *ts = NULL;
  ASTNode *node = parse_expression_from_cstring_with_tokens(
      &r->ast_arena, expr_text, &ts);
  if (!node) {
    if (ts) {
      token_stream_free(ts);
      free(ts);
    }
    return NULL;
  }

  /* Keep the token stream alive for as long as the parent result does. */
  if (r->sub_ts_cnt == r->sub_ts_cap) {
    size_t nc = r->sub_ts_cap ? r->sub_ts_cap * 2 : 4;
    TokenStream **nb = realloc(r->sub_ts, nc * sizeof(*nb));
    if (!nb) {
      /* Out of memory: drop the stream, node survives but token_text() on
       * those tokens will not work — caller should treat NULL out_tokens
       * as "tokens unavailable". */
      token_stream_free(ts);
      free(ts);
      if (out_tokens) *out_tokens = NULL;
      return node;
    }
    r->sub_ts = nb;
    r->sub_ts_cap = nc;
  }
  r->sub_ts[r->sub_ts_cnt++] = ts;

  if (out_tokens) *out_tokens = ts->tokens;
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Result Accessors
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns the root AST node (AST_SOURCE_FILE). */
const ASTNode *msf_root(const MSFResult *r) {
  return r ? r->root : NULL;
}

/** @brief Returns the source descriptor.  Owned by r. */
const Source *msf_source(const MSFResult *r) {
  return r ? &r->src : NULL;
}

/** @brief Returns the token array.  Owned by r. */
const Token *msf_tokens(const MSFResult *r) {
  return r ? r->ts.tokens : NULL;
}

/** @brief Returns the number of tokens (always >= 1, includes TOK_EOF). */
size_t msf_token_count(const MSFResult *r) {
  return r ? r->ts.count : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Error Accessors
 *
 * Lexer, parser, and sema diagnostics share a single flat index space in
 * pipeline order:
 *   [0, lex)               lexer diagnostics
 *   [lex, lex+par)         parser diagnostics
 *   [lex+par, total)       sema diagnostics
 * ═══════════════════════════════════════════════════════════════════════════════ */

static inline uint32_t msf_lex_count(const MSFResult *r) {
  return (uint32_t)r->lex_diag.count;
}

/** @brief Returns the total number of errors (lexer + parser + sema). */
uint32_t msf_error_count(const MSFResult *r) {
  if (!r) return 0;
  return msf_lex_count(r) + parser_error_count(r->parser)
       + sema_error_count(r->sema);
}

/** @brief Returns the error message at index i. */
const char *msf_error_message(const MSFResult *r, uint32_t i) {
  if (!r) return "";
  uint32_t lc = msf_lex_count(r);
  if (i < lc) return r->lex_diag.message[i];
  i -= lc;
  uint32_t pc = parser_error_count(r->parser);
  return (i < pc) ? parser_error_message(r->parser, i)
                  : sema_error_message(r->sema, i - pc);
}

/** @brief Returns the 1-based line number for error at index i. */
uint32_t msf_error_line(const MSFResult *r, uint32_t i) {
  if (!r) return 0;
  uint32_t lc = msf_lex_count(r);
  if (i < lc) return r->lex_diag.line[i];
  i -= lc;
  uint32_t pc = parser_error_count(r->parser);
  return (i < pc) ? parser_error_line(r->parser, i)
                  : sema_error_line(r->sema, i - pc);
}

/** @brief Returns the 1-based column number for error at index i. */
uint32_t msf_error_col(const MSFResult *r, uint32_t i) {
  if (!r) return 0;
  uint32_t lc = msf_lex_count(r);
  if (i < lc) return r->lex_diag.col[i];
  i -= lc;
  uint32_t pc = parser_error_count(r->parser);
  return (i < pc) ? parser_error_col(r->parser, i)
                  : sema_error_col(r->sema, i - pc);
}

/** @brief Returns the source byte offset where the error starts. */
uint32_t msf_error_start_offset(const MSFResult *r, uint32_t i) {
  if (!r) return 0;
  uint32_t lc = msf_lex_count(r);
  /* Lexer diagnostics don't track byte ranges yet. */
  if (i < lc) return 0;
  i -= lc;
  uint32_t pc = parser_error_count(r->parser);
  /* Parser doesn't track byte ranges yet — fall back to 0 for parser errors */
  return (i < pc) ? 0 : sema_error_start(r->sema, i - pc);
}

/** @brief Returns the source byte offset where the error ends. */
uint32_t msf_error_end_offset(const MSFResult *r, uint32_t i) {
  if (!r) return 0;
  uint32_t lc = msf_lex_count(r);
  if (i < lc) return 0;
  i -= lc;
  uint32_t pc = parser_error_count(r->parser);
  return (i < pc) ? 0 : sema_error_end(r->sema, i - pc);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * AST Dump
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Dumps the AST as indented plain text. */
void msf_dump_text(const MSFResult *r, FILE *out) {
  if (!r || !r->root) return;
  ast_print(r->root, &r->src, r->ts.tokens, 0, out ? out : stdout);
}

/** @brief Dumps the AST as JSON. */
void msf_dump_json(const MSFResult *r, FILE *out) {
  if (!r || !r->root) return;
  ast_dump_json(r->root, &r->src, r->ts.tokens, out ? out : stdout);
}

/** @brief Dumps the AST as an S-expression. */
void msf_dump_sexpr(const MSFResult *r, FILE *out) {
  if (!r || !r->root) return;
  ast_dump_sexpr(r->root, &r->src, r->ts.tokens, out ? out : stdout);
}
