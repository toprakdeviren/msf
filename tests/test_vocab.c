/**
 * @file test_vocab.c
 * @brief Module vocabulary tests — extraction from `.swiftinterface`, the
 *        `.msfvocab` text round-trip, and resolving against a loaded vocabulary.
 */
#include "test_framework.h"
#include "msf.h"

#include <stdlib.h>
#include <string.h>

/* A miniature `.swiftinterface`: the public surface msf should harvest, plus
 * non-exported and extension-target noise it must skip. */
static const char *kInterface =
    "public struct PublicStruct {}\n"
    "public class PublicClass {}\n"
    "public enum PublicEnum { case a }\n"
    "public protocol PublicProto {}\n"
    "public actor PublicActor {}\n"
    "public typealias PublicAlias = Int\n"
    "open class OpenClass {}\n"
    "struct InternalStruct {}\n"            /* no `public` → skipped     */
    "internal class InternalClass {}\n"     /* explicit internal → skip  */
    "extension PublicStruct {\n"            /* re-record deduped → 1×     */
    "  public struct Nested {}\n"           /* nested public → recorded  */
    "}\n"
    "extension SomeMod.ForeignOnly {}\n";   /* extended-only type, qualified:
                                             * records the LAST component
                                             * (ForeignOnly), not the module */

/* The 9 names we expect, regardless of order. */
static const char *kExpected[] = {
    "PublicStruct", "PublicClass", "PublicEnum",   "PublicProto",
    "PublicActor",  "PublicAlias", "OpenClass",    "Nested",
    "ForeignOnly",
};

static int vocab_has(const MSFVocab *v, const char *module, const char *name) {
  size_t n = 0;
  const char *const *names = msf_vocab_module_types(v, module, &n);
  if (!names) return 0;
  for (size_t i = 0; i < n; i++)
    if (strcmp(names[i], name) == 0) return 1;
  return 0;
}

/* Conformance string of the first type named @p type_name, or NULL. */
static const char *vocab_confs_of(const MSFVocab *v, const char *type_name) {
  size_t nm = msf_vocab_module_count(v);
  for (size_t m = 0; m < nm; m++) {
    size_t nt = msf_vocab_type_count(v, m);
    for (size_t t = 0; t < nt; t++)
      if (!strcmp(msf_vocab_type_name(v, m, t), type_name))
        return msf_vocab_type_conformances(v, m, t);
  }
  return NULL;
}

/* ── Extraction ──────────────────────────────────────────────────────────── */

static void test_extract_public_types(void) {
  TEST("vocab: extracts public nominal types + typealias");
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);

  size_t added = msf_vocab_add_interface(v, "TestMod", kInterface);
  ASSERT_EQ(added, (size_t)9);
  ASSERT_EQ(msf_vocab_module_count(v), (size_t)1);
  ASSERT_STR_EQ(msf_vocab_module_name(v, 0), "TestMod");
  ASSERT_EQ(msf_vocab_type_count(v, 0), (size_t)9);

  for (size_t i = 0; i < sizeof kExpected / sizeof kExpected[0]; i++)
    if (!vocab_has(v, "TestMod", kExpected[i])) { FAIL(kExpected[i]); msf_vocab_free(v); return; }

  msf_vocab_free(v);
  PASS();
}

static void test_extract_skips_non_public(void) {
  TEST("vocab: skips internal types; records extension targets (deduped)");
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  msf_vocab_add_interface(v, "TestMod", kInterface);

  ASSERT(!vocab_has(v, "TestMod", "InternalStruct"));
  ASSERT(!vocab_has(v, "TestMod", "InternalClass"));

  /* An extended-only type (no primary decl here, like `extension DispatchQueue`
   * whose class lives in another module) IS recorded — as its last path
   * component, not the module qualifier. */
  ASSERT(vocab_has(v, "TestMod", "ForeignOnly"));
  ASSERT(!vocab_has(v, "TestMod", "SomeMod"));

  /* PublicStruct must appear exactly once despite the extension on it (the
   * extension target re-record is deduped against the primary decl). */
  size_t n = 0, hits = 0;
  const char *const *names = msf_vocab_module_types(v, "TestMod", &n);
  for (size_t i = 0; i < n; i++)
    if (strcmp(names[i], "PublicStruct") == 0) hits++;
  ASSERT_EQ(hits, (size_t)1);

  msf_vocab_free(v);
  PASS();
}

