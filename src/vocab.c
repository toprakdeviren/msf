/**
 * @file vocab.c
 * @brief Module vocabulary — extract a module's public types AND their member
 *        signatures from its textual `.swiftinterface`, and (de)serialize them
 *        as portable `.msfvocab` text.
 *
 * WHY THIS EXISTS
 *
 *   Resolving cross-module references (`import SwiftUI` → `View`, `Text`, …)
 *   needs to know which type names a module exports.  Resolving member accesses
 *   (`view.addSubview(…)`, `view.layer`) further needs each type's members.  On
 *   a machine with the SDK that knowledge lives in `.swiftinterface` files (or
 *   is synthesized by `swift-synthesize-interface`).  msf parses those with its
 *   OWN parser (more faithful than a line regex: multi-line declarations,
 *   attribute chains, types nested in other types or extensions) and emits a
 *   compact text artifact.  That artifact then ships to environments with no
 *   SDK / `xcrun` / `.swiftinterface` at all — browser (WASM), Windows — where
 *   it is loaded back to seed type and member resolution.
 *
 *   msf stays frontend-only: it produces and consumes the vocabulary text but
 *   embeds no SDK data of its own.
 *
 * THE FORMAT (`.msfvocab`)
 *
 *   msfvocab 2
 *   module SwiftUI 751
 *   P View
 *   S Text
 *   C UIView
 *   \tf addSubview(_ view: UIView)
 *   \tp layer: CALayer
 *   \ti init(frame: CGRect)
 *
 *   Line 1 is a magic + version (1 = names only, 2 = names + members; a v2
 *   reader accepts both).  Each `module <name> <count>` line opens a section of
 *   <count> `<kind> <name>` type lines.  Type kind is one char: S struct ·
 *   C class · E enum · P protocol · A actor · T typealias.  A type line may be
 *   followed by TAB-prefixed member lines `\t<kind> <signature>`; member kind is
 *   f func · p property · i init · s subscript · c case · t typealias.
 */

#include "msf.h"
#include "internal/msf.h"   /* parser_init, parse_source_file, parser_destroy */
#include "internal/lexer.h" /* token_stream_init/free, lexer_tokenize         */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The SDK module vocabulary compiled into the library (generated/sdk_vocab.h,
 * produced by `make sdk-vocab`).  Absent in a fresh tree → empty fallback so the
 * build still works; regenerate to embed real SDK types. */
/* The WebAssembly playground build defines MSF_WEB_VOCAB and links the trimmed
 * hybrid vocab (all SDK type names + member signatures only for playground-core
 * modules — generated/sdk_vocab_web.h, ~3.7 MB vs ~13 MB).  Every other build
 * (the native msf analyzer) is unaffected and embeds the full sdk_vocab.h, so
 * whole-project analysis of real apps keeps its complete SDK type+member data. */
#if defined(MSF_WEB_VOCAB) && defined(__has_include) && \
    __has_include("sdk_vocab_web.h")
#  include "sdk_vocab_web.h"
#elif defined(__has_include) && __has_include("sdk_vocab.h")
#  include "sdk_vocab.h"
#else
static const char MSF_SDK_VOCAB[] = "msfvocab 2\n";
#endif

/* Guard against pathological nesting in untrusted input. `.swiftinterface`
 * nesting is shallow; this only bounds recursion, never normal extraction. */
#define VOCAB_MAX_DEPTH 64u

/* A single member's signature has a generous but finite cap. */
#define VOCAB_SIG_MAX 4096u

/* ═══════════════════════════════════════════════════════════════════════════
 * Data — struct-of-arrays so a module's names are a ready-to-return char**.
 * Each type carries a parallel MemberSet (its methods/properties/…).
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  char    **sigs;   /**< Owned signature strings.                  */
  uint8_t  *kinds;  /**< Parallel MSFVocabMemberKind bytes.        */
  size_t    count;
  size_t    cap;
} MemberSet;

typedef struct {
  char      *name;          /**< Owned module name.                        */
  char     **type_names;    /**< Owned array of owned type-name strings.   */
  uint8_t   *type_kinds;    /**< Parallel MSFVocabKind bytes.              */
  MemberSet *type_members;  /**< Parallel members of each type.            */
  char     **type_confs;    /**< Parallel space-joined conformance string  */
                            /**< per type (NULL if none) — drives literal  */
                            /**< coercion & protocol queries (vocab v3).   */
  size_t     count;
  size_t     cap;
  char     **deps;          /**< Owned names of imported modules (edges).  */
  size_t     dep_count;
  size_t     dep_cap;
} VocabModule;

