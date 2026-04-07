/**
 * @file resolver.c
 * @brief Top-level AST node resolution dispatcher, conformance checking,
 *        closure capture analysis, and sema_analyze entry point.
 */

#include "../private.h"

/* Forward declarations — implemented in expression/dispatch.c and declaration.c */
TypeInfo *resolve_node_expr(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_node_decl(SemaContext *ctx, ASTNode *node);

TypeInfo *resolve_node(SemaContext *ctx, ASTNode *node) {
  if (!node)
    return NULL;

  /* Don't re-resolve */
  if (node->type)
    return node->type;

  switch (node->kind) {

  /* ── Expression/literal kinds → resolve_node_expr */
  case AST_INTEGER_LITERAL:
  case AST_FLOAT_LITERAL:
  case AST_STRING_LITERAL:
  case AST_REGEX_LITERAL:
  case AST_BOOL_LITERAL:
  case AST_NIL_LITERAL:
    return resolve_node_expr(ctx, node);

  /*
   * Generic param node
   * ────────────────────────────────────────── AST_GENERIC_PARAM nodes are
   * already added to scope as TY_GENERIC_PARAM in pass1. Here we just
   * return the type pointer.
   */
  case AST_GENERIC_PARAM: {
    if (node->type)
      return node->type;
    /*
     * Build TY_GENERIC_PARAM from token and constraint children (<T: Proto>,
     * <T: ~Copyable>)
     */
    const char *pname = tok_intern(ctx, node->tok_idx);
    TypeInfo *gp_ti = type_arena_alloc(ctx->type_arena);
    gp_ti->kind = TY_GENERIC_PARAM;
    gp_ti->param.name = pname;
    gp_ti->param.index = 0;
    gp_ti->param.constraints = NULL;
    gp_ti->param.constraint_count = 0;
    for (const ASTNode *tc = node->first_child; tc; tc = tc->next_sibling) {
      const ASTNode *constraint_type = tc;
      if (tc->kind == AST_CONFORMANCE && tc->first_child)
        constraint_type = tc->first_child;
      TypeInfo *ct = resolve_type_annotation(ctx, (ASTNode *)constraint_type);
      if (!ct)
        continue;
      const char *proto_name =
          (ct->kind == TY_NAMED && ct->named.name) ? ct->named.name : NULL;
      uint32_t n = gp_ti->param.constraint_count;
      TypeConstraint *new_c =
          realloc(gp_ti->param.constraints, (n + 1) * sizeof(TypeConstraint));
      if (!new_c)
        continue;
      gp_ti->param.constraints = new_c;
      if (tc->kind == AST_CONFORMANCE &&
          (tc->modifiers & MOD_SUPPRESSED_CONFORMANCE)) {
        gp_ti->param.constraints[n].kind = TC_SUPPRESSED;
        gp_ti->param.constraints[n].protocol_name = proto_name;
      } else {
        gp_ti->param.constraints[n].kind = TC_CONFORMANCE;
        gp_ti->param.constraints[n].protocol_name = proto_name;
      }
      gp_ti->param.constraints[n].rhs_type = NULL;
      gp_ti->param.constraints[n].assoc_name = NULL;
      gp_ti->param.constraints[n].rhs_param_name = NULL;
      gp_ti->param.constraints[n].rhs_assoc_name = NULL;
      gp_ti->param.constraint_count++;
    }
    return (node->type = gp_ti);
  }

  /*
   * Where clause ──────────────────────────────────────────────
   * Resolve the BINARY_EXPR children of WHERE_CLAUSE.
   * Each child is a conformance/same-type requirement.
   * Result: error info if any; return type is void-equivalent.
   */
  case AST_WHERE_CLAUSE: {
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      if (c->kind != AST_BINARY_EXPR)
        continue;
      ASTNode *lhs = c->first_child;
      const ASTNode *rhs = lhs ? lhs->next_sibling : NULL;
      TypeInfo *lhs_t = lhs ? resolve_node(ctx, lhs) : NULL;
      TypeInfo *rhs_t = rhs ? resolve_type_annotation(ctx, rhs) : NULL;
      /* Conformance requirement (where T: Protocol) — add TC_CONFORMANCE */
      if (c->data.binary.op_tok == 0 && lhs_t &&
          lhs_t->kind == TY_GENERIC_PARAM && rhs_t && rhs_t->kind == TY_NAMED &&
          rhs_t->named.name) {
        uint32_t n = lhs_t->param.constraint_count;
        TypeConstraint *new_c =
            realloc(lhs_t->param.constraints, (n + 1) * sizeof(TypeConstraint));
        if (new_c) {
          lhs_t->param.constraints = new_c;
          lhs_t->param.constraints[n].kind = TC_CONFORMANCE;
          lhs_t->param.constraints[n].protocol_name = rhs_t->named.name;
          lhs_t->param.constraints[n].rhs_type = NULL;
          lhs_t->param.constraints[n].assoc_name = NULL;
          lhs_t->param.constraints[n].rhs_param_name = NULL;
          lhs_t->param.constraints[n].rhs_assoc_name = NULL;
          lhs_t->param.constraint_count++;
        }
      }
      /*
       * Same-type requirement (where T == Int) — add TC_SAME_TYPE to
       * generic param
       */
      if (c->data.binary.op_tok != 0 && lhs_t &&
          lhs_t->kind == TY_GENERIC_PARAM && rhs_t) {
        uint32_t n = lhs_t->param.constraint_count;
        TypeConstraint *new_c =
            realloc(lhs_t->param.constraints, (n + 1) * sizeof(TypeConstraint));
        if (new_c) {
          lhs_t->param.constraints = new_c;
          lhs_t->param.constraints[n].kind = TC_SAME_TYPE;
          lhs_t->param.constraints[n].protocol_name = NULL;
          lhs_t->param.constraints[n].rhs_type = rhs_t;
          lhs_t->param.constraints[n].assoc_name = NULL;
          lhs_t->param.constraints[n].rhs_param_name = NULL;
          lhs_t->param.constraints[n].rhs_assoc_name = NULL;
          lhs_t->param.constraint_count++;
        }
      }
      /* T.Item == U.Item — add TC_SAME_TYPE_ASSOC to LHS generic param */
      if (c->data.binary.op_tok != 0 && lhs_t && lhs_t->kind == TY_ASSOC_REF &&
          rhs_t && rhs_t->kind == TY_ASSOC_REF) {
        const char *left_param = lhs_t->assoc_ref.param_name;
        Symbol *sym = sema_lookup(ctx, left_param);
        if (sym && sym->type && sym->type->kind == TY_GENERIC_PARAM) {
          TypeInfo *param_ti = sym->type;
          uint32_t n = param_ti->param.constraint_count;
          TypeConstraint *new_c = realloc(param_ti->param.constraints,
                                          (n + 1) * sizeof(TypeConstraint));
          if (new_c) {
            param_ti->param.constraints = new_c;
            param_ti->param.constraints[n].kind = TC_SAME_TYPE_ASSOC;
            param_ti->param.constraints[n].protocol_name = NULL;
            param_ti->param.constraints[n].rhs_type = NULL;
            param_ti->param.constraints[n].assoc_name =
                lhs_t->assoc_ref.assoc_name;
            param_ti->param.constraints[n].rhs_param_name =
                rhs_t->assoc_ref.param_name;
            param_ti->param.constraints[n].rhs_assoc_name =
                rhs_t->assoc_ref.assoc_name;
            param_ti->param.constraint_count++;
          }
        }
      }
    }
    return NULL;
  }

  case AST_ARRAY_LITERAL:
  case AST_DICT_LITERAL:
  case AST_IDENT_EXPR:
    return resolve_node_expr(ctx, node);

  /* ── Declaration kinds → resolve_node_decl */
  case AST_VAR_DECL:
  case AST_LET_DECL:
  case AST_PARAM:
  case AST_FUNC_DECL:
  case AST_INIT_DECL:
  case AST_DEINIT_DECL:
  case AST_STRUCT_DECL:
  case AST_ENUM_DECL:
  case AST_PROTOCOL_DECL:
  case AST_CLASS_DECL:
  case AST_EXTENSION_DECL:
  case AST_ACTOR_DECL:
  case AST_SUBSCRIPT_DECL:
  case AST_BLOCK:
    return resolve_node_decl(ctx, node);

  /* ── Expr kinds (binary through subscript) → resolve_node_expr */
  case AST_BINARY_EXPR:
  case AST_UNARY_EXPR:
  case AST_ASSIGN_EXPR:
  case AST_CALL_EXPR:
  case AST_MEMBER_EXPR:
  case AST_TUPLE_EXPR:
  case AST_PAREN_EXPR:
  case AST_OPTIONAL_CHAIN:
  case AST_FORCE_UNWRAP:
  case AST_TRY_EXPR:
  case AST_AWAIT_EXPR:
  case AST_CONSUME_EXPR:
  case AST_CAST_EXPR:
  case AST_TERNARY_EXPR:
  case AST_SUBSCRIPT_EXPR:
  case AST_KEY_PATH_EXPR:
    return resolve_node_expr(ctx, node);

  /*
   * ── Closure expression
   * ───────────────────────────────────────────────────────
   */
  case AST_CLOSURE_EXPR: {
    TypeInfo *expected = ctx->expected_closure_type;

    sema_push_scope(ctx);
    TypeInfo *last_t = NULL;
    uint32_t param_idx = 0;
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      if (c->kind == AST_CLOSURE_CAPTURE) {
        last_t = resolve_node(ctx, c);
        continue;
      }
      if (c->kind == AST_PARAM) {
        TypeInfo *ti = NULL;
        if (expected && expected->kind == TY_FUNC && expected->func.params &&
            param_idx < expected->func.param_count && !c->first_child) {
          ti = expected->func.params[param_idx];
          c->type = ti;
          const char *iname = tok_intern(ctx, c->data.var.name_tok);
          sema_define(ctx, iname, SYM_PARAM, ti, c);
        } else
          ti = resolve_node(ctx, c);
        param_idx++;
        last_t = ti;
        continue;
      }
      last_t = resolve_node(ctx, c);
    }
    sema_pop_scope(ctx);

    expected = ctx->expected_closure_type;

    TypeInfo *ti = type_arena_alloc(ctx->type_arena);
    ti->kind = TY_FUNC;
    ti->func.ret = last_t;
    if (expected && expected->kind == TY_FUNC &&
        expected->func.param_count > 0 && expected->func.params) {
      ti->func.param_count = expected->func.param_count;
      ti->func.params = expected->func.params;
    } else {
      ti->func.param_count = 0;
      ti->func.params = NULL;
    }

    CaptureList *captures = malloc(sizeof(CaptureList));
    identify_captures(node, ctx, captures);
    node->data.closure.captures = captures;

    return (node->type = ti);
  }

  /*
   * ── Closure capture list element
   * ───────────────────────────────────────────────────────
   */
  case AST_CLOSURE_CAPTURE: {
    const char *iname = tok_intern(ctx, node->tok_idx);
    TypeInfo *ty = NULL;
    if (node->first_child) {
      /* `[x = expr]` */
      ty = resolve_node(ctx, node->first_child);
    } else {
      /* `[weak self]` or `[x]` */
      Symbol *sym = sema_lookup(ctx, iname);
      if (sym)
        ty = sym->type;
    }
    /* Define the captured variable in the closure's local scope */
    sema_define(ctx, iname, SYM_LET, ty, node);
    return ty;
  }

  /*
   * ── Expr statement
   * ───────────────────────────────────────────────────────────
   */
  case AST_EXPR_STMT:
    return resolve_node(ctx, node->first_child);

  case AST_DISCARD_STMT:
    return resolve_node(ctx, node->first_child);

  /*
   * ── For statement
   * ─────────────────────────────────────────────────────────────
   */
  case AST_FOR_STMT: {
    sema_push_scope(ctx);
    ASTNode *pat = node->first_child;
    ASTNode *seq = pat ? pat->next_sibling : NULL;
    ASTNode *where_guard =
        (seq && seq->next_sibling && seq->next_sibling->kind != AST_BLOCK)
            ? seq->next_sibling
            : NULL;
    ASTNode *body = node->last_child; /* brace_stmt is always last */

    if (seq) {
      TypeInfo *seq_t = resolve_node(ctx, seq);
      if (pat && pat->kind == AST_PARAM) {
        pat->type = get_sequence_element_type(ctx, seq_t);
        resolve_node(ctx, pat); /* bind to scope */
      } else if (pat) {
        resolve_node(ctx, pat);
      }
    }
    if (where_guard)
      resolve_node(ctx, where_guard);
    if (body)
      resolve_node(ctx, body);

    sema_pop_scope(ctx);
    return (node->type = TY_BUILTIN_VOID);
  }

  /*
   * ── Switch statement — G1.3: each clause in its own scope
   * ─────────────────────────────────────────────────────────
   */
  case AST_SWITCH_STMT: {
    /* Resolve subject (first child) */
    ASTNode *subj = node->first_child;
    if (subj)
      resolve_node(ctx, subj);
    /* Each case clause gets its own scope so bindings don't leak between arms */
    for (ASTNode *clause = subj ? subj->next_sibling : node->first_child;
         clause; clause = clause->next_sibling) {
      if (clause->kind != AST_CASE_CLAUSE)
        continue;
      resolve_node(ctx, clause);
    }
    return NULL;
  }

  case AST_CASE_CLAUSE: {
    /*
     * Open a fresh scope for each arm: 'let v' in one arm doesn't conflict
     * with 'let v' in another arm.
     */
    sema_push_scope(ctx);
    /* Resolve patterns (leading PATTERN_* children) */
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      resolve_node(ctx, c);
    }
    /* Resolve where guard (stored in data.cas.where_expr, not a child) */
    if (node->data.cas.has_guard && node->data.cas.where_expr)
      resolve_node(ctx, node->data.cas.where_expr);
    sema_pop_scope(ctx);
    return NULL;
  }

  /*
   * ── If statement — scoped optional bindings ──────────────────────────────
   * `if let x = opt { ... }` and `if cond, let y = opt2 { ... }`
   * All condition elements (including optional bindings) live in a scope
   * that spans both the condition list and the then-block.  The else-block
   * gets its own separate scope.
   */
  case AST_IF_STMT: {
    sema_push_scope(ctx);
    /* Resolve conditions + then-block in the same scope so that bindings
       introduced by `let x = opt` are visible inside the then-block. */
    ASTNode *else_block = NULL;
    int block_count = 0;
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      if (c->kind == AST_BLOCK) {
        block_count++;
        if (block_count == 2) {
          else_block = c; /* second block is the else body */
          continue;       /* resolve outside this scope */
        }
      }
      resolve_node(ctx, c);
    }
    sema_pop_scope(ctx);
    /* Else block in its own scope (bindings from the if-condition are not visible). */
    if (else_block) {
      sema_push_scope(ctx);
      resolve_node(ctx, else_block);
      sema_pop_scope(ctx);
    }
    return (node->type = TY_BUILTIN_VOID);
  }

  /*
   * ── Guard statement — bindings visible after the guard ──────────────────
   * `guard let x = opt else { return }` — x is visible in the enclosing
   * scope (not just inside the guard body), so we do NOT push a new scope.
   * The else-block is resolved in its own scope.
   */
  case AST_GUARD_STMT: {
    ASTNode *else_block = NULL;
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      if (c->kind == AST_BLOCK) {
        else_block = c;
        continue; /* resolve in separate scope below */
      }
      resolve_node(ctx, c); /* bindings go into the enclosing scope */
    }
    if (else_block) {
      sema_push_scope(ctx);
      resolve_node(ctx, else_block);
      sema_pop_scope(ctx);
    }
    return (node->type = TY_BUILTIN_VOID);
  }

  /*
   * ── Optional binding — `if let x = expr` or `guard let x = expr` ───────
   * Unwraps the init expression's optional type and defines `x` with the
   * inner type in the current scope.
   */
  case AST_OPTIONAL_BINDING: {
    const char *bname = tok_intern(ctx, node->data.var.name_tok);
    /* Resolve type annotation if present. */
    const ASTNode *tann = find_type_child(node);
    TypeInfo *annot_t = tann ? resolve_type_annotation(ctx, tann) : NULL;
    /* Resolve init expression (the value being unwrapped). */
    const ASTNode *init = find_init_child(node);
    TypeInfo *init_t = init ? resolve_node(ctx, (ASTNode *)init) : NULL;
    /* Unwrap Optional: if init_t is T?, the binding type is T. */
    TypeInfo *bound_t = annot_t;
    if (!bound_t && init_t) {
      if (init_t->kind == TY_OPTIONAL && init_t->inner)
        bound_t = init_t->inner; /* unwrap */
      else
        bound_t = init_t; /* non-optional — use as-is */
    }
    node->type = bound_t;
    /* Register the bound variable in the current scope. */
    if (bname && *bname) {
      SymbolKind sk = node->data.var.is_computed ? SYM_VAR : SYM_LET;
      sema_define(ctx, bname, sk, bound_t, node);
    }
    return bound_t;
  }

  /*
   * ── While / repeat
   * ──────────────────────────────────────────────────────────
   */
  case AST_WHILE_STMT:
  case AST_REPEAT_STMT:
    sema_push_scope(ctx);
    resolve_children(ctx, node);
    sema_pop_scope(ctx);
    return NULL;

  /*
   * ── Type annotation nodes (used by var decls, not standalone)
   * ───────────────
   */
  case AST_TYPE_IDENT:
  case AST_TYPE_OPTIONAL:
  case AST_TYPE_ARRAY:
  case AST_TYPE_DICT:
  case AST_TYPE_FUNC:
  case AST_TYPE_TUPLE:
  case AST_TYPE_SOME:
  case AST_TYPE_ANY:
  case AST_TYPE_GENERIC:
  case AST_TYPE_INOUT:
  case AST_TYPE_COMPOSITION:
    return (node->type = resolve_type_annotation(ctx, node));

  case AST_OWNERSHIP_SPEC:
  case AST_THROWS_CLAUSE:
  case AST_ACCESSOR_DECL:
    return resolve_children(ctx, node);

  case AST_MACRO_EXPANSION:
    resolve_children(ctx, node);
    if (node->data.aux.name_tok) {
      const char *macro_name = tok_intern(ctx, node->data.aux.name_tok);
      if (macro_name) {
        if (strcmp(macro_name, "file") == 0 ||
            strcmp(macro_name, "function") == 0)
          return (node->type = TY_BUILTIN_STRING);
        if (strcmp(macro_name, "line") == 0)
          return (node->type = TY_BUILTIN_INT);
      }
    }
    return (node->type = NULL);

  case AST_AVAILABILITY_EXPR:
    resolve_children(ctx, node);
    return (node->type = TY_BUILTIN_BOOL);

  /*
   * ── G1.3: Pattern nodes — type checking for switch/if-case patterns
   * ─────────────────────────────────────────────────────────────────────────
   */
  case AST_PATTERN_WILDCARD:
    /* _ — matches anything; no scope binding; no meaningful type */
    return (node->type = TY_BUILTIN_BOOL); /* always-true predicate */

  case AST_PATTERN_VALUE_BINDING: {
    /*
     * let x / var x inside a pattern — bind in current clause scope.
     * Pattern bindings may share the same name across separate switch arms
     * (e.g. `case let v where v > 0:` and `case let v where v < 0:`).
     * Each arm has its own scope (pushed/popped in CASE_CLAUSE above), so
     * there is no true redeclaration — suppress the error by checking the
     * CURRENT scope only before defining.
     */
    const char *bname = tok_intern(ctx, node->data.var.name_tok);
    if (bname && *bname) {
      /*
       * Only define if not already in the CURRENT (arm) scope —
       * use sema_lookup limited to current scope depth to avoid false
       * positives.
       */
      const Scope *cur = ctx->current_scope;
      int already = 0;
      if (cur) {
        uint32_t h = sym_hash(bname);
        for (Symbol *sym = cur->buckets[h]; sym; sym = sym->next)
          if (sym->name == bname) {
            already = 1;
            break;
          }
      }
      if (!already)
        sema_define(ctx, bname, SYM_VAR, NULL, node);
    }
    return (node->type = NULL);
  }

  case AST_PATTERN_ENUM: {
    /*
     * .caseName[(bindings)] — resolve each binding child; assign types from
     * enum case params when in switch/if-case. Validate arity: pattern
     * binding count must match enum case associated value count.
     */
    ASTNode *subj = NULL;
    if (node->parent && node->parent->kind == AST_CASE_CLAUSE &&
        node->parent->parent && node->parent->parent->kind == AST_SWITCH_STMT)
      subj = node->parent->parent->first_child;
    else if (node->parent && node->parent->kind == AST_ASSIGN_EXPR &&
             node->parent->first_child == node)
      subj = node->next_sibling;
    if (subj && subj->type && subj->type->kind == TY_NAMED &&
        subj->type->named.decl) {
      ASTNode *enum_decl = (ASTNode *)subj->type->named.decl;
      if (enum_decl->kind == AST_ENUM_DECL) {
        const char *case_name = tok_intern(ctx, node->data.var.name_tok);
        for (ASTNode *body = enum_decl->first_child; body;
             body = body->next_sibling) {
          if (body->kind != AST_BLOCK)
            continue;
          for (ASTNode *cd = body->first_child; cd; cd = cd->next_sibling) {
            if (cd->kind != AST_ENUM_CASE_DECL)
              continue;
            for (ASTNode *el = cd->first_child; el; el = el->next_sibling) {
              if (el->kind != AST_ENUM_ELEMENT_DECL)
                continue;
              const char *el_name = tok_intern(ctx, el->data.var.name_tok);
              if (el_name && case_name && strcmp(el_name, case_name) == 0) {
                uint32_t enum_arity = 0;
                for (ASTNode *p = el->first_child; p; p = p->next_sibling)
                  if (p->kind == AST_PARAM)
                    enum_arity++;
                uint32_t pattern_arity = 0;
                for (ASTNode *b = node->first_child; b; b = b->next_sibling)
                  pattern_arity++;
                if (enum_arity != pattern_arity) {
                  sema_error(ctx, node,
                             "enum case '.%s' has %u associated value(s), "
                             "pattern has %u subpattern(s)",
                             case_name, (unsigned)enum_arity,
                             (unsigned)pattern_arity);
                } else {
                  ASTNode *param = el->first_child;
                  for (ASTNode *child = node->first_child; child && param;
                       child = child->next_sibling,
                               param = param->next_sibling) {
                    if (param->kind != AST_PARAM || !param->first_child)
                      continue;
                    resolve_type_annotation(ctx, param->first_child);
                    if (child->kind == AST_PATTERN_VALUE_BINDING)
                      child->type = param->first_child->type;
                  }
                }
                break;
              }
            }
          }
        }
      }
    }
    for (ASTNode *c = node->first_child; c; c = c->next_sibling)
      resolve_node(ctx, c);
    return (node->type = TY_BUILTIN_BOOL); /* predicate: matched or not */
  }

  case AST_PATTERN_TUPLE: {
    /* (pat, pat, ...) — resolve each sub-pattern */
    for (ASTNode *c = node->first_child; c; c = c->next_sibling)
      resolve_node(ctx, c);
    return (node->type = TY_BUILTIN_BOOL);
  }

  case AST_PATTERN_TYPE: {
    /* is SomeType — result = Bool */
    const ASTNode *tn = node->first_child;
    if (tn)
      resolve_type_annotation(ctx, tn);
    return (node->type = TY_BUILTIN_BOOL);
  }

  case AST_PATTERN_RANGE: {
    /* lo...hi — resolve both bounds; result = Bool predicate */
    ASTNode *lo = node->first_child;
    ASTNode *hi = lo ? lo->next_sibling : NULL;
    if (lo)
      resolve_node(ctx, lo);
    if (hi)
      resolve_node(ctx, hi);
    return (node->type = TY_BUILTIN_BOOL);
  }

  case AST_PATTERN_GUARD: {
    /* pattern where expr */
    ASTNode *inner = node->first_child;
    ASTNode *guard = inner ? inner->next_sibling : NULL;
    if (inner)
      resolve_node(ctx, inner);
    if (guard)
      resolve_node(ctx, guard);
    return (node->type = TY_BUILTIN_BOOL);
  }

  case AST_PRECEDENCE_GROUP_DECL:
  case AST_OPERATOR_DECL:
  case AST_PG_HIGHER_THAN:
  case AST_PG_LOWER_THAN:
  case AST_PG_ASSOCIATIVITY:
  case AST_PG_ASSIGNMENT:
  case AST_PG_GROUP_REF:
    return NULL;

  /*
   * ── Default: visit children
   * ─────────────────────────────────────────────────
   */
  default:
    return resolve_children(ctx, node);
  }
}

