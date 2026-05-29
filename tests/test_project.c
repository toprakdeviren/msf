/**
 * @file test_project.c
 * @brief MSFProject API tests — generic discovery of a synthetic SwiftPM
 *        package + per-module whole-module analysis via the library.
 */
#include "test_framework.h"
#include "msf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT "/tmp/msf_project_test"

static void write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "wb");
  if (f) { fputs(content, f); fclose(f); }
}

/* Builds a minimal, convention-following SwiftPM package on disk:
 *   ROOT/Package.swift           (declares targets Alpha + Beta)
 *   ROOT/Sources/Alpha/a.swift   (declares struct Widget)
 *   ROOT/Sources/Beta/b.swift    (references Widget -> undeclared cross-module) */
static void make_fixture(void) {
  system("rm -rf " ROOT "; mkdir -p " ROOT "/Sources/Alpha " ROOT "/Sources/Beta");
  write_file(ROOT "/Package.swift",
             "// swift-tools-version:5.9\n"
             "import PackageDescription\n"
             "let package = Package(name: \"Demo\", targets: [\n"
             "  .target(name: \"Alpha\"),\n"
             "  .target(name: \"Beta\", dependencies: [\"Alpha\"]),\n"
             "])\n");
  write_file(ROOT "/Sources/Alpha/a.swift",
             "public struct Widget { public init() {} }\n"
             "public func make() -> Widget { Widget() }\n");
  write_file(ROOT "/Sources/Beta/b.swift",
             "import Alpha\n"                 /* the dependency edge */
             "func use(_ w: Widget) {}\n");   /* Widget is in Alpha → undeclared alone */
}

static int find_module(MSFProject *p, const char *name) {
  for (size_t i = 0; i < msf_project_module_count(p); i++)
    if (!strcmp(msf_project_module_name(p, i), name)) return (int)i;
  return -1;
}

static int module_has_file(MSFProject *p, size_t i, const char *needle) {
  for (size_t j = 0; j < msf_project_module_file_count(p, i); j++)
    if (strstr(msf_project_module_file(p, i, j), needle)) return 1;
  return 0;
}

static int has_external_import(MSFProject *p, const char *name) {
  for (size_t i = 0; i < msf_project_external_import_count(p); i++)
    if (!strcmp(msf_project_external_import(p, i), name)) return 1;
  return 0;
}

static int module_has_undeclared_type(MSFModule *m, const char *name) {
  for (uint32_t i = 0; i < msf_module_error_count(m); i++) {
    const char *msg = msf_module_error_message(m, i);
    if (strstr(msg, "undeclared type") && strstr(msg, name)) return 1;
  }
  return 0;
}

static void test_project_discovers_spm_targets(void) {
  TEST("project: discovers SwiftPM targets + their source files");
  make_fixture();
  MSFProject *p = msf_project_open(ROOT);
  ASSERT_NOT_NULL(p);
  ASSERT(msf_project_module_count(p) >= 2);

  int a = find_module(p, "Alpha"), b = find_module(p, "Beta");
  ASSERT(a >= 0 && b >= 0);
  ASSERT_STR_EQ(msf_project_module_kind(p, (size_t)a), "spm-target");
  ASSERT_EQ(msf_project_module_file_count(p, (size_t)a), (size_t)1);
  ASSERT(strstr(msf_project_module_file(p, (size_t)a, 0), "a.swift") != NULL);

  msf_project_free(p);
  PASS();
}

