/**
 * @file helpers.c
 * @brief Expression resolution helpers: resolve_children, wrap_optional_result,
 *        contextual type inference for implicit members.
 */
#include "../../private.h"

TypeInfo *resolve_node(SemaContext *ctx, ASTNode *node);
int class_has_init_with_param_count(SemaContext *ctx, const ASTNode *class_decl,
                                    uint32_t param_count);
int is_inherited_stored_property(SemaContext *ctx, const ASTNode *class_decl,
                                 const char *prop_name);

TypeInfo *resolve_children(SemaContext *ctx, ASTNode *node) {
  TypeInfo *last = NULL;
  for (ASTNode *c = node->first_child; c; c = c->next_sibling)
    last = resolve_node(ctx, c);
  return last;
}

/* ─── Helper: type category checks ──────────────────────────────────────────── */
TypeInfo *wrap_optional_result(TypeInfo *t, int do_wrap, SemaContext *ctx) {
  if (!do_wrap || !t)
    return t;
  if (t->kind == TY_OPTIONAL)
    return t;
  TypeInfo *opt = type_arena_alloc(ctx->type_arena);
  opt->kind = TY_OPTIONAL;
  opt->inner = t;
  return opt;
}

int is_lhs_optional_chain(const ASTNode *expr) {
  if (!expr)
    return 0;
  if (expr->kind == AST_MEMBER_EXPR && expr->first_child &&
      expr->first_child->kind == AST_OPTIONAL_CHAIN)
    return 1;
  if (expr->kind == AST_SUBSCRIPT_EXPR && expr->first_child &&
      expr->first_child->kind == AST_OPTIONAL_CHAIN)
    return 1;
  return 0;
}

/* Implicit member (.foo): get contextual type from parent when base is missing. */
TypeInfo *get_contextual_type_for_implicit_member(SemaContext *ctx,
                                                  const ASTNode *node) {
  const ASTNode *p = node->parent;
  if (!p)
    return NULL;
  if (p->kind == AST_VAR_DECL || p->kind == AST_LET_DECL) {
    /* let x: Color = .red  — contextual type is the variable's type */
    for (const ASTNode *c = p->first_child; c; c = c->next_sibling)
      if (c == node)
        return p->type;
    return p->type;
  }
  if (p->kind == AST_ASSIGN_EXPR ||
      (p->kind == AST_BINARY_EXPR && p->data.binary.op_tok)) {
    const ASTNode *lhs = p->first_child;
    if (lhs && lhs != node && lhs->next_sibling == node) {
      TypeInfo *lt = lhs->type ? lhs->type : NULL;
      if (!lt && lhs->kind == AST_IDENT_EXPR) {
        const char *n = tok_intern(ctx, lhs->tok_idx);
        Symbol *sym = sema_lookup(ctx, n);
        if (sym && sym->type)
          lt = sym->type;
      }
      return lt;
    }
  }
  if (p->kind == AST_RETURN_STMT && p->first_child == node) {
    if (ctx->current_func_decl) {
      ASTNode *fn = (ASTNode *)ctx->current_func_decl;
      return fn->type ? fn->type->kind == TY_FUNC ? fn->type->func.ret : NULL
                      : NULL;
    }
  }
  if (p->kind == AST_CALL_EXPR) {
    /* foo(.case) / obj.m(.case) — contextual type is the callee's parameter
       type at this argument's position. The callee reference's TY_FUNC carries
       no params (param_count==0); param types live on the decl, so resolve the
       callee decl and read its AST_PARAM at this index. (Initializers and
       memberwise structs are handled in irgen.) */
    const ASTNode *callee = p->first_child;
    if (callee && callee != node) {
      uint32_t idx = 0;
      int found = 0;
      for (const ASTNode *a = callee->next_sibling; a;
           a = a->next_sibling, idx++) {
        if (a == node) {
          found = 1;
          break;
        }
      }
      const ASTNode *cdecl = NULL;
      if (found && callee->kind == AST_IDENT_EXPR) {
        const char *cn = tok_intern(ctx, callee->tok_idx);
        Symbol *s = cn ? sema_lookup(ctx, cn) : NULL;
        if (s && s->decl && s->decl->kind == AST_FUNC_DECL)
          cdecl = s->decl;
      } else if (found && callee->kind == AST_MEMBER_EXPR) {
        const ASTNode *base = callee->first_child;
        TypeInfo *bt = base ? base->type : NULL;
        if (bt && bt->kind == TY_OPTIONAL)
          bt = bt->inner;
        const char *mn = callee->data.var.name_tok
                             ? tok_intern(ctx, callee->data.var.name_tok)
                             : NULL;
        if (bt && bt->kind == TY_NAMED && mn) {
          const ASTNode *tdecl = (const ASTNode *)bt->named.decl;
          if (!tdecl && bt->named.name) {
            Symbol *ts = sema_lookup(ctx, bt->named.name);
            if (ts && ts->decl)
              tdecl = ts->decl;
          }
          const ASTNode *body = tdecl ? class_decl_body(tdecl) : NULL;
          for (const ASTNode *ch = body ? body->first_child : NULL; ch;
               ch = ch->next_sibling)
            if (ch->kind == AST_FUNC_DECL &&
                tok_intern(ctx, ch->data.func.name_tok) == mn) {
              cdecl = ch;
              break;
            }
        }
      }
      if (cdecl) {
        uint32_t pi = 0;
        for (const ASTNode *pp = cdecl->first_child; pp; pp = pp->next_sibling) {
          if (pp->kind != AST_PARAM)
            continue;
          if (pi == idx)
            return pp->type;
          pi++;
        }
      }
    }
  }
  return NULL;
}

TypeInfo *resolve_binary_expr(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_member_expr(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_call_expr(SemaContext *ctx, ASTNode *node);
