/**
 * @file test_framework.h
 * @brief Minimal C test framework — no dependencies, colored output.
 */
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include "msf.h"
#include <string.h>

extern int _tf_pass;
extern int _tf_fail;
extern int _tf_total;
extern const char *_tf_suite;

#define TEST_SUITE(name) \
  do { _tf_suite = (name); printf("\n\033[1;34m── %s\033[0m\n", name); } while (0)

#define TEST(name) \
  printf("  %-50s ", name); _tf_total++

#define PASS() \
  do { _tf_pass++; printf("\033[32mPASS\033[0m\n"); } while (0)

#define FAIL(msg) \
  do { _tf_fail++; printf("\033[31mFAIL\033[0m  %s (line %d)\n", msg, __LINE__); } while (0)

#define ASSERT(cond) \
  do { if (!(cond)) { FAIL(#cond); return; } } while (0)

#define ASSERT_EQ(a, b) \
  do { if ((a) != (b)) { FAIL(#a " != " #b); return; } } while (0)

#define ASSERT_NEQ(a, b) \
  do { if ((a) == (b)) { FAIL(#a " == " #b); return; } } while (0)

#define ASSERT_STR_EQ(a, b) \
  do { if (strcmp((a), (b)) != 0) { \
    char _buf[256]; snprintf(_buf, sizeof(_buf), "'%s' != '%s'", (a), (b)); \
    FAIL(_buf); return; } } while (0)

#define ASSERT_NULL(p) \
  do { if ((p) != NULL) { FAIL(#p " is not NULL"); return; } } while (0)

#define ASSERT_NOT_NULL(p) \
  do { if ((p) == NULL) { FAIL(#p " is NULL"); return; } } while (0)

#define TEST_PASS() PASS()

#define TEST_SUMMARY() \
  do { \
    printf("\n\033[1m── Summary: %d passed, %d failed, %d total\033[0m\n", \
           _tf_pass, _tf_fail, _tf_total); \
    if (_tf_fail > 0) printf("\033[31m   FAILURES DETECTED\033[0m\n"); \
    else printf("\033[32m   ALL TESTS PASSED\033[0m\n"); \
  } while (0)

#define TEST_EXIT_CODE() (_tf_fail > 0 ? 1 : 0)

/* Helper: create a Source + tokenize in one call */
static inline int test_tokenize(const char *code, Source *src, TokenStream *ts) {
  src->data = code;
  src->len = strlen(code);
  src->filename = "<test>";
  token_stream_init(ts, 256);
  return lexer_tokenize(src, ts, 1, NULL);
}

#endif /* TEST_FRAMEWORK_H */
