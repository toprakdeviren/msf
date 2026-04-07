/**
 * @file str.c
 * @brief Type stringification — human-readable type representations.
 *
 * Converts a TypeInfo tree into a string like "Int", "[String: Any]",
 * "(Int, String) async throws -> Bool", etc.  Used by error messages,
 * debug dumps, and the public API.
 *
 * The output is written into a caller-provided buffer.  Recursive types
 * (Optional<Array<T>>) use stack-allocated sub-buffers — no heap allocation.
 */

#include "internal/type.h"
#include <stdio.h>
#include <string.h>

/**
 * @brief Produces a human-readable representation of a type.
 *
 * Recursively formats the type into the provided buffer.  Handles all
 * TypeKind values including compound types (func, tuple, dict, generic).
 *
 * Returns @p buf for convenience (allows inline use in printf).
 * On NULL input, writes "nil".
 *
 * @param t    Type to stringify (may be NULL).
 * @param buf  Output buffer.
 * @param sz   Buffer size in bytes.
 * @return     @p buf (same pointer passed in).
 */
const char *type_to_string(const TypeInfo *t, char *buf, size_t sz) {
  if (!t) {
    snprintf(buf, sz, "nil");
    return buf;
  }
  switch (t->kind) {
    /* @generated primitive cases (scripts/codegen.py) */
  case TY_VOID:        snprintf(buf, sz, "Void"); break;
  case TY_BOOL:        snprintf(buf, sz, "Bool"); break;
  case TY_INT:         snprintf(buf, sz, "Int"); break;
  case TY_INT8:        snprintf(buf, sz, "Int8"); break;
  case TY_INT16:       snprintf(buf, sz, "Int16"); break;
  case TY_INT32:       snprintf(buf, sz, "Int32"); break;
  case TY_INT64:       snprintf(buf, sz, "Int64"); break;
  case TY_UINT:        snprintf(buf, sz, "UInt"); break;
  case TY_UINT8:       snprintf(buf, sz, "UInt8"); break;
  case TY_UINT16:      snprintf(buf, sz, "UInt16"); break;
  case TY_UINT32:      snprintf(buf, sz, "UInt32"); break;
  case TY_UINT64:      snprintf(buf, sz, "UInt64"); break;
  case TY_FLOAT:       snprintf(buf, sz, "Float"); break;
  case TY_DOUBLE:      snprintf(buf, sz, "Double"); break;
  case TY_STRING:      snprintf(buf, sz, "String"); break;
  case TY_CHARACTER:   snprintf(buf, sz, "Character"); break;
  case TY_JSONENCODER: snprintf(buf, sz, "JSONEncoder"); break;
  case TY_JSONDECODER: snprintf(buf, sz, "JSONDecoder"); break;
  case TY_DATA:        snprintf(buf, sz, "Data"); break;
  case TY_SUBSTRING:   snprintf(buf, sz, "Substring"); break;

  case TY_OPTIONAL: {
    char inner[128];
    type_to_string(t->inner, inner, sizeof(inner));
    snprintf(buf, sz, "%s?", inner);
    break;
  }
  case TY_ARRAY: {
    char inner[128];
    type_to_string(t->inner, inner, sizeof(inner));
    if (t->array_fixed_len > 0)
      snprintf(buf, sz, "[%u of %s]", (unsigned)t->array_fixed_len, inner);
    else
      snprintf(buf, sz, "[%s]", inner);
    break;
  }
  case TY_SET: {
    char inner[128];
    type_to_string(t->inner, inner, sizeof(inner));
    snprintf(buf, sz, "Set<%s>", inner);
    break;
  }
  case TY_DICT: {
    char ks[128], vs[128];
    type_to_string(t->dict.key, ks, sizeof(ks));
    type_to_string(t->dict.value, vs, sizeof(vs));
    snprintf(buf, sz, "[%s: %s]", ks, vs);
    break;
  }
  case TY_TUPLE: {
    char *p = buf;
    size_t remain = sz;
    size_t n = (size_t)snprintf(p, remain, "(");
    p += n; remain -= (n < remain ? n : remain);
    for (size_t i = 0; i < t->tuple.elem_count && remain > 1; i++) {
      char elem[128];
      type_to_string(t->tuple.elems[i], elem, sizeof(elem));
      const char *label = (t->tuple.labels && t->tuple.labels[i])
                              ? t->tuple.labels[i] : NULL;
      if (label)
        n = (size_t)snprintf(p, remain, "%s%s: %s", i ? ", " : "", label, elem);
      else
        n = (size_t)snprintf(p, remain, "%s%s", i ? ", " : "", elem);
      if (n >= remain) break;
      p += n; remain -= n;
    }
    snprintf(p, remain, ")");
    break;
  }
  case TY_FUNC: {
    char *p = buf;
    size_t remain = sz;
    size_t n = (size_t)snprintf(p, remain, "(");
    p += n; remain -= (n < remain ? n : remain);
    for (size_t i = 0; i < t->func.param_count && remain > 1; i++) {
      char param[128];
      type_to_string(t->func.params[i], param, sizeof(param));
      n = (size_t)snprintf(p, remain, "%s%s", i ? ", " : "", param);
      if (n >= remain) break;
      p += n; remain -= n;
    }
    char ret[128];
    type_to_string(t->func.ret, ret, sizeof(ret));
    snprintf(p, remain, ") %s%s-> %s",
             t->func.is_async ? "async " : "",
             t->func.throws ? "throws " : "",
             ret);
    break;
  }
  case TY_METATYPE: {
    char inner[128];
    type_to_string(t->inner, inner, sizeof(inner));
    snprintf(buf, sz, "%s.Type", inner);
    break;
  }
  case TY_NAMED:
    snprintf(buf, sz, "%s", t->named.name ? t->named.name : "?");
    break;
  case TY_ERROR:
    snprintf(buf, sz, "<error>");
    break;
  case TY_GENERIC_PARAM:
    snprintf(buf, sz, "%s", t->param.name ? t->param.name : "?");
    break;
  case TY_GENERIC_INST: {
    char base_s[64];
    type_to_string(t->generic.base, base_s, sizeof(base_s));
    char args_s[256] = "";
    for (uint32_t i = 0; i < t->generic.arg_count; i++) {
      char arg_s[64];
      type_to_string(t->generic.args[i], arg_s, sizeof(arg_s));
      if (i > 0)
        strncat(args_s, ", ", sizeof(args_s) - strlen(args_s) - 1);
      strncat(args_s, arg_s, sizeof(args_s) - strlen(args_s) - 1);
    }
    snprintf(buf, sz, "%s<%s>", base_s, args_s);
    break;
  }
  case TY_ASSOC_REF:
    snprintf(buf, sz, "%s.%s",
             t->assoc_ref.param_name ? t->assoc_ref.param_name : "?",
             t->assoc_ref.assoc_name ? t->assoc_ref.assoc_name : "?");
    break;
  case TY_PROTOCOL_COMPOSITION: {
    char *p = buf;
    size_t remain = sz;
    for (uint32_t i = 0; i < t->composition.protocol_count && remain > 1; i++) {
      char part[64];
      type_to_string(t->composition.protocols[i], part, sizeof(part));
      size_t n = (size_t)snprintf(p, remain, "%s%s", i ? " & " : "", part);
      if (n >= remain) break;
      p += n;
      remain -= n;
    }
    break;
  }
  default:
    snprintf(buf, sz, "?");
    break;
  }
  return buf;
}
