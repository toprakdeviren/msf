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

/** @brief Returns 1 if the declaration is an extension member, not a local. */
int decl_is_inside_extension(const ASTNode *node) {
  for (const ASTNode *p = node ? node->parent : NULL; p; p = p->parent) {
    if (p->kind == AST_EXTENSION_DECL)
      return 1;
    if (p->kind == AST_BLOCK)
      return p->parent && p->parent->kind == AST_EXTENSION_DECL;
    if (p->kind == AST_FUNC_DECL || p->kind == AST_INIT_DECL ||
        p->kind == AST_SUBSCRIPT_DECL || p->kind == AST_ACCESSOR_DECL ||
        p->kind == AST_CLOSURE_EXPR || p->kind == AST_STRUCT_DECL ||
        p->kind == AST_CLASS_DECL ||
        p->kind == AST_ENUM_DECL || p->kind == AST_ACTOR_DECL)
      return 0;
  }
  return 0;
}

static void define_generic_params_in_current_scope(SemaContext *ctx,
                                                   ASTNode *node) {
  for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_GENERIC_PARAM)
      continue;
    TypeInfo *gp_ti = resolve_node(ctx, c);
    if (!gp_ti)
      continue;
    const char *pname = tok_intern(ctx, c->tok_idx);
    sema_define(ctx, pname, SYM_TYPE, gp_ti, c);
  }
}

static TypeInfo *resolve_typealias_rhs_with_generics(SemaContext *ctx,
                                                     ASTNode *node) {
  sema_push_scope(ctx);
  define_generic_params_in_current_scope(ctx, node);
  TypeInfo *aliased = resolve_type_annotation(ctx, find_type_child(node));
  sema_pop_scope(ctx);
  return aliased;
}

static void define_protocol_associated_types_in_scope(SemaContext *ctx,
                                                      ASTNode *proto) {
  const ASTNode *body = class_decl_body(proto);
  if (!body)
    return;
  for (ASTNode *req = (ASTNode *)body->first_child; req;
       req = (ASTNode *)req->next_sibling) {
    if (req->kind != AST_PROTOCOL_REQ ||
        !(req->modifiers & MOD_PROTOCOL_ASSOC_TYPE) || !req->tok_idx)
      continue;
    const char *name = tok_intern(ctx, req->tok_idx);
    TypeInfo *ti = req->type;
    if (!ti) {
      ti = type_arena_alloc(ctx->type_arena);
      if (!ti)
        continue;
      ti->kind = TY_GENERIC_PARAM;
      ti->param.name = name;
      ti->param.index = 0;
      ti->param.constraints = NULL;
      ti->param.constraint_count = 0;
      req->type = ti;
    }
    sema_define(ctx, name, SYM_TYPE, ti, req);
  }
}

/**
 * @brief Resolves a var/let declaration: @propertyWrapper, type annotation,
 *        init inference, computed property bodies, and access checks.
 */
