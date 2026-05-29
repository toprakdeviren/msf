/**
 * @file private.h
 * @brief Sema module internals — context, symbol table, all resolve/check protos.
 *
 * NOT part of the public API.  Included by all semantic .c files.
 * The public sema interface (sema_init/destroy/analyze) is declared
 * in internal/msf.h.
 *
 * WHAT THIS HEADER PROVIDES
 *
 *   SemaContext      — full definition (opaque in public header)
 *   Symbol / Scope   — symbol table with hash-based scope chain
 *   Conformance      — protocol conformance table + associated types
 *   Registries       — @propertyWrapper / @resultBuilder entries
 *   Capture analysis — closure capture identification
 *   Type helpers     — is_integer_kind, ACCESS_MODIFIER_MASK, BMKind
 *   All function protos — grouped by source file
 */
#pragma once

#include "../internal/sema.h"
#include "../internal/builtin_names.h"
#include "../internal/limits.h"
#include "module_stubs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  1. SYMBOL TABLE                                                       │
 * └──────────────────────────────────────────────────────────────────────────┘ */

typedef struct InternPool InternPool;

typedef enum {
  SYM_VAR, SYM_LET, SYM_FUNC, SYM_CLASS, SYM_STRUCT,
  SYM_ENUM, SYM_PROTOCOL, SYM_TYPEALIAS, SYM_PARAM,
  SYM_ENUM_CASE, SYM_TYPE, SYM_MODULE,
} SymbolKind;

typedef struct Symbol Symbol;
struct Symbol {
  const char *name;
  SymbolKind  kind;
  TypeInfo   *type;
  ASTNode    *decl;
  Symbol     *next;          /**< Hash bucket chain.                */
  uint8_t     is_initialized;
  uint8_t     is_deferred;
  uint8_t     is_resolving;  /**< Prevents infinite recursion.      */
};

typedef struct Scope Scope;
struct Scope {
  Symbol  *buckets[SCOPE_HASH_SIZE];
  Scope   *parent;
  uint32_t depth;
};

/* ── Member index ───────────────────────────────────────────────────────
 * Lazily-built name→member map for one type decl's body.  Without it every
 * `base.member` access linear-scans the whole type body looking for a
 * var/let/func of that name; on machine-generated files with a single huge
 * namespace type (thousands of nested decls) that is O(n^2).  Keyed by the
 * interned member-name pointer (names are interned, so pointer identity is
 * name identity); stores the first member declared under each name, exactly
 * matching the original first-match scan order.  A name miss is O(1) too —
 * the common case for accesses whose member is a nested *type* rather than a
 * stored/computed property. */
typedef struct {
  const char *name;     /**< interned member name (NULL = empty slot) */
  ASTNode    *member;   /**< matched member decl (var/let/func node)  */
} MemberSlot;

typedef struct MemberIndex MemberIndex;
struct MemberIndex {
  const ASTNode *decl;        /**< type decl this indexes (registry key) */
  MemberSlot    *slots;       /**< open-addressed table (cap is pow2)    */
  uint32_t       cap;
  uint32_t       count;
  MemberIndex   *next;        /**< registry bucket chain                 */
};

#define MEMBER_INDEX_BUCKETS 256u

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  2. CONFORMANCE & ASSOCIATED TYPES — moved to public msf.h (§14-15)    │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  ConformanceTable, ConformanceRecord, AssocTypeTable, AssocTypeBinding
 *  and their APIs now live in <msf.h> so backends can read them without
 *  pulling in sema internals.  Already available here via transitive
 *  include through internal/sema.h → msf.h.
 */

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  3. ATTRIBUTE REGISTRIES                                               │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief @propertyWrapper registration entry. */
typedef struct {
  const char *name;
  ASTNode    *decl;
  TypeInfo   *type;
  /* Member nodes inside the wrapper struct body — captured at register time
   * so we can answer "what is the type of $x" without re-walking the body
   * on every use. The pointers reference AST_VAR_DECL/AST_LET_DECL nodes
   * (or NULL when missing). */
  const ASTNode *wrapped_value_node;
  const ASTNode *projected_value_node;
} WrapperEntry;

