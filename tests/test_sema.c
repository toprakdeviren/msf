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
  test_sema_witness_happy_path();
  test_sema_witness_return_type_mismatch();
  test_sema_witness_param_count_mismatch();
  test_sema_witness_param_type_mismatch();
  test_sema_witness_param_label_mismatch();
  test_sema_witness_omitted_label_match();
  test_sema_witness_omitted_vs_named_label();
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
  test_sema_empty_enum_is_sendable();
  test_sema_generic_constraint_struct_rejected();
  test_sema_generic_constraint_int_rejected();
  test_sema_generic_constraint_protocol_ok();
  test_sema_generic_constraint_class_ok();
  test_sema_if_let_binding();
}
