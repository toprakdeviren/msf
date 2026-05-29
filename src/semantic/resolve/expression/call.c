/**
 * @file call.c
 * @brief Call expression resolution: callee(args), overload resolution,
 *        init delegation, generic constraint checking.
 */

#include "../../private.h"
#include <limits.h>

static TypeInfo *make_named_foundation_call_type(SemaContext *ctx,
                                                 const char *name) {
  TypeInfo *t = type_arena_alloc(ctx->type_arena);
  if (!t)
    return TY_BUILTIN_INT;
  t->kind = TY_NAMED;
  t->named.name = name;
  t->named.decl = NULL;
  return t;
}

static TypeInfo *make_default_array_call_type(SemaContext *ctx) {
  TypeInfo *t = type_arena_alloc(ctx->type_arena);
  if (!t)
    return TY_BUILTIN_INT;
  t->kind = TY_ARRAY;
  t->inner = TY_BUILTIN_INT;
  return t;
}

static TypeInfo *make_array_call_type(SemaContext *ctx, TypeInfo *elem) {
  TypeInfo *t = type_arena_alloc(ctx->type_arena);
  if (!t)
    return TY_BUILTIN_INT;
  t->kind = TY_ARRAY;
  t->inner = elem ? elem : TY_BUILTIN_INT;
  return t;
}

static TypeInfo *make_default_dict_call_type(SemaContext *ctx) {
  TypeInfo *t = type_arena_alloc(ctx->type_arena);
  if (!t)
    return TY_BUILTIN_INT;
  t->kind = TY_DICT;
  t->dict.key = TY_BUILTIN_STRING;
  t->dict.value = TY_BUILTIN_INT;
  return t;
}

static TypeInfo *foundation_contextual_result_type(SemaContext *ctx,
                                                   const ASTNode *node) {
  const ASTNode *p = node ? node->parent : NULL;
  if (p && (p->kind == AST_VAR_DECL || p->kind == AST_LET_DECL)) {
    const ASTNode *init = find_init_child(p);
    const ASTNode *ann = find_type_child(p);
    if (init == node && ann)
      return resolve_type_annotation(ctx, ann);
  }
  if (p && p->kind == AST_CAST_EXPR) {
    const ASTNode *ty = find_type_child(p);
    if (ty)
      return resolve_type_annotation(ctx, ty);
  }
  return NULL;
}

static TypeInfo *foundation_unwrap_optional_type(TypeInfo *t) {
  return (t && t->kind == TY_OPTIONAL && t->inner) ? t->inner : t;
}

static const char *range_family_type_name(TypeInfo *ty) {
  if (!ty)
    return NULL;
  if (ty->kind == TY_NAMED)
    return ty->named.name;
  if (ty->kind == TY_GENERIC_INST && ty->generic.base &&
      ty->generic.base->kind == TY_NAMED)
    return ty->generic.base->named.name;
  return NULL;
}

static int is_range_family_type(TypeInfo *ty) {
  const char *name = range_family_type_name(ty);
  return name &&
         (strcmp(name, "Range") == 0 ||
          strcmp(name, "ClosedRange") == 0 ||
          strcmp(name, "PartialRangeFrom") == 0 ||
          strcmp(name, "PartialRangeThrough") == 0 ||
          strcmp(name, "PartialRangeUpTo") == 0 ||
          strcmp(name, "UnboundedRange") == 0);
}

static int is_rangeset_type(TypeInfo *ty) {
  const char *name = range_family_type_name(ty);
  return name && strcmp(name, "RangeSet") == 0;
}

/* ── Overload scoring ──────────────────────────────────────────────────────
 * Scores a single (call site, candidate) pair. Lower score = better fit.
 * Returns -1 to mean "this candidate is not a match at all" (eliminated).
 *
 * Cost model:
 *   exact type match                          : 0
 *   integer literal arg → Float/Double param  : 1   (ExpressibleByIntegerLiteral)
 *   each default-arg slot left unfilled        : 1
 *   anything else                              : eliminated
 *
 * Label rules:
 *   param has explicit `_`                    : arg must have NO label
 *   param has external label "L"              : arg label must be "L" (or
 *                                                arg has no label, treated
 *                                                as positional in Swift —
 *                                                eliminated for safety here)
 *   param's internal name doubles as label    : arg label must equal it OR
 *                                                arg has no label
 */
#define OVERLOAD_NO_MATCH (-1)

/* Returns 1 if the param's element type matches the arg with at most a literal
 * coercion (an integer/float/etc. literal whose ExpressibleBy* protocol the
 * param type conforms to, e.g. Int literal → CGFloat). Updates *score. */
static int score_arg_against(SemaContext *ctx, TypeInfo *p_ty,
                             const ASTNode *arg, int *score) {
  TypeInfo *a_ty = arg->type;
  if (!p_ty || !a_ty) {
    *score += 1;
    return 1;
  }
  if (type_equal(p_ty, a_ty)) return 1;
  if (literal_coerces_to(ctx, arg, p_ty)) {
    *score += 1;
    return 1;
  }
  return 0;
}

/* Arity compatibility only (param count vs argc, accounting for defaults and
 * variadics) — ignores argument types. Used by score_overload_candidate and as
 * a forgiving fallback: when type scoring eliminates every candidate but
 * exactly one is arity-compatible, that one is picked (mirrors the lenient
 * single-overload path, so imperfect generic/nominal type scoring — e.g. a
 * `Range<Int>` arg — doesn't produce a spurious "no matching overload"). */
static int overload_arity_ok(const ASTNode *decl, uint32_t argc) {
  uint32_t param_count = 0, with_default = 0;
  int has_variadic = 0;
  for (const ASTNode *p = decl->first_child; p; p = p->next_sibling) {
    if (p->kind != AST_PARAM) continue;
    param_count++;
    if (p->modifiers & MOD_VARIADIC) has_variadic = 1;
    const ASTNode *ty_n = find_type_child(p);
    int has_default = 0;
    for (const ASTNode *c = p->first_child; c; c = c->next_sibling) {
      if (c == ty_n) continue;
      if (c->kind == AST_OWNERSHIP_SPEC) continue;
      has_default = 1;
      break;
    }
    if (has_default) with_default++;
  }
  if (!has_variadic && argc > param_count) return 0;
  if (argc + with_default < param_count) {
    if (!has_variadic || argc + with_default + 1 < param_count) return 0;
  }
  return 1;
}

