/**
 * @file conformance.c
 * @brief Builtin member/method resolution and sequence element type lookup.
 *
 * Contains the BUILTIN_MEMBERS table (~97 entries) mapping (type, member_name)
 * to result types for Array, String, Int, Dict, Set, Character, Optional,
 * Substring, Data, Bool, and Double.
 */
#include "private.h"

/* Forward declaration — defined in resolve/node.c */
TypeInfo *resolve_node(SemaContext *ctx, ASTNode *node);

static void define_generic_params_for_lazy_member_resolution(SemaContext *ctx,
                                                             const ASTNode *node) {
  for (ASTNode *c = node ? node->first_child : NULL; c; c = c->next_sibling) {
    if (c->kind != AST_GENERIC_PARAM)
      continue;
    const char *pname = tok_intern_at_node(ctx, c, c->tok_idx);
    TypeInfo *gp_ti = c->type;
    if (!gp_ti) {
      gp_ti = type_arena_alloc(ctx->type_arena);
      if (!gp_ti)
        continue;
      gp_ti->kind = TY_GENERIC_PARAM;
      gp_ti->param.name = pname;
      gp_ti->param.index = 0;
      gp_ti->param.constraints = NULL;
      gp_ti->param.constraint_count = 0;
      c->type = gp_ti;
    }
    sema_define(ctx, pname, SYM_TYPE, gp_ti, c);
  }
}

/**
 * @brief Looks up a named member in a user-defined type's declaration body.
 *
 * Searches for a var/let/func/init with a matching name, resolving its type
 * lazily if needed.
 */
TypeInfo *lookup_named_member(SemaContext *ctx, TypeInfo *ty,
                              const char *mname) {
  if (!ty)
    return NULL;
  TypeInfo *unwrapped = ty;
  if (ty->kind == TY_OPTIONAL)
    unwrapped = ty->inner;
  if (!unwrapped || unwrapped->kind != TY_NAMED || !unwrapped->named.decl)
    return NULL;

  const ASTNode *decl = (const ASTNode *)unwrapped->named.decl;
  const ASTNode *body = NULL;
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_BLOCK) {
      body = c;
      break;
    }
  }

  for (const ASTNode *ch = body ? body->first_child : decl->first_child; ch;
       ch = ch->next_sibling) {
    uint32_t name_tok = 0;
    if (ch->kind == AST_VAR_DECL || ch->kind == AST_LET_DECL ||
        ch->kind == AST_PARAM) {
      name_tok = ch->data.var.name_tok;
    } else if (ch->kind == AST_FUNC_DECL || ch->kind == AST_INIT_DECL) {
      name_tok = ch->data.func.name_tok;
    }

    if (name_tok) {
      const char *chn = tok_intern_at_node(ctx, ch, name_tok);
      if (strcmp(chn, mname) == 0) {
        if (!ch->type)
          resolve_node(ctx, (ASTNode *)ch);
        return ((ASTNode *)ch)->type;
      }
    }
  }
  return NULL;
}

/**
 * @brief Infers the element type of a Sequence for for-in loops.
 *
 * Handles Array, Set, generic Array<T>, String (yields Character), and custom
 * Sequence types (via makeIterator().next() chain).  Falls back to Int.
 */
TypeInfo *get_sequence_element_type(SemaContext *ctx, TypeInfo *seq_t) {
  if (!seq_t)
    return TY_BUILTIN_INT;

  /* 1. Array: [T] or Array<T> */
  if (seq_t->kind == TY_ARRAY)
    return seq_t->inner;
  if (seq_t->kind == TY_SET)
    return seq_t->inner;
  if (seq_t->kind == TY_NAMED && seq_t->named.name &&
      (strcmp(seq_t->named.name, "IndexPath") == 0 ||
       strcmp(seq_t->named.name, "IndexSet") == 0))
    return TY_BUILTIN_INT;
  if (seq_t->kind == TY_GENERIC_INST && seq_t->generic.base &&
      seq_t->generic.base->kind == TY_NAMED &&
      !strcmp(seq_t->generic.base->named.name, "Array")) {
    if (seq_t->generic.arg_count > 0)
      return seq_t->generic.args[0];
  }

  /* 2. String -> iterating yields Character (TY_NAMED "Character") */
  if (seq_t == TY_BUILTIN_STRING ||
      (seq_t->kind == TY_NAMED && seq_t->named.name &&
       !strcmp(seq_t->named.name, SW_TYPE_STRING))) {
    TypeInfo *char_ty = type_arena_alloc(ctx->type_arena);
    if (char_ty) {
      char_ty->kind = TY_NAMED;
      char_ty->named.name = "Character";
      char_ty->named.decl = NULL;
      return char_ty;
    }
    return TY_BUILTIN_STRING;
  }

  /* 3. Custom Sequence: look for makeIterator() -> Iterator, then
   *    Iterator.next() -> Element? */
  TypeInfo *it_func_t = lookup_named_member(ctx, seq_t, "makeIterator");
  if (it_func_t && it_func_t->kind == TY_FUNC) {
    TypeInfo *it_t = it_func_t->func.ret;
    if (it_t) {
      TypeInfo *next_func_t = lookup_named_member(ctx, it_t, "next");
      if (next_func_t && next_func_t->kind == TY_FUNC) {
        TypeInfo *opt_t = next_func_t->func.ret;
        if (opt_t && opt_t->kind == TY_OPTIONAL) {
          return opt_t->inner;
        }
      }
    }
  }

  return TY_BUILTIN_INT;
}

