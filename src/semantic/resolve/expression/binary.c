/**
 * @file binary.c
 * @brief Binary expression resolution: assignment, comparison, arithmetic,
 *        nil coalescing (??), logical operators.
 */
#include "../../private.h"

#define RANGE_EXPR_PREFIX (1u << 16)
#define RANGE_EXPR_POSTFIX (1u << 17)
#define RANGE_EXPR_UNBOUNDED (1u << 18)

static TypeInfo *make_range_expr_type(SemaContext *ctx, const char *name) {
  TypeInfo *t = type_arena_alloc(ctx->type_arena);
  if (!t)
    return TY_BUILTIN_INT;
  t->kind = TY_NAMED;
  t->named.name = name;
  t->named.decl = NULL;
  return t;
}

/* ─── Helper: binary expression type resolution ─────────────────────────────── */
TypeInfo *resolve_binary_expr(SemaContext *ctx, ASTNode *node) {
  ASTNode *lhs = node->first_child;
  ASTNode *rhs = lhs ? lhs->next_sibling : NULL;
  TypeInfo *lt = resolve_node(ctx, lhs);
  TypeInfo *rt = resolve_node(ctx, rhs);

  if (node->data.binary.op_tok) {
    const Token *op = &ctx->tokens[node->data.binary.op_tok];
    const char *op_str = ctx->src->data + op->pos;
    size_t op_len = op->len;

    if ((op_len == 3 && memcmp(op_str, "...", 3) == 0) ||
        (op_len == 3 && memcmp(op_str, "..<", 3) == 0)) {
      int exclusive = memcmp(op_str, "..<", 3) == 0;
      const char *name = exclusive ? "Range" : "ClosedRange";
      int prefix = (node->modifiers & RANGE_EXPR_PREFIX) != 0;
      int postfix = (node->modifiers & RANGE_EXPR_POSTFIX) != 0 ||
                    (!prefix && !exclusive && lhs && !rhs);
      if ((node->modifiers & RANGE_EXPR_UNBOUNDED) ||
          (!node->first_child && !exclusive))
        name = "UnboundedRange";
      else if (prefix)
        name = exclusive ? "PartialRangeUpTo" : "PartialRangeThrough";
      else if (postfix)
        name = "PartialRangeFrom";
      return (node->type = make_range_expr_type(ctx, name));
    }

    if (op_len == 1 && *op_str == '=') {
      if (lhs && lhs->kind == AST_IDENT_EXPR) {
        const char *iname = tok_intern(ctx, lhs->tok_idx);
        Symbol *sym = sema_lookup(ctx, iname);
        if (sym && sym->kind == SYM_LET) {
          if (!sym->is_initialized) {
            sym->is_initialized = 1;
            if (!sym->type && rt)
              sym->type = rt;
          } else {
            sema_error(ctx, lhs,
                       "Cannot assign to value: '%s' is a 'let' constant",
                       iname);
          }
        }
      }
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
            (lt->kind == TY_ARRAY || lt->kind == TY_SET ||
             lt->kind == TY_DICT)) {
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
          if ((is_int_float_mix(lt, rt) && !int_literal_adapts(lhs, lt, rhs, rt))) {
            sema_error(ctx, rhs,
                       "Cannot assign value of type '%s' to type '%s'; "
                       "use explicit conversion",
                       rt_s, lt_s);
          } else {
            sema_error(ctx, rhs,
                       "Type mismatch: expected '%s', got '%s'", lt_s,
                       rt_s);
          }
        }
      }
      return (node->type = lt);
    }

    int is_compound =
        (op_len == 2 && op_str[0] != '=' &&
         (memcmp(op_str, "+=", 2) == 0 || memcmp(op_str, "-=", 2) == 0 ||
          memcmp(op_str, "*=", 2) == 0 || memcmp(op_str, "/=", 2) == 0 ||
          memcmp(op_str, "%=", 2) == 0));
    if (is_compound) {
      if (lhs && lhs->kind == AST_IDENT_EXPR) {
        const char *iname = tok_intern(ctx, lhs->tok_idx);
        const Symbol *sym = sema_lookup(ctx, iname);
        if (sym && sym->kind == SYM_LET && sym->is_initialized &&
            !sym->is_deferred)
          sema_error(ctx, lhs,
                     "Cannot assign to value: '%s' is a 'let' constant", iname);
      }
      if (lt && rt && (is_int_float_mix(lt, rt) && !int_literal_adapts(lhs, lt, rhs, rt))) {
        char lt_s[64], rt_s[64];
        type_to_string(lt, lt_s, sizeof(lt_s));
        type_to_string(rt, rt_s, sizeof(rt_s));
        char op_buf[8] = {0};
        memcpy(op_buf, op_str, op_len < 7 ? op_len : 7);
        sema_error(ctx, node,
                   "Binary operator '%.*s' cannot be applied to operands "
                   "of type '%s' and '%s'",
                   (int)op_len, op_str, lt_s, rt_s);
      }
      return (node->type = TY_BUILTIN_VOID);
    }

    int is_comparison =
        (op_len == 3 &&
         (memcmp(op_str, "===", 3) == 0 || memcmp(op_str, "!==", 3) == 0)) ||
        (op_len == 2 &&
         (memcmp(op_str, "==", 2) == 0 || memcmp(op_str, "!=", 2) == 0 ||
          memcmp(op_str, "<=", 2) == 0 || memcmp(op_str, ">=", 2) == 0)) ||
        (op_len == 1 && (*op_str == '<' || *op_str == '>'));
    if (is_comparison) {
      if ((is_int_float_mix(lt, rt) && !int_literal_adapts(lhs, lt, rhs, rt))) {
        char lt_s[64], rt_s[64];
        type_to_string(lt, lt_s, sizeof(lt_s));
        type_to_string(rt, rt_s, sizeof(rt_s));
        char op_buf[8] = {0};
        memcpy(op_buf, op_str, op_len < 7 ? op_len : 7);
        sema_error(ctx, node,
                   "Binary operator '%s' cannot be applied to operands "
                   "of type '%s' and '%s'",
                   op_buf, lt_s, rt_s);
      }
      return (node->type = TY_BUILTIN_BOOL);
    }
    if (op_len == 2 &&
        (memcmp(op_str, "&&", 2) == 0 || memcmp(op_str, "||", 2) == 0))
      return (node->type = TY_BUILTIN_BOOL);
    if (op_len == 2 && memcmp(op_str, "??", 2) == 0) {
      if (!lt || lt->kind != TY_OPTIONAL || !lt->inner) {
        if (lt && lt->kind != TY_UNKNOWN)
          sema_error(ctx, node, "left side of '\?'\?' must be Optional type");
        return (node->type = rt ? rt : lt);
      }
      if (rt && !type_equal(lt->inner, rt) && rt->kind != TY_UNKNOWN &&
          lt->inner->kind != TY_UNKNOWN &&
          !literal_coerces_to(ctx, rhs, lt->inner)) {
        char ls[64], rs[64];
        type_to_string(lt->inner, ls, sizeof(ls));
        type_to_string(rt, rs, sizeof(rs));
        sema_error(ctx, node,
                   "right side of '\?'\?' has type '%s', expected '%s' "
                   "(unwrapped optional type)",
                   rs, ls);
      }
      return (node->type = lt->inner);
    }
    if (op_len == 1 && *op_str == '+' && lt &&
        lt->kind == TY_BUILTIN_STRING->kind)
      return (node->type = lt);
    int is_arithmetic =
        (op_len == 1 && (*op_str == '+' || *op_str == '-' || *op_str == '*' ||
                         *op_str == '/' || *op_str == '%'));
    int lt_is_meas = (lt && lt->kind == TY_NAMED && lt->named.name && strcmp(lt->named.name, "Measurement") == 0);
    int rt_is_meas = (rt && rt->kind == TY_NAMED && rt->named.name && strcmp(rt->named.name, "Measurement") == 0);
    if (is_arithmetic && (lt_is_meas || rt_is_meas)) {
      return (node->type = lt_is_meas ? lt : rt);
    }
    if (is_arithmetic && (is_int_float_mix(lt, rt) && !int_literal_adapts(lhs, lt, rhs, rt))) {
      char lt_s[64], rt_s[64];
      type_to_string(lt, lt_s, sizeof(lt_s));
      type_to_string(rt, rt_s, sizeof(rt_s));
      char op_buf[4] = {0};
      op_buf[0] = *op_str;
      sema_error(ctx, node,
                 "Binary operator '%s' cannot be applied to operands "
                 "of type '%s' and '%s'",
                 op_buf, lt_s, rt_s);
    }
  }
  if (lt && rt && type_equal(lt, rt))
    return (node->type = lt);
  return (node->type = lt ? lt : rt);
}