/*
 * ─── Public entry point
 * ───────────────────────────────────────────────────────
 */
int sema_analyze(SemaContext *ctx, ASTNode *root) {
  /* Initialize conformance table and register builtins */
  static ConformanceTable s_ctable;
  if (!ctx->conformance_table) {
    memset(&s_ctable, 0, sizeof(s_ctable));
    conformance_table_init_builtins(&s_ctable);
    ctx->conformance_table = &s_ctable;
  }
  /* Associated type bindings (typealias Item = X in conforming type) */
  static AssocTypeTable s_atable;
  if (!ctx->assoc_type_table) {
    memset(&s_atable, 0, sizeof(s_atable));
    ctx->assoc_type_table = &s_atable;
  }

  /* Collect precedence group names so operator decls can reference them */
  ctx->pg_count = 0;
  for (ASTNode *c = root->first_child; c; c = c->next_sibling)
    if (c->kind == AST_PRECEDENCE_GROUP_DECL)
      sema_add_precedence_group_name(ctx, c);

  /* Pass 1: forward declarations */
  for (ASTNode *c = root->first_child; c; c = c->next_sibling)
    declare_node(ctx, c);

  /* Pass 2: type resolution */
  for (ASTNode *c = root->first_child; c; c = c->next_sibling)
    resolve_node(ctx, c);

  /* Pass 3 — protocol conformance checking */
  pass3_check_conformances(ctx, root);

  return ctx->error_count == 0 ? 0 : 1;
}

