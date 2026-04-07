/**
 * @file private.h
 * @brief Parser module internals — struct, helpers, all parse function protos.
 *
 * NOT part of the public API.  Included by all parser .c files.
 * The public parser interface (parser_init/destroy/parse_source_file)
 * is declared in internal/msf.h.
 *
 * WHAT THIS HEADER PROVIDES
 *
 *   Parser struct     — full definition (opaque in public header)
 *   Token helpers     — p_tok, p_is_kw, p_is_punct, p_is_op, adv, ...
 *   Match macros      — tok_eq, p_is_ident_str, p_is_ck
 *   Contextual kws    — CK_GET, CK_SET, CK_WILL_SET, CK_INDIRECT, ...
 *   Pratt precedence  — Prec struct, get_infix_prec
 *   Parse functions   — grouped by: core, decl, stmt, expr, type
 */
#pragma once

#include "../internal/ast.h"
#include "../internal/limits.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  1. CONTEXTUAL KEYWORDS                                                │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Not reserved by the lexer — the parser matches them by text in context.
 *  Use p_is_ck(p, CK_GET) to check.
 */

/* Property accessors */
#define CK_GET        "get"
#define CK_SET        "set"
#define CK_WILL_SET   "willSet"
#define CK_DID_SET    "didSet"
#define CK_READ       "_read"
#define CK_MODIFY     "_modify"

/* Precedence-group attributes */
#define CK_HIGHER_THAN    "higherThan"
#define CK_LOWER_THAN     "lowerThan"
#define CK_ASSOCIATIVITY  "associativity"
#define CK_ASSIGNMENT     "assignment"

/* Associativity values */
#define CK_LEFT   "left"
#define CK_RIGHT  "right"
#define CK_NONE   "none"

/* Enum / operator fixity */
#define CK_INDIRECT  "indirect"
#define CK_PREFIX    "prefix"
#define CK_POSTFIX   "postfix"
#define CK_INFIX     "infix"

/* Capture qualifiers */
#define CK_WEAK      "weak"
#define CK_UNOWNED   "unowned"
#define CK_SAFE      "safe"

/* Declaration modifiers (contextual — not in keyword table) */
#define CK_NONISOLATED   "nonisolated"
#define CK_CONVENIENCE   "convenience"
#define CK_REQUIRED      "required"
#define CK_MAIN_ACTOR    "MainActor"
#define CK_PRECEDENCEGROUP_ID "precedencegroup"

/* Type attributes */
#define CK_ESCAPING      "escaping"
#define CK_AUTOCLOSURE   "autoclosure"
#define CK_INOUT         "inout"
#define CK_ASSOC_TYPE    "associatedtype"

/* Import attribute */
#define CK_TESTABLE  "testable"

/* Hash directive names */
#define CK_ELSEIF         "elseif"
#define CK_ENDIF           "endif"
#define CK_WARNING        "warning"
#define CK_ERROR          "error"
#define CK_SOURCE_LOCATION "sourceLocation"
#define CK_LINE           "line"

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  2. PARSER STRUCT                                                      │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief Per-source-file precedence group registration. */
typedef struct {
  char *name;
  int   lbp;
  int   rbp;
} ParserPrecGroup;

/** @brief Per-source-file custom operator registration. */
typedef struct {
  char *op;
  char *group_name;
} ParserCustomOp;

/** @brief Full parser state (opaque in public header). */
typedef struct Parser Parser;
struct Parser {
  const Source      *src;
  const TokenStream *ts;      /**< Whitespace-filtered token stream.       */
  size_t             pos;     /**< Current position in the token stream.   */
  ASTArena          *arena;   /**< Arena for node allocations (not owned). */

  /* Diagnostics */
  uint32_t error_count;
  char     errors[MAX_PARSE_ERRORS][256];
  uint32_t error_line[MAX_PARSE_ERRORS];
  uint32_t error_col[MAX_PARSE_ERRORS];

  /* Context flags */
  int     no_trailing_closure;   /**< Suppress trailing closure parsing.   */
  int     class_var_flag;        /**< Set by `class var` modifier.         */
  uint8_t setter_access;        /**< Setter access from private(set) etc. */
  uint8_t import_is_testable;   /**< @testable import seen.               */

  /* Custom operators / precedence groups (per source file) */
  ParserPrecGroup pg_table[MAX_PRECEDENCE_GROUPS];
  int             pg_count;
  ParserCustomOp  custom_ops[MAX_CUSTOM_OPERATORS];
  int             custom_op_count;
};

