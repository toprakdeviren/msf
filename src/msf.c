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
static MSFResult *analyze_impl(const char *code, const char *filename,
                               const char *const *module_types, size_t module_type_count,
                               const MSFVocab *vocab) {
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

  /* Seed module-level type names (sibling files within the same module, or
   * imported-module symbols) so references resolve instead of reporting
   * "use of undeclared type".  Empty for the single-file msf_analyze(). */
  for (size_t i = 0; i < module_type_count; i++)
    sema_predeclare_module_type(r->sema, module_types[i]);

  /* A runtime vocabulary, if given, resolves the file's `import X` statements
   * from a shipped .msfvocab artifact (no SDK needed). */
  if (vocab)
    sema_set_vocabulary(r->sema, vocab);

  sema_analyze(r->sema, r->root);

  return r;
}

MSFResult *msf_analyze(const char *code, const char *filename) {
  return analyze_impl(code, filename, NULL, 0, NULL);
}

MSFResult *msf_analyze_in_module(const char *code, const char *filename,
                                 const char *const *module_types, size_t module_type_count) {
  return analyze_impl(code, filename, module_types, module_type_count, NULL);
}

MSFResult *msf_analyze_with_vocab(const char *code, const char *filename,
                                  const MSFVocab *vocab) {
  return analyze_impl(code, filename, NULL, 0, vocab);
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
 * MSFModule — whole-module analysis over several files
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Each file is lexed + parsed into its own token stream / AST (parsed into one
 * shared AST arena), then sema_analyze_module() runs the passes across all files
 * with a single shared SemaContext — so cross-file references resolve.
 */

typedef struct {
  char            *src_buf;
  char            *src_filename;
  Source           src;
  TokenStream      ts;
  LexerDiagnostics lex_diag;
  Parser          *parser;
  ASTNode         *root;
} ModuleFile;

struct MSFModule {
  ModuleFile     *files;
  size_t          nfiles, cap;
  ASTArena        ast_arena;   /* shared by every file's parser  */
  TypeArena       type_arena;  /* shared                          */
  SemaContext    *sema;        /* allocated in msf_module_analyze */
  const MSFVocab *vocab;       /* optional; borrowed, not owned   */
  const MSFVocab *sdk_vocab;   /* optional global-fallback SDK vocab; borrowed */
  int             analyzed;
  /* Token streams owned by msf_module_parse_expression re-parses, mirroring the
   * single-file MSFResult.sub_ts; released in msf_module_free. */
  TokenStream   **sub_ts;
  size_t          sub_ts_cnt;
  size_t          sub_ts_cap;
};

MSFModule *msf_module_new(void) {
  MSFModule *m = calloc(1, sizeof(MSFModule));
  if (!m) return NULL;
  ast_arena_init(&m->ast_arena, 0);
  type_arena_init(&m->type_arena, 0);
  type_builtins_init(&m->type_arena);
  return m;
}

int msf_module_set_vocabulary(MSFModule *m, const MSFVocab *vocab) {
  if (!m || m->analyzed) return -1;
  m->vocab = vocab;
  return 0;
}

int msf_module_set_sdk_vocabulary(MSFModule *m, const MSFVocab *vocab) {
  if (!m || m->analyzed) return -1;
  m->sdk_vocab = vocab;
  return 0;
}

int msf_module_add_file(MSFModule *m, const char *code, const char *filename) {
  if (!m || !code || m->analyzed) return -1;

  if (m->nfiles == m->cap) {
    size_t nc = m->cap ? m->cap * 2 : 8;
    ModuleFile *nf = realloc(m->files, nc * sizeof(ModuleFile));
    if (!nf) return -1;
    m->files = nf;
    m->cap = nc;
  }

  ModuleFile *f = &m->files[m->nfiles];
  memset(f, 0, sizeof(*f));

  size_t code_len = strlen(code);
  f->src_buf = malloc(code_len + 1);
  if (!f->src_buf) return -1;
  memcpy(f->src_buf, code, code_len + 1);

  const char *fn = filename ? filename : "<input>";
  size_t fnl = strlen(fn);
  f->src_filename = malloc(fnl + 1);
  if (!f->src_filename) { free(f->src_buf); return -1; }
  memcpy(f->src_filename, fn, fnl + 1);

  f->src.data = f->src_buf;
  f->src.len = code_len;
  f->src.filename = f->src_filename;

  token_stream_init(&f->ts, f->src.len / 4 + 64);
  if (!f->ts.tokens) { free(f->src_buf); free(f->src_filename); return -1; }
  lexer_diag_init(&f->lex_diag);
  if (lexer_tokenize(&f->src, &f->ts, 1, &f->lex_diag) != 0) {
    for (size_t di = 0; di < f->lex_diag.count; di++) {
      fprintf(stderr, "error: %s:%u:%u: %s\n",
              fn, f->lex_diag.line[di], f->lex_diag.col[di], f->lex_diag.message[di]);
    }
    token_stream_free(&f->ts);
    free(f->src_buf);
    free(f->src_filename);
    return -1;
  }

  f->parser = parser_init(&f->src, &f->ts, &m->ast_arena);
  f->root = parse_source_file(f->parser);
  m->nfiles++;
  return 0;
}

int msf_module_analyze(MSFModule *m) {
  if (!m || m->analyzed || m->nfiles == 0) return -1;

  /* One shared sema context over the shared arenas.  Seed it with file 0's
   * source/tokens; sema_analyze_module() swaps per file as it sweeps. */
  m->sema = sema_init(&m->files[0].src, m->files[0].ts.tokens, &m->ast_arena, &m->type_arena);
  if (!m->sema) return -1;
  if (m->vocab)
    sema_set_vocabulary(m->sema, m->vocab);
  if (m->sdk_vocab)
    sema_set_sdk_vocabulary(m->sema, m->sdk_vocab);

  SemaModuleFile *sf = malloc(m->nfiles * sizeof(SemaModuleFile));
  if (!sf) return -1;
  for (size_t i = 0; i < m->nfiles; i++) {
    sf[i].src = &m->files[i].src;
    sf[i].tokens = m->files[i].ts.tokens;
    sf[i].token_count = (uint32_t)m->files[i].ts.count;
    sf[i].root = m->files[i].root;
  }
  int rc = sema_analyze_module(m->sema, sf, m->nfiles);
  free(sf);
  m->analyzed = 1;
  return rc;
}

uint32_t msf_module_error_count(const MSFModule *m) {
  return (m && m->sema) ? sema_error_count(m->sema) : 0;
}

const char *msf_module_error_message(const MSFModule *m, uint32_t index) {
  return (m && m->sema) ? sema_error_message(m->sema, index) : "";
}

const char *msf_module_error_file(const MSFModule *m, uint32_t index) {
  return (m && m->sema) ? sema_error_file(m->sema, index) : NULL;
}

uint32_t msf_module_error_line(const MSFModule *m, uint32_t index) {
  return (m && m->sema) ? sema_error_line(m->sema, index) : 0;
}

uint32_t msf_module_error_col(const MSFModule *m, uint32_t index) {
  return (m && m->sema) ? sema_error_col(m->sema, index) : 0;
}

/* Source byte length of the offending range (end - start), for sizing an
 * editor underline; 0 if unknown. */
uint32_t msf_module_error_length(const MSFModule *m, uint32_t index) {
  if (!m || !m->sema) return 0;
  uint32_t s = sema_error_start(m->sema, index), e = sema_error_end(m->sema, index);
  return e > s ? e - s : 0;
}

size_t msf_module_file_count(const MSFModule *m) { return m ? m->nfiles : 0; }

const char *msf_module_file_name(const MSFModule *m, size_t i) {
  return (m && i < m->nfiles) ? m->files[i].src_filename : NULL;
}

const ASTNode *msf_module_file_root(const MSFModule *m, size_t i) {
  return (m && i < m->nfiles) ? m->files[i].root : NULL;
}

const Source *msf_module_file_source(const MSFModule *m, size_t i) {
  return (m && i < m->nfiles) ? &m->files[i].src : NULL;
}

const Token *msf_module_file_tokens(const MSFModule *m, size_t i) {
  return (m && i < m->nfiles) ? m->files[i].ts.tokens : NULL;
}

size_t msf_module_file_token_count(const MSFModule *m, size_t i) {
  return (m && i < m->nfiles) ? m->files[i].ts.count : 0;
}

size_t msf_module_entry_file_index(const MSFModule *m) {
  if (!m) return (size_t)-1;
  for (size_t f = 0; f < m->nfiles; f++) {
    const ASTNode *root = m->files[f].root;
    if (!root) continue;
    for (const ASTNode *c = root->first_child; c; c = c->next_sibling) {
      switch (c->kind) {
      case AST_LET_DECL:   case AST_VAR_DECL:   case AST_EXPR_STMT:
      case AST_RETURN_STMT:case AST_IF_STMT:    case AST_GUARD_STMT:
      case AST_WHILE_STMT: case AST_REPEAT_STMT:case AST_FOR_STMT:
      case AST_DEFER_STMT: case AST_THROW_STMT: case AST_DO_STMT:
      case AST_SWITCH_STMT:case AST_BREAK_STMT:
        return f;
      default:
        break;
      }
    }
  }
  return (size_t)-1;
}

const ASTNode *msf_module_parse_expression(MSFModule *m, const char *expr_text,
                                           const Token **out_tokens) {
  if (!m || !expr_text) return NULL;

  TokenStream *ts = NULL;
  ASTNode *node = parse_expression_from_cstring_with_tokens(
      &m->ast_arena, expr_text, &ts);
  if (!node) {
    if (ts) { token_stream_free(ts); free(ts); }
    return NULL;
  }

  /* Keep the token stream alive for as long as the module does. */
  if (m->sub_ts_cnt == m->sub_ts_cap) {
    size_t nc = m->sub_ts_cap ? m->sub_ts_cap * 2 : 4;
    TokenStream **nb = realloc(m->sub_ts, nc * sizeof(*nb));
    if (!nb) {
      token_stream_free(ts);
      free(ts);
      if (out_tokens) *out_tokens = NULL;
      return node;
    }
    m->sub_ts = nb;
    m->sub_ts_cap = nc;
  }
  m->sub_ts[m->sub_ts_cnt++] = ts;

  if (out_tokens) *out_tokens = ts->tokens;
  return node;
}

void msf_module_free(MSFModule *m) {
  if (!m) return;
  if (m->sema) sema_destroy(m->sema);
  for (size_t i = 0; i < m->nfiles; i++) {
    parser_destroy(m->files[i].parser);
    token_stream_free(&m->files[i].ts);
    free(m->files[i].src_buf);
    free(m->files[i].src_filename);
  }
  /* Release sub-expression token streams owned by msf_module_parse_expression. */
  for (size_t i = 0; i < m->sub_ts_cnt; i++) {
    token_stream_free(m->sub_ts[i]);
    free(m->sub_ts[i]);
  }
  free(m->sub_ts);
  free(m->files);
  type_arena_free(&m->type_arena);
  ast_arena_free(&m->ast_arena);
  free(m);
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
