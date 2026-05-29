/**
 * @file msf_project.c
 * @brief CLI over the MSFProject library API: discover the module graph of any
 *        Xcode/SwiftPM project under a directory, then analyze each module
 *        whole-module — first independently, then in dependency order with a
 *        shared vocabulary so cross-module references resolve.  Logic lives in
 *        libmsf (src/project.c); this is a thin driver.
 *
 *   msf-project <project-dir>
 */
#include <msf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *p) {
  FILE *f = fopen(p, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
  size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f); return b;
}

static uint32_t undeclared_of(MSFModule *m) {
  uint32_t u = 0;
  for (uint32_t k = 0; k < msf_module_error_count(m); k++)
    if (strstr(msf_module_error_message(m, k), "undeclared type")) u++;
  return u;
}

/* Post-order DFS so a module is emitted after its dependencies (cycles are
 * tolerated: a node already on the stack is simply not re-entered). */
static void topo_visit(MSFProject *p, size_t i, char *state, size_t *order, size_t *n) {
  if (state[i]) return;            /* 0 = unvisited, 1 = done */
  state[i] = 2;                    /* on stack (cycle guard)  */
  for (size_t k = 0; k < msf_project_module_dep_count(p, i); k++) {
    size_t d = msf_project_module_dep(p, i, k);
    if (d != (size_t)-1 && state[d] != 2) topo_visit(p, d, state, order, n);
  }
  state[i] = 1;
  order[(*n)++] = i;
}

static int g_trace = 0;  /* MSF_PROJECT_TRACE: print each module to stderr so a
                          * crash/hang in a corpus sweep names the culprit. */

/* Write `s` as a JSON string literal (quotes + escaping) to `f`. */
static void json_str(FILE *f, const char *s) {
  fputc('"', f);
  for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
    switch (*p) {
      case '"':  fputs("\\\"", f); break;
      case '\\': fputs("\\\\", f); break;
      case '\n': fputs("\\n", f);  break;
      case '\r': fputs("\\r", f);  break;
      case '\t': fputs("\\t", f);  break;
      default:   if (*p < 0x20) fprintf(f, "\\u%04x", *p); else fputc(*p, f);
    }
  }
  fputc('"', f);
}

/* Build the shared vocabulary: a loaded --vocab artifact, else the compiled-in
 * SDK vocabulary (so SwiftUI/Foundation/UIKit names resolve with no SDK). */
static MSFVocab *build_shared_vocab(const char *vocab_path) {
  MSFVocab *shared = NULL;
  if (vocab_path) {
    char *vtext = slurp(vocab_path);
    if (vtext) { shared = msf_vocab_parse(vtext); free(vtext); }
    if (shared) fprintf(stderr, "loaded vocabulary: %s (%zu modules)\n",
                        vocab_path, msf_vocab_module_count(shared));
    else fprintf(stderr, "warning: could not load --vocab %s\n", vocab_path);
  }
  if (!shared) {
    shared = msf_vocab_builtin();
    fprintf(stderr, "built-in SDK vocabulary: %zu modules\n", msf_vocab_module_count(shared));
  }
  return shared;
}

/* `--json`: emit every diagnostic from the dependency-ordered resolved
 * whole-module analysis as a JSON array, for an editor/LSP to turn into
 * squiggles.  Each entry: {file,line,col,length,severity,module,message}. */
static int emit_json_diagnostics(MSFProject *proj, size_t n, const char *vocab_path) {
  char   *state = calloc(n, 1);
  size_t *order = calloc(n, sizeof *order), no = 0;
  if (!state || !order) { free(state); free(order); return 1; }
  for (size_t i = 0; i < n; i++) topo_visit(proj, i, state, order, &no);
  MSFVocab *shared = build_shared_vocab(vocab_path);
  /* A SEPARATE SDK vocabulary as the global fallback: SDK type names resolve
   * regardless of which framework re-exports them, while `shared` keeps the
   * per-import (cross-module) view as project types are published into it. */
  MSFVocab *sdk = build_shared_vocab(vocab_path);
  msf_project_set_sdk_vocabulary(proj, sdk);

  /* Harvest public type names from C/ObjC headers in bundled frameworks (.xcframework) */
  size_t nfw = msf_project_harvest_frameworks(proj, sdk);
  msf_project_harvest_packages(proj, sdk);  /* CocoaPods / Carthage type surface */
  msf_project_harvest_own_headers(proj, sdk); /* project's own ObjC headers */
  if (nfw) {
    fprintf(stderr, "Harvested C/ObjC types from %zu bundled framework(s) into SDK vocabulary\n", nfw);
  }

  printf("[");
  int first = 1;
  for (size_t t = 0; t < no; t++) {
    size_t i = order[t];
    if (g_trace) fprintf(stderr, "ANALYZE resolved %s\n", msf_project_module_name(proj, i));
    MSFModule *m = msf_project_analyze_module_resolved(proj, i, shared);
    if (!m) continue;
    const char *mod = msf_project_module_name(proj, i);
    for (uint32_t k = 0; k < msf_module_error_count(m); k++) {
      const char *file = msf_module_error_file(m, k);
      if (!file) continue;  /* locationless (e.g. the overflow summary) */
      const char *msg = msf_module_error_message(m, k);
      const char *sev = strstr(msg, "undeclared type") ? "warning" : "error";
      if (!first) printf(",");
      first = 0;
      printf("\n  {\"file\":");   json_str(stdout, file);
      printf(",\"line\":%u,\"col\":%u,\"length\":%u",
             msf_module_error_line(m, k), msf_module_error_col(m, k),
             msf_module_error_length(m, k));
      printf(",\"severity\":");   json_str(stdout, sev);
      printf(",\"module\":");     json_str(stdout, mod);
      printf(",\"message\":");    json_str(stdout, msg);
      printf("}");
    }
    msf_module_free(m);
  }
  printf("\n]\n");

  msf_vocab_free(shared);
  msf_vocab_free(sdk);
  free(state); free(order);
  return 0;
}

