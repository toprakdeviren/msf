# What Is a Lexer and How Does It Work?

> This document explains the msf lexer layer from scratch.
> Target audience: C developers who want to learn compiler development but don't know where to start.

## What Does a Lexer Do?

We have a piece of text:

```
let x = 1 + 2
```

The lexer reads this text and converts it into a list of **tokens**:

```
KEYWORD     "let"
IDENTIFIER  "x"
OPERATOR    "="
INTEGER_LIT "1"
OPERATOR    "+"
INTEGER_LIT "2"
EOF
```

Each token knows three things: **what kind** it is (keyword, identifier, number, etc.), **where** it lives in the source (line, column, byte offset), and **how long** it is (in bytes).

To see this in action:

```c
#include <msf.h>

Source src = { .data = "let x = 1 + 2", .len = 13, .filename = "demo" };
TokenStream ts;
token_stream_init(&ts, 64);
lexer_tokenize(&src, &ts, 1, NULL);   // skip_ws=1: skip whitespace

for (size_t i = 0; i < ts.count; i++) {
    Token t = ts.tokens[i];
    printf("%-12s  \"%.*s\"  line:%u col:%u\n",
           token_type_name(t.type),
           (int)t.len, src.data + t.pos,
           t.line, t.col);
}

token_stream_free(&ts);
```

Output:

```
keyword       "let"    line:1 col:1
identifier    "x"      line:1 col:5
operator      "="      line:1 col:7
int_lit       "1"      line:1 col:9
operator      "+"      line:1 col:11
int_lit       "2"      line:1 col:13
eof           ""       line:1 col:14
```

---

## Where It Sits in the Pipeline

```
"let x = 1 + 2"
       |
       v
 +----------+     +----------+     +----------+
 |  LEXER   |---->|  Parser  |---->|   Sema   |
 |  <- us   |     |  (AST)   |     |  (type)  |
 +----------+     +----------+     +----------+
       |
       v
   Token list
```

The lexer reads source code **character by character** and splits it into meaningful chunks. The parser takes this token list and builds a tree (AST). Without the lexer, the parser would have to ask "is this byte a space, a letter, a digit?" at every step.

---

## Code Structure

The lexer is spread across 10 files. Each file does one job:

```
src/lexer/
  private.h           -- lexer-internal shared declarations
  lexer.c             -- entry point: init, next, tokenize
  token.c             -- token type names, text extraction, stream
  helpers.c           -- multi-char operator table, string tok helpers
  diag.c              -- diagnostic recording
  char_tables.h       -- character classification + keyword tables
  unicode_ranges.h    -- Unicode identifier/operator range tables
  scan/
    comment.c         -- line and block comment scanners
    string.c          -- string literal scanners (regular, triple, raw)
    symbol.c          -- operator, regex, punctuation dispatch
    fast.c            -- SWAR ident scan, number scan, string body scan
```

**Why so many files?** Because the lexer started as a single 800-line file. Putting everything in one place made "find and fix" painful. Now you can tell what a file does just by looking at its name.

Data flow:

```
lexer.c  (dispatch)
  |
  +--- scan/comment.c  (// and /* */)
  +--- scan/string.c   ("...", """...""", #"..."#)
  +--- scan/symbol.c   (operators, regex, punctuation)
  +--- scan/fast.c     (scan_ident, scan_number, scan_string_body)
  |
  +--- helpers.c       (all scanners use these)
  +--- diag.c          (all scanners use this)
  |
  v
token.c  (consumes the produced tokens)
```

---

## What Is a Token?

A `Token` struct looks like this:

```c
typedef struct {
  TokenType type;      // what kind: keyword, identifier, int_lit, operator, ...
  uint32_t  pos;       // byte offset in the source
  uint32_t  len;       // byte length
  uint32_t  line;      // line number (1-based)
  uint32_t  col;       // column number (1-based)
  Keyword   keyword;   // which keyword (KW_FUNC, KW_LET, ...) -- KW_NONE if not a keyword
  OpKind    op_kind;   // which multi-char operator (OP_EQ, OP_ARROW, ...) -- OP_NONE if single-char
} Token;
```