static void test_kinds(void) {
  TEST("vocab: records the correct declaration kinds");
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  msf_vocab_add_interface(v, "TestMod", kInterface);

  /* Find each name's kind via the indexed accessors. */
  size_t tc = msf_vocab_type_count(v, 0);
  int seen_alias = 0, seen_proto = 0, seen_actor = 0;
  for (size_t t = 0; t < tc; t++) {
    const char *nm = msf_vocab_type_name(v, 0, t);
    MSFVocabKind k = msf_vocab_type_kind(v, 0, t);
    if (!strcmp(nm, "PublicAlias")) { ASSERT_EQ(k, MSF_VOCAB_TYPEALIAS); seen_alias = 1; }
    if (!strcmp(nm, "PublicProto")) { ASSERT_EQ(k, MSF_VOCAB_PROTOCOL);  seen_proto = 1; }
    if (!strcmp(nm, "PublicActor")) { ASSERT_EQ(k, MSF_VOCAB_ACTOR);     seen_actor = 1; }
  }
  ASSERT(seen_alias && seen_proto && seen_actor);

  msf_vocab_free(v);
  PASS();
}

/* ── Serialize / parse round-trip ────────────────────────────────────────── */

static void test_roundtrip(void) {
  TEST("vocab: serialize → parse preserves modules and types");
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  msf_vocab_add_interface(v, "TestMod", kInterface);

  char *text = msf_vocab_serialize(v);
  ASSERT_NOT_NULL(text);
  ASSERT(strstr(text, "msfvocab 3") != NULL);
  ASSERT(strstr(text, "module TestMod 9") != NULL);

  MSFVocab *v2 = msf_vocab_parse(text);
  ASSERT_NOT_NULL(v2);
  ASSERT_EQ(msf_vocab_module_count(v2), (size_t)1);
  ASSERT_STR_EQ(msf_vocab_module_name(v2, 0), "TestMod");
  ASSERT_EQ(msf_vocab_type_count(v2, 0), (size_t)9);
  for (size_t i = 0; i < sizeof kExpected / sizeof kExpected[0]; i++)
    if (!vocab_has(v2, "TestMod", kExpected[i])) {
      FAIL(kExpected[i]); free(text); msf_vocab_free(v); msf_vocab_free(v2); return;
    }

  free(text);
  msf_vocab_free(v);
  msf_vocab_free(v2);
  PASS();
}

static void test_parse_multimodule(void) {
  TEST("vocab: parses a multi-module .msfvocab blob");
  const char *blob =
      "msfvocab 1\n"
      "module Swift 2\n"
      "S Array\n"
      "P Collection\n"
      "module SwiftUI 1\n"
      "P View\n";
  MSFVocab *v = msf_vocab_parse(blob);
  ASSERT_NOT_NULL(v);
  ASSERT_EQ(msf_vocab_module_count(v), (size_t)2);
  ASSERT(vocab_has(v, "Swift", "Array"));
  ASSERT(vocab_has(v, "Swift", "Collection"));
  ASSERT(vocab_has(v, "SwiftUI", "View"));
  ASSERT(!vocab_has(v, "SwiftUI", "Array"));
  msf_vocab_free(v);
  PASS();
}

/* ── Resolution against a loaded vocabulary ──────────────────────────────── */

static uint32_t count_undeclared(MSFResult *r) {
  uint32_t hits = 0;
  for (uint32_t i = 0; i < msf_error_count(r); i++)
    if (strstr(msf_error_message(r, i), "undeclared type")) hits++;
  return hits;
}

static void test_resolution_with_vocab(void) {
  TEST("vocab: loaded names resolve cross-module references");
  const char *code = "func use(_ x: PublicStruct, _ y: PublicProto) {}\n";

  /* Baseline: no vocabulary → both types are undeclared. */
  MSFResult *bare = msf_analyze(code, "use.swift");
  ASSERT_NOT_NULL(bare);
  uint32_t bare_undeclared = count_undeclared(bare);
  ASSERT(bare_undeclared >= 2);
  msf_result_free(bare);

  /* With the module's vocabulary fed in, they resolve. */
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  msf_vocab_add_interface(v, "TestMod", kInterface);
  size_t n = 0;
  const char *const *names = msf_vocab_module_types(v, "TestMod", &n);
  ASSERT_NOT_NULL(names);

  MSFResult *r = msf_analyze_in_module(code, "use.swift", names, n);
  ASSERT_NOT_NULL(r);
  ASSERT_EQ(count_undeclared(r), (uint32_t)0);

  msf_result_free(r);
  msf_vocab_free(v);
  PASS();
}

