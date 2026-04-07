/**
 * @file type.h
 * @brief Type module API — arena, constraints, substitution, predicates.
 *
 * NOT part of the public API.  This header is the internal contract
 * between the type module and its consumers (sema, type.c, tests).
 *
 * WHAT THIS MODULE PROVIDES
 *
 *   Arena         — chunk-based allocator for TypeInfo values
 *   Builtins      — TY_BUILTIN_* singleton initialization
 *   Constraints   — TypeConstraint for generic where-clauses
 *   Substitution  — replace generic params with concrete types
 *   Predicates    — type_is_named(), type_is_any(), type_is_never(), etc.
 *
 * USAGE
 *
 *   TypeArena arena;
 *   type_arena_init(&arena, 0);
 *   type_builtins_init(&arena);    // sets TY_BUILTIN_INT, etc.
 *
 *   TypeInfo *ti = type_arena_alloc(&arena);
 *   ti->kind = TY_ARRAY;
 *   ti->inner = TY_BUILTIN_INT;   // [Int]
 *
 *   type_arena_free(&arena);       // frees all types at once
 *
 * OWNERSHIP
 *
 *   TypeInfo values are owned by the arena.  Do not free individually.
 *   Some TypeInfo fields (.func.params, .tuple.elems, .generic.args)
 *   are heap-allocated arrays — type_arena_free() handles them.
 */
#ifndef MSF_TYPE_INTERNAL_H
#define MSF_TYPE_INTERNAL_H

#include <msf.h>           /* public types: TypeInfo, TypeKind, builtins */
#include "builtin_names.h" /* SW_TYPE_*, SW_PROTO_* constants */
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  1. TYPE CONSTRAINTS — generic where-clause requirements               │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Stored in TypeInfo.param.constraints[] for TY_GENERIC_PARAM types.
 *
 *  where T: Equatable          → TC_CONFORMANCE
 *  where T == Int              → TC_SAME_TYPE
 *  where T: SomeClass          → TC_SUPERCLASS
 *  where T: ~Copyable          → TC_SUPPRESSED
 *  where T.Item == U.Item      → TC_SAME_TYPE_ASSOC
 */

typedef enum {
  TC_CONFORMANCE,       /**< T: Protocol          */
  TC_SAME_TYPE,         /**< T == ConcreteType     */
  TC_SUPERCLASS,        /**< T: SomeClass          */
  TC_SUPPRESSED,        /**< ~Copyable (opt-out)   */
  TC_SAME_TYPE_ASSOC,   /**< T.Assoc == U.Assoc    */
} TypeConstraintKind;

struct TypeConstraint {
  TypeConstraintKind kind;
  const char        *protocol_name;   /**< For TC_CONFORMANCE.           */
  TypeInfo          *rhs_type;        /**< For TC_SAME_TYPE.             */
  const char        *assoc_name;      /**< For TC_SAME_TYPE_ASSOC (LHS). */
  const char        *rhs_param_name;  /**< For TC_SAME_TYPE_ASSOC (RHS). */
  const char        *rhs_assoc_name;  /**< For TC_SAME_TYPE_ASSOC (RHS). */
};

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  2. TYPE ARENA                                                         │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Same pattern as ASTArena: chunk-based, calloc'd, bulk-free.
 */

#define TYPE_ARENA_CHUNK_SIZE 1024

typedef struct TypeArenaChunk TypeArenaChunk;
struct TypeArenaChunk {
  TypeInfo       types[TYPE_ARENA_CHUNK_SIZE];
  TypeArenaChunk *next;
};

typedef struct TypeArena {
  TypeArenaChunk *head;
  TypeArenaChunk *tail;
  size_t          count;   /**< Used slots in the tail chunk. */
} TypeArena;

/** @brief Initializes the arena with one empty chunk. */
void type_arena_init(TypeArena *a, size_t capacity);

/** @brief Frees all chunks and heap-allocated sub-arrays inside types. */
void type_arena_free(TypeArena *a);

/** @brief Allocates a zeroed TypeInfo.  Returns NULL on OOM. */
TypeInfo *type_arena_alloc(TypeArena *a);

/** @brief Initializes all TY_BUILTIN_* singletons from the arena. */
void type_builtins_init(TypeArena *a);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  3. GENERIC TYPE SUBSTITUTION                                          │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  TypeSubstitution sub = {0};
 *  type_sub_set(&sub, "T", TY_BUILTIN_INT);         // T → Int
 *  TypeInfo *concrete = type_substitute(generic_type, &sub, &arena);
 */

#define TYPE_SUB_MAX 16

typedef struct {
  const char *param_name;  /**< Generic parameter name (e.g. "T"). */
  TypeInfo   *concrete;    /**< Concrete type to substitute.       */
} TypeSubEntry;

typedef struct {
  TypeSubEntry entries[TYPE_SUB_MAX];
  uint32_t     count;
} TypeSubstitution;

/** @brief Adds or overwrites a (param → concrete) mapping. */
void type_sub_set(TypeSubstitution *sub, const char *param,
                  TypeInfo *concrete);

/** @brief Looks up a concrete type for a param name.  Returns NULL if not found. */
TypeInfo *type_sub_lookup(const TypeSubstitution *sub, const char *param_name);

/**
 * @brief Recursively replaces generic params with concrete types.
 *
 * Non-destructive: returns the original pointer if nothing changes.
 * Allocates new TypeInfo from @p arena only when substitution applies.
 */
TypeInfo *type_substitute(const TypeInfo *ty, const TypeSubstitution *sub,
                          TypeArena *arena);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  4. TYPE PREDICATES                                                    │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  if (type_is_never(node->type)) { ... }
 *  if (type_is_named(ty, "MyStruct")) { ... }
 */

/** @brief Returns 1 if ty is a TY_NAMED type with the given name. */
static inline int type_is_named(const TypeInfo *ty, const char *name) {
  return ty && ty->kind == TY_NAMED && ty->named.name &&
         strcmp(ty->named.name, name) == 0;
}

/** @brief True if ty is `Any`. */
static inline int type_is_any(const TypeInfo *ty) {
  return type_is_named(ty, SW_TYPE_ANY);
}

/** @brief True if ty is `AnyObject`. */
static inline int type_is_anyobject(const TypeInfo *ty) {
  return type_is_named(ty, SW_TYPE_ANY_OBJECT);
}

/** @brief True if ty is `Never`. */
static inline int type_is_never(const TypeInfo *ty) {
  return type_is_named(ty, SW_TYPE_NEVER);
}

/** @brief Returns the primary protocol name from a type or composition. */
static inline const char *type_primary_protocol_name(const TypeInfo *ty) {
  if (!ty) return NULL;
  if (ty->kind == TY_NAMED && ty->named.name) return ty->named.name;
  if (ty->kind == TY_PROTOCOL_COMPOSITION && ty->composition.protocol_count > 0 &&
      ty->composition.protocols[0] && ty->composition.protocols[0]->kind == TY_NAMED)
    return ty->composition.protocols[0]->named.name;
  return NULL;
}

#ifdef __cplusplus
}
#endif
#endif /* MSF_TYPE_INTERNAL_H */
