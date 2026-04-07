# How the Parser Works

> This document explains the msf parser layer from scratch.
> Previous chapters: [01 -- Lexer](01-lexer.md), [02 -- AST](02-ast.md)

## What Does the Parser Do?

The lexer gave us a flat token list:

```
KEYWORD "let"  IDENTIFIER "x"  OPERATOR "="  INTEGER "1"  OPERATOR "+"  INTEGER "2"  EOF
```

The parser transforms this flat list into a **tree** (AST):

```
source_file
  let_decl(x)
    binary_expr(+)
      integer_literal(1)
      integer_literal(2)
```

Every node in the tree is an **ASTNode**. The parser looks at the order of tokens and decides which node becomes the child of which. In `1 + 2`, the `+` becomes a `binary_expr`, and `1` and `2` become its children.

From the user's perspective, a single call does everything:

```c
MSFResult *r = msf_analyze("let x = 1 + 2", NULL);
msf_dump_text(r, stdout);
msf_result_free(r);
```

But what happens inside? That is what this document covers.

---

## Position in the Pipeline

```
Token list
     |
     v
+----------+
|  Parser  |  <- we are here
|          |
| token[0] token[1] token[2] ...
|   pos ->
+----------+
     |
     v
  AST tree
```

The parser holds three things:
- **Token stream** -- the array produced by the lexer
- **pos** -- a "where am I now" counter (starts at 0, incremented by every `adv(p)` call)
- **AST arena** -- the memory pool where nodes are allocated

The parser never backtracks (no backtracking, with the sole exception of subscript body detection). It only looks forward: "what is the current token? what is the next one?" This approach is called a **predictive parser**.

---

## Code Structure

The parser consists of 15 files. Each file has a single responsibility:

```
src/parser/
  private.h          (333 lines) -- Parser struct, inline helpers, all prototypes
  core.c             (571 lines) -- entry point: adv, alloc_node, error, modifiers
  top.c              (292 lines) -- parse_decl_stmt: top-level dispatch
  stmt.c             (432 lines) -- if, for, while, switch, guard, return, ...
  type.c             (625 lines) -- type parsing: Int, [String], (A, B) -> C, ...
  pattern.c          (279 lines) -- pattern matching: case .some(let x), ...
  decl/
    decl.c           (272 lines) -- block, import, typealias, enum body, nominal
    func.c           (275 lines) -- func, init, deinit, subscript
    var.c            (245 lines) -- var/let, computed properties, observers
    operator.c       (166 lines) -- precedencegroup, operator declarations
  expression/
    pratt.c          ( 90 lines) -- Pratt expression parser (precedence climbing)
    prefix.c         (410 lines) -- literal, ident, paren, array, dict, closure, ...
    postfix.c        (243 lines) -- call, member, subscript, optional chain, ...
    pre.c            (237 lines) -- precedence table, custom operator lookup
    closure.c        (193 lines) -- closure body + capture list parser
```

Data flow:

```
core.c
  |
  +--- top.c (parse_decl_stmt -- the dispatch point for everything)
  |      |
  |      +--- decl/*.c  (func, var, class, enum, ...)
  |      +--- stmt.c    (if, for, while, switch, ...)
  |      +--- expression/*.c (1 + 2, foo.bar(), { $0 + 1 }, ...)
  |      +--- type.c    ([Int], (A) -> B, Optional<T>, ...)
  |      +--- pattern.c (.some(let x), (a, b), ...)
  |
  v
AST tree
```

---

## Core Mechanisms

### 1. Token Navigation (core.c)

The parser walks over the token stream. Three fundamental operations:

```c
p_tok(p)     // look at the current token (no consume)
p_peek1(p)   // look one token ahead (no consume)
adv(p)       // consume the current token, pos++, return the token
```

Query functions:

```c
p_is_eof(p)          // is it TOK_EOF?
p_is_kw(p, KW_FUNC)  // is it the "func" keyword?
p_is_punct(p, '{')   // is it the '{' punctuation?
p_is_op(p, OP_ARROW) // is it the "->" operator?
cur_char(p)          // first byte of the current token
```

