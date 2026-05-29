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

typedef struct {
  Source src[2];
  TokenStream ts[2];
  ASTArena ast_arena;
  TypeArena type_arena;
  Parser *parser[2];
  SemaContext *sema;
  ASTNode *root[2];
} TestModulePipeline;

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

static int module_pipeline_run2(TestModulePipeline *mp, const char *code0,
                                const char *code1) {
  const char *codes[2] = {code0, code1};
  const char *names[2] = {"Protocol.swift", "Impl.swift"};

  ast_arena_init(&mp->ast_arena, 0);
  for (int i = 0; i < 2; i++) {
    mp->src[i].data = codes[i];
    mp->src[i].len = strlen(codes[i]);
    mp->src[i].filename = names[i];

    token_stream_init(&mp->ts[i], 256);
    lexer_tokenize(&mp->src[i], &mp->ts[i], 1, NULL);

    mp->parser[i] = parser_init(&mp->src[i], &mp->ts[i], &mp->ast_arena);
    mp->root[i] = parse_source_file(mp->parser[i]);
  }

  type_arena_init(&mp->type_arena, 0);
  type_builtins_init(&mp->type_arena);
  mp->sema = sema_init(&mp->src[0], mp->ts[0].tokens, &mp->ast_arena,
                       &mp->type_arena);

  SemaModuleFile files[2] = {
      {.src = &mp->src[0],
       .tokens = mp->ts[0].tokens,
       .token_count = (uint32_t)mp->ts[0].count,
       .root = mp->root[0]},
      {.src = &mp->src[1],
       .tokens = mp->ts[1].tokens,
       .token_count = (uint32_t)mp->ts[1].count,
       .root = mp->root[1]},
  };
  return sema_analyze_module(mp->sema, files, 2);
}

static void pipeline_free(TestPipeline *tp) {
  sema_destroy(tp->sema);
  parser_destroy(tp->parser);
  type_arena_free(&tp->type_arena);
  ast_arena_free(&tp->ast_arena);
  token_stream_free(&tp->ts);
}