static void test_project_analyzes_each_module(void) {
  TEST("project: analyzes a module whole-module (Alpha clean, Beta cross-module)");
  make_fixture();
  MSFProject *p = msf_project_open(ROOT);
  ASSERT_NOT_NULL(p);

  int a = find_module(p, "Alpha"), b = find_module(p, "Beta");
  ASSERT(a >= 0 && b >= 0);

  /* Alpha is self-contained → no undeclared-type diagnostics. */
  MSFModule *ma = msf_project_analyze_module(p, (size_t)a);
  ASSERT_NOT_NULL(ma);
  uint32_t a_undecl = 0;
  for (uint32_t k = 0; k < msf_module_error_count(ma); k++)
    if (strstr(msf_module_error_message(ma, k), "undeclared type")) a_undecl++;
  ASSERT_EQ(a_undecl, (uint32_t)0);
  msf_module_free(ma);

  /* Beta references Alpha's Widget, analyzed independently → undeclared. */
  MSFModule *mb = msf_project_analyze_module(p, (size_t)b);
  ASSERT_NOT_NULL(mb);
  uint32_t b_undecl = 0;
  for (uint32_t k = 0; k < msf_module_error_count(mb); k++)
    if (strstr(msf_module_error_message(mb, k), "undeclared type")) b_undecl++;
  ASSERT(b_undecl >= 1);
  msf_module_free(mb);

  msf_project_free(p);
  system("rm -rf " ROOT);
  PASS();
}

static void test_project_resolves_cross_module(void) {
  TEST("project: dependency-ordered vocab chaining resolves cross-module refs");
  make_fixture();
  MSFProject *p = msf_project_open(ROOT);
  ASSERT_NOT_NULL(p);
  int a = find_module(p, "Alpha"), b = find_module(p, "Beta");
  ASSERT(a >= 0 && b >= 0);

  /* Beta should record Alpha as a dependency (from `import Alpha`). */
  int beta_dep_on_alpha = 0;
  for (size_t k = 0; k < msf_project_module_dep_count(p, (size_t)b); k++)
    if (msf_project_module_dep(p, (size_t)b, k) == (size_t)a) beta_dep_on_alpha = 1;
  ASSERT(beta_dep_on_alpha);

  /* Chain in dependency order: Alpha first publishes Widget into the shared
   * vocab, then Beta resolves `Widget` from it. */
  MSFVocab *shared = msf_vocab_new();
  ASSERT_NOT_NULL(shared);
  MSFModule *ma = msf_project_analyze_module_resolved(p, (size_t)a, shared);
  ASSERT_NOT_NULL(ma); msf_module_free(ma);
  MSFModule *mb = msf_project_analyze_module_resolved(p, (size_t)b, shared);
  ASSERT_NOT_NULL(mb);
  uint32_t b_undecl = 0;
  for (uint32_t k = 0; k < msf_module_error_count(mb); k++)
    if (strstr(msf_module_error_message(mb, k), "undeclared type")) b_undecl++;
  ASSERT_EQ(b_undecl, (uint32_t)0); /* Widget now resolves via the chained vocab */
  msf_module_free(mb);

  msf_vocab_free(shared);
  msf_project_free(p);
  system("rm -rf " ROOT);
  PASS();
}

static void test_project_import_scanner_uses_tokens(void) {
  TEST("project: import scanner handles import kinds and ignores strings/comments");
  system("rm -rf " ROOT "_imports; mkdir -p "
         ROOT "_imports/Sources/Alpha "
         ROOT "_imports/Sources/Beta");
  write_file(ROOT "_imports/Package.swift",
             "import PackageDescription\n"
             "let package = Package(name: \"Imports\", targets: [\n"
             "  .target(name: \"Alpha\"),\n"
             "  .target(name: \"Beta\"),\n"
             "])\n");
  write_file(ROOT "_imports/Sources/Alpha/a.swift", "public struct Widget {}\n");
  write_file(ROOT "_imports/Sources/Beta/b.swift",
             "import struct Alpha.Widget\n"
             "@_exported import Foundation\n"
             "// import Ghost\n"
             "let ignored = \"import Phantom\"\n");

  MSFProject *p = msf_project_open(ROOT "_imports");
  ASSERT_NOT_NULL(p);
  int alpha = find_module(p, "Alpha");
  int beta = find_module(p, "Beta");
  ASSERT(alpha >= 0 && beta >= 0);
  int beta_dep_on_alpha = 0;
  for (size_t k = 0; k < msf_project_module_dep_count(p, (size_t)beta); k++)
    if (msf_project_module_dep(p, (size_t)beta, k) == (size_t)alpha) beta_dep_on_alpha = 1;
  ASSERT(beta_dep_on_alpha);
  ASSERT(has_external_import(p, "Foundation"));
  ASSERT(!has_external_import(p, "struct"));
  ASSERT(!has_external_import(p, "Ghost"));
  ASSERT(!has_external_import(p, "Phantom"));

  msf_project_free(p);
  system("rm -rf " ROOT "_imports");
  PASS();
}