All of these are defined inline in `private.h`. Every call is O(1).

### 2. Node Allocation (core.c)

```c
ASTNode *node = alloc_node(p, AST_FUNC_DECL);
```

Allocates a zeroed node from the arena, sets `kind` and `tok_idx`. On OOM it records an error and returns NULL. The caller always checks for NULL.

### 3. Error Recording (core.c)

```c
parse_error_push(p, "%s:%u:%u: expected '{'", filename, line, col);
```

The parser does **not stop** on error -- it records the problem and continues scanning. There is a `MAX_PARSE_ERRORS` limit. Sema reads the same error pool; the user sees all errors via `msf_error_count/message/line/col()`.

---

## Dispatch: How Everything Begins

### parse_source_file() (top.c)

The top-level function. It creates an `AST_SOURCE_FILE` node and enters the `parse_decl_stmt()` loop:

```c
ASTNode *parse_source_file(Parser *p) {
  ASTNode *root = alloc_node(p, AST_SOURCE_FILE);
  while (!p_is_eof(p))
    add_stmt_chain(p, root, parse_decl_stmt(p));
  return root;
}
```

### parse_decl_stmt() (top.c)

Everything passes through here. It looks at the current token and routes to the correct parser:

```
Token                           Calls
-----                           -----
@attribute                   -> AST_ATTRIBUTE node created
public/static/final/...      -> collect_modifiers() + continue
func                         -> parse_func_decl()
var / let                    -> parse_var_decl()
class / struct / enum / protocol -> parse_nominal()
if / for / while / switch    -> parse_if/for/while/switch()
return / throw / break       -> parse_return/throw/jump()
#if / #endif                 -> parse_hash_directive()
{ ... }                      -> parse_block()
label:                       -> labeled statement
other                        -> parse_expr() (fallback)
```

The "other" fallback is critical: in Swift, `print("hello")` is an expression statement. When there is no declaration or statement keyword, the parser treats it as an expression.

---

## Expression Parsing: The Pratt Parser

Expression parsing is the most complex part. Why is `1 + 2 * 3` parsed as `1 + (2 * 3)`?

### Precedence Climbing (pratt.c)

The Pratt parser logic fits in 90 lines:

```c
ASTNode *parse_expr_pratt(Parser *p, int min_prec) {
  ASTNode *lhs = parse_prefix(p);     // left side: literal, ident, (expr), ...

  while (1) {
    Prec pr = get_infix_prec(p);       // current operator's precedence
    if (pr.lbp < min_prec) break;      // not enough precedence -> stop

    // consume operator, parse right side with higher precedence
    ASTNode *bin = alloc_node(p, AST_BINARY_EXPR);
    adv(p);                            // operator
    ASTNode *rhs = parse_expr_pratt(p, pr.rbp);  // recursive!
    ast_add_child(bin, lhs);
    ast_add_child(bin, rhs);
    lhs = bin;
  }
  return lhs;
}
```

Example: `1 + 2 * 3`

```
1. parse_prefix() -> integer_literal(1)
2. "+" precedence = 140. min_prec = 0, 140 >= 0 -> continue
3. adv(p), parse_expr_pratt(p, 141)  <- right side with higher precedence
   3a. parse_prefix() -> integer_literal(2)
   3b. "*" precedence = 150. 150 >= 141 -> continue
   3c. adv(p), parse_expr_pratt(p, 151)
       -> integer_literal(3)
       -> rhs for "*" = 3
   3d. binary_expr(*): lhs=2, rhs=3
4. rhs for "+" = binary_expr(*)
5. binary_expr(+): lhs=1, rhs=binary_expr(*)
```

Result:
```
binary_expr(+)
  integer_literal(1)
  binary_expr(*)
    integer_literal(2)
    integer_literal(3)
```

The Pratt loop also handles two special cases before the generic binary path: **ternary expressions** (`expr ? then : else`) and **is/as casts** (`expr is Type`, `expr as? Type`).

