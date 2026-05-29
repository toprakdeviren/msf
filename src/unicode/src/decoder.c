/**
 * @file decoder.c
 * @brief msf's Unicode decoder — the entire handwritten implementation.
 *
 * Consolidated from the former core/{encoding,init}.c, api/normalization.c,
 * algorithms/utf_encoding.c, algorithms/normalization/{compose,core,decompose,
 * lookup}.c, platform/simd.c, utils/workspace.c and msf_shim.c.  The generated
 * Unicode tables stay separate in normalization.c / normalization.h.
 *
 * Sections, top to bottom:
 *   1. Thread-local workspace
 *   2. SIMD helpers (portable SWAR + WebAssembly SIMD128)
 *   3. Generated-table lookups (CCC, decomposition, composition, quick-check)
 *   4. Recursive decomposition + canonical ordering
 *   5. Canonical recomposition (NFC)
 *   6. Normalization engine (decompose → order → recompose)
 *   7. UTF-8 ⇄ UTF-32 conversion + the public NFC/NFD entry points
 */
#include "internal.h"

// =============================================================================
// 1. Thread-local workspace
// =============================================================================

#include <pthread.h>

static pthread_key_t g_workspace_key;
static pthread_once_t g_workspace_key_once = PTHREAD_ONCE_INIT;

static void decoder_workspace_cleanup(decoder_workspace_t *ws) {
    if (!ws)
        return;
    free(ws->decode_buf);
    free(ws->process_buf);
    free(ws->scratch_buf);
    ws->decode_buf = ws->process_buf = ws->scratch_buf = NULL;
    ws->decode_cap = ws->process_cap = ws->scratch_cap = 0;
}

static void workspace_destructor(void *ptr) {
    if (ptr) {
        decoder_workspace_cleanup((decoder_workspace_t *)ptr);
        free(ptr);
    }
}

static void make_workspace_key(void) {
    pthread_key_create(&g_workspace_key, workspace_destructor);
}

decoder_workspace_t *decoder_thread_workspace(void) {
    pthread_once(&g_workspace_key_once, make_workspace_key);
    decoder_workspace_t *ws = (decoder_workspace_t *)pthread_getspecific(g_workspace_key);
    if (!ws) {
        ws = calloc(1, sizeof(decoder_workspace_t));
        pthread_setspecific(g_workspace_key, ws);
    }
    return ws;
}

static bool ensure_buffer(uint32_t **buffer, size_t *capacity, size_t required) {
    if (required == 0)
        required = 1;
    if (*capacity >= required)
        return true;

    size_t new_capacity = (*capacity > 0) ? *capacity : 256;
    while (new_capacity < required) {
        if (new_capacity > (SIZE_MAX / 2)) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }

    uint32_t *new_buffer = (uint32_t *)realloc(*buffer, new_capacity * sizeof(uint32_t));
    if (!new_buffer)
        return false;

    *buffer = new_buffer;
    *capacity = new_capacity;
    return true;
}

bool decoder_workspace_ensure_decode(decoder_workspace_t *ws, size_t required) {
    return ws ? ensure_buffer(&ws->decode_buf, &ws->decode_cap, required) : false;
}
bool decoder_workspace_ensure_process(decoder_workspace_t *ws, size_t required) {
    return ws ? ensure_buffer(&ws->process_buf, &ws->process_cap, required) : false;
}
bool decoder_workspace_ensure_scratch(decoder_workspace_t *ws, size_t required) {
    return ws ? ensure_buffer(&ws->scratch_buf, &ws->scratch_cap, required) : false;
}

// =============================================================================
// 2. SIMD helpers — portable SWAR + WebAssembly SIMD128
// =============================================================================

// Count code points by counting bytes that are NOT UTF-8 continuation bytes
// (a continuation byte matches 0b10xxxxxx). SWAR: eight bytes per iteration.
static size_t count_codepoints_swar(const uint8_t *data, size_t len) {
    size_t count = 0;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t word;
        memcpy(&word, data + i, 8);
        uint64_t high = word & 0x8080808080808080ULL;
        uint64_t second = word & 0x4040404040404040ULL;
        uint64_t cont = high & ~(second << 1);
        size_t n_cont = (size_t)__builtin_popcountll(cont >> 7);
        count += 8 - n_cont;
    }
    for (; i < len; i++) {
        if ((data[i] & 0xC0) != 0x80)
            count++;
    }
    return count;
}

static bool is_ascii_swar(const uint8_t *text, size_t length) {
    size_t i = 0;
    for (; i + 8 <= length; i += 8) {
        uint64_t word;
        memcpy(&word, text + i, 8);
        if (word & 0x8080808080808080ULL)
            return false;
    }
    for (; i < length; i++) {
        if (text[i] & 0x80)
            return false;
    }
    return true;
}

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>

