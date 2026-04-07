/**
 * @file protocol.c
 * @brief Protocol requirement helpers: associated type checks, type AST
 *        matching, concrete type inference, and default implementation lookup.
 */

#include "../private.h"

/*
 * sema_protocol_helpers.h — Protocol requirement helpers: protocol_req_*,
 * type_ast_*, infer_concrete_at_assoc, protocol_extension_has_default. Include
 * after sema_type_resolution.h (uses find_type_child). Used by
 * sema_resolve_node.h (check_conformance).
 */

/*
 * True if protocol requirement is an associated type (associatedtype /
 * typealias)
 */
int protocol_req_is_associated_type(const ASTNode *req) {
  return req && (req->modifiers & MOD_PROTOCOL_ASSOC_TYPE);
}

/*
 * true if protocol requirement is a property (var x: Type) not
 * method/init/associatedtype
 */
int protocol_req_is_property(const ASTNode *req, const char *req_name) {
  if (!req_name || !strcmp(req_name, "init"))
    return 0;
  if (protocol_req_is_associated_type(req))
    return 0;
  /* bit23 is set by parser for `func` requirements — never a property */
  if (req->modifiers & (1u << 23))
    return 0;
  const ASTNode *first_ty = find_type_child(req);
  return (first_ty && req->first_child == first_ty);
}

/*
 * return the return-type node of a protocol method requirement (last
 * TYPE_* direct child)
 */
const ASTNode *protocol_req_return_type_node(const ASTNode *req) {
  if (!req)
    return NULL;
  const ASTNode *last_ty = NULL;
  for (const ASTNode *c = req->first_child; c; c = c->next_sibling) {
    if (c->kind >= AST_TYPE_IDENT &&
        (c->kind <= AST_TYPE_ANY || c->kind == AST_TYPE_COMPOSITION))
      last_ty = c;
  }
  return last_ty;
}

/* get identifier name from a type AST node (TYPE_IDENT) */
const char *type_ast_ident_name(const ASTNode *n, SemaContext *ctx) {
  if (!n || n->kind != AST_TYPE_IDENT || n->tok_idx == 0 || !ctx)
    return NULL;
  const Token *t = &ctx->tokens[n->tok_idx];
  return sema_intern(ctx, ctx->src->data + t->pos, t->len);
}

/*
 * true if protocol requirement type AST contains TYPE_IDENT with name
 * assoc_name
 */
int type_ast_contains_assoc(const ASTNode *type_ast, SemaContext *ctx,
                            const char *assoc_name) {
  if (!type_ast || !assoc_name)
    return 0;
  if (type_ast->kind == AST_TYPE_IDENT) {
    const char *name = type_ast_ident_name(type_ast, ctx);
    return (name && strcmp(name, assoc_name) == 0);
  }
  for (const ASTNode *c = type_ast->first_child; c; c = c->next_sibling)
    if (type_ast_contains_assoc(c, ctx, assoc_name))
      return 1;
  return 0;
}

/*
 * match protocol type AST with impl TypeInfo; return concrete TypeInfo at
 * assoc position
 */
TypeInfo *infer_concrete_at_assoc(const ASTNode *proto_ast, TypeInfo *impl_ty,
                                  const char *assoc_name, SemaContext *ctx) {
  if (!proto_ast || !impl_ty || !assoc_name)
    return NULL;
  if (proto_ast->kind == AST_TYPE_IDENT) {
    const char *name = type_ast_ident_name(proto_ast, ctx);
    if (name && strcmp(name, assoc_name) == 0)
      return impl_ty;
    return NULL;
  }
  if (proto_ast->kind == AST_TYPE_ARRAY && impl_ty->kind == TY_ARRAY &&
      impl_ty->inner)
    return infer_concrete_at_assoc(proto_ast->first_child, impl_ty->inner,
                                   assoc_name, ctx);
  if (proto_ast->kind == AST_TYPE_OPTIONAL && impl_ty->kind == TY_OPTIONAL &&
      impl_ty->inner)
    return infer_concrete_at_assoc(proto_ast->first_child, impl_ty->inner,
                                   assoc_name, ctx);
  if (proto_ast->kind == AST_TYPE_GENERIC && impl_ty->kind == TY_GENERIC_INST &&
      impl_ty->generic.args && impl_ty->generic.arg_count > 0 &&
      proto_ast->first_child)
    return infer_concrete_at_assoc(proto_ast->first_child,
                                   impl_ty->generic.args[0], assoc_name, ctx);
  if (proto_ast->kind == AST_TYPE_DICT && impl_ty->kind == TY_DICT &&
      impl_ty->dict.key && impl_ty->dict.value && proto_ast->first_child) {
    TypeInfo *k = infer_concrete_at_assoc(proto_ast->first_child,
                                          impl_ty->dict.key, assoc_name, ctx);
    if (k)
      return k;
    const ASTNode *val_ast = proto_ast->first_child->next_sibling;
    if (val_ast)
      return infer_concrete_at_assoc(val_ast, impl_ty->dict.value, assoc_name,
                                     ctx);
  }
  return NULL;
}

/*
 * Ch23 / Protocol default implementation: extension P { func f() { } } — does
 * protocol proto_name have an extension providing req_name
 * (method/var/init/subscript)?
 */
int protocol_extension_has_default(SemaContext *ctx, const ASTNode *root,
                                   const char *proto_name,
                                   const char *req_name) {
  if (!ctx || !root || !proto_name || !req_name)
    return 0;
  for (const ASTNode *top = root->first_child; top; top = top->next_sibling) {
    if (top->kind != AST_EXTENSION_DECL || !top->data.var.name_tok)
      continue;
    const Token *et = &ctx->tokens[top->data.var.name_tok];
    const char *ext_name = sema_intern(ctx, ctx->src->data + et->pos, et->len);
    if (strcmp(ext_name, proto_name) != 0)
      continue;
    const Symbol *sym = sema_lookup(ctx, ext_name);
    if (!sym || !sym->decl ||
        ((const ASTNode *)sym->decl)->kind != AST_PROTOCOL_DECL)
      continue;
    const ASTNode *block = NULL;
    for (const ASTNode *c = top->first_child; c; c = c->next_sibling)
      if (c->kind == AST_BLOCK) {
        block = c;
        break;
      }
    if (!block)
      continue;
    for (const ASTNode *m = block->first_child; m; m = m->next_sibling) {
      if (m->kind == AST_FUNC_DECL) {
        const Token *mt = &ctx->tokens[m->data.func.name_tok];
        const char *mn = sema_intern(ctx, ctx->src->data + mt->pos, mt->len);
        if (mn && strcmp(mn, req_name) == 0)
          return 1;
      } else if (m->kind == AST_VAR_DECL || m->kind == AST_LET_DECL) {
        const Token *mt = &ctx->tokens[m->data.var.name_tok];
        const char *mn = sema_intern(ctx, ctx->src->data + mt->pos, mt->len);
        if (mn && strcmp(mn, req_name) == 0)
          return 1;
      } else if (m->kind == AST_INIT_DECL && strcmp(req_name, "init") == 0)
        return 1;
      else if (m->kind == AST_SUBSCRIPT_DECL &&
               strcmp(req_name, "subscript") == 0)
        return 1;
    }
  }
  return 0;
}
