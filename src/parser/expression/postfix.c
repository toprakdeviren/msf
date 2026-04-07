/**
 * @file postfix.c
 * @brief Postfix expression chain: .member, (), [], ?, !, trailing closures.
 */
#include "../private.h"

/** @brief Parses member access: expr.name or expr.0 (tuple index). */
static ASTNode *parse_postfix_member(Parser *p, ASTNode *lhs) {
  int is_dot = (p_is_punct(p, '.')) ||
               (p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1 &&
                p->src->data[p_tok(p)->pos] == '.');
  if (is_dot) {
    adv(p);
    ASTNode *mem = alloc_node(p, AST_MEMBER_EXPR);
    if (!mem) return NULL;
    mem->data.var.name_tok = (uint32_t)p->pos;
    if (p_tok(p)->type == TOK_IDENTIFIER ||
        p_tok(p)->type == TOK_INTEGER_LIT || p_tok(p)->type == TOK_KEYWORD)
      adv(p);
    ast_add_child(mem, lhs);
    mem->tok_end = (uint32_t)p->pos;
    return mem;
  }
  return NULL;
}

/**
 * @brief Speculatively parses generic type arguments before a function call.
 *
 * Distinguishes `Dict<String, Int>()` (generic call) from `a < b` (comparison)
 * by scanning ahead for a balanced `>` followed by `(`.  Only consumes the
 * angle-bracketed arguments if the pattern matches.
 */
static ASTNode *parse_postfix_generic_call(Parser *p, ASTNode *lhs) {
  if (lhs->kind == AST_IDENT_EXPR && !p_is_eof(p) &&
      p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1 &&
      p->src->data[p_tok(p)->pos] == '<') {
    int found_generic_call = 0;
    size_t scan = p->pos + 1;
    int depth = 1;

    while (scan < p->ts->count && depth > 0) {
      const Token *st = &p->ts->tokens[scan];
      if (st->type == TOK_OPERATOR && st->len == 1) {
        char sc = p->src->data[st->pos];
        if (sc == '<') depth++;
        else if (sc == '>') {
          depth--;
          if (depth == 0) break;
        }
      }
      if (st->type == TOK_NEWLINE || scan - p->pos > 16) break;
      scan++;
    }

    if (depth == 0 && scan + 1 < p->ts->count) {
      const Token *after_gt = &p->ts->tokens[scan + 1];
      if (after_gt->type == TOK_PUNCT && p->src->data[after_gt->pos] == '(')
        found_generic_call = 1;
    }

    if (found_generic_call) {
      adv(p); /* '<' */
      int d = 1;
      while (!p_is_eof(p) && d > 0) {
        if (p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1) {
          char c = p->src->data[p_tok(p)->pos];
          if (c == '<') d++;
          else if (c == '>') { d--; adv(p); break; }
        }
        adv(p);
      }
      return lhs;
    }
  }
  return NULL;
}

/** @brief Parses a function call: expr(args), including a trailing closure if present. */
static ASTNode *parse_postfix_call(Parser *p, ASTNode *lhs) {
  if (P_LPAREN(p)) {
    ASTNode *call = alloc_node(p, AST_CALL_EXPR);
    if (!call) return NULL;
    ast_add_child(call, lhs);
    adv(p); /* '(' */
    parse_arg_list(p, call, ')');
    if (P_RPAREN(p)) adv(p);

    if (P_LBRACE(p) && !p->no_trailing_closure) {
      ASTNode *tc = parse_closure_body(p);
      if (tc) ast_add_child(call, tc);
    }

    call->tok_end = (uint32_t)p->pos;
    return call;
  }
  return NULL;
}

/**
 * @brief Parses subscript access: expr[args].
 *
 * Rejects `[` on a different line than the LHS to avoid consuming array
 * literals that happen to follow an expression on the next line.
 */
static ASTNode *parse_postfix_subscript(Parser *p, ASTNode *lhs) {
  if (P_LBRACK(p)) {
    if (lhs->tok_end > 0 && lhs->tok_end <= p->pos &&
        lhs->tok_end - 1 < p->ts->count) {
      const Token *last_lhs_tok = &p->ts->tokens[lhs->tok_end - 1];
      const Token *cur_tok = p_tok(p);
      if (last_lhs_tok->line != cur_tok->line)
        return NULL; /* next-line bracket — not a subscript */
    }
    ASTNode *sub = alloc_node(p, AST_SUBSCRIPT_EXPR);
    if (!sub) return NULL;
    ast_add_child(sub, lhs);
    adv(p);
    parse_arg_list(p, sub, ']');
    if (P_RBRACK(p)) adv(p);
    sub->tok_end = (uint32_t)p->pos;
    return sub;
  }
  return NULL;
}