void pass3_check_conformances(SemaContext *ctx, ASTNode *root);
int check_conformance(const ASTNode *type_decl, const ASTNode *proto_decl,
                      SemaContext *ctx, const ASTNode *ast_root);

/* ═══════════════════════════════════════════════════════════════════════════════
 * Sendable inference
 * ═══════════════════════════════════════════════════════════════════════════════
 * Value types (struct/enum) are Sendable when every stored property's type is
 * Sendable. This helper answers the question for any TypeInfo by consulting
 * the conformance table for nominal types and recursing into composites. */
int is_type_sendable(const SemaContext *ctx, const TypeInfo *t) {
  if (!t || !ctx || !ctx->conformance_table) return 0;
  if (t == TY_BUILTIN_INT || t == TY_BUILTIN_DOUBLE ||
      t == TY_BUILTIN_FLOAT || t == TY_BUILTIN_BOOL ||
      t == TY_BUILTIN_STRING || t == TY_BUILTIN_VOID ||
      t == TY_BUILTIN_DATA || t == TY_BUILTIN_SUBSTRING ||
      t == TY_BUILTIN_UINT || t == TY_BUILTIN_UINT8 ||
      t == TY_BUILTIN_UINT16 || t == TY_BUILTIN_UINT32 ||
      t == TY_BUILTIN_UINT64)
    return 1;
  switch (t->kind) {
  case TY_OPTIONAL:
  case TY_ARRAY:
  case TY_SET:
    return is_type_sendable(ctx, t->inner);
  case TY_DICT:
    return is_type_sendable(ctx, t->dict.key) &&
           is_type_sendable(ctx, t->dict.value);
  case TY_TUPLE: {
    for (size_t i = 0; i < t->tuple.elem_count; i++)
      if (!is_type_sendable(ctx, t->tuple.elems[i])) return 0;
    return 1;
  }
  case TY_NAMED:
    if (!t->named.name) return 0;
    return conformance_table_has(ctx->conformance_table, t->named.name,
                                 SW_PROTO_SENDABLE);
  case TY_GENERIC_INST:
    if (t->generic.base && t->generic.base->kind == TY_NAMED &&
        t->generic.base->named.name) {
      if (!conformance_table_has(ctx->conformance_table,
                                 t->generic.base->named.name,
                                 SW_PROTO_SENDABLE))
        return 0;
      for (uint32_t i = 0; i < t->generic.arg_count; i++)
        if (!is_type_sendable(ctx, t->generic.args[i])) return 0;
      return 1;
    }
    return 0;
  default:
    return 0;
  }
}

static int is_definitely_non_sendable_class_type(const SemaContext *ctx,
                                                 const TypeInfo *t) {
  if (!ctx || !t || !ctx->conformance_table) return 0;
  switch (t->kind) {
  case TY_OPTIONAL:
  case TY_ARRAY:
  case TY_SET:
    return is_definitely_non_sendable_class_type(ctx, t->inner);
  case TY_DICT:
    return is_definitely_non_sendable_class_type(ctx, t->dict.key) ||
           is_definitely_non_sendable_class_type(ctx, t->dict.value);
  case TY_TUPLE:
    for (size_t i = 0; i < t->tuple.elem_count; i++)
      if (is_definitely_non_sendable_class_type(ctx, t->tuple.elems[i]))
        return 1;
    return 0;
  case TY_GENERIC_INST:
    if (is_definitely_non_sendable_class_type(ctx, t->generic.base))
      return 1;
    for (uint32_t i = 0; i < t->generic.arg_count; i++)
      if (is_definitely_non_sendable_class_type(ctx, t->generic.args[i]))
        return 1;
    return 0;
  case TY_NAMED: {
    if (!t->named.name || !strcmp(t->named.name, SW_TYPE_ANY) ||
        !strcmp(t->named.name, SW_TYPE_ANY_OBJECT))
      return 0;
    const ASTNode *decl = (const ASTNode *)t->named.decl;
    if (!decl || decl->kind != AST_CLASS_DECL)
      return 0;
    return !conformance_table_has(ctx->conformance_table, t->named.name,
                                  SW_PROTO_SENDABLE);
  }
  default:
    return 0;
  }
}

/* Walks the source root once and:
 *   - infers Sendable for struct/enum declarations whose stored properties
 *     are all Sendable, registering the conformance in the table
 *   - diagnoses class declarations that explicitly conform to Sendable
 *     but have a non-Sendable stored property
 *
 * Single-pass — relies on source order for forward references. Mutually
 * recursive value types remain non-Sendable here, which matches the
 * expectation that user-visible diagnostics are conservative. */
