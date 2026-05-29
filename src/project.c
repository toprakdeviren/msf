/**
 * @file project.c
 * @brief Project discovery — find the module graph of an Xcode or SwiftPM
 *        project on disk and (optionally) run whole-module analysis per module.
 *
 * Generic: nothing about a specific project is assumed.  Under a root directory
 * it finds every `*.xcodeproj/project.pbxproj` and `Package.swift` and derives
 * modules from the project's OWN metadata:
 *
 *   - Xcode (modern "synchronized folder" model): each PBXNativeTarget ->
 *     its fileSystemSynchronizedGroups -> folder path -> the .swift under it.
 *   - SwiftPM: each `.target/.executableTarget/.testTarget(name: "X"[,path:..])`
 *     -> sources under `path` (default Sources/X, Tests/X for a test target).
 *
 * A module is analyzed independently (the correct Swift model): references to
 * OTHER modules' types surface as undeclared — that is the cross-module surface
 * a vocabulary (.msfvocab) would supply, not a resolution bug.
 *
 * Filesystem access (POSIX dirent) is native-only; the WebAssembly build, which
 * has no host filesystem, gets stubs that report an empty project.
 */
#include "msf.h"
#include "internal/msf.h"

#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
  char   name[128];
  char   dir[1024];   /* absolute source root for this module          */
  char   kind[32];    /* "xcode-target" | "spm-target" | "spm-test"    */
  char **files;       /* owned .swift paths under dir                  */
  size_t nfiles, cap;
  int    deps[128];   /* indices of OTHER discovered modules this one   */
  int    ndeps;       /* imports (the dependency edges)                */
} ProjModule;

struct MSFProject {
  ProjModule *modules;
  size_t      count, cap;
  char      **ext;        /* deduped names of imported modules NOT in the   */
  size_t      next, ecap; /* project (SDK / external framework imports)     */
  char      **pkgseen;    /* canonical dirs of SwiftPM packages already      */
  size_t      npkg, pcap; /* discovered (cycle / duplicate guard)            */
  const MSFVocab *sdk_vocab; /* optional global-fallback SDK vocab (borrowed) */
  /* Binary frameworks bundled with the project (e.g. WebRTC.xcframework):
   * their ObjC headers travel with the repo, so their public type names can be
   * harvested at analysis time (msf_project_harvest_frameworks) — no toolchain,
   * any OS.  fw_mod[i] is the module name (the .framework basename), fw_hdrs[i]
   * its Headers directory. */
  char      **fw_mod;
  char      **fw_hdrs;
  size_t      nfw, fwcap;
  char        root[1024]; /* the project root passed to msf_project_open, so   */
                          /* vendored deps (Pods/Carthage) can be harvested.    */
};

void msf_project_set_sdk_vocabulary(MSFProject *p, const MSFVocab *v) {
  if (p) p->sdk_vocab = v;
}

static char *dup_cstr(const char *s) {
  size_t n = strlen(s); char *p = malloc(n + 1); if (p) memcpy(p, s, n + 1); return p;
}

static ProjModule *project_add_module(MSFProject *p, const char *name,
                                      const char *dir, const char *kind) {
  /* de-dup by (name, dir): the same target can be reached via several scans */
  for (size_t i = 0; i < p->count; i++)
    if (!strcmp(p->modules[i].name, name) && !strcmp(p->modules[i].dir, dir))
      return NULL;
  if (p->count == p->cap) {
    size_t nc = p->cap ? p->cap * 2 : 8;
    ProjModule *nm = realloc(p->modules, nc * sizeof *nm);
    if (!nm) return NULL;
    p->modules = nm; p->cap = nc;
  }
  ProjModule *m = &p->modules[p->count++];
  memset(m, 0, sizeof *m);
  snprintf(m->name, sizeof m->name, "%s", name);
  snprintf(m->dir,  sizeof m->dir,  "%s", dir);
  snprintf(m->kind, sizeof m->kind, "%s", kind);
  return m;
}

static void module_add_file(ProjModule *m, const char *path) {
  /* Dedup: a file listed twice in a target (e.g. a duplicated pbxproj sources
   * build-ref, or reachable via two groups) must be analysed once — otherwise
   * every top-level decl in it is declared twice → false "Redefinition of X". */
  for (size_t i = 0; i < m->nfiles; i++)
    if (strcmp(m->files[i], path) == 0) return;

  if (m->nfiles == m->cap) {
    size_t nc = m->cap ? m->cap * 2 : 16;
    char **nf = realloc(m->files, nc * sizeof *nf);
    if (!nf) return;
    m->files = nf; m->cap = nc;
  }
  char *c = dup_cstr(path);
  if (c) m->files[m->nfiles++] = c;
}

#ifndef WASM_BUILD
/* ═══════════════════════════════════════════════════════════════════════════
 * Native implementation (POSIX dirent)
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>

static char *slurp(const char *p) {
  FILE *f = fopen(p, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
  size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f); return b;
}

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

/* A symlink (lstat does not follow).  Recursive walks must NOT descend into
 * symlinked directories: a repo that symlinks a subdir back to an ancestor
 * (e.g. a vendored submodule pointing at the repo root) is a cycle, and
 * following it re-discovers the same projects until the depth cap — turning
 * one library into hundreds of duplicate modules. */
static int is_symlink(const char *p) { struct stat st; return lstat(p, &st) == 0 && S_ISLNK(st.st_mode); }

/* Skip directories that never hold first-party sources we want to walk. */
static int skip_dir(const char *name) {
  return !strcmp(name, ".") || !strcmp(name, "..") ||
         !strcmp(name, ".build") || !strcmp(name, "build") ||
         !strcmp(name, ".git") || !strcmp(name, "DerivedData") ||
         /* Vendored dependency-manager trees: their source is NOT analyzed as
          * project modules (that counts the deps' own errors against the repo);
          * instead msf_project_harvest_packages publishes their public type
          * surface into the vocab so the app's `import <Dep>` types resolve. */
         !strcmp(name, "Pods") || !strcmp(name, "Carthage");
}

/* Recursively collect every *.swift under `dir` into the module. */
static void collect_swift(ProjModule *m, const char *dir, int depth) {
  if (depth > 64) return;
  DIR *d = opendir(dir); if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (skip_dir(e->d_name)) continue;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (is_symlink(path)) continue;            /* don't follow symlink cycles */
    if (is_dir(path)) { collect_swift(m, path, depth + 1); continue; }
    size_t L = strlen(e->d_name);
    if (L > 6 && !strcmp(e->d_name + L - 6, ".swift"))
      module_add_file(m, path);
  }
  closedir(d);
}

/* Recursively find files named `leaf`, invoking cb(path) for each. */
typedef void (*found_cb)(MSFProject *, const char *path);
static void find_manifests(MSFProject *p, const char *dir, const char *leaf,
                           found_cb cb, int depth) {
  if (depth > 64) return;
  DIR *d = opendir(dir); if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (skip_dir(e->d_name)) continue;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (is_symlink(path)) continue;            /* don't follow symlink cycles */
    if (is_dir(path)) { find_manifests(p, path, leaf, cb, depth + 1); continue; }
    if (!strcmp(e->d_name, leaf)) cb(p, path);
  }
  closedir(d);
}

static void dir_of(const char *path, char *out, size_t cap) {
  snprintf(out, cap, "%s", path);
  char *s = strrchr(out, '/'); if (s) *s = 0; else snprintf(out, cap, ".");
}

/* ── pbxproj (synchronized-folder + legacy file-list models) ─────────────── */
typedef struct {
  char (*ids)[40];
  size_t count, cap;
} PbxIdList;

typedef struct { char id[40], path[256]; } PbxSyncGroup;
typedef struct { char id[40], path[256], name[256], source_tree[64]; } PbxFileRef;
typedef struct { char id[40], file_ref[40]; } PbxBuildFile;
typedef struct { char id[40]; PbxIdList files; } PbxSourcesPhase;
typedef struct { char id[40], path[256], source_tree[64]; PbxIdList children; } PbxGroup;
typedef struct { char id[40], name[128]; PbxIdList phases, sync_groups; } PbxTarget;

typedef struct {
  PbxSyncGroup    *sync_groups; size_t nsync, csync;
  PbxFileRef      *files;       size_t nfile, cfile;
  PbxBuildFile    *builds;      size_t nbuild, cbuild;
  PbxSourcesPhase *phases;      size_t nphase, cphase;
  PbxGroup        *groups;      size_t ngroup, cgroup;
  PbxTarget       *targets;     size_t ntarget, ctarget;
} PbxIndex;

static int lead_hex(const char *line, char *out) {
  const char *s = line; while (*s == '\t' || *s == ' ') s++;
  int n = 0; while (n < 39 && ((s[n]>='0'&&s[n]<='9')||(s[n]>='A'&&s[n]<='F')||(s[n]>='a'&&s[n]<='f'))) n++;
  if (n < 8) return 0; memcpy(out, s, n); out[n] = 0; return n;
}