/** @brief @resultBuilder registration entry with method availability flags. */
typedef struct {
  const char *name;
  ASTNode    *decl;
  /* The token stream `decl` was parsed from, captured at registration.  In
   * whole-module mode the builder type may live in a different file than the
   * one currently being resolved, so its method-name tokens must be read
   * against THIS stream, not ctx's active one (else an out-of-bounds read). */
  const Source *src;
  const Token  *tokens;
  uint32_t      token_count;
  const char *build_block;
  const char *build_expression;
  const char *build_optional;
  const char *build_either;
  const char *build_array;
  const char *build_final;
  const char *build_limited;
} BuilderEntry;

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  4. SEMA CONTEXT                                                       │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief One recorded semantic diagnostic (message + source location/range). */
typedef struct {
  uint32_t line, col;   /**< 1-based line/column of the offending node.   */
  uint32_t start, end;  /**< Source byte range [start, end) for the node. */
  const char *file;     /**< Owning file path (whole-module: the node's   */
                        /**< origin file, captured at record time).       */
  char     msg[256];    /**< Formatted message (truncated to fit).        */
} SemaDiag;

typedef struct SemaContext SemaContext;
struct SemaContext {
  /* Input (not owned) */
  const Source *src;
  const Token  *tokens;
  ASTArena     *ast_arena;
  TypeArena    *type_arena;

  /* Symbol table */
  Scope      *current_scope;
  InternPool *intern;

  /* Protocol conformance */
  ConformanceTable *conformance_table;
  AssocTypeTable   *assoc_type_table;
  /* Whole-module conformance-witness index (built once before pass 3): which
   * member names a type provides across its body + ALL its extensions
   * (witness_members), and each type/protocol's direct supertypes/inherited
   * protocols (witness_inherits) so a requirement can be satisfied by an
   * extension, an inherited member, or a protocol-extension default — anywhere
   * in the module.  Reuses the ConformanceTable (name,name) hash. */
  ConformanceTable *witness_members;
  ConformanceTable *witness_inherits;

  /* Attribute registries */
  WrapperEntry wrapper_types[WRAPPER_TABLE_MAX];
  uint32_t     wrapper_count;
  BuilderEntry builder_types[BUILDER_TABLE_MAX];
  uint32_t     builder_count;

  /* Diagnostics — a heap array grown on demand (no fixed cap; bounded only by
   * SEMA_DIAG_MAX as a DoS guard).  start/end are the source byte range of the
   * offending node (leftmost..rightmost token) for LSP/highlighting; a point
   * diagnostic has start == end.  Owned by ctx; freed in sema_destroy. */
  uint32_t  error_count;
  uint32_t  suppressed_count;
  SemaDiag *diags;
  uint32_t  diag_cap;

  /* Context state (set during tree walk) */
  TypeInfo    *expected_closure_type;
  /* Contextual type pushed onto the immediately-resolved expression so a literal
   * adopts it (e.g. `5` becomes CGFloat in `let x: CGFloat = 5`).  Captured and
   * cleared at the top of resolve_node_expr so it never leaks into nested
   * sub-expressions — each set-site re-establishes it for the child it governs. */
  TypeInfo    *expected_type;
  uint8_t      requires_explicit_self;
  void        *current_func_decl;
  const ASTNode *ast_root;

  /* Actor isolation context — scope of the function/closure currently being
   * resolved. `main_actor` is set for @MainActor and @MainActor-class members;
   * `actor_decl` points at the enclosing AST_ACTOR_DECL when inside an actor
   * method (so cross-actor access can be diagnosed). */
  uint8_t        current_isolation_main_actor;
  const ASTNode *current_actor_decl;
  /* Async context — set inside the body of an `async` function or closure.
   * Read by AST_AWAIT_EXPR resolution to reject `await` outside an async
   * scope. */
  uint8_t        current_function_async;

  /* Two-phase class initialization */
  uint8_t     in_class_init_phase1;
  uint8_t     init_is_convenience;
  uint8_t     init_has_delegated;
  void       *init_class_decl;
  const char *init_own_props[16];
  uint32_t    init_own_prop_count;
  uint8_t     init_own_assigned[16];

