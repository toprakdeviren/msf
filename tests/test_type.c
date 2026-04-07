/**
 * @file test_type.c
 * @brief Unit tests for the type system: arena, builtins, equality, substitution, stringification.
 */
#include "test_framework.h"
#include "msf.h"
#include "internal/msf.h"

static TypeArena g_arena;

static void setup_type_arena(void) {
  type_arena_init(&g_arena, 0);
  type_builtins_init(&g_arena);
}

static void teardown_type_arena(void) {
  type_arena_free(&g_arena);
}

/* ── Arena ────────────────────────────────────────────────────────────────── */

static void test_arena_alloc(void) {
  TEST("type_arena_alloc returns non-NULL zeroed");
  TypeInfo *ti = type_arena_alloc(&g_arena);
  ASSERT_NOT_NULL(ti);
  ASSERT_EQ(ti->kind, TY_UNKNOWN);
  TEST_PASS();
}

static void test_arena_many_allocs(void) {
  TEST("arena handles >1024 allocs (crosses chunk boundary)");
  for (int i = 0; i < 2000; i++) {
    TypeInfo *ti = type_arena_alloc(&g_arena);
    ASSERT_NOT_NULL(ti);
  }
  TEST_PASS();
}

/* ── Builtins ─────────────────────────────────────────────────────────────── */

static void test_builtins_initialized(void) {
  TEST("builtin singletons are non-NULL after init");
  ASSERT_NOT_NULL(TY_BUILTIN_VOID);
  ASSERT_NOT_NULL(TY_BUILTIN_BOOL);
  ASSERT_NOT_NULL(TY_BUILTIN_INT);
  ASSERT_NOT_NULL(TY_BUILTIN_STRING);
  ASSERT_NOT_NULL(TY_BUILTIN_DOUBLE);
  ASSERT_NOT_NULL(TY_BUILTIN_FLOAT);
  TEST_PASS();
}

static void test_builtin_kinds(void) {
  TEST("builtin singletons have correct kinds");
  ASSERT_EQ(TY_BUILTIN_VOID->kind, TY_VOID);
  ASSERT_EQ(TY_BUILTIN_INT->kind, TY_INT);
  ASSERT_EQ(TY_BUILTIN_STRING->kind, TY_STRING);
  ASSERT_EQ(TY_BUILTIN_BOOL->kind, TY_BOOL);
  ASSERT_EQ(TY_BUILTIN_DOUBLE->kind, TY_DOUBLE);
  TEST_PASS();
}

static void test_type_kind_of(void) {
  TEST("type_kind_of resolves singletons correctly");
  ASSERT_EQ(type_kind_of(NULL), TY_VOID);
  ASSERT_EQ(type_kind_of(TY_BUILTIN_INT), TY_INT);
  ASSERT_EQ(type_kind_of(TY_BUILTIN_STRING), TY_STRING);
  ASSERT_EQ(type_kind_of(TY_BUILTIN_BOOL), TY_BOOL);
  TEST_PASS();
}

/* ── Equality ─────────────────────────────────────────────────────────────── */

static void test_equal_same_builtin(void) {
  TEST("type_equal: same builtin → 1");
  ASSERT(type_equal(TY_BUILTIN_INT, TY_BUILTIN_INT));
  TEST_PASS();
}

static void test_equal_different_builtin(void) {
  TEST("type_equal: different builtin → 0");
  ASSERT(!type_equal(TY_BUILTIN_INT, TY_BUILTIN_STRING));
  TEST_PASS();
}

static void test_equal_null(void) {
  TEST("type_equal: NULL == NULL → 1");
  ASSERT(type_equal(NULL, NULL));
  TEST_PASS();
}

static void test_equal_optional(void) {
  TEST("type_equal: Optional<Int> == Optional<Int>");
  TypeInfo *a = type_arena_alloc(&g_arena);
  a->kind = TY_OPTIONAL;
  a->inner = TY_BUILTIN_INT;
  TypeInfo *b = type_arena_alloc(&g_arena);
  b->kind = TY_OPTIONAL;
  b->inner = TY_BUILTIN_INT;
  ASSERT(type_equal(a, b));
  TEST_PASS();
}

static void test_equal_array(void) {
  TEST("type_equal: [String] == [String]");
  TypeInfo *a = type_arena_alloc(&g_arena);
  a->kind = TY_ARRAY;
  a->inner = TY_BUILTIN_STRING;
  TypeInfo *b = type_arena_alloc(&g_arena);
  b->kind = TY_ARRAY;
  b->inner = TY_BUILTIN_STRING;
  ASSERT(type_equal(a, b));
  TEST_PASS();
}

static void test_not_equal_array_different_inner(void) {
  TEST("type_equal: [Int] != [String]");
  TypeInfo *a = type_arena_alloc(&g_arena);
  a->kind = TY_ARRAY;
  a->inner = TY_BUILTIN_INT;
  TypeInfo *b = type_arena_alloc(&g_arena);
  b->kind = TY_ARRAY;
  b->inner = TY_BUILTIN_STRING;
  ASSERT(!type_equal(a, b));
  TEST_PASS();
}

/* ── type_to_string ───────────────────────────────────────────────────────── */