static void test_import_resolves_via_vocab(void) {
  TEST("vocab: `import` resolves module types via loaded vocabulary");
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  msf_vocab_add_interface(v, "Acme",
                          "public struct Widget {}\npublic protocol Drawable {}\n");

  const char *code = "import Acme\nfunc use(_ w: Widget, _ d: Drawable) {}\n";

  /* No vocabulary → the imported types are undeclared. */
  MSFResult *bare = msf_analyze(code, "u.swift");
  ASSERT_NOT_NULL(bare);
  ASSERT(count_undeclared(bare) >= 2);
  msf_result_free(bare);

  /* With the vocabulary, `import Acme` pulls Widget + Drawable. */
  MSFResult *r = msf_analyze_with_vocab(code, "u.swift", v);
  ASSERT_NOT_NULL(r);
  ASSERT_EQ(count_undeclared(r), (uint32_t)0);
  msf_result_free(r);

  /* An import the vocabulary doesn't know stays unresolved (no false positive). */
  MSFResult *r2 = msf_analyze_with_vocab("import Other\nlet x: Gadget? = nil\n",
                                         "o.swift", v);
  ASSERT_NOT_NULL(r2);
  ASSERT(count_undeclared(r2) >= 1);
  msf_result_free(r2);

  msf_vocab_free(v);
  PASS();
}

static void test_module_vocab(void) {
  TEST("vocab: msf_module_set_vocabulary resolves imports whole-module");
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  msf_vocab_add_interface(v, "Acme", "public struct Widget {}\n");

  MSFModule *m = msf_module_create();
  ASSERT_NOT_NULL(m);
  ASSERT_EQ(msf_module_set_vocabulary(m, v), 0);
  msf_module_add_file(m, "import Acme\nlet w: Widget? = nil\n", "a.swift");
  msf_module_analyze(m);

  uint32_t undeclared = 0;
  for (uint32_t i = 0; i < msf_module_error_count(m); i++)
    if (strstr(msf_module_error_message(m, i), "undeclared type")) undeclared++;
  ASSERT_EQ(undeclared, (uint32_t)0);

  msf_module_free(m);
  msf_vocab_free(v);
  PASS();
}

/* ── Edge cases ──────────────────────────────────────────────────────────── */

static void test_empty_and_null(void) {
  TEST("vocab: empty/NULL inputs are handled safely");
  ASSERT_EQ(msf_vocab_add_interface(NULL, "M", "public struct S {}"), (size_t)0);
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  ASSERT_EQ(msf_vocab_add_interface(v, "M", NULL), (size_t)0);
  ASSERT_EQ(msf_vocab_add_interface(v, "M", ""), (size_t)0);
  ASSERT_EQ(msf_vocab_module_count(v), (size_t)1); /* "M" created, empty */
  char *text = msf_vocab_serialize(v);
  ASSERT_NOT_NULL(text);
  free(text);
  msf_vocab_free(v);
  msf_vocab_free(NULL); /* must not crash */
  PASS();
}

/* ── Suite entry ─────────────────────────────────────────────────────────── */

/* ── Members (v2): per-type signatures ───────────────────────────────────── */

static int member_kind_of_sig(const MSFVocab *v, size_t m, size_t t,
                              const char *sig) {
  size_t n = msf_vocab_member_count(v, m, t);
  for (size_t i = 0; i < n; i++)
    if (strcmp(msf_vocab_member_signature(v, m, t, i), sig) == 0)
      return (int)msf_vocab_member_kind(v, m, t, i);
  return -1;
}

