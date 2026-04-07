/**
 * @file test_main.c
 * @brief Test runner — invokes all test suites and reports results.
 */
#include "test_framework.h"

/* Global counters (declared extern in test_framework.h) */
int _tf_pass = 0;
int _tf_fail = 0;
int _tf_total = 0;
const char *_tf_suite = "";

/* Test suite runners (defined in separate files) */
extern void run_lexer_tests(void);
extern void run_parser_tests(void);
extern void run_type_tests(void);
extern void run_sema_tests(void);
extern void run_integration_tests(void);
extern void run_regression_tests(void);
extern void run_error_recovery_tests(void);

int main(void) {
  printf("\033[1m══════════════════════════════════════════════════════════════\033[0m\n");
  printf("\033[1m  MiniSwiftFrontend — Unit Tests\033[0m\n");
  printf("\033[1m══════════════════════════════════════════════════════════════\033[0m\n");

  run_lexer_tests();
  run_parser_tests();
  run_type_tests();
  run_sema_tests();
  run_integration_tests();
  run_regression_tests();
  run_error_recovery_tests();

  TEST_SUMMARY();
  return TEST_EXIT_CODE();
}
