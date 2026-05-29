# MiniSwiftFrontend — Standalone Swift Lexer, Parser & Semantic Analysis Library
#
# Produces: libMiniSwiftFrontend.a (static library)
#
# Targets:
#   make                  # debug build (native)
#   make release          # optimized build (native)
#   make wasm             # WebAssembly build (requires emcc)
#   make asan             # AddressSanitizer test build (native)
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
#   src/unicode   — msf's own Swift-tailored Unicode module (NFC) public headers
INCLUDES = -I$(INCDIR) -I$(GENDIR) -I$(SRCDIR) -I$(ROOT)src/unicode/include

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

# ── Unicode module (msf's own Swift-tailored Unicode layer) ────────────────────
# msf's NFC/NFD decoder: three handwritten files (src/decoder.c, src/internal.h,
# include/decoder.h) plus the generated Unicode tables (src/normalization.{c,h}).
DECODER_DIR  = $(ROOT)src/unicode
DECODER_SRCS = $(wildcard $(DECODER_DIR)/src/*.c)
DECODER_INC  = -I$(DECODER_DIR)/include -I$(DECODER_DIR)/src
# Generated table code: silence its zero-initializer warnings.
DECODER_CFLAGS = -std=c11 -D_GNU_SOURCE -Wno-missing-field-initializers

# ── Native build ─────────────────────────────────────────────────────────────
NATIVE_DIR  = $(BUILDDIR)/native
NATIVE_OBJS = $(patsubst $(SRCDIR)/%.c, $(NATIVE_DIR)/%.o, $(SRCS))
DECODER_NATIVE_OBJS = $(patsubst $(DECODER_DIR)/%.c, $(NATIVE_DIR)/decoder/%.o, $(DECODER_SRCS))
NATIVE_DEPS = $(NATIVE_OBJS:.o=.d)
NATIVE_LIB  = $(NATIVE_DIR)/libMiniSwiftFrontend.a

# ── WASM build ───────────────────────────────────────────────────────────────
WASM_DIR     = $(BUILDDIR)/wasm
WASM_CC      = emcc
WASM_AR      = emar
WASM_RANLIB  = emranlib
# Note: no -DWASM_BUILD — the project API (src/project.c) needs the real
# filesystem implementation.  Under Node the host FS is exposed via NODEFS; in a
# browser (no FS) opendir() simply fails and a project resolves to 0 modules.
# _GNU_SOURCE: musl/emscripten hides realpath() et al. under strict -std=c11.
# MSF_WEB_VOCAB: pick the trimmed playground vocab (sdk_vocab_web.h) — wasm only;
# the native build omits it and embeds the full sdk_vocab.h (see vocab.c).
WASM_CFLAGS  = -std=c11 -O2 -DNDEBUG -msimd128 -D_GNU_SOURCE -DMSF_WEB_VOCAB
WASM_OBJS    = $(patsubst $(SRCDIR)/%.c, $(WASM_DIR)/%.o, $(SRCS))
DECODER_WASM_OBJS = $(patsubst $(DECODER_DIR)/%.c, $(WASM_DIR)/decoder/%.o, $(DECODER_SRCS))
WASM_LIB     = $(WASM_DIR)/libMiniSwiftFrontend.a

# ── Targets ───────────────────────────────────────────────────────────────────
# ── Test build ───────────────────────────────────────────────────────────────
TESTDIR    = $(ROOT)tests
# Exclude standalone harnesses that provide their own main().
TEST_SRCS  = $(filter-out $(TESTDIR)/swift_corpus_main.c \
                          $(TESTDIR)/swift_scan_main.c \
                          $(TESTDIR)/analyze_one.c \
                          $(TESTDIR)/proto_iface.c \
                          $(TESTDIR)/msf_vocab.c \
                          $(TESTDIR)/msf_project.c, \
                          $(wildcard $(TESTDIR)/*.c))
TEST_BIN    = $(BUILDDIR)/test_runner
VOCAB_BIN   = $(BUILDDIR)/msf-vocab
PROJECT_BIN = $(BUILDDIR)/msf-project
ASAN_DIR    = $(BUILDDIR)/asan
ASAN_OBJS   = $(patsubst $(SRCDIR)/%.c, $(ASAN_DIR)/%.o, $(SRCS))
DECODER_ASAN_OBJS = $(patsubst $(DECODER_DIR)/%.c, $(ASAN_DIR)/decoder/%.o, $(DECODER_SRCS))
ASAN_DEPS   = $(ASAN_OBJS:.o=.d)
ASAN_LIB    = $(ASAN_DIR)/libMiniSwiftFrontend.a
ASAN_TEST_BIN = $(BUILDDIR)/test_runner_asan
ASAN_PROJECT_BIN = $(BUILDDIR)/msf-project-asan
ASAN_CFLAGS = $(CFLAGS) -g -O1 -fsanitize=address -fno-omit-frame-pointer

# ── Vocabulary tool ──────────────────────────────────────────────────────────
# Standalone CLI that parses .swiftinterface files with msf's own parser and
# emits a portable .msfvocab artifact.  stubs.c supplies module_stub_find (the
# tool never runs sema, but the linked library references it).

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

.PHONY: all debug release wasm asan asan-project-tool dist clean codegen test vocab-tool project-tool \
        sdk-vocab swift-corpus-fetch test-swift-corpus

all: debug

debug: CFLAGS += -g -O0
debug: $(NATIVE_LIB)

release: CFLAGS += -O2 -DNDEBUG
release: $(NATIVE_LIB)

wasm: $(WASM_LIB)

# ── Native compile + archive ─────────────────────────────────────────────────
$(NATIVE_LIB): $(NATIVE_OBJS) $(DECODER_NATIVE_OBJS)
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "AR" "$(notdir $@)"
	@$(AR) rcs $@ $^
	@$(RANLIB) $@
	@echo "  \xf0\x9f\x93\xa6 $(notdir $@) ($(words $(SRCS)) msf + $(words $(DECODER_SRCS)) decoder files)"

$(NATIVE_DIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "CC" "$(notdir $<)"
	@$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(NATIVE_DIR)/decoder/%.o: $(DECODER_DIR)/%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "CC" "decoder/$(notdir $<)"
	@$(CC) $(DECODER_CFLAGS) $(INCLUDES) $(DECODER_INC) -c $< -o $@

-include $(NATIVE_DEPS)

# ── WASM compile + archive ───────────────────────────────────────────────────
$(WASM_LIB): $(WASM_OBJS) $(DECODER_WASM_OBJS)
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "EMAR" "$(notdir $@)"
	@$(WASM_AR) rcs $@ $^
	@$(WASM_RANLIB) $@
	@echo "  \xf0\x9f\x93\xa6 $(notdir $@) [wasm] ($(words $(SRCS)) msf + $(words $(DECODER_SRCS)) decoder files)"

$(WASM_DIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "EMCC" "$(notdir $<)"
	@$(WASM_CC) $(WASM_CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(WASM_DIR)/decoder/%.o: $(DECODER_DIR)/%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "EMCC" "decoder/$(notdir $<)"
	@$(WASM_CC) $(WASM_CFLAGS) -D_GNU_SOURCE -Wno-missing-field-initializers $(INCLUDES) $(DECODER_INC) -c $< -o $@

# ── Dist: package headers + libraries for distribution ───────────────────────
dist: release
	@rm -rf $(DISTDIR)
	@mkdir -p $(DISTDIR)/include $(DISTDIR)/include/decoder $(DISTDIR)/lib
	@cp $(INCDIR)/msf.h $(DISTDIR)/include/
	@if [ -d "$(GENDIR)" ] && ls $(GENDIR)/*.h >/dev/null 2>&1; then \
		cp $(GENDIR)/*.h $(DISTDIR)/include/; \
	fi
	@cp $(DECODER_DIR)/include/*.h $(DISTDIR)/include/decoder/
	@cp $(NATIVE_LIB) $(DISTDIR)/lib/
	@if [ -f "$(WASM_LIB)" ]; then \
		mkdir -p $(DISTDIR)/lib/wasm; \
		cp $(WASM_LIB) $(DISTDIR)/lib/wasm/; \
	fi
	@echo "  \xf0\x9f\x93\xa6 dist/"
	@echo "     include/  (msf.h + decoder/ + $(words $(wildcard $(GENDIR)/*.h)) generated)"
	@echo "     lib/      libMiniSwiftFrontend.a (decoder vendored in)"

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

# ── ASan: sanitizer builds for nondeterministic whole-module crashes ─────────
$(ASAN_LIB): $(ASAN_OBJS) $(DECODER_ASAN_OBJS)
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "AR" "$(notdir $@)"
	@$(AR) rcs $@ $^
	@$(RANLIB) $@

$(ASAN_DIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "ASAN" "$(notdir $<)"
	@$(CC) $(ASAN_CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(ASAN_DIR)/decoder/%.o: $(DECODER_DIR)/%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "ASAN" "decoder/$(notdir $<)"
	@$(CC) $(DECODER_CFLAGS) -g -O1 -fsanitize=address -fno-omit-frame-pointer \
		$(INCLUDES) $(DECODER_INC) -c $< -o $@

# Header-dependency tracking for the ASan objects (mirrors -include NATIVE_DEPS).
# Without this, editing a shared header like private.h (which defines the size
# of SemaContext) does NOT recompile every TU, leaving object files with a
# mismatched struct layout — silent UB that ASan reports as a phantom
# heap-overflow / double-free until `make clean`.
-include $(ASAN_DEPS)

asan: $(ASAN_LIB)
	@printf "  %-7s %s\n" "LINK" "test_runner_asan"
	@$(CC) $(ASAN_CFLAGS) $(INCLUDES) -I$(TESTDIR) \
		-DSWIFT_FIXTURES_DIR=\"$(TESTDIR)/swift-fixtures\" \
		$(TEST_SRCS) \
		-L$(ASAN_DIR) -lMiniSwiftFrontend \
		-o $(ASAN_TEST_BIN)
	@ASAN_OPTIONS=detect_leaks=0 $(ASAN_TEST_BIN)

asan-project-tool: $(ASAN_PROJECT_BIN)

$(ASAN_PROJECT_BIN): $(ASAN_LIB) $(TESTDIR)/msf_project.c $(TESTDIR)/stubs.c
	@mkdir -p $(BUILDDIR)
	@printf "  %-7s %s\n" "LINK" "msf-project-asan"
	@$(CC) $(ASAN_CFLAGS) $(INCLUDES) \
		$(TESTDIR)/msf_project.c $(TESTDIR)/stubs.c \
		-L$(ASAN_DIR) -lMiniSwiftFrontend \
		-o $@

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

vocab-tool: $(VOCAB_BIN)

$(VOCAB_BIN): debug $(TESTDIR)/msf_vocab.c $(TESTDIR)/stubs.c
	@mkdir -p $(BUILDDIR)
	@printf "  %-7s %s\n" "LINK" "msf-vocab"
	@$(CC) $(CFLAGS) -g -O0 $(INCLUDES) \
		$(TESTDIR)/msf_vocab.c $(TESTDIR)/stubs.c \
		-L$(NATIVE_DIR) -lMiniSwiftFrontend \
		-o $@

project-tool: $(PROJECT_BIN)

$(PROJECT_BIN): debug $(TESTDIR)/msf_project.c $(TESTDIR)/stubs.c
	@mkdir -p $(BUILDDIR)
	@printf "  %-7s %s\n" "LINK" "msf-project"
	@$(CC) $(CFLAGS) -g -O0 $(INCLUDES) \
		$(TESTDIR)/msf_project.c $(TESTDIR)/stubs.c \
		-L$(NATIVE_DIR) -lMiniSwiftFrontend \
		-o $@

# Regenerate the portable Swift vocabulary artifact (type names + member
# signatures + import edges) by synthesizing every module's interface
# (swift-synthesize-interface) for each SDK and parsing it with msf-vocab.
# Several SDKs are unioned (iOS UIKit + macOS AppKit, shared Foundation merged).
# Run when the toolchain/SDK is updated; commit the regenerated header so any
# project loads it anywhere (no SDK / xcrun at analysis time).
#   SDKS=            override the SDK roots (default: iOS + macOS via xcrun)
#   SDK_IFACE_CACHE= dir caching synthesized .swift, reused across runs
SDKS ?= $(shell xcrun --sdk iphoneos --show-sdk-path 2>/dev/null) $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
SDK_IFACE_CACHE ?= $(BUILDDIR)/sdkiface
sdk-vocab: $(VOCAB_BIN)
	@mkdir -p $(GENDIR)
	@echo "  GEN     sdk_vocab.h"
	@python3 $(ROOT)scripts/gen_sdk_vocab.py \
		--vocab-bin $(VOCAB_BIN) \
		--out $(GENDIR)/sdk_vocab.h \
		--synth-cache $(SDK_IFACE_CACHE) \
		$(SDKS)
	@$(MAKE) --no-print-directory web-vocab

# Trimmed playground vocab for the wasm build (hybrid: all type names + members
# only for core modules).  Derived from sdk_vocab.h — no SDK needed.  The wasm
# build (-DMSF_WEB_VOCAB) embeds this; native embeds the full sdk_vocab.h.
.PHONY: web-vocab
web-vocab:
	@echo "  GEN     sdk_vocab_web.h"
	@python3 $(ROOT)scripts/gen_web_vocab.py

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
