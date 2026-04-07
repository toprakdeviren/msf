/**
 * @file sub.c
 * @brief Generic type substitution — replaces type parameters with concrete types.
 *
 * When the sema resolves a generic call like `Array<Int>`, it builds a
 * substitution map (T -> Int) and applies it to the generic type's body.
 * This file provides that mechanism:
 *
 *   type_sub_set()    — add a (param_name -> concrete) mapping
 *   type_sub_lookup() — find the concrete type for a param name
 *   type_substitute() — recursively apply the map to a type tree
 *
 * type_substitute() is non-destructive: if no substitution applies, the
 * original pointer is returned unchanged.  New TypeInfo values are only
 * allocated (from the arena) when something actually changes.
 */

#include "internal/type.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Adds or overwrites a (param -> concrete) mapping in the substitution.
 *
 * If @p param already exists, its concrete type is updated in-place.
 * Otherwise a new entry is appended (up to TYPE_SUB_MAX entries).
 *
 * @param sub       Substitution map.
 * @param param     Generic parameter name (e.g. "T").
 * @param concrete  Concrete type to substitute (e.g. TY_BUILTIN_INT).
 */
void type_sub_set(TypeSubstitution *sub, const char *param,
                  TypeInfo *concrete) {
  for (uint32_t i = 0; i < sub->count; i++) {
    if (sub->entries[i].param_name &&
        strcmp(sub->entries[i].param_name, param) == 0) {
      sub->entries[i].concrete = concrete;
      return;
    }
  }
  if (sub->count < TYPE_SUB_MAX) {
    sub->entries[sub->count].param_name = param;
    sub->entries[sub->count].concrete = concrete;
    sub->count++;
  }
}

/**
 * @brief Looks up the concrete type for a generic parameter name.
 *
 * @param sub         Substitution map (may be NULL).
 * @param param_name  Parameter name to look up.
 * @return            Concrete type, or NULL if not found.
 */
TypeInfo *type_sub_lookup(const TypeSubstitution *sub, const char *param_name) {
  if (!sub || !param_name)
    return NULL;
  for (uint32_t i = 0; i < sub->count; i++) {
    if (sub->entries[i].param_name &&
        strcmp(sub->entries[i].param_name, param_name) == 0)
      return sub->entries[i].concrete;
  }
  return NULL;
}

/**
 * @brief Recursively replaces generic type parameters with concrete types.
 *
 * Walks the type tree and applies substitutions from @p sub.  Only allocates
 * new TypeInfo values (from @p arena) when a substitution actually changes
 * something — unchanged subtrees return the original pointer.
 *
 * Handles all compound types: Optional, Array, Set, Dict, Func, Tuple,
 * GenericInst.  TY_GENERIC_PARAM and TY_NAMED nodes are checked directly
 * against the substitution map.
 *
 * Deep-copies TY_TUPLE labels to avoid double-free when both original and
 * substituted tuples are freed by type_arena_free().
 *
 * @param ty     Type to substitute (may be NULL — returns NULL).
 * @param sub    Substitution map (may be NULL — returns ty unchanged).
 * @param arena  Arena for allocating new TypeInfo values.
 * @return       Substituted type, or original pointer if unchanged, or NULL on OOM.
 */
