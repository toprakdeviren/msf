/**
 * @file declaration.c
 * @brief Declaration type resolution: var/let, func, init, class, struct,
 *        enum, protocol, extension, actor, subscript, and access control checks.
 *
 * Resolves type annotations, infers types from initializers, validates
 * access levels, and populates ASTNode.type for all declaration kinds.
 */
#include "../private.h"

TypeInfo *resolve_node(SemaContext *ctx, ASTNode *node);

/** @brief Returns 1 if the node is inside an extension (not a nested nominal type). */
int decl_is_inside_extension(const ASTNode *node) {
  for (const ASTNode *p = node->parent; p; p = p->parent) {
    if (p->kind == AST_EXTENSION_DECL)
      return 1;
    if (p->kind == AST_STRUCT_DECL || p->kind == AST_CLASS_DECL ||
        p->kind == AST_ENUM_DECL || p->kind == AST_ACTOR_DECL)
      return 0;
  }
  return 0;
}

/**
 * @brief Resolves a var/let declaration: @propertyWrapper, type annotation,
 *        init inference, computed property bodies, and access checks.
 */
TypeInfo *resolve_var_decl(SemaContext *ctx, ASTNode *node) {
  const char *iname = tok_intern(ctx, node->data.var.name_tok);

  /* Extensions must not contain stored properties */
  if (decl_is_inside_extension(node) && !node->data.var.is_computed) {
    sema_error(ctx, node, "extensions must not contain stored properties");
    return node->type ? node->type : TY_BUILTIN_INT;
  }

  /* Detect @WrapperType attribute immediately preceding this var */
  if (!node->data.var.has_wrapper && node->parent) {
    for (const ASTNode *sib = node->parent->first_child; sib;
         sib = sib->next_sibling) {
      if (sib->next_sibling != node)
        continue;
      if (sib->kind != AST_ATTRIBUTE)
        break;
      const Token *at = &ctx->tokens[sib->data.var.name_tok];
      const char *attr_name =
          sema_intern(ctx, ctx->src->data + at->pos, at->len);
      for (uint32_t wi = 0; wi < ctx->wrapper_count; wi++) {
        if (ctx->wrapper_types[wi].name == attr_name) {
          ((ASTNode *)node)->data.var.has_wrapper = 1;
          ((ASTNode *)node)->data.var.wrapper_type_tok = sib->data.var.name_tok;
          break;
        }
      }
      break;
    }
  }

  /* Annotated type has priority */
  const ASTNode *tann = find_type_child(node);
  TypeInfo *annot_t = resolve_type_annotation(ctx, tann);

  /* If wrapper-backed, resolve from wrapper type */
  TypeInfo *wrapper_t = NULL;
  if (node->data.var.has_wrapper) {
    const Token *wt = &ctx->tokens[node->data.var.wrapper_type_tok];
    const char *wname = sema_intern(ctx, ctx->src->data + wt->pos, wt->len);
    for (uint32_t wi = 0; wi < ctx->wrapper_count; wi++) {
      if (ctx->wrapper_types[wi].name == wname) {
        wrapper_t = ctx->wrapper_types[wi].type;
        break;
      }
    }
  }

  /* Init expression type (inference) */
  const ASTNode *init = find_init_child(node);

  TypeInfo *init_t = resolve_node(ctx, (ASTNode *)init);

  /* Float literal inference:
   * In Swift, a float literal like 3.14 defaults to Double.
   * However, if the variable has an explicit Float annotation,
   * the literal should be narrowed to Float (f32).
   * e.g. var x: Float = 3.14  -> literal is Float, not Double */
  if (annot_t && annot_t == TY_BUILTIN_FLOAT && init &&
      init->kind == AST_FLOAT_LITERAL) {
    ((ASTNode *)init)->type = TY_BUILTIN_FLOAT;
    init_t = TY_BUILTIN_FLOAT;
  }

  /* Empty collection literal type propagation:
   * If the init is an empty array/dict literal ([] or [:]) and we have a
   * type annotation, propagate the annotation type to the init node so
   * the IR emitter knows whether to emit an array or dictionary. */
  if (annot_t && init && !init->first_child &&
      (init->kind == AST_ARRAY_LITERAL || init->kind == AST_DICT_LITERAL)) {
    ((ASTNode *)init)->type = annot_t;
    init_t = annot_t;
  }

  TypeInfo *final_t = annot_t ? annot_t : wrapper_t ? wrapper_t : init_t;
  node->type = final_t;

  /* Update symbol if it exists */
  Symbol *sym = sema_lookup(ctx, iname);
  if (sym && !sym->type)
    sym->type = final_t;
  else if (!sym) {
    /* Pass1 defines symbols in a temporary scope that gets freed between
     * passes. Re-define in the current scope so closure capture analysis can
     * find them. */
    SymbolKind sk = (node->kind == AST_LET_DECL) ? SYM_LET : SYM_VAR;
    sema_define(ctx, iname, sk, final_t, node);
    sym = sema_lookup(ctx, iname);
  }

  /* Deferred initialization for `let`:
   * If this is a LET_DECL with NO init expression, the variable starts
   * uninitialized. The symbol is marked so that exactly one future assignment
   * is allowed, and any read before that assignment is an error. */
  if (sym && node->kind == AST_LET_DECL && !init) {
    sym->is_initialized = 0;
    sym->is_deferred = 1;
  }

  /* Computed local -- resolve getter/setter bodies so inner refs and types
   * are set */
  if (node->data.var.is_computed) {
    if (node->data.var.getter_body)
      resolve_node(ctx, (ASTNode *)node->data.var.getter_body);
    if (node->data.var.setter_body)
      resolve_node(ctx, (ASTNode *)node->data.var.setter_body);
  }

  /* Tuple/function type access = min(constituent access); public var/let
   * cannot expose more private type */
  if ((node->modifiers & MOD_PUBLIC) && final_t) {
    uint32_t eff = type_effective_access(ctx, final_t);
    if (access_rank(eff) < 2)
      sema_error(ctx, node,
                 "public declaration cannot have a type with private or "
                 "fileprivate constituent");
  }

  /* Getter/setter access -- private(set) etc.: setter access must not be
   * more visible than the property */
  if (node->kind == AST_VAR_DECL && node->data.var.setter_access != 0) {
    uint32_t acc_mask = (MOD_PUBLIC | MOD_PRIVATE | MOD_INTERNAL |
                         MOD_FILEPRIVATE | MOD_PACKAGE);
    uint32_t prop_acc = node->modifiers & acc_mask;
    if (!prop_acc)
      prop_acc = MOD_INTERNAL;
    uint32_t setter_mod = 0;
    switch (node->data.var.setter_access) {
    case 1:
      setter_mod = MOD_PRIVATE;
      break;
    case 2:
      setter_mod = MOD_FILEPRIVATE;
      break;
    case 3:
      setter_mod = MOD_INTERNAL;
      break;
    case 4:
      setter_mod = MOD_PACKAGE;
      break;
    default:
      break;
    }
    if (setter_mod && access_rank(setter_mod) > access_rank(prop_acc))
      sema_error(
          ctx, node,
          "setter access level must not be more visible than the property");
  }
  return final_t;
}