typedef enum { PARSE_OK = 0, PARSE_ERROR = 1 } ParseStatus;

/** @brief Pratt precedence pair: left binding power, right binding power. */
typedef struct {
  int lbp;
  int rbp;
} Prec;

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  3. TOKEN QUERY HELPERS (inline)                                       │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  All O(1).  No token consumption — just inspection.
 */

/** @brief Returns the current token (no consume). */
static inline const Token *p_tok(const Parser *p) {
  return &p->ts->tokens[p->pos];
}

/** @brief Peeks one token ahead (no consume). */
static inline const Token *p_peek1(const Parser *p) {
  size_t next = p->pos + 1 < p->ts->count ? p->pos + 1 : p->pos;
  return &p->ts->tokens[next];
}

/** @brief True if at end of file. */
static inline int p_is_eof(const Parser *p) {
  return p_tok(p)->type == TOK_EOF;
}

/** @brief True if current token is keyword @p k. */
static inline int p_is_kw(const Parser *p, Keyword k) {
  const Token *t = p_tok(p);
  return t->type == TOK_KEYWORD && t->keyword == k;
}

/** @brief True if current token is punctuation @p c. */
static inline int p_is_punct(const Parser *p, char c) {
  const Token *t = p_tok(p);
  return t->type == TOK_PUNCT && p->src->data[t->pos] == c;
}

/** @brief True if current token is multi-char operator @p kind. */
static inline int p_is_op(const Parser *p, OpKind kind) {
  const Token *t = p_tok(p);
  return t->type == TOK_OPERATOR && t->op_kind == kind;
}

/** @brief True if current token is a single-char operator matching @p c. */
static inline int p_is_op_char(const Parser *p, char c) {
  const Token *t = p_tok(p);
  return t->type == TOK_OPERATOR && t->op_kind == OP_NONE &&
         p->src->data[t->pos] == c;
}

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  4. STRING MATCH MACROS                                                │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief True if token text matches a string literal (compile-time length). */
#define tok_eq(p, tok, lit) \
  ((tok)->len == (sizeof(lit) - 1) && \
   memcmp((p)->src->data + (tok)->pos, (lit), sizeof(lit) - 1) == 0)

/** @brief True if current token is an identifier matching a string literal. */
#define p_is_ident_str(p, lit) \
  (p_tok(p)->type == TOK_IDENTIFIER && tok_eq((p), p_tok(p), (lit)))

/** @brief True if current token is a contextual keyword: p_is_ck(p, CK_GET). */
#define p_is_ck(p, ck)  tok_is_ident((p), (ck), sizeof(ck) - 1)

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  5. PUNCTUATION SHORTHANDS                                             │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  p_is_punct(p, '{') → P_LBRACE(p)   — shorter, reads like prose.
 */

#define P_LBRACE(p)  p_is_punct((p), '{')
#define P_RBRACE(p)  p_is_punct((p), '}')
#define P_LPAREN(p)  p_is_punct((p), '(')
#define P_RPAREN(p)  p_is_punct((p), ')')
#define P_LBRACK(p)  p_is_punct((p), '[')
#define P_RBRACK(p)  p_is_punct((p), ']')
#define P_COMMA(p)   p_is_punct((p), ',')
#define P_COLON(p)   p_is_punct((p), ':')

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  6. PUBLIC API (also declared in internal/msf.h)                       │
 * └──────────────────────────────────────────────────────────────────────────┘ */

Parser  *parser_init(const Source *src, const TokenStream *ts, ASTArena *arena);
ASTNode *parse_source_file(Parser *p);
void     parser_destroy(Parser *p);
uint32_t    parser_error_count(const Parser *p);
const char *parser_error_message(const Parser *p, uint32_t index);
uint32_t    parser_error_line(const Parser *p, uint32_t index);
uint32_t    parser_error_col(const Parser *p, uint32_t index);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  7. INTERNAL PARSE FUNCTIONS                                           │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/* — Core (core.c) --------------------------------------------------------- */

void parser_ctx_reset(Parser *p, const Source *src, const TokenStream *ts,
                 ASTArena *arena);
