/**
 * @file declare.c
 * @brief Forward declaration pass — registers all names into the symbol table.
 *
 * This is the first pass over the AST.  It walks top-down, registering
 * every type, function, variable, import, and typealias before the second
 * pass (type resolution in resolve/) runs.  This ensures forward references
 * resolve correctly.
 *
 * Also handles:
 *   - Protocol conformance recording
 *   - @propertyWrapper / @resultBuilder registration
 *   - Class override/final enforcement
 *   - Enum indirect recursion checks
 *   - Access level validation
 */
#include "private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Tree Walking
 * ═══════════════════════════════════════════════════════════════════════════════ */

void declare_node(SemaContext *ctx, ASTNode *node);

/** @brief Recursively visits all children of a node. */
void declare_children(SemaContext *ctx, ASTNode *node) {
  for (ASTNode *c = node->first_child; c; c = c->next_sibling)
    declare_node(ctx, c);
}

/** @brief Visits children inside a new scope. */
void declare_in_scope(SemaContext *ctx, ASTNode *node) {
  sema_push_scope(ctx);
  declare_children(ctx, node);
  sema_pop_scope(ctx);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns the first AST_BLOCK child of a node, or NULL. */
static const ASTNode *find_body(const ASTNode *node) {
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
    if (c->kind == AST_BLOCK) return c;
  return NULL;
}

/** @brief Returns the interned name of a member (func or var/let). */
static const char *member_name(SemaContext *ctx, const ASTNode *m) {
  uint32_t tok = (m->kind == AST_FUNC_DECL) ? m->data.func.name_tok
                                             : m->data.var.name_tok;
  if (!tok) return NULL;
  return tok_intern(ctx, tok);
}

/**
 * @brief Returns the effective access level of a node, defaulting to internal.
 *
 * Used for access-level comparisons (subclass vs superclass, override, etc.).
 */
static uint32_t effective_access(uint32_t mods) {
  uint32_t acc_mask = MOD_PUBLIC | MOD_PRIVATE | MOD_INTERNAL |
                      MOD_FILEPRIVATE | MOD_PACKAGE;
  uint32_t acc = mods & acc_mask;
  return acc ? acc : MOD_INTERNAL;
}

/**
 * @brief Checks if a preceding sibling is an @attribute matching @p attr_name.
 *
 * Common pattern: walk siblings to find the AST_ATTRIBUTE just before a decl.
 * Returns the attribute node, or NULL if not found.
 */
static const ASTNode *find_preceding_attr(SemaContext *ctx, const ASTNode *node,
                                          const char *attr_name) {
  if (!node->parent) return NULL;
  for (const ASTNode *sib = node->parent->first_child; sib;
       sib = sib->next_sibling) {
    if (sib->next_sibling != node) continue;
    if (sib->kind != AST_ATTRIBUTE) return NULL;
    const Token *at = &ctx->tokens[sib->data.var.name_tok];
    const char *name = sema_intern(ctx, ctx->src->data + at->pos, at->len);
    return strcmp(name, attr_name) == 0 ? sib : NULL;
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Enum Indirect Recursion
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Checks that recursive enum cases are marked `indirect`. */
static void check_enum_indirect_recursion(SemaContext *ctx,
                                          const ASTNode *enum_decl) {
  if (!enum_decl->data.var.name_tok) return;
  const ASTNode *body = find_body(enum_decl);
  if (!body) return;

  const char *enum_name = tok_intern(ctx, enum_decl->data.var.name_tok);
  int enum_is_indirect = (enum_decl->modifiers & MOD_INDIRECT) != 0;

  for (const ASTNode *cd = body->first_child; cd; cd = cd->next_sibling) {
    if (cd->kind != AST_ENUM_CASE_DECL) continue;
    int case_indirect = enum_is_indirect || (cd->modifiers & MOD_INDIRECT);
    for (const ASTNode *el = cd->first_child; el; el = el->next_sibling) {
      if (el->kind != AST_ENUM_ELEMENT_DECL) continue;
      for (const ASTNode *p = el->first_child; p; p = p->next_sibling) {
        if (p->kind != AST_PARAM) continue;
        const ASTNode *ty = find_type_child(p);
        if (!ty || ty->kind != AST_TYPE_IDENT || ty->first_child) continue;
        const char *tname = tok_intern(ctx, ty->tok_idx);
        if (!tname || strcmp(enum_name, tname) != 0) continue;
        if (!case_indirect)
          sema_error(ctx, (ASTNode *)ty,
                     "recursive enum case '.%s' must be marked 'indirect' "
                     "or the enum must be declared 'indirect'",
                     tok_intern(ctx, el->data.var.name_tok));
      }
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Class Override / Final Checks
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Finds the parent class name from a class declaration's inheritance clause. */
static const char *find_parent_class_name(SemaContext *ctx,
                                          const ASTNode *node) {
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_BLOCK) break;
    if (c->kind == AST_CONFORMANCE && c->first_child && c->first_child->tok_idx)
      return tok_intern(ctx, c->first_child->tok_idx);
    if (c->kind == AST_TYPE_IDENT || c->kind == AST_IDENT_EXPR)
      return tok_intern(ctx, c->tok_idx);
  }
  return NULL;
}

/** @brief Finds a member in a class body by interned name. Returns the node. */
static const ASTNode *find_member_in_body(SemaContext *ctx,
                                          const ASTNode *body,
                                          const char *name) {
  if (!body) return NULL;
  for (const ASTNode *m = body->first_child; m; m = m->next_sibling) {
    const char *mname = member_name(ctx, m);
    if (mname && mname == name) return m;
  }
  return NULL;
}

/** @brief Checks a single member against the parent class for override violations. */
static void check_member_override(SemaContext *ctx, const ASTNode *m,
                                  const ASTNode *parent_body) {
  int has_override = (m->modifiers & MOD_OVERRIDE) != 0;
  int is_func = (m->kind == AST_FUNC_DECL);
  int is_prop = (m->kind == AST_VAR_DECL || m->kind == AST_LET_DECL);
  if (!is_func && !is_prop) return;

  const char *mname = member_name(ctx, m);
  if (!mname) return;

  const ASTNode *parent_m = find_member_in_body(ctx, parent_body, mname);
  if (!parent_m) return;

  int parent_is_final = (parent_m->modifiers & MOD_FINAL) != 0;
  const char *kind_str = is_func ? "method" : "property";

  /* Missing override keyword */
  if (!has_override)
    sema_error(ctx, (ASTNode *)m,
               "overriding declaration requires an 'override' keyword for %s '%s'",
               kind_str, mname);

  /* Cannot override final member */
  if (parent_is_final && has_override)
    sema_error(ctx, (ASTNode *)m, "cannot override 'final' %s '%s'",
               kind_str, mname);

  /* static property cannot override non-class-var */
  if (is_prop && has_override && (m->modifiers & MOD_STATIC) &&
      !parent_m->data.var.is_class_var)
    sema_error(ctx, (ASTNode *)m,
               "cannot override static property '%s'; only class var is overrideable",
               mname);

  /* Override access must be >= overridden access */
  if (has_override &&
      access_rank(effective_access(m->modifiers)) <
          access_rank(effective_access(parent_m->modifiers)))
    sema_error(ctx, (ASTNode *)m,
               "overriding %s must be at least as accessible as "
               "the overridden declaration",
               kind_str);
}

/**
 * @brief Enforces override/final rules for class declarations.
 *
 * Checks: final class cannot be subclassed, access level constraints,
 * public-but-not-open, and per-member override/final rules.
 */
static void check_class_overrides(SemaContext *ctx, const ASTNode *node) {
  if (node->kind != AST_CLASS_DECL) return;

  const char *parent_name = find_parent_class_name(ctx, node);
  if (!parent_name) return;

  const Symbol *parent_sym = sema_lookup(ctx, parent_name);
  if (!parent_sym || !parent_sym->decl ||
      parent_sym->decl->kind != AST_CLASS_DECL)
    return;
  const ASTNode *parent_decl = parent_sym->decl;

  /* Final class cannot be subclassed */
  if (parent_decl->modifiers & MOD_FINAL)
    sema_error(ctx, node, "cannot inherit from final class '%s'", parent_name);

  /* Subclass access must not exceed superclass access */
  if (access_rank(effective_access(node->modifiers)) >
      access_rank(effective_access(parent_decl->modifiers)))
    sema_error(ctx, node,
               "subclass must not have higher access level than superclass");

  /* Public but not open → cannot subclass */
  if ((parent_decl->modifiers & MOD_PUBLIC) &&
      !(parent_decl->modifiers & MOD_OPEN))
    sema_error(ctx, node,
               "cannot subclass; superclass is public but not 'open'");

  /* Per-member override checks */
  const ASTNode *body = find_body(node);
  const ASTNode *parent_body = find_body(parent_decl);
  if (!body) return;
  for (const ASTNode *m = body->first_child; m; m = m->next_sibling)
    check_member_override(ctx, m, parent_body);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Protocol Conformance Recording
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Records protocol conformances from a type's inheritance clause. */
static void record_conformances(SemaContext *ctx, const ASTNode *node,
                                const char *iname) {
  if (!ctx->conformance_table) return;
  if (node->kind != AST_STRUCT_DECL && node->kind != AST_CLASS_DECL &&
      node->kind != AST_ENUM_DECL && node->kind != AST_ACTOR_DECL)
    return;

  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_CONFORMANCE) continue;
    for (const ASTNode *proto = c->first_child; proto;
         proto = proto->next_sibling) {
      if (!proto->tok_idx) continue;
      const char *pname = tok_intern(ctx, proto->tok_idx);

      /* Protocol must be at least as visible as the conforming type */
      Symbol *ps = sema_lookup(ctx, pname);
      if (ps && ps->decl && ps->decl->kind == AST_PROTOCOL_DECL) {
        if (access_rank(effective_access(ps->decl->modifiers)) <
            access_rank(effective_access(node->modifiers)))
          sema_error(ctx, (ASTNode *)proto,
                     "cannot conform to protocol '%s' that is less "
                     "visible than the type",
                     pname);
      }
      conformance_table_add(ctx->conformance_table, iname, pname);
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * @propertyWrapper Registration
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Registers a @propertyWrapper struct into the wrapper registry. */
static void register_property_wrapper(SemaContext *ctx, ASTNode *node,
                                      const char *iname, TypeInfo *ti) {
  if (node->kind != AST_STRUCT_DECL) return;
  if (!find_preceding_attr(ctx, node, SW_ATTR_PROPERTY_WRAPPER)) return;
  if (ctx->wrapper_count >= WRAPPER_TABLE_MAX) return;

  ctx->wrapper_types[ctx->wrapper_count].name = iname;
  ctx->wrapper_types[ctx->wrapper_count].decl = node;
  ctx->wrapper_types[ctx->wrapper_count].type = ti;
  ctx->wrapper_count++;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * @resultBuilder Registration
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Builder method lookup table — maps method names to BuilderEntry field offsets. */
static const struct { const char *name; size_t offset; } BUILDER_METHODS[] = {
    {SW_BUILDER_BUILD_BLOCK,      offsetof(BuilderEntry, build_block)},
    {SW_BUILDER_BUILD_EXPRESSION, offsetof(BuilderEntry, build_expression)},
    {SW_BUILDER_BUILD_OPTIONAL,   offsetof(BuilderEntry, build_optional)},
    {SW_BUILDER_BUILD_EITHER,     offsetof(BuilderEntry, build_either)},
    {SW_BUILDER_BUILD_ARRAY,      offsetof(BuilderEntry, build_array)},
    {SW_BUILDER_BUILD_FINAL,      offsetof(BuilderEntry, build_final)},
};
#define BUILDER_METHOD_COUNT (sizeof(BUILDER_METHODS) / sizeof(BUILDER_METHODS[0]))

/** @brief Scans a type body for builder method declarations and populates the entry. */
static void scan_builder_methods(SemaContext *ctx, BuilderEntry *be,
                                 const ASTNode *body) {
  for (const ASTNode *m = body->first_child; m; m = m->next_sibling) {
    if (m->kind != AST_FUNC_DECL) continue;
    const char *mn = tok_intern(ctx, m->data.func.name_tok);
    for (size_t i = 0; i < BUILDER_METHOD_COUNT; i++) {
      const char **slot = (const char **)((char *)be + BUILDER_METHODS[i].offset);
      if (!*slot && !strcmp(mn, BUILDER_METHODS[i].name)) {
        *slot = mn;
        break;
      }
    }
  }
}

/** @brief Registers a @resultBuilder type and scans for its builder methods. */
static void register_result_builder(SemaContext *ctx, ASTNode *node,
                                    const char *iname) {
  if (node->kind != AST_STRUCT_DECL && node->kind != AST_CLASS_DECL &&
      node->kind != AST_ENUM_DECL)
    return;
  if (!find_preceding_attr(ctx, node, SW_ATTR_RESULT_BUILDER)) return;
  if (ctx->builder_count >= BUILDER_TABLE_MAX) return;

  BuilderEntry *be = &ctx->builder_types[ctx->builder_count++];
  memset(be, 0, sizeof(*be));
  be->name = iname;
  be->decl = node;

  const ASTNode *body = find_body(node);
  if (body)
    scan_builder_methods(ctx, be, body);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * @propertyWrapper Usage Detection
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Detects @WrapperType annotation on var/let declarations. */
static void detect_wrapper_usage(SemaContext *ctx, ASTNode *node) {
  if (node->kind != AST_VAR_DECL && node->kind != AST_LET_DECL) return;
  if (node->data.var.has_wrapper || ctx->wrapper_count == 0 || !node->parent)
    return;

  for (const ASTNode *sib = node->parent->first_child; sib;
       sib = sib->next_sibling) {
    if (sib->next_sibling != node) continue;
    if (sib->kind != AST_ATTRIBUTE) break;
    const Token *at = &ctx->tokens[sib->data.var.name_tok];
    const char *attr_name = sema_intern(ctx, ctx->src->data + at->pos, at->len);
    for (uint32_t i = 0; i < ctx->wrapper_count; i++) {
      if (strcmp(ctx->wrapper_types[i].name, attr_name) == 0) {
        node->data.var.has_wrapper = 1;
        node->data.var.wrapper_type_tok = sib->data.var.name_tok;
        break;
      }
    }
    break;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Simple Declaration Handlers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Registers a typealias and validates access level. */
void declare_typealias(SemaContext *ctx, ASTNode *node) {
  const char *iname = tok_intern(ctx, node->data.var.name_tok);
  const ASTNode *rhs = find_type_child(node);
  TypeInfo *aliased = resolve_type_annotation(ctx, rhs);
  sema_define(ctx, iname, SYM_TYPEALIAS, aliased, node);

  if (aliased) {
    uint32_t aliased_eff = type_effective_access(ctx, aliased);
    if (access_rank(effective_access(node->modifiers)) > access_rank(aliased_eff))
      sema_error(ctx, node,
                 "type alias cannot be more visible than the type it aliases");
  }
}

/** @brief Imports module types and tracks @testable. */
static void declare_import(SemaContext *ctx, ASTNode *node) {
  if (node->modifiers & MOD_TESTABLE_IMPORT)
    ctx->has_testable_import = 1;
  const char *mod = tok_intern(ctx, node->data.var.name_tok);
  sema_import_module(ctx, mod);
}

/** @brief Validates an operator declaration's precedence group reference. */
static void declare_operator(SemaContext *ctx, const ASTNode *node) {
  const ASTNode *ref = node->first_child;
  if (!ref || ref->kind != AST_PG_GROUP_REF || !ref->data.var.name_tok) return;
  const char *gname = tok_intern(ctx, ref->data.var.name_tok);
  if (!sema_has_precedence_group(ctx, gname))
    sema_error(ctx, node, "unknown precedence group '%s'", gname);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Character Literal Validation
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Validates that a Character-typed var/let is initialized with a single character. */
static void validate_character_literal(SemaContext *ctx, const ASTNode *node) {
  if (node->kind != AST_VAR_DECL && node->kind != AST_LET_DECL) return;
  const ASTNode *tann = find_type_child(node);
  if (!tann || tann->kind != AST_TYPE_IDENT) return;
  const Token *tt = &ctx->tokens[tann->tok_idx];
  if (tt->len != 9 || memcmp(ctx->src->data + tt->pos, "Character", 9) != 0)
    return;

  const ASTNode *init = find_init_child(node);
  if (!init || init->kind != AST_STRING_LITERAL) return;
  const Token *st = &ctx->tokens[init->tok_idx];
  const char *lit = ctx->src->data + st->pos;
  if (st->len < 2 || lit[0] != '"') return;

  size_t content_len = st->len - 2;
  if (content_len == 0) {
    sema_error(ctx, (ASTNode *)init,
               "Cannot convert empty string to 'Character'");
  } else if (content_len > 1) {
    /* Check if it's a simple multi-char ASCII string (not unicode/escape) */
    int simple = 1;
    for (size_t i = 0; i < content_len; i++)
      if ((unsigned char)lit[1 + i] > 127 || lit[1 + i] == '\\')
        { simple = 0; break; }
    if (simple)
      sema_error(ctx, (ASTNode *)init,
                 "Cannot convert \"%.*s\" to 'Character'; use exactly one character",
                 (int)st->len, lit);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Named Declaration Registration
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Registers a named symbol and runs post-declaration checks.
 *
 * For nominal types: allocates TY_NAMED.  For variables: resolves type
 * annotation.  Then runs conformance recording, attribute registration,
 * class override checks, and descends into children.
 */
void declare_named(SemaContext *ctx, ASTNode *node, SymbolKind sk,
                   int is_nominal) {
  const char *iname = member_name(ctx, node);

  /* Allocate type info */
  TypeInfo *ti = NULL;
  if (node->kind == AST_STRUCT_DECL || node->kind == AST_CLASS_DECL ||
      node->kind == AST_ENUM_DECL || node->kind == AST_ACTOR_DECL ||
      node->kind == AST_PROTOCOL_DECL) {
    ti = type_arena_alloc(ctx->type_arena);
    if (!ti) return;
    ti->kind = TY_NAMED;
    ti->named.name = iname;
    ti->named.decl = node;
  } else if (node->kind != AST_FUNC_DECL) {
    ti = resolve_type_annotation(ctx, find_type_child(node));
  }

  sema_define(ctx, iname, sk, ti, node);
  if (ti && !node->type)
    ((ASTNode *)node)->type = ti;

  validate_character_literal(ctx, node);

  /* Post-declaration: conformances, attributes, overrides */
  if (node->kind == AST_ENUM_DECL)
    check_enum_indirect_recursion(ctx, node);

  record_conformances(ctx, node, iname);
  register_property_wrapper(ctx, node, iname, ti);
  check_class_overrides(ctx, node);
  register_result_builder(ctx, node, iname);
  detect_wrapper_usage(ctx, node);

  if (is_nominal)
    declare_in_scope(ctx, node);
  else
    declare_children(ctx, node);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Main Dispatcher
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Dispatches AST node to the appropriate declaration handler. */
void declare_node(SemaContext *ctx, ASTNode *node) {
  if (!node) return;

  switch (node->kind) {
  case AST_FUNC_DECL:     declare_named(ctx, node, SYM_FUNC, 0);     return;
  case AST_VAR_DECL:      declare_named(ctx, node, SYM_VAR, 0);      return;
  case AST_LET_DECL:      declare_named(ctx, node, SYM_LET, 0);      return;
  case AST_CLASS_DECL:    declare_named(ctx, node, SYM_CLASS, 1);    return;
  case AST_STRUCT_DECL:   declare_named(ctx, node, SYM_STRUCT, 1);   return;
  case AST_ENUM_DECL:     declare_named(ctx, node, SYM_ENUM, 1);     return;
  case AST_PROTOCOL_DECL: declare_named(ctx, node, SYM_PROTOCOL, 1); return;
  case AST_ACTOR_DECL:    declare_named(ctx, node, SYM_CLASS, 1);    return;
  case AST_TYPEALIAS_DECL: declare_typealias(ctx, node);              return;
  case AST_EXTENSION_DECL: declare_in_scope(ctx, node);               return;
  case AST_IMPORT_DECL:   declare_import(ctx, node);                  return;
  case AST_PRECEDENCE_GROUP_DECL: return;
  case AST_OPERATOR_DECL: declare_operator(ctx, node);                return;
  case AST_CLOSURE_EXPR:
  case AST_BLOCK:         declare_in_scope(ctx, node);                return;
  default: break;
  }

  declare_children(ctx, node);
}