/** @brief Returns 1 if the class has a superclass (inheritance clause before body). */
int class_decl_has_superclass(const ASTNode *decl) {
  if (!decl || decl->kind != AST_CLASS_DECL)
    return 0;
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_BLOCK)
      break;
    if (c->kind == AST_CONFORMANCE || c->kind == AST_TYPE_IDENT ||
        c->kind == AST_IDENT_EXPR)
      return 1;
  }
  return 0;
}

/** @brief Returns the body block (first AST_BLOCK child) of a type declaration. */
const ASTNode *class_decl_body(const ASTNode *decl) {
  if (!decl)
    return NULL;
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling)
    if (c->kind == AST_BLOCK)
      return c;
  return NULL;
}

/** @brief Defines nested types in the current scope with qualified names (Outer.Inner). */
void define_nested_types_in_scope(SemaContext *ctx, const ASTNode *type_decl) {
  const char *prefix = ctx->current_type_name;
  if (!prefix)
    return;
  const ASTNode *body = class_decl_body(type_decl);
  if (!body)
    return;
  for (const ASTNode *c = body->first_child; c; c = c->next_sibling) {
    SymbolKind sk;
    if (c->kind == AST_STRUCT_DECL)
      sk = SYM_STRUCT;
    else if (c->kind == AST_CLASS_DECL)
      sk = SYM_CLASS;
    else if (c->kind == AST_ENUM_DECL)
      sk = SYM_ENUM;
    else
      continue;
    if (!c->data.var.name_tok)
      continue;
    const char *short_name = tok_intern(ctx, c->data.var.name_tok);
    char qbuf[256];
    int n = snprintf(qbuf, sizeof(qbuf), "%s.%s", prefix, short_name);
    if (n <= 0 || (size_t)n >= sizeof(qbuf))
      continue;
    const char *qname = sema_intern(ctx, qbuf, (size_t)n);
    TypeInfo *ti = type_arena_alloc(ctx->type_arena);
    if (!ti) continue;
    ti->kind = TY_NAMED;
    ti->named.name = qname;
    ti->named.decl = (ASTNode *)c;
    sema_define(ctx, short_name, sk, ti, (ASTNode *)c);
    if (ti && !((ASTNode *)c)->type)
      ((ASTNode *)c)->type = ti;
  }
}

/** @brief Applies default member access: public type → internal members, enum cases inherit enum access. */
void apply_default_member_access(const ASTNode *type_decl) {
  const ASTNode *body = class_decl_body(type_decl);
  if (!body)
    return;
  uint32_t type_access = type_decl->modifiers & ACCESS_MODIFIER_MASK;
  uint32_t default_member;
  if (type_access & MOD_PUBLIC)
    default_member = MOD_INTERNAL;
  else if (type_access & MOD_PRIVATE)
    default_member = MOD_PRIVATE;
  else if (type_access & MOD_FILEPRIVATE)
    default_member = MOD_FILEPRIVATE;
  else if (type_access & MOD_PACKAGE)
    default_member = MOD_PACKAGE;
  else
    default_member = MOD_INTERNAL;
  uint32_t enum_case_access = type_access ? type_access : MOD_INTERNAL;
  for (ASTNode *c = (ASTNode *)body->first_child; c;
       c = (ASTNode *)c->next_sibling) {
    int is_member =
        (c->kind == AST_VAR_DECL || c->kind == AST_LET_DECL ||
         c->kind == AST_FUNC_DECL || c->kind == AST_INIT_DECL ||
         c->kind == AST_DEINIT_DECL || c->kind == AST_TYPEALIAS_DECL ||
         c->kind == AST_ENUM_CASE_DECL || c->kind == AST_SUBSCRIPT_DECL ||
         c->kind == AST_STRUCT_DECL || c->kind == AST_CLASS_DECL ||
         c->kind == AST_ENUM_DECL);
    if (!is_member)
      continue;
    uint32_t access_to_apply = default_member;
    if (type_decl->kind == AST_ENUM_DECL && c->kind == AST_ENUM_CASE_DECL)
      access_to_apply = enum_case_access;
    /* Default init access = type access (not internal when type is
     * public) */
    if (c->kind == AST_INIT_DECL)
      access_to_apply = type_access ? type_access : (uint32_t)MOD_INTERNAL;
    if ((c->modifiers & ACCESS_MODIFIER_MASK) == 0)
      c->modifiers |= access_to_apply;
    if (type_decl->kind == AST_ENUM_DECL && c->kind == AST_ENUM_CASE_DECL) {
      for (ASTNode *el = (ASTNode *)c->first_child; el;
           el = (ASTNode *)el->next_sibling)
        if (el->kind == AST_ENUM_ELEMENT_DECL &&
            (el->modifiers & ACCESS_MODIFIER_MASK) == 0)
          el->modifiers |= enum_case_access;
    }
  }
}

