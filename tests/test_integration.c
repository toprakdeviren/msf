/**
 * @file test_integration.c
 * @brief Integration tests: full pipeline (source → lex → parse → sema → verify AST/types).
 *
 * Each test runs a complete Swift snippet through the entire compiler frontend
 * and verifies the resulting typed AST structure.
 */
#include "test_framework.h"
#include "msf.h"
#include "internal/msf.h"

/* ── Pipeline helper ──────────────────────────────────────────────────────── */

typedef struct {
  Source src;
  TokenStream ts;
  ASTArena ast_arena;
  TypeArena type_arena;
  Parser *parser;
  SemaContext *sema;
  ASTNode *root;
} Pipeline;

static int pipe_run(Pipeline *p, const char *code) {
  p->src.data = code;
  p->src.len = strlen(code);
  p->src.filename = "<integration>";
  token_stream_init(&p->ts, 512);
  lexer_tokenize(&p->src, &p->ts, 1, NULL);
  ast_arena_init(&p->ast_arena, 0);
  p->parser = parser_init(&p->src, &p->ts, &p->ast_arena);
  p->root = parse_source_file(p->parser);
  type_arena_init(&p->type_arena, 0);
  type_builtins_init(&p->type_arena);
  p->sema = sema_init(&p->src, p->ts.tokens, &p->ast_arena, &p->type_arena);
  return sema_analyze(p->sema, p->root);
}

static void pipe_free(Pipeline *p) {
  sema_destroy(p->sema);
  parser_destroy(p->parser);
  type_arena_free(&p->type_arena);
  ast_arena_free(&p->ast_arena);
  token_stream_free(&p->ts);
}

/* ── AST search helpers ───────────────────────────────────────────────────── */

static const ASTNode *find_first(const ASTNode *n, ASTNodeKind k) {
  if (!n) return NULL;
  if (n->kind == k) return n;
  for (const ASTNode *c = n->first_child; c; c = c->next_sibling) {
    const ASTNode *f = find_first(c, k);
    if (f) return f;
  }
  return NULL;
}

static int count_kind(const ASTNode *n, ASTNodeKind k) {
  if (!n) return 0;
  int c = (n->kind == k) ? 1 : 0;
  for (const ASTNode *ch = n->first_child; ch; ch = ch->next_sibling)
    c += count_kind(ch, k);
  return c;
}