**Important:** A Token does **not** copy any string from the source. It only stores `pos` and `len`. The actual text lives at `source.data[tok.pos .. tok.pos + tok.len]`. This means:

- Zero allocation (even millions of tokens use no heap)
- Original line/column information is always available for error messages
- Trade-off: the source text must stay in memory as long as the tokens are alive

---

## File-by-File Walkthrough

### 1. `private.h` -- Shared Infrastructure

This file is not exposed externally (it is not a public header). Everything shared between the lexer's .c files lives here:

**Pattern macros** -- give names to repeated character checks:

```c
#define IS_NEWLINE(c)           ((c) == '\n' || (c) == '\r')
#define IS_TRIPLE_QUOTE(s, p)   ((s)[(p)] == '"' && (s)[(p)+1] == '"' && (s)[(p)+2] == '"')
#define IS_RAW_STRING_START(c)  ((c) == '#')
#define IS_BLOCK_OPEN(s, p, len)   (/* s[p..p+1] == / * */)
#define IS_BLOCK_CLOSE(s, p, len)  (/* s[p..p+1] == * / */)
```

**State advance macros** -- standardize how the lexer updates pos/line/col:

```c
#define ADVANCE(l)           // pos++, col++       (1 byte forward on the same line)
#define ADVANCE_BY(l, n)     // pos += n, col += n (n bytes forward on the same line)
#define NEWLINE_ADVANCE(l)   // pos++, line++, col = 1 (move to the next line)
```

**Diagnostic message constants** -- all scanners use the same strings:

```c
#define DIAG_UNTERM_STRING       "unterminated string literal"
#define DIAG_UNTERM_TRIPLE       "unterminated triple-quoted string literal"
#define DIAG_UNTERM_RAW_STRING   "unterminated raw string literal"
#define DIAG_UNTERM_BLOCK_COMMENT "unterminated block comment"
#define DIAG_INVALID_ESCAPE      "invalid escape sequence in string literal"
#define DIAG_SINGLE_QUOTE        "single-quoted string is not allowed; use double quotes"
```

Why macros? Because when you want to change a message, you only touch one place. And in tests you can write `strcmp(diag.message[0], DIAG_UNTERM_STRING)`.

**Scanner declarations** -- functions from each .c file are declared here so they can call each other (e.g. `scan_symbol` -> `scan_line_comment`).

---

### 2. `lexer.c` -- Entry Point

This file is the lexer's "brain". It has 3 functions:

**`lexer_init(l, src)`** -- Resets lexer state: pos=0, line=1, col=1.

**`lexer_next(l)`** -- Returns one token per call. Table-driven dispatch:

```c
uint8_t c   = source[pos];
uint8_t cls = LEX_CHAR_CLASS[c];     // 256-byte table: byte -> character class
uint8_t act = LEX_ACTION[cls];       // class -> action (0-4)
```

| Action | Character class | What it does |
|--------|----------------|--------------|
| -- | `CC_NEWLINE` | Increment line, return `TOK_NEWLINE` |
| 0 | `CC_WHITESPACE` | Consume adjacent whitespace, return `TOK_WHITESPACE` |
| 1 | Letter/`_`/`$`/Unicode | `scan_ident()` + keyword lookup |
| 2 | Digit | `scan_number()` |
| 3 | `"` or `'` | `scan_string()` -> scan/string.c |
| -- | `#` | `scan_raw_string()` -> scan/string.c |
| 4 | Other | `scan_symbol()` -> scan/symbol.c |

**Why table-driven?** The 256-byte lookup table fits in a CPU cache line. Every character classification is O(1). Much faster than an `if (c >= 'a' && c <= 'z' || c >= 'A' ...)` chain.

**`lexer_tokenize(src, out, skip_ws, diag)`** -- Calls `lexer_next()` in a loop. Pushes tokens into the stream. Additional work:

- **Whitespace filtering:** if `skip_ws=1`, whitespace/comment/newline tokens are skipped
- **Circuit breaker:** stops after `4 * source_len + 1024` iterations (infinite loop protection)
- **EOF guarantee:** the stream always ends with `TOK_EOF`
- **Shrink-to-fit:** returns excess allocated memory if the array is less than half used