/** @brief Applies extension member default access (private extension → private members). */
void apply_extension_member_access(const ASTNode *ext_decl) {
  const ASTNode *body = class_decl_body(ext_decl);
  if (!body)
    return;
  uint32_t ext_acc = ext_decl->modifiers & ACCESS_MODIFIER_MASK;
  if (!ext_acc)
    ext_acc = MOD_INTERNAL;
  for (ASTNode *c = (ASTNode *)body->first_child; c;
       c = (ASTNode *)c->next_sibling) {
    int is_member =
        (c->kind == AST_VAR_DECL || c->kind == AST_LET_DECL ||
         c->kind == AST_FUNC_DECL || c->kind == AST_INIT_DECL ||
         c->kind == AST_TYPEALIAS_DECL || c->kind == AST_SUBSCRIPT_DECL);
    if (!is_member)
      continue;
    if ((c->modifiers & ACCESS_MODIFIER_MASK) == 0)
      c->modifiers |= ext_acc;
  }
}

/** @brief Sets protocol requirement access to the protocol's own access level. */
void apply_protocol_requirement_access(ASTNode *proto_decl) {
  uint32_t proto_acc = proto_decl->modifiers & ACCESS_MODIFIER_MASK;
  if (!proto_acc)
    proto_acc = MOD_INTERNAL;
  for (ASTNode *c = (ASTNode *)proto_decl->first_child; c;
       c = (ASTNode *)c->next_sibling) {
    if (c->kind != AST_PROTOCOL_REQ)
      continue;
    if ((c->modifiers & ACCESS_MODIFIER_MASK) == 0)
      c->modifiers |= proto_acc;
  }
}

/** @brief Checks that inherited protocols are at least as visible as the declaring protocol. */
void check_protocol_inheritance_access(SemaContext *ctx,
                                       const ASTNode *proto_decl) {
  uint32_t acc_mask =
      (MOD_PUBLIC | MOD_PRIVATE | MOD_INTERNAL | MOD_FILEPRIVATE | MOD_PACKAGE);
  uint32_t proto_acc = proto_decl->modifiers & acc_mask;
  if (!proto_acc)
    proto_acc = MOD_INTERNAL;
  int proto_rank = access_rank(proto_acc);
  for (const ASTNode *c = proto_decl->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_CONFORMANCE)
      continue;
    for (const ASTNode *inh = c->first_child; inh; inh = inh->next_sibling) {
      if (inh->kind != AST_TYPE_IDENT || inh->tok_idx == 0)
        continue;
      const Token *t = &ctx->tokens[inh->tok_idx];
      const char *pname = sema_intern(ctx, ctx->src->data + t->pos, t->len);
      Symbol *ps = sema_lookup(ctx, pname);
      if (!ps || !ps->decl || ps->decl->kind != AST_PROTOCOL_DECL)
        continue;
      if (ps->decl == proto_decl)
        continue;
      uint32_t inh_acc = ps->decl->modifiers & acc_mask;
      if (!inh_acc)
        inh_acc = MOD_INTERNAL;
      if (access_rank(inh_acc) < proto_rank)
        sema_error(ctx, (ASTNode *)inh,
                   "inherited protocol '%s' must be at least as visible as the "
                   "protocol declaring it",
                   pname);
    }
  }
}

/** @brief Checks that types used in enum case values are not more private than the enum. */
void check_enum_case_values_access(SemaContext *ctx, const ASTNode *enum_decl) {
  uint32_t enum_access = enum_decl->modifiers & ACCESS_MODIFIER_MASK;
  uint32_t min_ok = enum_access ? enum_access : MOD_INTERNAL;
  int min_rank = access_rank(min_ok);
  const ASTNode *body = class_decl_body(enum_decl);
  if (!body)
    return;
  for (const ASTNode *c = body->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_ENUM_CASE_DECL)
      continue;
    for (const ASTNode *el = c->first_child; el; el = el->next_sibling) {
      if (el->kind != AST_ENUM_ELEMENT_DECL)
        continue;
      for (const ASTNode *ch = el->first_child; ch; ch = ch->next_sibling) {
        TypeInfo *t = NULL;
        if (ch->kind == AST_PARAM) {
          const ASTNode *assoc_ty = find_type_child(ch);
          if (assoc_ty)
            t = resolve_type_annotation(ctx, (ASTNode *)assoc_ty);
        } else if (ch->kind != AST_PARAM && ch->type) {
          t = ch->type; /* raw value expr already resolved */
        }
        if (!t)
          continue;
        uint32_t acc = type_effective_access(ctx, t);
        if (access_rank(acc) < min_rank)
          sema_error(ctx, (ASTNode *)ch,
                     "type used in enum case (raw or associated value) must "
                     "not be more private than the enum");
      }
    }
  }
}

