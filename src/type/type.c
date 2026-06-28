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
 * Built-in Type Singletons
 *
 * Each TY_BUILTIN_* points to an immortal TypeInfo with static storage
 * duration — no arena dependency, no per-MSFResult initialization, safe
 * across concurrent analyses and across MSFResult lifetimes.
 *
 * Sema code compares against these via pointer equality — no strcmp needed
 * for built-in type checks.  Treat the underlying objects as read-only;
 * mutating a builtin would corrupt every other analysis.
 *
 * type_builtins_init() is kept as a no-op for backward compatibility.
 * ═══════════════════════════════════════════════════════════════════════════════ */

#define MSF_BUILTIN_DEF(NAME, KIND) \
  static TypeInfo _msf_builtin_##NAME = { .kind = (KIND) }; \
  MSF_API TypeInfo *TY_BUILTIN_##NAME = &_msf_builtin_##NAME

MSF_BUILTIN_DEF(VOID,        TY_VOID);
MSF_BUILTIN_DEF(BOOL,        TY_BOOL);
MSF_BUILTIN_DEF(INT,         TY_INT);
MSF_BUILTIN_DEF(STRING,      TY_STRING);
MSF_BUILTIN_DEF(DOUBLE,      TY_DOUBLE);
MSF_BUILTIN_DEF(FLOAT,       TY_FLOAT);
MSF_BUILTIN_DEF(JSONENCODER, TY_JSONENCODER);
MSF_BUILTIN_DEF(JSONDECODER, TY_JSONDECODER);
MSF_BUILTIN_DEF(DATA,        TY_DATA);
MSF_BUILTIN_DEF(SUBSTRING,   TY_SUBSTRING);
MSF_BUILTIN_DEF(UINT64,      TY_UINT64);
MSF_BUILTIN_DEF(UINT,        TY_UINT);
MSF_BUILTIN_DEF(UINT32,      TY_UINT32);
MSF_BUILTIN_DEF(UINT16,      TY_UINT16);
MSF_BUILTIN_DEF(UINT8,       TY_UINT8);

#undef MSF_BUILTIN_DEF

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
 * @brief No-op.  Built-in type singletons are now immortal static objects
 *        and no longer require per-arena initialization.  Retained as an
 *        export so existing callers keep linking.
 *
 * @param a  Arena (ignored).
 */
void type_builtins_init(TypeArena *a) {
  (void)a;
}