// wasm_i8x16_bitmask is the wasm equivalent of SSE2 _mm_movemask_epi8.
static inline uint32_t wasm_movemask(v128_t v) {
    return (uint32_t)wasm_i8x16_bitmask(v);
}

static size_t count_codepoints_wasm(const uint8_t *data, size_t len) {
    size_t count = 0, i = 0;
    const v128_t mask_c0 = wasm_i8x16_splat((int8_t)0xC0);
    const v128_t val_80 = wasm_i8x16_splat((int8_t)0x80);
    for (; i + 16 <= len; i += 16) {
        v128_t chunk = wasm_v128_load(data + i);
        v128_t is_cont = wasm_i8x16_eq(wasm_v128_and(chunk, mask_c0), val_80);
        // 16-bit lane mask → i32.popcnt via the compiler builtin.
        count += 16 - (size_t)__builtin_popcount(wasm_movemask(is_cont));
    }
    count += count_codepoints_swar(data + i, len - i);  // scalar tail
    return count;
}

static bool is_ascii_wasm(const uint8_t *text, size_t length) {
    size_t i = 0;
    for (; i + 16 <= length; i += 16) {
        v128_t chunk = wasm_v128_load(text + i);
        if (wasm_movemask(chunk) != 0)
            return false;
    }
    for (; i < length; i++) {
        if (text[i] & 0x80)
            return false;
    }
    return true;
}
#endif /* __wasm_simd128__ */

size_t simd_count_codepoints_utf8(const uint8_t *data, size_t len) {
    if (!data || len == 0)
        return 0;
#if defined(__wasm_simd128__)
    return count_codepoints_wasm(data, len);
#else
    return count_codepoints_swar(data, len);
#endif
}

bool simd_is_ascii_text(const uint8_t *text, size_t length) {
    if (!text || length == 0)
        return true;
#if defined(__wasm_simd128__)
    return is_ascii_wasm(text, length);
#else
    return is_ascii_swar(text, length);
#endif
}

bool simd_all_below_threshold(const uint32_t *text, size_t len, uint32_t threshold) {
    if (!text || len == 0)
        return true;
    for (size_t i = 0; i < len; i++) {
        if (text[i] >= threshold)
            return false;
    }
    return true;
}

// =============================================================================
// 3. Generated-table lookups
// =============================================================================

// Canonical Combining Class.
uint8_t get_ccc(uint32_t cp) {
    // Fast path: direct O(1) lookup for the hot range U+0300-U+036F.
    if (cp >= ccc_flat_start && cp <= ccc_flat_end) {
        return ccc_flat_0300[cp - ccc_flat_start];
    }
    // Otherwise: binary search on the CCC range table.
    int left = 0, right = (int)ccc_ranges_count - 1;
    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (cp < ccc_ranges[mid].start)
            right = mid - 1;
        else if (cp > ccc_ranges[mid].end)
            left = mid + 1;
        else
            return (uint8_t)ccc_ranges[mid].value;
    }
    return 0;
}

// Canonical decomposition mapping via the generated perfect-hash table.
void get_canonical_decomposition(uint32_t cp, const uint32_t **map, size_t *len) {
    if (decomp_hash_size > 0) {
        uint32_t h = decomp_hash_seed;
        h = ((h ^ (cp & 0xFFFF)) * 0x01000193u) & 0xFFFFFFFFu;
        h = ((h ^ (cp >> 16)) * 0x01000193u) & 0xFFFFFFFFu;
        uint32_t slot = h % (uint32_t)decomp_hash_size;

        for (uint32_t probe = 0; probe <= decomp_hash_max_probe; probe++) {
            uint32_t idx = (slot + probe) % (uint32_t)decomp_hash_size;
            const decomp_index_t *e = &decomp_hash_table[idx];
            if (e->codepoint == 0 && e->length == 0) {
                break;  // Empty slot → not found
            }
            if (e->codepoint == cp) {
                *map = &decomp_data[e->start_index];
                *len = e->length;
                return;
            }
        }
    }
    *map = NULL;
    *len = 0;
}

