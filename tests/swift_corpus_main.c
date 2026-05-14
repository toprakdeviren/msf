/**
 * @file swift_corpus_main.c
 * @brief Standalone runner that walks an apple/swift-style .swift corpus
 *        and reports diagnostic parity against msf_analyze().
 *
 * Designed for the apple/swift `test/Parse` and `test/Sema` directories,
 * which use lit-style annotations:
 *
 *   let x: Int = "hi"            // expected-error{{cannot convert ...}}
 *   func f(                       // expected-error@-1{{...}}
 *   let y = 1 + 2 + 3 + // OK    // expected-warning{{...}}
 *   let z = 1                     // expected-error 2 {{...}}
 *
 * The runner is deliberately exploratory by default — msf does not
 * implement the full Swift dialect, so most fixtures will mismatch on
 * something.  Use it like libwgsl's `make test-cts`: counts pass/fail,
 * writes a JSON report, only fails the target under `--strict`.
 *
 * Usage:
 *   swift_corpus_runner --corpus DIR [--report PATH] [--max-failures N]
 *                       [--filter SUBSTR] [--limit N] [--strict] [--quiet]
 *
 * Exit codes:
 *   0  success (or non-strict mode regardless of mismatches)
 *   1  --strict and any mismatch occurred
 *   2  usage / I/O error
 */
#include <msf.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── Options ──────────────────────────────────────────────────────────── */

typedef struct {
  const char *corpus;       /* required */
  const char *report_path;  /* optional JSON output */
  const char *filter;       /* optional substring filter on path */
  int  max_failures;        /* 0 = unlimited terminal-printed failures */
  int  limit;               /* 0 = unlimited fixtures */
  int  timeout_sec;         /* per-fixture wall-time cap (0 = no isolation) */
  int  strict;              /* exit 1 on mismatch */
  int  quiet;               /* don't print per-fixture status */
} Options;

static void usage(void) {
  fprintf(stderr,
    "usage: swift_corpus_runner --corpus DIR [options]\n"
    "  --report PATH       write JSON summary to PATH\n"
    "  --filter SUBSTR     skip fixtures whose path does not contain SUBSTR\n"
    "  --limit N           cap the number of fixtures run\n"
    "  --timeout SEC       per-fixture wall-time cap; runs each msf_analyze\n"
    "                      in a forked child so a hang/crash on one fixture\n"
    "                      doesn't kill the run (default 10, 0 disables)\n"
    "  --max-failures N    cap terminal-printed failure detail (default 20)\n"
    "  --strict            exit 1 if any fixture mismatches\n"
    "  --quiet             suppress per-fixture status lines\n");
}

static int parse_int(const char *s) {
  return (int)strtol(s, NULL, 10);
}

static int parse_options(int argc, char **argv, Options *opt) {
  memset(opt, 0, sizeof(*opt));
  opt->max_failures = 20;
  opt->timeout_sec  = 10;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--corpus") && i + 1 < argc) {
      opt->corpus = argv[++i];
    } else if (!strcmp(argv[i], "--report") && i + 1 < argc) {
      opt->report_path = argv[++i];
    } else if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
      opt->filter = argv[++i];
    } else if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
      opt->limit = parse_int(argv[++i]);
    } else if (!strcmp(argv[i], "--max-failures") && i + 1 < argc) {
      opt->max_failures = parse_int(argv[++i]);
    } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
      opt->timeout_sec = parse_int(argv[++i]);
    } else if (!strcmp(argv[i], "--strict")) {
      opt->strict = 1;
    } else if (!strcmp(argv[i], "--quiet")) {
      opt->quiet = 1;
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage();
      return 0;
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      usage();
      return -1;
    }
  }
  if (!opt->corpus) {
    fprintf(stderr, "--corpus DIR is required\n");
    usage();
    return -1;
  }
  return 1;
}

/* ── Annotation parsing ───────────────────────────────────────────────── */

typedef enum { KIND_ERROR, KIND_WARNING, KIND_NOTE } AnnKind;

typedef struct {
  AnnKind kind;
  int     line;       /* target line (1-based, after applying offset) */
  int     count;      /* expected count (default 1) */
  int     matched;    /* how many diagnostics have matched so far */
  char    substr[256];
} Annotation;

#define MAX_ANNOTATIONS 256

/* Returns 1-based line number of byte @p p inside @p src. */
static int line_of(const char *src, const char *p) {
  int line = 1;
  for (const char *q = src; q < p; q++)
    if (*q == '\n') line++;
  return line;
}

