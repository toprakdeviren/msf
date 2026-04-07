/**
 * @file conformance_table.c
 * @brief Protocol conformance tracking and associated-type resolution.
 *
 * The conformance table records "Type X conforms to Protocol Y" relationships.
 * It is populated with Swift stdlib builtins at init and extended during sema
 * pass 1 as user-defined conformances are discovered.
 *
 * @note String comparisons use strcmp() rather than pointer equality because
 *       some entries (e.g. builtin conformances) use static string literals
 *       that are not interned.  If all strings were guaranteed interned,
 *       these could be replaced with pointer equality for O(1) lookups.
 */
#include "private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Conformance Table
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Registers an unconditional conformance: type_name conforms to protocol_name. */
void conformance_table_add(ConformanceTable *ct, const char *type_name,
                           const char *protocol_name) {
  if (!ct || ct->count >= CONFORMANCE_TABLE_MAX)
    return;
  ct->entries[ct->count].type_name = type_name;
  ct->entries[ct->count].protocol_name = protocol_name;
  ct->entries[ct->count].where_ast = NULL;
  ct->count++;
}

/** @brief Registers a conditional conformance with an associated where clause AST. */
void conformance_table_add_conditional(ConformanceTable *ct,
                                       const char *type_name,
                                       const char *protocol_name,
                                       const void *where_ast) {
  if (!ct || ct->count >= CONFORMANCE_TABLE_MAX)
    return;
  ct->entries[ct->count].type_name = type_name;
  ct->entries[ct->count].protocol_name = protocol_name;
  ct->entries[ct->count].where_ast = where_ast;
  ct->count++;
}

/** @brief Returns the where-clause AST for a conditional conformance, or NULL. */
const void *conformance_table_get_where(const ConformanceTable *ct,
                                        const char *type_name,
                                        const char *protocol_name) {
  if (!ct || !type_name || !protocol_name)
    return NULL;
  for (uint32_t i = 0; i < ct->count; i++) {
    if (ct->entries[i].type_name && ct->entries[i].protocol_name &&
        strcmp(ct->entries[i].type_name, type_name) == 0 &&
        strcmp(ct->entries[i].protocol_name, protocol_name) == 0)
      return ct->entries[i].where_ast;
  }
  return NULL;
}

