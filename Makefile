# MiniSwiftFrontend — Standalone Swift Lexer, Parser & Semantic Analysis Library
#
# Produces: libMiniSwiftFrontend.a (static library)
#
# Targets:
#   make                  # debug build (native)
#   make release          # optimized build (native)
#   make wasm             # WebAssembly build (requires emcc)
#   make dist             # copy headers + libs to dist/
#   make codegen          # regenerate .h files from data/
#   make clean
#
# Cross-compile:
#   CC=gcc make                              # GCC
#   CC=x86_64-linux-gnu-gcc make             # Linux cross-compile
#
# Usage (as subproject):
#   Parent Makefile calls: $(MAKE) -C lib/MiniSwiftFrontend
#   Then links with: -Llib/MiniSwiftFrontend/build/native -lMiniSwiftFrontend
#   And includes:    -Ilib/MiniSwiftFrontend/include -Ilib/MiniSwiftFrontend/generated

CC      ?= clang
AR      ?= ar
RANLIB  ?= ranlib
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic

# Directories
ROOT    := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
INCDIR   = $(ROOT)include
GENDIR   = $(ROOT)generated
SRCDIR   = $(ROOT)src
BUILDDIR = $(ROOT)build
DISTDIR  = $(ROOT)dist

# Include paths
#   INCDIR        — public headers (include/)
#   GENDIR        — generated .h files (generated/)
#   SRCDIR        — internal headers (src/internal/*.h)
#   libs/         — libunicode (decoder) headers
INCLUDES = -I$(INCDIR) -I$(GENDIR) -I$(SRCDIR) -I$(ROOT)libs/include

