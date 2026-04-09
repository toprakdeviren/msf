/**
 * @file segment.h
 * @brief Text Segmentation — Grapheme clusters, words, and sentences (UAX #29).
 *
 * Provides two API styles:
 *  - **Stateless**: single-call functions for navigation (next/prev), counting,
 *    and boundary testing. Ideal for simple queries.
 *  - **Iterator**: struct-based iterators for efficient sequential scanning.
 *    Ideal for processing all segments in a string.
 *
 * All functions operate on UTF-32 code point arrays.
 */
#ifndef DECODER_SEGMENT_H
#define DECODER_SEGMENT_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Grapheme Clusters (UAX #29, §3)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Returns the position immediately after the grapheme cluster starting at pos.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @param pos  Current position (must be a valid grapheme boundary).
 * @return The next grapheme cluster boundary, or len if at the end.
 */
size_t decoder_next_grapheme(const uint32_t *str, size_t len, size_t pos);

/**
 * @brief Returns the position of the grapheme cluster boundary before pos.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @param pos  Current position.
 * @return The previous grapheme cluster boundary, or 0 if at the start.
 */
size_t decoder_prev_grapheme(const uint32_t *str, size_t len, size_t pos);

/**
 * @brief Counts the total number of grapheme clusters in a string.
 *
 * A grapheme cluster represents what a user perceives as a single "character"
 * (e.g. a base letter + combining accents, an emoji with skin tone, a flag).
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @return Number of grapheme clusters.
 */
size_t decoder_count_graphemes(const uint32_t *str, size_t len);

/**
 * @brief Checks if a position is a grapheme cluster boundary.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @param pos  Position to test.
 * @return true if pos is at a grapheme cluster boundary.
 */
bool decoder_is_grapheme_boundary(const uint32_t *str, size_t len, size_t pos);

/**
 * @brief Finds all grapheme cluster boundary positions in a string.
 *
 * The output always starts with `0` and ends with `len` when the buffer is
 * large enough.
 *
 * @param text            Code point array.
 * @param len             Length of the array.
 * @param boundaries      Output buffer for boundary positions.
 * @param boundaries_cap  Capacity of the output buffer.
 * @param boundaries_len  Output: number of boundaries written.
 * @return DECODER_SUCCESS on success, DECODER_ERROR_INVALID_INPUT for invalid
 *         arguments, or DECODER_ERROR_BUFFER_TOO_SMALL if only a prefix fits.
 */