  /* Opaque return type enforcement */
  TypeInfo *opaque_return_constraint;
  TypeInfo *opaque_return_first_type;

  /* Access control */
  uint8_t     has_testable_import;
  const char *current_type_name;

  uint32_t do_catch_depth;

  /* Type-resolution recursion guard.  resolve_type_ident can re-enter itself
   * through type aliases and nested-type lookups; a cyclic alias chain (e.g.
   * `typealias A = B` / `typealias B = A`) would otherwise recurse until the
   * stack overflows.  Bounded by SEMA_TYPE_RESOLVE_MAX_DEPTH. */
  uint32_t type_resolve_depth;

  /* resolve_node recursion guard.  A self-referential declaration/member/expr
   * chain (a property whose resolution re-enters its own decl, mutually
   * recursive members, …) can drive resolve_node back into itself before a
   * node's ->type memo is set.  Bounded by SEMA_RESOLVE_MAX_DEPTH. */
  uint32_t resolve_depth;

  /* check_conformance recursion guard — bounds climbing inherited protocols
   * so an inheritance cycle (`protocol A: B` / `protocol B: A`) terminates.
   * Bounded by SEMA_CONFORMANCE_MAX_DEPTH. */
  uint32_t conformance_depth;

  /* Precedence group names (for duplicate checking) */
  const char *pg_names[SEMA_PG_NAMES_MAX];
  uint32_t    pg_count;

  /* Per-decl member lookup cache — see MemberIndex.  Avoids O(n^2) member
   * scans on huge generated types.  Owned by ctx; freed in sema_destroy. */
  MemberIndex *member_index[MEMBER_INDEX_BUCKETS];

  /* Lazy presence-set of conformance-table type names.  Lets member
   * resolution skip the O(conformances) associated-type scan when the base
   * type has no recorded conformance at all (the common case for namespace
   * enums in generated code).  Rebuilt when the table's count changes.
   * Owned by ctx; freed in sema_destroy.  See conf_name_present() in member.c. */
  const char **conf_name_set;   /* open-addressed; NULL slot = empty */
  uint32_t     conf_name_cap;   /* power of two                      */
  uint32_t     conf_name_built; /* conformance_table->count at build */

  /* tok_intern memoization: maps token index → its interned name pointer.
   * The same token is interned at many call sites across the sema passes;
   * caching by index skips the repeat ASCII scan + FNV hash + table probe.
   * Grows lazily; owned by ctx, freed in sema_destroy. */
  const char **tok_cache;
  uint32_t     tok_cache_cap;

  /* Lazy index for unqualified nested-type lookup across a nominal type's
   * primary declaration and extensions.  Avoids re-scanning every top-level
   * declaration for every `Kind` lookup in generated extension-heavy code.
   * Opaque to the context; owned by ctx, freed in sema_destroy. */
  void *nested_type_index;

  /* Whole-module analysis (sema_analyze_module): the number of tokens in the
   * current file's stream (0 in single-file mode = bounds check disabled), and
   * a map from a foreign file's decl node to the token stream it was parsed
   * from.  Together these keep cross-file token reads (e.g. lazy member-index
   * builds) reading the right file's tokens — see member_index.c. */
  uint32_t token_count;
  void    *origin_map;

  /* Optional runtime module vocabulary (a parsed .msfvocab).  When set,
   * `import X` resolves X's type names from this vocabulary before falling
   * back to the host-provided compiled module table (module_stub_find).
   * Not owned by ctx — the caller keeps it alive across analysis. */
  const struct MSFVocab *vocab;

  /* Optional SDK vocabulary used ONLY as a global last-resort fallback: a type
   * name present in ANY of its modules resolves regardless of which module was
   * imported (SDK types are reachable in Swift via re-export chains the
   * per-import `vocab` view doesn't model).  Kept distinct from `vocab` so that
   * project modules' published types stay import-scoped (cross-module
   * precision) while SDK names resolve globally.  Not owned by ctx. */
  const struct MSFVocab *sdk_vocab;
};

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  5. CAPTURE ANALYSIS                                                   │
 * └──────────────────────────────────────────────────────────────────────────┘ */

