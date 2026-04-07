/**
 * @file properties.h
 * @brief Unicode character properties (UAX #44).
 *
 * Provides property queries for individual Unicode code points: validity,
 * General Category, character class predicates, numeric values, and
 * character names. All functions accept a uint32_t code point and return
 * the corresponding property. Out-of-range code points return the default
 * value (false, 0, DECODER_CATEGORY_UNASSIGNED, etc.).
 */
#ifndef DECODER_PROPERTIES_H
#define DECODER_PROPERTIES_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Validity
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Checks if a code point is a valid Unicode scalar value (0..U+10FFFF, excluding
 * surrogates).
 */
bool decoder_is_valid(uint32_t cp);

/**
 * @brief Checks if a code point is assigned in the Unicode Character Database.
 */
bool decoder_is_assigned(uint32_t cp);

/**
 * @brief Checks if a code point belongs to a Private Use Area (U+E000..U+F8FF, etc.).
 */
bool decoder_is_private_use(uint32_t cp);

/**
 * @brief Checks if a code point is a surrogate (U+D800..U+DFFF).
 *
 * Surrogates are not valid scalar values and must not appear in UTF-32 text.
 */
bool decoder_is_surrogate(uint32_t cp);

/**
 * @brief Checks if a code point is a noncharacter (e.g. U+FFFE, U+FFFF, U+nFFFE, U+nFFFF).
 */
