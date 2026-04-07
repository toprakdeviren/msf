# Lexer Nedir ve Nasil Calisir?

> Bu belge MiniSwiftFrontend'in lexer katmanini sifirdan acikliyor.
> Hedef kitle: C bilen, compiler yazmak isteyen ama nereden baslayacagini bilmeyen gelistiriciler.

## Lexer Ne Yapar?

Elimizde bir metin var:

```
let x = 1 + 2
```

Lexer bu metni okur ve **token** listesine donusturur:

```
KEYWORD     "let"
IDENTIFIER  "x"
OPERATOR    "="
INTEGER_LIT "1"
OPERATOR    "+"
INTEGER_LIT "2"
EOF
```

Her token sunu bilir: **ne tur** (keyword mi, identifier mi, sayi mi), kaynakta **nerede** (satir, sutun, byte offset), ve **ne kadar uzun** (byte cinsinden).

Bunu canli gormek istersen:

```c
#include <msf.h>

Source src = { .data = "let x = 1 + 2", .len = 13, .filename = "demo" };
TokenStream ts;
token_stream_init(&ts, 64);
lexer_tokenize(&src, &ts, 1, NULL);   // skip_ws=1: bosluklari atla

for (size_t i = 0; i < ts.count; i++) {
    Token t = ts.tokens[i];
    printf("%-12s  \"%.*s\"  line:%u col:%u\n",
           token_type_name(t.type),
           (int)t.len, src.data + t.pos,
           t.line, t.col);
}

token_stream_free(&ts);
```

Cikti:

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

## Pipeline'daki Yeri

```
"let x = 1 + 2"
       |
       v
 +----------+     +----------+     +----------+
 |  LEXER   |---->|  Parser  |---->|   Sema   |
 |  <- biz  |     |  (AST)   |     |  (type)  |
 +----------+     +----------+     +----------+
       |
       v
   Token listesi
```

Lexer, kaynak kodu **karakter karakter** okur ve anlamli parcalara boler. Parser bu token listesini alir ve agac (AST) insa eder. Lexer olmasa parser her seferinde "bu byte bosluk mu, harf mi, rakam mi?" diye dusunmek zorunda kalirdi.

---

## Kod Yapisi

Lexer 7 dosyadan olusur. Her dosya tek bir is yapar:

```
src/lexer/
  lexer_private.h  (109 satir) -- dosyalar arasi paylasilan macro ve bildirimler
  lexer.c          (165 satir) -- giris noktasi: init, next, tokenize
  scan_comment.c   ( 77 satir) -- // ve /* */ tarayicilari
  scan_string.c    (194 satir) -- "...", """...""", #"..."# tarayicilari
  scan_symbol.c    (102 satir) -- operator, regex, noktalama tarayicisi
  helpers.c        ( 96 satir) -- tablolar ve yardimci fonksiyonlar
  diag.c           ( 85 satir) -- hata kayit mekanizmasi
  token.c          (126 satir) -- token tipi adlari, metin cikarma, stream yonetimi
```

**Neden 7 dosya?** Cunku lexer ilk basta 800 satirlik tek bir dosyaydi. Her seyi ayni yere koymak "bul ve duzelt" yaklasimini zorlastiriyordu. Simdi her dosyanin adina bakarak ne yaptigini anlayabilirsin.

Veri akisi:

```
lexer.c  (dispatch)
  |
  +--- scan_comment.c  (// ve /* */)
  +--- scan_string.c   ("...", """...""", #"..."#)
  +--- scan_symbol.c   (operatorler, regex, noktalama)
  |
  +--- helpers.c       (hepsi kullanir)
  +--- diag.c          (hepsi kullanir)
  |
  v
token.c  (uretilen token'lari tuketir)
```

---

## Token Nedir?

Bir `Token` struct'i soyle gorunur:

```c
typedef struct {
  TokenType type;      // ne tur: keyword, identifier, int_lit, operator, ...
  uint32_t  pos;       // kaynak koddaki byte offset
  uint32_t  len;       // byte uzunlugu
  uint32_t  line;      // satir (1-based)
  uint32_t  col;       // sutun (1-based)
  Keyword   keyword;   // hangi keyword (KW_FUNC, KW_LET, ...) -- KW_NONE ise keyword degil
  OpKind    op_kind;   // hangi cok-karakterli operator (OP_EQ, OP_ARROW, ...) -- OP_NONE ise tek-char
} Token;
```

