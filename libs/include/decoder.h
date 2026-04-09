/**
 * =============================================================================
 * Decoder — Unicode Engine
 * Copyright (c) 2025 Ugur Toprakdeviren
 * Licensed under MIT License
 *
 * The fastest Unicode ingress engine.
 * Unicode 17.0 · SIMD-optimized · Thread-safe
 *
 * This single header provides two levels of access:
 *
 *   1. HIGH-LEVEL PIPELINE API (declared below)
 *      UTF-8-native, pipeline-oriented functions for validation, normalization,
 *      sanitization, case folding, security scanning, and streaming.
 *      Designed for: PDF extraction → normalization → tokenizer pre-processing.
 *
 *   2. LOW-LEVEL MODULAR API (via umbrella includes)
 *      Full access to encoding, normalization primitives, segmentation,
 *      case mapping, security, and properties through individual headers.
 *
 * Usage (high-level):
 *   #include <decoder.h>
 *
 *   decoder_pipeline_init();
 *   if (decoder_validate_utf8(buf, len, &info)) {
 *       decoder_normalize_text(buf, len, DECODER_NFC, out, cap, &out_len);
 *       decoder_casefold_text(out, out_len, folded, cap, &folded_len);
 *   }
 *   decoder_pipeline_cleanup();
 *
 * Usage (low-level — include individual modules):
 *   #include <encoding.h>
 *   #include <normalize.h>
 *   #include <segment.h>
 *   #include <security.h>
 * =============================================================================
 */
#ifndef DECODER_H
#define DECODER_H

/* ─── Low-Level Modular API ─────────────────────────────────────────────── */
#include "case.h"
#include "core.h"
#include "emoji.h"
#include "encoding.h"
#include "normalize.h"
#include "parallel.h"
#include "properties.h"
#include "script.h"
#include "security.h"
#include "segment.h"
#include "types.h"

/* ─── High-Level Pipeline API ───────────────────────────────────────────── */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  §1  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Call once at startup. Thread-safe, idempotent.
 *  Loads normalization tables, SIMD dispatch, confusable hash.
 */

/** @brief Initialize the pipeline engine (calls decoder_init internally). */
void decoder_pipeline_init(void);

/** @brief Release all pipeline resources (calls decoder_cleanup internally). */
void decoder_pipeline_cleanup(void);

/** @brief Engine version string, e.g. "1.1.0 (Unicode 17.0)". */
const char *decoder_pipeline_version(void);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §2  UTF-8 Gate — Validation
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  First stage of every pipeline. Rejects overlong sequences,
 *  surrogate injections, broken multi-byte, and truncated trails.
 *  SIMD-accelerated on ARM NEON and x86 SSE2.
 */

/**
 * @brief Detailed validation result.
 *
 * Provides diagnostic information beyond a simple pass/fail,
 * useful for logging and error reporting in ingest pipelines.
 */
typedef struct {
    bool valid;               /**< true if entire input is well-formed UTF-8.    */
    size_t codepoints;        /**< Number of Unicode scalar values decoded.      */
    size_t bytes_consumed;    /**< Bytes successfully validated (≤ input_len).   */
    size_t first_error_byte;  /**< Byte offset of first error (SIZE_MAX if ok).  */
    uint8_t error_byte_value; /**< The invalid byte value (0 if no error).       */
} decoder_utf8_validation_t;

/**
 * @brief Validate UTF-8 with diagnostics.
 *
 * @param input      Raw bytes.
 * @param input_len  Number of bytes.
 * @param result     Output validation result (may be NULL for quick check).
 * @return true if valid.
 */
bool decoder_validate_utf8(const uint8_t *input, size_t input_len,
                           decoder_utf8_validation_t *result);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §3  Normalization — UTF-8 Native
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Pure UAX #15. No sanitization mixed in.
 *  Use NFC for stable training corpus, NFD for decomposition analysis,
 *  NFKC for aggressive cleanup, NFKD for compatibility decomposition.
 */