static void test_members_extract_and_roundtrip(void) {
  TEST("vocab: extracts member signatures + members survive round-trip");
  const char *iface =
      "public class View {\n"
      "  public func addSubview(_ v: View)\n"
      "  public var tag: Int { get set }\n"
      "  public init(frame: Rect)\n"
      "}\n"
      "public extension View {\n"          /* extension members fold onto View */
      "  func bringToFront()\n"
      "}\n";
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  msf_vocab_add_interface(v, "UI", iface);

  ASSERT_EQ(msf_vocab_type_count(v, 0), (size_t)1);          /* View (deduped) */
  ASSERT_EQ(msf_vocab_member_count(v, 0, 0), (size_t)4);
  ASSERT_EQ(member_kind_of_sig(v, 0, 0, "addSubview(_ v: View)"),
            (int)MSF_VOCAB_MEMBER_FUNC);
  ASSERT_EQ(member_kind_of_sig(v, 0, 0, "tag: Int"), (int)MSF_VOCAB_MEMBER_PROPERTY);
  ASSERT_EQ(member_kind_of_sig(v, 0, 0, "init(frame: Rect)"),
            (int)MSF_VOCAB_MEMBER_INIT);
  ASSERT_EQ(member_kind_of_sig(v, 0, 0, "bringToFront()"),
            (int)MSF_VOCAB_MEMBER_FUNC);

  char *text = msf_vocab_serialize(v);
  ASSERT_NOT_NULL(text);
  ASSERT(strstr(text, "msfvocab 3") != NULL);
  ASSERT(strstr(text, "\tf addSubview(_ v: View)") != NULL);
  ASSERT(strstr(text, "\tp tag: Int") != NULL);
  ASSERT(strstr(text, "\ti init(frame: Rect)") != NULL);

  MSFVocab *v2 = msf_vocab_parse(text);
  ASSERT_NOT_NULL(v2);
  ASSERT_EQ(msf_vocab_member_count(v2, 0, 0), (size_t)4);
  ASSERT_EQ(member_kind_of_sig(v2, 0, 0, "init(frame: Rect)"),
            (int)MSF_VOCAB_MEMBER_INIT);
  ASSERT_EQ(member_kind_of_sig(v2, 0, 0, "tag: Int"),
            (int)MSF_VOCAB_MEMBER_PROPERTY);

  free(text);
  msf_vocab_free(v);
  msf_vocab_free(v2);
  PASS();
}

static void test_dependency_graph_and_closure(void) {
  TEST("vocab: import edges extracted + transitive closure (deps survive round-trip)");
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  msf_vocab_add_interface(v, "A", "import B\npublic struct AX {}\n");
  msf_vocab_add_interface(v, "B", "import C\nimport Foundation\npublic struct BX {}\n");
  msf_vocab_add_interface(v, "C", "public struct CX {}\n");
  msf_vocab_add_interface(v, "D", "public struct DX {}\n"); /* isolated */

  ASSERT_EQ(msf_vocab_module_dep_count(v, 0), (size_t)1);   /* A imports B */
  ASSERT_STR_EQ(msf_vocab_module_dep(v, 0, 0), "B");
  ASSERT_EQ(msf_vocab_module_dep_count(v, 1), (size_t)2);   /* B imports C, Foundation */

  /* closure of {A} = {A, B, C}; Foundation is a dep but not in the vocab → skipped. */
  const char *seeds[] = { "A" };
  const char *out[16];
  size_t n = msf_vocab_import_closure(v, seeds, 1, out, 16);
  ASSERT_EQ(n, (size_t)3);
  int hasA = 0, hasB = 0, hasC = 0, hasD = 0, hasFnd = 0;
  for (size_t i = 0; i < n; i++) {
    if (!strcmp(out[i], "A")) hasA = 1;
    if (!strcmp(out[i], "B")) hasB = 1;
    if (!strcmp(out[i], "C")) hasC = 1;
    if (!strcmp(out[i], "D")) hasD = 1;
    if (!strcmp(out[i], "Foundation")) hasFnd = 1;
  }
  ASSERT(hasA && hasB && hasC && !hasD && !hasFnd);

  char *text = msf_vocab_serialize(v);
  ASSERT_NOT_NULL(text);
  ASSERT(strstr(text, "import B") != NULL);
  ASSERT(strstr(text, "import Foundation") != NULL);
  MSFVocab *v2 = msf_vocab_parse(text);
  ASSERT_NOT_NULL(v2);
  size_t n2 = msf_vocab_import_closure(v2, seeds, 1, out, 16);
  ASSERT_EQ(n2, (size_t)3);

  free(text);
  msf_vocab_free(v);
  msf_vocab_free(v2);
  PASS();
}

/* ── Member resolution in sema (v2 member db) ────────────────────────────── */