struct MSFVocab {
  VocabModule *modules;
  size_t       count;
  size_t       cap;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Kind ↔ character
 * ═══════════════════════════════════════════════════════════════════════════ */

static char kind_to_char(MSFVocabKind k) {
  switch (k) {
    case MSF_VOCAB_STRUCT:    return 'S';
    case MSF_VOCAB_CLASS:     return 'C';
    case MSF_VOCAB_ENUM:      return 'E';
    case MSF_VOCAB_PROTOCOL:  return 'P';
    case MSF_VOCAB_ACTOR:     return 'A';
    case MSF_VOCAB_TYPEALIAS: return 'T';
  }
  return 'S';
}

/* Returns 1 and writes *out if `c` names a vocabulary kind; 0 otherwise. */
static int char_to_kind(char c, MSFVocabKind *out) {
  switch (c) {
    case 'S': *out = MSF_VOCAB_STRUCT;    return 1;
    case 'C': *out = MSF_VOCAB_CLASS;     return 1;
    case 'E': *out = MSF_VOCAB_ENUM;      return 1;
    case 'P': *out = MSF_VOCAB_PROTOCOL;  return 1;
    case 'A': *out = MSF_VOCAB_ACTOR;     return 1;
    case 'T': *out = MSF_VOCAB_TYPEALIAS; return 1;
    default:  return 0;
  }
}

static char member_kind_to_char(MSFVocabMemberKind k) {
  switch (k) {
    case MSF_VOCAB_MEMBER_FUNC:      return 'f';
    case MSF_VOCAB_MEMBER_PROPERTY:  return 'p';
    case MSF_VOCAB_MEMBER_INIT:      return 'i';
    case MSF_VOCAB_MEMBER_SUBSCRIPT: return 's';
    case MSF_VOCAB_MEMBER_CASE:      return 'c';
    case MSF_VOCAB_MEMBER_TYPEALIAS: return 't';
  }
  return 'f';
}

static int char_to_member_kind(char c, MSFVocabMemberKind *out) {
  switch (c) {
    case 'f': *out = MSF_VOCAB_MEMBER_FUNC;      return 1;
    case 'p': *out = MSF_VOCAB_MEMBER_PROPERTY;  return 1;
    case 'i': *out = MSF_VOCAB_MEMBER_INIT;      return 1;
    case 's': *out = MSF_VOCAB_MEMBER_SUBSCRIPT; return 1;
    case 'c': *out = MSF_VOCAB_MEMBER_CASE;      return 1;
    case 't': *out = MSF_VOCAB_MEMBER_TYPEALIAS; return 1;
    default:  return 0;
  }
}

/* strdup is POSIX, not C11; provide a portable copy (msf builds with -std=c11). */
static char *dup_str(const char *s) {
  size_t n = strlen(s);
  char *p = malloc(n + 1);
  if (p) memcpy(p, s, n + 1);
  return p;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Module / type / member storage
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Finds a module by name, or appends a new empty one.  NULL on OOM. */
static VocabModule *vocab_module_get(MSFVocab *v, const char *name) {
  for (size_t i = 0; i < v->count; i++)
    if (strcmp(v->modules[i].name, name) == 0)
      return &v->modules[i];

  if (v->count == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 8;
    VocabModule *nm = realloc(v->modules, nc * sizeof *nm);
    if (!nm) return NULL;
    v->modules = nm;
    v->cap = nc;
  }
  VocabModule *mod = &v->modules[v->count];
  memset(mod, 0, sizeof *mod);
  mod->name = dup_str(name);
  if (!mod->name) return NULL;
  v->count++;
  return mod;
}

/**
 * @brief Adds a type to a module, deduplicating by name.
 * @param out_new  If non-NULL, set to 1 when a new entry was created, else 0.
 * @return The type's index (existing or new), or SIZE_MAX on allocation failure.
 */
static size_t vocab_module_add(VocabModule *mod, const char *name, size_t len,
                               MSFVocabKind kind, int *out_new) {
  if (out_new) *out_new = 0;
  for (size_t i = 0; i < mod->count; i++)
    if (strlen(mod->type_names[i]) == len &&
        memcmp(mod->type_names[i], name, len) == 0)
      return i; /* already have it */

  if (mod->count == mod->cap) {
    size_t nc = mod->cap ? mod->cap * 2 : 16;
    char     **nn = realloc(mod->type_names, nc * sizeof *nn);
    if (!nn) return SIZE_MAX;
    mod->type_names = nn;
    uint8_t   *nk = realloc(mod->type_kinds, nc * sizeof *nk);
    if (!nk) return SIZE_MAX;
    mod->type_kinds = nk;
    MemberSet *mm = realloc(mod->type_members, nc * sizeof *mm);
    if (!mm) return SIZE_MAX;
    mod->type_members = mm;
    char **ncf = realloc(mod->type_confs, nc * sizeof *ncf);
    if (!ncf) return SIZE_MAX;
    mod->type_confs = ncf;
    mod->cap = nc;
  }
  char *copy = malloc(len + 1);
  if (!copy) return SIZE_MAX;
  memcpy(copy, name, len);
  copy[len] = '\0';
  size_t idx = mod->count;
  mod->type_names[idx] = copy;
  mod->type_kinds[idx] = (uint8_t)kind;
  memset(&mod->type_members[idx], 0, sizeof mod->type_members[idx]);
  mod->type_confs[idx] = NULL;
  mod->count++;
  if (out_new) *out_new = 1;
  return idx;
}

/** @brief Appends a member signature to a type's MemberSet, deduplicating by
 *  (kind, signature).  Returns 1 if newly added, 0 if duplicate / on failure. */
static int vocab_type_add_member(VocabModule *mod, size_t type_idx,
                                 const char *sig, size_t len,
                                 MSFVocabMemberKind kind) {
  if (type_idx >= mod->count || len == 0) return 0;
  MemberSet *ms = &mod->type_members[type_idx];
  for (size_t i = 0; i < ms->count; i++)
    if (ms->kinds[i] == (uint8_t)kind && strlen(ms->sigs[i]) == len &&
        memcmp(ms->sigs[i], sig, len) == 0)
      return 0; /* already have it */

  if (ms->count == ms->cap) {
    size_t nc = ms->cap ? ms->cap * 2 : 8;
    char    **ns = realloc(ms->sigs, nc * sizeof *ns);
    if (!ns) return 0;
    ms->sigs = ns;
    uint8_t  *nk = realloc(ms->kinds, nc * sizeof *nk);
    if (!nk) return 0;
    ms->kinds = nk;
    ms->cap = nc;
  }
  char *copy = malloc(len + 1);
  if (!copy) return 0;
  memcpy(copy, sig, len);
  copy[len] = '\0';
  ms->sigs[ms->count] = copy;
  ms->kinds[ms->count] = (uint8_t)kind;
  ms->count++;
  return 1;
}

/** @brief Adds one protocol name to a type's space-joined conformance string,
 *  deduplicating exact tokens.  Tolerates OOM by leaving the string unchanged. */
static void vocab_type_add_conf(VocabModule *mod, size_t idx, const char *name,
                                size_t len) {
  if (idx >= mod->count || len == 0) return;
  char *cur = mod->type_confs[idx];
  if (cur) {                                    /* dedup against existing tokens */
    for (const char *q = cur; *q;) {
      const char *sp = strchr(q, ' ');
      size_t tlen = sp ? (size_t)(sp - q) : strlen(q);
      if (tlen == len && memcmp(q, name, len) == 0) return;
      if (!sp) break;
      q = sp + 1;
    }
  }
  size_t curlen = cur ? strlen(cur) : 0;
  size_t need = curlen + (curlen ? 1 : 0) + len + 1;
  char *grown = realloc(cur, need);
  if (!grown) return;
  if (curlen) grown[curlen] = ' ';
  memcpy(grown + curlen + (curlen ? 1 : 0), name, len);
  grown[need - 1] = '\0';
  mod->type_confs[idx] = grown;
}

/** @brief Records a dependency (imported module) for @p mod, deduplicating and
 *  skipping self-imports.  Returns 1 if newly added, 0 otherwise. */
static int vocab_module_add_dep(VocabModule *mod, const char *name, size_t len) {
  if (len == 0) return 0;
  if (strlen(mod->name) == len && memcmp(mod->name, name, len) == 0)
    return 0; /* self */
  for (size_t i = 0; i < mod->dep_count; i++)
    if (strlen(mod->deps[i]) == len && memcmp(mod->deps[i], name, len) == 0)
      return 0; /* already have it */

  if (mod->dep_count == mod->dep_cap) {
    size_t nc = mod->dep_cap ? mod->dep_cap * 2 : 8;
    char **nd = realloc(mod->deps, nc * sizeof *nd);
    if (!nd) return 0;
    mod->deps = nd;
    mod->dep_cap = nc;
  }
  char *copy = malloc(len + 1);
  if (!copy) return 0;
  memcpy(copy, name, len);
  copy[len] = '\0';
  mod->deps[mod->dep_count++] = copy;
  return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

MSFVocab *msf_vocab_create(void) {
  return calloc(1, sizeof(MSFVocab));
}

/* A type referenced in a conformance/superclass clause (`C UIView : … X …`) is
 * by definition a type — but ObjC/C-originated SDK types (UICoordinateSpace,
 * OSStatus, CFIndex, …) are only REFERENCED in a `.swiftinterface`, never
 * declared there (their declaration lives in the ObjC/C module), so the vocab
 * lacks an entry for them and Swift uses of them report "use of undeclared
 * type".  Materialize each such referenced-but-undeclared name as a type so it
 * resolves (kind doesn't matter for existence — most are protocols). */
typedef struct { const char **slots; size_t cap; } NameSet;
static unsigned long vocab_fnv1a(const char *s) {
  unsigned long h = 1469598103934665603UL;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211UL; }
  return h;
}
static int nameset_has(const NameSet *s, const char *k) {
  if (!s->slots) return 0;
  size_t m = s->cap - 1, i = vocab_fnv1a(k) & m;
  while (s->slots[i]) { if (!strcmp(s->slots[i], k)) return 1; i = (i + 1) & m; }
  return 0;
}
static void nameset_add(NameSet *s, const char *k) {
  if (!s->slots) return;
  size_t m = s->cap - 1, i = vocab_fnv1a(k) & m;
  while (s->slots[i]) { if (!strcmp(s->slots[i], k)) return; i = (i + 1) & m; }
  s->slots[i] = k;
}

size_t msf_vocab_materialize_referenced_types(MSFVocab *v) {
  if (!v) return 0;
  size_t total = 0;
  for (size_t i = 0; i < v->count; i++) total += v->modules[i].count;
  NameSet set = {0};
  set.cap = 64; while (set.cap < (total + 16) * 2) set.cap <<= 1;
  set.slots = calloc(set.cap, sizeof *set.slots);
  if (!set.slots) return 0;
  /* index every DECLARED name (strings are heap-owned and stable across a
   * v->modules realloc, so these pointers stay valid while we add below) */
  for (size_t i = 0; i < v->count; i++)
    for (size_t t = 0; t < v->modules[i].count; t++)
      nameset_add(&set, v->modules[i].type_names[t]);

  size_t added = 0, nm = v->count;
  for (size_t i = 0; i < nm; i++) {
    size_t tc = v->modules[i].count;            /* fixed: additions go elsewhere */
    for (size_t t = 0; t < tc; t++) {
      /* re-index v->modules[i] each step — add_type below may realloc it */
      const char *confs = v->modules[i].type_confs ? v->modules[i].type_confs[t]
                                                   : NULL;
      if (!confs) continue;
      for (const char *p = confs; *p;) {
        while (*p == ' ') p++;
        const char *s = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - s);
        if (!len || len >= 128) continue;
        char name[128];
        memcpy(name, s, len); name[len] = 0;
        if (!nameset_has(&set, name)) {
          /* dedup of repeats is handled by vocab_module_add's within-module
           * dedup, so a name referenced N times is materialized once. */
          if (msf_vocab_add_type(v, "__objc_referenced", name,
                                 MSF_VOCAB_PROTOCOL))
            added++;
        }
      }
    }
  }
  free(set.slots);
  return added;
}

MSFVocab *msf_vocab_builtin(void) {
  MSFVocab *v = msf_vocab_parse(MSF_SDK_VOCAB);
  if (v) msf_vocab_materialize_referenced_types(v);
  return v;
}

int msf_vocab_add_type(MSFVocab *v, const char *module, const char *name,
                       MSFVocabKind kind) {
  if (!v || !module || !*module || !name || !*name) return 0;
  VocabModule *mod = vocab_module_get(v, module);
  if (!mod) return 0;
  int is_new = 0;
  size_t idx = vocab_module_add(mod, name, strlen(name), kind, &is_new);
  return (idx != SIZE_MAX) ? is_new : 0;
}

void msf_vocab_free(MSFVocab *v) {
  if (!v) return;
  for (size_t i = 0; i < v->count; i++) {
    VocabModule *mod = &v->modules[i];
    for (size_t j = 0; j < mod->count; j++) {
      free(mod->type_names[j]);
      MemberSet *ms = &mod->type_members[j];
      for (size_t k = 0; k < ms->count; k++)
        free(ms->sigs[k]);
      free(ms->sigs);
      free(ms->kinds);
      if (mod->type_confs) free(mod->type_confs[j]);
    }
    free(mod->type_names);
    free(mod->type_kinds);
    free(mod->type_members);
    free(mod->type_confs);
    for (size_t d = 0; d < mod->dep_count; d++)
      free(mod->deps[d]);
    free(mod->deps);
    free(mod->name);
  }
  free(v->modules);
  free(v);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Extraction — walk the parsed AST collecting public nominal types + members
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Returns the vocabulary kind for a nominal/typealias decl node, or -1. */
static int decl_kind(const ASTNode *n) {
  switch (n->kind) {
    case AST_STRUCT_DECL:    return MSF_VOCAB_STRUCT;
    case AST_CLASS_DECL:     return MSF_VOCAB_CLASS;
    case AST_ENUM_DECL:      return MSF_VOCAB_ENUM;
    case AST_PROTOCOL_DECL:  return MSF_VOCAB_PROTOCOL;
    case AST_ACTOR_DECL:     return MSF_VOCAB_ACTOR;
    case AST_TYPEALIAS_DECL: return MSF_VOCAB_TYPEALIAS;
    default:                 return -1;
  }
}

/* Returns the member kind for a member decl node, or -1 if not a member. */
static int member_kind_of(const ASTNode *n) {
  switch (n->kind) {
    case AST_FUNC_DECL:         return MSF_VOCAB_MEMBER_FUNC;
    case AST_VAR_DECL:
    case AST_LET_DECL:          return MSF_VOCAB_MEMBER_PROPERTY;
    case AST_INIT_DECL:         return MSF_VOCAB_MEMBER_INIT;
    case AST_SUBSCRIPT_DECL:    return MSF_VOCAB_MEMBER_SUBSCRIPT;
    /* The enum-case ELEMENT carries the name; the CASE_DECL is just its wrapper
     * (recording both would double-count `case a, b`). */
    case AST_ENUM_ELEMENT_DECL: return MSF_VOCAB_MEMBER_CASE;
    default:                    return -1;
  }
}

static int is_exported(const ASTNode *n) {
  return (n->modifiers & (MOD_PUBLIC | MOD_OPEN)) != 0;
}

/* `@testable import` visibility: everything a test target can see — public,
 * open, and internal (the default) — but NOT private/fileprivate. */
static int is_testable_visible(const ASTNode *n) {
  return (n->modifiers & (MOD_PRIVATE | MOD_FILEPRIVATE)) == 0;
}

/* Collapses the source span of token range [a,b) into buf: cut at the first
 * '{' (drops bodies / accessor blocks), collapse whitespace runs to one space,
 * trim ends.  A declaration node's tok_idx starts at the NAME, so the slice is
 * already free of leading attributes and the `func`/`var`/`open` keywords. */
static void vocab_sig_of(const Source *src, const Token *toks, size_t tok_count,
                         uint32_t a, uint32_t b, char *buf, size_t cap) {
  buf[0] = '\0';
  if (b <= a || a >= tok_count || b > tok_count) return;
  size_t s = toks[a].pos;
  size_t e = (size_t)toks[b - 1].pos + toks[b - 1].len;
  size_t o = 0;
  int sp = 1; /* leading: treat as space so we trim */
  for (size_t i = s; i < e && o + 1 < cap; i++) {
    char c = src->data[i];
    if (c == '{') break;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!sp) { buf[o++] = ' '; sp = 1; }
    } else {
      buf[o++] = c; sp = 0;
    }
  }
  while (o > 0 && buf[o - 1] == ' ') o--;
  buf[o] = '\0';
}