### Prefix: The Left Side (prefix.c)

`parse_prefix()` parses the beginning of an expression:

| Token | Produces |
|-------|----------|
| Number | `AST_INTEGER_LITERAL` / `AST_FLOAT_LITERAL` |
| String | `AST_STRING_LITERAL` |
| `true` / `false` | `AST_BOOL_LITERAL` |
| Identifier | `AST_IDENT_EXPR` |
| `(` | Grouped expression or tuple |
| `[` | Array literal or dictionary literal |
| `{` | Closure expression |
| `-` / `!` / `~` | `AST_UNARY_EXPR` (prefix operator) |
| `self` / `super` | `AST_SELF_EXPR` / `AST_SUPER_EXPR` |
| `nil` | `AST_NIL_LITERAL` |
| `try` / `await` | Wrapper node + inner expression |

### Postfix: The Right Side (postfix.c)

`parse_postfix()` chains operations onto the node returned by prefix:

| Token | Produces |
|-------|----------|
| `(` | `AST_CALL_EXPR` -- function call |
| `.` | `AST_MEMBER_EXPR` -- member access |
| `[` | `AST_SUBSCRIPT_EXPR` -- subscript |
| `?` | `AST_OPTIONAL_CHAIN` |
| `!` | `AST_FORCE_UNWRAP` |
| `{` | Trailing closure (appended to CALL_EXPR) |

Example: how `foo.bar(42)` is parsed:

```
1. prefix: AST_IDENT_EXPR(foo)
2. postfix ".": AST_MEMBER_EXPR(.bar), child = ident(foo)
3. postfix "(": AST_CALL_EXPR, children = [member(.bar), integer(42)]
```

### Precedence Table (pre.c)

Every operator has two numbers: **lbp** (left binding power) and **rbp** (right binding power).

```c
// Examples (simplified):
// =   -> lbp=90,  rbp=89  (right-associative assignment)
// ||  -> lbp=110, rbp=111
// &&  -> lbp=120, rbp=121
// ==  -> lbp=130, rbp=131
// +   -> lbp=140, rbp=141
// *   -> lbp=150, rbp=151
```

`rbp = lbp + 1` means **left-associative** (left side binds first).
`rbp = lbp - 1` means **right-associative** (right side binds first -- like assignment).

Custom operators are also supported: `precedencegroup` and `operator` declarations are registered into the parser's runtime table and work within the same Pratt loop.

---

## Declaration Parsing

### Modifiers (core.c)

Swift declarations begin with modifiers: `public static func ...`

`collect_modifiers()` consumes them one by one and collects them into a bitmask:

```c
uint32_t mods = collect_modifiers(p);
// mods = MOD_PUBLIC | MOD_STATIC
// Then: parse_func_decl(p, mods)
```

Special cases:
- `private(set)` -- setter access restriction (consumes 4 tokens)
- `class func` / `class var` -- treated as `MOD_STATIC` (overrideable)

### func (decl/func.c)

8-step parsing:

```
func name<T>(params) async throws -> RetType where T: P { body }
      1   2    3       4     5        6          7         8
```

Every step can be optional (protocol requirements have no body, simple functions have no generics).

### var/let (decl/var.c)

The most complex declaration. 5 different forms:

```swift
var x: Int = 42                          // stored
var x: Int { get { } set { } }           // computed
var x: Int { return expr }               // shorthand getter
var x: Int = 0 { willSet { } didSet { } } // observer
var x = 0, y = 0, z = 0                  // multi-var
```

When the parser sees `{`, it peeks inside: is it `get`/`set`, `willSet`/`didSet`, or something else? Based on this, it calls the correct sub-parser.

Multi-var declarations are chained via `next_sibling`. `add_stmt_chain()` walks this chain and adds each one as a separate child.

### Nominal Types (decl/decl.c)

class, struct, enum, protocol, actor, extension -- all go through `parse_nominal()`:

```swift
class Name<T>: Proto where T: P { body }
```