/* Parses every `// expected-{error,warning,note}[@-N|@+N][ N]{{substr}}`
 * annotation in @p src.  Only KIND_ERROR annotations are checked against
 * diagnostics; warnings/notes are recognised so the count is reported but
 * skipped during matching (msf has no warning channel yet). */
static size_t parse_annotations(const char *src, Annotation *out, size_t cap) {
  size_t count = 0;
  const char *p = src;
  while (count < cap) {
    const char *marker = strstr(p, "// expected-");
    if (!marker) break;
    const char *cur = marker + strlen("// expected-");

    AnnKind kind;
    if      (!strncmp(cur, "error",   5)) { kind = KIND_ERROR;   cur += 5; }
    else if (!strncmp(cur, "warning", 7)) { kind = KIND_WARNING; cur += 7; }
    else if (!strncmp(cur, "note",    4)) { kind = KIND_NOTE;    cur += 4; }
    else { p = marker + 1; continue; }

    /* Optional @-N or @+N offset. */
    int offset = 0;
    if (*cur == '@') {
      cur++;
      int sign = 1;
      if (*cur == '+') cur++;
      else if (*cur == '-') { sign = -1; cur++; }
      while (*cur >= '0' && *cur <= '9') {
        offset = offset * 10 + (*cur - '0');
        cur++;
      }
      offset *= sign;
    }

    /* Optional ` N ` count modifier. */
    int expect_count = 1;
    while (*cur == ' ' || *cur == '\t') cur++;
    if (*cur >= '0' && *cur <= '9') {
      expect_count = 0;
      while (*cur >= '0' && *cur <= '9') {
        expect_count = expect_count * 10 + (*cur - '0');
        cur++;
      }
      while (*cur == ' ' || *cur == '\t') cur++;
    }

    if (cur[0] != '{' || cur[1] != '{') { p = marker + 1; continue; }
    cur += 2;
    const char *end = strstr(cur, "}}");
    if (!end) break;

    Annotation *a = &out[count++];
    a->kind  = kind;
    a->line  = line_of(src, marker) + offset;
    a->count = expect_count;
    a->matched = 0;
    size_t slen = (size_t)(end - cur);
    if (slen >= sizeof(a->substr)) slen = sizeof(a->substr) - 1;
    memcpy(a->substr, cur, slen);
    a->substr[slen] = '\0';

    p = end + 2;
  }
  return count;
}

/* ── File walk ────────────────────────────────────────────────────────── */

static char *slurp(const char *path, size_t *out_len) {
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

static int strptr_cmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void walk(const char *dir, char ***out, size_t *cnt, size_t *cap) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      walk(path, out, cnt, cap);
    } else if (S_ISREG(st.st_mode)) {
      const char *dot = strrchr(e->d_name, '.');
      if (!dot || strcmp(dot, ".swift") != 0) continue;
      if (*cnt == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        char **nb = realloc(*out, nc * sizeof(*nb));
        if (!nb) continue;
        *out = nb;
        *cap = nc;
      }
      (*out)[(*cnt)++] = strdup(path);
    }
  }
  closedir(d);
}

/* ── JSON output ──────────────────────────────────────────────────────── */

/* Writes a JSON-escaped string (best effort — handles \, ", \n, control). */
static void json_str(FILE *f, const char *s) {
  fputc('"', f);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
    else if (*p == '\n') fputs("\\n", f);
    else if (*p == '\r') fputs("\\r", f);
    else if (*p == '\t') fputs("\\t", f);
    else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
    else fputc(*p, f);
  }
  fputc('"', f);
}

/* ── Result accumulator ───────────────────────────────────────────────── */

typedef struct {
  char *path;
  int   anns_error;       /* count of error-kind annotations */
  int   anns_other;       /* warning + note (informational only) */
  int   diags;            /* total msf diagnostics */
  int   unmatched_anns;   /* error annotations with no matching diag */
  int   extra_diags;      /* msf diagnostics no annotation accounted for */
  int   ok;               /* 1 = perfect parity; 0 = any mismatch */
  int   timed_out;        /* killed by --timeout */
  int   crashed;          /* child died from a signal (SIGSEGV/SIGBUS/...) */
} FixtureResult;

static int annotation_satisfied(const Annotation *a) {
  return a->kind != KIND_ERROR || a->matched >= a->count;
}

