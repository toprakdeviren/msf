# AST Nedir ve Nasıl Çalışır?

> Bu belge MiniSwiftFrontend'in AST katmanını sıfırdan açıklıyor.
> Önceki bölüm: [01 — Lexer Nedir?](01-lexer-nedir.md)

## Büyük Resim

Elimizde bir Swift dosyası var. Bunu analiz etmek için 3 adımdan geçiriyoruz:

```
"let x = 1 + 2"
       │
       ▼
 ┌──────────┐     ┌──────────┐     ┌──────────┐
 │  Lexer   │────▶│  Parser  │────▶│   Sema   │
 │ (token)  │     │  (AST)   │     │  (type)  │
 └──────────┘     └──────────┘     └──────────┘
```

1. **Lexer** — kaynak kodu token'lara ayırır: `let`, `x`, `=`, `1`, `+`, `2`
2. **Parser** — token'ları bir **ağaç** (tree) yapısına dönüştürür: **AST**
3. **Sema** — ağacın her node'una tip atar: `x` → `Int`, `1 + 2` → `Int`

Kütüphaneyi kullanan kişi bu 3 adımı tek satırda yapar:

```c
#include <msf.h>

MSFResult *r = msf_analyze("let x = 1 + 2", "main.swift");
```

Ama aşağıda bu adımların her biri nasıl çalışıyor, onu anlatacağız. Bu bölümde 2. adımın çıktısına — yani AST'ye — odaklanacağız.

---

## AST Nedir?

AST = **Abstract Syntax Tree** = Soyut Sözdizim Ağacı.

`let x = 1 + 2` yazdığında parser şu ağacı üretir:

```
source_file
  └── let_decl(x)
        ├── type_ident(Int)      ← tip (varsa)
        └── binary_expr(+)       ← init expression
              ├── integer_literal(1)
              └── integer_literal(2)
```

Her kutu bir **ASTNode**. Her node'un:
- Bir **kind**'ı var — ne olduğu (`AST_FUNC_DECL`, `AST_IF_STMT`, `AST_CALL_EXPR`, ...)
- **Children**'ları var — alt node'ları (linked list: `first_child → next_sibling → ...`)
- Opsiyonel bir **type**'ı var — sema tarafından doldurulur (`TypeInfo*`)

**"Abstract" neden?** Kaynak koddaki parantezler, noktalı virgüller, boşluklar ağaçta yok. Sadece anlam taşıyan yapı var. `(1 + 2)` ile `1 + 2` aynı ağacı üretir.

Bunu canlı görmek istersen:

```c
MSFResult *r = msf_analyze("let x = 1 + 2", NULL);
msf_dump_text(r, stdout);    // indented tree
msf_dump_json(r, stdout);    // JSON — Monaco, web UI
msf_dump_sexpr(r, stdout);   // S-expression — test, diff
msf_result_free(r);
```

---

## Kod Yapısı

AST ile ilgili kod iki dosyada yaşıyor:

```
src/
  ast/
    ast.c        ← bellek yönetimi + ağaç operasyonları  (~120 satır)
    ast_dump.c   ← serialization (text, JSON, S-expr)    (~280 satır)
```

Neden ayrı? **Single Responsibility Principle.** `ast.c` bellek verir, ağaç bağlar. `ast_dump.c` okur ve yazdırır. Yarın yeni bir output format eklemek istersen (GraphViz, XML) sadece `ast_dump.c`'ye dokunursun.

Bu ayrımı biz ilk başta yapmamıştık — her şey tek dosyaydı. Sonra fark ettik ki dosyanın %60'ı print kodu. Bölünce her iki dosya da çok daha okunur hale geldi.

Gerçek compiler'larda da böyle yapılır:
- **LLVM:** `AST.cpp` vs `ASTDumper.cpp`
- **Clang:** `Decl.cpp` vs `ASTDumper.cpp` + `JSONNodeDumper.cpp`
- **Swift compiler:** `ASTContext.cpp` vs `ASTDumper.cpp`

---

## `ast.c` — Altyapı

Bu dosya AST'nin **iskeletidir**. Node alloc eder, ağaca bağlar, kind ismini döner. Yazdırma, format, I/O — hiçbiri burada yok. 120 satır. Tek bir `fprintf` bile yok.

### Arena Allocator

**Problem:** Parser binlerce node üretir. Her biri için `malloc()` çağırmak yavaş ve bellek fragmentation'ı yaratır.

**Çözüm:** Chunk-based arena. Büyük bir blok al, node'ları sırayla yerleştir, sonunda hepsini bir kerede serbest bırak.

```c
#define AST_ARENA_CHUNK_SIZE 1024

typedef struct ASTArenaChunk {
  ASTNode nodes[1024];       // düz array — cache-friendly
  ASTArenaChunk *next;       // dolu olunca yeni chunk
} ASTArenaChunk;
```

