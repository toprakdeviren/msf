/**
 * @file pattern.c
 * @brief Pattern parsing for switch/case, if-case, guard-case, for-case.
 *
 * Grammar (subset):
 *   pattern := '_'                              → PATTERN_WILDCARD
 *            | '.' ident ['(' binding-list ')'] → PATTERN_ENUM
 *            | Type.ident ['(' ... ')']         → PATTERN_ENUM (qualified)
 *            | '(' pattern-list ')'             → PATTERN_TUPLE
 *            | 'is' Type                        → PATTERN_TYPE
 *            | ('let'|'var') ident              → PATTERN_VALUE_BINDING
 *            | ('let'|'var') '(' ident, ... ')' → PATTERN_TUPLE (destructure)
 *            | expr '...' expr                  → PATTERN_RANGE
 *            | expr '..<' expr                  → PATTERN_RANGE
 *            | expr                             → literal/value (fallback)
 */
#include "private.h"

/* Minimum precedence to stop Pratt parsing before range operators (prec 135). */
#define PREC_ABOVE_RANGE 136

/* Pattern-specific modifier flags (reuse unused MOD bits on pattern nodes). */
#define PAT_IS_VAR        (1u << 8)
#define PAT_RANGE_PREFIX  (1u << 16)
#define PAT_RANGE_POSTFIX (1u << 17)
#define PAT_OPTIONAL      (1u << 18)  /* `x?` optional-some binding suffix */

/* ═══════════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns 1 if the current token is `_`. */
int tok_is_underscore(const Parser *p) {
  const Token *t = p_tok(p);
  return t->type == TOK_IDENTIFIER && t->len == 1 &&
         p->src->data[t->pos] == '_';
}

/** @brief Returns 1 if the current token is `...` or `..<`. */
int tok_is_range_op(const Parser *p) {
  const Token *t = p_tok(p);
  return t->type == TOK_OPERATOR &&
         (tok_eq(p, t, "...") || tok_eq(p, t, "..<"));
}

/** @brief Returns 1 if the current token is a single-char `.` (PUNCT or OPERATOR). */
static int p_is_dot(const Parser *p) {
  if (p_is_punct(p, '.')) return 1;
  const Token *t = p_tok(p);
  return t->type == TOK_OPERATOR && t->len == 1 && p->src->data[t->pos] == '.';
}

/** @brief Parses (...) binding list for enum associated values. */
void parse_pattern_binding_list(Parser *p, ASTNode *parent) {
  if (!P_LPAREN(p)) return;
  adv(p);
  while (!p_is_eof(p) && !P_RPAREN(p)) {
    ASTNode *sub = parse_pattern(p);
    if (sub) ast_add_child(parent, sub);
    else     adv(p);
    if (P_COMMA(p)) adv(p);
  }
  if (P_RPAREN(p)) adv(p);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Pattern Parsers — one per pattern kind
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief `_` → PATTERN_WILDCARD */
static ASTNode *parse_pat_wildcard(Parser *p) {
  ASTNode *n = alloc_node(p, AST_PATTERN_WILDCARD);
  if (!n) return NULL;
  adv(p);
  n->tok_end = (uint32_t)p->pos;
  return n;
}

/** @brief `.caseName(bindings)` or `Type.caseName(bindings)` → PATTERN_ENUM */
static ASTNode *parse_pat_enum(Parser *p, int qualified) {
  ASTNode *pat = alloc_node(p, AST_PATTERN_ENUM);
  if (!pat) return NULL;

  if (qualified) {
    ASTNode *tn = alloc_node(p, AST_TYPE_IDENT);
    if (tn) ast_add_child(pat, tn);
    adv(p); /* type name */
  }
  adv(p); /* '.' */

  pat->data.var.name_tok = (uint32_t)p->pos;
  if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD)
    adv(p);

  if (P_LPAREN(p))
    parse_pattern_binding_list(p, pat);

  pat->tok_end = (uint32_t)p->pos;
  return pat;
}

/** @brief `(pat, pat, ...)` → PATTERN_TUPLE */
static ASTNode *parse_pat_tuple(Parser *p) {
  ASTNode *tup = alloc_node(p, AST_PATTERN_TUPLE);
  if (!tup) return NULL;
  adv(p);
  while (!p_is_eof(p) && !P_RPAREN(p)) {
    ASTNode *sub = parse_pattern(p);
    if (sub) ast_add_child(tup, sub);
    else     adv(p);
    if (P_COMMA(p)) adv(p);
  }
  if (P_RPAREN(p)) adv(p);
  tup->tok_end = (uint32_t)p->pos;
  return tup;
}