void infer_and_check_sendable(SemaContext *ctx, const ASTNode *root) {
  if (!ctx || !root || !ctx->conformance_table) return;
  for (const ASTNode *node = root->first_child; node; node = node->next_sibling) {
    if (node->kind != AST_STRUCT_DECL && node->kind != AST_CLASS_DECL &&
        node->kind != AST_ENUM_DECL)
      continue;
    if (!node->data.var.name_tok) continue;
    const char *type_name =
        tok_intern_at_node(ctx, node, node->data.var.name_tok);
    if (!type_name) continue;

    /* Collect stored-property types from the body. */
    const ASTNode *body = NULL;
    for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
      if (c->kind == AST_BLOCK) { body = c; break; }
    int all_sendable = 1;
    int has_any_property = 0;
    const TypeInfo *first_unsendable = NULL;
    if (body) {
      sema_push_scope(ctx);
      define_generic_params_for_lazy_member_resolution(ctx, node);
      for (const ASTNode *m = body->first_child; m; m = m->next_sibling) {
        if (m->kind != AST_VAR_DECL && m->kind != AST_LET_DECL) continue;
        if (m->data.var.is_computed) continue;
        if (m->modifiers & MOD_STATIC) continue;
        has_any_property = 1;
        /* Pass 2's resolve_node early-returns on nominal types whose type
         * is forward-declared; that leaves stored-property TypeInfo
         * unresolved for struct/class bodies. Force-resolve here. */
        if (!m->type)
          resolve_node(ctx, (ASTNode *)m);
        if (!is_type_sendable(ctx, m->type)) {
          all_sendable = 0;
          if (!first_unsendable) first_unsendable = m->type;
        }
      }
      sema_pop_scope(ctx);
    }

    int already_sendable =
        conformance_table_has(ctx->conformance_table, type_name,
                              SW_PROTO_SENDABLE);

    if (node->kind == AST_CLASS_DECL) {
      /* Class only conforms to Sendable when explicitly declared so. If the
       * user wrote it, stored properties must be Sendable. */
      if (already_sendable && !all_sendable &&
          is_definitely_non_sendable_class_type(ctx, first_unsendable)) {
        char tn[64];
        type_to_string(first_unsendable, tn, sizeof(tn));
        sema_error(ctx, (ASTNode *)node,
                   "Class '%s' conforms to Sendable but has stored property "
                   "of non-Sendable type '%s'",
                   type_name, tn);
      }
      continue;
    }

    /* Struct/enum: auto-Sendable when every stored property is Sendable.
     * Empty struct/enum (no stored property) trivially Sendable. */
    if (!already_sendable && (all_sendable || !has_any_property))
      conformance_table_add(ctx->conformance_table, type_name,
                            SW_PROTO_SENDABLE);
  }
}

/* Walks the AST and, for every AST_CLOSURE_EXPR explicitly annotated as
 * @Sendable (`{ @Sendable in ... }`), checks that every captured value's
 * type is Sendable. Runs after infer_and_check_sendable so user-defined
 * value types have already been registered. */
static void check_sendable_closures_rec(SemaContext *ctx, const ASTNode *node) {
  if (!node) return;
  if (node->kind == AST_CLOSURE_EXPR && (node->modifiers & MOD_SENDABLE)) {
    const CaptureList *captures = (const CaptureList *)node->data.closure.captures;
    if (captures) {
      for (uint32_t i = 0; i < captures->count; i++) {
        const CaptureInfo *ci = &captures->captures[i];
        if (!ci->type) continue;
        if (!is_type_sendable(ctx, ci->type)) {
          char ts[64];
          type_to_string(ci->type, ts, sizeof(ts));
          sema_error(ctx, (ASTNode *)node,
                     "Capture of '%s' with non-Sendable type '%s' in a "
                     "@Sendable closure",
                     ci->name ? ci->name : "?", ts);
        }
      }
    }
  }
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
    check_sendable_closures_rec(ctx, c);
}

