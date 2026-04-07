/**
 * @file emoji.h
 * @brief Unicode Emoji properties and sequence detection (UTS #51).
 *
 * Provides per-code-point emoji property queries, emoji sequence parsing,
 * skin tone detection, flag/keycap identification, and emoji counting.
 * All property checks are SIMD-accelerated via LUT-backed range tables.
 */
#ifndef DECODER_EMOJI_H
#define DECODER_EMOJI_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Single Code Point Properties
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Checks the Emoji property (UTS #51). */
bool decoder_is_emoji(uint32_t cp);

/** @brief Checks the Emoji_Presentation property. */
bool decoder_is_emoji_presentation(uint32_t cp);

/** @brief Checks the Emoji_Modifier property (skin tone modifiers U+1F3FB..U+1F3FF). */
bool decoder_is_emoji_modifier(uint32_t cp);

/** @brief Checks the Emoji_Modifier_Base property. */
bool decoder_is_emoji_modifier_base(uint32_t cp);

/** @brief Checks the Emoji_Component property (ZWJ, VS16, skin tones, keycap). */
bool decoder_is_emoji_component(uint32_t cp);

/** @brief Checks the Extended_Pictographic property. */
bool decoder_is_extended_pictographic(uint32_t cp);

/** @brief Checks if a code point is a Regional Indicator (U+1F1E6..U+1F1FF). */
bool decoder_is_regional_indicator(uint32_t cp);

/** @brief Checks if a code point is an emoji variation selector (VS15/VS16). */
bool decoder_is_emoji_variation_selector(uint32_t cp);

/** @brief Checks if a code point is a skin tone modifier (U+1F3FB..U+1F3FF). */
bool decoder_is_skin_tone_modifier(uint32_t cp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Skin Tone
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Returns the skin tone of a modifier code point.
 *
 * @param cp  Unicode code point.
 * @return The skin tone value, or DECODER_SKIN_TONE_NONE if not a modifier.
 */
decoder_emoji_skin_tone_t decoder_get_skin_tone(uint32_t cp);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Emoji Sequences
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses the next emoji sequence starting at a given position.
 *
 * Recognizes basic emoji, modifier sequences, ZWJ sequences, flag sequences,
 * and keycap sequences (UTS #51).
 *
 * @param text       Code point array.
 * @param len        Length of the array.
 * @param start_pos  Position to start parsing from.
 * @param info       Output: filled with sequence details (type, length, skin tone, etc.).
 * @return DECODER_SUCCESS if an emoji sequence was found,
 *         DECODER_ERROR_INVALID_INPUT on invalid arguments,
 *         DECODER_ERROR_NOT_FOUND if no emoji at start_pos.
 */
int decoder_next_emoji_sequence(const uint32_t *text, size_t len, size_t start_pos,
                                decoder_emoji_sequence_info_t *info);

/**
 * @brief Checks if a two-codepoint sequence forms a valid flag (Regional Indicator pair).
 *
 * @param text  Code point array (must have at least 2 elements).
 * @param len   Length of the array.
 * @return true if text[0..1] is a valid flag sequence.
 */
bool decoder_is_valid_flag_sequence(const uint32_t *text, size_t len);

/**
 * @brief Checks if a two-codepoint sequence forms a keycap sequence (digit + U+20E3).
 *
 * @param text  Code point array (must have at least 2 elements).
 * @param len   Length of the array.
 * @return true if text[0..1] is a valid keycap sequence.
 */
bool decoder_is_keycap_sequence(const uint32_t *text, size_t len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Counting
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Counts the number of emoji sequences in a code point array.
 *
 * Walks the text using decoder_next_emoji_sequence(), so ZWJ sequences
 * and flag pairs each count as one emoji.
 *
 * @param text  Code point array.
 * @param len   Length of the array.
 * @return Number of emoji sequences found.
 */
size_t decoder_count_emoji(const uint32_t *text, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_EMOJI_H */
