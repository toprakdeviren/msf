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
      SW_PROTO_CODABLE, SW_PROTO_SENDABLE, SW_PROTO_VIEW,
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
TypeInfo *resolve_type_ident(SemaContext *ctx, const ASTNode *tnode) {
  const char *iname = tok_intern(ctx, tnode->tok_idx);
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

  /* 6. Forward type reference: scan AST root */
  result = resolve_forward_decl(ctx, iname);
  if (result) return result;

  /* 7. Not found — report error, return placeholder TY_NAMED */
  report_undeclared_type(ctx, tnode, iname);
  TypeInfo *ti = make_type(ctx, TY_NAMED);
  if (!ti) return NULL;
  ti->named.name = iname;
  return ti;
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
  if (n == 0) return ti;

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

/**
 * @brief Validates that a type conforms to Hashable (required for Dict/Set keys).
 *
 * Builtin types are always Hashable; user types must have explicit conformance.
 */
static void check_hashable(SemaContext *ctx, const ASTNode *site,
                           const TypeInfo *key_type, const char *container) {
  if (!ctx->conformance_table || !key_type) return;
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

  if (arg_count == 1 && strcmp(base_name, SW_TYPE_ARRAY) == 0) {
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
    arg_count++;
  if (arg_count == 0)
    return base_ti;

  TypeInfo **args = malloc(arg_count * sizeof(TypeInfo *));
  if (!args) return NULL;
  uint32_t i = 0;
  for (const ASTNode *c = tnode->first_child; c; c = c->next_sibling)
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
  init_type_resolvers();
  TypeResolver fn =
      (tnode->kind < AST__COUNT) ? type_resolvers[tnode->kind] : NULL;
  return fn ? fn(ctx, tnode) : NULL;
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
