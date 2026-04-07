# msf — Mini Swift Frontend

A single-file C library that takes Swift source code and produces a fully typed abstract syntax tree. No LLVM, no codegen, no runtime — just the frontend.

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

## Build

```bash
make              # debug build
make release      # optimized build (-O2)
make test         # run 280+ tests
make wasm         # WebAssembly build (requires emcc)
```

Produces `libMiniSwiftFrontend.a` — link against it and `#include <msf.h>`.

**Requirements:** C11 compiler (Clang, GCC, MSVC). No external dependencies.

**Platforms:** macOS, Linux, Windows, WebAssembly.

## API

### One-shot analysis

```c
MSFResult *r = msf_analyze(source_code, filename);
```

Does everything: tokenize, parse, type-check. Returns an opaque result you can query.

### Inspect the result

```c
const ASTNode  *root   = msf_root(r);           // AST root node
const Source   *src    = msf_source(r);          // source descriptor
const Token    *tokens = msf_tokens(r);          // token array
uint32_t        count  = msf_token_count(r);     // token count
```

### Check errors

```c
for (uint32_t i = 0; i < msf_error_count(r); i++)
    fprintf(stderr, "%u:%u: %s\n",
            msf_error_line(r, i),
            msf_error_col(r, i),
            msf_error_message(r, i));
```

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

### Cleanup

```c
msf_result_free(r);  // frees everything at once
```

## Project structure

```
include/
  msf.h                 Public API (the only header you include)

src/
  msf.c                 Pipeline entry point
  internal/             Module APIs (not public)
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

tests/                  Test suite (280+ assertions)
docs/                   Tutorial series (Turkish)
data/                   AST node definitions
```

## Design decisions

**Arena allocation** — AST nodes and TypeInfo values are allocated from chunk-based arenas. No per-node malloc/free. Everything is released at once via `msf_result_free()`.

**Zero-copy tokens** — Tokens store byte offset + length into the source. No string copies. The source must outlive the result.

**Pointer identity for builtins** — `TY_BUILTIN_INT`, `TY_BUILTIN_STRING`, etc. are singleton pointers. Type checks use `==` instead of `strcmp`.

**String interning** — All identifier strings are interned (FNV-1a hash + NFC normalization). Symbol lookup uses pointer equality.

**Table-driven dispatch** — Character classification (256-byte lookup), type resolution (function pointer table indexed by AST kind), builtin member lookup (~97 entries).

## License

MIT