/**
 * @brief Normalize UTF-8 text to the specified form.
 *
 * @param input      Source UTF-8 bytes (must be valid UTF-8).
 * @param input_len  Number of source bytes.
 * @param form       Normalization form: DECODER_NFC / NFD / NFKC / NFKD.
 * @param output     Destination buffer for normalized UTF-8.
 * @param output_cap Capacity of destination buffer in bytes.
 * @param output_len Output: number of bytes written.
 * @return DECODER_SUCCESS, DECODER_ERROR_INVALID_UTF8, or DECODER_ERROR_BUFFER_TOO_SMALL.
 */
decoder_status_t decoder_normalize_text(const uint8_t *input, size_t input_len,
                                        decoder_normalization_form_t form, uint8_t *output,
                                        size_t output_cap, size_t *output_len);

/**
 * @brief Quick check: is this UTF-8 text already normalized?
 *
 * Avoids a full normalize+compare cycle for text that is already clean.
 *
 * @param input      UTF-8 bytes.
 * @param input_len  Number of bytes.
 * @param form       Normalization form to check against.
 * @return true if already normalized — no work needed.
 */
bool decoder_text_is_normalized(const uint8_t *input, size_t input_len,
                                decoder_normalization_form_t form);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §4  Sanitization — Separate from Normalization
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Sanitization strips characters that are valid Unicode but harmful
 *  or useless for text processing. This is intentionally NOT part of
 *  normalization to preserve determinism and testability.
 *
 *  Typical pipeline: validate → normalize → sanitize → tokenize
 */

/**
 * @brief Sanitization options.
 *
 * Each flag independently controls one class of removal.
 * New flags can be added without breaking ABI (struct grows safely).
 */
typedef struct {
    bool strip_control_chars;     /**< Remove C0/C1 control chars (except TAB/LF/CR). */
    bool strip_format_chars;      /**< Remove Cf category (ZWJ, ZWNJ, BOM, BiDi).     */
    bool strip_replacement_chars; /**< Remove U+FFFD replacement characters.           */
    bool strip_private_use;       /**< Remove Co category (private use area).          */
    bool strip_surrogates;        /**< Remove Cs category (unpaired surrogates).       */
    bool normalize_whitespace;    /**< Collapse runs of whitespace to single U+0020.   */
    bool normalize_newlines;      /**< Convert all line endings to U+000A (LF).        */
    bool unwrap_lines;            /**< Join soft line breaks into paragraphs.           */
    uint32_t _reserved[3];        /**< @private Future flags. Initialize to zero.       */
} decoder_sanitize_options_t;

/**
 * @brief Returns a default sanitize config (all stripping disabled).
 *
 * Safe starting point — enable only what you need.
 */
decoder_sanitize_options_t decoder_sanitize_defaults(void);

/**
 * @brief Returns a preset config suitable for LLM training corpus cleanup.
 *
 * Enables: strip_control_chars, strip_format_chars, strip_replacement_chars,
 *          normalize_whitespace, normalize_newlines.
 */
decoder_sanitize_options_t decoder_sanitize_preset_llm(void);

/**
 * @brief Returns a preset config for PDF text extraction cleanup.
 *
 * Enables everything in LLM preset plus unwrap_lines for paragraph reflow.
 * Joins soft line breaks (single \n between continuation lines) into
 * flowing paragraphs while preserving paragraph boundaries (double \n,
 * sentence-terminal + uppercase start).
 */
decoder_sanitize_options_t decoder_sanitize_preset_pdf(void);

/**
 * @brief Sanitize UTF-8 text according to the given options.
 *
 * @param input        Source UTF-8 bytes (must be valid UTF-8).
 * @param input_len    Number of source bytes.
 * @param options      Sanitization options.
 * @param output       Destination buffer.
 * @param output_cap   Capacity in bytes.
 * @param output_len   Output: bytes written.
 * @return DECODER_SUCCESS or error.
 */
