/** @file builtin_names.h — Canonical Swift type name constants
 *
 * Single source of truth for all builtin/well-known type names used in
 * comparisons throughout the compiler. Eliminates magic strings.
 *
 * Usage: #include "builtin_names.h"
 *        if (strcmp(name, SW_TYPE_INT) == 0) ...
 */

#ifndef BUILTIN_NAMES_H
#define BUILTIN_NAMES_H
#include <string.h>

/* ── Primitive types ────────────────────────────────────────────────────────── */
#define SW_TYPE_VOID "Void"
#define SW_TYPE_BOOL "Bool"
#define SW_TYPE_INT "Int"
#define SW_TYPE_INT8 "Int8"
#define SW_TYPE_INT16 "Int16"
#define SW_TYPE_INT32 "Int32"
#define SW_TYPE_INT64 "Int64"
#define SW_TYPE_UINT "UInt"
#define SW_TYPE_UINT8 "UInt8"
#define SW_TYPE_UINT16 "UInt16"
#define SW_TYPE_UINT32 "UInt32"
#define SW_TYPE_UINT64 "UInt64"
#define SW_TYPE_FLOAT "Float"
#define SW_TYPE_FLOAT32 "Float32"
#define SW_TYPE_FLOAT64 "Float64"
#define SW_TYPE_DOUBLE "Double"
#define SW_TYPE_CGFLOAT "CGFloat"
#define SW_TYPE_STRING "String"
#define SW_TYPE_CHARACTER "Character"

/* ── Generic container types ────────────────────────────────────────────────── */
#define SW_TYPE_ARRAY "Array"
#define SW_TYPE_DICTIONARY "Dictionary"
#define SW_TYPE_OPTIONAL "Optional"

/* ── Well-known protocol names ──────────────────────────────────────────────── */
#define SW_PROTO_EQUATABLE    "Equatable"
#define SW_PROTO_HASHABLE     "Hashable"
#define SW_PROTO_COMPARABLE   "Comparable"
#define SW_PROTO_CODABLE      "Codable"
#define SW_PROTO_SENDABLE     "Sendable"
#define SW_PROTO_CUSTOM_STR   "CustomStringConvertible"
#define SW_PROTO_ERROR        "Error"
#define SW_PROTO_VIEW         "View"

/* ── Special type names ────────────────────────────────────────────────────── */
#define SW_TYPE_SELF          "Self"
#define SW_TYPE_ANY           "Any"
#define SW_TYPE_ANY_OBJECT    "AnyObject"
#define SW_TYPE_NEVER         "Never"

/* ── Attribute names ────────────────────────────────────────────────────────── */
#define SW_ATTR_PROPERTY_WRAPPER "propertyWrapper"
#define SW_ATTR_RESULT_BUILDER   "resultBuilder"
#define SW_ATTR_MAIN_ACTOR       "MainActor"

/* ── Result builder method names ────────────────────────────────────────────── */
#define SW_BUILDER_BUILD_BLOCK "buildBlock"
#define SW_BUILDER_BUILD_EXPRESSION "buildExpression"
#define SW_BUILDER_BUILD_OPTIONAL "buildOptional"
#define SW_BUILDER_BUILD_EITHER "buildEither"
#define SW_BUILDER_BUILD_ARRAY "buildArray"
#define SW_BUILDER_BUILD_FINAL "buildFinalResult"

/* ═════════════════════════════════════════════════════════════════════════════
 * Builtin function names (BN_*)
 * Used in strcmp() calls throughout the backend code generation.
 * ═════════════════════════════════════════════════════════════════════════════ */

/* ── Array ──────────────────────────────────────────────────────────────────── */
#define BN_ARRAY_INIT        "__array_init"
#define BN_ARRAY_APPEND        "__array_append"
#define BN_ARRAY_LEN           "__array_len"
#define BN_ARRAY_GET_PTR       "__array_get_ptr"
#define BN_ARRAY_SET           "__array_set"
#define BN_ARRAY_CONCAT        "__array_concat"
#define BN_ARRAY_PLUS          "__array_plus"
#define BN_ARRAY_CONTAINS      "__array_contains"
#define BN_ARRAY_FIRST         "__array_first"
#define BN_ARRAY_LAST          "__array_last"
#define BN_ARRAY_FIRST_INDEX   "__array_first_index"
#define BN_ARRAY_INSERT        "__array_insert"
#define BN_ARRAY_REMOVE_AT     "__array_remove_at"
#define BN_ARRAY_SWAP_AT       "__array_swap_at"
#define BN_ARRAY_DROP_FIRST    "__array_drop_first"
#define BN_ARRAY_DROP_LAST     "__array_drop_last"
#define BN_ARRAY_PREFIX        "__array_prefix"
#define BN_ARRAY_SUFFIX        "__array_suffix"
#define BN_ARRAY_REVERSED      "__array_reversed"
#define BN_ARRAY_MAX           "__array_max"
#define BN_ARRAY_MIN           "__array_min"
#define BN_ARRAY_SORTED        "__array_sorted"
#define BN_ARRAY_SORTED_INT    "__array_sorted_int"
#define BN_ARRAY_SORTED_STR    "__array_sorted_str"
#define BN_ARRAY_SPLIT         "__array_split"
#define BN_ARRAY_REPLACING_SUBRANGE "__array_replacing_subrange"
#define BN_ARRAY_REPLACE_RANGE      "__array_replace_range"
#define BN_ARRAY_CHUNKED                 "__array_chunked"
#define BN_ARRAY_WINDOWS                 "__array_windows"
#define BN_ARRAY_ELEMENTS_EQUAL  "__array_elements_equal"
#define BN_ARRAY_STARTS_WITH     "__array_starts_with"
#define BN_ARRAY_LEX_PRECEDES    "__array_lex_precedes"
#define BN_ARRAY_PRINT           "__array_print"
#define BN_ARRAY_CAPACITY        "__array_capacity"
#define BN_ARRAY_POP_LAST        "__array_pop_last"
#define BN_ARRAY_CLEAR                      "__array_clear"
#define BN_ARRAY_CLEAR_DISCARDING_CAPACITY  "__array_clear_discarding_capacity"


/* ── String ─────────────────────────────────────────────────────────────────── */
#define BN_STR_CONCAT          "__str_concat"
#define BN_STR_CONCAT_3        "__str_concat_3"
#define BN_STR_LEN             "__str_len"
#define BN_STR_CODEPOINT_COUNT "__str_codepoint_count"
#define BN_STR_CHAR_AT         "__str_char_at"
#define BN_STR_SUBSTR          "__str_substr"
#define BN_STR_CONTAINS        "__str_contains"
#define BN_STR_HAS_PREFIX      "__str_has_prefix"
#define BN_STR_HAS_SUFFIX      "__str_has_suffix"
#define BN_STR_LOWERCASED      "__str_lowercased"
#define BN_STR_UPPERCASED      "__str_uppercased"
#define BN_STR_REVERSED        "__str_reversed"
#define BN_STR_SPLIT           "__str_split"
#define BN_STR_SPLIT_EX        "__str_split_ex"
#define BN_STR_JOIN_ARRAY      "__str_join_array"
#define BN_STR_EQ              "__str_eq"
#define BN_STR_NEQ             "__str_neq"
#define BN_STR_INTERP          "__str_interp"
#define BN_STR_FROM_INT        "__str_from_int"
#define BN_STR_FROM_BOOL       "__str_from_bool"
#define BN_STR_FROM_DOUBLE     "__str_from_double"
#define BN_STR_FROM_INT_RADIX  "__str_from_int_radix"
#define BN_STR_FIRST_INDEX     "__str_first_index"
#define BN_STR_LAST_INDEX      "__str_last_index"
#define BN_STR_REPEAT          "__str_repeat"
#define BN_STR_TRIM            "__str_trim"
#define BN_STR_CAPITALIZED     "__str_capitalized"
#define BN_STR_IS_NUMBER       "__str_is_number"
#define BN_STR_IS_LETTER       "__str_is_letter"
#define BN_STR_IS_WHITESPACE   "__str_is_whitespace"
#define BN_STR_IS_ALPHANUMERIC "__str_is_alphanumeric"
#define BN_STR_IS_PUNCTUATION  "__str_is_punctuation"
#define BN_STR_IS_SYMBOL       "__str_is_symbol"
#define BN_STR_IS_UPPERCASE    "__str_is_uppercase"
#define BN_STR_IS_LOWERCASE    "__str_is_lowercase"
#define BN_STR_IS_HEX_DIGIT    "__str_is_hex_digit"
#define BN_STR_ASCII_VALUE     "__str_ascii_value"
#define BN_STR_IS_ASCII        "__str_is_ascii"
#define BN_STR_JOIN_ARRAY_SEP  "__str_join_array_sep"
#define BN_STR_PADDING         "__str_padding"
#define BN_STR_HASH            "__str_hash"
#define BN_STR_INSERT          "__str_insert"
#define BN_STR_REMOVE_RANGE    "__str_remove_range"
#define BN_STR_REPLACE_SUBRANGE "__str_replace_subrange"
#define BN_STR_UTF8_COUNT      "__str_utf8_count"
#define BN_STR_UTF16_COUNT     "__str_utf16_count"
#define BN_SUBSTR_INIT       "__substr_init"
#define BN_SUBSTR_TO_STRING    "__substr_to_string"
#define BN_STR_DROP_FIRST_SUB  "__str_drop_first_sub"
#define BN_STR_DROP_LAST_SUB   "__str_drop_last_sub"
#define BN_SUBSTR_DROP_FIRST   "__substr_drop_first"
#define BN_SUBSTR_DROP_LAST    "__substr_drop_last"
#define BN_STR_PREFIX_SUB      "__str_prefix_sub"
#define BN_STR_SUFFIX_SUB      "__str_suffix_sub"
#define BN_SUBSTR_PREFIX       "__substr_prefix"
#define BN_SUBSTR_SUFFIX       "__substr_suffix"
#define BN_SUBSTR_LEN          "__substr_len"
#define BN_SUBSTR_CHAR_AT      "__substr_char_at"
/* Mutation: removeFirst / removeLast (k=0 means single grapheme) */
#define BN_STR_REMOVE_FIRST    "__str_remove_first"
#define BN_STR_REMOVE_LAST     "__str_remove_last"
#define BN_STR_REMOVE_FIRST_K  "__str_remove_first_k"
#define BN_STR_REMOVE_LAST_K   "__str_remove_last_k"
#define BN_STR_CMP             "__str_cmp"