---

### 3. `scan/comment.c` -- Comment Scanners

Two functions, two comment styles:

**`scan_line_comment()`** -- `// ...` until end of line.

```c
// This is a line comment
```

Uses `memchr` to find the first newline -- skips the comment body in a single call instead of walking byte by byte. Does **not** consume the newline itself -- that becomes a separate `TOK_NEWLINE`.

**`scan_block_comment()`** -- `/* ... */` with nesting support.

```c
/* outer /* inner */ still outer */
```

Maintains a `depth` counter: each `/*` increments, each `*/` decrements. Finishes when depth reaches 0. If EOF is reached first, records `DIAG_UNTERM_BLOCK_COMMENT`.

Why does nesting support matter? Because when you comment out a block of code that already contains `/* */`, a lexer without nesting support closes at the first `*/` and the rest of the code is left hanging.

```
/* a lexer without nesting gets this wrong:
   /* inner comment */   <-- closes here
   remaining code        <-- ends up outside the comment
*/
```

---

### 4. `scan/string.c` -- String Scanners

Swift's most complex token types live here. There are 4 kinds of string:

| Kind | Example | Function |
|------|---------|----------|
| Regular | `"hello"` | `scan_string()` |
| Triple-quoted | `"""multi\nline"""` | `scan_string()` |
| Raw | `#"no \escape"#` | `scan_raw_string()` |
| Raw triple | `#"""raw multi"""#` | `scan_raw_string()` |

**`validate_string_escapes(s, start, end, ...)`**

Called after the string is scanned. Walks the body, checks every backslash:

```
\n \t \r \0 \" \' \\    -- simple escapes
\(expr)                  -- string interpolation (balanced parenthesis counting)
\u{XXXX}                 -- Unicode escape (at least 1 hex digit + closing })
```

If an invalid escape is found (like `\q`), it records the position and returns 0.

**`scan_string()`** -- Scans regular and triple-quoted strings.

Triple-quote detection uses the `IS_TRIPLE_QUOTE` macro. On unterminated input, it returns everything up to EOF as a token (no crash -- a diagnostic is produced instead).

Also catches single-quote usage (`'hello'`) and reports a "use double quotes" diagnostic.

**`scan_raw_string()`** -- Counts `#` characters, matches opening/closing delimiters.

```swift
##"this can contain " and # inside"##
```

If the opening has 2 `#` characters, the closing must be `"##`. The `#` count (`hc`) must match. Also supports triple-quoted raw strings (`#"""..."""#`).

If `#` is not followed by `"`, this is not a raw string -- a zero-length sentinel token is returned and `lexer_next()` tries another scanner.

---

### 5. `scan/symbol.c` -- Operators and Punctuation

This file is the catch-all dispatcher for "everything else":

**`scan_symbol()`** -- Dispatch order (first match wins):

```
1. "//" -> scan_line_comment()   (delegate to scan/comment.c)
2. "/*" -> scan_block_comment()  (delegate to scan/comment.c)
3. Multi-char operator? -> walk the MULTI_OPS[] table
4. "/" + non-delimiter? -> try scan_regex_literal()
5. Single character -> LEX_CHAR_TOKEN[c] table lookup
```

**Multi-char operators** -- The `MULTI_OPS[]` table (defined in helpers.c) tries the longest operator first:

```c
{"===", 3, OP_IDENTITY_EQ},   // 3 chars -- tried first
{"==",  2, OP_EQ},             // 2 chars -- tried second
{"->",  2, OP_ARROW},
{"??",  2, OP_NIL_COAL},
```

Order matters: if `===` is not tried before `==`, `===` will never match.

**`scan_regex_literal()`** -- Scans `/pattern/` form.

Regex and the division operator (`/`) are ambiguous. The heuristic: if `/` is **not** followed by whitespace, closing bracket, comma, semicolon, or colon, try it as a regex. If no closing `/` is found, it fails and `/` is treated as a single-character operator.

Supports backslash-escapes inside regexes (`/a\/b/`). Returns failure on newline -- Swift regex literals cannot span multiple lines.

---

### 6. `scan/fast.c` -- Performance-Critical Scanners

