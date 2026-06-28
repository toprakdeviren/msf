/**
 * @file ast.c
 * @brief AST core: arena allocator, tree operations, node kind names.
 *
 * This file is the AST's infrastructure layer — it knows how to allocate
 * nodes, link them into a tree, and look up kind names.  It does NOT know
 * how to print, serialize, or inspect node payloads.  Serialization lives
 * in ast_dump.c.
 */

#include "internal/ast.h"
#include "ast_names.h"   /* generated AST_KIND_NAMES[] — from data/ast_nodes.def */
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════════
 * Arena Allocator
 *
 * Chunk-based arena for AST nodes.  Each chunk holds AST_ARENA_CHUNK_SIZE
 * nodes (defined in ast.h).  When full, a new chunk is appended to the
 * linked list.  All chunks are freed at once via ast_arena_free().
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initializes the arena with one empty chunk.
 *
 * Allocates the first chunk via calloc (all nodes zero-initialized).
 * On allocation failure the arena is left zeroed — ast_arena_alloc()
 * will return NULL and ast_arena_free() is a safe no-op.
 *
 * @param a    Arena to initialize.
 * @param cap  Ignored (reserved for future use). Chunk size is fixed
 *             at AST_ARENA_CHUNK_SIZE.
 */
void ast_arena_init(ASTArena *a, size_t cap) {
  (void)cap;
  a->head = a->tail = calloc(1, sizeof(ASTArenaChunk));
  if (!a->head) {
    a->tail = NULL;
    a->count = 0;
    return;
  }
  a->count = 0;
}

/**
 * @brief Frees heap-allocated payloads inside AST nodes.
 *
 * Walks every node in the arena and frees any heap memory that the node
 * owns.  Currently only AST_CLOSURE_EXPR nodes own heap memory
 * (data.closure.captures).
 *
 * Separated from ast_arena_free() so the arena stays a pure allocator —
 * it doesn't need to know about node semantics.  Called automatically
 * by ast_arena_free(), but can also be called independently to release
 * payload memory while keeping the arena alive.
 *
 * @param a  Arena whose node payloads will be freed.
 */
void ast_arena_cleanup_payloads(ASTArena *a) {
  ASTArenaChunk *curr = a->head;
  while (curr) {
    size_t limit = (curr->next == NULL) ? a->count : AST_ARENA_CHUNK_SIZE;
    for (size_t i = 0; i < limit; i++) {
      ASTNode *n = &curr->nodes[i];
      if (n->kind == AST_CLOSURE_EXPR && n->data.closure.captures) {
        free(n->data.closure.captures);
        n->data.closure.captures = NULL;
      }
    }
    curr = curr->next;
  }
}

/**
 * @brief Frees all chunks in the arena and releases node payloads.
 *
 * Calls ast_arena_cleanup_payloads() first, then frees every chunk
 * in the linked list.  After this call the arena is zeroed out —
 * safe to call on an already-freed or zero-initialized arena.
 *
 * @param a  Arena to destroy.
 */
void ast_arena_free(ASTArena *a) {
  ast_arena_cleanup_payloads(a);
  ASTArenaChunk *curr = a->head;
  while (curr) {
    ASTArenaChunk *next = curr->next;
    free(curr);
    curr = next;
  }
  a->head = a->tail = NULL;
  a->count = 0;
}

/**
 * @brief Allocates a zeroed ASTNode from the arena.
 *
 * If the current chunk is full (count >= AST_ARENA_CHUNK_SIZE), a new
 * chunk is allocated and appended to the linked list.  Returns NULL
 * on OOM without modifying the arena.
 *
 * The returned node is zero-initialized (calloc'd chunk), so all fields
 * — kind, pointers, union — start at zero/NULL.
 *
 * @param a  Arena to allocate from.
 * @return   Pointer to a fresh ASTNode, or NULL on OOM.
 */
ASTNode *ast_arena_alloc(ASTArena *a) {
  if (!a->tail) return NULL;
  if (a->count >= AST_ARENA_CHUNK_SIZE) {
    ASTArenaChunk *chunk = calloc(1, sizeof(ASTArenaChunk));
    if (!chunk)
      return NULL;
    a->tail->next = chunk;
    a->tail = chunk;
    a->count = 0;
  }
  return &a->tail->nodes[a->count++];
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Tree Operations
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Appends a child node to a parent's child list.
 *
 * O(1) insertion via last_child tracking — no list traversal needed.
 * Clears child->next_sibling to prevent stale pointer chains from
 * a previous parent.  Both arguments may be NULL (no-op).
 *
 * @param parent  Node to append to.
 * @param child   Node to append (becomes parent's last child).
 */
void ast_add_child(ASTNode *parent, ASTNode *child) {
  if (!parent || !child)
    return;
  child->parent = parent;
  child->next_sibling = NULL;

  if (!parent->first_child) {
    parent->first_child = child;
    parent->last_child = child;
  } else {
    parent->last_child->next_sibling = child;
    parent->last_child = child;
  }
}

/**
 * @brief Returns the human-readable name for an AST node kind.
 *
 * Uses the generated AST_KIND_NAMES[] table (from ast_names.h).
 * Returns "?" for out-of-range values.
 *
 * @param k  Node kind enum value.
 * @return   Static string — never free it.
 */
const char *ast_kind_name(ASTNodeKind k) {
  if (k < 0 || k >= AST__COUNT)
    return "?";
  return AST_KIND_NAMES[k];
}
