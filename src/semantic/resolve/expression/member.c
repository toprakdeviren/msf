/**
 * @file member.c
 * @brief Member expression resolution: base.member, implicit .member,
 *        optional chaining, tuple indexing, associated type resolution.
 */

#include "../../private.h"

/* ── Conformance type-name presence set ──────────────────────────────────
 * The associated-type resolution scan below linear-walks the whole
 * conformance table (with strcmp) on every member access that didn't match a
 * stored/computed member.  On generated code (thousands of Codable/Hashable
 * conformances, thousands of nested-type accesses) that is O(n^2).  But if the
 * base type isn't recorded as conforming to anything, the scan can match
 * nothing — so we keep a lazy hash set of the table's type names and skip the
 * scan entirely in that (dominant) case.  Equivalent to the original loop
 * because a name absent from the table makes every strcmp branch `continue`. */
static uint32_t conf_pow2(uint32_t n) {
  uint32_t p = 16;
  while (p < n) p <<= 1;
  return p;
}

static uint32_t conf_hash(const char *s) {
  uint32_t h = 2166136261u;             /* FNV-1a */
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}

static void conf_set_insert(SemaContext *ctx, const char *tn) {
  uint32_t cap = ctx->conf_name_cap;
  uint32_t h = conf_hash(tn) & (cap - 1);
  while (ctx->conf_name_set[h]) {
    if (strcmp(ctx->conf_name_set[h], tn) == 0) return;  /* dedup */
    h = (h + 1) & (cap - 1);
  }
  ctx->conf_name_set[h] = tn;
}

static int conf_name_present(SemaContext *ctx, const char *name) {
  const ConformanceTable *ct = ctx->conformance_table;
  if (!ct || !name) return 0;
  /* Maintain the set incrementally: the conformance table only ever appends,
   * so when it grows we just add the new entries (and rehash on a real
   * capacity bump) — never a full O(count) rebuild per access. */
  if (ctx->conf_name_built < ct->count) {
    if (ctx->conf_name_cap < conf_pow2(ct->count * 2 + 1)) {
      uint32_t ncap = conf_pow2(ct->count * 2 + 1);
      const char **old = ctx->conf_name_set;
      uint32_t oldcap = ctx->conf_name_cap;
      ctx->conf_name_set = calloc(ncap, sizeof(const char *));
      if (!ctx->conf_name_set) { ctx->conf_name_set = old; return 1; }
      ctx->conf_name_cap = ncap;
      for (uint32_t i = 0; i < oldcap; i++)
        if (old[i]) conf_set_insert(ctx, old[i]);  /* rehash existing names */
      free(old);
    }
    for (uint32_t i = ctx->conf_name_built; i < ct->count; i++)
      if (ct->entries[i].type_name) conf_set_insert(ctx, ct->entries[i].type_name);
    ctx->conf_name_built = ct->count;
  }
  uint32_t cap = ctx->conf_name_cap;
  if (!cap) return 1;
  uint32_t h = conf_hash(name) & (cap - 1);
  while (ctx->conf_name_set[h]) {
    if (strcmp(ctx->conf_name_set[h], name) == 0) return 1;
    h = (h + 1) & (cap - 1);
  }
  return 0;
}

/* Walk up from a member-expression to see if it sits under an `await`
 * expression in the same function/closure scope. We bail at any function
 * boundary so an outer `await` doesn't cover an inner closure body. */
static int member_is_under_await(const ASTNode *node) {
  for (const ASTNode *p = node ? node->parent : NULL; p; p = p->parent) {
    if (p->kind == AST_AWAIT_EXPR) return 1;
    if (p->kind == AST_FUNC_DECL || p->kind == AST_CLOSURE_EXPR ||
        p->kind == AST_INIT_DECL || p->kind == AST_DEINIT_DECL)
      return 0;
  }
  return 0;
}

static TypeInfo *make_string_string_dict_member_type(SemaContext *ctx) {
  TypeInfo *t = type_arena_alloc(ctx->type_arena);
  if (!t)
    return TY_BUILTIN_INT;
  t->kind = TY_DICT;
  t->dict.key = TY_BUILTIN_STRING;
  t->dict.value = TY_BUILTIN_STRING;
  return t;
}

static int url_cache_storage_policy_code(const char *name) {
  if (!name)
    return -1;
  if (name[0] == '.')
    name++;
  if (strcmp(name, "allowed") == 0)
    return 0;
  if (strcmp(name, "allowedInMemoryOnly") == 0)
    return 1;
  if (strcmp(name, "notAllowed") == 0)
    return 2;
  return -1;
}

static int character_set_static_member_code(const char *name) {
  if (!name)
    return -1;
  if (strcmp(name, "urlQueryAllowed") == 0)
    return 0;
  if (strcmp(name, "urlPathAllowed") == 0)
    return 1;
  if (strcmp(name, "urlFragmentAllowed") == 0)
    return 2;
  if (strcmp(name, "urlHostAllowed") == 0)
    return 3;
  if (strcmp(name, "urlUserAllowed") == 0)
    return 4;
  if (strcmp(name, "urlPasswordAllowed") == 0)
    return 5;
  if (strcmp(name, "alphanumerics") == 0)
    return 6;
  if (strcmp(name, "decimalDigits") == 0)
    return 7;
  if (strcmp(name, "letters") == 0 || strcmp(name, "capitalizedLetters") == 0 ||
      strcmp(name, "nonBaseCharacters") == 0 ||
      strcmp(name, "decomposables") == 0)
    return 8;
  if (strcmp(name, "whitespaces") == 0)
    return 9;
  if (strcmp(name, "whitespacesAndNewlines") == 0)
    return 10;
  if (strcmp(name, "newlines") == 0)
    return 11;
  if (strcmp(name, "punctuationCharacters") == 0)
    return 12;
  if (strcmp(name, "symbols") == 0)
    return 13;
  if (strcmp(name, "controlCharacters") == 0)
    return 14;
  if (strcmp(name, "illegalCharacters") == 0)
    return 15;
  if (strcmp(name, "lowercaseLetters") == 0)
    return 16;
  if (strcmp(name, "uppercaseLetters") == 0)
    return 17;
  return -1;
}

