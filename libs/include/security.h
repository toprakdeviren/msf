/**
 * @file security.h
 * @brief Unicode Security Mechanisms (UTS #39 & UAX #31).
 *
 * Provides confusable detection (homoglyphs), identifier validation,
 * spoofing analysis, skeleton computation, and string sanitization.
 * These functions help protect against visual spoofing attacks, IDN
 * homograph attacks, and other Unicode-based security threats.
 */
#ifndef DECODER_SECURITY_H
#define DECODER_SECURITY_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Identifier Properties (UAX #31)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Checks if a code point can start an identifier (ID_Start).
 *
 * @param cp  Unicode code point.
 * @return true if the code point has the ID_Start property.
 */
bool decoder_is_identifier_start(uint32_t cp);

/**
 * @brief Checks if a code point can continue an identifier (ID_Continue).
 *
 * @param cp  Unicode code point.
 * @return true if the code point has the ID_Continue property.
 */
bool decoder_is_identifier_continue(uint32_t cp);

/**
 * @brief Checks if a string is a valid Unicode identifier.
 *
 * Validates that the first code point has ID_Start and all subsequent
 * code points have ID_Continue.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if the string forms a valid identifier.
 */
bool decoder_is_valid_identifier(const uint32_t *str, size_t len);

/**
 * @brief Checks if a code point is Pattern_Syntax (UAX #31).
 */
bool decoder_is_pattern_syntax(uint32_t cp);

/**
 * @brief Checks if a code point is Pattern_White_Space (UAX #31).
 */
bool decoder_is_pattern_whitespace(uint32_t cp);

/**
 * @brief Checks if a code point has a restricted ID_Start property.
 */
bool decoder_is_restricted_identifier_start(uint32_t cp);

/**
 * @brief Checks if a code point has a restricted ID_Continue property.
 */