/* ── Data ───────────────────────────────────────────────────────────────────── */
#define BN_DATA_INIT         "__data_init"
#define BN_DATA_INIT_COUNT   "__data_init_count"
#define BN_DATA_FROM_BYTES     "__data_from_bytes"
#define BN_DATA_FROM_STR       "__data_from_str"
#define BN_DATA_COUNT          "__data_count"
#define BN_DATA_IS_EMPTY       "__data_is_empty"
#define BN_DATA_APPEND_BYTE    "__data_append_byte"
#define BN_DATA_GET_BYTE       "__data_get_byte"
#define BN_DATA_BASE64_ENC     "__data_base64_encode"
#define BN_DATA_BASE64_DEC     "__data_base64_decode"

/* ── Alarm host builtins (__alarm_*) ───────────────────────────────────────── */
#define BN_ALARM_SCHEDULE       "__alarm_schedule"
#define BN_ALARM_CANCEL         "__alarm_cancel"
#define BN_ALARM_PAUSE          "__alarm_pause"
#define BN_ALARM_RESUME         "__alarm_resume"
#define BN_ALARM_STOP           "__alarm_stop"
#define BN_ALARM_REQUEST_AUTH   "__alarm_request_auth"
#define BN_ALARM_GET_ALL        "__alarm_get_all"
#define BN_ALARM_COUNTDOWN      "__alarm_countdown"
/* AlarmPresentationState */
#define BN_ALARM_PS_READ        "__alarm_ps_read"
#define BN_ALARM_PS_MODE        "__alarm_ps_mode"

/* ── Dict ───────────────────────────────────────────────────────────────────── */
#define BN_DICT_INIT         "__dict_init"
#define BN_DICT_SET            "__dict_set"
#define BN_DICT_GET            "__dict_get"
#define BN_DICT_CONTAINS       "__dict_contains"
#define BN_DICT_REMOVE         "__dict_remove"
#define BN_DICT_COUNT          "__dict_count"
#define BN_DICT_FREE           "__dict_free"
#define BN_DICT_KEYS_ARRAY     "__dict_keys_array"
#define BN_DICT_VALUES_ARRAY   "__dict_values_array"
/* grouping:by: helper — gets existing sub-array or inits a new one */
#define BN_DICT_GET_OR_INIT_ARR "__dict_get_or_init_arr"
#define BN_DICT_MAKE_ITERATOR "__dict_make_iterator"
#define BN_DICT_ITER_NEXT     "__dict_iter_next"

/* ── Set ────────────────────────────────────────────────────────────────────── */
#define BN_SET_INIT          "__set_init"
#define BN_SET_INIT_STR      "__set_init_str"
#define BN_SET_INSERT          "__set_insert"
#define BN_SET_INSERT_I64      "__set_insert_i64"
#define BN_SET_CONTAINS        "__set_contains"
#define BN_SET_REMOVE          "__set_remove"
#define BN_SET_REMOVE_ALL      "__set_remove_all"
#define BN_SET_COUNT           "__set_count"
#define BN_SET_GET_PTR         "__set_get_ptr"
#define BN_SET_SORTED          "__set_sorted"
#define BN_SET_UNION           "__set_union"
#define BN_SET_INTERSECTION    "__set_intersection"
#define BN_SET_SUBTRACTING     "__set_subtracting"
#define BN_SET_SYMMETRIC_DIFFERENCE "__set_symmetric_difference"
#define BN_SET_IS_SUBSET       "__set_is_subset"
#define BN_SET_IS_SUPERSET     "__set_is_superset"
#define BN_SET_IS_DISJOINT     "__set_is_disjoint"

/* ── Any (boxing/unboxing) ──────────────────────────────────────────────────── */
#define BN_ANY_BOX_INT         "__any_box_int"
#define BN_ANY_BOX_PTR         "__any_box_ptr"
#define BN_ANY_GET_TAG         "__any_get_tag"
#define BN_ANY_AS_PTR          "__any_as_ptr"
#define BN_ANY_AS_INT          "__any_as_int"

/* ── Print ──────────────────────────────────────────────────────────────────── */
#define BN_PRINT               "print"
#define BN_SWIFT_PRINT         "__swift_print"
#define BN_SWIFT_PRINT_ITEMS   "__swift_print_items"

/* ── Conversion / type check ────────────────────────────────────────────────── */
#define BN_INT_FROM_STR        "__int_from_str"
#define BN_SWIFT_IS_TYPE       "__swift_is_type"
#define BN_SUBSCRIPT           "__subscript"

/* ── Memory / lifecycle ─────────────────────────────────────────────────────── */
#define BN_ALLOC_CLASS           "__miniswift_alloc_class"
#define BN_SWIFT_FATAL_ERROR     "__swift_fatalError"
#define BN_THROW                 "__throw"
#define BN_CONSTRAINT_FAIL       "__constraint_fail"
#define BN_SWIFT_ASSERT          "__swift_assert"
#define BN_SWIFT_PRECONDITION    "__swift_precondition"

/* ── Async / concurrency ────────────────────────────────────────────────────── */
#define BN_SWIFT_TASK_INIT   "__swift_task_init"
#define BN_SWIFT_TASK_RUN      "__swift_task_run"

/* ── Entry point ────────────────────────────────────────────────────────────── */
#define BN_SWIFT_ENTRY         "__swift_entry"

/* ═════════════════════════════════════════════════════════════════════════════
 * BuiltinID — enum-based dispatch for builtin function calls
 *
 * Instead of expensive strcmp chains at emit time, the IR builder resolves
 * call_name → BuiltinID once.  The emitter uses a fast switch dispatch.
 *
 * Pipeline:  call_name → resolve_builtin() → ins->builtin_id → switch emit
 * ═════════════════════════════════════════════════════════════════════════════ */