// Canonical composition pair via the generated perfect-hash table.
uint32_t get_composition(uint32_t first, uint32_t second) {
    if (comp_hash_size == 0)
        return 0;

    uint32_t h = comp_hash_seed;
    h = ((h ^ (first & 0xFFFF)) * 0x01000193u) & 0xFFFFFFFFu;
    h = ((h ^ (first >> 16)) * 0x01000193u) & 0xFFFFFFFFu;
    h = ((h ^ (second & 0xFFFF)) * 0x01000193u) & 0xFFFFFFFFu;
    h = ((h ^ (second >> 16)) * 0x01000193u) & 0xFFFFFFFFu;

    extern const struct {
        uint32_t first;
        uint32_t second;
        uint32_t composed;
    } comp_hash_table[];

    uint32_t slot = h % (uint32_t)comp_hash_size;
    for (uint32_t probe = 0; probe <= comp_hash_max_probe; probe++) {
        uint32_t idx = (slot + probe) % (uint32_t)comp_hash_size;
        uint32_t ef = comp_hash_table[idx].first;
        uint32_t es = comp_hash_table[idx].second;
        uint32_t ec = comp_hash_table[idx].composed;
        if (ef == 0 && es == 0 && ec == 0)
            return 0;  // Empty slot → not found
        if (ef == first && es == second)
            return ec;
    }
    return 0;
}

uint32_t find_composition(uint32_t first, uint32_t second) {
    return get_composition(first, second);
}

static bool normalize_single_codepoint(uint32_t codepoint, normalization_form_t form,
                                       uint32_t *buffer, size_t cap, size_t *length) {
    size_t len = 0;
    if (!recursive_decompose(codepoint, form, buffer, cap, &len))
        return false;
    canonical_ordering(buffer, len);
    if (form == NFC)
        len = recompose(buffer, len);
    *length = len;
    return true;
}

// Quick-check class for a single code point (hot path during scans).
quick_check_result_t get_quick_check(uint32_t codepoint, int form) {
    if (codepoint <= 0x7F) {
        return QC_YES;  // ASCII is always normalized
    }

    if (form == NFD) {
        if (codepoint >= 0xAC00 && codepoint <= 0xD7A3)
            return QC_NO;
        const uint32_t *map;
        size_t dlen;
        get_canonical_decomposition(codepoint, &map, &dlen);
        return map ? QC_NO : QC_YES;
    }

    // NFC path
    uint8_t ccc = get_ccc(codepoint);
    const uint32_t *map = NULL;
    size_t dlen = 0;
    get_canonical_decomposition(codepoint, &map, &dlen);

    if (!map) {
        // Combining marks are sequence-sensitive in NFC.
        if (ccc > 0)
            return QC_MAYBE;
        return QC_YES;
    }

    uint32_t normalized[64];
    size_t normalized_len = 0;
    if (!normalize_single_codepoint(codepoint, (normalization_form_t)form, normalized,
                                    sizeof(normalized) / sizeof(normalized[0]), &normalized_len)) {
        return QC_MAYBE;
    }
    if (normalized_len == 1 && normalized[0] == codepoint)
        return (ccc > 0) ? QC_MAYBE : QC_YES;
    return QC_NO;
}

// =============================================================================
// 4. Recursive decomposition + canonical ordering
// =============================================================================

static bool recursive_decompose_impl(uint32_t cp, normalization_form_t form, uint32_t *buffer,
                                     size_t cap, size_t *len, int depth) {
    if (depth > 18)
        return false;

    // 1. Hangul syllable → algorithmic decomposition.
    if (cp >= HANGUL_S_BASE && cp < HANGUL_S_BASE + HANGUL_S_COUNT) {
        if (*len + 3 > cap)
            return false;
        uint32_t s_index = cp - HANGUL_S_BASE;
        uint32_t l_index = s_index / HANGUL_N_COUNT;
        uint32_t v_index = (s_index % HANGUL_N_COUNT) / HANGUL_T_COUNT;
        uint32_t t_index = s_index % HANGUL_T_COUNT;
        buffer[(*len)++] = HANGUL_L_BASE + l_index;
        buffer[(*len)++] = HANGUL_V_BASE + v_index;
        if (t_index > 0)
            buffer[(*len)++] = HANGUL_T_BASE + t_index;
        return true;
    }

    // 2. Canonical decomposition mapping (compat tables were removed). Look it
    //    up directly — the decompose path needs only the mapping, not the full
    //    entry (so we skip the unused combining-class lookup).
    const uint32_t *map;
    size_t map_len;
    get_canonical_decomposition(cp, &map, &map_len);
    if (!map) {
        if (*len + 1 > cap)
            return false;
        buffer[(*len)++] = cp;
        return true;
    }

    // 3. Recursively decompose each character in the mapping. The mapping points
    //    into the generated decomp_data[] table, which is stable across the
    //    recursive calls below.
    for (size_t i = 0; i < map_len; i++) {
        if (!recursive_decompose_impl(map[i], form, buffer, cap, len, depth + 1))
            return false;
    }
    return true;
}

bool recursive_decompose(uint32_t cp, normalization_form_t form, uint32_t *buffer, size_t cap,
                         size_t *len) {
    return recursive_decompose_impl(cp, form, buffer, cap, len, 0);
}