typedef enum {
  CAPTURE_STRONG = 0, CAPTURE_WEAK = 1, CAPTURE_UNOWNED = 2, CAPTURE_VALUE = 3,
} CaptureMode;

typedef struct {
  const char  *name;
  CaptureMode  mode;
  TypeInfo    *type;
  ASTNode     *decl;
  int          is_outer;
} CaptureInfo;

typedef struct {
  CaptureInfo captures[CAPTURE_LIST_MAX];
  uint32_t    count;
} CaptureList;

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  6. TYPE HELPERS                                                       │
 * └──────────────────────────────────────────────────────────────────────────┘ */

#define ACCESS_MODIFIER_MASK \
  (MOD_PUBLIC | MOD_PRIVATE | MOD_INTERNAL | MOD_FILEPRIVATE | MOD_PACKAGE | MOD_OPEN)

static inline int is_integer_kind(TypeKind k) {
  return k == TY_INT || k == TY_INT8 || k == TY_INT16 || k == TY_INT32 ||
         k == TY_INT64 || k == TY_UINT || k == TY_UINT8 || k == TY_UINT16 ||
         k == TY_UINT32 || k == TY_UINT64;
}

static inline int is_float_kind(TypeKind k) {
  return k == TY_FLOAT || k == TY_DOUBLE;
}

static inline int is_int_float_mix(const TypeInfo *a, const TypeInfo *b) {
  if (!a || !b) return 0;
  return (is_integer_kind(a->kind) && is_float_kind(b->kind)) ||
         (is_float_kind(a->kind) && is_integer_kind(b->kind));
}

/* An Int/Float operand mix is NOT an error when the integer side is a literal:
 * Swift integer literals adopt the Double/Float type of the other operand
 * (`d >= 1024`, `d /= 2`, `let d: Double = 5`).  Only a non-literal Int vs
 * Float genuinely needs an explicit conversion. */
static inline int int_literal_adapts(const ASTNode *lhs, const TypeInfo *lt,
                                     const ASTNode *rhs, const TypeInfo *rt) {
  /* Only an integer literal opposite a Float/Double adapts; an integer literal
   * against a non-float type (a struct, String, …) is still a real mismatch. */
  if (lt && is_integer_kind(lt->kind) && lhs && lhs->kind == AST_INTEGER_LITERAL &&
      rt && is_float_kind(rt->kind))
    return 1;
  if (rt && is_integer_kind(rt->kind) && rhs && rhs->kind == AST_INTEGER_LITERAL &&
      lt && is_float_kind(lt->kind))
    return 1;
  return 0;
}

/* Closure-literal return/parameter inference is unreliable (an empty or
 * Void-bodied closure can come back as `() -> nil`), so a function-typed value
 * assigned to a function-typed target — optionally Optional — is accepted
 * rather than risk a false "Type mismatch" like `(() -> Void)?` vs `() -> nil`.
 * Used by both the `=` (binary) and AST_ASSIGN_EXPR (dispatch) paths. */
static inline int func_to_func_assign(const TypeInfo *lt, const TypeInfo *rt) {
  if (lt && lt->kind == TY_OPTIONAL && lt->inner) lt = lt->inner;
  return lt && lt->kind == TY_FUNC && rt && rt->kind == TY_FUNC;
}

/* Any/AnyObject is the top type: every value converts to it (and, via optional
 * promotion, to Any?/AnyObject?).  So an assignment whose target is Any[?] /
 * AnyObject[?] is never a mismatch. */
static inline int assign_target_is_any(const TypeInfo *lt) {
  if (!lt) return 0;
  if (lt->kind == TY_OPTIONAL && lt->inner) lt = lt->inner;
  return type_is_any(lt) || type_is_anyobject(lt);
}

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  7. BUILTIN MEMBER RESOLUTION                                         │
 * └──────────────────────────────────────────────────────────────────────────┘ */

typedef enum {
  BMK_ARRAY, BMK_STRING, BMK_INT, BMK_DICT, BMK_SET, BMK_CHARACTER,
  BMK_BOOL, BMK_DOUBLE, BMK_OPTIONAL, BMK_SUBSTRING, BMK_DATA,
} BMKind;

