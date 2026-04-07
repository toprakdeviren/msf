/**
 * @file call.c
 * @brief Call expression resolution: callee(args), overload resolution,
 *        init delegation, generic constraint checking.
 */

#include "../../private.h"

/*
 */

TypeInfo *resolve_call_expr(SemaContext *ctx, ASTNode *node) {
  ASTNode *callee = node->first_child;
  int is_delegation = 0;

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
   * Overload resolution: when callee is bare name, pick best matching overload
   * by param count + types. Use decl's AST_PARAM children for param types (sema
   * does not fill TypeInfo.func.params).
   */
  if (callee && callee->kind == AST_IDENT_EXPR) {
    const char *cname = tok_intern(ctx, callee->tok_idx);
    Symbol *overloads[16];
    uint32_t n = sema_lookup_overloads(ctx, cname, overloads, 16);
    if (n > 0) {
      uint32_t argc = 0;
      TypeInfo *arg_types[16];
      for (ASTNode *a = callee->next_sibling; a && argc < 16;
           a = a->next_sibling, argc++)
        arg_types[argc] = a->type;
      for (uint32_t i = 0; i < n; i++) {
        const ASTNode *decl = overloads[i]->decl;
        if (!decl)
          continue;
        uint32_t param_count = 0;
        for (const ASTNode *p = decl->first_child; p; p = p->next_sibling)
          if (p->kind == AST_PARAM)
            param_count++;
        if (param_count != argc)
          continue;
        int match = 1;
        uint32_t j = 0;
        for (const ASTNode *p = decl->first_child; p && match;
             p = p->next_sibling) {
          if (p->kind != AST_PARAM)
            continue;
          if (j >= argc || !arg_types[j] || !p->type ||
              !type_equal(p->type, arg_types[j]))
            match = 0;
          j++;
        }
        if (match && j == argc) {
          callee->type = overloads[i]->type;
          node->data.call.resolved_callee_decl = overloads[i]->decl;
          callee_t = overloads[i]->type;
          break;
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
      TypeInfo *mirror_t = type_arena_alloc(ctx->type_arena);
      mirror_t->kind = TY_NAMED;
      mirror_t->named.name = cname;
      mirror_t->named.decl = NULL;
      return (node->type = mirror_t);
    }
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

  return NULL;
}
