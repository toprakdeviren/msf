/**
 * @file pratt.c
 * @brief Pratt expression parser — binary operators, ternary (?:), and is/as casts.
 */
#include "../private.h"

/**
 * @brief Pratt expression parser — binary operators, ternary (?:), and is/as casts.
 *
 * Uses precedence climbing: calls parse_prefix() for the LHS, then loops
 * consuming infix operators whose left binding power >= @p min_prec.
 * Ternary and cast expressions are special-cased before the generic binary path.
 *
 * @param p         Parser state.
 * @param min_prec  Minimum precedence to continue parsing (0 for top-level).
 * @return          The parsed expression AST, or NULL on failure.
 */
ASTNode *parse_expr_pratt(Parser *p, int min_prec) {
  ASTNode *lhs = parse_prefix(p);
  if (!lhs)
    return NULL;

  while (1) {
    Prec pr = get_infix_prec(p);
    if (pr.lbp < min_prec || pr.lbp < 0)
      break;

    /* Ternary special-case: expr ? then : else */
    if (p_is_op_char(p, '?')) {
      adv(p); /* '?' */
      ASTNode *tern = alloc_node(p, AST_TERNARY_EXPR);
      if (!tern)
        return NULL;
      ast_add_child(tern, lhs);
      ASTNode *then = parse_expr_pratt(p, 0);
      if (then)
        ast_add_child(tern, then);
      if (!p_is_eof(p) && P_COLON(p))
        adv(p);
      ASTNode *els = parse_expr_pratt(p, pr.rbp);
      if (els)
        ast_add_child(tern, els);
      tern->tok_end = (uint32_t)p->pos;
      lhs = tern;
      continue;
    }

    /* is / as cast */
    if (p_is_kw(p, KW_IS) || p_is_kw(p, KW_AS)) {
      ASTNode *cast = alloc_node(p, AST_CAST_EXPR);
      if (!cast)
        return NULL;
      cast->data.binary.op_tok = (uint32_t)p->pos;
      adv(p);
      /* as? as! as */
      if (!p_is_eof(p) && (p->src->data[p_tok(p)->pos] == '?' ||
                           p->src->data[p_tok(p)->pos] == '!'))
        adv(p);
      ast_add_child(cast, lhs);
      ASTNode *rhs = parse_type(p);
      if (rhs)
        ast_add_child(cast, rhs);
      cast->tok_end = (uint32_t)p->pos;
      lhs = cast;
      continue;
    }

    /* Binary / assignment */
    ASTNode *bin = alloc_node(p, AST_BINARY_EXPR);
    if (!bin)
      return NULL;
    bin->data.binary.op_tok = (uint32_t)p->pos;
    adv(p); /* operator */
    ASTNode *rhs = parse_expr_pratt(p, pr.rbp);
    if (rhs) {
      ast_add_child(bin, lhs);
      ast_add_child(bin, rhs);
      bin->tok_end = (uint32_t)p->pos;
      lhs = bin;
    } else {
      /* no rhs — report error for invalid expression like "1 + * 2" */
      if (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR) {
        parse_error_push(p, "%s:%u:%u: expected expression after operator",
                         p->src->filename, p_tok(p)->line, p_tok(p)->col);
      }
      break;
    }
  }
  return lhs;
}