int decoder_find_grapheme_boundaries(const uint32_t *text, size_t len, size_t *boundaries,
                                     size_t boundaries_cap, size_t *boundaries_len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Words (UAX #29, §4)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Returns the position immediately after the word starting at pos.
 *
 * @param str  Code point array.
 * @param len  Length of the array.
 * @param pos  Current position (must be a valid word boundary).
 * @return The next word boundary, or len if at the end.
 */
size_t decoder_next_word(const uint32_t *str, size_t len, size_t pos);

/**
 * @brief Returns the position of the word boundary before pos.
 */
size_t decoder_prev_word(const uint32_t *str, size_t len, size_t pos);

/**
 * @brief Counts the total number of words in a string.
 *
 * Uses UAX #29 word break rules. Note that whitespace and punctuation
 * sequences are counted as separate "words" per the Unicode algorithm.
 */
size_t decoder_count_words(const uint32_t *str, size_t len);

/**
 * @brief Checks if a position is a word boundary.
 */
bool decoder_is_word_boundary(const uint32_t *str, size_t len, size_t pos);

/**
 * @brief Finds all word boundary positions in a string.
 *
 * The output always starts with `0` and ends with `len` when the buffer is
 * large enough.
 *
 * @param text            Code point array.
 * @param len             Length of the array.
 * @param boundaries      Output buffer for boundary positions.
 * @param boundaries_cap  Capacity of the output buffer.
 * @param boundaries_len  Output: number of boundaries written.
 * @return DECODER_SUCCESS on success, DECODER_ERROR_INVALID_INPUT for invalid
 *         arguments, or DECODER_ERROR_BUFFER_TOO_SMALL if only a prefix fits.
 */
int decoder_find_word_boundaries(const uint32_t *text, size_t len, size_t *boundaries,
                                 size_t boundaries_cap, size_t *boundaries_len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Sentences (UAX #29, §5)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Returns the position immediately after the sentence starting at pos.
 */
size_t decoder_next_sentence(const uint32_t *str, size_t len, size_t pos);

/**
 * @brief Returns the position of the sentence boundary before pos.
 */
size_t decoder_prev_sentence(const uint32_t *str, size_t len, size_t pos);

/**
 * @brief Counts the total number of sentences in a string.
 */
size_t decoder_count_sentences(const uint32_t *str, size_t len);

/**
 * @brief Checks if a position is a sentence boundary.
 */
bool decoder_is_sentence_boundary(const uint32_t *str, size_t len, size_t pos);

/**
 * @brief Finds all sentence boundary positions in a string.
 *
 * The output always starts with `0` and ends with `len` when the buffer is
 * large enough.
 *
 * @param text            Code point array.
 * @param len             Length of the array.
 * @param boundaries      Output buffer for boundary positions.
 * @param boundaries_cap  Capacity of the output buffer.
 * @param boundaries_len  Output: number of boundaries written.
 * @return DECODER_SUCCESS on success, DECODER_ERROR_INVALID_INPUT for invalid
 *         arguments, or DECODER_ERROR_BUFFER_TOO_SMALL if only a prefix fits.
 */
int decoder_find_sentence_boundaries(const uint32_t *text, size_t len, size_t *boundaries,
                                     size_t boundaries_cap, size_t *boundaries_len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Iterators
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initializes a grapheme cluster iterator.
 *
 * @param it    Iterator state to initialize.
 * @param text  Code point array (must remain valid for the iterator's lifetime).
 * @param len   Length of the array.
 */
void decoder_grapheme_iter_init(decoder_grapheme_iter_t *it, const uint32_t *text, size_t len);

/**
 * @brief Advances the grapheme iterator and returns the next segment.
 *
 * @param it   Iterator state.
 * @param seg  Output: filled with the start and end positions of the next grapheme cluster.
 * @return true if a segment was returned, false if the iterator is exhausted.
 */
bool decoder_grapheme_iter_next(decoder_grapheme_iter_t *it, decoder_segment_t *seg);

/**
 * @brief Resets the grapheme iterator to the beginning of the text.
 */
void decoder_grapheme_iter_reset(decoder_grapheme_iter_t *it);

/**
 * @brief Initializes a word iterator.
 *
 * @param it    Iterator state to initialize.
 * @param text  Code point array (must remain valid for the iterator's lifetime).
 * @param len   Length of the array.
 */
void decoder_word_iter_init(decoder_word_iter_t *it, const uint32_t *text, size_t len);

/**
 * @brief Advances the word iterator and returns the next segment.
 *
 * @param it   Iterator state.
 * @param seg  Output: filled with the start and end positions of the next word.
 * @return true if a segment was returned, false if the iterator is exhausted.
 */
bool decoder_word_iter_next(decoder_word_iter_t *it, decoder_segment_t *seg);

/**
 * @brief Resets the word iterator to the beginning of the text.
 */
void decoder_word_iter_reset(decoder_word_iter_t *it);

/**
 * @brief Initializes a sentence iterator.
 *
 * @param it    Iterator state to initialize.
 * @param text  Code point array (must remain valid for the iterator's lifetime).
 * @param len   Length of the array.
 */
void decoder_sentence_iter_init(decoder_sentence_iter_t *it, const uint32_t *text, size_t len);

/**
 * @brief Advances the sentence iterator and returns the next segment.
 *
 * @param it   Iterator state.
 * @param seg  Output: filled with the start and end positions of the next sentence.
 * @return true if a segment was returned, false if the iterator is exhausted.
 */
bool decoder_sentence_iter_next(decoder_sentence_iter_t *it, decoder_segment_t *seg);

/**
 * @brief Resets the sentence iterator to the beginning of the text.
 */
void decoder_sentence_iter_reset(decoder_sentence_iter_t *it);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Boundary Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Computes comprehensive boundary statistics for a text.
 *
 * Counts grapheme clusters, words, sentences, whitespace, and punctuation
 * in a single pass.
 *
 * @param str    Code point array.
 * @param len    Length of the array.
 * @param stats  Output: filled with all boundary counts.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_get_boundary_stats(const uint32_t *str, size_t len, decoder_boundary_stats_t *stats);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_SEGMENT_H */