static int is_hex_char(char c) {
  return (c>='0'&&c<='9') || (c>='A'&&c<='F') || (c>='a'&&c<='f');
}

static int scan_hex_id(const char *s, const char *end, char out[40]) {
  int n = 0;
  while (s + n < end && n < 39 && is_hex_char(s[n])) n++;
  if (n < 8) return 0;
  memcpy(out, s, (size_t)n); out[n] = 0; return 1;
}

static void pbx_id_list_add(PbxIdList *l, const char *id) {
  if (!l || !id || !id[0]) return;
  if (l->count == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 8;
    char (*ni)[40] = realloc(l->ids, nc * sizeof *ni);
    if (!ni) return;
    l->ids = ni; l->cap = nc;
  }
  snprintf(l->ids[l->count++], 40, "%s", id);
}

static int pbx_id_list_contains(const PbxIdList *l, const char *id) {
  if (!l || !id) return 0;
  for (size_t i = 0; i < l->count; i++) if (!strcmp(l->ids[i], id)) return 1;
  return 0;
}

static int pbx_key_char(char c) {
  return (c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9') || c=='_';
}

static const char *pbx_find_key(const char *b, const char *end, const char *key) {
  size_t klen = strlen(key);
  for (const char *p = b; p + klen < end; p++) {
    if (memcmp(p, key, klen)) continue;
    if (p > b && pbx_key_char(p[-1])) continue;
    if (pbx_key_char(p[klen])) continue;
    const char *q = p + klen;
    while (q < end && (*q == ' ' || *q == '\t')) q++;
    if (q < end && *q == '=') return p;
  }
  return NULL;
}

static int pbx_block_has_isa(const char *b, const char *end, const char *isa) {
  const char *p = b;
  size_t ilen = strlen(isa);
  while ((p = strstr(p, isa)) && p + ilen <= end) return 1;
  return 0;
}

static int pbx_block_kv(const char *b, const char *end, const char *key,
                        char *out, size_t cap) {
  const char *k = pbx_find_key(b, end, key); if (!k) return 0;
  const char *eq = memchr(k, '=', (size_t)(end - k)); if (!eq) return 0;
  const char *v = eq + 1; while (v < end && (*v == ' ' || *v == '\t')) v++;
  const char *semi = memchr(v, ';', (size_t)(end - v)); if (!semi) return 0;
  const char *comment = NULL;
  for (const char *p = v; p + 1 < semi; p++)
    if (p[0] == '/' && p[1] == '*') { comment = p; break; }
  const char *e = comment ? comment : semi;
  while (e > v && (e[-1]==' ' || e[-1]=='\t' || e[-1]=='\r' || e[-1]=='\n')) e--;
  if (e - v >= 2 && v[0] == '"' && e[-1] == '"') { v++; e--; }
  size_t n = (size_t)(e - v);
  if (n >= cap) n = cap - 1;
  memcpy(out, v, n); out[n] = 0; return 1;
}

static int pbx_block_id_kv(const char *b, const char *end, const char *key,
                           char out[40]) {
  char tmp[160];
  if (!pbx_block_kv(b, end, key, tmp, sizeof tmp)) return 0;
  return scan_hex_id(tmp, tmp + strlen(tmp), out);
}

static void pbx_block_ids_for_key(const char *b, const char *end,
                                  const char *key, PbxIdList *ids) {
  const char *k = pbx_find_key(b, end, key); if (!k) return;
  const char *eq = memchr(k, '=', (size_t)(end - k)); if (!eq) return;
  const char *open = memchr(eq, '(', (size_t)(end - eq)); if (!open) return;
  const char *close = strstr(open, ");"); if (!close || close > end) return;
  for (const char *p = open + 1; p < close; p++) {
    while (p < close && (*p == ' ' || *p == '\t' || *p == '\n' ||
                         *p == '\r' || *p == ',')) p++;
    char id[40];
    if (scan_hex_id(p, close, id)) {
      pbx_id_list_add(ids, id);
      while (p < close && is_hex_char(*p)) p++;
    }
  }
}

static void *pbx_grow(void *items, size_t *cap, size_t count, size_t elem_size) {
  if (count < *cap) return items;
  size_t nc = *cap ? *cap * 2 : 16;
  void *n = realloc(items, nc * elem_size);
  if (n) *cap = nc;
  return n;
}

static void pbx_parse_object(PbxIndex *idx, const char *b, const char *end,
                             const char *id) {
  if (pbx_block_has_isa(b, end, "PBXFileSystemSynchronizedRootGroup")) {
    PbxSyncGroup *items = pbx_grow(idx->sync_groups, &idx->csync, idx->nsync, sizeof *items);
    if (!items) return;
    idx->sync_groups = items;
    PbxSyncGroup *g = &idx->sync_groups[idx->nsync++];
    memset(g, 0, sizeof *g); snprintf(g->id, sizeof g->id, "%s", id);
    pbx_block_kv(b, end, "path", g->path, sizeof g->path);
  } else if (pbx_block_has_isa(b, end, "PBXFileReference")) {
    PbxFileRef *items = pbx_grow(idx->files, &idx->cfile, idx->nfile, sizeof *items);
    if (!items) return;
    idx->files = items;
    PbxFileRef *f = &idx->files[idx->nfile++];
    memset(f, 0, sizeof *f); snprintf(f->id, sizeof f->id, "%s", id);
    pbx_block_kv(b, end, "path", f->path, sizeof f->path);
    pbx_block_kv(b, end, "name", f->name, sizeof f->name);
    pbx_block_kv(b, end, "sourceTree", f->source_tree, sizeof f->source_tree);
  } else if (pbx_block_has_isa(b, end, "PBXBuildFile")) {
    PbxBuildFile *items = pbx_grow(idx->builds, &idx->cbuild, idx->nbuild, sizeof *items);
    if (!items) return;
    idx->builds = items;
    PbxBuildFile *bf = &idx->builds[idx->nbuild++];
    memset(bf, 0, sizeof *bf); snprintf(bf->id, sizeof bf->id, "%s", id);
    pbx_block_id_kv(b, end, "fileRef", bf->file_ref);
  } else if (pbx_block_has_isa(b, end, "PBXSourcesBuildPhase")) {
    PbxSourcesPhase *items = pbx_grow(idx->phases, &idx->cphase, idx->nphase, sizeof *items);
    if (!items) return;
    idx->phases = items;
    PbxSourcesPhase *ph = &idx->phases[idx->nphase++];
    memset(ph, 0, sizeof *ph); snprintf(ph->id, sizeof ph->id, "%s", id);
    pbx_block_ids_for_key(b, end, "files", &ph->files);
  } else if (pbx_block_has_isa(b, end, "PBXGroup")) {
    PbxGroup *items = pbx_grow(idx->groups, &idx->cgroup, idx->ngroup, sizeof *items);
    if (!items) return;
    idx->groups = items;
    PbxGroup *g = &idx->groups[idx->ngroup++];
    memset(g, 0, sizeof *g); snprintf(g->id, sizeof g->id, "%s", id);
    pbx_block_kv(b, end, "path", g->path, sizeof g->path);
    pbx_block_kv(b, end, "sourceTree", g->source_tree, sizeof g->source_tree);
    pbx_block_ids_for_key(b, end, "children", &g->children);
  } else if (pbx_block_has_isa(b, end, "PBXNativeTarget")) {
    PbxTarget *items = pbx_grow(idx->targets, &idx->ctarget, idx->ntarget, sizeof *items);
    if (!items) return;
    idx->targets = items;
    PbxTarget *t = &idx->targets[idx->ntarget++];
    memset(t, 0, sizeof *t); snprintf(t->id, sizeof t->id, "%s", id);
    pbx_block_kv(b, end, "name", t->name, sizeof t->name);
    pbx_block_ids_for_key(b, end, "buildPhases", &t->phases);
    pbx_block_ids_for_key(b, end, "fileSystemSynchronizedGroups", &t->sync_groups);
  }
}

static void pbx_index_free(PbxIndex *idx) {
  for (size_t i = 0; i < idx->nphase; i++) free(idx->phases[i].files.ids);
  for (size_t i = 0; i < idx->ngroup; i++) free(idx->groups[i].children.ids);
  for (size_t i = 0; i < idx->ntarget; i++) {
    free(idx->targets[i].phases.ids);
    free(idx->targets[i].sync_groups.ids);
  }
  free(idx->sync_groups);
  free(idx->files);
  free(idx->builds);
  free(idx->phases);
  free(idx->groups);
  free(idx->targets);
}

static const PbxSyncGroup *pbx_find_sync(const PbxIndex *idx, const char *id) {
  for (size_t i = 0; i < idx->nsync; i++) if (!strcmp(idx->sync_groups[i].id, id)) return &idx->sync_groups[i];
  return NULL;
}

static const PbxFileRef *pbx_find_file(const PbxIndex *idx, const char *id) {
  for (size_t i = 0; i < idx->nfile; i++) if (!strcmp(idx->files[i].id, id)) return &idx->files[i];
  return NULL;
}

static const PbxBuildFile *pbx_find_build(const PbxIndex *idx, const char *id) {
  for (size_t i = 0; i < idx->nbuild; i++) if (!strcmp(idx->builds[i].id, id)) return &idx->builds[i];
  return NULL;
}

static const PbxSourcesPhase *pbx_find_phase(const PbxIndex *idx, const char *id) {
  for (size_t i = 0; i < idx->nphase; i++) if (!strcmp(idx->phases[i].id, id)) return &idx->phases[i];
  return NULL;
}

static const PbxGroup *pbx_find_parent_group(const PbxIndex *idx, const char *child_id) {
  for (size_t i = 0; i < idx->ngroup; i++)
    if (pbx_id_list_contains(&idx->groups[i].children, child_id)) return &idx->groups[i];
  return NULL;
}

static void path_join(char *out, size_t cap, const char *base, const char *leaf) {
  if (!leaf || !leaf[0]) snprintf(out, cap, "%s", base ? base : "");
  else if (leaf[0] == '/') snprintf(out, cap, "%s", leaf);
  else if (!base || !base[0]) snprintf(out, cap, "%s", leaf);
  else snprintf(out, cap, "%s/%s", base, leaf);
}

static void pbx_group_abs_path(const PbxIndex *idx, const char *rootdir,
                               const PbxGroup *g, char *out, size_t cap,
                               int depth) {
  if (!g || depth > 64) { snprintf(out, cap, "%s", rootdir); return; }
  if (g->path[0] == '/') { snprintf(out, cap, "%s", g->path); return; }
  char base[1024];
  if (!strcmp(g->source_tree, "SOURCE_ROOT")) snprintf(base, sizeof base, "%s", rootdir);
  else {
    const PbxGroup *parent = pbx_find_parent_group(idx, g->id);
    if (parent) pbx_group_abs_path(idx, rootdir, parent, base, sizeof base, depth + 1);
    else snprintf(base, sizeof base, "%s", rootdir);
  }
  path_join(out, cap, base, g->path);
}

static void pbx_file_abs_path(const PbxIndex *idx, const char *rootdir,
                              const PbxFileRef *f, char *out, size_t cap) {
  const char *rel = f->path[0] ? f->path : f->name;
  if (!rel || !rel[0]) { out[0] = 0; return; }
  if (rel[0] == '/') { snprintf(out, cap, "%s", rel); return; }
  char base[1024];
  if (!strcmp(f->source_tree, "SOURCE_ROOT")) snprintf(base, sizeof base, "%s", rootdir);
  else {
    const PbxGroup *parent = pbx_find_parent_group(idx, f->id);
    if (parent) pbx_group_abs_path(idx, rootdir, parent, base, sizeof base, 0);
    else snprintf(base, sizeof base, "%s", rootdir);
  }
  path_join(out, cap, base, rel);
}

static int swift_path(const char *path) {
  size_t L = strlen(path);
  return L > 6 && !strcmp(path + L - 6, ".swift");
}

static void discover_xcode(MSFProject *p, const char *pbxproj_path) {
  char projdir[1024]; dir_of(pbxproj_path, projdir, sizeof projdir);  /* X.xcodeproj */
  char rootdir[1024]; dir_of(projdir, rootdir, sizeof rootdir);       /* its parent  */

  char *txt = slurp(pbxproj_path); if (!txt) return;

  PbxIndex idx; memset(&idx, 0, sizeof idx);
  const char *cursor = txt;
  while ((cursor = strstr(cursor, "= {"))) {
    const char *line = cursor;
    while (line > txt && line[-1] != '\n') line--;
    char id[40];
    if (!lead_hex(line, id)) { cursor += 3; continue; }
    const char *end = strstr(cursor, "};");
    if (!end) break;
    end += 2;
    pbx_parse_object(&idx, line, end, id);
    cursor = end;
  }

  for (size_t ti = 0; ti < idx.ntarget; ti++) {
    PbxTarget *t = &idx.targets[ti];
    if (!t->name[0]) continue;

    if (t->sync_groups.count) {
      for (size_t gi = 0; gi < t->sync_groups.count; gi++) {
        const PbxSyncGroup *sg = pbx_find_sync(&idx, t->sync_groups.ids[gi]);
        if (!sg || !sg->path[0]) continue;
        char full[1024]; path_join(full, sizeof full, rootdir, sg->path);
        ProjModule *m = project_add_module(p, t->name, full, "xcode-target");
        if (m) collect_swift(m, full, 0);
      }
      continue;
    }

    size_t before = p->count;
    ProjModule *m = project_add_module(p, t->name, rootdir, "xcode-target");
    if (!m) continue;
    for (size_t pi = 0; pi < t->phases.count; pi++) {
      const PbxSourcesPhase *ph = pbx_find_phase(&idx, t->phases.ids[pi]);
      if (!ph) continue;
      for (size_t fi = 0; fi < ph->files.count; fi++) {
        const PbxBuildFile *bf = pbx_find_build(&idx, ph->files.ids[fi]);
        const PbxFileRef *fr = bf ? pbx_find_file(&idx, bf->file_ref) : NULL;
        if (!fr) continue;
        char full[2048]; pbx_file_abs_path(&idx, rootdir, fr, full, sizeof full);
        if (full[0] && swift_path(full)) module_add_file(m, full);
      }
    }
    if (m->nfiles == 0 && p->count == before + 1) p->count--;
  }

  pbx_index_free(&idx);
  free(txt);
}

/* ── Package.swift (SwiftPM) ─────────────────────────────────────────────── */

/* Does `dir` contain a .swift file anywhere beneath it? */
static int dir_has_swift(const char *dir, int depth) {
  if (depth > 16) return 0;
  DIR *d = opendir(dir); if (!d) return 0;
  struct dirent *e; int found = 0;
  while (!found && (e = readdir(d))) {
    if (skip_dir(e->d_name)) continue;
    char path[2048]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (is_symlink(path)) continue;            /* don't follow symlink cycles */
    if (is_dir(path)) { if (dir_has_swift(path, depth + 1)) found = 1; }
    else { size_t L = strlen(e->d_name); if (L > 6 && !strcmp(e->d_name + L - 6, ".swift")) found = 1; }
  }
  closedir(d);
  return found;
}

/* Does `dir` contain a .swift file as a DIRECT child (flat layout)? */
static int dir_has_swift_direct(const char *dir) {
  DIR *d = opendir(dir); if (!d) return 0;
  struct dirent *e; int found = 0;
  while (!found && (e = readdir(d))) {
    size_t L = strlen(e->d_name);
    if (L > 6 && !strcmp(e->d_name + L - 6, ".swift")) found = 1;
  }
  closedir(d);
  return found;
}

static int spm_name_matches_dir(const char *name, const char *dir) {
  size_t i = 0;
  for (; name[i] && dir[i]; i++) {
    char a = name[i], b = dir[i];
    if ((a == '_' || a == '-') && (b == '_' || b == '-')) continue;
    if (a != b) return 0;
  }
  return name[i] == 0 && dir[i] == 0;
}

static int find_spm_target_dir(const char *pkgdir, const char *root,
                               const char *name, char *out, size_t cap) {
  char base[1280];
  if (root && root[0]) snprintf(base, sizeof base, "%s/%s", pkgdir, root);
  else snprintf(base, sizeof base, "%s", pkgdir);
  DIR *d = opendir(base); if (!d) return 0;
  struct dirent *e; int found = 0;
  while (!found && (e = readdir(d))) {
    if (skip_dir(e->d_name) || !spm_name_matches_dir(name, e->d_name)) continue;
    char sub[2048]; snprintf(sub, sizeof sub, "%s/%s", base, e->d_name);
    if (is_dir(sub) && dir_has_swift(sub, 0)) {
      snprintf(out, cap, "%s", sub);
      found = 1;
    }
  }
  closedir(d);
  return found;
}

/* The package's own name — the first `name:` literal after `Package(`. */
static void pkg_name_of(const char *txt, char *out, size_t cap) {
  out[0] = 0;
  const char *pk = strstr(txt, "Package(");
  const char *nk = strstr(pk ? pk : txt, "name:");
  if (!nk) return;
  const char *a = strchr(nk, '"'); if (!a) return; a++;
  const char *e = strchr(a, '"'); if (!e) return;
  size_t n = (size_t)(e - a); if (n >= cap) n = cap - 1;
  memcpy(out, a, n); out[n] = 0;
}

/* SwiftPM convention layout: a manifest that declares no explicit targets (or
 * uses a custom target DSL we can't read) derives its targets from the on-disk
 * structure.  In each source search path (Sources/Source/src/srcs; Tests for
 * tests) every subdirectory that holds .swift is a target named after the
 * directory; if the search path holds .swift directly it is a single target
 * named after the package.  This is the dominant form among older packages. */
static void discover_convention(MSFProject *p, const char *pkgdir, const char *txt) {
  char pname[128]; pkg_name_of(txt, pname, sizeof pname);
  size_t start = p->count;
  static const char *srcdirs[]  = { "Sources", "Source", "src", "srcs" };
  static const char *testdirs[] = { "Tests" };
  struct { const char **dirs; size_t n; const char *kind; int flat; } groups[2] = {
    { srcdirs, 4, "spm-target", 1 }, { testdirs, 1, "spm-test", 0 }
  };
  for (int g = 0; g < 2; g++) {
    for (size_t si = 0; si < groups[g].n; si++) {
      char base[1280]; snprintf(base, sizeof base, "%s/%s", pkgdir, groups[g].dirs[si]);
      if (!is_dir(base)) continue;
      /* Flat layout takes precedence: .swift directly under the search dir means
       * a single target named after the package; subdirs are then resources, not
       * targets (e.g. `Sources/Foo.playground` next to direct Swift files). */
      if (groups[g].flat && pname[0] && dir_has_swift_direct(base)) {
        ProjModule *m = project_add_module(p, pname, base, groups[g].kind);
        if (m) collect_swift(m, base, 0);
        continue;
      }
      DIR *d = opendir(base); if (!d) continue;
      struct dirent *e;
      while ((e = readdir(d))) {
        if (skip_dir(e->d_name)) continue;
        char sub[2048]; snprintf(sub, sizeof sub, "%s/%s", base, e->d_name);
        if (is_symlink(sub)) continue;         /* don't follow symlink cycles */
        if (is_dir(sub) && dir_has_swift(sub, 0)) {
          ProjModule *m = project_add_module(p, e->d_name, sub, groups[g].kind);
          if (m) collect_swift(m, sub, 0);
        }
      }
      closedir(d);
    }
  }
  /* Pre-`Sources/` SwiftPM (Swift 2/3): a single module lived in a top-level
   * directory named after the package, not under Sources/.  If nothing was
   * found above, adopt `<pkgdir>/<package>` when it holds .swift. */
  if (p->count == start && pname[0]) {
    char root[1408]; snprintf(root, sizeof root, "%s/%s", pkgdir, pname);
    if (is_dir(root) && dir_has_swift(root, 0)) {
      ProjModule *m = project_add_module(p, pname, root, "spm-target");
      if (m) collect_swift(m, root, 0);
    }
  }
}

static void scan_targets(MSFProject *p, const char *txt, const char *pkgdir,
                         const char *call, const char *kind, const char *defroot) {
  const char *s = txt; size_t clen = strlen(call);
  while ((s = strstr(s, call))) {
    const char *q = s + clen;
    char nm[128] = "", path[256] = "";
    const char *namekey = strstr(q, "name:");
    if (namekey && namekey - q < 400) {
      const char *a = strchr(namekey, '"');
      if (a) { a++; const char *e = strchr(a, '"'); if (e && e - a < 127) { size_t n = (size_t)(e - a); memcpy(nm, a, n); nm[n] = 0; } }
    }
    const char *pathkey = strstr(q, "path:");
    if (pathkey && pathkey - q < 600) {
      const char *a = strchr(pathkey, '"');
      if (a) { a++; const char *e = strchr(a, '"'); if (e && e - a < 255) { size_t n = (size_t)(e - a); memcpy(path, a, n); path[n] = 0; } }
    }
    if (nm[0]) {
      char full[1024];
      if (path[0]) snprintf(full, sizeof full, "%s/%s", pkgdir, path);
      else if (defroot && defroot[0]) snprintf(full, sizeof full, "%s/%s/%s", pkgdir, defroot, nm);
      else snprintf(full, sizeof full, "%s/%s", pkgdir, nm);
      if (!path[0] && !dir_has_swift(full, 0))
        find_spm_target_dir(pkgdir, defroot, nm, full, sizeof full);
      ProjModule *m = project_add_module(p, nm, full, kind);
      if (m) {
        size_t before_files = m->nfiles;
        collect_swift(m, full, 0);
        if (m->nfiles == before_files && p->count > 0) p->count--;
      }
    }
    s = q;
  }
}
/* Follow `.package(path: "REL")` dependencies so packages OUTSIDE the scanned
 * root (e.g. a sibling/relative local package) are discovered too — their
 * targets become modules and their types resolve via vocab chaining. */
static void discover_spm(MSFProject *p, const char *pkg_path) {
  char pkgdir[1024]; dir_of(pkg_path, pkgdir, sizeof pkgdir);

  /* canonicalize and skip if this package was already processed */
  char canon[1024];
  if (!realpath(pkgdir, canon)) snprintf(canon, sizeof canon, "%s", pkgdir);
  for (size_t i = 0; i < p->npkg; i++) if (!strcmp(p->pkgseen[i], canon)) return;
  if (p->npkg == p->pcap) { p->pcap = p->pcap ? p->pcap * 2 : 8; p->pkgseen = realloc(p->pkgseen, p->pcap * sizeof *p->pkgseen); }
  if (p->pkgseen) { p->pkgseen[p->npkg] = malloc(strlen(canon)+1); if (p->pkgseen[p->npkg]) strcpy(p->pkgseen[p->npkg++], canon); }

  char *txt = slurp(pkg_path); if (!txt) return;
  size_t before = p->count;
  scan_targets(p, txt, pkgdir, ".executableTarget(", "spm-executable", "Sources");
  scan_targets(p, txt, pkgdir, ".testTarget(",       "spm-test",   "Tests");
  scan_targets(p, txt, pkgdir, ".target(",           "spm-target", "Sources");
  scan_targets(p, txt, pkgdir, "Target(",            "spm-target", "");
  /* No explicit targets parsed (old manifest, or a custom target DSL) — fall
   * back to SwiftPM's convention-based layout so the package still analyzes. */
  if (p->count == before) discover_convention(p, pkgdir, txt);

  /* recurse into local-path package dependencies */
  const char *s = txt; const char *key = ".package(";
  while ((s = strstr(s, key))) {
    const char *q = s + strlen(key);
    const char *pk = strstr(q, "path:");
    if (pk && pk - q < 200) {
      const char *a = strchr(pk, '"');
      if (a) {
        a++; const char *e = strchr(a, '"');
        if (e && e - a < 700) {
          char rel[768]; size_t n = (size_t)(e - a); memcpy(rel, a, n); rel[n] = 0;
          char dep[2048]; snprintf(dep, sizeof dep, "%s/%s/Package.swift", pkgdir, rel);
          discover_spm(p, dep);     /* dedup guard above prevents cycles */
        }
      }
    }
    s = q;
  }
  free(txt);
}

static int module_index(const MSFProject *p, const char *name, size_t self) {
  for (size_t i = 0; i < p->count; i++)
    if (i != self && !strcmp(p->modules[i].name, name)) return (int)i;
  return -1;
}

static int module_has_name(const MSFProject *p, const char *name) {
  return p && module_index(p, name, p->count) >= 0;
}

static void module_record_dep(ProjModule *m, int dep) {
  for (int k = 0; k < m->ndeps; k++)
    if (m->deps[k] == dep)
      return;
  if (m->ndeps < 128)
    m->deps[m->ndeps++] = dep;
}

static void project_record_external_import(MSFProject *p, const char *name) {
  if (!p || !name || !name[0])
    return;
  for (size_t e = 0; e < p->next; e++)
    if (!strcmp(p->ext[e], name))
      return;
  if (p->next == p->ecap) {
    size_t nc = p->ecap ? p->ecap * 2 : 16;
    char **ne = realloc(p->ext, nc * sizeof *ne);
    if (!ne)
      return;
    p->ext = ne;
    p->ecap = nc;
  }
  p->ext[p->next] = dup_cstr(name);
  if (p->ext[p->next])
    p->next++;
}

static void module_record_import(MSFProject *p, size_t mi, const char *name) {
  if (!p || mi >= p->count || !name || !name[0])
    return;
  ProjModule *m = &p->modules[mi];
  int dep = module_index(p, name, mi);
  if (dep >= 0) {
    module_record_dep(m, dep);
  } else if (!module_has_name(p, name)) {
    project_record_external_import(p, name);
  }
}

static int token_text_is(const Source *src, const Token *t, const char *lit) {
  size_t n = strlen(lit);
  return t && t->len == n && memcmp(src->data + t->pos, lit, n) == 0;
}

static int import_kind_token(const Source *src, const Token *t) {
  if (!t)
    return 0;
  if (t->type == TOK_KEYWORD) {
    switch (t->keyword) {
      case KW_TYPEALIAS:
      case KW_STRUCT:
      case KW_CLASS:
      case KW_ENUM:
      case KW_PROTOCOL:
      case KW_LET:
      case KW_VAR:
      case KW_FUNC:
      case KW_OPERATOR:
        return 1;
      default:
        return 0;
    }
  }
  return token_text_is(src, t, "operator");
}

static int import_module_name_from_tokens(const Source *src, const Token *tokens,
                                          size_t count, size_t import_idx,
                                          char *out, size_t cap) {
  out[0] = 0;
  size_t i = import_idx + 1;
  if (i >= count)
    return 0;
  if (import_kind_token(src, &tokens[i]))
    i++;
  if (i >= count ||
      (tokens[i].type != TOK_IDENTIFIER && tokens[i].type != TOK_KEYWORD))
    return 0;
  size_t n = tokens[i].len;
  if (n >= cap)
    n = cap - 1;
  memcpy(out, src->data + tokens[i].pos, n);
  out[n] = 0;
  return out[0] != 0;
}

/* Derive dependency edges from Swift import tokens: module M depends on module D
 * iff some source in M imports D and D is another discovered module. This keeps
 * project discovery independent from fragile manifest dependency parsing while
 * avoiding raw line scanning through comments, strings, or import-kind syntax. */
static void module_scan_imports(MSFProject *p, size_t mi) {
  ProjModule *m = &p->modules[mi];
  for (size_t j = 0; j < m->nfiles; j++) {
    char *text = slurp(m->files[j]);
    if (!text)
      continue;

    Source src = { .data = text, .len = strlen(text), .filename = m->files[j] };
    TokenStream ts;
    token_stream_init(&ts, src.len / 4 + 64);
    if (!ts.tokens) {
      free(text);
      continue;
    }
    if (lexer_tokenize(&src, &ts, 1, NULL) == 0) {
      for (size_t k = 0; k < ts.count; k++) {
        if (ts.tokens[k].type != TOK_KEYWORD ||
            ts.tokens[k].keyword != KW_IMPORT)
          continue;
        char name[128];
        if (import_module_name_from_tokens(&src, ts.tokens, ts.count, k,
                                           name, sizeof name))
          module_record_import(p, mi, name);
      }
    }
    token_stream_free(&ts);
    free(text);
  }
}

/* ── Binary-framework (.xcframework) type harvesting ──────────────────────── *
 *
 * A bundled `.xcframework` ships its public ObjC headers in the repo (they
 * travel with the project, every OS), so we read the type NAMES a Swift
 * `import <Framework>` would expose — enough to resolve references (names, not
 * signatures).  Tolerant by design: a harvested name resolves, a missed one
 * stays "undeclared" (positive-only), so over/under-extraction never breaks. */

static void project_add_framework(MSFProject *p, const char *mod,
                                   const char *hdrs) {
  for (size_t i = 0; i < p->nfw; i++)
    if (!strcmp(p->fw_hdrs[i], hdrs)) return;        /* de-dup by headers dir */
  if (p->nfw == p->fwcap) {
    size_t nc = p->fwcap ? p->fwcap * 2 : 8;
    char **nm = realloc(p->fw_mod, nc * sizeof *nm);
    if (!nm) return;
    p->fw_mod = nm;
    char **nh = realloc(p->fw_hdrs, nc * sizeof *nh);
    if (!nh) return;
    p->fw_hdrs = nh;
    p->fwcap = nc;
  }
  p->fw_mod[p->nfw]  = dup_cstr(mod);
  p->fw_hdrs[p->nfw] = dup_cstr(hdrs);
  if (p->fw_mod[p->nfw] && p->fw_hdrs[p->nfw]) p->nfw++;
}

static int str_ends_with(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && !strcmp(s + a - b, suf);
}

/* Inside X.xcframework, find a "<slice>/<Name>.framework/Headers" dir and
 * register it as framework module <Name>. */
static void register_xcframework(MSFProject *p, const char *xcf) {
  DIR *d = opendir(xcf); if (!d) return;
  struct dirent *e;
  char xcf_name[128];
  const char *base = strrchr(xcf, '/');
  base = base ? base + 1 : xcf;
  size_t bn = strlen(base);
  if (bn > 12 && !strcmp(base + bn - 12, ".xcframework")) bn -= 12;
  if (bn >= sizeof xcf_name) bn = sizeof xcf_name - 1;
  memcpy(xcf_name, base, bn); xcf_name[bn] = 0;

  while ((e = readdir(d))) {
    if (skip_dir(e->d_name)) continue;
    char slice[2048]; snprintf(slice, sizeof slice, "%s/%s", xcf, e->d_name);
    if (!is_dir(slice)) continue;

    /* Check if "Headers" exists directly under the slice (static C/Rust frameworks) */
    char direct_hdrs[2304];
    snprintf(direct_hdrs, sizeof direct_hdrs, "%s/Headers", slice);
    if (is_dir(direct_hdrs)) {
      project_add_framework(p, xcf_name, direct_hdrs);
      continue;
    }

    DIR *sd = opendir(slice); if (!sd) continue;
    struct dirent *se;
    while ((se = readdir(sd))) {
      if (!str_ends_with(se->d_name, ".framework")) continue;
      char hdrs[2304];
      snprintf(hdrs, sizeof hdrs, "%s/%s/Headers", slice, se->d_name);
      if (!is_dir(hdrs)) continue;
      char mod[128];
      size_t mn = strlen(se->d_name) - 10;            /* strip ".framework" */
      if (mn >= sizeof mod) mn = sizeof mod - 1;
      memcpy(mod, se->d_name, mn); mod[mn] = 0;
      project_add_framework(p, mod, hdrs);
    }
    closedir(sd);
  }
  closedir(d);
}

/* Recursively find every *.xcframework under `dir`. */
static void find_xcframeworks(MSFProject *p, const char *dir, int depth) {
  if (depth > 64) return;
  DIR *d = opendir(dir); if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (skip_dir(e->d_name)) continue;
    char path[2048]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (is_symlink(path) || !is_dir(path)) continue;
    if (str_ends_with(e->d_name, ".xcframework")) { register_xcframework(p, path); continue; }
    find_xcframeworks(p, path, depth + 1);
  }
  closedir(d);
}