This file contains the lexer hot path -- the functions called for the most common token types. They use SWAR (8-byte word-at-a-time processing), bitmap lookups, and `memchr` (which maps to NEON/AVX2/WASM128 SIMD on modern platforms).

**`keyword_detect()`** -- Binary search over the sorted `LEX_KEYWORDS[]` table (67 entries). At most 6-7 iterations. Each iteration: one length-checked `memcmp` (no `strlen` needed).

**`scan_ident()`** -- Two-phase identifier scanning (detailed in the next section).

**`scan_number()`** -- Handles decimal, hex (`0x`), octal (`0o`), binary (`0b`), underscore separators (`1_000`), fractional parts (`.5`), and exponents (`e`/`E` for decimal, `p`/`P` for hex float). Sets the output type to `TT_FLOAT_LITERAL` or `TT_INTEGER_LITERAL`.

**`scan_string_body()`** -- Uses two parallel `memchr` searches -- one for the closing quote, one for backslash. Whichever comes first determines the segment boundary. For long escape-free strings, this is essentially a single pass (~O(n/16) on SIMD platforms). Newlines within each segment are counted via a third `memchr('\n')` call.

---

### 7. `helpers.c` -- Utility Functions

Three tools shared by the scanners:

**`MULTI_OPS[]` table** -- The multi-character operator lookup table. Lives in `.rodata` -- zero runtime cost, no initialization. `scan_symbol()` scans it linearly.

**`make_string_tok(sp, tl, sl, sc)`** -- Constructs a `TOK_STRING_LIT` token. Instead of writing a 7-field compound literal in every string scanner, this helper is called. `keyword` and `op_kind` are always `KW_NONE` / `OP_NONE`.

**`advance_past_token(l, s, tok_start, tok_len, extra_lines)`** -- Updates lexer state after a multiline token.

Strings and comments can span multiple lines. When they do, `line` and `col` must be updated:

```c
// After scanning """hello\nworld""":
advance_past_token(l, s, start, 18, 1);
// l->line += 1
// l->col = number of chars after the last \n
// l->pos = start + 18
```

Without this helper, the same column-calculation logic was repeated in 4 different places (triple-quoted, regular, raw string, unterminated). Now it's in one place.

---

### 8. `diag.c` -- Diagnostic Recording

The lexer does not stop on errors -- it records the problem and keeps scanning. This file provides the recording mechanism.

**`lexer_diag_init(d)`** -- Resets the counter. Call before `lexer_tokenize()`.

**`lexer_diag_record(l, line, col, fmt, ...)`** -- Called by scanners (internal). Supports `printf`-style formatting. Silently no-ops if `l->diag` is NULL.

**`lexer_diag_push(d, line, col, fmt, ...)`** -- Called by the parser (public API). Writes to the same diagnostic struct. For example, the parser can add an "expected expression after operator" warning.

Both functions write into the same `LexerDiagnostics` struct: an array of `(message, line, col)` triples. Capped at `LEXER_DIAG_MAX` entries -- when the array is full, additional diagnostics are silently dropped.

Usage:

```c
LexerDiagnostics diag;
lexer_diag_init(&diag);
lexer_tokenize(&src, &ts, 1, &diag);

for (size_t i = 0; i < diag.count; i++)
    printf("%u:%u: %s\n", diag.line[i], diag.col[i], diag.message[i]);
```

---

### 9. `token.c` -- Token Utilities

Independent of the lexer -- operates only on `Token` and `TokenStream`. The lexer **produces** tokens; token.c helps you **consume** them.

**`token_type_name(t)`** -- Converts enum to human-readable name: `TOK_KEYWORD` -> `"keyword"`, `TOK_EOF` -> `"eof"`. Table-driven (designated initializer array).

**`token_text(src, tok)`** -- Returns the token's source text as a NUL-terminated string.

```c
const char *text = token_text(src, &tok);
printf("Token text: %s\n", text);
```

**Warning:** Uses a thread-local buffer. Each call overwrites the previous result. This code is **incorrect**:

```c
// WRONG -- both calls write to the same buffer, both show b's text:
printf("%s %s", token_text(src, &a), token_text(src, &b));
```