/* ── Per-fixture run ──────────────────────────────────────────────────── */

static void run_one_inline(const char *path, FixtureResult *out) {
  out->path = strdup(path);
  out->anns_error = out->anns_other = 0;
  out->diags = out->unmatched_anns = out->extra_diags = 0;
  out->ok = 0;

  char *src = slurp(path, NULL);
  if (!src) { out->extra_diags = 1; return; }

  Annotation anns[MAX_ANNOTATIONS];
  size_t n_anns = parse_annotations(src, anns, MAX_ANNOTATIONS);
  for (size_t i = 0; i < n_anns; i++) {
    if (anns[i].kind == KIND_ERROR) out->anns_error++;
    else out->anns_other++;
  }

  MSFResult *r = msf_analyze(src, path);
  if (!r) { free(src); return; }
  uint32_t total = msf_error_count(r);
  out->diags = (int)total;

  /* Greedy match: each diag matches the first error annotation on the same
   * line whose substring appears in the message and hasn't reached its
   * expected count yet.  Diagnostics that don't match anything count as
   * extras (msf saying something apple didn't expect). */
  int *diag_matched = calloc(total ? total : 1, sizeof(int));
  for (uint32_t i = 0; i < total; i++) {
    uint32_t eline = msf_error_line(r, i);
    const char *msg = msf_error_message(r, i);
    for (size_t j = 0; j < n_anns; j++) {
      if (anns[j].kind != KIND_ERROR) continue;
      if (anns[j].matched >= anns[j].count) continue;
      if ((int)eline != anns[j].line) continue;
      if (!strstr(msg, anns[j].substr)) continue;
      anns[j].matched++;
      diag_matched[i] = 1;
      break;
    }
  }
  for (size_t j = 0; j < n_anns; j++)
    if (!annotation_satisfied(&anns[j])) out->unmatched_anns++;
  for (uint32_t i = 0; i < total; i++)
    if (!diag_matched[i]) out->extra_diags++;

  out->ok = (out->unmatched_anns == 0 && out->extra_diags == 0);

  free(diag_matched);
  msf_result_free(r);
  free(src);
}

/* ── Fork + timeout wrapper ───────────────────────────────────────────── */

/* The corpus contains real apple/swift test files; some hit msf bugs
 * (infinite loops, deep recursion, OOM crashes) that would otherwise kill
 * the whole run.  We fork a child per fixture, cap its wall time, kill it
 * on overrun, and report timed_out/crashed in the result struct. */
static void run_one_forked(const char *path, int timeout_sec,
                           FixtureResult *out) {
  out->path = strdup(path);
  out->ok = 0;
  out->timed_out = out->crashed = 0;

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    /* Pipe creation failed — fall back to inline (lose isolation but
     * still produce a result). */
    free(out->path);
    run_one_inline(path, out);
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]); close(pipefd[1]);
    free(out->path);
    run_one_inline(path, out);
    return;
  }
  if (pid == 0) {
    /* Child: run inline, serialise the 5 counters to the pipe, exit. */
    close(pipefd[0]);
    FixtureResult local;
    run_one_inline(path, &local);
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%d %d %d %d %d %d\n",
                     local.anns_error, local.anns_other, local.diags,
                     local.unmatched_anns, local.extra_diags, local.ok);
    if (n > 0) {
      ssize_t w = write(pipefd[1], buf, (size_t)n);
      (void)w;  /* best effort; if the parent closed early we just exit */
    }
    close(pipefd[1]);
    _exit(0);
  }

  /* Parent: wait with timeout via polling waitpid(WNOHANG). */
  close(pipefd[1]);
  time_t start = time(NULL);
  int status = 0;
  while (1) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    if (r < 0) break;
    if (timeout_sec > 0 && time(NULL) - start >= timeout_sec) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      out->timed_out = 1;
      close(pipefd[0]);
      return;
    }
    struct timespec ts = { 0, 10 * 1000 * 1000 };  /* 10ms */
    nanosleep(&ts, NULL);
  }

  if (WIFSIGNALED(status)) {
    out->crashed = 1;
    close(pipefd[0]);
    return;
  }

  /* Read the counters from the pipe.  If parsing fails, treat as crash —
   * the child probably died mid-analysis without ever writing. */
  char buf[256] = {0};
  ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
  close(pipefd[0]);
  if (n <= 0) { out->crashed = 1; return; }
  int ok = 0;
  int got = sscanf(buf, "%d %d %d %d %d %d",
                   &out->anns_error, &out->anns_other, &out->diags,
                   &out->unmatched_anns, &out->extra_diags, &ok);
  if (got != 6) { out->crashed = 1; return; }
  out->ok = ok;
}