/* Records the direct protocol conformances of a nominal/extension decl onto its
 * vocab type: each AST_CONFORMANCE child lists one-or-more protocol nodes whose
 * tok_idx names the protocol.  Read from @p toks/@p src (the foreign interface),
 * not ctx->tokens. */
static void vocab_collect_confs(const Source *src, const Token *toks,
                                const ASTNode *decl, VocabModule *mod,
                                size_t idx) {
  for (const ASTNode *c = decl->first_child; c; c = c->next_sibling) {
    if (c->kind != AST_CONFORMANCE) continue;
    for (const ASTNode *p = c->first_child; p; p = p->next_sibling) {
      if (!p->tok_idx) continue;
      const Token *t = &toks[p->tok_idx];
      vocab_type_add_conf(mod, idx, src->data + t->pos, t->len);
    }
  }
}

/**
 * @brief Recursively records public nominal types, typealiases, and the members
 *        of each type under @p node.
 *
 * A type's members sit under an intervening container (a brace_stmt); the walk
 * carries the enclosing type's index so members attribute to it.  Extensions
 * find-or-add their target type, so `extension UIView { func foo() }` adds `foo`
 * to UIView.  Members are recorded only inside a known type (top-level globals
 * are skipped for now).  Names/signatures are copied out of @p src here, so the
 * parse arena may be freed afterward.
 */
