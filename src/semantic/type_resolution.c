/**
 * @file type_resolution.c
 * @brief Resolves AST type annotation nodes (AST_TYPE_*) into TypeInfo values.
 *
 * Each AST_TYPE_* kind has a dedicated resolver function.  These are wired
 * into a dispatch table (type_resolvers[]) that resolve_type_annotation()
 * indexes by node kind for O(1) dispatch.
 *
 * Resolver functions:
 *   resolve_type_ident()       — Int, MyType, A.B.C, T.Item
 *   resolve_type_optional()    — T?
 *   resolve_type_array()       — [T], [N of T]
 *   resolve_type_dict()        — [K: V]
 *   resolve_type_func()        — (A, B) async throws -> C
 *   resolve_type_tuple()       — (A, label: B)
 *   resolve_type_passthrough() — some T, any T, inout T
 *   resolve_type_generic()     — Array<T>, Dict<K,V>, Custom<T>
 *   resolve_type_composition() — P & Q
 */
#include "private.h"

/* Forward declarations for recursive calls. */
TypeInfo *resolve_type_annotation(SemaContext *ctx, const ASTNode *tnode);
TypeInfo *resolve_node(SemaContext *ctx, ASTNode *node);
const ASTNode *find_type_child(const ASTNode *decl);

/** @brief Function pointer type for per-kind type resolvers. */
typedef TypeInfo *(*TypeResolver)(SemaContext *ctx, const ASTNode *tnode);

/** @brief Allocates a TypeInfo with the given kind. Returns NULL on OOM. */
static TypeInfo *make_type(SemaContext *ctx, TypeKind kind) {
  TypeInfo *ti = type_arena_alloc(ctx->type_arena);
  if (ti) ti->kind = kind;
  return ti;
}

/* ── resolve_type_ident helpers ────────────────────────────────────────────── */

/** @brief Resolves T.Item → TY_ASSOC_REF (associated type on a generic param). */
static TypeInfo *resolve_assoc_type(SemaContext *ctx, const char *base_name,
                                    const ASTNode *member_node) {
  Symbol *sym = sema_lookup(ctx, base_name);
  TypeInfo *base_t = (sym && sym->type) ? sym->type : NULL;
  if (!base_t || base_t->kind != TY_GENERIC_PARAM)
    return NULL;
  TypeInfo *ti = make_type(ctx, TY_ASSOC_REF);
  ti->assoc_ref.param_name = base_t->param.name;
  ti->assoc_ref.assoc_name = tok_intern(ctx, member_node->tok_idx);
  return ti;
}

/** @brief Resolves A.B.C → TY_NAMED with a dot-joined qualified name. */
static TypeInfo *resolve_qualified_name(SemaContext *ctx, const ASTNode *tnode) {
  char qbuf[256];
  size_t off = 0;
  for (const ASTNode *cur = tnode;
       cur && cur->kind == AST_TYPE_IDENT && off < sizeof(qbuf) - 1;
       cur = cur->first_child) {
    const char *part = tok_intern(ctx, cur->tok_idx);
    if (off > 0) qbuf[off++] = '.';
    size_t plen = strlen(part);
    if (off + plen >= sizeof(qbuf)) break;
    memcpy(qbuf + off, part, plen + 1);
    off += plen;
  }
  if (off == 0) return NULL;
  TypeInfo *ti = make_type(ctx, TY_NAMED);
  if (!ti) return NULL;
  ti->named.name = sema_intern(ctx, qbuf, off);
  return ti;
}

/**
 * @brief Extra type aliases not in BUILTIN_TYPE_TABLE.
 *
 * These are type names that the backend maps to canonical types but that
 * aren't full builtin singletons.  Table-driven — one entry per alias.
 */
static const struct { const char *name; TypeInfo **target; } TYPE_ALIASES[] = {
    {SW_TYPE_NEVER,    &TY_BUILTIN_VOID},
    {"StaticString",   &TY_BUILTIN_STRING},
    {"Float16",        &TY_BUILTIN_FLOAT},
    {"Float80",        &TY_BUILTIN_DOUBLE},
    {NULL, NULL}
};

/** @brief Resolves extra aliases (Never→Void, StaticString→String, etc.). */
static TypeInfo *resolve_type_alias(const char *name) {
  for (int i = 0; TYPE_ALIASES[i].name; i++)
    if (strcmp(name, TYPE_ALIASES[i].name) == 0)
      return *TYPE_ALIASES[i].target;
  return NULL;
}

static int is_nominal_or_extension(const ASTNode *node) {
  return node &&
         (node->kind == AST_STRUCT_DECL || node->kind == AST_CLASS_DECL ||
          node->kind == AST_ENUM_DECL || node->kind == AST_ACTOR_DECL ||
          node->kind == AST_PROTOCOL_DECL || node->kind == AST_EXTENSION_DECL);
}

static int node_is_ancestor_of(const ASTNode *ancestor, const ASTNode *node) {
  for (const ASTNode *p = node; p; p = p->parent)
    if (p == ancestor)
      return 1;
  return 0;
}

TypeInfo *resolve_typealias_decl(SemaContext *ctx, const ASTNode *decl) {
  if (!decl)
    return NULL;
  if (decl->type)
    return decl->type;
  sema_push_scope(ctx);
  for (ASTNode *gp = ((ASTNode *)decl)->first_child; gp; gp = gp->next_sibling) {
    if (gp->kind != AST_GENERIC_PARAM)
      continue;
    TypeInfo *gp_t = resolve_node(ctx, gp);
    const char *gp_name = tok_intern(ctx, gp->tok_idx);
    if (gp_t && gp_name)
      sema_define(ctx, gp_name, SYM_TYPE, gp_t, gp);
  }
  TypeInfo *aliased = resolve_type_annotation(ctx, find_type_child(decl));
  sema_pop_scope(ctx);
  ((ASTNode *)decl)->type = aliased;
  /* Alias-visibility check (moved here from declare_typealias, which now defers
   * RHS resolution): runs once, on first resolution, with the aliased type
   * known. */
  if (aliased) {
    uint32_t aliased_eff = type_effective_access(ctx, aliased);
    if (access_rank(aliased_eff) <= access_rank(MOD_FILEPRIVATE) &&
        access_rank(effective_access(decl->modifiers)) > access_rank(aliased_eff))
      sema_error(ctx, (ASTNode *)decl,
                 "type alias cannot be more visible than the type it aliases");
  }
  return aliased;
}

static TypeInfo *resolve_typealias_from_body(SemaContext *ctx,
                                             const ASTNode *body,
                                             const ASTNode *site,
                                             const char *name) {
  if (!body)
    return NULL;
  for (const ASTNode *c = body->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_TYPEALIAS_DECL || !c->data.var.name_tok)
      continue;
    if (node_is_ancestor_of(c, site))
      continue;
    const char *alias_name = tok_intern(ctx, c->data.var.name_tok);
    if (alias_name == name)
      return resolve_typealias_decl(ctx, c);
  }
  return NULL;
}

