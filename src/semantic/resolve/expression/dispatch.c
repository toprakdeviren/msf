/**
 * @file dispatch.c
 * @brief Expression resolution: literals, identifiers, binary, unary,
 *        assignment, call, member, cast, ternary, subscript, key path.
 */
#include "../../private.h"

static int node_is_within_decl(const ASTNode *node, const ASTNode *decl) {
  for (const ASTNode *p = node; p; p = p->parent)
    if (p == decl)
      return 1;
  return 0;
}

static const char *dispatch_named_family_name(TypeInfo *ty) {
  if (!ty)
    return NULL;
  if (ty->kind == TY_NAMED)
    return ty->named.name;
  if (ty->kind == TY_GENERIC_INST && ty->generic.base &&
      ty->generic.base->kind == TY_NAMED)
    return ty->generic.base->named.name;
  return NULL;
}

static int dispatch_type_is_range_family(TypeInfo *ty) {
  const char *name = dispatch_named_family_name(ty);
  return name &&
         (strcmp(name, "Range") == 0 ||
          strcmp(name, "ClosedRange") == 0 ||
          strcmp(name, "PartialRangeFrom") == 0 ||
          strcmp(name, "PartialRangeThrough") == 0 ||
          strcmp(name, "PartialRangeUpTo") == 0 ||
          strcmp(name, "UnboundedRange") == 0);
}