/* ── ObjC/C header → type-name scanner ────────────────────────────────────── */

static int hv_id(char c) {
  return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';
}

/* Read the identifier starting at `s` (after skipping spaces/tabs) into out;
 * return the position just past it. */
static const char *hv_read_id(const char *s, const char *end, char *out, size_t cap) {
  while (s < end && (*s==' '||*s=='\t'||*s=='\n'||*s=='\r')) s++;
  size_t n = 0;
  while (s < end && hv_id(*s) && n + 1 < cap) out[n++] = *s++;
  out[n] = 0;
  return s;
}

/* `word` appears at s[i] on an identifier boundary. */
static int hv_word(const char *s, const char *end, const char *w) {
  size_t n = strlen(w);
  if (s + n > end || memcmp(s, w, n)) return 0;
  return s + n == end || !hv_id(s[n]);
}

static void hv_add(MSFVocab *v, const char *mod, const char *name) {
  if (name && name[0] && (name[0] < '0' || name[0] > '9'))
    msf_vocab_add_type(v, mod, name, MSF_VOCAB_CLASS);
}

/* After a declaration keyword, read comma-separated type names into vocab `mod`.
 * For each name `Ident`, also harvest a macro-wrapped inner name `Ident(Inner)`
 * — this is generic: it covers ANY framework's class-name macro (e.g. a
 * `FOO_OBJC_TYPE(Bar)` wrapper) AND Objective-C categories `Class (Cat)`,
 * without knowing any framework's macro by name.  Over-harvesting a stray
 * identifier is harmless because resolution is positive-only.  Returns the
 * position just past the names. */