static TypeInfo *resolve_nested_nominal_decl(SemaContext *ctx,
                                             const ASTNode *decl,
                                             const char *name) {
  if (!decl)
    return NULL;
  if (decl->type)
    return decl->type;
  TypeInfo *ti = make_type(ctx, TY_NAMED);
  if (!ti)
    return NULL;
  ti->named.name = name;
  ti->named.decl = (ASTNode *)decl;
  ((ASTNode *)decl)->type = ti;
  return ti;
}

static int is_nested_nominal_decl(const ASTNode *node) {
  return node &&
         (node->kind == AST_STRUCT_DECL || node->kind == AST_CLASS_DECL ||
          node->kind == AST_ENUM_DECL || node->kind == AST_PROTOCOL_DECL ||
          node->kind == AST_ACTOR_DECL);
}

static TypeInfo *resolve_nested_nominal_from_body(SemaContext *ctx,
                                                  const ASTNode *body,
                                                  const char *name) {
  if (!body)
    return NULL;
  for (const ASTNode *c = body->first_child; c; c = c->next_sibling) {
    if (!is_nested_nominal_decl(c) || !c->data.var.name_tok)
      continue;
    const char *decl_name = tok_intern(ctx, c->data.var.name_tok);
    if (decl_name == name)
      return resolve_nested_nominal_decl(ctx, c, name);
  }
  return NULL;
}

static TypeInfo *resolve_type_from_body(SemaContext *ctx, const ASTNode *body,
                                        const ASTNode *site,
                                        const char *name) {
  TypeInfo *found = resolve_typealias_from_body(ctx, body, site, name);
  if (found)
    return found;
  return resolve_nested_nominal_from_body(ctx, body, name);
}

typedef enum {
  NESTED_INDEX_TYPEALIAS,
  NESTED_INDEX_NOMINAL,
} NestedIndexKind;

typedef struct NestedTypeEntry {
  const char *owner;
  const char *name;
  const ASTNode *decl;
  NestedIndexKind kind;
  struct NestedTypeEntry *next;
} NestedTypeEntry;

typedef struct {
  const void *module_key; /* ctx->origin_map identity — one per module */
  NestedTypeEntry **buckets;
  uint32_t cap;
} NestedTypeIndex;

static uint32_t nested_hash_pair(const char *owner, const char *name,
                                 uint32_t cap) {
  uintptr_t a = (uintptr_t)owner, b = (uintptr_t)name;
  uint32_t h = (uint32_t)((a >> 4) ^ (a >> 15) ^ (b >> 7) ^ (b >> 19));
  return h & (cap - 1);
}

static void nested_index_put(NestedTypeIndex *idx, const char *owner,
                             const char *name, const ASTNode *decl,
                             NestedIndexKind kind) {
  if (!idx || !owner || !name || !decl)
    return;
  uint32_t b = nested_hash_pair(owner, name, idx->cap);
  NestedTypeEntry *e = calloc(1, sizeof(*e));
  if (!e)
    return;
  e->owner = owner;
  e->name = name;
  e->decl = decl;
  e->kind = kind;
  if (!idx->buckets[b]) {
    idx->buckets[b] = e;
    return;
  }
  NestedTypeEntry *tail = idx->buckets[b];
  while (tail->next)
    tail = tail->next;
  tail->next = e;
}

static void nested_index_add_body(SemaContext *ctx, NestedTypeIndex *idx,
                                  const char *owner, const ASTNode *body,
                                  NestedIndexKind kind) {
  if (!body)
    return;
  for (const ASTNode *c = body->first_child; c; c = c->next_sibling) {
    if (kind == NESTED_INDEX_TYPEALIAS) {
      if (c->kind != AST_TYPEALIAS_DECL || !c->data.var.name_tok)
        continue;
    } else if (!is_nested_nominal_decl(c) || !c->data.var.name_tok) {
      continue;
    }
    nested_index_put(idx, owner, tok_intern(ctx, c->data.var.name_tok), c, kind);
  }
}

void sema_nested_type_index_free(SemaContext *ctx) {
  if (!ctx || !ctx->nested_type_index)
    return;
  NestedTypeIndex *idx = (NestedTypeIndex *)ctx->nested_type_index;
  if (idx->buckets) {
    for (uint32_t b = 0; b < idx->cap; b++) {
      NestedTypeEntry *e = idx->buckets[b];
      while (e) {
        NestedTypeEntry *next = e->next;
        free(e);
        e = next;
      }
    }
  }
  free(idx->buckets);
  free(idx);
  ctx->nested_type_index = NULL;
}

/* Adds one file root's top-level type/extension bodies to the nested-type
 * index.  Invoked per file by sema_origin_for_each_root with that file's token
 * stream active, so member names intern from the right tokens. */
static void nested_index_add_root(SemaContext *ctx, const ASTNode *root,
                                  void *user) {
  NestedTypeIndex *idx = (NestedTypeIndex *)user;
  if (!idx || !root)
    return;
  for (const ASTNode *tl = root->first_child; tl; tl = tl->next_sibling) {
    if (!is_nominal_or_extension(tl) || !tl->data.var.name_tok)
      continue;
    const char *owner = tok_intern(ctx, tl->data.var.name_tok);
    const ASTNode *body = class_decl_body(tl);
    /* Match resolve_type_from_body's precedence for each owner declaration:
     * typealiases in body order first, then nested nominal types. */
    nested_index_add_body(ctx, idx, owner, body, NESTED_INDEX_TYPEALIAS);
    nested_index_add_body(ctx, idx, owner, body, NESTED_INDEX_NOMINAL);
  }
}

/* The nested-type index spans the WHOLE module — every file, not just the one
 * being resolved — because a type's nested types/typealiases declared in a
 * sibling `extension T { … }` in ANOTHER file are visible unqualified from T's
 * members.  Built once per module and cached by origin-map identity (one origin
 * map per module; see msf_module_analyze), so it survives the per-file
 * ctx->ast_root swaps of whole-module analysis. */
static NestedTypeIndex *nested_type_index_get(SemaContext *ctx) {
  if (!ctx)
    return NULL;
  const void *key =
      ctx->origin_map ? ctx->origin_map : (const void *)ctx->ast_root;
  if (!key)
    return NULL;

  NestedTypeIndex *idx = (NestedTypeIndex *)ctx->nested_type_index;
  if (idx && idx->module_key == key)
    return idx;
  sema_nested_type_index_free(ctx);

  idx = calloc(1, sizeof(*idx));
  if (!idx)
    return NULL;
  idx->cap = 1024;
  idx->module_key = key;
  idx->buckets = calloc(idx->cap, sizeof(*idx->buckets));
  if (!idx->buckets) {
    free(idx);
    return NULL;
  }

  sema_origin_for_each_root(ctx, nested_index_add_root, idx);

  ctx->nested_type_index = idx;
  return idx;
}

