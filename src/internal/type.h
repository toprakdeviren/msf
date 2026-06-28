/**
 * @file type.h
 * @brief Type module API — arena, constraints, substitution, predicates.
 *
 * NOT part of the public API.  This header is the internal contract
 * between the type module and its consumers (sema, type.c, tests).
 *
 * WHAT THIS MODULE PROVIDES
 *
 *   Arena         — chunk-based allocator for TypeInfo values
 *   Builtins      — TY_BUILTIN_* singleton initialization
 *   Constraints   — TypeConstraint for generic where-clauses
 *   Substitution  — replace generic params with concrete types
 *   Predicates    — type_is_named(), type_is_any(), type_is_never(), etc.
 *
 * USAGE
 *
 *   TypeArena arena;
 *   type_arena_init(&arena, 0);
 *
 *   TypeInfo *ti = type_arena_alloc(&arena);
 *   ti->kind = TY_ARRAY;
 *   ti->inner = TY_BUILTIN_INT;   // [Int]
 *
 *   type_arena_free(&arena);       // frees all types at once
 *
 * OWNERSHIP
 *
 *   TypeInfo values are owned by the arena.  Do not free individually.
 *   Some TypeInfo fields (.func.params, .tuple.elems, .generic.args)
 *   are heap-allocated arrays — type_arena_free() handles them.
 */
#ifndef MSF_TYPE_INTERNAL_H
#define MSF_TYPE_INTERNAL_H

/*
 * Type module internal header.
 *
 * Runtime ABI (TypeArena, TypeConstraint, TypeSubstitution, type predicates)
 * now lives in the public msf.h — see SECTIONS 10-13.  This file just
 * re-exports it for internal consumers so existing #include "type.h" sites
 * do not have to change.
 */

#include <msf.h>
#include "builtin_names.h" /* SW_TYPE_*, SW_PROTO_* constants */

#endif /* MSF_TYPE_INTERNAL_H */
