/**
 * @file dispatch.c
 * @brief Expression resolution: literals, identifiers, binary, unary,
 *        assignment, call, member, cast, ternary, subscript, key path.
 */
#include "../../private.h"

/*
 */

/* ─── Expression case dispatcher (called from resolve_node) ─── */
TypeInfo *resolve_node_expr(SemaContext *ctx, ASTNode *node) {
  switch (node->kind) {
  case AST_INTEGER_LITERAL:
    return (node->type = TY_BUILTIN_INT);
  case AST_FLOAT_LITERAL:
    return (node->type = TY_BUILTIN_DOUBLE);
  case AST_STRING_LITERAL:
    resolve_children(ctx, node);
    return (node->type = TY_BUILTIN_STRING);
  case AST_REGEX_LITERAL:
    resolve_children(ctx, node);
    return (node->type =
                TY_BUILTIN_STRING); /* pattern as String until Regex type */
  case AST_BOOL_LITERAL:
    return (node->type = TY_BUILTIN_BOOL);
  case AST_NIL_LITERAL:
    return NULL;
  case AST_ARRAY_LITERAL: {
    TypeInfo *elem_t = NULL;
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      TypeInfo *et = resolve_node(ctx, c);
      if (!elem_t) {
        elem_t = et;
      } else if (et && !type_equal(elem_t, et)) {
        char exp_s[64], got_s[64];
        type_to_string(elem_t, exp_s, sizeof(exp_s));
        type_to_string(et, got_s, sizeof(got_s));
        sema_error(ctx, c,
                   "Array elements must be the same type: expected '%s', found "
                   "'%s'",
                   exp_s, got_s);
      }
    }
    TypeInfo *ti = type_arena_alloc(ctx->type_arena);
    ti->kind = TY_ARRAY;
    ti->inner = elem_t;
    return (node->type = ti);
  }
  case AST_DICT_LITERAL: {
    TypeInfo *kt = NULL, *vt = NULL;
    int is_key = 1;
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      TypeInfo *t = resolve_node(ctx, c);
      if (is_key && !kt)
        kt = t;
      else if (!is_key && !vt)
        vt = t;
      is_key = !is_key;
    }
    TypeInfo *ti = type_arena_alloc(ctx->type_arena);
    ti->kind = TY_DICT;
    ti->dict.key = kt;
    ti->dict.value = vt;
    return (node->type = ti);
  }
  case AST_IDENT_EXPR: {
    const char *iname = tok_intern(ctx, node->tok_idx);
    if (ctx->in_class_init_phase1 && iname && strcmp(iname, "self") == 0) {
      int allowed = 0;
      if (node->parent && node->parent->kind == AST_MEMBER_EXPR &&
          node->parent->first_child == node) {
        const ASTNode *assign = node->parent->parent;
        if (assign && assign->kind == AST_ASSIGN_EXPR &&
            assign->first_child == node->parent)
          allowed = 1;
      }
      if (!allowed)
        sema_error(ctx, node,
                   "Cannot use 'self' before 'super.init()' (two-phase "
                   "initialization)");
    }
    TypeInfo *bi = resolve_builtin(iname);
    if (bi)
      return (node->type = bi);
    Symbol *sym = sema_lookup(ctx, iname);
    if (sym) {
      if (!sym->type && sym->decl) {
        if (sym->is_resolving) {
          /* Circular reference: let x = x */
          sema_error(ctx, node, "circular reference in declaration of '%s'", iname);
          return (node->type = TY_BUILTIN_INT); /* break cycle with fallback type */
        }
        sym->is_resolving = 1;
        sym->type = resolve_node(ctx, sym->decl);
        sym->is_resolving = 0;
      }
      if (!sym->is_initialized &&
          (sym->kind == SYM_LET || sym->kind == SYM_VAR)) {
        int is_assign_lhs = 0;
        if (node->parent && node->parent->first_child == node) {
          if (node->parent->kind == AST_ASSIGN_EXPR)
            is_assign_lhs = 1;
          else if (node->parent->kind == AST_BINARY_EXPR &&
                   node->parent->data.binary.op_tok) {
            const Token *op = &ctx->tokens[node->parent->data.binary.op_tok];
            if (op->len == 1 && ctx->src->data[op->pos] == '=')
              is_assign_lhs = 1;
          }
        }
        /* Deferred let: treat as LHS when we're (possibly nested) under
         * assignment LHS */
        if (!is_assign_lhs && sym->is_deferred) {
          for (ASTNode *p = node->parent; p; p = p->parent) {
            if (p->kind == AST_BINARY_EXPR && p->data.binary.op_tok) {
              const Token *op = &ctx->tokens[p->data.binary.op_tok];
              if (op->len == 1 && ctx->src->data[op->pos] == '=') {
                for (ASTNode *n = node; n && n != p; n = n->parent)
                  if (n == p->first_child) {
                    is_assign_lhs = 1;
                    break;
                  }
                if (is_assign_lhs)
                  break;
              }
            }
            if (p->kind == AST_ASSIGN_EXPR) {
              for (ASTNode *n = node; n && n != p; n = n->parent)
                if (n == p->first_child) {
                  is_assign_lhs = 1;
                  break;
                }
              if (is_assign_lhs)
                break;
            }
          }
        }
        /* Deferred let: allow use before init (definite assignment checked
         * per-branch) */
        if (!is_assign_lhs && !sym->is_deferred) {
          sema_error(ctx, node, "Variable '%s' used before being initialized",
                     iname);
        }
      }
      if (ctx->requires_explicit_self && iname && strcmp(iname, "self") != 0) {
        const ASTNode *closure = find_ancestor_closure(node);
        if (closure) {
          const ASTNode *enclosing = find_enclosing_type_decl(closure);
          if (enclosing && symbol_is_instance_member_of(sym, enclosing))
            sema_error(ctx, node,
                       "explicit 'self' required in escaping closure");
        }
      }
      /* apply(safe): contextual param type is () throws -> T but safe is () ->
       * T */
      if (sym->type && sym->type->kind == TY_FUNC &&
          ctx->expected_closure_type &&
          ctx->expected_closure_type->kind == TY_FUNC &&
          (sym->kind == SYM_FUNC ||
           (sym->decl && sym->decl->kind == AST_FUNC_DECL))) {
        const TypeInfo *g = sym->type;
        TypeInfo *e = ctx->expected_closure_type;
        if (g->func.param_count == e->func.param_count && g->func.ret &&
            e->func.ret && type_equal(g->func.ret, e->func.ret)) {
          int params_ok = 1;
          for (uint32_t pi = 0; pi < g->func.param_count && params_ok; pi++) {
            if (!g->func.params || !e->func.params || !g->func.params[pi] ||
                !e->func.params[pi] ||
                !type_equal(g->func.params[pi], e->func.params[pi]))
              params_ok = 0;
          }
          if (params_ok && !g->func.throws && e->func.throws)
            return (node->type = e);
        }
      }
      return (node->type = sym->type);
    }
    return NULL;
  }
  case AST_BINARY_EXPR:
    return resolve_binary_expr(ctx, node);
  case AST_UNARY_EXPR: {
    TypeInfo *operand_t = resolve_node(ctx, node->first_child);
    if (node->data.binary.op_tok) {
      const Token *op = &ctx->tokens[node->data.binary.op_tok];
      if (op->len == 1 && ctx->src->data[op->pos] == '!')
        return (node->type = TY_BUILTIN_BOOL);
    }
    return (node->type = operand_t);
  }
  case AST_ASSIGN_EXPR: {
    ASTNode *lhs = node->first_child;
    ASTNode *rhs = lhs ? lhs->next_sibling : NULL;
    if (ctx->init_class_decl && lhs && lhs->kind == AST_MEMBER_EXPR) {
      const ASTNode *base = lhs->first_child;
      const char *bname = (base && base->kind == AST_IDENT_EXPR)
                              ? tok_intern(ctx, base->tok_idx)
                              : NULL;
      if (bname && strcmp(bname, "self") == 0) {
        const char *pname = lhs->data.var.name_tok
                                ? tok_intern(ctx, lhs->data.var.name_tok)
                            : (base && base->next_sibling)
                                ? tok_intern(ctx, base->next_sibling->tok_idx)
                                : NULL;
        if (pname) {
          if (ctx->init_is_convenience && !ctx->init_has_delegated) {
            sema_error(ctx, lhs,
                       "Convenience initializer must delegate (call self.init) "
                       "before assigning to any property (safety check 3)");
          } else if (!ctx->init_is_convenience && ctx->in_class_init_phase1 &&
                     is_inherited_stored_property(
                         ctx, (const ASTNode *)ctx->init_class_decl, pname)) {
            sema_error(ctx, lhs,
                       "Cannot assign to inherited property '%s' before "
                       "super.init() (safety check 2)",
                       pname);
          } else if (!ctx->init_is_convenience && ctx->in_class_init_phase1) {
            for (uint32_t i = 0; i < ctx->init_own_prop_count; i++)
              if (ctx->init_own_props[i] &&
                  strcmp(ctx->init_own_props[i], pname) == 0) {
                ctx->init_own_assigned[i] = 1;
                break;
              }
          }
        }
      }
    }
    TypeInfo *lt = resolve_node(ctx, lhs);
    TypeInfo *rt = resolve_node(ctx, rhs);
    if (lhs && ctx->current_func_decl) {
      const ASTNode *fn = (ASTNode *)ctx->current_func_decl;
      const ASTNode *struct_decl = find_enclosing_struct_decl(fn);
      if (struct_decl && !(fn->modifiers & MOD_STATIC) &&
          !(fn->modifiers & MOD_MUTATING)) {
        int mutation_of_self = 0;
        if (lhs->kind == AST_MEMBER_EXPR) {
          const ASTNode *base = lhs->first_child;
          if (base && base->kind == AST_IDENT_EXPR) {
            const char *bname = tok_intern(ctx, base->tok_idx);
            if (bname && strcmp(bname, "self") == 0 && lhs->data.var.name_tok) {
              const char *mname = tok_intern(ctx, lhs->data.var.name_tok);
              if (is_stored_property_of_struct(ctx, struct_decl, mname))
                mutation_of_self = 1;
            }
          }
        } else if (lhs->kind == AST_IDENT_EXPR) {
          const char *iname = tok_intern(ctx, lhs->tok_idx);
          const Symbol *sym = sema_lookup(ctx, iname);
          if (sym && symbol_is_instance_member_of(sym, struct_decl))
            mutation_of_self = 1;
        }
        if (mutation_of_self)
          sema_error(
              ctx, lhs,
              "Cannot assign to property of 'self' in non-mutating method; "
              "mark method as 'mutating'");
      }
    }
    if (lhs && lhs->kind == AST_MEMBER_EXPR) {
      const ASTNode *base = lhs->first_child;
      if (base && base->type && base->type->kind == TY_NAMED &&
          base->type->named.decl &&
          ((const ASTNode *)base->type->named.decl)->kind == AST_STRUCT_DECL) {
        const ASTNode *root = root_ident_of_expr(base);
        if (root && root->kind == AST_IDENT_EXPR) {
          const char *bname = tok_intern(ctx, root->tok_idx);
          const Symbol *base_sym = sema_lookup(ctx, bname);
          if (base_sym && base_sym->kind == SYM_LET) {
            sema_error(ctx, lhs,
                       "Cannot assign to property: '%s' is a 'let' constant",
                       bname);
          }
        }
      }
    }
    if (lhs && lhs->kind == AST_IDENT_EXPR) {
      const char *iname = tok_intern(ctx, lhs->tok_idx);
      Symbol *sym = sema_lookup(ctx, iname);
      if (sym && sym->kind == SYM_LET) {
        if (!sym->is_initialized) {
          sym->is_initialized = 1;
          if (!sym->type && rt)
            sym->type = rt;
        } else if (!sym->is_deferred) {
          /* Deferred let: allow assignment in mutually exclusive branches
           * (if/else, switch cases) */
          sema_error(ctx, lhs,
                     "Cannot assign to value: '%s' is a 'let' constant", iname);
        }
      }
    }
    if (lt && rt && !type_equal(lt, rt) && lt->kind != TY_UNKNOWN &&
        rt->kind != TY_UNKNOWN && lt->kind != TY_NAMED &&
        rt->kind != TY_NAMED) {
      int is_empty_collection_literal = 0;
      if (rhs && rhs->kind == AST_ARRAY_LITERAL && !rhs->first_child &&
          (lt->kind == TY_ARRAY || lt->kind == TY_SET || lt->kind == TY_DICT)) {
        is_empty_collection_literal = 1;
        rhs->type = lt;
      }
      if (rhs && rhs->kind == AST_DICT_LITERAL && !rhs->first_child &&
          lt->kind == TY_DICT) {
        is_empty_collection_literal = 1;
        rhs->type = lt;
      }
      if (!is_empty_collection_literal) {
        char lt_s[64], rt_s[64];
        type_to_string(lt, lt_s, sizeof(lt_s));
        type_to_string(rt, rt_s, sizeof(rt_s));
        if (is_int_float_mix(lt, rt)) {
          sema_error(ctx, rhs,
                     "Cannot assign value of type '%s' to type '%s'; "
                     "use explicit conversion",
                     rt_s, lt_s);
        } else {
          sema_error(ctx, rhs, "Type mismatch: expected '%s', got '%s'",
                     lt_s, rt_s);
        }
      }
    }
    if (lhs && is_lhs_optional_chain(lhs)) {
      TypeInfo *void_opt = type_arena_alloc(ctx->type_arena);
      void_opt->kind = TY_OPTIONAL;
      void_opt->inner = TY_BUILTIN_VOID;
      return (node->type = void_opt);
    }
    return (node->type = lt);
  }
  case AST_CALL_EXPR:
    return resolve_call_expr(ctx, node);
  case AST_MEMBER_EXPR:
    return resolve_member_expr(ctx, node);
  case AST_KEY_PATH_EXPR: {
    /* Basic KeyPath resolution: \Type.property -> (Type) -> PropertyType
     * Inferred root (\.property) resolves to the property type from context. */
    const char *prop_name = node->data.var.name_tok
                                ? tok_intern(ctx, node->data.var.name_tok)
                                : NULL;
    TypeInfo *root_type = NULL;
    if (node->first_child) {
      root_type = resolve_node(ctx, node->first_child);
      if (!root_type && node->first_child->type)
        root_type = node->first_child->type;
    }
    if (!root_type) {
      TypeInfo *ctx_type = get_contextual_type_for_implicit_member(ctx, node);
      if (ctx_type && ctx_type->kind == TY_OPTIONAL && ctx_type->inner)
        ctx_type = ctx_type->inner;
      root_type = ctx_type;
    }
    TypeInfo *value_type = NULL;
    if (root_type && prop_name) {
      const ASTNode *decl = named_type_decl(ctx, root_type);
      if (decl) {
        for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
          if (c->kind != AST_BLOCK)
            continue;
          for (const ASTNode *m = c->first_child; m; m = m->next_sibling) {
            if (m->kind != AST_VAR_DECL && m->kind != AST_LET_DECL)
              continue;
            if (!m->data.var.name_tok)
              continue;
            const char *mn = tok_intern(ctx, m->data.var.name_tok);
            if (mn && strcmp(mn, prop_name) == 0) {
              value_type = m->type;
              break;
            }
          }
          break;
        }
      }
      if (!value_type)
        value_type = lookup_builtin_member(ctx, root_type, prop_name);
    }
    if (!value_type)
      value_type = TY_BUILTIN_INT;
    TypeInfo *fn_ty = type_arena_alloc(ctx->type_arena);
    if (!fn_ty)
      return (node->type = value_type);
    fn_ty->kind = TY_FUNC;
    fn_ty->func.param_count = 1;
    fn_ty->func.params = calloc(1, sizeof(TypeInfo *));
    if (fn_ty->func.params)
      fn_ty->func.params[0] = root_type;
    fn_ty->func.ret = value_type;
    return (node->type = fn_ty);
  }
  case AST_TUPLE_EXPR: {
    uint32_t n = 0;
    for (ASTNode *c = node->first_child; c; c = c->next_sibling)
      n++;
    TypeInfo *ty = type_arena_alloc(ctx->type_arena);
    if (!ty)
      return (node->type = NULL);
    ty->kind = TY_TUPLE;
    ty->tuple.elem_count = n;
    ty->tuple.elems = calloc(n, sizeof(TypeInfo *));
    ty->tuple.labels = calloc(n, sizeof(const char *));
    if (!ty->tuple.elems || !ty->tuple.labels) {
      free(ty->tuple.elems);
      free(ty->tuple.labels);
      return (node->type = NULL);
    }
    uint32_t i = 0;
    for (ASTNode *c = node->first_child; c; c = c->next_sibling, i++) {
      ty->tuple.elems[i] = resolve_node(ctx, c);
      if (c->kind != AST_INTEGER_LITERAL && c->kind != AST_FLOAT_LITERAL &&
          c->kind != AST_BOOL_LITERAL && c->data.binary.op_tok != 0) {
        const Token *lt = &ctx->tokens[c->data.binary.op_tok];
        if (lt->type == TOK_IDENTIFIER) {
          ty->tuple.labels[i] =
              sema_intern(ctx, ctx->src->data + lt->pos, lt->len);
        }
      }
      if (!ty->tuple.labels[i] && c->tok_idx >= 2) {
        const Token *colon_tok = &ctx->tokens[c->tok_idx - 1];
        const Token *label_tok = &ctx->tokens[c->tok_idx - 2];
        if (colon_tok->type == TOK_PUNCT &&
            ctx->src->data[colon_tok->pos] == ':' &&
            label_tok->type == TOK_IDENTIFIER) {
          ty->tuple.labels[i] =
              sema_intern(ctx, ctx->src->data + label_tok->pos, label_tok->len);
        }
      }
    }
    return (node->type = ty);
  }
  case AST_PAREN_EXPR:
    return (node->type = resolve_node(ctx, node->first_child));
  case AST_OPTIONAL_CHAIN: {
    TypeInfo *inner = resolve_node(ctx, node->first_child);
    if (inner)
      return (node->type = inner);
    return NULL;
  }
  case AST_FORCE_UNWRAP: {
    TypeInfo *opt_t = resolve_node(ctx, node->first_child);
    if (opt_t && opt_t->kind == TY_OPTIONAL)
      return (node->type = opt_t->inner);
    return (node->type = opt_t);
  }
  case AST_TRY_EXPR:
    return (node->type = resolve_node(ctx, node->first_child));
  case AST_CONSUME_EXPR:
    return (node->type = resolve_node(ctx, node->first_child));
  case AST_AWAIT_EXPR: {
    TypeInfo *inner_t = resolve_node(ctx, node->first_child);
    if (inner_t && inner_t->kind == TY_FUNC)
      inner_t = inner_t->func.ret ? inner_t->func.ret : TY_BUILTIN_VOID;
    return (node->type = inner_t);
  }
  case AST_CAST_EXPR: {
    ASTNode *expr = node->first_child;
    const ASTNode *type_n = expr ? expr->next_sibling : NULL;
    resolve_node(ctx, expr);
    TypeInfo *cast_t = resolve_type_annotation(ctx, type_n);
    const Token *op = node->data.binary.op_tok
                          ? &ctx->tokens[node->data.binary.op_tok]
                          : NULL;
    if (op && op->len == 2 && memcmp(ctx->src->data + op->pos, "is", 2) == 0)
      return (node->type = TY_BUILTIN_BOOL);
    return (node->type = cast_t);
  }
  case AST_TERNARY_EXPR: {
    ASTNode *cond_n = node->first_child;
    ASTNode *then_n = cond_n ? cond_n->next_sibling : NULL;
    ASTNode *else_n = then_n ? then_n->next_sibling : NULL;
    resolve_node(ctx, cond_n);
    TypeInfo *then_t = resolve_node(ctx, then_n);
    TypeInfo *else_t = resolve_node(ctx, else_n);
    return (node->type = then_t ? then_t : else_t);
  }
  case AST_SUBSCRIPT_EXPR: {
    ASTNode *base = node->first_child;
    TypeInfo *base_t = resolve_node(ctx, base);
    if (base && base->next_sibling)
      resolve_node(ctx, base->next_sibling);
    TypeInfo *unwrapped = base_t;
    if (base_t && base_t->kind == TY_OPTIONAL)
      unwrapped = base_t->inner;
    int base_is_opt_chain = (base && base->kind == AST_OPTIONAL_CHAIN);
    int base_is_dict = (unwrapped && unwrapped->kind == TY_DICT);
    TypeInfo *elem_t = NULL;
    if (unwrapped && unwrapped->kind == TY_ARRAY)
      elem_t = unwrapped->inner;
    else if (base_is_dict)
      elem_t = unwrapped->dict.value;
    else if (unwrapped && unwrapped->kind == TY_NAMED &&
             unwrapped->named.decl) {
      const ASTNode *tdecl = unwrapped->named.decl;
      for (const ASTNode *c = tdecl->first_child; c; c = c->next_sibling) {
        if (c->kind != AST_BLOCK)
          continue;
        for (const ASTNode *m = c->first_child; m; m = m->next_sibling) {
          if (m->kind == AST_SUBSCRIPT_DECL) {
            for (const ASTNode *tc = m->first_child; tc;
                 tc = tc->next_sibling) {
              if (tc->kind == AST_TYPE_IDENT && tc->type) {
                elem_t = tc->type;
                break;
              }
            }
            if (!elem_t)
              for (ASTNode *tc = (ASTNode *)m->first_child; tc;
                   tc = tc->next_sibling) {
                if (tc->kind == AST_TYPE_IDENT) {
                  elem_t = resolve_type_annotation(ctx, tc);
                  if (elem_t)
                    break;
                }
              }
            break;
          }
        }
      }
    }
    if (elem_t) {
      if (base_is_dict || base_is_opt_chain)
        return (node->type = wrap_optional_result(elem_t, 1, ctx));
      return (node->type = elem_t);
    }
    return NULL;
  }
  default:
    return NULL;
  }
}