typedef enum {
  BMR_INT, BMR_BOOL, BMR_STRING, BMR_DOUBLE, BMR_VOID,
  BMR_OPT_INNER, BMR_ARRAY_UNKNOWN, BMR_ARRAY_SAME, BMR_ARRAY_STRING,
  BMR_OPT_INT, BMR_OPT_DATA, BMR_SUBSTRING, BMR_DATA,
  BMR_ARRAY_KEY, BMR_ARRAY_VALUE, BMR_SELF_BASE, BMR_OPT_DICT_ELEMENT,
} BMResult;

typedef struct {
  BMKind      base_kind;
  const char *name;
  BMResult    result;
} BuiltinMemberEntry;

/* Public API (sema_init, sema_analyze, sema_destroy, error accessors)
 * is declared in internal/sema.h — included transitively above. */

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  8. INTERNAL FUNCTION PROTOTYPES                                       │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/* — Core (core.c) --------------------------------------------------------- */

void sema_ctx_reset(SemaContext *ctx, const Source *src, const Token *tokens,
                    ASTArena *ast_arena, TypeArena *type_arena);
void sema_free(SemaContext *ctx);
Scope  *sema_push_scope(SemaContext *ctx);
void    sema_pop_scope(SemaContext *ctx);
Symbol *sema_lookup(SemaContext *ctx, const char *name);
uint32_t sema_lookup_overloads(const SemaContext *ctx, const char *name,
                               Symbol **out, uint32_t max);
Symbol *sema_define(SemaContext *ctx, const char *name, SymbolKind kind,
                    TypeInfo *type, ASTNode *decl);
const char *sema_intern(SemaContext *ctx, const char *str, size_t len);
const char *tok_intern(SemaContext *ctx, uint32_t tok_idx);
uint32_t sym_hash(const char *name);
void sema_error(SemaContext *ctx, const ASTNode *node, const char *fmt, ...);
void sema_error_suggest(SemaContext *ctx, const ASTNode *node,
                        const char *suggestion, const char *fmt, ...);
const char *sema_find_similar_type_name(SemaContext *ctx, const char *name);
int lev_distance(const char *a, const char *b);
TypeInfo *resolve_builtin(const char *name);
void sema_import_module(SemaContext *ctx, const char *module_name);
void sema_add_precedence_group_name(SemaContext *ctx, const ASTNode *node);
int sema_has_precedence_group(const SemaContext *ctx, const char *name);
const ASTNode *find_ancestor_closure(const ASTNode *node);
const ASTNode *find_enclosing_type_decl(const ASTNode *node);
const ASTNode *find_enclosing_struct_decl(const ASTNode *node);
const ASTNode *root_ident_of_expr(const ASTNode *expr);
const ASTNode *named_type_decl(SemaContext *ctx, TypeInfo *t);
int type_is_value_type(SemaContext *ctx, TypeInfo *t);
int method_is_mutating(SemaContext *ctx, const ASTNode *decl, const char *mname);
int is_stored_property_of_struct(SemaContext *ctx, const ASTNode *struct_decl,
                                 const char *name);
int symbol_is_instance_member_of(const Symbol *sym, const ASTNode *type_decl);

/* — Declare pass (declare.c) ---------------------------------------------- */

void declare_node(SemaContext *ctx, ASTNode *node);
void declare_children(SemaContext *ctx, ASTNode *node);
void declare_in_scope(SemaContext *ctx, ASTNode *node);
void declare_named(SemaContext *ctx, ASTNode *node, SymbolKind sk, int is_nominal);
void declare_typealias(SemaContext *ctx, ASTNode *node);

/* — Type resolution (type_resolution.c) ----------------------------------- */

TypeInfo *resolve_type_annotation(SemaContext *ctx, const ASTNode *tnode);
TypeInfo *resolve_typealias_decl(SemaContext *ctx, const ASTNode *decl);
const ASTNode *find_type_child(const ASTNode *decl);
const ASTNode *find_init_child(const ASTNode *decl);
void init_type_resolvers(void);
TypeInfo *resolve_type_ident(SemaContext *ctx, const ASTNode *tnode);
/** @brief Resolves a member access on a vocabulary (SDK) type known only by
 *  name, using the v2 vocabulary's member signatures.  Returns the member's
 *  type, or NULL if not found / not modelled (positive-only). */