typedef enum {
  BUILTIN_NONE = 0,

  /* ── Array ────────────────────────────────────────────────────────────────── */
  BUILTIN_ARRAY_INIT,
  BUILTIN_ARRAY_INIT_N,
  BUILTIN_ARRAY_APPEND,
  BUILTIN_ARRAY_LEN,
  BUILTIN_ARRAY_GET_PTR,
  BUILTIN_ARRAY_SET,
  BUILTIN_ARRAY_CONCAT,
  BUILTIN_ARRAY_PLUS,
  BUILTIN_ARRAY_CONTAINS,
  BUILTIN_ARRAY_FIRST,
  BUILTIN_ARRAY_LAST,
  BUILTIN_ARRAY_FIRST_INDEX,
  BUILTIN_ARRAY_INSERT,
  BUILTIN_ARRAY_REMOVE_AT,
  BUILTIN_ARRAY_SWAP_AT,
  BUILTIN_ARRAY_DROP_FIRST,
  BUILTIN_ARRAY_DROP_LAST,
  BUILTIN_ARRAY_PREFIX,
  BUILTIN_ARRAY_SUFFIX,
  BUILTIN_ARRAY_REVERSED,
  BUILTIN_ARRAY_MAX,
  BUILTIN_ARRAY_MIN,
  BUILTIN_ARRAY_SORTED,
  BUILTIN_ARRAY_SORTED_INT,
  BUILTIN_ARRAY_SORTED_STR,
  BUILTIN_ARRAY_SPLIT,
  BUILTIN_ARRAY_REPLACING_SUBRANGE,
  BUILTIN_ARRAY_REPLACE_RANGE,
  BUILTIN_ARRAY_CHUNKED,
  BUILTIN_ARRAY_WINDOWS,
  BUILTIN_ARRAY_ELEMENTS_EQUAL,
  BUILTIN_ARRAY_STARTS_WITH,
  BUILTIN_ARRAY_LEX_PRECEDES,
  BUILTIN_ARRAY_PRINT,
  BUILTIN_ARRAY_CAPACITY,
  BUILTIN_ARRAY_POP_LAST,
  BUILTIN_ARRAY_CLEAR,
  BUILTIN_ARRAY_CLEAR_DISCARDING_CAPACITY,
  BUILTIN_SUBSCRIPT,

  /* ── String ───────────────────────────────────────────────────────────────── */
  BUILTIN_STR_CONCAT,
  BUILTIN_STR_CONCAT_3,
  BUILTIN_STR_LEN,
  BUILTIN_STR_CHAR_AT,
  BUILTIN_STR_SUBSTR,
  BUILTIN_STR_CONTAINS,
  BUILTIN_STR_HAS_PREFIX,
  BUILTIN_STR_HAS_SUFFIX,
  BUILTIN_STR_LOWERCASED,
  BUILTIN_STR_UPPERCASED,
  BUILTIN_STR_REVERSED,
  BUILTIN_STR_SPLIT,
  BUILTIN_STR_SPLIT_EX,  /* split(separator:omittingEmptySubsequences:maxSplits:) */
  BUILTIN_STR_JOIN_ARRAY,
  BUILTIN_STR_EQ,
  BUILTIN_STR_NEQ,
  BUILTIN_STR_INTERP,
  BUILTIN_STR_FROM_INT,
  BUILTIN_STR_FROM_BOOL,
  BUILTIN_STR_FROM_DOUBLE,
  BUILTIN_STR_FROM_INT_RADIX,
  BUILTIN_STR_FIRST_INDEX,
  BUILTIN_STR_LAST_INDEX,
  BUILTIN_STR_REPEAT,
  BUILTIN_STR_TRIM,
  BUILTIN_STR_CAPITALIZED,
  BUILTIN_STR_IS_NUMBER,
  BUILTIN_STR_IS_LETTER,
  BUILTIN_STR_IS_WHITESPACE,
  BUILTIN_STR_IS_ALPHANUMERIC,
  BUILTIN_STR_IS_PUNCTUATION,
  BUILTIN_STR_IS_SYMBOL,
  BUILTIN_STR_IS_UPPERCASE,
  BUILTIN_STR_IS_LOWERCASE,
  BUILTIN_STR_IS_HEX_DIGIT,
  BUILTIN_STR_ASCII_VALUE,
  BUILTIN_STR_IS_ASCII,
  BUILTIN_STR_JOIN_ARRAY_SEP,
  BUILTIN_STR_PADDING,
  BUILTIN_STR_HASH,
  BUILTIN_SUBSTR_INIT,
  BUILTIN_SUBSTR_TO_STRING,
  BUILTIN_STR_DROP_FIRST_SUB,
  BUILTIN_STR_DROP_LAST_SUB,
  BUILTIN_SUBSTR_DROP_FIRST,
  BUILTIN_SUBSTR_DROP_LAST,
  BUILTIN_STR_PREFIX_SUB,
  BUILTIN_STR_SUFFIX_SUB,
  BUILTIN_SUBSTR_PREFIX,
  BUILTIN_SUBSTR_SUFFIX,
  BUILTIN_SUBSTR_LEN,
  BUILTIN_SUBSTR_CHAR_AT,
  /* Mutation: removeFirst / removeLast */
  BUILTIN_STR_REMOVE_FIRST,
  BUILTIN_STR_REMOVE_LAST,
  BUILTIN_STR_REMOVE_FIRST_K,
  BUILTIN_STR_REMOVE_LAST_K,
  BUILTIN_STR_CMP,           /* __str_cmp(a, b) → Int (<0, 0, >0) */
  BUILTIN_STR_INSERT,
  BUILTIN_STR_REMOVE_RANGE,
  BUILTIN_STR_REPLACE_SUBRANGE,
  BUILTIN_STR_UTF8_COUNT,
  BUILTIN_STR_UTF16_COUNT,

  /* ── Data ─────────────────────────────────────────────────────────────────── */
  BUILTIN_DATA_INIT,
  BUILTIN_DATA_INIT_COUNT,
  BUILTIN_DATA_FROM_BYTES,
  BUILTIN_DATA_FROM_STR,
  BUILTIN_DATA_COUNT,
  BUILTIN_DATA_IS_EMPTY,
  BUILTIN_DATA_APPEND_BYTE,
  BUILTIN_DATA_GET_BYTE,
  BUILTIN_DATA_BASE64_ENC,
  BUILTIN_DATA_BASE64_DEC,

  /* ── Dictionary ───────────────────────────────────────────────────────────── */
  BUILTIN_DICT_INIT,
  BUILTIN_DICT_SET,
  BUILTIN_DICT_GET,
  BUILTIN_DICT_CONTAINS,
  BUILTIN_DICT_REMOVE,
  BUILTIN_DICT_COUNT,
  BUILTIN_DICT_FREE,
  BUILTIN_DICT_KEYS_ARRAY,
  BUILTIN_DICT_VALUES_ARRAY,
  BUILTIN_DICT_GET_OR_INIT_ARR,  /* grouping:by: helper */
  BUILTIN_DICT_FIRST,              /* .first → (key, value) tuple ptr */
  BUILTIN_DICT_RANDOM_ELEMENT,     /* .randomElement() → (key, value) tuple ptr */
  BUILTIN_DICT_MAKE_ITERATOR,     /* .makeIterator() → iterator handle */
  BUILTIN_DICT_ITER_NEXT,         /* iterator.next() → Optional<(key,value)> tuple ptr or nil */

  /* ── Set ──────────────────────────────────────────────────────────────────── */
  BUILTIN_SET_INIT,
  BUILTIN_SET_INIT_STR,
  BUILTIN_SET_INSERT,
  BUILTIN_SET_INSERT_I64,
  BUILTIN_SET_CONTAINS,
  BUILTIN_SET_REMOVE,
  BUILTIN_SET_REMOVE_ALL,
  BUILTIN_SET_COUNT,
  BUILTIN_SET_GET_PTR,
  BUILTIN_SET_SORTED,
  BUILTIN_SET_UNION,
  BUILTIN_SET_INTERSECTION,
  BUILTIN_SET_SUBTRACTING,
  BUILTIN_SET_SYMMETRIC_DIFFERENCE,
  BUILTIN_SET_IS_SUBSET,
  BUILTIN_SET_IS_SUPERSET,
  BUILTIN_SET_IS_DISJOINT,

  /* ── Any (boxing/unboxing) ────────────────────────────────────────────────── */
  BUILTIN_ANY_BOX_INT,
  BUILTIN_ANY_BOX_PTR,
  BUILTIN_ANY_GET_TAG,
  BUILTIN_ANY_AS_INT,
  BUILTIN_ANY_AS_PTR,

  /* ── Print ────────────────────────────────────────────────────────────────── */
  BUILTIN_PRINT,
  BUILTIN_SWIFT_PRINT,
  BUILTIN_SWIFT_PRINT_ITEMS,

  /* ── Conversion / type check ──────────────────────────────────────────────── */
  BUILTIN_INT_FROM_STR,
  BUILTIN_SWIFT_IS_TYPE,

  /* ── Memory / lifecycle ───────────────────────────────────────────────────── */
  BUILTIN_MALLOC,
  BUILTIN_ALLOC_CLASS,
  BUILTIN_EXIT,
  BUILTIN_CALLOC,
  BUILTIN_MEMCPY,
  BUILTIN_SWIFT_FATAL_ERROR,
  BUILTIN_THROW,
  BUILTIN_CONSTRAINT_FAIL,
  BUILTIN_SWIFT_ASSERT,
  BUILTIN_SWIFT_PRECONDITION,

  /* ── Async / concurrency ──────────────────────────────────────────────────── */
  BUILTIN_SWIFT_TASK_INIT,
  BUILTIN_SWIFT_TASK_RUN,
  BUILTIN_SWIFT_TASK_AWAIT,
  BUILTIN_SWIFT_TASK_RESULT,
  BUILTIN_SWIFT_TASK_FREE,
  BUILTIN_SWIFT_TASKGROUP_INIT,
  BUILTIN_SWIFT_TASKGROUP_ADD,
  BUILTIN_SWIFT_TASKGROUP_AWAIT_ALL,
  BUILTIN_SWIFT_TASKGROUP_FREE,
  BUILTIN_SWIFT_MAIN_ACTOR_RUN,
  BUILTIN_ASYNC_STUB,       /* generic async/actor noop stub */

  /* ── Math ─────────────────────────────────────────────────────────────────── */
  BUILTIN_MATH_POW,
  BUILTIN_MATH_SQRT,
  BUILTIN_MATH_FLOOR,
  BUILTIN_MATH_CEIL,
  BUILTIN_MATH_ABS,
  BUILTIN_MATH_MIN,
  BUILTIN_MATH_MAX,
  BUILTIN_MATH_SIN,
  BUILTIN_MATH_COS,
  BUILTIN_MATH_TAN,
  BUILTIN_MATH_LOG,
  BUILTIN_MATH_LOG2,
  BUILTIN_MATH_LOG10,
  BUILTIN_MATH_EXP,
  BUILTIN_MATH_ATAN,
  BUILTIN_MATH_ROUND,
  BUILTIN_MATH_ATAN2,
  BUILTIN_MATH_ASIN,
  BUILTIN_MATH_ACOS,
  BUILTIN_MATH_SINH,
  BUILTIN_MATH_COSH,
  BUILTIN_MATH_TANH,
  BUILTIN_MATH_FMOD,

  /* ── Global math (Swift max/min/abs → stdlib import) ───────────────────────── */
  BUILTIN_SWIFT_MAX_F64,
  BUILTIN_SWIFT_MIN_F64,
  BUILTIN_SWIFT_ABS_F64,
  BUILTIN_SWIFT_ABS_I64,

  /* Date */
  BUILTIN_DATE_NOW,
  BUILTIN_UUID_NEW,

  /* URL */
  BUILTIN_URL_SCHEME,
  BUILTIN_URL_HOST,
  BUILTIN_URL_PATH,
  BUILTIN_URL_QUERY,
  BUILTIN_URL_USER,
  BUILTIN_URL_PASSWORD,
  BUILTIN_URL_PORT,
  BUILTIN_URL_FRAGMENT,
  BUILTIN_URL_APPEND_PATH,
  BUILTIN_URL_COMPONENTS_GET_URL,
  BUILTIN_URL_LAST_PATH_COMPONENT,
  BUILTIN_URL_PATH_EXTENSION,
  BUILTIN_URL_DELETING_LAST_PATH_COMPONENT,
  BUILTIN_URL_DELETING_PATH_EXTENSION,
  BUILTIN_URL_APPENDING_PATH_EXTENSION,
  BUILTIN_URL_APPENDING_QUERY_ITEMS,

  /* UserDefaults */
  BUILTIN_UD_SET_STR,
  BUILTIN_UD_SET_INT,
  BUILTIN_UD_SET_BOOL,
  BUILTIN_UD_GET_STR,
  BUILTIN_UD_GET_INT,
  BUILTIN_UD_GET_BOOL,
  BUILTIN_UD_REMOVE,

  /* JSON parsing */
  BUILTIN_JSON_VALUE,
  BUILTIN_JSON_ARRAY_COUNT,
  BUILTIN_JSON_ARRAY_ITEM,

  /* Date formatting */
  BUILTIN_DATE_ISO8601,
  BUILTIN_DATE_FORMATTER_STRING,
  BUILTIN_DATE_FROM_COMPONENTS,

  /* Calendar/Date runtime */
  BUILTIN_DATE_COMPARE,
  BUILTIN_DATE_START_OF_DAY,
  BUILTIN_DATE_IS_IN_TODAY,
  BUILTIN_DATE_IS_IN_TOMORROW,
  BUILTIN_DATE_IS_IN_YESTERDAY,
  BUILTIN_DATE_IS_IN_WEEKEND,
  BUILTIN_DATE_IS_SAME_DAY,
  BUILTIN_DATE_IS_EQUAL_GRAN,
  BUILTIN_DATE_TO_COMPONENTS,
  BUILTIN_DATE_ADD_YEARS,
  BUILTIN_DATE_ADD_MONTHS,
  BUILTIN_DATE_ADD_DAYS,
  BUILTIN_DATE_ADD_HOURS,
  BUILTIN_DATE_ADD_MINUTES,
  BUILTIN_DATE_ADD_SECONDS,
  BUILTIN_DATE_ADD_WEEKS,
  BUILTIN_DATE_ADD_QUARTERS,
  BUILTIN_DATE_BY_ADDING_COMPONENTS,
  BUILTIN_DATE_BY_SETTING,
  BUILTIN_DATE_ORDINALITY_OF_IN,
  BUILTIN_DATE_RANGE_OF_IN,
  BUILTIN_DATE_BY_SETTING_TIME,
  BUILTIN_DATE_MATCHES_COMPONENTS,
  BUILTIN_DATE_COMPONENTS_DIFF,
  BUILTIN_DATE_COMPONENT,          /* component(_:from:) → FIDX_DATE_COMPONENT */

  BUILTIN_ALARM_SCHEDULE,
  BUILTIN_ALARM_CANCEL,
  BUILTIN_ALARM_PAUSE,
  BUILTIN_ALARM_RESUME,
  BUILTIN_ALARM_STOP,
  BUILTIN_ALARM_REQUEST_AUTH,
  BUILTIN_ALARM_GET_ALL,
  BUILTIN_ALARM_COUNTDOWN,

  /* AlarmPresentationState */
  BUILTIN_ALARM_PS_READ,
  BUILTIN_ALARM_PS_MODE,

  BUILTIN_STR_CODEPOINT_COUNT,

  BUILTIN__COUNT  /* sentinel */
} BuiltinID;