/**
 * @brief Parses optional chaining: expr?.
 *
 * Only consumed when `?` is adjacent to the LHS (no whitespace), to avoid
 * conflicting with the ternary operator `a ? b : c`.
 */
static ASTNode *parse_postfix_optional_chain(Parser *p, ASTNode *lhs) {
  if (!p_is_eof(p) && p->src->data[p_tok(p)->pos] == '?' &&
      p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1) {
    uint32_t prev_end = lhs->tok_end > 0 ? lhs->tok_end : (p_tok(p)->pos);

    if (p_tok(p)->pos == prev_end ||
        (p_tok(p)->col > 1 && lhs->tok_idx + 1 >= p->pos - 1)) {
      uint32_t qpos = p_tok(p)->pos;
      if (qpos > 0 && p->src->data[qpos - 1] != ' ' &&
          p->src->data[qpos - 1] != '\n' && p->src->data[qpos - 1] != '\t') {
        ASTNode *oc = alloc_node(p, AST_OPTIONAL_CHAIN);
        if (!oc) return NULL;
        ast_add_child(oc, lhs);
        adv(p);
        oc->tok_end = (uint32_t)p->pos;
        return oc;
      }
    }
  }
  return NULL;
}

/** @brief Parses force unwrap: expr!. */
static ASTNode *parse_postfix_force_unwrap(Parser *p, ASTNode *lhs) {
  if (!p_is_eof(p) && p->src->data[p_tok(p)->pos] == '!' &&
      p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1) {
    ASTNode *fu = alloc_node(p, AST_FORCE_UNWRAP);
    if (!fu) return NULL;
    ast_add_child(fu, lhs);
    adv(p);
    fu->tok_end = (uint32_t)p->pos;
    return fu;
  }
  return NULL;
}

/**
 * @brief Parses trailing and multi-trailing closures: expr { } label: { }.
 *
 * Wraps the LHS in a CALL_EXPR.  Skips `{` if the next token is `case` or
 * `default` (switch body, not a trailing closure).  Multi-trailing closures
 * follow the pattern: `identifier: { body }`.
 */
static ASTNode *parse_postfix_trailing_closure(Parser *p, ASTNode *lhs) {
  if (P_LBRACE(p) && !p->no_trailing_closure) {
    int lhs_callable =
        (lhs->kind == AST_IDENT_EXPR || lhs->kind == AST_CALL_EXPR ||
         lhs->kind == AST_MEMBER_EXPR);
    if (!lhs_callable) return NULL;

    if (p->pos + 1 < p->ts->count) {
      const Token *next = &p->ts->tokens[p->pos + 1];
      if (next->type == TOK_KEYWORD &&
          (next->keyword == KW_CASE || next->keyword == KW_DEFAULT))
        return NULL;
    }

    ASTNode *call = alloc_node(p, AST_CALL_EXPR);
    if (!call) return NULL;
    ast_add_child(call, lhs);
    ASTNode *tc = parse_closure_body(p);
    if (tc) ast_add_child(call, tc);

    /* Multi-trailing closures: label: { } */
    while (!p_is_eof(p)) {
      if (p_tok(p)->type != TOK_IDENTIFIER) break;
      size_t saved = p->pos;
      uint32_t extra_label_tok = (uint32_t)saved;
      adv(p);
      if (!P_COLON(p)) { p->pos = saved; break; }
      adv(p);
      if (!P_LBRACE(p)) { p->pos = saved; break; }
      ASTNode *extra_tc = parse_closure_body(p);
      if (extra_tc) {
        extra_tc->arg_label_tok = extra_label_tok;
        ast_add_child(call, extra_tc);
      }
    }

    call->tok_end = (uint32_t)p->pos;
    return call;
  }
  return NULL;
}

/**
 * @brief Chains postfix operations onto an expression in a loop.
 *
 * Tries each postfix form (.member, generic<T>(), call(), subscript[],
 * optional?, force!, trailing closure) in priority order.  Returns when
 * no postfix operator matches.
 */
ASTNode *parse_postfix(Parser *p, ASTNode *lhs) {
  if (!lhs) return NULL;

  while (!p_is_eof(p)) {
    ASTNode *node = NULL;

    if ((node = parse_postfix_member(p, lhs)))          { lhs = node; continue; }
    if ((node = parse_postfix_generic_call(p, lhs)))    { lhs = node; continue; }
    if ((node = parse_postfix_call(p, lhs)))            { lhs = node; continue; }
    if ((node = parse_postfix_subscript(p, lhs)))       { lhs = node; continue; }
    if ((node = parse_postfix_optional_chain(p, lhs)))  { lhs = node; continue; }
    if ((node = parse_postfix_force_unwrap(p, lhs)))    { lhs = node; continue; }
    if ((node = parse_postfix_trailing_closure(p, lhs))){ lhs = node; continue; }

    break;
  }

  return lhs;
}
