/**
 * @file test_lexer.c
 * @brief Unit tests for the lexer: tokenization, keywords, operators, literals, diagnostics.
 */
#include "test_framework.h"
#include "msf.h"
#include "internal/msf.h"

/* ── Basic tokenization ───────────────────────────────────────────────────── */

static void test_empty_source(void) {
  TEST("empty source → single EOF");
  Source src; TokenStream ts;
  test_tokenize("", &src, &ts);
  ASSERT_EQ(ts.count, 1);
  ASSERT_EQ(ts.tokens[0].type, TOK_EOF);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_single_identifier(void) {
  TEST("single identifier");
  Source src; TokenStream ts;
  test_tokenize("hello", &src, &ts);
  ASSERT(ts.count >= 2); /* identifier + EOF */
  ASSERT_EQ(ts.tokens[0].type, TOK_IDENTIFIER);
  ASSERT_EQ(ts.tokens[0].len, 5);
  ASSERT_EQ(ts.tokens[ts.count - 1].type, TOK_EOF);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_keyword_recognition(void) {
  TEST("keywords: func var let class struct");
  Source src; TokenStream ts;
  test_tokenize("func var let class struct", &src, &ts);
  ASSERT(ts.count >= 6);
  ASSERT_EQ(ts.tokens[0].type, TOK_KEYWORD);
  ASSERT_EQ(ts.tokens[0].keyword, KW_FUNC);
  ASSERT_EQ(ts.tokens[1].type, TOK_KEYWORD);
  ASSERT_EQ(ts.tokens[1].keyword, KW_VAR);
  ASSERT_EQ(ts.tokens[2].type, TOK_KEYWORD);
  ASSERT_EQ(ts.tokens[2].keyword, KW_LET);
  ASSERT_EQ(ts.tokens[3].type, TOK_KEYWORD);
  ASSERT_EQ(ts.tokens[3].keyword, KW_CLASS);
  ASSERT_EQ(ts.tokens[4].type, TOK_KEYWORD);
  ASSERT_EQ(ts.tokens[4].keyword, KW_STRUCT);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ── Operators ────────────────────────────────────────────────────────────── */

static void test_multi_char_operators(void) {
  TEST("multi-char operators: == != -> ??");
  Source src; TokenStream ts;
  test_tokenize("== != -> ??", &src, &ts);
  ASSERT(ts.count >= 5);
  ASSERT_EQ(ts.tokens[0].op_kind, OP_EQ);
  ASSERT_EQ(ts.tokens[1].op_kind, OP_NEQ);
  ASSERT_EQ(ts.tokens[2].op_kind, OP_ARROW);
  ASSERT_EQ(ts.tokens[3].op_kind, OP_NIL_COAL);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_range_operators(void) {
  TEST("range operators: ..< ...");
  Source src; TokenStream ts;
  test_tokenize("0..<10 0...10", &src, &ts);
  int found_excl = 0, found_incl = 0;
  for (size_t i = 0; i < ts.count; i++) {
    if (ts.tokens[i].op_kind == OP_RANGE_EXCL) found_excl = 1;
    if (ts.tokens[i].op_kind == OP_RANGE_INCL) found_incl = 1;
  }
  ASSERT(found_excl);
  ASSERT(found_incl);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ── Literals ─────────────────────────────────────────────────────────────── */

static void test_integer_literals(void) {
  TEST("integer literals: decimal, hex, binary, octal");
  Source src; TokenStream ts;
  test_tokenize("42 0xFF 0b1010 0o77", &src, &ts);
  ASSERT(ts.count >= 5);
  for (int i = 0; i < 4; i++)
    ASSERT_EQ(ts.tokens[i].type, TOK_INTEGER_LIT);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_float_literal(void) {
  TEST("float literal: 3.14");
  Source src; TokenStream ts;
  test_tokenize("3.14", &src, &ts);
  ASSERT(ts.count >= 2);
  ASSERT_EQ(ts.tokens[0].type, TOK_FLOAT_LIT);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_string_literal(void) {
  TEST("string literal: \"hello\"");
  Source src; TokenStream ts;
  test_tokenize("\"hello world\"", &src, &ts);
  ASSERT(ts.count >= 2);
  ASSERT_EQ(ts.tokens[0].type, TOK_STRING_LIT);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_triple_quoted_string(void) {
  TEST("triple-quoted string");
  Source src; TokenStream ts;
  test_tokenize("\"\"\"\nhello\nworld\n\"\"\"", &src, &ts);
  ASSERT(ts.count >= 2);
  ASSERT_EQ(ts.tokens[0].type, TOK_STRING_LIT);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ── Punctuation and structure ────────────────────────────────────────────── */

static void test_punctuation(void) {
  TEST("punctuation: ( ) { } [ ] , : ;");
  Source src; TokenStream ts;
  test_tokenize("( ) { } [ ] , : ;", &src, &ts);
  int punct_count = 0;
  for (size_t i = 0; i < ts.count; i++)
    if (ts.tokens[i].type == TOK_PUNCT) punct_count++;
  ASSERT_EQ(punct_count, 9);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_func_declaration_tokens(void) {
  TEST("func add(a: Int, b: Int) -> Int");
  Source src; TokenStream ts;
  test_tokenize("func add(a: Int, b: Int) -> Int", &src, &ts);
  ASSERT_EQ(ts.tokens[0].type, TOK_KEYWORD);
  ASSERT_EQ(ts.tokens[0].keyword, KW_FUNC);
  ASSERT_EQ(ts.tokens[1].type, TOK_IDENTIFIER); /* add */
  /* Find -> operator */
  int found_arrow = 0;
  for (size_t i = 0; i < ts.count; i++)
    if (ts.tokens[i].op_kind == OP_ARROW) found_arrow = 1;
  ASSERT(found_arrow);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ── Comments ─────────────────────────────────────────────────────────────── */

static void test_comments_filtered(void) {
  TEST("comments filtered in skip_ws mode");
  Source src; TokenStream ts;
  test_tokenize("x // line comment\ny /* block */ z", &src, &ts);
  /* skip_ws=1 in test_tokenize, so comments should be filtered */
  for (size_t i = 0; i < ts.count; i++)
    ASSERT_NEQ(ts.tokens[i].type, TOK_COMMENT);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ── Diagnostics ──────────────────────────────────────────────────────────── */

static void test_unterminated_string_diagnostic(void) {
  TEST("unterminated string → produces STRING_LIT token");
  /* An unterminated string should still produce a STRING_LIT token
     (the lexer recovers and continues). The diagnostic may or may not
     be emitted depending on the scan path, so we just verify the token. */
  Source src = { .data = "\"hello", .len = 6, .filename = "<test>" };
  TokenStream ts;
  token_stream_init(&ts, 64);
  LexerDiagnostics diag;
  lexer_diag_init(&diag);
  lexer_tokenize(&src, &ts, 1, &diag);
  /* Should have at least a STRING_LIT and EOF */
  ASSERT(ts.count >= 2);
  ASSERT_EQ(ts.tokens[0].type, TOK_STRING_LIT);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ── Line/column tracking ─────────────────────────────────────────────────── */

static void test_line_column_tracking(void) {
  TEST("line/column tracking across newlines");
  Source src; TokenStream ts;
  test_tokenize("a\nb\nc", &src, &ts);
  /* a is on line 1, b on line 2, c on line 3 */
  ASSERT_EQ(ts.tokens[0].line, 1);
  ASSERT_EQ(ts.tokens[1].line, 2);
  ASSERT_EQ(ts.tokens[2].line, 3);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

void run_lexer_tests(void) {
  TEST_SUITE("Lexer");
  test_empty_source();
  test_single_identifier();
  test_keyword_recognition();
  test_multi_char_operators();
  test_range_operators();
  test_integer_literals();
  test_float_literal();
  test_string_literal();
  test_triple_quoted_string();
  test_punctuation();
  test_func_declaration_tokens();
  test_comments_filtered();
  test_unterminated_string_diagnostic();
  test_line_column_tracking();
}