// Canonically order each combining-mark run in place (insertion sort per run).
void canonical_ordering(uint32_t *buffer, size_t len) {
    if (len < 2)
        return;

    size_t i = 0;
    while (i < len) {
        if (get_ccc(buffer[i]) == 0) {  // skip starters
            i++;
            continue;
        }
        size_t window_start = i;
        while (i < len && get_ccc(buffer[i]) != 0)
            i++;
        size_t window_len = i - window_start;
        if (window_len < 2)
            continue;

        uint8_t ccc_cache[32];
        bool use_cache = (window_len <= 32);
        if (use_cache) {
            for (size_t j = 0; j < window_len; j++)
                ccc_cache[j] = get_ccc(buffer[window_start + j]);
        }
        for (size_t j = 1; j < window_len; j++) {
            uint32_t key = buffer[window_start + j];
            uint8_t key_ccc = use_cache ? ccc_cache[j] : get_ccc(key);
            int k = (int)j - 1;
            while (k >= 0) {
                uint8_t prev_ccc = use_cache ? ccc_cache[k] : get_ccc(buffer[window_start + k]);
                if (prev_ccc <= key_ccc)
                    break;
                buffer[window_start + k + 1] = buffer[window_start + k];
                if (use_cache)
                    ccc_cache[k + 1] = prev_ccc;
                k--;
            }
            buffer[window_start + k + 1] = key;
            if (use_cache)
                ccc_cache[k + 1] = key_ccc;
        }
    }
}

// =============================================================================
// 5. Canonical recomposition (NFC)
// =============================================================================

size_t recompose(uint32_t *buffer, size_t len) {
    if (len < 2)
        return len;

    int last_starter_idx = (get_ccc(buffer[0]) == 0) ? 0 : -1;
    size_t result_len = 1;

    for (size_t i = 1; i < len; i++) {
        uint32_t ch = buffer[i];
        uint8_t ch_ccc = get_ccc(ch);

        if (last_starter_idx >= 0) {
            uint32_t starter = buffer[last_starter_idx];

            // Blocked: an intervening char B has ccc(B) >= ccc(ch).
            bool blocked = false;
            if (last_starter_idx < (int)result_len - 1) {
                for (int k = last_starter_idx + 1; k < (int)result_len; k++) {
                    if (get_ccc(buffer[k]) >= ch_ccc) {
                        blocked = true;
                        break;
                    }
                }
            }

            if (!blocked) {
                uint32_t combined = 0;
                // L + V -> LV
                if (starter >= HANGUL_L_BASE && starter < HANGUL_L_BASE + HANGUL_L_COUNT &&
                    ch >= HANGUL_V_BASE && ch < HANGUL_V_BASE + HANGUL_V_COUNT) {
                    uint32_t l_idx = starter - HANGUL_L_BASE;
                    uint32_t v_idx = ch - HANGUL_V_BASE;
                    combined = HANGUL_S_BASE + (l_idx * HANGUL_N_COUNT) + (v_idx * HANGUL_T_COUNT);
                }
                // LV + T -> LVT
                else if (starter >= HANGUL_S_BASE && starter < HANGUL_S_BASE + HANGUL_S_COUNT &&
                         ((starter - HANGUL_S_BASE) % HANGUL_T_COUNT) == 0 && ch >= HANGUL_T_BASE &&
                         ch < HANGUL_T_BASE + HANGUL_T_COUNT) {
                    combined = starter + (ch - HANGUL_T_BASE);
                } else {
                    combined = find_composition(starter, ch);
                }

                if (combined) {
                    buffer[last_starter_idx] = combined;
                    continue;
                }
            }
        }

        if (ch_ccc == 0)
            last_starter_idx = result_len;
        buffer[result_len++] = ch;
    }

    return result_len;
}

// =============================================================================
// 6. Normalization engine
// =============================================================================

static bool is_hangul_l_jamo(uint32_t cp) {
    return cp >= HANGUL_L_BASE && cp < HANGUL_L_BASE + HANGUL_L_COUNT;
}
static bool is_hangul_v_jamo(uint32_t cp) {
    return cp >= HANGUL_V_BASE && cp < HANGUL_V_BASE + HANGUL_V_COUNT;
}
static bool is_hangul_t_jamo(uint32_t cp) {
    return cp > HANGUL_T_BASE && cp < HANGUL_T_BASE + HANGUL_T_COUNT;
}
static bool is_hangul_lv_syllable(uint32_t cp) {
    return cp >= HANGUL_S_BASE && cp < HANGUL_S_BASE + HANGUL_S_COUNT &&
           ((cp - HANGUL_S_BASE) % HANGUL_T_COUNT) == 0;
}

