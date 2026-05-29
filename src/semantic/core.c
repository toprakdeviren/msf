/**
 * @file core.c
 * @brief Semantic analysis core: intern pool, symbol table, scope management,
 *        error reporting, module import, and builtin type resolution.
 *
 * This is the sema foundation layer — every other sema file depends on
 * the primitives defined here:
 *
 *   sema_intern()         — NFC-normalized string interning (FNV-1a hash table)
 *   sema_lookup/define()  — symbol table with scope chain
 *   sema_push/pop_scope() — lexical scope stack
 *   sema_error()          — diagnostic recording
 *   resolve_builtin()     — table-driven builtin type lookup
 *
 * Also contains AST traversal helpers and type query utilities used
 * across multiple sema passes.
 */
#include "private.h"
#include <decoder.h> /* decoder_init(), decoder_normalize_utf8(), decoder_is_normalized_utf8() */

/* These are defined in access.c but used by sema_define's visibility checks. */
int access_rank(uint32_t mods);
uint32_t type_effective_access(SemaContext *ctx, TypeInfo *ty);
int private_member_visible(SemaContext *ctx, const ASTNode *member_decl,
                                const ASTNode *owning_type_decl);

static void sema_record(SemaContext *ctx, const ASTNode *node, const char *msg);

/* ═══════════════════════════════════════════════════════════════════════════════
 * Precedence Group Registry
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Registers a precedence group name for later duplicate checking. */
void sema_add_precedence_group_name(SemaContext *ctx, const ASTNode *node) {
  if (ctx->pg_count >= SEMA_PG_NAMES_MAX || !node->data.var.name_tok)
    return;
  const char *name = tok_intern_at_node(ctx, node, node->data.var.name_tok);
  for (uint32_t i = 0; i < ctx->pg_count; i++)
    if (ctx->pg_names[i] == name)
      return;
  ctx->pg_names[ctx->pg_count++] = name;
}

/** @brief Returns 1 if a precedence group with the given interned name exists. */
int sema_has_precedence_group(const SemaContext *ctx, const char *name) {
  for (uint32_t i = 0; i < ctx->pg_count; i++)
    if (ctx->pg_names[i] == name)
      return 1;
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * String Intern Pool
 *
 * Open-addressing hash table with linear probing (load factor ≤ 75%).
 * FNV-1a hash.  Strings are stored in a contiguous linear buffer; the hash
 * table holds pointers into that buffer.  O(1) amortized lookup.
 *
 * NFC normalization: Swift compilers normalize identifiers to NFC so that
 * canonically equivalent sequences (e.g. 'e' + U+0301 vs. U+00E9) map to
 * the same interned string.  The quick-check fast path ensures zero overhead
 * for purely-ASCII identifiers (the common case).
 *
 * Lifecycle: ctx->intern is per-SemaContext, allocated on first use,
 * freed by the caller (interned strings outlive sema analysis).
 * ═══════════════════════════════════════════════════════════════════════════════ */

/* ── Chunked string buffer ──────────────────────────────────────────────────
 *
 * Each chunk is a stable block of bytes — once a string is written into a
 * chunk, its pointer never moves.  When the active chunk fills up, a fresh
 * chunk is allocated and linked in.  This keeps every interned `const char*`
 * we hand out valid for the lifetime of the pool, even as the pool grows.
 *
 * The hash table itself (table + lengths) is allowed to be reallocated on
 * growth — only the *contents* of the table change, not the strings.
 */
#define INTERN_CHUNK_MIN  (32u * 1024u)   /* 32 KB minimum chunk allocation */

typedef struct InternChunk {
  struct InternChunk *next;
  size_t              used;
  size_t              cap;
  /* `data` is allocated as a flexible-array region following the struct;
   * we use a separate malloc rather than a flexible array member to keep
   * MSVC happy and avoid alignment quirks. */
  char               *data;
} InternChunk;

struct InternPool {
  InternChunk  *head;        /**< First chunk (oldest). */
  InternChunk  *tail;        /**< Active chunk for new strings. */
  const char  **table;       /**< Open-addressing hash table.   */
  uint32_t     *lengths;     /**< Per-slot string length.        */
  size_t        table_cap;   /**< Power of two; INTERN_POOL_CAP initially. */
  size_t        count;       /**< Live entries in the table.    */
  uint8_t       oom;         /**< Set on first allocation failure. */
};

/** @brief Releases all chunks and table arrays owned by the pool.
 *  The InternPool struct itself is freed by the caller. */
static void intern_pool_release(InternPool *pool) {
  if (!pool) return;
  InternChunk *c = pool->head;
  while (c) {
    InternChunk *next = c->next;
    free(c->data);
    free(c);
    c = next;
  }
  pool->head = pool->tail = NULL;
  free(pool->table);   pool->table = NULL;
  free(pool->lengths); pool->lengths = NULL;
  pool->table_cap = 0;
  pool->count = 0;
}

/** @brief Allocates the initial hash table.  Returns 0 on success, -1 OOM. */
static int intern_pool_init_table(InternPool *pool) {
  pool->table_cap = INTERN_POOL_CAP;
  pool->table   = calloc(pool->table_cap, sizeof(*pool->table));
  pool->lengths = calloc(pool->table_cap, sizeof(*pool->lengths));
  if (!pool->table || !pool->lengths) {
    free(pool->table);   pool->table = NULL;
    free(pool->lengths); pool->lengths = NULL;
    pool->table_cap = 0;
    return -1;
  }
  return 0;
}

/** @brief Doubles the hash table and rehashes existing entries.
 *  Returns 0 on success, -1 on OOM (the old table stays intact). */
static int intern_pool_grow_table(InternPool *pool) {
  size_t new_cap = pool->table_cap * 2;
  if (new_cap < pool->table_cap) return -1;  /* overflow */
  const char **new_tab = calloc(new_cap, sizeof(*new_tab));
  uint32_t    *new_len = calloc(new_cap, sizeof(*new_len));
  if (!new_tab || !new_len) {
    free(new_tab); free(new_len);
    return -1;
  }
  size_t new_mask = new_cap - 1;
  for (size_t i = 0; i < pool->table_cap; i++) {
    const char *s = pool->table[i];
    if (!s) continue;
    uint32_t h = 2166136261u;
    for (uint32_t j = 0; j < pool->lengths[i]; j++)
      h = (h ^ (uint8_t)s[j]) * 16777619u;
    size_t slot = h & new_mask;
    while (new_tab[slot]) slot = (slot + 1) & new_mask;
    new_tab[slot] = s;
    new_len[slot] = pool->lengths[i];
  }
  free(pool->table);   pool->table   = new_tab;
  free(pool->lengths); pool->lengths = new_len;
  pool->table_cap = new_cap;
  return 0;
}

/** @brief Reserves @p need bytes in the active chunk, allocating a new one
 *  if necessary.  Returns a pointer into the chunk on success, NULL on OOM. */
static char *intern_pool_reserve(InternPool *pool, size_t need) {
  if (!pool->tail || pool->tail->used + need > pool->tail->cap) {
    size_t cap = need > INTERN_CHUNK_MIN ? need : INTERN_CHUNK_MIN;
    InternChunk *c = malloc(sizeof(InternChunk));
    if (!c) return NULL;
    c->data = malloc(cap);
    if (!c->data) { free(c); return NULL; }
    c->used = 0;
    c->cap  = cap;
    c->next = NULL;
    if (pool->tail) pool->tail->next = c;
    else            pool->head = c;
    pool->tail = c;
  }
  char *p = pool->tail->data + pool->tail->used;
  pool->tail->used += need;
  return p;
}

/** @brief FNV-1a hash for intern pool lookup. */
static uint32_t intern_hash(const char *s, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++)
    h = (h ^ (uint8_t)s[i]) * 16777619u;
  return h;
}

