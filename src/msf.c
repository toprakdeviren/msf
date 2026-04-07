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

#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════════
 * MSFResult — full definition (opaque in public header)
 * ═══════════════════════════════════════════════════════════════════════════════ */

struct MSFResult {
  Source       src;
  TokenStream  ts;
  ASTNode      *root;
  ASTArena     ast_arena;
  TypeArena    type_arena;
  Parser       *parser;
  SemaContext  *sema;
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

  r->src.data = code;
  r->src.len = strlen(code);
  r->src.filename = filename ? filename : "<input>";

  /* 1. Tokenize */
  token_stream_init(&r->ts, r->src.len / 4 + 64);
  lexer_tokenize(&r->src, &r->ts, 1, NULL);

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
 * Destruction order mirrors creation order in reverse: sema → parser →
 * type arena → AST arena → token stream → result struct.
 */
void msf_result_free(MSFResult *r) {
  if (!r) return;
  sema_destroy(r->sema);
  parser_destroy(r->parser);
  type_arena_free(&r->type_arena);
  ast_arena_free(&r->ast_arena);
  token_stream_free(&r->ts);
  free(r);
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
 * Parser and sema errors are combined into a single flat index space:
 * indices [0, parser_count) are parser errors, [parser_count, total) are sema.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns the total number of errors (parser + sema combined). */
uint32_t msf_error_count(const MSFResult *r) {
  if (!r) return 0;
  return parser_error_count(r->parser) + sema_error_count(r->sema);
}

/** @brief Returns the error message at index i. */
const char *msf_error_message(const MSFResult *r, uint32_t i) {
  if (!r) return "";
  uint32_t pc = parser_error_count(r->parser);
  return (i < pc) ? parser_error_message(r->parser, i)
                  : sema_error_message(r->sema, i - pc);
}

/** @brief Returns the 1-based line number for error at index i. */
uint32_t msf_error_line(const MSFResult *r, uint32_t i) {
  if (!r) return 0;
  uint32_t pc = parser_error_count(r->parser);
  return (i < pc) ? parser_error_line(r->parser, i)
                  : sema_error_line(r->sema, i - pc);
}

/** @brief Returns the 1-based column number for error at index i. */
uint32_t msf_error_col(const MSFResult *r, uint32_t i) {
  if (!r) return 0;
  uint32_t pc = parser_error_count(r->parser);
  return (i < pc) ? parser_error_col(r->parser, i)
                  : sema_error_col(r->sema, i - pc);
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
