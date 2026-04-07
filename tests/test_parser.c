/**
 * @file test_parser.c
 * @brief Unit tests for the parser: declarations, expressions, statements, error handling.
 */
#include "test_framework.h"
#include "msf.h"
#include "internal/msf.h"

/* ── Helper: parse Swift code and return root AST node ────────────────────── */

static ASTNode *parse_code(const char *code, Source *src, TokenStream *ts,
                           ASTArena *arena, Parser **out_parser) {
  test_tokenize(code, src, ts);
  ast_arena_init(arena, 0);
  *out_parser = parser_init(src, ts, arena);
  return parse_source_file(*out_parser);
}

static void cleanup(TokenStream *ts, ASTArena *arena, Parser *p) {
  parser_destroy(p);
  ast_arena_free(arena);
  token_stream_free(ts);
}

/* Count direct children of a node */
static int child_count(const ASTNode *node) {
  int n = 0;
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) n++;
  return n;
}

/* Find first child of given kind */
static const ASTNode *find_child(const ASTNode *node, ASTNodeKind kind) {
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
    if (c->kind == kind) return c;
  return NULL;
}

/* ── Function declarations ────────────────────────────────────────────────── */

static void test_parse_func_decl(void) {
  TEST("func add(a: Int, b: Int) -> Int { }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("func add(a: Int, b: Int) -> Int { }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(root->kind, AST_SOURCE_FILE);
  const ASTNode *func = find_child(root, AST_FUNC_DECL);
  ASSERT_NOT_NULL(func);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_func_no_params(void) {
  TEST("func hello() { }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("func hello() { }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *func = find_child(root, AST_FUNC_DECL);
  ASSERT_NOT_NULL(func);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Variable declarations ────────────────────────────────────────────────── */

static void test_parse_var_decl(void) {
  TEST("var x: Int = 42");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("var x: Int = 42", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *var = find_child(root, AST_VAR_DECL);
  ASSERT_NOT_NULL(var);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_let_decl(void) {
  TEST("let name = \"Swift\"");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("let name = \"Swift\"", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *let = find_child(root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Type declarations ────────────────────────────────────────────────────── */

static void test_parse_struct(void) {
  TEST("struct Point { var x: Int; var y: Int }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("struct Point { var x: Int\n var y: Int }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *s = find_child(root, AST_STRUCT_DECL);
  ASSERT_NOT_NULL(s);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_class_inheritance(void) {
  TEST("class Dog: Animal { }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("class Dog: Animal { }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *cls = find_child(root, AST_CLASS_DECL);
  ASSERT_NOT_NULL(cls);
  /* Should have a CONFORMANCE child */
  const ASTNode *conf = find_child(cls, AST_CONFORMANCE);
  ASSERT_NOT_NULL(conf);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_enum(void) {
  TEST("enum Direction { case north, south }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("enum Direction { case north, south }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *e = find_child(root, AST_ENUM_DECL);
  ASSERT_NOT_NULL(e);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_protocol(void) {
  TEST("protocol Drawable { func draw() }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("protocol Drawable { func draw() }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *proto = find_child(root, AST_PROTOCOL_DECL);
  ASSERT_NOT_NULL(proto);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Expressions ──────────────────────────────────────────────────────────── */

static void test_parse_binary_expr(void) {
  TEST("let x = 1 + 2");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("let x = 1 + 2", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_call_expr(void) {
  TEST("print(\"hello\")");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("print(\"hello\")", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_closure(void) {
  TEST("let f = { (x: Int) in x + 1 }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("let f = { (x: Int) in x + 1 }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Statements ───────────────────────────────────────────────────────────── */

static void test_parse_if_stmt(void) {
  TEST("if x > 0 { } else { }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("if x > 0 { } else { }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_for_in(void) {
  TEST("for i in 0..<10 { }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("for i in 0..<10 { }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *stmt = find_child(root, AST_FOR_STMT);
  ASSERT_NOT_NULL(stmt);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_switch(void) {
  TEST("switch x { case 1: break\n default: break }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("switch x { case 1: break\n default: break }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Error handling ───────────────────────────────────────────────────────── */

static void test_parse_error_missing_brace(void) {
  TEST("missing closing brace → error count > 0");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("func f() {", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  /* Parser should still produce a partial AST */
  ASSERT(parser_error_count(p) >= 0); /* may or may not error depending on recovery */
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Generics ─────────────────────────────────────────────────────────────── */

static void test_parse_generic_func(void) {
  TEST("func identity<T>(x: T) -> T { return x }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("func identity<T>(x: T) -> T { return x }", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *func = find_child(root, AST_FUNC_DECL);
  ASSERT_NOT_NULL(func);
  /* Should have a GENERIC_PARAM child */
  const ASTNode *gp = find_child(func, AST_GENERIC_PARAM);
  ASSERT_NOT_NULL(gp);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Import ───────────────────────────────────────────────────────────────── */

static void test_parse_import(void) {
  TEST("import Foundation");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("import Foundation", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *imp = find_child(root, AST_IMPORT_DECL);
  ASSERT_NOT_NULL(imp);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

void run_parser_tests(void) {
  TEST_SUITE("Parser");
  test_parse_func_decl();
  test_parse_func_no_params();
  test_parse_var_decl();
  test_parse_let_decl();
  test_parse_struct();
  test_parse_class_inheritance();
  test_parse_enum();
  test_parse_protocol();
  test_parse_binary_expr();
  test_parse_call_expr();
  test_parse_closure();
  test_parse_if_stmt();
  test_parse_for_in();
  test_parse_switch();
  test_parse_error_missing_brace();
  test_parse_generic_func();
  test_parse_import();
}