/**
 * @brief Interns a string, returning a pointer to the canonical copy.
 *
 * If the string is already interned, returns the existing pointer (O(1)).
 * Otherwise copies it into a chunked buffer and inserts into the hash table.
 * NFC-normalizes non-ASCII input before interning.
 *
 * Both the chunk buffer and the hash table grow on demand — there is no
 * fixed capacity to overflow.  On allocation failure, returns NULL and
 * marks the pool as OOM so callers can surface a real diagnostic instead
 * of silently aliasing distinct identifiers.
 *
 * @param ctx  Sema context (owns the intern pool).
 * @param str  String bytes to intern.
 * @param len  String length.
 * @return     Interned pointer (valid until the pool is freed) or NULL on OOM.
 */
/* 8-byte SWAR ASCII check: non-zero iff every byte of [s, s+n) is < 0x80.
 * Pure ASCII is always NFC-normalized, so this lets sema_intern skip the
 * decoder's UTF-8→UTF-32 decode + quick-check for the overwhelming majority
 * of identifiers (which are ASCII) at the cost of one cheap word-at-a-time
 * scan. */
static inline int sema_str_is_ascii(const char *s, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    memcpy(&w, s + i, 8);
    if (w & 0x8080808080808080ULL) return 0;
  }
  for (; i < n; i++)
    if ((unsigned char)s[i] & 0x80u) return 0;
  return 1;
}

const char *sema_intern(SemaContext *ctx, const char *str, size_t len) {
  if (!ctx->intern) {
    ctx->intern = calloc(1, sizeof(InternPool));
    if (!ctx->intern) return NULL;
    if (intern_pool_init_table(ctx->intern) != 0) {
      ctx->intern->oom = 1;
      return NULL;
    }
  }

  /* Defensive bound: a corrupt or cross-file token index can surface a garbage
   * length here (whole-module analysis can read a sibling file's node with the
   * wrong active stream).  No real identifier/type name approaches this, so
   * treat it as empty instead of scanning gigabytes out of bounds — analysis
   * stays sound-ish and never crashes. */
  if (len > (size_t)(1u << 20)) { str = ""; len = 0; }

  /* NFC normalization.  Pure ASCII is always normalized, so a one-word-at-a-
   * time SWAR scan lets us skip the decoder's UTF-8→UTF-32 decode + quick-check
   * entirely for the (overwhelming) ASCII-identifier case. */
  uint8_t nfc_stack[256];
  const char *src = str;
  size_t src_len = len;

  if (!sema_str_is_ascii(str, len) &&
      !decoder_is_normalized_utf8((const uint8_t *)str, len, DECODER_NFC)) {
    size_t nfc_len = 0;
    int rc = decoder_normalize_utf8((const uint8_t *)str, len, DECODER_NFC,
                                    nfc_stack, sizeof(nfc_stack), &nfc_len);
    if (rc == DECODER_SUCCESS) {
      src = (const char *)nfc_stack;
      src_len = nfc_len;
    }
  }

  InternPool *pool = ctx->intern;

  /* Probe for existing entry. */
  uint32_t h = intern_hash(src, src_len);
  size_t mask = pool->table_cap - 1;
  size_t slot = h & mask;
  for (size_t probe = 0; probe < pool->table_cap; probe++) {
    size_t idx = (slot + probe) & mask;
    if (!pool->table[idx]) { slot = idx; break; }
    if (pool->lengths[idx] == (uint32_t)src_len &&
        memcmp(pool->table[idx], src, src_len) == 0)
      return pool->table[idx];
  }

  /* Grow table if load factor would exceed 3/4 after this insert.
   * Reslot afterwards since rehash changes positions. */
  if ((pool->count + 1) * 4 >= pool->table_cap * 3) {
    if (intern_pool_grow_table(pool) != 0) {
      pool->oom = 1;
      return NULL;
    }
    mask = pool->table_cap - 1;
    slot = h & mask;
    while (pool->table[slot]) slot = (slot + 1) & mask;
  }

  /* Allocate space for the string (plus NUL). */
  char *p = intern_pool_reserve(pool, src_len + 1);
  if (!p) {
    pool->oom = 1;
    return NULL;
  }
  memcpy(p, src, src_len);
  p[src_len] = '\0';

  pool->table[slot] = p;
  pool->lengths[slot] = (uint32_t)src_len;
  pool->count++;
  return p;
}

