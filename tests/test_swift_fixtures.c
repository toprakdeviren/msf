/**
 * @file test_swift_fixtures.c
 * @brief Diagnostic-parity runner over tests/swift-fixtures/*.swift.
 *
 * Each fixture is a real .swift file annotated with apple/swift-style
 * markers in line comments:
 *
 *   let x: Int = "hi"  // expected-error{{Type mismatch}}
 *   let y = "unterm    // expected-error{{unterminated string}}
 *
 * The runner walks the fixture tree, calls msf_analyze() on each file,
 * and checks that:
 *
 *   1. Every `expected-error{{substring}}` annotation matches at least one
 *      diagnostic on the same line whose message contains the substring.
 *   2. (Optionally, via `// expected-no-diagnostics` at file scope)
 *      the file produces zero diagnostics.
 *
 * The match is asymmetric: a fixture without `// expected-no-diagnostics`
 * may produce extra diagnostics msf reports beyond the annotated ones —
 * those are ignored.  This keeps the corpus from over-locking msf's
 * current diagnostic wording while still asserting the load-bearing
 * cases pass.
 *
 * Each fixture becomes one entry in the regular TEST() output; passes
 * and failures roll up into the suite summary like any other test.
 */
#include "test_framework.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SWIFT_FIXTURES_DIR
#define SWIFT_FIXTURES_DIR "tests/swift-fixtures"
#endif

/* ── Expectation parsing ──────────────────────────────────────────────── */

typedef struct {
  int  line;             /* 1-based source line the annotation appears on */
  char substr[200];      /* required substring of the diagnostic message  */
  int  matched;          /* set when a diagnostic satisfies this entry    */
} Expectation;

#define MAX_EXPECTATIONS 128

/* Returns the 1-based line of byte offset @p p inside @p src. */
static int line_of_offset(const char *src, const char *p) {
  int line = 1;
  for (const char *q = src; q < p; q++)
    if (*q == '\n') line++;
  return line;
}

/* Scans @p src for `// expected-error{{message}}` markers.  Stores each
 * annotation's source line and substring; returns the count (capped). */
static size_t parse_expectations(const char *src, Expectation *out, size_t cap) {
  static const char marker[] = "// expected-error{{";
  size_t count = 0;
  const char *p = src;
  while ((p = strstr(p, marker)) != NULL && count < cap) {
    const char *m = p + (sizeof(marker) - 1);
    const char *end = strstr(m, "}}");
    if (!end) break;
    size_t len = (size_t)(end - m);
    if (len >= sizeof(out[count].substr)) len = sizeof(out[count].substr) - 1;
    memcpy(out[count].substr, m, len);
    out[count].substr[len] = '\0';
    out[count].line = line_of_offset(src, p);
    out[count].matched = 0;
    count++;
    p = end + 2;
  }
  return count;
}

/* ── File I/O ─────────────────────────────────────────────────────────── */

static char *slurp_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  char *buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = '\0';
  fclose(f);
  if (out_len) *out_len = got;
  return buf;
}

/* Trims SWIFT_FIXTURES_DIR off the front of @p path for prettier output. */
static const char *short_name(const char *path) {
  size_t plen = strlen(SWIFT_FIXTURES_DIR);
  if (strncmp(path, SWIFT_FIXTURES_DIR, plen) == 0) {
    const char *p = path + plen;
    while (*p == '/') p++;
    return p;
  }
  return path;
}

/* ── One fixture ──────────────────────────────────────────────────────── */

static void run_one_fixture(const char *path) {
  /* Use the path (minus the fixtures-dir prefix) as the test name so a
   * failing fixture is grep-able in the suite output. */
  char label[256];
  snprintf(label, sizeof(label), "fixture: %s", short_name(path));
  TEST(label);

  size_t src_len = 0;
  char *src = slurp_file(path, &src_len);
  if (!src) { FAIL("cannot read fixture"); return; }

  Expectation expectations[MAX_EXPECTATIONS];
  size_t exp_count = parse_expectations(src, expectations, MAX_EXPECTATIONS);
  int expect_clean = (strstr(src, "// expected-no-diagnostics") != NULL);

  MSFResult *r = msf_analyze(src, path);
  if (!r) { free(src); FAIL("msf_analyze returned NULL"); return; }

  uint32_t total = msf_error_count(r);

  /* Walk diagnostics, mark each expectation that matches. */
  for (uint32_t i = 0; i < total; i++) {
    uint32_t eline = msf_error_line(r, i);
    const char *msg = msf_error_message(r, i);
    for (size_t j = 0; j < exp_count; j++) {
      if (expectations[j].matched) continue;
      if ((int)eline != expectations[j].line) continue;
      if (strstr(msg, expectations[j].substr)) {
        expectations[j].matched = 1;
        break;
      }
    }
  }

  int unmatched = 0;
  for (size_t j = 0; j < exp_count; j++)
    if (!expectations[j].matched) unmatched++;

  int ok;
  if (expect_clean) {
    ok = (total == 0);
  } else {
    ok = (unmatched == 0);
  }

  if (ok) {
    msf_result_free(r);
    free(src);
    PASS();
    return;
  }

  /* Failure: report just enough to debug.  Don't dump every diagnostic —
   * that'd flood the output for fixtures where msf emits cascades. */
  if (expect_clean && total > 0) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "expected-no-diagnostics but got %u (first: '%s')",
             total, msf_error_message(r, 0));
    msf_result_free(r);
    free(src);
    FAIL(buf);
    return;
  }
  for (size_t j = 0; j < exp_count; j++) {
    if (expectations[j].matched) continue;
    char buf[256];
    snprintf(buf, sizeof(buf), "unmatched expectation line %d: '%s'",
             expectations[j].line, expectations[j].substr);
    msf_result_free(r);
    free(src);
    FAIL(buf);
    return;
  }
  msf_result_free(r);
  free(src);
  FAIL("unknown failure");
}

/* ── Directory walk ───────────────────────────────────────────────────── */

/* qsort callback for deterministic fixture ordering. */
static int strptr_cmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Recursively collects all *.swift files under @p dir into @p out.
 * Caller owns the returned strings; results are sorted for stable output. */
static void collect_fixtures(const char *dir, char ***out, size_t *cnt,
                             size_t *cap) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      collect_fixtures(path, out, cnt, cap);
    } else if (S_ISREG(st.st_mode)) {
      const char *dot = strrchr(e->d_name, '.');
      if (!dot || strcmp(dot, ".swift") != 0) continue;
      if (*cnt == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 32;
        char **n = realloc(*out, new_cap * sizeof(*n));
        if (!n) continue;
        *out = n;
        *cap = new_cap;
      }
      (*out)[(*cnt)++] = strdup(path);
    }
  }
  closedir(d);
}

/* ── Suite entry point ────────────────────────────────────────────────── */

void run_swift_fixture_tests(void) {
  TEST_SUITE("Swift Fixtures");
  char **paths = NULL;
  size_t count = 0, cap = 0;
  collect_fixtures(SWIFT_FIXTURES_DIR, &paths, &count, &cap);
  if (count == 0) {
    /* No fixtures — emit one TEST so the suite line is non-empty and
     * a missing directory is loud rather than silent. */
    TEST("(no fixtures found)");
    FAIL("SWIFT_FIXTURES_DIR is empty or unreadable");
    return;
  }
  qsort(paths, count, sizeof(*paths), strptr_cmp);
  for (size_t i = 0; i < count; i++) {
    run_one_fixture(paths[i]);
    free(paths[i]);
  }
  free(paths);
}