/* Collect stored property names introduced by this class (var/let, not
 * computed). Returns count. */
uint32_t class_stored_property_names(SemaContext *ctx,
                                     const ASTNode *class_decl,
                                     const char **names, uint32_t max) {
  const ASTNode *body = class_decl_body(class_decl);
  if (!body)
    return 0;
  uint32_t n = 0;
  for (const ASTNode *c = body->first_child; c && n < max;
       c = c->next_sibling) {
    if (c->kind != AST_VAR_DECL && c->kind != AST_LET_DECL)
      continue;
    if (c->data.var.is_computed)
      continue;
    if (!c->data.var.name_tok)
      continue;
    const char *name = tok_intern(ctx, c->data.var.name_tok);
    if (name)
      names[n++] = name;
  }
  return n;
}

/* Get superclass AST decl from class decl (first type in conformance).
 * Returns NULL if no superclass. */
const ASTNode *class_superclass_decl(SemaContext *ctx,
                                     const ASTNode *class_decl) {
  if (!class_decl || class_decl->kind != AST_CLASS_DECL)
    return NULL;
  const ASTNode *conf = NULL;
  for (const ASTNode *c = class_decl->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_BLOCK)
      break;
    if (c->kind == AST_CONFORMANCE) {
      conf = c;
      break;
    }
  }
  if (!conf || !conf->first_child)
    return NULL;
  TypeInfo *super_t =
      resolve_type_annotation(ctx, (ASTNode *)conf->first_child);
  if (!super_t || super_t->kind != TY_NAMED || !super_t->named.decl)
    return NULL;
  const ASTNode *super_decl = (const ASTNode *)super_t->named.decl;
  if (super_decl->kind != AST_CLASS_DECL)
    return NULL;
  return super_decl;
}

/* Return 1 if prop_name is a stored property of a superclass of
 * class_decl. */
int is_inherited_stored_property(SemaContext *ctx, const ASTNode *class_decl,
                                 const char *prop_name) {
  const ASTNode *super_decl = class_superclass_decl(ctx, class_decl);
  if (!super_decl)
    return 0;
  const char *super_props[16];
  uint32_t n = class_stored_property_names(ctx, super_decl, super_props, 16);
  for (uint32_t i = 0; i < n; i++)
    if (super_props[i] && strcmp(super_props[i], prop_name) == 0)
      return 1;
  return is_inherited_stored_property(ctx, super_decl, prop_name);
}

/* Count parameters of an init/func decl (number of AST_PARAM children). */
uint32_t init_param_count(const ASTNode *init_decl) {
  if (!init_decl ||
      (init_decl->kind != AST_INIT_DECL && init_decl->kind != AST_FUNC_DECL))
    return 0;
  uint32_t n = 0;
  for (const ASTNode *c = init_decl->first_child; c; c = c->next_sibling)
    if (c->kind == AST_PARAM)
      n++;
  return n;
}

/* Collect param counts of designated inits (not convenience) from class
 * body. Returns count. */
uint32_t class_designated_init_param_counts(SemaContext *ctx,
                                            const ASTNode *class_decl,
                                            uint32_t *counts, uint32_t max) {
  (void)ctx;
  const ASTNode *body = class_decl_body(class_decl);
  if (!body)
    return 0;
  uint32_t n = 0;
  for (const ASTNode *c = body->first_child; c && n < max;
       c = c->next_sibling) {
    if (c->kind != AST_INIT_DECL)
      continue;
    if (c->modifiers & MOD_CONVENIENCE)
      continue;
    counts[n++] = init_param_count(c);
  }
  return n;
}

/* Collect param counts of convenience inits from class body. Returns
 * count. */
uint32_t class_convenience_init_param_counts(SemaContext *ctx,
                                             const ASTNode *class_decl,
                                             uint32_t *counts, uint32_t max) {
  (void)ctx;
  const ASTNode *body = class_decl_body(class_decl);
  if (!body)
    return 0;
  uint32_t n = 0;
  for (const ASTNode *c = body->first_child; c && n < max;
       c = c->next_sibling) {
    if (c->kind != AST_INIT_DECL)
      continue;
    if (!(c->modifiers & MOD_CONVENIENCE))
      continue;
    counts[n++] = init_param_count(c);
  }
  return n;
}

/* Collect param counts of required inits (designated or convenience) from
 * class body. Returns count. */
uint32_t class_required_init_param_counts(SemaContext *ctx,
                                          const ASTNode *class_decl,
                                          uint32_t *counts, uint32_t max) {
  (void)ctx;
  if (!class_decl || class_decl->kind != AST_CLASS_DECL)
    return 0;
  const ASTNode *body = class_decl_body(class_decl);
  if (!body)
    return 0;
  uint32_t n = 0;
  for (const ASTNode *c = body->first_child; c && n < max;
       c = c->next_sibling) {
    if (c->kind != AST_INIT_DECL)
      continue;
    if (!(c->modifiers & MOD_REQUIRED))
      continue;
    counts[n++] = init_param_count(c);
  }
  return n;
}

