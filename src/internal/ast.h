/**
 * @file ast.h
 * @brief AST module API — arena, tree ops, modifiers, serialization.
 *
 * NOT part of the public API.  This header is the internal contract
 * between the AST module and its consumers (parser, sema, msf.c).
 *
 * WHAT THIS MODULE PROVIDES
 *
 *   Arena     — chunk-based allocator for AST nodes
 *   Tree ops  — ast_add_child (O(1) via last_child tracking)
 *   Names     — ast_kind_name (generated lookup table)
 *   Dump      — text / JSON / S-expr serialization
 *   Modifiers — MOD_* bitmask flags for ASTNode.modifiers
 *
 * USAGE
 *
 *   ASTArena arena;
 *   ast_arena_init(&arena, 0);
 *
 *   ASTNode *n = ast_arena_alloc(&arena);   // zeroed node
 *   n->kind = AST_FUNC_DECL;
 *   ast_add_child(parent, n);               // O(1)
 *
 *   ast_arena_free(&arena);                 // frees all nodes at once
 *
 * OWNERSHIP
 *
 *   Nodes are owned by the arena.  Do not free individual nodes.
 *   Call ast_arena_free() to release everything at once.
 */
#ifndef MSF_AST_INTERNAL_H
#define MSF_AST_INTERNAL_H

#include <msf.h>  /* public types: ASTNode, ASTNodeKind, Source, Token */

#ifdef __cplusplus
extern "C" {
#endif

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  1. MODIFIER FLAGS — moved to public msf.h (SECTION 9)                 │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  MOD_* bit defines now live in <msf.h> so backend consumers can read
 *  ASTNode.modifiers without including an internal header.  Use them
 *  exactly as before:
 *
 *    if (node->modifiers & MOD_STATIC) { ... }
 */

#define OWNERSHIP_BORROWING  1u
#define OWNERSHIP_CONSUMING  2u

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  2. ACCESSOR & AVAILABILITY KINDS                                      │
 * └──────────────────────────────────────────────────────────────────────────┘ */

#define ACCESSOR_GET         1u
#define ACCESSOR_SET         2u
#define ACCESSOR_WILL_SET    3u
#define ACCESSOR_DID_SET     4u
#define ACCESSOR_READ        5u
#define ACCESSOR_MODIFY      6u

#define AVAILABILITY_AVAILABLE   1u
#define AVAILABILITY_UNAVAILABLE 2u

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  3. ARENA ALLOCATOR                                                    │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Chunk-based arena.  Each chunk holds AST_ARENA_CHUNK_SIZE nodes.
 *  Nodes are zeroed on allocation (calloc'd chunks).
 *  All chunks are freed at once via ast_arena_free().
 */

#define AST_ARENA_CHUNK_SIZE 1024

typedef struct ASTArenaChunk ASTArenaChunk;
struct ASTArenaChunk {
  ASTNode       nodes[AST_ARENA_CHUNK_SIZE];
  ASTArenaChunk *next;
};

typedef struct ASTArena {
  ASTArenaChunk *head;
  ASTArenaChunk *tail;
  size_t         count;   /**< Used slots in the tail chunk. */
} ASTArena;

/** @brief Initializes the arena with one empty chunk. */
void ast_arena_init(ASTArena *a, size_t capacity);

/** @brief Frees heap payloads inside nodes (closure captures, etc.). */
void ast_arena_cleanup_payloads(ASTArena *a);

/** @brief Frees all chunks (calls cleanup_payloads automatically). */
void ast_arena_free(ASTArena *a);

/** @brief Allocates a zeroed ASTNode.  Returns NULL on OOM. */
ASTNode *ast_arena_alloc(ASTArena *a);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  4. TREE OPERATIONS                                                    │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief Appends a child to a parent.  O(1) via last_child tracking. */
void ast_add_child(ASTNode *parent, ASTNode *child);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  5. SERIALIZATION                                                      │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  ast_print()      → indented text   (debug, terminal)
 *  ast_dump_json()  → JSON            (editors, web UI)
 *  ast_dump_sexpr() → S-expression    (testing, diffing)
 */

void ast_print(const ASTNode *root, const Source *src,
               const Token *tokens, int depth, FILE *out);
void ast_dump_json(const ASTNode *root, const Source *src,
                   const Token *tokens, FILE *out);
void ast_dump_sexpr(const ASTNode *root, const Source *src,
                    const Token *tokens, FILE *out);

#ifdef __cplusplus
}
#endif
#endif /* MSF_AST_INTERNAL_H */
