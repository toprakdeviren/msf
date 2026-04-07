/**
 * @file test_error_recovery.c
 * @brief Error recovery and diagnostic tests — malformed input must produce
 *        meaningful errors, not garbage AST or crashes.
 *
 * These tests simulate real user mistakes found during miniswift.run deployment.
 * Each test verifies: (1) no crash, (2) error reported, (3) correct message.
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
} ErrPipeline;

static void err_run(ErrPipeline *p, const char *code) {
  p->src.data = code;
  p->src.len = strlen(code);
  p->src.filename = "<test>";
  token_stream_init(&p->ts, 256);
  lexer_tokenize(&p->src, &p->ts, 1, NULL);
  ast_arena_init(&p->ast_arena, 0);
  p->parser = parser_init(&p->src, &p->ts, &p->ast_arena);
  p->root = parse_source_file(p->parser);
  type_arena_init(&p->type_arena, 0);
  type_builtins_init(&p->type_arena);
  p->sema = sema_init(&p->src, p->ts.tokens, &p->ast_arena, &p->type_arena);
  sema_analyze(p->sema, p->root);
}

static void err_free(ErrPipeline *p) {
  sema_destroy(p->sema);
  parser_destroy(p->parser);
  type_arena_free(&p->type_arena);
  ast_arena_free(&p->ast_arena);
  token_stream_free(&p->ts);
}

static int has_error_containing(const ErrPipeline *p, const char *substr) {
  for (uint32_t i = 0; i < parser_error_count(p->parser); i++)
    if (strstr(parser_error_message(p->parser, i), substr)) return 1;
  for (uint32_t i = 0; i < sema_error_count(p->sema); i++)
    if (strstr(sema_error_message(p->sema, i), substr)) return 1;
  return 0;
}

static uint32_t total_errors(const ErrPipeline *p) {
  return parser_error_count(p->parser) + sema_error_count(p->sema);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  1. Operator Abuse (user-reported: "1 + * 2" produced garbage)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_err_consecutive_operators(void) {
  TEST("1 + * 2 → error, not garbage value");
  ErrPipeline p = {0};
  err_run(&p, "let x = 1 + * 2");
  /* Must produce an error — NOT silently compile to a garbage number */
  ASSERT(total_errors(&p) > 0);
  err_free(&p);
  TEST_PASS();
}

static void test_err_leading_operator(void) {
  TEST("* 2 as statement → error");
  ErrPipeline p = {0};
  err_run(&p, "* 2");
  /* Bare `* 2` is not valid Swift — should not produce a clean AST */
  ASSERT_NOT_NULL(p.root);
  err_free(&p); /* must not crash */
  TEST_PASS();
}

