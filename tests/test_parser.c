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

/* Find first child of given kind */
static const ASTNode *find_child(const ASTNode *node, ASTNodeKind kind) {
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
    if (c->kind == kind) return c;
  return NULL;
}

static const ASTNode *find_descendant(const ASTNode *node, ASTNodeKind kind) {
  if (!node) return NULL;
  if (node->kind == kind) return node;
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
    const ASTNode *found = find_descendant(c, kind);
    if (found) return found;
  }
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

static void test_parse_contextual_package_variable(void) {
  TEST("let package = 1");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("let package = 1", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  ASSERT_NOT_NULL(root->first_child);
  ASSERT_EQ(root->first_child->kind, AST_LET_DECL);
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

static void test_parse_enum_case_named_lazy(void) {
  TEST("enum case may use contextual keyword lazy");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("enum RequestSetup { case lazy, eager }",
                             &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
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

static void test_parse_qualified_extension(void) {
  TEST("extension Foo.Bar: Proto { }");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("protocol Proto {}\nstruct Foo {}\nextension Foo.Bar: Proto { }",
                             &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *ext = find_child(root, AST_EXTENSION_DECL);
  ASSERT_NOT_NULL(ext);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_qualified_type_tuple_alias(void) {
  TEST("typealias Pair = (Base.Element, Base.Element)");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("typealias Pair = (Base.Element, Base.Element)",
                             &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *alias = find_child(root, AST_TYPEALIAS_DECL);
  ASSERT_NOT_NULL(alias);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_typealias_where_clause(void) {
  TEST("typealias generic where clause");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root =
      parse_code("public typealias CountableClosedRange<Bound> = "
                 "ClosedRange<Bound> where Bound: Strideable, "
                 "Bound.Stride: SignedInteger\n"
                 "public struct AfterAlias {}",
                 &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *alias = find_child(root, AST_TYPEALIAS_DECL);
  ASSERT_NOT_NULL(alias);
  ASSERT_NOT_NULL(find_child(alias, AST_WHERE_CLAUSE));
  const ASTNode *st = find_child(root, AST_STRUCT_DECL);
  ASSERT_NOT_NULL(st);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_function_type_with_local_parameter_name(void) {
  TEST("typealias Handler = @Sendable (_ progress: Progress) -> Void");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root =
      parse_code("typealias Handler = @Sendable (_ progress: Progress) -> Void",
                 &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *alias = find_child(root, AST_TYPEALIAS_DECL);
  ASSERT_NOT_NULL(alias);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_owned_some_parameter_type(void) {
  TEST("func f(_ other: __owned some Sequence<Int>) {}");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root =
      parse_code("func f(_ other: __owned some Sequence<Int>) {}",
                 &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *func = find_child(root, AST_FUNC_DECL);
  ASSERT_NOT_NULL(func);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_borrowing_function_type_parameter(void) {
  TEST("typealias Mapper = (borrowing Element) -> Int");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root =
      parse_code("typealias Mapper = (borrowing Element) -> Int",
                 &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *alias = find_child(root, AST_TYPEALIAS_DECL);
  ASSERT_NOT_NULL(alias);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_init_where_clause(void) {
  TEST("init generic where clause");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root =
      parse_code("struct Box<Element> {\n"
                 "  init<S: Sequence>(_ s: S) where S.Element == Element {}\n"
                 "}",
                 &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *st = find_child(root, AST_STRUCT_DECL);
  ASSERT_NOT_NULL(st);
  const ASTNode *init = find_descendant(st, AST_INIT_DECL);
  ASSERT_NOT_NULL(init);
  ASSERT_NOT_NULL(find_child(init, AST_WHERE_CLAUSE));
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_suppressed_copyable_constraints(void) {
  TEST("suppressed Copyable constraints in inheritance and where clauses");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root =
      parse_code("public struct InlineArray<Element> : ~Copyable "
                 "where Element : ~Copyable { }",
                 &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *st = find_child(root, AST_STRUCT_DECL);
  ASSERT_NOT_NULL(st);
  ASSERT_NOT_NULL(find_child(st, AST_CONFORMANCE));
  ASSERT_NOT_NULL(find_child(st, AST_WHERE_CLAUSE));
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_value_generic_param_with_suppressed_constraints(void) {
  TEST("value generic parameter before suppressed Copyable constraints");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root =
      parse_code("public struct InlineArray<let count : Swift.Int, Element> : "
                 "~Swift.Copyable where Element : ~Copyable { }",
                 &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *st = find_child(root, AST_STRUCT_DECL);
  ASSERT_NOT_NULL(st);
  ASSERT_NOT_NULL(find_child(st, AST_GENERIC_PARAM));
  ASSERT_NOT_NULL(find_child(st, AST_CONFORMANCE));
  ASSERT_NOT_NULL(find_child(st, AST_WHERE_CLAUSE));
  ASSERT_EQ(parser_error_count(p), 0);
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

static void test_parse_for_where_package_identifier(void) {
  TEST("for target in package.targets where ...");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root =
      parse_code("for target in package.targets where target.type == .system {\n"
                 "  target.swiftSettings?.append(contentsOf: [])\n"
                 "}",
                 &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *stmt = find_child(root, AST_FOR_STMT);
  ASSERT_NOT_NULL(stmt);
  ASSERT_EQ(parser_error_count(p), 0);
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

static void test_parse_switch_member_call_subject(void) {
  TEST("switch member call subject with string cases");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(
      "func f(name: String) {\n"
      "  switch name.lowercased() {\n"
      "  case \"utf-8\": break\n"
      "  default: break\n"
      "  }\n"
      "}",
      &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Error handling ───────────────────────────────────────────────────────── */

static void test_parse_error_missing_brace(void) {
  TEST("missing closing brace → parser recovers with a partial AST");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("func f() {", &src, &ts, &arena, &p);
  /* Best-effort recovery: EOF closes the unterminated block, so the parser
   * still yields the function declaration instead of hanging or returning
   * nothing. */
  ASSERT_NOT_NULL(root);
  ASSERT(root->first_child != NULL && root->first_child->kind == AST_FUNC_DECL);
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

static void test_parse_import_kind_module_name(void) {
  TEST("import struct Foundation.Date records module token");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("import struct Foundation.Date", &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *imp = find_child(root, AST_IMPORT_DECL);
  ASSERT_NOT_NULL(imp);
  const Token *t = &ts.tokens[imp->data.var.name_tok];
  ASSERT_EQ(t->len, (uint32_t)10);
  ASSERT(memcmp(src.data + t->pos, "Foundation", 10) == 0);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_dot_prefixed_operator_decl(void) {
  TEST("infix operator .== : ComparisonPrecedence");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code("infix operator .== : ComparisonPrecedence",
                             &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *op = find_child(root, AST_OPERATOR_DECL);
  ASSERT_NOT_NULL(op);
  ASSERT_NOT_NULL(find_child(op, AST_PG_GROUP_REF));
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Custom operator precedence (Tier 2.4) ───────────────────────────── */

/* Find the first BINARY_EXPR descendant */
static const ASTNode *find_first_binary(const ASTNode *node) {
  if (!node) return NULL;
  if (node->kind == AST_BINARY_EXPR) return node;
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
    const ASTNode *r = find_first_binary(c);
    if (r) return r;
  }
  return NULL;
}

/* Returns the operator-token's text length (for distinguishing + vs <+>) */
static int binary_op_text_eq(const ASTNode *bin, const Source *src,
                             const TokenStream *ts, const char *expected) {
  if (!bin || !bin->data.binary.op_tok) return 0;
  const Token *t = &ts->tokens[bin->data.binary.op_tok];
  size_t exp = strlen(expected);
  return t->len == exp && memcmp(src->data + t->pos, expected, exp) == 0;
}

static void test_parse_custom_op_higher_than_addition(void) {
  TEST("custom <+> higherThan: AdditionPrecedence — `1 + 2 <+> 3` = `1 + (2 <+> 3)`");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(
      "precedencegroup MyHigher { higherThan: AdditionPrecedence }\n"
      "infix operator <+> : MyHigher\n"
      "let r = 1 + 2 <+> 3",
      &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *let = find_child(root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  const ASTNode *top = find_first_binary(let);
  ASSERT_NOT_NULL(top);
  ASSERT(binary_op_text_eq(top, &src, &ts, "+"));
  const ASTNode *rhs = top->first_child ? top->first_child->next_sibling : NULL;
  ASSERT_NOT_NULL(rhs);
  ASSERT_EQ(rhs->kind, AST_BINARY_EXPR);
  ASSERT(binary_op_text_eq(rhs, &src, &ts, "<+>"));
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_custom_op_lower_than_addition(void) {
  TEST("custom <-> lowerThan: AdditionPrecedence — `1 + 2 <-> 3` = `(1 + 2) <-> 3`");
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(
      "precedencegroup MyLower { lowerThan: AdditionPrecedence }\n"
      "infix operator <-> : MyLower\n"
      "let r = 1 + 2 <-> 3",
      &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  const ASTNode *let = find_child(root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  const ASTNode *top = find_first_binary(let);
  ASSERT_NOT_NULL(top);
  /* Top-level operator must be `<->`, with `+` nested in the left child */
  ASSERT(binary_op_text_eq(top, &src, &ts, "<->"));
  const ASTNode *lhs = top->first_child;
  ASSERT_NOT_NULL(lhs);
  ASSERT_EQ(lhs->kind, AST_BINARY_EXPR);
  ASSERT(binary_op_text_eq(lhs, &src, &ts, "+"));
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Regression: SDK interface constructs (swift-synthesize-interface) ─────── */

/* Each of these is a real construct emitted by `swift-synthesize-interface` for
 * Apple frameworks that previously derailed the parser; all must parse cleanly. */

static void test_parse_subscript_where_clause(void) {
  TEST("subscript<T>(...) -> T.Value where T.Value.RawValue == CGFloat");
  const char *code =
      "struct B {\n"
      "  subscript<T>(t: T.Type) -> T.Value where T : P,"
      " T.Value : RawRepresentable, T.Value.RawValue == CGFloat { get }\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_NOT_NULL(find_descendant(root, AST_SUBSCRIPT_DECL));
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_keyword_named_func_and_var(void) {
  TEST("keyword-named members: `func open(...)`, `var lazy`");
  const char *code =
      "class A {\n"
      "  open func open(_ u: URL, options: [String : Any] = [:]) async -> Bool\n"
      "  var lazy: Int { get }\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_convention_c_function_type(void) {
  TEST("@convention(c) / @convention(c, cType:) function-type params");
  const char *code =
      "class A {\n"
      "  func s(_ cmp: @convention(c) (Any, Any) -> Int, ctx: Int) -> [Any]\n"
      "  var h: (@convention(c, cType: \"NSUInteger (*)(void)\") (Int) -> Int)?\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_attributed_function_type_params_not_eaten(void) {
  TEST("(@Sendable () -> Void)? and @escaping (Int) -> Void keep their param list");
  const char *code =
      "class A {\n"
      "  func a(h: (@Sendable () -> Void)? = nil) {}\n"
      "  func d(e: @escaping (Int) -> Void) {}\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_isolated_parameter_modifier(void) {
  TEST("isolated (any Actor)? parameter modifier");
  const char *code =
      "struct S {\n"
      "  func next(isolation actor: isolated (any Actor)?) async -> Int?\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_borrowing_consuming_method_modifiers(void) {
  TEST("borrowing func / consuming func method modifiers");
  const char *code =
      "struct S {\n"
      "  public borrowing func idx(_ i: Int) -> Int\n"
      "  public consuming func fin() -> Int\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_NOT_NULL(find_descendant(root, AST_FUNC_DECL));
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_variadic_generics_parameter_packs(void) {
  TEST("parameter packs: <each T>, repeat each T, where repeat each T : P");
  const char *code =
      "struct S {\n"
      "  func g<each T>(_ x: repeat each T) -> (repeat each T)"
      " where repeat each T : Sendable\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_NOT_NULL(find_descendant(root, AST_FUNC_DECL));
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_sending_parameter_and_result(void) {
  TEST("sending parameter/result modifiers, incl. before @escaping");
  const char *code =
      "struct S {\n"
      "  func f(_ x: sending Int) -> sending Foo\n"
      "  func g(_ op: sending @escaping () async -> Int)\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_nonisolated_nonsending_function_type(void) {
  TEST("nonisolated(nonsending) function-type modifier");
  const char *code =
      "struct S {\n"
      "  func w<T, E>(_ op: nonisolated(nonsending) (any P) async throws(E) -> T)"
      " async throws(E) -> T where E : Error\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_viewbuilder_parameter_attribute(void) {
  TEST("@ViewBuilder parameter attribute before the label");
  const char *code =
      "struct V {\n"
      "  init<C>(title: String, @ViewBuilder content: () -> C)\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_nested_generic_close_with_trailing(void) {
  TEST("nested `>>` generic close followed by more tokens (and plain Array<Array<Int>>)");
  const char *code =
      "struct V {\n"
      "  init<C>(x: Int) where Content == Foo<C, Bar<Text, Image>>, C : View\n"
      "  var nested: Array<Array<Int>>\n"
      "  var deep: Dictionary<String, Array<Set<Int>>>\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_property_wrapper_attribute_with_generics(void) {
  TEST("@Published<T> / @Published<Set<T>> property-wrapper attributes");
  const char *code =
      "class A {\n"
      "  @Published<Bool> public var flag: Bool { get }\n"
      "  @Published<Set<Int>> public var ids: Set<Int>\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_backtick_escaped_identifiers(void) {
  TEST("backtick-escaped identifiers: Foo.`Type`, dotted import, `class` member");
  const char *code =
      "let a: Foo.`Type`\n"
      "let b: Bar.`Protocol`\n"
      "import Darwin.POSIX.net.`if`\n"
      "struct S { var `class`: Int { get } }";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_protocol_subscript_where_clause(void) {
  TEST("protocol subscript requirement with a where clause");
  const char *code =
      "protocol P {\n"
      "  subscript<R>(bounds: R) -> Int where R : RangeExpression { get }\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_enum_case_named_with_keyword(void) {
  TEST("enum case named with a keyword: case actor, case `default`");
  const char *code = "enum E {\n  case actor\n  case `default`\n}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

static void test_parse_exotic_default_values(void) {
  TEST("if-expression + multiline-call default values (param and enum assoc value)");
  const char *code =
      "struct S {\n"
      "  func f(x: Foo = if c { .a } else { .b }, y: Int = 0)\n"
      "}\n"
      "enum E {\n"
      "  case hourly(during: DateInterval = DateInterval(\n"
      "    start: .now,\n"
      "    end: .now\n"
      "  ))\n"
      "}";
  Source src; TokenStream ts; ASTArena arena; Parser *p;
  ASTNode *root = parse_code(code, &src, &ts, &arena, &p);
  ASSERT_NOT_NULL(root);
  ASSERT_EQ(parser_error_count(p), 0);
  cleanup(&ts, &arena, p);
  TEST_PASS();
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

void run_parser_tests(void) {
  TEST_SUITE("Parser");
  test_parse_func_decl();
  test_parse_func_no_params();
  test_parse_var_decl();
  test_parse_contextual_package_variable();
  test_parse_let_decl();
  test_parse_struct();
  test_parse_class_inheritance();
  test_parse_enum();
  test_parse_enum_case_named_lazy();
  test_parse_protocol();
  test_parse_qualified_extension();
  test_parse_qualified_type_tuple_alias();
  test_parse_typealias_where_clause();
  test_parse_function_type_with_local_parameter_name();
  test_parse_owned_some_parameter_type();
  test_parse_borrowing_function_type_parameter();
  test_parse_init_where_clause();
  test_parse_suppressed_copyable_constraints();
  test_parse_value_generic_param_with_suppressed_constraints();
  test_parse_binary_expr();
  test_parse_call_expr();
  test_parse_closure();
  test_parse_if_stmt();
  test_parse_for_in();
  test_parse_for_where_package_identifier();
  test_parse_switch();
  test_parse_switch_member_call_subject();
  test_parse_error_missing_brace();
  test_parse_generic_func();
  test_parse_import();
  test_parse_import_kind_module_name();
  test_parse_dot_prefixed_operator_decl();
  test_parse_custom_op_higher_than_addition();
  test_parse_custom_op_lower_than_addition();
  test_parse_subscript_where_clause();
  test_parse_keyword_named_func_and_var();
  test_parse_convention_c_function_type();
  test_parse_attributed_function_type_params_not_eaten();
  test_parse_isolated_parameter_modifier();
  test_parse_borrowing_consuming_method_modifiers();
  test_parse_variadic_generics_parameter_packs();
  test_parse_sending_parameter_and_result();
  test_parse_nonisolated_nonsending_function_type();
  test_parse_viewbuilder_parameter_attribute();
  test_parse_nested_generic_close_with_trailing();
  test_parse_property_wrapper_attribute_with_generics();
  test_parse_backtick_escaped_identifiers();
  test_parse_protocol_subscript_where_clause();
  test_parse_enum_case_named_with_keyword();
  test_parse_exotic_default_values();
}