static int number_formatter_style_code(const char *name) {
  if (!name)
    return -1;
  if (name[0] == '.')
    name++;
  if (strcmp(name, "none") == 0 || strcmp(name, "decimal") == 0)
    return 0;
  if (strcmp(name, "currency") == 0)
    return 1;
  if (strcmp(name, "percent") == 0)
    return 2;
  return -1;
}

static int date_formatter_style_code(const char *name) {
  if (!name)
    return -1;
  if (name[0] == '.')
    name++;
  if (strcmp(name, "none") == 0)
    return 0;
  if (strcmp(name, "short") == 0)
    return 1;
  if (strcmp(name, "medium") == 0)
    return 2;
  if (strcmp(name, "long") == 0)
    return 3;
  if (strcmp(name, "full") == 0)
    return 4;
  return -1;
}

static int iso8601_formatter_options_code(const char *name) {
  if (!name)
    return -1;
  if (name[0] == '.')
    name++;
  if (strcmp(name, "withYear") == 0)
    return 1;
  if (strcmp(name, "withMonth") == 0)
    return 2;
  if (strcmp(name, "withWeekOfYear") == 0)
    return 4;
  if (strcmp(name, "withDay") == 0)
    return 16;
  if (strcmp(name, "withTime") == 0)
    return 32;
  if (strcmp(name, "withTimeZone") == 0)
    return 64;
  if (strcmp(name, "withSpaceBetweenDateAndTime") == 0)
    return 128;
  if (strcmp(name, "withDashSeparatorInDate") == 0)
    return 256;
  if (strcmp(name, "withColonSeparatorInTime") == 0)
    return 512;
  if (strcmp(name, "withColonSeparatorInTimeZone") == 0)
    return 1024;
  if (strcmp(name, "withFractionalSeconds") == 0)
    return 2048;
  if (strcmp(name, "withFullDate") == 0)
    return 275;
  if (strcmp(name, "withFullTime") == 0)
    return 1632;
  if (strcmp(name, "withInternetDateTime") == 0)
    return 1907;
  return -1;
}

static int property_list_format_code(const char *name) {
  if (!name)
    return -1;
  if (name[0] == '.')
    name++;
  if (strcmp(name, "openStep") == 0)
    return 1;
  if (strcmp(name, "xml") == 0)
    return 2;
  if (strcmp(name, "binary") == 0)
    return 3;
  return -1;
}

static int measurement_formatter_unit_style_code(const char *name) {
  if (!name)
    return -1;
  if (name[0] == '.')
    name++;
  if (strcmp(name, "medium") == 0)
    return 0;
  if (strcmp(name, "short") == 0)
    return 1;
  if (strcmp(name, "long") == 0)
    return 2;
  return -1;
}

static int measurement_formatter_unit_options_code(const char *name) {
  if (!name)
    return -1;
  if (name[0] == '.')
    name++;
  if (strcmp(name, "providedUnit") == 0)
    return 1;
  if (strcmp(name, "naturalScale") == 0)
    return 2;
  if (strcmp(name, "temperatureWithoutUnit") == 0)
    return 4;
  return -1;
}

static int foundation_unit_static_member_code(const char *base,
                                              const char *member) {
  if (!base || !member)
    return -1;
  if (strcmp(base, "UnitLength") == 0) {
    if (strcmp(member, "meters") == 0 ||
        strcmp(member, "kilometers") == 0 ||
        strcmp(member, "centimeters") == 0 ||
        strcmp(member, "millimeters") == 0 ||
        strcmp(member, "miles") == 0 ||
        strcmp(member, "yards") == 0 ||
        strcmp(member, "feet") == 0 ||
        strcmp(member, "inches") == 0)
      return 1;
  }
  if (strcmp(base, "UnitMass") == 0) {
    if (strcmp(member, "kilograms") == 0 ||
        strcmp(member, "grams") == 0 ||
        strcmp(member, "milligrams") == 0 ||
        strcmp(member, "pounds") == 0 ||
        strcmp(member, "ounces") == 0)
      return 1;
  }
  if (strcmp(base, "UnitDuration") == 0) {
    if (strcmp(member, "seconds") == 0 ||
        strcmp(member, "minutes") == 0 ||
        strcmp(member, "hours") == 0 ||
        strcmp(member, "milliseconds") == 0 ||
        strcmp(member, "microseconds") == 0 ||
        strcmp(member, "nanoseconds") == 0 ||
        strcmp(member, "picoseconds") == 0)
      return 1;
  }
  if (strcmp(base, "UnitSpeed") == 0) {
    if (strcmp(member, "metersPerSecond") == 0 ||
        strcmp(member, "kilometersPerHour") == 0 ||
        strcmp(member, "milesPerHour") == 0 ||
        strcmp(member, "knots") == 0)
      return 1;
  }
  if (strcmp(base, "UnitTemperature") == 0) {
    if (strcmp(member, "celsius") == 0 ||
        strcmp(member, "fahrenheit") == 0 ||
        strcmp(member, "kelvin") == 0)
      return 1;
  }
  return -1;
}

static TypeInfo *make_named_character_set_type(SemaContext *ctx) {
  TypeInfo *t = type_arena_alloc(ctx->type_arena);
  if (!t)
    return TY_BUILTIN_INT;
  t->kind = TY_NAMED;
  t->named.name = "CharacterSet";
  t->named.decl = NULL;
  return t;
}

static TypeInfo *make_named_foundation_type(SemaContext *ctx,
                                            const char *name) {
  TypeInfo *t = type_arena_alloc(ctx->type_arena);
  if (!t)
    return TY_BUILTIN_INT;
  t->kind = TY_NAMED;
  t->named.name = name;
  t->named.decl = NULL;
  return t;
}

static int is_range_family_name(const char *name) {
  return name &&
         (strcmp(name, "Range") == 0 ||
          strcmp(name, "ClosedRange") == 0 ||
          strcmp(name, "PartialRangeFrom") == 0 ||
          strcmp(name, "PartialRangeThrough") == 0 ||
          strcmp(name, "PartialRangeUpTo") == 0 ||
          strcmp(name, "UnboundedRange") == 0);
}