/** @brief Returns non-zero if type_name conforms to protocol_name. */
int conformance_table_has(const ConformanceTable *ct, const char *type_name,
                          const char *protocol_name) {
  if (!ct || !type_name || !protocol_name)
    return 0;
  for (uint32_t i = 0; i < ct->count; i++) {
    if (ct->entries[i].type_name && ct->entries[i].protocol_name &&
        strcmp(ct->entries[i].type_name, type_name) == 0 &&
        strcmp(ct->entries[i].protocol_name, protocol_name) == 0)
      return 1;
  }
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Associated Type Table
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Resets the associated-type table to empty. */
void assoc_type_table_init(AssocTypeTable *at) {
  if (at)
    at->count = 0;
}

/** @brief Binds an associated type: (type, protocol, assoc) -> concrete name. */
void assoc_type_table_add(AssocTypeTable *at, const char *type_name,
                          const char *protocol_name, const char *assoc_name,
                          const char *concrete_type_name) {
  if (!at || at->count >= ASSOC_TYPE_TABLE_MAX || !type_name ||
      !protocol_name || !assoc_name || !concrete_type_name)
    return;
  at->entries[at->count].type_name = type_name;
  at->entries[at->count].protocol_name = protocol_name;
  at->entries[at->count].assoc_name = assoc_name;
  at->entries[at->count].concrete_type_name = concrete_type_name;
  at->count++;
}

/** @brief Looks up the concrete type name for an associated-type binding. */
const char *assoc_type_table_get(const AssocTypeTable *at,
                                 const char *type_name,
                                 const char *protocol_name,
                                 const char *assoc_name) {
  if (!at || !type_name || !protocol_name || !assoc_name)
    return NULL;
  for (uint32_t i = 0; i < at->count; i++) {
    if (at->entries[i].type_name && at->entries[i].protocol_name &&
        at->entries[i].assoc_name &&
        strcmp(at->entries[i].type_name, type_name) == 0 &&
        strcmp(at->entries[i].protocol_name, protocol_name) == 0 &&
        strcmp(at->entries[i].assoc_name, assoc_name) == 0)
      return at->entries[i].concrete_type_name;
  }
  return NULL;
}

/**
 * @brief Resolves an associated type to a concrete TypeInfo.
 *
 * Given a concrete type (e.g. Array<Int>) and a protocol's associated type
 * name (e.g. "Element"), returns the bound concrete type (e.g. Int).
 *
 * Resolution order:
 *   1. TY_GENERIC_INST: match generic param name to assoc_name
 *   2. Explicit bindings from assoc_type_table
 *   3. Builtin type name lookup (Int, String, Bool, etc.)
 *   4. Symbol table lookup
 *   5. Synthesize a TY_NAMED as fallback
 */
TypeInfo *resolve_assoc_type_to_concrete(SemaContext *ctx,
                                         const TypeInfo *concrete_type,
                                         const char *protocol_name,
                                         const char *assoc_name) {
  if (!ctx || !concrete_type || !assoc_name)
    return NULL;
  if (concrete_type->kind == TY_GENERIC_INST && concrete_type->generic.base &&
      concrete_type->generic.args && concrete_type->generic.arg_count > 0) {
    const char *nominal_name = concrete_type->generic.base->named.name;
    if (!nominal_name)
      goto table_lookup;
    const ASTNode *decl = concrete_type->generic.base->named.decl;
    if (!decl) {
      const Symbol *sym = sema_lookup(ctx, nominal_name);
      if (sym && sym->decl)
        decl = sym->decl;
    }
    if (decl) {
      uint32_t idx = 0;
      for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
        if (c->kind != AST_GENERIC_PARAM || !c->type ||
            c->type->kind != TY_GENERIC_PARAM)
          continue;
        if (c->type->param.name && strcmp(c->type->param.name, assoc_name) == 0)
          return idx < concrete_type->generic.arg_count
                     ? concrete_type->generic.args[idx]
                     : NULL;
        idx++;
      }
    }
  }
table_lookup:
  if (!protocol_name || !ctx->assoc_type_table)
    return NULL;
  char type_name_buf[128];
  type_to_string(concrete_type, type_name_buf, sizeof(type_name_buf));
  const char *nominal =
      (concrete_type->kind == TY_NAMED && concrete_type->named.name)
          ? concrete_type->named.name
          : type_name_buf;
  const char *concrete_name = assoc_type_table_get(
      ctx->assoc_type_table, nominal, protocol_name, assoc_name);
  if (!concrete_name)
    return NULL;
  if (concrete_type->kind == TY_GENERIC_INST && concrete_type->generic.base &&
      concrete_type->generic.args && concrete_type->generic.arg_count > 0) {
    const ASTNode *decl = concrete_type->generic.base->named.decl;
    if (!decl && nominal) {
      const Symbol *s = sema_lookup(ctx, nominal);
      if (s && s->decl)
        decl = s->decl;
    }
    if (decl) {
      uint32_t k = 0;
      for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
        if (c->kind != AST_GENERIC_PARAM || !c->type ||
            c->type->kind != TY_GENERIC_PARAM)
          continue;
        if (c->type->param.name &&
            strcmp(c->type->param.name, concrete_name) == 0)
          return k < concrete_type->generic.arg_count
                     ? concrete_type->generic.args[k]
                     : NULL;
        k++;
      }
    }
  }
  if (strcmp(concrete_name, SW_TYPE_INT) == 0)
    return TY_BUILTIN_INT;
  if (strcmp(concrete_name, SW_TYPE_STRING) == 0)
    return TY_BUILTIN_STRING;
  if (strcmp(concrete_name, SW_TYPE_BOOL) == 0)
    return TY_BUILTIN_BOOL;
  if (strcmp(concrete_name, SW_TYPE_DOUBLE) == 0)
    return TY_BUILTIN_DOUBLE;
  if (strcmp(concrete_name, SW_TYPE_FLOAT) == 0)
    return TY_BUILTIN_FLOAT;
  Symbol *sym = sema_lookup(ctx, concrete_name);
  if (sym && sym->type)
    return sym->type;
  TypeInfo *ti = type_arena_alloc(ctx->type_arena);
  if (!ti) return NULL;
  ti->kind = TY_NAMED;
  ti->named.name =
      sema_intern(ctx, concrete_name, (size_t)strlen(concrete_name));
  ti->named.decl = sym && sym->decl ? sym->decl : NULL;
  return ti;
}

/**
 * @brief Populates the conformance table with Swift stdlib builtin conformances.
 *
 * Registers protocol conformances for Int, Double, Float, String, Bool,
 * Array, and Dictionary — covering Equatable, Hashable, Comparable,
 * Collection, Sendable, Codable, and other commonly used protocols.
 */