static void vocab_collect(const Source *src, const Token *toks, size_t tok_count,
                          const ASTNode *node, unsigned depth, VocabModule *mod,
                          size_t cur_type, int testable) {
  if (depth >= VOCAB_MAX_DEPTH) return;

  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
    size_t child_type = cur_type;
    int kind = decl_kind(c);
    int mkind;

    /* `import X` / `import X.Sub` — record X as a dependency edge.  name_tok is
     * the first path component (the top-level module).  Imports are top-level
     * and carry no body to collect. */
    if (c->kind == AST_IMPORT_DECL) {
      if (c->data.var.name_tok) {
        const Token *t = &toks[c->data.var.name_tok];
        vocab_module_add_dep(mod, src->data + t->pos, t->len);
      }
      continue;
    }

    if (kind >= 0 && (testable ? is_testable_visible(c) : is_exported(c)) &&
        c->data.var.name_tok) {
      const Token *t = &toks[c->data.var.name_tok];
      size_t idx = vocab_module_add(mod, src->data + t->pos, t->len,
                                    (MSFVocabKind)kind, NULL);
      if (idx != SIZE_MAX) {
        /* A real declaration's kind wins over a placeholder kind recorded by an
         * earlier `extension <Type>` (which can't know if Type is a class/enum). */
        mod->type_kinds[idx] = (uint8_t)kind;
        vocab_collect_confs(src, toks, c, mod, idx);
        if (kind != MSF_VOCAB_TYPEALIAS) child_type = idx;
      }
    } else if (c->kind == AST_EXTENSION_DECL && c->data.var.name_tok) {
      /* The extended type is a real public type even when its primary
       * declaration lives in another module.  Record it (deduped) and route
       * its members onto it.  name_tok is the last path component. */
      const Token *t = &toks[c->data.var.name_tok];
      size_t idx = vocab_module_add(mod, src->data + t->pos, t->len,
                                    MSF_VOCAB_STRUCT, NULL);
      if (idx != SIZE_MAX) {
        vocab_collect_confs(src, toks, c, mod, idx);  /* `extension T: P` */
        child_type = idx;
      }
    } else if (cur_type != SIZE_MAX && (mkind = member_kind_of(c)) >= 0) {
      char sig[VOCAB_SIG_MAX];
      vocab_sig_of(src, toks, tok_count, c->tok_idx, c->tok_end, sig, sizeof sig);
      if (sig[0]) {
        /* init/subscript nodes start their slice just past the keyword (it was
         * consumed before the node was allocated); restore it for a readable,
         * self-describing signature.  func/property/case keep the slice as-is
         * (it already begins at the member name). */
        const char *kw = (mkind == MSF_VOCAB_MEMBER_INIT)      ? "init"
                       : (mkind == MSF_VOCAB_MEMBER_SUBSCRIPT) ? "subscript"
                                                               : "";
        if (*kw) {
          char full[VOCAB_SIG_MAX];
          int fn = snprintf(full, sizeof full, "%s%s", kw, sig);
          if (fn > 0 && (size_t)fn < sizeof full)
            vocab_type_add_member(mod, cur_type, full, (size_t)fn,
                                  (MSFVocabMemberKind)mkind);
        } else {
          vocab_type_add_member(mod, cur_type, sig, strlen(sig),
                                (MSFVocabMemberKind)mkind);
        }
      }
    }
    vocab_collect(src, toks, tok_count, c, depth + 1, mod, child_type, testable);
  }
}