void check_sendable_closures(SemaContext *ctx, const ASTNode *root) {
  check_sendable_closures_rec(ctx, root);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Builtin member / method lookup — table-driven
 * ═══════════════════════════════════════════════════════════════════════════════
 * Centralised table: (base_kind, member_name) -> result TypeInfo*.
 * Used by both resolve_member_expr and resolve_call_expr to avoid duplicated
 * strcmp chains for Array / String / Int / Dict members and methods.
 *
 * BM_KIND encodes which TypeKind the entry applies to.  For builtins that are
 * represented as TY_BUILTIN_STRING->kind etc., we store the inner kind value
 * so the lookup compares `base_t->kind == entry.base_kind`.
 */

/* ── BUILTIN_MEMBERS — Builtin type member resolution table ──────────────────
 *
 * This sentinel-terminated table maps (base_type, member_name) -> return_type
 * for all builtin types (Array, String, Int, Dict, Set, Character, Optional).
 *
 * HOW TO ADD A NEW ENTRY:
 *   1. Find the section for the base type (or add one)
 *   2. Add {BMK_xxx, "memberName", BMR_xxx} before the sentinel
 *   3. If the member's return type doesn't fit existing BMResult values,
 *      add a new BMR_xxx to the BMResult enum above and handle it in
 *      bmr_to_type() below
 */
static const BuiltinMemberEntry BUILTIN_MEMBERS[] = {
    /* ── Array properties ───────────────────────────────────────────────────── */
    {BMK_ARRAY, "count", BMR_INT},
    {BMK_ARRAY, "startIndex", BMR_INT},
    {BMK_ARRAY, "endIndex", BMR_INT},
    {BMK_ARRAY, "isEmpty", BMR_BOOL},
    {BMK_ARRAY, "contains", BMR_BOOL},
    {BMK_ARRAY, "first", BMR_OPT_INNER},
    {BMK_ARRAY, "last", BMR_OPT_INNER},
    /* ── Array methods ──────────────────────────────────────────────────────── */
    {BMK_ARRAY, "append", BMR_VOID},
    {BMK_ARRAY, "remove", BMR_VOID},
    {BMK_ARRAY, "removeLast", BMR_VOID},
    {BMK_ARRAY, "removeAll", BMR_VOID},
    {BMK_ARRAY, "insert", BMR_VOID},
    {BMK_ARRAY, "sort", BMR_VOID},
    {BMK_ARRAY, "reverse", BMR_VOID},
    {BMK_ARRAY, "shuffle", BMR_VOID},
    {BMK_ARRAY, "sorted", BMR_ARRAY_SAME},
    {BMK_ARRAY, "reversed", BMR_ARRAY_SAME},
    {BMK_ARRAY, "enumerated", BMR_ARRAY_UNKNOWN},
    {BMK_ARRAY, "lazy", BMR_SELF_BASE},
    {BMK_ARRAY, "allSatisfy", BMR_BOOL},
    {BMK_ARRAY, "any", BMR_BOOL},
    {BMK_ARRAY, "map", BMR_ARRAY_UNKNOWN},
    {BMK_ARRAY, "filter", BMR_ARRAY_UNKNOWN},
    {BMK_ARRAY, "reduce", BMR_ARRAY_UNKNOWN},
    {BMK_ARRAY, "forEach", BMR_VOID},
    {BMK_ARRAY, "compactMap", BMR_ARRAY_UNKNOWN},
    {BMK_ARRAY, "flatMap", BMR_ARRAY_UNKNOWN},

    /* ── Optional (Int?, etc.) map / flatMap ──────────────────────────────── */
    {BMK_OPTIONAL, "map", BMR_OPT_INNER},
    {BMK_OPTIONAL, "flatMap", BMR_OPT_INNER},

    /* ── Substring ──────────────────────────────────────────────────────────── */
    {BMK_SUBSTRING, "count", BMR_INT},
    {BMK_SUBSTRING, "utf8Count", BMR_INT},
    {BMK_SUBSTRING, "isEmpty", BMR_BOOL},
    {BMK_SUBSTRING, "description", BMR_STRING},
    {BMK_SUBSTRING, "hasPrefix", BMR_BOOL},
    {BMK_SUBSTRING, "hasSuffix", BMR_BOOL},
    {BMK_SUBSTRING, "contains", BMR_BOOL},
    {BMK_SUBSTRING, "prefix", BMR_SUBSTRING},
    {BMK_SUBSTRING, "suffix", BMR_SUBSTRING},
    {BMK_SUBSTRING, "dropFirst", BMR_SUBSTRING},
    {BMK_SUBSTRING, "dropLast", BMR_SUBSTRING},
    {BMK_SUBSTRING, "enumerated", BMR_ARRAY_UNKNOWN},
    {BMK_SUBSTRING, "utf8Start", BMR_STRING},
    {BMK_SUBSTRING, "unicodeScalar", BMR_STRING},
    {BMK_SUBSTRING, "utf8CodeUnitCount", BMR_INT},
    {BMK_SUBSTRING, "hasPointerRepresentation", BMR_BOOL},
    {BMK_SUBSTRING, "isASCII", BMR_BOOL},

    /* ── Data properties/methods ────────────────────────────────────────── */
    {BMK_DATA, "count", BMR_INT},
    {BMK_DATA, "isEmpty", BMR_BOOL},
    {BMK_DATA, "base64EncodedString", BMR_STRING},
    {BMK_DATA, "subdata", BMR_DATA},

    /* ── String properties ──────────────────────────────────────────────────── */
    {BMK_STRING, "count", BMR_INT},
    /* utf8/utf16 return collection views in Swift; lowered to Int here
       since only their .count is typically accessed in this type system. */
    {BMK_STRING, "utf8",  BMR_INT},
    {BMK_STRING, "utf16", BMR_INT},
    {BMK_STRING, "utf8Count", BMR_INT},
    {BMK_STRING, "startIndex", BMR_INT},
    {BMK_STRING, "endIndex", BMR_INT},
    /* unicodeScalars returns a UnicodeScalarView; lowered to String here
       so member access (e.g. .count) resolves correctly. */
    {BMK_STRING, "unicodeScalars", BMR_STRING},
    {BMK_STRING, "isEmpty", BMR_BOOL},
    {BMK_STRING, "lowercased", BMR_STRING},
    {BMK_STRING, "uppercased", BMR_STRING},
    {BMK_STRING, "description", BMR_STRING},
    {BMK_STRING, "isNumber", BMR_BOOL},
    {BMK_STRING, "isLetter", BMR_BOOL},
    {BMK_STRING, "isWhitespace", BMR_BOOL},
    {BMK_STRING, "isPunctuation", BMR_BOOL},
    {BMK_STRING, "isUppercase", BMR_BOOL},
    {BMK_STRING, "isLowercase", BMR_BOOL},
    {BMK_STRING, "isHexDigit", BMR_BOOL},
    {BMK_STRING, "isASCII", BMR_BOOL},
    {BMK_STRING, "asciiValue", BMR_INT},
    {BMK_STRING, "utf8Start", BMR_STRING},
    {BMK_STRING, "unicodeScalar", BMR_STRING},
    {BMK_STRING, "utf8CodeUnitCount", BMR_INT},
    {BMK_STRING, "hasPointerRepresentation", BMR_BOOL},
    /* Substring operations */
    {BMK_STRING, "prefix", BMR_SUBSTRING},
    {BMK_STRING, "suffix", BMR_SUBSTRING},
    {BMK_STRING, "dropFirst", BMR_SUBSTRING},
    {BMK_STRING, "dropLast", BMR_SUBSTRING},
    /* ── String methods ─────────────────────────────────────────────────────── */
    {BMK_STRING, "hasPrefix", BMR_BOOL},
    {BMK_STRING, "hasSuffix", BMR_BOOL},
    {BMK_STRING, "contains", BMR_BOOL},
    {BMK_STRING, "localizedCaseInsensitiveContains", BMR_BOOL},
    {BMK_STRING, "localizedStandardContains", BMR_BOOL},
    {BMK_STRING, "trimmingCharacters", BMR_STRING},
    {BMK_STRING, "addingPercentEncoding", BMR_STRING},
    {BMK_STRING, "removingPercentEncoding", BMR_STRING},
    {BMK_STRING, "replacingOccurrences", BMR_STRING},
    {BMK_STRING, "map", BMR_ARRAY_UNKNOWN},
    {BMK_STRING, "filter", BMR_STRING},
    {BMK_STRING, "compactMap", BMR_ARRAY_UNKNOWN},
    {BMK_STRING, "appending", BMR_STRING},
    {BMK_STRING, "components", BMR_ARRAY_STRING},
    {BMK_STRING, "range", BMR_OPT_INT},
    {BMK_STRING, "rangeOfCharacter", BMR_OPT_INT},
    {BMK_STRING, "split", BMR_ARRAY_STRING},
    {BMK_STRING, "append", BMR_STRING},
    {BMK_STRING, "insert", BMR_STRING},
    {BMK_STRING, "remove", BMR_STRING},
    {BMK_STRING, "index", BMR_INT},
    {BMK_STRING, "removeSubrange", BMR_STRING},
    {BMK_STRING, "indices", BMR_INT},
    {BMK_STRING, "data", BMR_OPT_DATA},
    {BMK_STRING, "lazy", BMR_SELF_BASE},

    /* ── Int properties ─────────────────────────────────────────────────────── */
    {BMK_INT, "description", BMR_STRING},
    {BMK_INT, "advanced", BMR_INT},
    {BMK_INT, "distance", BMR_INT},

    /* ── Dict properties ────────────────────────────────────────────────────── */
    {BMK_DICT, "count", BMR_INT},
    {BMK_DICT, "keys", BMR_ARRAY_KEY},
    {BMK_DICT, "values", BMR_ARRAY_VALUE},
    {BMK_DICT, "isEmpty", BMR_BOOL},
    {BMK_DICT, "first", BMR_OPT_DICT_ELEMENT},
    {BMK_DICT, "contains", BMR_BOOL},
    {BMK_DICT, "forEach", BMR_VOID},
    {BMK_DICT, "compactMap", BMR_ARRAY_UNKNOWN},
    {BMK_DICT, "sorted", BMR_ARRAY_KEY},
    {BMK_DICT, "lazy", BMR_SELF_BASE},

    /* ── Set (Hashable element collection) ────────────────────────────────── */
    {BMK_SET, "count", BMR_INT},
    {BMK_SET, "isEmpty", BMR_BOOL},
    {BMK_SET, "contains", BMR_BOOL},
    {BMK_SET, "first", BMR_OPT_INNER},
    {BMK_SET, "insert", BMR_VOID},
    {BMK_SET, "remove", BMR_VOID},
    {BMK_SET, "removeAll", BMR_VOID},
    {BMK_SET, "sorted", BMR_ARRAY_SAME},
    {BMK_SET, "union", BMR_ARRAY_SAME},
    {BMK_SET, "intersection", BMR_ARRAY_SAME},
    {BMK_SET, "subtracting", BMR_ARRAY_SAME},
    {BMK_SET, "isSubset", BMR_BOOL},
    {BMK_SET, "isSuperset", BMR_BOOL},
    {BMK_SET, "isDisjoint", BMR_BOOL},
    {BMK_SET, "lazy", BMR_SELF_BASE},

    /* ── Character properties ───────────────────────────────────────────────── */
    {BMK_CHARACTER, "isLetter", BMR_BOOL},
    {BMK_CHARACTER, "isNumber", BMR_BOOL},
    {BMK_CHARACTER, "isHexDigit", BMR_BOOL},
    {BMK_CHARACTER, "isWhitespace", BMR_BOOL},
    {BMK_CHARACTER, "isNewline", BMR_BOOL},
    {BMK_CHARACTER, "isPunctuation", BMR_BOOL},
    {BMK_CHARACTER, "isSymbol", BMR_BOOL},
    {BMK_CHARACTER, "isUppercase", BMR_BOOL},
    {BMK_CHARACTER, "isLowercase", BMR_BOOL},
    {BMK_CHARACTER, "asciiValue", BMR_INT},
    {BMK_CHARACTER, "isASCII", BMR_BOOL},
    {BMK_CHARACTER, "description", BMR_STRING},
    {BMK_CHARACTER, "uppercased", BMR_STRING},
    {BMK_CHARACTER, "lowercased", BMR_STRING},

    /* ── Hashable protocol stubs (hashValue) ────────────────────────────────── */
    {BMK_INT, "hashValue", BMR_INT},
    {BMK_STRING, "hashValue", BMR_INT},
    {BMK_BOOL, "hashValue", BMR_INT},
    {BMK_DOUBLE, "hashValue", BMR_INT},

    /* ── Date: Foundation's Date is lowered to Double in this type system.
       These members model the calendar-component accessors from DateComponents,
       mapped onto the Double representation for basic date arithmetic. ──────── */
    {BMK_DOUBLE, "year", BMR_INT},
    {BMK_DOUBLE, "month", BMR_INT},
    {BMK_DOUBLE, "day", BMR_INT},
    {BMK_DOUBLE, "hour", BMR_INT},
    {BMK_DOUBLE, "minute", BMR_INT},
    {BMK_DOUBLE, "second", BMR_INT},
    {BMK_DOUBLE, "weekday", BMR_INT},
    {BMK_DOUBLE, "timeIntervalSince1970", BMR_DOUBLE},
    {BMK_DOUBLE, "timeIntervalSinceReferenceDate", BMR_DOUBLE},

    {0, NULL, 0} /* sentinel */
};

/**
 * @brief Converts a BMResult code to a concrete TypeInfo pointer.
 *
 * Allocates wrapper types (Optional, Array, Tuple) from the type arena as needed.
 * For composite results, @p base_t provides the inner/key/value types.
 */
TypeInfo *bmr_to_type(BMResult r, SemaContext *ctx, TypeInfo *base_t) {
  TypeInfo *t;
  switch (r) {
  case BMR_INT:       return TY_BUILTIN_INT;
  case BMR_BOOL:      return TY_BUILTIN_BOOL;
  case BMR_STRING:    return TY_BUILTIN_STRING;
  case BMR_DOUBLE:    return TY_BUILTIN_DOUBLE;
  case BMR_VOID:      return TY_BUILTIN_VOID;
  case BMR_SUBSTRING: return TY_BUILTIN_SUBSTRING;
  case BMR_DATA:      return TY_BUILTIN_DATA;
  case BMR_SELF_BASE: return base_t;
  case BMR_OPT_INNER:
    t = type_arena_alloc(ctx->type_arena);
    if (!t) return NULL;
    t->kind = TY_OPTIONAL;
    t->inner = base_t ? base_t->inner : NULL;
    return t;
  case BMR_ARRAY_UNKNOWN:
    t = type_arena_alloc(ctx->type_arena);
    if (!t) return NULL;
    t->kind = TY_ARRAY;
    return t;
  case BMR_ARRAY_SAME:
    t = type_arena_alloc(ctx->type_arena);
    if (!t) return NULL;
    t->kind = TY_ARRAY;
    t->inner = base_t ? base_t->inner : NULL;
    return t;
  case BMR_ARRAY_STRING:
    t = type_arena_alloc(ctx->type_arena);
    if (!t) return NULL;
    t->kind = TY_ARRAY;
    t->inner = TY_BUILTIN_STRING;
    return t;
  case BMR_OPT_INT:
    t = type_arena_alloc(ctx->type_arena);
    if (!t) return TY_BUILTIN_INT;
    t->kind = TY_OPTIONAL;
    t->inner = TY_BUILTIN_INT;
    return t;
  case BMR_ARRAY_KEY:
    t = type_arena_alloc(ctx->type_arena);
    if (!t) return NULL;
    t->kind = TY_ARRAY;
    t->inner = base_t ? base_t->dict.key : NULL;
    return t;
  case BMR_ARRAY_VALUE:
    t = type_arena_alloc(ctx->type_arena);
    if (!t) return NULL;
    t->kind = TY_ARRAY;
    t->inner = base_t ? base_t->dict.value : NULL;
    return t;
  case BMR_OPT_DICT_ELEMENT: {
    TypeInfo *tuple = type_arena_alloc(ctx->type_arena);
    if (!tuple) return NULL;
    tuple->kind = TY_TUPLE;
    tuple->tuple.elem_count = 2;
    tuple->tuple.elems = calloc(2, sizeof(TypeInfo *));
    tuple->tuple.labels = calloc(2, sizeof(const char *));
    if (tuple->tuple.elems) {
      tuple->tuple.elems[0] = base_t ? base_t->dict.key : TY_BUILTIN_STRING;
      tuple->tuple.elems[1] = base_t ? base_t->dict.value : TY_BUILTIN_INT;
    }
    if (tuple->tuple.labels) {
      tuple->tuple.labels[0] = "key";
      tuple->tuple.labels[1] = "value";
    }
    TypeInfo *opt = type_arena_alloc(ctx->type_arena);
    if (!opt) return tuple; /* return unwrapped tuple rather than crash */
    opt->kind = TY_OPTIONAL;
    opt->inner = tuple;
    return opt;
  }
  case BMR_OPT_DATA:
    t = type_arena_alloc(ctx->type_arena);
    if (!t) return TY_BUILTIN_DATA; /* return unwrapped rather than crash */
    t->kind = TY_OPTIONAL;
    t->inner = TY_BUILTIN_DATA;
    return t;
  }
  return NULL;
}

/** @brief Maps a TypeInfo to its BMKind category, or -1 if not a builtin type. */
int base_to_bmkind(const TypeInfo *base_t) {
  if (!base_t)
    return -1;
  if (base_t->kind == TY_ARRAY)
    return BMK_ARRAY;
  if (base_t->kind == TY_DICT)
    return BMK_DICT;
  if (base_t->kind == TY_SET)
    return BMK_SET;
  if (TY_BUILTIN_STRING && base_t->kind == TY_BUILTIN_STRING->kind)
    return BMK_STRING;
  if (TY_BUILTIN_SUBSTRING && base_t->kind == TY_BUILTIN_SUBSTRING->kind)
    return BMK_SUBSTRING;
  if (TY_BUILTIN_INT && base_t->kind == TY_BUILTIN_INT->kind)
    return BMK_INT;
  if (TY_BUILTIN_BOOL && base_t->kind == TY_BUILTIN_BOOL->kind)
    return BMK_BOOL;
  if (TY_BUILTIN_DOUBLE && base_t->kind == TY_BUILTIN_DOUBLE->kind)
    return BMK_DOUBLE;
  if (base_t->kind == TY_NAMED && base_t->named.name &&
      strcmp(base_t->named.name, "Character") == 0)
    return BMK_CHARACTER;
  if (base_t->kind == TY_OPTIONAL)
    return BMK_OPTIONAL;
  if (TY_BUILTIN_DATA && base_t->kind == TY_BUILTIN_DATA->kind)
    return BMK_DATA;
  return -1;
}

/**
 * @brief Looks up a member on a builtin type via the BUILTIN_MEMBERS table.
 *
 * @return The resolved return TypeInfo, or NULL if not a known builtin member.
 */
TypeInfo *lookup_builtin_member(SemaContext *ctx, TypeInfo *base_t,
                                const char *mname) {
  int bk = base_to_bmkind(base_t);
  if (bk < 0)
    return NULL;
  for (const BuiltinMemberEntry *e = BUILTIN_MEMBERS; e->name; e++) {
    if ((int)e->base_kind == bk && !strcmp(mname, e->name))
      return bmr_to_type(e->result, ctx, base_t);
  }
  return NULL;
}

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  LITERAL COERCION                                                      │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/* The canonical name used as the conformance-table key when asking whether a
 * scalar/named target can be initialised from a literal.  Returns NULL for
 * compound (`[Int]`, `Int?`, funcs) and unbound generic params, which are never
 * ExpressibleBy*Literal scalar targets. */
static const char *literal_target_canonical_name(const TypeInfo *t, char *buf,
                                                  size_t sz) {
  if (!t) return NULL;
  switch (t->kind) {
  case TY_GENERIC_PARAM:                     /* unbound `T` — not a real type */
    return NULL;
  case TY_NAMED:
    return t->named.name;                    /* e.g. "CGFloat" (from vocab)   */
  case TY_GENERIC_INST:
    return (t->generic.base && t->generic.base->kind == TY_NAMED)
               ? t->generic.base->named.name
               : NULL;
  default:
    break;
  }
  /* Primitive scalar kinds spell exactly to their conformance-table key. */
  if (is_integer_kind(t->kind) || is_float_kind(t->kind) ||
      t->kind == TY_BOOL || t->kind == TY_STRING)
    return type_to_string(t, buf, sz);
  return NULL;
}

/* 1 if a value of type `rt` is assignable to `lt` by subtyping: rt directly
 * inherits from or conforms to lt (lt may be Optional<Base>, accepting a subtype
 * of Base via optional promotion).  Uses the conformance table, which records
 * class inheritance + protocol conformance for builtin and vocab-loaded types.
 * Direct (non-transitive) — the O(1) hash query keeps the checkpoint cheap. */
int subtype_assignable(SemaContext *ctx, const TypeInfo *lt,
                       const TypeInfo *rt) {
  if (!ctx || !ctx->conformance_table || !lt || !rt) return 0;
  if (lt->kind == TY_OPTIONAL && lt->inner) lt = lt->inner; /* T? accepts T-sub */
  if (lt->kind != TY_NAMED || !lt->named.name) return 0;

  const char *sub = NULL;
  if (rt->kind == TY_NAMED)
    sub = rt->named.name;
  else if (rt->kind == TY_GENERIC_INST && rt->generic.base &&
           rt->generic.base->kind == TY_NAMED)
    sub = rt->generic.base->named.name;
  if (!sub) return 0;

  return conformance_table_has(ctx->conformance_table, sub, lt->named.name);
}

int literal_coerces_to(SemaContext *ctx, const ASTNode *lit,
                       const TypeInfo *target) {
  if (!ctx || !lit || !target) return 0;

  /* `nil` adopts any Optional regardless of conformance bookkeeping. */
  if (lit->kind == AST_NIL_LITERAL && target->kind == TY_OPTIONAL) return 1;

  const ConformanceTable *ct = ctx->conformance_table;
  if (!ct) return 0;

  char buf[64];
  const char *name = literal_target_canonical_name(target, buf, sizeof buf);
  if (!name) return 0;

  switch (lit->kind) {
  case AST_INTEGER_LITERAL:
    return conformance_table_has(ct, name, SW_PROTO_EXPR_BY_INT_LIT);
  case AST_FLOAT_LITERAL:
    return conformance_table_has(ct, name, SW_PROTO_EXPR_BY_FLOAT_LIT);
  case AST_STRING_LITERAL:
    return conformance_table_has(ct, name, SW_PROTO_EXPR_BY_STRING_LIT);
  case AST_BOOL_LITERAL:
    return conformance_table_has(ct, name, SW_PROTO_EXPR_BY_BOOL_LIT);
  case AST_NIL_LITERAL:
    return conformance_table_has(ct, name, SW_PROTO_EXPR_BY_NIL_LIT);
  default:
    return 0;
  }
}

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  WHOLE-MODULE CONFORMANCE WITNESS INDEX                                │
 * └──────────────────────────────────────────────────────────────────────────┘
 * A requirement is satisfied if the conforming type provides a witness named
 * for it ANYWHERE in the module: its own body, any `extension T {…}`, an
 * inherited (superclass / refined-protocol) member, or a protocol-extension
 * default `extension P {…}`.  pass 3 runs per-file, so this index is built once
 * over every file's top-level decls and answers the cross-file question. */

/* The witness member name of a member decl, or NULL if it isn't a member. */
static const char *witness_member_name(SemaContext *ctx, const ASTNode *m) {
  switch (m->kind) {
  case AST_FUNC_DECL:
    return m->data.func.name_tok
               ? tok_intern_at_node(ctx, m, m->data.func.name_tok)
               : NULL;
  case AST_VAR_DECL:
  case AST_LET_DECL:
  case AST_TYPEALIAS_DECL:
    return m->data.var.name_tok
               ? tok_intern_at_node(ctx, m, m->data.var.name_tok)
               : NULL;
  case AST_INIT_DECL:
    return "init";
  case AST_SUBSCRIPT_DECL:
    return "subscript";
  default:
    return NULL;
  }
}

void sema_witness_index_add_root(SemaContext *ctx, const ASTNode *root) {
  if (!ctx || !root) return;
  if (!ctx->witness_members)
    ctx->witness_members = calloc(1, sizeof(ConformanceTable));
  if (!ctx->witness_inherits)
    ctx->witness_inherits = calloc(1, sizeof(ConformanceTable));
  if (!ctx->witness_members || !ctx->witness_inherits) return;

  for (const ASTNode *d = root->first_child; d; d = d->next_sibling) {
    int is_nominal = d->kind == AST_STRUCT_DECL || d->kind == AST_CLASS_DECL ||
                     d->kind == AST_ENUM_DECL || d->kind == AST_ACTOR_DECL;
    int is_ext = d->kind == AST_EXTENSION_DECL;
    int is_proto = d->kind == AST_PROTOCOL_DECL;
    if ((!is_nominal && !is_ext && !is_proto) || !d->data.var.name_tok)
      continue;
    const char *tn = tok_intern_at_node(ctx, d, d->data.var.name_tok);
    if (!tn) continue;

    /* Inheritance/conformance clause → supertypes (nominal, extension, proto). */
    for (const ASTNode *c = d->first_child; c; c = c->next_sibling) {
      if (c->kind != AST_CONFORMANCE) continue;
      for (const ASTNode *p = c->first_child; p; p = p->next_sibling) {
        if (!p->tok_idx) continue;
        const char *sn = tok_intern_at_node(ctx, p, p->tok_idx);
        if (sn && !conformance_table_has(ctx->witness_inherits, tn, sn))
          conformance_table_add(ctx->witness_inherits, tn, sn);
      }
    }

    /* A protocol's body holds requirements, not witnesses — skip it (its
     * `extension P` defaults arrive via the AST_EXTENSION_DECL branch). */
    if (is_proto) continue;

    const ASTNode *body = NULL;
    for (const ASTNode *c = d->first_child; c; c = c->next_sibling)
      if (c->kind == AST_BLOCK) { body = c; break; }
    if (!body) continue;
    for (const ASTNode *m = body->first_child; m; m = m->next_sibling) {
      const char *mn = witness_member_name(ctx, m);
      if (mn && !conformance_table_has(ctx->witness_members, tn, mn))
        conformance_table_add(ctx->witness_members, tn, mn);
    }
  }
}

int witness_satisfies(SemaContext *ctx, const char *type_name,
                      const char *req_name) {
  if (!ctx || !ctx->witness_members || !type_name || !req_name) return 0;
  if (conformance_table_has(ctx->witness_members, type_name, req_name)) return 1;
  const ConformanceTable *inh = ctx->witness_inherits;
  if (!inh) return 0;

  /* Bounded BFS over the inheritance graph (chains are short; guard cycles). */
  const char *queue[64];
  int qh = 0, qt = 0;
  queue[qt++] = type_name;
  while (qh < qt) {
    const char *cur = queue[qh++];
    for (uint32_t i = 0; i < inh->count; i++) {
      if (!inh->entries[i].type_name || !inh->entries[i].protocol_name) continue;
      if (strcmp(inh->entries[i].type_name, cur) != 0) continue;
      const char *sup = inh->entries[i].protocol_name;
      if (conformance_table_has(ctx->witness_members, sup, req_name)) return 1;
      if (qt < 64) {
        int seen = 0;
        for (int k = 0; k < qt; k++)
          if (queue[k] == sup || strcmp(queue[k], sup) == 0) { seen = 1; break; }
        if (!seen) queue[qt++] = sup;
      }
    }
  }
  return 0;
}