Correct usage: either copy the result (`strdup`), or use separate `printf` calls.

**`token_stream_init(ts, cap)` / `token_stream_free(ts)` / `token_stream_push(ts, tok)`**

The token stream is a dynamic array. `push` uses a 1.5x growth strategy (amortized O(1)). On OOM, the token is silently dropped -- existing tokens are preserved, the process does not crash.

---

### 10. `char_tables.h` -- Character Classification Tables

This file is code-generated (`scripts/codegen.py`) and provides the lookup tables that drive the lexer's dispatch:

**`LEX_CHAR_CLASS[256]`** -- Maps every byte value to a character class:

| Class | Constant | Bytes |
|-------|----------|-------|
| 0 | `CC_NULL` | NUL byte |
| 1 | `CC_WHITESPACE` | space, tab |
| 2 | `CC_NEWLINE` | LF, CR |
| 3 | `CC_DIGIT` | 0-9 |
| 4 | `CC_UPPER` | A-Z |
| 5 | `CC_LOWER` | a-z, _ |
| 6 | `CC_UNICODE` | bytes >= 0x80 |
| 7 | `CC_STRING` | `"` or `'` |
| 8 | `CC_DOLLAR` | `$` |
| 9 | `CC_SYMBOL` | all other printable ASCII |

**`LEX_ACTION[10]`** -- Maps each character class to one of 5 scan actions (0-4). This two-level indirection (byte -> class -> action) keeps the dispatch code minimal.

**`LEX_CHAR_TOKEN[256]`** -- Maps every byte to a token type for single-character fallback in `scan_symbol()`.

**`LEX_KEYWORDS[]`** -- A sorted table of 67 Swift keywords used by `keyword_detect()` for binary search.

---

## Identifier + Keyword Scanning

When `lexer_next()` reaches the identifier action, it calls `scan_ident()` (implemented in `scan/fast.c`). The scan has two phases:

**ASCII fast-path (SWAR):** Reads 8 bytes at a time, checks them against a 256-bit bitmap. Purely ASCII identifiers (about 95% of real code) go through this path -- the byte-by-byte loop is never entered.

```c
// 8-byte SWAR: are all bytes ASCII identifier chars?
while (pos + 8 <= len) {
  uint64_t word;
  memcpy(&word, src + pos, 8);
  if (word & 0x8080808080808080ULL) break;  // non-ASCII -> slow path
  if (!all_ident_chars(word)) break;         // non-ident char -> stop
  pos += 8;
}
```

**Unicode fallback:** When a non-ASCII byte appears, it UTF-8 decodes the codepoint and checks it against Swift's Unicode range tables (`unicode_ranges.h`). The first character must be an `identifier-head`, subsequent characters must be `identifier-continue`.

After the identifier is scanned, **is it a keyword?** Binary search over the sorted `LEX_KEYWORDS[]` table:

```c
uint32_t kw_id = keyword_detect(src + start, len);
Keyword kw = map_kw_id(kw_id);
// kw == KW_FUNC means "func" keyword, KW_NONE means plain identifier
```

---

## Errors and How We Fixed Them

| Bug | What happened | Fix |
|-----|---------------|-----|
| `token_stream_push` called `exit()` | OOM killed the process | Silent drop -- existing tokens are preserved |
| 4x copy-pasted col tracking | Column was wrong in multiline strings | `advance_past_token()` helper |
| `size_t -> uint32_t` truncation | `len` would wrap on >4GB files | `LEXER_MAX_SOURCE_LEN` guard |
| Regex false positive | `a/b` division was tried as regex | Tightened the heuristic |
| Magic strings | Error messages were hardcoded in 6 places | Extracted to `DIAG_*` constants |
| Repeated patterns | `l->pos++; l->col++` appeared 10+ times | `ADVANCE()`, `ADVANCE_BY()` macros |
| Everything in one file | Finding bugs in an 800-line file was painful | Split into 10 files |
| String interpolation | `\(expr)` was treated as a 2-char escape | Balanced parenthesis counter |

---

## Next Chapter

-> [02 -- What Is an AST and How Does It Work?](02-ast.md)

---

*This document is part of the [msf](https://github.com/toprakdeviren/msf) project.*