static TypeInfo *resolve_nested_from_index(SemaContext *ctx,
                                           const ASTNode *site,
                                           const char *owner,
                                           const char *name,
                                           int *used_index) {
  NestedTypeIndex *idx = nested_type_index_get(ctx);
  if (!idx) {
    if (used_index) *used_index = 0;
    return NULL;
  }
  if (used_index) *used_index = 1;
  uint32_t b = nested_hash_pair(owner, name, idx->cap);
  for (NestedTypeEntry *e = idx->buckets[b]; e; e = e->next) {
    if (e->owner != owner || e->name != name)
      continue;
    if (e->kind == NESTED_INDEX_TYPEALIAS && node_is_ancestor_of(e->decl, site))
      continue;
    /* e->decl may live in another file (a sibling extension in whole-module
     * mode); resolve it with its own token stream active so a typealias RHS
     * reads from the right source. */
    SemaOriginState st;
    int sw = sema_origin_enter(ctx, e->decl, &st);
    TypeInfo *r = (e->kind == NESTED_INDEX_TYPEALIAS)
                      ? resolve_typealias_decl(ctx, e->decl)
                      : resolve_nested_nominal_decl(ctx, e->decl, name);
    if (sw) sema_origin_leave(ctx, &st);
    return r;
  }
  return NULL;
}

/* Search every declaration of `type_name` — its primary nominal decl plus all
 * `extension type_name { ... }` blocks — for a nested type `nested_name`.
 * A nested type may be declared in any of them and is visible from all of them
 * (and unqualified from the type's own members), e.g. `var k: Kind` in
 * `struct List { ... }` referring to `enum Kind` in `extension List { ... }`. */
static TypeInfo *resolve_nested_in_all_decls_of(SemaContext *ctx,
                                                const ASTNode *site,
                                                const char *type_name,
                                                const char *nested_name) {
  if (!ctx->ast_root || !type_name)
    return NULL;
  int used_index = 0;
  TypeInfo *indexed =
      resolve_nested_from_index(ctx, site, type_name, nested_name, &used_index);
  if (indexed || used_index)
    return indexed;

  /* Allocation failure fallback: preserve the previous linear behavior. */
  for (const ASTNode *tl = ctx->ast_root->first_child; tl;
       tl = tl->next_sibling) {
    if (!is_nominal_or_extension(tl) || !tl->data.var.name_tok)
      continue;
    if (tok_intern(ctx, tl->data.var.name_tok) != type_name)
      continue;
    TypeInfo *found =
        resolve_type_from_body(ctx, class_decl_body(tl), site, nested_name);
    if (found)
      return found;
  }
  return NULL;
}

/* Match an associatedtype / typealias requirement in a protocol body.  Unlike a
 * struct/class/enum/superclass `typealias` (a real AST_TYPEALIAS_DECL handled by
 * resolve_typealias_from_body), a protocol-body `typealias X = T` AND
 * `associatedtype X` are both AST_PROTOCOL_REQ + MOD_PROTOCOL_ASSOC_TYPE (parser
 * parse_proto_assoc_req); the name is at req->tok_idx and the aliased / default /
 * constraint type, when present, is the requirement's type child.  Resolve a
 * concrete `typealias X = Int64` to its aliased type; otherwise fall back to the
 * abstract associated-type param, then a named placeholder, so an inherited
 * associatedtype name is still "declared" rather than flagged undeclared. */
static TypeInfo *resolve_protocol_assoc_from_body(SemaContext *ctx,
                                                  const ASTNode *body,
                                                  const char *name) {
  if (!body)
    return NULL;
  for (const ASTNode *c = body->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_PROTOCOL_REQ ||
        !(c->modifiers & MOD_PROTOCOL_ASSOC_TYPE) || c->tok_idx == 0)
      continue;
    if (tok_intern(ctx, c->tok_idx) != name)
      continue;
    const ASTNode *aliased = find_type_child(c);
    if (aliased) {
      TypeInfo *t = resolve_type_annotation(ctx, aliased);
      if (t)
        return t;
    }
    if (c->type)
      return c->type;
    TypeInfo *ti = make_type(ctx, TY_NAMED);
    if (ti)
      ti->named.name = name;
    return ti;
  }
  return NULL;
}

/* A conformed protocol's (or a superclass's) typealiases and nested types are
 * visible UNQUALIFIED inside the conforming/subclassing type — e.g.
 * `protocol P { typealias RowId = Int64 }; struct S: P { var id: RowId }`.
 * That visibility comes from the inheritance graph, not the type's own body, so
 * resolve_type_from_body misses it.  Search the bodies of `type_decl`'s
 * conformances (protocols + superclass) for `name`, recursing through refined
 * protocols / the superclass chain.  Returns the first match.
 *
 * Cross-file safe: a conformance ident is read at its own origin
 * (tok_intern_at_node); the looked-up decl's body is searched with that decl's
 * token stream active (sema_origin_enter).  Stored properties only live in a
 * type's primary body, so the dominant `var id: RowId` case enters here with
 * `type_decl` = the nominal that carries the conformance. */