/* A generic function (has `<T>` parameters). The arity fallback only applies to
 * these — for concrete-typed overloads the scorer is precise, so a type
 * mismatch (e.g. a variadic called with a wrong element type) is a real
 * non-match and must NOT be rescued by arity. */
static int decl_has_generic_params(const ASTNode *decl) {
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling)
    if (c->kind == AST_GENERIC_PARAM) return 1;
  return 0;
}

static int score_overload_candidate(SemaContext *ctx, const ASTNode *decl,
                                    ASTNode **args, uint32_t argc) {
  if (!overload_arity_ok(decl, argc)) return OVERLOAD_NO_MATCH;

  int score = 0;
  uint32_t arg_idx = 0;
  for (const ASTNode *p = decl->first_child; p; p = p->next_sibling) {
    if (p->kind != AST_PARAM) continue;

    if (p->modifiers & MOD_VARIADIC) {
      /* Match the remaining args against this param's element type. Each
       * extra arg is +0 cost (exact) or +1 (literal widening) so a
       * non-variadic same-typed candidate still wins ties. */
      while (arg_idx < argc) {
        const ASTNode *arg = args[arg_idx];
        if (arg->arg_label_tok) {
          /* call-site labels not allowed for individual variadic args */
          return OVERLOAD_NO_MATCH;
        }
        if (!score_arg_against(ctx, p->type, arg, &score))
          return OVERLOAD_NO_MATCH;
        arg_idx++;
      }
      break;
    }

    if (arg_idx >= argc) {
      score += 1; /* default-filled slot */
      continue;
    }

    int p_omitted = 0;
    const char *p_label = param_external_label_str(ctx, p, &p_omitted);
    const ASTNode *arg = args[arg_idx];
    const char *arg_label = NULL;
    if (arg->arg_label_tok) {
      const Token *t = &ctx->tokens[arg->arg_label_tok];
      arg_label = sema_intern(ctx, ctx->src->data + t->pos, t->len);
    }
    if (p_omitted) {
      if (arg_label) return OVERLOAD_NO_MATCH;
    } else if (p_label) {
      if (arg_label && strcmp(arg_label, p_label) != 0)
        return OVERLOAD_NO_MATCH;
      if (!arg_label) return OVERLOAD_NO_MATCH;
    }

    if (!score_arg_against(ctx, p->type, arg, &score))
      return OVERLOAD_NO_MATCH;
    arg_idx++;
  }
  return score;
}

/* Marker on a CALL_EXPR node: resolution was attempted and failed (no type).
 * Bit 30 — well clear of the decl MOD_* flags (<= bit 23); call nodes don't
 * carry declaration modifiers. Lets us memoize an unresolved call without
 * setting node->type (which must stay NULL to mean "unresolved"). */
#define MOD_CALL_RESOLVE_FAILED (1u << 30)