static void collect_member_types(const ASTNode *n, char buf[][64], int *cnt,
                                 int max) {
  if (!n) return;
  if ((n->kind == AST_MEMBER_EXPR || n->kind == AST_CALL_EXPR) && n->type &&
      *cnt < max)
    type_to_string(n->type, buf[(*cnt)++], 64);
  for (const ASTNode *c = n->first_child; c; c = c->next_sibling)
    collect_member_types(c, buf, cnt, max);
}

static void test_vocab_member_resolution(void) {
  TEST("vocab: SDK property member access resolves its type (incl. a chain)");
  const char *blob =
      "msfvocab 2\n"
      "module M 3\n"
      "C View\n"
      "\tp layer: Layer\n"
      "\tp tag: Int\n"
      "C Layer\n"
      "\tp bounds: Rect\n"
      "S Rect\n";
  MSFVocab *v = msf_vocab_parse(blob);
  ASSERT_NOT_NULL(v);
  /* v.layer must resolve to Layer (from the vocab), and .bounds on that must
   * re-enter the vocab to resolve to Rect — a two-step SDK member chain. */
  const char *code =
      "import M\n"
      "func f(v: View) -> Rect { return v.layer.bounds }\n";
  MSFResult *r = msf_analyze_with_vocab(code, "t.swift", v);
  ASSERT_NOT_NULL(r);
  const ASTNode *root = msf_root(r);
  ASSERT_NOT_NULL(root);
  char types[16][64];
  int n = 0;
  collect_member_types(root, types, &n, 16);
  int has_layer = 0, has_rect = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(types[i], "Layer") == 0) has_layer = 1;
    if (strcmp(types[i], "Rect") == 0) has_rect = 1;
  }
  ASSERT(has_layer); /* v.layer : Layer */
  ASSERT(has_rect);  /* v.layer.bounds : Rect */
  msf_result_free(r);
  msf_vocab_free(v);
  PASS();
}

static void test_vocab_method_call_resolution(void) {
  TEST("vocab: SDK method call resolves to its return type (and chains)");
  const char *blob =
      "msfvocab 2\n"
      "module M 3\n"
      "C View\n"
      "\tf makeLayer() -> Layer\n"
      "\tf noop()\n"               /* Void-returning: no top-level arrow */
      "C Layer\n"
      "\tp bounds: Rect\n"
      "S Rect\n";
  MSFVocab *v = msf_vocab_parse(blob);
  ASSERT_NOT_NULL(v);
  /* v.makeLayer() resolves to Layer (the func's return), and .bounds on the
   * result resolves to Rect — an SDK method call feeding a property chain. */
  const char *code =
      "import M\n"
      "func f(v: View) -> Rect { return v.makeLayer().bounds }\n";
  MSFResult *r = msf_analyze_with_vocab(code, "t.swift", v);
  ASSERT_NOT_NULL(r);
  char types[16][64];
  int n = 0;
  collect_member_types(msf_root(r), types, &n, 16);
  int has_layer = 0, has_rect = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(types[i], "Layer") == 0) has_layer = 1;
    if (strcmp(types[i], "Rect") == 0) has_rect = 1;
  }
  ASSERT(has_layer); /* v.makeLayer() : Layer */
  ASSERT(has_rect);  /* v.makeLayer().bounds : Rect */
  msf_result_free(r);
  msf_vocab_free(v);
  PASS();
}

static void test_vocab_property_vs_method_disambiguation(void) {
  TEST("vocab: plain member access prefers the property over a same-named method");
  /* `frame` exists as BOTH a method and a property (ObjC's `frame` /
   * `frame(forAlignmentRect:)`); a plain `v.frame` must pick the property. */
  const char *blob =
      "msfvocab 2\n"
      "module M 2\n"
      "C View\n"
      "\tf frame(forRect r: Rect) -> Rect\n" /* method listed first */
      "\tp frame: Rect\n"                     /* property */
      "S Rect\n";
  MSFVocab *v = msf_vocab_parse(blob);
  ASSERT_NOT_NULL(v);
  const char *code = "import M\nfunc f(v: View, r: Rect) { v.frame = r }\n";
  MSFResult *res = msf_analyze_with_vocab(code, "t.swift", v);
  ASSERT_NOT_NULL(res);
  char types[16][64];
  int n = 0, prop_ok = 0, func_seen = 0;
  collect_member_types(msf_root(res), types, &n, 16);
  for (int i = 0; i < n; i++) {
    if (strcmp(types[i], "Rect") == 0) prop_ok = 1;
    if (strstr(types[i], "->")) func_seen = 1;
  }
  ASSERT(prop_ok);    /* v.frame resolved as the Rect property */
  ASSERT(!func_seen); /* not as a `() -> Rect` method */
  msf_result_free(res);
  msf_vocab_free(v);
  PASS();
}