static bool can_canonically_compose(uint32_t starter, uint32_t cp) {
    if (is_hangul_l_jamo(starter) && is_hangul_v_jamo(cp))
        return true;
    if (is_hangul_lv_syllable(starter) && is_hangul_t_jamo(cp))
        return true;
    return find_composition(starter, cp) != 0;
}

// Scan for quick-check failures and reordering/composition hazards.
static quick_check_t scan_normalization(normalization_form_t form, const uint32_t *text, size_t len,
                                        bool exact) {
    quick_check_t result = QC_YES_ALT;
    uint8_t last_ccc = 0;
    bool has_starter = false;
    uint32_t last_starter = 0;
    uint8_t max_ccc_since_starter = 0;
    bool has_nonstarter_since_starter = false;

    for (size_t i = 0; i < len; i++) {
        uint32_t cp = text[i];
        quick_check_result_t qc = get_quick_check(cp, form);
        if (qc == QC_NO)
            return QC_NO_ALT;
        if (!exact && qc == QC_MAYBE)
            result = QC_MAYBE_ALT;

        uint8_t ccc = get_ccc(cp);
        if (ccc > 0 && ccc < last_ccc)
            return QC_NO_ALT;

        if (form == NFC && has_starter) {
            bool blocked =
                (ccc == 0) ? has_nonstarter_since_starter : (max_ccc_since_starter >= ccc);
            if (!blocked && can_canonically_compose(last_starter, cp))
                return QC_NO_ALT;
        }

        if (ccc == 0) {
            has_starter = true;
            last_starter = cp;
            max_ccc_since_starter = 0;
            has_nonstarter_since_starter = false;
        } else if (has_starter) {
            has_nonstarter_since_starter = true;
            if (ccc > max_ccc_since_starter)
                max_ccc_since_starter = ccc;
        }

        last_ccc = ccc;
    }

    return result;
}

static bool normalized_output_matches_input(normalization_form_t form, const uint32_t *text,
                                            size_t len) {
    decoder_workspace_t *workspace = decoder_thread_workspace();
    size_t cap = len * 4 + 64;
    if (!decoder_workspace_ensure_process(workspace, cap))
        return false;

    size_t normalized_len = 0;
    if (normalize_with_workspace(form, text, len, workspace->process_buf, workspace->process_cap,
                                 &normalized_len, workspace) != 0) {
        return false;
    }
    return normalized_len == len &&
           memcmp(workspace->process_buf, text, len * sizeof(uint32_t)) == 0;
}

bool is_normalized(normalization_form_t form, const uint32_t *text, size_t len) {
    if (!text || len == 0)
        return true;
    if (simd_all_below_threshold(text, len, NQC_SAFE_THRESHOLD))
        return true;
    if (scan_normalization(form, text, len, true) == QC_NO_ALT)
        return false;
    return normalized_output_matches_input(form, text, len);
}

int normalize_with_workspace(normalization_form_t form, const uint32_t *input, size_t len,
                             uint32_t *output, size_t cap, size_t *output_length,
                             decoder_workspace_t *workspace) {
    if (!input || !output || !output_length || len == 0) {
        if (output_length)
            *output_length = 0;
        return -1;
    }

    if (!workspace)
        workspace = decoder_thread_workspace();

    // ASCII is normalization-stable for every form.
    if (simd_all_below_threshold(input, len, NQC_SAFE_THRESHOLD)) {
        if (len > cap) {
            *output_length = len;
            return -2;
        }
        memcpy(output, input, len * sizeof(uint32_t));
        *output_length = len;
        return 0;
    }

    // Full pipeline with safe chunking: accumulate into temp_buffer, flush at
    // safe boundaries instead of requiring one monolithic temporary buffer.
    size_t temp_cap = (len * 4) > 4096 ? 4096 : (len * 4 + 64);
    if (!decoder_workspace_ensure_scratch(workspace, temp_cap))
        return -1;

    uint32_t *temp_buffer = workspace->scratch_buf;
    size_t temp_len = 0;
    size_t total_out_len = 0;

    for (size_t i = 0; i < len; i++) {
        uint32_t cp = input[i];

        if (temp_len >= 2048 && is_safe_normalization_chunk_boundary(cp)) {
            canonical_ordering(temp_buffer, temp_len);
            if (form == NFC)
                temp_len = recompose(temp_buffer, temp_len);
            if (total_out_len + temp_len > cap) {
                *output_length = total_out_len + temp_len;
                return -2;
            }
            memcpy(output + total_out_len, temp_buffer, temp_len * sizeof(uint32_t));
            total_out_len += temp_len;
            temp_len = 0;
        }

        size_t checkpoint = temp_len;
        while (!recursive_decompose(cp, form, temp_buffer, temp_cap, &temp_len)) {
            temp_len = checkpoint;
            size_t next_cap = (temp_cap > 0) ? (temp_cap * 2) : 256;
            if (next_cap <= temp_cap)
                next_cap = temp_cap + 256;
            if (!decoder_workspace_ensure_scratch(workspace, next_cap))
                return -1;
            temp_buffer = workspace->scratch_buf;
            temp_cap = workspace->scratch_cap;
        }
    }

    if (temp_len > 0) {
        canonical_ordering(temp_buffer, temp_len);
        if (form == NFC)
            temp_len = recompose(temp_buffer, temp_len);
        if (total_out_len + temp_len > cap) {
            *output_length = total_out_len + temp_len;
            return -2;
        }
        memcpy(output + total_out_len, temp_buffer, temp_len * sizeof(uint32_t));
        total_out_len += temp_len;
    }

    *output_length = total_out_len;
    return 0;
}