static void test_to_string_builtins(void) {
  TEST("type_to_string: Int, String, Bool, Void");
  char buf[64];
  ASSERT_STR_EQ(type_to_string(TY_BUILTIN_INT, buf, sizeof(buf)), "Int");
  ASSERT_STR_EQ(type_to_string(TY_BUILTIN_STRING, buf, sizeof(buf)), "String");
  ASSERT_STR_EQ(type_to_string(TY_BUILTIN_BOOL, buf, sizeof(buf)), "Bool");
  ASSERT_STR_EQ(type_to_string(TY_BUILTIN_VOID, buf, sizeof(buf)), "Void");
  TEST_PASS();
}

static void test_to_string_optional(void) {
  TEST("type_to_string: Int? → \"Int?\"");
  TypeInfo *opt = type_arena_alloc(&g_arena);
  opt->kind = TY_OPTIONAL;
  opt->inner = TY_BUILTIN_INT;
  char buf[64];
  ASSERT_STR_EQ(type_to_string(opt, buf, sizeof(buf)), "Int?");
  TEST_PASS();
}

static void test_to_string_array(void) {
  TEST("type_to_string: [String] → \"[String]\"");
  TypeInfo *arr = type_arena_alloc(&g_arena);
  arr->kind = TY_ARRAY;
  arr->inner = TY_BUILTIN_STRING;
  char buf[64];
  ASSERT_STR_EQ(type_to_string(arr, buf, sizeof(buf)), "[String]");
  TEST_PASS();
}

static void test_to_string_dict(void) {
  TEST("type_to_string: [String: Int] → \"[String: Int]\"");
  TypeInfo *dict = type_arena_alloc(&g_arena);
  dict->kind = TY_DICT;
  dict->dict.key = TY_BUILTIN_STRING;
  dict->dict.value = TY_BUILTIN_INT;
  char buf[64];
  ASSERT_STR_EQ(type_to_string(dict, buf, sizeof(buf)), "[String: Int]");
  TEST_PASS();
}

static void test_to_string_func(void) {
  TEST("type_to_string: (Int) -> String");
  TypeInfo *func = type_arena_alloc(&g_arena);
  func->kind = TY_FUNC;
  TypeInfo **params = malloc(sizeof(TypeInfo *));
  params[0] = TY_BUILTIN_INT;
  func->func.params = params;
  func->func.param_count = 1;
  func->func.ret = TY_BUILTIN_STRING;
  char buf[64];
  type_to_string(func, buf, sizeof(buf));
  /* Should contain "Int" and "String" */
  ASSERT(strstr(buf, "Int") != NULL);
  ASSERT(strstr(buf, "String") != NULL);
  ASSERT(strstr(buf, "->") != NULL);
  TEST_PASS();
}

static void test_to_string_null(void) {
  TEST("type_to_string: NULL → \"nil\"");
  char buf[64];
  ASSERT_STR_EQ(type_to_string(NULL, buf, sizeof(buf)), "nil");
  TEST_PASS();
}

/* ── Substitution ─────────────────────────────────────────────────────────── */

static void test_substitute_identity(void) {
  TEST("type_substitute: no match → same pointer");
  TypeSubstitution sub = {0};
  type_sub_set(&sub, "T", TY_BUILTIN_INT);
  /* Substituting a type with no generic params → unchanged */
  TypeInfo *result = type_substitute(TY_BUILTIN_STRING, &sub, &g_arena);
  ASSERT_EQ(result, TY_BUILTIN_STRING);
  TEST_PASS();
}

static void test_substitute_generic_param(void) {
  TEST("type_substitute: T → Int");
  TypeInfo *param = type_arena_alloc(&g_arena);
  param->kind = TY_GENERIC_PARAM;
  param->param.name = "T";
  TypeSubstitution sub = {0};
  type_sub_set(&sub, "T", TY_BUILTIN_INT);
  TypeInfo *result = type_substitute(param, &sub, &g_arena);
  ASSERT_EQ(result, TY_BUILTIN_INT);
  TEST_PASS();
}

static void test_substitute_optional(void) {
  TEST("type_substitute: T? → Int?");
  TypeInfo *param = type_arena_alloc(&g_arena);
  param->kind = TY_GENERIC_PARAM;
  param->param.name = "T";
  TypeInfo *opt = type_arena_alloc(&g_arena);
  opt->kind = TY_OPTIONAL;
  opt->inner = param;
  TypeSubstitution sub = {0};
  type_sub_set(&sub, "T", TY_BUILTIN_INT);
  TypeInfo *result = type_substitute(opt, &sub, &g_arena);
  ASSERT_NOT_NULL(result);
  ASSERT_EQ(result->kind, TY_OPTIONAL);
  ASSERT_EQ(result->inner, TY_BUILTIN_INT);
  TEST_PASS();
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

void run_type_tests(void) {
  TEST_SUITE("Type System");
  setup_type_arena();
  test_arena_alloc();
  test_arena_many_allocs();
  test_builtins_initialized();
  test_builtin_kinds();
  test_type_kind_of();
  test_equal_same_builtin();
  test_equal_different_builtin();
  test_equal_null();
  test_equal_optional();
  test_equal_array();
  test_not_equal_array_different_inner();
  test_to_string_builtins();
  test_to_string_optional();
  test_to_string_array();
  test_to_string_dict();
  test_to_string_func();
  test_to_string_null();
  test_substitute_identity();
  test_substitute_generic_param();
  test_substitute_optional();
  teardown_type_arena();
}