static const char *hv_decl_names(MSFVocab *v, const char *mod,
                                 const char *q, const char *end, char *name,
                                 size_t cap, int allow_commas) {
  for (;;) {
    const char *r = hv_read_id(q, end, name, cap);
    if (!name[0]) break;
    hv_add(v, mod, name);                 /* the identifier itself */
    const char *t = r; while (t < end && (*t==' '||*t=='\t')) t++;
    if (t < end && *t == '(') {           /* MACRO(Inner) or Class (Category) */
      hv_read_id(t + 1, end, name, cap);
      hv_add(v, mod, name);               /* the inner identifier too */
      while (t < end && *t != ')' && *t != '\n') t++;
      r = (t < end) ? t + 1 : end;
    }
    if (!allow_commas) return r;
    while (r < end && *r==' ') r++;
    if (r < end && *r == ',') { q = r + 1; continue; }   /* @class A, B, C; */
    return r;
  }
  return q;
}

/* Trailing ObjC type-annotation macros that follow the type name in a typedef
 * (`typedef NSString *Name NS_STRING_ENUM;`) — they must NOT be mistaken for the
 * type name when scanning a typedef for its last identifier. */
static int hv_is_type_annotation(const char *id, size_t len) {
  static const char *const m[] = {
    "NS_ENUM", "NS_OPTIONS", "NS_CLOSED_ENUM", "NS_STRING_ENUM",
    "NS_EXTENSIBLE_STRING_ENUM", "NS_TYPED_ENUM", "NS_TYPED_EXTENSIBLE_ENUM",
    "NS_SWIFT_NAME", "NS_REFINED_FOR_SWIFT", "NS_SWIFT_UNAVAILABLE",
    "NS_UNAVAILABLE", "NS_SWIFT_SENDABLE", "NS_DESIGNATED_INITIALIZER",
    "API_AVAILABLE", "API_UNAVAILABLE", "API_DEPRECATED",
  };
  for (size_t i = 0; i < sizeof m / sizeof *m; i++)
    if (strlen(m[i]) == len && !memcmp(m[i], id, len)) return 1;
  return 0;
}