const Token *adv(Parser *p);
ASTNode *alloc_node(Parser *p, ASTNodeKind kind);
void parse_error_push(Parser *p, const char *fmt, ...);
char cur_char(Parser *p);
int tok_text_eq(const Parser *p, const char *str, uint32_t len);
int tok_is_ident(const Parser *p, const char *name, size_t len);
int tok_is_underscore(const Parser *p);
int tok_is_range_op(const Parser *p);
void skip_balanced(Parser *p, char open, char close);
void skip_generic_params(Parser *p);
void add_child_unwrapped(ASTNode *parent, ASTNode *e);
void add_stmt_chain(Parser *p, ASTNode *parent, ASTNode *s);
ASTNode *closure_to_block(Parser *p, ASTNode *closure);
ASTNode *parse_hash_directive(Parser *p);
ASTNode *parse_throws_clause(Parser *p);
int throws_clause_is_throwing(const Source *src, const TokenStream *ts,
                              const ASTNode *clause);
uint32_t collect_modifiers(Parser *p);
int try_consume_setter_access(Parser *p, uint8_t access_value);

/* — String interpolation (core.c) ----------------------------------------- */

ASTNode *parse_expression_from_cstring(ASTArena *arena, const char *str);
ASTNode *parse_expression_from_cstring_with_tokens(ASTArena *arena, const char *str,
                                                   TokenStream **out_ts);

/* — Test entry points ----------------------------------------------------- */

ASTNode *parse_decl(Parser *p);
ASTNode *parse_stmt(Parser *p);
ASTNode *parse_expr(Parser *p);
ASTNode *parse_block(Parser *p);

/* — Declarations (decl/) -------------------------------------------------- */

ASTNode *parse_func_decl(Parser *p, uint32_t mods);
ASTNode *parse_var_decl(Parser *p, int is_let, uint32_t mods);
ASTNode *parse_init_decl(Parser *p, uint32_t mods);
ASTNode *parse_deinit_decl(Parser *p);
ASTNode *parse_nominal(Parser *p, ASTNodeKind kind, uint32_t mods);
ASTNode *parse_import_decl(Parser *p);
ASTNode *parse_typealias(Parser *p, uint32_t mods);
ASTNode *parse_subscript_decl(Parser *p, uint32_t mods);
ASTNode *parse_operator_decl(Parser *p, int is_infix);
ASTNode *parse_precedence_group_decl(Parser *p);
void parse_params(Parser *p, ASTNode *parent);
void parse_generic_params(Parser *p, ASTNode *parent);
void add_generic_constraint_nodes(Parser *p, ASTNode *gp,
                                  ASTNode *constraint_type,
                                  int *suppressed_first);
ASTNode *parse_inheritance_clause(Parser *p);
ASTNode *parse_where_clause(Parser *p);
void parse_protocol_body(Parser *p, ASTNode *proto);
void parse_enum_body(Parser *p, ASTNode *parent);
void parse_computed_property(Parser *p, ASTNode *node);
void parse_property_observers(Parser *p, ASTNode *node);
void parse_pattern_binding_list(Parser *p, ASTNode *parent);
uint32_t parse_optional_param_name(Parser *p);
void register_operator_from_ast(Parser *p, ASTNode *node);
void register_precedence_group_from_ast(Parser *p, ASTNode *node);

/* — Statements (stmt.c) -------------------------------------------------- */

ASTNode *parse_decl_stmt(Parser *p);
ASTNode *parse_if(Parser *p);
ASTNode *parse_for(Parser *p);
ASTNode *parse_while(Parser *p);
ASTNode *parse_repeat(Parser *p);
ASTNode *parse_switch(Parser *p);
ASTNode *parse_guard(Parser *p);
ASTNode *parse_return(Parser *p);
ASTNode *parse_throw(Parser *p);
ASTNode *parse_defer(Parser *p);
ASTNode *parse_do(Parser *p);
ASTNode *parse_jump(Parser *p, ASTNodeKind kind);
ASTNode *parse_discard(Parser *p);
int parse_condition_element(Parser *p, ASTNode *parent);

/* — Expressions (expression/) --------------------------------------------- */

ASTNode *parse_prefix(Parser *p);
ASTNode *parse_postfix(Parser *p, ASTNode *lhs);
ASTNode *parse_expr_pratt(Parser *p, int min_prec);
ASTNode *parse_closure_body(Parser *p);
ASTNode *parse_pattern(Parser *p);
Prec get_infix_prec(Parser *p);
Prec get_custom_infix_prec(Parser *p);
void parse_arg_list(Parser *p, ASTNode *parent, char end_char);
ASTNode *strip_trailing_closure_from_expr(ASTNode *e);

/* — Types (type.c) -------------------------------------------------------- */

ASTNode *parse_type(Parser *p);