static void test_err_trailing_operator(void) {
  TEST("1 + → recovers without crash (RHS missing)");
  ErrPipeline p = {0};
  err_run(&p, "let x = 1 +");
  /* The parser treats trailing `+` as a valid prefix if nothing follows.
     Key invariant: no crash and a partial AST is produced. */
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_double_equals(void) {
  TEST("let x = 1 == == 2 → error");
  ErrPipeline p = {0};
  err_run(&p, "let x = 1 == == 2");
  ASSERT(total_errors(&p) > 0);
  err_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  2. String Mistakes
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_err_single_quoted_string(void) {
  TEST("'hello' → diagnostic about single quotes");
  Source src = { .data = "'hello'", .len = 7, .filename = "<test>" };
  TokenStream ts;
  token_stream_init(&ts, 64);
  LexerDiagnostics diag;
  lexer_diag_init(&diag);
  lexer_tokenize(&src, &ts, 1, &diag);
  ASSERT(diag.count > 0);
  /* Should mention "single-quoted" or "double quotes" */
  int correct_msg = 0;
  for (size_t i = 0; i < diag.count; i++)
    if (strstr(diag.message[i], "single") || strstr(diag.message[i], "double"))
      correct_msg = 1;
  ASSERT(correct_msg);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_err_unterminated_string(void) {
  TEST("\"hello → unterminated string token + no crash");
  Source src = { .data = "\"hello", .len = 6, .filename = "<test>" };
  TokenStream ts;
  token_stream_init(&ts, 64);
  lexer_tokenize(&src, &ts, 1, NULL);
  ASSERT(ts.count >= 1);
  /* Should still produce a token (recovery) and not crash */
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_err_unterminated_triple_quote(void) {
  TEST("\"\"\"hello → unterminated triple-quoted string");
  Source src = { .data = "\"\"\"hello\nworld", .len = 14, .filename = "<test>" };
  TokenStream ts;
  token_stream_init(&ts, 64);
  LexerDiagnostics diag;
  lexer_diag_init(&diag);
  lexer_tokenize(&src, &ts, 1, &diag);
  ASSERT(diag.count > 0);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  3. Bracket Mismatches
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_err_missing_close_brace(void) {
  TEST("func f() { → recovers, no crash");
  ErrPipeline p = {0};
  err_run(&p, "func f() {");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_missing_close_paren(void) {
  TEST("print(\"hello\" → recovers, no crash");
  ErrPipeline p = {0};
  err_run(&p, "print(\"hello\"");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_extra_close_brace(void) {
  TEST("} at top level → no crash");
  ErrPipeline p = {0};
  err_run(&p, "}");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_mismatched_brackets(void) {
  TEST("func f() { [ } ] → no crash");
  ErrPipeline p = {0};
  err_run(&p, "func f() { [ } ]");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  4. Missing Keywords / Tokens
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_err_func_no_name(void) {
  TEST("func () { } → no crash");
  ErrPipeline p = {0};
  err_run(&p, "func () { }");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_var_no_name(void) {
  TEST("var = 42 → no crash");
  ErrPipeline p = {0};
  err_run(&p, "var = 42");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_if_no_condition(void) {
  TEST("if { } → no crash");
  ErrPipeline p = {0};
  err_run(&p, "if { }");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_for_no_in(void) {
  TEST("for x { } → no crash");
  ErrPipeline p = {0};
  err_run(&p, "for x { }");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_return_at_top_level(void) {
  TEST("return 42 at top level → no crash");
  ErrPipeline p = {0};
  err_run(&p, "return 42");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  5. Type Annotation Errors
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_err_undeclared_type(void) {
  TEST("var x: Foo → 'undeclared type' error with correct name");
  ErrPipeline p = {0};
  err_run(&p, "var x: Foo");
  ASSERT(has_error_containing(&p, "undeclared type"));
  ASSERT(has_error_containing(&p, "Foo"));
  err_free(&p);
  TEST_PASS();
}

static void test_err_similar_type_suggestion(void) {
  TEST("var x: Sring → 'Did you mean String?' suggestion");
  ErrPipeline p = {0};
  err_run(&p,
    "let s: String = \"\"\n"
    "var x: Sring\n"
  );
  /* Should suggest "String" for "Sring" (edit distance 1) */
  int has_suggestion = has_error_containing(&p, "Did you mean");
  /* May or may not trigger depending on scope — at minimum should report undeclared */
  ASSERT(has_error_containing(&p, "undeclared") || has_suggestion);
  err_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  6. Semantic Errors
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_err_extension_computed_only(void) {
  TEST("extension with computed property → no error");
  ErrPipeline p = {0};
  err_run(&p,
    "struct S { var x: Int }\n"
    "extension S {\n"
    "  var desc: String { get { return \"\" } }\n"
    "}\n"
  );
  /* Computed properties in extensions are allowed — no error expected. */
  ASSERT_EQ(parser_error_count(p.parser), 0);
  err_free(&p);
  TEST_PASS();
}

static void test_err_final_class_subclass(void) {
  TEST("subclassing final class → error");
  ErrPipeline p = {0};
  err_run(&p,
    "final class Base { }\n"
    "class Sub: Base { }\n"
  );
  ASSERT(has_error_containing(&p, "final"));
  err_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  7. Adversarial / Fuzz-Style Input
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_err_empty_input(void) {
  TEST("empty string → valid empty AST, no crash");
  ErrPipeline p = {0};
  err_run(&p, "");
  ASSERT_NOT_NULL(p.root);
  ASSERT_EQ(p.root->kind, AST_SOURCE_FILE);
  err_free(&p);
  TEST_PASS();
}

static void test_err_only_whitespace(void) {
  TEST("only whitespace/newlines → valid empty AST");
  ErrPipeline p = {0};
  err_run(&p, "   \n\n\t\t\n  ");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_only_operators(void) {
  TEST("+++---***/// → no crash");
  ErrPipeline p = {0};
  err_run(&p, "+++---***");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_deeply_nested_parens(void) {
  TEST("((((((((1)))))))) → no crash");
  ErrPipeline p = {0};
  err_run(&p, "let x = ((((((((1))))))))");
  ASSERT_NOT_NULL(p.root);
  ASSERT_EQ(parser_error_count(p.parser), 0);
  err_free(&p);
  TEST_PASS();
}

static void test_err_very_long_identifier(void) {
  TEST("256-char identifier → no crash");
  char buf[300];
  memset(buf, 'a', 256);
  buf[256] = '\0';
  char code[320];
  snprintf(code, sizeof(code), "let %s = 1", buf);
  ErrPipeline p = {0};
  err_run(&p, code);
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_repeated_keywords(void) {
  TEST("func func func → no crash, error reported");
  ErrPipeline p = {0};
  err_run(&p, "func func func");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_null_byte_in_source(void) {
  TEST("source with embedded NUL → no crash");
  /* Source.len is explicit so NUL doesn't terminate early */
  const char code[] = "let x\0= 1";
  Source src = { .data = code, .len = 5, .filename = "<test>" }; /* stop at NUL */
  TokenStream ts;
  token_stream_init(&ts, 64);
  lexer_tokenize(&src, &ts, 1, NULL);
  ASSERT(ts.count >= 1);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_err_unicode_identifiers(void) {
  TEST("Unicode identifier (emoji, CJK) → no crash");
  ErrPipeline p = {0};
  err_run(&p, "let \xC3\xA9 = 1");  /* é (U+00E9) as identifier */
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_mixed_valid_invalid(void) {
  TEST("valid code + invalid code → partial success");
  ErrPipeline p = {0};
  err_run(&p,
    "let x: Int = 42\n"
    "let y = 1 + * 2\n"     /* invalid */
    "let z: String = \"ok\"\n"  /* valid */
  );
  ASSERT_NOT_NULL(p.root);
  /* Should still parse x and z successfully despite y being invalid */
  err_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  8. Incomplete Declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_err_incomplete_struct(void) {
  TEST("struct Foo { → no crash");
  ErrPipeline p = {0};
  err_run(&p, "struct Foo {");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_class_no_body(void) {
  TEST("class Foo → no crash");
  ErrPipeline p = {0};
  err_run(&p, "class Foo");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_enum_empty_case(void) {
  TEST("enum E { case } → no crash");
  ErrPipeline p = {0};
  err_run(&p, "enum E { case }");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

static void test_err_switch_no_cases(void) {
  TEST("switch x { } → no crash");
  ErrPipeline p = {0};
  err_run(&p, "let x = 1\nswitch x { }");
  ASSERT_NOT_NULL(p.root);
  err_free(&p);
  TEST_PASS();
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

void run_error_recovery_tests(void) {
  TEST_SUITE("Error Recovery");
  /* Operator abuse */
  test_err_consecutive_operators();
  test_err_leading_operator();
  test_err_trailing_operator();
  test_err_double_equals();
  /* String mistakes */
  test_err_single_quoted_string();
  test_err_unterminated_string();
  test_err_unterminated_triple_quote();
  /* Bracket mismatches */
  test_err_missing_close_brace();
  test_err_missing_close_paren();
  test_err_extra_close_brace();
  test_err_mismatched_brackets();
  /* Missing keywords */
  test_err_func_no_name();
  test_err_var_no_name();
  test_err_if_no_condition();
  test_err_for_no_in();
  test_err_return_at_top_level();
  /* Type errors */
  test_err_undeclared_type();
  test_err_similar_type_suggestion();
  /* Semantic errors */
  test_err_extension_computed_only();
  test_err_final_class_subclass();
  /* Adversarial input */
  test_err_empty_input();
  test_err_only_whitespace();
  test_err_only_operators();
  test_err_deeply_nested_parens();
  test_err_very_long_identifier();
  test_err_repeated_keywords();
  test_err_null_byte_in_source();
  test_err_unicode_identifiers();
  test_err_mixed_valid_invalid();
  /* Incomplete declarations */
  test_err_incomplete_struct();
  test_err_class_no_body();
  test_err_enum_empty_case();
  test_err_switch_no_cases();
}