int normalize(normalization_form_t form, const uint32_t *input, size_t len, uint32_t *output,
              size_t cap, size_t *output_length) {
    return normalize_with_workspace(form, input, len, output, cap, output_length,
                                    decoder_thread_workspace());
}

// =============================================================================
// 7. UTF-8 ⇄ UTF-32 conversion + public NFC/NFD entry points
// =============================================================================

// First-byte → sequence length (0 = invalid lead/continuation byte).
static const uint8_t utf8_sequence_length_table[256] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x00-0x0F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x10-0x1F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x20-0x2F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x30-0x3F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x40-0x4F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x50-0x5F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x60-0x6F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x70-0x7F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x80-0x8F (continuation)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x90-0x9F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xA0-0xAF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xB0-0xBF
    0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // 0xC0-0xCF (0xC0/0xC1 overlong)
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // 0xD0-0xDF
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  // 0xE0-0xEF
    4, 4, 4, 4, 4, 4, 4, 4,                          // 0xF0-0xF7
    0, 0, 0, 0, 0, 0, 0, 0                           // 0xF8-0xFF (invalid)
};

static int utf_result_to_decoder_status(utf_result_t result) {
    switch (result) {
        case UTF_OK:
            return DECODER_SUCCESS;
        case UTF_BUFFER_OVERFLOW:
            return DECODER_ERROR_BUFFER_TOO_SMALL;
        case UTF_INVALID_UTF8:
            return DECODER_ERROR_INVALID_UTF8;
        case UTF_INVALID_UTF16:
            return DECODER_ERROR_INVALID_UTF16;
        case UTF_INVALID_UTF32:
            return DECODER_ERROR_INVALID_CODEPOINT;
        case UTF_INVALID_INPUT:
        default:
            return DECODER_ERROR_INVALID_INPUT;
    }
}

// Decode one UTF-8 sequence; validates continuation bytes, overlong encodings,
// surrogates and out-of-range scalars.  Returns bytes consumed, or 0 on error.
static inline size_t decode_utf8_char(const char *utf8, size_t len, uint32_t *out_cp) {
    if (UNLIKELY(len == 0))
        return 0;

    unsigned char b0 = (unsigned char)utf8[0];
    if (LIKELY(b0 < 0x80)) {  // ASCII fast path
        *out_cp = b0;
        return 1;
    }

    size_t seq_len = utf8_sequence_length_table[b0];
    if (UNLIKELY(seq_len == 0 || seq_len > len))
        return 0;

    for (size_t i = 1; i < seq_len; i++) {
        if (UNLIKELY(((unsigned char)utf8[i] & 0xC0) != 0x80))
            return 0;
    }

    uint32_t cp;
    if (seq_len == 2) {
        cp = ((b0 & 0x1F) << 6) | (utf8[1] & 0x3F);
        if (UNLIKELY(cp < 0x80))
            return 0;
    } else if (seq_len == 3) {
        cp = ((b0 & 0x0F) << 12) | ((utf8[1] & 0x3F) << 6) | (utf8[2] & 0x3F);
        if (UNLIKELY(cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)))
            return 0;
    } else { /* seq_len == 4 */
        cp = ((b0 & 0x07) << 18) | ((utf8[1] & 0x3F) << 12) | ((utf8[2] & 0x3F) << 6) |
             (utf8[3] & 0x3F);
        if (UNLIKELY(cp < 0x10000 || cp > 0x10FFFF))
            return 0;
    }

    *out_cp = cp;
    return seq_len;
}