/* Return 1 if superclass has a required init with the given param count. */
int superclass_has_required_init_with_param_count(SemaContext *ctx,
                                                  const ASTNode *super_decl,
                                                  uint32_t param_count) {
  uint32_t required[16];
  uint32_t nr = class_required_init_param_counts(ctx, super_decl, required, 16);
  for (uint32_t i = 0; i < nr; i++)
    if (required[i] == param_count)
      return 1;
  return 0;
}

/* Return 1 if class has an available init that takes param_count
 * arguments (own or inherited per 2 rules). */
int class_has_init_with_param_count(SemaContext *ctx, const ASTNode *class_decl,
                                    uint32_t param_count) {
  uint32_t designated[16], convenience[16];
  uint32_t nd =
      class_designated_init_param_counts(ctx, class_decl, designated, 16);
  uint32_t nc =
      class_convenience_init_param_counts(ctx, class_decl, convenience, 16);
  for (uint32_t i = 0; i < nd; i++)
    if (designated[i] == param_count)
      return 1;
  for (uint32_t i = 0; i < nc; i++)
    if (convenience[i] == param_count)
      return 1;
  const ASTNode *super = class_superclass_decl(ctx, class_decl);
  if (!super) {
    /* No superclass: if no explicit inits defined, class gets implicit
     * default zero-arg init (Swift synthesizes init() when all stored
     * properties have defaults). */
    if (nd == 0 && nc == 0 && param_count == 0)
      return 1;
    return 0;
  }
  uint32_t super_designated[16], super_convenience[16];
  uint32_t sd =
      class_designated_init_param_counts(ctx, super, super_designated, 16);
  uint32_t sc =
      class_convenience_init_param_counts(ctx, super, super_convenience, 16);
  int inherit_all_designated = (nd == 0);
  int implements_all_designated = 1;
  if (sd > 0 && !inherit_all_designated) {
    for (uint32_t i = 0; i < sd; i++) {
      int found = 0;
      for (uint32_t j = 0; j < nd && !found; j++)
        if (designated[j] == super_designated[i])
          found = 1;
      for (uint32_t j = 0; j < nc && !found; j++)
        if (convenience[j] == super_designated[i])
          found = 1;
      if (!found) {
        implements_all_designated = 0;
        break;
      }
    }
  }
  if (inherit_all_designated)
    for (uint32_t i = 0; i < sd; i++)
      if (super_designated[i] == param_count)
        return 1;
  /* If subclass has no explicit inits and we're looking for zero-arg init,
   * check if superclass has one (explicit or implicit, recursively). */
  if (inherit_all_designated && nc == 0 && param_count == 0 && sd == 0)
    if (class_has_init_with_param_count(ctx, super, 0))
      return 1;
  if (implements_all_designated)
    for (uint32_t i = 0; i < sc; i++)
      if (super_convenience[i] == param_count)
        return 1;
  return 0;
}

/* Apply preceding @MainActor attribute to func/struct/class decl. */
void apply_preceding_main_actor(SemaContext *ctx, ASTNode *node) {
  if (!node || !node->parent)
    return;
  for (const ASTNode *sib = node->parent->first_child; sib;
       sib = sib->next_sibling) {
    if (sib->next_sibling != node)
      continue;
    if (sib->kind != AST_ATTRIBUTE)
      break;
    const Token *at = &ctx->tokens[sib->data.var.name_tok];
    const char *attr_name = sema_intern(ctx, ctx->src->data + at->pos, at->len);
    if (attr_name && strcmp(attr_name, SW_ATTR_MAIN_ACTOR) == 0)
      node->modifiers |= MOD_MAIN_ACTOR;
    break;
  }
}

/**
 * @brief Resolves a function declaration: return type, generic params, opaque
 *        return (some P), cycle-breaking, async/throws, builder transform, and
 *        access checks.
 */
