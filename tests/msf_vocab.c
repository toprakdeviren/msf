/**
 * @file msf_vocab.c
 * @brief CLI: extract module vocabularies from `.swiftinterface` files into one
 *        portable `.msfvocab` artifact, using msf's own parser.
 *
 *   msf-vocab Module=a.swiftinterface [Module=b ...] > out.msfvocab
 *   msf-vocab --scan <sdk-dir>                       > sdk.msfvocab
 *
 * The `--scan` form walks a directory for every `*.swiftinterface` and derives
 * each module name from its `*.swiftmodule` folder (one interface per module —
 * architectures are equivalent).  Run once on a machine with the SDK; commit /
 * publish the resulting `.msfvocab` and ship it to environments that have no SDK
 * (browser, Windows) — the SDK changes rarely, so the artifact is regenerated
 * only when the SDK is updated.
 */
#include <msf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads an entire file into a heap buffer (NUL-terminated).  NULL on error. */
static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return NULL; }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long n = ftell(f);
  if (n < 0) { fclose(f); return NULL; }
  rewind(f);
  char *buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t rd = fread(buf, 1, (size_t)n, f);
  buf[rd] = '\0';
  fclose(f);
  return buf;
}

/* Derive the module name from an interface path: the basename of the enclosing
 * `*.swiftmodule` directory.  e.g. .../SwiftUI.swiftmodule/arm64.swiftinterface
 * -> "SwiftUI".  Returns 0 if the pattern isn't present. */
static int module_from_path(const char *path, char *out, size_t cap) {
  const char *sm = strstr(path, ".swiftmodule/");
  if (!sm) return 0;
  const char *end = sm;            /* points at ".swiftmodule/" */
  const char *start = end;
  while (start > path && start[-1] != '/') start--;
  size_t n = (size_t)(end - start);
  if (n == 0 || n >= cap) return 0;
  memcpy(out, start, n); out[n] = 0;
  return 1;
}

/* The `*.swiftmodule` directory of an interface path (dedup key — one arch per
 * dir, but the SAME module from different platform SDKs stays distinct so its
 * types get unioned).  e.g. .../SwiftUI.swiftmodule/arm64.swiftinterface ->
 * .../SwiftUI.swiftmodule */
static int swiftmodule_dir(const char *path, char *out, size_t cap) {
  const char *sm = strstr(path, ".swiftmodule/");
  if (!sm) return 0;
  size_t len = (size_t)(sm - path) + strlen(".swiftmodule");
  if (len >= cap) return 0;
  memcpy(out, path, len); out[len] = 0;
  return 1;
}

typedef struct { char name[256]; char path[4096]; } IfaceEntry;

#define MAX_IFACE 16384

/* Add entry `idx`'s vocabulary under `key`, expanding `@_exported import X` so
 * that `import key` resolves the umbrella's re-exported types too (SwiftUI
 * re-exports SwiftUICore -> View/Text).  DFS with a per-seed visited set. */