static const char *named_type_family_name(TypeInfo *ty) {
  if (!ty)
    return NULL;
  if (ty->kind == TY_NAMED)
    return ty->named.name;
  if (ty->kind == TY_GENERIC_INST && ty->generic.base &&
      ty->generic.base->kind == TY_NAMED)
    return ty->generic.base->named.name;
  return NULL;
}

/* Diagnose cross-actor member access. Two patterns are caught:
 *
 *   - `@MainActor` type's member accessed from a non-MainActor context
 *     without `await`
 *   - any actor's member accessed from outside that actor without `await`
 *
 * `nonisolated` members are exempt. The `await` cover lets async code
 * cross the boundary explicitly. */
static void check_actor_isolation_for_member(SemaContext *ctx,
                                              const ASTNode *node,
                                              const TypeInfo *base_t,
                                              const ASTNode *member_decl) {
  if (!base_t || base_t->kind != TY_NAMED || !base_t->named.decl)
    return;
  const ASTNode *target_decl = (const ASTNode *)base_t->named.decl;

  /* Pass 2's resolve_node early-returns when a nominal type already has
   * its TypeInfo set — which is the case for struct/class/actor decls
   * that pass 1 forward-declared. The consequence: apply_preceding_main_actor
   * never fires for those decls. Apply the lazy version here so we can read
   * MOD_MAIN_ACTOR off the AST node. */
  if (!(target_decl->modifiers & MOD_MAIN_ACTOR)) {
    /* target_decl (the base type) may live in another file (whole-module), so
     * read its attribute tokens against ITS stream, not the active one. */
    SemaOriginState ost;
    int sw = sema_origin_enter(ctx, target_decl, &ost);
    /* Children-attached attribute (new structural form). */
    for (const ASTNode *c = target_decl->first_child; c; c = c->next_sibling) {
      if (c->kind != AST_ATTRIBUTE) continue;
      const char *an = tok_intern(ctx, c->data.var.name_tok);
      if (an && strcmp(an, SW_ATTR_MAIN_ACTOR) == 0) {
        ((ASTNode *)target_decl)->modifiers |= MOD_MAIN_ACTOR;
        break;
      }
    }
    /* Sibling fallback. */
    if (!(target_decl->modifiers & MOD_MAIN_ACTOR) && target_decl->parent) {
      for (const ASTNode *sib = target_decl->parent->first_child; sib;
           sib = sib->next_sibling) {
        if (sib->next_sibling != target_decl) continue;
        if (sib->kind != AST_ATTRIBUTE) break;
        const char *an = tok_intern(ctx, sib->data.var.name_tok);
        if (an && strcmp(an, SW_ATTR_MAIN_ACTOR) == 0)
          ((ASTNode *)target_decl)->modifiers |= MOD_MAIN_ACTOR;
        break;
      }
    }
    if (sw) sema_origin_leave(ctx, &ost);
  }

  /* nonisolated member is always reachable */
  if (member_decl && (member_decl->modifiers & MOD_NONISOLATED))
    return;

  /* A `static let` constant is nonisolated under SE-0412 (immutable value of a
   * Sendable type — and a @MainActor class is implicitly Sendable), so it is
   * reachable across actors without `await`. This is the ubiquitous `.shared`
   * singleton accessor. */
  if (member_decl && member_decl->kind == AST_LET_DECL &&
      (member_decl->modifiers & MOD_STATIC))
    return;

  /* Actor type — direct property access from outside the actor body
   * always crosses the actor boundary. */
  if (target_decl->kind == AST_ACTOR_DECL) {
    if (ctx->current_actor_decl == target_decl) return;
    if (member_is_under_await(node)) return;
    sema_error(ctx, (ASTNode *)node,
               "Actor-isolated member access requires 'await'");
    return;
  }

  /* @MainActor type — cross-context access without await */
  if (target_decl->modifiers & MOD_MAIN_ACTOR) {
    if (ctx->current_isolation_main_actor) return;
    if (member_is_under_await(node)) return;
    sema_error(ctx, (ASTNode *)node,
               "Main actor-isolated member access from a non-MainActor "
               "context requires 'await'");
  }
}

