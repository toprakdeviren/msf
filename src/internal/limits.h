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

/** @brief Sema diagnostics live in a heap array that grows on demand, starting
 *         at this capacity (see SemaContext.diags in private.h). */
#define SEMA_DIAG_INITIAL_CAP 16u

/** @brief Hard ceiling on retained sema diagnostics — a DoS guard against
 *         pathological inputs.  The last retained slot carries an
 *         "and N more diagnostics suppressed" summary so a truncated list is
 *         never mistaken for "no further errors".  Whole-module analysis of
 *         real modules stays well under this. */
#define SEMA_DIAG_MAX         100000u

/* The MAX_PRECEDENCE_GROUPS and MAX_CUSTOM_OPERATORS caps used to bound the
 * per-Parser arrays in private.h; those tables are now heap-grown so the
 * limits no longer apply.  Retained as guidance values for tools that want
 * to pre-size buffers. */
#define MAX_PRECEDENCE_GROUPS 32
#define MAX_CUSTOM_OPERATORS  64

/* ── Semantic Analysis ────────────────────────────────────────────────────── */

/** @brief Max precedence group names for duplicate checking. */
#define SEMA_PG_NAMES_MAX     32

/** @brief Max nested re-entry of resolve_type_ident before bailing out, to
 *  contain cyclic type-alias / nested-type chains (real code never nests type
 *  resolution remotely this deep; the limit only trips on a resolution cycle). */
#define SEMA_TYPE_RESOLVE_MAX_DEPTH 128u

/** @brief Max recursion depth of resolve_node (the central AST-node resolver),
 *  guarding against self-referential decl/member/expr resolution cycles that
 *  would otherwise overflow the stack.  Set well above any legitimate AST
 *  nesting (the parser caps nesting far lower) so it only trips on a cycle. */
#define SEMA_RESOLVE_MAX_DEPTH 512u

/** @brief Max recursion depth of check_conformance when climbing inherited
 *  protocols (`protocol A: B`), guarding against an inheritance cycle
 *  (`A: B`, `B: A`).  Real protocol inheritance never nests near this. */
#define SEMA_CONFORMANCE_MAX_DEPTH 64u

/** @brief Initial intern hash-table capacity (grows on demand; must be a
 *  power of 2). */
#define INTERN_POOL_CAP      4096

/** @brief Legacy buffer size — no longer enforced now that the intern pool
 *  uses a chunked, growable buffer.  Retained for source compatibility. */
#define INTERN_BUF_SIZE      (256 * 1024)

/* Conformance and associated-type tables are now heap-grown (see msf.h).
 * The previous MAX caps no longer apply; macros retained for compatibility
 * with downstream code that referenced them. */
#define CONFORMANCE_TABLE_MAX 256
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