// Convert UTF-8 → UTF-32, allocating only when the scratch buffer is too small.
utf_result_t utf8_to_utf32_auto(const char *utf8, size_t utf8_len, uint32_t *stack_buf,
                                size_t stack_cap, uint32_t **utf32_out, size_t *utf32_len,
                                bool *is_allocated) {
    if (is_allocated) *is_allocated = false;
    if (UNLIKELY(!utf8 || !stack_buf || stack_cap == 0 || !utf32_out || !utf32_len)) {
        return UTF_INVALID_INPUT;
    }

    *utf32_out = stack_buf;
    *utf32_len = 0;
    if (UNLIKELY(utf8_len == 0)) {
        return UTF_OK;
    }

    uint32_t *buffer = stack_buf;
    size_t cap = stack_cap;
    bool using_heap = false;

    if (LIKELY(simd_is_ascii_text((const uint8_t *)utf8, utf8_len))) {
        if (utf8_len > cap) {
            buffer = (uint32_t *)malloc(utf8_len * sizeof(uint32_t));
            if (UNLIKELY(!buffer))
                return UTF_INVALID_INPUT;
            cap = utf8_len;
            using_heap = true;
            if (is_allocated) *is_allocated = true;
        }
        for (size_t i = 0; i < utf8_len; i++)
            buffer[i] = (uint8_t)utf8[i];
        *utf32_out = buffer;
        *utf32_len = utf8_len;
        return UTF_OK;
    }

    size_t input_pos = 0;
    size_t output_pos = 0;

    // Every code point is >= 1 byte, so the decoded output never exceeds
    // utf8_len.  When the caller's buffer already holds utf8_len entries we
    // decode straight into it, skipping both the O(n) code-point count and the
    // heap allocation.  (Invalid all-continuation input is still caught by the
    // decode loop below, which returns UTF_INVALID_UTF8.)
    if (utf8_len > cap) {
        size_t cp_count = simd_count_codepoints_utf8((const uint8_t *)utf8, utf8_len);
        if (cp_count > cap) {
            buffer = (uint32_t *)malloc(cp_count * sizeof(uint32_t));
            if (UNLIKELY(!buffer))
                return UTF_INVALID_INPUT;
            cap = cp_count;
            using_heap = true;
            if (is_allocated) *is_allocated = true;
        }
    }

    while (input_pos < utf8_len) {
        uint32_t cp;
        size_t consumed = decode_utf8_char(&utf8[input_pos], utf8_len - input_pos, &cp);
        if (UNLIKELY(consumed == 0)) {
            if (using_heap) {
                free(buffer);
                if (is_allocated) *is_allocated = false;
            }
            *utf32_out = NULL;
            return UTF_INVALID_UTF8;
        }
        buffer[output_pos++] = cp;
        input_pos += consumed;
    }

    *utf32_out = buffer;
    *utf32_len = output_pos;
    return UTF_OK;
}