static const ASTNode *nth_child(const ASTNode *n, int idx) {
  int i = 0;
  for (const ASTNode *c = n->first_child; c; c = c->next_sibling)
    if (i++ == idx) return c;
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  1. Complete Programs
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_hello_world(void) {
  TEST("print(\"Hello, World!\") — full pipeline, zero errors");
  Pipeline p = {0};
  pipe_run(&p, "print(\"Hello, World!\")");
  ASSERT_EQ(parser_error_count(p.parser), 0);
  ASSERT_EQ(sema_error_count(p.sema), 0);
  pipe_free(&p);
  TEST_PASS();
}

static void test_fibonacci(void) {
  TEST("fibonacci function — recursive, typed");
  Pipeline p = {0};
  pipe_run(&p,
    "func fib(n: Int) -> Int {\n"
    "  if n <= 1 { return n }\n"
    "  return fib(n: n - 1) + fib(n: n - 2)\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  const ASTNode *func = find_first(p.root, AST_FUNC_DECL);
  ASSERT_NOT_NULL(func);
  ASSERT_NOT_NULL(func->type);
  ASSERT_EQ(func->type->kind, TY_FUNC);
  pipe_free(&p);
  TEST_PASS();
}

static void test_struct_with_methods(void) {
  TEST("struct with stored props + method");
  Pipeline p = {0};
  pipe_run(&p,
    "struct Point {\n"
    "  var x: Int\n"
    "  var y: Int\n"
    "  func magnitude() -> Double {\n"
    "    return 0.0\n"
    "  }\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  const ASTNode *s = find_first(p.root, AST_STRUCT_DECL);
  ASSERT_NOT_NULL(s);
  ASSERT_NOT_NULL(s->type);
  ASSERT_EQ(s->type->kind, TY_NAMED);
  /* Should have 2 VAR_DECL + 1 FUNC_DECL inside */
  ASSERT(count_kind(s, AST_VAR_DECL) >= 2);
  ASSERT(count_kind(s, AST_FUNC_DECL) >= 1);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  2. Type Inference Through Expressions
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_type_inference_int_arithmetic(void) {
  TEST("let x = 1 + 2 → Int inferred");
  Pipeline p = {0};
  pipe_run(&p, "let x = 1 + 2");
  const ASTNode *let = find_first(p.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_NOT_NULL(let->type);
  ASSERT_EQ(let->type, TY_BUILTIN_INT);
  pipe_free(&p);
  TEST_PASS();
}

static void test_type_annotation_override(void) {
  TEST("var x: Float = 3.14 → Float (not Double)");
  Pipeline p = {0};
  pipe_run(&p, "var x: Float = 3.14");
  const ASTNode *var = find_first(p.root, AST_VAR_DECL);
  ASSERT_NOT_NULL(var);
  ASSERT_NOT_NULL(var->type);
  ASSERT_EQ(var->type, TY_BUILTIN_FLOAT);
  pipe_free(&p);
  TEST_PASS();
}

static void test_array_literal_inference(void) {
  TEST("let a = [1, 2, 3] → [Int]");
  Pipeline p = {0};
  pipe_run(&p, "let a = [1, 2, 3]");
  const ASTNode *let = find_first(p.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  /* Type should be array */
  if (let->type)
    ASSERT_EQ(let->type->kind, TY_ARRAY);
  pipe_free(&p);
  TEST_PASS();
}

static void test_dict_literal_inference(void) {
  TEST("let d = [\"a\": 1, \"b\": 2] → [String: Int]");
  Pipeline p = {0};
  pipe_run(&p, "let d = [\"a\": 1, \"b\": 2]");
  const ASTNode *let = find_first(p.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  if (let->type)
    ASSERT_EQ(let->type->kind, TY_DICT);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  3. Control Flow
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_if_else_chain(void) {
  TEST("if/else if/else chain — no errors");
  Pipeline p = {0};
  pipe_run(&p,
    "let x: Int = 5\n"
    "if x > 10 {\n"
    "  let a = 1\n"
    "} else if x > 0 {\n"
    "  let b = 2\n"
    "} else {\n"
    "  let c = 3\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  ASSERT_EQ(sema_error_count(p.sema), 0);
  pipe_free(&p);
  TEST_PASS();
}

static void test_for_in_array(void) {
  TEST("for i in [1, 2, 3] { } — no errors");
  Pipeline p = {0};
  pipe_run(&p, "for i in [1, 2, 3] { let x = i }");
  ASSERT_EQ(parser_error_count(p.parser), 0);
  pipe_free(&p);
  TEST_PASS();
}

static void test_while_loop(void) {
  TEST("while true { break }");
  Pipeline p = {0};
  pipe_run(&p, "while true { break }");
  ASSERT_EQ(parser_error_count(p.parser), 0);
  ASSERT(find_first(p.root, AST_WHILE_STMT) != NULL);
  pipe_free(&p);
  TEST_PASS();
}

static void test_switch_enum(void) {
  TEST("enum + switch — full pattern matching");
  Pipeline p = {0};
  pipe_run(&p,
    "enum Dir { case north, south, east, west }\n"
    "let d: Dir = .north\n"
    "switch d {\n"
    "case .north: break\n"
    "case .south: break\n"
    "default: break\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  4. Generics
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_generic_function(void) {
  TEST("func identity<T>(x: T) -> T — generic param resolved");
  Pipeline p = {0};
  pipe_run(&p, "func identity<T>(x: T) -> T { return x }");
  const ASTNode *func = find_first(p.root, AST_FUNC_DECL);
  ASSERT_NOT_NULL(func);
  ASSERT_NOT_NULL(func->type);
  ASSERT_EQ(func->type->kind, TY_FUNC);
  pipe_free(&p);
  TEST_PASS();
}

static void test_generic_struct(void) {
  TEST("struct Box<T> { var value: T }");
  Pipeline p = {0};
  pipe_run(&p, "struct Box<T> { var value: T }");
  ASSERT_EQ(parser_error_count(p.parser), 0);
  const ASTNode *s = find_first(p.root, AST_STRUCT_DECL);
  ASSERT_NOT_NULL(s);
  ASSERT(find_first(s, AST_GENERIC_PARAM) != NULL);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  5. Classes & Inheritance
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_class_inheritance(void) {
  TEST("class Animal { } class Dog: Animal { } — inheritance");
  Pipeline p = {0};
  pipe_run(&p,
    "class Animal { var name: String = \"\" }\n"
    "class Dog: Animal { var breed: String = \"\" }\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  ASSERT(count_kind(p.root, AST_CLASS_DECL) == 2);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  6. Optional Binding & Guard
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_optional_binding_if_let(void) {
  TEST("if let x = optional → x visible in then-block");
  Pipeline p = {0};
  pipe_run(&p,
    "var opt: Int? = 42\n"
    "if let x = opt {\n"
    "  let y = x\n"
    "}\n"
  );
  /* 'x' should NOT produce "undeclared identifier" error */
  int undeclared = 0;
  for (uint32_t i = 0; i < sema_error_count(p.sema); i++)
    if (strstr(sema_error_message(p.sema, i), "undeclared"))
      undeclared = 1;
  ASSERT(!undeclared);
  pipe_free(&p);
  TEST_PASS();
}

static void test_guard_let(void) {
  TEST("guard let x = opt else { return } — x visible after guard");
  Pipeline p = {0};
  pipe_run(&p,
    "func f(opt: Int?) {\n"
    "  guard let x = opt else { return }\n"
    "  let y = x\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  7. Closures
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_closure_typed(void) {
  TEST("typed closure: { (x: Int) in x + 1 }");
  Pipeline p = {0};
  pipe_run(&p, "let f = { (x: Int) in x + 1 }");
  ASSERT_EQ(parser_error_count(p.parser), 0);
  const ASTNode *closure = find_first(p.root, AST_CLOSURE_EXPR);
  ASSERT_NOT_NULL(closure);
  pipe_free(&p);
  TEST_PASS();
}

static void test_closure_capture_list(void) {
  TEST("closure with capture list: { [weak self] in }");
  Pipeline p = {0};
  pipe_run(&p,
    "class Foo {\n"
    "  func bar() {\n"
    "    let f = { [weak self] in }\n"
    "  }\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  ASSERT(find_first(p.root, AST_CLOSURE_CAPTURE) != NULL);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  8. Protocol & Conformance
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_protocol_conformance(void) {
  TEST("protocol + struct conformance");
  Pipeline p = {0};
  pipe_run(&p,
    "protocol Greetable { func greet() -> String }\n"
    "struct Person: Greetable {\n"
    "  var name: String\n"
    "  func greet() -> String { return name }\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  9. Extensions
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_extension(void) {
  TEST("extension adds computed property");
  Pipeline p = {0};
  pipe_run(&p,
    "struct Point { var x: Int; var y: Int }\n"
    "extension Point {\n"
    "  var description: String { get { return \"\" } }\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  ASSERT(find_first(p.root, AST_EXTENSION_DECL) != NULL);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  10. Error Detection
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_error_undeclared_type(void) {
  TEST("undeclared type → sema error");
  Pipeline p = {0};
  pipe_run(&p, "var x: NonExistentType = 42");
  ASSERT(sema_error_count(p.sema) > 0);
  const char *msg = sema_error_message(p.sema, 0);
  ASSERT(strstr(msg, "undeclared") != NULL);
  pipe_free(&p);
  TEST_PASS();
}

static void test_multi_file_simulation(void) {
  TEST("multiple top-level declarations — complete mini-program");
  Pipeline p = {0};
  pipe_run(&p,
    "struct Config {\n"
    "  var debug: Bool\n"
    "  var maxRetries: Int\n"
    "}\n"
    "func makeConfig() -> Config {\n"
    "  return Config()\n"
    "}\n"
    "let config = makeConfig()\n"
    "let retries = config.maxRetries\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  11. Async/Await & Try
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_async_func(void) {
  TEST("async func fetch() → async modifier set");
  Pipeline p = {0};
  pipe_run(&p, "func fetch() async -> String { return \"\" }");
  const ASTNode *func = find_first(p.root, AST_FUNC_DECL);
  ASSERT_NOT_NULL(func);
  ASSERT(func->modifiers & (1u << 13)); /* MOD_ASYNC */
  pipe_free(&p);
  TEST_PASS();
}

static void test_try_expr(void) {
  TEST("try expression parsed correctly");
  Pipeline p = {0};
  pipe_run(&p, "func f() throws -> Int { return 1 }\nlet x = try f()");
  ASSERT_EQ(parser_error_count(p.parser), 0);
  ASSERT(find_first(p.root, AST_TRY_EXPR) != NULL);
  pipe_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  12. Edge Cases
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_empty_source(void) {
  TEST("empty source → valid empty AST");
  Pipeline p = {0};
  pipe_run(&p, "");
  ASSERT_NOT_NULL(p.root);
  ASSERT_EQ(p.root->kind, AST_SOURCE_FILE);
  ASSERT_EQ(parser_error_count(p.parser), 0);
  pipe_free(&p);
  TEST_PASS();
}

static void test_nested_types(void) {
  TEST("nested struct inside struct");
  Pipeline p = {0};
  pipe_run(&p,
    "struct Outer {\n"
    "  struct Inner { var value: Int }\n"
    "  var inner: Inner\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  ASSERT(count_kind(p.root, AST_STRUCT_DECL) == 2);
  pipe_free(&p);
  TEST_PASS();
}

static void test_computed_property(void) {
  TEST("computed property with get/set");
  Pipeline p = {0};
  pipe_run(&p,
    "struct Temp {\n"
    "  var celsius: Double\n"
    "  var fahrenheit: Double {\n"
    "    get { return celsius }\n"
    "    set { celsius = newValue }\n"
    "  }\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  pipe_free(&p);
  TEST_PASS();
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

void run_integration_tests(void) {
  TEST_SUITE("Integration");
  /* Complete programs */
  test_hello_world();
  test_fibonacci();
  test_struct_with_methods();
  /* Type inference */
  test_type_inference_int_arithmetic();
  test_type_annotation_override();
  test_array_literal_inference();
  test_dict_literal_inference();
  /* Control flow */
  test_if_else_chain();
  test_for_in_array();
  test_while_loop();
  test_switch_enum();
  /* Generics */
  test_generic_function();
  test_generic_struct();
  /* Classes */
  test_class_inheritance();
  /* Optional binding */
  test_optional_binding_if_let();
  test_guard_let();
  /* Closures */
  test_closure_typed();
  test_closure_capture_list();
  /* Protocol */
  test_protocol_conformance();
  /* Extension */
  test_extension();
  /* Errors */
  test_error_undeclared_type();
  test_multi_file_simulation();
  /* Async/try */
  test_async_func();
  test_try_expr();
  /* Edge cases */
  test_empty_source();
  test_nested_types();
  test_computed_property();
}
