/**
 * @file access.c
 * @brief Access control helpers: ranking, visibility, and effective access computation.
 *
 * Swift 5.9 `package` is ranked but no package boundary tracking is implemented yet.
 */
#include "../private.h"

/** @brief Returns an integer rank for access level (0=private .. 4=public/open). */
int access_rank(uint32_t mods) {
  uint32_t m = mods & ACCESS_MODIFIER_MASK;
  if (m & MOD_PRIVATE)
    return 0;
  if (m & MOD_FILEPRIVATE)
    return 1;
  if (m & MOD_INTERNAL)
    return 2;
  if (m & MOD_PACKAGE)
    return 3;
  if (m & MOD_PUBLIC || m & MOD_OPEN)
    return 4;
  return 2; /* no modifier → internal */
}
/** @brief Converts an access rank back to the corresponding MOD_* flag. */
uint32_t access_from_rank(int r) {
  if (r <= 0)
    return MOD_PRIVATE;
  if (r == 1)
    return MOD_FILEPRIVATE;
  if (r == 2)
    return MOD_INTERNAL;
  if (r == 3)
    return MOD_PACKAGE;
  return MOD_PUBLIC;
}
/** @brief Returns the more restrictive of two access levels. */
uint32_t access_min(const uint32_t a, const uint32_t b) {
  const int ra = access_rank(a);
  const int rb = access_rank(b);
  return access_from_rank(ra < rb ? ra : rb);
}

/** @brief Returns the interned name of a nominal type declaration. */
const char *nominal_type_name(SemaContext *ctx, const ASTNode *decl) {
  if (!decl || !decl->data.var.name_tok)
    return NULL;
  return tok_intern(ctx, decl->data.var.name_tok);
}

/** @brief Returns 1 if a private member is visible from the current type context. */
int private_member_visible(SemaContext *ctx, const ASTNode *member_decl,
                                const ASTNode *owning_type_decl) {
  if (!(member_decl->modifiers & ACCESS_MODIFIER_MASK))
    return 1;
  if (!(member_decl->modifiers & MOD_PRIVATE))
    return 1;
  const char *decl_ty = nominal_type_name(ctx, owning_type_decl);
  if (!ctx->current_type_name || !decl_ty)
    return 0;
  return ctx->current_type_name == decl_ty;
}

/**
 * @brief Computes the effective access level of a type.
 *
 * For composite types (tuples, functions, generics), returns the minimum
 * access of all constituent types.  For named types, reads the declaration's
 * access modifier.  Defaults to MOD_INTERNAL.
 */
uint32_t type_effective_access(SemaContext *ctx, TypeInfo *ty) {
  if (!ty)
    return MOD_INTERNAL;
  switch (ty->kind) {
  case TY_TUPLE: {
    uint32_t acc = MOD_PUBLIC;
    for (size_t i = 0; i < ty->tuple.elem_count && ty->tuple.elems; i++)
      acc = access_min(acc, type_effective_access(ctx, ty->tuple.elems[i]));
    return acc;
  }
  case TY_FUNC: {
    uint32_t acc =
        ty->func.ret ? type_effective_access(ctx, ty->func.ret) : MOD_INTERNAL;
    if (ty->func.params)
      for (size_t i = 0; i < ty->func.param_count; i++)
        acc = access_min(acc, type_effective_access(ctx, ty->func.params[i]));
    return acc;
  }
  case TY_NAMED: {
    if (ty->named.decl) {
      const ASTNode *decl = (const ASTNode *)ty->named.decl;
      uint32_t m = decl->modifiers & ACCESS_MODIFIER_MASK;
      if (m)
        return m;
    }
    return MOD_INTERNAL;
  }
  case TY_OPTIONAL:
  case TY_ARRAY:
  case TY_SET:
    return ty->inner ? type_effective_access(ctx, ty->inner) : MOD_INTERNAL;
  case TY_DICT:
    if (!ty->dict.key && !ty->dict.value)
      return MOD_INTERNAL;
    if (!ty->dict.key)
      return type_effective_access(ctx, ty->dict.value);
    if (!ty->dict.value)
      return type_effective_access(ctx, ty->dict.key);
    return access_min(type_effective_access(ctx, ty->dict.key),
                      type_effective_access(ctx, ty->dict.value));
  case TY_GENERIC_INST: {
    uint32_t acc = ty->generic.base
                       ? type_effective_access(ctx, ty->generic.base)
                       : MOD_INTERNAL;
    if (ty->generic.args)
      for (size_t i = 0; i < ty->generic.arg_count; i++)
        acc = access_min(acc, type_effective_access(ctx, ty->generic.args[i]));
    return acc;
  }
  case TY_GENERIC_PARAM: {
    uint32_t acc = MOD_INTERNAL;
    if (ty->param.constraints)
      for (uint32_t i = 0; i < ty->param.constraint_count; i++) {
        if (ty->param.constraints[i].kind != TC_CONFORMANCE ||
            !ty->param.constraints[i].protocol_name)
          continue;
        Symbol *ps = sema_lookup(ctx, ty->param.constraints[i].protocol_name);
        if (ps && ps->decl && ps->decl->kind == AST_PROTOCOL_DECL) {
          uint32_t m = ps->decl->modifiers & ACCESS_MODIFIER_MASK;
          acc = access_min(acc, m ? m : MOD_INTERNAL);
        }
      }
    return acc;
  }
  default:
    return MOD_INTERNAL;
  }
}