bool decoder_is_noncharacter(uint32_t cp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  General Category (UAX #44, §5.7.1)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Returns the General Category of a code point.
 *
 * @param cp  Unicode code point.
 * @return A decoder_category_t value (e.g. DECODER_CATEGORY_UPPERCASE_LETTER).
 */
decoder_category_t decoder_get_category(uint32_t cp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Character Class Predicates
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Letter (L): Lu | Ll | Lt | Lm | Lo. */
bool decoder_is_letter(uint32_t cp);

/** @brief Uppercase letter (Lu). */
bool decoder_is_uppercase(uint32_t cp);

/** @brief Lowercase letter (Ll). */
bool decoder_is_lowercase(uint32_t cp);

/** @brief Titlecase letter (Lt). */
bool decoder_is_titlecase(uint32_t cp);

/** @brief Decimal digit (Nd). */
bool decoder_is_digit(uint32_t cp);

/** @brief Number (N): Nd | Nl | No. */
bool decoder_is_number(uint32_t cp);

/** @brief Punctuation (P): Pc | Pd | Ps | Pe | Pi | Pf | Po. */
bool decoder_is_punctuation(uint32_t cp);

/** @brief Symbol (S): Sm | Sc | Sk | So. */
bool decoder_is_symbol(uint32_t cp);

/** @brief Mark (M): Mn | Mc | Me. */
bool decoder_is_mark(uint32_t cp);

/** @brief Separator (Z): Zs | Zl | Zp. */
bool decoder_is_separator(uint32_t cp);

/** @brief Control character (Cc). */
bool decoder_is_control(uint32_t cp);

/** @brief Format character (Cf). */
bool decoder_is_format(uint32_t cp);

/**
 * @brief Checks the Unicode White_Space property.
 *
 * Includes U+0009..U+000D, U+0020, U+0085, U+00A0, U+1680, U+2000..U+200A,
 * U+2028, U+2029, U+202F, U+205F, U+3000.
 */
bool decoder_is_whitespace(uint32_t cp);

/** @brief Equivalent to decoder_is_whitespace(). */
bool decoder_is_space(uint32_t cp);

/** @brief Letter or digit: is_letter(cp) || is_digit(cp). */
bool decoder_is_alphanumeric(uint32_t cp);

/** @brief Derived Alphabetic property (includes letters, combining marks, etc.). */
bool decoder_is_alphabetic(uint32_t cp);

/** @brief Derived Numeric property (Nd | Nl | No with a defined numeric value). */
bool decoder_is_numeric(uint32_t cp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Numeric Values
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Returns the numeric value of a code point.
 *
 * @param cp  Unicode code point.
 * @return The numeric value as a double, or -1.0 if the code point has no numeric value.
 *
 * @note For fractional values (e.g. U+00BC VULGAR FRACTION ONE QUARTER), returns 0.25.
 */
double decoder_get_numeric_value(uint32_t cp);

/**
 * @brief Returns the decimal digit value of a code point.
 *
 * @param cp  Unicode code point.
 * @return 0–9 for decimal digit characters, or -1 if not a decimal digit.
 */
int decoder_get_digit_value(uint32_t cp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Character Name & Age
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Gets the Unicode character name for a code point.
 *
 * @param cp        Unicode code point.
 * @param buffer    Destination buffer for the null-terminated name string.
 * @param capacity  Maximum number of bytes (including null terminator) to write.
 * @return DECODER_SUCCESS on success, DECODER_ERROR_BUFFER_TOO_SMALL if buffer is too small.
 *
 * @note Example: U+0041 → "LATIN CAPITAL LETTER A"
 */
int decoder_get_name(uint32_t cp, char *buffer, size_t capacity);

/**
 * @brief Looks up a code point by its Unicode character name (case-insensitive).
 *
 * @param name  Null-terminated character name string.
 * @return The code point, or 0xFFFFFFFF if not found.
 */
uint32_t decoder_from_name(const char *name);

/**
 * @brief Returns the Unicode version in which a code point was assigned.
 *
 * @param cp  Unicode code point.
 * @return A null-terminated version string such as "1.1" or "17.0",
 *         or NULL if the code point is unassigned.
 */
const char *decoder_get_age(uint32_t cp);

/**
 * @brief Checks if a code point was assigned in a specific Unicode version or earlier.
 *
 * @param cp     Unicode code point.
 * @param major  Major version number (e.g. 17).
 * @param minor  Minor version number (e.g. 0).
 * @return true if the code point was assigned in version major.minor or earlier.
 */
bool decoder_is_in_version(uint32_t cp, int major, int minor);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Batch Classification (for BPE pre-tokenization)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Character class values for pre-tokenization.
 *
 * Matches the CharClass enum used in the JavaScript PreTokenizer.
 */
typedef enum {
    DECODER_CHARCLASS_LETTER = 0,      /**< Lu|Ll|Lt|Lm|Lo + Mn|Mc|Me (marks stay with letters) */
    DECODER_CHARCLASS_DIGIT = 1,       /**< Nd|Nl|No */
    DECODER_CHARCLASS_WHITESPACE = 2,  /**< Zs|Zl|Zp + Cc whitespace (tab, etc.) */
    DECODER_CHARCLASS_PUNCTUATION = 3, /**< Pc|Pd|Ps|Pe|Pi|Pf|Po */
    DECODER_CHARCLASS_SYMBOL = 4,      /**< Sm|Sc|Sk|So */
    DECODER_CHARCLASS_NEWLINE = 5,     /**< \n, \r, U+0085, U+2028, U+2029 */
    DECODER_CHARCLASS_OTHER = 6        /**< Cc (non-whitespace control), Cf (format), etc. */
} decoder_charclass_t;

/**
 * @brief Batch-classify UTF-8 bytes into per-codepoint character classes.
 *
 * Decodes UTF-8 to codepoints internally and classifies each into a
 * CharClass value. This replaces N individual WASM calls with a single
 * batch operation.
 *
 * @param utf8            Input UTF-8 bytes.
 * @param utf8_len        Number of input bytes.
 * @param classes_out     Output buffer — one uint8_t (CharClass) per codepoint.
 * @param classes_cap     Capacity of classes_out.
 * @param codepoint_count [out] Number of codepoints processed (= classes written).
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_classify_codepoints(const uint8_t *utf8, size_t utf8_len, uint8_t *classes_out,
                                size_t classes_cap, size_t *codepoint_count);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_PROPERTIES_H */