**Onemli:** Token, kaynak koddan string **kopyalamaz**. Sadece `pos` ve `len` tutar. Asil metin `source.data[tok.pos .. tok.pos + tok.len]`'da. Bu sayede:

- Sifir allocation (milyonlarca token bile heap kullanmaz)
- Hata mesajlarinda orijinal satir/sutun bilgisi mevcut
- Trade-off: kaynak kod metin, token'lar yasadigi surece bellekte kalmali

---

## Dosya Dosya Anlatim

### 1. `lexer_private.h` -- Ortak Altyapi

Bu dosya disariya acilmaz (public header degil). Lexer'in .c dosyalari arasinda paylasilan her sey burada:

**Pattern macro'lari** -- tekrar eden karakter kontrollerini isimlendirir:

```c
#define IS_NEWLINE(c)           ((c) == '\n' || (c) == '\r')
#define IS_TRIPLE_QUOTE(s, p)   ((s)[(p)] == '"' && (s)[(p)+1] == '"' && (s)[(p)+2] == '"')
#define IS_RAW_STRING_START(c)  ((c) == '#')
#define IS_BLOCK_OPEN(s, p, len)   (/* s[p..p+1] == / * */)
#define IS_BLOCK_CLOSE(s, p, len)  (/* s[p..p+1] == * / */)
```

**State advance macro'lari** -- lexer'in pos/line/col guncellemesini standartlastirir:

```c
#define ADVANCE(l)           // pos++, col++       (ayni satirda 1 byte ilerle)
#define ADVANCE_BY(l, n)     // pos += n, col += n (ayni satirda n byte ilerle)
#define NEWLINE_ADVANCE(l)   // pos++, line++, col = 1 (yeni satira gec)
```

**Hata mesaji sabitleri** -- tum scanner'lar ayni stringleri kullanir:

```c
#define DIAG_UNTERM_STRING       "unterminated string literal"
#define DIAG_UNTERM_TRIPLE       "unterminated triple-quoted string literal"
#define DIAG_UNTERM_RAW_STRING   "unterminated raw string literal"
#define DIAG_UNTERM_BLOCK_COMMENT "unterminated block comment"
#define DIAG_INVALID_ESCAPE      "invalid escape sequence in string literal"
#define DIAG_SINGLE_QUOTE        "single-quoted string is not allowed; use double quotes"
```

Neden macro? Cunku ayni mesaji degistirmek istediginde tek yere dokunman yeter. Ve testlerde `strcmp(diag.message[0], DIAG_UNTERM_STRING)` yazabilirsin.

**Scanner bildirimleri** -- her .c dosyasindaki fonksiyonlar burada ilan edilir, boylece birbirlerini cagirabillirler (ornegin `scan_symbol` -> `scan_line_comment`).

---

### 2. `lexer.c` -- Giris Noktasi

Bu dosya lexer'in "beyni". 3 fonksiyon var:

**`lexer_init(l, src)`** -- Lexer state'ini sifirlar: pos=0, line=1, col=1.

**`lexer_next(l)`** -- Her cagrida bir token doner. Table-driven dispatch:

```c
uint8_t c   = source[pos];
uint8_t cls = LEX_CHAR_CLASS[c];     // 256-byte tablo: byte -> karakter sinifi
uint8_t act = LEX_ACTION[cls];       // sinif -> aksiyon (0-4)
```

| Aksiyon | Karakter sinifi | Ne yapar |
|---------|----------------|----------|
| -- | `CC_NEWLINE` | Satir arttir, `TOK_NEWLINE` |
| 0 | `CC_WHITESPACE` | Bitisik bosluklari yut, `TOK_WHITESPACE` |
| 1 | Harf/`_`/`$`/Unicode | `scan_ident()` + keyword lookup |
| 2 | Rakam | `scan_number()` |
| 3 | `"` veya `'` | `scan_string()` -> scan_string.c |
| -- | `#` | `scan_raw_string()` -> scan_string.c |
| 4 | Diger | `scan_symbol()` -> scan_symbol.c |