/* ── resolve_builtin: call_name (with optional '@' prefix) → BuiltinID ─────── */
/* Called once per IR_CALL/IR_INVOKE during IR construction.                     */
/* Returns BUILTIN_NONE for user functions and unknown names.                    */

static inline BuiltinID resolve_builtin_id(const char *raw_name) {
  if (!raw_name) return BUILTIN_NONE;
  const char *n = (raw_name[0] == '@') ? (raw_name + 1) : raw_name;

  /* ── Array ── */
  if (strcmp(n, BN_ARRAY_INIT)     == 0) return BUILTIN_ARRAY_INIT;
  if (strcmp(n, "__array_init_n")   == 0) return BUILTIN_ARRAY_INIT_N;
  if (strcmp(n, BN_ARRAY_APPEND)     == 0) return BUILTIN_ARRAY_APPEND;
  if (strcmp(n, BN_ARRAY_LEN)        == 0) return BUILTIN_ARRAY_LEN;
  if (strcmp(n, BN_ARRAY_GET_PTR)    == 0) return BUILTIN_ARRAY_GET_PTR;
  if (strcmp(n, BN_ARRAY_SET)        == 0) return BUILTIN_ARRAY_SET;
  if (strcmp(n, BN_ARRAY_CONCAT)     == 0) return BUILTIN_ARRAY_CONCAT;
  if (strcmp(n, BN_ARRAY_PLUS)       == 0) return BUILTIN_ARRAY_PLUS;
  if (strcmp(n, BN_ARRAY_CONTAINS)   == 0) return BUILTIN_ARRAY_CONTAINS;
  if (strcmp(n, BN_ARRAY_FIRST)      == 0) return BUILTIN_ARRAY_FIRST;
  if (strcmp(n, BN_ARRAY_LAST)       == 0) return BUILTIN_ARRAY_LAST;
  if (strcmp(n, BN_ARRAY_FIRST_INDEX)== 0) return BUILTIN_ARRAY_FIRST_INDEX;
  if (strcmp(n, BN_ARRAY_INSERT)     == 0) return BUILTIN_ARRAY_INSERT;
  if (strcmp(n, BN_ARRAY_REMOVE_AT)  == 0) return BUILTIN_ARRAY_REMOVE_AT;
  if (strcmp(n, BN_ARRAY_SWAP_AT)    == 0) return BUILTIN_ARRAY_SWAP_AT;
  if (strcmp(n, BN_ARRAY_DROP_FIRST) == 0) return BUILTIN_ARRAY_DROP_FIRST;
  if (strcmp(n, BN_ARRAY_DROP_LAST)  == 0) return BUILTIN_ARRAY_DROP_LAST;
  if (strcmp(n, BN_ARRAY_PREFIX)     == 0) return BUILTIN_ARRAY_PREFIX;
  if (strcmp(n, BN_ARRAY_SUFFIX)     == 0) return BUILTIN_ARRAY_SUFFIX;
  if (strcmp(n, BN_ARRAY_REVERSED)   == 0) return BUILTIN_ARRAY_REVERSED;
  if (strcmp(n, BN_ARRAY_MAX)        == 0) return BUILTIN_ARRAY_MAX;
  if (strcmp(n, BN_ARRAY_MIN)        == 0) return BUILTIN_ARRAY_MIN;
  if (strcmp(n, BN_ARRAY_SORTED)     == 0) return BUILTIN_ARRAY_SORTED;
  if (strcmp(n, BN_ARRAY_SORTED_INT) == 0) return BUILTIN_ARRAY_SORTED_INT;
  if (strcmp(n, BN_ARRAY_SORTED_STR) == 0) return BUILTIN_ARRAY_SORTED_STR;
  if (strcmp(n, BN_ARRAY_SPLIT)      == 0) return BUILTIN_ARRAY_SPLIT;
  if (strcmp(n, BN_ARRAY_REPLACING_SUBRANGE) == 0) return BUILTIN_ARRAY_REPLACING_SUBRANGE;
  if (strcmp(n, BN_ARRAY_REPLACE_RANGE) == 0) return BUILTIN_ARRAY_REPLACE_RANGE;
  if (strcmp(n, BN_ARRAY_CHUNKED)    == 0) return BUILTIN_ARRAY_CHUNKED;
  if (strcmp(n, BN_ARRAY_WINDOWS)    == 0) return BUILTIN_ARRAY_WINDOWS;
  if (strcmp(n, BN_ARRAY_ELEMENTS_EQUAL) == 0) return BUILTIN_ARRAY_ELEMENTS_EQUAL;
  if (strcmp(n, BN_ARRAY_STARTS_WITH)    == 0) return BUILTIN_ARRAY_STARTS_WITH;
  if (strcmp(n, BN_ARRAY_LEX_PRECEDES)   == 0) return BUILTIN_ARRAY_LEX_PRECEDES;
  if (strcmp(n, BN_ARRAY_PRINT)          == 0) return BUILTIN_ARRAY_PRINT;
  if (strcmp(n, BN_ARRAY_CAPACITY)       == 0) return BUILTIN_ARRAY_CAPACITY;
  if (strcmp(n, BN_ARRAY_POP_LAST)       == 0) return BUILTIN_ARRAY_POP_LAST;
  if (strcmp(n, BN_ARRAY_CLEAR) == 0) return BUILTIN_ARRAY_CLEAR;
  if (strcmp(n, BN_ARRAY_CLEAR_DISCARDING_CAPACITY) == 0)
    return BUILTIN_ARRAY_CLEAR_DISCARDING_CAPACITY;
  if (strcmp(n, BN_SUBSCRIPT)        == 0) return BUILTIN_SUBSCRIPT;

  /* ── String ── */
  if (strcmp(n, BN_STR_CONCAT)       == 0) return BUILTIN_STR_CONCAT;
  if (strcmp(n, BN_STR_CONCAT_3)     == 0) return BUILTIN_STR_CONCAT_3;
  if (strcmp(n, BN_STR_LEN)          == 0) return BUILTIN_STR_LEN;
  if (strcmp(n, BN_STR_CHAR_AT)      == 0) return BUILTIN_STR_CHAR_AT;
  if (strcmp(n, BN_STR_SUBSTR)       == 0) return BUILTIN_STR_SUBSTR;
  if (strcmp(n, BN_STR_CONTAINS)     == 0) return BUILTIN_STR_CONTAINS;
  if (strcmp(n, BN_STR_HAS_PREFIX)   == 0) return BUILTIN_STR_HAS_PREFIX;
  if (strcmp(n, BN_STR_HAS_SUFFIX)   == 0) return BUILTIN_STR_HAS_SUFFIX;
  if (strcmp(n, BN_STR_LOWERCASED)   == 0) return BUILTIN_STR_LOWERCASED;
  if (strcmp(n, BN_STR_UPPERCASED)   == 0) return BUILTIN_STR_UPPERCASED;
  if (strcmp(n, BN_STR_REVERSED)     == 0) return BUILTIN_STR_REVERSED;
  if (strcmp(n, BN_STR_SPLIT)        == 0) return BUILTIN_STR_SPLIT;
  if (strcmp(n, BN_STR_SPLIT_EX)     == 0) return BUILTIN_STR_SPLIT_EX;
  if (strcmp(n, BN_STR_JOIN_ARRAY)   == 0) return BUILTIN_STR_JOIN_ARRAY;
  if (strcmp(n, BN_STR_EQ)           == 0) return BUILTIN_STR_EQ;
  if (strcmp(n, BN_STR_NEQ)          == 0) return BUILTIN_STR_NEQ;
  if (strcmp(n, BN_STR_INTERP)       == 0) return BUILTIN_STR_INTERP;
  if (strcmp(n, BN_STR_FROM_INT)          == 0) return BUILTIN_STR_FROM_INT;
  if (strcmp(n, BN_STR_FROM_BOOL)         == 0) return BUILTIN_STR_FROM_BOOL;
  if (strcmp(n, BN_STR_FROM_DOUBLE)       == 0) return BUILTIN_STR_FROM_DOUBLE;
  if (strcmp(n, BN_STR_FROM_INT_RADIX)    == 0) return BUILTIN_STR_FROM_INT_RADIX;
  if (strcmp(n, BN_STR_FIRST_INDEX)  == 0) return BUILTIN_STR_FIRST_INDEX;
  if (strcmp(n, BN_STR_LAST_INDEX)   == 0) return BUILTIN_STR_LAST_INDEX;
  if (strcmp(n, BN_STR_REPEAT)       == 0) return BUILTIN_STR_REPEAT;
  if (strcmp(n, BN_STR_TRIM)         == 0) return BUILTIN_STR_TRIM;
  if (strcmp(n, BN_STR_CAPITALIZED)  == 0) return BUILTIN_STR_CAPITALIZED;
  if (strcmp(n, BN_STR_IS_NUMBER)    == 0) return BUILTIN_STR_IS_NUMBER;
  if (strcmp(n, BN_STR_IS_LETTER)    == 0) return BUILTIN_STR_IS_LETTER;
  if (strcmp(n, BN_STR_IS_WHITESPACE)== 0) return BUILTIN_STR_IS_WHITESPACE;
  if (strcmp(n, BN_STR_IS_ALPHANUMERIC)==0) return BUILTIN_STR_IS_ALPHANUMERIC;
  if (strcmp(n, BN_STR_IS_PUNCTUATION)==0) return BUILTIN_STR_IS_PUNCTUATION;
  if (strcmp(n, BN_STR_IS_SYMBOL)    == 0) return BUILTIN_STR_IS_SYMBOL;
  if (strcmp(n, BN_STR_IS_UPPERCASE) == 0) return BUILTIN_STR_IS_UPPERCASE;
  if (strcmp(n, BN_STR_IS_LOWERCASE) == 0) return BUILTIN_STR_IS_LOWERCASE;
  if (strcmp(n, BN_STR_IS_HEX_DIGIT) == 0) return BUILTIN_STR_IS_HEX_DIGIT;
  if (strcmp(n, BN_STR_ASCII_VALUE)  == 0) return BUILTIN_STR_ASCII_VALUE;
  if (strcmp(n, BN_STR_IS_ASCII)     == 0) return BUILTIN_STR_IS_ASCII;
  if (strcmp(n, BN_STR_JOIN_ARRAY_SEP)==0) return BUILTIN_STR_JOIN_ARRAY_SEP;
  if (strcmp(n, BN_STR_PADDING)      == 0) return BUILTIN_STR_PADDING;
  if (strcmp(n, BN_STR_HASH)         == 0) return BUILTIN_STR_HASH;
  if (strcmp(n, BN_SUBSTR_INIT)    == 0) return BUILTIN_SUBSTR_INIT;
  if (strcmp(n, BN_SUBSTR_TO_STRING) == 0) return BUILTIN_SUBSTR_TO_STRING;
  if (strcmp(n, BN_STR_DROP_FIRST_SUB)== 0) return BUILTIN_STR_DROP_FIRST_SUB;
  if (strcmp(n, BN_STR_DROP_LAST_SUB) == 0) return BUILTIN_STR_DROP_LAST_SUB;
  if (strcmp(n, BN_SUBSTR_DROP_FIRST)== 0) return BUILTIN_SUBSTR_DROP_FIRST;
  if (strcmp(n, BN_SUBSTR_DROP_LAST) == 0) return BUILTIN_SUBSTR_DROP_LAST;
  if (strcmp(n, BN_STR_PREFIX_SUB)   == 0) return BUILTIN_STR_PREFIX_SUB;
  if (strcmp(n, BN_STR_SUFFIX_SUB)   == 0) return BUILTIN_STR_SUFFIX_SUB;
  if (strcmp(n, BN_STR_REMOVE_FIRST)  == 0) return BUILTIN_STR_REMOVE_FIRST;
  if (strcmp(n, BN_STR_REMOVE_LAST)   == 0) return BUILTIN_STR_REMOVE_LAST;
  if (strcmp(n, BN_STR_REMOVE_FIRST_K)== 0) return BUILTIN_STR_REMOVE_FIRST_K;
  if (strcmp(n, BN_STR_REMOVE_LAST_K) == 0) return BUILTIN_STR_REMOVE_LAST_K;
  if (strcmp(n, BN_SUBSTR_PREFIX)    == 0) return BUILTIN_SUBSTR_PREFIX;
  if (strcmp(n, BN_SUBSTR_SUFFIX)    == 0) return BUILTIN_SUBSTR_SUFFIX;
  if (strcmp(n, BN_SUBSTR_LEN)       == 0) return BUILTIN_SUBSTR_LEN;
  if (strcmp(n, BN_SUBSTR_CHAR_AT)   == 0) return BUILTIN_SUBSTR_CHAR_AT;
  if (strcmp(n, BN_STR_CMP)          == 0) return BUILTIN_STR_CMP;
  if (strcmp(n, BN_STR_INSERT)       == 0) return BUILTIN_STR_INSERT;
  if (strcmp(n, BN_STR_REMOVE_RANGE) == 0) return BUILTIN_STR_REMOVE_RANGE;
  if (strcmp(n, BN_STR_REPLACE_SUBRANGE) == 0) return BUILTIN_STR_REPLACE_SUBRANGE;
  if (strcmp(n, BN_STR_UTF8_COUNT)   == 0) return BUILTIN_STR_UTF8_COUNT;
  if (strcmp(n, BN_STR_UTF16_COUNT)  == 0) return BUILTIN_STR_UTF16_COUNT;

  /* ── Data ── */
  if (strcmp(n, BN_DATA_INIT)      == 0) return BUILTIN_DATA_INIT;
  if (strcmp(n, BN_DATA_INIT_COUNT)== 0) return BUILTIN_DATA_INIT_COUNT;
  if (strcmp(n, BN_DATA_FROM_BYTES)  == 0) return BUILTIN_DATA_FROM_BYTES;
  if (strcmp(n, BN_DATA_FROM_STR)    == 0) return BUILTIN_DATA_FROM_STR;
  if (strcmp(n, BN_DATA_COUNT)       == 0) return BUILTIN_DATA_COUNT;
  if (strcmp(n, BN_DATA_IS_EMPTY)    == 0) return BUILTIN_DATA_IS_EMPTY;
  if (strcmp(n, BN_DATA_APPEND_BYTE) == 0) return BUILTIN_DATA_APPEND_BYTE;
  if (strcmp(n, BN_DATA_GET_BYTE)    == 0) return BUILTIN_DATA_GET_BYTE;
  if (strcmp(n, BN_DATA_BASE64_ENC)  == 0) return BUILTIN_DATA_BASE64_ENC;
  if (strcmp(n, BN_DATA_BASE64_DEC)  == 0) return BUILTIN_DATA_BASE64_DEC;

  /* ── Dictionary ── */
  if (strcmp(n, BN_DICT_INIT)      == 0) return BUILTIN_DICT_INIT;
  if (strcmp(n, BN_DICT_SET)         == 0) return BUILTIN_DICT_SET;
  if (strcmp(n, BN_DICT_GET)         == 0) return BUILTIN_DICT_GET;
  if (strcmp(n, BN_DICT_CONTAINS)    == 0) return BUILTIN_DICT_CONTAINS;
  if (strcmp(n, BN_DICT_REMOVE)      == 0) return BUILTIN_DICT_REMOVE;
  if (strcmp(n, BN_DICT_COUNT)       == 0) return BUILTIN_DICT_COUNT;
  if (strcmp(n, BN_DICT_FREE)        == 0) return BUILTIN_DICT_FREE;
  if (strcmp(n, BN_DICT_KEYS_ARRAY)  == 0) return BUILTIN_DICT_KEYS_ARRAY;
  if (strcmp(n, BN_DICT_VALUES_ARRAY)== 0) return BUILTIN_DICT_VALUES_ARRAY;
  if (strcmp(n, BN_DICT_GET_OR_INIT_ARR) == 0) return BUILTIN_DICT_GET_OR_INIT_ARR;
  if (strcmp(n, "__dict_first") == 0) return BUILTIN_DICT_FIRST;
  if (strcmp(n, "__dict_random_element") == 0) return BUILTIN_DICT_RANDOM_ELEMENT;
  if (strcmp(n, BN_DICT_MAKE_ITERATOR) == 0) return BUILTIN_DICT_MAKE_ITERATOR;
  if (strcmp(n, BN_DICT_ITER_NEXT) == 0) return BUILTIN_DICT_ITER_NEXT;

  /* ── Set ── */
  if (strcmp(n, BN_SET_INIT)       == 0) return BUILTIN_SET_INIT;
  if (strcmp(n, BN_SET_INIT_STR)   == 0) return BUILTIN_SET_INIT_STR;
  if (strcmp(n, BN_SET_INSERT)       == 0) return BUILTIN_SET_INSERT;
  if (strcmp(n, BN_SET_INSERT_I64)   == 0) return BUILTIN_SET_INSERT_I64;
  if (strcmp(n, BN_SET_CONTAINS)     == 0) return BUILTIN_SET_CONTAINS;
  if (strcmp(n, BN_SET_REMOVE)       == 0) return BUILTIN_SET_REMOVE;
  if (strcmp(n, BN_SET_REMOVE_ALL)   == 0) return BUILTIN_SET_REMOVE_ALL;
  if (strcmp(n, BN_SET_COUNT)        == 0) return BUILTIN_SET_COUNT;
  if (strcmp(n, BN_SET_GET_PTR)      == 0) return BUILTIN_SET_GET_PTR;
  if (strcmp(n, BN_SET_SORTED)       == 0) return BUILTIN_SET_SORTED;
  if (strcmp(n, BN_SET_UNION)        == 0) return BUILTIN_SET_UNION;
  if (strcmp(n, BN_SET_INTERSECTION) == 0) return BUILTIN_SET_INTERSECTION;
  if (strcmp(n, BN_SET_SUBTRACTING)  == 0) return BUILTIN_SET_SUBTRACTING;
  if (strcmp(n, BN_SET_SYMMETRIC_DIFFERENCE) == 0) return BUILTIN_SET_SYMMETRIC_DIFFERENCE;
  if (strcmp(n, BN_SET_IS_SUBSET)    == 0) return BUILTIN_SET_IS_SUBSET;
  if (strcmp(n, BN_SET_IS_SUPERSET)  == 0) return BUILTIN_SET_IS_SUPERSET;
  if (strcmp(n, BN_SET_IS_DISJOINT)  == 0) return BUILTIN_SET_IS_DISJOINT;

  /* ── Any ── */
  if (strcmp(n, BN_ANY_BOX_INT)      == 0) return BUILTIN_ANY_BOX_INT;
  if (strcmp(n, BN_ANY_BOX_PTR)      == 0) return BUILTIN_ANY_BOX_PTR;
  if (strcmp(n, BN_ANY_GET_TAG)      == 0) return BUILTIN_ANY_GET_TAG;
  if (strcmp(n, BN_ANY_AS_INT)       == 0) return BUILTIN_ANY_AS_INT;
  if (strcmp(n, BN_ANY_AS_PTR)       == 0) return BUILTIN_ANY_AS_PTR;

  /* ── Print ── */
  if (strcmp(n, BN_PRINT)            == 0) return BUILTIN_PRINT;
  if (strcmp(n, BN_SWIFT_PRINT)      == 0) return BUILTIN_SWIFT_PRINT;
  if (strcmp(n, BN_SWIFT_PRINT_ITEMS)== 0) return BUILTIN_SWIFT_PRINT_ITEMS;

  /* ── Conversion ── */
  if (strcmp(n, BN_INT_FROM_STR)     == 0) return BUILTIN_INT_FROM_STR;
  if (strcmp(n, BN_SWIFT_IS_TYPE)    == 0) return BUILTIN_SWIFT_IS_TYPE;

  /* ── Memory / lifecycle ── */
  if (strcmp(n, "malloc")            == 0) return BUILTIN_MALLOC;
  if (strcmp(n, BN_ALLOC_CLASS)      == 0) return BUILTIN_ALLOC_CLASS;
  if (strcmp(n, "exit")              == 0) return BUILTIN_EXIT;
  if (strcmp(n, "calloc")            == 0) return BUILTIN_CALLOC;
  if (strcmp(n, "memcpy")            == 0) return BUILTIN_MEMCPY;
  if (strcmp(n, BN_SWIFT_FATAL_ERROR)     == 0) return BUILTIN_SWIFT_FATAL_ERROR;
  if (strcmp(n, BN_THROW)                 == 0) return BUILTIN_THROW;
  if (strcmp(n, BN_CONSTRAINT_FAIL)       == 0) return BUILTIN_CONSTRAINT_FAIL;
  if (strcmp(n, BN_SWIFT_ASSERT)          == 0) return BUILTIN_SWIFT_ASSERT;
  if (strcmp(n, BN_SWIFT_PRECONDITION)    == 0) return BUILTIN_SWIFT_PRECONDITION;

  /* ── Async / concurrency ── */
  if (strcmp(n, BN_SWIFT_TASK_INIT) == 0) return BUILTIN_SWIFT_TASK_INIT;
  if (strcmp(n, BN_SWIFT_TASK_RUN)    == 0) return BUILTIN_SWIFT_TASK_RUN;
  if (strcmp(n, "__swift_task_await")  == 0) return BUILTIN_SWIFT_TASK_AWAIT;
  if (strcmp(n, "__swift_task_result") == 0) return BUILTIN_SWIFT_TASK_RESULT;
  if (strcmp(n, "__swift_task_free")   == 0) return BUILTIN_SWIFT_TASK_FREE;
  if (strcmp(n, "__swift_taskgroup_init")    == 0) return BUILTIN_SWIFT_TASKGROUP_INIT;
  if (strcmp(n, "__swift_taskgroup_add")       == 0) return BUILTIN_SWIFT_TASKGROUP_ADD;
  if (strcmp(n, "__swift_taskgroup_await_all") == 0) return BUILTIN_SWIFT_TASKGROUP_AWAIT_ALL;
  if (strcmp(n, "__swift_taskgroup_free")      == 0) return BUILTIN_SWIFT_TASKGROUP_FREE;
  if (strcmp(n, "__swift_main_actor_run")      == 0) return BUILTIN_SWIFT_MAIN_ACTOR_RUN;
  /* Generic async/actor stubs — prefix match */
  if (strncmp(n, "__swift_task_",    13) == 0 ||
      strncmp(n, "__swift_async_",   14) == 0 ||
      strncmp(n, "__swift_taskgroup_",17) == 0 ||
      strncmp(n, "__actor_",          8) == 0 ||
      strncmp(n, "__main_actor_",    13) == 0 ||
      strncmp(n, "__swift_main_actor_",19) == 0)
    return BUILTIN_ASYNC_STUB;

  /* ── Math ── */
  if (strcmp(n, "pow")   == 0) return BUILTIN_MATH_POW;
  if (strcmp(n, "sqrt")  == 0) return BUILTIN_MATH_SQRT;
  if (strcmp(n, "floor") == 0) return BUILTIN_MATH_FLOOR;
  if (strcmp(n, "ceil")  == 0) return BUILTIN_MATH_CEIL;
  if (strcmp(n, "abs")   == 0) return BUILTIN_MATH_ABS;
  if (strcmp(n, "min")   == 0) return BUILTIN_MATH_MIN;
  if (strcmp(n, "max")   == 0) return BUILTIN_MATH_MAX;
  if (strcmp(n, "sin")   == 0) return BUILTIN_MATH_SIN;
  if (strcmp(n, "cos")   == 0) return BUILTIN_MATH_COS;
  if (strcmp(n, "tan")   == 0) return BUILTIN_MATH_TAN;
  if (strcmp(n, "log")   == 0) return BUILTIN_MATH_LOG;
  if (strcmp(n, "log2")  == 0) return BUILTIN_MATH_LOG2;
  if (strcmp(n, "log10") == 0) return BUILTIN_MATH_LOG10;
  if (strcmp(n, "exp")   == 0) return BUILTIN_MATH_EXP;
  if (strcmp(n, "atan")  == 0) return BUILTIN_MATH_ATAN;
  if (strcmp(n, "round") == 0) return BUILTIN_MATH_ROUND;
  if (strcmp(n, "atan2") == 0) return BUILTIN_MATH_ATAN2;
  if (strcmp(n, "asin")  == 0) return BUILTIN_MATH_ASIN;
  if (strcmp(n, "acos")  == 0) return BUILTIN_MATH_ACOS;
  if (strcmp(n, "sinh")  == 0) return BUILTIN_MATH_SINH;
  if (strcmp(n, "cosh")  == 0) return BUILTIN_MATH_COSH;
  if (strcmp(n, "tanh")  == 0) return BUILTIN_MATH_TANH;
  if (strcmp(n, "fmod")  == 0) return BUILTIN_MATH_FMOD;
  if (strcmp(n, "__swift_max_f64") == 0) return BUILTIN_SWIFT_MAX_F64;
  if (strcmp(n, "__swift_min_f64") == 0) return BUILTIN_SWIFT_MIN_F64;
  if (strcmp(n, "__swift_abs_f64") == 0) return BUILTIN_SWIFT_ABS_F64;
  if (strcmp(n, "__swift_abs_i64") == 0) return BUILTIN_SWIFT_ABS_I64;

  /* ── Date ── */
  if (strcmp(n, "__date_now") == 0) return BUILTIN_DATE_NOW;
  if (strcmp(n, "__uuid_new") == 0) return BUILTIN_UUID_NEW;

  /* ── URL ── */
  if (strcmp(n, "__url_scheme")      == 0) return BUILTIN_URL_SCHEME;
  if (strcmp(n, "__url_host")        == 0) return BUILTIN_URL_HOST;
  if (strcmp(n, "__url_path")        == 0) return BUILTIN_URL_PATH;
  if (strcmp(n, "__url_query")       == 0) return BUILTIN_URL_QUERY;
  if (strcmp(n, "__url_user")        == 0) return BUILTIN_URL_USER;
  if (strcmp(n, "__url_password")    == 0) return BUILTIN_URL_PASSWORD;
  if (strcmp(n, "__url_port")        == 0) return BUILTIN_URL_PORT;
  if (strcmp(n, "__url_fragment")    == 0) return BUILTIN_URL_FRAGMENT;
  if (strcmp(n, "__url_append_path") == 0) return BUILTIN_URL_APPEND_PATH;
  if (strcmp(n, "__url_components_get_url") == 0) return BUILTIN_URL_COMPONENTS_GET_URL;
  if (strcmp(n, "__url_last_path_component") == 0) return BUILTIN_URL_LAST_PATH_COMPONENT;
  if (strcmp(n, "__url_path_extension")      == 0) return BUILTIN_URL_PATH_EXTENSION;
  if (strcmp(n, "__url_deleting_last_path_component") == 0) return BUILTIN_URL_DELETING_LAST_PATH_COMPONENT;
  if (strcmp(n, "__url_deleting_path_extension")      == 0) return BUILTIN_URL_DELETING_PATH_EXTENSION;
  if (strcmp(n, "__url_appending_path_extension")     == 0) return BUILTIN_URL_APPENDING_PATH_EXTENSION;
  if (strcmp(n, "__url_appending_query_items")        == 0) return BUILTIN_URL_APPENDING_QUERY_ITEMS;

  /* ── UserDefaults ── */
  if (strcmp(n, "__ud_set_str")  == 0) return BUILTIN_UD_SET_STR;
  if (strcmp(n, "__ud_set_int")  == 0) return BUILTIN_UD_SET_INT;
  if (strcmp(n, "__ud_set_bool") == 0) return BUILTIN_UD_SET_BOOL;
  if (strcmp(n, "__ud_get_str")  == 0) return BUILTIN_UD_GET_STR;
  if (strcmp(n, "__ud_get_int")  == 0) return BUILTIN_UD_GET_INT;
  if (strcmp(n, "__ud_get_bool") == 0) return BUILTIN_UD_GET_BOOL;
  if (strcmp(n, "__ud_remove")   == 0) return BUILTIN_UD_REMOVE;

  /* ── JSON ── */
  if (strcmp(n, "__json_value")       == 0) return BUILTIN_JSON_VALUE;
  if (strcmp(n, "__json_array_count") == 0) return BUILTIN_JSON_ARRAY_COUNT;
  if (strcmp(n, "__json_array_item")  == 0) return BUILTIN_JSON_ARRAY_ITEM;

  /* ── Date ── */
  if (strcmp(n, "__date_iso8601")          == 0) return BUILTIN_DATE_ISO8601;
  if (strcmp(n, "__date_formatter_string") == 0) return BUILTIN_DATE_FORMATTER_STRING;
  if (strcmp(n, "__date_from_components")  == 0) return BUILTIN_DATE_FROM_COMPONENTS;

  /* ── Calendar/Date runtime ── */
  if (strcmp(n, "__date_compare")              == 0) return BUILTIN_DATE_COMPARE;
  if (strcmp(n, "__date_start_of_day")         == 0) return BUILTIN_DATE_START_OF_DAY;
  if (strcmp(n, "__date_is_in_today")          == 0) return BUILTIN_DATE_IS_IN_TODAY;
  if (strcmp(n, "__date_is_in_tomorrow")       == 0) return BUILTIN_DATE_IS_IN_TOMORROW;
  if (strcmp(n, "__date_is_in_yesterday")      == 0) return BUILTIN_DATE_IS_IN_YESTERDAY;
  if (strcmp(n, "__date_is_in_weekend")        == 0) return BUILTIN_DATE_IS_IN_WEEKEND;
  if (strcmp(n, "__date_is_same_day")          == 0) return BUILTIN_DATE_IS_SAME_DAY;
  if (strcmp(n, "__date_is_equal_to_granularity") == 0) return BUILTIN_DATE_IS_EQUAL_GRAN;
  if (strcmp(n, "__date_to_components")        == 0) return BUILTIN_DATE_TO_COMPONENTS;
  if (strcmp(n, "__date_add_years")            == 0) return BUILTIN_DATE_ADD_YEARS;
  if (strcmp(n, "__date_add_months")           == 0) return BUILTIN_DATE_ADD_MONTHS;
  if (strcmp(n, "__date_add_days")             == 0) return BUILTIN_DATE_ADD_DAYS;
  if (strcmp(n, "__date_add_hours")            == 0) return BUILTIN_DATE_ADD_HOURS;
  if (strcmp(n, "__date_add_minutes")          == 0) return BUILTIN_DATE_ADD_MINUTES;
  if (strcmp(n, "__date_add_seconds")          == 0) return BUILTIN_DATE_ADD_SECONDS;
  if (strcmp(n, "__date_add_weeks")            == 0) return BUILTIN_DATE_ADD_WEEKS;
  if (strcmp(n, "__date_add_quarters")         == 0) return BUILTIN_DATE_ADD_QUARTERS;
  if (strcmp(n, "__date_by_adding_components") == 0) return BUILTIN_DATE_BY_ADDING_COMPONENTS;
  if (strcmp(n, "__date_by_setting")           == 0) return BUILTIN_DATE_BY_SETTING;
  if (strcmp(n, "__date_ordinality_of_in")     == 0) return BUILTIN_DATE_ORDINALITY_OF_IN;
  if (strcmp(n, "__date_range_of_in")          == 0) return BUILTIN_DATE_RANGE_OF_IN;
  if (strcmp(n, "__date_by_setting_time")      == 0) return BUILTIN_DATE_BY_SETTING_TIME;
  if (strcmp(n, "__date_matches_components")   == 0) return BUILTIN_DATE_MATCHES_COMPONENTS;
  if (strcmp(n, "__date_components_diff")      == 0) return BUILTIN_DATE_COMPONENTS_DIFF;
  if (strcmp(n, "__date_component")            == 0) return BUILTIN_DATE_COMPONENT;

  /* ── Alarm builtins ── */
  if (strcmp(n, BN_ALARM_SCHEDULE)      == 0) return BUILTIN_ALARM_SCHEDULE;
  if (strcmp(n, BN_ALARM_CANCEL)        == 0) return BUILTIN_ALARM_CANCEL;
  if (strcmp(n, BN_ALARM_PAUSE)         == 0) return BUILTIN_ALARM_PAUSE;
  if (strcmp(n, BN_ALARM_RESUME)        == 0) return BUILTIN_ALARM_RESUME;
  if (strcmp(n, BN_ALARM_STOP)          == 0) return BUILTIN_ALARM_STOP;
  if (strcmp(n, BN_ALARM_REQUEST_AUTH)  == 0) return BUILTIN_ALARM_REQUEST_AUTH;
  if (strcmp(n, BN_ALARM_GET_ALL)       == 0) return BUILTIN_ALARM_GET_ALL;
  if (strcmp(n, BN_ALARM_COUNTDOWN)     == 0) return BUILTIN_ALARM_COUNTDOWN;

  /* AlarmPresentationState */
  if (strcmp(n, BN_ALARM_PS_READ)       == 0) return BUILTIN_ALARM_PS_READ;
  if (strcmp(n, BN_ALARM_PS_MODE)       == 0) return BUILTIN_ALARM_PS_MODE;

  if (strcmp(n, BN_STR_CODEPOINT_COUNT) == 0) return BUILTIN_STR_CODEPOINT_COUNT;

  return BUILTIN_NONE;
}

