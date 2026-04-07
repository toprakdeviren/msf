# Parser Nasil Calisir?

> Bu belge MiniSwiftFrontend'in parser katmanini sifirdan acikliyor.
> Onceki bolumler: [01 — Lexer](01-lexer-nedir.md), [02 — AST](02-ast-nedir.md)

## Parser Ne Yapar?

Lexer bize token listesi verdi:

```
KEYWORD "let"  IDENTIFIER "x"  OPERATOR "="  INTEGER "1"  OPERATOR "+"  INTEGER "2"  EOF
```

Parser bu düz listeyi bir **agac**a (AST) donusturur:

```
source_file
  let_decl(x)
    binary_expr(+)
      integer_literal(1)
      integer_literal(2)
```

Agacin her dugumu bir **ASTNode**. Parser, token'larin sirasina bakarak hangi dugumun hangisinin cocugu olduguna karar verir. `1 + 2`'deki `+` bir `binary_expr` olur, `1` ve `2` onun cocuklari olur.

Kullanici olarak tek satirda:

```c
MSFResult *r = msf_analyze("let x = 1 + 2", NULL);
msf_dump_text(r, stdout);
msf_result_free(r);
```

Ama iceride ne oluyor? Bunu anlatacagiz.

---

## Pipeline'daki Yeri

```
Token listesi
     |
     v
+----------+
|  Parser  |  <- biz
|          |
| token[0] token[1] token[2] ...
|   pos ->
+----------+
     |
     v
  AST agaci
```

