/**
 * @file limits.h
 * @brief Compile-time limits and tuning constants.
 *
 * This is an INTERNAL header — not part of the public API.
 *
 * Constants that appear in public struct definitions (LEXER_DIAG_MAX,
 * TYPE_SUB_MAX) are defined in their respective public headers.
 * This file contains limits that are only used internally.
 */
#ifndef MSF_LIMITS_H
#define MSF_LIMITS_H

/* ── Lexer ────────────────────────────────────────────────────────────────── */

/** @brief Max bytes for token_text() thread-local buffer. */
#define LEXER_TOKEN_TEXT_MAX  512

/* ── Parser ───────────────────────────────────────────────────────────────── */

/** @brief Max number of parse errors before silently dropping. */
#define MAX_PARSE_ERRORS      32

/** @brief Max user-defined precedence groups per source file. */
#define MAX_PRECEDENCE_GROUPS 32

/** @brief Max user-defined custom operators per source file. */
#define MAX_CUSTOM_OPERATORS  64

/* ── Semantic Analysis ────────────────────────────────────────────────────── */

/** @brief Max precedence group names for duplicate checking. */
#define SEMA_PG_NAMES_MAX     32

/** @brief Intern pool hash table capacity (must be power of 2). */
#define INTERN_POOL_CAP      4096

/** @brief Intern pool string buffer size in bytes (256 KB). */
#define INTERN_BUF_SIZE      (256 * 1024)

/** @brief Max entries in the protocol conformance table. */
#define CONFORMANCE_TABLE_MAX 256

/** @brief Max entries in the associated-type binding table. */
#define ASSOC_TYPE_TABLE_MAX  128

/** @brief Max registered @propertyWrapper types. */
#define WRAPPER_TABLE_MAX     64

/** @brief Max registered @resultBuilder types. */
#define BUILDER_TABLE_MAX     32

/** @brief Number of hash buckets per scope in the symbol table. */
#define SCOPE_HASH_SIZE       64

/** @brief Max captured variables per closure. */
#define CAPTURE_LIST_MAX      64

#endif /* MSF_LIMITS_H */
