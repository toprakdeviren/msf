/**
 * @file equal.c
 * @brief Type equality — structural comparison of TypeInfo trees.
 *
 * Two public entry points:
 *   type_equal()      — shallow: TY_GENERIC_PARAM matches by name only
 *   type_equal_deep() — deep: also compares param.index
 *
 * Both delegate to a single type_cmp() implementation parameterized
 * by a `deep` flag.  Comparison is recursive and structural — two
 * types are equal if they have the same kind and all sub-components
 * are equal.  Primitive types (Int, Bool, ...) are equal if their
 * kind matches (they're singletons).
 */

#include "internal/type.h"

/**
 * @brief Recursive structural comparison of two types.
 *
 * Returns 1 if the types are structurally equal, 0 otherwise.
 * NULL types are equal only if both are NULL.
 *
 * @param a     First type (may be NULL).
 * @param b     Second type (may be NULL).
 * @param deep  If 1, also compare TY_GENERIC_PARAM index fields.
 * @return      1 if equal, 0 otherwise.
 */
static int type_cmp(const TypeInfo *a, const TypeInfo *b, int deep) {
  if (!a || !b)
    return a == b;
  if (a->kind != b->kind)
    return 0;
  switch (a->kind) {
  case TY_ARRAY:
  case TY_OPTIONAL:
  case TY_SET:
    return type_cmp(a->inner, b->inner, deep);
  case TY_NAMED:
    return a->named.name == b->named.name; /* interned: pointer equality */
  case TY_GENERIC_PARAM:
    if (!a->param.name || !b->param.name)
      return 0;
    if (strcmp(a->param.name, b->param.name) != 0)
      return 0;
    return deep ? (a->param.index == b->param.index) : 1;
  case TY_GENERIC_INST: {
    if (a->generic.arg_count != b->generic.arg_count)
      return 0;
    if (!type_cmp(a->generic.base, b->generic.base, deep))
      return 0;
    for (uint32_t i = 0; i < a->generic.arg_count; i++)
      if (!type_cmp(a->generic.args[i], b->generic.args[i], deep))
        return 0;
    return 1;
  }
  case TY_ASSOC_REF:
    return a->assoc_ref.param_name && b->assoc_ref.param_name &&
           a->assoc_ref.assoc_name && b->assoc_ref.assoc_name &&
           strcmp(a->assoc_ref.param_name, b->assoc_ref.param_name) == 0 &&
           strcmp(a->assoc_ref.assoc_name, b->assoc_ref.assoc_name) == 0;
  case TY_PROTOCOL_COMPOSITION:
    if (a->composition.protocol_count != b->composition.protocol_count)
      return 0;
    for (uint32_t i = 0; i < a->composition.protocol_count; i++)
      if (!type_cmp(a->composition.protocols[i], b->composition.protocols[i], deep))
        return 0;
    return 1;
  case TY_FUNC: {
    if (a->func.param_count != b->func.param_count)
      return 0;
    if (a->func.is_async != b->func.is_async || a->func.throws != b->func.throws)
      return 0;
    if (!type_cmp(a->func.ret, b->func.ret, deep))
      return 0;
    for (size_t i = 0; i < a->func.param_count; i++)
      if (!type_cmp(a->func.params[i], b->func.params[i], deep))
        return 0;
    return 1;
  }
  case TY_DICT:
    return type_cmp(a->dict.key, b->dict.key, deep) &&
           type_cmp(a->dict.value, b->dict.value, deep);
  case TY_TUPLE: {
    if (a->tuple.elem_count != b->tuple.elem_count)
      return 0;
    for (size_t i = 0; i < a->tuple.elem_count; i++)
      if (!type_cmp(a->tuple.elems[i], b->tuple.elems[i], deep))
        return 0;
    return 1;
  }
  default:
    return 1; /* primitive types: kind match is sufficient */
  }
}

/**
 * @brief Shallow type equality — compares structure, not generic param indices.
 *
 * Two TY_GENERIC_PARAM types with the same name but different indices
 * are considered equal.  Use type_equal_deep() when index matters.
 *
 * @param a  First type (may be NULL).
 * @param b  Second type (may be NULL).
 * @return   1 if structurally equal, 0 otherwise.
 */
int type_equal(const TypeInfo *a, const TypeInfo *b) {
  return type_cmp(a, b, 0);
}

/**
 * @brief Deep type equality — also compares generic parameter indices.
 *
 * Identical to type_equal() except TY_GENERIC_PARAM nodes must also
 * have matching param.index values.
 *
 * @param a  First type (may be NULL).
 * @param b  Second type (may be NULL).
 * @return   1 if deeply equal, 0 otherwise.
 */
int type_equal_deep(const TypeInfo *a, const TypeInfo *b) {
  return type_cmp(a, b, 1);
}
