/**
 * @file types.h
 * @brief Core type definitions, enumerations, and data structures for the Decoder library.
 *
 * This header defines all public types used across the Decoder API:
 * version constants, status codes, General Category, Normalization Forms,
 * Script identifiers, break properties (Grapheme/Word/Sentence),
 * confusable types, and iterator/result structures.
 *
 * Included automatically by all other decoder headers.
 */
#ifndef DECODER_TYPES_H
#define DECODER_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Compile-Time Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Library major version. */
#define DECODER_VERSION_MAJOR 1
/** @brief Library minor version. */
#define DECODER_VERSION_MINOR 1
/** @brief Library patch version. */
#define DECODER_VERSION_PATCH 0
/** @brief Library version as a string (e.g. "1.1.0"). */
#define DECODER_VERSION_STRING "1.1.0"
/** @brief Unicode Standard version supported (e.g. "17.0.0"). */
#define DECODER_STANDARD_VERSION "17.0.0"
/** @brief Full version string including Unicode version. */
#define DECODER_VERSION_FULL "1.1.0 (Unicode 17.0)"
/** @brief ABI version for binary compatibility checks. */
#define DECODER_ABI_VERSION 1

/**
 * @brief Compile-time version check macro.
 *
 * Evaluates to true if the library version is at least major.minor.patch.
 *
 * @code
 * #if DECODER_VERSION_CHECK(1, 1, 0)
 *   // Use features from v1.1.0+
 * #endif
 * @endcode
 */
#define DECODER_VERSION_CHECK(major, minor, patch)                            \
    ((DECODER_VERSION_MAJOR > (major)) ||                                     \
     (DECODER_VERSION_MAJOR == (major) && DECODER_VERSION_MINOR > (minor)) || \
     (DECODER_VERSION_MAJOR == (major) && DECODER_VERSION_MINOR == (minor) && \
      DECODER_VERSION_PATCH >= (patch)))

/** @brief Set to 1 if SIMD acceleration is compiled in. */
#define DECODER_FEATURE_SIMD 1
/** @brief Set to 1 if parallel (multi-threaded) operations are compiled in. */
#define DECODER_FEATURE_PARALLEL 1
/** @brief Sentinel value indicating "no block" in block queries. */
#define DECODER_BLOCK_NO_BLOCK 0

/* ═══════════════════════════════════════════════════════════════════════════
 *  Status Codes
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Return status codes for decoder functions.
 *
 * All functions that can fail return a decoder_status_t. Success is 0;
 * errors are negative values. Use decoder_error_message() to get a
 * human-readable description.
 */