/* ═════════════════════════════════════════════════════════════════════════════
 * Temp local names for call/ builtins (lmap_get_or_add keys)
 * ═════════════════════════════════════════════════════════════════════════════ */

/* ── print temps ────────────────────────────────────────────────────────────── */
#define TMP_FLOAT_STR      "__tmp_float_str"
#define TMP_STR_LEN        "__str_len"
#define TMP_STR_PTR        "__str_ptr"

/* ── convert (Int.init from String) temps ───────────────────────────────────── */
#define TMP_IFS_PTR        "__ifs_ptr"
#define TMP_IFS_ACC        "__ifs_acc"
#define TMP_IFS_CH         "__ifs_ch"
#define TMP_IFS_NEG        "__ifs_neg"

/* ── libc: calloc/memcpy temps ──────────────────────────────────────────────── */
#define TMP_CAC_TOTAL      "__cac_total"
#define TMP_CAC_PTR        "__cac_ptr"
#define TMP_CAC_I          "__cac_i"
#define TMP_CAC_NB         "__cac_nb"
#define TMP_MCP_DST        "__mcp_dst"
#define TMP_MCP_SRC        "__mcp_src"
#define TMP_MCP_N          "__mcp_n"
#define TMP_MCP_I          "__mcp_i"

/* ── any boxing/unboxing temps ──────────────────────────────────────────────── */
#define TMP_ANY_HDR        "__any_hdr"
#define TMP_ANY_TMP32      "__any_tmp32"

