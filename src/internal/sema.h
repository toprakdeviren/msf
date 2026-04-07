/**
 * @file sema.h
 * @brief Semantic analysis module API — context lifecycle and error accessors.
 *
 * NOT part of the public API.  This header is the internal contract
 * between the sema module and its consumers (msf.c, tests).
 *
 * WHAT THIS MODULE PROVIDES
 *
 *   Lifecycle   — sema_init, sema_analyze, sema_destroy
 *   Errors      — sema_error_count, sema_error_message, line, col
 *
 * USAGE
 *
 *   SemaContext *s = sema_init(&src, tokens, &ast_arena, &type_arena);
 *   sema_analyze(s, root);
 *   // ... AST nodes now have ->type populated ...
 *   for (uint32_t i = 0; i < sema_error_count(s); i++)
 *       printf("%u:%u: %s\n",
 *              sema_error_line(s, i), sema_error_col(s, i),
 *              sema_error_message(s, i));
 *   sema_destroy(s);
 *
 * OWNERSHIP
 *
 *   SemaContext is heap-allocated.  Free with sema_destroy().
 *   References (but does not own) the arenas, source, and token stream —
 *   the caller manages those lifetimes.
 *
 *   The intern pool (interned strings) survives sema_destroy() because
 *   AST nodes and TypeInfo values reference interned pointers.  The pool
 *   is freed when the caller frees ctx->intern (or lets it leak if the
 *   process is exiting).
 */
#ifndef MSF_SEMA_INTERNAL_H
#define MSF_SEMA_INTERNAL_H

#include "ast.h"
#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque semantic analysis context.  Init with sema_init(). */
typedef struct SemaContext SemaContext;

/** @brief Initializes a heap-allocated sema context.  Caller frees with sema_destroy(). */
SemaContext *sema_init(const Source *src, const Token *tokens,
                         ASTArena *ast_arena, TypeArena *type_arena);

/** @brief Runs semantic analysis: forward declarations + type resolution. */
int sema_analyze(SemaContext *ctx, ASTNode *root);

/** @brief Destroys the sema context (including intern pool). */
void sema_destroy(SemaContext *ctx);

/* — Error accessors -------------------------------------------------------- */

/** @brief Returns the number of recorded semantic errors. */
uint32_t    sema_error_count(const SemaContext *ctx);

/** @brief Returns the error message at @p index, or "" if out of range. */
const char *sema_error_message(const SemaContext *ctx, uint32_t index);

/** @brief Returns the 1-based line number of the error at @p index. */
uint32_t    sema_error_line(const SemaContext *ctx, uint32_t index);

/** @brief Returns the 1-based column number of the error at @p index. */
uint32_t    sema_error_col(const SemaContext *ctx, uint32_t index);

#ifdef __cplusplus
}
#endif
#endif /* MSF_SEMA_INTERNAL_H */
