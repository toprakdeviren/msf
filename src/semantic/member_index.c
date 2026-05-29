/**
 * @file member_index.c
 * @brief Lazily-built, per-decl name→member index for member-access resolution.
 *
 * `base.member` resolution used to linear-scan a type's whole body on every
 * access (see resolve_member_expr).  On machine-generated Swift — one
 * `enum Components { enum Schemas { struct A; struct B; ... } }` namespace with
 * thousands of nested decls, referenced thousands of times — that is O(n^2)
 * and dominates analysis time (a 3.6 MB OpenAPI `Types.swift` spent ~1.5 s
 * almost entirely here).
 *
 * This builds a small open-addressed hash (name pointer → member node) once
 * per type decl and caches it on the SemaContext.  Names are interned, so the
 * key is a pointer compare and a lookup miss — the common case, since most
 * such accesses name a nested *type* rather than a stored/computed member — is
 * O(1) instead of a full-body walk.
 */
#include "private.h"

/* Next power of two ≥ n (min 16). */
static uint32_t next_pow2(uint32_t n) {
  uint32_t p = 16;
  while (p < n) p <<= 1;
  return p;
}

/* Mix an interned-name pointer into a table slot. */
static uint32_t slot_of(const char *name, uint32_t cap) {
  uintptr_t p = (uintptr_t)name;
  uint32_t h = (uint32_t)((p >> 4) ^ (p >> 13) ^ (p >> 23));
  return h & (cap - 1);
}

/* Insert (name → member), first-wins: a name already present is left alone so
 * the index reports the first member declared under it (matching the original
 * declaration-order scan). */
static void idx_put(MemberIndex *mi, const char *name, ASTNode *member) {
  if (!name) return;
  uint32_t i = slot_of(name, mi->cap);
  while (mi->slots[i].name) {
    if (mi->slots[i].name == name) return;  /* keep first */
    i = (i + 1) & (mi->cap - 1);
  }
  mi->slots[i].name = name;
  mi->slots[i].member = member;
  mi->count++;
}

/* Walk @p start's sibling chain, indexing the same member kinds (and using the
 * same name token per kind) that resolve_member_expr's scan matched. */
static void idx_build(SemaContext *ctx, MemberIndex *mi, const ASTNode *start) {
  for (const ASTNode *ch = start; ch; ch = ch->next_sibling) {
    if (ch->kind == AST_VAR_DECL || ch->kind == AST_LET_DECL) {
      idx_put(mi, tok_intern(ctx, ch->tok_idx), (ASTNode *)ch);
    } else if (ch->kind == AST_FUNC_DECL) {
      idx_put(mi, tok_intern(ctx, ch->data.func.name_tok), (ASTNode *)ch);
    } else if (ch->kind == AST_ATTRIBUTE && ch->next_sibling &&
               (ch->next_sibling->kind == AST_VAR_DECL ||
                ch->next_sibling->kind == AST_LET_DECL)) {
      const ASTNode *var = ch->next_sibling;
      idx_put(mi, tok_intern(ctx, var->data.var.name_tok), (ASTNode *)var);
    }
  }
}

/* ── Whole-module origin map: decl node → its source file's token stream ─────
 * In whole-module analysis a member-index is built lazily when a sibling file
 * first accesses the type — but at that point the *active* token stream is the
 * accessing file's, not the type's.  We record, during the declare phase, which
 * file each decl came from, and switch ctx to it around the build so member
 * names are interned from the right tokens.  Empty (and inert) in single-file
 * mode. */
#define ORIGIN_BUCKETS 256u
typedef struct SemaOrigin {
  const ASTNode *node;
  const Source  *src;
  const Token   *tokens;
  uint32_t       token_count;
  const ASTNode *ast_root;
  struct SemaOrigin *next;
} SemaOrigin;

void sema_origin_register(SemaContext *ctx, const ASTNode *node, const Source *src,
                          const Token *tokens, uint32_t token_count,
                          const ASTNode *ast_root) {
  if (!ctx || !node) return;
  if (!ctx->origin_map) {
    ctx->origin_map = calloc(ORIGIN_BUCKETS, sizeof(SemaOrigin *));
    if (!ctx->origin_map) return;
  }
  SemaOrigin **buckets = (SemaOrigin **)ctx->origin_map;
  uint32_t b = slot_of((const char *)node, ORIGIN_BUCKETS);
  SemaOrigin *o = calloc(1, sizeof(SemaOrigin));
  if (!o) return;
  o->node = node;
  o->src = src;
  o->tokens = tokens;
  o->token_count = token_count;
  o->ast_root = ast_root;
  o->next = buckets[b];
  buckets[b] = o;
}

static const SemaOrigin *origin_find_exact(SemaContext *ctx, const ASTNode *node) {
  if (!ctx->origin_map) return NULL;
  SemaOrigin **buckets = (SemaOrigin **)ctx->origin_map;
  for (SemaOrigin *o = buckets[slot_of((const char *)node, ORIGIN_BUCKETS)]; o; o = o->next)
    if (o->node == node)
      return o;
  return NULL;
}

static const SemaOrigin *origin_find(SemaContext *ctx, const ASTNode *node) {
  for (const ASTNode *n = node; n; n = n->parent) {
    const SemaOrigin *o = origin_find_exact(ctx, n);
    if (o) return o;
  }
  return NULL;
}

static void tok_cache_clear(SemaContext *ctx) {
  if (ctx && ctx->tok_cache && ctx->tok_cache_cap)
    memset(ctx->tok_cache, 0,
           (size_t)ctx->tok_cache_cap * sizeof(*ctx->tok_cache));
}