The difference is only in the body:
- **protocol** -> `parse_protocol_body()` (requirements only)
- **enum** -> `parse_enum_body()` (case declarations + methods)
- **others** -> `parse_block()` (general purpose)

---

## Statement Parsing (stmt.c)

Each statement keyword has its own parser:

| Keyword | Function | Special case |
|---------|----------|--------------|
| `if` | `parse_if()` | if-let, if-case, else-if chaining |
| `guard` | `parse_guard()` | guard-let, guard-case |
| `for` | `parse_for()` | for-in, pattern matching |
| `while` | `parse_while()` | while-let |
| `switch` | `parse_switch()` | case clauses, default, where guard |
| `do` | `parse_do()` | do-catch-let |
| `return` | `parse_return()` | optional expression |
| `throw` | `parse_throw()` | required expression |
| `defer` | `parse_defer()` | block |
| `repeat` | `parse_repeat()` | repeat-while |
| `discard` | `parse_discard()` | discard statement |

`if` and `guard` are special because they support **condition elements**:

```swift
if let x = optional, x > 0, case .some(let y) = foo { }
//  |_____________|  |_____|  |___________________|
//  optional binding  boolean   pattern matching
```

`parse_condition_element()` distinguishes these three forms.

---

## Type Parsing (type.c)

Swift's type system is rich -- the parser recognizes these forms:

| Form | Example | Result |
|------|---------|--------|
| Simple | `Int` | `AST_TYPE_IDENT` |
| Generic | `Array<Int>` | `AST_TYPE_GENERIC` |
| Optional | `Int?` | `AST_TYPE_OPTIONAL` |
| Array | `[Int]` | `AST_TYPE_ARRAY` |
| Dict | `[String: Int]` | `AST_TYPE_DICT` |
| Tuple | `(Int, String)` | `AST_TYPE_TUPLE` |
| Function | `(Int) -> Bool` | `AST_TYPE_FUNC` |
| Opaque | `some Proto` | `AST_TYPE_SOME` |
| Existential | `any Proto` | `AST_TYPE_ANY` |
| Metatype | `Int.Type` | `AST_TYPE_METATYPE` |
| Composition | `Proto1 & Proto2` | `AST_TYPE_COMPOSITION` |

These can nest arbitrarily: `[String: [Int]?]` = dictionary whose value is an optional array of int.

---

## Pattern Parsing (pattern.c)

Pattern matching is used in `case` clauses, `if case`, `guard case`, and `for-in`:

```swift
case .some(let x):       // enum case pattern with binding
case (a, b):             // tuple destructuring
case let x?:             // optional pattern
case is MyType:          // type-check pattern
case let x as MyType:    // type-cast pattern
```

`parse_pattern()` handles all these forms and is called from `stmt.c` whenever the parser encounters a pattern context.

---

## File-by-File Walkthrough

### private.h -- Internal Header

The parser's contract with itself. 333 lines. Provides:

- Parser struct definition (opaque in the public header)
- Token query helpers (`p_tok`, `p_is_kw`, `p_is_punct`, etc.) -- all inline, all O(1)
- String match macros (`tok_eq`, `p_is_ident_str`, `p_is_ck`)
- Contextual keyword constants (`CK_GET`, `CK_SET`, `CK_WILL_SET`, ...)
- Prec struct for Pratt binding powers
- Prototypes for every internal parse function

### core.c -- Foundation Layer

Not the parser's "brain" but its "nervous system". 571 lines. Contains:

- `adv()`, `cur_char()`, `tok_text_eq()` -- token navigation
- `alloc_node()` -- arena node allocation
- `parse_error_push()` -- error recording
- `collect_modifiers()` -- modifier bitmask collection
- `skip_balanced()`, `skip_generic_params()` -- balanced-bracket skippers
- `parse_throws_clause()` -- throws/rethrows/throws(ErrorType)
- `parse_hash_directive()` -- #if/#else/#endif/#warning/#error
- `parser_init()` / `parser_destroy()` -- public lifecycle
- `parser_ctx_reset()` -- internal state reset
- `parse_expression_from_cstring()` -- helper for string interpolation