TypeInfo *type_substitute(const TypeInfo *ty, const TypeSubstitution *sub,
                          TypeArena *arena) {
  if (!ty || !sub)
    return (TypeInfo *)ty;

  switch (ty->kind) {

  case TY_GENERIC_PARAM: {
    TypeInfo *found = type_sub_lookup(sub, ty->param.name);
    return found ? found : (TypeInfo *)ty;
  }

  case TY_NAMED: {
    if (ty->named.name) {
      TypeInfo *found = type_sub_lookup(sub, ty->named.name);
      if (found)
        return found;
    }
    return (TypeInfo *)ty;
  }

  case TY_OPTIONAL:
  case TY_ARRAY:
  case TY_SET: {
    TypeInfo *new_inner = type_substitute(ty->inner, sub, arena);
    if (new_inner == ty->inner)
      return (TypeInfo *)ty; /* unchanged */
    TypeInfo *result = type_arena_alloc(arena);
    if (!result)
      return NULL;
    *result = *ty;
    result->inner = new_inner;
    return result;
  }

  case TY_DICT: {
    TypeInfo *new_key = type_substitute(ty->dict.key, sub, arena);
    TypeInfo *new_val = type_substitute(ty->dict.value, sub, arena);
    if (new_key == ty->dict.key && new_val == ty->dict.value)
      return (TypeInfo *)ty;
    TypeInfo *result = type_arena_alloc(arena);
    if (!result)
      return NULL;
    *result = *ty;
    result->dict.key = new_key;
    result->dict.value = new_val;
    return result;
  }

  case TY_FUNC: {
    TypeInfo *new_ret = type_substitute(ty->func.ret, sub, arena);
    int changed = (new_ret != ty->func.ret);
    TypeInfo **new_params = (TypeInfo **)ty->func.params;
    if (ty->func.param_count > 0) {
      TypeInfo **tmp = malloc(ty->func.param_count * sizeof(TypeInfo *));
      if (!tmp)
        return NULL;
      for (size_t i = 0; i < ty->func.param_count; i++) {
        tmp[i] = type_substitute(ty->func.params[i], sub, arena);
        if (tmp[i] != ty->func.params[i])
          changed = 1;
      }
      if (changed)
        new_params = tmp;
      else
        free(tmp);
    }
    if (!changed)
      return (TypeInfo *)ty;
    TypeInfo *result = type_arena_alloc(arena);
    if (!result) {
      if (new_params != ty->func.params)
        free(new_params);
      return NULL;
    }
    *result = *ty;
    result->func.params = new_params;
    result->func.ret = new_ret;
    return result;
  }

  case TY_GENERIC_INST: {
    int changed = 0;
    TypeInfo **new_args = malloc(ty->generic.arg_count * sizeof(TypeInfo *));
    if (!new_args)
      return NULL;
    for (uint32_t i = 0; i < ty->generic.arg_count; i++) {
      new_args[i] = type_substitute(ty->generic.args[i], sub, arena);
      if (new_args[i] != ty->generic.args[i])
        changed = 1;
    }
    if (!changed) {
      free(new_args);
      return (TypeInfo *)ty;
    }
    TypeInfo *result = type_arena_alloc(arena);
    if (!result) {
      free(new_args);
      return NULL;
    }
    *result = *ty;
    result->generic.args = new_args;
    return result;
  }

  case TY_TUPLE: {
    if (ty->tuple.elem_count == 0)
      return (TypeInfo *)ty;
    int changed = 0;
    TypeInfo **new_elems = malloc(ty->tuple.elem_count * sizeof(TypeInfo *));
    if (!new_elems)
      return NULL;
    for (size_t i = 0; i < ty->tuple.elem_count; i++) {
      new_elems[i] = type_substitute(ty->tuple.elems[i], sub, arena);
      if (new_elems[i] != ty->tuple.elems[i])
        changed = 1;
    }
    if (!changed) {
      free(new_elems);
      return (TypeInfo *)ty;
    }
    TypeInfo *result = type_arena_alloc(arena);
    if (!result) { free(new_elems); return NULL; }
    *result = *ty;
    result->tuple.elems = new_elems;
    /* Deep-copy labels to avoid double-free when both original and substituted
       tuple are freed by type_arena_free(). */
    if (ty->tuple.labels && ty->tuple.elem_count > 0) {
      const char **new_labels = malloc(ty->tuple.elem_count * sizeof(const char *));
      if (new_labels)
        memcpy(new_labels, ty->tuple.labels, ty->tuple.elem_count * sizeof(const char *));
      result->tuple.labels = new_labels;
    }
    return result;
  }

  default:
    return (TypeInfo *)ty;
  }
}