int sema_origin_enter(SemaContext *ctx, const ASTNode *node,
                      SemaOriginState *state) {
  if (!state)
    return 0;
  memset(state, 0, sizeof(*state));
  if (!ctx || !node)
    return 0;

  const SemaOrigin *o = origin_find(ctx, node);
  if (!o)
    return 0;
  if (ctx->src == o->src && ctx->tokens == o->tokens &&
      ctx->token_count == o->token_count)
    return 0;

  state->src = ctx->src;
  state->tokens = ctx->tokens;
  state->token_count = ctx->token_count;
  state->ast_root = ctx->ast_root;
  state->switched = 1;

  tok_cache_clear(ctx);
  ctx->src = o->src;
  ctx->tokens = o->tokens;
  ctx->token_count = o->token_count;
  if (o->ast_root)
    ctx->ast_root = o->ast_root;
  tok_cache_clear(ctx);
  return 1;
}

void sema_origin_leave(SemaContext *ctx, const SemaOriginState *state) {
  if (!ctx || !state || !state->switched)
    return;
  tok_cache_clear(ctx);
  ctx->src = state->src;
  ctx->tokens = state->tokens;
  ctx->token_count = state->token_count;
  ctx->ast_root = state->ast_root;
}

const char *tok_intern_at_node(SemaContext *ctx, const ASTNode *node,
                               uint32_t tok_idx) {
  SemaOriginState st;
  sema_origin_enter(ctx, node, &st);
  const char *name = tok_intern(ctx, tok_idx);
  sema_origin_leave(ctx, &st);
  return name;
}

void sema_origin_free(SemaContext *ctx) {
  if (!ctx || !ctx->origin_map) return;
  SemaOrigin **buckets = (SemaOrigin **)ctx->origin_map;
  for (uint32_t b = 0; b < ORIGIN_BUCKETS; b++) {
    SemaOrigin *it = buckets[b];
    while (it) {
      SemaOrigin *nx = it->next;
      free(it);
      it = nx;
    }
  }
  free(buckets);
  ctx->origin_map = NULL;
}

/* Invoke @p fn once per registered file root (whole-module: every file in the
 * module; single file: its one root), with ctx switched to that root's token
 * stream for the duration of the call.  Only file roots are registered, so
 * iterating the origin map yields exactly the module's files.  Lets a
 * module-wide index intern names from each file's own tokens.  If no origins
 * were registered, falls back to the current ctx->ast_root. */
void sema_origin_for_each_root(SemaContext *ctx,
                               void (*fn)(SemaContext *, const ASTNode *, void *),
                               void *user) {
  if (!ctx || !fn) return;
  if (!ctx->origin_map) {
    if (ctx->ast_root) fn(ctx, ctx->ast_root, user);
    return;
  }
  SemaOrigin **buckets = (SemaOrigin **)ctx->origin_map;
  for (uint32_t b = 0; b < ORIGIN_BUCKETS; b++) {
    for (SemaOrigin *o = buckets[b]; o; o = o->next) {
      if (!o->node) continue;
      SemaOriginState st;
      int sw = sema_origin_enter(ctx, o->node, &st);
      fn(ctx, o->node, user);
      if (sw) sema_origin_leave(ctx, &st);
    }
  }
}

ASTNode *sema_member_lookup(SemaContext *ctx, const ASTNode *decl,
                            const ASTNode *body, const char *mname) {
  if (!ctx || !decl || !mname) return NULL;

  uint32_t b = slot_of((const char *)decl, MEMBER_INDEX_BUCKETS);
  MemberIndex *mi = NULL;
  for (MemberIndex *it = ctx->member_index[b]; it; it = it->next) {
    if (it->decl == decl) { mi = it; break; }
  }

  if (!mi) {
    const ASTNode *start = body ? body->first_child : decl->first_child;
    uint32_t n = 0;
    for (const ASTNode *ch = start; ch; ch = ch->next_sibling) n++;
    mi = calloc(1, sizeof(MemberIndex));
    if (!mi) return NULL;
    mi->decl = decl;
    mi->cap = next_pow2(n * 2 + 1);
    mi->slots = calloc(mi->cap, sizeof(MemberSlot));
    if (!mi->slots) { free(mi); return NULL; }

    /* Build with the decl's *own* file tokens.  In whole-module mode `decl`
     * may be from a sibling file whose stream is not the active one; switch to
     * it (a no-op map → no-op in single-file mode). */
    SemaOriginState st;
    if (sema_origin_enter(ctx, decl, &st)) {
      idx_build(ctx, mi, start);
      sema_origin_leave(ctx, &st);
    } else {
      idx_build(ctx, mi, start);
    }

    mi->next = ctx->member_index[b];
    ctx->member_index[b] = mi;
  }

  uint32_t i = slot_of(mname, mi->cap);
  while (mi->slots[i].name) {
    if (mi->slots[i].name == mname) return mi->slots[i].member;
    i = (i + 1) & (mi->cap - 1);
  }
  return NULL;
}

void sema_member_index_free(SemaContext *ctx) {
  if (!ctx) return;
  for (uint32_t b = 0; b < MEMBER_INDEX_BUCKETS; b++) {
    MemberIndex *it = ctx->member_index[b];
    while (it) {
      MemberIndex *nx = it->next;
      free(it->slots);
      free(it);
      it = nx;
    }
    ctx->member_index[b] = NULL;
  }
  /* Conformance type-name presence set (see member.c). */
  free(ctx->conf_name_set);
  ctx->conf_name_set = NULL;
  ctx->conf_name_cap = ctx->conf_name_built = 0;
}