static void run_one(const char *path, int timeout_sec, FixtureResult *out) {
  if (timeout_sec > 0) run_one_forked(path, timeout_sec, out);
  else run_one_inline(path, out);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
  Options opt;
  int rc = parse_options(argc, argv, &opt);
  if (rc <= 0) return rc == 0 ? 0 : 2;

  char **paths = NULL;
  size_t n = 0, cap = 0;
  walk(opt.corpus, &paths, &n, &cap);
  if (n == 0) {
    fprintf(stderr, "no .swift files under '%s'\n", opt.corpus);
    return 2;
  }
  qsort(paths, n, sizeof(*paths), strptr_cmp);

  if (opt.filter) {
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
      if (strstr(paths[i], opt.filter)) paths[kept++] = paths[i];
      else free(paths[i]);
    }
    n = kept;
  }
  if (opt.limit > 0 && (size_t)opt.limit < n) {
    for (size_t i = (size_t)opt.limit; i < n; i++) free(paths[i]);
    n = (size_t)opt.limit;
  }

  FixtureResult *results = calloc(n, sizeof(*results));
  int pass = 0, fail = 0, timed_out = 0, crashed = 0;
  int printed_failures = 0;

  printf("Running %zu fixture(s) under %s "
         "(timeout=%ds per fixture)\n", n, opt.corpus, opt.timeout_sec);
  for (size_t i = 0; i < n; i++) {
    run_one(paths[i], opt.timeout_sec, &results[i]);
    if (results[i].timed_out) timed_out++;
    if (results[i].crashed)   crashed++;
    if (results[i].ok) {
      pass++;
      if (!opt.quiet)
        printf("  PASS  %s\n", results[i].path);
    } else {
      fail++;
      if (!opt.quiet) {
        if (opt.max_failures == 0 || printed_failures < opt.max_failures) {
          const char *tag = results[i].timed_out ? "TIME"
                          : results[i].crashed   ? "CRASH"
                          : "FAIL";
          printf("  %-5s %s  (anns=%d diags=%d unmatched=%d extras=%d)\n",
                 tag, results[i].path, results[i].anns_error,
                 results[i].diags, results[i].unmatched_anns,
                 results[i].extra_diags);
          printed_failures++;
        }
      }
    }
  }

  printf("\nSummary: %d passed, %d failed (of which %d timed out, %d crashed),"
         " %zu total  (%.1f%% pass)\n",
         pass, fail, timed_out, crashed, n,
         n ? (100.0 * pass / (double)n) : 0.0);

  if (opt.report_path) {
    FILE *f = fopen(opt.report_path, "w");
    if (f) {
      fprintf(f, "{\n  \"corpus\": ");
      json_str(f, opt.corpus);
      fprintf(f, ",\n  \"total\": %zu,\n  \"pass\": %d,\n  \"fail\": %d,\n"
                 "  \"timed_out\": %d,\n  \"crashed\": %d,\n",
              n, pass, fail, timed_out, crashed);
      fprintf(f, "  \"fixtures\": [\n");
      for (size_t i = 0; i < n; i++) {
        fprintf(f, "    { \"path\": ");
        json_str(f, results[i].path);
        fprintf(f, ", \"ok\": %s, \"annotations\": %d, \"diagnostics\": %d,"
                   " \"unmatched\": %d, \"extras\": %d,"
                   " \"timed_out\": %s, \"crashed\": %s }%s\n",
                results[i].ok ? "true" : "false",
                results[i].anns_error, results[i].diags,
                results[i].unmatched_anns, results[i].extra_diags,
                results[i].timed_out ? "true" : "false",
                results[i].crashed   ? "true" : "false",
                i + 1 < n ? "," : "");
      }
      fprintf(f, "  ]\n}\n");
      fclose(f);
      printf("Wrote report: %s\n", opt.report_path);
    } else {
      fprintf(stderr, "warning: cannot write report at %s\n", opt.report_path);
    }
  }

  /* Cleanup. */
  for (size_t i = 0; i < n; i++) {
    free(results[i].path);
    free(paths[i]);
  }
  free(results);
  free(paths);

  if (opt.strict && fail > 0) return 1;
  return 0;
}