TypeInfo *resolve_func_decl(SemaContext *ctx, ASTNode *node) {
  apply_preceding_main_actor(ctx, node);
  /* Resolve return type annotation FIRST (before body) */
  TypeInfo *ret_t = NULL;
  ASTNode *ret_type_node = NULL;
  for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind >= AST_TYPE_IDENT &&
        (c->kind <= AST_TYPE_ANY || c->kind == AST_TYPE_COMPOSITION)) {
      ret_type_node = c;
      ret_t = resolve_type_annotation(ctx, c);
      break;
    }
  }
  /* some P -> opaque return; enforce single concrete return type */
  TypeInfo *saved_opaque_constraint = ctx->opaque_return_constraint;
  TypeInfo *saved_opaque_first = ctx->opaque_return_first_type;
  if (ret_type_node && ret_type_node->kind == AST_TYPE_SOME && ret_t) {
    ctx->opaque_return_constraint = ret_t;
    ctx->opaque_return_first_type = NULL;
  } else {
    ctx->opaque_return_constraint = NULL;
    ctx->opaque_return_first_type = NULL;
  }

  /* Build TY_FUNC early and register — breaks recursion for mutually recursive functions. */
  TypeInfo *ti = type_arena_alloc(ctx->type_arena);
  if (!ti) return NULL;
  ti->kind = TY_FUNC;
  /* Implicit generic -- unnamed type param for opaque return (-> some P) */
  TypeInfo *effective_ret = ret_t ? ret_t : TY_BUILTIN_VOID;
  if (ret_type_node && ret_type_node->kind == AST_TYPE_SOME && ret_t &&
      ret_t->kind == TY_NAMED && ret_t->named.name) {
    TypeInfo *opaque_param = type_arena_alloc(ctx->type_arena);
    opaque_param->kind = TY_GENERIC_PARAM;
    opaque_param->param.name = sema_intern(ctx, "$OpqRet", 7);
    opaque_param->param.index = 0;
    TypeConstraint *tc = malloc(sizeof(TypeConstraint));
    if (tc) {
      tc->kind = TC_CONFORMANCE;
      tc->protocol_name = ret_t->named.name;
      tc->rhs_type = NULL;
      tc->assoc_name = NULL;
      tc->rhs_param_name = NULL;
      tc->rhs_assoc_name = NULL;
      opaque_param->param.constraints = tc;
      opaque_param->param.constraint_count = 1;
      effective_ret = opaque_param;
    }
  }
  ti->func.ret = effective_ret;
  ti->func.param_count = 0;
  ti->func.params = NULL;
  node->type = ti; /* set early -- stops re-entry via resolve_node guard */

  const char *fname = tok_intern(ctx, node->data.func.name_tok);
  Symbol *sym = sema_lookup(ctx, fname);
  if (sym && !sym->type)
    sym->type = ti;

  /* Resolve body in a new scope */
  sema_push_scope(ctx);

  /* F1.4: If async, register __async_ctx in scope */
  int is_async = (node->modifiers & MOD_ASYNC) != 0;
  if (is_async) {
    const char *ctx_name = sema_intern(ctx, "__async_ctx", 11);
    sema_define(ctx, ctx_name, SYM_VAR, NULL, node);
  }

  /* Generic params -- register each <T: Proto> in function scope */
  for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_GENERIC_PARAM)
      continue;
    const char *pname = tok_intern(ctx, c->tok_idx);
    TypeInfo *gp_ti = type_arena_alloc(ctx->type_arena);
    gp_ti->kind = TY_GENERIC_PARAM;
    gp_ti->param.name = pname;
    gp_ti->param.index = 0;
    gp_ti->param.constraints = NULL;
    gp_ti->param.constraint_count = 0;
    c->type = gp_ti;
    sema_define(ctx, pname, SYM_TYPE, gp_ti, c);
  }

  /* Track current function for mutating check (struct instance methods) */
  ctx->current_func_decl = node;
  /* Params + body */
  for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_PARAM)
      resolve_node(ctx, c);
    else if (c->kind == AST_BLOCK) {
      const BuilderEntry *be = node_get_builder(ctx, node);
      if (be) {
        ASTNode *blk_call = transform_builder_body(ctx, be, c);
        if (blk_call) {
          resolve_node(ctx, blk_call);
          if (!ret_t && blk_call->type)
            ret_t = blk_call->type;
        }
      } else {
        resolve_children(ctx, c);
      }
    } else if (c->kind == AST_WHERE_CLAUSE) {
      resolve_node(ctx, c);
    }
  }
  ctx->current_func_decl = NULL;
  /* restore opaque return state */
  ctx->opaque_return_constraint = saved_opaque_constraint;
  ctx->opaque_return_first_type = saved_opaque_first;

  sema_pop_scope(ctx);

  /* Update ret if inferred from body (non-annotated funcs) */
  if (!ret_t)
    ti->func.ret = TY_BUILTIN_VOID;

  /* Function type access = min(param + return access); public func cannot
   * expose more private type */
  if ((node->modifiers & MOD_PUBLIC) && ti) {
    uint32_t eff = type_effective_access(ctx, ti);
    if (access_rank(eff) < 2)
      sema_error(ctx, node,
                 "public function cannot have a type with private or "
                 "fileprivate constituent");
  }
  return ti;
}