static void test_project_reexport_folding_uses_parsed_imports(void) {
  TEST("project: re-export folding uses parsed imports");
  system("rm -rf " ROOT "_reexports; mkdir -p "
         ROOT "_reexports/Sources/Alpha "
         ROOT "_reexports/Sources/Ghost "
         ROOT "_reexports/Sources/Umbrella "
         ROOT "_reexports/Sources/Client");
  write_file(ROOT "_reexports/Package.swift",
             "import PackageDescription\n"
             "let package = Package(name: \"Reexports\", targets: [\n"
             "  .target(name: \"Alpha\"),\n"
             "  .target(name: \"Ghost\"),\n"
             "  .target(name: \"Umbrella\"),\n"
             "  .target(name: \"Client\"),\n"
             "])\n");
  write_file(ROOT "_reexports/Sources/Alpha/a.swift",
             "public struct Widget { public init() {} }\n");
  write_file(ROOT "_reexports/Sources/Ghost/g.swift",
             "public struct Phantom { public init() {} }\n");
  write_file(ROOT "_reexports/Sources/Umbrella/u.swift",
             "@_exported import struct Alpha.Widget\n"
             "// @_exported import Ghost\n"
             "let fake = \"@_exported import Ghost\"\n");
  write_file(ROOT "_reexports/Sources/Client/c.swift",
             "import Umbrella\n"
             "func ok(_ w: Widget) {}\n"
             "func stillMissing(_ p: Phantom) {}\n");

  MSFProject *p = msf_project_open(ROOT "_reexports");
  ASSERT_NOT_NULL(p);
  int alpha = find_module(p, "Alpha");
  int ghost = find_module(p, "Ghost");
  int umbrella = find_module(p, "Umbrella");
  int client = find_module(p, "Client");
  ASSERT(alpha >= 0 && ghost >= 0 && umbrella >= 0 && client >= 0);

  MSFVocab *shared = msf_vocab_new();
  ASSERT_NOT_NULL(shared);
  MSFModule *ma = msf_project_analyze_module_resolved(p, (size_t)alpha, shared);
  ASSERT_NOT_NULL(ma); msf_module_free(ma);
  MSFModule *mg = msf_project_analyze_module_resolved(p, (size_t)ghost, shared);
  ASSERT_NOT_NULL(mg); msf_module_free(mg);
  MSFModule *mu = msf_project_analyze_module_resolved(p, (size_t)umbrella, shared);
  ASSERT_NOT_NULL(mu); msf_module_free(mu);
  MSFModule *mc = msf_project_analyze_module_resolved(p, (size_t)client, shared);
  ASSERT_NOT_NULL(mc);
  ASSERT(!module_has_undeclared_type(mc, "Widget"));
  ASSERT(module_has_undeclared_type(mc, "Phantom"));
  msf_module_free(mc);

  msf_vocab_free(shared);
  msf_project_free(p);
  system("rm -rf " ROOT "_reexports");
  PASS();
}

/* SwiftPM convention layout: a manifest with NO explicit `targets:` list gets
 * its targets from the directory structure.  Covers the three forms found in
 * the wild: (1) Sources/<Name>/ subdir, (2) direct Swift files in Sources -> one target
 * named after the package, (3) pre-Sources/ layout — a top-level dir named
 * after the package. */
