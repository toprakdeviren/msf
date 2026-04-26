/**
 * @file test_sema.c
 * @brief Unit tests for semantic analysis: intern pool, scopes, type resolution, optional binding.
 */
#include "test_framework.h"
#include "msf.h"
#include "internal/msf.h"

/* ── Helper: full pipeline (tokenize → parse → sema) ─────────────────────── */

typedef struct {
  Source src;
  TokenStream ts;
  ASTArena ast_arena;
  TypeArena type_arena;
  Parser *parser;
  SemaContext *sema;
  ASTNode *root;
} TestPipeline;

static int pipeline_run(TestPipeline *tp, const char *code) {
  tp->src.data = code;
  tp->src.len = strlen(code);
  tp->src.filename = "<test>";

  token_stream_init(&tp->ts, 256);
  lexer_tokenize(&tp->src, &tp->ts, 1, NULL);

  ast_arena_init(&tp->ast_arena, 0);
  tp->parser = parser_init(&tp->src, &tp->ts, &tp->ast_arena);
  tp->root = parse_source_file(tp->parser);

  type_arena_init(&tp->type_arena, 0);
  type_builtins_init(&tp->type_arena);

  tp->sema = sema_init(&tp->src, tp->ts.tokens, &tp->ast_arena, &tp->type_arena);
  return sema_analyze(tp->sema, tp->root);
}

static void pipeline_free(TestPipeline *tp) {
  sema_destroy(tp->sema);
  parser_destroy(tp->parser);
  type_arena_free(&tp->type_arena);
  ast_arena_free(&tp->ast_arena);
  token_stream_free(&tp->ts);
}

/* Find first descendant of given kind (BFS-ish, depth-first actually) */
static const ASTNode *find_desc(const ASTNode *node, ASTNodeKind kind) {
  if (!node) return NULL;
  if (node->kind == kind) return node;
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
    const ASTNode *found = find_desc(c, kind);
    if (found) return found;
  }
  return NULL;
}

/* ── Basic type resolution ────────────────────────────────────────────────── */