/*
 * check_conformance
 * ─────────────────────────────────────────────
 */
int check_conformance(const ASTNode *type_decl, const ASTNode *proto_decl,
                      SemaContext *ctx, const ASTNode *ast_root) {
  if (!type_decl || !proto_decl || !ctx)
    return 1;

  const ASTNode *type_body = NULL;
  for (const ASTNode *c = type_decl->first_child; c; c = c->next_sibling)
    if (c->kind == AST_BLOCK) {
      type_body = c;
      break;
    }
  if (!type_body)
    return 1;

  const Token *pt = &ctx->tokens[proto_decl->data.var.name_tok];
  const char *proto_name = sema_intern(ctx, ctx->src->data + pt->pos, pt->len);
  const Token *tt = &ctx->tokens[type_decl->data.var.name_tok];
  const char *type_name = sema_intern(ctx, ctx->src->data + tt->pos, tt->len);
  int all_ok = 1;

  /*
   * Inherited requirements — protocol P: P1, P2 { } → type must satisfy
   * P1 and P2 too
   */
  for (const ASTNode *c = proto_decl->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_CONFORMANCE)
      continue;
    for (const ASTNode *inh = c->first_child; inh; inh = inh->next_sibling) {
      const char *pname = NULL;
      if (inh->kind == AST_TYPE_IDENT && inh->tok_idx != 0) {
        const Token *t = &ctx->tokens[inh->tok_idx];
        pname = sema_intern(ctx, ctx->src->data + t->pos, t->len);
      }
      if (!pname)
        continue;
      /*
       * protocol P: AnyObject { } — class-bound protocol; only classes
       * may conform
       */
      if (strcmp(pname, SW_TYPE_ANY_OBJECT) == 0) {
        if (type_decl->kind != AST_CLASS_DECL) {
          sema_error(
              ctx, (ASTNode *)type_decl,
              "Non-class type '%s' cannot conform to class-bound protocol '%s'",
              type_name, proto_name);
          all_ok = 0;
        }
        continue;
      }
      const Symbol *ps = sema_lookup(ctx, pname);
      if (!ps || !ps->decl || ps->decl->kind != AST_PROTOCOL_DECL)
        continue;
      if (ps->decl == proto_decl)
        continue;
      if (!check_conformance(type_decl, ps->decl, ctx, ast_root))
        all_ok = 0;
    }
  }

  for (const ASTNode *req = proto_decl->first_child; req;
       req = req->next_sibling) {
    if (req->kind != AST_PROTOCOL_REQ || req->tok_idx == 0)
      continue;
    const Token *rt = &ctx->tokens[req->tok_idx];
    if (rt->len == 0)
      continue;
    const char *req_name = sema_intern(ctx, ctx->src->data + rt->pos, rt->len);
    if (!strcmp(req_name, "associatedtype") || !strcmp(req_name, "typealias") ||
        !strcmp(req_name, "protocol"))
      continue;

    int found = 0, is_init = !strcmp(req_name, "init");
    int req_is_assoc = protocol_req_is_associated_type(req);
    int req_is_property = protocol_req_is_property(req, req_name);
    const ASTNode *found_init_impl = NULL;
    int req_init_failable = is_init && (req->modifiers & MOD_FAILABLE);
    for (const ASTNode *impl = type_body->first_child; impl;
         impl = impl->next_sibling) {
      if (is_init) {
        if (impl->kind == AST_INIT_DECL) {
          int impl_failable =
              (impl->modifiers & MOD_FAILABLE) ||
              (impl->modifiers & MOD_IMPLICITLY_UNWRAPPED_FAILABLE);
          if (req_init_failable && !impl_failable)
            continue; /* init? req needs init?/init! */
          if (!req_init_failable && impl_failable)
            continue; /* init req needs non-failable init */
          found = 1;
          found_init_impl = impl;
          break;
        }
        continue;
      }
      const char *iname = NULL;
      if (impl->kind == AST_FUNC_DECL) {
        const Token *it = &ctx->tokens[impl->data.func.name_tok];
        iname = sema_intern(ctx, ctx->src->data + it->pos, it->len);
      } else if (impl->kind == AST_VAR_DECL || impl->kind == AST_LET_DECL) {
        const Token *it = &ctx->tokens[impl->data.var.name_tok];
        iname = sema_intern(ctx, ctx->src->data + it->pos, it->len);
      } else if (impl->kind == AST_TYPEALIAS_DECL) {
        const Token *it = &ctx->tokens[impl->data.var.name_tok];
        iname = sema_intern(ctx, ctx->src->data + it->pos, it->len);
      }
      if (iname && (iname == req_name || strcmp(iname, req_name) == 0)) {
        /* Associated type requirements must be satisfied by a typealias */
        if (req_is_assoc && impl->kind != AST_TYPEALIAS_DECL)
          continue;
        /*
         * Conformance check (property exists) — property requirement must
         * be implemented by var/let
         */
        if (req_is_property && impl->kind != AST_VAR_DECL &&
            impl->kind != AST_LET_DECL) {
          /* not a property impl */
          continue;
        }
        /*
         * static var in protocol — requirement and impl must both be
         * static or both instance
         */
        if (req_is_property &&
            (req->modifiers & MOD_STATIC) != (impl->modifiers & MOD_STATIC)) {
          continue;
        }
        /* var x: T { get set } — impl must be var (writable), not let */
        if (req_is_property && (req->modifiers & MOD_PROTOCOL_PROP_SET)) {
          if (impl->kind == AST_LET_DECL) {
            sema_error(ctx, (ASTNode *)impl,
                       "protocol requirement 'var %s { get set }' must be "
                       "implemented by 'var', not 'let'",
                       req_name);
            all_ok = 0;
          }
        }
        found = 1;
        /*
         * Record associated type binding (typealias Item = X in
         * conforming type) and enforce constraint (associatedtype Item :
         * Protocol)
         */
        if (impl->kind == AST_TYPEALIAS_DECL && ctx->assoc_type_table &&
            impl->first_child && impl->first_child->type) {
          TypeInfo *concrete_ty = impl->first_child->type;
          char buf[64];
          type_to_string(concrete_ty, buf, sizeof(buf));
          const char *concrete = sema_intern(ctx, buf, (size_t)strlen(buf));
          assoc_type_table_add(ctx->assoc_type_table, type_name, proto_name,
                               req_name, concrete);
          /*
           * Constraints on associated types — require concrete type to
           * conform to constraint protocol
           */
          if (req->first_child && ctx->conformance_table) {
            TypeInfo *constraint_ty =
                resolve_type_annotation(ctx, (ASTNode *)req->first_child);
            const Symbol *proto_sym =
                constraint_ty && constraint_ty->kind == TY_NAMED &&
                        constraint_ty->named.name
                    ? sema_lookup(ctx, constraint_ty->named.name)
                    : NULL;
            if (constraint_ty && constraint_ty->kind == TY_NAMED &&
                constraint_ty->named.name && proto_sym &&
                proto_sym->kind == SYM_PROTOCOL) {
              const char *concrete_name =
                  (concrete_ty->kind == TY_NAMED && concrete_ty->named.name)
                      ? concrete_ty->named.name
                      : concrete;
              if (!conformance_table_has(ctx->conformance_table, concrete_name,
                                         constraint_ty->named.name)) {
                char want[64], got[64];
                type_to_string(constraint_ty, want, sizeof(want));
                type_to_string(concrete_ty, got, sizeof(got));
                sema_error(ctx, (ASTNode *)impl,
                           "type '%s' does not conform to protocol '%s' "
                           "(associated type '%s' constraint)",
                           got, want, req_name);
                all_ok = 0;
              }
            }
          }
        }
        /*
         * Associated type with opaque return — requirement -> some P:
         * impl return must conform to P
         */
        if (impl->kind == AST_FUNC_DECL && impl->type &&
            impl->type->kind == TY_FUNC && ctx->conformance_table) {
          const ASTNode *req_ret_node = protocol_req_return_type_node(req);
          if (req_ret_node && req_ret_node->kind == AST_TYPE_SOME) {
            const TypeInfo *constraint =
                resolve_type_annotation(ctx, (ASTNode *)req_ret_node);
            if (constraint && constraint->kind == TY_NAMED &&
                constraint->named.name) {
              const TypeInfo *impl_ret = impl->type->func.ret;
              if (impl_ret && impl_ret->kind == TY_NAMED &&
                  impl_ret->named.name) {
                if (!conformance_table_has(ctx->conformance_table,
                                           impl_ret->named.name,
                                           constraint->named.name)) {
                  char want[64], got[64];
                  type_to_string(constraint, want, sizeof(want));
                  type_to_string(impl_ret, got, sizeof(got));
                  sema_error(ctx, (ASTNode *)impl,
                             "implementation return type '%s' does not conform "
                             "to protocol requirement 'some %s'",
                             got, want);
                  all_ok = 0;
                }
              }
              /*
               * TY_GENERIC_PARAM with name "$OpqRet" = impl also has -> some P;
               * body already checked
               */
            }
          }
        }
        break;
      }
    }
    /*
     * required init enforcement — class conforming to protocol with init
     * must mark it required
     */
    if (found && is_init && type_decl->kind == AST_CLASS_DECL &&
        found_init_impl && !(found_init_impl->modifiers & MOD_REQUIRED)) {
      sema_error(ctx, (ASTNode *)found_init_impl,
                 "initializer in class '%s' conforming to protocol '%s' must "
                 "be marked 'required'",
                 type_name, proto_name);
      all_ok = 0;
    }
    /*
     * Associated type inference — infer from property that uses the
     * associated type
     */
    if (!found && find_type_child(req) && strcmp(req_name, "init") != 0 &&
        ctx->assoc_type_table) {
      for (const ASTNode *r2 = proto_decl->first_child; r2;
           r2 = r2->next_sibling) {
        if (r2->kind != AST_PROTOCOL_REQ || r2->tok_idx == 0 || r2 == req)
          continue;
        const Token *r2t = &ctx->tokens[r2->tok_idx];
        const char *r2_name =
            sema_intern(ctx, ctx->src->data + r2t->pos, r2t->len);
        const ASTNode *r2_ty = find_type_child(r2);
        if (!r2_ty || !type_ast_contains_assoc(r2_ty, ctx, req_name))
          continue;
        if (!protocol_req_is_property(r2, r2_name))
          continue;
        const ASTNode *impl2 = NULL;
        for (const ASTNode *i = type_body->first_child; i;
             i = i->next_sibling) {
          const char *iname = NULL;
          if (i->kind == AST_VAR_DECL || i->kind == AST_LET_DECL)
            iname = sema_intern(
                ctx, ctx->src->data + ctx->tokens[i->data.var.name_tok].pos,
                ctx->tokens[i->data.var.name_tok].len);
          if (iname && iname == r2_name) {
            impl2 = i;
            break;
          }
        }
        if (!impl2 || !impl2->type)
          continue;
        const TypeInfo *concrete =
            infer_concrete_at_assoc(r2_ty, impl2->type, req_name, ctx);
        if (concrete) {
          char buf[128];
          type_to_string(concrete, buf, sizeof(buf));
          const char *concrete_interned =
              sema_intern(ctx, buf, (size_t)strlen(buf));
          assoc_type_table_add(ctx->assoc_type_table, type_name, proto_name,
                               req_name, concrete_interned);
          found = 1;
          break;
        }
      }
    }
    /*
     * Synthesized conformance — allow Equatable/Hashable/Comparable
     * without explicit impl
     */
    if (!found && (type_decl->kind == AST_STRUCT_DECL ||
                   type_decl->kind == AST_ENUM_DECL)) {
      if ((strcmp(proto_name, SW_PROTO_EQUATABLE) == 0 &&
           strcmp(req_name, "==") == 0) ||
          (strcmp(proto_name, SW_PROTO_HASHABLE) == 0 &&
           (strcmp(req_name, "hash") == 0 ||
            strcmp(req_name, "hashValue") == 0)) ||
          (strcmp(proto_name, SW_PROTO_COMPARABLE) == 0 && strcmp(req_name, "<") == 0))
        found = 1;
    }
    /*
     * Protocol default implementation — requirement satisfied by
     * extension P { func f() {} }
     */
    if (!found && ast_root &&
        protocol_extension_has_default(ctx, ast_root, proto_name, req_name))
      found = 1;
    if (!found) {
      sema_error(
          ctx, (ASTNode *)type_decl,
          "'%s' does not implement requirement '%s' of protocol '%s'",
          type_name, proto_name, req_name);
      all_ok = 0;
    }
  }
  return all_ok;
}