static void test_project_convention_layouts(void) {
  TEST("project: convention-based discovery (no explicit targets)");

  /* (1) subdir layout */
  system("rm -rf " ROOT "_c1; mkdir -p " ROOT "_c1/Sources/Core");
  write_file(ROOT "_c1/Package.swift",
             "import PackageDescription\nlet package = Package(name: \"C1\")\n");
  write_file(ROOT "_c1/Sources/Core/x.swift", "public struct X {}\n");
  MSFProject *p1 = msf_project_open(ROOT "_c1");
  ASSERT_NOT_NULL(p1);
  ASSERT(find_module(p1, "Core") >= 0);
  msf_project_free(p1);
  system("rm -rf " ROOT "_c1");

  /* (2) flat layout -> single target named after the package */
  system("rm -rf " ROOT "_c2; mkdir -p " ROOT "_c2/Sources");
  write_file(ROOT "_c2/Package.swift",
             "import PackageDescription\nlet package = Package(name: \"Flatty\")\n");
  write_file(ROOT "_c2/Sources/x.swift", "public struct X {}\n");
  MSFProject *p2 = msf_project_open(ROOT "_c2");
  ASSERT_NOT_NULL(p2);
  ASSERT(find_module(p2, "Flatty") >= 0);
  msf_project_free(p2);
  system("rm -rf " ROOT "_c2");

  /* (3) pre-Sources/ layout -> a top-level dir named after the package */
  system("rm -rf " ROOT "_c3; mkdir -p " ROOT "_c3/Rooty");
  write_file(ROOT "_c3/Package.swift",
             "import PackageDescription\nlet package = Package(name: \"Rooty\")\n");
  write_file(ROOT "_c3/Rooty/x.swift", "public struct X {}\n");
  MSFProject *p3 = msf_project_open(ROOT "_c3");
  ASSERT_NOT_NULL(p3);
  ASSERT(find_module(p3, "Rooty") >= 0);
  msf_project_free(p3);
  system("rm -rf " ROOT "_c3");

  PASS();
}

static void test_project_spm_target_edge_cases(void) {
  TEST("project: SwiftPM target edge cases");

  system("rm -rf " ROOT "_spm_edges; mkdir -p "
         ROOT "_spm_edges/Sources/Text-Field "
         ROOT "_spm_edges/LegacyCore");
  write_file(ROOT "_spm_edges/Package.swift",
             "import PackageDescription\n"
             "let package = Package(name: \"Edges\", targets: [\n"
             "  .target(name: \"Text_Field\"),\n"
             "  Target(name: \"LegacyCore\"),\n"
             "])\n");
  write_file(ROOT "_spm_edges/Sources/Text-Field/x.swift", "public struct Hyphenated {}\n");
  write_file(ROOT "_spm_edges/LegacyCore/y.swift", "public struct Legacy {}\n");

  MSFProject *p = msf_project_open(ROOT "_spm_edges");
  ASSERT_NOT_NULL(p);
  int text = find_module(p, "Text_Field");
  int legacy = find_module(p, "LegacyCore");
  ASSERT(text >= 0 && legacy >= 0);
  ASSERT(module_has_file(p, (size_t)text, "Sources/Text-Field/x.swift"));
  ASSERT(module_has_file(p, (size_t)legacy, "LegacyCore/y.swift"));
  msf_project_free(p);
  system("rm -rf " ROOT "_spm_edges");

  PASS();
}