/* Extract public ObjC/C type names from one header's text into vocab `mod`.
 * Generic — no framework-specific knowledge:
 *   @interface / @protocol / @class Name   → Name (+ inner of any MACRO(Name))
 *   typedef NS_ENUM/NS_OPTIONS(_, Name)     → Name  (universal Apple macros)
 *   typedef … Name;                         → Name
 * Comments/strings are skipped; resolution is positive-only so an extra or
 * missed name never breaks anything. */
static void harvest_header_text(MSFVocab *v, const char *mod, const char *txt) {
  const char *s = txt, *end = txt + strlen(txt);
  int line_c = 0, block_c = 0, str_c = 0;
  char name[128];
  for (const char *p = s; p < end; p++) {
    char c = *p, n = p + 1 < end ? p[1] : 0;
    if (line_c)  { if (c=='\n') line_c = 0; continue; }
    if (block_c) { if (c=='*'&&n=='/') { block_c=0; p++; } continue; }
    if (str_c)   { if (c=='\\') p++; else if (c=='"') str_c=0; continue; }
    if (c=='/'&&n=='/') { line_c=1; p++; continue; }
    if (c=='/'&&n=='*') { block_c=1; p++; continue; }
    if (c=='"') { str_c=1; continue; }

    /* @interface / @protocol / @class */
    if (c=='@') {
      int kw = hv_word(p+1,end,"interface") ? 10
             : hv_word(p+1,end,"protocol")  ? 9
             : hv_word(p+1,end,"class")     ? 6 : 0;
      if (kw) {
        int multi = (kw == 6);            /* @class A, B, C; */
        p = hv_decl_names(v, mod, p + kw, end, name, sizeof name, multi);
        p--;                              /* loop's p++ resumes after names  */
      }
      continue;
    }

    if (!hv_id(c) || (p>s && hv_id(p[-1]))) continue;  /* word start only */

    /* typedef NS_ENUM(type, Name) / NS_OPTIONS / NS_CLOSED_ENUM → 2nd arg */
    if (hv_word(p,end,"NS_ENUM") || hv_word(p,end,"NS_OPTIONS") ||
        hv_word(p,end,"NS_CLOSED_ENUM")) {
      const char *q = p; while (q<end && *q!='(' && *q!='\n') q++;
      if (q<end && *q=='(') {
        const char *comma = q; while (comma<end && *comma!=',' && *comma!=')') comma++;
        if (comma<end && *comma==',') { hv_read_id(comma+1, end, name, sizeof name); hv_add(v, mod, name); }
      }
      continue;
    }
    /* typedef … Name;  (struct/enum body skipped; take the last identifier
     * before ';' that is not a trailing annotation macro).  This covers both
     * `typedef NS_CLOSED_ENUM(NSInteger, Name) {…};` (Name is the last id before
     * the `{` body) and `typedef NSString *Name NS_STRING_ENUM;` (the trailing
     * macro is skipped, so Name wins instead of the macro). */
    if (hv_word(p,end,"typedef")) {
      const char *q = p + 7; int depth = 0; const char *li = NULL, *le = NULL;
      size_t budget = 4096;
      while (q<end && budget--) {
        if (*q=='{') depth++;
        else if (*q=='}') { if (depth>0) depth--; }
        else if (*q==';' && depth==0) break;
        else if (depth==0 && hv_id(*q) && (q==txt || !hv_id(q[-1]))) {
          const char *r = q; while (r<end && hv_id(*r)) r++;
          if (!hv_is_type_annotation(q, (size_t)(r - q))) { li = q; le = r; }
          q = r; continue;
        }
        q++;
      }
      if (li && le) {
        size_t L = (size_t)(le - li);
        if (L < sizeof name) { memcpy(name, li, L); name[L]=0; hv_add(v, mod, name); }
      }
      p = (q<end)? q : end-1;
      continue;
    }
  }
}

