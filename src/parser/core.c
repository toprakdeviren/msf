/**
 * @file core.c
 * @brief Parser core — token navigation, node allocation, error recording.
 *
 * This is the parser's foundation layer.  Every other parse_*.c file
 * depends on the helpers defined here:
 *
 *   adv()                — consume one token
 *   alloc_node()         — allocate an AST node from the arena
 *   parse_error_push()   — record a diagnostic
 *   collect_modifiers()  — consume modifier keywords into a bitmask
 *   skip_balanced()      — skip a balanced bracket pair
 *   parse_hash_directive() — handle #if/#else/#endif/#warning/#error
 *
 * Also contains the public lifecycle API: parser_init/destroy and
 * error accessors.
 */
#include "private.h"
#include "../internal/builtin_names.h"
#include "../internal/lexer.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Token Navigation
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Advances the parser by one token and returns the consumed token. */
const Token *adv(Parser *p) {
  const Token *t = &p->ts->tokens[p->pos];
  if (!p_is_eof(p))
    p->pos++;
  return t;
}

/** @brief Returns the first byte of the current token, or '\0' at EOF. */
char cur_char(Parser *p) {
  if (p_is_eof(p))
    return '\0';
  return p->src->data[p_tok(p)->pos];
}

/** @brief Returns 1 if the current token's text matches @p str exactly. */
int tok_text_eq(const Parser *p, const char *str, uint32_t len) {
  if (p_is_eof(p) || p_tok(p)->len != len)
    return 0;
  return memcmp(p->src->data + p_tok(p)->pos, str, len) == 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Node Allocation & Diagnostics
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Allocates an AST node of the given kind, stamped with the current
 *        token position.
 *
 * On OOM, records an error and returns NULL.
 *
 * @param p     Parser state.
 * @param kind  Node kind to assign.
 * @return      Fresh node, or NULL on OOM.
 */
ASTNode *alloc_node(Parser *p, ASTNodeKind kind) {
  ASTNode *n = ast_arena_alloc(p->arena);
  if (!n) {
    parse_error_push(p, "out of memory");
    return NULL;
  }
  n->kind = kind;
  n->tok_idx = (uint32_t)p->pos;
  return n;
}

/**
 * @brief Records a parse error with printf-style formatting.
 *
 * Captures the current token's line/col for the error location.
 * Silently drops the error if MAX_PARSE_ERRORS is reached.
 *
 * @param p    Parser state.
 * @param fmt  printf-style format string.
 */
void parse_error_push(Parser *p, const char *fmt, ...) {
  if (p->error_count >= MAX_PARSE_ERRORS)
    return;
  uint32_t i = p->error_count;
  p->error_line[i] = p_is_eof(p) ? 0 : p_tok(p)->line;
  p->error_col[i] = p_is_eof(p) ? 0 : p_tok(p)->col;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(p->errors[i], sizeof(p->errors[i]), fmt, ap);
  va_end(ap);
  p->error_count++;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Bracket & Generic Skipping
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Skips a balanced bracket pair, consuming both delimiters.
 *
 * Handles nested pairs: skip_balanced(p, '(', ')') on "((a))" consumes
 * all 5 tokens.
 *
 * @param p      Parser state (pos must be at the opening bracket).
 * @param open   Opening character ('(', '{', or '[').
 * @param close  Closing character (')', '}', or ']').
 */
void skip_balanced(Parser *p, char open, char close) {
  int depth = 1;
  adv(p);
  while (!p_is_eof(p) && depth > 0) {
    char c = p->src->data[p_tok(p)->pos];
    if (c == open)  depth++;
    if (c == close) depth--;
    adv(p);
  }
}

/**
 * @brief Skips a generic parameter list (<T: Proto, U>) by balancing
 *        angle brackets.
 */
void skip_generic_params(Parser *p) {
  if (p_is_eof(p) || cur_char(p) != '<')
    return;
  adv(p);
  int d = 1;
  while (!p_is_eof(p) && d > 0) {
    char c = cur_char(p);
    if (c == '<') d++;
    else if (c == '>') d--;
    adv(p);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Modifier Collection
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Tries to consume a setter-access restriction: private(set), etc.
 *
 * If the current token is followed by "(set)", consumes all 4 tokens and
 * sets p->setter_access.  Otherwise does nothing and returns 0.
 *
 * @param p             Parser state.
 * @param access_value  Value to assign to p->setter_access on match.
 * @return              1 if consumed, 0 otherwise.
 */
int try_consume_setter_access(Parser *p, uint8_t access_value) {
  if (p->pos + 3 >= p->ts->count)
    return 0;
  const Token *t1 = &p->ts->tokens[p->pos + 1];
  const Token *t2 = &p->ts->tokens[p->pos + 2];
  const Token *t3 = &p->ts->tokens[p->pos + 3];
  if (t1->type != TOK_PUNCT || p->src->data[t1->pos] != '(')
    return 0;
  if (t2->type != TOK_IDENTIFIER || !tok_eq(p, t2, CK_SET))
    return 0;
  if (t3->type != TOK_PUNCT || p->src->data[t3->pos] != ')')
    return 0;
  p->setter_access = access_value;
  adv(p); adv(p); adv(p); adv(p);
  return 1;
}

/**
 * @brief Collects modifier keywords into a MOD_* bitmask.
 *
 * Consumes consecutive modifier keywords (public, static, final, lazy, etc.)
 * and returns the accumulated flags.  Also handles:
 *   - Setter-access restrictions: private(set), fileprivate(set), etc.
 *   - The `class func` / `class var` modifier (treated as MOD_STATIC).
 *
 * Stops when a non-modifier keyword is encountered (bare `class` for a
 * class declaration, or any other keyword/token).
 *
 * @param p  Parser state.
 * @return   Accumulated modifier bitmask.
 */
/** @brief Tries setter-access, otherwise sets the modifier flag and advances. */
static void consume_access_modifier(Parser *p, uint32_t *mods,
                                    uint32_t mod_flag, uint8_t setter_val) {
  if (try_consume_setter_access(p, setter_val)) return;
  *mods |= mod_flag;
  adv(p);
}

uint32_t collect_modifiers(Parser *p) {
  uint32_t mods = 0;
  p->class_var_flag = 0;
  p->setter_access = 0;
  while (!p_is_eof(p) && p_tok(p)->type == TOK_KEYWORD) {
    switch (p_tok(p)->keyword) {
    case KW_PUBLIC:      mods |= MOD_PUBLIC;      adv(p); break;
    case KW_OPEN:        mods |= MOD_OPEN;        adv(p); break;
    case KW_STATIC:      mods |= MOD_STATIC;      adv(p); break;
    case KW_FINAL:       mods |= MOD_FINAL;       adv(p); break;
    case KW_OVERRIDE:    mods |= MOD_OVERRIDE;    adv(p); break;
    case KW_LAZY:        mods |= MOD_LAZY;        adv(p); break;
    case KW_MUTATING:    mods |= MOD_MUTATING;    adv(p); break;
    case KW_ASYNC:       mods |= MOD_ASYNC;       adv(p); break;

    /* Access modifiers that support setter-access: private(set), etc. */
    case KW_PRIVATE:     consume_access_modifier(p, &mods, MOD_PRIVATE,     1); break;
    case KW_FILEPRIVATE: consume_access_modifier(p, &mods, MOD_FILEPRIVATE, 2); break;
    case KW_INTERNAL:    consume_access_modifier(p, &mods, MOD_INTERNAL,    3); break;
    case KW_PACKAGE:     consume_access_modifier(p, &mods, MOD_PACKAGE,     4); break;

    /* `class func/var/subscript` → MOD_STATIC (overrideable) */
    case KW_CLASS:
      if (p->pos + 1 < p->ts->count) {
        Keyword next_kw = p->ts->tokens[p->pos + 1].keyword;
        if (next_kw == KW_SUBSCRIPT || next_kw == KW_FUNC || next_kw == KW_VAR) {
          mods |= MOD_STATIC;
          if (next_kw == KW_VAR) p->class_var_flag = 1;
          adv(p);
          break;
        }
      }
      goto done;
    default:
      goto done;
    }
  }
done:
  return mods;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Throws Clause
 * ═══════════════════════════════════════════════════════════════════════════════ */

ASTNode *parse_type(Parser *p);

/**
 * @brief Parses a `throws`, `rethrows`, or typed throws clause.
 *
 * Typed throws: `throws(MyError)` — the error type becomes a child node.
 * Plain `throws` / `rethrows` — no children.
 *
 * @param p  Parser state.
 * @return   AST_THROWS_CLAUSE node, or NULL if no throws keyword present.
 */
ASTNode *parse_throws_clause(Parser *p) {
  if (!p_is_kw(p, KW_THROWS) && !p_is_kw(p, KW_RETHROWS))
    return NULL;
  uint32_t throws_tok = (uint32_t)p->pos;
  int is_rethrows = p_is_kw(p, KW_RETHROWS);
  adv(p);

  ASTNode *node = alloc_node(p, AST_THROWS_CLAUSE);
  if (!node)
    return NULL;
  node->tok_idx = throws_tok;
  if (is_rethrows)
    node->modifiers |= MOD_RETHROWS;

  /* Typed throws: throws(ErrorType) */
  if (!p_is_eof(p) && P_LPAREN(p)) {
    adv(p);
    ASTNode *err_ty = parse_type(p);
    if (err_ty)
      ast_add_child(node, err_ty);
    if (!p_is_eof(p) && P_RPAREN(p))
      adv(p);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/**
 * @brief Returns 1 if a throws clause actually throws (not `throws(Never)`).
 *
 * Used by sema to determine if a function's error type is non-trivial.
 *
 * @param src     Source (for token text comparison).
 * @param ts      Token stream.
 * @param clause  AST_THROWS_CLAUSE node (may be NULL → returns 0).
 * @return        1 if throwing, 0 if non-throwing or NULL.
 */
int throws_clause_is_throwing(const Source *src, const TokenStream *ts,
                              const ASTNode *clause) {
  if (!clause)
    return 0;
  if (clause->modifiers & MOD_RETHROWS)
    return 1;
  const ASTNode *err_ty = clause->first_child;
  if (!err_ty)
    return 1;
  if (err_ty->kind == AST_TYPE_IDENT && ts && src) {
    const Token *t = &ts->tokens[err_ty->tok_idx];
    if (t->len == sizeof(SW_TYPE_NEVER) - 1 &&
        memcmp(src->data + t->pos, SW_TYPE_NEVER, sizeof(SW_TYPE_NEVER) - 1) == 0)
      return 0;
  }
  return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * AST Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Adds a statement chain to a parent, handling multi-var siblings
 *        and registering operator/precedence group declarations.
 *
 * parse_var_decl() returns sibling-chained nodes (var x = 0, y = 0).
 * This helper walks the chain, registers side effects, and adds each
 * node as a separate child.  Used by parse_block() and parse_source_file().
 */
void add_stmt_chain(Parser *p, ASTNode *parent, ASTNode *s) {
  while (s) {
    ASTNode *nxt = s->next_sibling;
    if (s->kind == AST_PRECEDENCE_GROUP_DECL)
      register_precedence_group_from_ast(p, s);
    if (s->kind == AST_OPERATOR_DECL)
      register_operator_from_ast(p, s);
    ast_add_child(parent, s);
    s = nxt;
  }
}

/**
 * @brief Converts a CLOSURE_EXPR's children into a BLOCK (brace_stmt) node.
 *
 * Used when the Pratt expression parser consumes `{ body }` as a trailing
 * closure but the caller needs a plain block statement instead.
 */
ASTNode *closure_to_block(Parser *p, ASTNode *closure) {
  ASTNode *block = alloc_node(p, AST_BLOCK);
  if (!block)
    return NULL;
  for (ASTNode *s = closure->first_child; s;) {
    ASTNode *nx = s->next_sibling;
    s->next_sibling = NULL;
    ast_add_child(block, s);
    s = nx;
  }
  return block;
}

/**
 * @brief Adds a child to parent, unwrapping trivial CALL_EXPR wrappers.
 *
 * If @p e is a CALL_EXPR with a single child (callee only, no args), the
 * callee is added directly instead.  This produces cleaner AST after
 * trailing-closure stripping.
 */
void add_child_unwrapped(ASTNode *parent, ASTNode *e) {
  if (e->kind == AST_CALL_EXPR && e->first_child &&
      !e->first_child->next_sibling) {
    ast_add_child(parent, e->first_child);
  } else {
    ast_add_child(parent, e);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Hash Directives: #if #else #elseif #endif #warning #error
 *
 * Both branches of #if/#else are parsed normally — no compile-time
 * evaluation.  Directives don't produce AST nodes.
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief True if the current token is a declaration keyword (stops #if condition scan). */
static int p_is_decl_start(const Parser *p) {
  if (p_tok(p)->type != TOK_KEYWORD) return 0;
  switch (p_tok(p)->keyword) {
  case KW_FUNC: case KW_VAR: case KW_LET: case KW_CLASS:
  case KW_STRUCT: case KW_ENUM: case KW_PROTOCOL: case KW_IMPORT:
  case KW_ACTOR: case KW_TYPEALIAS: case KW_EXTENSION:
    return 1;
  default:
    return 0;
  }
}

/** @brief Skips a #if / #elseif condition (tokens until body boundary). */
static void skip_directive_condition(Parser *p) {
  adv(p);
  while (!p_is_eof(p)) {
    if (p_tok(p)->type == TOK_NEWLINE)           { adv(p); return; }
    if (P_LPAREN(p))                      { skip_balanced(p, '(', ')'); continue; }
    if (P_LBRACE(p) || P_RBRACE(p)) return;
    if (p_is_decl_start(p))                      return;
    if (p_is_op_char(p, '#'))                    return;
    adv(p);
  }
}

/** @brief Skips to line boundary (best effort for unknown directives). */
static void skip_to_line_end(Parser *p) {
  while (!p_is_eof(p) && p_tok(p)->type != TOK_NEWLINE &&
         !P_LBRACE(p) && !P_RBRACE(p))
    adv(p);
  if (!p_is_eof(p) && p_tok(p)->type == TOK_NEWLINE)
    adv(p);
}

/**
 * @brief Parses a compiler directive (#if, #else, #endif, #warning, #error).
 *
 * Directives don't produce AST nodes — they're consumed and skipped.
 */
ASTNode *parse_hash_directive(Parser *p) {
  adv(p);
  if (p_is_eof(p)) return NULL;

  /* #if / #elseif */
  if (p_is_kw(p, KW_IF) || tok_text_eq(p, CK_ELSEIF, 6))
    { skip_directive_condition(p); return NULL; }

  /* #else / #endif */
  if (p_is_kw(p, KW_ELSE) || tok_text_eq(p, CK_ENDIF, 5))
    { adv(p); return NULL; }

  /* #warning("...") / #error("...") / #sourceLocation(...) / #line */
  if (tok_text_eq(p, CK_WARNING, 7) || tok_text_eq(p, CK_ERROR, 5) ||
      tok_text_eq(p, CK_SOURCE_LOCATION, 14) || tok_text_eq(p, CK_LINE, 4)) {
    adv(p);
    if (P_LPAREN(p)) skip_balanced(p, '(', ')');
    return NULL;
  }

  /* Unknown directive */
  skip_to_line_end(p);
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Public API — Lifecycle & Diagnostics
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Resets parser state (internal — called by parser_init). */
void parser_ctx_reset(Parser *p, const Source *src, const TokenStream *ts,
                      ASTArena *arena) {
  memset(p, 0, sizeof(*p));
  p->src = src;
  p->ts = ts;
  p->pos = 0;
  p->arena = arena;
}

/**
 * @brief Initializes a heap-allocated parser.
 *
 * The caller must free with parser_destroy().
 *
 * @param src    Source text.
 * @param ts     Whitespace-filtered token stream.
 * @param arena  AST arena for node allocations.
 * @return       Parser instance, or NULL on OOM.
 */
Parser *parser_init(const Source *src, const TokenStream *ts,
                      ASTArena *arena) {
  Parser *p = calloc(1, sizeof(Parser));
  if (!p)
    return NULL;
  parser_ctx_reset(p, src, ts, arena);
  return p;
}

/**
 * @brief Destroys a parser and frees associated resources.
 *
 * Frees heap-allocated strings from custom operator and precedence group
 * registrations.
 */
void parser_destroy(Parser *p) {
  if (!p) return;
  for (int i = 0; i < p->pg_count; i++)
    free(p->pg_table[i].name);
  for (int i = 0; i < p->custom_op_count; i++) {
    free(p->custom_ops[i].op);
    free(p->custom_ops[i].group_name);
  }
  free(p);
}

/** @brief Returns the number of recorded parse errors. */
uint32_t parser_error_count(const Parser *p) {
  return p ? p->error_count : 0;
}

/** @brief Returns the error message at @p index, or "" if out of range. */
const char *parser_error_message(const Parser *p, uint32_t index) {
  if (!p || index >= p->error_count)
    return "";
  return p->errors[index];
}

/** @brief Returns the line number of the error at @p index, or 0 if out of range. */
uint32_t parser_error_line(const Parser *p, uint32_t index) {
  if (!p || index >= p->error_count)
    return 0;
  return p->error_line[index];
}

/** @brief Returns the column number of the error at @p index, or 0 if out of range. */
uint32_t parser_error_col(const Parser *p, uint32_t index) {
  if (!p || index >= p->error_count)
    return 0;
  return p->error_col[index];
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Expression Parsing from C Strings
 *
 * Used by string interpolation lowering when \(...) segments contain
 * complex expressions that need to be parsed independently.
 * ═══════════════════════════════════════════════════════════════════════════════ */

#define INTERP_TOKEN_CAP 64

/**
 * @brief Parses a single expression from a C string.
 *
 * Convenience wrapper — calls parse_expression_from_cstring_with_tokens()
 * without retaining the intermediate token stream.
 */
ASTNode *parse_expression_from_cstring(ASTArena *arena, const char *str) {
  return parse_expression_from_cstring_with_tokens(arena, str, NULL);
}

/**
 * @brief Parses an expression from a C string, optionally returning the
 *        token stream.
 *
 * Tokenizes the string internally, parses one expression, and optionally
 * hands back the token stream for the caller to manage.
 *
 * @param arena   Arena for AST allocations.
 * @param str     NUL-terminated expression text.
 * @param out_ts  If non-NULL, receives a heap-allocated TokenStream
 *                (caller frees with token_stream_free() then free()).
 * @return        Parsed expression node, or NULL on failure.
 */
ASTNode *parse_expression_from_cstring_with_tokens(ASTArena *arena, const char *str,
                                                   TokenStream **out_ts) {
  if (!arena || !str)
    return NULL;
  size_t len = strlen(str);
  if (len == 0)
    return NULL;

  Source sub = {
      .data = str,
      .len = len,
      .filename = "<interpolation>",
  };
  TokenStream ts_stack;
  TokenStream *ts = out_ts ? malloc(sizeof(TokenStream)) : &ts_stack;
  if (out_ts && !ts) return NULL;
  token_stream_init(ts, INTERP_TOKEN_CAP);
  if (lexer_tokenize(&sub, ts, 1, NULL) != 0) {
    token_stream_free(ts);
    if (out_ts) free(ts);
    return NULL;
  }
  Parser p;
  parser_ctx_reset(&p, &sub, ts, arena);
  ASTNode *expr = parse_expr(&p);
  if (out_ts) {
    *out_ts = ts;
  } else {
    token_stream_free(ts);
  }
  return expr;
}