/** @brief Main declaration dispatcher — resolves type for any declaration AST node. */
TypeInfo *resolve_node_decl(SemaContext *ctx, ASTNode *node) {
  switch (node->kind) {
  case AST_VAR_DECL:
  case AST_LET_DECL:
    return resolve_var_decl(ctx, node);
  case AST_PARAM: {
    const char *iname = tok_intern(ctx, node->data.var.name_tok);
    const ASTNode *tann = find_type_child(node);
    TypeInfo *ti = resolve_type_annotation(ctx, tann);
    node->type = ti;
    Symbol *sym = sema_lookup(ctx, iname);
    if (sym && !sym->type)
      sym->type = ti;
    else if (!sym)
      sema_define(ctx, iname, SYM_PARAM, ti, node);
    return ti;
  }
  case AST_FUNC_DECL:
    return resolve_func_decl(ctx, node);
  case AST_INIT_DECL: {
    int phase1 = 0;
    const ASTNode *enclosing = node->parent;
    if (enclosing && enclosing->kind == AST_BLOCK && enclosing->parent &&
        enclosing->parent->kind == AST_CLASS_DECL &&
        class_decl_has_superclass(enclosing->parent)) {
      ctx->in_class_init_phase1 = 1;
      phase1 = 1;
    }
    resolve_children(ctx, node);
    if (ctx->init_class_decl) {
      const ASTNode *super =
          class_superclass_decl(ctx, (const ASTNode *)ctx->init_class_decl);
      if (super &&
          superclass_has_required_init_with_param_count(
              ctx, super, init_param_count(node)) &&
          !(node->modifiers & MOD_REQUIRED))
        sema_error(ctx, node,
                   "subclass implementation of required initializer must be "
                   "marked 'required'");
    }
    if (enclosing && enclosing->kind == AST_BLOCK && enclosing->parent) {
      const ASTNode *type_decl = enclosing->parent;
      if (type_decl->kind == AST_STRUCT_DECL ||
          type_decl->kind == AST_CLASS_DECL ||
          type_decl->kind == AST_ENUM_DECL) {
        uint32_t acc_mask = (MOD_PUBLIC | MOD_PRIVATE | MOD_INTERNAL |
                             MOD_FILEPRIVATE | MOD_PACKAGE);
        uint32_t type_acc = type_decl->modifiers & acc_mask;
        uint32_t init_acc = node->modifiers & acc_mask;
        if (!type_acc)
          type_acc = MOD_INTERNAL;
        if (!init_acc)
          init_acc = MOD_INTERNAL;
        if (access_rank(init_acc) > access_rank(type_acc))
          sema_error(ctx, node,
                     "initializer must not be more visible than the type it "
                     "initializes");
        if (type_decl->kind == AST_STRUCT_DECL) {
          const char *dummy[16];
          uint32_t stored_count =
              class_stored_property_names(ctx, type_decl, dummy, 16);
          if (init_param_count(node) == stored_count && stored_count > 0) {
            const ASTNode *body = class_decl_body(type_decl);
            uint32_t min_prop_acc = MOD_PUBLIC;
            if (body) {
              for (const ASTNode *m = body->first_child; m;
                   m = m->next_sibling) {
                if (m->kind != AST_VAR_DECL && m->kind != AST_LET_DECL)
                  continue;
                if (m->modifiers & MOD_STATIC)
                  continue;
                if (m->data.var.is_computed)
                  continue;
                uint32_t ma = m->modifiers & acc_mask;
                if (!ma)
                  ma = MOD_INTERNAL;
                if (access_rank(ma) < access_rank(min_prop_acc))
                  min_prop_acc = ma;
              }
            }
            if (access_rank(init_acc) > access_rank(min_prop_acc))
              sema_error(ctx, node,
                         "memberwise initializer must not be more visible than "
                         "the least visible stored property");
          }
        }
      }
    }
    if (phase1)
      ctx->in_class_init_phase1 = 0;
    return (node->type = TY_BUILTIN_VOID);
  }
  case AST_DEINIT_DECL: {
    for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
      if (c->kind == AST_BLOCK)
        resolve_node(ctx, (ASTNode *)c);
    return (node->type = TY_BUILTIN_VOID);
  }
  case AST_STRUCT_DECL:
  case AST_ENUM_DECL:
  case AST_PROTOCOL_DECL: {
    if (node->kind != AST_PROTOCOL_DECL)
      apply_preceding_main_actor(ctx, node);
    sema_push_scope(ctx);
    if (node->kind != AST_PROTOCOL_DECL)
      apply_default_member_access(node);
    else
      apply_protocol_requirement_access(node);
    const char *saved_type = ctx->current_type_name;
    if (node->kind != AST_PROTOCOL_DECL) {
      const char *nominal = nominal_type_name(ctx, node);
      ctx->current_type_name =
          (node->type && node->type->kind == TY_NAMED && node->type->named.name)
              ? node->type->named.name
              : nominal;
      define_nested_types_in_scope(ctx, node);
    }
    resolve_children(ctx, node);
    ctx->current_type_name = saved_type;
    if (node->kind == AST_ENUM_DECL)
      check_enum_case_values_access(ctx, node);
    if (node->kind == AST_PROTOCOL_DECL)
      check_protocol_inheritance_access(ctx, node);
    sema_pop_scope(ctx);
    return NULL;
  }
  case AST_CLASS_DECL: {
    apply_preceding_main_actor(ctx, node);
    sema_push_scope(ctx);
    apply_default_member_access(node);
    const char *saved_type = ctx->current_type_name;
    {
      const char *nominal = nominal_type_name(ctx, node);
      ctx->current_type_name =
          (node->type && node->type->kind == TY_NAMED && node->type->named.name)
              ? node->type->named.name
              : nominal;
    }
    define_nested_types_in_scope(ctx, node);
    int has_super = class_decl_has_superclass(node);
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      if (c->kind == AST_INIT_DECL) {
        ctx->init_class_decl = node;
        ctx->init_is_convenience = (c->modifiers & MOD_CONVENIENCE) ? 1 : 0;
        ctx->init_has_delegated = 0;
        ctx->init_own_prop_count =
            class_stored_property_names(ctx, node, ctx->init_own_props, 16);
        memset(ctx->init_own_assigned, 0, sizeof(ctx->init_own_assigned));
        if (has_super && !ctx->init_is_convenience)
          ctx->in_class_init_phase1 = 1;
        resolve_node(ctx, c);
        ctx->in_class_init_phase1 = 0;
        ctx->init_class_decl = NULL;
      } else {
        resolve_node(ctx, c);
      }
    }
    if (has_super) {
      const ASTNode *super = class_superclass_decl(ctx, (const ASTNode *)node);
      if (super) {
        uint32_t required[16];
        uint32_t nr =
            class_required_init_param_counts(ctx, super, required, 16);
        for (uint32_t i = 0; i < nr; i++)
          if (!class_has_init_with_param_count(ctx, (const ASTNode *)node,
                                               required[i]))
            sema_error(ctx, node,
                       "subclass must implement or inherit required "
                       "initializer (argument count %u)",
                       (unsigned)required[i]);
      }
    }
    ctx->current_type_name = saved_type;
    sema_pop_scope(ctx);
    return NULL;
  }
  case AST_EXTENSION_DECL: {
    apply_extension_member_access(node);
    const char *saved_type = ctx->current_type_name;
    if (node->data.var.name_tok)
      ctx->current_type_name = tok_intern(ctx, node->data.var.name_tok);
    if (ctx->conformance_table && node->data.var.name_tok) {
      const char *ext_name = tok_intern(ctx, node->data.var.name_tok);
      const ASTNode *where_node = NULL;
      for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
        if (c->kind == AST_WHERE_CLAUSE) {
          where_node = c;
          break;
        }
      }
      for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
        if (c->kind != AST_CONFORMANCE)
          continue;
        for (const ASTNode *proto = c->first_child; proto;
             proto = proto->next_sibling) {
          if (proto->tok_idx == 0)
            continue;
          const Token *pt = &ctx->tokens[proto->tok_idx];
          const char *pname =
              sema_intern(ctx, ctx->src->data + pt->pos, pt->len);
          if (where_node)
            conformance_table_add_conditional(ctx->conformance_table, ext_name,
                                              pname, where_node);
          else
            conformance_table_add(ctx->conformance_table, ext_name, pname);
        }
      }
    }
    sema_push_scope(ctx);
    if (node->data.var.name_tok) {
      const char *ext_name = tok_intern(ctx, node->data.var.name_tok);
      Symbol *sym = sema_lookup(ctx, ext_name);
      if (sym && sym->type && sym->type->kind == TY_NAMED &&
          sym->type->named.decl) {
        const ASTNode *ext_decl = (const ASTNode *)sym->type->named.decl;
        if (ext_decl->kind == AST_STRUCT_DECL ||
            ext_decl->kind == AST_CLASS_DECL ||
            ext_decl->kind == AST_ENUM_DECL) {
          for (ASTNode *c = (ASTNode *)ext_decl->first_child; c;
               c = (ASTNode *)c->next_sibling) {
            if (c->kind != AST_GENERIC_PARAM)
              continue;
            const char *pname = tok_intern(ctx, c->tok_idx);
            TypeInfo *gp_ti = type_arena_alloc(ctx->type_arena);
            gp_ti->kind = TY_GENERIC_PARAM;
            gp_ti->param.name = pname;
            gp_ti->param.index = 0;
            gp_ti->param.constraints = NULL;
            gp_ti->param.constraint_count = 0;
            c->type = gp_ti;
            sema_define(ctx, pname, SYM_TYPE, gp_ti, c);
          }
        }
      }
    }
    resolve_children(ctx, node);
    if (node->data.var.name_tok) {
      const char *ext_name = tok_intern(ctx, node->data.var.name_tok);
      Symbol *sym = sema_lookup(ctx, ext_name);
      int extended_is_class =
          sym && sym->type && sym->type->kind == TY_NAMED &&
          sym->type->named.decl &&
          ((const ASTNode *)sym->type->named.decl)->kind == AST_CLASS_DECL;
      if (extended_is_class) {
        for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
          if (c->kind != AST_BLOCK)
            continue;
          for (const ASTNode *m = c->first_child; m; m = m->next_sibling) {
            if (m->kind == AST_INIT_DECL && !(m->modifiers & MOD_CONVENIENCE)) {
              sema_error(ctx, (ASTNode *)m,
                         "initializer in extension of class must be marked "
                         "'convenience'");
              break;
            }
          }
          break;
        }
      }
    }
    ctx->current_type_name = saved_type;
    sema_pop_scope(ctx);
    return NULL;
  }
  case AST_ACTOR_DECL: {
    sema_push_scope(ctx);
    const char *aname = tok_intern(ctx, node->data.var.name_tok);
    const char *self_name = sema_intern(ctx, "self", 4);
    TypeInfo *self_ty = type_arena_alloc(ctx->type_arena);
    self_ty->kind = TY_NAMED;
    self_ty->named.name = aname;
    self_ty->named.decl = node;
    sema_define(ctx, self_name, SYM_VAR, self_ty, node);
    const ASTNode *body = NULL;
    for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
      if (c->kind == AST_BLOCK) {
        body = c;
        break;
      }
    if (body) {
      for (ASTNode *m = (ASTNode *)body->first_child; m;
           m = (ASTNode *)m->next_sibling) {
        if ((m->kind == AST_FUNC_DECL) && !(m->modifiers & MOD_NONISOLATED))
          m->modifiers |= MOD_ASYNC;
        resolve_node(ctx, m);
      }
      for (const ASTNode *m = body->first_child; m; m = m->next_sibling) {
        if (m->kind == AST_VAR_DECL || m->kind == AST_LET_DECL)
          resolve_node(ctx, (ASTNode *)m);
      }
    }
    sema_pop_scope(ctx);
    return (node->type = self_ty);
  }
  case AST_SUBSCRIPT_DECL: {
    sema_push_scope(ctx);
    for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
      if (c->kind != AST_GENERIC_PARAM)
        continue;
      const char *pname = tok_intern(ctx, c->tok_idx);
      TypeInfo *gp_ti = type_arena_alloc(ctx->type_arena);
      gp_ti->kind = TY_GENERIC_PARAM;
      gp_ti->param.name = pname;
      gp_ti->param.index = 0;
      gp_ti->param.constraints = NULL;
      gp_ti->param.constraint_count = 0;
      c->type = gp_ti;
      sema_define(ctx, pname, SYM_TYPE, gp_ti, c);
    }
    resolve_children(ctx, node);
    sema_pop_scope(ctx);
    return NULL;
  }
  case AST_BLOCK: {
    sema_push_scope(ctx);
    resolve_children(ctx, node);
    sema_pop_scope(ctx);
    return NULL;
  }
  default:
    return NULL;
  }
}
