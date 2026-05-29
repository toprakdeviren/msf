/**
 * @file swift_scan_main.c
 * @brief Bulk scanner that runs msf_analyze() over a tree of real-world
 *        .swift files and emits per-file metrics for performance and
 *        error reporting.
 *
 * Unlike swift_corpus_main.c (which checks lit-style `// expected-error`
 * annotations against an apple/swift test tree), this runner targets
 * *arbitrary* Swift source — e.g. a directory of cloned GitHub repos.
 * There are no expectations to match; we just measure what msf does:
 *
 *   - wall time of msf_analyze() per file        (performance)
 *   - bytes / lines / tokens / AST nodes         (throughput denominators)
 *   - diagnostic count + first message           (error reporting)
 *   - crashes (SIGSEGV/SIGBUS/...) and hangs      (robustness)
 *
 * Robustness model: each file is analyzed in a forked child with a
 * per-file wall-clock alarm and an address-space rlimit, so a crash,
 * infinite loop, or runaway allocation on one file cannot take down the
 * whole scan.  Up to --jobs children run concurrently.
 *
 * Output: a TSV (one row per file) that downstream tooling aggregates.
 *   outcome  bytes  lines  tokens  ast_nodes  elapsed_ns  errors  sig  path  first_error
 *
 *   outcome ∈ { ok, errors, crash, timeout, ioerr }
 *
 * Usage:
 *   swift_scan_runner --root DIR [--report TSV] [--jobs N]
 *                     [--timeout SEC] [--limit N] [--progress N]
 */
#include <msf.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── Options ──────────────────────────────────────────────────────────── */

typedef struct {
  const char *root;          /* required: directory to walk */
  const char *report_path;   /* optional TSV output */
  int  jobs;                 /* concurrent children (default = ncpu) */
  int  timeout_sec;          /* per-file wall-time cap (default 10) */
  long limit;                /* 0 = unlimited files */
  long progress_every;       /* stderr progress cadence (files) */
  long mem_limit_mb;         /* per-child RLIMIT_AS, 0 = none */
} Options;

static void usage(void) {
  fprintf(stderr,
    "usage: swift_scan_runner --root DIR [options]\n"
    "  --report PATH    write per-file TSV (default: stdout)\n"
    "  --jobs N         concurrent analyzer children (default: ncpu)\n"
    "  --timeout SEC    per-file wall-time cap (default 10)\n"
    "  --limit N        cap number of files scanned (default: all)\n"
    "  --progress N     emit a progress line every N files (default 2000)\n"
    "  --mem-mb N       per-child address-space cap in MB (default 6144)\n");
}

/* ── Shared per-file record (lives in an mmap'd array) ────────────────── */

enum {
  OUT_UNKNOWN = 0, OUT_OK, OUT_ERRORS, OUT_CRASH, OUT_TIMEOUT, OUT_IOERR
};

typedef struct {
  volatile uint8_t done;     /* child finished and wrote its data        */
  uint8_t  read_fail;        /* child could not read the file            */
  uint8_t  outcome;          /* parent-assigned final classification     */
  int      term_sig;         /* signal number if the child died          */
  uint32_t err_count;
  uint32_t bytes;
  uint32_t lines;
  uint32_t tokens;
  uint32_t ast_nodes;
  uint64_t elapsed_ns;
  char     first_err[224];
} Rec;

/* ── File walk ────────────────────────────────────────────────────────── */

