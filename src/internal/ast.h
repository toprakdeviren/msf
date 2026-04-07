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
 * │  1. MODIFIER FLAGS — bitmask stored in ASTNode.modifiers               │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  uint32_t mods = MOD_PUBLIC | MOD_STATIC;
 *  node->modifiers = mods;
 *  if (mods & MOD_STATIC) { ... }
 */

/* — Access control -------------------------------------------------------- */
#define MOD_PUBLIC                        (1u << 0)
#define MOD_PRIVATE                       (1u << 1)
#define MOD_INTERNAL                      (1u << 2)
#define MOD_FILEPRIVATE                   (1u << 3)
#define MOD_OPEN                          (1u << 4)
#define MOD_PACKAGE                       (1u << 30)

/* — Declaration modifiers ------------------------------------------------- */
#define MOD_STATIC                        (1u << 5)
#define MOD_FINAL                         (1u << 6)
#define MOD_OVERRIDE                      (1u << 7)
#define MOD_MUTATING                      (1u << 8)
#define MOD_NONMUTATING                   (1u << 9)
#define MOD_LAZY                          (1u << 10)
#define MOD_WEAK                          (1u << 11)
#define MOD_UNOWNED                       (1u << 12)
#define MOD_INDIRECT                      (1u << 16)
#define MOD_REQUIRED                      (1u << 17)
#define MOD_CONVENIENCE                   (1u << 18)
#define MOD_DYNAMIC                       (1u << 19)

/* — Concurrency ----------------------------------------------------------- */
#define MOD_ASYNC                         (1u << 13)
#define MOD_THROWS                        (1u << 14)
#define MOD_RETHROWS                      (1u << 15)
#define MOD_NONISOLATED                   (1u << 20)
#define MOD_ISOLATED                      (1u << 21)
#define MOD_MAIN_ACTOR                    (1u << 25)

/* — Closure / parameter attributes --------------------------------------- */
#define MOD_ESCAPING                      (1u << 26)
#define MOD_AUTOCLOSURE                   (1u << 27)
#define MOD_VARIADIC                      (1u << 28)

/* — Initializer modifiers ------------------------------------------------- */
#define MOD_FAILABLE                      (1u << 29)
#define MOD_IMPLICITLY_UNWRAPPED_FAILABLE (1u << 30)
#define MOD_SUPPRESSED_CONFORMANCE        (1u << 31)

/* — Import ---------------------------------------------------------------- */
#define MOD_TESTABLE_IMPORT               (1u << 22)

/* — Protocol requirements (context-dependent, share bits with capture) ---- */
#define MOD_PROTOCOL_PROP_SET             (1u << 24)
#define MOD_PROTOCOL_ASSOC_TYPE           (1u << 22)

/* — Closure capture qualifiers (on AST_CLOSURE_CAPTURE nodes) ------------- */
#define MOD_CAPTURE_STRONG                (0u)
#define MOD_CAPTURE_WEAK                  (1u << 22)
#define MOD_CAPTURE_UNOWNED               (1u << 23)
#define MOD_CAPTURE_SAFE                  (1u << 24)

/* — Swift 5.9 parameter ownership (on AST_PARAM; shares capture bits) ----- */
#define MOD_BORROWING                     (1u << 22)
#define MOD_CONSUMING                     (1u << 23)

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