/*
 * pass3_check_conformances
 * ──────────────────────────────────────
 */
void pass3_check_conformances(SemaContext *ctx, ASTNode *root) {
  if (!root || !ctx)
    return;
  for (ASTNode *node = root->first_child; node; node = node->next_sibling) {
    if (node->kind != AST_STRUCT_DECL && node->kind != AST_CLASS_DECL &&
        node->kind != AST_ENUM_DECL && node->kind != AST_ACTOR_DECL)
      continue;
    for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
      if (c->kind != AST_CONFORMANCE)
        continue;
      for (const ASTNode *pi = c->first_child; pi; pi = pi->next_sibling) {
        if (pi->tok_idx == 0)
          continue;
        const Token *ppt = &ctx->tokens[pi->tok_idx];
        const char *pname =
            sema_intern(ctx, ctx->src->data + ppt->pos, ppt->len);
        const Symbol *ps = sema_lookup(ctx, pname);
        if (!ps || !ps->decl || ps->decl->kind != AST_PROTOCOL_DECL)
          continue;
        check_conformance(node, ps->decl, ctx, root);
      }
    }
  }
}

/*
 * identify_captures / closure capture analysis
 * ────────────── Scans every IDENT_EXPR node inside a CLOSURE_EXPR node.
 * If the ident belongs to the enclosing scope (outer_ctx), add it to the
 * capture list.
 *
 * Capture list construction strategy:
 *   1. First collect names defined inside the closure (local scope)
 *   2. Scan IDENT_EXPRs — if not local, find via outer lookup
 *   3. Process AST_CLOSURE_CAPTURE nodes with qualifier info
 */