TypeInfo *resolve_member_expr(SemaContext *ctx, ASTNode *node) {
  ASTNode *base = node->first_child;
  const ASTNode *member = base ? base->next_sibling : NULL;
  const char *mname = member ? tok_intern(ctx, member->tok_idx)
                             : (node->data.var.name_tok
                                    ? tok_intern(ctx, node->data.var.name_tok)
                                    : NULL);
  if (base && base->kind == AST_IDENT_EXPR && mname) {
    const char *bname = tok_intern(ctx, base->tok_idx);
    if (bname && strcmp(bname, "CommandLine") == 0 &&
        strcmp(mname, "arguments") == 0) {
      TypeInfo *arr_t = type_arena_alloc(ctx->type_arena);
      arr_t->kind = TY_ARRAY;
      arr_t->inner = TY_BUILTIN_STRING;
      return (node->type = arr_t);
    }
    if (bname && strcmp(bname, "CharacterSet") == 0 &&
        character_set_static_member_code(mname) >= 0) {
      return (node->type = make_named_character_set_type(ctx));
    }
    if (bname && foundation_unit_static_member_code(bname, mname) >= 0) {
      return (node->type = make_named_foundation_type(ctx, bname));
    }
    if (bname && strcmp(bname, "TimeZone") == 0 &&
        (strcmp(mname, "current") == 0 ||
         strcmp(mname, "autoupdatingCurrent") == 0 ||
         strcmp(mname, "gmt") == 0)) {
      return (node->type = make_named_foundation_type(ctx, "TimeZone"));
    }
    if (bname && strcmp(bname, "Locale") == 0) {
      if (strcmp(mname, "current") == 0 ||
          strcmp(mname, "autoupdatingCurrent") == 0) {
        return (node->type = make_named_foundation_type(ctx, "Locale"));
      }
      if (strcmp(mname, "preferredLanguages") == 0) {
        TypeInfo *arr_t = type_arena_alloc(ctx->type_arena);
        arr_t->kind = TY_ARRAY;
        arr_t->inner = TY_BUILTIN_STRING;
        return (node->type = arr_t);
      }
      if (strcmp(mname, "canonicalIdentifier") == 0 ||
          strcmp(mname, "canonicalLanguageIdentifier") == 0) {
        return (node->type = TY_BUILTIN_STRING);
      }
    }
    if (bname && (strncmp(bname, "Unit", 4) == 0 || strcmp(bname, "Dimension") == 0)) {
      if (strcmp(mname, "baseUnit") == 0) {
        return (node->type = make_named_foundation_type(ctx, bname));
      }
    }
    if (bname && strcmp(bname, "Decimal") == 0 &&
        strcmp(mname, "zero") == 0) {
      return (node->type = make_named_foundation_type(ctx, "Decimal"));
    }
    if (bname && strcmp(bname, "NumberFormatter") == 0 &&
        strcmp(mname, "Style") == 0) {
      return (node->type =
                  make_named_foundation_type(ctx, "NumberFormatter.Style"));
    }
    if (bname && strcmp(bname, "DateFormatter") == 0 &&
        strcmp(mname, "Style") == 0) {
      return (node->type =
                  make_named_foundation_type(ctx, "DateFormatter.Style"));
    }
    if (bname && strcmp(bname, "ISO8601DateFormatter") == 0 &&
        strcmp(mname, "Options") == 0) {
      return (node->type =
                  make_named_foundation_type(ctx, "ISO8601DateFormatter.Options"));
    }
    if (bname && strcmp(bname, "PropertyListSerialization") == 0) {
      if (strcmp(mname, "PropertyListFormat") == 0)
        return (node->type =
                    make_named_foundation_type(ctx, "PropertyListSerialization.PropertyListFormat"));
      if (strcmp(mname, "data") == 0 ||
          strcmp(mname, "propertyList") == 0 ||
          strcmp(mname, "dataFromPropertyList") == 0 ||
          strcmp(mname, "propertyListFromData") == 0)
        return (node->type = TY_BUILTIN_INT);
    }
    if (bname && strcmp(bname, "FileManager") == 0 &&
        strcmp(mname, "default") == 0) {
      return (node->type = make_named_foundation_type(ctx, "FileManager"));
    }
    if (bname && strcmp(bname, "Bundle") == 0 &&
        strcmp(mname, "main") == 0) {
      return (node->type = make_named_foundation_type(ctx, "Bundle"));
    }
    if (bname && strcmp(bname, "URLCache") == 0 &&
        strcmp(mname, "StoragePolicy") == 0) {
      return (node->type =
                  make_named_foundation_type(ctx, "URLCache.StoragePolicy"));
    }
    if (bname && strcmp(bname, "URLCache.StoragePolicy") == 0 &&
        url_cache_storage_policy_code(mname) >= 0) {
      return (node->type = TY_BUILTIN_INT);
    }
    if (bname && strcmp(bname, "MeasurementFormatter") == 0 &&
        strcmp(mname, "UnitStyle") == 0) {
      return (node->type =
                  make_named_foundation_type(ctx, "MeasurementFormatter.UnitStyle"));
    }
    if (bname && strcmp(bname, "MeasurementFormatter") == 0 &&
        strcmp(mname, "UnitOptions") == 0) {
      return (node->type =
                  make_named_foundation_type(ctx, "MeasurementFormatter.UnitOptions"));
    }
  }
  if (ctx->in_class_init_phase1 && base && base->kind == AST_IDENT_EXPR) {
    const char *bname = tok_intern(ctx, base->tok_idx);
    if (bname && strcmp(bname, "self") == 0) {
      if (mname && strcmp(mname, "init") != 0) {
        int is_lhs = (node->parent && node->parent->kind == AST_ASSIGN_EXPR &&
                      node->parent->first_child == node);
        if (!is_lhs)
          sema_error(ctx, node,
                     "Cannot read instance property before 'super.init()' "
                     "(two-phase initialization)");
      }
    }
  }
  TypeInfo *base_t = base ? resolve_node(ctx, base) : NULL;
  if (!base_t && !base && mname) {
    /* Implicit member expression (.foo) — resolve from contextual type */
    TypeInfo *ctx_type = get_contextual_type_for_implicit_member(ctx, node);
    if (ctx_type) {
      if (ctx_type->kind == TY_OPTIONAL && ctx_type->inner)
        ctx_type = ctx_type->inner;
      TypeInfo *found = lookup_builtin_member(ctx, ctx_type, mname);
      if (found)
        return (node->type = found);
      if (number_formatter_style_code(mname) >= 0 &&
          type_kind_of(ctx_type) == TY_INT)
        return (node->type = TY_BUILTIN_INT);
      if ((date_formatter_style_code(mname) >= 0 ||
           iso8601_formatter_options_code(mname) >= 0) &&
          type_kind_of(ctx_type) == TY_INT)
        return (node->type = TY_BUILTIN_INT);
      if ((measurement_formatter_unit_style_code(mname) >= 0 ||
           measurement_formatter_unit_options_code(mname) >= 0) &&
          type_kind_of(ctx_type) == TY_INT)
        return (node->type = TY_BUILTIN_INT);
      if (url_cache_storage_policy_code(mname) >= 0 &&
          type_kind_of(ctx_type) == TY_INT)
        return (node->type = TY_BUILTIN_INT);
      if (ctx_type->kind == TY_TUPLE && node->data.var.name_tok) {
        const Token *mtok = &ctx->tokens[node->data.var.name_tok];
        if (mtok->type == TOK_INTEGER_LIT) {
          char ibuf[16];
          size_t ilen = mtok->len < 15 ? mtok->len : 15;
          memcpy(ibuf, ctx->src->data + mtok->pos, ilen);
          ibuf[ilen] = '\0';
          uint32_t idx = (uint32_t)atoi(ibuf);
          if (idx < ctx_type->tuple.elem_count && ctx_type->tuple.elems[idx])
            return (node->type = ctx_type->tuple.elems[idx]);
        }
      }
      if (ctx_type->kind == TY_NAMED && ctx_type->named.decl) {
        const ASTNode *decl = (const ASTNode *)ctx_type->named.decl;
        const ASTNode *body = class_decl_body(decl);
        ASTNode *m = sema_member_lookup(ctx, decl, body, mname);
        if (m) {
          if (!m->type)
            resolve_node(ctx, m);
          return (node->type = m->type);
        }
        if (decl->kind == AST_ENUM_DECL) {
          for (const ASTNode *b = decl->first_child; b; b = b->next_sibling) {
            if (b->kind != AST_BLOCK)
              continue;
            for (const ASTNode *cd = b->first_child; cd;
                 cd = cd->next_sibling) {
              if (cd->kind != AST_ENUM_CASE_DECL)
                continue;
              for (const ASTNode *el = cd->first_child; el;
                   el = el->next_sibling) {
                if (el->kind != AST_ENUM_ELEMENT_DECL)
                  continue;
                const char *el_name = tok_intern(ctx, el->data.var.name_tok);
                if (el_name && strcmp(el_name, mname) == 0)
                  return (node->type = ctx_type);
              }
            }
          }
        }
      }
    }
  }
  TypeInfo *unwrapped_base = base_t;
  if (base_t && base_t->kind == TY_OPTIONAL)
    unwrapped_base = base_t->inner;
  /* Optional chaining propagates through a member chain: in `a?.b.c`, the AST is
   * Member(Member(OptChain(a), b), c) — only the innermost access has an
   * AST_OPTIONAL_CHAIN base, yet the whole chain's result is optional. The
   * carrier of that propagation is the base's resolved TYPE being optional
   * (in real, compiling Swift a member access on an optional-typed base only
   * occurs inside such a chain — accessing `.m` on `T?` without `?`/`!` is
   * otherwise an error), so wrap the result optional in that case too. */
  int base_is_opt_chain = (base && base->kind == AST_OPTIONAL_CHAIN) ||
                          (base_t && base_t->kind == TY_OPTIONAL);

  if (unwrapped_base && unwrapped_base == TY_BUILTIN_STRING && mname &&
      strcmp(mname, "uuidString") == 0) {
    return (node->type = wrap_optional_result(TY_BUILTIN_STRING,
                                              base_is_opt_chain, ctx));
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "URLCache.StoragePolicy") == 0 &&
      url_cache_storage_policy_code(mname) >= 0) {
    return (node->type = TY_BUILTIN_INT);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "FileManager") == 0) {
    if (strcmp(mname, "currentDirectoryPath") == 0)
      return (node->type = TY_BUILTIN_STRING);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "FileHandle") == 0) {
    if (strcmp(mname, "availableData") == 0)
      return (node->type = TY_BUILTIN_DATA);
    if (strcmp(mname, "offsetInFile") == 0 ||
        strcmp(mname, "fileDescriptor") == 0)
      return (node->type = TY_BUILTIN_INT);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "Bundle") == 0) {
    if (strcmp(mname, "bundlePath") == 0 ||
        strcmp(mname, "resourcePath") == 0 ||
        strcmp(mname, "executablePath") == 0 ||
        strcmp(mname, "bundleURL") == 0 ||
        strcmp(mname, "resourceURL") == 0)
      return (node->type = TY_BUILTIN_STRING);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "URLRequest") == 0) {
    if (strcmp(mname, "url") == 0 ||
        strcmp(mname, "mainDocumentURL") == 0 ||
        strcmp(mname, "httpMethod") == 0)
      return (node->type = TY_BUILTIN_STRING);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "CachedURLResponse") == 0) {
    if (strcmp(mname, "response") == 0)
      return (node->type = make_named_foundation_type(ctx, "URLResponse"));
    if (strcmp(mname, "data") == 0)
      return (node->type = TY_BUILTIN_DATA);
    if (strcmp(mname, "userInfo") == 0)
      return (node->type = make_string_string_dict_member_type(ctx));
    if (strcmp(mname, "storagePolicy") == 0)
      return (node->type = TY_BUILTIN_INT);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      (strcmp(unwrapped_base->named.name, "URLResponse") == 0 ||
       strcmp(unwrapped_base->named.name, "HTTPURLResponse") == 0)) {
    if (strcmp(mname, "url") == 0 ||
        strcmp(mname, "mimeType") == 0 ||
        strcmp(mname, "textEncodingName") == 0 ||
        strcmp(mname, "suggestedFilename") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "expectedContentLength") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(unwrapped_base->named.name, "HTTPURLResponse") == 0) {
      if (strcmp(mname, "statusCode") == 0)
        return (node->type = TY_BUILTIN_INT);
      if (strcmp(mname, "allHeaderFields") == 0)
        return (node->type = make_string_string_dict_member_type(ctx));
    }
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "URLComponents") == 0) {
    if (strcmp(mname, "url") == 0 ||
        strcmp(mname, "string") == 0 ||
        strcmp(mname, "scheme") == 0 ||
        strcmp(mname, "user") == 0 ||
        strcmp(mname, "percentEncodedUser") == 0 ||
        strcmp(mname, "password") == 0 ||
        strcmp(mname, "percentEncodedPassword") == 0 ||
        strcmp(mname, "host") == 0 ||
        strcmp(mname, "percentEncodedHost") == 0 ||
        strcmp(mname, "encodedHost") == 0 ||
        strcmp(mname, "path") == 0 ||
        strcmp(mname, "percentEncodedPath") == 0 ||
        strcmp(mname, "query") == 0 ||
        strcmp(mname, "percentEncodedQuery") == 0 ||
        strcmp(mname, "fragment") == 0 ||
        strcmp(mname, "percentEncodedFragment") == 0 ||
        strcmp(mname, "description") == 0 ||
        strcmp(mname, "debugDescription") == 0) {
      return (node->type = TY_BUILTIN_STRING);
    }
    if (strcmp(mname, "port") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "hashValue") == 0)
      return (node->type = TY_BUILTIN_INT);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "Date") == 0) {
    if (strcmp(mname, "timeIntervalSince1970") == 0 ||
        strcmp(mname, "timeIntervalSinceNow") == 0 ||
        strcmp(mname, "timeIntervalSinceReferenceDate") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
    if (strcmp(mname, "year") == 0 ||
        strcmp(mname, "month") == 0 ||
        strcmp(mname, "day") == 0 ||
        strcmp(mname, "hour") == 0 ||
        strcmp(mname, "minute") == 0 ||
        strcmp(mname, "second") == 0 ||
        strcmp(mname, "weekday") == 0)
      return (node->type = TY_BUILTIN_INT);
  }

  if (mname && is_range_family_name(named_type_family_name(unwrapped_base))) {
    if (strcmp(mname, "contains") == 0 ||
        strcmp(mname, "overlaps") == 0)
      return (node->type = TY_BUILTIN_BOOL);
    if (strcmp(mname, "relative") == 0)
      return (node->type = make_named_foundation_type(ctx, "Range"));
    if (strcmp(named_type_family_name(unwrapped_base), "PartialRangeFrom") == 0 &&
        strcmp(mname, "makeIterator") == 0)
      return (node->type =
                  make_named_foundation_type(ctx, "PartialRangeFrom.Iterator"));
    if (strcmp(named_type_family_name(unwrapped_base), "PartialRangeFrom") == 0 &&
        strcmp(mname, "prefix") == 0) {
      TypeInfo *arr_t = type_arena_alloc(ctx->type_arena);
      if (!arr_t)
        return (node->type = TY_BUILTIN_INT);
      arr_t->kind = TY_ARRAY;
      arr_t->inner = TY_BUILTIN_INT;
      return (node->type = arr_t);
    }
    if (strcmp(named_type_family_name(unwrapped_base), "PartialRangeFrom") == 0 &&
        strcmp(mname, "dropFirst") == 0)
      return (node->type = unwrapped_base);
    if (strcmp(mname, "hashValue") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "lowerBound") == 0 &&
        strcmp(named_type_family_name(unwrapped_base), "UnboundedRange") != 0 &&
        strcmp(named_type_family_name(unwrapped_base), "PartialRangeUpTo") != 0 &&
        strcmp(named_type_family_name(unwrapped_base), "PartialRangeThrough") != 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "upperBound") == 0 &&
        strcmp(named_type_family_name(unwrapped_base), "UnboundedRange") != 0 &&
        strcmp(named_type_family_name(unwrapped_base), "PartialRangeFrom") != 0)
      return (node->type = TY_BUILTIN_INT);
    if ((strcmp(mname, "count") == 0 || strcmp(mname, "isEmpty") == 0) &&
        (strcmp(named_type_family_name(unwrapped_base), "Range") == 0 ||
         strcmp(named_type_family_name(unwrapped_base), "ClosedRange") == 0))
      return (node->type = strcmp(mname, "count") == 0 ? TY_BUILTIN_INT
                                                       : TY_BUILTIN_BOOL);
  }

  if (mname && named_type_family_name(unwrapped_base) &&
      strcmp(named_type_family_name(unwrapped_base),
             "PartialRangeFrom.Iterator") == 0) {
    if (strcmp(mname, "next") == 0)
      return (node->type = wrap_optional_result(TY_BUILTIN_INT, 1, ctx));
  }

  if (mname && named_type_family_name(unwrapped_base) &&
      strcmp(named_type_family_name(unwrapped_base), "RangeSet") == 0) {
    if (strcmp(mname, "isEmpty") == 0 ||
        strcmp(mname, "contains") == 0 ||
        strcmp(mname, "isSubset") == 0 ||
        strcmp(mname, "isSuperset") == 0 ||
        strcmp(mname, "isStrictSubset") == 0 ||
        strcmp(mname, "isStrictSuperset") == 0 ||
        strcmp(mname, "isDisjoint") == 0)
      return (node->type = TY_BUILTIN_BOOL);
    if (strcmp(mname, "insert") == 0 ||
        strcmp(mname, "remove") == 0 ||
        strcmp(mname, "formUnion") == 0 ||
        strcmp(mname, "formIntersection") == 0 ||
        strcmp(mname, "formSymmetricDifference") == 0 ||
        strcmp(mname, "subtract") == 0)
      return (node->type = TY_BUILTIN_VOID);
    if (strcmp(mname, "union") == 0 ||
        strcmp(mname, "intersection") == 0 ||
        strcmp(mname, "subtracting") == 0 ||
        strcmp(mname, "symmetricDifference") == 0)
      return (node->type = unwrapped_base);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "DateInterval") == 0) {
    if (strcmp(mname, "start") == 0 ||
        strcmp(mname, "end") == 0 ||
        strcmp(mname, "duration") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
    if (strcmp(mname, "contains") == 0 ||
        strcmp(mname, "intersects") == 0)
      return (node->type = TY_BUILTIN_BOOL);
    if (strcmp(mname, "intersection") == 0)
      return (node->type = make_named_foundation_type(ctx, "DateInterval"));
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "CharacterSet") == 0) {
    if (strcmp(mname, "inverted") == 0 ||
        strcmp(mname, "union") == 0 ||
        strcmp(mname, "intersection") == 0 ||
        strcmp(mname, "subtracting") == 0 ||
        strcmp(mname, "symmetricDifference") == 0)
      return (node->type = make_named_character_set_type(ctx));
    if (strcmp(mname, "isSuperset") == 0)
      return (node->type = TY_BUILTIN_BOOL);
    if (strcmp(mname, "formUnion") == 0 ||
        strcmp(mname, "formIntersection") == 0 ||
        strcmp(mname, "subtract") == 0 ||
        strcmp(mname, "formSymmetricDifference") == 0)
      return (node->type = TY_BUILTIN_VOID);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "TimeZone") == 0) {
    if (strcmp(mname, "identifier") == 0 ||
        strcmp(mname, "description") == 0 ||
        strcmp(mname, "abbreviation") == 0 ||
        strcmp(mname, "localizedName") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "secondsFromGMT") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "isDaylightSavingTime") == 0)
      return (node->type = TY_BUILTIN_BOOL);
    if (strcmp(mname, "daylightSavingTimeOffset") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "Locale") == 0) {
    if (strcmp(mname, "identifier") == 0 ||
        strcmp(mname, "description") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "languageCode") == 0 ||
        strcmp(mname, "regionCode") == 0 ||
        strcmp(mname, "countryCode") == 0 ||
        strcmp(mname, "currencyCode") == 0 ||
        strcmp(mname, "decimalSeparator") == 0 ||
        strcmp(mname, "groupingSeparator") == 0 ||
        strcmp(mname, "localizedString") == 0)
      return (node->type = wrap_optional_result(TY_BUILTIN_STRING, 1, ctx));
    if (strcmp(mname, "hashValue") == 0)
      return (node->type = TY_BUILTIN_INT);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "Decimal") == 0) {
    if (strcmp(mname, "description") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "doubleValue") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
    if (strcmp(mname, "isZero") == 0)
      return (node->type = TY_BUILTIN_BOOL);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "NumberFormatter") == 0) {
    if (strcmp(mname, "numberStyle") == 0 ||
        strcmp(mname, "minimumFractionDigits") == 0 ||
        strcmp(mname, "maximumFractionDigits") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "currencyCode") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "usesGroupingSeparator") == 0)
      return (node->type = TY_BUILTIN_BOOL);
    if (strcmp(mname, "string") == 0 ||
        strcmp(mname, "stringForObjectValue") == 0)
      return (node->type = TY_BUILTIN_STRING);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "DateFormatter") == 0) {
    if (strcmp(mname, "dateFormat") == 0 ||
        strcmp(mname, "string") == 0 ||
        strcmp(mname, "localizedString") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "dateStyle") == 0 ||
        strcmp(mname, "timeStyle") == 0 ||
        strcmp(mname, "formatterBehavior") == 0 ||
        strcmp(mname, "defaultFormatterBehavior") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "date") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
    if (strcmp(mname, "setLocalizedDateFormatFromTemplate") == 0)
      return (node->type = TY_BUILTIN_VOID);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "ISO8601DateFormatter") == 0) {
    if (strcmp(mname, "string") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "formatOptions") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "date") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "Measurement") == 0) {
    if (strcmp(mname, "value") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
    if (strcmp(mname, "unit") == 0)
      return (node->type = make_named_foundation_type(ctx, "Unit"));
    if (strcmp(mname, "description") == 0 ||
        strcmp(mname, "debugDescription") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "converted") == 0)
      return (node->type = make_named_foundation_type(ctx, "Measurement"));
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      (strncmp(unwrapped_base->named.name, "Unit", 4) == 0 ||
       strcmp(unwrapped_base->named.name, "Dimension") == 0)) {
    if (strcmp(mname, "symbol") == 0 ||
        strcmp(mname, "description") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "converter") == 0)
      return (node->type = make_named_foundation_type(ctx, "UnitConverter"));
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "NumberFormatter.Style") == 0 &&
      number_formatter_style_code(mname) >= 0)
    return (node->type = TY_BUILTIN_INT);

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "DateFormatter.Style") == 0 &&
      date_formatter_style_code(mname) >= 0)
    return (node->type = TY_BUILTIN_INT);

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "ISO8601DateFormatter.Options") == 0 &&
      iso8601_formatter_options_code(mname) >= 0)
    return (node->type = TY_BUILTIN_INT);

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name,
             "PropertyListSerialization.PropertyListFormat") == 0 &&
      property_list_format_code(mname) >= 0)
    return (node->type = TY_BUILTIN_INT);

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "MeasurementFormatter") == 0) {
    if (strcmp(mname, "unitStyle") == 0 ||
        strcmp(mname, "unitOptions") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "string") == 0)
      return (node->type = TY_BUILTIN_STRING);
    if (strcmp(mname, "locale") == 0)
      return (node->type = wrap_optional_result(make_named_foundation_type(ctx, "Locale"), 1, ctx));
    if (strcmp(mname, "numberFormatter") == 0)
      return (node->type = wrap_optional_result(make_named_foundation_type(ctx, "NumberFormatter"), 1, ctx));
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "MeasurementFormatter.UnitStyle") == 0 &&
      measurement_formatter_unit_style_code(mname) >= 0)
    return (node->type = TY_BUILTIN_INT);

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "MeasurementFormatter.UnitOptions") == 0 &&
      measurement_formatter_unit_options_code(mname) >= 0)
    return (node->type = TY_BUILTIN_INT);

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      (strcmp(unwrapped_base->named.name, "UnitConverter") == 0 ||
       strcmp(unwrapped_base->named.name, "UnitConverterLinear") == 0)) {
    if (strcmp(mname, "coefficient") == 0 ||
        strcmp(mname, "constant") == 0 ||
        strcmp(mname, "baseUnitValue") == 0 ||
        strcmp(mname, "value") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "CLLocationCoordinate2D") == 0) {
    if (strcmp(mname, "latitude") == 0 || strcmp(mname, "longitude") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "CLLocation") == 0) {
    if (strcmp(mname, "coordinate") == 0)
      return (node->type = make_named_foundation_type(ctx, "CLLocationCoordinate2D"));
    if (strcmp(mname, "altitude") == 0 ||
        strcmp(mname, "horizontalAccuracy") == 0 ||
        strcmp(mname, "verticalAccuracy") == 0 ||
        strcmp(mname, "course") == 0 ||
        strcmp(mname, "speed") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
    if (strcmp(mname, "timestamp") == 0)
      return (node->type = make_named_foundation_type(ctx, "Date"));
    if (strcmp(mname, "distance") == 0) {
      TypeInfo *ft = type_arena_alloc(ctx->type_arena);
      ft->kind = TY_FUNC;
      ft->func.ret = TY_BUILTIN_DOUBLE;
      ft->func.param_count = 1;
      ft->func.params = malloc(sizeof(TypeInfo *));
      ft->func.params[0] = make_named_foundation_type(ctx, "CLLocation");
      return (node->type = ft);
    }
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "CLLocationManager") == 0) {
    if (strcmp(mname, "delegate") == 0)
      return (node->type = wrap_optional_result(make_named_foundation_type(ctx, "CLLocationManagerDelegate"), 1, ctx));
    if (strcmp(mname, "desiredAccuracy") == 0 || strcmp(mname, "distanceFilter") == 0)
      return (node->type = TY_BUILTIN_DOUBLE);
    if (strcmp(mname, "location") == 0)
      return (node->type = wrap_optional_result(make_named_foundation_type(ctx, "CLLocation"), 1, ctx));
    if (strcmp(mname, "requestWhenInUseAuthorization") == 0 ||
        strcmp(mname, "requestAlwaysAuthorization") == 0 ||
        strcmp(mname, "startUpdatingLocation") == 0 ||
        strcmp(mname, "stopUpdatingLocation") == 0 ||
        strcmp(mname, "requestLocation") == 0) {
      TypeInfo *ft = type_arena_alloc(ctx->type_arena);
      ft->kind = TY_FUNC;
      ft->func.ret = TY_BUILTIN_VOID;
      ft->func.param_count = 0;
      ft->func.params = NULL;
      return (node->type = ft);
    }
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "IndexPath") == 0) {
    if (strcmp(mname, "count") == 0 ||
        strcmp(mname, "startIndex") == 0 ||
        strcmp(mname, "endIndex") == 0 ||
        strcmp(mname, "first") == 0 ||
        strcmp(mname, "last") == 0 ||
        strcmp(mname, "index") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "isEmpty") == 0)
      return (node->type = TY_BUILTIN_BOOL);
    if (strcmp(mname, "append") == 0)
      return (node->type = TY_BUILTIN_VOID);
    if (strcmp(mname, "appending") == 0 ||
        strcmp(mname, "dropLast") == 0 ||
        strcmp(mname, "removingLastIndex") == 0)
      return (node->type = make_named_foundation_type(ctx, "IndexPath"));
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "IndexSet") == 0) {
    if (strcmp(mname, "count") == 0 ||
        strcmp(mname, "startIndex") == 0 ||
        strcmp(mname, "endIndex") == 0 ||
        strcmp(mname, "first") == 0 ||
        strcmp(mname, "last") == 0)
      return (node->type = TY_BUILTIN_INT);
    if (strcmp(mname, "isEmpty") == 0 ||
        strcmp(mname, "contains") == 0)
      return (node->type = TY_BUILTIN_BOOL);
    if (strcmp(mname, "insert") == 0 ||
        strcmp(mname, "remove") == 0)
      return (node->type = TY_BUILTIN_VOID);
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "Mirror") == 0) {
    if (strcmp(mname, "displayStyle") == 0)
      return (node->type = wrap_optional_result(TY_BUILTIN_STRING, 1, ctx));
  }

  /* Optional.map / Optional.flatMap: resolve on Optional type (not unwrapped) */
  if (base_t && base_t->kind == TY_OPTIONAL && mname) {
    TypeInfo *bm_opt = lookup_builtin_member(ctx, base_t, mname);
    if (bm_opt)
      return (node->type = bm_opt);
  }
  if (unwrapped_base && mname) {
    TypeInfo *bm = lookup_builtin_member(ctx, unwrapped_base, mname);
    if (bm)
      return (node->type = wrap_optional_result(bm, base_is_opt_chain, ctx));
  }

  if (unwrapped_base && unwrapped_base->kind == TY_TUPLE &&
      node->data.var.name_tok) {
    const Token *mtok = &ctx->tokens[node->data.var.name_tok];
    if (mtok->type == TOK_INTEGER_LIT) {
      char ibuf[16];
      size_t ilen = mtok->len < 15 ? mtok->len : 15;
      memcpy(ibuf, ctx->src->data + mtok->pos, ilen);
      ibuf[ilen] = '\0';
      uint32_t idx = (uint32_t)atoi(ibuf);
      if (idx < unwrapped_base->tuple.elem_count &&
          unwrapped_base->tuple.elems[idx]) {
        TypeInfo *t = unwrapped_base->tuple.elems[idx];
        return (node->type = wrap_optional_result(t, base_is_opt_chain, ctx));
      }
    }
    if (mtok->type == TOK_IDENTIFIER && unwrapped_base->tuple.labels) {
      const char *mname =
          sema_intern(ctx, ctx->src->data + mtok->pos, mtok->len);
      for (size_t li = 0; li < unwrapped_base->tuple.elem_count; li++) {
        if (unwrapped_base->tuple.labels[li] &&
            unwrapped_base->tuple.labels[li] == mname) {
          TypeInfo *t = unwrapped_base->tuple.elems[li];
          return (node->type = wrap_optional_result(t, base_is_opt_chain, ctx));
        }
      }
    }
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.decl && mname) {
    const ASTNode *decl = (const ASTNode *)unwrapped_base->named.decl;
    const ASTNode *body = class_decl_body(decl);
    ASTNode *m = sema_member_lookup(ctx, decl, body, mname);
    if (m) {
      if (!private_member_visible(ctx, m, decl))
        sema_error(ctx, node, "private member is not accessible");
      check_actor_isolation_for_member(ctx, node, unwrapped_base, m);
      if (!m->type)
        resolve_node(ctx, m);
      TypeInfo *t = m->type;
      return (node->type = wrap_optional_result(t, base_is_opt_chain, ctx));
    }
    /* Associated type resolution: T.Element → concrete type via conformance */
    if (ctx->conformance_table && ctx->assoc_type_table &&
        conf_name_present(ctx, unwrapped_base->named.name)) {
      const ConformanceTable *ct = ctx->conformance_table;
      for (uint32_t ci = 0; ci < ct->count; ci++) {
        if (!ct->entries[ci].type_name || !ct->entries[ci].protocol_name)
          continue;
        if (strcmp(ct->entries[ci].type_name, unwrapped_base->named.name) != 0)
          continue;
        TypeInfo *resolved = resolve_assoc_type_to_concrete(
            ctx, unwrapped_base, ct->entries[ci].protocol_name, mname);
        if (resolved)
          return (node->type =
                      wrap_optional_result(resolved, base_is_opt_chain, ctx));
      }
    }
  }
  /* SDK / vocabulary member: a named type known only by name (no local decl) —
   * resolve a property's type from the loaded vocabulary.  Positive-only: a hit
   * types the access (and lets a chain like view.layer.bounds keep resolving); a
   * miss leaves it unresolved, never an error. */
  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname) {
    /* `obj.m(...)` wants a method; a plain `obj.m` wants the property (ObjC
     * types carry both a `frame` property and a `frame(forAlignmentRect:)`
     * method) — let the call position pick. */
    int prefer_callable = (node->parent && node->parent->kind == AST_CALL_EXPR &&
                           node->parent->first_child == node);
    TypeInfo *vt = resolve_vocab_member_type(ctx, unwrapped_base->named.name,
                                             mname, prefer_callable);
    if (vt)
      return (node->type = wrap_optional_result(vt, base_is_opt_chain, ctx));
  }
  if (node->type)
    return node->type;
  return NULL;
}
