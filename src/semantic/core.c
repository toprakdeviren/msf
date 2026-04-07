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
#include <core.h>      /* decoder_init() */
#include <normalize.h> /* decoder_normalize_utf8(), decoder_is_normalized_utf8() */

/* These are defined in access.c but used by sema_define's visibility checks. */
int access_rank(uint32_t mods);
uint32_t type_effective_access(SemaContext *ctx, TypeInfo *ty);
int private_member_visible(SemaContext *ctx, const ASTNode *member_decl,
                                const ASTNode *owning_type_decl);

/* ═══════════════════════════════════════════════════════════════════════════════
 * Precedence Group Registry
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Registers a precedence group name for later duplicate checking. */
void sema_add_precedence_group_name(SemaContext *ctx, const ASTNode *node) {
  if (ctx->pg_count >= SEMA_PG_NAMES_MAX || !node->data.var.name_tok)
    return;
  const Token *t = &ctx->tokens[node->data.var.name_tok];
  const char *name = sema_intern(ctx, ctx->src->data + t->pos, t->len);
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

struct InternPool {
  char        buf[INTERN_BUF_SIZE];       /**< Contiguous string storage. */
  size_t      used;                       /**< Bytes consumed in buf. */
  const char *table[INTERN_POOL_CAP];     /**< Open-addressing hash table. */
  uint32_t    lengths[INTERN_POOL_CAP];   /**< Cached lengths for fast reject. */
  size_t      count;                      /**< Entries in the table. */
};

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
 * Otherwise copies it into the buffer and inserts into the hash table.
 * NFC-normalizes non-ASCII input before interning.
 *
 * Returns "<intern_overflow>" sentinel on buffer/table exhaustion.
 *
 * @param ctx  Sema context (owns the intern pool).
 * @param str  String bytes to intern.
 * @param len  String length.
 * @return     Interned pointer (valid until the pool is freed).
 */
const char *sema_intern(SemaContext *ctx, const char *str, size_t len) {
  if (!ctx->intern) {
    ctx->intern = calloc(1, sizeof(InternPool));
    if (!ctx->intern)
      return "<intern_overflow>";
  }

  /* NFC normalization — zero overhead for ASCII (quick-check fast path). */
  uint8_t nfc_stack[256];
  const char *src = str;
  size_t src_len = len;

  if (!decoder_is_normalized_utf8((const uint8_t *)str, len, DECODER_NFC)) {
    size_t nfc_len = 0;
    int rc = decoder_normalize_utf8((const uint8_t *)str, len, DECODER_NFC,
                                    nfc_stack, sizeof(nfc_stack), &nfc_len);
    if (rc == DECODER_SUCCESS) {
      src = (const char *)nfc_stack;
      src_len = nfc_len;
    }
  }

  InternPool *pool = ctx->intern;
  uint32_t h = intern_hash(src, src_len);
  uint32_t mask = INTERN_POOL_CAP - 1;
  uint32_t slot = h & mask;

  /* Probe for existing entry. */
  for (uint32_t probe = 0; probe < INTERN_POOL_CAP; probe++) {
    uint32_t idx = (slot + probe) & mask;
    if (!pool->table[idx])
      break;
    if (pool->lengths[idx] == (uint32_t)src_len &&
        memcmp(pool->table[idx], src, src_len) == 0)
      return pool->table[idx];
  }

  /* Not found — insert. */
  if (pool->used + src_len + 1 > INTERN_BUF_SIZE)
    return "<intern_overflow>";
  if (pool->count >= (INTERN_POOL_CAP * 3 / 4))
    return "<intern_overflow>";

  char *p = pool->buf + pool->used;
  memcpy(p, src, src_len);
  p[src_len] = '\0';
  pool->used += src_len + 1;

  for (uint32_t probe = 0; probe < INTERN_POOL_CAP; probe++) {
    uint32_t idx = (slot + probe) & mask;
    if (!pool->table[idx]) {
      pool->table[idx] = p;
      pool->lengths[idx] = (uint32_t)src_len;
      pool->count++;
      return p;
    }
  }
  return "<intern_overflow>";
}

/** @brief Interns the text of token at @p tok_idx. */
const char *tok_intern(SemaContext *ctx, uint32_t tok_idx) {
  const Token *t = &ctx->tokens[tok_idx];
  return sema_intern(ctx, ctx->src->data + t->pos, t->len);
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
  if (!type_decl->parent)
    return 0;
  for (const ASTNode *sib = type_decl->parent->first_child; sib;
       sib = sib->next_sibling) {
    if (sib->next_sibling != type_decl)
      continue;
    if (sib->kind == AST_ATTRIBUTE) {
      const Token *at = &ctx->tokens[sib->data.var.name_tok];
      const char *aname = sema_intern(ctx, ctx->src->data + at->pos, at->len);
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

  for (Symbol *sym = s->buckets[h]; sym; sym = sym->next) {
    if (sym->name != name)
      continue;

    /* Legitimate redeclarations — suppress error */
    if (is_wildcard(name))            return sym;
    if (is_operator_in_type(decl, name)) return sym;
    if (name[0] == '(')              return sym; /* tuple decomposition */
    if (is_for_loop_var(decl))       return sym;
    if (is_result_builder_method(ctx, decl)) return sym;

    /* Function overloads: allow multiple SYM_FUNC with same name */
    if (decl && decl->kind == AST_FUNC_DECL && sym->kind == SYM_FUNC) {
      Symbol *new_sym = malloc(sizeof(Symbol));
      if (!new_sym) return sym;
      new_sym->name = name;
      new_sym->kind = kind;
      new_sym->type = type;
      new_sym->decl = decl;
      new_sym->next = s->buckets[h]; /* prepend to head, not to sym */
      new_sym->is_initialized = 1;
      s->buckets[h] = new_sym;
      return new_sym;
    }

    /* Actual redeclaration — report error */
    uint32_t line = 0, col = 0;
    if (decl && ctx->tokens) {
      const Token *t = &ctx->tokens[decl->tok_idx];
      line = t->line;
      col = t->col;
    }
    if (ctx->error_count < 32)
      snprintf(ctx->errors[ctx->error_count++], 256,
               "%u:%u: Redefinition of '%s'", line, col, name);
    return sym;
  }

  /* New symbol — insert at head of bucket. */
  Symbol *sym = malloc(sizeof(Symbol));
  if (!sym) return NULL;
  sym->name = name;
  sym->kind = kind;
  sym->type = type;
  sym->decl = decl;
  sym->next = s->buckets[h];
  sym->is_initialized = 1;
  s->buckets[h] = sym;
  return sym;
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
      const char *chn = tok_intern(ctx, ch->data.func.name_tok);
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
      const Token *t = &ctx->tokens[m->data.var.name_tok];
      const char *pname = sema_intern(ctx, ctx->src->data + t->pos, t->len);
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

/** @brief Records a semantic error with printf-style formatting. */
void sema_error(SemaContext *ctx, const ASTNode *node, const char *fmt, ...) {
  if (ctx->error_count >= 32) return;
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  uint32_t line = 0, col = 0;
  if (node && ctx->tokens) {
    const Token *t = &ctx->tokens[node->tok_idx];
    line = t->line;
    col = t->col;
  }
  ctx->error_line[ctx->error_count] = line;
  ctx->error_col[ctx->error_count] = col;
  snprintf(ctx->errors[ctx->error_count++], 256, "%s", msg);
}

/** @brief Like sema_error(), but appends " Did you mean 'X'?" when suggestion is non-NULL. */
void sema_error_suggest(SemaContext *ctx, const ASTNode *node,
                        const char *suggestion, const char *fmt, ...) {
  if (ctx->error_count >= 32) return;
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (suggestion && strlen(msg) + 4 + strlen(suggestion) + 2 < sizeof(msg))
    snprintf(msg + strlen(msg), sizeof(msg) - (size_t)strlen(msg),
             " Did you mean '%s'?", suggestion);

  uint32_t line = 0, col = 0;
  if (node && ctx->tokens) {
    const Token *t = &ctx->tokens[node->tok_idx];
    line = t->line;
    col = t->col;
  }
  ctx->error_line[ctx->error_count] = line;
  ctx->error_col[ctx->error_count] = col;
  snprintf(ctx->errors[ctx->error_count++], 256, "%s", msg);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Module Import & Builtin Type Lookup
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Imports all public type names from a module stub into the current scope. */
void sema_import_module(SemaContext *ctx, const char *module_name) {
  const ModuleStub *stub = module_stub_find(module_name);
  if (!stub) return;
  for (uint32_t i = 0; i < stub->count; i++) {
    const ModuleTypeEntry *e = &stub->types[i];
    const char *iname = sema_intern(ctx, e->name, strlen(e->name));
    if (sema_lookup(ctx, iname)) continue;
    TypeInfo *ti = type_arena_alloc(ctx->type_arena);
    if (!ti) continue;
    ti->kind = TY_NAMED;
    ti->named.name = iname;
    sema_define(ctx, iname, SYM_TYPE, ti, NULL);
  }
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
    BTE(SW_TYPE_CGFLOAT, TY_BUILTIN_DOUBLE),
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
  sema_free(ctx);
  free(ctx->intern);
  free(ctx->conformance_table);
  free(ctx->assoc_type_table);
  free(ctx);
}

/** @brief Returns the number of recorded semantic errors. */
uint32_t sema_error_count(const SemaContext *ctx) {
  return ctx ? ctx->error_count : 0;
}

/** @brief Returns the error message at @p index, or "" if out of range. */
const char *sema_error_message(const SemaContext *ctx, uint32_t index) {
  return (!ctx || index >= ctx->error_count) ? "" : ctx->errors[index];
}

/** @brief Returns the line number of the error at @p index, or 0. */
uint32_t sema_error_line(const SemaContext *ctx, uint32_t index) {
  return (!ctx || index >= ctx->error_count) ? 0 : ctx->error_line[index];
}

/** @brief Returns the column number of the error at @p index, or 0. */
uint32_t sema_error_col(const SemaContext *ctx, uint32_t index) {
  return (!ctx || index >= ctx->error_count) ? 0 : ctx->error_col[index];
}
