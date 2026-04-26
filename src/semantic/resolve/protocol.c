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
 * External (call-site) label of a parameter. Returns the interned label
 * string, or NULL if the parameter is explicitly omitted (`_`).
 *
 * The parser stores the *internal* name in `data.var.name_tok` and discards
 * the external label, so we recover it by inspecting the token immediately
 * before it. The pre-name token is one of:
 *   '_' operator     → label is omitted (return NULL, *out_omitted = 1)
 *   identifier       → that identifier is the external label
 *   anything else    → no separate label; internal name doubles as label
 */
const char *param_external_label_str(SemaContext *ctx, const ASTNode *param,
                                     int *out_omitted) {
  if (out_omitted)
    *out_omitted = 0;
  if (!param || !param->data.var.name_tok)
    return NULL;
  uint32_t nt = param->data.var.name_tok;
  if (nt >= 1) {
    const Token *prev = &ctx->tokens[nt - 1];
    if (prev->type == TOK_OPERATOR && prev->len == 1 &&
        ctx->src->data[prev->pos] == '_') {
      if (out_omitted)
        *out_omitted = 1;
      return NULL;
    }
    if (prev->type == TOK_IDENTIFIER)
      return sema_intern(ctx, ctx->src->data + prev->pos, prev->len);
  }
  const Token *t = &ctx->tokens[nt];
  return sema_intern(ctx, ctx->src->data + t->pos, t->len);
}

/* Returns the return-type AST node of a func/init AST decl, or NULL. */
static const ASTNode *func_decl_return_type_node(const ASTNode *decl) {
  if (!decl)
    return NULL;
  const ASTNode *last_ty = NULL;
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
    if (c->kind >= AST_TYPE_IDENT &&
        (c->kind <= AST_TYPE_ANY || c->kind == AST_TYPE_COMPOSITION))
      last_ty = c;
  }
  return last_ty;
}

/* Format a function signature for diagnostics:
 *   "func name(label: T, label2: T2) -> Ret"
 * Reads the AST_PARAM children (for labels + type AST) and the explicit
 * return type AST node. Used only for error messages — keep it permissive. */
static void format_func_signature(SemaContext *ctx, const ASTNode *decl,
                                  const char *fname, char *buf, size_t bufsz) {
  size_t off = 0;
  int n;
  n = snprintf(buf + off, bufsz - off, "func %s(", fname ? fname : "?");
  if (n < 0 || (size_t)n >= bufsz - off) return;
  off += (size_t)n;
  int first = 1;
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_PARAM) continue;
    int omitted = 0;
    const char *label = param_external_label_str(ctx, c, &omitted);
    const ASTNode *ty_n = find_type_child(c);
    char ty_s[64] = "?";
    if (ty_n) {
      TypeInfo *ti = resolve_type_annotation(ctx, (ASTNode *)ty_n);
      if (ti) type_to_string(ti, ty_s, sizeof(ty_s));
    }
    n = snprintf(buf + off, bufsz - off, "%s%s: %s",
                 first ? "" : ", ", omitted ? "_" : (label ? label : "_"),
                 ty_s);
    if (n < 0 || (size_t)n >= bufsz - off) return;
    off += (size_t)n;
    first = 0;
  }
  const ASTNode *ret_n = func_decl_return_type_node(decl);
  char ret_s[64] = "Void";
  if (ret_n) {
    TypeInfo *ti = resolve_type_annotation(ctx, (ASTNode *)ret_n);
    if (ti) type_to_string(ti, ret_s, sizeof(ret_s));
  }
  snprintf(buf + off, bufsz - off, ") -> %s", ret_s);
}

/* True if `t` references something we cannot reliably compare against a
 * concrete impl type — generic params, associated-type refs, or unresolved
 * names. We skip strict signature matching when either side is one of these
 * to avoid false positives until full generic substitution is wired up. */
static int type_is_unresolved_for_witness(const TypeInfo *t) {
  if (!t) return 1;
  switch (t->kind) {
  case TY_GENERIC_PARAM:
  case TY_ASSOC_REF:
  case TY_ERROR:
    return 1;
  case TY_OPTIONAL:
  case TY_ARRAY:
  case TY_SET:
    return type_is_unresolved_for_witness(t->inner);
  case TY_DICT:
    return type_is_unresolved_for_witness(t->dict.key) ||
           type_is_unresolved_for_witness(t->dict.value);
  case TY_FUNC: {
    if (type_is_unresolved_for_witness(t->func.ret))
      return 1;
    for (size_t i = 0; i < t->func.param_count; i++)
      if (t->func.params && type_is_unresolved_for_witness(t->func.params[i]))
        return 1;
    return 0;
  }
  default:
    return 0;
  }
}

