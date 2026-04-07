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
      const char *chn = tok_intern(ctx, name_tok);
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
 *
 * Entry counts per type:
 *   Array:     29 entries (7 props + 22 methods)
 *   Optional:   2 entries (map, flatMap)
 *   String:    29 entries (11 props + 18 methods)
 *   Int:        3 entries (description, advanced, distance)
 *   Dict:       5 entries (count, keys, values, isEmpty, first)
 *   Set:       13 entries (2 props + 11 methods)
 *   Character: 13 entries (11 props + 2 methods)
 *   Hashable:   4 entries (hashValue stubs)
 *
 * Total: ~97 entries
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

    /* ── Data properties/methods ────────────────────────────────────────── */
    {BMK_DATA, "count", BMR_INT},
    {BMK_DATA, "isEmpty", BMR_BOOL},
    {BMK_DATA, "base64EncodedString", BMR_STRING},

    /* ── String properties ──────────────────────────────────────────────────── */
    {BMK_STRING, "count", BMR_INT},
    {BMK_STRING, "utf8", BMR_INT},
    {BMK_STRING, "utf16", BMR_INT},
    {BMK_STRING, "utf8Count", BMR_INT},
    {BMK_STRING, "startIndex", BMR_INT},
    {BMK_STRING, "endIndex", BMR_INT},
    /* View is lowered as String in IR; must not be Int or print() treats .count
       as ptr. */
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
    /* Substring operations */
    {BMK_STRING, "prefix", BMR_SUBSTRING},
    {BMK_STRING, "suffix", BMR_SUBSTRING},
    {BMK_STRING, "dropFirst", BMR_SUBSTRING},
    {BMK_STRING, "dropLast", BMR_SUBSTRING},
    /* ── String methods ─────────────────────────────────────────────────────── */
    {BMK_STRING, "hasPrefix", BMR_BOOL},
    {BMK_STRING, "hasSuffix", BMR_BOOL},
    {BMK_STRING, "contains", BMR_BOOL},
    {BMK_STRING, "trimmingCharacters", BMR_STRING},
    {BMK_STRING, "replacingOccurrences", BMR_STRING},
    {BMK_STRING, "map", BMR_ARRAY_UNKNOWN},
    {BMK_STRING, "filter", BMR_STRING},
    {BMK_STRING, "compactMap", BMR_ARRAY_UNKNOWN},
    {BMK_STRING, "appending", BMR_STRING},
    {BMK_STRING, "components", BMR_ARRAY_STRING},
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

    /* ── Date (typed as Double) components ──────────────────────────────────── */
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