/* Collect definitions in the inner scope (VAR_DECL/LET_DECL/PARAM/FUNC_DECL names) */
void collect_local_names(const ASTNode *node, const char **names,
                         uint32_t *count, uint32_t max, SemaContext *ctx) {
  if (!node)
    return;
  const Token *toks = ctx->tokens;
  const char *src = ctx->src->data;
  uint32_t ni = 0;

  switch (node->kind) {
  case AST_VAR_DECL:
  case AST_LET_DECL:
    ni = node->data.var.name_tok;
    goto add_name;
  case AST_PARAM:
    ni = node->data.var.name_tok;
    goto add_name;
  case AST_FUNC_DECL:
    ni = node->data.func.name_tok;
    goto add_name;
  add_name:
    if (*count < max) {
      names[(*count)++] = sema_intern(ctx, src + toks[ni].pos, toks[ni].len);
    }
    break;
  default:
    break;
  }
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
    collect_local_names(c, names, count, max, ctx);
}

/* Deep-scan the AST, find IDENT_EXPR nodes */
void scan_idents(const ASTNode *node, const char **local_names,
                 uint32_t local_count, SemaContext *outer_ctx,
                 CaptureList *out) {
  if (!node || out->count >= CAPTURE_LIST_MAX)
    return;

  if (node->kind == AST_IDENT_EXPR) {
    const Token *t = &outer_ctx->tokens[node->tok_idx];
    const char *iname =
        sema_intern(outer_ctx, outer_ctx->src->data + t->pos, t->len);

    /* Is it a local name? */
    for (uint32_t i = 0; i < local_count; i++)
      if (local_names[i] == iname) {
        return;
      }

    /* Exists in outer scope? */
    Symbol *sym = sema_lookup(outer_ctx, iname);
    if (!sym)
      return; /* unknown — stdlib/builtin */

    /* Duplicate check */
    for (uint32_t i = 0; i < out->count; i++)
      if (out->captures[i].name == iname)
        return;

    /* Add to CaptureList — default strong */
    CaptureInfo *ci = &out->captures[out->count++];
    ci->name = iname;
    ci->mode = CAPTURE_STRONG;
    ci->type = sym->type;
    ci->decl = sym->decl;
    ci->is_outer = 1;
    return;
  }

  /* Skip closure EXPRs inside closures (nested capture analysis is separate) */
  if (node->kind == AST_CLOSURE_EXPR)
    return;

  for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
    scan_idents(c, local_names, local_count, outer_ctx, out);
}

