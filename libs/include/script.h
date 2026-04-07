/**
 * @file script.h
 * @brief Unicode Script detection and Block queries (UAX #24).
 *
 * Provides per-code-point script identification, block lookup, and
 * multi-script string analysis for security and text classification.
 */
#ifndef DECODER_SCRIPT_H
#define DECODER_SCRIPT_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Returns the Script property of a code point (UAX #24).
 *
 * @param cp  Unicode code point.
 * @return The script (e.g. DECODER_SCRIPT_LATIN, DECODER_SCRIPT_ARABIC).
 *         Returns DECODER_SCRIPT_UNKNOWN for unassigned code points.
 */
decoder_script_t decoder_get_script(uint32_t cp);

/**
 * @brief Returns the human-readable name of a script.
 *
 * @param script  A decoder_script_t value.
 * @return A null-terminated string such as "Latin" or "Cyrillic".
 *         The pointer is valid for the lifetime of the program (static storage).
 */
const char *decoder_get_script_name(decoder_script_t script);

/**
 * @brief Returns the Unicode Block name for a code point.
 *
 * @param cp  Unicode code point.
 * @return A null-terminated string such as "Basic Latin" or "Cyrillic".
 *         Returns "No_Block" if the code point is not in any defined block.
 */
const char *decoder_get_block_name(uint32_t cp);

/**
 * @brief Returns the Block identifier for a code point.
 *
 * @param cp  Unicode code point.
 * @return A decoder_block_t identifier, or DECODER_BLOCK_NO_BLOCK (0).
 */
decoder_block_t decoder_get_block(uint32_t cp);

/**
 * @brief Checks if a code point belongs to a specific block.
 *
 * @param cp     Unicode code point.
 * @param block  Block identifier.
 * @return true if the code point is within the given block's range.
 */
bool decoder_is_in_block(uint32_t cp, decoder_block_t block);

/**
 * @brief Gets the start and end code points defining a block's range.
 *
 * @param block  Block identifier.
 * @param start  Output: first code point in the block (inclusive).
 * @param end    Output: last code point in the block (inclusive).
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t
 *         if the block identifier is invalid.
 */
int decoder_get_block_range(decoder_block_t block, uint32_t *start, uint32_t *end);

/**
 * @brief Analyzes the scripts present in a string of code points.
 *
 * Identifies all distinct scripts used, flags Common/Inherited presence,
 * and sets is_suspicious if the string contains a potentially confusing
 * mix of scripts (e.g. Latin + Cyrillic).
 *
 * @param str       Sequence of code points.
 * @param len       Length of the sequence.
 * @param analysis  Output: filled with the analysis result.
 *                  The caller must free the result with decoder_script_analysis_free().
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 *
 * @see decoder_script_analysis_free
 */
int decoder_analyze_scripts(const uint32_t *str, size_t len, decoder_script_analysis_t *analysis);

/**
 * @brief Detects contiguous script runs in a text per UAX #24.
 *
 * Scans the string and groups characters into contiguous script runs.
 * Common and Inherited characters are grouped according to UAX #24 rules.
 *
 * @param text   Code point array.
 * @param length Length of the array.
 * @param runs   Output: pointer to an array of decoder_script_run_t. The caller must free().
 * @param count  Output: number of runs found.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_detect_script_runs(const uint32_t *text, size_t length, decoder_script_run_t **runs, size_t *count);

/**
 * @brief Frees resources allocated by decoder_analyze_scripts().
 *
 * @param analysis  Pointer to the analysis result to free.
 *                  Safe to call with NULL or an already-freed result.
 */
void decoder_script_analysis_free(decoder_script_analysis_t *analysis);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Extended Script Queries
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Returns a human-readable description of a script.
 *
 * @param script  A decoder_script_t value.
 * @return A null-terminated string (static storage), or NULL for unknown scripts.
 */
const char *decoder_get_script_description(decoder_script_t script);

/**
 * @brief Checks if a script value is valid (known to the library).
 *
 * @param script  A decoder_script_t value.
 * @return true if the script is recognized.
 */
bool decoder_is_valid_script(decoder_script_t script);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Script-Specific Character Checks
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Checks if a code point belongs to the Latin script. */
bool decoder_is_latin(uint32_t cp);

/** @brief Checks if a code point belongs to the Cyrillic script. */
bool decoder_is_cyrillic(uint32_t cp);

/** @brief Checks if a code point belongs to the Greek script. */
bool decoder_is_greek(uint32_t cp);

/** @brief Checks if a code point belongs to the Arabic script. */
bool decoder_is_arabic(uint32_t cp);

/** @brief Checks if a code point belongs to the Hebrew script. */
bool decoder_is_hebrew(uint32_t cp);

/** @brief Checks if a code point belongs to the Devanagari script. */
bool decoder_is_devanagari(uint32_t cp);

/** @brief Checks if a code point belongs to the Thai script. */
bool decoder_is_thai(uint32_t cp);

/** @brief Checks if a code point belongs to a CJK script (Han/Hiragana/Katakana). */
bool decoder_is_cjk(uint32_t cp);

/** @brief Checks if a code point belongs to the Hangul script (Korean). */
bool decoder_is_hangul(uint32_t cp);

/** @brief Checks if a code point belongs to the Hiragana script. */
bool decoder_is_hiragana(uint32_t cp);

/** @brief Checks if a code point belongs to the Katakana script. */
bool decoder_is_katakana(uint32_t cp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Multi-Script String Analysis
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Returns the dominant (most frequent) script in a string.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return The most frequent script, or DECODER_SCRIPT_COMMON if the string is empty.
 */
decoder_script_t decoder_get_primary_script(const uint32_t *str, size_t len);

/**
 * @brief Counts the number of distinct scripts used in a string.
 *
 * Common and Inherited scripts are not counted.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return Number of distinct scripts.
 */
size_t decoder_count_scripts(const uint32_t *str, size_t len);

/**
 * @brief Checks if a string contains characters from more than one script.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return true if multiple scripts are present (excluding Common/Inherited).
 */
bool decoder_is_mixed_script(const uint32_t *str, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_SCRIPT_H */
