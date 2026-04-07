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

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  2. CONFORMANCE & ASSOCIATED TYPES                                     │
 * └──────────────────────────────────────────────────────────────────────────┘ */

typedef struct {
  const char *type_name;
  const char *protocol_name;
  const void *where_ast;     /**< AST_WHERE_CLAUSE for conditional conformance. */
} ConformanceRecord;

typedef struct ConformanceTable {
  ConformanceRecord entries[CONFORMANCE_TABLE_MAX];
  uint32_t          count;
} ConformanceTable;

typedef struct {
  const char *type_name;
  const char *protocol_name;
  const char *assoc_name;
  const char *concrete_type_name;
} AssocTypeBinding;

typedef struct {
  AssocTypeBinding entries[ASSOC_TYPE_TABLE_MAX];
  uint32_t         count;
} AssocTypeTable;

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  3. ATTRIBUTE REGISTRIES                                               │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief @propertyWrapper registration entry. */
typedef struct {
  const char *name;
  ASTNode    *decl;
  TypeInfo   *type;
} WrapperEntry;

/** @brief @resultBuilder registration entry with method availability flags. */
typedef struct {
  const char *name;
  ASTNode    *decl;
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

  /* Attribute registries */
  WrapperEntry wrapper_types[WRAPPER_TABLE_MAX];
  uint32_t     wrapper_count;
  BuilderEntry builder_types[BUILDER_TABLE_MAX];
  uint32_t     builder_count;

  /* Diagnostics */
  uint32_t error_count;
  char     errors[32][256];
  uint32_t error_line[32];
  uint32_t error_col[32];

  /* Context state (set during tree walk) */
  TypeInfo    *expected_closure_type;
  uint8_t      requires_explicit_self;
  void        *current_func_decl;
  const ASTNode *ast_root;

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

  /* Precedence group names (for duplicate checking) */
  const char *pg_names[SEMA_PG_NAMES_MAX];
  uint32_t    pg_count;
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
  BMR_OPT_DATA, BMR_SUBSTRING, BMR_DATA,
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
const ASTNode *find_type_child(const ASTNode *decl);
const ASTNode *find_init_child(const ASTNode *decl);
void init_type_resolvers(void);
TypeInfo *resolve_type_ident(SemaContext *ctx, const ASTNode *tnode);
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

void conformance_table_init_builtins(ConformanceTable *ct);
void conformance_table_add(ConformanceTable *ct, const char *type_name,
                           const char *protocol_name);
void conformance_table_add_conditional(ConformanceTable *ct, const char *type_name,
                                       const char *protocol_name, const void *where_ast);
const void *conformance_table_get_where(const ConformanceTable *ct, const char *type_name,
                                        const char *protocol_name);
int conformance_table_has(const ConformanceTable *ct, const char *type_name,
                          const char *protocol_name);
int check_conformance(const ASTNode *type_decl, const ASTNode *proto_decl,
                      SemaContext *ctx, const ASTNode *ast_root);
void pass3_check_conformances(SemaContext *ctx, ASTNode *root);
void assoc_type_table_init(AssocTypeTable *at);
void assoc_type_table_add(AssocTypeTable *at, const char *type_name,
                          const char *protocol_name, const char *assoc_name,
                          const char *concrete_type_name);
const char *assoc_type_table_get(const AssocTypeTable *at, const char *type_name,
                                 const char *protocol_name, const char *assoc_name);
TypeInfo *resolve_assoc_type_to_concrete(SemaContext *ctx, const TypeInfo *concrete_type,
                                         const char *protocol_name, const char *assoc_name);
TypeInfo *infer_concrete_at_assoc(const ASTNode *proto_ast, TypeInfo *impl_ty,
                                  const char *assoc_name, SemaContext *ctx);
int type_ast_contains_assoc(const ASTNode *type_ast, SemaContext *ctx,
                            const char *assoc_name);
const char *type_ast_ident_name(const ASTNode *n, SemaContext *ctx);

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
uint32_t access_from_rank(int r);
uint32_t access_min(uint32_t a, uint32_t b);
uint32_t type_effective_access(SemaContext *ctx, TypeInfo *ty);
int private_member_visible(SemaContext *ctx, const ASTNode *member_decl,
                                const ASTNode *owning_type_decl);
void apply_protocol_requirement_access(ASTNode *proto_decl);
void apply_extension_member_access(const ASTNode *ext_decl);
void apply_default_member_access(const ASTNode *type_decl);
void apply_preceding_main_actor(SemaContext *ctx, ASTNode *node);
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
int class_decl_has_superclass(const ASTNode *decl);
const ASTNode *class_superclass_decl(SemaContext *ctx, const ASTNode *class_decl);
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