void conformance_table_init_builtins(ConformanceTable *ct) {
  if (!ct)
    return;
  ct->count = 0;

  /* ── Int ──────────────────────────────────────────────────────────────────── */
  /* In Swift, Int conforms to: Equatable, Hashable, Comparable,
   * Numeric, BinaryInteger, SignedInteger, CustomStringConvertible,
   * Codable... We register the ones relevant to the compiler. */
  static const char *int_protos[] = {SW_PROTO_EQUATABLE,
                                     SW_PROTO_HASHABLE,
                                     SW_PROTO_COMPARABLE,
                                     "Numeric",
                                     "BinaryInteger",
                                     "SignedInteger",
                                     "SignedNumeric",
                                     "FixedWidthInteger",
                                     SW_PROTO_CUSTOM_STR,
                                     SW_PROTO_SENDABLE,
                                     SW_PROTO_CODABLE,
                                     "Encodable",
                                     "Decodable",
                                     "Copyable", /* implicit Copyable */
                                     NULL};
  for (int i = 0; int_protos[i]; i++)
    conformance_table_add(ct, SW_TYPE_INT, int_protos[i]);

  /* ── Double ───────────────────────────────────────────────────────────────── */
  static const char *double_protos[] = {SW_PROTO_EQUATABLE,
                                        SW_PROTO_HASHABLE,
                                        SW_PROTO_COMPARABLE,
                                        "Numeric",
                                        "FloatingPoint",
                                        "BinaryFloatingPoint",
                                        SW_PROTO_CUSTOM_STR,
                                        SW_PROTO_SENDABLE,
                                        SW_PROTO_CODABLE,
                                        "Copyable",
                                        NULL};
  for (int i = 0; double_protos[i]; i++)
    conformance_table_add(ct, SW_TYPE_DOUBLE, double_protos[i]);

  /* ── Float ────────────────────────────────────────────────────────────────── */
  static const char *float_protos[] = {SW_PROTO_EQUATABLE,
                                       SW_PROTO_HASHABLE,
                                       SW_PROTO_COMPARABLE,
                                       "Numeric",
                                       "FloatingPoint",
                                       "BinaryFloatingPoint",
                                       SW_PROTO_CUSTOM_STR,
                                       SW_PROTO_SENDABLE,
                                       SW_PROTO_CODABLE,
                                       "Copyable",
                                       NULL};
  for (int i = 0; float_protos[i]; i++)
    conformance_table_add(ct, SW_TYPE_FLOAT, float_protos[i]);

  /* ── String ───────────────────────────────────────────────────────────────── */
  static const char *string_protos[] = {SW_PROTO_EQUATABLE,
                                        SW_PROTO_HASHABLE,
                                        SW_PROTO_COMPARABLE,
                                        SW_PROTO_CUSTOM_STR,
                                        "ExpressibleByStringLiteral",
                                        SW_PROTO_SENDABLE,
                                        SW_PROTO_CODABLE,
                                        "Collection",
                                        "Copyable",
                                        NULL};
  for (int i = 0; string_protos[i]; i++)
    conformance_table_add(ct, SW_TYPE_STRING, string_protos[i]);

  /* ── Bool ─────────────────────────────────────────────────────────────────── */
  static const char *bool_protos[] = {
      SW_PROTO_EQUATABLE, SW_PROTO_HASHABLE, SW_PROTO_CUSTOM_STR, SW_PROTO_SENDABLE, SW_PROTO_CODABLE,
      "Copyable",  NULL};
  for (int i = 0; bool_protos[i]; i++)
    conformance_table_add(ct, SW_TYPE_BOOL, bool_protos[i]);

  /* ── Array ────────────────────────────────────────────────────────────────── */
  /* Array<Element> — even though some are conditional on Element's
   * conformance, add unconditional conformances. */
  static const char *array_protos[] = {"Collection",
                                       "Sequence",
                                       "RandomAccessCollection",
                                       "MutableCollection",
                                       "RangeReplaceableCollection",
                                       "ExpressibleByArrayLiteral",
                                       SW_PROTO_SENDABLE,
                                       NULL};
  for (int i = 0; array_protos[i]; i++)
    conformance_table_add(ct, SW_TYPE_ARRAY, array_protos[i]);

  /* ── Dictionary ───────────────────────────────────────────────────────────── */
  static const char *dict_protos[] = {"Collection", "Sequence",
                                      "ExpressibleByDictionaryLiteral",
                                      SW_PROTO_SENDABLE, NULL};
  for (int i = 0; dict_protos[i]; i++)
    conformance_table_add(ct, SW_TYPE_DICTIONARY, dict_protos[i]);
}