**Neden table-driven?** 256-byte lookup tablosu CPU cache'ine sigar. Her karakter siniflandirma O(1). `if (c >= 'a' && c <= 'z' || c >= 'A' ...)` zincirinden cok daha hizli.

**`lexer_tokenize(src, out, skip_ws, diag)`** -- `lexer_next()`'i dongude cagirir. Token'lari stream'e push eder. Ekstra isler:

- **Whitespace filtreleme:** `skip_ws=1` ise bosluk/yorum/newline atlanir
- **Circuit breaker:** `4 * source_len` iterasyondan sonra durur (sonsuz dongu korumasi)
- **EOF garantisi:** Stream her zaman `TOK_EOF` ile biter
- **Shrink-to-fit:** Fazla ayrilmis bellegi geri verir

---

### 3. `scan_comment.c` -- Yorum Tarayicilari

Iki fonksiyon, iki yorum stili:

**`scan_line_comment()`** -- `// ...` satirsonuna kadar.

```c
// Bu bir line comment
```

`memchr` ile ilk newline'i bulur -- yorum govdesini byte-byte yurumez, tek cagrida atlar. Newline'in kendisini **yemez** -- o ayri bir `TOK_NEWLINE` olur.

**`scan_block_comment()`** -- `/* ... */` ic ice gelebilir.

```c
/* dis /* ic */ hala dis */
```

`depth` sayaci tutar: her `/*` arttirir, her `*/` azaltir. depth=0 olunca biter. EOF'a ulasirsa `DIAG_UNTERM_BLOCK_COMMENT` kaydeder.

Ic ice destek neden onemli? Cunku kod bloklarini yorum icine alirken o blokta zaten `/* */` varsa, ic ice destegi olmayan lexer'lar ilk `*/`'da kapanir ve geri kalan kod aciliga kalir.

```
/* ic ice olmayan lexer bunu yanlis parse eder:
   /* ic yorum */   <-- burda kapanir
   kalan kod        <-- bu yorum disinda kalir
*/
```

---

### 4. `scan_string.c` -- String Tarayicilari

Swift'in en karmasik token turleri buradadir. 4 cesit string var:

| Tur | Ornek | Fonksiyon |
|-----|-------|-----------|
| Regular | `"hello"` | `scan_string()` |
| Triple-quoted | `"""multi\nline"""` | `scan_string()` |
| Raw | `#"no \escape"#` | `scan_raw_string()` |
| Raw triple | `#"""raw multi"""#` | `scan_raw_string()` |

**`validate_string_escapes(s, start, end, ...)`**

String taranan sonra cagrilir. Govdeyi yurur, her backslash'i kontrol eder:

```
\n \t \r \0 \" \' \\    -- basit escape'ler
\(expr)                  -- string interpolation (dengeli parantez sayar)
\u{XXXX}                 -- Unicode escape (en az 1 hex digit + kapanan })
```

Gecersiz escape bulursa (`\q` gibi) pozisyonu kaydeder ve 0 doner.

**`scan_string()`** -- Regular ve triple-quoted string'leri tarar.

Triple-quote tespiti `IS_TRIPLE_QUOTE` macro'suyla yapilir. Unterminated durumda EOF'a kadar olan kismi token olarak doner (crash etmez, diagnostic uretir).

Tek tirnak (`'hello'`) kullanimini da yakalar ve "use double quotes" uyarisi verir.

**`scan_raw_string()`** -- `#` sayisini sayar, acilis/kapanis eslestirir.

```swift
##"bu icinde " ve # olabilir"##
```

Acilista 2 tane `#` varsa, kapanis da `"##` olmali. `#` sayisi (`hc`) eslesene kadar aramaya devam eder. Triple-quoted raw string'leri de destekler (`#"""..."""#`).

Eger `#` dan sonra `"` gelmezse, bu raw string degildir -- sifir uzunluklu sentinel token doner ve `lexer_next()` baska bir scanner dener.

---

### 5. `scan_symbol.c` -- Operator ve Noktalama