Parser'in elinde uc sey var:
- **Token stream** — lexer'in urettigi dizi
- **pos** — "simdi neredeyim" sayaci (0'dan baslar, her `adv(p)` ile artar)
- **AST arena** — node'lari allocate ettigi bellek havuzu

Parser hic geri donmez (backtrack yok, tek istisna subscript body'si). Sadece ileriye bakar: "simdiki token ne? Sonraki ne?" Bu yaklasima **predictive parser** denir.

---

## Kod Yapisi

Parser 14 dosyadan olusur. Her dosya tek bir is yapar:

```
src/parser/
  internal.h          (256 satir) -- Parser struct, inline helpers, tum prototipler
  core.c              (544 satir) -- giris noktasi: adv, alloc_node, error, modifiers
  top.c               (338 satir) -- parse_decl_stmt: ust seviye dispatch
  stmt.c              (486 satir) -- if, for, while, switch, guard, return, ...
  type.c              (726 satir) -- tip parse: Int, [String], (A, B) -> C, ...
  pattern.c           (278 satir) -- pattern matching: case .some(let x), ...
  decl/
    decl.c            (311 satir) -- block, import, typealias, enum body, nominal
    func.c            (275 satir) -- func, init, deinit, subscript
    var.c             (245 satir) -- var/let, computed properties, observers
    operator.c        (161 satir) -- precedencegroup, operator declarations
  expression/
    pratt.c           ( 86 satir) -- Pratt expression parser (precedence climbing)
    prefix.c          (361 satir) -- literal, ident, paren, array, dict, closure, ...
    postfix.c         (243 satir) -- call, member, subscript, optional chain, ...
    pre.c             (237 satir) -- precedence table, custom operator lookup
    closure.c         (193 satir) -- closure body + capture list parser
```

Veri akisi:

```
core.c
  |
  +--- top.c (parse_decl_stmt -- her seyin dispatch noktasi)
  |      |
  |      +--- decl/*.c  (func, var, class, enum, ...)
  |      +--- stmt.c    (if, for, while, switch, ...)
  |      +--- expression/*.c (1 + 2, foo.bar(), { $0 + 1 }, ...)
  |      +--- type.c    ([Int], (A) -> B, Optional<T>, ...)
  |      +--- pattern.c (.some(let x), (a, b), ...)
  |
  v
AST agaci
```

---

## Temel Mekanizma

### 1. Token Navigasyonu (core.c)

Parser token stream uzerinde yurur. Uc temel islem:

```c
p_tok(p)     // simdiki token'a bak (tuketme)
p_peek1(p)   // bir sonrakine bak (tuketme)
adv(p)       // simdiki token'i tuket, pos++ yap, token'i doner
```

Sorgu fonksiyonlari:

```c
p_is_eof(p)          // TOK_EOF mi?
p_is_kw(p, KW_FUNC)  // "func" keyword'u mu?
p_is_punct(p, '{')   // '{' punctuation'i mi?
p_is_op(p, OP_ARROW) // "->" operatoru mu?
cur_char(p)          // simdiki token'in ilk byte'i
```

Bunlarin hepsi `internal.h`'de inline olarak tanimli. Her cagri O(1).

### 2. Node Allocation (core.c)

```c
ASTNode *node = alloc_node(p, AST_FUNC_DECL);
```

Arena'dan sifirlanmis bir node alir, `kind` ve `tok_idx` set eder. OOM'da hata kaydeder ve NULL doner. Caller her zaman NULL kontrol yapar.

### 3. Hata Kaydi (core.c)

```c
parse_error_push(p, "%s:%u:%u: expected '{'", filename, line, col);
```

Parser hatada **durmaz** — sorunu kaydeder ve taramaya devam eder. `MAX_PARSE_ERRORS` sinirinda. Sema da ayni hata havuzunu okur, kullanici tum hatalari `msf_error_count/message/line/col()` ile gorur.

---

## Dispatch: Her Sey Nasil Baslar

### parse_source_file() (top.c)

En ust seviye fonksiyon. Bir `AST_SOURCE_FILE` node'u olusturur ve `parse_decl_stmt()` dongusune girer:

```c
ASTNode *parse_source_file(Parser *p) {
  ASTNode *root = alloc_node(p, AST_SOURCE_FILE);
  while (!p_is_eof(p)) {
    ASTNode *s = parse_decl_stmt(p);
    ast_add_child(root, s);
  }
  return root;
}
```

### parse_decl_stmt() (top.c)

Her sey buradan gecer. Simdiki token'a bakar ve dogru parser'a yonlendirir:

```
Token ne?                          Ne cagirilir?
------                             -------------
@attribute                      -> AST_ATTRIBUTE node olustur
public/static/final/...         -> collect_modifiers() + devam
func                            -> parse_func_decl()
var / let                       -> parse_var_decl()
class / struct / enum / protocol -> parse_nominal()
if / for / while / switch       -> parse_if/for/while/switch()
return / throw / break          -> parse_return/throw/jump()
#if / #endif                    -> parse_hash_directive()
{ ... }                         -> parse_block()
label:                          -> labeled statement
diger                           -> parse_expr() (fallback)
```

Bu "diger" fallback cok onemli: Swift'te `print("hello")` bir expression statement'tir. Declaration ya da statement keyword'u yoksa, parser expression olarak parse eder.

---

## Expression Parsing: Pratt Parser

Expression parsing en karmasik kisim. `1 + 2 * 3` neden `1 + (2 * 3)` olarak parse edilir?

### Precedence Climbing (pratt.c)

Pratt parser'in mantigi 86 satirda:

```c
ASTNode *parse_expr_pratt(Parser *p, int min_prec) {
  ASTNode *lhs = parse_prefix(p);     // sol taraf: literal, ident, (expr), ...

  while (1) {
    Prec pr = get_infix_prec(p);       // simdiki operator'un onceligi
    if (pr.lbp < min_prec) break;      // oncelik yetmiyor -> dur

    // operator'u tuket, sag tarafi daha yuksek oncelikle parse et
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

Ornek: `1 + 2 * 3`

```
1. parse_prefix() -> integer_literal(1)
2. "+" onceligi = 140. min_prec = 0, 140 >= 0 -> devam
3. adv(p), parse_expr_pratt(p, 141)  <- sag taraf daha yuksek oncelikle
   3a. parse_prefix() -> integer_literal(2)
   3b. "*" onceligi = 150. 150 >= 141 -> devam
   3c. adv(p), parse_expr_pratt(p, 151)
       -> integer_literal(3)
       -> "*" icin rhs = 3
   3d. binary_expr(*): lhs=2, rhs=3
4. "+" icin rhs = binary_expr(*)
5. binary_expr(+): lhs=1, rhs=binary_expr(*)
```

Sonuc:
```
binary_expr(+)
  integer_literal(1)
  binary_expr(*)
    integer_literal(2)
    integer_literal(3)
```

### Prefix: Sol Taraf (prefix.c)

`parse_prefix()` expression'in basini parse eder:

| Token | Ne uretir |
|-------|-----------|
| Sayi | `AST_INTEGER_LITERAL` / `AST_FLOAT_LITERAL` |
| String | `AST_STRING_LITERAL` |
| `true` / `false` | `AST_BOOL_LITERAL` |
| Identifier | `AST_IDENT_EXPR` |
| `(` | Gruplanmis expression veya tuple |
| `[` | Array literal veya dictionary literal |
| `{` | Closure expression |
| `-` / `!` / `~` | `AST_UNARY_EXPR` (prefix operator) |
| `self` / `super` | `AST_SELF_EXPR` / `AST_SUPER_EXPR` |
| `nil` | `AST_NIL_LITERAL` |
| `try` / `await` | Wrapper node + ic expression |

### Postfix: Sag Taraf (postfix.c)

`parse_postfix()` prefix'ten donen node'a zincirleme islemler ekler:

| Token | Ne uretir |
|-------|-----------|
| `(` | `AST_CALL_EXPR` — fonksiyon cagrisi |
| `.` | `AST_MEMBER_EXPR` — member access |
| `[` | `AST_SUBSCRIPT_EXPR` — subscript |
| `?` | `AST_OPTIONAL_CHAIN` |
| `!` | `AST_FORCE_UNWRAP` |
| `{` | Trailing closure (CALL_EXPR'e eklenir) |

Ornek: `foo.bar(42)` nasil parse edilir:

```
1. prefix: AST_IDENT_EXPR(foo)
2. postfix ".": AST_MEMBER_EXPR(.bar), child = ident(foo)
3. postfix "(": AST_CALL_EXPR, children = [member(.bar), integer(42)]
```

### Precedence Tablosu (pre.c)

Her operator'un iki sayisi var: **lbp** (sol baglama gucu) ve **rbp** (sag baglama gucu).

```c
// Ornek (basitlestirilmis):
// =   -> lbp=90,  rbp=90  (sag-birlesmeli atama)
// ||  -> lbp=110, rbp=111
// &&  -> lbp=120, rbp=121
// ==  -> lbp=130, rbp=131
// +   -> lbp=140, rbp=141
// *   -> lbp=150, rbp=151
```

`rbp = lbp + 1` demek **sol-birlesmeli** (sol taraf once baglanir).
`rbp = lbp` demek **sag-birlesmeli** (sag taraf once baglanir — atama gibi).

Custom operator'lar da desteklenir: `precedencegroup` ve `operator` declaration'lari parser'in tablosuna eklenir ve ayni Pratt dongusu icinde calisir.

---

## Declaration Parsing

### Modifiers (core.c)

Swift'te declaration'lar modifier ile baslar: `public static func ...`

`collect_modifiers()` bunlari teker teker tuketir ve bitmask'a toplar:

```c
uint32_t mods = collect_modifiers(p);
// mods = MOD_PUBLIC | MOD_STATIC
// Sonra: parse_func_decl(p, mods)
```

Ozel durumlar:
- `private(set)` — setter erisim kisitlamasi (4 token tuketir)
- `class func` / `class var` — `MOD_STATIC` olarak islenir (override edilebilir)

### func (decl/func.c)

8 adimli parsing:

```
func name<T>(params) async throws -> RetType where T: P { body }
      1   2    3       4     5        6          7         8
```

Her adim opsiyonel olabilir (protocol requirement'larda body yok, basit fonksiyonlarda generics yok).

### var/let (decl/var.c)

En karmasik declaration. 5 farkli form:

```swift
var x: Int = 42                          // stored
var x: Int { get { } set { } }           // computed
var x: Int { return expr }               // shorthand getter
var x: Int = 0 { willSet { } didSet { } } // observer
var x = 0, y = 0, z = 0                  // multi-var
```

Parser `{` gordigunde iceri peek eder: `get`/`set` mi, `willSet`/`didSet` mi, baska bir sey mi? Buna gore dogru alt-parser'i cagirir.

Multi-var declaration'lar `next_sibling` ile zincirlenir. `parse_block()` bu zinciri yuruyor ve her birini ayri child olarak ekliyor.

### Nominal Types (decl/decl.c)

class, struct, enum, protocol, actor, extension — hepsi `parse_nominal()` uzerinden gecer:

```swift
class Name<T>: Proto where T: P { body }
```

Fark sadece body'de:
- **protocol** -> `parse_protocol_body()` (sadece requirement'lar)
- **enum** -> `parse_enum_body()` (case declaration'lar + metotlar)
- **digerleri** -> `parse_block()` (genel amaçli)

---

## Statement Parsing (stmt.c)

Her statement keyword'u kendi parser'ina sahip:

| Keyword | Fonksiyon | Ozel durum |
|---------|-----------|------------|
| `if` | `parse_if()` | if-let, if-case, else-if zinciri |
| `guard` | `parse_guard()` | guard-let, guard-case |
| `for` | `parse_for()` | for-in, pattern matching |
| `while` | `parse_while()` | while-let |
| `switch` | `parse_switch()` | case clause'lar, default, where guard |
| `do` | `parse_do()` | do-catch-let |
| `return` | `parse_return()` | opsiyonel expression |
| `throw` | `parse_throw()` | zorunlu expression |
| `defer` | `parse_defer()` | block |

`if` ve `guard` ozel cunku **condition element** destekliyorlar:

```swift
if let x = optional, x > 0, case .some(let y) = foo { }
//  |_____________|  |_____|  |___________________|
//  optional binding  boolean   pattern matching
```

`parse_condition_element()` bu uc formu ayirt eder.

---

## Type Parsing (type.c)

Swift'in tip sistemi zengin — parser su formlari tanir:

| Form | Ornek | Sonuc |
|------|-------|-------|
| Basit | `Int` | `AST_TYPE_IDENT` |
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

Bunlarin bazilari ic ice gecebilir: `[String: [Int]?]` = dictionary, value'su optional array of int.

---

## Dosya Dosya Anlatim

### core.c — Temel Altyapi

Parser'in "beyni" degil ama "sinir sistemi". 544 satir. Icerir:

- `adv()`, `cur_char()`, `tok_text_eq()` — token navigasyonu
- `alloc_node()` — arena'dan node tahsisi
- `parse_error_push()` — hata kaydi
- `collect_modifiers()` — modifier bitmask toplama
- `skip_balanced()`, `skip_generic_params()` — dengeleme atlayicilari
- `parse_throws_clause()` — throws/rethrows/throws(ErrorType)
- `parse_hash_directive()` — #if/#else/#endif
- `parser_create()` / `parser_destroy()` — public lifecycle
- `parse_expression_from_cstring()` — string interpolation icin yardimci

### top.c — Dispatch

338 satir. Tek bir fonksiyon: `parse_decl_stmt()`. Her token'a bakip dogru parser'a yonlendirir. Attribute'lari (`@MainActor`), contextual keyword'leri (`nonisolated`, `indirect`, `convenience`), ve modifier zincirlerini isler.

### expression/pratt.c — Pratt Parser

86 satir. Tum expression parsing'in kalbi. Ternary (`? :`), is/as cast, ve binary operator'lari isler. Precedence climbing ile operator onceligi saglanir.

### expression/prefix.c — Sol Taraf

361 satir. Literal'ler, identifier'lar, gruplanmis expression'lar, array/dict literal'ler, closure'lar, try/await wrapper'lari.

### expression/postfix.c — Sag Taraf

243 satir. Fonksiyon cagrisi, member access, subscript, optional chaining, force unwrap, trailing closure.

### expression/pre.c — Precedence Tablosu

237 satir. Swift'in tum operator'larinin oncelik ve birlesme tablosu. Custom operator lookup.

### expression/closure.c — Closure Parser

193 satir. `{ [capture] (params) -> RetType in body }` formunu parse eder. Capture list, parameter list, ve closure body ayri ayri islenir.

### decl/func.c — Fonksiyon Benzeri Declaration'lar

275 satir. func, init, deinit, subscript. Hepsi ayni pattern: keyword + isim + generic + params + throws + return type + body.

### decl/var.c — Degisken Declaration'lari

245 satir. var/let'in 5 farkli formunu isler. Computed property ve observer parser'lari da burada.

### decl/decl.c — Block ve Nominal Type'lar

311 satir. parse_block (temel yapi tasi), import, typealias, enum body, parse_nominal (class/struct/enum/protocol/actor/extension).

### decl/operator.c — Operator Declaration'lari

161 satir. precedencegroup ve operator declaration'lari. Kendi dunyasi — baska hicbir parser tarafindan cagirilmaz.

### stmt.c — Statement'lar

486 satir. if, for, while, switch, guard, return, throw, defer, do-catch, break, continue, fallthrough.

### type.c — Tip Parse

726 satir. Swift'in tum tip formlarini parse eder. Recursive descent — `[String: [Int]?]` gibi ic ice tipler dogal olarak islenir.

### pattern.c — Pattern Matching

278 satir. case pattern'leri, tuple destructuring, optional pattern, is/as pattern.

---

## Hatalarimiz ve Cozumleri

| Hata | Ne oldu | Cozum |
|------|---------|-------|
| `1 + * 2` garbage AST | Pratt parser rhs=NULL'da devam ediyordu | NULL kontrolu + hata mesaji |
| Union clash | `data.binary.op_tok` closure'in `captures`'ini ezdi | `arg_label_tok` kullanildi |
| Sonsuz dongu | Unknown token'da `adv(p)` cagirilmiyordu | Progress guard: `pos == before -> adv(p)` |
| `last_error[256]` | Dead code — hicbir yerde okunmuyordu | Silindi |
| 755 satirlik decl.c | Operator parsing kayboluyordu | 4 dosyaya bolundu |
| `// Ch10: class var` | Kriptik yorum | Silindi, Doxygen ile aciklandi |

---

## Sonraki Bolum

-> [04 — Type Sistemi](04-type-sistemi-nedir.md)

---

*Bu belge [MiniSwiftFrontend](https://github.com/ugurtoprakdeviren/MiniSwiftFrontend) projesinin bir parcasidir.*
