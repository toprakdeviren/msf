# msf — Mini Swift Frontend

A single-header C library that takes Swift source code and produces a fully typed abstract syntax tree. No LLVM, no codegen, no runtime — just the frontend.

```c
#include <msf.h>

MSFResult *r = msf_analyze("let x: Int = 1 + 2", "main.swift");

const ASTNode *root = msf_root(r);
for (const ASTNode *c = root->first_child; c; c = c->next_sibling)
    printf("%s\n", ast_kind_name(c->kind));

msf_dump_json(r, stdout);
msf_result_free(r);
```

## What it does

msf implements the first three stages of a Swift compiler:

```
Source code
     |
     v
 +--------+     +--------+     +--------+
 | Lexer  | --> | Parser | --> |  Sema  |
 | tokens |     |  AST   |     | types  |
 +--------+     +--------+     +--------+
                                    |
                                    v
                              Typed AST
```

**Lexer** — Tokenizes Swift source into keywords, identifiers, literals, operators. SWAR fast-path for ASCII identifiers, memchr-based SIMD string scanning, FNV-1a keyword detection via binary search.

**Parser** — Recursive descent with Pratt precedence climbing for expressions. Produces an immutable AST. Handles the full Swift grammar: generics, closures, pattern matching, custom operators, `async`/`await`, property wrappers.

**Sema** — Three-pass semantic analysis:
1. **Declare** — Forward-registers all symbols (enables mutual references)
2. **Resolve** — Bottom-up type inference with overload resolution
3. **Conform** — Protocol conformance checking with conditional conformance support

The output is a typed AST where every node has a resolved `TypeInfo*`. You can walk it, serialize it (text / JSON / S-expression), or feed it to your own backend.

Single-file analysis is just the entry point. msf also:

- **Analyzes a whole module** — many files compiled as one unit, so a type declared in one file resolves from its siblings (`MSFModule`).
- **Resolves cross-module imports with no SDK present** — a module's public type surface is extracted from its `.swiftinterface` into a compact, portable `.msfvocab` that loads anywhere, including the browser (WASM) and Windows (`MSFVocab`).
- **Discovers a project's module graph** — points it at an Xcode or SwiftPM directory and it finds the targets, their source files, and their dependency order (`MSFProject`).

## Build

```bash
make              # debug build
make release      # optimized build (-O2)
make test         # run the test suite (300+ assertions)
make wasm         # WebAssembly build (requires emcc)
```

Produces `libMiniSwiftFrontend.a` — link against it and `#include <msf.h>`.

**Requirements:** C11 compiler (Clang, GCC, MSVC). No external dependencies.

**Platforms:** macOS, Linux, WebAssembly. Core analysis (`msf_analyze`, `MSFModule`, `MSFVocab`) also builds on Windows; project discovery (`MSFProject`) is POSIX-only (uses `dirent.h`).

## API

`msf.h` is the only header you include. It is organized in numbered sections: 1–8 cover everyday use (analyze, read, errors, dump); the lower half (9–16, *Backend ABI*) exposes the runtime shapes a compiler backend needs. Read-only consumers (editors, linters, pretty-printers) only need 1–8.

### One-shot analysis

```c
MSFResult *r = msf_analyze(source_code, filename);
```

Does everything: tokenize, parse, type-check. Returns an opaque result you can query. The result owns its own copy of the source, so you may free your buffer immediately. Two variants predeclare names that live outside the file:

```c
// Names known to be in scope (sibling files, an SDK's .swiftinterface, ...)
MSFResult *r = msf_analyze_in_module(code, "View.swift", type_names, n);

// Resolve the file's `import X` against a loaded vocabulary (see below)
MSFResult *r = msf_analyze_with_vocab(code, "View.swift", vocab);
```

### Inspect the result

```c
const ASTNode  *root   = msf_root(r);           // AST root node
const Source   *src    = msf_source(r);          // source descriptor
const Token    *tokens = msf_tokens(r);          // token array
size_t          count  = msf_token_count(r);     // token count
```

### Check errors

```c
for (uint32_t i = 0; i < msf_error_count(r); i++)
    fprintf(stderr, "%u:%u: %s\n",
            msf_error_line(r, i),
            msf_error_col(r, i),
            msf_error_message(r, i));
```

`msf_error_start_offset()` / `msf_error_end_offset()` give the `[start, end)` byte range for LSP-style highlighting. Analysis never fails silently: a best-effort AST is produced even when errors exist.

### Serialize the AST

```c
msf_dump_text(r, stdout);    // indented plain text
msf_dump_json(r, stdout);    // JSON (editors, web UI)
msf_dump_sexpr(r, stdout);   // S-expression (testing, diffing)
```

### Read type information