int identify_captures(const ASTNode *closure_node, SemaContext *outer_ctx,
                      CaptureList *out) {
  if (!closure_node || !outer_ctx || !out)
    return 0;
  memset(out, 0, sizeof(*out));

  /* 1. Process explicitly specified capture list ([weak self, unowned x]) */
  for (const ASTNode *c = closure_node->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_CLOSURE_CAPTURE)
      continue;
    if (out->count >= CAPTURE_LIST_MAX)
      break;

    const Token *t = &outer_ctx->tokens[c->tok_idx];
    if (t->len == 0)
      continue;
    const char *cname =
        sema_intern(outer_ctx, outer_ctx->src->data + t->pos, t->len);

    /* Qualifier → CaptureMode */
    CaptureMode mode = CAPTURE_STRONG;
    if (c->modifiers & MOD_CAPTURE_WEAK)
      mode = CAPTURE_WEAK;
    if (c->modifiers & MOD_CAPTURE_UNOWNED)
      mode = CAPTURE_UNOWNED;

    Symbol *sym = sema_lookup(outer_ctx, cname);
    CaptureInfo *ci = &out->captures[out->count++];
    ci->name = cname;
    ci->mode = mode;
    /* [x = expr]: type from resolved init expr (c->type); else from outer sym */
    ci->type = c->type ? c->type : (sym ? sym->type : NULL);
    ci->decl = sym ? sym->decl : NULL;
    ci->is_outer = 1;
  }

  /* 2. Find implicit captures in the body */
  const char *local_names[CAPTURE_LIST_MAX];
  uint32_t local_count = 0;
  collect_local_names(closure_node, local_names, &local_count, CAPTURE_LIST_MAX,
                      outer_ctx);

  /*
   * Scan body children only (skip AST_CLOSURE_CAPTURE and AST_PARAM — those
   * are not body idents). scan_idents will recurse into the body subtree.
   */
  for (const ASTNode *c = closure_node->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_CLOSURE_CAPTURE || c->kind == AST_PARAM)
      continue;
    scan_idents(c, local_names, local_count, outer_ctx, out);
  }

  return (int)out->count;
}