static size_t vocab_add_interface_impl(MSFVocab *v, const char *module_name,
                                       const char *interface_src, int testable) {
  if (!v || !module_name || !interface_src) return 0;

  VocabModule *mod = vocab_module_get(v, module_name);
  if (!mod) return 0;
  size_t before = mod->count;

  Source src = { interface_src, strlen(interface_src), "<interface>" };

  TokenStream ts;
  token_stream_init(&ts, src.len / 4 + 64);
  if (!ts.tokens) return 0;
  LexerDiagnostics diag;
  lexer_diag_init(&diag);
  if (lexer_tokenize(&src, &ts, 1, &diag) != 0) {
    token_stream_free(&ts);
    return 0;
  }

  ASTArena arena;
  ast_arena_init(&arena, 0);
  Parser *p = parser_init(&src, &ts, &arena);
  ASTNode *root = p ? parse_source_file(p) : NULL;
  if (root)
    vocab_collect(&src, ts.tokens, ts.count, root, 0, mod, SIZE_MAX, testable);

  parser_destroy(p);
  ast_arena_free(&arena);
  token_stream_free(&ts);

  return mod->count - before;
}

size_t msf_vocab_add_interface(MSFVocab *v, const char *module_name,
                               const char *interface_src) {
  return vocab_add_interface_impl(v, module_name, interface_src, 0);
}

