/**
 * @file stmt.c
 * @brief Statement parsers: if, guard, for, while, repeat, switch, do-catch,
 *        return, throw, defer, break, continue, fallthrough, discard.
 */
#include "private.h"

ASTNode *parse_pattern(Parser *p);

/* ═══════════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses an expression with trailing-closure suppression.
 *
 * In conditions (if/while/for) and catch clauses, a '{' must NOT be
 * consumed as a trailing closure — it's the statement body.
 */
static ASTNode *parse_expr_no_trailing_closure(Parser *p) {
  int saved = p->no_trailing_closure;
  p->no_trailing_closure = 1;
  ASTNode *e = parse_expr_pratt(p, 0);
  p->no_trailing_closure = saved;
  return e;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Simple Statements
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief `return [expr]` — expr is optional (bare return in void functions). */
ASTNode *parse_return(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_RETURN_STMT);
  if (!node) return NULL;
  if (!p_is_eof(p) && !P_RBRACE(p) && !P_RPAREN(p)) {
    ASTNode *val = parse_expr_pratt(p, 0);
    if (val) ast_add_child(node, val);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/** @brief `throw expr` */
ASTNode *parse_throw(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_THROW_STMT);
  if (!node) return NULL;
  ASTNode *val = parse_expr_pratt(p, 0);
  if (val) ast_add_child(node, val);
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/** @brief `defer { body }` */
ASTNode *parse_defer(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_DEFER_STMT);
  if (!node) return NULL;
  if (P_LBRACE(p)) ast_add_child(node, parse_block(p));
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/** @brief `discard expr` (Swift 5.9) */
ASTNode *parse_discard(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_DISCARD_STMT);
  if (!node) return NULL;
  ASTNode *expr = parse_expr_pratt(p, 0);
  if (expr) ast_add_child(node, expr);
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/** @brief `break [label]` / `continue [label]` / `fallthrough` */
ASTNode *parse_jump(Parser *p, ASTNodeKind kind) {
  adv(p);
  ASTNode *node = alloc_node(p, kind);
  if (!node) return NULL;
  node->data.var.name_tok = 0;
  if (p_tok(p)->type == TOK_IDENTIFIER) {
    node->data.var.name_tok = (uint32_t)p->pos;
    adv(p);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Condition Element — shared by if, guard, while
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Parses `case pattern = subject` inside a condition list. */
static int parse_case_condition(Parser *p, ASTNode *parent) {
  adv(p); /* case */
  ASTNode *pat = parse_pattern(p);
  if (!pat) return 1;

  if (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
      p->src->data[p_tok(p)->pos] == '=' && p_tok(p)->len == 1) {
    adv(p);
    ASTNode *subj = parse_expr_no_trailing_closure(p);
    ASTNode *assign = alloc_node(p, AST_ASSIGN_EXPR);
    if (!assign) return -1;
    assign->modifiers |= MOD_INDIRECT; /* marks as "if case" assign */
    ast_add_child(assign, pat);
    if (subj) ast_add_child(assign, subj);
    assign->tok_end = (uint32_t)p->pos;
    ast_add_child(parent, assign);
  } else {
    ast_add_child(parent, pat);
  }
  return 1;
}

/** @brief Parses `let x: Type = expr` or `var x = expr` optional binding. */
static int parse_optional_binding(Parser *p, ASTNode *parent) {
  int is_let = p_is_kw(p, KW_LET);
  adv(p);
  ASTNode *binding = alloc_node(p, AST_OPTIONAL_BINDING);
  if (!binding) return -1;
  binding->data.var.name_tok = (uint32_t)p->pos;
  binding->data.var.is_computed = is_let ? 0 : 1;
  if (p_tok(p)->type == TOK_IDENTIFIER) adv(p);

  if (P_COLON(p)) {
    adv(p);
    ASTNode *ty = parse_type(p);
    if (ty) ast_add_child(binding, ty);
  }
  if (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
      p->src->data[p_tok(p)->pos] == '=' && p_tok(p)->len == 1) {
    adv(p);
    ASTNode *init = parse_expr_no_trailing_closure(p);
    if (init) ast_add_child(binding, init);
  }
  ast_add_child(parent, binding);
  return 1;
}

/**
 * @brief Parses one condition element and adds it as a child.
 *
 * Handles three forms:
 *   - `case pattern = subject`
 *   - `let x = expr` / `var x = expr` (optional binding)
 *   - plain boolean expression
 *
 * @return 1 = parsed, 0 = nothing recognized, -1 = trailing closure consumed.
 */
int parse_condition_element(Parser *p, ASTNode *parent) {
  if (p_is_kw(p, KW_CASE))
    return parse_case_condition(p, parent);

  if (p_is_kw(p, KW_LET) || p_is_kw(p, KW_VAR))
    return parse_optional_binding(p, parent);

  /* Plain boolean expression */
  size_t before = p->pos;
  ASTNode *e = parse_expr_no_trailing_closure(p);
  if (e) { ast_add_child(parent, e); return 1; }
  if (p->pos == before) adv(p);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Conditional Statements
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Parses the else clause of an if statement (else if / else { }). */
static void parse_else_clause(Parser *p, ASTNode *node) {
  if (!p_is_kw(p, KW_ELSE)) return;
  adv(p);
  if (p_is_kw(p, KW_IF))
    ast_add_child(node, parse_if(p));
  else if (P_LBRACE(p))
    ast_add_child(node, parse_block(p));
}

/** @brief `if cond, cond { } else if { } else { }` */
ASTNode *parse_if(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_IF_STMT);
  if (!node) return NULL;

  while (!p_is_eof(p) && !P_LBRACE(p)) {
    int rc = parse_condition_element(p, node);
    if (rc == -1) { parse_else_clause(p, node); node->tok_end = (uint32_t)p->pos; return node; }
    if (P_COMMA(p)) adv(p);
  }
  if (P_LBRACE(p)) ast_add_child(node, parse_block(p));
  parse_else_clause(p, node);
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/** @brief `guard cond else { }` */
ASTNode *parse_guard(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_GUARD_STMT);
  if (!node) return NULL;

  while (!p_is_eof(p) && !p_is_kw(p, KW_ELSE)) {
    parse_condition_element(p, node);
    if (P_COMMA(p)) adv(p);
  }
  if (p_is_kw(p, KW_ELSE)) adv(p);
  if (P_LBRACE(p)) ast_add_child(node, parse_block(p));
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/** @brief `while cond { }` */
ASTNode *parse_while(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_WHILE_STMT);
  if (!node) return NULL;

  while (!p_is_eof(p) && !P_LBRACE(p)) {
    int rc = parse_condition_element(p, node);
    if (rc == -1) { node->tok_end = (uint32_t)p->pos; return node; }
    if (P_COMMA(p)) adv(p);
  }
  if (P_LBRACE(p)) ast_add_child(node, parse_block(p));
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/** @brief `repeat { } while cond` */
ASTNode *parse_repeat(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_REPEAT_STMT);
  if (!node) return NULL;
  if (P_LBRACE(p)) ast_add_child(node, parse_block(p));
  if (p_is_kw(p, KW_WHILE)) {
    adv(p);
    ASTNode *cond = parse_expr_pratt(p, 0);
    if (cond) ast_add_child(node, cond);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * For-In
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief `for [case let] pattern in sequence [where guard] { body }` */
ASTNode *parse_for(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_FOR_STMT);
  if (!node) return NULL;

  /* Optional `case let` prefix */
  int is_for_case = 0;
  if (p_is_kw(p, KW_CASE)) {
    adv(p);
    is_for_case = 1;
    node->modifiers |= MOD_OVERRIDE; /* marks for-case */
    if (p_is_kw(p, KW_LET)) adv(p);
  }

  /* Pattern or binding */
  if (!p_is_eof(p) && !p_is_kw(p, KW_IN) && !P_LBRACE(p)) {
    size_t before = p->pos;
    ASTNode *pat = is_for_case ? parse_pattern(p) : parse_expr_pratt(p, 0);
    if (pat) {
      if (is_for_case) {
        pat->modifiers |= MOD_OVERRIDE;
        ast_add_child(node, pat);
      } else {
        ASTNode *binding = alloc_node(p, AST_PARAM);
        if (!binding) return NULL;
        binding->data.var.name_tok = pat->tok_idx;
        binding->tok_end = pat->tok_end;
        ast_add_child(node, binding);
      }
    } else {
      p->pos = before;
      while (!p_is_eof(p) && !p_is_kw(p, KW_IN) && !P_LBRACE(p)) adv(p);
    }
  }

  if (p_is_kw(p, KW_IN)) adv(p);

  /* Sequence expression (no trailing closure — '{' is the body) */
  ASTNode *seq = parse_expr_no_trailing_closure(p);
  if (seq) ast_add_child(node, seq);

  /* where guard */
  if (p_is_kw(p, KW_WHERE)) {
    adv(p);
    ASTNode *guard = parse_expr_pratt(p, 0);
    if (guard) ast_add_child(node, guard);
  }

  if (P_LBRACE(p)) ast_add_child(node, parse_block(p));
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Do-Catch
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Parses a single catch clause: `catch pattern { body }`. */
static ASTNode *parse_catch_clause(Parser *p) {
  adv(p); /* catch */
  ASTNode *clause = alloc_node(p, AST_CATCH_CLAUSE);
  if (!clause) return NULL;

  /* Optional pattern: `catch let e as MyError`, `catch MyError.foo` */
  if (!p_is_eof(p) && !P_LBRACE(p)) {
    size_t before = p->pos;
    ASTNode *pat = parse_pattern(p);
    if (pat)
      ast_add_child(clause, pat);
    else {
      p->pos = before;
      while (!p_is_eof(p) && !P_LBRACE(p)) adv(p);
    }
  }

  if (P_LBRACE(p)) ast_add_child(clause, parse_block(p));
  clause->tok_end = (uint32_t)p->pos;
  return clause;
}

/** @brief `do [throws(E)] { } catch pat { } catch { }` */
ASTNode *parse_do(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_DO_STMT);
  if (!node) return NULL;

  /* Swift 6: typed do-catch: `do throws(ErrorType) { }` */
  ASTNode *throws_clause = parse_throws_clause(p);
  if (throws_clause) {
    if (throws_clause_is_throwing(p->src, p->ts, throws_clause))
      node->modifiers |= MOD_THROWS;
    if (throws_clause->modifiers & MOD_RETHROWS)
      node->modifiers |= MOD_RETHROWS;
    ast_add_child(node, throws_clause);
  }

  if (P_LBRACE(p)) ast_add_child(node, parse_block(p));

  while (p_is_kw(p, KW_CATCH)) {
    ASTNode *clause = parse_catch_clause(p);
    if (clause) ast_add_child(node, clause);
  }

  node->tok_end = (uint32_t)p->pos;
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Switch
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns 1 if at a case clause boundary (case/default/'}' after switch body). */
static int at_case_boundary(const Parser *p) {
  return p_is_kw(p, KW_CASE) || p_is_kw(p, KW_DEFAULT) || P_RBRACE(p);
}

/** @brief Parses comma-separated patterns + optional where guard for a case clause. */
static void parse_case_patterns(Parser *p, ASTNode *clause) {
  while (!p_is_eof(p) && p->src->data[p_tok(p)->pos] != ':' &&
         !at_case_boundary(p)) {
    size_t before = p->pos;
    ASTNode *pat = parse_pattern(p);
    if (pat) ast_add_child(clause, pat);
    else if (p->pos == before) adv(p);

    if (p_is_kw(p, KW_WHERE)) {
      adv(p);
      ASTNode *guard = parse_expr_pratt(p, 0);
      if (guard) {
        clause->data.cas.has_guard = 1;
        clause->data.cas.where_expr = guard;
      }
    }
    if (P_COMMA(p)) adv(p);
  }
}

/** @brief Parses the body statements of a case clause (until next case/default/}). */
static void parse_case_body(Parser *p, ASTNode *clause) {
  while (!p_is_eof(p) && !at_case_boundary(p))
    add_stmt_chain(p, clause, parse_decl_stmt(p));
}

/** @brief `switch subject { case pat: stmts  default: stmts }` */
ASTNode *parse_switch(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_SWITCH_STMT);
  if (!node) return NULL;

  /* Subject expression */
  ASTNode *subj = parse_expr_pratt(p, 0);
  if (subj) ast_add_child(node, subj);

  if (!P_LBRACE(p)) { node->tok_end = (uint32_t)p->pos; return node; }
  adv(p);

  /* Case clauses */
  while (!p_is_eof(p) && !P_RBRACE(p)) {
    if (p_is_kw(p, KW_CASE) || p_is_kw(p, KW_DEFAULT)) {
      ASTNode *clause = alloc_node(p, AST_CASE_CLAUSE);
      if (!clause) return NULL;
      clause->data.cas.is_default = p_is_kw(p, KW_DEFAULT);
      adv(p);

      if (!clause->data.cas.is_default)
        parse_case_patterns(p, clause);

      /* Consume ':' */
      if (!p_is_eof(p) && p->src->data[p_tok(p)->pos] == ':') adv(p);

      parse_case_body(p, clause);
      clause->tok_end = (uint32_t)p->pos;
      ast_add_child(node, clause);
    } else {
      adv(p); /* recovery */
    }
  }

  if (P_RBRACE(p)) adv(p);
  node->tok_end = (uint32_t)p->pos;
  return node;
}