/** @brief Returns non-zero if the interner has reported an OOM since init.
 *  Callers can surface this as a diagnostic. */
int sema_intern_oom(const SemaContext *ctx) {
  return ctx && ctx->intern && ctx->intern->oom;
}

/** @brief Interns the text of token at @p tok_idx. */
const char *tok_intern(SemaContext *ctx, uint32_t tok_idx) {
  if (tok_idx < ctx->tok_cache_cap && ctx->tok_cache[tok_idx])
    return ctx->tok_cache[tok_idx];

  /* Whole-module safety net: a token index belonging to a *different* file's
   * stream would read out of bounds here.  token_count is the current stream's
   * length (0 in single-file mode → check disabled, indices always valid). */
  if (ctx->token_count && tok_idx >= ctx->token_count)
    return sema_intern(ctx, "", 0);

  const Token *t = &ctx->tokens[tok_idx];
  const char *r = sema_intern(ctx, ctx->src->data + t->pos, t->len);

  if (tok_idx >= ctx->tok_cache_cap) {
    uint32_t ncap = ctx->tok_cache_cap ? ctx->tok_cache_cap : 256;
    while (ncap <= tok_idx) ncap *= 2;
    const char **nc = realloc(ctx->tok_cache, (size_t)ncap * sizeof(*nc));
    if (nc) {
      memset(nc + ctx->tok_cache_cap, 0,
             (size_t)(ncap - ctx->tok_cache_cap) * sizeof(*nc));
      ctx->tok_cache = nc;
      ctx->tok_cache_cap = ncap;
    }
  }
  if (tok_idx < ctx->tok_cache_cap) ctx->tok_cache[tok_idx] = r;
  return r;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Symbol Table
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief FNV-1a hash for symbol table bucket selection. */
uint32_t sym_hash(const char *name) {
  uint32_t h = 2166136261u;
  for (; *name; name++)
    h = (h ^ (uint8_t)*name) * 16777619u;
  return h % SCOPE_HASH_SIZE;
}

/**
 * @brief Looks up a symbol by interned name, walking the scope chain.
 *
 * Uses pointer equality (interned strings) — no strcmp needed.
 */
Symbol *sema_lookup(SemaContext *ctx, const char *name) {
  for (Scope *s = ctx->current_scope; s; s = s->parent) {
    uint32_t h = sym_hash(name);
    for (Symbol *sym = s->buckets[h]; sym; sym = sym->next)
      if (sym->name == name)
        return sym;
  }
  return NULL;
}

/**
 * @brief Collects all function overloads for an interned name.
 *
 * Only SYM_FUNC symbols are collected.  Returns the count (0..max).
 */
uint32_t sema_lookup_overloads(const SemaContext *ctx, const char *name,
                               Symbol **out, uint32_t max) {
  if (!ctx->current_scope || !out || max == 0)
    return 0;
  uint32_t h = sym_hash(name);
  uint32_t n = 0;
  for (Scope *s = ctx->current_scope; s; s = s->parent)
    for (Symbol *sym = s->buckets[h]; sym && n < max; sym = sym->next)
      if (sym->name == name && sym->kind == SYM_FUNC)
        out[n++] = sym;
  return n;
}

/* ── Redeclaration suppression helpers for sema_define ────────────────────── */

/** @brief Wildcard `_` — multiple `let _ = expr` in the same scope is legal. */
static int is_wildcard(const char *name) {
  return name[0] == '_' && name[1] == '\0';
}

/** @brief Operator function inside a type body (static func + etc.). */
static int is_operator_in_type(const ASTNode *decl, const char *name) {
  if (!decl || decl->kind != AST_FUNC_DECL)
    return 0;
  if (!name[0] || (name[0] >= 'a' && name[0] <= 'z') ||
      (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_')
    return 0;
  const ASTNode *pb = decl->parent;
  if (!pb || !pb->parent)
    return 0;
  ASTNodeKind pk = pb->parent->kind;
  return pk == AST_STRUCT_DECL || pk == AST_CLASS_DECL || pk == AST_ENUM_DECL;
}

/** @brief Is this a @resultBuilder type's method (overloads are expected)? */
static int is_result_builder_method(SemaContext *ctx, const ASTNode *decl) {
  if (!decl || decl->kind != AST_FUNC_DECL || !decl->parent)
    return 0;
  const ASTNode *parent_block = decl->parent;
  if (!parent_block || !parent_block->parent)
    return 0;
  const ASTNode *type_decl = parent_block->parent;
  if (type_decl->kind != AST_STRUCT_DECL && type_decl->kind != AST_CLASS_DECL)
    return 0;
  /* Children-attached attribute (new representation). */
  for (const ASTNode *c = type_decl->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_ATTRIBUTE) continue;
    const char *aname = tok_intern_at_node(ctx, c, c->data.var.name_tok);
    if (!strcmp(aname, SW_ATTR_RESULT_BUILDER))
      return 1;
  }
  /* Sibling fallback. */
  if (!type_decl->parent) return 0;
  for (const ASTNode *sib = type_decl->parent->first_child; sib;
       sib = sib->next_sibling) {
    if (sib->next_sibling != type_decl) continue;
    if (sib->kind == AST_ATTRIBUTE) {
      const char *aname = tok_intern_at_node(ctx, sib, sib->data.var.name_tok);
      if (!strcmp(aname, SW_ATTR_RESULT_BUILDER))
        return 1;
    }
    break;
  }
  return 0;
}

/** @brief For-in loop variable — each loop has its own scope, so reuse is legal. */
static int is_for_loop_var(const ASTNode *decl) {
  return decl && decl->kind == AST_PARAM && decl->parent &&
         decl->parent->kind == AST_FOR_STMT;
}

static int is_optional_binding_shadow(const ASTNode *decl) {
  return decl && decl->kind == AST_OPTIONAL_BINDING;
}

static int is_property_symbol(SymbolKind kind) {
  return kind == SYM_VAR || kind == SYM_LET;
}

static int decl_is_static_member(const ASTNode *decl) {
  return decl && (decl->modifiers & MOD_STATIC) != 0;
}

static int symbol_kinds_can_coexist(const Symbol *existing, SymbolKind incoming,
                                    const ASTNode *incoming_decl) {
  if (!existing) return 0;

  if (existing->kind == SYM_FUNC && incoming == SYM_FUNC)
    return 1;
  if (existing->kind == SYM_FUNC && is_property_symbol(incoming))
    return 1;
  if (is_property_symbol(existing->kind) && incoming == SYM_FUNC)
    return 1;
  if (is_property_symbol(existing->kind) && is_property_symbol(incoming) &&
      decl_is_static_member(existing->decl) != decl_is_static_member(incoming_decl))
    return 1;
  return 0;
}

/* A capture-list entry (`[self]`, `[weak self]`, `[fileManager]`, `[y = expr]`)
 * is not a redeclaration — it binds a closure-local name aliasing an outer
 * binding of the same name.  Treat ANY capture as a legitimate shadow rather
 * than "Redefinition of X" (the captured property/var lives in an outer scope;
 * the closure re-introduces the name locally). */
static int is_closure_capture_shadow(const ASTNode *decl, const char *name) {
  (void)name;
  return decl && decl->kind == AST_CLOSURE_CAPTURE;
}

static Symbol *insert_symbol(Scope *s, uint32_t h, const char *name,
                             SymbolKind kind, TypeInfo *type, ASTNode *decl) {
  Symbol *sym = malloc(sizeof(Symbol));
  if (!sym) return NULL;
  sym->name = name;
  sym->kind = kind;
  sym->type = type;
  sym->decl = decl;
  sym->next = s->buckets[h];
  sym->is_initialized = 1;
  sym->is_deferred = 0;
  sym->is_resolving = 0;
  s->buckets[h] = sym;
  return sym;
}

/**
 * @brief Defines a symbol in the current scope.
 *
 * Handles redeclaration suppression for several legitimate cases:
 *   - Wildcards (`let _ = ...`)
 *   - Operator functions inside type bodies
 *   - Function overloads (same name, different signatures)
 *   - @resultBuilder method overloads
 *   - Tuple decomposition placeholders (`let (x, y) = ...`)
 *   - For-in loop variables
 *
 * Reports a redeclaration error for all other cases.
 */
Symbol *sema_define(SemaContext *ctx, const char *name, SymbolKind kind,
                    TypeInfo *type, ASTNode *decl) {
  Scope *s = ctx->current_scope;
  uint32_t h = sym_hash(name);
  Symbol *compatible = NULL;

  for (Symbol *sym = s->buckets[h]; sym; sym = sym->next) {
    if (sym->name != name)
      continue;

    /* Legitimate redeclarations — suppress error */
    if (is_wildcard(name))            return sym;
    if (is_operator_in_type(decl, name)) return sym;
    if (name[0] == '(')              return sym; /* tuple decomposition */
    if (is_for_loop_var(decl))       return sym;
    if (is_result_builder_method(ctx, decl)) return sym;
    if (is_optional_binding_shadow(decl))
      return insert_symbol(s, h, name, kind, type, decl);
    if (is_closure_capture_shadow(decl, name))
      return insert_symbol(s, h, name, kind, type, decl);

    /* An imported/predeclared name (decl == NULL — a vocabulary type pulled in
     * by `import X`, or a builtin) does NOT conflict with a real source
     * declaration of the same name: Swift lets a module's own type shadow an
     * imported one of the same name.  Keep whichever is the real declaration;
     * never report a redefinition between an import and a local decl. */
    if (sym->decl == NULL || decl == NULL) {
      if (sym->decl == NULL && decl != NULL) { /* local declaration shadows it */
        sym->kind = kind;
        sym->type = type;
        sym->decl = decl;
      }
      return sym;
    }

    /* Function overloads and property/method pairs share a base name legally. */
    if (symbol_kinds_can_coexist(sym, kind, decl)) {
      if (!compatible) compatible = sym;
      continue;
    }

    /* Two file-private (private/fileprivate) declarations are file-scoped: in
     * whole-module analysis they typically live in different files and do not
     * conflict — Swift lets every file have its own `private typealias X` /
     * `private struct Helper`.  (Internal/public same-name IS a real
     * module-level clash, so both sides must be file-private here.) */
    if ((decl->modifiers & (MOD_PRIVATE | MOD_FILEPRIVATE)) &&
        (sym->decl->modifiers & (MOD_PRIVATE | MOD_FILEPRIVATE)))
      return sym;

    /* Actual redeclaration — report error */
    char msg[256];
    uint32_t line = 0, col = 0;
    if (decl && ctx->tokens) {
      SemaOriginState origin_state;
      sema_origin_enter(ctx, decl, &origin_state);
      if (!ctx->token_count || decl->tok_idx < ctx->token_count) {
        const Token *t = &ctx->tokens[decl->tok_idx];
        line = t->line;
        col = t->col;
      }
      sema_origin_leave(ctx, &origin_state);
    }
    snprintf(msg, sizeof(msg), "%u:%u: Redefinition of '%s'", line, col, name);
    sema_record(ctx, decl, msg);
    return sym;
  }

  Symbol *inserted = insert_symbol(s, h, name, kind, type, decl);
  return inserted ? inserted : compatible;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Scope Management
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Pushes a new child scope onto the scope stack. */
Scope *sema_push_scope(SemaContext *ctx) {
  Scope *s = calloc(1, sizeof(Scope));
  if (!s) return NULL;
  s->parent = ctx->current_scope;
  s->depth = ctx->current_scope ? ctx->current_scope->depth + 1 : 0;
  ctx->current_scope = s;
  return s;
}

/** @brief Pops the current scope, freeing all symbols in it. */
void sema_pop_scope(SemaContext *ctx) {
  if (!ctx->current_scope) return;
  Scope *old = ctx->current_scope;
  ctx->current_scope = old->parent;
  for (int b = 0; b < SCOPE_HASH_SIZE; b++) {
    Symbol *sym = old->buckets[b];
    while (sym) {
      Symbol *nx = sym->next;
      free(sym);
      sym = nx;
    }
  }
  free(old);
}

/** @brief Resets a SemaContext (internal — called by sema_init). */
void sema_ctx_reset(SemaContext *ctx, const Source *src, const Token *tokens,
               ASTArena *ast_arena, TypeArena *type_arena) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->src = src;
  ctx->tokens = tokens;
  ctx->ast_arena = ast_arena;
  ctx->type_arena = type_arena;
  sema_push_scope(ctx);
}

/**
 * @brief Frees scopes but not interned strings.
 *
 * Interned strings are referenced by AST nodes and TypeInfo values that
 * outlive sema analysis.  The caller frees ctx->intern when done.
 */
void sema_free(SemaContext *ctx) {
  while (ctx->current_scope)
    sema_pop_scope(ctx);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * AST Traversal Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Walks up the AST to find the innermost enclosing CLOSURE_EXPR. */
const ASTNode *find_ancestor_closure(const ASTNode *node) {
  for (const ASTNode *p = node ? node->parent : NULL; p; p = p->parent)
    if (p->kind == AST_CLOSURE_EXPR)
      return p;
  return NULL;
}

/** @brief Walks up the AST to find the enclosing type decl (class/struct/enum). */
const ASTNode *find_enclosing_type_decl(const ASTNode *node) {
  for (const ASTNode *p = node ? node->parent : NULL; p; p = p->parent)
    if (p->kind == AST_CLASS_DECL || p->kind == AST_STRUCT_DECL ||
        p->kind == AST_ENUM_DECL)
      return p;
  return NULL;
}

/** @brief Walks up the AST to find the enclosing struct decl (not class/enum). */
const ASTNode *find_enclosing_struct_decl(const ASTNode *node) {
  for (const ASTNode *p = node ? node->parent : NULL; p; p = p->parent)
    if (p->kind == AST_STRUCT_DECL)
      return p;
  return NULL;
}

/** @brief Returns the root IDENT_EXPR of a dotted expression chain (s in s.a.b). */
const ASTNode *root_ident_of_expr(const ASTNode *expr) {
  if (!expr) return NULL;
  if (expr->kind == AST_IDENT_EXPR) return expr;
  if (expr->kind == AST_MEMBER_EXPR) return root_ident_of_expr(expr->first_child);
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Type Query Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns the AST declaration node for a TY_NAMED type, or NULL. */
const ASTNode *named_type_decl(SemaContext *ctx, TypeInfo *t) {
  if (!t || t->kind != TY_NAMED) return NULL;
  if (t->named.decl) return (const ASTNode *)t->named.decl;
  if (t->named.name) {
    Symbol *sym = sema_lookup(ctx, t->named.name);
    if (sym && sym->decl) return sym->decl;
  }
  return NULL;
}

/** @brief Returns 1 if the type is a value type (struct or enum). */
int type_is_value_type(SemaContext *ctx, TypeInfo *t) {
  if (!t) return 0;
  if (t->kind == TY_OPTIONAL) t = t->inner;
  if (!t || t->kind != TY_NAMED) return 0;
  const ASTNode *decl = named_type_decl(ctx, t);
  if (decl)
    return decl->kind == AST_STRUCT_DECL || decl->kind == AST_ENUM_DECL;
  if (t->named.name) {
    const Symbol *sym = sema_lookup(ctx, t->named.name);
    if (sym && (sym->kind == SYM_STRUCT || sym->kind == SYM_ENUM))
      return 1;
  }
  return 0;
}

/** @brief Returns 1 if the named method on @p decl has the `mutating` modifier. */
int method_is_mutating(SemaContext *ctx, const ASTNode *decl,
                       const char *mname) {
  if (!decl || !mname) return 0;
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_BLOCK) continue;
    for (const ASTNode *ch = c->first_child; ch; ch = ch->next_sibling) {
      if (ch->kind != AST_FUNC_DECL) continue;
      const char *chn = tok_intern_at_node(ctx, ch, ch->data.func.name_tok);
      if (chn && strcmp(chn, mname) == 0)
        return (ch->modifiers & MOD_MUTATING) != 0;
    }
  }
  return 0;
}

/** @brief Returns 1 if @p name is a non-static stored property of the struct. */
int is_stored_property_of_struct(SemaContext *ctx, const ASTNode *struct_decl,
                                 const char *name) {
  if (!struct_decl || struct_decl->kind != AST_STRUCT_DECL || !name)
    return 0;
  for (const ASTNode *c = struct_decl->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_BLOCK) continue;
    for (const ASTNode *m = c->first_child; m; m = m->next_sibling) {
      if (m->kind != AST_VAR_DECL && m->kind != AST_LET_DECL) continue;
      if (m->modifiers & MOD_STATIC) continue;
      if (!m->data.var.name_tok) continue;
      const char *pname = tok_intern_at_node(ctx, m, m->data.var.name_tok);
      if (pname && strcmp(pname, name) == 0) return 1;
    }
    break;
  }
  return 0;
}

/** @brief Returns 1 if sym is a non-static instance member of type_decl. */
int symbol_is_instance_member_of(const Symbol *sym, const ASTNode *type_decl) {
  if (!sym || !sym->decl || !type_decl) return 0;
  if (sym->decl->modifiers & MOD_STATIC) return 0;
  if (sym->kind != SYM_VAR && sym->kind != SYM_LET && sym->kind != SYM_FUNC)
    return 0;
  for (const ASTNode *p = sym->decl->parent; p; p = p->parent) {
    if (p->kind == AST_CLASS_DECL || p->kind == AST_STRUCT_DECL ||
        p->kind == AST_ENUM_DECL)
      return (p == type_decl);
  }
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Error Reporting & "Did You Mean?" Suggestions
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Levenshtein edit distance (for "did you mean?" suggestions). */
int lev_distance(const char *a, const char *b) {
  size_t na = strlen(a), nb = strlen(b);
  if (na > 64 || nb > 64) return 999;
  int d[65][65];
  for (size_t i = 0; i <= na; i++) d[i][0] = (int)i;
  for (size_t j = 0; j <= nb; j++) d[0][j] = (int)j;
  for (size_t i = 1; i <= na; i++)
    for (size_t j = 1; j <= nb; j++) {
      int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      int x = d[i - 1][j] + 1;
      int y = d[i][j - 1] + 1;
      int z = d[i - 1][j - 1] + cost;
      d[i][j] = (x < y) ? (x < z ? x : z) : (y < z ? y : z);
    }
  return d[na][nb];
}

/** @brief Finds the closest type name in scope (max edit distance 3). */
const char *sema_find_similar_type_name(SemaContext *ctx, const char *name) {
  const char *best = NULL;
  int best_d = 4;
  for (Scope *sc = ctx->current_scope; sc; sc = sc->parent) {
    for (unsigned i = 0; i < SCOPE_HASH_SIZE; i++) {
      for (Symbol *s = sc->buckets[i]; s; s = s->next) {
        if (!(s->kind == SYM_TYPE || s->kind == SYM_STRUCT ||
              s->kind == SYM_CLASS || s->kind == SYM_ENUM ||
              s->kind == SYM_PROTOCOL || s->kind == SYM_TYPEALIAS))
          continue;
        if (!s->name) continue;
        int d = lev_distance(name, s->name);
        if (d < best_d && d > 0) { best_d = d; best = s->name; }
      }
    }
  }
  return best;
}

/* The last retained slot is reserved for the overflow summary; real
 * diagnostics fill slots [0, SEMA_DIAG_MAX-1). */
#define SEMA_OVERFLOW_SLOT (SEMA_DIAG_MAX - 1)

/* Grows ctx->diags to hold at least `need` entries (capped at SEMA_DIAG_MAX).
 * Returns 1 on success, 0 on allocation failure. */
static int ensure_diag_cap(SemaContext *ctx, uint32_t need) {
  if (need <= ctx->diag_cap) return 1;
  uint32_t nc = ctx->diag_cap ? ctx->diag_cap : SEMA_DIAG_INITIAL_CAP;
  while (nc < need) nc *= 2;
  if (nc > SEMA_DIAG_MAX) nc = SEMA_DIAG_MAX;
  SemaDiag *nd = realloc(ctx->diags, (size_t)nc * sizeof *nd);
  if (!nd) return 0;
  ctx->diags = nd;
  ctx->diag_cap = nc;
  return 1;
}

/* Common tail: stores msg/line/col/range into ctx, growing the diagnostics
 * array on demand and folding everything past the DoS ceiling into a single
 * "N more suppressed" summary. Treats `msg` as already formatted. */
static void sema_record(SemaContext *ctx, const ASTNode *node, const char *msg) {
  if (ctx->error_count >= SEMA_OVERFLOW_SLOT) {
    ctx->suppressed_count++;
    if (!ensure_diag_cap(ctx, SEMA_DIAG_MAX)) return;
    SemaDiag *d = &ctx->diags[SEMA_OVERFLOW_SLOT];
    snprintf(d->msg, sizeof d->msg,
             "... and %u more diagnostics suppressed", ctx->suppressed_count);
    if (ctx->error_count == SEMA_OVERFLOW_SLOT) {
      d->line = d->col = d->start = d->end = 0;
      d->file = NULL;
      ctx->error_count = SEMA_OVERFLOW_SLOT + 1;
    }
    return;
  }
  if (!ensure_diag_cap(ctx, ctx->error_count + 1)) {
    ctx->suppressed_count++; /* OOM: drop this diagnostic rather than crash */
    return;
  }
  uint32_t line = 0, col = 0, start = 0, end = 0;
  const char *file = ctx->src ? ctx->src->filename : NULL;
  if (node && ctx->tokens) {
    SemaOriginState origin_state;
    sema_origin_enter(ctx, node, &origin_state);
    /* Inside the switch ctx->src is the node's OWN file (whole-module), so this
     * is the file the line/col below are relative to. */
    file = ctx->src ? ctx->src->filename : file;
    if (!ctx->token_count || node->tok_idx < ctx->token_count) {
      const Token *t = &ctx->tokens[node->tok_idx];
      line = t->line;
      col = t->col;
      start = t->pos;
      /* node->tok_end is one-past-the-last token index; if set, the range
       * extends to the last token's end. Otherwise fall back to the start
       * token's own length. */
      if (node->tok_end > node->tok_idx &&
          (!ctx->token_count || node->tok_end <= ctx->token_count)) {
        const Token *last = &ctx->tokens[node->tok_end - 1];
        end = last->pos + last->len;
      } else {
        end = t->pos + t->len;
      }
    }
    sema_origin_leave(ctx, &origin_state);
  }
  SemaDiag *d = &ctx->diags[ctx->error_count++];
  d->line = line;
  d->col = col;
  d->start = start;
  d->end = end;
  d->file = file;
  snprintf(d->msg, sizeof d->msg, "%s", msg);
}

/** @brief Records a semantic error with printf-style formatting. */
void sema_error(SemaContext *ctx, const ASTNode *node, const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  sema_record(ctx, node, msg);
}

/** @brief Like sema_error(), but appends " Did you mean 'X'?" when suggestion is non-NULL. */
void sema_error_suggest(SemaContext *ctx, const ASTNode *node,
                        const char *suggestion, const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (suggestion && strlen(msg) + 4 + strlen(suggestion) + 2 < sizeof(msg))
    snprintf(msg + strlen(msg), sizeof(msg) - (size_t)strlen(msg),
             " Did you mean '%s'?", suggestion);
  sema_record(ctx, node, msg);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Module Import & Builtin Type Lookup
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Predeclares one external type name into the current scope as TY_NAMED.
 *
 * Used both for `import`ed module symbols and for the enclosing module's own
 * declarations (sibling files): the frontend sees only one file, so names that
 * live elsewhere must be injected so references resolve instead of erroring.
 */
void sema_predeclare_module_type(SemaContext *ctx, const char *name) {
  if (!name || !*name) return;
  const char *iname = sema_intern(ctx, name, strlen(name));
  if (sema_lookup(ctx, iname)) return;
  TypeInfo *ti = type_arena_alloc(ctx->type_arena);
  if (!ti) return;
  ti->kind = TY_NAMED;
  ti->named.name = iname;
  sema_define(ctx, iname, SYM_TYPE, ti, NULL);
}

/* Membership test for one protocol name in a space-joined conformance string. */
static int conf_str_has(const char *confs, const char *proto) {
  if (!confs) return 0;
  size_t plen = strlen(proto);
  for (const char *q = confs; *q;) {
    while (*q == ' ') q++;
    const char *e = q;
    while (*e && *e != ' ') e++;
    if ((size_t)(e - q) == plen && memcmp(q, proto, plen) == 0) return 1;
    q = e;
  }
  return 0;
}

/* Fold a vocabulary's per-type conformances (vocab v3) into the conformance
 * table, so SDK types like CGFloat answer the ExpressibleBy*Literal queries that
 * drive literal coercion.  The vocab clause only lists *direct* conformances, so
 * a numeric protocol implies its literal protocol (BinaryFloatingPoint ⇒ both
 * float+integer literals; BinaryInteger/Numeric ⇒ integer literals).  Only the
 * static literal-protocol names are registered (stable lifetime); the table
 * keys on the vocab's stable type-name pointer. */
void sema_load_vocab_conformances(SemaContext *ctx, const struct MSFVocab *v) {
  if (!ctx || !v || !ctx->conformance_table) return;
  ConformanceTable *ct = ctx->conformance_table;
  size_t nm = msf_vocab_module_count(v);
  for (size_t m = 0; m < nm; m++) {
    size_t nt = msf_vocab_type_count(v, m);
    for (size_t t = 0; t < nt; t++) {
      const char *confs = msf_vocab_type_conformances(v, m, t);
      if (!confs) continue;
      const char *name = msf_vocab_type_name(v, m, t);
      if (!name) continue;

      int is_float = conf_str_has(confs, SW_PROTO_FLOATING_POINT) ||
                     conf_str_has(confs, SW_PROTO_BINARY_FLOATING_PT) ||
                     conf_str_has(confs, SW_PROTO_EXPR_BY_FLOAT_LIT);
      int is_int = is_float ||                 /* float literals ⇒ integer too */
                   conf_str_has(confs, SW_PROTO_BINARY_INTEGER) ||
                   conf_str_has(confs, SW_PROTO_FIXED_WIDTH_INTEGER) ||
                   conf_str_has(confs, SW_PROTO_NUMERIC) ||
                   conf_str_has(confs, SW_PROTO_EXPR_BY_INT_LIT);
      int is_str = conf_str_has(confs, SW_PROTO_EXPR_BY_STRING_LIT);
      int is_bool = conf_str_has(confs, SW_PROTO_EXPR_BY_BOOL_LIT);

      if (is_float && !conformance_table_has(ct, name, SW_PROTO_EXPR_BY_FLOAT_LIT))
        conformance_table_add(ct, name, SW_PROTO_EXPR_BY_FLOAT_LIT);
      if (is_int && !conformance_table_has(ct, name, SW_PROTO_EXPR_BY_INT_LIT))
        conformance_table_add(ct, name, SW_PROTO_EXPR_BY_INT_LIT);
      if (is_str && !conformance_table_has(ct, name, SW_PROTO_EXPR_BY_STRING_LIT))
        conformance_table_add(ct, name, SW_PROTO_EXPR_BY_STRING_LIT);
      if (is_bool && !conformance_table_has(ct, name, SW_PROTO_EXPR_BY_BOOL_LIT))
        conformance_table_add(ct, name, SW_PROTO_EXPR_BY_BOOL_LIT);

      /* Register the raw direct conformances too (superclass + protocols), each
       * interned so the table holds a NUL-terminated stable key.  This lets
       * subtype queries answer "is UIToolbar a UIView?" (the inheritance clause
       * `class UIToolbar : UIView` is recorded as a conformance). */
      for (const char *q = confs; *q;) {
        while (*q == ' ') q++;
        const char *e = q;
        while (*e && *e != ' ') e++;
        if (e > q) {
          const char *proto = sema_intern(ctx, q, (size_t)(e - q));
          if (proto && !conformance_table_has(ct, name, proto))
            conformance_table_add(ct, name, proto);
        }
        q = e;
      }
    }
  }
}

/** @brief Attaches a runtime module vocabulary consulted by sema_import_module. */
void sema_set_sdk_vocabulary(SemaContext *ctx, const struct MSFVocab *vocab) {
  if (!ctx) return;
  ctx->sdk_vocab = vocab;
  sema_load_vocab_conformances(ctx, vocab);
}

void sema_set_vocabulary(SemaContext *ctx, const struct MSFVocab *vocab) {
  if (!ctx) return;
  ctx->vocab = vocab;
  sema_load_vocab_conformances(ctx, vocab);
}

/** @brief Imports all public type names of a module into the current scope.
 *
 * A runtime vocabulary (.msfvocab), if attached, wins for modules it knows;
 * otherwise we fall back to the host-provided compiled module table. This lets
 * a host with no SDK (browser/Windows) resolve imports from a shipped artifact
 * while a host with compiled stubs keeps working unchanged. */
void sema_import_module(SemaContext *ctx, const char *module_name) {
  if (ctx->vocab) {
    size_t n = 0;
    const char *const *names = msf_vocab_module_types(ctx->vocab, module_name, &n);
    if (names) {
      for (size_t i = 0; i < n; i++)
        sema_predeclare_module_type(ctx, names[i]);
      return;
    }
  }
  const ModuleStub *stub = module_stub_find(module_name);
  if (!stub) return;
  for (uint32_t i = 0; i < stub->count; i++)
    sema_predeclare_module_type(ctx, stub->types[i].name);
}

/**
 * Table-driven builtin type lookup.  Adding a new builtin = one entry.
 * All names come from builtin_names.h — no magic strings.
 */
typedef struct {
  const char *name;
  TypeInfo  **type_ptr;
} BuiltinTypeEntry;

#define BTE(n, t) {n, &t}
static const BuiltinTypeEntry BUILTIN_TYPE_TABLE[] = {
    BTE(SW_TYPE_VOID, TY_BUILTIN_VOID),
    BTE(SW_TYPE_BOOL, TY_BUILTIN_BOOL),
    BTE(SW_TYPE_INT, TY_BUILTIN_INT),
    BTE(SW_TYPE_INT8, TY_BUILTIN_INT),
    BTE(SW_TYPE_INT16, TY_BUILTIN_INT),
    BTE(SW_TYPE_INT32, TY_BUILTIN_INT),
    BTE(SW_TYPE_INT64, TY_BUILTIN_INT),
    BTE(SW_TYPE_UINT, TY_BUILTIN_UINT),
    BTE(SW_TYPE_UINT8, TY_BUILTIN_UINT8),
    BTE(SW_TYPE_UINT16, TY_BUILTIN_UINT16),
    BTE(SW_TYPE_UINT32, TY_BUILTIN_UINT32),
    BTE(SW_TYPE_UINT64, TY_BUILTIN_UINT64),
    BTE(SW_TYPE_STRING, TY_BUILTIN_STRING),
    BTE(SW_TYPE_DOUBLE, TY_BUILTIN_DOUBLE),
    BTE(SW_TYPE_FLOAT64, TY_BUILTIN_DOUBLE),
    BTE(SW_TYPE_FLOAT, TY_BUILTIN_FLOAT),
    BTE(SW_TYPE_FLOAT32, TY_BUILTIN_FLOAT),
    BTE(SW_TYPE_CHARACTER, TY_BUILTIN_STRING),
    BTE("Substring", TY_BUILTIN_STRING),
    {NULL, NULL}
};
#undef BTE

/** @brief Resolves a type name to a builtin singleton (TY_BUILTIN_*), or NULL. */
TypeInfo *resolve_builtin(const char *name) {
  for (const BuiltinTypeEntry *e = BUILTIN_TYPE_TABLE; e->name; e++)
    if (!strcmp(name, e->name))
      return *e->type_ptr;
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Public API — Lifecycle & Diagnostics
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initializes a heap-allocated SemaContext with all subsystems.
 *
 * Initializes the Unicode decoder (idempotent), pushes the global scope,
 * allocates the conformance table and associated-type table.
 */
SemaContext *sema_init(const Source *src, const Token *tokens,
                         ASTArena *ast_arena, TypeArena *type_arena) {
  decoder_init();

  SemaContext *ctx = calloc(1, sizeof(SemaContext));
  if (!ctx) return NULL;
  sema_ctx_reset(ctx, src, tokens, ast_arena, type_arena);

  ctx->conformance_table = calloc(1, sizeof(ConformanceTable));
  if (ctx->conformance_table)
    conformance_table_init_builtins(ctx->conformance_table);

  ctx->assoc_type_table = calloc(1, sizeof(AssocTypeTable));
  if (ctx->assoc_type_table)
    assoc_type_table_init(ctx->assoc_type_table);

  return ctx;
}

/** @brief Destroys a SemaContext and frees all owned resources. */
void sema_destroy(SemaContext *ctx) {
  if (!ctx) return;
  sema_member_index_free(ctx);
  sema_nested_type_index_free(ctx);
  sema_origin_free(ctx);
  free(ctx->tok_cache);
  free(ctx->diags);
  sema_free(ctx);
  if (ctx->intern) {
    intern_pool_release(ctx->intern);
    free(ctx->intern);
  }
  if (ctx->conformance_table) {
    conformance_index_free(ctx->conformance_table);
    free(ctx->conformance_table->entries);
    free(ctx->conformance_table);
  }
  if (ctx->witness_members) {
    conformance_index_free(ctx->witness_members);
    free(ctx->witness_members->entries);
    free(ctx->witness_members);
  }
  if (ctx->witness_inherits) {
    conformance_index_free(ctx->witness_inherits);
    free(ctx->witness_inherits->entries);
    free(ctx->witness_inherits);
  }
  if (ctx->assoc_type_table) {
    free(ctx->assoc_type_table->entries);
    free(ctx->assoc_type_table);
  }
  free(ctx);
}

/** @brief Returns the number of recorded semantic errors. */
uint32_t sema_error_count(const SemaContext *ctx) {
  return ctx ? ctx->error_count : 0;
}

/** @brief Returns the error message at @p index, or "" if out of range. */
const char *sema_error_message(const SemaContext *ctx, uint32_t index) {
  return (!ctx || index >= ctx->error_count) ? "" : ctx->diags[index].msg;
}

/** @brief Returns the line number of the error at @p index, or 0. */
uint32_t sema_error_line(const SemaContext *ctx, uint32_t index) {
  return (!ctx || index >= ctx->error_count) ? 0 : ctx->diags[index].line;
}

/** @brief Returns the column number of the error at @p index, or 0. */
uint32_t sema_error_col(const SemaContext *ctx, uint32_t index) {
  return (!ctx || index >= ctx->error_count) ? 0 : ctx->diags[index].col;
}

/** @brief Returns the owning file path of the error at @p index, or NULL.
 *  In whole-module analysis this is the node's origin file. */
const char *sema_error_file(const SemaContext *ctx, uint32_t index) {
  return (!ctx || index >= ctx->error_count) ? NULL : ctx->diags[index].file;
}

/** @brief Returns the conformance table for inspection by callers. */
const ConformanceTable *sema_conformance_table(const SemaContext *ctx) {
  return ctx ? ctx->conformance_table : NULL;
}

/** @brief Returns the source byte offset where the error starts. */
uint32_t sema_error_start(const SemaContext *ctx, uint32_t index) {
  return (!ctx || index >= ctx->error_count) ? 0 : ctx->diags[index].start;
}

/** @brief Returns the source byte offset where the error ends (exclusive). */
uint32_t sema_error_end(const SemaContext *ctx, uint32_t index) {
  return (!ctx || index >= ctx->error_count) ? 0 : ctx->diags[index].end;
}