int main(int argc, char **argv) {
  const char *dir = NULL, *vocab_path = NULL;
  int g_json = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--vocab") && i + 1 < argc) vocab_path = argv[++i];
    else if (!strcmp(argv[i], "--json")) g_json = 1;
    else if (!dir) dir = argv[i];
  }
  g_trace = getenv("MSF_PROJECT_TRACE") != NULL;
  if (!dir) {
    fprintf(stderr, "usage: %s <project-dir> [--json] [--vocab sdk.msfvocab]\n", argv[0]);
    return 2;
  }

  MSFProject *proj = msf_project_open(dir);
  if (!proj) { fprintf(stderr, "could not open project (or unsupported build)\n"); return 1; }

  size_t n = msf_project_module_count(proj);
  if (n == 0) {
    if (g_json) printf("[]\n");
    else printf("No Xcode/SwiftPM modules found under %s\n", dir);
    msf_project_free(proj);
    return 0;
  }

  if (g_json) {
    int rc = emit_json_diagnostics(proj, n, vocab_path);
    msf_project_free(proj);
    return rc;
  }

  printf("=== %zu module(s) discovered under %s ===\n", n, dir);
  for (size_t i = 0; i < n; i++) {
    printf("  - %-24s [%-12s] %3zu files", msf_project_module_name(proj, i),
           msf_project_module_kind(proj, i), msf_project_module_file_count(proj, i));
    size_t nd = msf_project_module_dep_count(proj, i);
    if (nd) {
      printf("  deps:");
      for (size_t k = 0; k < nd; k++) printf(" %s", msf_project_module_name(proj, msf_project_module_dep(proj, i, k)));
    }
    printf("\n");
  }

  /* External (SDK/framework) imports — supplied by a loaded .msfvocab (--vocab). */
  size_t ne = msf_project_external_import_count(proj);
  printf("\nexternal imports (%zu): ", ne);
  for (size_t k = 0; k < ne; k++) printf("%s ", msf_project_external_import(proj, k));
  printf("\n");

  /* Independent baseline: each module alone, no cross-module knowledge. */
  MSFVocab *sdk = build_shared_vocab(vocab_path);
  msf_project_set_sdk_vocabulary(proj, sdk);
  size_t nfw = msf_project_harvest_frameworks(proj, sdk);
  msf_project_harvest_packages(proj, sdk);  /* CocoaPods / Carthage type surface */
  msf_project_harvest_own_headers(proj, sdk); /* project's own ObjC headers */
  if (nfw) {
    fprintf(stderr, "Harvested C/ObjC types from %zu bundled framework(s) into SDK vocabulary\n", nfw);
  }

  uint32_t *indep = calloc(n, sizeof *indep);
  for (size_t i = 0; i < n; i++) {
    if (g_trace) fprintf(stderr, "ANALYZE indep %s\n", msf_project_module_name(proj, i));
    MSFModule *m = msf_project_analyze_module(proj, i);
    if (m) { indep[i] = undeclared_of(m); msf_module_free(m); }
  }

  /* Resolved: analyze in dependency order, chaining a shared vocabulary so a
   * module's `import <sibling>` resolves the sibling's published types. */
  char  *state = calloc(n, 1);
  size_t *order = calloc(n, sizeof *order), no = 0;
  for (size_t i = 0; i < n; i++) topo_visit(proj, i, state, order, &no);

  MSFVocab *shared = build_shared_vocab(vocab_path);
  uint32_t *res = calloc(n, sizeof *res);
  for (size_t t = 0; t < no; t++) {
    size_t i = order[t];
    if (g_trace) fprintf(stderr, "ANALYZE resolved %s\n", msf_project_module_name(proj, i));
    MSFModule *m = msf_project_analyze_module_resolved(proj, i, shared);
    if (m) { res[i] = undeclared_of(m); msf_module_free(m); }
  }
  msf_vocab_free(shared);

  printf("\n=== whole-module analysis: undeclared types (independent -> resolved) ===\n");
  uint32_t ti = 0, tr = 0;
  for (size_t i = 0; i < n; i++) {
    printf("  %-24s %3zu files | undeclared %4u -> %4u  (%+d)\n",
           msf_project_module_name(proj, i), msf_project_module_file_count(proj, i),
           indep[i], res[i], (int)res[i] - (int)indep[i]);
    ti += indep[i]; tr += res[i];
  }
  printf("  %-24s            | undeclared %4u -> %4u  (resolved: %u)\n",
         "TOTAL", ti, tr, ti - tr);

  free(indep); free(res); free(state); free(order);
  msf_vocab_free(sdk);
  msf_project_free(proj);
  return 0;
}