TypeInfo *resolve_call_expr(SemaContext *ctx, ASTNode *node) {
  ASTNode *callee = node->first_child;
  int is_delegation = 0;
  if (node->modifiers & MOD_CALL_RESOLVE_FAILED)
    return NULL; /* already attempted; do not re-resolve (avoids O(2^n)) */

  if (callee && callee->kind == AST_MEMBER_EXPR) {
    ASTNode *base = callee->first_child;
    const char *base_name = (base && base->kind == AST_IDENT_EXPR)
                                ? tok_intern(ctx, base->tok_idx)
                                : NULL;
    const char *mname = (callee->data.var.name_tok != 0)
                            ? tok_intern(ctx, callee->data.var.name_tok)
                        : (base && base->next_sibling)
                            ? tok_intern(ctx, base->next_sibling->tok_idx)
                            : NULL;
    if (base_name && mname) {
      if (strcmp(mname, "init") == 0 &&
          (strcmp(base_name, "super") == 0 || strcmp(base_name, "self") == 0))
        is_delegation = 1;
      else if (ctx->in_class_init_phase1 && strcmp(base_name, "self") == 0)
        sema_error(ctx, node,
                   "Cannot call instance method before 'super.init()' "
                   "(two-phase initialization)");
    }
    TypeInfo *base_t = base ? base->type : NULL;
    if (!base_t && base)
      base_t = resolve_node(ctx, base);
    if (base_t && mname) {
      TypeInfo *unwrapped = base_t;
      if (unwrapped->kind == TY_OPTIONAL)
        unwrapped = unwrapped->inner;
      if (type_is_value_type(ctx, unwrapped)) {
        const ASTNode *root = root_ident_of_expr(base);
        if (root && root->kind == AST_IDENT_EXPR) {
          const char *rname = tok_intern(ctx, root->tok_idx);
          const Symbol *sym = sema_lookup(ctx, rname);
          const ASTNode *tdecl = named_type_decl(ctx, unwrapped);
          if (!tdecl && unwrapped->named.name) {
            const Symbol *tsym = sema_lookup(ctx, unwrapped->named.name);
            if (tsym && tsym->decl)
              tdecl = tsym->decl;
          }
          if (sym && sym->kind == SYM_LET && tdecl &&
              method_is_mutating(ctx, tdecl, mname)) {
            sema_error(ctx, node,
                       "Cannot call mutating method on 'let' constant");
          }
        }
      }
    }
  }

  TypeInfo *callee_t = resolve_node(ctx, callee);

  uint32_t arg_idx = 0;
  for (ASTNode *a = callee ? callee->next_sibling : NULL; a;
       a = a->next_sibling, arg_idx++) {
    ctx->expected_closure_type = NULL;
    ctx->requires_explicit_self = 0;
    if (callee_t && callee_t->kind == TY_FUNC && callee_t->func.params &&
        arg_idx < callee_t->func.param_count &&
        callee_t->func.params[arg_idx] &&
        callee_t->func.params[arg_idx]->kind == TY_FUNC) {
      ctx->expected_closure_type = callee_t->func.params[arg_idx];
      ctx->requires_explicit_self =
          ctx->expected_closure_type->func.escaping ? 1 : 0;
    }
    if (a->kind == AST_CLOSURE_EXPR && callee &&
        callee->kind == AST_MEMBER_EXPR) {
      const ASTNode *base = callee->first_child;
      if (base && base->type &&
          (base->type == TY_BUILTIN_STRING || base->type->kind == TY_STRING)) {
        const char *mname = NULL;
        if (callee->data.var.name_tok)
          mname = tok_intern(ctx, callee->data.var.name_tok);
        if (mname &&
            (strcmp(mname, "map") == 0 || strcmp(mname, "filter") == 0 ||
             strcmp(mname, "compactMap") == 0) &&
            arg_idx == 0) {
          TypeInfo *expected_ty = type_arena_alloc(ctx->type_arena);
          expected_ty->kind = TY_FUNC;
          expected_ty->func.param_count = 1;
          TypeInfo **params = malloc(sizeof(TypeInfo *));
          params[0] = TY_BUILTIN_STRING;
          expected_ty->func.params = params;
          expected_ty->func.ret = TY_BUILTIN_VOID;
          ctx->expected_closure_type = expected_ty;
        }
      }
    }
    resolve_node(ctx, a);
    ctx->expected_closure_type = NULL;
    ctx->requires_explicit_self = 0;
  }

  /*
   * Overload resolution: when callee is a bare name, pick the best matching
   * overload by scoring each candidate. Lowest score wins; tied lowest scores
   * → ambiguity diagnostic.
   */
  if (callee && callee->kind == AST_IDENT_EXPR) {
    const char *cname = tok_intern(ctx, callee->tok_idx);
    Symbol *overloads[16];
    uint32_t n = sema_lookup_overloads(ctx, cname, overloads, 16);
    if (n > 1) {
      ASTNode *args[16];
      uint32_t argc = 0;
      for (ASTNode *a = callee->next_sibling; a && argc < 16;
           a = a->next_sibling)
        args[argc++] = a;

      int scores[16];
      int best_score = INT_MAX;
      int best_count = 0;
      int best_idx = -1;
      for (uint32_t i = 0; i < n; i++) {
        scores[i] = overloads[i]->decl
                        ? score_overload_candidate(ctx, overloads[i]->decl, args, argc)
                        : -1;
        int s = scores[i];
        if (s < 0) continue;
        if (s < best_score) {
          best_score = s;
          best_count = 1;
          best_idx = (int)i;
        } else if (s == best_score) {
          best_count++;
        }
      }
      /* A tie among candidates with IDENTICAL signatures is not genuine
       * ambiguity — it's msf's whole-module symbol table pooling the same-named
       * member from every type (e.g. a protocol method implemented by N types)
       * or file-private helpers copied across files.  Collapse those to the
       * first.  A tie among DISTINCT signatures (`h(Float)` vs `h(Double)` both
       * reachable via literal conversion) IS real ambiguity and is kept. */
      if (best_count > 1 && best_idx >= 0) {
        int distinct = 0;
        for (uint32_t i = 0; i < n && !distinct; i++) {
          if ((int)i == best_idx || scores[i] != best_score) continue;
          if (!overloads[i]->type || !overloads[best_idx]->type ||
              !type_equal(overloads[i]->type, overloads[best_idx]->type))
            distinct = 1;
        }
        if (!distinct) best_count = 1; /* effectively identical → pick first */
      }
      if (best_count == 1 && best_idx >= 0) {
        callee->type = overloads[best_idx]->type;
        node->data.call.resolved_callee_decl = overloads[best_idx]->decl;
        callee_t = overloads[best_idx]->type;
      } else if (best_count > 1) {
        sema_error(ctx, node, "ambiguous use of '%s'", cname);
      } else {
        /* Type scoring eliminated every candidate. Before erroring, fall back
         * to arity: if exactly one overload is arity-compatible, pick it. This
         * resolves generic overloads whose parameter types the scorer can't
         * precisely match (e.g. `[T]` vs `[Int]`, `Range<Int>` args) the same
         * forgiving way the single-overload path already behaves. */
        int arity_idx = -1;
        for (uint32_t i = 0; i < n && arity_idx < 0; i++) {
          if (overloads[i]->decl &&
              decl_has_generic_params(overloads[i]->decl) &&
              overload_arity_ok(overloads[i]->decl, argc))
            arity_idx = (int)i; /* first arity-compatible generic overload */
        }
        /* Pick the first arity-compatible generic overload even if several match
         * — msf's whole-module symbol table pools `private`/`fileprivate`
         * helpers from every file (a common pattern: the same private
         * `withCString`-style wrapper copied per file), which Swift would scope
         * to one file.  They're effectively identical, so resolving to one
         * avoids a spurious "no matching overload"; msf isn't a strict overload
         * resolver. */
        if (arity_idx >= 0) {
          callee->type = overloads[arity_idx]->type;
          node->data.call.resolved_callee_decl = overloads[arity_idx]->decl;
          callee_t = overloads[arity_idx]->type;
        } else {
          sema_error(ctx, node, "no matching overload for call to '%s'", cname);
          callee_t = NULL;
        }
      }
    } else if (n == 1) {
      /* Single overload — score for label/type match; if accepted, hook decl */
      ASTNode *args[16];
      uint32_t argc = 0;
      for (ASTNode *a = callee->next_sibling; a && argc < 16;
           a = a->next_sibling)
        args[argc++] = a;
      if (overloads[0]->decl) {
        int s =
            score_overload_candidate(ctx, overloads[0]->decl, args, argc);
        if (s >= 0) {
          callee->type = overloads[0]->type;
          node->data.call.resolved_callee_decl = overloads[0]->decl;
          callee_t = overloads[0]->type;
        }
      }
    }
  }

  if (is_delegation) {
    const char *del_base =
        (callee && callee->kind == AST_MEMBER_EXPR && callee->first_child &&
         callee->first_child->kind == AST_IDENT_EXPR)
            ? tok_intern(ctx, callee->first_child->tok_idx)
            : NULL;
    if (del_base && strcmp(del_base, "self") == 0)
      ctx->init_has_delegated = 1;
    if (del_base && strcmp(del_base, "super") == 0 && ctx->init_class_decl &&
        !ctx->init_is_convenience) {
      for (uint32_t i = 0; i < ctx->init_own_prop_count; i++) {
        if (!ctx->init_own_assigned[i] && ctx->init_own_props[i]) {
          sema_error(ctx, node,
                     "Property '%s' must be initialized before delegating to "
                     "super.init() (safety check 1)",
                     ctx->init_own_props[i]);
          break;
        }
      }
    }
    if (ctx->in_class_init_phase1)
      ctx->in_class_init_phase1 = 0;
  }

  /*
   * Generic constraint checking: at call site, verify concrete argument types
   * satisfy the callee's generic parameter constraints (where T: Equatable
   * etc.).
   */
  {
    const ASTNode *callee_decl = node->data.call.resolved_callee_decl;
    if (!callee_decl && callee && callee->kind == AST_IDENT_EXPR) {
      const char *cname = tok_intern(ctx, callee->tok_idx);
      const Symbol *sym = sema_lookup(ctx, cname);
      if (sym)
        callee_decl = sym->decl;
    }
    if (callee_decl &&
        (callee_decl->kind == AST_FUNC_DECL ||
         callee_decl->kind == AST_INIT_DECL) &&
        ctx->conformance_table) {
      TypeInfo *gp_tis[16];
      uint32_t ng = 0;
      for (const ASTNode *c = callee_decl->first_child; c && ng < 16;
           c = c->next_sibling)
        if (c->kind == AST_GENERIC_PARAM && c->type &&
            c->type->kind == TY_GENERIC_PARAM)
          gp_tis[ng++] = c->type;
      if (ng > 0) {
        TypeInfo *arg_tis[16];
        uint32_t narg = 0;
        for (ASTNode *a = callee ? callee->next_sibling : NULL; a && narg < 16;
             a = a->next_sibling)
          arg_tis[narg++] = a->type;
        TypeInfo *concrete_for_param[16];
        memset(concrete_for_param, 0, sizeof(concrete_for_param));
        uint32_t arg_idx = 0;
        for (const ASTNode *p = callee_decl->first_child; p && arg_idx < narg;
             p = p->next_sibling) {
          if (p->kind != AST_PARAM)
            continue;
          const TypeInfo *pt = p->type;
          for (uint32_t j = 0; j < ng; j++)
            if (pt == gp_tis[j]) {
              concrete_for_param[j] = arg_tis[arg_idx];
              break;
            }
          arg_idx++;
        }
        check_generic_args((TypeInfo *const *)gp_tis, ng, concrete_for_param,
                           ng, ctx->conformance_table, ctx, node);
      }
    }
  }

  if (callee && callee->kind == AST_MEMBER_EXPR) {
    ASTNode *base = callee->first_child;
    const char *method = callee->data.var.name_tok
                             ? tok_intern(ctx, callee->data.var.name_tok)
                             : NULL;
    if (base && base->kind == AST_MEMBER_EXPR && method) {
      ASTNode *ud_base = base->first_child;
      const char *ud_obj = (ud_base && ud_base->kind == AST_IDENT_EXPR)
                               ? tok_intern(ctx, ud_base->tok_idx)
                               : NULL;
      const char *ud_member = base->data.var.name_tok
                                  ? tok_intern(ctx, base->data.var.name_tok)
                                  : NULL;
      if (ud_obj && ud_member && strcmp(ud_obj, "UserDefaults") == 0 &&
          strcmp(ud_member, "standard") == 0) {
        TypeInfo *ctx_ty = foundation_contextual_result_type(ctx, node);
        TypeInfo *unwrapped_ctx_ty = foundation_unwrap_optional_type(ctx_ty);
        if (strcmp(method, "set") == 0 ||
            strcmp(method, "removeObject") == 0)
          return (node->type = TY_BUILTIN_VOID);
        if (strcmp(method, "object") == 0)
          return (node->type = unwrapped_ctx_ty ? unwrapped_ctx_ty
                                                : TY_BUILTIN_STRING);
        if (strcmp(method, "string") == 0)
          return (node->type = TY_BUILTIN_STRING);
        if (strcmp(method, "stringArray") == 0)
          return (node->type = make_array_call_type(ctx, TY_BUILTIN_STRING));
        if (strcmp(method, "integer") == 0)
          return (node->type = TY_BUILTIN_INT);
        if (strcmp(method, "double") == 0)
          return (node->type = TY_BUILTIN_DOUBLE);
        if (strcmp(method, "bool") == 0)
          return (node->type = TY_BUILTIN_BOOL);
        if (strcmp(method, "data") == 0)
          return (node->type = TY_BUILTIN_DATA);
        if (strcmp(method, "date") == 0)
          return (node->type = make_named_foundation_call_type(ctx, "Date"));
        if (strcmp(method, "array") == 0) {
          if (unwrapped_ctx_ty && unwrapped_ctx_ty->kind == TY_ARRAY)
            return (node->type = unwrapped_ctx_ty);
          return (node->type = make_default_array_call_type(ctx));
        }
        if (strcmp(method, "dictionary") == 0) {
          if (unwrapped_ctx_ty && unwrapped_ctx_ty->kind == TY_DICT)
            return (node->type = unwrapped_ctx_ty);
          return (node->type = make_default_dict_call_type(ctx));
        }
      }
      if (ud_obj && ud_member && strcmp(ud_obj, "FileManager") == 0 &&
          strcmp(ud_member, "default") == 0) {
        if (strcmp(method, "fileExists") == 0)
          return (node->type = TY_BUILTIN_BOOL);
        if (strcmp(method, "contentsOfDirectory") == 0)
          return (node->type = make_array_call_type(ctx, TY_BUILTIN_STRING));
        if (strcmp(method, "createDirectory") == 0 ||
            strcmp(method, "removeItem") == 0)
          return (node->type = TY_BUILTIN_VOID);
      }
    }
    if (base && method) {
      TypeInfo *base_type = resolve_node(ctx, base);
      if (base_type && base_type->kind == TY_OPTIONAL && base_type->inner)
        base_type = base_type->inner;
      if (base_type && base_type->kind == TY_NAMED && base_type->named.name &&
          strcmp(base_type->named.name, "FileManager") == 0) {
        if (strcmp(method, "fileExists") == 0)
          return (node->type = TY_BUILTIN_BOOL);
        if (strcmp(method, "contentsOfDirectory") == 0)
          return (node->type = make_array_call_type(ctx, TY_BUILTIN_STRING));
        if (strcmp(method, "createDirectory") == 0 ||
            strcmp(method, "removeItem") == 0)
          return (node->type = TY_BUILTIN_VOID);
      }
      if (base_type && type_kind_of(base_type) == TY_DATA &&
          strcmp(method, "write") == 0)
        return (node->type = TY_BUILTIN_VOID);
      if (base_type && base_type->kind == TY_NAMED && base_type->named.name &&
          strcmp(base_type->named.name, "Data") == 0 &&
          strcmp(method, "write") == 0)
        return (node->type = TY_BUILTIN_VOID);
      if (base_type && base_type->kind == TY_NAMED && base_type->named.name &&
          strcmp(base_type->named.name, "FileHandle") == 0) {
        if (strcmp(method, "readDataToEndOfFile") == 0 ||
            strcmp(method, "readToEnd") == 0)
          return (node->type = TY_BUILTIN_DATA);
        if (strcmp(method, "seekToEndOfFile") == 0 ||
            strcmp(method, "seekToEnd") == 0)
          return (node->type = TY_BUILTIN_INT);
        if (strcmp(method, "write") == 0 ||
            strcmp(method, "closeFile") == 0 ||
            strcmp(method, "close") == 0)
          return (node->type = TY_BUILTIN_VOID);
      }
      if (base_type && base_type->kind == TY_NAMED && base_type->named.name &&
          strcmp(base_type->named.name, "Bundle") == 0) {
        if (strcmp(method, "path") == 0 ||
            strcmp(method, "url") == 0 ||
            strcmp(method, "localizedString") == 0)
          return (node->type = TY_BUILTIN_STRING);
      }
      if (base_type && base_type->kind == TY_NAMED && base_type->named.name &&
          strcmp(base_type->named.name, "URLRequest") == 0) {
        if (strcmp(method, "setValue") == 0 ||
            strcmp(method, "addValue") == 0)
          return (node->type = TY_BUILTIN_VOID);
        if (strcmp(method, "value") == 0)
          return (node->type = TY_BUILTIN_STRING);
      }
      if (base_type && base_type->kind == TY_NAMED && base_type->named.name &&
          strcmp(base_type->named.name, "HTTPURLResponse") == 0) {
        if (strcmp(method, "value") == 0 ||
            strcmp(method, "localizedString") == 0)
          return (node->type = TY_BUILTIN_STRING);
      }
    }
    if (base && base->kind == AST_IDENT_EXPR && method) {
      const char *root = tok_intern(ctx, base->tok_idx);
      if (root && strcmp(root, "PropertyListSerialization") == 0) {
        if (strcmp(method, "data") == 0 ||
            strcmp(method, "dataFromPropertyList") == 0)
          return (node->type = TY_BUILTIN_DATA);
        if (strcmp(method, "propertyList") == 0 ||
            strcmp(method, "propertyListFromData") == 0) {
          TypeInfo *ctx_ty = foundation_contextual_result_type(ctx, node);
          ctx_ty = foundation_unwrap_optional_type(ctx_ty);
          return (node->type = ctx_ty ? ctx_ty : make_default_dict_call_type(ctx));
        }
      }
      if (root && strcmp(root, "Locale") == 0) {
        if (strcmp(method, "canonicalIdentifier") == 0 ||
            strcmp(method, "canonicalLanguageIdentifier") == 0) {
          return (node->type = TY_BUILTIN_STRING);
        }
      }
      if (root && (strncmp(root, "Unit", 4) == 0 || strcmp(root, "Dimension") == 0)) {
        if (strcmp(method, "baseUnit") == 0) {
          return (node->type = make_named_foundation_call_type(ctx, root));
        }
      }
    }
  }

  if (callee_t && callee_t->kind == TY_FUNC) {
    TypeInfo *ret = callee_t->func.ret ? callee_t->func.ret : TY_BUILTIN_VOID;
    int via_opt_chain =
        (callee && callee->kind == AST_MEMBER_EXPR && callee->first_child &&
         callee->first_child->kind == AST_OPTIONAL_CHAIN);
    if (via_opt_chain)
      return (node->type = wrap_optional_result(ret, 1, ctx));
    return (node->type = ret);
  }

  if (callee && callee->kind == AST_IDENT_EXPR) {
    const char *cname = tok_intern(ctx, callee->tok_idx);
    if (strcmp(cname, "print") == 0 || strcmp(cname, "print_int") == 0) {
      return (node->type = TY_BUILTIN_VOID);
    }
    if (strcmp(cname, "debugPrint") == 0) {
      return (node->type = TY_BUILTIN_VOID);
    }
    if (strcmp(cname, "dump") == 0) {
      ASTNode *arg0 = callee->next_sibling;
      TypeInfo *dump_t = arg0 ? arg0->type : NULL;
      return (node->type = dump_t ? dump_t : TY_BUILTIN_VOID);
    }
    if (strcmp(cname, "Mirror") == 0) {
      TypeInfo *base_t = type_arena_alloc(ctx->type_arena);
      base_t->kind = TY_NAMED;
      base_t->named.name = cname;
      base_t->named.decl = NULL;

      ASTNode *arg = callee->next_sibling;
      TypeInfo *subject_t = arg ? arg->type : TY_BUILTIN_VOID;
      if (!subject_t && arg) {
        subject_t = resolve_node(ctx, arg);
      }
      if (!subject_t) subject_t = TY_BUILTIN_VOID;

      TypeInfo *mirror_t = type_arena_alloc(ctx->type_arena);
      mirror_t->kind = TY_GENERIC_INST;
      mirror_t->generic.base = base_t;
      mirror_t->generic.arg_count = 1;
      mirror_t->generic.args = malloc(sizeof(TypeInfo *));
      mirror_t->generic.args[0] = subject_t;

      return (node->type = mirror_t);
    }
    if (strcmp(cname, "IndexPath") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "IndexPath"));
    if (strcmp(cname, "IndexSet") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "IndexSet"));
    if (strcmp(cname, "DateInterval") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "DateInterval"));
    if (strcmp(cname, "TimeZone") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "TimeZone"));
    if (strcmp(cname, "Locale") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "Locale"));
    if (strcmp(cname, "Decimal") == 0) {
      TypeInfo *base_t = make_named_foundation_call_type(ctx, "Decimal");
      const ASTNode *arg0 = callee->next_sibling;
      if (arg0 && arg0->arg_label_tok &&
          strcmp(tok_intern(ctx, arg0->arg_label_tok), "string") == 0) {
        return (node->type = wrap_optional_result(base_t, 1, ctx));
      }
      return (node->type = base_t);
    }
    if (strcmp(cname, "NumberFormatter") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "NumberFormatter"));
    if (strcmp(cname, "Bundle") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "Bundle"));
    if (strcmp(cname, "CachedURLResponse") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "CachedURLResponse"));
    if (strcmp(cname, "FileHandle") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "FileHandle"));
    if (strcmp(cname, "URLResponse") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "URLResponse"));
    if (strcmp(cname, "HTTPURLResponse") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "HTTPURLResponse"));
    if (strcmp(cname, "URLRequest") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "URLRequest"));
    if (strcmp(cname, "DateFormatter") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "DateFormatter"));
    if (strcmp(cname, "ISO8601DateFormatter") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "ISO8601DateFormatter"));
    if (strcmp(cname, "Measurement") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "Measurement"));
    if (strcmp(cname, "MeasurementFormatter") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "MeasurementFormatter"));
    if (strcmp(cname, "UnitConverter") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "UnitConverter"));
    if (strcmp(cname, "UnitConverterLinear") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "UnitConverterLinear"));
    if (strcmp(cname, "CLLocationCoordinate2D") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "CLLocationCoordinate2D"));
    if (strcmp(cname, "CLLocation") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "CLLocation"));
    if (strcmp(cname, "CLLocationManager") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "CLLocationManager"));
    if (strncmp(cname, "Unit", 4) == 0 || strcmp(cname, "Dimension") == 0)
      return (node->type = make_named_foundation_call_type(ctx, cname));
    if (strcmp(cname, SW_TYPE_BOOL) == 0) {
      ASTNode *arg0 = callee->next_sibling;
      const TypeInfo *arg_t = arg0 ? arg0->type : NULL;
      if (arg_t && (type_kind_of(arg_t) == TY_STRING ||
                    type_kind_of(arg_t) == TY_SUBSTRING))
        return (node->type = wrap_optional_result(TY_BUILTIN_BOOL, 1, ctx));
      return (node->type = TY_BUILTIN_BOOL);
    }
    if (strcmp(cname, "type") == 0) {
      return (node->type = TY_BUILTIN_STRING);
    }
    if (strcmp(cname, "JSONDecoder") == 0)
      return (node->type = TY_BUILTIN_JSONDECODER);
    if (strcmp(cname, "JSONEncoder") == 0)
      return (node->type = TY_BUILTIN_JSONENCODER);
    if (strcmp(cname, "PropertyListSerialization") == 0)
      return (node->type = make_named_foundation_call_type(ctx, "PropertyListSerialization"));
    TypeInfo *conv_ty = resolve_builtin(cname);
    if (conv_ty &&
        (is_integer_kind(conv_ty->kind) || is_float_kind(conv_ty->kind))) {
      return (node->type = conv_ty);
    }
  }

  if (callee && callee->kind == AST_IDENT_EXPR) {
    const char *cname = tok_intern(ctx, callee->tok_idx);
    if (strcmp(cname, "zip") == 0) {
      ASTNode *arg1 = callee->next_sibling;
      ASTNode *arg2 = arg1 ? arg1->next_sibling : NULL;
      TypeInfo *t1 = arg1 ? arg1->type : NULL;
      TypeInfo *t2 = arg2 ? arg2->type : NULL;
      if (!t1 && arg1)
        t1 = resolve_node(ctx, arg1);
      if (!t2 && arg2)
        t2 = resolve_node(ctx, arg2);
      if (t1 && t2 && t1->kind == TY_ARRAY && t2->kind == TY_ARRAY) {
        TypeInfo *tuple_t = type_arena_alloc(ctx->type_arena);
        tuple_t->kind = TY_TUPLE;
        tuple_t->tuple.elem_count = 2;
        TypeInfo **elems = malloc(sizeof(TypeInfo *) * 2);
        elems[0] = t1->inner ? t1->inner : TY_BUILTIN_INT;
        elems[1] = t2->inner ? t2->inner : TY_BUILTIN_INT;
        tuple_t->tuple.elems = elems;
        tuple_t->tuple.labels = NULL;

        TypeInfo *arr_t = type_arena_alloc(ctx->type_arena);
        arr_t->kind = TY_ARRAY;
        arr_t->inner = tuple_t;
        return (node->type = arr_t);
      }
    }
    Symbol *sym = sema_lookup(ctx, cname);
    if (sym && (sym->kind == SYM_STRUCT || sym->kind == SYM_CLASS ||
                sym->kind == SYM_ENUM || sym->kind == SYM_TYPE)) {
      if (sym->kind == SYM_CLASS && sym->type && sym->type->kind == TY_NAMED &&
          sym->type->named.decl) {
        uint32_t argc = 0;
        for (ASTNode *a = callee->next_sibling; a; a = a->next_sibling)
          argc++;
        if (!class_has_init_with_param_count(
                ctx, (const ASTNode *)sym->type->named.decl, argc))
          sema_error(ctx, node, "no matching initializer for argument count %u",
                     (unsigned)argc);
      }
      /* An enum's `init?(rawValue:)` is the RawRepresentable FAILABLE
       * initializer — `T(rawValue: x)` returns `T?` (so `… ?? default` is
       * valid).  Restrict to enums: an OptionSet/struct `init(rawValue:)` is
       * NOT failable (returns `Self`), so wrapping it would wrongly flag a
       * non-optional assignment. */
      ASTNode *a0 = callee->next_sibling;
      if (sym->kind == SYM_ENUM && a0 && a0->arg_label_tok &&
          strcmp(tok_intern(ctx, a0->arg_label_tok), "rawValue") == 0 &&
          !a0->next_sibling)
        return (node->type = wrap_optional_result(sym->type, 1, ctx));
      return (node->type = sym->type);
    }
  }

  if (callee && callee->kind == AST_MEMBER_EXPR) {
    ASTNode *base = callee->first_child;
    const ASTNode *method_id = base ? base->next_sibling : NULL;
    TypeInfo *base_t = base ? base->type : NULL;
    if (!base_t && base)
      base_t = resolve_node(ctx, base);
    if (base_t) {
      const char *mname = NULL;
      if (method_id)
        mname = tok_intern(ctx, method_id->tok_idx);
      else if (callee->data.var.name_tok)
        mname = tok_intern(ctx, callee->data.var.name_tok);
      if (mname) {
        if (base_t->kind == TY_JSONDECODER && strcmp(mname, "decode") == 0) {
          ASTNode *arg = callee->next_sibling;
          if (arg) {
            TypeInfo *arg_t = arg->type;
            if (!arg_t)
              arg_t = resolve_node(ctx, arg);

            if (!arg_t && arg->kind == AST_MEMBER_EXPR) {
              const char *m = NULL;
              if (arg->data.var.name_tok)
                m = tok_intern(ctx, arg->data.var.name_tok);
              if (m && strcmp(m, "self") == 0 && arg->first_child) {
                TypeInfo *bt = arg->first_child->type;
                if (!bt)
                  bt = resolve_node(ctx, arg->first_child);
                arg_t = bt;
              }
            }

            if (arg_t) {
              /*
               * Usually arg_t is the type instance type for T.self or it wraps
               * it in TY_METATYPE.
               */
              if (arg_t->kind == TY_NAMED) {
                node->type = arg_t;
              } else if (arg_t->kind ==
                         TY_METATYPE) { /* not officially in Miniswift but just
                                          in case */
                node->type = arg_t->inner;
              } else {
                /* Some other type? like unwrap labels if any */
                node->type = arg_t;
              }
            } else {
              node->type = TY_BUILTIN_VOID;
            }
            return node->type;
          }
        }
        if (base_t->kind == TY_JSONENCODER && strcmp(mname, "encode") == 0) {
          return (node->type = TY_BUILTIN_STRING);
        }
        TypeInfo *unwrapped_base = base_t;
        if (unwrapped_base->kind == TY_OPTIONAL && unwrapped_base->inner)
          unwrapped_base = unwrapped_base->inner;
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "URLComponents") == 0 &&
            strcmp(mname, "url") == 0) {
          return (node->type = TY_BUILTIN_STRING);
        }
        if (strcmp(mname, "formatted") == 0 ||
            strcmp(mname, "ISO8601Format") == 0) {
          TypeKind bk = type_kind_of(unwrapped_base);
          if (bk == TY_DOUBLE || bk == TY_FLOAT ||
              (unwrapped_base->kind == TY_NAMED &&
               unwrapped_base->named.name &&
               strcmp(unwrapped_base->named.name, "Date") == 0))
            return (node->type = TY_BUILTIN_STRING);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "DateInterval") == 0) {
          if (strcmp(mname, "contains") == 0 ||
              strcmp(mname, "intersects") == 0)
            return (node->type = TY_BUILTIN_BOOL);
          if (strcmp(mname, "intersection") == 0)
            return (node->type = unwrapped_base);
        }
        if (is_range_family_type(unwrapped_base) &&
            (strcmp(mname, "contains") == 0 ||
             strcmp(mname, "overlaps") == 0)) {
          return (node->type = TY_BUILTIN_BOOL);
        }
        if (is_range_family_type(unwrapped_base) &&
            strcmp(mname, "relative") == 0) {
          return (node->type = make_named_foundation_call_type(ctx, "Range"));
        }
        if (range_family_type_name(unwrapped_base) &&
            strcmp(range_family_type_name(unwrapped_base),
                   "PartialRangeFrom") == 0 &&
            strcmp(mname, "makeIterator") == 0) {
          return (node->type = make_named_foundation_call_type(
                      ctx, "PartialRangeFrom.Iterator"));
        }
        if (range_family_type_name(unwrapped_base) &&
            strcmp(range_family_type_name(unwrapped_base),
                   "PartialRangeFrom") == 0 &&
            strcmp(mname, "prefix") == 0) {
          return (node->type = make_default_array_call_type(ctx));
        }
        if (range_family_type_name(unwrapped_base) &&
            strcmp(range_family_type_name(unwrapped_base),
                   "PartialRangeFrom") == 0 &&
            strcmp(mname, "dropFirst") == 0) {
          return (node->type = unwrapped_base);
        }
        if (range_family_type_name(unwrapped_base) &&
            strcmp(range_family_type_name(unwrapped_base),
                   "PartialRangeFrom.Iterator") == 0 &&
            strcmp(mname, "next") == 0) {
          return (node->type = wrap_optional_result(TY_BUILTIN_INT, 1, ctx));
        }
        if (is_rangeset_type(unwrapped_base)) {
          if (strcmp(mname, "contains") == 0 ||
              strcmp(mname, "isSubset") == 0 ||
              strcmp(mname, "isSuperset") == 0 ||
              strcmp(mname, "isStrictSubset") == 0 ||
              strcmp(mname, "isStrictSuperset") == 0 ||
              strcmp(mname, "isDisjoint") == 0)
            return (node->type = TY_BUILTIN_BOOL);
          if (strcmp(mname, "insert") == 0 ||
              strcmp(mname, "remove") == 0 ||
              strcmp(mname, "formUnion") == 0 ||
              strcmp(mname, "formIntersection") == 0 ||
              strcmp(mname, "formSymmetricDifference") == 0 ||
              strcmp(mname, "subtract") == 0)
            return (node->type = TY_BUILTIN_VOID);
          if (strcmp(mname, "union") == 0 ||
              strcmp(mname, "intersection") == 0 ||
              strcmp(mname, "subtracting") == 0 ||
              strcmp(mname, "symmetricDifference") == 0)
            return (node->type = unwrapped_base);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "CharacterSet") == 0) {
          if (strcmp(mname, "union") == 0 ||
              strcmp(mname, "intersection") == 0 ||
              strcmp(mname, "subtracting") == 0 ||
              strcmp(mname, "symmetricDifference") == 0)
            return (node->type = unwrapped_base);
          if (strcmp(mname, "isSuperset") == 0)
            return (node->type = TY_BUILTIN_BOOL);
          if (strcmp(mname, "formUnion") == 0 ||
              strcmp(mname, "formIntersection") == 0 ||
              strcmp(mname, "subtract") == 0 ||
              strcmp(mname, "formSymmetricDifference") == 0)
            return (node->type = TY_BUILTIN_VOID);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "TimeZone") == 0) {
          if (strcmp(mname, "secondsFromGMT") == 0)
            return (node->type = TY_BUILTIN_INT);
          if (strcmp(mname, "abbreviation") == 0 ||
              strcmp(mname, "localizedName") == 0)
            return (node->type = TY_BUILTIN_STRING);
          if (strcmp(mname, "isDaylightSavingTime") == 0)
            return (node->type = TY_BUILTIN_BOOL);
          if (strcmp(mname, "daylightSavingTimeOffset") == 0)
            return (node->type = TY_BUILTIN_DOUBLE);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "Locale") == 0) {
          if (strncmp(mname, "localizedString", 15) == 0)
            return (node->type = wrap_optional_result(TY_BUILTIN_STRING, 1, ctx));
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "NumberFormatter") == 0) {
          if (strcmp(mname, "string") == 0 ||
              strcmp(mname, "stringForObjectValue") == 0)
            return (node->type = TY_BUILTIN_STRING);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "DateFormatter") == 0) {
          if (strcmp(mname, "string") == 0 ||
              strcmp(mname, "localizedString") == 0)
            return (node->type = TY_BUILTIN_STRING);
          if (strcmp(mname, "date") == 0)
            return (node->type = TY_BUILTIN_DOUBLE);
          if (strcmp(mname, "setLocalizedDateFormatFromTemplate") == 0)
            return (node->type = TY_BUILTIN_VOID);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "ISO8601DateFormatter") == 0) {
          if (strcmp(mname, "string") == 0)
            return (node->type = TY_BUILTIN_STRING);
          if (strcmp(mname, "date") == 0)
            return (node->type = TY_BUILTIN_DOUBLE);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "Measurement") == 0) {
          if (strcmp(mname, "converted") == 0)
            return (node->type = unwrapped_base);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "MeasurementFormatter") == 0) {
          if (strcmp(mname, "string") == 0)
            return (node->type = TY_BUILTIN_STRING);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "HTTPURLResponse") == 0) {
          if (strcmp(mname, "value") == 0 ||
              strcmp(mname, "localizedString") == 0)
            return (node->type = TY_BUILTIN_STRING);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            (strcmp(unwrapped_base->named.name, "UnitConverter") == 0 ||
             strcmp(unwrapped_base->named.name, "UnitConverterLinear") == 0)) {
          if (strcmp(mname, "baseUnitValue") == 0 ||
              strcmp(mname, "value") == 0)
            return (node->type = TY_BUILTIN_DOUBLE);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "IndexPath") == 0) {
          if (strcmp(mname, "append") == 0)
            return (node->type = TY_BUILTIN_VOID);
          if (strcmp(mname, "appending") == 0 ||
              strcmp(mname, "dropLast") == 0 ||
              strcmp(mname, "removingLastIndex") == 0)
            return (node->type = unwrapped_base);
          if (strcmp(mname, "index") == 0)
            return (node->type = TY_BUILTIN_INT);
        }
        if (unwrapped_base->kind == TY_NAMED && unwrapped_base->named.name &&
            strcmp(unwrapped_base->named.name, "IndexSet") == 0) {
          if (strcmp(mname, "contains") == 0)
            return (node->type = TY_BUILTIN_BOOL);
          if (strcmp(mname, "insert") == 0 ||
              strcmp(mname, "remove") == 0)
            return (node->type = TY_BUILTIN_VOID);
        }

        TypeInfo *bm = lookup_builtin_member(ctx, base_t, mname);
        if (bm) {
          /*
           * Optional.map / Optional.flatMap: result type is
           * Optional<closure_return_type>.
           */
          if (base_t->kind == TY_OPTIONAL && base_t->inner &&
              (strcmp(mname, "map") == 0 || strcmp(mname, "flatMap") == 0)) {
            ASTNode *arg = callee->next_sibling;
            if (arg && arg->type && arg->type->kind == TY_FUNC &&
                arg->type->func.ret) {
              TypeInfo *opt = type_arena_alloc(ctx->type_arena);
              opt->kind = TY_OPTIONAL;
              opt->inner = arg->type->func.ret;
              return (node->type = opt);
            }
          }
          return (node->type = bm);
        }
        /* String firstIndex(of:)/lastIndex(of:) -> Int? (not firstIndex(where:)) */
        if (TY_BUILTIN_STRING && base_t->kind == TY_BUILTIN_STRING->kind &&
            callee->next_sibling) {
          const ASTNode *a = callee->next_sibling;
          if (a->kind != AST_CLOSURE_EXPR &&
              (strcmp(mname, "firstIndex") == 0 ||
               strcmp(mname, "lastIndex") == 0)) {
            TypeInfo *opt = type_arena_alloc(ctx->type_arena);
            opt->kind = TY_OPTIONAL;
            opt->inner = TY_BUILTIN_INT;
            return (node->type = opt);
          }
        }
      }
    }
  }

  if (callee && callee->kind == AST_ARRAY_LITERAL) {
    if (callee_t)
      return (node->type = callee_t);
  }

  if (callee && callee->kind == AST_IDENT_EXPR) {
    const char *cname = tok_intern(ctx, callee->tok_idx);
    if (cname && strcmp(cname, "Dictionary") == 0) {
      TypeInfo *ti = type_arena_alloc(ctx->type_arena);
      ti->kind = TY_DICT;
      ti->dict.key = TY_BUILTIN_STRING;
      ti->dict.value = TY_BUILTIN_INT;
      return (node->type = ti);
    }
    if (cname && strcmp(cname, "Array") == 0) {
      TypeInfo *ti = type_arena_alloc(ctx->type_arena);
      ti->kind = TY_ARRAY;
      ti->inner = TY_BUILTIN_INT;
      return (node->type = ti);
    }
    if (cname && strcmp(cname, "Set") == 0) {
      TypeInfo *ti = type_arena_alloc(ctx->type_arena);
      ti->kind = TY_SET;
      ti->inner = TY_BUILTIN_INT;
      return (node->type = ti);
    }
    if (cname && strcmp(cname, "Data") == 0) {
      return (node->type = TY_BUILTIN_DATA);
    }
  }

  /* Unresolvable call: memoize the failure so the node is not re-resolved on
   * every visit. node->type stays NULL (callers rely on that to mean
   * "unresolved"), so we mark the call node itself. Without this, nested
   * unresolvable calls — a chain of methods returning a self-referential
   * associated type, `s.e().e()...` with `associatedtype E: Gen` — re-resolve
   * their base AND callee at each level (both leave node->type NULL), giving
   * O(2^depth) work and a frontend hang on deep chains. */
  node->modifiers |= MOD_CALL_RESOLVE_FAILED;
  return NULL;
}