bool decoder_is_restricted_identifier_continue(uint32_t cp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Confusable Detection (UTS #39)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Checks if two code points are visually confusable.
 *
 * Uses the Unicode confusables.txt data (UTS #39) to determine if two
 * characters could be confused by a human reader.
 *
 * @param a  First code point.
 * @param b  Second code point.
 * @return true if the code points are confusable.
 *
 * @note Example: decoder_is_confusable('A', 0x0410) → true (Latin A vs Cyrillic А)
 */
bool decoder_is_confusable(uint32_t a, uint32_t b);

/**
 * @brief Returns the type of confusability between two code points.
 *
 * @param a  First code point.
 * @param b  Second code point.
 * @return A decoder_confusable_type_t value indicating the confusability level.
 */
decoder_confusable_type_t decoder_get_confusable_type(uint32_t a, uint32_t b);

/**
 * @brief Checks if two strings are confusable (whole-string comparison).
 *
 * Computes the skeleton (UTS #39, §4) of each string and compares them.
 *
 * @param s1    First code point sequence.
 * @param len1  Length of the first sequence.
 * @param s2    Second code point sequence.
 * @param len2  Length of the second sequence.
 * @return 0 if the strings are confusable, non-zero otherwise.
 */
int decoder_check_confusables(const uint32_t *s1, size_t len1, const uint32_t *s2, size_t len2);

/**
 * @brief Checks if two strings are confusable with configurable options.
 *
 * @param s1       First code point sequence.
 * @param len1     Length of the first sequence.
 * @param s2       Second code point sequence.
 * @param len2     Length of the second sequence.
 * @param options  Security check options controlling which confusable types to test.
 * @return 0 if the strings are confusable under the given options, non-zero otherwise.
 */
int decoder_check_confusables_with_options(const uint32_t *s1, size_t len1, const uint32_t *s2,
                                           size_t len2, const decoder_security_options_t *options);

/**
 * @brief Computes the skeleton of a string (UTS #39, §4).
 *
 * The skeleton is a normalized representation where confusable characters
 * are mapped to a common form. Two strings are confusable if and only if
 * their skeletons are identical.
 *
 * @param src          Source code points.
 * @param src_len      Number of source code points.
 * @param dst          Destination buffer for skeleton code points.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len      Output: number of code points written to dst.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_get_skeleton(const uint32_t *src, size_t src_len, uint32_t *dst, size_t dst_capacity,
                         size_t *dst_len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Spoofing Analysis
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Checks if a string contains suspicious character combinations.
 *
 * Detects mixed-script usage, invisible characters, and other patterns
 * commonly used in spoofing attacks.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if the string is suspicious.
 */
bool decoder_is_suspicious(const uint32_t *str, size_t len);

/**
 * @brief Checks if a string could be spoofed by another string.
 *
 * A string is spoofable if there exist visually similar characters from
 * different scripts that could replace its characters.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if the string is potentially spoofable.
 */
bool decoder_is_spoofable(const uint32_t *str, size_t len);

/**
 * @brief Checks if a string is highly spoofable (multiple risk factors).
 *
 * Applies stricter checks than decoder_is_spoofable(), considering
 * mixed scripts, invisible characters, and restricted character usage.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if the string has a high spoofing risk.
 */
bool decoder_is_highly_spoofable(const uint32_t *str, size_t len);

/**
 * @brief Checks if a string contains restricted characters.
 *
 * Restricted characters include those that are often used in spoofing
 * or are not recommended for general interchange.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if any restricted characters are found.
 */
bool decoder_has_restricted_characters(const uint32_t *str, size_t len);

/**
 * @brief Checks if a string is well-formed per Unicode Security Annex requirements.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if the string passes all well-formedness checks.
 */
bool decoder_is_well_formed(const uint32_t *str, size_t len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Sanitization & Homoglyphs
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Sanitizes a string by removing dangerous or restricted characters.
 *
 * Strips invisible formatting characters, unassigned code points, and other
 * potentially dangerous code points. Useful for user input sanitization.
 *
 * @param src             Source code points.
 * @param src_len         Number of source code points.
 * @param dst             Destination buffer for sanitized code points.
 * @param dst_capacity    Capacity of the destination buffer.
 * @param dst_len         Output: number of code points written to dst.
 * @param errors_removed  Output: number of dangerous code points that were stripped.
 * @return DECODER_SUCCESS on success, DECODER_ERROR_BUFFER_TOO_SMALL if the
 *         valid output prefix does not fit in dst, or another negative
 *         decoder_status_t on error.
 */
int decoder_sanitize(const uint32_t *src, size_t src_len, uint32_t *dst, size_t dst_capacity,
                     size_t *dst_len, size_t *errors_removed);

/**
 * @brief Finds all known homoglyphs (visually similar characters) for a code point.
 *
 * @param cp      Unicode code point.
 * @param result  Output: filled with an array of homoglyph code points.
 *                The caller must free the result with decoder_homoglyph_result_free().
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 *
 * @see decoder_homoglyph_result_free
 */
int decoder_find_homoglyphs(uint32_t cp, decoder_homoglyph_result_t *result);

/**
 * @brief Frees resources allocated by decoder_find_homoglyphs().
 *
 * @param result  Pointer to the homoglyph result to free.
 *                Safe to call with NULL.
 */
void decoder_homoglyph_result_free(decoder_homoglyph_result_t *result);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Character-Level Security Checks
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Checks if a code point is invisible (ZWJ, ZWNJ, zero-width spaces, etc.).
 *
 * @param cp  Unicode code point.
 * @return true if the character is invisible.
 */
bool decoder_is_invisible(uint32_t cp);

/**
 * @brief Checks if a code point is restricted (surrogates, non-characters,
 *        unassigned, private use, etc.).
 *
 * @param cp  Unicode code point.
 * @return true if the character is restricted for general interchange.
 */
bool decoder_is_restricted(uint32_t cp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  String-Level Security Checks
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Checks if a string contains invisible characters.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if any invisible characters are found.
 */
bool decoder_contains_invisible(const uint32_t *str, size_t len);

/**
 * @brief Checks if a string contains mixed numeral systems (e.g. ASCII digits + Arabic-Indic).
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if multiple numeral systems are present.
 */
bool decoder_contains_mixed_numbers(const uint32_t *str, size_t len);

/**
 * @brief Checks if a string is safe for use as a filename.
 *
 * Validates against control characters, path separators, reserved names,
 * and dangerous Unicode patterns.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if the string is safe for use as a filename.
 */
bool decoder_is_safe_filename(const uint32_t *str, size_t len);

/**
 * @brief Checks if a string is safe for use in a URL.
 *
 * Validates against dangerous Unicode patterns, invisible characters,
 * and potentially confusing bidirectional text.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if the string is safe for use in a URL.
 */
bool decoder_is_safe_url(const uint32_t *str, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_SECURITY_H */