decoder_status_t decoder_sanitize_text(const uint8_t *input, size_t input_len,
                                       const decoder_sanitize_options_t *options, uint8_t *output,
                                       size_t output_cap, size_t *output_len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §5  Case Folding — UTF-8 Native
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Full Unicode case folding (not simple lowercase).
 *  Essential for tokenizer vocabulary consistency.
 *  Example: "Straße" → "strasse" (ß folds to ss).
 */

/**
 * @brief Case-fold UTF-8 text using full Unicode case folding.
 *
 * @param input      Source UTF-8 bytes.
 * @param input_len  Number of source bytes.
 * @param output     Destination buffer (may be larger than input due to expansion).
 * @param output_cap Capacity in bytes.
 * @param output_len Output: bytes written.
 * @return DECODER_SUCCESS or error.
 */
decoder_status_t decoder_casefold_text(const uint8_t *input, size_t input_len, uint8_t *output,
                                       size_t output_cap, size_t *output_len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §6  Grapheme Iterator — UTF-8 Native
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  UAX #29 grapheme cluster segmentation directly on UTF-8 bytes.
 *  Handles ZWJ emoji sequences, regional indicators, Hangul jamo,
 *  Indic conjunct clusters. Essential for Unicode-aware tokenizers.
 */

/**
 * @brief A single grapheme cluster as a byte range in the source.
 */
typedef struct {
    size_t byte_start;      /**< Start byte offset (inclusive) in source UTF-8.  */
    size_t byte_end;        /**< End byte offset (exclusive) in source UTF-8.    */
    size_t codepoint_count; /**< Number of codepoints in this cluster.           */
    bool is_emoji;          /**< true if cluster is an emoji sequence.           */
} decoder_grapheme_t;

/**
 * @brief Opaque grapheme iterator state for UTF-8.
 *
 * Stack-allocated, no heap. 128 bytes reserved for internal state.
 */
typedef struct {
    uint8_t _state[128]; /**< @private Internal state. Do not access directly. */
} decoder_utf8_grapheme_iter_t;

/**
 * @brief Initialize a UTF-8 grapheme iterator.
 *
 * @param it        Iterator to initialize (stack-allocated, caller-owned).
 * @param input     UTF-8 byte sequence (must remain valid for iterator lifetime).
 * @param input_len Number of bytes.
 * @return DECODER_SUCCESS or DECODER_ERROR_INVALID_INPUT.
 */
decoder_status_t decoder_utf8_grapheme_iter_init(decoder_utf8_grapheme_iter_t *it,
                                                 const uint8_t *input, size_t input_len);

/**
 * @brief Advance to the next grapheme cluster.
 *
 * @param it       Iterator state.
 * @param grapheme Output: filled with the next cluster's byte range and metadata.
 * @return true if a cluster was returned, false if exhausted.
 */
bool decoder_utf8_grapheme_iter_next(decoder_utf8_grapheme_iter_t *it,
                                     decoder_grapheme_t *grapheme);

/**
 * @brief Reset iterator to the beginning.
 */
void decoder_utf8_grapheme_iter_reset(decoder_utf8_grapheme_iter_t *it);

/**
 * @brief Free resources associated with a UTF-8 iterator.
 * Must be called if you initialize an iterator but break out of the loop early,
 * to prevent leaking the internal arrays.
 * 
 * @param it Iterator instance.
 */
void decoder_utf8_grapheme_iter_free(decoder_utf8_grapheme_iter_t *it);

/**
 * @brief Count total grapheme clusters in UTF-8 text.
 *
 * Convenience function — iterates internally.
 *
 * @param input      UTF-8 bytes.
 * @param input_len  Number of bytes.
 * @return Number of grapheme clusters.
 */
size_t decoder_count_graphemes_utf8(const uint8_t *input, size_t input_len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §7  Security Scanner — UTF-8 Native
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  UTS #39 confusable detection and mixed-script analysis.
 *  Run as a pre-pass before LLM inference to catch prompt injection
 *  via homograph attacks, invisible characters, and script mixing.
 */

/**
 * @brief Security scan result — aggregated threat flags.
 */
typedef struct {
    bool has_mixed_scripts;    /**< Different scripts mixed (Latin + Cyrillic). */
    bool has_confusable_chars; /**< Contains characters confusable with others. */
    bool has_invisible_chars;  /**< Contains invisible/zero-width characters.   */
    bool has_bidi_override;    /**< Contains BiDi override characters.          */
    bool has_restricted_chars; /**< Contains restricted codepoints (UTS #39).   */
    bool is_safe;              /**< All checks passed — no threats detected.    */
    size_t threat_count;       /**< Total number of individual threats found.   */
} decoder_security_scan_t;

/**
 * @brief Run a full security scan on UTF-8 text.
 *
 * @param input      UTF-8 bytes.
 * @param input_len  Number of bytes.
 * @param result     Output scan result.
 * @return DECODER_SUCCESS or error.
 */
decoder_status_t decoder_security_scan(const uint8_t *input, size_t input_len,
                                       decoder_security_scan_t *result);

/**
 * @brief Compute the skeleton of UTF-8 text (UTS #39 confusable mapping).
 *
 * Two strings are confusable if and only if their skeletons are identical.
 *
 * @param input      Source UTF-8 bytes.
 * @param input_len  Number of source bytes.
 * @param output     Destination buffer for skeleton UTF-8.
 * @param output_cap Capacity in bytes.
 * @param output_len Output: bytes written.
 * @return DECODER_SUCCESS or error.
 */
decoder_status_t decoder_skeleton_text(const uint8_t *input, size_t input_len, uint8_t *output,
                                       size_t output_cap, size_t *output_len);

/**
 * @brief Check if two UTF-8 strings are confusable (skeleton comparison).
 *
 * @param a      First UTF-8 string.
 * @param a_len  Length of first string.
 * @param b      Second UTF-8 string.
 * @param b_len  Length of second string.
 * @return true if the strings are visually confusable.
 */
bool decoder_are_confusable(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §8  Text Analysis — Corpus Statistics
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Single-pass text analysis for BPE training diagnostics.
 *  Not part of the hot path — use for corpus quality assessment.
 */

/**
 * @brief Comprehensive text statistics from a single analysis pass.
 */
typedef struct {
    /* Counts */
    size_t bytes;      /**< Total UTF-8 bytes.                    */
    size_t codepoints; /**< Total Unicode codepoints.             */
    size_t graphemes;  /**< Total grapheme clusters.              */
    size_t words;      /**< Word count (UAX #29 word boundaries). */
    size_t sentences;  /**< Sentence count (UAX #29).             */

    /* Character class breakdown */
    size_t ascii_count;       /**< Codepoints in U+0000..U+007F.         */
    size_t latin_count;       /**< Codepoints with Script=Latin.         */
    size_t emoji_count;       /**< Emoji grapheme clusters.              */
    size_t combining_count;   /**< Combining marks (CCC > 0).            */
    size_t whitespace_count;  /**< Whitespace codepoints.                */
    size_t control_count;     /**< Control characters.                   */
    size_t punctuation_count; /**< Punctuation codepoints.               */

    /* Ratios (0.0 – 1.0) */
    float ascii_ratio;     /**< ascii_count / codepoints.             */
    float combining_ratio; /**< combining_count / codepoints.         */
} decoder_text_stats_t;

/**
 * @brief Analyze UTF-8 text and collect comprehensive statistics.
 *
 * Performs a single pass over the input, computing all statistics.
 * Useful for BPE training diagnostics and corpus quality assessment.
 *
 * @param input      UTF-8 bytes.
 * @param input_len  Number of bytes.
 * @param stats      Output statistics.
 * @return DECODER_SUCCESS or error.
 */
decoder_status_t decoder_analyze_text(const uint8_t *input, size_t input_len,
                                      decoder_text_stats_t *stats);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §9  Streaming Pipeline — Large File Processing
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Chunk-safe streaming for files that don't fit in memory.
 *  Handles normalization boundaries across chunks: if a combining
 *  mark sequence spans a chunk boundary, the stream buffers the
 *  incomplete sequence and prepends it to the next chunk.
 *
 *  Typical flow:
 *    open → feed → feed → ... → finish
 *
 *  The stream does NOT allocate heap memory. All buffering uses
 *  a fixed internal ringbuffer (sized for worst-case combining
 *  mark sequences — 64 codepoints / ~256 bytes).
 */

/**
 * @brief Stream configuration.
 */
typedef struct {
    decoder_normalization_form_t form; /**< Normalization form to apply.    */
    bool case_fold;                    /**< Also apply case folding.        */
    uint32_t _reserved[4];             /**< @private Future options.         */
} decoder_stream_config_t;

/**
 * @brief Returns a default stream config (NFC, no folding).
 */
decoder_stream_config_t decoder_stream_defaults(void);

/**
 * @brief Opaque stream state.
 *
 * Stack-allocated. 512 bytes reserved for internal ringbuffer
 * and normalization state.
 */
typedef struct {
    uint8_t _state[512]; /**< @private Internal state. */
} decoder_stream_t;

/**
 * @brief Open a streaming pipeline.
 *
 * @param stream  Stream state (caller-owned, stack-allocated).
 * @param config  Stream configuration.
 * @return DECODER_SUCCESS or error.
 */
decoder_status_t decoder_stream_open(decoder_stream_t *stream,
                                     const decoder_stream_config_t *config);

/**
 * @brief Feed a chunk of UTF-8 data into the stream.
 *
 * Produces normalized (and optionally case-folded) output. May buffer
 * trailing bytes that could be part of a combining sequence spanning
 * the chunk boundary.
 *
 * @param stream     Stream state.
 * @param chunk      Input UTF-8 bytes.
 * @param chunk_len  Number of input bytes.
 * @param output     Destination buffer for processed output.
 * @param output_cap Capacity of destination buffer.
 * @param output_len Output: number of bytes written.
 * @return DECODER_SUCCESS or error.
 */
decoder_status_t decoder_stream_feed(decoder_stream_t *stream, const uint8_t *chunk,
                                     size_t chunk_len, uint8_t *output, size_t output_cap,
                                     size_t *output_len);

/**
 * @brief Finalize the stream — flush any buffered trailing bytes.
 *
 * Must be called after the last feed() to emit remaining data.
 *
 * @param stream     Stream state.
 * @param output     Destination buffer.
 * @param output_cap Capacity.
 * @param output_len Output: bytes written.
 * @return DECODER_SUCCESS or error.
 */
decoder_status_t decoder_stream_finish(decoder_stream_t *stream, uint8_t *output, size_t output_cap,
                                       size_t *output_len);

/* ═══════════════════════════════════════════════════════════════════════════
 *  §10  Convenience — Full Pipeline in One Call
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  For small-to-medium texts where streaming is overkill.
 *  Runs: validate → normalize → sanitize → case fold → security scan.
 *  Skips steps based on config flags.
 */

/**
 * @brief Full pipeline configuration.
 */
typedef struct {
    decoder_normalization_form_t norm_form;   /**< Normalization form.                */
    bool case_fold;                           /**< Apply case folding after normalize. */
    bool sanitize;                            /**< Apply sanitization.                */
    bool security_scan;                       /**< Run security scan.                 */
    decoder_sanitize_options_t sanitize_opts; /**< Sanitization options (if enabled).  */
    uint32_t _reserved[4];                    /**< @private Future options.            */
} decoder_pipeline_config_t;

/**
 * @brief Returns a default pipeline config (NFC, no extras).
 */
decoder_pipeline_config_t decoder_pipeline_defaults(void);

/**
 * @brief Returns a preset config for LLM training corpus ingestion.
 *
 * NFC + case fold + LLM sanitize preset + security scan.
 */
decoder_pipeline_config_t decoder_pipeline_preset_llm(void);

/**
 * @brief Full pipeline result.
 */
typedef struct {
    decoder_status_t status;          /**< Overall pipeline status.         */
    decoder_security_scan_t security; /**< Security scan result (if run).   */
    size_t output_len;                /**< Bytes written to output buffer.  */
    bool was_normalized;              /**< true if input was already normalized. */
} decoder_pipeline_result_t;

/**
 * @brief Run the full pipeline on UTF-8 text.
 *
 * @param input      Source UTF-8 bytes.
 * @param input_len  Number of source bytes.
 * @param config     Pipeline configuration.
 * @param output     Destination buffer for processed text.
 * @param output_cap Capacity of destination buffer.
 * @param result     Output: pipeline result with status and diagnostics.
 * @return DECODER_SUCCESS or first error encountered.
 */
decoder_status_t decoder_pipeline_process(const uint8_t *input, size_t input_len,
                                          const decoder_pipeline_config_t *config, uint8_t *output,
                                          size_t output_cap, decoder_pipeline_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* DECODER_H */