TypeInfo *resolve_vocab_member_type(SemaContext *ctx, const char *type_name,
                                    const char *member_name, int prefer_callable);
TypeInfo *resolve_type_optional(SemaContext *ctx, const ASTNode *tnode);
TypeInfo *resolve_type_array(SemaContext *ctx, const ASTNode *tnode);
TypeInfo *resolve_type_dict(SemaContext *ctx, const ASTNode *tnode);
TypeInfo *resolve_type_func(SemaContext *ctx, const ASTNode *tnode);
TypeInfo *resolve_type_tuple(SemaContext *ctx, const ASTNode *tnode);
TypeInfo *resolve_type_passthrough(SemaContext *ctx, const ASTNode *tnode);
TypeInfo *resolve_type_generic(SemaContext *ctx, const ASTNode *tnode);
TypeInfo *resolve_type_composition(SemaContext *ctx, const ASTNode *tnode);

/* — Node resolution (resolve/) -------------------------------------------- */

TypeInfo *resolve_node(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_node_decl(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_node_expr(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_children(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_var_decl(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_func_decl(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_binary_expr(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_call_expr(SemaContext *ctx, ASTNode *node);
TypeInfo *resolve_member_expr(SemaContext *ctx, ASTNode *node);

/* — Conformance (conformance.c, conformance_table.c) ---------------------- */
/* conformance_table_* and assoc_type_table_* declared in <msf.h> §14-15. */

int check_conformance(const ASTNode *type_decl, const ASTNode *proto_decl,
                      SemaContext *ctx, const ASTNode *ast_root);
void pass3_check_conformances(SemaContext *ctx, ASTNode *root);
int  is_type_sendable(const SemaContext *ctx, const TypeInfo *t);

/* Add one file's top-level decls to the whole-module conformance-witness index
 * (call per file, with that file's tokens active). */
void sema_witness_index_add_root(SemaContext *ctx, const ASTNode *root);
/* 1 if `type_name` provides a witness named `req_name` anywhere in the module:
 * its body/extensions, a superclass/inherited-protocol member, or a protocol-
 * extension default — walking the inheritance graph transitively. */
int  witness_satisfies(SemaContext *ctx, const char *type_name,
                       const char *req_name);

/* Literal coercion: 1 if `lit` (an integer/float/string/bool/nil literal node)
 * may adopt `target` — i.e. `target` conforms to the literal's ExpressibleBy*
 * protocol (nil also adopts any Optional).  Used both to accept a literal at a
 * type-mismatch checkpoint and to let it adopt a contextual type. */
int literal_coerces_to(SemaContext *ctx, const ASTNode *lit,
                       const TypeInfo *target);

/* 1 if `rt` is assignable to `lt` by (direct) subtyping — rt inherits from or
 * conforms to lt (lt may be Optional<Base>).  Backed by the conformance table. */
int subtype_assignable(SemaContext *ctx, const TypeInfo *lt, const TypeInfo *rt);

/* Folds a vocabulary's (v3) per-type conformances into the conformance table,
 * synthesizing the ExpressibleBy*Literal protocols numeric SDK types imply. */
void sema_load_vocab_conformances(SemaContext *ctx, const struct MSFVocab *v);
void infer_and_check_sendable(SemaContext *ctx, const ASTNode *root);
void check_sendable_closures(SemaContext *ctx, const ASTNode *root);
TypeInfo *resolve_assoc_type_to_concrete(SemaContext *ctx, const TypeInfo *concrete_type,
                                         const char *protocol_name, const char *assoc_name);
TypeInfo *infer_concrete_at_assoc(const ASTNode *proto_ast, TypeInfo *impl_ty,
                                  const char *assoc_name, SemaContext *ctx);
int type_ast_contains_assoc(const ASTNode *type_ast, SemaContext *ctx,
                            const char *assoc_name);
const char *type_ast_ident_name(const ASTNode *n, SemaContext *ctx);
const char *param_external_label_str(SemaContext *ctx, const ASTNode *param,
                                     int *out_omitted);
int validate_witness_func_signature(SemaContext *ctx, const ASTNode *type_decl,
                                    const ASTNode *proto_decl,
                                    const ASTNode *req, const ASTNode *impl,
                                    const char *type_name,
                                    const char *proto_name,
                                    const char *req_name);

/* — Generics (generics.c) ------------------------------------------------- */

int check_constraint_satisfaction(const TypeInfo *concrete,
                                  const TypeConstraint *constraint,
                                  const ConformanceTable *ct, SemaContext *ctx,
                                  const ASTNode *site);
int check_generic_args(TypeInfo *const *params, uint32_t param_cnt,
                       TypeInfo *const *args, uint32_t arg_cnt,
                       const ConformanceTable *ct, SemaContext *ctx,
                       const ASTNode *site);

/* — Builder (@resultBuilder — builder.c) ---------------------------------- */

const BuilderEntry *node_get_builder(SemaContext *ctx, const ASTNode *node);
uint32_t builder_method_name_tok(const SemaContext *ctx, const BuilderEntry *be,
                                 const char *method_name);
ASTNode *wrap_in_build_expression(SemaContext *ctx, const BuilderEntry *be,
                                  ASTNode *expr_node);
ASTNode *wrap_builder_method_call(SemaContext *ctx, const BuilderEntry *be,
                                  const char *method_name, ASTNode *inner);
ASTNode *build_block_call_from_stmts(SemaContext *ctx, const BuilderEntry *be,
                                     const ASTNode *first_stmt);
ASTNode *transform_builder_body(SemaContext *ctx, const BuilderEntry *be,
                                const ASTNode *body_block);

/* — Capture analysis (resolve/node.c) ------------------------------------- */

int identify_captures(const ASTNode *closure_node, SemaContext *outer_ctx,
                      CaptureList *out);
void scan_idents(const ASTNode *node, const char **local_names,
                 uint32_t local_count, SemaContext *outer_ctx, CaptureList *out);
void collect_local_names(const ASTNode *node, const char **names,
                         uint32_t *count, uint32_t max, SemaContext *ctx);

/* — Access control (access.c) -------------------------------------------- */

int access_rank(uint32_t mods);
uint32_t effective_access(uint32_t mods);
uint32_t access_from_rank(int r);
uint32_t access_min(uint32_t a, uint32_t b);
uint32_t type_effective_access(SemaContext *ctx, TypeInfo *ty);
int private_member_visible(SemaContext *ctx, const ASTNode *member_decl,
                                const ASTNode *owning_type_decl);
void apply_protocol_requirement_access(ASTNode *proto_decl);
void apply_extension_member_access(const ASTNode *ext_decl);
void apply_default_member_access(const ASTNode *type_decl);
void apply_preceding_main_actor(SemaContext *ctx, ASTNode *node);
/* Non-zero if `type_decl` is main-actor-isolated: explicit @MainActor, or
 * conforms to / inherits a well-known @MainActor framework anchor (SwiftUI
 * View/App/Scene/…, UIKit/AppKit UI base classes). Pure (no AST mutation) —
 * used only to recognise the *caller* context, never to mark a flagging
 * target. */
int type_decl_is_main_actor_isolated(SemaContext *ctx, const ASTNode *type_decl);
void check_protocol_inheritance_access(SemaContext *ctx, const ASTNode *proto_decl);
void check_enum_case_values_access(SemaContext *ctx, const ASTNode *enum_decl);

/* — Builtin members (resolve/expr_member.c) ------------------------------- */

TypeInfo *lookup_builtin_member(SemaContext *ctx, TypeInfo *base_t, const char *mname);
TypeInfo *lookup_named_member(SemaContext *ctx, TypeInfo *ty, const char *mname);
int base_to_bmkind(const TypeInfo *base_t);
TypeInfo *bmr_to_type(BMResult r, SemaContext *ctx, TypeInfo *base_t);
TypeInfo *wrap_optional_result(TypeInfo *t, int do_wrap, SemaContext *ctx);
TypeInfo *get_contextual_type_for_implicit_member(SemaContext *ctx, const ASTNode *node);
TypeInfo *get_sequence_element_type(SemaContext *ctx, TypeInfo *seq_t);
int is_lhs_optional_chain(const ASTNode *expr);

/* — Class / protocol helpers ---------------------------------------------- */

const ASTNode *class_decl_body(const ASTNode *decl);

/* Returns the var/let/func member named @p mname (interned) declared directly
 * in @p body's children (falling back to @p decl's children when body is
 * NULL), or NULL.  Builds and caches a name→member index for @p decl on first
 * use, so repeated member lookups on the same type are O(1) instead of an
 * O(members) linear scan.  Semantics match the original scan: first match in
 * declaration order, var/let by node tok_idx, func by name token, and the
 * `@attr` + var/let pattern by the var's name token. */
ASTNode *sema_member_lookup(SemaContext *ctx, const ASTNode *decl,
                            const ASTNode *body, const char *mname);

/* Frees every cached MemberIndex (called from sema_destroy). */
void sema_member_index_free(SemaContext *ctx);

/* Whole-module origin map: record the token stream / source a decl node was
 * parsed from, so a later member-index build (possibly while a *different*
 * file's stream is active) reads the decl's own tokens.  Registered during
 * sema_analyze_module()'s declare phase; freed from sema_destroy(). */
typedef struct {
  const Source  *src;
  const Token   *tokens;
  uint32_t       token_count;
  const ASTNode *ast_root;
  uint8_t        switched;
} SemaOriginState;

void sema_origin_register(SemaContext *ctx, const ASTNode *node, const Source *src,
                          const Token *tokens, uint32_t token_count,
                          const ASTNode *ast_root);
int sema_origin_enter(SemaContext *ctx, const ASTNode *node,
                      SemaOriginState *state);
void sema_origin_leave(SemaContext *ctx, const SemaOriginState *state);
const char *tok_intern_at_node(SemaContext *ctx, const ASTNode *node,
                               uint32_t tok_idx);
void sema_origin_free(SemaContext *ctx);
void sema_origin_for_each_root(SemaContext *ctx,
                               void (*fn)(SemaContext *, const ASTNode *, void *),
                               void *user);
void sema_nested_type_index_free(SemaContext *ctx);

/* Frees the conformance table's internal (type,protocol) lookup index. */
void conformance_index_free(ConformanceTable *ct);

int class_decl_has_superclass(const ASTNode *decl);
const ASTNode *class_superclass_decl(SemaContext *ctx, const ASTNode *class_decl);
int class_has_unresolved_superclass(SemaContext *ctx, const ASTNode *class_decl);
uint32_t class_stored_property_names(SemaContext *ctx, const ASTNode *class_decl,
                                     const char **names, uint32_t max);
int is_inherited_stored_property(SemaContext *ctx, const ASTNode *class_decl,
                                 const char *prop_name);
uint32_t init_param_count(const ASTNode *init_decl);
uint32_t class_required_init_param_counts(SemaContext *ctx, const ASTNode *class_decl,
                                          uint32_t *counts, uint32_t max);
uint32_t class_designated_init_param_counts(SemaContext *ctx, const ASTNode *class_decl,
                                            uint32_t *counts, uint32_t max);
uint32_t class_convenience_init_param_counts(SemaContext *ctx, const ASTNode *class_decl,
                                             uint32_t *counts, uint32_t max);
int class_has_init_with_param_count(SemaContext *ctx, const ASTNode *class_decl,
                                    uint32_t param_count);
int superclass_has_required_init_with_param_count(SemaContext *ctx, const ASTNode *super_decl,
                                                  uint32_t param_count);
const char *nominal_type_name(SemaContext *ctx, const ASTNode *decl);
int decl_is_inside_extension(const ASTNode *node);
void define_nested_types_in_scope(SemaContext *ctx, const ASTNode *type_decl);
int protocol_extension_has_default(SemaContext *ctx, const ASTNode *root,
                                   const char *proto_name, const char *req_name);
int protocol_req_is_associated_type(const ASTNode *req);
int protocol_req_is_property(const ASTNode *req, const char *req_name);
const ASTNode *protocol_req_return_type_node(const ASTNode *req);
const ASTNode *func_decl_return_type_node(const ASTNode *decl);