static void test_project_discovers_legacy_xcode_sources(void) {
  TEST("project: discovers legacy Xcode PBXSourcesBuildPhase files");
  system("rm -rf " ROOT "_xcode; mkdir -p "
         ROOT "_xcode/LegacyApp.xcodeproj "
         ROOT "_xcode/LegacyApp");
  write_file(ROOT "_xcode/LegacyApp/AppDelegate.swift",
             "public struct AppDelegate {}\n");
  write_file(ROOT "_xcode/LegacyApp/Model.swift",
             "public struct LegacyModel {}\n");
  write_file(ROOT "_xcode/LegacyApp.xcodeproj/project.pbxproj",
             "{\n"
             "  objects = {\n"
             "    AAAAAAAA0000000000000001 /* AppDelegate.swift in Sources */ = {isa = PBXBuildFile; fileRef = BBBBBBBB0000000000000001 /* AppDelegate.swift */; };\n"
             "    AAAAAAAA0000000000000002 /* Model.swift in Sources */ = {isa = PBXBuildFile; fileRef = BBBBBBBB0000000000000002 /* Model.swift */; };\n"
             "    BBBBBBBB0000000000000001 /* AppDelegate.swift */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = AppDelegate.swift; sourceTree = \"<group>\"; };\n"
             "    BBBBBBBB0000000000000002 /* Model.swift */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = Model.swift; sourceTree = \"<group>\"; };\n"
             "    CCCCCCCC0000000000000001 = {\n"
             "      isa = PBXGroup;\n"
             "      children = (\n"
             "        CCCCCCCC0000000000000002 /* LegacyApp */,\n"
             "      );\n"
             "      sourceTree = \"<group>\";\n"
             "    };\n"
             "    CCCCCCCC0000000000000002 /* LegacyApp */ = {\n"
             "      isa = PBXGroup;\n"
             "      children = (\n"
             "        BBBBBBBB0000000000000001 /* AppDelegate.swift */,\n"
             "        BBBBBBBB0000000000000002 /* Model.swift */,\n"
             "      );\n"
             "      path = LegacyApp;\n"
             "      sourceTree = \"<group>\";\n"
             "    };\n"
             "    DDDDDDDD0000000000000001 /* Sources */ = {\n"
             "      isa = PBXSourcesBuildPhase;\n"
             "      files = (\n"
             "        AAAAAAAA0000000000000001 /* AppDelegate.swift in Sources */,\n"
             "        AAAAAAAA0000000000000002 /* Model.swift in Sources */,\n"
             "      );\n"
             "    };\n"
             "    EEEEEEEE0000000000000001 /* LegacyApp */ = {\n"
             "      isa = PBXNativeTarget;\n"
             "      buildPhases = (\n"
             "        DDDDDDDD0000000000000001 /* Sources */,\n"
             "      );\n"
             "      name = LegacyApp;\n"
             "      productName = LegacyApp;\n"
             "    };\n"
             "  };\n"
             "}\n");

  MSFProject *p = msf_project_open(ROOT "_xcode");
  ASSERT_NOT_NULL(p);
  int app = find_module(p, "LegacyApp");
  ASSERT(app >= 0);
  ASSERT_STR_EQ(msf_project_module_kind(p, (size_t)app), "xcode-target");
  ASSERT_EQ(msf_project_module_file_count(p, (size_t)app), (size_t)2);
  ASSERT(module_has_file(p, (size_t)app, "LegacyApp/AppDelegate.swift"));
  ASSERT(module_has_file(p, (size_t)app, "LegacyApp/Model.swift"));
  msf_project_free(p);
  system("rm -rf " ROOT "_xcode");
  PASS();
}

