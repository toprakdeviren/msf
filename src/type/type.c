/**
 * @file type.c
 * @brief Type arena allocator and built-in type singletons.
 *
 * This file manages the lifetime of TypeInfo values.  All TypeInfo
 * structs are allocated from a chunk-based arena (same pattern as
 * ASTArena in ast.c).  Built-in types (Int, String, Bool, ...) are
 * pre-allocated singletons initialized once via type_builtins_init().
 *
 * Other type files:
 *   type_str.c    — type_to_string (human-readable representation)
 *   type_equal.c  — type_cmp, type_equal, type_equal_deep
 *   type_sub.c    — generic type substitution
 */

#include "internal/type.h"
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════════
 * Global Built-in Type Singletons
 *
 * Each pointer is set once by type_builtins_init() and lives until
 * type_arena_free().  Sema code compares against these via pointer
 * equality — no strcmp needed for built-in type checks.
 * ═══════════════════════════════════════════════════════════════════════════════ */

TypeInfo *TY_BUILTIN_VOID = NULL;
TypeInfo *TY_BUILTIN_BOOL = NULL;
TypeInfo *TY_BUILTIN_INT = NULL;
TypeInfo *TY_BUILTIN_STRING = NULL;
TypeInfo *TY_BUILTIN_DOUBLE = NULL;
TypeInfo *TY_BUILTIN_FLOAT = NULL;
TypeInfo *TY_BUILTIN_JSONENCODER = NULL;
TypeInfo *TY_BUILTIN_JSONDECODER = NULL;
TypeInfo *TY_BUILTIN_DATA = NULL;
TypeInfo *TY_BUILTIN_SUBSTRING = NULL;
TypeInfo *TY_BUILTIN_UINT64 = NULL;
TypeInfo *TY_BUILTIN_UINT = NULL;
TypeInfo *TY_BUILTIN_UINT32 = NULL;
TypeInfo *TY_BUILTIN_UINT16 = NULL;
TypeInfo *TY_BUILTIN_UINT8 = NULL;

/* ═══════════════════════════════════════════════════════════════════════════════
 * Type Arena
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initializes the type arena with one empty chunk.
 *
 * All types within a chunk are zero-initialized (calloc).  On allocation
 * failure the arena is left zeroed — type_arena_alloc() will return NULL
 * and type_arena_free() is a safe no-op.
 *
 * @param a    Arena to initialize.
 * @param cap  Ignored (reserved). Chunk size is fixed at TYPE_ARENA_CHUNK_SIZE.
 */
void type_arena_init(TypeArena *a, size_t cap) {
  (void)cap;
  a->head = a->tail = calloc(1, sizeof(TypeArenaChunk));
  if (!a->head) {
    a->tail = NULL;
    a->count = 0;
    return;
  }
  a->count = 0;
}

/**
 * @brief Frees all chunks and any heap-allocated sub-arrays inside TypeInfo values.
 *
 * Walks every allocated TypeInfo and frees dynamically allocated payloads:
 *   - TY_FUNC:    params array
 *   - TY_TUPLE:   elems + labels arrays
 *   - TY_GENERIC_PARAM: constraints array
 *   - TY_GENERIC_INST:  args array
 *   - TY_PROTOCOL_COMPOSITION: protocols array
 *
 * After this call the arena is zeroed — safe to call on an already-freed arena.
 *
 * @param a  Arena to destroy.
 */
void type_arena_free(TypeArena *a) {
  TypeArenaChunk *curr = a->head;
  while (curr) {
    size_t limit = (curr == a->tail) ? a->count : TYPE_ARENA_CHUNK_SIZE;
    for (size_t i = 0; i < limit; i++) {
      TypeInfo *t = &curr->types[i];
      if (t->kind == TY_FUNC && t->func.params)
        free(t->func.params);
      else if (t->kind == TY_TUPLE) {
        if (t->tuple.elems) free(t->tuple.elems);
        if (t->tuple.labels) free(t->tuple.labels);
      } else if (t->kind == TY_GENERIC_PARAM && t->param.constraints)
        free(t->param.constraints);
      else if (t->kind == TY_GENERIC_INST && t->generic.args)
        free(t->generic.args);
      else if (t->kind == TY_PROTOCOL_COMPOSITION && t->composition.protocols)
        free(t->composition.protocols);
    }
    TypeArenaChunk *next = curr->next;
    free(curr);
    curr = next;
  }
  a->head = a->tail = NULL;
  a->count = 0;
}

/**
 * @brief Allocates a zeroed TypeInfo from the arena.
 *
 * If the current chunk is full, a new chunk is appended to the linked list.
 * Returns NULL on OOM without modifying the arena.
 *
 * @param a  Arena to allocate from.
 * @return   Pointer to a fresh TypeInfo, or NULL on OOM.
 */
TypeInfo *type_arena_alloc(TypeArena *a) {
  if (!a->tail) return NULL; /* arena init failed */
  if (a->count >= TYPE_ARENA_CHUNK_SIZE) {
    TypeArenaChunk *new_chunk = calloc(1, sizeof(TypeArenaChunk));
    if (!new_chunk)
      return NULL;
    a->tail->next = new_chunk;
    a->tail = new_chunk;
    a->count = 0;
  }
  return &a->tail->types[a->count++];
}

/**
 * @brief Initializes all built-in type singletons from the given arena.
 *
 * Allocates one TypeInfo per built-in type and assigns it to the
 * corresponding TY_BUILTIN_* global pointer.  Most types come from
 * the generated type_builtins.h; unsigned integers are added manually.
 *
 * Must be called once before any sema pass.  The singletons remain
 * valid until type_arena_free(a) is called.
 *
 * @param a  Arena to allocate singletons from.
 */
void type_builtins_init(TypeArena *a) {
  TypeInfo *t;
  /* @generated (scripts/codegen.py) */
  t = type_arena_alloc(a); t->kind = TY_VOID;        TY_BUILTIN_VOID        = t;
  t = type_arena_alloc(a); t->kind = TY_BOOL;        TY_BUILTIN_BOOL        = t;
  t = type_arena_alloc(a); t->kind = TY_INT;         TY_BUILTIN_INT         = t;
  t = type_arena_alloc(a); t->kind = TY_FLOAT;       TY_BUILTIN_FLOAT       = t;
  t = type_arena_alloc(a); t->kind = TY_DOUBLE;      TY_BUILTIN_DOUBLE      = t;
  t = type_arena_alloc(a); t->kind = TY_STRING;      TY_BUILTIN_STRING      = t;
  t = type_arena_alloc(a); t->kind = TY_JSONENCODER; TY_BUILTIN_JSONENCODER = t;
  t = type_arena_alloc(a); t->kind = TY_JSONDECODER; TY_BUILTIN_JSONDECODER = t;
  t = type_arena_alloc(a); t->kind = TY_DATA;        TY_BUILTIN_DATA        = t;
  t = type_arena_alloc(a); t->kind = TY_SUBSTRING;   TY_BUILTIN_SUBSTRING   = t;
  /* Unsigned integer singletons (not in types.yaml yet) */
  t = type_arena_alloc(a); t->kind = TY_UINT64; TY_BUILTIN_UINT64 = t;
  t = type_arena_alloc(a); t->kind = TY_UINT;   TY_BUILTIN_UINT   = t;
  t = type_arena_alloc(a); t->kind = TY_UINT32; TY_BUILTIN_UINT32 = t;
  t = type_arena_alloc(a); t->kind = TY_UINT16; TY_BUILTIN_UINT16 = t;
  t = type_arena_alloc(a); t->kind = TY_UINT8;  TY_BUILTIN_UINT8  = t;
}