/* ── math pow temps ────────────────────────────────────────────────────────── */
#define MATH_TMP_POW_BASE  "__pow_base"
#define MATH_TMP_POW_EXP   "__pow_exp"
#define MATH_TMP_POW_RES   "__pow_res"
#define MATH_TMP_POW_I     "__pow_i"
/* ── math trig temps ───────────────────────────────────────────────────────── */
#define MATH_TMP_TRIG_X    "__trig_x"
#define MATH_TMP_TRIG_X2   "__trig_x2"
#define MATH_TMP_TRIG_R    "__trig_r"
#define MATH_TMP_TRIG_T    "__trig_t"
#define MATH_TMP_TRIG_ORIG "__trig_orig"
#define MATH_TMP_TRIG_SIN  "__trig_sin"
/* ── math log temps ────────────────────────────────────────────────────────── */
#define MATH_TMP_LOG_U     "__log_u"
#define MATH_TMP_LOG_U2    "__log_u2"

/* ═════════════════════════════════════════════════════════════════════════════
 * String temp local names (STR_TMP_*)
 * ═════════════════════════════════════════════════════════════════════════════ */

/* ── string transform temps ─────────────────────────────────────────────────── */
#define STR_TMP_T_PTR   "__st_ptr"
#define STR_TMP_T_LEN   "__st_len"
#define STR_TMP_T_DST   "__st_dst"
#define STR_TMP_T_I     "__st_i"
#define STR_TMP_T_CH    "__st_ch"
/* ── string char_at temps ───────────────────────────────────────────────────── */
#define STR_TMP_SCA_PTR "__tmp_sca_ptr"
#define STR_TMP_SCA_IDX "__tmp_sca_idx"
#define STR_TMP_SCA_RES "__tmp_sca_res"
/* ── string join_array temps ────────────────────────────────────────────────── */
#define STR_TMP_JA_ARR   "__sja_arr"
#define STR_TMP_JA_DATA  "__sja_data"
#define STR_TMP_JA_COUNT "__sja_count"
#define STR_TMP_JA_ESZ   "__sja_esz"
#define STR_TMP_JA_I     "__sja_i"
#define STR_TMP_JA_STR   "__sja_str"
#define STR_TMP_JA_LEN   "__sja_len"
#define STR_TMP_JA_DST   "__sja_dst"
#define STR_TMP_JA_J     "__sja_j"
/* ── string substr temps ───────────────────────────────────────────────────── */
#define STR_TMP_SS_PTR    "__ss_ptr"
#define STR_TMP_SS_START  "__ss_start"
#define STR_TMP_SS_END    "__ss_end"
#define STR_TMP_SS_LEN    "__ss_len"
#define STR_TMP_SS_DST    "__ss_dst"
#define STR_TMP_SS_SUBLEN "__ss_sublen"
#define STR_TMP_SS_I      "__ss_i"
/* ── string contains temps ─────────────────────────────────────────────────── */
#define STR_TMP_SC_S       "__sc_s"
#define STR_TMP_SC_SUB     "__sc_sub"
#define STR_TMP_SC_LEN_S   "__sc_len_s"
#define STR_TMP_SC_LEN_SUB "__sc_len_sub"
#define STR_TMP_SC_I       "__sc_i"
#define STR_TMP_SC_J       "__sc_j"
#define STR_TMP_SC_FOUND   "__sc_found"
#define STR_TMP_SC_A       "__sca"
#define STR_TMP_SC_B       "__scb"
#define STR_TMP_SC_D       "__scd"
#define STR_TMP_SC_CI      "__sci"
#define STR_TMP_SC_LA      "__scla"
#define STR_TMP_SC_LB      "__sclb"
/* ── string eq/neq temps ───────────────────────────────────────────────────── */
#define STR_TMP_EQ_A   "__seq_a"
#define STR_TMP_EQ_B   "__seq_b"
#define STR_TMP_EQ_CA  "__seq_ca"
#define STR_TMP_EQ_CB  "__seq_cb"
#define STR_TMP_EQ_I   "__seq_i"
#define STR_TMP_EQ_RES "__seq_res"
/* ── string from_int temps ─────────────────────────────────────────────────── */
#define STR_TMP_FI_VAL "__sfi_val"
#define STR_TMP_FI_DST "__sfi_dst"
#define STR_TMP_FI_POS "__sfi_pos"
#define STR_TMP_FI_NEG "__sfi_neg"
#define STR_TMP_FI_DIG "__sfi_dig"
/* ── string from_bool temps ────────────────────────────────────────────────── */
#define STR_TMP_FB_DST "__sfb_dst"
/* ── string from_double temps ──────────────────────────────────────────────── */
#define STR_TMP_FD_FVAL "__sfd_fval"
#define STR_TMP_FD_DST  "__sfd_dst"
#define STR_TMP_FD_POS  "__sfd_pos"
#define STR_TMP_FD_NEG  "__sfd_neg"
#define STR_TMP_FD_IP   "__sfd_ip"
#define STR_TMP_FD_DIG  "__sfd_dig"
#define STR_TMP_FD_FRAC "__sfd_frac"
#define STR_TMP_FD_CI   "__sfd_ci"
#define STR_TMP_FD_DP   "__sfd_dp"
/* ── string has_prefix temps ───────────────────────────────────────────────── */
#define STR_TMP_HP_S   "__shp_s"
#define STR_TMP_HP_PRE "__shp_pre"
#define STR_TMP_HP_SC  "__shp_sc"
#define STR_TMP_HP_PC  "__shp_pc"
#define STR_TMP_HP_I   "__shp_i"
#define STR_TMP_HP_RES "__shp_res"
/* ── string has_suffix temps ───────────────────────────────────────────────── */
#define STR_TMP_HS_S      "__shs_s"
#define STR_TMP_HS_SUF    "__shs_suf"
#define STR_TMP_HS_SLEN   "__shs_slen"
#define STR_TMP_HS_SUFLEN "__shs_suflen"
#define STR_TMP_HS_I      "__shs_i"
#define STR_TMP_HS_RES    "__shs_res"
/* ── string len temps ──────────────────────────────────────────────────────── */
#define STR_TMP_L_PTR  "__sl_ptr"
#define STR_TMP_L_LEN  "__sl_len"
/* ── extra string temps (phase1 registration) ──────────────────────────────── */
#define STR_TMP_FB_VAL  "__sfb_val"
#define STR_TMP_SC_C    "__scc"
#define STR_TMP_SC_LC   "__sclc"
#define STR_TMP_EQ_R    "__seq_r"
/* ── codegen temps ─────────────────────────────────────────────────────────── */
#define CODEGEN_TMP_SWITCH_VAL "__switch_val"

#endif /* BUILTIN_NAMES_H */
