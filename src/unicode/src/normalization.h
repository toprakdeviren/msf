/**
 * @file lut_uax15_normalization.h
 * @brief UAX#15 Unicode Normalization — Data Layer
 *
 * Auto-generated — do not edit by hand.
 * This header only declares DATA symbols.
 * Lookup FUNCTIONS are in src/algorithms/uax15_normalization.h
 */

#ifndef LUT_NORMALIZATION_H
#define LUT_NORMALIZATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Check if types are already defined
#ifndef DECOMPOSITION_ENTRY_DEFINED
#define DECOMPOSITION_ENTRY_DEFINED

// Decomposition entry structure
typedef struct {
    uint32_t codepoint;
    const uint32_t *decomposed;
    size_t count;
    bool is_compatibility;
    const uint32_t *decomposition;
    size_t decomposition_len;
    uint8_t combining_class;
} decomposition_entry_t;

#endif  // DECOMPOSITION_ENTRY_DEFINED

// Basic types needed for generation
typedef struct {
    uint32_t codepoint;
    uint8_t ccc;
} ccc_entry_t;

typedef struct {
    uint32_t codepoint;
    uint32_t start_index;
    uint32_t length;
} decomp_index_t;

// Composition entry structure
typedef struct {
    uint32_t first;
    uint32_t second;
    uint32_t composed;
} composition_entry_t;

// SIMD range type (must match simd.h definition)
#ifndef SIMD_RANGE_DEFINED
#define SIMD_RANGE_DEFINED
typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t value;
} simd_range_t;
#endif

#ifndef QUICK_CHECK_RESULT_DEFINED
#define QUICK_CHECK_RESULT_DEFINED

// Quick check result enumeration
typedef enum {
    QC_YES = 0,   // Character is already normalized
    QC_NO = 1,    // Character needs normalization
    QC_MAYBE = 2  // Character might need normalization
} quick_check_result_t;

#endif  // QUICK_CHECK_RESULT_DEFINED

#ifndef NORMALIZATION_FORM_DEFINED
#define NORMALIZATION_FORM_DEFINED

// Normalization form enumeration
typedef enum {
    NFD = 0,  // Canonical Decomposition
    NFC = 1   // Canonical Composition
    // NFKD/NFKC (compatibility forms) removed — msf normalizes to NFC/NFD only.
} normalization_form_t;

#endif  // NORMALIZATION_FORM_DEFINED

// =============================================================================
// Original binary-search tables (defined in lut_uax15_normalization.c)
// =============================================================================

extern const ccc_entry_t ccc_table[];
extern const size_t ccc_table_size;

extern const uint32_t decomp_data[];
extern const decomp_index_t decomp_table[];
extern const size_t decomp_table_size;


extern const composition_entry_t comp_table[];
extern const size_t comp_table_size;

// =============================================================================
// SIMD Range Tables (for range-based SIMD lookups)
// =============================================================================

// CCC ranges (contiguous codepoints with same CCC value)
extern const simd_range_t ccc_ranges[];
extern const size_t ccc_ranges_count;

// CCC flat array for hot range U+0300-U+036F (direct O(1) lookup)
extern const uint8_t ccc_flat_0300[];
extern const uint32_t ccc_flat_start;
extern const uint32_t ccc_flat_end;

// Canonical decomposition presence ranges (does cp have a decomposition?)
extern const simd_range_t decomp_ranges[];
extern const size_t decomp_ranges_count;

// =============================================================================
// Composition Perfect Hash Table (O(1) lookup)
// =============================================================================

extern const uint32_t comp_hash_seed;
extern const size_t comp_hash_size;
extern const uint32_t comp_hash_max_probe;

// =============================================================================
// Decomposition Perfect Hash Tables (O(1) lookup)
// =============================================================================

// Canonical decomposition hash
extern const uint32_t decomp_hash_seed;
extern const size_t decomp_hash_size;
extern const uint32_t decomp_hash_max_probe;
extern const decomp_index_t decomp_hash_table[];

// =============================================================================
// Lookup functions (defined in uax15_normalization.c)
// =============================================================================

uint8_t get_ccc(uint32_t cp);
void get_canonical_decomposition(uint32_t cp, const uint32_t **map, size_t *len);
uint32_t get_composition(uint32_t first, uint32_t second);
quick_check_result_t get_quick_check(uint32_t cp, int form);

#endif  // LUT_NORMALIZATION_H