```c
char buf[64];
const ASTNode *node = root->first_child;
printf("type: %s\n", type_to_string(node->type, buf, sizeof(buf)));
// "type: Int"
```

Use `type_kind_of(node->type)` to switch over a canonical `TypeKind`; builtin types (`Int`, `String`, ...) are singleton pointers you can also compare with `==` (e.g. `node->type == TY_BUILTIN_INT`). `type_equal()` / `type_equal_deep()` compare two types, and convenience predicates — `type_is_named()`, `type_is_any()`, `type_is_anyobject()`, `type_is_never()` — cover common checks.

### Cleanup

```c
msf_result_free(r);  // frees everything at once
```

### Lexing on its own (optional)

Need tokens without a full analysis? The lexer is a standalone stage:

```c
Source src = { code, strlen(code), "main.swift" };
TokenStream ts;
token_stream_init(&ts, 0);
lexer_tokenize(&src, &ts, /*skip_ws=*/1, NULL);
for (size_t i = 0; i < ts.count; i++)
    printf("%s: %s\n", token_type_name(ts.tokens[i].type),
           token_text(&src, &ts.tokens[i]));
token_stream_free(&ts);
```

### Whole-module analysis

A Swift module is a set of files compiled together — a type declared in one file is visible to its siblings. `MSFModule` analyzes them as a unit: all files are parsed, their declarations collected into one shared symbol table, then each file is resolved against it. No text concatenation; each file keeps its own source and tokens.

```c
MSFModule *m = msf_module_new();
msf_module_add_file(m, codeA, "A.swift");
msf_module_add_file(m, codeB, "B.swift");
msf_module_analyze(m);

for (uint32_t i = 0; i < msf_module_error_count(m); i++)
    fprintf(stderr, "%s:%u:%u: %s\n",
            msf_module_error_file(m, i), msf_module_error_line(m, i),
            msf_module_error_col(m, i),  msf_module_error_message(m, i));

msf_module_free(m);
```

After analysis, each file's typed AST is available individually (`msf_module_file_root` / `_source` / `_tokens`) so a backend can lower every file into one shared output. `msf_module_set_vocabulary()` resolves the module's `import`s against a vocabulary (below).

### Module vocabulary — resolve imports with no SDK

A *vocabulary* is the set of public type names a module exports (`import SwiftUI` → `View`, `Text`, ...). msf extracts it by parsing a module's textual `.swiftinterface` with its own parser, then serializes it to a portable `.msfvocab` text format. This decouples type resolution from the host SDK: generate once on a machine that has the SDK, then load the artifact anywhere — browser (WASM), Windows — where no SDK or `xcrun` exists.

```c
// Generate (on a machine with the SDK):
MSFVocab *v = msf_vocab_new();
msf_vocab_add_interface(v, "SwiftUI", swiftui_interface_src);
char *text = msf_vocab_serialize(v);          // write to SwiftUI.msfvocab

// Load anywhere and resolve against it:
MSFVocab *loaded = msf_vocab_parse(text);
MSFResult *r = msf_analyze_with_vocab(code, "View.swift", loaded);
```

`msf_vocab_builtin()` returns the SDK vocabulary baked into the library at build time (`make sdk-vocab`). The vocabulary also records per-type members (`msf_vocab_find_member`), protocol conformances, and the inter-module dependency graph (`msf_vocab_import_closure`).

### Project discovery — Xcode / SwiftPM

Point msf at a project directory and it discovers the module graph — one module per Xcode target / SwiftPM target — each with its Swift source files and dependency order. Discovery is generic: it reads only the project's own metadata (synchronized-folder Xcode targets, `.target`/`.executableTarget`/`.testTarget` in `Package.swift`), with no project special-cased.

```c
MSFProject *proj = msf_project_open("/path/to/MyApp");
for (size_t i = 0; i < msf_project_module_count(proj); i++) {
    MSFModule *m = msf_project_analyze_module(proj, i);   // whole-module
    printf("%s: %u diagnostics\n",
           msf_project_module_name(proj, i), msf_module_error_count(m));
    msf_module_free(m);
}
msf_project_free(proj);
```

`msf_project_compile_order()` returns dependency order, and `msf_project_analyze_module_resolved()` chains a shared vocabulary across modules so cross-module references resolve. Helpers harvest type names from bundled `.xcframework`s, CocoaPods/Carthage dependencies, and the project's own ObjC headers. (Filesystem-backed, so a native-host feature — the WASM build returns an empty project.)

### Backend ABI (sections 9–16)

For compiler backends that lower the typed AST into code, the lower half of `msf.h` exposes the runtime shapes `msf_analyze()` writes: the `ASTNode.modifiers` bitmask (`MOD_*`), the `TypeArena` allocator, generic `where`-clause constraints, generic substitution (`type_substitute`), the conformance table (`ConformanceTable`), and associated-type bindings. A related helper, `msf_parse_expression()`, re-parses a bare expression string (e.g. a `\( … )` string-interpolation segment) against an analyzed result, so a backend lowering interpolations needs no parser of its own. Read-only consumers can ignore all of this.