TypeInfo *resolve_var_decl(SemaContext *ctx, ASTNode *node) {
  const char *iname = tok_intern(ctx, node->data.var.name_tok);

  /* Extensions may not introduce instance storage. Static stored properties
   * live on the type itself and are valid Swift. */
  if (decl_is_inside_extension(node) && !node->data.var.is_computed &&
      !(node->modifiers & MOD_STATIC)) {
    sema_error(ctx, node, "extensions must not contain stored properties");
    return node->type ? node->type : TY_BUILTIN_INT;
  }

  /* Detect @WrapperType attribute on this var.  Children-attached first
   * (new structural form), then preceding-sibling fallback. */
  if (!node->data.var.has_wrapper) {
    const ASTNode *attr = NULL;
    for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
      if (c->kind == AST_ATTRIBUTE) { attr = c; break; }
    }
    if (!attr && node->parent) {
      for (const ASTNode *sib = node->parent->first_child; sib;
           sib = sib->next_sibling) {
        if (sib->next_sibling != node) continue;
        if (sib->kind == AST_ATTRIBUTE) attr = sib;
        break;
      }
    }
    if (attr) {
      const Token *at = &ctx->tokens[attr->data.var.name_tok];
      const char *attr_name =
          sema_intern(ctx, ctx->src->data + at->pos, at->len);
      for (uint32_t wi = 0; wi < ctx->wrapper_count; wi++) {
        if (ctx->wrapper_types[wi].name == attr_name) {
          ((ASTNode *)node)->data.var.has_wrapper = 1;
          ((ASTNode *)node)->data.var.wrapper_type_tok = attr->data.var.name_tok;
          break;
        }
      }
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

  /* Init expression type (inference).  Push the annotation as the contextual
   * type so a literal initializer adopts it (`let x: CGFloat = 5` → 5 : CGFloat,
   * generalizing the Float-only narrowing below to every ExpressibleBy target). */
  const ASTNode *init = find_init_child(node);

  TypeInfo *saved_expected = ctx->expected_type;
  ctx->expected_type = annot_t;
  TypeInfo *init_t = resolve_node(ctx, (ASTNode *)init);
  ctx->expected_type = saved_expected;

  /* Float literal inference:
   * In Swift, a float literal like 3.14 defaults to Double.
   * If the variable has an explicit Float annotation, the literal
   * should be narrowed to Float (f32). The same pushdown applies to
   * each element of an array literal (`let arr: [Float] = [1.0, 2.0]`)
   * and to each value of a dict literal (`let d: [K: Float] = [..: 1.0]`),
   * so the IR sees Float at every leaf instead of a [Double] expression
   * implicitly converted to a [Float] variable. */
  if (annot_t && annot_t == TY_BUILTIN_FLOAT && init &&
      init->kind == AST_FLOAT_LITERAL) {
    ((ASTNode *)init)->type = TY_BUILTIN_FLOAT;
    init_t = TY_BUILTIN_FLOAT;
  }
  if (annot_t && init && annot_t->kind == TY_ARRAY &&
      annot_t->inner == TY_BUILTIN_FLOAT &&
      init->kind == AST_ARRAY_LITERAL && init->first_child) {
    for (ASTNode *c = init->first_child; c; c = c->next_sibling)
      if (c->kind == AST_FLOAT_LITERAL)
        c->type = TY_BUILTIN_FLOAT;
    ((ASTNode *)init)->type = annot_t;
    init_t = annot_t;
  }
  if (annot_t && init && annot_t->kind == TY_DICT &&
      annot_t->dict.value == TY_BUILTIN_FLOAT &&
      init->kind == AST_DICT_LITERAL && init->first_child) {
    /* Children alternate key, value, key, value... narrow value slots. */
    int is_key = 1;
    for (ASTNode *c = init->first_child; c; c = c->next_sibling) {
      if (!is_key && c->kind == AST_FLOAT_LITERAL)
        c->type = TY_BUILTIN_FLOAT;
      is_key = !is_key;
    }
    ((ASTNode *)init)->type = annot_t;
    init_t = annot_t;
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

  /* Contextual `[Any]` annotation on a non-empty array
   * literal — propagate the annotation type so the IR emitter sees [Any]
   * and boxes each element via __any_box_*. Per-element types are
   * preserved so collections.c picks the right box helper. */
  if (annot_t && init && init->first_child &&
      init->kind == AST_ARRAY_LITERAL &&
      annot_t->kind == TY_ARRAY && annot_t->inner &&
      (type_is_any(annot_t->inner) || type_is_anyobject(annot_t->inner))) {
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

  /* Wrapper-backed: synthesise / type the `$x` projection symbol. The symbol
   * was inserted in pass 1 (declare.c:detect_wrapper_usage) when the wrapper
   * exposes projectedValue; here we resolve and bind its type. */
  if (node->data.var.has_wrapper && node->data.var.wrapper_type_tok) {
    const Token *wt = &ctx->tokens[node->data.var.wrapper_type_tok];
    const char *wname = sema_intern(ctx, ctx->src->data + wt->pos, wt->len);
    for (uint32_t wi = 0; wi < ctx->wrapper_count; wi++) {
      if (ctx->wrapper_types[wi].name != wname) continue;
      const ASTNode *pv = ctx->wrapper_types[wi].projected_value_node;
      if (!pv) break;
      /* The projectedValue's type may not have been resolved yet — force it. */
      if (!pv->type)
        resolve_node(ctx, (ASTNode *)pv);
      if (!pv->type) break;
      char dollar_buf[64];
      const Token *nt = &ctx->tokens[node->data.var.name_tok];
      if (nt->len + 1 >= sizeof(dollar_buf)) break;
      dollar_buf[0] = '$';
      memcpy(dollar_buf + 1, ctx->src->data + nt->pos, nt->len);
      dollar_buf[nt->len + 1] = '\0';
      const char *dollar_name = sema_intern(ctx, dollar_buf, nt->len + 1);
      Symbol *dsym = sema_lookup(ctx, dollar_name);
      if (dsym && !dsym->type) {
        dsym->type = pv->type;
      } else if (!dsym) {
        sema_define(ctx, dollar_name, SYM_VAR, pv->type, (ASTNode *)node);
      }
      break;
    }
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
  if (decl->kind == AST_PROTOCOL_DECL)
    return decl;
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

  for (const ASTNode *c = body->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_TYPEALIAS_DECL || !c->data.var.name_tok)
      continue;
    const char *name = tok_intern(ctx, c->data.var.name_tok);
    TypeInfo *aliased =
        resolve_typealias_rhs_with_generics(ctx, (ASTNode *)c);
    ((ASTNode *)c)->type = aliased;
    sema_define(ctx, name, SYM_TYPEALIAS, aliased, (ASTNode *)c);
  }
}

static int current_scope_has_symbol(SemaContext *ctx, const char *name) {
  if (!ctx || !ctx->current_scope || !name)
    return 0;
  for (uint32_t i = 0; i < SCOPE_HASH_SIZE; i++)
    for (Symbol *sym = ctx->current_scope->buckets[i]; sym; sym = sym->next)
      if (sym->name == name)
        return 1;
  return 0;
}

static SymbolKind nominal_symbol_kind(const ASTNode *node) {
  switch (node ? node->kind : AST_UNKNOWN) {
  case AST_CLASS_DECL: return SYM_CLASS;
  case AST_ENUM_DECL: return SYM_ENUM;
  case AST_PROTOCOL_DECL: return SYM_PROTOCOL;
  default: return SYM_STRUCT;
  }
}

static void define_nominal_type_in_scope(SemaContext *ctx, ASTNode *node,
                                         const char *prefix) {
  if (!node || !node->data.var.name_tok)
    return;
  if (node->kind != AST_STRUCT_DECL && node->kind != AST_CLASS_DECL &&
      node->kind != AST_ENUM_DECL && node->kind != AST_PROTOCOL_DECL)
    return;

  const char *short_name = tok_intern(ctx, node->data.var.name_tok);
  if (!short_name || current_scope_has_symbol(ctx, short_name))
    return;

  TypeInfo *ti = node->type;
  if (!ti) {
    ti = type_arena_alloc(ctx->type_arena);
    if (!ti)
      return;
    ti->kind = TY_NAMED;
    if (prefix) {
      char qbuf[256];
      int n = snprintf(qbuf, sizeof(qbuf), "%s.%s", prefix, short_name);
      ti->named.name = (n > 0 && (size_t)n < sizeof(qbuf))
                           ? sema_intern(ctx, qbuf, (size_t)n)
                           : short_name;
    } else {
      ti->named.name = short_name;
    }
    ti->named.decl = node;
    node->type = ti;
  }

  sema_define(ctx, short_name, nominal_symbol_kind(node), ti, node);
}

static void define_typealias_in_scope(SemaContext *ctx, ASTNode *node) {
  if (!node || node->kind != AST_TYPEALIAS_DECL || !node->data.var.name_tok)
    return;
  const char *name = tok_intern(ctx, node->data.var.name_tok);
  if (!name || current_scope_has_symbol(ctx, name))
    return;
  TypeInfo *aliased = resolve_typealias_rhs_with_generics(ctx, node);
  node->type = aliased;
  sema_define(ctx, name, SYM_TYPEALIAS, aliased, node);
}

static void define_member_types_from_body(SemaContext *ctx, const ASTNode *decl,
                                          const char *prefix) {
  const ASTNode *body = class_decl_body(decl);
  if (!body)
    return;
  for (ASTNode *m = (ASTNode *)body->first_child; m;
       m = (ASTNode *)m->next_sibling) {
    define_nominal_type_in_scope(ctx, m, prefix);
    define_typealias_in_scope(ctx, m);
  }
}

/** @brief Well-known generic parameter names for stdlib generic types, which
 *  have no AST decl in user code (they come from the SDK vocab), so the
 *  decl-based extension binding can't find them.  Lets
 *  `extension Optional { … Wrapped … }` resolve the parameter instead of
 *  reporting `use of undeclared type 'Wrapped'`.  Returns the count (0–2). */
static int stdlib_generic_param_names(const char *type_name, const char *out[2]) {
  if (!type_name) return 0;
  if (!strcmp(type_name, "Optional")) { out[0] = "Wrapped"; return 1; }
  if (!strcmp(type_name, "Result")) {
    out[0] = "Success"; out[1] = "Failure"; return 2;
  }
  if (!strcmp(type_name, "Dictionary")) {
    out[0] = "Key"; out[1] = "Value"; return 2;
  }
  if (!strcmp(type_name, "Array") || !strcmp(type_name, "ContiguousArray") ||
      !strcmp(type_name, "ArraySlice") || !strcmp(type_name, "Set")) {
    out[0] = "Element"; return 1;
  }
  if (!strcmp(type_name, "Range") || !strcmp(type_name, "ClosedRange") ||
      !strcmp(type_name, "PartialRangeFrom") ||
      !strcmp(type_name, "PartialRangeUpTo") ||
      !strcmp(type_name, "PartialRangeThrough")) {
    out[0] = "Bound"; return 1;
  }
  return 0;
}

static void define_extension_member_types_in_scope(SemaContext *ctx,
                                                   ASTNode *extension_decl) {
  if (!ctx || !extension_decl || !extension_decl->data.var.name_tok)
    return;
  const char *target = tok_intern(ctx, extension_decl->data.var.name_tok);
  if (!target || !ctx->ast_root)
    return;

  Symbol *target_sym = sema_lookup(ctx, target);
  if (target_sym && target_sym->type && target_sym->type->kind == TY_NAMED &&
      target_sym->type->named.decl) {
    /* The target type may be declared in a DIFFERENT file of the module than
     * this extension.  define_member_types_from_body reads each nested type's
     * name token via tok_intern, which indexes the CURRENT file's tokens — so
     * without switching to the target's origin file, a nested `enum X` from
     * another file is read against this file's tokens and gets the wrong name
     * (then `X` stays "use of undeclared type" in the extension).  Enter the
     * target decl's origin so its tokens resolve correctly. */
    const ASTNode *tdecl = (const ASTNode *)target_sym->type->named.decl;
    SemaOriginState ost;
    int sw = sema_origin_enter(ctx, tdecl, &ost);
    define_member_types_from_body(ctx, tdecl, target);
    if (sw)
      sema_origin_leave(ctx, &ost);
  }

  for (ASTNode *ext = (ASTNode *)ctx->ast_root->first_child; ext;
       ext = (ASTNode *)ext->next_sibling) {
    if (ext->kind != AST_EXTENSION_DECL || !ext->data.var.name_tok)
      continue;
    const char *ext_target = tok_intern(ctx, ext->data.var.name_tok);
    if (ext_target != target)
      continue;
    define_member_types_from_body(ctx, ext, target);
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
/* Resolve the immediate superclass decl (one step up), no cycle handling. */
static const ASTNode *immediate_superclass_decl(SemaContext *ctx,
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

const ASTNode *class_superclass_decl(SemaContext *ctx,
                                     const ASTNode *class_decl) {
  const ASTNode *super = immediate_superclass_decl(ctx, class_decl);
  if (!super)
    return NULL;
  /* Inheritance-cycle guard: a class that transitively inherits from itself
   * (`class A : C; class C : B; class B : A`) has no *valid* superclass — the
   * cycle is a separate diagnostic. Without this, every superclass-chain
   * walker (init lookup, inherited-stored-property, …) recurses infinitely
   * and the frontend stack-overflows. Walk up from `super`; if the chain
   * revisits `class_decl` (or any node), break it by reporting no superclass. */
  const ASTNode *seen[128];
  int ns = 0;
  seen[ns++] = class_decl;
  for (const ASTNode *walk = super; walk;
       walk = immediate_superclass_decl(ctx, walk)) {
    for (int i = 0; i < ns; i++)
      if (seen[i] == walk)
        return NULL; /* cycle */
    if (ns >= (int)(sizeof(seen) / sizeof(seen[0])))
      return NULL; /* pathologically deep — bail rather than risk overflow */
    seen[ns++] = walk;
  }
  return super;
}

int class_has_unresolved_superclass(SemaContext *ctx, const ASTNode *class_decl) {
  if (!class_decl || class_decl->kind != AST_CLASS_DECL)
    return 0;
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
    return 0;
  TypeInfo *super_t =
      resolve_type_annotation(ctx, (ASTNode *)conf->first_child);
  if (super_t && super_t->kind == TY_NAMED) {
    /* If the superclass symbol lookup has no decl, it is unresolved */
    if (!super_t->named.decl) {
      return 1;
    }
    /* Recursively check superclass */
    const ASTNode *super_decl = (const ASTNode *)super_t->named.decl;
    if (super_decl->kind == AST_CLASS_DECL) {
      return class_has_unresolved_superclass(ctx, super_decl);
    }
  }
  return 0;
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
/* The class's inheritance clause leads with a named type we can't resolve to a
 * local declaration — by Swift convention the superclass comes first, so this is
 * very likely an SDK/base class (NSView, NSObject, …) whose initializers are
 * invisible to us.  Used to stay lenient instead of flagging a missing init. */
static int class_inherits_unresolved(SemaContext *ctx, const ASTNode *class_decl) {
  if (!class_decl || class_decl->kind != AST_CLASS_DECL) return 0;
  const ASTNode *conf = NULL;
  for (const ASTNode *c = class_decl->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_BLOCK) break;
    if (c->kind == AST_CONFORMANCE) { conf = c; break; }
  }
  if (!conf || !conf->first_child) return 0;
  TypeInfo *t = resolve_type_annotation(ctx, (ASTNode *)conf->first_child);
  return t && t->kind == TY_NAMED && !t->named.decl;
}

int class_has_init_with_param_count(SemaContext *ctx, const ASTNode *class_decl,
                                    uint32_t param_count) {
  uint32_t designated[16], convenience[16];
  uint32_t nd =
      class_designated_init_param_counts(ctx, class_decl, designated, 16);
  uint32_t nc =
      class_convenience_init_param_counts(ctx, class_decl, convenience, 16);
  /* `>=` not `==`: an init with more parameters than arguments is callable when
   * the surplus have default values (`init(a:, b: Int = 0)` called as `T(a:)`).
   * We don't track which params have defaults, so assume they might rather than
   * flag a false "no matching initializer". */
  for (uint32_t i = 0; i < nd; i++)
    if (designated[i] >= param_count)
      return 1;
  for (uint32_t i = 0; i < nc; i++)
    if (convenience[i] >= param_count)
      return 1;
  const ASTNode *super = class_superclass_decl(ctx, class_decl);
  if (!super) {
    /* No superclass: if no explicit inits defined, class gets implicit
     * default zero-arg init (Swift synthesizes init() when all stored
     * properties have defaults). */
    if (nd == 0 && nc == 0 && param_count == 0)
      return 1;
    /* Declares a superclass we can't resolve (SDK/base class): its inits are
     * invisible, so don't claim none matches. */
    if (class_inherits_unresolved(ctx, class_decl))
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
      if (super_designated[i] >= param_count)
        return 1;
  /* If subclass has no explicit inits and we're looking for zero-arg init,
   * check if superclass has one (explicit or implicit, recursively). */
  if (inherit_all_designated && nc == 0 && param_count == 0 && sd == 0)
    if (class_has_init_with_param_count(ctx, super, 0))
      return 1;
  if (implements_all_designated)
    for (uint32_t i = 0; i < sc; i++)
      if (super_convenience[i] >= param_count)
        return 1;
  return 0;
}

/* Apply attached @MainActor attribute to func/struct/class decl.  Checks
 * children first (new structural attachment), then preceding sibling for
 * backward compatibility. */
void apply_preceding_main_actor(SemaContext *ctx, ASTNode *node) {
  if (!node) return;
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_ATTRIBUTE) continue;
    /* tok_intern bounds-checks tok_idx; a foreign file's attribute node (whole-
     * module) would otherwise index this file's tokens/source out of bounds. */
    const char *attr_name = tok_intern(ctx, c->data.var.name_tok);
    if (attr_name && strcmp(attr_name, SW_ATTR_MAIN_ACTOR) == 0) {
      node->modifiers |= MOD_MAIN_ACTOR;
      return;
    }
  }
  if (!node->parent) return;
  for (const ASTNode *sib = node->parent->first_child; sib;
       sib = sib->next_sibling) {
    if (sib->next_sibling != node) continue;
    if (sib->kind != AST_ATTRIBUTE) break;
    const char *attr_name = tok_intern(ctx, sib->data.var.name_tok);
    if (attr_name && strcmp(attr_name, SW_ATTR_MAIN_ACTOR) == 0)
      node->modifiers |= MOD_MAIN_ACTOR;
    break;
  }
}

/* Well-known Apple-framework protocols / base classes whose conformers are
 * implicitly @MainActor-isolated: SwiftUI view/scene construction, UIKit/AppKit
 * UI objects. General framework knowledge (like the literal-protocol implication
 * table), not a per-project list. */
static int name_is_main_actor_anchor(const char *n) {
  if (!n) return 0;
  static const char *const anchors[] = {
      /* SwiftUI */
      "View", "App", "Scene", "ViewModifier", "Commands", "ToolbarContent",
      "CustomizableToolbarContent", "WidgetConfiguration", "DynamicProperty",
      "PreviewProvider", "TableRowContent", "UIViewRepresentable",
      "UIViewControllerRepresentable", "NSViewRepresentable",
      "NSViewControllerRepresentable", "WKInterfaceObjectRepresentable",
      /* UIKit */
      "UIViewController", "UIView", "UIResponder", "UIControl", "UIWindow",
      "UIApplicationDelegate", "UISceneDelegate", "UIWindowSceneDelegate",
      "UICollectionViewCell", "UITableViewCell", "UITableViewHeaderFooterView",
      "UICollectionReusableView", "UIGestureRecognizer", "UIScrollView",
      /* AppKit */
      "NSViewController", "NSView", "NSResponder", "NSWindowController",
      "NSWindowDelegate", "NSApplicationDelegate", "NSWindow",
      /* WatchKit */
      "WKInterfaceController", "WKExtensionDelegate",
  };
  for (size_t i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++)
    if (strcmp(n, anchors[i]) == 0)
      return 1;
  return 0;
}

static int type_decl_main_actor_rec(SemaContext *ctx, const ASTNode *td,
                                    int depth) {
  if (!td || depth > 6)
    return 0;
  if (td->kind != AST_STRUCT_DECL && td->kind != AST_CLASS_DECL &&
      td->kind != AST_ENUM_DECL && td->kind != AST_ACTOR_DECL)
    return 0;
  if (td->modifiers & MOD_MAIN_ACTOR)
    return 1;

  /* Read this decl's own attribute/conformance tokens against ITS stream — it
   * may live in another file under whole-module analysis.  Name-only scan: no
   * resolve_type_annotation (it would emit spurious "undeclared type"
   * diagnostics from this resolution phase). */
  SemaOriginState ost;
  int sw = sema_origin_enter(ctx, td, &ost);
  int hit = 0;
  const char *super_name = NULL; /* first inheritance-clause entry (superclass) */
  for (const ASTNode *c = td->first_child; c && !hit; c = c->next_sibling) {
    if (c->kind == AST_ATTRIBUTE) {
      const char *an = tok_intern(ctx, c->data.var.name_tok);
      if (an && strcmp(an, SW_ATTR_MAIN_ACTOR) == 0)
        hit = 1;
    } else if (c->kind == AST_CONFORMANCE) {
      for (const ASTNode *p = c->first_child; p; p = p->next_sibling) {
        if (!p->tok_idx)
          continue;
        const char *pn = tok_intern(ctx, p->tok_idx);
        if (!pn)
          continue;
        if (!super_name)
          super_name = pn; /* Swift requires the superclass to be listed first */
        if (name_is_main_actor_anchor(pn)) {
          hit = 1;
          break;
        }
      }
    }
  }
  if (sw)
    sema_origin_leave(ctx, &ost);
  if (hit)
    return 1;

  /* A subclass of a main-actor base class (e.g. class Foo: MyBaseVC where
   * MyBaseVC: UIViewController) is also main-actor-isolated.  Follow the
   * superclass by NAME via the symbol table (no type-annotation resolution). */
  if (td->kind == AST_CLASS_DECL && super_name) {
    Symbol *s = sema_lookup(ctx, super_name);
    if (s && s->decl && s->decl != td && s->decl->kind == AST_CLASS_DECL)
      return type_decl_main_actor_rec(ctx, s->decl, depth + 1);
  }
  return 0;
}

int type_decl_is_main_actor_isolated(SemaContext *ctx, const ASTNode *td) {
  return type_decl_main_actor_rec(ctx, td, 0);
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
  sema_push_scope(ctx);
  define_generic_params_in_current_scope(ctx, node);
  for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind >= AST_TYPE_IDENT &&
        (c->kind <= AST_TYPE_ANY || c->kind == AST_TYPE_COMPOSITION)) {
      ret_type_node = c;
      ret_t = resolve_type_annotation(ctx, c);
      break;
    }
  }
  sema_pop_scope(ctx);
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
  /* Find the symbol that was registered for THIS decl, not the bucket
   * head — overloads share a name and we'd otherwise tag the wrong sym. */
  Symbol *overload_syms[16];
  uint32_t no = sema_lookup_overloads(ctx, fname, overload_syms, 16);
  Symbol *sym = NULL;
  for (uint32_t i = 0; i < no; i++) {
    if (overload_syms[i]->decl == node) { sym = overload_syms[i]; break; }
  }
  if (!sym)
    sym = sema_lookup(ctx, fname);
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

  /* Actor isolation: derive this function's isolation from its modifiers and
   * the enclosing type, then push it for the body. `nonisolated` opts out
   * even inside a MainActor type. */
  uint8_t saved_isolation = ctx->current_isolation_main_actor;
  const ASTNode *saved_actor = ctx->current_actor_decl;
  uint8_t saved_async = ctx->current_function_async;
  ctx->current_function_async = (node->modifiers & MOD_ASYNC) ? 1 : 0;
  if (node->modifiers & MOD_NONISOLATED) {
    ctx->current_isolation_main_actor = 0;
    ctx->current_actor_decl = NULL;
  } else {
    if (node->modifiers & MOD_MAIN_ACTOR)
      ctx->current_isolation_main_actor = 1;
    /* Inherit from enclosing type if not already set */
    const ASTNode *enclosing_type = NULL;
    const ASTNode *enclosing_ext = NULL;
    for (const ASTNode *p = node->parent; p; p = p->parent) {
      if (p->kind == AST_STRUCT_DECL || p->kind == AST_CLASS_DECL ||
          p->kind == AST_ACTOR_DECL || p->kind == AST_ENUM_DECL) {
        enclosing_type = p;
        break;
      }
      if (p->kind == AST_EXTENSION_DECL) {
        enclosing_ext = p;
        break;
      }
    }
    /* A method declared in `extension T { … }` inherits T's isolation; resolve
     * the extended type's decl (it may live in another file). */
    if (!enclosing_type && enclosing_ext && enclosing_ext->data.var.name_tok) {
      const char *en = tok_intern(ctx, enclosing_ext->data.var.name_tok);
      Symbol *es = en ? sema_lookup(ctx, en) : NULL;
      if (es && es->type && es->type->kind == TY_NAMED && es->type->named.decl)
        enclosing_type = (const ASTNode *)es->type->named.decl;
    }
    if (enclosing_type) {
      if (type_decl_is_main_actor_isolated(ctx, enclosing_type))
        ctx->current_isolation_main_actor = 1;
      if (enclosing_type->kind == AST_ACTOR_DECL)
        ctx->current_actor_decl = enclosing_type;
    }
  }

  /* Params + body */
  for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_PARAM)
      resolve_node(ctx, c);
    else if (c->kind == AST_BLOCK) {
      const BuilderEntry *be = node_get_builder(ctx, node);
      if (be) {
        ASTNode *blk_call = transform_builder_body(ctx, be, c);
        if (blk_call) {
          /* Stitch the synthesised buildBlock(...) call back into the body
           * as its sole statement, replacing whatever sequence of stmts the
           * user wrote. Wrapping in an EXPR_STMT keeps downstream walkers
           * (which expect a block of stmts) happy. */
          ASTNode *expr_stmt = ast_arena_alloc(ctx->ast_arena);
          if (expr_stmt) {
            expr_stmt->kind = AST_EXPR_STMT;
            expr_stmt->first_child = blk_call;
            expr_stmt->last_child = blk_call;
            blk_call->parent = expr_stmt;
            blk_call->next_sibling = NULL;
            expr_stmt->parent = c;
            expr_stmt->next_sibling = NULL;
            ((ASTNode *)c)->first_child = expr_stmt;
            ((ASTNode *)c)->last_child = expr_stmt;
          }
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
  ctx->current_isolation_main_actor = saved_isolation;
  ctx->current_actor_decl = saved_actor;
  ctx->current_function_async = saved_async;
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
      define_nominal_type_in_scope(ctx, node, NULL);
      define_generic_params_in_current_scope(ctx, node);
      define_nested_types_in_scope(ctx, node);
    } else {
      define_protocol_associated_types_in_scope(ctx, node);
    }
    /* Property initializers (resolved here, not inside a func) run in the
     * type's isolation domain — set it so e.g. a @MainActor / SwiftUI-View
     * type's defaults can reach main-actor members. */
    uint8_t saved_iso = ctx->current_isolation_main_actor;
    if (node->kind != AST_PROTOCOL_DECL)
      ctx->current_isolation_main_actor =
          type_decl_is_main_actor_isolated(ctx, node) ? 1 : 0;
    resolve_children(ctx, node);
    ctx->current_isolation_main_actor = saved_iso;
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
    define_nominal_type_in_scope(ctx, node, NULL);
    define_generic_params_in_current_scope(ctx, node);
    define_nested_types_in_scope(ctx, node);
    int has_super = class_decl_has_superclass(node);
    uint8_t saved_iso = ctx->current_isolation_main_actor;
    ctx->current_isolation_main_actor =
        type_decl_is_main_actor_isolated(ctx, node) ? 1 : 0;
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
    ctx->current_isolation_main_actor = saved_iso;
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
          SemaOriginState ost;
          int sw = sema_origin_enter(ctx, ext_decl, &ost);
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
          if (sw) sema_origin_leave(ctx, &ost);
        }
      }
      /* Stdlib generic types (Optional/Result/Array/Dictionary/Range/…) have no
       * AST decl, so the loop above can't bind their parameters.  Bind the
       * well-known names so `extension Optional { … Wrapped … }` resolves. */
      const char *gp_names[2] = {0, 0};
      int ngp = stdlib_generic_param_names(ext_name, gp_names);
      for (int gi = 0; gi < ngp; gi++) {
        const char *pname =
            sema_intern(ctx, gp_names[gi], strlen(gp_names[gi]));
        TypeInfo *gp_ti = type_arena_alloc(ctx->type_arena);
        gp_ti->kind = TY_GENERIC_PARAM;
        gp_ti->param.name = pname;
        gp_ti->param.index = (uint32_t)gi;
        gp_ti->param.constraints = NULL;
        gp_ti->param.constraint_count = 0;
        sema_define(ctx, pname, SYM_TYPE, gp_ti, node);
      }
    }
    define_extension_member_types_in_scope(ctx, node);
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