size_t msf_project_harvest_frameworks(const MSFProject *p, MSFVocab *v) {
  if (!p || !v) return 0;
  for (size_t f = 0; f < p->nfw; f++) {
    DIR *d = opendir(p->fw_hdrs[f]); if (!d) continue;
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!str_ends_with(e->d_name, ".h")) continue;
      char path[2304]; snprintf(path, sizeof path, "%s/%s", p->fw_hdrs[f], e->d_name);
      char *txt = slurp(path);
      if (txt) { harvest_header_text(v, p->fw_mod[f], txt); free(txt); }
    }
    closedir(d);
  }
  return p->nfw;   /* number of bundled frameworks harvested */
}

/* ── Vendored package (CocoaPods / Carthage) type-surface harvesting ───────── *
 *
 * CocoaPods (`Pods/`) and Carthage (`Carthage/`) vendor their dependencies'
 * SOURCE (and, for Carthage, prebuilt `.framework`s) into the repo.  We do NOT
 * analyze that source as project modules — that would count each dependency's
 * own sema errors against the app (a Swift pod like Nimble/Quick alone yields
 * dozens of errors from the dependency's code).  Instead we publish only the
 * public TYPE SURFACE into the per-import vocab — the same model used for
 * `.xcframework` ObjC headers — so the app's `import <Dep>` references resolve.
 * `Pods`/`Carthage` are in skip_dir, so module discovery never descends into
 * them; this is the one place that reads them.                                  */

/* Recursively harvest every Swift / Swift-interface / ObjC-header file under
 * `dir` into vocab module `mod`.  `.swift` and `.swiftinterface` go through the
 * Swift parser (public type surface); `.h` through the ObjC header scanner. */
static void harvest_source_tree(MSFVocab *v, const char *mod, const char *dir,
                                int depth) {
  if (depth > 10) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (skip_dir(e->d_name)) continue;
    char path[2304];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (is_dir(path)) { harvest_source_tree(v, mod, path, depth + 1); continue; }
    if (str_ends_with(e->d_name, ".swift") ||
        str_ends_with(e->d_name, ".swiftinterface")) {
      char *txt = slurp(path);
      if (txt) { msf_vocab_add_interface(v, mod, txt); free(txt); }
    } else if (str_ends_with(e->d_name, ".h")) {
      char *txt = slurp(path);
      if (txt) { harvest_header_text(v, mod, txt); free(txt); }
    }
  }
  closedir(d);
}

/* Carthage prebuilt frameworks live at Carthage/Build/<platform>/<Name>.framework.
 * Harvest each framework's public ObjC headers (Headers/*.h) and, when present,
 * its textual Swift module interface (Modules/<Name>.swiftmodule/*.swiftinterface)
 * into vocab module <Name>.  The same framework is built per-platform; the vocab
 * dedups by name, so harvesting every slice is harmless. */
static void harvest_carthage_build(MSFVocab *v, const char *build_dir,
                                   size_t *count) {
  DIR *d = opendir(build_dir);
  if (!d) return;
  struct dirent *plat;
  while ((plat = readdir(d))) {
    if (skip_dir(plat->d_name)) continue;
    char platdir[1600];
    snprintf(platdir, sizeof platdir, "%s/%s", build_dir, plat->d_name);
    if (!is_dir(platdir)) continue;
    DIR *pd = opendir(platdir);
    if (!pd) continue;
    struct dirent *fw;
    while ((fw = readdir(pd))) {
      if (!str_ends_with(fw->d_name, ".framework")) continue;
      size_t nl = strlen(fw->d_name) - 10; /* strip ".framework" */
      char name[256];
      if (nl >= sizeof name) nl = sizeof name - 1;
      memcpy(name, fw->d_name, nl);
      name[nl] = 0;
      char fwdir[1900];
      snprintf(fwdir, sizeof fwdir, "%s/%s", platdir, fw->d_name);
      /* ObjC headers */
      char hdrs[2048];
      snprintf(hdrs, sizeof hdrs, "%s/Headers", fwdir);
      harvest_source_tree(v, name, hdrs, 0);
      /* Textual Swift interface(s) */
      char mods[2048];
      snprintf(mods, sizeof mods, "%s/Modules", fwdir);
      harvest_source_tree(v, name, mods, 0);
      (*count)++;
    }
    closedir(pd);
  }
  closedir(d);
}

