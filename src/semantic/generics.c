/**
 * @file generics.c
 * @brief Generic constraint checking: conformance, same-type, superclass,
 *        suppressed conformance (~Copyable), and conditional conformance.
 */
#include "private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Writes a type's name into @p buf.  Returns the name pointer. */
static const char *type_name_str(const TypeInfo *ty, char *buf, size_t sz) {
  if (ty->kind == TY_NAMED && ty->named.name)
    return ty->named.name;
  type_to_string(ty, buf, sz);
  return buf;
}

/**
 * @brief Checks conditional conformance for a generic instantiation.
 *
 * Given `Array<Int>` conforming to `Equatable`, verifies that `Element: Equatable`
 * is satisfied by checking if `Int` conforms to `Equatable`.
 *
 * Reads the where-clause AST from the conformance table, maps generic parameters
 * to concrete arguments, and checks each requirement.
 *
 * @return 1 if all conditional requirements are satisfied, 0 otherwise.
 */
static int check_conditional_conformance(const TypeInfo *concrete,
                                         const char *protocol_name,
                                         const ConformanceTable *ct,
                                         SemaContext *ctx,
                                         const ASTNode *site) {
  const char *nominal = concrete->generic.base->named.name;
  const void *where_ast = conformance_table_get_where(ct, nominal, protocol_name);
  if (!where_ast || !concrete->generic.args || !ctx)
    return 0;

  const ASTNode *wc = (const ASTNode *)where_ast;
  const ASTNode *decl = concrete->generic.base->named.decl;

  /* Collect generic parameter names from the type declaration. */
  const char *param_names[16];
  uint32_t param_count = 0;
  if (decl) {
    for (const ASTNode *c = decl->first_child; c && param_count < 16;
         c = c->next_sibling)
      if (c->kind == AST_GENERIC_PARAM && c->tok_idx != 0)
        param_names[param_count++] = tok_intern(ctx, c->tok_idx);
  }
  if (param_count > concrete->generic.arg_count)
    param_count = concrete->generic.arg_count;

  /* Check each where-clause requirement (e.g. Element: Equatable). */
  for (const ASTNode *req = wc->first_child; req; req = req->next_sibling) {
    if (req->kind != AST_BINARY_EXPR || !req->first_child)
      continue;
    const ASTNode *lhs = req->first_child;
    const ASTNode *rhs = lhs->next_sibling;
    if (!rhs) continue;

    TypeInfo *lhs_t = lhs->type ? lhs->type
                                : resolve_type_annotation(ctx, (ASTNode *)lhs);
    TypeInfo *rhs_t = rhs->type ? rhs->type
                                : resolve_type_annotation(ctx, (ASTNode *)rhs);

    if (req->data.binary.op_tok != 0 || !lhs_t || !rhs_t)
      continue;
    if (lhs_t->kind != TY_GENERIC_PARAM || rhs_t->kind != TY_NAMED || !rhs_t->named.name)
      continue;

    /* Map parameter name → concrete argument index. */
    const char *pname = lhs_t->param.name;
    uint32_t idx = param_count; /* sentinel: not found */
    for (uint32_t k = 0; k < param_count; k++)
      if (param_names[k] && strcmp(param_names[k], pname) == 0) { idx = k; break; }
    if (idx >= concrete->generic.arg_count)
      continue;

    /* Check: does the concrete argument conform to the required protocol? */
    TypeInfo *arg = concrete->generic.args[idx];
    char arg_buf[64];
    const char *arg_name = type_name_str(arg, arg_buf, sizeof(arg_buf));

    if (!arg_name || !conformance_table_has(ct, arg_name, rhs_t->named.name)) {
      if (ctx)
        sema_error(ctx, site,
                   "Conditional conformance: type '%s' does not conform "
                   "to protocol '%s'",
                   arg_name ? arg_name : "?", rhs_t->named.name);
      return 0;
    }
  }
  return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Single Constraint Check
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Checks TC_CONFORMANCE: concrete type must conform to a protocol. */
static int check_proto_conformance(const TypeInfo *concrete,
                             const TypeConstraint *constraint,
                             const ConformanceTable *ct,
                             SemaContext *ctx, const ASTNode *site) {
  if (!constraint->protocol_name || !ct)
    return 1;

  /* Generic instantiation (e.g. Array<Int>) — check conditional conformance. */
  if (concrete->kind == TY_GENERIC_INST && concrete->generic.base &&
      concrete->generic.base->kind == TY_NAMED &&
      concrete->generic.base->named.name) {
    if (check_conditional_conformance(concrete, constraint->protocol_name, ct, ctx, site))
      return 1;
    /* Fall through to check nominal conformance of the base type. */
    return conformance_table_has(ct, concrete->generic.base->named.name,
                                 constraint->protocol_name);
  }

  char buf[64];
  const char *type_name = type_name_str(concrete, buf, sizeof(buf));
  if (!type_name) return 1;

  int ok = conformance_table_has(ct, type_name, constraint->protocol_name);
  if (!ok && ctx)
    sema_error(ctx, site, "Type '%s' does not conform to protocol '%s'",
               type_name, constraint->protocol_name);
  return ok;
}

/** @brief Checks TC_SAME_TYPE: concrete == constraint.rhs_type. */
static int check_same_type(const TypeInfo *concrete,
                           const TypeConstraint *constraint,
                           SemaContext *ctx, const ASTNode *site) {
  if (!constraint->rhs_type) return 1;
  int ok = type_equal_deep(concrete, constraint->rhs_type);
  if (!ok && ctx) {
    char lhs_s[64], rhs_s[64];
    type_to_string(concrete, lhs_s, sizeof(lhs_s));
    type_to_string(constraint->rhs_type, rhs_s, sizeof(rhs_s));
    sema_error(ctx, site,
               "Same-type constraint not satisfied: expected '%s' == '%s'",
               lhs_s, rhs_s);
  }
  return ok;
}

/** @brief Checks TC_SUPERCLASS: concrete must be a subclass of the specified class. */
static int check_superclass(const TypeInfo *concrete,
                            const TypeConstraint *constraint,
                            SemaContext *ctx, const ASTNode *site) {
  if (!constraint->rhs_type) return 1;
  int ok = type_equal(concrete, constraint->rhs_type);
  if (!ok && ctx) {
    char lhs_s[64], rhs_s[64];
    type_to_string(concrete, lhs_s, sizeof(lhs_s));
    type_to_string(constraint->rhs_type, rhs_s, sizeof(rhs_s));
    sema_error(ctx, site,
               "Superclass constraint not satisfied: '%s' is not a subclass of '%s'",
               lhs_s, rhs_s);
  }
  return ok;
}

/**
 * @brief Checks whether a concrete type satisfies a single generic constraint.
 *
 * Dispatches to the appropriate checker based on constraint kind:
 *   - TC_CONFORMANCE      → protocol conformance (+ conditional conformance)
 *   - TC_SAME_TYPE        → structural type equality
 *   - TC_SUPERCLASS       → subclass relationship
 *   - TC_SUPPRESSED       → ~Copyable, always satisfied
 *   - TC_SAME_TYPE_ASSOC  → deferred (T.Item == U.Item)
 *
 * @param concrete    The concrete type to validate.
 * @param constraint  The constraint to check against.
 * @param ct          Conformance table (NULL limits checks to same-type only).
 * @param ctx         Sema context for error reporting (NULL = silent).
 * @param site        AST node for error location (NULL = generic).
 * @return            1 if satisfied, 0 if not.
 */
int check_constraint_satisfaction(const TypeInfo *concrete,
                                  const TypeConstraint *constraint,
                                  const ConformanceTable *ct, SemaContext *ctx,
                                  const ASTNode *site) {
  if (!concrete || !constraint)
    return 1;

  switch (constraint->kind) {
  case TC_CONFORMANCE:    return check_proto_conformance(concrete, constraint, ct, ctx, site);
  case TC_SAME_TYPE:      return check_same_type(concrete, constraint, ctx, site);
  case TC_SUPERCLASS:     return check_superclass(concrete, constraint, ctx, site);
  case TC_SUPPRESSED:     return 1; /* ~Copyable — no conformance required */
  case TC_SAME_TYPE_ASSOC: return 1; /* deferred check (T.Item == U.Item) */
  default:                return 1;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Multi-Argument Validation
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Validates that all concrete type arguments satisfy their parameter constraints.
 *
 * Iterates params[0..count-1] and checks each constraint against the
 * corresponding args[i].  Returns 1 only if every constraint passes.
 */
int check_generic_args(TypeInfo *const *params, uint32_t param_cnt,
                       TypeInfo *const *args, uint32_t arg_cnt,
                       const ConformanceTable *ct, SemaContext *ctx,
                       const ASTNode *site) {
  if (!params || !args)
    return 1;

  uint32_t count = param_cnt < arg_cnt ? param_cnt : arg_cnt;
  int all_ok = 1;

  for (uint32_t i = 0; i < count; i++) {
    const TypeInfo *param = params[i];
    const TypeInfo *arg = args[i];
    if (!param || param->kind != TY_GENERIC_PARAM || !arg)
      continue;
    for (uint32_t c = 0; c < param->param.constraint_count; c++) {
      if (!check_constraint_satisfaction(arg, &param->param.constraints[c],
                                         ct, ctx, site))
        all_ok = 0;
    }
  }

  return all_ok;
}
