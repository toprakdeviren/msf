/**
 * @file helpers.c
 * @brief Expression resolution helpers: resolve_children, wrap_optional_result,
 *        contextual type inference for implicit members.
 */
#include "../../private.h"

/*
 * resolution. Include after sema_resolve_access.h. Followed by
 * class_has_init_with_param_count, is_inherited_stored_property in
 * sema_resolve_decl.h.
 */

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
  return NULL;
}

TypeInfo *resolve_binary_expr(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_member_expr(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_call_expr(SemaContext *ctx, ASTNode *node);