static void test_project_symlink_cycle_no_explosion(void) {
  TEST("project: a directory symlink cycle does not multiply modules");
  system("rm -rf " ROOT "_loop; mkdir -p "
         ROOT "_loop/App.xcodeproj " ROOT "_loop/App");
  write_file(ROOT "_loop/App/Main.swift", "public struct Main {}\n");
  write_file(ROOT "_loop/App.xcodeproj/project.pbxproj",
             "{\n  objects = {\n"
             "    AAAAAAAA00000001 /* Main.swift in Sources */ = {isa = PBXBuildFile; fileRef = BBBBBBBB00000001 /* Main.swift */; };\n"
             "    BBBBBBBB00000001 /* Main.swift */ = {isa = PBXFileReference; path = Main.swift; sourceTree = \"<group>\"; };\n"
             "    CCCCCCCC00000001 /* App */ = {\n"
             "      isa = PBXGroup;\n"
             "      children = ( BBBBBBBB00000001 /* Main.swift */, );\n"
             "      path = App; sourceTree = \"<group>\";\n"
             "    };\n"
             "    DDDDDDDD00000001 /* Sources */ = {\n"
             "      isa = PBXSourcesBuildPhase;\n"
             "      files = ( AAAAAAAA00000001 /* Main.swift in Sources */, );\n"
             "    };\n"
             "    EEEEEEEE00000001 /* App */ = {\n"
             "      isa = PBXNativeTarget;\n"
             "      buildPhases = ( DDDDDDDD00000001 /* Sources */, );\n"
             "      name = App; productName = App;\n"
             "    };\n"
             "  };\n}\n");
  /* A subdirectory symlinked back to the project root is an infinite cycle for a
   * naive recursive walk: it re-finds App.xcodeproj at every level (each under a
   * different path → not de-duped), exploding one target into many.  The walk
   * must not follow the symlink. */
  system("ln -s " ROOT "_loop " ROOT "_loop/App/cycle");
  MSFProject *p = msf_project_open(ROOT "_loop");
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(msf_project_module_count(p), (size_t)1); /* exactly one, not N copies */
  msf_project_free(p);
  system("rm -rf " ROOT "_loop");
  PASS();
}

static void test_project_cross_file_result_builder(void) {
  TEST("project: cross-file @resultBuilder reads the builder's own tokens");
  system("rm -rf " ROOT "_rb; mkdir -p " ROOT "_rb/Sources/RB");
  write_file(ROOT "_rb/Package.swift",
             "import PackageDescription\n"
             "let package = Package(name: \"RB\", targets: [ .target(name: \"RB\") ])\n");
  /* The builder type lives in its own file, so its method-name tokens belong to
   * THIS file's stream — padded long so its `buildBlock` token index exceeds the
   * (short) using file's token count.  Reading it against the using file's
   * stream (the old bug) is then an out-of-bounds access. */
  write_file(ROOT "_rb/Sources/RB/Builder.swift",
             "// a long leading comment to push token indices higher and higher\n"
             "@resultBuilder enum WidePaddedBuilderTypeName {\n"
             "  static func buildBlock(_ parts: String...) -> String { \"\" }\n"
             "}\n");
  write_file(ROOT "_rb/Sources/RB/Use.swift",
             "func make(@WidePaddedBuilderTypeName _ b: () -> String) -> String { b() }\n");

  MSFProject *p = msf_project_open(ROOT "_rb");
  ASSERT_NOT_NULL(p);
  int m = find_module(p, "RB");
  ASSERT(m >= 0);
  MSFModule *mm = msf_project_analyze_module(p, (size_t)m); /* must not crash */
  ASSERT_NOT_NULL(mm);
  msf_module_free(mm);
  msf_project_free(p);
  system("rm -rf " ROOT "_rb");
  PASS();
}

static void test_project_empty_dir(void) {
  TEST("project: a directory with no Xcode/SwiftPM project yields 0 modules");
  system("rm -rf " ROOT "_empty; mkdir -p " ROOT "_empty");
  MSFProject *p = msf_project_open(ROOT "_empty");
  ASSERT_NOT_NULL(p);
  ASSERT_EQ(msf_project_module_count(p), (size_t)0);
  msf_project_free(p);
  msf_project_free(NULL); /* must not crash */
  system("rm -rf " ROOT "_empty");
  PASS();
}

void run_project_tests(void) {
  TEST_SUITE("Project (Xcode/SwiftPM)");
  test_project_discovers_spm_targets();
  test_project_analyzes_each_module();
  test_project_resolves_cross_module();
  test_project_import_scanner_uses_tokens();
  test_project_reexport_folding_uses_parsed_imports();
  test_project_convention_layouts();
  test_project_spm_target_edge_cases();
  test_project_discovers_legacy_xcode_sources();
  test_project_symlink_cycle_no_explosion();
  test_project_cross_file_result_builder();
  test_project_empty_dir();
}