/* Validate that an implementation func/init satisfies the protocol
 * requirement's signature: parameter count, external labels, parameter
 * types, and return type. Emits a diagnostic on the first mismatch and
 * returns 0; returns 1 on full match. */
int validate_witness_func_signature(SemaContext *ctx, const ASTNode *type_decl,
                                    const ASTNode *proto_decl,
                                    const ASTNode *req, const ASTNode *impl,
                                    const char *type_name,
                                    const char *proto_name,
                                    const char *req_name) {
  if (!ctx || !req || !impl) return 1;

  uint32_t req_pc = 0, impl_pc = 0;
  for (const ASTNode *c = req->first_child; c; c = c->next_sibling)
    if (c->kind == AST_PARAM) req_pc++;
  for (const ASTNode *c = impl->first_child; c; c = c->next_sibling)
    if (c->kind == AST_PARAM) impl_pc++;

  char want_buf[256], got_buf[256];
  format_func_signature(ctx, req, req_name, want_buf, sizeof(want_buf));
  format_func_signature(ctx, impl, req_name, got_buf, sizeof(got_buf));

  if (req_pc != impl_pc) {
    sema_error(ctx, (ASTNode *)impl,
               "type '%s' does not conform to protocol '%s': expected '%s', "
               "found '%s'",
               type_name, proto_name, want_buf, got_buf);
    return 0;
  }

  /* Per-parameter: external label + resolved type */
  const ASTNode *rp = req->first_child;
  const ASTNode *ip = impl->first_child;
  while (rp || ip) {
    while (rp && rp->kind != AST_PARAM) rp = rp->next_sibling;
    while (ip && ip->kind != AST_PARAM) ip = ip->next_sibling;
    if (!rp || !ip) break;

    int rp_omitted = 0, ip_omitted = 0;
    const char *r_label = param_external_label_str(ctx, rp, &rp_omitted);
    const char *i_label = param_external_label_str(ctx, ip, &ip_omitted);
    int label_mismatch =
        (rp_omitted != ip_omitted) ||
        (!rp_omitted && !ip_omitted &&
         ((r_label == NULL) != (i_label == NULL) ||
          (r_label && i_label && strcmp(r_label, i_label) != 0)));
    if (label_mismatch) {
      sema_error(ctx, (ASTNode *)impl,
                 "type '%s' does not conform to protocol '%s': expected '%s', "
                 "found '%s' (parameter label mismatch)",
                 type_name, proto_name, want_buf, got_buf);
      return 0;
    }

    const ASTNode *r_ty_n = find_type_child(rp);
    const ASTNode *i_ty_n = find_type_child(ip);
    TypeInfo *r_ty = r_ty_n ? resolve_type_annotation(ctx, (ASTNode *)r_ty_n) : NULL;
    TypeInfo *i_ty = i_ty_n ? resolve_type_annotation(ctx, (ASTNode *)i_ty_n) : NULL;
    if (!type_is_unresolved_for_witness(r_ty) &&
        !type_is_unresolved_for_witness(i_ty) &&
        !type_equal(r_ty, i_ty)) {
      sema_error(ctx, (ASTNode *)impl,
                 "type '%s' does not conform to protocol '%s': expected '%s', "
                 "found '%s'",
                 type_name, proto_name, want_buf, got_buf);
      return 0;
    }

    rp = rp->next_sibling;
    ip = ip->next_sibling;
  }

  /* Return type */
  if (impl->kind == AST_FUNC_DECL) {
    const ASTNode *r_ret_n = func_decl_return_type_node(req);
    const ASTNode *i_ret_n = func_decl_return_type_node(impl);
    TypeInfo *r_ret = r_ret_n ? resolve_type_annotation(ctx, (ASTNode *)r_ret_n)
                              : TY_BUILTIN_VOID;
    TypeInfo *i_ret = i_ret_n ? resolve_type_annotation(ctx, (ASTNode *)i_ret_n)
                              : TY_BUILTIN_VOID;
    /* A protocol func with `-> some P` is handled separately as opaque
     * return; skip strict equality there. */
    int req_is_opaque = (r_ret_n && r_ret_n->kind == AST_TYPE_SOME);
    if (!req_is_opaque &&
        !type_is_unresolved_for_witness(r_ret) &&
        !type_is_unresolved_for_witness(i_ret) &&
        !type_equal(r_ret, i_ret)) {
      sema_error(ctx, (ASTNode *)impl,
                 "type '%s' does not conform to protocol '%s': expected '%s', "
                 "found '%s'",
                 type_name, proto_name, want_buf, got_buf);
      return 0;
    }
  }
  (void)proto_decl;
  (void)type_decl;
  return 1;
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