static void module_pipeline_free(TestModulePipeline *mp) {
  sema_destroy(mp->sema);
  parser_destroy(mp->parser[0]);
  parser_destroy(mp->parser[1]);
  type_arena_free(&mp->type_arena);
  ast_arena_free(&mp->ast_arena);
  token_stream_free(&mp->ts[0]);
  token_stream_free(&mp->ts[1]);
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

static int has_error_with(SemaContext *s, const char *a, const char *b,
                          const char *c);

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

static void test_sema_extension_static_stored_property_ok(void) {
  TEST("extension static stored property → no stored-property diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Foo {}\n"
               "extension Foo { static let answer: Int = 42 }");
  ASSERT(!has_error_with(tp.sema,
                         "extensions must not contain stored properties",
                         NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_extension_escaped_static_computed_property_ok(void) {
  TEST("extension escaped static computed property → no stored-property diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Foo {}\n"
               "extension Foo { static var `default`: Int { return 1 } }");
  ASSERT(!has_error_with(tp.sema,
                         "extensions must not contain stored properties",
                         NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_extension_computed_parenthesized_optional_type_ok(void) {
  TEST("extension computed property with parenthesized optional type → no stored-property diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol Error {}\n"
               "struct Foo {}\n"
               "extension Foo { var underlyingError: (any Error)? { return nil } }");
  ASSERT(!has_error_with(tp.sema,
                         "extensions must not contain stored properties",
                         NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_extension_instance_stored_property_rejected(void) {
  TEST("extension instance stored property → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Foo {}\n"
               "extension Foo { var answer: Int = 42 }");
  ASSERT(has_error_with(tp.sema,
                        "extensions must not contain stored properties",
                        NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_extension_method_local_var_ok(void) {
  TEST("extension method local var → no stored-property diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Foo {}\n"
               "extension Foo { func f() { let value: Int = 1 } }");
  ASSERT(!has_error_with(tp.sema,
                         "extensions must not contain stored properties",
                         NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_property_method_same_name_ok(void) {
  TEST("property and method with same base name → no redefinition");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Encoder {\n"
               "  static var urlEncodedForm: Int { return 1 }\n"
               "  static func urlEncodedForm(destination: Int) -> Int { return destination }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "Redefinition of 'urlEncodedForm'",
                         NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_duplicate_property_same_name_rejected(void) {
  TEST("duplicate properties with same name → redefinition");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Encoder {\n"
               "  static var urlEncodedForm: Int { return 1 }\n"
               "  static var urlEncodedForm: Int { return 2 }\n"
               "}");
  ASSERT(has_error_with(tp.sema, "Redefinition of 'urlEncodedForm'",
                        NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_static_and_instance_property_same_name_ok(void) {
  TEST("static and instance property may share a base name");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Bitmap {\n"
               "  static var capacity: Int { return 64 }\n"
               "  var capacity: Int { return 64 }\n"
               "}\n"
               "protocol Extended {\n"
               "  static var af: Int { get }\n"
               "  var af: Int { get }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "Redefinition of 'capacity'", NULL, NULL));
  ASSERT(!has_error_with(tp.sema, "Redefinition of 'af'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_switch_case_local_reuse_ok(void) {
  TEST("switch case local names are scoped per arm");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func f(x: Int) -> Int {\n"
               "  switch x {\n"
               "  case 0:\n"
               "    let cell: Int = 1\n"
               "    return cell\n"
               "  default:\n"
               "    let cell: Int = 2\n"
               "    return cell\n"
               "  }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "Redefinition of 'cell'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_nominal_generic_param_visible_in_members(void) {
  TEST("generic type parameter visible in member signatures");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Box<Element> {\n"
               "  var value: Element\n"
               "  func get(_ fallback: Element) -> Element { return fallback }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Element'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_extension_typealias_visible_in_members(void) {
  TEST("extension typealias visible in member signatures");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Pair<Base> {}\n"
               "extension Pair {\n"
               "  typealias Element = Base\n"
               "  func next() -> Element? { return nil }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Element'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_extension_typealias_visible_across_extensions(void) {
  TEST("extension typealias visible across extensions");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Pair<Base> {}\n"
               "extension Pair { typealias Element = Base }\n"
               "extension Pair {\n"
               "  func value(_ element: Element) -> Element { return element }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Element'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_extension_nested_lookup_many_extensions(void) {
  TEST("extension nested lookup handles many sibling extensions");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Box {}\n"
               "extension Box { struct Item {} }\n"
               "extension Box { func f0(_ x: Item) -> Item { return x } }\n"
               "extension Box { func f1(_ x: Item) -> Item { return x } }\n"
               "extension Box { func f2(_ x: Item) -> Item { return x } }\n"
               "extension Box { func f3(_ x: Item) -> Item { return x } }\n"
               "extension Box { func f4(_ x: Item) -> Item { return x } }\n");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Item'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_typealias_generic_params_visible_in_rhs(void) {
  TEST("typealias generic parameters visible in aliased type");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "public protocol Error {}\n"
               "public struct DataStreamRequest {\n"
               "  public struct Stream<Success, Failure: Error> {}\n"
               "  public typealias Handler<Success, Failure: Error> = "
               "(Stream<Success, Failure>) -> Void\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Success'", NULL, NULL));
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Failure'", NULL, NULL));
  ASSERT(!has_error_with(tp.sema,
                         "type alias cannot be more visible than the type it aliases",
                         NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_function_generic_params_visible_in_return_type(void) {
  TEST("function generic parameters visible in return type");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "public protocol Error {}\n"
               "public enum Result<Success, Failure> {}\n"
               "func tryMap<NewSuccess>(_ value: NewSuccess) -> "
               "Result<NewSuccess, any Error> { return value }\n");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'NewSuccess'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_protocol_associated_type_visible_in_requirements(void) {
  TEST("protocol associated type visible in requirements");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol Serializer {\n"
               "  associatedtype SerializedObject\n"
               "  func serialize() -> SerializedObject\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'SerializedObject'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_nominal_typealias_visible_in_extension(void) {
  TEST("nominal typealias visible in nominal and extension members");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Bitmap {\n"
               "  typealias Value = UInt32\n"
               "  var raw: Value\n"
               "}\n"
               "extension Bitmap {\n"
               "  var capacity: Value { return 0 }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Value'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_nested_nominal_visible_in_member_signatures(void) {
  TEST("nested nominal type visible in member signatures");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct ResponseCacher {\n"
               "  enum Behavior { case cache }\n"
               "  let behavior: Behavior\n"
               "}\n"
               "struct ChainIndex {\n"
               "  enum Representation { case first(Int) }\n"
               "  let position: Representation\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Behavior'", NULL, NULL));
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Representation'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_extension_nested_type_visible_across_extensions(void) {
  TEST("extension nested type visible across extensions");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Pair<Base> {}\n"
               "extension Pair {\n"
               "  struct Index { func same(_ other: Index) -> Bool { return true } }\n"
               "}\n"
               "extension Pair {\n"
               "  func at(_ index: Index) -> Index { return index }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Index'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_metatype_suffix_type_silenced(void) {
  TEST("metatype .Type suffix does not report undeclared Type");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct ExtensionBox<Wrapped> {}\n"
               "protocol Extended {\n"
               "  static var box: ExtensionBox<Self>.Type { get }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Type'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_generic_qualified_suffix_not_generic_arg(void) {
  TEST("generic qualified suffix is not resolved as a generic argument");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct TreeDictionary<Key, Value> { struct Keys {} }\n"
               "struct TreeSet<Element> {\n"
               "  func formUnion<Value>(_ other: TreeDictionary<Element, Value>.Keys) {}\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "undeclared type 'Keys'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_public_alias_to_builtin_and_any_ok(void) {
  TEST("public typealias may mention builtin and Any types");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "typealias Data = Any\n"
               "public typealias Handler = (Int, Data) -> Void");
  ASSERT(!has_error_with(tp.sema,
                         "type alias cannot be more visible than the type it aliases",
                         NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_internal_type_can_conform_to_private_protocol(void) {
  TEST("internal type may use same-file private protocol");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "private protocol Lock {}\n"
               "final class UnfairLock: Lock {}");
  ASSERT(!has_error_with(tp.sema, "less visible", "Lock", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_public_type_private_protocol_rejected(void) {
  TEST("public type cannot expose private protocol conformance");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "private protocol Hidden {}\n"
               "public final class PublicBox: Hidden {}");
  ASSERT(has_error_with(tp.sema, "less visible", "Hidden", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_set_of_enum_hashable_ok(void) {
  TEST("Set of enum type satisfies Hashable requirement");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "enum HTTPMethod { case get }\n"
               "let methods: Set<HTTPMethod> = []");
  ASSERT(!has_error_with(tp.sema, "Hashable", "HTTPMethod", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_set_of_erased_and_generic_hashable_ok(void) {
  TEST("Set of erased or generic preview type skips Hashable false positive");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func f<Subject>(_ value: Subject) {\n"
               "  let erased: Set<Any> = []\n"
               "  let generic: Set<Subject> = []\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "Hashable", "Any", NULL));
  ASSERT(!has_error_with(tp.sema, "Hashable", "Subject", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Protocol witness signature matching (Tier 1.1) ────────────────────── */

/* Helper: returns 1 if any error message contains all of the given substrings. */
static int has_error_with(SemaContext *s, const char *a, const char *b,
                          const char *c) {
  for (uint32_t i = 0; i < sema_error_count(s); i++) {
    const char *msg = sema_error_message(s, i);
    if (!msg) continue;
    if (a && !strstr(msg, a)) continue;
    if (b && !strstr(msg, b)) continue;
    if (c && !strstr(msg, c)) continue;
    return 1;
  }
  return 0;
}

static void test_sema_witness_happy_path(void) {
  TEST("witness: happy path, signature matches → no error");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol Drawable { func draw() -> Int }\n"
               "struct Circle: Drawable { func draw() -> Int { return 1 } }");
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_witness_return_type_mismatch(void) {
  TEST("witness: wrong return type Bool vs Int → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol Drawable { func draw() -> Int }\n"
               "struct Circle: Drawable { func draw() -> Bool { return true } }");
  ASSERT(sema_error_count(tp.sema) > 0);
  ASSERT(has_error_with(tp.sema, "Circle", "Drawable", "Int"));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_witness_param_count_mismatch(void) {
  TEST("witness: parameter count mismatch → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol P { func f(_ a: Int, _ b: Int) -> Int }\n"
               "struct S: P { func f(_ a: Int) -> Int { return a } }");
  ASSERT(sema_error_count(tp.sema) > 0);
  ASSERT(has_error_with(tp.sema, "S", "P", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_witness_param_type_mismatch(void) {
  TEST("witness: parameter type mismatch → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol P { func f(_ x: Int) -> Int }\n"
               "struct S: P { func f(_ x: String) -> Int { return 0 } }");
  ASSERT(sema_error_count(tp.sema) > 0);
  ASSERT(has_error_with(tp.sema, "S", "P", "Int"));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_witness_param_label_mismatch(void) {
  TEST("witness: parameter label mismatch → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol P { func draw(at p: Int) }\n"
               "struct S: P { func draw(in p: Int) { } }");
  ASSERT(sema_error_count(tp.sema) > 0);
  ASSERT(has_error_with(tp.sema, "S", "P", "label"));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_witness_omitted_label_match(void) {
  TEST("witness: matching omitted labels (`_ x: Int`) → no error");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol P { func f(_ x: Int) -> Int }\n"
               "struct S: P { func f(_ y: Int) -> Int { return y } }");
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_witness_omitted_vs_named_label(void) {
  TEST("witness: omitted req vs named impl label → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol P { func f(_ x: Int) -> Int }\n"
               "struct S: P { func f(x: Int) -> Int { return x } }");
  ASSERT(sema_error_count(tp.sema) > 0);
  ASSERT(has_error_with(tp.sema, "S", "P", "label"));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_whole_module_conformance_origin_tokens(void) {
  TEST("whole-module witness check reads protocol and impl tokens from their own files");
  TestModulePipeline mp = {0};
  int rc = module_pipeline_run2(
      &mp,
      "struct Pad00 {}\n"
      "struct Pad01 {}\n"
      "struct Pad02 {}\n"
      "struct Pad03 {}\n"
      "struct Pad04 {}\n"
      "struct Pad05 {}\n"
      "struct Pad06 {}\n"
      "struct Pad07 {}\n"
      "struct Pad08 {}\n"
      "struct Pad09 {}\n"
      "struct Pad10 {}\n"
      "struct Pad11 {}\n"
      "struct Pad12 {}\n"
      "struct Pad13 {}\n"
      "struct Pad14 {}\n"
      "struct Pad15 {}\n"
      "protocol Drawable { func draw(at p: Int) -> Int }\n",
      "struct Circle: Drawable { func draw(at p: Int) -> Int { return p } }\n");
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(sema_error_count(mp.sema), 0);
  module_pipeline_free(&mp);
  TEST_PASS();
}

/* ── Overload resolution (Tier 1.4) ────────────────────────────────────── */

static void test_sema_overload_int_double_picks_int(void) {
  TEST("overload: foo(Int)/foo(Double) + Int arg → picks Int");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func foo(_ x: Int) -> Int { return x }\n"
               "func foo(_ x: Double) -> Double { return x }\n"
               "let r = foo(42)");
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_EQ(let->type, TY_BUILTIN_INT);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_overload_int_double_picks_double(void) {
  TEST("overload: foo(Int)/foo(Double) + Double arg → picks Double");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func foo(_ x: Int) -> Int { return x }\n"
               "func foo(_ x: Double) -> Double { return x }\n"
               "let r = foo(3.14)");
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_EQ(let->type, TY_BUILTIN_DOUBLE);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_overload_label_distinguishes(void) {
  TEST("overload: f(x: Int) vs f(y: Int) — picks by label");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func f(x: Int) -> Int { return x }\n"
               "func f(y: Int) -> String { return \"y\" }\n"
               "let r = f(y: 1)");
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_EQ(let->type, TY_BUILTIN_STRING);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_overload_default_arg_omitted(void) {
  TEST("overload: default arg + omitted call → resolves with default");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func compute(value v: Int = 0, scale s: Double = 1.0) -> Double {\n"
               "    return Double(v) * s\n"
               "}\n"
               "let r = compute(scale: 2.0)");
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_EQ(let->type, TY_BUILTIN_DOUBLE);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_overload_ambiguity(void) {
  TEST("overload: two implicit-conv candidates tie → ambiguity diagnostic");
  TestPipeline tp = {0};
  /* Int literal can convert to Float (cost 1) or Double (cost 1) — tie */
  pipeline_run(&tp,
               "func h(_ x: Float) -> Int { return 0 }\n"
               "func h(_ x: Double) -> String { return \"x\" }\n"
               "let r = h(42)");
  ASSERT(has_error_with(tp.sema, "ambiguous", "h", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Property wrapper projected value (Tier 1.3) ──────────────────────── */

static void test_sema_wrapper_projected_basic(void) {
  TEST("@State var x = 0 → $x is bound with projectedValue type");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@propertyWrapper struct State<T> {\n"
               "    var wrappedValue: T\n"
               "    var projectedValue: Bool { return true }\n"
               "}\n"
               "@State var count: Int = 0\n"
               "let p = $count");
  /* $count must be looked up successfully and produce a Bool */
  int found_undeclared = 0;
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "$count")) { found_undeclared = 1; break; }
  }
  ASSERT(!found_undeclared);
  const ASTNode *root = tp.root;
  const ASTNode *p_let = NULL;
  for (const ASTNode *c = root->first_child; c; c = c->next_sibling) {
    if ((c->kind == AST_LET_DECL || c->kind == AST_VAR_DECL) &&
        c->data.var.name_tok) {
      const Token *t = &tp.ts.tokens[c->data.var.name_tok];
      if (t->len == 1 && tp.src.data[t->pos] == 'p') { p_let = c; break; }
    }
  }
  ASSERT_NOT_NULL(p_let);
  ASSERT_EQ(p_let->type, TY_BUILTIN_BOOL);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_wrapper_missing_wrapped_value(void) {
  TEST("@propertyWrapper without wrappedValue → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@propertyWrapper struct Bad {\n"
               "    var something: Int\n"
               "}");
  ASSERT(has_error_with(tp.sema, "Bad", "wrappedValue", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_wrapper_no_projected_means_no_dollar(void) {
  TEST("@Wrapper without projectedValue → $x has no symbol bound");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@propertyWrapper struct Plain<T> {\n"
               "    var wrappedValue: T\n"
               "}\n"
               "@Plain var n: Int = 0\n"
               "let p = $n");
  /* Plain has no projectedValue — `$n` should not have been synthesised
   * as a sibling symbol; the IDENT lookup for `$n` returns nothing and
   * `let p` ends up with no inferred type. */
  const ASTNode *root = tp.root;
  const ASTNode *p_let = NULL;
  for (const ASTNode *c = root->first_child; c; c = c->next_sibling) {
    if ((c->kind == AST_LET_DECL || c->kind == AST_VAR_DECL) &&
        c->data.var.name_tok) {
      const Token *t = &tp.ts.tokens[c->data.var.name_tok];
      if (t->len == 1 && tp.src.data[t->pos] == 'p') { p_let = c; break; }
    }
  }
  ASSERT_NOT_NULL(p_let);
  ASSERT_EQ(p_let->type, NULL);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Actor isolation (Tier 1.2) ──────────────────────────────────────── */

static void test_sema_main_actor_member_from_sync_rejected(void) {
  TEST("@MainActor class member accessed from sync ctx → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@MainActor class ViewModel { var count: Int = 0 }\n"
               "func update() {\n"
               "    let vm = ViewModel()\n"
               "    let _ = vm.count\n"
               "}");
  ASSERT(has_error_with(tp.sema, "Main actor", "await", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_main_actor_member_from_main_actor_ok(void) {
  TEST("@MainActor func accessing @MainActor member → no error");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@MainActor class ViewModel { var count: Int = 0 }\n"
               "@MainActor func update() {\n"
               "    let vm = ViewModel()\n"
               "    let _ = vm.count\n"
               "}");
  /* Must not produce the cross-actor diagnostic */
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "Main actor"))
      ASSERT(0);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_main_actor_member_with_await_ok(void) {
  TEST("await vm.count from non-MainActor ctx → no diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@MainActor class ViewModel { var count: Int = 0 }\n"
               "func update() async {\n"
               "    let vm = ViewModel()\n"
               "    let _ = await vm.count\n"
               "}");
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "Main actor"))
      ASSERT(0);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_actor_member_from_outside_rejected(void) {
  TEST("actor member accessed from outside actor → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "actor Counter { var n: Int = 0 }\n"
               "func bump(_ c: Counter) {\n"
               "    let _ = c.n\n"
               "}");
  ASSERT(has_error_with(tp.sema, "Actor-isolated", "await", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_actor_member_with_await_ok(void) {
  TEST("await c.n from async ctx → no diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "actor Counter { var n: Int = 0 }\n"
               "func bump(_ c: Counter) async {\n"
               "    let _ = await c.n\n"
               "}");
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "Actor-isolated"))
      ASSERT(0);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_nonisolated_member_always_reachable(void) {
  TEST("nonisolated member of @MainActor class reachable from sync ctx");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@MainActor class V {\n"
               "    nonisolated var id: Int { return 0 }\n"
               "}\n"
               "func read(_ v: V) {\n"
               "    let _ = v.id\n"
               "}");
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "Main actor"))
      ASSERT(0);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Sendable inference (Tier 2.2) ──────────────────────────────────── */

static void test_sema_struct_of_sendable_is_sendable(void) {
  TEST("struct of Int+String → auto Sendable conformance");
  TestPipeline tp = {0};
  pipeline_run(&tp, "struct Pair { var a: Int; var b: String }");
  const ConformanceTable *ct = sema_conformance_table(tp.sema);
  ASSERT_NOT_NULL(ct);
  ASSERT(conformance_table_has(ct, "Pair", "Sendable"));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_struct_with_class_member_not_sendable(void) {
  TEST("struct holding a class instance → NOT auto Sendable");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "class Bag { }\n"
               "struct Holder { var b: Bag }");
  const ConformanceTable *ct = sema_conformance_table(tp.sema);
  ASSERT_NOT_NULL(ct);
  ASSERT(!conformance_table_has(ct, "Holder", "Sendable"));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_class_explicit_sendable_with_class_member_rejected(void) {
  TEST("class : Sendable + non-Sendable stored property → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "class Bag { }\n"
               "class Wrong: Sendable { var b: Bag = Bag() }");
  ASSERT(has_error_with(tp.sema, "Sendable", "Bag", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_class_explicit_sendable_with_generic_member_ok(void) {
  TEST("class : Sendable + unresolved generic stored property → no diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "class Box<Value>: Sendable { var value: Value }\n");
  ASSERT(!has_error_with(tp.sema, "non-Sendable", "Value", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_empty_enum_is_sendable(void) {
  TEST("enum without payload → auto Sendable");
  TestPipeline tp = {0};
  pipeline_run(&tp, "enum E { case a; case b }");
  const ConformanceTable *ct = sema_conformance_table(tp.sema);
  ASSERT_NOT_NULL(ct);
  ASSERT(conformance_table_has(ct, "E", "Sendable"));
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Generic constraint validation (Tier 2.6) ─────────────────────────── */

static void test_sema_generic_constraint_struct_rejected(void) {
  TEST("where T: SomeStruct → diagnostic (struct is not class/protocol)");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Box { var x: Int }\n"
               "func f<T>(_ x: T) where T: Box { }");
  ASSERT(has_error_with(tp.sema, "Box", "class or protocol", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_generic_constraint_int_rejected(void) {
  TEST("where T: Int → diagnostic (builtin is not class/protocol)");
  TestPipeline tp = {0};
  pipeline_run(&tp, "func f<T>(_ x: T) where T: Int { }");
  ASSERT(has_error_with(tp.sema, "Int", "class or protocol", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_generic_constraint_protocol_ok(void) {
  TEST("where T: SomeProtocol → no error");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol P { }\n"
               "func f<T>(_ x: T) where T: P { }");
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "class or protocol"))
      ASSERT(0);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_conditional_extension_generic_arg_constraint_ok(void) {
  TEST("conditional extension constraint satisfies generic argument check");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "protocol Equatable {}\n"
               "class Protected<Value> {}\n"
               "extension Protected: Equatable where Value: Equatable {\n"
               "  static func ==(lhs: Protected<Value>, rhs: Protected<Value>) -> Bool { return true }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "Value", "Equatable", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_generic_constraint_class_ok(void) {
  TEST("where T: SomeClass → no error (superclass constraint)");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "class Base { }\n"
               "func f<T>(_ x: T) where T: Base { }");
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "class or protocol"))
      ASSERT(0);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Result builder nesting (Tier 2.1) ───────────────────────────────── */

/* Counts CALL_EXPR descendants whose member name is `target` (e.g. how many
 * synthesised buildBlock(...) calls fire after a builder transformation). */
static int count_member_calls(const ASTNode *node, const Source *src,
                              const TokenStream *ts, const char *target) {
  if (!node) return 0;
  int n = 0;
  if (node->kind == AST_CALL_EXPR && node->first_child &&
      node->first_child->kind == AST_MEMBER_EXPR) {
    const ASTNode *m = node->first_child;
    if (m->data.var.name_tok) {
      const Token *t = &ts->tokens[m->data.var.name_tok];
      size_t exp = strlen(target);
      if (t->len == exp && memcmp(src->data + t->pos, target, exp) == 0)
        n++;
    }
  }
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
    n += count_member_calls(c, src, ts, target);
  return n;
}

static void test_sema_builder_single_level(void) {
  TEST("@resultBuilder single-level transform produces buildBlock call");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@resultBuilder struct B {\n"
               "    static func buildBlock(_ items: Int...) -> Int { return 0 }\n"
               "}\n"
               "@B func body() -> Int {\n"
               "    1\n"
               "    2\n"
               "}");
  int n = count_member_calls(tp.root, &tp.src, &tp.ts, "buildBlock");
  ASSERT(n >= 1);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_builder_two_level_nesting(void) {
  TEST("@B body containing trailing-closure call → 2 buildBlock calls (outer + inner)");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@resultBuilder struct B {\n"
               "    static func buildBlock(_ items: Int...) -> Int { return 0 }\n"
               "}\n"
               "func wrap(_ make: () -> Int) -> Int { return make() }\n"
               "@B func body() -> Int {\n"
               "    1\n"
               "    wrap {\n"
               "        2\n"
               "        3\n"
               "    }\n"
               "}");
  int n = count_member_calls(tp.root, &tp.src, &tp.ts, "buildBlock");
  ASSERT(n >= 2);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_builder_nested_call_arg_closure(void) {
  TEST("@resultBuilder body containing trailing-closure call → inner closure also transformed");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "@resultBuilder struct B {\n"
               "    static func buildBlock(_ items: Int...) -> Int { return 0 }\n"
               "}\n"
               "func wrap(_ x: Int) -> Int { return x }\n"
               "@B func body() -> Int {\n"
               "    1\n"
               "    wrap(0)\n"
               "    2\n"
               "}");
  /* The outer buildBlock should fire — inner content is a flat call expr.
   * This baseline test guards against regressions in the single-level path. */
  int n = count_member_calls(tp.root, &tp.src, &tp.ts, "buildBlock");
  ASSERT(n >= 1);
  ASSERT_EQ(sema_error_count(tp.sema), 0);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Diagnostic limit overflow indicator (Tier 3.3) ──────────────────── */

static void test_sema_diagnostic_overflow_indicator(void) {
  TEST("200 errors → all retained (diagnostics grow, no 64-cap)");
  TestPipeline tp = {0};
  /* 200 distinct undeclared-type usages — each fires one sema diagnostic.
   * Diagnostics now live in a heap array that grows on demand (cap removed),
   * so all 200 are retained and none are truncated below SEMA_DIAG_MAX. */
  char src[32768];
  size_t off = 0;
  const int N = 200;
  for (int i = 0; i < N && off < sizeof(src) - 64; i++) {
    int n = snprintf(src + off, sizeof(src) - off, "let x%d: Sring = \"\"\n", i);
    if (n <= 0) break;
    off += (size_t)n;
  }
  pipeline_run(&tp, src);
  uint32_t ec = sema_error_count(tp.sema);
  ASSERT(ec >= (uint32_t)N); /* all retained — previously capped at 64 */
  /* Well under the DoS ceiling, so nothing is suppressed. */
  for (uint32_t i = 0; i < ec; i++)
    ASSERT(strstr(sema_error_message(tp.sema, i), "more diagnostics suppressed") == NULL);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Async/await context (Tier 3.4) ──────────────────────────────────── */

static void test_sema_await_in_sync_func_rejected(void) {
  TEST("await inside non-async func → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func op() -> Int { return 0 }\n"
               "func sync() {\n"
               "    let _ = await op()\n"
               "}");
  ASSERT(has_error_with(tp.sema, "await", "async", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_await_in_async_func_ok(void) {
  TEST("await inside async func → no diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func op() async -> Int { return 0 }\n"
               "func work() async {\n"
               "    let _ = await op()\n"
               "}");
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "await") && strstr(m, "async"))
      ASSERT(0);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_await_in_closure_body_ok(void) {
  TEST("await inside closure body is treated as async closure context");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func op() async -> Int { return 0 }\n"
               "func work() {\n"
               "  let f = { await op() }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "await", "async", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_await_at_toplevel_rejected(void) {
  TEST("await at top level → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func op() -> Int { return 0 }\n"
               "let x = await op()");
  ASSERT(has_error_with(tp.sema, "await", "async", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Diagnostic source range (Tier 3.2) ──────────────────────────────── */

static void test_sema_error_range_undeclared_type(void) {
  TEST("undeclared type → range covers the type identifier");
  TestPipeline tp = {0};
  const char *src = "let x: Sring = \"hi\"";
  pipeline_run(&tp, src);
  ASSERT(sema_error_count(tp.sema) > 0);
  uint32_t start = sema_error_start(tp.sema, 0);
  uint32_t end = sema_error_end(tp.sema, 0);
  /* The "Sring" token should fall inside [start, end). */
  ASSERT(end > start);
  size_t srclen = strlen(src);
  ASSERT(start < srclen);
  ASSERT(end <= srclen);
  /* The slice itself should literally be "Sring". */
  ASSERT_EQ(end - start, 5);
  ASSERT(memcmp(src + start, "Sring", 5) == 0);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_error_range_distinct_per_diagnostic(void) {
  TEST("two errors at different sites → distinct ranges");
  TestPipeline tp = {0};
  const char *src = "let a: Foo = 1\nlet b: Bar = 2";
  pipeline_run(&tp, src);
  ASSERT(sema_error_count(tp.sema) >= 2);
  uint32_t s0 = sema_error_start(tp.sema, 0);
  uint32_t s1 = sema_error_start(tp.sema, 1);
  ASSERT(s0 != s1);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── Variadic call site (Tier 2.5) ───────────────────────────────────── */

static void test_sema_variadic_zero_args_ok(void) {
  TEST("variadic with zero args → resolves");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func sum(_ nums: Int...) -> Int { return 0 }\n"
               "let r = sum()");
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "sum")) ASSERT(0);
  }
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_EQ(let->type, TY_BUILTIN_INT);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_variadic_many_args_ok(void) {
  TEST("variadic with 3 args of element type → resolves");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func sum(_ nums: Int...) -> Int { return 0 }\n"
               "let r = sum(1, 2, 3)");
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_EQ(let->type, TY_BUILTIN_INT);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_variadic_after_fixed_ok(void) {
  TEST("fixed + variadic mix: f(Int, String...) → resolves");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func tag(_ id: Int, _ parts: String...) -> Int { return id }\n"
               "let r = tag(1, \"a\", \"b\")");
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_EQ(let->type, TY_BUILTIN_INT);
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_variadic_type_mismatch_rejected(void) {
  TEST("variadic with wrong element type → no match");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func sum(_ nums: Int...) -> Int { return 0 }\n"
               "func sum(_ tag: String) -> String { return tag }\n"
               "let r = sum(1, \"a\")");
  /* Neither overload should match `sum(1, "a")` cleanly:
   *   - Int... overload: second arg is String, not Int → eliminated
   *   - String overload: argc=2 > param_count=1, no variadic → eliminated
   * So r's type should not be assigned (NULL or fallback). */
  const ASTNode *let = find_desc(tp.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_EQ(let->type, NULL);
  pipeline_free(&tp);
  TEST_PASS();
}

/* ── @Sendable closure capture (Tier 2.2 part 2) ─────────────────────── */

static void test_sema_sendable_closure_captures_class_rejected(void) {
  TEST("@Sendable closure capturing a class instance → diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "class Bag { var x: Int = 0 }\n"
               "let obj = Bag()\n"
               "let f = { @Sendable in let _ = obj }");
  ASSERT(has_error_with(tp.sema, "non-Sendable", "@Sendable", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_sendable_closure_captures_struct_ok(void) {
  TEST("@Sendable closure capturing a Sendable struct → no diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "struct Pair { var a: Int; var b: Int }\n"
               "let s: Pair = Pair(a: 1, b: 2)\n"
               "let f = { @Sendable in let _ = s }");
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "non-Sendable")) ASSERT(0);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_plain_closure_ignores_sendable_check(void) {
  TEST("non-@Sendable closure capturing class → no diagnostic");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "class Bag { var x: Int = 0 }\n"
               "let obj = Bag()\n"
               "let f = { let _ = obj }");
  for (uint32_t i = 0; i < sema_error_count(tp.sema); i++) {
    const char *m = sema_error_message(tp.sema, i);
    if (m && strstr(m, "non-Sendable")) ASSERT(0);
  }
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_unannotated_closure_param_no_cycle(void) {
  TEST("unannotated closure parameter does not self-resolve as circular");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func f() {\n"
               "  let mapper = { index in index }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "circular reference", "index", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_let_self_reference_reports_cycle(void) {
  TEST("let x = x still reports a circular binding");
  TestPipeline tp = {0};
  pipeline_run(&tp, "let x = x");
  ASSERT(has_error_with(tp.sema, "circular reference", "x", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_as_question_mark_is_optional(void) {
  TEST("as? produces an optional result for nil coalescing");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func f(value: Any) -> Int {\n"
               "  let x = (value as? Int) ?? 0\n"
               "  return x\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "left side", "Optional type", NULL));
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

static void test_sema_optional_binding_may_shadow_parameter(void) {
  TEST("guard let x = x may shadow a parameter");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func f(member: Int) -> Int {\n"
               "  guard let member = member else { return 0 }\n"
               "  return member\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "Redefinition of 'member'", NULL, NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_if_let_shadow_with_unresolved_initializer_no_cycle(void) {
  TEST("if let x = unresolved(x) does not report a circular binding");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "func f(other: Int) -> Int {\n"
               "  if let other = makeOptional(other) {\n"
               "    return other\n"
               "  }\n"
               "  return 0\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "circular reference", "other", NULL));
  pipeline_free(&tp);
  TEST_PASS();
}

static void test_sema_unowned_self_capture_does_not_redefine_self(void) {
  TEST("[unowned self] trailing closure does not redefine self");
  TestPipeline tp = {0};
  pipeline_run(&tp,
               "class Owner {\n"
               "  func validate(_ body: (Int, Int, Int) -> Bool) {}\n"
               "  func run() {\n"
               "    validate { [unowned self] _, response, _ in\n"
               "      return true\n"
               "    }\n"
               "  }\n"
               "}");
  ASSERT(!has_error_with(tp.sema, "Redefinition of 'self'", NULL, NULL));
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
  test_sema_extension_static_stored_property_ok();
  test_sema_extension_escaped_static_computed_property_ok();
  test_sema_extension_computed_parenthesized_optional_type_ok();
  test_sema_extension_instance_stored_property_rejected();
  test_sema_extension_method_local_var_ok();
  test_sema_property_method_same_name_ok();
  test_sema_duplicate_property_same_name_rejected();
  test_sema_static_and_instance_property_same_name_ok();
  test_sema_switch_case_local_reuse_ok();
  test_sema_nominal_generic_param_visible_in_members();
  test_sema_extension_typealias_visible_in_members();
  test_sema_extension_typealias_visible_across_extensions();
  test_sema_extension_nested_lookup_many_extensions();
  test_sema_typealias_generic_params_visible_in_rhs();
  test_sema_function_generic_params_visible_in_return_type();
  test_sema_protocol_associated_type_visible_in_requirements();
  test_sema_nominal_typealias_visible_in_extension();
  test_sema_nested_nominal_visible_in_member_signatures();
  test_sema_extension_nested_type_visible_across_extensions();
  test_sema_metatype_suffix_type_silenced();
  test_sema_generic_qualified_suffix_not_generic_arg();
  test_sema_public_alias_to_builtin_and_any_ok();
  test_sema_internal_type_can_conform_to_private_protocol();
  test_sema_public_type_private_protocol_rejected();
  test_sema_set_of_enum_hashable_ok();
  test_sema_set_of_erased_and_generic_hashable_ok();
  test_sema_witness_happy_path();
  test_sema_witness_return_type_mismatch();
  test_sema_witness_param_count_mismatch();
  test_sema_witness_param_type_mismatch();
  test_sema_witness_param_label_mismatch();
  test_sema_witness_omitted_label_match();
  test_sema_witness_omitted_vs_named_label();
  test_sema_whole_module_conformance_origin_tokens();
  test_sema_overload_int_double_picks_int();
  test_sema_overload_int_double_picks_double();
  test_sema_overload_label_distinguishes();
  test_sema_overload_default_arg_omitted();
  test_sema_overload_ambiguity();
  test_sema_wrapper_projected_basic();
  test_sema_wrapper_missing_wrapped_value();
  test_sema_wrapper_no_projected_means_no_dollar();
  test_sema_main_actor_member_from_sync_rejected();
  test_sema_main_actor_member_from_main_actor_ok();
  test_sema_main_actor_member_with_await_ok();
  test_sema_actor_member_from_outside_rejected();
  test_sema_actor_member_with_await_ok();
  test_sema_nonisolated_member_always_reachable();
  test_sema_struct_of_sendable_is_sendable();
  test_sema_struct_with_class_member_not_sendable();
  test_sema_class_explicit_sendable_with_class_member_rejected();
  test_sema_class_explicit_sendable_with_generic_member_ok();
  test_sema_empty_enum_is_sendable();
  test_sema_generic_constraint_struct_rejected();
  test_sema_generic_constraint_int_rejected();
  test_sema_generic_constraint_protocol_ok();
  test_sema_conditional_extension_generic_arg_constraint_ok();
  test_sema_generic_constraint_class_ok();
  test_sema_builder_single_level();
  test_sema_builder_two_level_nesting();
  test_sema_builder_nested_call_arg_closure();
  test_sema_diagnostic_overflow_indicator();
  test_sema_await_in_sync_func_rejected();
  test_sema_await_in_async_func_ok();
  test_sema_await_in_closure_body_ok();
  test_sema_await_at_toplevel_rejected();
  test_sema_error_range_undeclared_type();
  test_sema_error_range_distinct_per_diagnostic();
  test_sema_variadic_zero_args_ok();
  test_sema_variadic_many_args_ok();
  test_sema_variadic_after_fixed_ok();
  test_sema_variadic_type_mismatch_rejected();
  test_sema_sendable_closure_captures_class_rejected();
  test_sema_sendable_closure_captures_struct_ok();
  test_sema_plain_closure_ignores_sendable_check();
  test_sema_unannotated_closure_param_no_cycle();
  test_sema_let_self_reference_reports_cycle();
  test_sema_as_question_mark_is_optional();
  test_sema_if_let_binding();
  test_sema_optional_binding_may_shadow_parameter();
  test_sema_if_let_shadow_with_unresolved_initializer_no_cycle();
  test_sema_unowned_self_capture_does_not_redefine_self();
}