size_t msf_project_harvest_packages(const MSFProject *p, MSFVocab *v) {
  if (!p || !v || !p->root[0]) return 0;
  size_t count = 0;

  /* CocoaPods: <root>/Pods/<PodName>/  (each top-level dir is a pod whose
   * import name is the dir name).  Skip the bookkeeping dirs. */
  char pods[1280];
  snprintf(pods, sizeof pods, "%s/Pods", p->root);
  DIR *d = opendir(pods);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      if (!strcmp(e->d_name, "Target Support Files") ||
          !strcmp(e->d_name, "Local Podspecs") ||
          !strcmp(e->d_name, "Headers") ||
          str_ends_with(e->d_name, ".xcodeproj"))
        continue;
      char pd[1408];
      snprintf(pd, sizeof pd, "%s/%s", pods, e->d_name);
      if (!is_dir(pd)) continue;
      harvest_source_tree(v, e->d_name, pd, 0);
      count++;
    }
    closedir(d);
  }

  /* Carthage: prebuilt frameworks (Build) + checked-out source (Checkouts). */
  char cbuild[1280];
  snprintf(cbuild, sizeof cbuild, "%s/Carthage/Build", p->root);
  harvest_carthage_build(v, cbuild, &count);

  char cchk[1280];
  snprintf(cchk, sizeof cchk, "%s/Carthage/Checkouts", p->root);
  DIR *cd = opendir(cchk);
  if (cd) {
    struct dirent *e;
    while ((e = readdir(cd))) {
      if (e->d_name[0] == '.') continue;
      char dep[1408];
      snprintf(dep, sizeof dep, "%s/%s", cchk, e->d_name);
      if (!is_dir(dep)) continue;
      harvest_source_tree(v, e->d_name, dep, 0);
      count++;
    }
    closedir(cd);
  }

  return count; /* number of harvested pods + Carthage frameworks/checkouts */
}

/* ── The project's OWN Objective-C headers ─────────────────────────────────── *
 *
 * Mixed ObjC/Swift apps (Signal, most large/older iOS codebases) define many
 * types in Objective-C `.h` headers inside the project's own modules and
 * reference them from Swift via the ObjC↔Swift interop.  msf analyzes the
 * modules' `.swift` files but never read their `.h`, so every such ObjC class
 * surfaced as "use of undeclared type" (Signal: ~764 of ~1071 — TSMessage,
 * TSInteraction, OWSGroupCallMessage, …).  Harvest the project's own header
 * type surface into the vocab — the same name-only model as bundled
 * frameworks / vendored pods — so Swift references to the app's ObjC classes
 * resolve.  (The `.swift` half is already published by analysis, so this walks
 * headers only — re-parsing the Swift would be wasteful on a 3500-file app.) */
static void harvest_headers_tree(MSFVocab *v, const char *mod, const char *dir,
                                 int depth) {
  if (depth > 12) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (skip_dir(e->d_name)) continue;
    char path[2304];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (is_dir(path)) { harvest_headers_tree(v, mod, path, depth + 1); continue; }
    if (str_ends_with(e->d_name, ".h")) {
      char *txt = slurp(path);
      if (txt) { harvest_header_text(v, mod, txt); free(txt); }
    }
  }
  closedir(d);
}

size_t msf_project_harvest_own_headers(const MSFProject *p, MSFVocab *v) {
  if (!p || !v) return 0;
  size_t n = 0;
  for (size_t i = 0; i < p->count; i++) {
    if (!p->modules[i].dir[0]) continue;
    harvest_headers_tree(v, p->modules[i].name, p->modules[i].dir, 0);
    n++;
  }
  return n;
}

MSFProject *msf_project_open(const char *root_dir) {
  if (!root_dir) return NULL;
  MSFProject *p = calloc(1, sizeof(MSFProject));
  if (!p) return NULL;
  snprintf(p->root, sizeof p->root, "%s", root_dir);
  find_manifests(p, root_dir, "project.pbxproj", discover_xcode, 0);
  find_manifests(p, root_dir, "Package.swift",   discover_spm,   0);
  for (size_t i = 0; i < p->count; i++) module_scan_imports(p, i);
  find_xcframeworks(p, root_dir, 0);   /* bundled binary frameworks (headers) */
  return p;
}

MSFModule *msf_project_analyze_module(const MSFProject *p, size_t i) {
  if (!p || i >= p->count) return NULL;
  const ProjModule *m = &p->modules[i];
  MSFModule *mm = msf_module_new();
  if (!mm) return NULL;
  for (size_t j = 0; j < m->nfiles; j++) {
    char *src = slurp(m->files[j]);
    if (!src) {
      fprintf(stderr, "warning: could not read file '%s'\n", m->files[j]);
      continue;
    }
    if (msf_module_add_file(mm, src, m->files[j]) != 0) {
      fprintf(stderr, "warning: skipped file '%s' due to tokenization failure\n", m->files[j]);
    }
    free(src);
  }
  msf_module_analyze(mm);
  return mm;
}

/* Index of a vocabulary module by name, or -1. */
static int vocab_module_idx(const MSFVocab *v, const char *name) {
  for (size_t i = 0; i < msf_vocab_module_count(v); i++) {
    const char *n = msf_vocab_module_name(v, i);
    if (n && !strcmp(n, name)) return (int)i;
  }
  return -1;
}

static void vocab_copy_module_types(MSFVocab *v, const char *into,
                                    const char *module) {
  int idx = (module && module[0]) ? vocab_module_idx(v, module) : -1;
  if (idx < 0)
    return;
  size_t tc = msf_vocab_type_count(v, (size_t)idx);
  for (size_t t = 0; t < tc; t++)
    msf_vocab_add_type(v, into, msf_vocab_type_name(v, (size_t)idx, t),
                       msf_vocab_type_kind(v, (size_t)idx, t));
}

static int import_decl_has_attribute(const Source *src, const Token *tokens,
                                     size_t token_count,
                                     const ASTNode *import_decl,
                                     const char *attr) {
  for (const ASTNode *c = import_decl ? import_decl->first_child : NULL;
       c; c = c->next_sibling) {
    if (c->kind != AST_ATTRIBUTE || c->data.var.name_tok >= token_count)
      continue;
    if (token_text_is(src, &tokens[c->data.var.name_tok], attr))
      return 1;
  }
  return 0;
}

static int import_decl_module_name(const Source *src, const Token *tokens,
                                   size_t token_count,
                                   const ASTNode *import_decl,
                                   char *out, size_t cap) {
  out[0] = 0;
  if (!import_decl || import_decl->data.var.name_tok >= token_count)
    return 0;
  const Token *t = &tokens[import_decl->data.var.name_tok];
  if (t->type != TOK_IDENTIFIER && t->type != TOK_KEYWORD)
    return 0;
  size_t n = t->len;
  if (n >= cap)
    n = cap - 1;
  memcpy(out, src->data + t->pos, n);
  out[n] = 0;
  return out[0] != 0;
}

/* For every parsed `@_exported import X`, copy module X's vocabulary into
 * @p into — so a dependent that imports `into` also sees X's types. */
static void vocab_fold_reexports(MSFVocab *v, const char *into,
                                 const char *source_text) {
  if (!v || !into || !source_text)
    return;

  Source src = {
      .data = source_text,
      .len = strlen(source_text),
      .filename = "<project-module>",
  };
  TokenStream ts;
  token_stream_init(&ts, src.len / 4 + 64);
  if (!ts.tokens)
    return;
  if (lexer_tokenize(&src, &ts, 1, NULL) != 0) {
    token_stream_free(&ts);
    return;
  }

  ASTArena arena;
  ast_arena_init(&arena, 0);
  Parser *parser = parser_init(&src, &ts, &arena);
  ASTNode *root = parser ? parse_source_file(parser) : NULL;
  for (const ASTNode *n = root ? root->first_child : NULL;
       n; n = n->next_sibling) {
    if (n->kind != AST_IMPORT_DECL ||
        !import_decl_has_attribute(&src, ts.tokens, ts.count, n, "_exported"))
      continue;
    char module[160];
    if (import_decl_module_name(&src, ts.tokens, ts.count, n,
                                module, sizeof module))
      vocab_copy_module_types(v, into, module);
  }
  parser_destroy(parser);
  ast_arena_free(&arena);
  token_stream_free(&ts);
}

/* Collect, deduplicated, the names of modules imported with `@testable` in
 * @p source_text (so a test target can be given those modules' internals). */
static void collect_testable_imports(const char *source_text,
                                     char names[][160], int max, int *count) {
  if (!source_text || !names || !count) return;
  Source src = { .data = source_text, .len = strlen(source_text),
                 .filename = "<project-module>" };
  TokenStream ts;
  token_stream_init(&ts, src.len / 4 + 64);
  if (!ts.tokens) return;
  if (lexer_tokenize(&src, &ts, 1, NULL) != 0) { token_stream_free(&ts); return; }
  ASTArena arena;
  ast_arena_init(&arena, 0);
  Parser *parser = parser_init(&src, &ts, &arena);
  ASTNode *root = parser ? parse_source_file(parser) : NULL;
  for (const ASTNode *n = root ? root->first_child : NULL; n; n = n->next_sibling) {
    /* @testable is parsed as a MOD_TESTABLE_IMPORT modifier on the import decl,
     * not an @-attribute node (see parser parse_single_attribute). */
    if (n->kind != AST_IMPORT_DECL || !(n->modifiers & MOD_TESTABLE_IMPORT))
      continue;
    char module[160];
    if (!import_decl_module_name(&src, ts.tokens, ts.count, n, module,
                                 sizeof module))
      continue;
    int seen = 0;
    for (int k = 0; k < *count; k++)
      if (!strcmp(names[k], module)) { seen = 1; break; }
    if (!seen && *count < max) {
      strncpy(names[*count], module, 159);
      names[*count][159] = 0;
      (*count)++;
    }
  }
  parser_destroy(parser);
  ast_arena_free(&arena);
  token_stream_free(&ts);
}