# Source files
SRCS = $(wildcard $(SRCDIR)/*.c) \
       $(wildcard $(SRCDIR)/ast/*.c) \
       $(wildcard $(SRCDIR)/lexer/*.c) \
       $(wildcard $(SRCDIR)/lexer/scan/*.c) \
       $(wildcard $(SRCDIR)/type/*.c) \
       $(wildcard $(SRCDIR)/parser/*.c) \
       $(wildcard $(SRCDIR)/parser/decl/*.c) \
       $(wildcard $(SRCDIR)/parser/expression/*.c) \
       $(wildcard $(SRCDIR)/semantic/*.c) \
       $(wildcard $(SRCDIR)/semantic/resolve/*.c) \
       $(wildcard $(SRCDIR)/semantic/resolve/expression/*.c)

# ── Native build ─────────────────────────────────────────────────────────────
NATIVE_DIR  = $(BUILDDIR)/native
NATIVE_OBJS = $(patsubst $(SRCDIR)/%.c, $(NATIVE_DIR)/%.o, $(SRCS))
NATIVE_DEPS = $(NATIVE_OBJS:.o=.d)
NATIVE_LIB  = $(NATIVE_DIR)/libMiniSwiftFrontend.a

# ── WASM build ───────────────────────────────────────────────────────────────
WASM_DIR     = $(BUILDDIR)/wasm
WASM_CC      = emcc
WASM_AR      = emar
WASM_RANLIB  = emranlib
WASM_CFLAGS  = -std=c11 -O2 -DNDEBUG -DWASM_BUILD
WASM_OBJS    = $(patsubst $(SRCDIR)/%.c, $(WASM_DIR)/%.o, $(SRCS))
WASM_LIB     = $(WASM_DIR)/libMiniSwiftFrontend.a

# ── Targets ───────────────────────────────────────────────────────────────────
# ── Test build ───────────────────────────────────────────────────────────────
TESTDIR    = $(ROOT)tests
# Exclude swift_corpus_main.c — it has its own main() and is built into a
# separate `swift_corpus_runner` binary by the test-swift-corpus target.
TEST_SRCS  = $(filter-out $(TESTDIR)/swift_corpus_main.c, $(wildcard $(TESTDIR)/*.c))
TEST_BIN   = $(BUILDDIR)/test_runner

# ── Swift corpus runner (Option A: apple/swift test/Parse parity) ────────────
# Sparse-clones apple/swift's test directories under build/swift-corpus/ and
# walks them with a standalone runner.  Mirrors the libwgsl `make test-cts`
# pattern: exploratory by default, --strict to gate the build.
SWIFT_CORPUS_REPO ?= https://github.com/apple/swift
SWIFT_CORPUS_REF  ?= main
SWIFT_CORPUS_DIR  ?= $(BUILDDIR)/swift-corpus
# Subdirectories under apple/swift/test/ that exercise the parser + sema
# layers msf actually implements.  SILGen / IRGen / runtime / Demangle etc.
# test backend code and would produce mostly noise.  Override with
# `SWIFT_CORPUS_SUBS="test/Parse test/Sema"` for a narrower run.
SWIFT_CORPUS_SUBS ?= test/Parse test/Sema test/decl test/expr test/stmt test/type test/Constraints test/Generics
# Walk one level above the per-sub roots; the runner recurses.
SWIFT_CORPUS_WALK ?= $(SWIFT_CORPUS_DIR)/test
SWIFT_CORPUS_ARGS ?= --max-failures 30 --quiet
SWIFT_CORPUS_RUNNER = $(BUILDDIR)/swift_corpus_runner

.PHONY: all debug release wasm dist clean codegen test \
        swift-corpus-fetch test-swift-corpus

all: debug

debug: CFLAGS += -g -O0
debug: $(NATIVE_LIB)

release: CFLAGS += -O2 -DNDEBUG
release: $(NATIVE_LIB)

wasm: $(WASM_LIB)

# ── Native compile + archive ─────────────────────────────────────────────────
$(NATIVE_LIB): $(NATIVE_OBJS)
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "AR" "$(notdir $@)"
	@$(AR) rcs $@ $^
	@$(RANLIB) $@
	@echo "  \xf0\x9f\x93\xa6 $(notdir $@) ($(words $(SRCS)) files)"

$(NATIVE_DIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "CC" "$(notdir $<)"
	@$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(NATIVE_DEPS)

# ── WASM compile + archive ───────────────────────────────────────────────────
$(WASM_LIB): $(WASM_OBJS)
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "EMAR" "$(notdir $@)"
	@$(WASM_AR) rcs $@ $^
	@$(WASM_RANLIB) $@
	@echo "  \xf0\x9f\x93\xa6 $(notdir $@) [wasm] ($(words $(SRCS)) files)"

$(WASM_DIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "EMCC" "$(notdir $<)"
	@$(WASM_CC) $(WASM_CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# ── Dist: package headers + libraries for distribution ───────────────────────
dist: release
	@rm -rf $(DISTDIR)
	@mkdir -p $(DISTDIR)/include $(DISTDIR)/include/decoder $(DISTDIR)/lib
	@cp $(INCDIR)/msf.h $(DISTDIR)/include/
	@if [ -d "$(GENDIR)" ] && ls $(GENDIR)/*.h >/dev/null 2>&1; then \
		cp $(GENDIR)/*.h $(DISTDIR)/include/; \
	fi
	@cp $(ROOT)libs/include/*.h $(DISTDIR)/include/decoder/
	@cp $(NATIVE_LIB) $(DISTDIR)/lib/
	@cp $(ROOT)libs/libunicode.a $(DISTDIR)/lib/ 2>/dev/null || true
	@if [ -f "$(WASM_LIB)" ]; then \
		mkdir -p $(DISTDIR)/lib/wasm; \
		cp $(WASM_LIB) $(DISTDIR)/lib/wasm/; \
	fi
	@echo "  \xf0\x9f\x93\xa6 dist/"
	@echo "     include/  (msf.h + decoder/ + $(words $(wildcard $(GENDIR)/*.h)) generated)"
	@echo "     lib/      libMiniSwiftFrontend.a + libunicode.a"

# ── Test: build and run unit tests ────────────────────────────────────────────
# SWIFT_FIXTURES_DIR is baked in as an absolute path so the runner finds
# tests/swift-fixtures/*.swift regardless of cwd at invocation time.
test: debug
	@printf "  %-7s %s\n" "LINK" "test_runner"
	@$(CC) $(CFLAGS) -g -O0 $(INCLUDES) -I$(TESTDIR) \
		-DSWIFT_FIXTURES_DIR=\"$(TESTDIR)/swift-fixtures\" \
		$(TEST_SRCS) \
		-L$(NATIVE_DIR) -lMiniSwiftFrontend \
		-o $(TEST_BIN)
	@$(TEST_BIN)

# ── Swift corpus: sparse-clone + run ─────────────────────────────────────────
# Sparse-clone only the parser/sema-relevant subdirs of apple/swift's test
# tree.  The full repo is ~3 GB; with --filter=blob:none + sparse-checkout
# we end up with a few hundred MB on disk.  `SWIFT_CORPUS_SUBS` is a
# space-separated list of paths under the repo root.
swift-corpus-fetch:
	@if [ ! -d "$(SWIFT_CORPUS_DIR)/.git" ]; then \
	    printf "  %-7s %s\n" "GIT" "$(SWIFT_CORPUS_REPO) -> $(SWIFT_CORPUS_DIR)"; \
	    git clone --depth=1 --filter=blob:none --sparse --branch "$(SWIFT_CORPUS_REF)" \
	        "$(SWIFT_CORPUS_REPO)" "$(SWIFT_CORPUS_DIR)" >/dev/null 2>&1 || { \
	        echo "  clone failed"; exit 2; }; \
	fi
	@cd "$(SWIFT_CORPUS_DIR)" && \
	    patterns=""; for sub in $(SWIFT_CORPUS_SUBS); do \
	        patterns="$$patterns /$$sub/**"; \
	    done; \
	    git sparse-checkout set --no-cone $$patterns >/dev/null
	@echo "  corpus walk root: $(SWIFT_CORPUS_WALK)"
	@echo "  total .swift files: $$(find $(SWIFT_CORPUS_WALK) -name '*.swift' 2>/dev/null | wc -l | tr -d ' ')"

$(SWIFT_CORPUS_RUNNER): debug $(TESTDIR)/swift_corpus_main.c $(TESTDIR)/stubs.c
	@mkdir -p $(BUILDDIR)
	@printf "  %-7s %s\n" "LINK" "swift_corpus_runner"
	@$(CC) $(CFLAGS) -g -O0 $(INCLUDES) -I$(TESTDIR) \
		$(TESTDIR)/swift_corpus_main.c $(TESTDIR)/stubs.c \
		-L$(NATIVE_DIR) -lMiniSwiftFrontend \
		-o $@

test-swift-corpus: $(SWIFT_CORPUS_RUNNER) swift-corpus-fetch
	@$(SWIFT_CORPUS_RUNNER) \
	    --corpus "$(SWIFT_CORPUS_WALK)" \
	    --report "$(BUILDDIR)/swift-corpus-report.json" \
	    $(SWIFT_CORPUS_ARGS)

clean:
	rm -rf $(BUILDDIR) $(DISTDIR)

# ── Code Generation ──────────────────────────────────────────────────────────
# Regenerate .h files from source-of-truth data files.
# Run this after editing data/ast_nodes.def or scripts/types.yaml.

codegen:
	@echo "  GEN     ast_kinds.h + ast_names.h"
	@python3 $(ROOT)scripts/gen_ast_names.py \
		--input $(ROOT)data/ast_nodes.def \
		--kinds $(GENDIR)/ast_kinds.h \
		--names $(GENDIR)/ast_names.h
	@echo "  GEN     sw_unicode.h"
	@python3 $(ROOT)scripts/gen_unicode_tables.py \
		--output $(GENDIR)/sw_unicode.h
	@echo "  GEN     type_kinds.h + type_str.h + type_builtins.h"
	@python3 $(ROOT)scripts/codegen.py types \
		--config $(ROOT)scripts/types.yaml \
		--outdir $(GENDIR)
	@echo "  GEN     sw_tables.h + map_kw_id.h"
	@python3 $(ROOT)scripts/codegen.py tables \
		--config $(ROOT)scripts/lexer.yaml \
		--token-header $(INCDIR)/token.h \
		--outdir $(GENDIR)
