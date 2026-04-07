/**
 * @file closure.c
 * @brief Closure expression parsing: { [captures] params in body }.
 */
#include "../private.h"

/**
 * @brief Parses the optional capture list: [weak self, unowned(safe) x].
 *
 * Produces AST_CLOSURE_CAPTURE nodes with capture qualifier modifiers
 * (strong/weak/unowned/unowned(safe)) and optional initializer expressions.
 */
static void parse_closure_capture_list(Parser *p, ASTNode *cl) {
  if (!P_LBRACK(p))
    return;
  adv(p); /* '[' */

  while (!p_is_eof(p) && !P_RBRACK(p)) {
    ASTNode *cap = alloc_node(p, AST_CLOSURE_CAPTURE);
    if (!cap)
      return;
    uint32_t qual = MOD_CAPTURE_STRONG;

    /* Qualifier: weak | unowned | unowned(safe) */
    if (p_tok(p)->type == TOK_IDENTIFIER) {
      const Token *qt = p_tok(p);
      if (tok_eq(p, qt, CK_WEAK)) {
        qual = MOD_CAPTURE_WEAK;
        adv(p);
      } else if (tok_eq(p, qt, CK_UNOWNED)) {
        qual = MOD_CAPTURE_UNOWNED;
        adv(p);
        if (P_LPAREN(p)) {
          adv(p);
          if (p_tok(p)->type == TOK_IDENTIFIER) {
            const Token *st = p_tok(p);
            if (tok_eq(p, st, CK_SAFE))
              qual |= MOD_CAPTURE_SAFE;
            adv(p);
          }
          if (P_RPAREN(p))
            adv(p);
        }
      }
    }

    cap->tok_idx = (uint32_t)p->pos;
    cap->modifiers = qual;
    if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD)
      adv(p); /* name / self / super */

    /* Optional capture initializer: = expr */
    if (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
        p_tok(p)->len == 1 && p->src->data[p_tok(p)->pos] == '=') {
      adv(p);
      ASTNode *init = parse_expr_pratt(p, 0);
      if (init)
        ast_add_child(cap, init);
    }

    cap->tok_end = (uint32_t)p->pos;
    ast_add_child(cl, cap);

    if (P_COMMA(p))
      adv(p);
    else
      break;
  }

  if (P_RBRACK(p))
    adv(p);
}

/**
 * @brief Parses the closure signature before the `in` keyword.
 *
 * Supports two forms:
 *   - Typed: (a: Int, b: Int) -> RetType in
 *   - Simple: x, y in
 *
 * If no `in` keyword is found after the parameter-like tokens, the parser
 * backtracks and the tokens are re-parsed as body statements instead.
 */
static void parse_closure_signature(Parser *p, ASTNode *cl) {
  size_t saved = p->pos;
  int has_in = 0;

  if (P_LPAREN(p)) {
    /* Full typed param list: (a: Int, b: Int) -> RetType in */
    parse_params(p, cl);

    if (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
        p_tok(p)->op_kind == OP_ARROW) {
      adv(p);
      ASTNode *ret_ty = parse_type(p);
      if (ret_ty)
        ast_add_child(cl, ret_ty);
    }

    if (p_is_kw(p, KW_IN)) {
      has_in = 1;
      adv(p);
    } else {
      /* Not a param list — backtrack */
      cl->first_child = NULL;
      cl->last_child = NULL;
      p->pos = saved;
    }
  } else {
    /* Simple: x, y in — identifier list before 'in' */
    while (p_tok(p)->type == TOK_IDENTIFIER && !p_is_eof(p)) {
      ASTNode *param = alloc_node(p, AST_PARAM);
      if (!param)
        return;
      param->data.var.name_tok = (uint32_t)p->pos;
      adv(p);

      /* Optional type annotation: x: Int */
      if (P_COLON(p)) {
        adv(p);
        ASTNode *ty = parse_type(p);
        if (ty)
          ast_add_child(param, ty);
      }

      ast_add_child(cl, param);

      if (p_is_kw(p, KW_IN)) {
        has_in = 1;
        adv(p);
        break;
      }

      if (P_COMMA(p))
        adv(p);
      else
        break;
    }

    /* Optional explicit result: -> Type before 'in' */
    if (!has_in && !p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
        p_tok(p)->op_kind == OP_ARROW) {
      adv(p);
      ASTNode *ret_ty = parse_type(p);
      if (ret_ty)
        ast_add_child(cl, ret_ty);
    }

    if (!has_in && p_is_kw(p, KW_IN)) {
      has_in = 1;
      adv(p);
    }

    if (!has_in) {
      /* Not a param list — backtrack */
      cl->first_child = NULL;
      cl->last_child = NULL;
      p->pos = saved;
    }
  }
}

/**
 * @brief Parses a complete closure expression: { [captures] params in body }.
 *
 * Delegates to parse_closure_capture_list() and parse_closure_signature()
 * for the header, then parses body statements until the closing '}'.
 *
 * @return An AST_CLOSURE_EXPR node.
 */
ASTNode *parse_closure_body(Parser *p) {
  ASTNode *cl = alloc_node(p, AST_CLOSURE_EXPR);
  if (!cl)
    return NULL;
  cl->tok_idx = (uint32_t)p->pos;
  adv(p); /* '{' */

  parse_closure_capture_list(p, cl);
  parse_closure_signature(p, cl);

  /* Body statements */
  while (!p_is_eof(p) && !P_RBRACE(p)) {
    ASTNode *stmt = parse_stmt(p);
    if (stmt)
      ast_add_child(cl, stmt);
  }

  if (P_RBRACE(p))
    adv(p);

  cl->tok_end = (uint32_t)p->pos;
  return cl;
}