static void test_func_to_func_closure_assign_lenient(void) {
  TEST("closure assigned to an optional function-typed target does not mismatch");
  /* A closure literal's return inference is unreliable; assigning it to a
   * function-typed (here Optional) target must not raise a Type mismatch. */
  const char *code =
      "func g() {}\n"
      "var handler: (() -> Void)? = nil\n"
      "func f() { handler = { g() } }\n";
  MSFResult *r = msf_analyze(code, "t.swift");
  ASSERT_NOT_NULL(r);
  ASSERT_EQ(msf_error_count(r), (uint32_t)0);
  msf_result_free(r);
  PASS();
}

static void test_init_check_leniency(void) {
  TEST("init check: default params and an external superclass don't false-flag");
  const char *blob = "msfvocab 2\nmodule M 1\nC NSView\n";
  MSFVocab *v = msf_vocab_parse(blob);
  ASSERT_NOT_NULL(v);
  const char *code =
      "import M\n"
      "class Box { init(a: Int, b: Int = 0) {} }\n" /* default param */
      "class V: NSView { init(x: Int) {} }\n"        /* external superclass */
      "func f() {\n"
      "  let _ = Box(a: 1)\n"  /* argc 1, init has 2 (b defaulted) → OK */
      "  let _ = V()\n"        /* argc 0; NSView's inits are invisible → OK */
      "}\n";
  MSFResult *r = msf_analyze_with_vocab(code, "t.swift", v);
  ASSERT_NOT_NULL(r);
  int init_err = 0;
  for (uint32_t i = 0; i < msf_error_count(r); i++)
    if (strstr(msf_error_message(r, i), "no matching initializer")) init_err++;
  ASSERT_EQ(init_err, 0);
  msf_result_free(r);
  msf_vocab_free(v);
  PASS();
}

/* ── v3: per-type conformances ───────────────────────────────────────────── */

static void test_conformances_v3(void) {
  TEST("vocab: extracts + round-trips per-type conformances (v3)");
  MSFVocab *v = msf_vocab_create();
  ASSERT_NOT_NULL(v);
  msf_vocab_add_interface(
      v, "CoreGraphics",
      "public struct CGFloat : BinaryFloatingPoint, Hashable {}\n"
      "public struct Plain {}\n");

  const char *cg = vocab_confs_of(v, "CGFloat");
  ASSERT_NOT_NULL(cg);
  ASSERT(strstr(cg, "BinaryFloatingPoint") != NULL);
  ASSERT(strstr(cg, "Hashable") != NULL);
  ASSERT(vocab_confs_of(v, "Plain") == NULL); /* no conformance clause */

  /* serialize emits a ` : ` suffix; parse recovers the conformances. */
  char *text = msf_vocab_serialize(v);
  ASSERT_NOT_NULL(text);
  ASSERT(strstr(text, "S CGFloat : ") != NULL);
  MSFVocab *v2 = msf_vocab_parse(text);
  ASSERT_NOT_NULL(v2);
  const char *cg2 = vocab_confs_of(v2, "CGFloat");
  ASSERT_NOT_NULL(cg2);
  ASSERT(strstr(cg2, "BinaryFloatingPoint") != NULL);
  ASSERT(strstr(cg2, "Hashable") != NULL);
  ASSERT(vocab_confs_of(v2, "Plain") == NULL);

  free(text);
  msf_vocab_free(v);
  msf_vocab_free(v2);
  PASS();
}

void run_vocab_tests(void) {
  TEST_SUITE("Module Vocabulary");
  test_init_check_leniency();
  test_members_extract_and_roundtrip();
  test_dependency_graph_and_closure();
  test_vocab_member_resolution();
  test_vocab_method_call_resolution();
  test_vocab_property_vs_method_disambiguation();
  test_func_to_func_closure_assign_lenient();
  test_extract_public_types();
  test_extract_skips_non_public();
  test_kinds();
  test_roundtrip();
  test_conformances_v3();
  test_parse_multimodule();
  test_resolution_with_vocab();
  test_import_resolves_via_vocab();
  test_module_vocab();
  test_empty_and_null();
}