## Project structure

```
include/
  msf.h                 Public API (the only header you include)

src/
  msf.c                 Pipeline entry point + whole-module (MSFModule)
  vocab.c               Module vocabulary (.swiftinterface → .msfvocab)
  project.c             Xcode / SwiftPM project discovery
  internal/             Module APIs (not public)
    msf.h               Cross-module internal declarations
    ast.h               AST arena, modifiers, serialization
    lexer.h             Tokenization, diagnostics
    type.h              Type arena, constraints, substitution
    sema.h              Semantic analysis lifecycle
    limits.h            Compile-time constants
    builtin_names.h     Swift type/protocol name constants

  lexer/                Lexer module
    lexer.c             Core dispatch loop
    token.c             Token utilities, stream management
    helpers.c           Multi-char operator table, string token helpers
    diag.c              Diagnostic recording
    private.h           Lexer-internal declarations
    char_tables.h       Character classification + keyword tables
    unicode_ranges.h    Unicode identifier/operator range tables
    scan/
      comment.c         Line and block comment scanners
      string.c          String literal scanners (regular, triple, raw)
      symbol.c          Operator, regex, punctuation dispatch
      fast.c            SWAR identifier scan, number scan, string body scan

  parser/               Parser module
    core.c              Token navigation, node allocation, modifiers
    top.c               Top-level dispatch (parse_decl_stmt)
    stmt.c              Statement parsers (if, for, switch, ...)
    type.c              Type expression parsing
    pattern.c           Pattern matching
    private.h           Parser-internal declarations
    decl/
      decl.c            Block, import, typealias, enum, nominal types
      func.c            func, init, deinit, subscript
      var.c             var/let, computed properties, observers
      operator.c        Operator and precedence group declarations
    expression/
      pratt.c           Pratt precedence climbing
      prefix.c          Literals, identifiers, collections, closures
      postfix.c         Calls, member access, subscript, optional chain
      pre.c             Precedence table, custom operator lookup
      closure.c         Closure body and capture list

  ast/                  AST module
    ast.c               Arena allocator, tree ops, kind names
    ast_dump.c          Text, JSON, S-expression serialization

  type/                 Type module
    type.c              Type arena, builtin singletons
    equal.c             Structural type equality
    str.c               Type-to-string conversion
    sub.c               Generic type substitution

  semantic/             Semantic analysis module
    core.c              Intern pool, symbol table, scope management
    declare.c           Forward declaration pass
    type_resolution.c   AST type node resolution
    conformance.c       Builtin member lookup table
    conformance_table.c Protocol conformance tracking
    generics.c          Generic constraint checking
    member_index.c      Per-type member index (vocab-backed lookup)
    builder.c           @resultBuilder transformation
    private.h           Sema-internal declarations
    module_stubs.h      SDK module type stubs
    resolve/
      resolver.c        Top-level node dispatch, sema_analyze
      declaration.c     Declaration type resolution
      access.c          Access control
      protocol.c        Protocol requirement helpers
      expression/
        dispatch.c      Expression case dispatcher
        binary.c        Binary operator resolution
        call.c          Call expression, overload resolution
        member.c        Member access, implicit members
        helpers.c       Shared expression helpers

  unicode/              Vendored Unicode library (NFC normalization)
    include/decoder.h   UTF-8 decode + normalization API
    src/                Decoder + generated normalization tables

generated/              Codegen output (committed): AST/type kind tables,
                        keyword map, baked-in SDK vocabulary (.h)
tests/                  Test suite (300+ assertions)
docs/                   Tutorial series (English + Turkish)
data/                   AST node definitions
```

## Design decisions

**Arena allocation** — AST nodes and TypeInfo values are allocated from chunk-based arenas. No per-node malloc/free. Everything is released at once via `msf_result_free()`.

**Zero-copy tokens** — Tokens store byte offset + length into the source. No string copies. The source must outlive the result.

**Pointer identity for builtins** — `TY_BUILTIN_INT`, `TY_BUILTIN_STRING`, etc. are singleton pointers. Type checks use `==` instead of `strcmp`.

**String interning** — All identifier strings are interned (FNV-1a hash + NFC normalization). Symbol lookup uses pointer equality.

**Table-driven dispatch** — Character classification (256-byte lookup), type resolution (function pointer table indexed by AST kind), builtin member lookup.

**SDK-free resolution** — Cross-module and SDK type resolution runs off a portable vocabulary, so analysis works with no toolchain installed — on any OS, and in the browser.

## License

MIT