Bu dosya "geri kalan her sey" icin catch-all dispatcher:

**`scan_symbol()`** -- Dispatch sirasi (ilk eslesme kazanir):

```
1. "//" -> scan_line_comment()  (scan_comment.c'ye delege)
2. "/*" -> scan_block_comment() (scan_comment.c'ye delege)
3. Multi-char operator? -> MULTI_OPS[] tablosunu tara
4. "/" + non-delimiter? -> scan_regex_literal() dene
5. Tek karakter -> SW_CHAR_TOKEN[] tablosuna bak
```

**Multi-char operatorler** -- `MULTI_OPS[]` tablosu (helpers.c'de) en uzun operatoru once dener:

```c
{"===", 3, OP_IDENTITY_EQ},   // 3 karakter -- once
{"==",  2, OP_EQ},             // 2 karakter -- sonra
{"->",  2, OP_ARROW},
{"??",  2, OP_NIL_COAL},
```

Sira onemli: `===` `==`'den once denenmezse, `===` hic eslesmez.

**`scan_regex_literal()`** -- `/pattern/` formunu tarar.

Regex ile bolme operatoru (`/`) belirsizdir. Heuristik: eger `/`'den sonra bosluk, kapanan parantez, virgul, noktali virgul veya iki nokta **gelmiyorsa** regex olarak dener. Kapanan `/` bulamazsa basarisiz doner ve `/` tek-karakter operator olarak islenir.

Regex icinde backslash-escape destekler (`/a\/b/`). Newline gorurse basarisiz doner -- Swift regex literal'lari cok satirli olamaz.

---

### 6. `helpers.c` -- Yardimci Fonksiyonlar

Scanner'larin paylastigi 3 arac:

**`MULTI_OPS[]` tablosu** -- Cok-karakterli operatorlerin lookup tablosu. `.rodata` section'inda yasiar, runtime maliyeti sifir, initialization gerektirmez. `scan_symbol()` bunu lineer tarer.

**`make_string_tok(sp, tl, sl, sc)`** -- `TOK_STRING_LIT` token'i olusturur. Her string scanner'da 7-field compound literal yazmak yerine bu helper cagrilir. `keyword` ve `op_kind` her zaman `KW_NONE` / `OP_NONE`.

**`advance_past_token(l, s, tok_start, tok_len, extra_lines)`** -- Multiline token'dan sonra lexer state'ini gunceller.

String ve comment'ler birden fazla satira yayilabilir. Bu durumda `line` ve `col` guncellenmeli:

```c
// """hello\nworld""" taradiktan sonra:
advance_past_token(l, s, start, 18, 1);
// l->line += 1
// l->col = son \n'den sonraki karakter sayisi
// l->pos = start + 18
```

Bu helper olmadan ayni col hesaplama mantigi 4 farkli yerde tekrarlaniyordu (triple-quoted, regular, raw string, unterminated). Simdi tek yer.

---

### 7. `diag.c` -- Hata Kayit Mekanizmasi

Lexer hatada durmaz -- sorunu kaydeder ve taramaya devam eder. Bu dosya kayit mekanizmasini saglar.

**`lexer_diag_init(d)`** -- Sayaci sifirlar. `lexer_tokenize()` oncesi cagir.

**`lexer_diag_record(l, line, col, fmt, ...)`** -- Scanner'lar tarafindan cagrilir (internal). `printf` formati destekler. `l->diag` NULL ise sessizce atlar.

**`lexer_diag_push(d, line, col, fmt, ...)`** -- Parser tarafindan cagrilir (public). Ayni diagnostic struct'a yazar. Ornegin parser "expected expression after operator" uyarisi ekleyebilir.

Her iki fonksiyon da ayni `LexerDiagnostics` struct'ina yazar: `(message, line, col)` uclulerinden olusan bir dizi. `LEXER_DIAG_MAX` sinirinda -- dizi dolunca ek hatalar sessizce atilir.

Tuketim:

```c
LexerDiagnostics diag;
lexer_diag_init(&diag);
lexer_tokenize(&src, &ts, 1, &diag);

for (size_t i = 0; i < diag.count; i++)
    printf("%u:%u: %s\n", diag.line[i], diag.col[i], diag.message[i]);
```

---

### 8. `token.c` -- Token Utility'leri

Lexer'dan bagimsiz -- sadece `Token` ve `TokenStream` uzerinde calisir. Lexer **uretir**, token.c **tuketir**.

**`token_type_name(t)`** -- Enum'dan insan-okunabilir isme: `TOK_KEYWORD` -> `"keyword"`, `TOK_EOF` -> `"eof"`. Tablo-driven (designated initializer array).

**`token_text(src, tok)`** -- Token'in kaynak koddaki metnini NUL-terminated string olarak doner.

```c
const char *text = token_text(src, &tok);
printf("Token metni: %s\n", text);
```

**Dikkat:** Thread-local buffer kullanir. Her cagri oncekini ezer. Su kod hatalidir:

```c
// HATALI -- ikisi de ayni buffer'a yazar, ikisi de b'nin metnini gosterir:
printf("%s %s", token_text(src, &a), token_text(src, &b));
```

Dogru kullanim: ya arayi kopyala (`strdup`), ya da ayri `printf` cagrilari yap.

**`token_stream_init(ts, cap)` / `token_stream_free(ts)` / `token_stream_push(ts, tok)`**

Token stream'i bir dinamik dizidir. `push` 1.5x buyume stratejisi kullanir (amortized O(1)). OOM'da token sessizce atilir -- mevcut token'lar korunur, process olmez.

---

## Identifier + Keyword Tarama

`lexer_next()` identifer action'ina girince `scan_ident()` cagrilir (external, char_tables.h'de). Iki asamali:

