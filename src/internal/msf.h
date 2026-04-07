/**
 * @file msf.h
 * @brief Pipeline orchestration — ties lexer, parser, and sema together.
 *
 * NOT part of the public API.  This header is included by msf.c and
 * test files that need direct access to the raw parser/sema interface.
 *
 * WHAT THIS HEADER PROVIDES
 *
 *   Includes   — all four module APIs (ast, lexer, type, sema)
 *   Parser     — init/destroy, parse, error accessors
 *
 * PIPELINE
 *
 *   Source → lexer_tokenize() → parser_init() → parse_source_file()
 *         → sema_init() → sema_analyze() → done
 *
 *   msf_analyze() does all of this in one call.  Use this header only
 *   when you need step-by-step control (testing, debugging, tooling).
 *
 * OWNERSHIP
 *
 *   Parser and SemaContext are heap-allocated.  Free with their
 *   respective _destroy() functions.  Both reference (but do not own)
 *   the arenas and source — the caller manages those lifetimes.
 */
#ifndef MSF_INTERNAL_H
#define MSF_INTERNAL_H

#include "ast.h"
#include "type.h"
#include "lexer.h"
#include "sema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  PARSER                                                                │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Parser *p = parser_init(&src, &ts, &arena);
 *  ASTNode *root = parse_source_file(p);
 *  parser_destroy(p);
 */

/** @brief Opaque parser state.  Init with parser_init(). */
typedef struct Parser Parser;

/** @brief Initializes a heap-allocated parser.  Caller frees with parser_destroy(). */
Parser *parser_init(const Source *src, const TokenStream *ts,
                      ASTArena *arena);

/** @brief Parses the full source file into an AST_SOURCE_FILE node. */
ASTNode *parse_source_file(Parser *p);

/** @brief Destroys the parser and frees associated resources. */
void parser_destroy(Parser *p);

/* — Parser error accessors ------------------------------------------------ */

/** @brief Returns the number of recorded parse errors. */
uint32_t    parser_error_count(const Parser *p);

/** @brief Returns the error message at @p index, or "" if out of range. */
const char *parser_error_message(const Parser *p, uint32_t index);

/** @brief Returns the 1-based line number of the error at @p index. */
uint32_t    parser_error_line(const Parser *p, uint32_t index);

/** @brief Returns the 1-based column number of the error at @p index. */
uint32_t    parser_error_col(const Parser *p, uint32_t index);

/* Sema API is provided by sema.h (included above). */

#ifdef __cplusplus
}
#endif
#endif /* MSF_INTERNAL_H */