size_t msf_vocab_add_interface_testable(MSFVocab *v, const char *module_name,
                                        const char *interface_src) {
  return vocab_add_interface_impl(v, module_name, interface_src, 1);
}

size_t msf_vocab_module_type_count(const MSFVocab *v, const char *module_name) {
  if (!v || !module_name) return 0;
  for (size_t i = 0; i < v->count; i++)
    if (v->modules[i].name && !strcmp(v->modules[i].name, module_name))
      return v->modules[i].count;
  return 0;
}

void msf_vocab_module_truncate(MSFVocab *v, const char *module_name,
                               size_t count) {
  if (!v || !module_name) return;
  VocabModule *mod = NULL;
  for (size_t i = 0; i < v->count; i++)
    if (v->modules[i].name && !strcmp(v->modules[i].name, module_name)) {
      mod = &v->modules[i];
      break;
    }
  if (!mod || count >= mod->count) return;
  for (size_t j = count; j < mod->count; j++) {
    free(mod->type_names[j]);
    MemberSet *ms = &mod->type_members[j];
    for (size_t k = 0; k < ms->count; k++)
      free(ms->sigs[k]);
    free(ms->sigs);
    free(ms->kinds);
    if (mod->type_confs) free(mod->type_confs[j]);
  }
  mod->count = count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Serialize → .msfvocab text
 * ═══════════════════════════════════════════════════════════════════════════ */

/* A growable output buffer. */
typedef struct { char *buf; size_t len, cap; int oom; } StrBuf;

static void sb_append(StrBuf *sb, const char *data, size_t n) {
  if (sb->oom) return;
  if (sb->len + n + 1 > sb->cap) {
    size_t nc = sb->cap ? sb->cap : 256;
    while (sb->len + n + 1 > nc) nc *= 2;
    char *nb = realloc(sb->buf, nc);
    if (!nb) { sb->oom = 1; return; }
    sb->buf = nb;
    sb->cap = nc;
  }
  memcpy(sb->buf + sb->len, data, n);
  sb->len += n;
  sb->buf[sb->len] = '\0';
}

static void sb_appendz(StrBuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }

char *msf_vocab_serialize(const MSFVocab *v) {
  if (!v) return NULL;
  StrBuf sb = {0};
  sb_appendz(&sb, "msfvocab 3\n");

  char hdr[64];
  for (size_t i = 0; i < v->count; i++) {
    const VocabModule *mod = &v->modules[i];
    int hn = snprintf(hdr, sizeof hdr, "module %s %zu\n", mod->name, mod->count);
    /* Module name longer than the small buffer: emit it field-by-field. */
    if (hn < 0 || (size_t)hn >= sizeof hdr) {
      sb_appendz(&sb, "module ");
      sb_appendz(&sb, mod->name);
      snprintf(hdr, sizeof hdr, " %zu\n", mod->count);
      sb_appendz(&sb, hdr);
    } else {
      sb_append(&sb, hdr, (size_t)hn);
    }
    for (size_t d = 0; d < mod->dep_count; d++) {
      sb_appendz(&sb, "import ");
      sb_appendz(&sb, mod->deps[d]);
      sb_append(&sb, "\n", 1);
    }
    for (size_t j = 0; j < mod->count; j++) {
      char line[3] = { kind_to_char((MSFVocabKind)mod->type_kinds[j]), ' ', '\0' };
      sb_append(&sb, line, 2);
      sb_appendz(&sb, mod->type_names[j]);
      if (mod->type_confs && mod->type_confs[j]) {  /* v3: ` : Proto Proto…` */
        sb_appendz(&sb, " : ");
        sb_appendz(&sb, mod->type_confs[j]);
      }
      sb_append(&sb, "\n", 1);
      const MemberSet *ms = &mod->type_members[j];
      for (size_t k = 0; k < ms->count; k++) {
        char mline[4] = { '\t',
                          member_kind_to_char((MSFVocabMemberKind)ms->kinds[k]),
                          ' ', '\0' };
        sb_append(&sb, mline, 3);
        sb_appendz(&sb, ms->sigs[k]);
        sb_append(&sb, "\n", 1);
      }
    }
  }

  if (sb.oom) { free(sb.buf); return NULL; }
  if (!sb.buf) return dup_str("msfvocab 3\n"); /* defensive: empty vocab */
  return sb.buf;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Parse ← .msfvocab text
 * ═══════════════════════════════════════════════════════════════════════════ */

MSFVocab *msf_vocab_parse(const char *text) {
  if (!text) return NULL;
  MSFVocab *v = msf_vocab_create();
  if (!v) return NULL;

  VocabModule *cur = NULL;
  size_t cur_type = SIZE_MAX; /* index of the most recent type line in `cur` */
  const char *p = text;

  while (*p) {
    /* [line, eol) is one logical line (CRLF tolerated). */
    const char *line = p;
    const char *eol = line;
    while (*eol && *eol != '\n') eol++;
    const char *next = (*eol == '\n') ? eol + 1 : eol;
    size_t len = (size_t)(eol - line);
    if (len && line[len - 1] == '\r') len--; /* strip CR */

    if (len >= 8 && memcmp(line, "msfvocab", 8) == 0) {
      /* version header — ignore (v1 names-only and v2 names+members both load) */
    } else if (len >= 7 && memcmp(line, "module ", 7) == 0) {
      /* module <name> [count] — name is up to the next space or EOL. */
      const char *ns = line + 7;
      const char *ne = ns;
      while (ne < line + len && *ne != ' ') ne++;
      size_t nlen = (size_t)(ne - ns);
      cur_type = SIZE_MAX;
      if (nlen) {
        char *name = malloc(nlen + 1);
        if (!name) { msf_vocab_free(v); return NULL; }
        memcpy(name, ns, nlen);
        name[nlen] = '\0';
        cur = vocab_module_get(v, name);
        free(name);
        if (!cur) { msf_vocab_free(v); return NULL; }
      }
    } else if (len >= 7 && memcmp(line, "import ", 7) == 0 && cur) {
      /* Dependency edge: `import <module>` (emitted before the type lines). */
      vocab_module_add_dep(cur, line + 7, len - 7);
    } else if (len >= 3 && line[0] == '\t' && cur && cur_type != SIZE_MAX) {
      /* Member line: \t<kind> <signature> */
      MSFVocabMemberKind mk;
      if (line[2] == ' ' && char_to_member_kind(line[1], &mk))
        vocab_type_add_member(cur, cur_type, line + 3, len - 3, mk);
    } else if (len >= 3 && line[1] == ' ' && cur) {
      MSFVocabKind kind;
      if (char_to_kind(line[0], &kind)) {
        const char *tstart = line + 2;
        size_t tlen = len - 2;
        /* v3: an optional ` : Proto Proto…` conformance suffix splits the line
         * into the bare type name and its space-separated conformances. */
        const char *colon = NULL;
        for (size_t z = 0; z + 2 < tlen; z++)
          if (tstart[z] == ' ' && tstart[z + 1] == ':' && tstart[z + 2] == ' ') {
            colon = tstart + z;
            break;
          }
        size_t namelen = colon ? (size_t)(colon - tstart) : tlen;
        cur_type = vocab_module_add(cur, tstart, namelen, kind, NULL);
        if (colon && cur_type != SIZE_MAX) {
          const char *cf = colon + 3, *cend = tstart + tlen;
          while (cf < cend) {
            while (cf < cend && *cf == ' ') cf++;
            const char *ws = cf;
            while (ws < cend && *ws != ' ') ws++;
            if (ws > cf)
              vocab_type_add_conf(cur, cur_type, cf, (size_t)(ws - cf));
            cf = ws;
          }
        }
      }
    }

    p = next;
  }
  return v;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Query
 * ═══════════════════════════════════════════════════════════════════════════ */

size_t msf_vocab_module_count(const MSFVocab *v) {
  return v ? v->count : 0;
}

const char *msf_vocab_module_name(const MSFVocab *v, size_t index) {
  if (!v || index >= v->count) return NULL;
  return v->modules[index].name;
}

size_t msf_vocab_type_count(const MSFVocab *v, size_t index) {
  if (!v || index >= v->count) return 0;
  return v->modules[index].count;
}

const char *msf_vocab_type_name(const MSFVocab *v, size_t m, size_t t) {
  if (!v || m >= v->count || t >= v->modules[m].count) return NULL;
  return v->modules[m].type_names[t];
}

MSFVocabKind msf_vocab_type_kind(const MSFVocab *v, size_t m, size_t t) {
  if (!v || m >= v->count || t >= v->modules[m].count) return MSF_VOCAB_STRUCT;
  return (MSFVocabKind)v->modules[m].type_kinds[t];
}

const char *msf_vocab_type_conformances(const MSFVocab *v, size_t m, size_t t) {
  if (!v || m >= v->count || t >= v->modules[m].count) return NULL;
  return v->modules[m].type_confs ? v->modules[m].type_confs[t] : NULL;
}

size_t msf_vocab_member_count(const MSFVocab *v, size_t m, size_t t) {
  if (!v || m >= v->count || t >= v->modules[m].count) return 0;
  return v->modules[m].type_members[t].count;
}

const char *msf_vocab_member_signature(const MSFVocab *v, size_t m, size_t t,
                                       size_t i) {
  if (!v || m >= v->count || t >= v->modules[m].count) return NULL;
  const MemberSet *ms = &v->modules[m].type_members[t];
  if (i >= ms->count) return NULL;
  return ms->sigs[i];
}

MSFVocabMemberKind msf_vocab_member_kind(const MSFVocab *v, size_t m, size_t t,
                                         size_t i) {
  if (!v || m >= v->count || t >= v->modules[m].count) return MSF_VOCAB_MEMBER_FUNC;
  const MemberSet *ms = &v->modules[m].type_members[t];
  if (i >= ms->count) return MSF_VOCAB_MEMBER_FUNC;
  return (MSFVocabMemberKind)ms->kinds[i];
}

static int member_is_callable(MSFVocabMemberKind k) {
  return k == MSF_VOCAB_MEMBER_FUNC || k == MSF_VOCAB_MEMBER_INIT ||
         k == MSF_VOCAB_MEMBER_SUBSCRIPT;
}

const char *msf_vocab_find_member(const MSFVocab *v, const char *type_name,
                                  const char *member_name, int prefer_callable,
                                  MSFVocabMemberKind *out_kind) {
  if (!v || !type_name || !member_name) return NULL;
  size_t want = strlen(member_name);
  /* A type can carry both a property and a same-named method (ObjC `frame` /
   * `frame(forAlignmentRect:)`).  `obj.member` wants the property; a call
   * `obj.member(...)` wants the method.  Track the best of each so the caller's
   * position decides, instead of returning whichever was declared first. */
  const char *prop = NULL, *call = NULL, *any = NULL;
  MSFVocabMemberKind pk = 0, ck = 0, ak = 0;
  for (size_t mi = 0; mi < v->count; mi++) {
    const VocabModule *mod = &v->modules[mi];
    for (size_t ti = 0; ti < mod->count; ti++) {
      if (strcmp(mod->type_names[ti], type_name) != 0) continue;
      const MemberSet *ms = &mod->type_members[ti];
      for (size_t k = 0; k < ms->count; k++) {
        const char *sig = ms->sigs[k];
        /* The member name is the signature up to its first delimiter — '(' for
         * a call, ':' for a property, '<' for generics, '?'/'!' for a failable
         * init, or a space. */
        size_t nl = 0;
        while (sig[nl] && sig[nl] != '(' && sig[nl] != ':' && sig[nl] != ' ' &&
               sig[nl] != '?' && sig[nl] != '!' && sig[nl] != '<')
          nl++;
        if (nl != want || memcmp(sig, member_name, want) != 0) continue;
        MSFVocabMemberKind k2 = (MSFVocabMemberKind)ms->kinds[k];
        if (!any) { any = sig; ak = k2; }
        if (member_is_callable(k2)) { if (!call) { call = sig; ck = k2; } }
        else if (!prop) { prop = sig; pk = k2; }
      }
    }
  }
  const char *r;
  MSFVocabMemberKind rk;
  if (prefer_callable) { r = call ? call : (prop ? prop : any); rk = call ? ck : (prop ? pk : ak); }
  else                 { r = prop ? prop : (call ? call : any); rk = prop ? pk : (call ? ck : ak); }
  if (!r) return NULL;
  if (out_kind) *out_kind = rk;
  return r;
}

size_t msf_vocab_module_dep_count(const MSFVocab *v, size_t m) {
  if (!v || m >= v->count) return 0;
  return v->modules[m].dep_count;
}

const char *msf_vocab_module_dep(const MSFVocab *v, size_t m, size_t d) {
  if (!v || m >= v->count || d >= v->modules[m].dep_count) return NULL;
  return v->modules[m].deps[d];
}

/* Index of the module named [name,len), or SIZE_MAX if absent. */
static size_t vocab_find_module(const MSFVocab *v, const char *name, size_t len) {
  for (size_t i = 0; i < v->count; i++)
    if (strlen(v->modules[i].name) == len &&
        memcmp(v->modules[i].name, name, len) == 0)
      return i;
  return SIZE_MAX;
}

size_t msf_vocab_import_closure(const MSFVocab *v, const char *const *seeds,
                               size_t seed_count, const char **out,
                               size_t out_cap) {
  if (!v || v->count == 0) return 0;
  unsigned char *seen = calloc(v->count, 1);
  size_t *queue = malloc(v->count * sizeof *queue);
  if (!seen || !queue) { free(seen); free(queue); return 0; }
  size_t qhead = 0, qtail = 0, found = 0;

  for (size_t s = 0; s < seed_count; s++) {
    if (!seeds || !seeds[s]) continue;
    size_t idx = vocab_find_module(v, seeds[s], strlen(seeds[s]));
    if (idx != SIZE_MAX && !seen[idx]) { seen[idx] = 1; queue[qtail++] = idx; }
  }
  while (qhead < qtail) {
    size_t idx = queue[qhead++];
    if (out && found < out_cap) out[found] = v->modules[idx].name;
    found++;
    const VocabModule *mod = &v->modules[idx];
    for (size_t d = 0; d < mod->dep_count; d++) {
      size_t di = vocab_find_module(v, mod->deps[d], strlen(mod->deps[d]));
      if (di != SIZE_MAX && !seen[di]) { seen[di] = 1; queue[qtail++] = di; }
    }
  }
  free(seen);
  free(queue);
  return found;
}

int msf_vocab_has_type(const MSFVocab *v, const char *name) {
  if (!v || !name) return 0;
  for (size_t i = 0; i < v->count; i++) {
    const VocabModule *m = &v->modules[i];
    for (size_t j = 0; j < m->count; j++)
      if (strcmp(m->type_names[j], name) == 0) return 1;
  }
  return 0;
}

const char *const *msf_vocab_module_types(const MSFVocab *v,
                                          const char *module_name,
                                          size_t *out_count) {
  if (out_count) *out_count = 0;
  if (!v || !module_name) return NULL;
  for (size_t i = 0; i < v->count; i++) {
    if (strcmp(v->modules[i].name, module_name) == 0) {
      if (out_count) *out_count = v->modules[i].count;
      return (const char *const *)v->modules[i].type_names;
    }
  }
  return NULL;
}