**ASCII fast-path (SWAR):** 8 byte'i bir kerede oku, bitmap'le kontrol et. Tamamen ASCII identifier'lar (kodun %95'i) bu yoldan gecer -- byte-by-byte loop'a hic girmez.

```c
// 8 byte'lik SWAR: tum byte'lar ASCII identifier char mi?
while (pos + 8 <= len) {
  uint64_t word;
  memcpy(&word, src + pos, 8);
  if (word & 0x8080808080808080ULL) break;  // non-ASCII -> yavas yola gec
  if (!all_ident_chars(word)) break;         // non-ident char -> dur
  pos += 8;
}
```

**Unicode fallback:** Non-ASCII byte gelince UTF-8 decode et, Swift'in Unicode range tablolarina bak (`unicode_ranges.h`). Ilk karakter `identifier-head`, devami `identifier-continue` olmali.

Identifier tarandiktan sonra **keyword mi?** FNV-1a hash -> 2-probe lookup:

```c
uint32_t kw_id = keyword_detect(src + start, len);
Keyword kw = map_kw_id(kw_id);
// kw == KW_FUNC ise "func" keyword'u, KW_NONE ise identifier
```

---

## Hatalarimiz ve Cozumleri

| Hata | Ne oldu | Cozum |
|------|---------|-------|
| `token_stream_push` exit() | OOM'da process oluyordu | Sessiz drop -- mevcut token'lar korunur |
| 4x copy-paste col tracking | Multiline string'de col yanlis hesaplaniyordu | `advance_past_token()` helper |
| `size_t -> uint32_t` truncation | >4GB dosyada `len` wrap ederdi | `LEXER_MAX_SOURCE_LEN` guard |
| Regex false positive | `a/b` division'i regex olarak deneniyordu | Heuristik sikilastirildi |
| Magic string'ler | Hata mesajlari 6 yerde hardcoded | `DIAG_*` sabitlerine cikarildi |
| Tekrar eden pattern'ler | `l->pos++; l->col++` 10+ yerde | `ADVANCE()`, `ADVANCE_BY()` macro'lari |
| Her sey tek dosya | 800 satirlik dosyada bug bulmak zordu | 7 dosyaya bolundu |
| String interpolation | `\(expr)` 2-char escape olarak goruluyordu | Dengeli parantez sayaci |

---

## Sonraki Bolum

-> [02 -- AST Nedir ve Nasil Calisir?](02-ast-nedir.md)

---

*Bu belge [MiniSwiftFrontend](https://github.com/ugurtoprakdeviren/msf) projesinin bir parcasidir.*