static size_t add_with_reexports(MSFVocab *v, const char *key, IfaceEntry *e, size_t n,
                                 size_t idx, char *visited, int depth) {
  if (idx >= n || visited[idx] || depth > 8) return 0;
  visited[idx] = 1;
  char *src = read_file(e[idx].path);
  if (!src) return 0;
  size_t added = msf_vocab_add_interface(v, key, src);
  char *save;
  for (char *l = strtok_r(src, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
    const char *x = strstr(l, "@_exported import ");
    if (!x) continue;
    const char *s = x + strlen("@_exported import ");
    while (*s == ' ') s++;
    char xm[256]; int k = 0;
    while (k < 255 && ((s[k]>='A'&&s[k]<='Z')||(s[k]>='a'&&s[k]<='z')||(s[k]>='0'&&s[k]<='9')||s[k]=='_')) { xm[k]=s[k]; k++; }
    xm[k] = 0;
    if (k) for (size_t m = 0; m < n; m++)
      if (!strcmp(e[m].name, xm)) added += add_with_reexports(v, key, e, n, m, visited, depth + 1);
  }
  free(src);
  return added;
}

/* ── Objective-C framework headers ───────────────────────────────────────── */
/* Lightweight name harvest (NOT a parser): pull the TYPE NAMES an ObjC framework
 * exports so `import UIKit` resolves UIImage/NSString/… as predeclared types. */

static int ident_at(const char *s, char *out, size_t cap) {
  int n = 0;
  while (n + 1 < (int)cap && ((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z') ||
                              (s[n] >= '0' && s[n] <= '9') || s[n] == '_'))
    { out[n] = s[n]; n++; }
  out[n] = 0;
  return n;
}

/* Add an ObjC type; Foundation/CoreData/… drop the leading "NS" in Swift
 * (NSURLSession→URLSession), so also add the stripped form. */
static void objc_add(MSFVocab *v, const char *module, const char *name, MSFVocabKind kind) {
  if (!name[0] || (name[0] >= '0' && name[0] <= '9')) return;
  msf_vocab_add_type(v, module, name, kind);
  size_t L = strlen(name);
  if (L > 3 && name[0] == 'N' && name[1] == 'S' && name[2] >= 'A' && name[2] <= 'Z')
    msf_vocab_add_type(v, module, name + 2, kind);
}

static void objc_extract(MSFVocab *v, const char *module, char *src) {
  char *save;
  for (char *l = strtok_r(src, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
    const char *s = l; while (*s == ' ' || *s == '\t') s++;
    char nm[160];
    if (!strncmp(s, "@interface ", 11)) {
      if (ident_at(s + 11, nm, sizeof nm)) objc_add(v, module, nm, MSF_VOCAB_CLASS);
    } else if (!strncmp(s, "@protocol ", 10)) {
      if (ident_at(s + 10, nm, sizeof nm)) objc_add(v, module, nm, MSF_VOCAB_PROTOCOL);
    } else if (!strncmp(s, "@class ", 7)) {
      const char *p = s + 7;                       /* comma list, until ';' */
      while (*p && *p != ';') {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        int n = ident_at(p, nm, sizeof nm);
        if (n) { objc_add(v, module, nm, MSF_VOCAB_CLASS); p += n; } else if (*p) p++;
      }
    } else if (s[0] == '}' ) {                      /* `} CGRect;` struct typedef */
      const char *p = s + 1; while (*p == ' ' || *p == '\t') p++;
      if (ident_at(p, nm, sizeof nm)) objc_add(v, module, nm, MSF_VOCAB_STRUCT);
    } else if (!strncmp(s, "typedef ", 8) && !strchr(s, '(')) {
      /* one-line C typedef: `typedef double CFTimeInterval;`,
       * `typedef struct CGRect CGRect;` → the last identifier before ';'.
       * Skip when it's an all-caps trailing macro (NS_TYPED_ENUM, …). */
      const char *semi = strchr(s, ';');
      if (semi) {
        const char *e = semi; while (e > s && (e[-1]==' '||e[-1]=='\t')) e--;
        const char *b = e; while (b > s && ((b[-1]>='A'&&b[-1]<='Z')||(b[-1]>='a'&&b[-1]<='z')||
                                            (b[-1]>='0'&&b[-1]<='9')||b[-1]=='_')) b--;
        size_t L = (size_t)(e - b);
        if (L && L < sizeof nm) {
          memcpy(nm, b, L); nm[L] = 0;
          int lower = 0; for (size_t i = 0; i < L; i++) if (nm[i]>='a'&&nm[i]<='z') { lower = 1; break; }
          if (lower) objc_add(v, module, nm, MSF_VOCAB_TYPEALIAS);  /* skip ALL-CAPS macros */
        }
      }
    }
    /* NS_ENUM(type, Name) / NS_OPTIONS / NS_CLOSED_ENUM / NS_ERROR_ENUM */
    static const char *EK[] = { "NS_ENUM(", "NS_OPTIONS(", "NS_CLOSED_ENUM(", "NS_ERROR_ENUM(", NULL };
    for (const char **k = EK; *k; k++) {
      const char *p = strstr(s, *k);
      if (!p) continue;
      const char *c = strchr(p, ','); if (!c) continue;
      c++; while (*c == ' ' || *c == '\t') c++;
      if (ident_at(c, nm, sizeof nm)) objc_add(v, module, nm, MSF_VOCAB_ENUM);
    }
  }
}

/* framework name from `.../X.framework/Headers/...` → "X" */
static int framework_name(const char *path, char *out, size_t cap) {
  const char *fw = strstr(path, ".framework/");
  if (!fw) return 0;
  const char *start = fw; while (start > path && start[-1] != '/') start--;
  size_t n = (size_t)(fw - start);
  if (n == 0 || n >= cap) return 0;
  memcpy(out, start, n); out[n] = 0;
  return 1;
}

static void scan_objc(MSFVocab *v, char **roots, int nroots) {
  size_t nheaders = 0;
  for (int r = 0; r < nroots; r++) {
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "find -L '%s' -path '*.framework/Headers/*.h' 2>/dev/null", roots[r]);
    FILE *pp = popen(cmd, "r"); if (!pp) continue;
    char path[4096];
    while (fgets(path, sizeof path, pp)) {
      size_t L = strlen(path); while (L && (path[L-1]=='\n'||path[L-1]=='\r')) path[--L]=0;
      if (!L) continue;
      char fw[160];
      if (!framework_name(path, fw, sizeof fw)) continue;
      char *src = read_file(path);
      if (!src) continue;
      objc_extract(v, fw, src);
      free(src);
      nheaders++;
    }
    pclose(pp);
  }
  fprintf(stderr, "scanned %zu Objective-C framework headers\n", nheaders);
}

/* --scan: walk every root for *.swiftinterface, dedup by *.swiftmodule dir
 * (drops equivalent architectures), and union each module's types — across
 * platforms (macOS + iOS + …) and across @_exported umbrellas. */
static int scan_sdk(MSFVocab *v, char **roots, int nroots) {
  IfaceEntry *e = NULL; size_t n = 0, cap = 0;
  char **seen = NULL; size_t nseen = 0, scap = 0;       /* dedup by smdir */

  for (int r = 0; r < nroots && n < MAX_IFACE; r++) {
    char cmd[2048];
    /* -L: SDK roots are usually symlinks (MacOSX.sdk -> MacOSX<ver>.sdk). */
    snprintf(cmd, sizeof cmd, "find -L '%s' -name '*.swiftinterface' 2>/dev/null", roots[r]);
    FILE *pp = popen(cmd, "r");
    if (!pp) { fprintf(stderr, "scan: cannot list %s\n", roots[r]); continue; }
    char path[4096];
    while (fgets(path, sizeof path, pp) && n < MAX_IFACE) {
      size_t L = strlen(path); while (L && (path[L-1]=='\n'||path[L-1]=='\r')) path[--L]=0;
      if (!L) continue;
      char smdir[4096], mod[256];
      if (!swiftmodule_dir(path, smdir, sizeof smdir)) continue;
      if (!module_from_path(path, mod, sizeof mod)) continue;
      int dup = 0; for (size_t i = 0; i < nseen; i++) if (!strcmp(seen[i], smdir)) { dup = 1; break; }
      if (dup) continue;                                 /* another arch of a seen module */
      if (nseen == scap) { scap = scap ? scap * 2 : 256; seen = realloc(seen, scap * sizeof *seen); }
      seen[nseen] = malloc(strlen(smdir)+1); if (seen[nseen]) strcpy(seen[nseen++], smdir);
      if (n == cap) { cap = cap ? cap * 2 : 256; e = realloc(e, cap * sizeof *e); }
      snprintf(e[n].name, sizeof e[n].name, "%s", mod);
      snprintf(e[n].path, sizeof e[n].path, "%s", path);
      n++;
    }
    pclose(pp);
  }

  char *visited = malloc(n ? n : 1);
  for (size_t i = 0; i < n; i++) {
    memset(visited, 0, n);
    add_with_reexports(v, e[i].name, e, n, i, visited, 0);
  }
  free(visited);
  for (size_t i = 0; i < nseen; i++) free(seen[i]);
  free(seen); free(e);
  fprintf(stderr, "scanned %zu module interfaces from %d root(s)\n", n, nroots);

  /* Also harvest type names from Objective-C framework headers (UIKit,
   * Foundation, …) so their classes resolve as predeclared types. */
  scan_objc(v, roots, nroots);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s Module=interface.swiftinterface [Module=path ...] > out.msfvocab\n"
            "       %s --scan <sdk-dir> [sdk-dir ...]                     > sdk.msfvocab\n",
            argv[0], argv[0]);
    return 2;
  }

  MSFVocab *v = msf_vocab_create();
  if (!v) { fprintf(stderr, "out of memory\n"); return 1; }

  int failures = 0;
  if (!strcmp(argv[1], "--scan")) {
    if (argc < 3) { fprintf(stderr, "usage: %s --scan <sdk-dir> [sdk-dir ...]\n", argv[0]); msf_vocab_free(v); return 2; }
    failures = scan_sdk(v, &argv[2], argc - 2);
  } else {
    for (int i = 1; i < argc; i++) {
      char *eq = strchr(argv[i], '=');
      if (!eq || eq == argv[i]) {
        fprintf(stderr, "skip: '%s' is not MODULE=path\n", argv[i]);
        failures++;
        continue;
      }
      *eq = '\0';
      const char *module = argv[i];
      const char *path = eq + 1;
      char *src = read_file(path);
      if (!src) { failures++; continue; }
      size_t added = msf_vocab_add_interface(v, module, src);
      free(src);
      fprintf(stderr, "  %-24s %zu types\n", module, added);
    }
  }

  char *text = msf_vocab_serialize(v);
  if (!text) { fprintf(stderr, "serialize failed\n"); msf_vocab_free(v); return 1; }
  fputs(text, stdout);
  free(text);
  msf_vocab_free(v);

  return failures ? 1 : 0;
}