/* Index of the discovered project module named @p name, or -1. */
static int proj_module_idx_by_name(const MSFProject *p, const char *name) {
  for (size_t i = 0; i < p->count; i++)
    if (!strcmp(p->modules[i].name, name)) return (int)i;
  return -1;
}

MSFModule *msf_project_analyze_module_resolved(const MSFProject *p, size_t i,
                                               MSFVocab *shared_vocab) {
  if (!p || i >= p->count) return NULL;
  const ProjModule *m = &p->modules[i];

  MSFModule *mm = msf_module_new();
  if (!mm) return NULL;
  for (size_t j = 0; j < m->nfiles; j++) {
    char *src = slurp(m->files[j]);
    if (!src) {
      fprintf(stderr, "warning: could not read file '%s'\n", m->files[j]);
      continue;
    }
    if (msf_module_add_file(mm, src, m->files[j]) != 0) {
      fprintf(stderr, "warning: skipped file '%s' due to tokenization failure\n", m->files[j]);
    }
    free(src);
  }
  /* Resolve this module's `import`s from the shared vocabulary (filled by its
   * already-analyzed dependencies — caller must analyze in dependency order). */
  if (shared_vocab) msf_module_set_vocabulary(mm, shared_vocab);
  /* SDK names resolve globally via the project's SDK vocab (if set), kept
   * separate from the per-import shared vocab so cross-module refs stay scoped. */
  if (p->sdk_vocab) msf_module_set_sdk_vocabulary(mm, p->sdk_vocab);

  /* `@testable import X` — a test target sees module X's INTERNAL declarations,
   * not just its public interface.  For each testable dependency that is a
   * discovered project module, add its internal interface to the shared vocab
   * for the duration of THIS module's analysis, then restore the prior type
   * count — so the internals stay scoped to this importer and do not leak to
   * plain `import X` users. */
  char tmods[32][160];
  int n_tmods = 0;
  char restore_name[32][160];
  size_t restore_count[32];
  int restore_n = 0;
  if (shared_vocab) {
    for (size_t j = 0; j < m->nfiles; j++) {
      char *src = slurp(m->files[j]);
      if (!src) continue;
      collect_testable_imports(src, tmods, 32, &n_tmods);
      free(src);
    }
    for (int t = 0; t < n_tmods; t++) {
      int xi = proj_module_idx_by_name(p, tmods[t]);
      if (xi < 0) continue; /* not an analyzable project module — skip */
      restore_count[restore_n] = msf_vocab_module_type_count(shared_vocab, tmods[t]);
      strncpy(restore_name[restore_n], tmods[t], 159);
      restore_name[restore_n][159] = 0;
      restore_n++;
      const ProjModule *xm = &p->modules[xi];
      for (size_t j = 0; j < xm->nfiles; j++) {
        char *src = slurp(xm->files[j]);
        if (!src) continue;
        msf_vocab_add_interface_testable(shared_vocab, tmods[t], src);
        free(src);
      }
    }
  }

  msf_module_analyze(mm);

  for (int t = 0; t < restore_n; t++)
    msf_vocab_module_truncate(shared_vocab, restore_name[t], restore_count[t]);

  /* Publish this module's own public types into the shared vocabulary so that
   * later (dependent) modules resolve `import <this module>`.  Also fold in
   * `@_exported import X` re-exports: a dependent that imports this module sees
   * X's types too (e.g. an umbrella module re-exporting its sub-modules).  X was
   * analyzed earlier (dependency order) and ITS re-exports are already merged,
   * so transitivity falls out. */
  if (shared_vocab)
    for (size_t j = 0; j < m->nfiles; j++) {
      char *src = slurp(m->files[j]);
      if (!src) continue;
      msf_vocab_add_interface(shared_vocab, m->name, src);
      vocab_fold_reexports(shared_vocab, m->name, src);
      free(src);
    }
  return mm;
}

#else  /* WASM_BUILD — no host filesystem */
MSFProject *msf_project_open(const char *root_dir) { (void)root_dir; return NULL; }
MSFModule  *msf_project_analyze_module(const MSFProject *p, size_t i) { (void)p; (void)i; return NULL; }
MSFModule  *msf_project_analyze_module_resolved(const MSFProject *p, size_t i, MSFVocab *v) {
  (void)p; (void)i; (void)v; return NULL;
}
size_t msf_project_harvest_frameworks(const MSFProject *p, MSFVocab *v) {
  (void)p; (void)v; return 0;
}
size_t msf_project_harvest_packages(const MSFProject *p, MSFVocab *v) {
  (void)p; (void)v; return 0;
}
size_t msf_project_harvest_own_headers(const MSFProject *p, MSFVocab *v) {
  (void)p; (void)v; return 0;
}
#endif

/* ── Accessors (FS-independent) ──────────────────────────────────────────── */
size_t msf_project_module_count(const MSFProject *p) { return p ? p->count : 0; }

const char *msf_project_module_name(const MSFProject *p, size_t i) {
  return (p && i < p->count) ? p->modules[i].name : NULL;
}
const char *msf_project_module_kind(const MSFProject *p, size_t i) {
  return (p && i < p->count) ? p->modules[i].kind : NULL;
}
const char *msf_project_module_dir(const MSFProject *p, size_t i) {
  return (p && i < p->count) ? p->modules[i].dir : NULL;
}
size_t msf_project_module_file_count(const MSFProject *p, size_t i) {
  return (p && i < p->count) ? p->modules[i].nfiles : 0;
}
const char *msf_project_module_file(const MSFProject *p, size_t i, size_t j) {
  if (!p || i >= p->count || j >= p->modules[i].nfiles) return NULL;
  return p->modules[i].files[j];
}

size_t msf_project_module_dep_count(const MSFProject *p, size_t i) {
  return (p && i < p->count) ? (size_t)p->modules[i].ndeps : 0;
}
size_t msf_project_module_dep(const MSFProject *p, size_t i, size_t k) {
  if (!p || i >= p->count || k >= (size_t)p->modules[i].ndeps) return (size_t)-1;
  return (size_t)p->modules[i].deps[k];
}

size_t msf_project_entry_module(const MSFProject *p) {
  if (!p) return (size_t)-1;
  for (size_t i = 0; i < p->count; i++)
    if (strcmp(p->modules[i].kind, "spm-executable") == 0) return i;
  return (size_t)-1;
}

/* Post-order DFS: a module is emitted after its dependencies.  state[i] is
 * 0 = unvisited, 2 = on the current stack (cycle guard), 1 = done. */
static void proj_order_visit(const MSFProject *p, size_t i, char *state,
                             size_t *out, size_t *n, size_t cap) {
  if (state[i]) return;
  state[i] = 2;
  for (int k = 0; k < p->modules[i].ndeps; k++) {
    int d = p->modules[i].deps[k];
    if (d >= 0 && (size_t)d < p->count && state[d] != 2)
      proj_order_visit(p, (size_t)d, state, out, n, cap);
  }
  state[i] = 1;
  if (*n < cap) out[(*n)++] = i;
}

size_t msf_project_compile_order(const MSFProject *p, size_t *out, size_t cap) {
  if (!p || !out || cap == 0) return 0;
  char *state = calloc(p->count, 1);
  if (!state) return 0;
  size_t n = 0;
  for (size_t i = 0; i < p->count; i++)
    proj_order_visit(p, i, state, out, &n, cap);
  free(state);
  return n;
}

size_t msf_project_external_import_count(const MSFProject *p) { return p ? p->next : 0; }
const char *msf_project_external_import(const MSFProject *p, size_t k) {
  return (p && k < p->next) ? p->ext[k] : NULL;
}

void msf_project_free(MSFProject *p) {
  if (!p) return;
  for (size_t i = 0; i < p->count; i++) {
    for (size_t j = 0; j < p->modules[i].nfiles; j++) free(p->modules[i].files[j]);
    free(p->modules[i].files);
  }
  for (size_t e = 0; e < p->next; e++) free(p->ext[e]);
  free(p->ext);
  for (size_t i = 0; i < p->npkg; i++) free(p->pkgseen[i]);
  free(p->pkgseen);
  for (size_t i = 0; i < p->nfw; i++) { free(p->fw_mod[i]); free(p->fw_hdrs[i]); }
  free(p->fw_mod);
  free(p->fw_hdrs);
  free(p->modules);
  free(p);
}