static int strptr_cmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void walk(const char *dir, char ***out, size_t *cnt, size_t *cap) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;            /* skip dotfiles/.git */
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    struct stat st;
    if (lstat(path, &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) continue;            /* don't follow symlinks */
    if (S_ISDIR(st.st_mode)) {
      walk(path, out, cnt, cap);
    } else if (S_ISREG(st.st_mode)) {
      const char *dot = strrchr(e->d_name, '.');
      if (!dot || strcmp(dot, ".swift") != 0) continue;
      if (*cnt == *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
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

/* ── AST metrics (iterative — no recursion, no heap) ──────────────────── */

/* Corpus-wide AST production stats, shared across worker children and merged
 * atomically.  Lets the run report not just "how fast" but "what msf
 * extracted": the node-kind histogram and how many nodes carry a resolved
 * TypeInfo (msf's whole point — a *typed* AST). */
typedef struct {
  uint64_t total_nodes;
  uint64_t typed_nodes;          /* nodes with a non-NULL ->type after sema */
  uint64_t kind_hist[AST__COUNT];
} GlobalStats;

static GlobalStats *g_stats;     /* mmap'd MAP_SHARED before forking */

/* Pre-order traversal (first_child / next_sibling / parent — no recursion,
 * safe on pathologically deep trees).  Tallies a local per-kind histogram and
 * the typed-node count; returns the node total. */
static uint32_t walk_stats(const ASTNode *root, uint64_t *hist,
                           uint64_t *typed) {
  if (!root) return 0;
  uint32_t n = 0;
  const ASTNode *node = root;
  while (node) {
    n++;
    if ((unsigned)node->kind < (unsigned)AST__COUNT) hist[node->kind]++;
    if (node->type) (*typed)++;
    if (node->first_child) { node = node->first_child; continue; }
    while (node && node != root && !node->next_sibling) node = node->parent;
    if (!node || node == root) break;
    node = node->next_sibling;
  }
  return n;
}

static uint32_t count_lines(const char *src, size_t len) {
  uint32_t lines = len ? 1 : 0;
  for (size_t i = 0; i < len; i++) if (src[i] == '\n') lines++;
  return lines;
}

/* ── Child: analyze one file, fill its record, exit ───────────────────── */

static void child_run(const char *path, Rec *rec, const Options *opt) {
  /* Guard rails so one bad file can't wedge the machine. */
  if (opt->mem_limit_mb > 0) {
    struct rlimit rl = { (rlim_t)opt->mem_limit_mb * 1024 * 1024,
                         (rlim_t)opt->mem_limit_mb * 1024 * 1024 };
    setrlimit(RLIMIT_AS, &rl);
  }
  alarm((unsigned)opt->timeout_sec);  /* default SIGALRM disposition kills us */

  size_t len = 0;
  char *src = slurp(path, &len);
  if (!src) { rec->read_fail = 1; rec->done = 1; _exit(0); }

  rec->bytes = (uint32_t)len;
  rec->lines = count_lines(src, len);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  MSFResult *r = msf_analyze(src, path);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  rec->elapsed_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull
                  + (uint64_t)(t1.tv_nsec - t0.tv_nsec);

  if (r) {
    rec->err_count  = msf_error_count(r);
    rec->tokens     = (uint32_t)msf_token_count(r);
    uint64_t hist[AST__COUNT] = {0}, typed = 0;
    rec->ast_nodes  = walk_stats(msf_root(r), hist, &typed);
    if (g_stats) {  /* merge into the shared corpus histogram */
      __atomic_fetch_add(&g_stats->total_nodes, rec->ast_nodes, __ATOMIC_RELAXED);
      __atomic_fetch_add(&g_stats->typed_nodes, typed, __ATOMIC_RELAXED);
      for (int k = 0; k < AST__COUNT; k++)
        if (hist[k])
          __atomic_fetch_add(&g_stats->kind_hist[k], hist[k], __ATOMIC_RELAXED);
    }
    if (rec->err_count > 0) {
      const char *m = msf_error_message(r, 0);
      if (m) {
        /* Some msf diagnostics prefix the message with "<path>:line:col: "
         * (lexer/parser), others don't (sema).  Strip the prefix — which is
         * exactly the filename we passed — so messages group cleanly and the
         * real text isn't pushed past the buffer by a long path. */
        size_t plen = strlen(path);
        if (strncmp(m, path, plen) == 0 && m[plen] == ':') {
          const char *q = m + plen + 1;
          while (*q >= '0' && *q <= '9') q++;            /* line  */
          if (*q == ':') { q++; while (*q >= '0' && *q <= '9') q++;
                           if (*q == ':') q++; }          /* col   */
          while (*q == ' ') q++;
          if (*q) m = q;
        }
        strncpy(rec->first_err, m, sizeof(rec->first_err) - 1);
        rec->first_err[sizeof(rec->first_err) - 1] = '\0';
      }
    }
  }
  /* Skip msf_result_free: process is about to die; freeing only adds risk. */
  rec->done = 1;
  _exit(0);
}

/* ── Parent: bounded fork pool ────────────────────────────────────────── */

typedef struct { pid_t pid; size_t idx; time_t start; } Slot;

static uint64_t now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void classify(Rec *rec, int status) {
  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    rec->term_sig = sig;
    if (sig == SIGALRM || sig == SIGXCPU || sig == SIGKILL)
      rec->outcome = OUT_TIMEOUT;
    else
      rec->outcome = OUT_CRASH;
    return;
  }
  if (WIFEXITED(status)) {
    if (!rec->done) { rec->outcome = OUT_CRASH; return; }  /* exited mid-write */
    if (rec->read_fail)       rec->outcome = OUT_IOERR;
    else if (rec->err_count)  rec->outcome = OUT_ERRORS;
    else                      rec->outcome = OUT_OK;
    return;
  }
  rec->outcome = OUT_CRASH;
}

static const char *outcome_name(uint8_t o) {
  switch (o) {
    case OUT_OK:      return "ok";
    case OUT_ERRORS:  return "errors";
    case OUT_CRASH:   return "crash";
    case OUT_TIMEOUT: return "timeout";
    case OUT_IOERR:   return "ioerr";
    default:          return "unknown";
  }
}

int main(int argc, char **argv) {
  Options opt;
  memset(&opt, 0, sizeof(opt));
  opt.jobs           = (int)sysconf(_SC_NPROCESSORS_ONLN);
  opt.timeout_sec    = 10;
  opt.progress_every = 2000;
  opt.mem_limit_mb   = 6144;
  if (opt.jobs <= 0) opt.jobs = 4;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--root") && i + 1 < argc)            opt.root = argv[++i];
    else if (!strcmp(argv[i], "--report") && i + 1 < argc)     opt.report_path = argv[++i];
    else if (!strcmp(argv[i], "--jobs") && i + 1 < argc)       opt.jobs = (int)strtol(argv[++i], NULL, 10);
    else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)    opt.timeout_sec = (int)strtol(argv[++i], NULL, 10);
    else if (!strcmp(argv[i], "--limit") && i + 1 < argc)      opt.limit = strtol(argv[++i], NULL, 10);
    else if (!strcmp(argv[i], "--progress") && i + 1 < argc)   opt.progress_every = strtol(argv[++i], NULL, 10);
    else if (!strcmp(argv[i], "--mem-mb") && i + 1 < argc)     opt.mem_limit_mb = strtol(argv[++i], NULL, 10);
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
    else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(); return 2; }
  }
  if (!opt.root) { fprintf(stderr, "--root DIR is required\n"); usage(); return 2; }
  if (opt.jobs < 1) opt.jobs = 1;

  /* Collect files. */
  char **paths = NULL; size_t n = 0, cap = 0;
  fprintf(stderr, "walking %s ...\n", opt.root);
  walk(opt.root, &paths, &n, &cap);
  if (n == 0) { fprintf(stderr, "no .swift files under '%s'\n", opt.root); return 2; }
  qsort(paths, n, sizeof(*paths), strptr_cmp);
  if (opt.limit > 0 && (size_t)opt.limit < n) n = (size_t)opt.limit;
  fprintf(stderr, "scanning %zu files with %d jobs (timeout=%ds)\n",
          n, opt.jobs, opt.timeout_sec);

  /* Shared result array. */
  Rec *recs = mmap(NULL, n * sizeof(Rec), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANON, -1, 0);
  if (recs == MAP_FAILED) { perror("mmap"); return 2; }
  memset(recs, 0, n * sizeof(Rec));

  /* Shared corpus-wide AST stats (children merge atomically). */
  g_stats = mmap(NULL, sizeof(GlobalStats), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANON, -1, 0);
  if (g_stats == MAP_FAILED) { g_stats = NULL; }

  Slot *slots = calloc((size_t)opt.jobs, sizeof(Slot));
  for (int i = 0; i < opt.jobs; i++) slots[i].pid = -1;

  size_t next = 0, finished = 0;
  int running = 0;
  uint64_t t_start = now_ms();
  int hard_grace = opt.timeout_sec + 5;  /* parent watchdog margin */

  while (finished < n) {
    /* Launch up to the job cap. */
    while (running < opt.jobs && next < n) {
      int s = -1;
      for (int i = 0; i < opt.jobs; i++) if (slots[i].pid == -1) { s = i; break; }
      if (s < 0) break;
      size_t idx = next++;
      pid_t pid = fork();
      if (pid == 0) { child_run(paths[idx], &recs[idx], &opt); _exit(0); }
      if (pid < 0) {
        /* Fork failed (transient) — back off and retry this index. */
        next--;
        break;
      }
      slots[s].pid = pid; slots[s].idx = idx; slots[s].start = time(NULL);
      running++;
    }

    /* Reap any finished child. */
    int status = 0;
    pid_t r = waitpid(-1, &status, WNOHANG);
    if (r > 0) {
      for (int i = 0; i < opt.jobs; i++) {
        if (slots[i].pid == r) {
          classify(&recs[slots[i].idx], status);
          slots[i].pid = -1;
          running--; finished++;
          break;
        }
      }
      if (opt.progress_every > 0 && finished % opt.progress_every == 0) {
        uint64_t el = now_ms() - t_start;
        double rate = el ? (double)finished * 1000.0 / el : 0;
        fprintf(stderr, "  %zu/%zu  (%.0f files/s, %.1fs)\n",
                finished, n, rate, el / 1000.0);
      }
    } else {
      /* Nothing reaped: run watchdog, then nap briefly. */
      time_t now = time(NULL);
      for (int i = 0; i < opt.jobs; i++)
        if (slots[i].pid != -1 && now - slots[i].start > hard_grace)
          kill(slots[i].pid, SIGKILL);
      struct timespec ts = { 0, 5 * 1000 * 1000 };  /* 5ms */
      nanosleep(&ts, NULL);
    }
  }

  uint64_t total_ms = now_ms() - t_start;
  fprintf(stderr, "done: %zu files in %.1fs\n", n, total_ms / 1000.0);

  /* Emit per-file TSV. */
  FILE *out = stdout;
  if (opt.report_path) {
    out = fopen(opt.report_path, "w");
    if (!out) { perror("report"); out = stdout; }
  }
  fprintf(out, "outcome\tbytes\tlines\ttokens\tast_nodes\telapsed_ns\terrors\tsig\tpath\tfirst_error\n");
  for (size_t i = 0; i < n; i++) {
    Rec *rc = &recs[i];
    /* sanitize first_err: strip tabs/newlines so the TSV stays one row */
    char msg[224]; size_t k = 0;
    for (const char *p = rc->first_err; *p && k < sizeof(msg) - 1; p++)
      msg[k++] = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
    msg[k] = '\0';
    fprintf(out, "%s\t%u\t%u\t%u\t%u\t%llu\t%u\t%d\t%s\t%s\n",
            outcome_name(rc->outcome), rc->bytes, rc->lines, rc->tokens,
            rc->ast_nodes, (unsigned long long)rc->elapsed_ns,
            rc->err_count, rc->term_sig, paths[i], msg);
  }
  if (out != stdout) { fclose(out); fprintf(stderr, "wrote %s\n", opt.report_path); }

  /* Emit the corpus-wide AST node-kind histogram + typed-node coverage to a
   * sidecar TSV next to the per-file report. */
  if (g_stats && opt.report_path) {
    char apath[4096];
    snprintf(apath, sizeof(apath), "%s.aststats.tsv", opt.report_path);
    FILE *af = fopen(apath, "w");
    if (af) {
      fprintf(af, "kind\tcount\n");
      fprintf(af, "_TOTAL_NODES_\t%llu\n",
              (unsigned long long)g_stats->total_nodes);
      fprintf(af, "_TYPED_NODES_\t%llu\n",
              (unsigned long long)g_stats->typed_nodes);
      for (int k = 0; k < AST__COUNT; k++)
        if (g_stats->kind_hist[k])
          fprintf(af, "%s\t%llu\n", ast_kind_name((ASTNodeKind)k),
                  (unsigned long long)g_stats->kind_hist[k]);
      fclose(af);
      fprintf(stderr, "wrote %s\n", apath);
    }
  }

  /* Terminal summary. */
  size_t c[6] = {0};
  uint64_t sum_ns = 0, sum_bytes = 0, sum_lines = 0;
  for (size_t i = 0; i < n; i++) {
    c[recs[i].outcome]++;
    sum_ns += recs[i].elapsed_ns;
    sum_bytes += recs[i].bytes;
    sum_lines += recs[i].lines;
  }
  fprintf(stderr,
    "\nsummary: %zu files | ok %zu  errors %zu  crash %zu  timeout %zu  ioerr %zu\n"
    "  analyze cpu-time %.1fs | %llu bytes | %llu lines | wall %.1fs\n",
    n, c[OUT_OK], c[OUT_ERRORS], c[OUT_CRASH], c[OUT_TIMEOUT], c[OUT_IOERR],
    sum_ns / 1e9, (unsigned long long)sum_bytes, (unsigned long long)sum_lines,
    total_ms / 1000.0);

  munmap(recs, n * sizeof(Rec));
  free(slots);
  for (size_t i = 0; i < n; i++) free(paths[i]);
  free(paths);
  return 0;
}