/** @brief `is Type` → PATTERN_TYPE */
static ASTNode *parse_pat_type(Parser *p) {
  ASTNode *pat = alloc_node(p, AST_PATTERN_TYPE);
  if (!pat) return NULL;
  adv(p); /* 'is' */
  ASTNode *tn = parse_type(p);
  if (tn) ast_add_child(pat, tn);
  pat->tok_end = (uint32_t)p->pos;
  return pat;
}

/** @brief Builds a single value binding node (let x / var x). */
static ASTNode *make_binding(Parser *p, int is_var) {
  ASTNode *b = alloc_node(p, AST_PATTERN_VALUE_BINDING);
  if (!b) return NULL;
  b->data.var.name_tok = (uint32_t)p->pos;
  if (is_var) b->modifiers |= PAT_IS_VAR;
  if (p_tok(p)->type == TOK_IDENTIFIER) adv(p);
  b->tok_end = (uint32_t)p->pos;
  return b;
}

static ASTNode *parse_binding_tuple(Parser *p, int is_var); /* fwd */

/**
 * @brief Parses one element of a `let (...)` / `var (...)` binding tuple.
 *
 * `let` distributes over the tuple, so an identifier element is a value
 * binding (`a` → bind a).  Elements may also be optional bindings (`a?`),
 * wildcards (`_`), `nil`, nested tuples (`(x, y)`), or enum/literal patterns
 * (`.some`, `nil`).  Anything not handled as a binding falls back to the
 * general pattern parser.  Always makes forward progress (never returns
 * without advancing) to avoid an infinite loop on unexpected tokens.
 */
static ASTNode *parse_binding_tuple_element(Parser *p, int is_var) {
  size_t before = p->pos;
  ASTNode *el = NULL;

  if (P_LPAREN(p)) {
    el = parse_binding_tuple(p, is_var); /* nested destructure */
  } else if (tok_is_underscore(p)) {
    el = parse_pat_wildcard(p);
  } else if (p_tok(p)->type == TOK_IDENTIFIER) {
    el = make_binding(p, is_var);
    /* `a?` / `a!` optional-some binding suffix */
    if (el && !p_is_eof(p) && p_tok(p)->len == 1 &&
        (p->src->data[p_tok(p)->pos] == '?' ||
         p->src->data[p_tok(p)->pos] == '!')) {
      el->modifiers |= PAT_OPTIONAL;
      adv(p);
      el->tok_end = (uint32_t)p->pos;
    }
  } else {
    /* nil / literal / .case — defer to the general pattern parser */
    el = parse_pattern(p);
  }

  /* Guarantee progress: an unexpected token (no identifier, no pattern)
     must still be consumed or the enclosing while-loop spins forever. */
  if (p->pos == before)
    adv(p);
  return el;
}

/** @brief `let (x, y)` / `var (x, y)` → PATTERN_TUPLE of value bindings. */
static ASTNode *parse_binding_tuple(Parser *p, int is_var) {
  ASTNode *tup = alloc_node(p, AST_PATTERN_TUPLE);
  if (!tup) return NULL;
  adv(p); /* '(' */
  while (!p_is_eof(p) && !P_RPAREN(p)) {
    ASTNode *b = parse_binding_tuple_element(p, is_var);
    if (b) ast_add_child(tup, b);
    if (P_COMMA(p)) adv(p);
  }
  if (P_RPAREN(p)) adv(p);
  tup->tok_end = (uint32_t)p->pos;
  return tup;
}

ASTNode *parse_binding_tuple_pattern(Parser *p, int is_var) {
  return parse_binding_tuple(p, is_var);
}

/**
 * @brief Handles `as Type` suffix after a value binding: `let x as String`.
 *
 * Wraps the binding + type pattern in a PATTERN_TUPLE so the caller
 * sees both nodes as siblings.
 */