### top.c -- Dispatch

292 lines. One main function: `parse_decl_stmt()`. Looks at each token and routes to the correct parser. Handles attributes (`@MainActor`), contextual keywords (`nonisolated`, `indirect`, `convenience`), modifier chains, hash directives, labeled statements, and expression fallback.

Also contains `parse_source_file()`, the top-level entry point that loops `parse_decl_stmt()` until EOF.

### expression/pratt.c -- Pratt Parser

90 lines. The heart of all expression parsing. Handles ternary (`? :`), is/as casts, and binary operators. Precedence climbing ensures correct operator precedence.

### expression/prefix.c -- Left Side

410 lines. Literals, identifiers, grouped expressions, array/dict literals, closures, try/await wrappers, self/super, unary prefix operators.

### expression/postfix.c -- Right Side

243 lines. Function call, member access, subscript, optional chaining, force unwrap, trailing closure.

### expression/pre.c -- Precedence Table

237 lines. The complete operator precedence and associativity table for Swift. Handles both built-in operators (via `op_kind` switch -- O(1)) and custom operators (via runtime lookup against registered `precedencegroup` and `operator` declarations).

### expression/closure.c -- Closure Parser

193 lines. Parses `{ [capture] (params) -> RetType in body }`. Capture list, parameter list, and closure body are each handled separately.

### decl/func.c -- Function-Like Declarations

275 lines. func, init, deinit, subscript. All follow the same pattern: keyword + name + generics + params + throws + return type + where clause + body.

### decl/var.c -- Variable Declarations

245 lines. Handles all 5 forms of var/let. Computed property and observer parsers live here too.

### decl/decl.c -- Block and Nominal Types

272 lines. `parse_block()` (fundamental building block), import, typealias, enum body, `parse_nominal()` (class/struct/enum/protocol/actor/extension).

### decl/operator.c -- Operator Declarations

166 lines. `precedencegroup` and `operator` declarations. Its own world -- not called by any other parser.

### stmt.c -- Statements

432 lines. if, for, while, repeat, switch, guard, return, throw, defer, do-catch, break, continue, fallthrough, discard.

### type.c -- Type Parsing

625 lines. Parses all of Swift's type forms. Recursive descent -- nested types like `[String: [Int]?]` are handled naturally.

### pattern.c -- Pattern Matching

279 lines. Case patterns, tuple destructuring, optional pattern, is/as pattern, wildcard and binding patterns.

---

## Error Table

| Bug | What happened | Fix |
|-----|---------------|-----|
| `1 + * 2` garbage AST | Pratt parser continued when rhs was NULL | NULL check + error message |
| Union clash | `data.binary.op_tok` overwrote the closure's `captures` | Used `arg_label_tok` instead |
| Infinite loop | `adv(p)` was not called on unknown tokens | Progress guard: `pos == before -> adv(p)` |
| `last_error[256]` | Dead code -- never read anywhere | Removed |
| 755-line decl.c | Operator parsing got lost in the noise | Split into 4 files |
| `// Ch10: class var` | Cryptic comment | Removed, replaced with Doxygen annotations |

---

## Public API Summary

```c
// Lifecycle
Parser  *parser_init(const Source *src, const TokenStream *ts, ASTArena *arena);
void     parser_destroy(Parser *p);

// Parsing
ASTNode *parse_source_file(Parser *p);

// Error accessors
uint32_t    parser_error_count(const Parser *p);
const char *parser_error_message(const Parser *p, uint32_t index);
uint32_t    parser_error_line(const Parser *p, uint32_t index);
uint32_t    parser_error_col(const Parser *p, uint32_t index);
```

Internal helper for reuse across multiple parse passes:

```c
void parser_ctx_reset(Parser *p, const Source *src, const TokenStream *ts,
                      ASTArena *arena);
```

---

## Next Chapter

-> [04 -- Type System](04-type-system.md)

---

*This document is part of the [msf](https://github.com/toprakdeviren/msf) project.*