Alloc fonksiyonu absürt derecede basit:

```c
ASTNode *ast_arena_alloc(ASTArena *a) {
  if (a->count >= AST_ARENA_CHUNK_SIZE) {
    // Chunk doldu → yeni chunk ekle
    ASTArenaChunk *chunk = calloc(1, sizeof(ASTArenaChunk));
    a->tail->next = chunk;
    a->tail = chunk;
    a->count = 0;
  }
  return &a->tail->nodes[a->count++];  // sadece index arttır
}
```

Neden arena?
- **Hız:** `malloc()` ~50ns, arena ~2ns (sadece index++)
- **Cache:** Tüm node'lar bitişik bellekte → CPU cache miss yok
- **Free:** Tek operasyon — chunk listesini dolaş, hepsini free et. Per-node free yok.

**Önemli tasarım kararı:** Arena, node'ların içeriğini **bilmez**. Sadece bellek verir ve geri alır. Bazı node'lar heap'te pointer tutabilir (mesela closure capture listesi). Bunlar ayrı bir `ast_arena_cleanup_payloads()` fonksiyonuyla temizlenir. Allocator ile iş mantığı karışmaz.

Biz bunu ilk başta yanlış yapmıştık — cleanup kodu arena'nın `free` fonksiyonunun içindeydi. Sonra anladık ki her yeni heap field eklediğimizde arena kodunu modifiye etmemiz gerekiyordu. Allocator, node semantiğinden haberdar olmamalı.

### Ağaç Operasyonları

Node'a çocuk eklemek O(1):

```c
void ast_add_child(ASTNode *parent, ASTNode *child) {
  child->parent = parent;
  child->next_sibling = NULL;

  if (!parent->first_child) {
    parent->first_child = child;
    parent->last_child = child;
  } else {
    parent->last_child->next_sibling = child;
    parent->last_child = child;
  }
}
```

`last_child` pointer'ı sayesinde listenin sonunu aramana gerek yok. Bu olmasa her ekleme O(n) olurdu.

### Kind Name Lookup

Her `ASTNodeKind` enum değerinin insan-okunur adı var:

```c
#include "ast_names.inc"   // @generated — "source_file", "func_decl", ...

const char *ast_kind_name(ASTNodeKind k) {
  if (k < 0 || k >= AST__COUNT) return "?";
  return AST_KIND_NAMES[k];  // O(1) array index
}
```

Bu tablo `data/ast_nodes.def` dosyasından `make codegen` ile otomatik üretilir.

---

## `ast_dump.c` — Serialization

Bu dosya AST'nin **tüketicisidir** — node'ları okur ve yazdırır. 3 format:

| Format | Ne zaman | Örnek |
|--------|----------|-------|
| **Text** | Debug, terminal | `func_decl(main)\n  brace_stmt\n    ...` |
| **JSON** | Web UI, Monaco editör | `{"kind":"func_decl","value":"main","children":[...]}` |
| **S-expr** | Test, tooling, diff | `(func_decl "main" (brace_stmt ...))` |

### Table-Driven Tasarım

**Problem:** 3 format × 130+ node kind = 390 switch case mı yazacağız?

**Hayır.** Her node kind'ı bir "dump mode"a map'lenir:

```c
typedef enum {
  DUMP_PLAIN,       // payload yok
  DUMP_FUNC_NAME,   // fonksiyon adı
  DUMP_VAR_NAME,    // değişken adı
  DUMP_OP,          // operatör
  DUMP_INT_LIT,     // integer değeri
  // ...
} DumpMode;

static const DumpMode dump_modes[AST__COUNT] = {
  [AST_FUNC_DECL]      = DUMP_FUNC_NAME,
  [AST_VAR_DECL]       = DUMP_VAR_NAME,
  [AST_BINARY_EXPR]    = DUMP_OP,
  [AST_INTEGER_LITERAL] = DUMP_INT_LIT,
  // ...
};
```

Sonra tek bir `extract_value()` fonksiyonu payload'ı çıkarır, ve her format sadece render eder:

```c
NodeValue v = extract_value(node, src, tokens);
// → v.kind = VAL_STRING, v.text = "main"
// → v.kind = VAL_INT, v.ival = 42
```

Yeni node kind eklemek = tabloya 1 satır. Yeni format eklemek = sadece renderer yazmak. İkisi birbirinden bağımsız.

---

## `ASTNode` Yapısı