typedef enum {
    DECODER_SUCCESS = 0,                  /**< Operation completed successfully. */
    DECODER_ERROR_INVALID_INPUT = -1,     /**< Input pointer is NULL or input is malformed. */
    DECODER_ERROR_BUFFER_TOO_SMALL = -2,  /**< Output buffer is too small. */
    DECODER_ERROR_INVALID_UTF8 = -3,      /**< Input is not valid UTF-8. */
    DECODER_ERROR_INVALID_UTF16 = -4,     /**< Input is not valid UTF-16. */
    DECODER_ERROR_INVALID_CODEPOINT = -5, /**< Code point is out of range or a surrogate. */
    DECODER_ERROR_OUT_OF_MEMORY = -6,     /**< Memory allocation failed. */
    DECODER_ERROR_NOT_IMPLEMENTED = -7,   /**< Feature is not yet implemented. */
    DECODER_ERROR_IO = -8,                /**< I/O error (file operations). */
    DECODER_ERROR_INVALID_ARGUMENT = -9,  /**< Invalid argument value. */
    DECODER_ERROR_OVERFLOW = -10,         /**< Integer overflow during computation. */
    DECODER_ERROR_NOT_FOUND = -11         /**< Requested item not found. */
} decoder_status_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  General Category (UAX #44, §5.7.1)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Unicode General Category values.
 *
 * Each code point has exactly one General Category. The two-letter
 * abbreviation is shown in brackets for reference.
 */
typedef enum {
    DECODER_CATEGORY_UNASSIGNED = 0,        /**< [Cn] Not assigned. */
    DECODER_CATEGORY_UPPERCASE_LETTER,      /**< [Lu] Uppercase letter. */
    DECODER_CATEGORY_LOWERCASE_LETTER,      /**< [Ll] Lowercase letter. */
    DECODER_CATEGORY_TITLECASE_LETTER,      /**< [Lt] Titlecase letter (e.g. Dž). */
    DECODER_CATEGORY_MODIFIER_LETTER,       /**< [Lm] Modifier letter. */
    DECODER_CATEGORY_OTHER_LETTER,          /**< [Lo] Other letter (e.g. CJK ideographs). */
    DECODER_CATEGORY_NONSPACING_MARK,       /**< [Mn] Non-spacing combining mark. */
    DECODER_CATEGORY_SPACING_MARK,          /**< [Mc] Spacing combining mark. */
    DECODER_CATEGORY_ENCLOSING_MARK,        /**< [Me] Enclosing combining mark. */
    DECODER_CATEGORY_DECIMAL_NUMBER,        /**< [Nd] Decimal digit. */
    DECODER_CATEGORY_LETTER_NUMBER,         /**< [Nl] Letter number (e.g. Roman numerals). */
    DECODER_CATEGORY_OTHER_NUMBER,          /**< [No] Other number (e.g. fractions). */
    DECODER_CATEGORY_CONNECTOR_PUNCTUATION, /**< [Pc] Connector (e.g. underscore). */
    DECODER_CATEGORY_DASH_PUNCTUATION,      /**< [Pd] Dash. */
    DECODER_CATEGORY_OPEN_PUNCTUATION,      /**< [Ps] Open bracket. */
    DECODER_CATEGORY_CLOSE_PUNCTUATION,     /**< [Pe] Close bracket. */
    DECODER_CATEGORY_INITIAL_PUNCTUATION,   /**< [Pi] Initial quote. */
    DECODER_CATEGORY_FINAL_PUNCTUATION,     /**< [Pf] Final quote. */
    DECODER_CATEGORY_OTHER_PUNCTUATION,     /**< [Po] Other punctuation. */
    DECODER_CATEGORY_MATH_SYMBOL,           /**< [Sm] Math symbol. */
    DECODER_CATEGORY_CURRENCY_SYMBOL,       /**< [Sc] Currency symbol. */
    DECODER_CATEGORY_MODIFIER_SYMBOL,       /**< [Sk] Modifier symbol. */
    DECODER_CATEGORY_OTHER_SYMBOL,          /**< [So] Other symbol. */
    DECODER_CATEGORY_SPACE_SEPARATOR,       /**< [Zs] Space separator. */
    DECODER_CATEGORY_LINE_SEPARATOR,        /**< [Zl] Line separator (U+2028). */
    DECODER_CATEGORY_PARAGRAPH_SEPARATOR,   /**< [Zp] Paragraph separator (U+2029). */
    DECODER_CATEGORY_CONTROL,               /**< [Cc] Control character. */
    DECODER_CATEGORY_FORMAT,                /**< [Cf] Format character (e.g. ZWJ, ZWNJ). */
    DECODER_CATEGORY_SURROGATE,             /**< [Cs] Surrogate (not a scalar value). */
    DECODER_CATEGORY_PRIVATE_USE            /**< [Co] Private use. */
} decoder_category_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Normalization (UAX #15)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Unicode Normalization Forms.
 * @see https://unicode.org/reports/tr15/
 */
typedef enum {
    DECODER_NFC = 0,  /**< Canonical Decomposition, followed by Canonical Composition. */
    DECODER_NFD = 1,  /**< Canonical Decomposition. */
    DECODER_NFKC = 2, /**< Compatibility Decomposition, followed by Canonical Composition. */
    DECODER_NFKD = 3  /**< Compatibility Decomposition. */
} decoder_normalization_form_t;

/**
 * @brief Quick Check result for normalization tests.
 *
 * Used by decoder_quick_check() to provide a fast (but possibly inconclusive)
 * answer about whether a string is normalized.
 */
typedef enum {
    DECODER_MAYBE = 0, /**< Inconclusive; a full check is needed. */
    DECODER_YES,       /**< Definitely normalized. */
    DECODER_NO         /**< Definitely not normalized. */
} decoder_quick_check_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Script (UAX #24)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Unicode Script property values (UAX #24).
 *
 * Covers all scripts defined in Unicode 17.0. The special values
 * Common and Inherited are used for characters shared across scripts
 * (e.g. digits, combining marks).
 */
typedef enum {
    DECODER_SCRIPT_UNKNOWN = 0,
    DECODER_SCRIPT_COMMON, /**< Characters used by multiple scripts (e.g. digits, punctuation). */
    DECODER_SCRIPT_INHERITED, /**< Combining marks that inherit the script of their base. */
    DECODER_SCRIPT_LATIN,
    DECODER_SCRIPT_GREEK,
    DECODER_SCRIPT_CYRILLIC,
    DECODER_SCRIPT_ARMENIAN,
    DECODER_SCRIPT_HEBREW,
    DECODER_SCRIPT_ARABIC,
    DECODER_SCRIPT_SYRIAC,
    DECODER_SCRIPT_THAANA,
    DECODER_SCRIPT_DEVANAGARI,
    DECODER_SCRIPT_BENGALI,
    DECODER_SCRIPT_GURMUKHI,
    DECODER_SCRIPT_GUJARATI,
    DECODER_SCRIPT_ORIYA,
    DECODER_SCRIPT_TAMIL,
    DECODER_SCRIPT_TELUGU,
    DECODER_SCRIPT_KANNADA,
    DECODER_SCRIPT_MALAYALAM,
    DECODER_SCRIPT_SINHALA,
    DECODER_SCRIPT_THAI,
    DECODER_SCRIPT_LAO,
    DECODER_SCRIPT_TIBETAN,
    DECODER_SCRIPT_MYANMAR,
    DECODER_SCRIPT_GEORGIAN,
    DECODER_SCRIPT_HANGUL,
    DECODER_SCRIPT_ETHIOPIC,
    DECODER_SCRIPT_CHEROKEE,
    DECODER_SCRIPT_CANADIAN_ABORIGINAL,
    DECODER_SCRIPT_OGHAM,
    DECODER_SCRIPT_RUNIC,
    DECODER_SCRIPT_KHMER,
    DECODER_SCRIPT_MONGOLIAN,
    DECODER_SCRIPT_HIRAGANA,
    DECODER_SCRIPT_KATAKANA,
    DECODER_SCRIPT_BOPOMOFO,
    DECODER_SCRIPT_HAN,
    DECODER_SCRIPT_YI,
    DECODER_SCRIPT_OLD_ITALIC,
    DECODER_SCRIPT_GOTHIC,
    DECODER_SCRIPT_DESERET,
    DECODER_SCRIPT_TAGALOG,
    DECODER_SCRIPT_HANUNOO,
    DECODER_SCRIPT_BUHID,
    DECODER_SCRIPT_TAGBANWA,
    DECODER_SCRIPT_LIMBU,
    DECODER_SCRIPT_TAI_LE,
    DECODER_SCRIPT_LINEAR_B,
    DECODER_SCRIPT_UGARITIC,
    DECODER_SCRIPT_SHAVIAN,
    DECODER_SCRIPT_OSMANYA,
    DECODER_SCRIPT_CYPRIOT,
    DECODER_SCRIPT_BRAILLE,
    DECODER_SCRIPT_BUGINESE,
    DECODER_SCRIPT_COPTIC,
    DECODER_SCRIPT_NEW_TAI_LUE,
    DECODER_SCRIPT_GLAGOLITIC,
    DECODER_SCRIPT_TIFINAGH,
    DECODER_SCRIPT_SYLOTI_NAGRI,
    DECODER_SCRIPT_OLD_PERSIAN,
    DECODER_SCRIPT_KHAROSHTHI,
    DECODER_SCRIPT_BALINESE,
    DECODER_SCRIPT_CUNEIFORM,
    DECODER_SCRIPT_PHOENICIAN,
    DECODER_SCRIPT_PHAGS_PA,
    DECODER_SCRIPT_NKO,
    DECODER_SCRIPT_SUNDANESE,
    DECODER_SCRIPT_LEPCHA,
    DECODER_SCRIPT_OL_CHIKI,
    DECODER_SCRIPT_VAI,
    DECODER_SCRIPT_SAURASHTRA,
    DECODER_SCRIPT_KAYAH_LI,
    DECODER_SCRIPT_REJANG,
    DECODER_SCRIPT_LYCIAN,
    DECODER_SCRIPT_CARIAN,
    DECODER_SCRIPT_LYDIAN,
    DECODER_SCRIPT_CHAM,
    DECODER_SCRIPT_TAI_THAM,
    DECODER_SCRIPT_TAI_VIET,
    DECODER_SCRIPT_AVESTAN,
    DECODER_SCRIPT_EGYPTIAN_HIEROGLYPHS,
    DECODER_SCRIPT_SAMARITAN,
    DECODER_SCRIPT_LISU,
    DECODER_SCRIPT_BAMUM,
    DECODER_SCRIPT_JAVANESE,
    DECODER_SCRIPT_MEETEI_MAYEK,
    DECODER_SCRIPT_IMPERIAL_ARAMAIC,
    DECODER_SCRIPT_OLD_SOUTH_ARABIAN,
    DECODER_SCRIPT_INSCRIPTIONAL_PARTHIAN,
    DECODER_SCRIPT_INSCRIPTIONAL_PAHLAVI,
    DECODER_SCRIPT_OLD_TURKIC,
    DECODER_SCRIPT_KAITHI,
    DECODER_SCRIPT_BATAK,
    DECODER_SCRIPT_BRAHMI,
    DECODER_SCRIPT_MANDAIC,
    DECODER_SCRIPT_CHAKMA,
    DECODER_SCRIPT_MEROITIC_CURSIVE,
    DECODER_SCRIPT_MEROITIC_HIEROGLYPHS,
    DECODER_SCRIPT_MIAO,
    DECODER_SCRIPT_SHARADA,
    DECODER_SCRIPT_SORA_SOMPENG,
    DECODER_SCRIPT_TAKRI,
    DECODER_SCRIPT_CAUCASIAN_ALBANIAN,
    DECODER_SCRIPT_BASSA_VAH,
    DECODER_SCRIPT_DUPLOYAN,
    DECODER_SCRIPT_ELBASAN,
    DECODER_SCRIPT_GRANTHA,
    DECODER_SCRIPT_KHOJKI,
    DECODER_SCRIPT_KHUDAWADI,
    DECODER_SCRIPT_LINEAR_A,
    DECODER_SCRIPT_MAHAJANI,
    DECODER_SCRIPT_MANICHAEAN,
    DECODER_SCRIPT_MENDE_KIKAKUI,
    DECODER_SCRIPT_MODI,
    DECODER_SCRIPT_MRO,
    DECODER_SCRIPT_NABATAEAN,
    DECODER_SCRIPT_OLD_NORTH_ARABIAN,
    DECODER_SCRIPT_OLD_PERMIC,
    DECODER_SCRIPT_PAHAWH_HMONG,
    DECODER_SCRIPT_PALMYRENE,
    DECODER_SCRIPT_PAU_CIN_HAU,
    DECODER_SCRIPT_PSALTER_PAHLAVI,
    DECODER_SCRIPT_SIDDHAM,
    DECODER_SCRIPT_TIRHUTA,
    DECODER_SCRIPT_WARANG_CITI,
    DECODER_SCRIPT_AHOM,
    DECODER_SCRIPT_ANATOLIAN_HIEROGLYPHS,
    DECODER_SCRIPT_HATRAN,
    DECODER_SCRIPT_MULTANI,
    DECODER_SCRIPT_OLD_HUNGARIAN,
    DECODER_SCRIPT_SIGNWRITING,
    DECODER_SCRIPT_ADLAM,
    DECODER_SCRIPT_BHAIKSUKI,
    DECODER_SCRIPT_MARCHEN,
    DECODER_SCRIPT_NEWA,
    DECODER_SCRIPT_OSAGE,
    DECODER_SCRIPT_TANGUT,
    DECODER_SCRIPT_MASARAM_GONDI,
    DECODER_SCRIPT_NUSHU,
    DECODER_SCRIPT_SOYOMBO,
    DECODER_SCRIPT_ZANABAZAR_SQUARE,
    DECODER_SCRIPT_DOGRA,
    DECODER_SCRIPT_GUNJALA_GONDI,
    DECODER_SCRIPT_HANIFI_ROHINGYA,
    DECODER_SCRIPT_MAKASAR,
    DECODER_SCRIPT_MEDEFAIDRIN,
    DECODER_SCRIPT_OLD_SOGDIAN,
    DECODER_SCRIPT_SOGDIAN,
    DECODER_SCRIPT_ELYMAIC,
    DECODER_SCRIPT_NANDINAGARI,
    DECODER_SCRIPT_NYIAKENG_PUACHUE_HMONG,
    DECODER_SCRIPT_WANCHO,
    DECODER_SCRIPT_CHORASMIAN,
    DECODER_SCRIPT_DIVES_AKURU,
    DECODER_SCRIPT_KHITAN_SMALL_SCRIPT,
    DECODER_SCRIPT_YEZIDI,
    DECODER_SCRIPT_CYPRO_MINOAN,
    DECODER_SCRIPT_OLD_UYGHUR,
    DECODER_SCRIPT_TANGSA,
    DECODER_SCRIPT_TOTO,
    DECODER_SCRIPT_VITHKUQI,
    DECODER_SCRIPT_KATAKANA_OR_HIRAGANA,
    DECODER_SCRIPT_KAWI,
    DECODER_SCRIPT_NAG_MUNDARI,
    DECODER_SCRIPT_OL_ONAL,
    DECODER_SCRIPT_TITUS,
    DECODER_SCRIPT_TOLONG_SIKI,
    DECODER_SCRIPT_SUNUWAR,
    DECODER_SCRIPT_TODHRI,
    DECODER_SCRIPT_ARA_NAUZ,
    DECODER_SCRIPT_GARAY,
    DECODER_SCRIPT_GURUNG_KHEMA,
    DECODER_SCRIPT_KIRAT_RAI,
    DECODER_SCRIPT_ONEY,
    DECODER_SCRIPT_TULU_TIGALARI,
    DECODER_SCRIPT_SIDETIC,
    DECODER_SCRIPT_BERIA_ERFE,
    DECODER_SCRIPT_TAI_YO,
    DECODER_SCRIPT_COUNT /**< Total number of scripts (sentinel, not a valid script). */
} decoder_script_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Break Properties (UAX #29)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Grapheme Cluster Break property values (UAX #29, Table 2).
 *
 * Used internally by the grapheme segmentation algorithm.
 */
typedef enum {
    DECODER_GRAPHEME_OTHER = 0,          /**< Any other character. */
    DECODER_GRAPHEME_CR,                 /**< Carriage return (U+000D). */
    DECODER_GRAPHEME_LF,                 /**< Line feed (U+000A). */
    DECODER_GRAPHEME_CONTROL,            /**< Control, Cf, Zl, Zp (except ZWJ). */
    DECODER_GRAPHEME_EXTEND,             /**< Grapheme_Extend = true (Mn, Me, etc.). */
    DECODER_GRAPHEME_ZWJ,                /**< Zero Width Joiner (U+200D). */
    DECODER_GRAPHEME_REGIONAL_INDICATOR, /**< Regional indicator (U+1F1E6..U+1F1FF). */
    DECODER_GRAPHEME_PREPEND,            /**< Prepend characters. */
    DECODER_GRAPHEME_SPACINGMARK,        /**< SpacingMark (Mc with specific exclusions). */
    DECODER_GRAPHEME_L,                  /**< Hangul Leading Jamo (L). */
    DECODER_GRAPHEME_V,                  /**< Hangul Vowel Jamo (V). */
    DECODER_GRAPHEME_T,                  /**< Hangul Trailing Jamo (T). */
    DECODER_GRAPHEME_LV,                 /**< Hangul LV syllable. */
    DECODER_GRAPHEME_LVT                 /**< Hangul LVT syllable. */
} decoder_grapheme_break_t;

/**
 * @brief Word Break property values (UAX #29, Table 3).
 */
typedef enum {
    DECODER_WORD_OTHER = 0,          /**< Any other character. */
    DECODER_WORD_CR,                 /**< Carriage return. */
    DECODER_WORD_LF,                 /**< Line feed. */
    DECODER_WORD_NEWLINE,            /**< Other line breaks (NEL, PS, LS). */
    DECODER_WORD_EXTEND,             /**< Grapheme extending characters. */
    DECODER_WORD_ZWJ,                /**< Zero Width Joiner. */
    DECODER_WORD_REGIONAL_INDICATOR, /**< Regional indicator. */
    DECODER_WORD_FORMAT,             /**< Format characters (Cf). */
    DECODER_WORD_KATAKANA,           /**< Katakana. */
    DECODER_WORD_HEBREWLETTER,       /**< Hebrew letter. */
    DECODER_WORD_ALETTER,            /**< Alphabetic letter. */
    DECODER_WORD_SINGLEQUOTE,        /**< Single quote (U+0027). */
    DECODER_WORD_DOUBLEQUOTE,        /**< Double quote (U+0022). */
    DECODER_WORD_MIDNUMLET,          /**< Mid-number-letter (e.g. full stop). */
    DECODER_WORD_MIDLETTER,          /**< Mid-letter (e.g. colon). */
    DECODER_WORD_MIDNUM,             /**< Mid-number (e.g. comma). */
    DECODER_WORD_NUMERIC,            /**< Numeric. */
    DECODER_WORD_EXTENDNUMLET,       /**< Extend-number-letter (underscore, etc.). */
    DECODER_WORD_WSEGSPACE           /**< Whitespace for word segmentation. */
} decoder_word_break_t;

/**
 * @brief Sentence Break property values (UAX #29, Table 4).
 */
typedef enum {
    DECODER_SENTENCE_OTHER = 0, /**< Any other character. */
    DECODER_SENTENCE_CR,        /**< Carriage return. */
    DECODER_SENTENCE_LF,        /**< Line feed. */
    DECODER_SENTENCE_EXTEND,    /**< Grapheme extend. */
    DECODER_SENTENCE_SEP,       /**< Separator (line/paragraph). */
    DECODER_SENTENCE_FORMAT,    /**< Format character.  */
    DECODER_SENTENCE_SP,        /**< Whitespace. */
    DECODER_SENTENCE_LOWER,     /**< Lowercase letter. */
    DECODER_SENTENCE_UPPER,     /**< Uppercase letter. */
    DECODER_SENTENCE_OLETTER,   /**< Other letter. */
    DECODER_SENTENCE_NUMERIC,   /**< Numeric. */
    DECODER_SENTENCE_ATERM,     /**< Sentence-ending period (.). */
    DECODER_SENTENCE_SCONTINUE, /**< Sentence continuation (;). */
    DECODER_SENTENCE_STERM,     /**< Sentence terminal (! ?). */
    DECODER_SENTENCE_CLOSE      /**< Close punctuation. */
} decoder_sentence_break_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Security Types (UTS #39)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Confusable detection result types (UTS #39, §4).
 */
typedef enum {
    DECODER_CONFUSABLE_NONE = 0,      /**< Not confusable. */
    DECODER_CONFUSABLE_SINGLE_SCRIPT, /**< Confusable within the same script. */
    DECODER_CONFUSABLE_MIXED_SCRIPT,  /**< Confusable across different scripts. */
    DECODER_CONFUSABLE_WHOLE_SCRIPT,  /**< Entire-script confusability. */
    DECODER_CONFUSABLE_RESTRICTED     /**< Involves restricted characters. */
} decoder_confusable_type_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Block identifier (opaque 16-bit value). */
typedef uint16_t decoder_block_t;

/**
 * @brief Locale descriptor for locale-sensitive operations.
 *
 * Pass to functions like decoder_string_to_upper_locale() to enable
 * language-specific case mappings. Current tailorings cover Turkic dotted and
 * dotless I rules; unsupported locale tags fall back to default Unicode
 * mappings.
 * Any field may be NULL to use the default.
 */
typedef struct {
    const char *language; /**< ISO 639-1 language code (e.g. "tr", "lt"). */
    const char *country;  /**< ISO 3166-1 country code (e.g. "TR"). */
    const char *variant;  /**< Optional variant subtag. */
    const char *encoding; /**< Optional encoding name (e.g. "UTF-8"). */
} decoder_locale_t;

/**
 * @brief Result of a multi-script analysis.
 *
 * Returned by decoder_analyze_scripts(). The scripts array is heap-allocated
 * and must be freed with decoder_script_analysis_free().
 */
typedef struct {
    decoder_script_t *scripts; /**< Array of distinct scripts found. */
    size_t count;              /**< Number of distinct scripts. */
    bool has_common;           /**< Whether DECODER_SCRIPT_COMMON was found. */
    bool has_inherited;        /**< Whether DECODER_SCRIPT_INHERITED was found. */
    bool is_suspicious;        /**< Whether the script mix is potentially confusing. */
} decoder_script_analysis_t;

/**
 * @brief Represents a contiguous run of characters belonging to the same script.
 * 
 * Used by decoder_detect_script_runs() to segment text into script blocks
 * following UAX #24 rules for Common and Inherited characters.
 */
typedef struct {
    size_t start;             /**< Start index (inclusive). */
    size_t end;               /**< End index (exclusive). */
    decoder_script_t script;  /**< Script of this run. */
} decoder_script_run_t;

/**
 * @brief Configuration for confusable/security checks.
 *
 * Pass to decoder_check_confusables_with_options() to control which
 * categories of confusables are tested.
 */
typedef struct {
    bool check_single_script_confusables; /**< Test within-script confusables. */
    bool check_mixed_script_confusables;  /**< Test cross-script confusables. */
    bool check_whole_script_confusables;  /**< Test whole-script confusables. */
    bool check_restricted_confusables;    /**< Test restricted-character confusables. */
    bool check_identifier_restrictions;   /**< Test identifier restriction rules. */
    bool strict_mode;                     /**< Enable stricter checks. */
} decoder_security_options_t;

/**
 * @brief Result of a homoglyph lookup.
 *
 * Returned by decoder_find_homoglyphs(). The homoglyphs array is
 * heap-allocated and must be freed with decoder_homoglyph_result_free().
 */
typedef struct {
    uint32_t *homoglyphs; /**< Array of homoglyph code points. */
    size_t count;         /**< Number of homoglyphs found. */
} decoder_homoglyph_result_t;

/**
 * @brief A text segment defined by its start and end positions.
 *
 * Used by segmentation iterators to report grapheme cluster, word,
 * or sentence boundaries. The range is [start, end) — start is
 * inclusive and end is exclusive.
 */
typedef struct {
    size_t start; /**< Start position (inclusive). */
    size_t end;   /**< End position (exclusive). */
} decoder_segment_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Iterator State Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Grapheme cluster iterator state.
 *
 * Initialize with decoder_grapheme_iter_init(), advance with
 * decoder_grapheme_iter_next(). Fields prefixed with _ are private.
 */
typedef struct {
    const uint32_t *text; /**< Pointer to the text being iterated. */
    size_t len;           /**< Length of the text. */
    size_t pos;           /**< Current position. */
    uint32_t _prev_prop;  /**< @private Previous break property. */
    int _ri_count;        /**< @private Regional indicator count. */
    bool _in_emoji_seq;   /**< @private Inside emoji sequence flag. */
    bool _started;        /**< @private Whether iteration has begun. */
} decoder_grapheme_iter_t;

/**
 * @brief Word iterator state.
 *
 * Initialize with decoder_word_iter_init(), advance with
 * decoder_word_iter_next().
 */
typedef struct {
    const uint32_t *text; /**< Pointer to the text being iterated. */
    size_t len;           /**< Length of the text. */
    size_t pos;           /**< Current position. */
    uint32_t _prev_prop;  /**< @private Previous break property. */
    int _ri_count;        /**< @private Regional indicator count. */
    bool _started;        /**< @private Whether iteration has begun. */
} decoder_word_iter_t;

/**
 * @brief Sentence iterator state.
 *
 * Initialize with decoder_sentence_iter_init(), advance with
 * decoder_sentence_iter_next().
 */
typedef struct {
    const uint32_t *text; /**< Pointer to the text being iterated. */
    size_t len;           /**< Length of the text. */
    size_t pos;           /**< Current position. */
    uint32_t _prev_prop;  /**< @private Previous break property. */
    bool _after_term;     /**< @private After sentence terminator flag. */
    bool _started;        /**< @private Whether iteration has begun. */
} decoder_sentence_iter_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Emoji Types (UTS #51)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Emoji skin tone modifier values.
 */
typedef enum {
    DECODER_SKIN_TONE_NONE = 0,
    DECODER_SKIN_TONE_LIGHT = 1,        /**< U+1F3FB Fitzpatrick Type-1-2 */
    DECODER_SKIN_TONE_MEDIUM_LIGHT = 2, /**< U+1F3FC Fitzpatrick Type-3   */
    DECODER_SKIN_TONE_MEDIUM = 3,       /**< U+1F3FD Fitzpatrick Type-4   */
    DECODER_SKIN_TONE_MEDIUM_DARK = 4,  /**< U+1F3FE Fitzpatrick Type-5   */
    DECODER_SKIN_TONE_DARK = 5          /**< U+1F3FF Fitzpatrick Type-6   */
} decoder_emoji_skin_tone_t;

/**
 * @brief Emoji sequence types.
 */
typedef enum {
    DECODER_EMOJI_SEQ_BASIC = 0,    /**< Single emoji codepoint.        */
    DECODER_EMOJI_SEQ_MODIFIER = 1, /**< Emoji + skin tone modifier.    */
    DECODER_EMOJI_SEQ_ZWJ = 2,      /**< Zero-width joiner sequence.    */
    DECODER_EMOJI_SEQ_FLAG = 3,     /**< Regional indicator pair.       */
    DECODER_EMOJI_SEQ_KEYCAP = 4    /**< Digit + combining enclosing.   */
} decoder_emoji_sequence_type_t;

/**
 * @brief Information about a parsed emoji sequence.
 */
typedef struct {
    decoder_emoji_sequence_type_t type;  /**< Type of the sequence.                  */
    size_t length;                       /**< Length in code points.                  */
    uint32_t base_emoji;                 /**< The base emoji code point.             */
    decoder_emoji_skin_tone_t skin_tone; /**< Skin tone (NONE if no modifier).       */
    bool has_variation_selector;         /**< true if VS15/VS16 is present.          */
    bool has_zwj;                        /**< true if sequence contains ZWJ.         */
} decoder_emoji_sequence_info_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Boundary Statistics (UAX #29)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Comprehensive text segmentation statistics.
 */
typedef struct {
    size_t total_chars;       /**< Total number of code points.         */
    size_t grapheme_count;    /**< Number of grapheme clusters.         */
    size_t word_count;        /**< Number of words.                     */
    size_t sentence_count;    /**< Number of sentences.                 */
    size_t whitespace_count;  /**< Number of whitespace code points.    */
    size_t punctuation_count; /**< Number of punctuation code points.   */
} decoder_boundary_stats_t;

#ifdef __cplusplus
}
#endif

#endif /* DECODER_TYPES_H */
