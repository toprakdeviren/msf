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

/** @brief One parsed source file of a module, for sema_analyze_module(). */
typedef struct {
  const Source *src;          /**< File source (for diagnostics + token text). */
  const Token  *tokens;       /**< File token stream.                          */
  uint32_t      token_count;  /**< Number of tokens (for bounds-safe interning).*/
  ASTNode      *root;         /**< File AST root (parsed into shared AST arena).*/
} SemaModuleFile;

/**
 * @brief Whole-module semantic analysis over several files sharing one context.
 *
 * Runs the passes module-wide: declare ALL files' top-level decls into the
 * shared symbol table first, then resolve ALL files, then check conformances.
 * Because the symbol table / type arena / intern pool are shared, a reference
 * in one file to a type, member, or nested type declared in a sibling file
 * resolves naturally — no SDK stubs, no text concatenation. Each file keeps its
 * own token stream and source, so diagnostics and token text stay correct.
 */
int sema_analyze_module(SemaContext *ctx, const SemaModuleFile *files, size_t nfiles);

/**
 * @brief Predeclares an external type name into the global scope (TY_NAMED).
 *
 * Call between sema_init() and sema_analyze() to make module-level names from
 * sibling files / imported modules visible, so references resolve instead of
 * reporting "use of undeclared type". Idempotent; ignores already-defined names.
 */
void sema_predeclare_module_type(SemaContext *ctx, const char *name);

/**
 * @brief Attaches a runtime module vocabulary (a parsed .msfvocab).
 *
 * Call between sema_init() and sema_analyze().  When set, an `import X` in the
 * source resolves X's public type names from @p vocab before falling back to
 * the host's compiled module table.  @p vocab is borrowed, not owned.
 */
void sema_set_vocabulary(SemaContext *ctx, const struct MSFVocab *vocab);

/** @brief Sets the global-fallback SDK vocabulary (see SemaContext.sdk_vocab). */
void sema_set_sdk_vocabulary(SemaContext *ctx, const struct MSFVocab *vocab);

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

/** @brief Returns the owning file path of the error at @p index (or NULL). */
const char *sema_error_file(const SemaContext *ctx, uint32_t index);

/** @brief Returns the conformance table (NULL if sema not initialized). */
const ConformanceTable *sema_conformance_table(const SemaContext *ctx);

/** @brief Source byte offset where the error at @p index starts (inclusive). */
uint32_t sema_error_start(const SemaContext *ctx, uint32_t index);

/** @brief Source byte offset where the error at @p index ends (exclusive). */
uint32_t sema_error_end(const SemaContext *ctx, uint32_t index);

#ifdef __cplusplus
}
#endif
#endif /* MSF_SEMA_INTERNAL_H */
