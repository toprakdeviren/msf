/**
 * @file builder.c
 * @brief @resultBuilder AST transformation — transforms annotated function/closure
 *        bodies into buildBlock / buildOptional / buildEither / buildArray calls.
 */
#include "private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Synthetic AST Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Allocates a synthetic AST node with no token stamp. */
static ASTNode *synth_node(SemaContext *ctx, ASTNodeKind kind) {
  ASTNode *n = ast_arena_alloc(ctx->ast_arena);
  if (!n) return NULL;
  n->kind = kind;
  return n;
}

/** @brief Shallow-copies a node, clearing pointers so it can be re-parented. */
static ASTNode *clone_node(SemaContext *ctx, const ASTNode *src) {
  ASTNode *n = synth_node(ctx, src->kind);
  if (!n) return NULL;
  *n = *src;
  n->next_sibling = NULL;
  n->parent = NULL;
  return n;
}

/** @brief Appends a child to a synthetic CALL_EXPR (manual linked-list). */
static void call_append_arg(ASTNode *call, ASTNode *arg) {
  if (!arg) return;
  arg->parent = call;
  arg->next_sibling = NULL;
  call->last_child->next_sibling = arg;
  call->last_child = arg;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Builder Lookup
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Finds a registered @resultBuilder by interned name. */
static const BuilderEntry *find_builder(SemaContext *ctx, const char *name) {
  for (uint32_t i = 0; i < ctx->builder_count; i++)
    if (ctx->builder_types[i].name == name)
      return &ctx->builder_types[i];
  return NULL;
}

/**
 * @brief Checks if a node has a preceding @BuilderType attribute.
 *
 * Walks backwards through siblings to find an immediately preceding
 * AST_ATTRIBUTE and matches against registered @resultBuilder types.
 */
const BuilderEntry *node_get_builder(SemaContext *ctx, const ASTNode *node) {
  if (!node || !node->parent)
    return NULL;
  for (const ASTNode *sib = node->parent->first_child; sib;
       sib = sib->next_sibling) {
    if (sib->next_sibling != node)
      continue;
    if (sib->kind != AST_ATTRIBUTE)
      break;
    const Token *at = &ctx->tokens[sib->data.var.name_tok];
    const char *attr_name = sema_intern(ctx, ctx->src->data + at->pos, at->len);
    const BuilderEntry *be = find_builder(ctx, attr_name);
    if (be)
      return be;
    break;
  }
  return NULL;
}

/**
 * @brief Finds the token index of a static method inside the builder type.
 *
 * @return Token index of the method name, or 0 if not found.
 */
uint32_t builder_method_name_tok(const SemaContext *ctx, const BuilderEntry *be,
                                 const char *method_name) {
  if (!be || !be->decl)
    return 0;
  const ASTNode *bbody = NULL;
  for (const ASTNode *bc = be->decl->first_child; bc; bc = bc->next_sibling)
    if (bc->kind == AST_BLOCK) { bbody = bc; break; }
  if (!bbody)
    return 0;
  const char *src = ctx->src->data;
  for (const ASTNode *bm = bbody->first_child; bm; bm = bm->next_sibling) {
    if (bm->kind != AST_FUNC_DECL)
      continue;
    const Token *nt = &ctx->tokens[bm->data.func.name_tok];
    size_t nlen = nt->len;
    if (nlen && method_name && strncmp(src + nt->pos, method_name, nlen) == 0 &&
        method_name[nlen] == '\0')
      return (uint32_t)bm->data.func.name_tok;
  }
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Builder Call Construction
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Builds a synthetic BuilderType.methodName(arg) call node. */
static ASTNode *make_builder_call(SemaContext *ctx, const BuilderEntry *be,
                                  uint32_t method_tok, ASTNode *arg) {
  ASTNode *base = synth_node(ctx, AST_IDENT_EXPR);
  if (!base) return arg;
  base->tok_idx = be->decl->data.var.name_tok;

  ASTNode *callee = synth_node(ctx, AST_MEMBER_EXPR);
  if (!callee) return arg;
  callee->first_child = base;
  callee->data.var.name_tok = method_tok;

  ASTNode *call = synth_node(ctx, AST_CALL_EXPR);
  if (!call) return arg;
  call->first_child = callee;
  call->last_child = arg;
  callee->next_sibling = arg;
  arg->parent = call;
  arg->next_sibling = NULL;
  return call;
}

/** @brief Wraps expr in BuilderType.buildExpression(expr) if available. */
ASTNode *wrap_in_build_expression(SemaContext *ctx, const BuilderEntry *be,
                                  ASTNode *expr_node) {
  if (!be->build_expression)
    return expr_node;
  uint32_t tok = builder_method_name_tok(ctx, be, SW_BUILDER_BUILD_EXPRESSION);
  if (!tok)
    return expr_node;
  return make_builder_call(ctx, be, tok, expr_node);
}

/** @brief Wraps inner in BuilderType.methodName(inner). */
ASTNode *wrap_builder_method_call(SemaContext *ctx, const BuilderEntry *be,
                                  const char *method_name, ASTNode *inner) {
  if (!inner || !method_name)
    return inner;
  uint32_t tok = builder_method_name_tok(ctx, be, method_name);
  if (!tok)
    return inner;
  return make_builder_call(ctx, be, tok, inner);
}

/**
 * @brief Builds a BuilderType.buildBlock(callee) CALL_EXPR with no args yet.
 *
 * Returns the call node; use call_append_arg() to add arguments.
 * Returns NULL if the builder has no buildBlock or the method isn't found.
 */
static ASTNode *make_build_block_call(SemaContext *ctx, const BuilderEntry *be) {
  uint32_t tok = builder_method_name_tok(ctx, be, SW_BUILDER_BUILD_BLOCK);
  if (!tok) return NULL;

  ASTNode *base = synth_node(ctx, AST_IDENT_EXPR);
  if (!base) return NULL;
  base->tok_idx = be->decl->data.var.name_tok;

  ASTNode *callee = synth_node(ctx, AST_MEMBER_EXPR);
  if (!callee) return NULL;
  callee->first_child = base;
  callee->data.var.name_tok = tok;

  ASTNode *call = synth_node(ctx, AST_CALL_EXPR);
  if (!call) return NULL;
  call->first_child = callee;
  call->last_child = callee;
  return call;
}

/**
 * @brief Clones an expression statement's child, optionally wrapping in buildExpression.
 *
 * Returns NULL for return statements and non-expression nodes.
 */
static ASTNode *transform_expr_stmt(SemaContext *ctx, const BuilderEntry *be,
                                    const ASTNode *c) {
  const ASTNode *expr = c->first_child;
  if (!expr) return NULL;
  ASTNode *arg = clone_node(ctx, expr);
  if (!arg) return NULL;
  if (be->build_expression) {
    ASTNode *wrapped = wrap_in_build_expression(ctx, be, arg);
    if (wrapped != arg) arg = wrapped;
  }
  return arg;
}

/**
 * @brief Transforms an if/else into buildOptional or buildEither calls.
 *
 * - if without else → buildOptional(buildBlock(...))
 * - if with else    → buildEither(then) + buildEither(else), both appended to call
 *
 * Returns the argument node, or NULL if both branches were directly appended.
 */
static ASTNode *transform_if_stmt(SemaContext *ctx, const BuilderEntry *be,
                                  const ASTNode *c, ASTNode *call) {
  const ASTNode *then_blk = NULL, *else_blk = NULL;
  for (const ASTNode *ch = c->first_child; ch; ch = ch->next_sibling) {
    if (ch->kind == AST_BLOCK) {
      if (!then_blk) then_blk = ch;
      else { else_blk = ch; break; }
    }
  }

  /* if without else → buildOptional */
  if (then_blk && !else_blk && be->build_optional) {
    ASTNode *inner = build_block_call_from_stmts(ctx, be, then_blk->first_child);
    if (inner)
      return wrap_builder_method_call(ctx, be, SW_BUILDER_BUILD_OPTIONAL, inner);
  }

  /* if with else → buildEither for each branch */
  if (then_blk && else_blk && be->build_either) {
    ASTNode *then_inner = build_block_call_from_stmts(ctx, be, then_blk->first_child);
    ASTNode *else_inner = build_block_call_from_stmts(ctx, be, else_blk->first_child);
    if (then_inner)
      call_append_arg(call, wrap_builder_method_call(ctx, be, SW_BUILDER_BUILD_EITHER, then_inner));
    if (else_inner)
      call_append_arg(call, wrap_builder_method_call(ctx, be, SW_BUILDER_BUILD_EITHER, else_inner));
    return NULL; /* both branches already appended */
  }

  /* Fallback: keep if-stmt as-is */
  return clone_node(ctx, c);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Builds BuilderType.buildBlock(s1, s2, ...) from a statement list.
 *
 * Each expression statement becomes an argument.  Return statements are skipped.
 * If buildExpression is available, each argument is wrapped in it.
 */
ASTNode *build_block_call_from_stmts(SemaContext *ctx, const BuilderEntry *be,
                                     const ASTNode *first_stmt) {
  if (!be->build_block || !first_stmt)
    return NULL;
  ASTNode *call = make_build_block_call(ctx, be);
  if (!call) return NULL;

  for (const ASTNode *c = first_stmt; c; c = c->next_sibling) {
    if (c->kind == AST_EXPR_STMT)
      call_append_arg(call, transform_expr_stmt(ctx, be, c));
  }
  return call;
}

/**
 * @brief Transforms an entire @resultBuilder-annotated function body.
 *
 * Statement dispatch:
 *   - AST_EXPR_STMT   → buildBlock argument (optionally wrapped in buildExpression)
 *   - AST_RETURN_STMT → skipped
 *   - AST_IF_STMT     → buildOptional / buildEither
 *   - AST_FOR_STMT    → buildArray (or fallback copy)
 *   - Final result    → buildFinalResult (if available)
 */
ASTNode *transform_builder_body(SemaContext *ctx, const BuilderEntry *be,
                                const ASTNode *body_block) {
  if (!body_block || !be->build_block)
    return NULL;
  ASTNode *call = make_build_block_call(ctx, be);
  if (!call) return NULL;

  for (const ASTNode *c = body_block->first_child; c; c = c->next_sibling) {
    ASTNode *arg = NULL;

    switch (c->kind) {
    case AST_EXPR_STMT:
      arg = transform_expr_stmt(ctx, be, c);
      break;

    case AST_RETURN_STMT:
      continue;

    case AST_IF_STMT:
      arg = transform_if_stmt(ctx, be, c, call);
      break;

    case AST_FOR_STMT:
      if (be->build_array)
        arg = wrap_builder_method_call(ctx, be, SW_BUILDER_BUILD_ARRAY, (ASTNode *)c);
      if (!arg)
        arg = clone_node(ctx, c);
      break;

    default:
      break;
    }

    call_append_arg(call, arg);
  }

  if (be->build_final)
    call = wrap_builder_method_call(ctx, be, SW_BUILDER_BUILD_FINAL, call);
  return call;
}