static void test_sema_var_int_type(void) {
  TEST("var x: Int = 42 → type is Int");
  TestPipeline tp = {0};
  pipeline_run(&tp, "var x: Int = 42");
  const ASTNode *var = find_desc(tp.root, AST_VAR_DECL);
  ASSERT_NOT_NULL(var);
  ASSERT_NOT_NULL(var->type);
  ASSERT_EQ(var->type, TY_BUILTIN_INT);
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_let_string_type(void) {
  TEST("let s = \"hello\" → type is String");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let s = \"hello\"");
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_NOT_NULL(let->type);
  ASSERT_EQ(let->type, TY_BUILTIN_STRING);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_bool_literal(void) {
  TEST("let b = true → type is Bool");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let b = true");
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_NOT_NULL(let->type);
  ASSERT_EQ(let->type, TY_BUILTIN_BOOL);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_float_literal(void) {
  TEST("let d = 3.14 → type is Double");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let d = 3.14");
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_NOT_NULL(let->type);
  ASSERT_EQ(let->type, TY_BUILTIN_DOUBLE);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_float_array_literal_default_double(void) {
  TEST("let arr = [1.0, 2.0, 3.0] → [Double]");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let arr = [1.0, 2.0, 3.0]");
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_NOT_NULL(let->type);
  ASSERT_EQ(let->type->kind, TY_ARRAY);
  ASSERT_EQ(let->type->inner, TY_BUILTIN_DOUBLE);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_float_array_annotated_float(void) {
  TEST("let arr: [Float] = [1.0, 2.0] → [Float] (annotation narrows)");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let arr: [Float] = [1.0, 2.0]");
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_NOT_NULL(let->type);
  ASSERT_EQ(let->type->kind, TY_ARRAY);
  ASSERT_EQ(let->type->inner, TY_BUILTIN_FLOAT);
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_float_array_annotated_double(void) {
  TEST("let arr: [Double] = [1.0, 2.0] → [Double]");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let arr: [Double] = [1.0, 2.0]");
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_NOT_NULL(let->type);
  ASSERT_EQ(let->type->kind, TY_ARRAY);
  ASSERT_EQ(let->type->inner, TY_BUILTIN_DOUBLE);
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_float_dict_value_default_double(void) {
  TEST("let d = [\"a\": 1.0] → [String: Double]");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let d = [\"a\": 1.0]");
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_NOT_NULL(let->type);
  ASSERT_EQ(let->type->kind, TY_DICT);
  ASSERT_EQ(let->type->dict.value, TY_BUILTIN_DOUBLE);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_float_array_annot_inner_literals_narrowed(void) {
  TEST("let arr: [Float] = [1.0, 2.0] → inner literals also Float (not Double)");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let arr: [Float] = [1.0, 2.0]");
  const ASTNode *arr = find_desc(tp.root, AST_ARRAY_LITERAL);
  ASSERT_NOT_NULL(arr);
  ASSERT_NOT_NULL(arr->type);
  /* Array literal node itself should reflect the annotation. */
  ASSERT_EQ(arr->type->kind, TY_ARRAY);
  ASSERT_EQ(arr->type->inner, TY_BUILTIN_FLOAT);
  /* Each child float literal must be narrowed to Float, not Double. */
  for (const ASTNode *c = arr->first_child; c; c = c->next_sibling) {
    ASSERT_EQ(c->kind, AST_FLOAT_LITERAL);
    ASSERT_EQ(c->type, TY_BUILTIN_FLOAT);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_float_var_annot_inner_literal_narrowed(void) {
  TEST("var x: Float = 3.14 → init literal also Float (not Double)");
  TestPipeline tp = {0};
  pipeline_run(&tp, "var x: Float = 3.14");
  const ASTNode *var = find_desc(tp.root, AST_VAR_DECL);
  ASSERT_NOT_NULL(var);
  ASSERT_EQ(var->type, TY_BUILTIN_FLOAT);
  /* The init literal child should also be narrowed. */
  const ASTNode *lit = find_desc(var, AST_FLOAT_LITERAL);
  ASSERT_NOT_NULL(lit);
  ASSERT_EQ(lit->type, TY_BUILTIN_FLOAT);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_float_dict_annot_inner_literals_narrowed(void) {
  TEST("let d: [String: Float] = [\"a\": 1.0] → inner value literal Float");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let d: [String: Float] = [\"a\": 1.0]");
  const ASTNode *dict = find_desc(tp.root, AST_DICT_LITERAL);
  ASSERT_NOT_NULL(dict);
  ASSERT_NOT_NULL(dict->type);
  ASSERT_EQ(dict->type->kind, TY_DICT);
  ASSERT_EQ(dict->type->dict.value, TY_BUILTIN_FLOAT);
  /* The value child (second) should be narrowed Float. */
  const ASTNode *value_lit = dict->first_child ? dict->first_child->next_sibling
                                                : NULL;
  ASSERT_NOT_NULL(value_lit);
  ASSERT_EQ(value_lit->kind, AST_FLOAT_LITERAL);
  ASSERT_EQ(value_lit->type, TY_BUILTIN_FLOAT);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Struct type resolution ───────────────────────────────────────────────── */

static void test_sema_struct_type(void) {
  TEST("struct Point { var x: Int } → type is TY_NAMED");
  TestPipeline tp = {0};
  pipeline_run(&tp, "struct Point { var x: Int }");
  const ASTNode *s = find_desc(tp.root, AST_STRUCT_DECL);
  ASSERT_NOT_NULL(s);
  ASSERT_NOT_NULL(s->type);
  ASSERT_EQ(s->type->kind, TY_NAMED);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Function type resolution ─────────────────────────────────────────────── */

static void test_sema_func_type(void) {
  TEST("func add(a: Int, b: Int) -> Int → type is TY_FUNC");
  TestPipeline tp = {0};
  pipeline_run(&tp, "func add(a: Int, b: Int) -> Int { return a }");
  const ASTNode *func = find_desc(tp.root, AST_FUNC_DECL);
  ASSERT_NOT_NULL(func);
  ASSERT_NOT_NULL(func->type);
  ASSERT_EQ(func->type->kind, TY_FUNC);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Optional type ────────────────────────────────────────────────────────── */

static void test_sema_optional_type(void) {
  TEST("var x: Int? → type is Optional<Int>");
  TestPipeline tp = {0};
  pipeline_run(&tp, "var x: Int?");
  const ASTNode *var = find_desc(tp.root, AST_VAR_DECL);
  ASSERT_NOT_NULL(var);
  ASSERT_NOT_NULL(var->type);
  ASSERT_EQ(var->type->kind, TY_OPTIONAL);
  ASSERT_EQ(var->type->inner, TY_BUILTIN_INT);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Array type ───────────────────────────────────────────────────────────── */

static void test_sema_array_type(void) {
  TEST("var a: [String] → type is Array<String>");
  TestPipeline tp = {0};
  pipeline_run(&tp, "var a: [String]");
  const ASTNode *var = find_desc(tp.root, AST_VAR_DECL);
  ASSERT_NOT_NULL(var);
  ASSERT_NOT_NULL(var->type);
  ASSERT_EQ(var->type->kind, TY_ARRAY);
  ASSERT_EQ(var->type->inner, TY_BUILTIN_STRING);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Dict type ────────────────────────────────────────────────────────────── */

static void test_sema_dict_type(void) {
  TEST("var d: [String: Int] → type is Dict<String, Int>");
  TestPipeline tp = {0};
  pipeline_run(&tp, "var d: [String: Int]");
  const ASTNode *var = find_desc(tp.root, AST_VAR_DECL);
  ASSERT_NOT_NULL(var);
  ASSERT_NOT_NULL(var->type);
  ASSERT_EQ(var->type->kind, TY_DICT);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Error detection ──────────────────────────────────────────────────────── */

static void test_sema_undeclared_type_error(void) {
  TEST("var x: Foo → error for undeclared type");
  TestPipeline tp = {0};
  pipeline_run(&tp, "var x: Foo");
  ASSERT(sema_error_count(tp.sema) > 0);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_no_errors_clean_code(void) {
  TEST("clean code → zero sema errors");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let x: Int = 10\nlet y: String = \"hi\"");
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Optional binding (if let) ────────────────────────────────────────────── */

static void test_sema_if_let_binding(void) {
  TEST("if let x = opt → x is bound with unwrapped type");
  TestPipeline tp = {0};
  /* var opt: Int? = 42; if let x = opt { } */
  pipeline_run(&tp, "var opt: Int? = 42\nif let x = opt { }");
  /* Should produce no "undeclared identifier" errors for x */
  int has_undeclared_x = 0;
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *msg = sema_error_message(tp.sema, i);
    if (strstr(msg, "undeclared") && strstr(msg, "'x'"))
      has_undeclared_x = 1;
  }
  ASSERT(!has_undeclared_x);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

void run_sema_tests(void) {
  TEST_SUITE("Semantic Analysis");
  test_sema_var_int_type();
  test_sema_let_string_type();
  test_sema_bool_literal();
  test_sema_float_literal();
  test_sema_float_array_literal_default_double();
  test_sema_float_array_annotated_float();
  test_sema_float_array_annotated_double();
  test_sema_float_dict_value_default_double();
  test_sema_float_var_annot_inner_literal_narrowed();
  test_sema_float_array_annot_inner_literals_narrowed();
  test_sema_float_dict_annot_inner_literals_narrowed();
  test_sema_struct_type();
  test_sema_func_type();
  test_sema_optional_type();
  test_sema_array_type();
  test_sema_dict_type();
  test_sema_undeclared_type_error();
  test_sema_no_errors_clean_code();
  test_sema_if_let_binding();
}