// Convert UTF-32 → UTF-8. Stops on the first invalid scalar or when the buffer
// runs out; *utf8_len reports the valid prefix already written.
static utf_result_t utf32_to_utf8(const uint32_t *utf32, size_t utf32_len, char *utf8,
                                  size_t utf8_cap, size_t *utf8_len) {
    if (UNLIKELY(!utf32 || !utf8 || !utf8_len))
        return UTF_INVALID_INPUT;
    if (UNLIKELY(utf8_cap == 0))
        return UTF_BUFFER_OVERFLOW;

    size_t output_pos = 0;
    for (size_t i = 0; i < utf32_len; i++) {
        uint32_t cp = utf32[i];

        if (LIKELY(cp < 0x80)) {
            if (UNLIKELY(output_pos >= utf8_cap)) {
                *utf8_len = output_pos;
                return UTF_BUFFER_OVERFLOW;
            }
            utf8[output_pos++] = (char)cp;
            continue;
        }

        if (UNLIKELY(cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) {
            *utf8_len = output_pos;
            return UTF_INVALID_UTF32;
        }

        size_t bytes_needed = (cp < 0x800) ? 2 : (cp < 0x10000) ? 3 : 4;
        if (UNLIKELY(output_pos + bytes_needed > utf8_cap)) {
            *utf8_len = output_pos;
            return UTF_BUFFER_OVERFLOW;
        }

        if (bytes_needed == 2) {
            utf8[output_pos] = (char)(0xC0 | (cp >> 6));
            utf8[output_pos + 1] = (char)(0x80 | (cp & 0x3F));
        } else if (bytes_needed == 3) {
            utf8[output_pos] = (char)(0xE0 | (cp >> 12));
            utf8[output_pos + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8[output_pos + 2] = (char)(0x80 | (cp & 0x3F));
        } else {
            utf8[output_pos] = (char)(0xF0 | (cp >> 18));
            utf8[output_pos + 1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            utf8[output_pos + 2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8[output_pos + 3] = (char)(0x80 | (cp & 0x3F));
        }
        output_pos += bytes_needed;
    }

    *utf8_len = output_pos;
    return UTF_OK;
}

// The public form enum and the core form enum use a different ordering.
static const normalization_form_t normalization_form_lut[2] = {
    NFC,  // DECODER_NFC = 0 -> NFC = 1
    NFD   // DECODER_NFD = 1 -> NFD = 0
};

static normalization_form_t convert_normalization_form(decoder_normalization_form_t form) {
    if (LIKELY(form <= DECODER_NFD))
        return normalization_form_lut[form];
    return NFC;
}

static bool decoder_is_normalized(const uint32_t *str, size_t len,
                                  decoder_normalization_form_t form) {
    if (!str || len == 0)
        return true;
    return is_normalized(convert_normalization_form(form), str, len);
}

static int decoder_normalize(const uint32_t *src, size_t src_len, decoder_normalization_form_t form,
                             uint32_t *dst, size_t dst_capacity, size_t *dst_len) {
    if (!src || !dst || !dst_len)
        return DECODER_ERROR_INVALID_INPUT;

    size_t out_len = 0;
    int res = normalize(convert_normalization_form(form), src, src_len, dst, dst_capacity, &out_len);
    if (res >= 0) {
        *dst_len = out_len;
        return DECODER_SUCCESS;
    }
    if (res == -2) {
        *dst_len = out_len;
        return DECODER_ERROR_BUFFER_TOO_SMALL;
    }
    return DECODER_ERROR_INVALID_INPUT;
}

// ── Public API ───────────────────────────────────────────────────────────────

// No global state to set up (tables are static const, SIMD needs no init).
void decoder_init(void) {}

int decoder_normalize_utf8(const uint8_t *src, size_t src_len, decoder_normalization_form_t form,
                           uint8_t *dst, size_t dst_capacity, size_t *dst_len) {
    if (!src || !dst || !dst_len)
        return DECODER_ERROR_INVALID_INPUT;
    if (src_len == 0) {
        *dst_len = 0;
        return DECODER_SUCCESS;
    }

    decoder_workspace_t *workspace = decoder_thread_workspace();

    // Step 1: Decode UTF-8 → UTF-32 into the workspace buffer.  Sizing it to
    // src_len (>= the code-point count) guarantees utf8_to_utf32_auto decodes
    // in place with no allocation, so the returned pointer is never freed here.
    if (!decoder_workspace_ensure_decode(workspace, src_len))
        return DECODER_ERROR_OUT_OF_MEMORY;
    uint32_t *codepoints = workspace->decode_buf;
    size_t chars_written = 0;
    utf_result_t dec = utf8_to_utf32_auto((const char *)src, src_len, workspace->decode_buf,
                                          workspace->decode_cap, &codepoints, &chars_written, NULL);
    if (dec == UTF_INVALID_UTF8)
        return DECODER_ERROR_INVALID_UTF8;
    if (dec != UTF_OK)
        return DECODER_ERROR_OUT_OF_MEMORY;

    // Step 2: Normalize. NFC composes (output <= input), so chars_written*3 is
    // ample; NFD can expand a single code point up to ~4x, so retry with a
    // larger buffer if a canonical decomposition overflows it.
    size_t norm_cap = chars_written * 3 + 16;
    size_t norm_len = 0;
    int ret;
    for (;;) {
        if (!decoder_workspace_ensure_process(workspace, norm_cap))
            return DECODER_ERROR_OUT_OF_MEMORY;
        ret = decoder_normalize(codepoints, chars_written, form, workspace->process_buf,
                                workspace->process_cap, &norm_len);
        if (ret != DECODER_ERROR_BUFFER_TOO_SMALL)
            break;
        if (norm_cap > SIZE_MAX / 2)
            return DECODER_ERROR_OUT_OF_MEMORY;
        norm_cap *= 2;
    }
    if (ret != DECODER_SUCCESS)
        return ret;

    // Step 3: Re-encode UTF-32 → UTF-8
    utf_result_t enc =
        utf32_to_utf8(workspace->process_buf, norm_len, (char *)dst, dst_capacity, dst_len);
    return (enc == UTF_OK) ? DECODER_SUCCESS : utf_result_to_decoder_status(enc);
}

bool decoder_is_normalized_utf8(const uint8_t *str, size_t len, decoder_normalization_form_t form) {
    if (!str || len == 0)
        return true;

    decoder_workspace_t *workspace = decoder_thread_workspace();

    // Decode into the workspace buffer; len (>= the code-point count) guarantees
    // an in-place decode with no allocation.
    if (!decoder_workspace_ensure_decode(workspace, len))
        return false;
    uint32_t *codepoints = workspace->decode_buf;
    size_t chars_written = 0;
    if (utf8_to_utf32_auto((const char *)str, len, workspace->decode_buf, workspace->decode_cap,
                           &codepoints, &chars_written, NULL) != UTF_OK) {
        return false;
    }
    return decoder_is_normalized(codepoints, chars_written, form);
}