```c
struct ASTNode {
  ASTNodeKind kind;        // ne tür: FUNC_DECL, VAR_DECL, IF_STMT, ...
  uint32_t    tok_idx;     // kaynak koddaki ilk token'ın indexi
  uint32_t    tok_end;     // son token (hata mesajları için)

  // Ağaç bağlantıları
  ASTNode *parent;         // üst node
  ASTNode *first_child;    // ilk çocuk
  ASTNode *last_child;     // son çocuk (O(1) ekleme)
  ASTNode *next_sibling;   // kardeş

  // Semantik bilgi (sema sonrası doldurulur)
  TypeInfo *type;          // NULL → sema çalışmadı
  uint32_t  modifiers;     // public, static, async, ...
  uint32_t  arg_label_tok; // fonksiyon çağrısında argument label

  // Node'a özel veri — kind'a göre hangi field aktif
  union {
    struct { uint32_t name_tok; }    func;     // FUNC_DECL
    struct { uint32_t name_tok; ... } var;     // VAR_DECL
    struct { int64_t ival; }          integer;  // INTEGER_LITERAL
    struct { double fval; }           flt;      // FLOAT_LITERAL
    struct { uint32_t op_tok; }       binary;   // BINARY_EXPR
    // ...
  } data;
};
```

**Neden children linked-list, array değil?**
- Parser ağacı inşa ederken kaç çocuk olacağını bilmiyor
- Linked-list ekleme O(1), array ekleme O(n) (realloc)
- Ağaç derinliği az (5-10 seviye) — traverse maliyeti düşük

**Neden union?**
Tüm field'lar aynı bellek alanını paylaşır. `ASTNode` ~120 byte kalır (union olmasaydı ~300).

Ama **tehlikeli**: yanlış field'a yazarsan başka field'ı ezersin. Biz bunu canlıda yaşadık — multi-trailing closure'da `data.binary.op_tok` yazarak `data.closure.captures` pointer'ını ezdik, derleyici `SIGSEGV` ile çöktü.

**Ders:** `node->kind == AST_CLOSURE_EXPR` ise `node->data.binary`'ye **asla** dokunma.

**Neden `tok_idx`, string kopyası değil?**
Node'lar kaynak koddan string kopyalamaz. Token stream'deki index'i tutar:

```c
node->data.func.name_tok = 42;  // token #42 = "main"
// İsim lazım olunca: tokens[42].pos → kaynak kodun o offset'i
```

Avantaj: Sıfır allocation, orijinal satır/sütun bilgisi mevcut.
Trade-off: Kaynak kod ve token stream, AST yaşadığı sürece bellekte kalmalı.

---

## Dersleri Öğrendiğimiz Hatalar

Bu projeyi geliştirirken yaptığımız hatalar ve çözümleri:

| Hata | Ne oldu | Çözüm |
|------|---------|-------|
| Arena'da `exit()` | OOM'da tüm process ölüyordu | Arena zeroed bırakılır, `alloc` NULL döner |
| Arena'da `perror()` | stderr'a yazdırıyorduk | Kütüphane stderr'a yazmamalı — kaldırıldı |
| Cleanup arena'da | Her yeni heap field = arena modifikasyonu | `cleanup_payloads()` ayrı fonksiyon |
| Union clash | `data.binary` ile `data.closure` çakıştı | Union dışındaki `arg_label_tok` kullanıldı |
| Her şey tek dosya | `ast.c` %60 print kodu | `ast.c` + `ast_dump.c` ayrımı |
| `#include` fonksiyon içinde | `ast_kind_name()` içinde `#include` | Dosya scope'una taşındı |

---

## Kullanıcı Olarak AST ile Ne Yaparsın?

```c
#include <msf.h>

// 1. Analiz et
MSFResult *r = msf_analyze("func add(a: Int, b: Int) -> Int { return a + b }", NULL);

// 2. Hata var mı?
for (uint32_t i = 0; i < msf_error_count(r); i++)
    fprintf(stderr, "%u:%u: %s\n",
            msf_error_line(r, i), msf_error_col(r, i),
            msf_error_message(r, i));

// 3. Ağacı gez
const ASTNode *root = msf_root(r);
for (const ASTNode *c = root->first_child; c; c = c->next_sibling)
    printf("  %s\n", ast_kind_name(c->kind));  // "func_decl"

// 4. JSON olarak dump et
msf_dump_json(r, stdout);

// 5. Tip bilgisi oku
char buf[64];
printf("type: %s\n", type_to_string(root->first_child->type, buf, sizeof(buf)));

// 6. Temizle
msf_result_free(r);
```

Dikkat: arena, token stream, parser, sema — hiçbirini kendin oluşturmuyorsun. `msf_analyze()` hepsini yapıyor, `msf_result_free()` hepsini temizliyor.

---

## Sonraki Bölüm

→ [03 — Parser: Token'lardan AST'ye](03-parser-nasil-calisir.md)

---

*Bu belge [MiniSwiftFrontend](https://github.com/ugurtoprakdeviren/MiniSwiftFrontend) projesinin bir parçasıdır.*