static ASTNode *wrap_binding_as_type(Parser *p, ASTNode *binding) {
  adv(p); /* 'as' */
  /* consume optional ? or ! after as */
  if (!p_is_eof(p) && p_tok(p)->len == 1 &&
      (p->src->data[p_tok(p)->pos] == '?' || p->src->data[p_tok(p)->pos] == '!'))
    adv(p);

  ASTNode *type_pat = alloc_node(p, AST_PATTERN_TYPE);
  if (!type_pat) return binding;
  ASTNode *tn = parse_type(p);
  if (tn) ast_add_child(type_pat, tn);
  type_pat->tok_end = (uint32_t)p->pos;

  ASTNode *combo = alloc_node(p, AST_PATTERN_TUPLE);
  if (!combo) return binding;
  ast_add_child(combo, binding);
  ast_add_child(combo, type_pat);
  combo->tok_end = (uint32_t)p->pos;
  return combo;
}

/** @brief `let x` / `var x` / `let (x, y)` / `let x as Type` → value binding(s). */
static ASTNode *parse_pat_binding(Parser *p) {
  int is_var = p_is_kw(p, KW_VAR);
  adv(p); /* let/var */

  if (P_LPAREN(p))
    return parse_binding_tuple(p, is_var);

  ASTNode *binding = make_binding(p, is_var);
  if (!binding) return NULL;

  if (p_is_kw(p, KW_AS))
    return wrap_binding_as_type(p, binding);

  return binding;
}

/** @brief `...expr` or `..<expr` (prefix range, no lower bound). */
static ASTNode *parse_pat_prefix_range(Parser *p) {
  ASTNode *range = alloc_node(p, AST_PATTERN_RANGE);
  if (!range) return NULL;
  range->data.var.name_tok = (uint32_t)p->pos;
  range->modifiers |= PAT_RANGE_PREFIX;
  adv(p); /* range op */
  ASTNode *rhs = parse_expr_pratt(p, PREC_ABOVE_RANGE);
  if (rhs) ast_add_child(range, rhs);
  range->tok_end = (uint32_t)p->pos;
  return range;
}

/** @brief Checks for `expr...expr` / `expr..<expr` range after an expression. */
static ASTNode *try_range_suffix(Parser *p, ASTNode *lhs) {
  if (!tok_is_range_op(p)) return lhs;

  ASTNode *range = alloc_node(p, AST_PATTERN_RANGE);
  if (!range) return lhs;
  range->data.var.name_tok = (uint32_t)p->pos;
  adv(p); /* range op */

  int has_rhs = !p_is_eof(p) && !P_COLON(p) && !P_LBRACE(p) &&
                !P_COMMA(p) && !p_is_kw(p, KW_WHERE);
  ASTNode *rhs = has_rhs ? parse_expr_pratt(p, PREC_ABOVE_RANGE) : NULL;

  ast_add_child(range, lhs);
  if (rhs) ast_add_child(range, rhs);
  else     range->modifiers |= PAT_RANGE_POSTFIX;
  range->tok_end = (uint32_t)p->pos;
  return range;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Main Dispatch
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses a single match pattern.
 *
 * Tries each pattern form in order.  Falls back to parse_expr_pratt()
 * for literal/value patterns, then checks for a range operator suffix.
 */
ASTNode *parse_pattern(Parser *p) {
  if (p_is_eof(p)) return NULL;

  /* Wildcard: _ */
  if (tok_is_underscore(p))
    return parse_pat_wildcard(p);

  /* Enum pattern: .caseName or Type.caseName */
  if (p_is_dot(p))
    return parse_pat_enum(p, 0);
  if (p_tok(p)->type == TOK_IDENTIFIER) {
    const Token *nxt = p_peek1(p);
    if (nxt && (nxt->type == TOK_OPERATOR || nxt->type == TOK_PUNCT) &&
        nxt->len == 1 && p->src->data[nxt->pos] == '.')
      return parse_pat_enum(p, 1);
  }

  /* Tuple: (pat, pat) */
  if (P_LPAREN(p))
    return parse_pat_tuple(p);

  /* Type check: is SomeType */
  if (p_is_kw(p, KW_IS))
    return parse_pat_type(p);

  /* Value binding: let x, var x, let (x, y), let x as Type */
  if (p_is_kw(p, KW_LET) || p_is_kw(p, KW_VAR))
    return parse_pat_binding(p);

  /* Prefix range: ...expr, ..<expr */
  if (tok_is_range_op(p))
    return parse_pat_prefix_range(p);

  /* Expression (possibly followed by range: expr...expr) */
  size_t before = p->pos;
  ASTNode *lhs = parse_expr_pratt(p, PREC_ABOVE_RANGE);
  if (!lhs) { p->pos = before; return NULL; }
  return try_range_suffix(p, lhs);
}