/* ─── Expression case dispatcher (called from resolve_node) ─── */
TypeInfo *resolve_node_expr(SemaContext *ctx, ASTNode *node) {
  /* Consume any contextual type set by a set-site (var/let init).  It governs
   * only THIS node — clear it up front so it can't leak into nested
   * sub-expressions (e.g. operands of a binary `+`). */
  TypeInfo *want = ctx->expected_type;
  ctx->expected_type = NULL;

  switch (node->kind) {
  case AST_INTEGER_LITERAL:
    /* A literal adopts the contextual type when that type can be built from it
     * (ExpressibleByIntegerLiteral); otherwise it defaults to Int. */
    if (want && literal_coerces_to(ctx, node, want)) return (node->type = want);
    return (node->type = TY_BUILTIN_INT);
  case AST_FLOAT_LITERAL:
    if (want && literal_coerces_to(ctx, node, want)) return (node->type = want);
    return (node->type = TY_BUILTIN_DOUBLE);
  case AST_STRING_LITERAL:
    resolve_children(ctx, node);
    if (want && literal_coerces_to(ctx, node, want)) return (node->type = want);
    return (node->type = TY_BUILTIN_STRING);
  case AST_REGEX_LITERAL:
    resolve_children(ctx, node);
    return (node->type =
                TY_BUILTIN_STRING); /* pattern as String until Regex type */
  case AST_BOOL_LITERAL:
    if (want && literal_coerces_to(ctx, node, want)) return (node->type = want);
    return (node->type = TY_BUILTIN_BOOL);
  case AST_NIL_LITERAL:
    if (want && want->kind == TY_OPTIONAL) return (node->type = want);
    return NULL;
  case AST_ARRAY_LITERAL: {
    TypeInfo *elem_t = NULL;
    int mixed = 0;
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      TypeInfo *et = resolve_node(ctx, c);
      if (!elem_t) {
        elem_t = et;
      } else if (et && !type_equal(elem_t, et)) {
        /* Heterogeneous element types fall back to [Any]
         * so contextual `let xs: [Any] = [1, "x"]` works. miniswift IR-gen
         * boxes each element via __any_box_* based on per-element type. */
        mixed = 1;
      }
    }
    TypeInfo *ti = type_arena_alloc(ctx->type_arena);
    ti->kind = TY_ARRAY;
    if (mixed) {
      TypeInfo *any_t = type_arena_alloc(ctx->type_arena);
      if (any_t) {
        any_t->kind = TY_NAMED;
        any_t->named.name = sema_intern(ctx, SW_TYPE_ANY,
                                        sizeof(SW_TYPE_ANY) - 1);
      }
      ti->inner = any_t;
    } else {
      ti->inner = elem_t;
    }
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
      if (!sym->type && sym->decl && sym->decl->kind == AST_PARAM) {
        sym->type = TY_BUILTIN_INT;
        sym->decl->type = sym->type;
      }
      if (!sym->type && sym->decl) {
        if (sym->is_resolving) {
          if (node_is_within_decl(node, sym->decl))
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
    /* Type-mismatch check.  Carve-outs match resolve_binary_expr's `=` path
     * so the parser switching `=` from AST_BINARY_EXPR to AST_ASSIGN_EXPR
     * doesn't change diagnostics. */
    if (lt && rt && !type_equal(lt, rt) && lt->kind != TY_UNKNOWN &&
        rt->kind != TY_UNKNOWN && lt->kind != TY_PROTOCOL_COMPOSITION &&
        lt->kind != TY_TUPLE && rt->kind != TY_TUPLE &&
        !int_literal_adapts(lhs, lt, rhs, rt) &&
        !literal_coerces_to(ctx, rhs, lt) &&
        !func_to_func_assign(lt, rt) &&
        !assign_target_is_any(lt) &&
        !subtype_assignable(ctx, lt, rt) &&
        !(lt->kind == TY_OPTIONAL && lt->inner &&
          type_equal(lt->inner, rt))) {
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
        if (is_int_float_mix(lt, rt) && !int_literal_adapts(lhs, lt, rhs, rt)) {
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
    if (root_type && root_type->kind == TY_GENERIC_INST && root_type->generic.base &&
        root_type->generic.base->kind == TY_NAMED && root_type->generic.base->named.name &&
        (strcmp(root_type->generic.base->named.name, "KeyPath") == 0 ||
         strcmp(root_type->generic.base->named.name, "WritableKeyPath") == 0 ||
         strcmp(root_type->generic.base->named.name, "ReferenceWritableKeyPath") == 0 ||
         strcmp(root_type->generic.base->named.name, "PartialKeyPath") == 0) &&
        root_type->generic.arg_count >= 1) {
      root_type = root_type->generic.args[0];
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
      /* data.binary.op_tok is only meaningful for kinds that store a
       * binary-style payload (BINARY / ASSIGN / CAST / UNARY).  Reading it
       * for any other kind aliases an unrelated union slot — e.g. a CALL
       * with a captures pointer or a MEMBER with a name_tok — and the
       * resulting token index points past the stream.  Whitelist the kinds
       * before touching it. */
      int has_op_tok = (c->kind == AST_BINARY_EXPR ||
                        c->kind == AST_ASSIGN_EXPR ||
                        c->kind == AST_CAST_EXPR   ||
                        c->kind == AST_UNARY_EXPR);
      if (has_op_tok && c->data.binary.op_tok != 0) {
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
  case AST_TRY_EXPR: {
    TypeInfo *inner_t = resolve_node(ctx, node->first_child);
    /* try? wraps the result in Optional<T>; the parser stamps MOD_WEAK
     * on the AST_TRY_EXPR node when the user wrote `try?`. Without this
     * wrap, downstream code (most visibly `??`) sees a bare T and rejects
     * the LHS as non-Optional. `try!` and bare `try` keep the inner
     * result type — failure paths trap or propagate. */
    if (inner_t && (node->modifiers & MOD_WEAK)) {
      TypeInfo *opt = type_arena_alloc(ctx->type_arena);
      if (opt) {
        opt->kind = TY_OPTIONAL;
        opt->inner = inner_t;
        return (node->type = opt);
      }
    }
    return (node->type = inner_t);
  }
  case AST_CONSUME_EXPR:
    return (node->type = resolve_node(ctx, node->first_child));
  case AST_AWAIT_EXPR: {
    if (!ctx->current_function_async) {
      sema_error(ctx, node,
                 "'await' is only allowed inside an async function or closure");
    }
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
    if (node->modifiers & MOD_WEAK)
      return (node->type = wrap_optional_result(cast_t, 1, ctx));
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
    ASTNode *index = base ? base->next_sibling : NULL;
    TypeInfo *index_t = index ? resolve_node(ctx, index) : NULL;
    TypeInfo *unwrapped = base_t;
    if (base_t && base_t->kind == TY_OPTIONAL)
      unwrapped = base_t->inner;
    int base_is_opt_chain = (base && base->kind == AST_OPTIONAL_CHAIN);
    int base_is_dict = (unwrapped && unwrapped->kind == TY_DICT);
    TypeInfo *elem_t = NULL;
    if (unwrapped && unwrapped->kind == TY_ARRAY &&
        dispatch_type_is_range_family(index_t))
      return (node->type = unwrapped);
    if (unwrapped && (type_kind_of(unwrapped) == TY_STRING ||
                      type_kind_of(unwrapped) == TY_SUBSTRING) &&
        dispatch_type_is_range_family(index_t))
      return (node->type = TY_BUILTIN_SUBSTRING);
    if (unwrapped && unwrapped->kind == TY_ARRAY)
      elem_t = unwrapped->inner;
    else if (base_is_dict)
      elem_t = unwrapped->dict.value;
    else if (unwrapped && unwrapped->kind == TY_NAMED &&
             unwrapped->named.name &&
             (strcmp(unwrapped->named.name, "IndexPath") == 0 ||
              strcmp(unwrapped->named.name, "IndexSet") == 0))
      elem_t = TY_BUILTIN_INT;
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