static TypeInfo *resolve_inherited_member_type(SemaContext *ctx,
                                               const ASTNode *site,
                                               const ASTNode *type_decl,
                                               const char *name, int depth) {
  if (!type_decl || depth > 8)
    return NULL;
  for (const ASTNode *c = type_decl->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_BLOCK)
      break; /* conformance clauses precede the body */
    if (c->kind != AST_CONFORMANCE)
      continue;
    for (const ASTNode *inh = c->first_child; inh; inh = inh->next_sibling) {
      if (inh->kind != AST_TYPE_IDENT || inh->tok_idx == 0)
        continue;
      /* inh belongs to type_decl, which may live in another file in
       * whole-module mode — read its token against its own origin. */
      const char *pname = tok_intern_at_node(ctx, inh, inh->tok_idx);
      if (!pname)
        continue;
      const Symbol *ps = sema_lookup(ctx, pname);
      const ASTNode *pdecl = (ps && ps->decl) ? ps->decl : NULL;
      if (!pdecl || pdecl == type_decl)
        continue;
      if (pdecl->kind != AST_PROTOCOL_DECL && pdecl->kind != AST_CLASS_DECL &&
          pdecl->kind != AST_STRUCT_DECL && pdecl->kind != AST_ENUM_DECL)
        continue;
      TypeInfo *found = NULL;
      const ASTNode *pbody = class_decl_body(pdecl);
      SemaOriginState ost;
      int sw = sema_origin_enter(ctx, pdecl, &ost);
      /* superclass/enum typealiases + nested nominals, then protocol-body
       * associatedtype/typealias requirements. */
      found = resolve_type_from_body(ctx, pbody, site, name);
      if (!found)
        found = resolve_protocol_assoc_from_body(ctx, pbody, name);
      if (sw)
        sema_origin_leave(ctx, &ost);
      if (found)
        return found;
      /* Recurse: a refined protocol's / superclass's own inherited members. */
      found = resolve_inherited_member_type(ctx, site, pdecl, name, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

static TypeInfo *resolve_enclosing_member_type(SemaContext *ctx,
                                               const ASTNode *site,
                                               const char *name) {
  for (const ASTNode *p = site ? site->parent : NULL; p; p = p->parent) {
    if (p->kind != AST_BLOCK || !is_nominal_or_extension(p->parent))
      continue;

    TypeInfo *found = resolve_type_from_body(ctx, p, site, name);
    if (found)
      return found;

    /* Not in this body — scan every declaration (primary + all extensions) of
     * the enclosing type, since the nested type may live in a sibling
     * extension. Works whether `site` is in the primary decl or an extension. */
    if (!p->parent->data.var.name_tok)
      continue;
    const char *type_name = tok_intern(ctx, p->parent->data.var.name_tok);
    found = resolve_nested_in_all_decls_of(ctx, site, type_name, name);
    if (found)
      return found;

    /* Still nothing — the name may be a typealias/nested type inherited from a
     * conformed protocol or superclass, visible unqualified here. */
    found = resolve_inherited_member_type(ctx, site, p->parent, name, 0);
    if (found)
      return found;
  }
  return NULL;
}

/** @brief Returns a default TY_DICT for KeyValuePairs/DictionaryLiteral. */
static TypeInfo *make_default_dict(SemaContext *ctx) {
  TypeInfo *ti = make_type(ctx, TY_DICT);
  if (!ti) return NULL;
  ti->dict.key = TY_BUILTIN_STRING;
  ti->dict.value = TY_BUILTIN_INT;
  return ti;
}

/**
 * @brief Scans top-level AST for a forward type declaration.
 *
 * Enables circular references: class A { var b: B? } class B { var a: A? }
 */
static TypeInfo *resolve_forward_decl(SemaContext *ctx, const char *iname) {
  if (!ctx->ast_root) return NULL;
  for (const ASTNode *tl = ctx->ast_root->first_child; tl; tl = tl->next_sibling) {
    if (tl->kind != AST_CLASS_DECL && tl->kind != AST_STRUCT_DECL &&
        tl->kind != AST_ENUM_DECL && tl->kind != AST_ACTOR_DECL &&
        tl->kind != AST_PROTOCOL_DECL)
      continue;
    if (!tl->data.var.name_tok) continue;
    const Token *nt = &ctx->tokens[tl->data.var.name_tok];
    if (nt->len != strlen(iname)) continue;
    if (memcmp(ctx->src->data + nt->pos, iname, nt->len) != 0) continue;
    TypeInfo *fwd = make_type(ctx, TY_NAMED);
    if (!fwd) return NULL;
    fwd->named.name = iname;
    fwd->named.decl = (void *)tl;
    sema_define(ctx, iname, SYM_CLASS, fwd, (ASTNode *)tl);
    return fwd;
  }
  return NULL;
}

/** @brief Names that should not trigger "undeclared type" errors. */
static int is_silenced_type_name(const char *iname) {
  size_t len = strlen(iname);
  if (len <= 1) return 1; /* single-letter generic placeholders: T, U, V */

  static const char *const SILENCED[] = {
      SW_TYPE_SELF, SW_TYPE_ANY, SW_TYPE_ANY_OBJECT,
      SW_PROTO_EQUATABLE, SW_PROTO_HASHABLE, SW_PROTO_COMPARABLE,
      SW_PROTO_CODABLE, SW_PROTO_SENDABLE,
      /* Numeric / arithmetic protocol family — valid generic constraints. */
      SW_PROTO_ADDITIVE_ARITHMETIC, SW_PROTO_NUMERIC, SW_PROTO_SIGNED_NUMERIC,
      SW_PROTO_BINARY_INTEGER, SW_PROTO_FIXED_WIDTH_INTEGER,
      SW_PROTO_SIGNED_INTEGER, SW_PROTO_UNSIGNED_INTEGER,
      SW_PROTO_FLOATING_POINT, SW_PROTO_BINARY_FLOATING_PT, SW_PROTO_STRIDEABLE,
      "Type",
      NULL
  };
  for (const char *const *p = SILENCED; *p; p++)
    if (strcmp(iname, *p) == 0) return 1;
  return 0;
}

/** @brief Reports "undeclared type" with optional "did you mean?" suggestion. */
static void report_undeclared_type(SemaContext *ctx, const ASTNode *tnode,
                                   const char *iname) {
  if (is_silenced_type_name(iname)) return;
  const char *suggestion = sema_find_similar_type_name(ctx, iname);
  if (suggestion)
    sema_error_suggest(ctx, tnode, suggestion, "use of undeclared type '%s'", iname);
  else
    sema_error(ctx, tnode, "use of undeclared type '%s'", iname);
}

/* ── resolve_type_ident ───────────────────────────────────────────────────── */

/**
 * @brief Resolves AST_TYPE_IDENT to a TypeInfo.
 *
 * Resolution order:
 *   1. Dotted name? → associated type (T.Item) or qualified name (A.B.C)
 *   2. Type alias?  → Never, StaticString, Float16, Float80
 *   3. Dict alias?  → KeyValuePairs, DictionaryLiteral
 *   4. Builtin?     → Int, String, Bool, Double, Float, ...
 *   5. In scope?    → symbol table lookup
 *   6. Forward ref? → scan AST root for top-level type declarations
 *   7. Error        → "undeclared type" with "did you mean?" suggestion
 */
/* Inner resolver: the steps that can mutually recurse (alias chains, nested
 * types). Wrapped by resolve_type_ident, which bounds the recursion depth. */
static TypeInfo *resolve_type_ident_inner(SemaContext *ctx, const ASTNode *tnode,
                                          const char *iname) {
  TypeInfo *result;

  /* 1. Dotted name: T.Item or A.B.C */
  const ASTNode *member = tnode->first_child;
  if (member && member->kind == AST_TYPE_IDENT) {
    result = resolve_assoc_type(ctx, iname, member);
    if (result) return result;
    result = resolve_qualified_name(ctx, tnode);
    if (result) return result;
  }

  /* 2. Type alias: Never→Void, StaticString→String, etc. */
  result = resolve_type_alias(iname);
  if (result) return result;

  /* 3. Dict alias: KeyValuePairs, DictionaryLiteral */
  if (strcmp(iname, "KeyValuePairs") == 0 || strcmp(iname, "DictionaryLiteral") == 0)
    return make_default_dict(ctx);

  /* 4. Builtin type: Int, String, Bool, ... */
  result = resolve_builtin(iname);
  if (result) return result;

  /* 5. Symbol table lookup */
  Symbol *sym = sema_lookup(ctx, iname);
  if (sym && sym->type) return sym->type;

  /* 5b. Deferred typealias: declare_typealias registers the name with no type
   * and defers RHS resolution to Pass 2 (the aliased type may be cross-file).
   * Resolve it now that the whole module is declared — under the alias decl's
   * own origin, since it may live in another file. */
  if (sym && sym->kind == SYM_TYPEALIAS && sym->decl && !sym->decl->type) {
    SemaOriginState st;
    int sw = sema_origin_enter(ctx, sym->decl, &st);
    TypeInfo *t = resolve_typealias_decl(ctx, sym->decl);
    if (sw) sema_origin_leave(ctx, &st);
    if (t) { sym->type = t; return t; }
  } else if (sym && sym->kind == SYM_TYPEALIAS && sym->decl &&
             sym->decl->type) {
    sym->type = sym->decl->type;
    return sym->type;
  }

  result = resolve_enclosing_member_type(ctx, tnode, iname);
  if (result) return result;

  /* 6. Forward type reference: scan AST root */
  result = resolve_forward_decl(ctx, iname);
  if (result) return result;

  /* 7. SDK global fallback: a name present in ANY module of the SDK vocabulary
   * is a real SDK type, reachable in Swift through re-export chains the
   * per-import view doesn't model (Foundation→CoreFoundation,
   * CoreLocation→_LocationEssentials, …).  Resolve it rather than flag it.
   * Uses sdk_vocab (SDK only), NOT vocab — so a sibling project module's
   * published types stay import-scoped (cross-module precision). */
  if (ctx->sdk_vocab && msf_vocab_has_type(ctx->sdk_vocab, iname)) {
    TypeInfo *ti = make_type(ctx, TY_NAMED);
    if (!ti) return NULL;
    ti->named.name = iname;
    sema_define(ctx, iname, SYM_TYPE, ti, NULL); /* cache for later lookups */
    return ti;
  }

  /* 8. Not found — report error, return placeholder TY_NAMED */
  report_undeclared_type(ctx, tnode, iname);
  TypeInfo *ti = make_type(ctx, TY_NAMED);
  if (!ti) return NULL;
  ti->named.name = iname;
  return ti;
}

/* Builds a TypeInfo from a type spelling in a vocabulary signature, e.g.
 * "CALayer", "Int", "UIHoverStyle?".  Handles a trailing optional marker and a
 * plain identifier (mapped to a builtin when one matches, else a TY_NAMED so a
 * member chain re-enters the vocabulary).  Anything more complex — brackets,
 * generics, compositions, qualified names — returns NULL (positive-only: an
 * unresolved member is left unknown, never an error). */
static TypeInfo *vocab_type_from_spelling(SemaContext *ctx, const char *s,
                                          size_t len) {
  while (len && s[0] == ' ') { s++; len--; }
  while (len && s[len - 1] == ' ') len--;
  if (!len) return NULL;
  int opt = 0;
  if (s[len - 1] == '?' || s[len - 1] == '!') {
    opt = 1; len--;
    while (len && s[len - 1] == ' ') len--;
  }
  if (!len) return NULL;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_'))
      return NULL; /* complex spelling — give up */
  }
  const char *name = sema_intern(ctx, s, len);
  TypeInfo *base = resolve_builtin(name);
  if (!base) {
    base = make_type(ctx, TY_NAMED);
    if (!base) return NULL;
    base->named.name = name;
  }
  if (opt) {
    TypeInfo *o = make_type(ctx, TY_OPTIONAL);
    if (!o) return base;
    o->inner = base;
    base = o;
  }
  return base;
}

/* Finds a function signature's return type: the spelling after the last
 * top-level "->" (depth 0, so a closure parameter's inner "->" is skipped).
 * Returns NULL (and *out_len = 0) when there is no top-level arrow — a
 * Void-returning function. */
static const char *func_return_spelling(const char *sig, size_t *out_len) {
  int depth = 0;
  const char *arrow = NULL;
  for (const char *p = sig; *p; p++) {
    if (p[0] == '-' && p[1] == '>') {
      if (depth == 0) arrow = p + 2;
      p++; /* skip '>' so it does not affect depth */
      continue;
    }
    char c = *p;
    if (c == '(' || c == '[' || c == '<') depth++;
    else if (c == ')' || c == ']' || c == '>') { if (depth) depth--; }
  }
  *out_len = arrow ? strlen(arrow) : 0;
  return arrow;
}

/* Resolves the type of member @p member_name on a vocabulary type @p type_name
 * (an SDK type known only by name).  v2 vocabularies carry member signatures:
 *   - property  → the spelling after its ':'
 *   - func      → a function type whose return is the spelling after the
 *                 top-level "->" (Void if none), so `obj.method(...)` resolves
 *                 to that return via call.c
 * init/subscript are not modelled yet → NULL.  Positive-only: an unparseable
 * type leaves the member unresolved rather than risking a wrong type. */
TypeInfo *resolve_vocab_member_type(SemaContext *ctx, const char *type_name,
                                    const char *member_name, int prefer_callable) {
  if (!ctx || !type_name || !member_name) return NULL;
  MSFVocabMemberKind kind = MSF_VOCAB_MEMBER_FUNC;
  const char *sig = NULL;
  if (ctx->vocab)
    sig = msf_vocab_find_member(ctx->vocab, type_name, member_name,
                                prefer_callable, &kind);
  if (!sig && ctx->sdk_vocab)
    sig = msf_vocab_find_member(ctx->sdk_vocab, type_name, member_name,
                                prefer_callable, &kind);
  if (!sig) return NULL;

  if (kind == MSF_VOCAB_MEMBER_PROPERTY) {
    const char *colon = strchr(sig, ':');
    if (colon)
      return vocab_type_from_spelling(ctx, colon + 1, strlen(colon + 1));
    return NULL;
  }
  if (kind == MSF_VOCAB_MEMBER_FUNC) {
    size_t rlen = 0;
    const char *rspell = func_return_spelling(sig, &rlen);
    TypeInfo *ret;
    if (!rspell) {
      ret = TY_BUILTIN_VOID; /* no arrow → Void-returning */
    } else {
      ret = vocab_type_from_spelling(ctx, rspell, rlen);
      if (!ret) return NULL; /* unparseable return → leave unresolved */
    }
    TypeInfo *fn = make_type(ctx, TY_FUNC);
    if (!fn) return NULL;
    fn->func.params = NULL;
    fn->func.param_count = 0;
    fn->func.ret = ret;
    fn->func.is_async = fn->func.throws = fn->func.escaping = 0;
    return fn;
  }
  return NULL;
}

TypeInfo *resolve_type_ident(SemaContext *ctx, const ASTNode *tnode) {
  const char *iname = tok_intern(ctx, tnode->tok_idx);

  /* Cyclic alias chains (`typealias A = B` / `typealias B = A`) and other
   * self-referential nested-type lookups would recurse here until the stack
   * overflows.  Bound the re-entry depth: on overflow, return an unresolved
   * placeholder (no error — the cycle is reported once the chain unwinds) so
   * resolution terminates instead of crashing. */
  if (ctx->type_resolve_depth >= SEMA_TYPE_RESOLVE_MAX_DEPTH) {
    TypeInfo *ti = make_type(ctx, TY_NAMED);
    if (ti) ti->named.name = iname;
    return ti;
  }
  ctx->type_resolve_depth++;
  TypeInfo *result = resolve_type_ident_inner(ctx, tnode, iname);
  ctx->type_resolve_depth--;
  return result;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Simple Type Resolvers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief T? → TY_OPTIONAL wrapping the inner type. */
TypeInfo *resolve_type_optional(SemaContext *ctx, const ASTNode *tnode) {
  TypeInfo *ti = make_type(ctx, TY_OPTIONAL);
  if (ti) ti->inner = resolve_type_annotation(ctx, tnode->first_child);
  return ti;
}

/** @brief [T] or [N of T] → TY_ARRAY. */
TypeInfo *resolve_type_array(SemaContext *ctx, const ASTNode *tnode) {
  const ASTNode *c0 = tnode->first_child;
  TypeInfo *ti = make_type(ctx, TY_ARRAY);
  if (!ti) return NULL;
  /* [N of T] — fixed-length array */
  if (c0 && c0->kind == AST_INTEGER_LITERAL && c0->next_sibling) {
    int64_t n = c0->data.integer.ival;
    ti->array_fixed_len = (n > 0) ? (uint32_t)n : 0;
    ti->inner = resolve_type_annotation(ctx, c0->next_sibling);
  } else {
    ti->inner = resolve_type_annotation(ctx, c0);
  }
  return ti;
}

/** @brief [K: V] → TY_DICT. */
TypeInfo *resolve_type_dict(SemaContext *ctx, const ASTNode *tnode) {
  const ASTNode *kn = tnode->first_child;
  TypeInfo *ti = make_type(ctx, TY_DICT);
  if (!ti) return NULL;
  ti->dict.key = resolve_type_annotation(ctx, kn);
  ti->dict.value = resolve_type_annotation(ctx, kn ? kn->next_sibling : NULL);
  return ti;
}

/** @brief some T / any T / inout T → pass through to inner type. */
TypeInfo *resolve_type_passthrough(SemaContext *ctx, const ASTNode *tnode) {
  if (tnode->first_child)
    return resolve_type_annotation(ctx, tnode->first_child);
  if (tnode->kind == AST_TYPE_SOME || tnode->kind == AST_TYPE_ANY)
    return resolve_type_ident(ctx, tnode);
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Function Type Resolver
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns 1 if a throws clause actually throws (not throws(Never)). */
static int throws_clause_is_throwing(SemaContext *ctx, const ASTNode *clause) {
  if (clause->modifiers & MOD_RETHROWS) return 1;
  if (!clause->first_child) return 1;
  return !type_is_never(resolve_type_annotation(ctx, clause->first_child));
}

/** @brief (A, B) async throws -> C → TY_FUNC. */
TypeInfo *resolve_type_func(SemaContext *ctx, const ASTNode *tnode) {
  TypeInfo *ti = make_type(ctx, TY_FUNC);
  if (!ti) return NULL;

  /* Count params, detect throws, find return type */
  size_t param_count = 0;
  const ASTNode *ret_node = NULL;
  for (const ASTNode *c = tnode->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_PARAM)
      param_count++;
    else if (c->kind == AST_THROWS_CLAUSE)
      ti->func.throws = throws_clause_is_throwing(ctx, c);
    else
      ret_node = c;
  }

  ti->func.param_count = param_count;
  ti->func.escaping = (tnode->modifiers & MOD_ESCAPING) ? 1 : 0;

  /* Resolve parameter types */
  if (param_count > 0) {
    TypeInfo **params = malloc(param_count * sizeof(TypeInfo *));
    if (!params) return NULL;
    size_t pi = 0;
    for (const ASTNode *c = tnode->first_child; c && pi < param_count; c = c->next_sibling) {
      if (c->kind != AST_PARAM) continue;
      const ASTNode *pty = find_type_child(c);
      params[pi++] = pty ? resolve_type_annotation(ctx, pty) : TY_BUILTIN_INT;
    }
    ti->func.params = params;
  }

  ti->func.ret = ret_node ? resolve_type_annotation(ctx, ret_node) : TY_BUILTIN_VOID;
  return ti;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Tuple Type Resolver
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Resolves a single tuple element (labeled or plain). */
static void resolve_tuple_element(SemaContext *ctx, const ASTNode *c,
                                  TypeInfo **out_type, const char **out_label) {
  if (c->kind == AST_PARAM) {
    /* Named element: (label: Type) */
    if (c->data.var.name_tok)
      *out_label = tok_intern(ctx, c->data.var.name_tok);
    const ASTNode *ety = find_type_child(c);
    *out_type = ety ? resolve_type_annotation(ctx, ety) : TY_BUILTIN_INT;
  } else {
    /* Plain type node */
    *out_type = resolve_type_annotation(ctx, c);
  }
}

/** @brief (A, label: B) → TY_TUPLE with optional labels. */
TypeInfo *resolve_type_tuple(SemaContext *ctx, const ASTNode *tnode) {
  TypeInfo *ti = make_type(ctx, TY_TUPLE);
  if (!ti) return NULL;

  size_t n = 0;
  for (const ASTNode *c = tnode->first_child; c; c = c->next_sibling) n++;
  ti->tuple.elem_count = n;
  if (n == 0) return ti; /* `()` — empty tuple / Void */

  /* A single parenthesized type `(T)` is just T — grouping parens, not a
   * 1-tuple (Swift has no 1-tuples).  Without this, `(() -> Void)?` resolved to
   * Optional<(tuple-of-one-func)> instead of Optional<() -> Void>, breaking
   * function-type comparisons (closure assignments, etc.). */
  if (n == 1) {
    TypeInfo *only = NULL;
    const char *lbl = NULL;
    resolve_tuple_element(ctx, tnode->first_child, &only, &lbl);
    if (only) return only;
  }

  TypeInfo **elems = calloc(n, sizeof(TypeInfo *));
  const char **labels = calloc(n, sizeof(const char *));
  if (!elems || !labels) { free(elems); free(labels); return NULL; }

  size_t i = 0;
  for (const ASTNode *c = tnode->first_child; c && i < n; c = c->next_sibling, i++)
    resolve_tuple_element(ctx, c, &elems[i], &labels[i]);

  ti->tuple.elems = elems;
  ti->tuple.labels = labels;
  return ti;
}

/* ── Generic type sugar helpers ────────────────────────────────────────────── */

static int type_satisfies_hashable(SemaContext *ctx, const TypeInfo *ty) {
  if (!ctx || !ctx->conformance_table || !ty)
    return 1;
  if (ty == TY_BUILTIN_INT || ty == TY_BUILTIN_STRING ||
      ty == TY_BUILTIN_DOUBLE || ty == TY_BUILTIN_FLOAT ||
      ty == TY_BUILTIN_BOOL || ty == TY_BUILTIN_UINT ||
      ty == TY_BUILTIN_UINT8 || ty == TY_BUILTIN_UINT16 ||
      ty == TY_BUILTIN_UINT32 || ty == TY_BUILTIN_UINT64)
    return 1;
  switch (ty->kind) {
  case TY_OPTIONAL:
  case TY_ARRAY:
  case TY_SET:
    return type_satisfies_hashable(ctx, ty->inner);
  case TY_DICT:
    return type_satisfies_hashable(ctx, ty->dict.key) &&
           type_satisfies_hashable(ctx, ty->dict.value);
  case TY_GENERIC_PARAM:
    return 1;
  case TY_NAMED:
    if (!ty->named.name || strcmp(ty->named.name, SW_TYPE_ANY) == 0 ||
        strcmp(ty->named.name, SW_TYPE_ANY_OBJECT) == 0 ||
        strcmp(ty->named.name, "AnyHashable") == 0 ||
        /* `Self` is a type parameter (like a generic param, already accepted
         * above): if `Set<Self>` is written, the context guarantees
         * `Self: Hashable` — e.g. `extension Hashable { ... Set<Self> ... }`. */
        strcmp(ty->named.name, SW_TYPE_SELF) == 0)
      return 1;
    if (conformance_table_has(ctx->conformance_table, ty->named.name,
                              SW_PROTO_HASHABLE))
      return 1;
    if (ty->named.decl) {
      const ASTNode *decl = (const ASTNode *)ty->named.decl;
      if (decl->kind == AST_ENUM_DECL)
        return 1;
      if (decl->kind == AST_CLASS_DECL)
        return 0;
    }
    return strchr(ty->named.name, '.') != NULL;
  case TY_GENERIC_INST:
    return ty->generic.base &&
           type_satisfies_hashable(ctx, ty->generic.base);
  default:
    return ty->kind == TY_UNKNOWN;
  }
}

/**
 * @brief Validates that a type conforms to Hashable (required for Dict/Set keys).
 *
 * Builtin types and payload-free enum skeletons are accepted as Hashable.
 */
static void check_hashable(SemaContext *ctx, const ASTNode *site,
                           const TypeInfo *key_type, const char *container) {
  if (!ctx->conformance_table || !key_type) return;
  if (type_satisfies_hashable(ctx, key_type)) return;
  char buf[64];
  const char *tn = NULL;
  if      (key_type == TY_BUILTIN_INT)    tn = SW_TYPE_INT;
  else if (key_type == TY_BUILTIN_STRING) tn = SW_TYPE_STRING;
  else if (key_type == TY_BUILTIN_DOUBLE) tn = SW_TYPE_DOUBLE;
  else if (key_type == TY_BUILTIN_FLOAT)  tn = SW_TYPE_FLOAT;
  else if (key_type == TY_BUILTIN_BOOL)   tn = SW_TYPE_BOOL;
  else if (key_type->kind == TY_NAMED && key_type->named.name)
    tn = key_type->named.name;
  else {
    type_to_string(key_type, buf, sizeof(buf));
    tn = buf;
  }
  if (tn && !conformance_table_has(ctx->conformance_table, tn, SW_PROTO_HASHABLE))
    sema_error(ctx, (ASTNode *)site,
               "Type '%s' does not conform to protocol 'Hashable'; "
               "%s requires 'Hashable' elements", tn, container);
}

/**
 * @brief Tries to desugar a generic type into a builtin collection type.
 *
 * Array<T>      → TY_ARRAY
 * Dictionary<K,V> → TY_DICT (+ Hashable check on K)
 * Optional<T>   → TY_OPTIONAL
 * Set<T>        → TY_SET (+ Hashable check on element)
 *
 * @return The desugared TypeInfo, or NULL if no sugar applies.
 */
static TypeInfo *try_desugar_generic(SemaContext *ctx, const ASTNode *tnode,
                                     const char *base_name,
                                     TypeInfo **args, uint32_t arg_count) {
  TypeInfo *ti = NULL;

  if (arg_count == 1 && (strcmp(base_name, SW_TYPE_ARRAY) == 0 ||
                         strcmp(base_name, "ArraySlice") == 0 ||
                         strcmp(base_name, "ContiguousArray") == 0 ||
                         strcmp(base_name, "AnySequence") == 0 ||
                         strcmp(base_name, "AnyIterator") == 0 ||
                         strcmp(base_name, "AnyCollection") == 0 ||
                         strcmp(base_name, "AnyBidirectionalCollection") == 0 ||
                         strcmp(base_name, "AnyRandomAccessCollection") == 0)) {
    ti = make_type(ctx, TY_ARRAY);
    if (ti) ti->inner = args[0];
  } else if (arg_count == 2 && strcmp(base_name, SW_TYPE_DICTIONARY) == 0) {
    check_hashable(ctx, tnode, args[0], "'Dictionary'");
    ti = make_type(ctx, TY_DICT);
    if (ti) { ti->dict.key = args[0]; ti->dict.value = args[1]; }
  } else if (arg_count == 1 && strcmp(base_name, SW_TYPE_OPTIONAL) == 0) {
    ti = make_type(ctx, TY_OPTIONAL);
    if (ti) ti->inner = args[0];
  } else if (arg_count == 1 && strcmp(base_name, "Set") == 0) {
    check_hashable(ctx, tnode, args[0], "'Set'");
    ti = make_type(ctx, TY_SET);
    if (ti) ti->inner = args[0];
  }

  if (ti) free(args);
  return ti;
}

static int generic_child_is_qualified_suffix(SemaContext *ctx,
                                             const ASTNode *child) {
  if (!child || child->kind != AST_TYPE_IDENT || child->tok_idx == 0)
    return 0;
  const Token *prev = &ctx->tokens[child->tok_idx - 1];
  return ((prev->type == TOK_PUNCT || prev->type == TOK_OPERATOR) &&
          prev->len == 1 && ctx->src->data[prev->pos] == '.');
}

/** @brief Checks generic parameter constraints (where T: Equatable, etc.). */
static void validate_generic_constraints(SemaContext *ctx, const ASTNode *tnode,
                                         const char *base_name,
                                         TypeInfo **args, uint32_t arg_count) {
  if (!ctx->conformance_table || arg_count == 0) return;
  const Symbol *sym = sema_lookup(ctx, base_name);
  if (!sym || !sym->decl) return;

  TypeInfo *params[16];
  uint32_t np = 0;
  for (const ASTNode *c = sym->decl->first_child; c && np < 16;
       c = c->next_sibling)
    if (c->kind == AST_GENERIC_PARAM && c->type &&
        c->type->kind == TY_GENERIC_PARAM)
      params[np++] = c->type;

  if (np > 0 && np == arg_count)
    check_generic_args((TypeInfo *const *)params, np, args, arg_count,
                       ctx->conformance_table, ctx, (ASTNode *)tnode);
}

/**
 * @brief Resolves AST_TYPE_GENERIC: Array\<Int\>, Dict\<K,V\>, Set\<T\>, Custom\<T\>.
 *
 * First tries to desugar into a builtin collection type (Array, Dict, Optional, Set).
 * Otherwise produces TY_GENERIC_INST and validates parameter constraints.
 */
TypeInfo *resolve_type_generic(SemaContext *ctx, const ASTNode *tnode) {
  const char *base_name = tok_intern(ctx, tnode->tok_idx);

  /* Resolve base type */
  TypeInfo *base_ti = NULL;
  if (strcmp(base_name, SW_TYPE_ARRAY) == 0 ||
      strcmp(base_name, "ArraySlice") == 0 ||
      strcmp(base_name, "ContiguousArray") == 0 ||
      strcmp(base_name, "AnySequence") == 0 ||
      strcmp(base_name, "AnyIterator") == 0 ||
      strcmp(base_name, "AnyCollection") == 0 ||
      strcmp(base_name, "AnyBidirectionalCollection") == 0 ||
      strcmp(base_name, "AnyRandomAccessCollection") == 0 ||
      strcmp(base_name, SW_TYPE_OPTIONAL) == 0 ||
      strcmp(base_name, SW_TYPE_DICTIONARY) == 0) {
    base_ti = make_type(ctx, TY_NAMED);
    if (!base_ti) return NULL;
    base_ti->named.name = base_name;
  } else {
    TypeInfo *bi = resolve_builtin(base_name);
    if (bi) return bi;
    Symbol *sym = sema_lookup(ctx, base_name);
    if (sym && sym->type) {
      base_ti = sym->type;
    } else {
      base_ti = make_type(ctx, TY_NAMED);
      if (!base_ti) return NULL;
      base_ti->named.name = base_name;
      if (sym) base_ti->named.decl = sym->decl;
    }
  }

  /* Collect type arguments */
  uint32_t arg_count = 0;
  for (const ASTNode *c = tnode->first_child; c; c = c->next_sibling)
    if (!generic_child_is_qualified_suffix(ctx, c))
      arg_count++;
  if (arg_count == 0)
    return base_ti;

  TypeInfo **args = malloc(arg_count * sizeof(TypeInfo *));
  if (!args) return NULL;
  uint32_t i = 0;
  for (const ASTNode *c = tnode->first_child; c; c = c->next_sibling)
    if (!generic_child_is_qualified_suffix(ctx, c))
      args[i++] = resolve_type_annotation(ctx, c);

  /* Sugar: Array<T>→TY_ARRAY, Dictionary<K,V>→TY_DICT, etc. */
  TypeInfo *sugar = try_desugar_generic(ctx, tnode, base_name, args, arg_count);
  if (sugar) return sugar; /* args freed by try_desugar_generic */

  /* General case: TY_GENERIC_INST */
  TypeInfo *ti = make_type(ctx, TY_GENERIC_INST);
  if (!ti) { free(args); return NULL; }
  ti->generic.base = base_ti;
  ti->generic.args = args;
  ti->generic.arg_count = arg_count;

  validate_generic_constraints(ctx, tnode, base_name, args, arg_count);
  return ti;
}

/** @brief Resolves AST_TYPE_COMPOSITION (P & Q) — flattens into TY_PROTOCOL_COMPOSITION. */
TypeInfo *resolve_type_composition(SemaContext *ctx, const ASTNode *tnode) {
  const ASTNode *left_n = tnode->first_child;
  const ASTNode *right_n = left_n ? left_n->next_sibling : NULL;
  if (!left_n || !right_n)
    return NULL;
  TypeInfo *l = resolve_type_annotation(ctx, left_n);
  TypeInfo *r = resolve_type_annotation(ctx, right_n);
  if (!l || !r)
    return l ? l : r;
  uint32_t nl =
      (l->kind == TY_PROTOCOL_COMPOSITION) ? l->composition.protocol_count : 1;
  uint32_t nr =
      (r->kind == TY_PROTOCOL_COMPOSITION) ? r->composition.protocol_count : 1;
  uint32_t n = nl + nr;
  if (n > 16)
    n = 16;
  TypeInfo **list = malloc(n * sizeof(TypeInfo *));
  if (!list)
    return l;
  uint32_t k = 0;
  if (l->kind == TY_PROTOCOL_COMPOSITION) {
    for (uint32_t i = 0; i < l->composition.protocol_count && k < n; i++)
      list[k++] = l->composition.protocols[i];
  } else
    list[k++] = l;
  if (r->kind == TY_PROTOCOL_COMPOSITION) {
    for (uint32_t i = 0; i < r->composition.protocol_count && k < n; i++)
      list[k++] = r->composition.protocols[i];
  } else
    list[k++] = r;
  TypeInfo *ti = type_arena_alloc(ctx->type_arena);
  if (!ti) {
    free(list);
    return NULL;
  }
  ti->kind = TY_PROTOCOL_COMPOSITION;
  ti->composition.protocols = list;
  ti->composition.protocol_count = k;
  return ti;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Dispatch Table
 * ═══════════════════════════════════════════════════════════════════════════════ */
static TypeResolver type_resolvers[AST__COUNT] = {0};
static int type_resolvers_ready = 0;

/** @brief Populates the dispatch table (called once, idempotent). */
void init_type_resolvers(void) {
  if (type_resolvers_ready)
    return;
  type_resolvers[AST_TYPE_IDENT] = resolve_type_ident;
  type_resolvers[AST_TYPE_OPTIONAL] = resolve_type_optional;
  type_resolvers[AST_TYPE_ARRAY] = resolve_type_array;
  type_resolvers[AST_TYPE_DICT] = resolve_type_dict;
  type_resolvers[AST_TYPE_FUNC] = resolve_type_func;
  type_resolvers[AST_TYPE_TUPLE] = resolve_type_tuple;
  type_resolvers[AST_TYPE_SOME] = resolve_type_passthrough;
  type_resolvers[AST_TYPE_ANY] = resolve_type_passthrough;
  type_resolvers[AST_TYPE_INOUT] = resolve_type_passthrough;
  type_resolvers[AST_TYPE_GENERIC] = resolve_type_generic;
  type_resolvers[AST_TYPE_COMPOSITION] = resolve_type_composition;
  type_resolvers_ready = 1;
}

/** @brief Main entry point: resolves any AST type node to a TypeInfo via dispatch table. */
TypeInfo *resolve_type_annotation(SemaContext *ctx, const ASTNode *tnode) {
  if (!tnode)
    return NULL;
  if (tnode->type)
    return tnode->type;
  SemaOriginState origin_state;
  sema_origin_enter(ctx, tnode, &origin_state);
  init_type_resolvers();
  TypeResolver fn =
      (tnode->kind < AST__COUNT) ? type_resolvers[tnode->kind] : NULL;
  TypeInfo *resolved = fn ? fn(ctx, tnode) : NULL;
  ((ASTNode *)tnode)->type = resolved;
  sema_origin_leave(ctx, &origin_state);
  return resolved;
}

/** @brief Returns the first AST_TYPE_* child of a declaration node (type annotation). */
const ASTNode *find_type_child(const ASTNode *decl) {
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
    switch (c->kind) {
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
      return c;
    default:
      break;
    }
  }
  return NULL;
}

/** @brief Returns the first non-type-annotation child (the initializer expression). */
const ASTNode *find_init_child(const ASTNode *decl) {
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
    switch (c->kind) {
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
    case AST_OWNERSHIP_SPEC:
    case AST_THROWS_CLAUSE:
    case AST_ACCESSOR_DECL:
      continue; /* skip type annotations */
    default:
      return c;
    }
  }
  return NULL;
}
