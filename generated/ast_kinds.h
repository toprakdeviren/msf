// ast_kinds.h — @generated from data/ast_nodes.def
// DO NOT EDIT — regenerate: make codegen
// 103 AST node kinds

  // ── Top-level ───────────────────────────────────────────────────
  AST_UNKNOWN = 0,
  AST_SOURCE_FILE,

  // ── Declarations ────────────────────────────────────────────────
  AST_FUNC_DECL,
  AST_VAR_DECL,
  AST_LET_DECL,  // var_decl
  AST_CLASS_DECL,
  AST_STRUCT_DECL,
  AST_ENUM_DECL,
  AST_PROTOCOL_DECL,
  AST_EXTENSION_DECL,
  AST_IMPORT_DECL,
  AST_TYPEALIAS_DECL,
  AST_INIT_DECL,  // constructor_decl
  AST_DEINIT_DECL,  // destructor_decl
  AST_ACTOR_DECL,
  AST_PARAM,  // parameter
  AST_ATTRIBUTE,

  // ── Top-level ───────────────────────────────────────────────────
  AST_OWNERSHIP_SPEC,
  AST_THROWS_CLAUSE,

  // ── Declarations ────────────────────────────────────────────────
  AST_ACCESSOR_DECL,
  AST_GENERIC_PARAM,
  AST_WHERE_CLAUSE,
  AST_PROTOCOL_REQ,  // protocol_requirement
  AST_CONFORMANCE,
  AST_ENUM_CASE_DECL,
  AST_ENUM_ELEMENT_DECL,
  AST_SUBSCRIPT_DECL,
  AST_PRECEDENCE_GROUP_DECL,
  AST_OPERATOR_DECL,

  // ── Top-level ───────────────────────────────────────────────────
  AST_PG_HIGHER_THAN,
  AST_PG_LOWER_THAN,
  AST_PG_ASSOCIATIVITY,
  AST_PG_ASSIGNMENT,
  AST_PG_GROUP_REF,

  // ── Statements ──────────────────────────────────────────────────
  AST_RETURN_STMT,
  AST_THROW_STMT,
  AST_IF_STMT,
  AST_GUARD_STMT,
  AST_FOR_STMT,  // for_each_stmt
  AST_WHILE_STMT,
  AST_REPEAT_STMT,  // repeat_while_stmt
  AST_DO_STMT,  // do_catch_stmt
  AST_SWITCH_STMT,
  AST_CASE_CLAUSE,  // case_stmt
  AST_BREAK_STMT,
  AST_CONTINUE_STMT,
  AST_FALLTHROUGH_STMT,
  AST_DEFER_STMT,
  AST_DISCARD_STMT,
  AST_EXPR_STMT,
  AST_BLOCK,  // brace_stmt

  // ── Expressions ─────────────────────────────────────────────────
  AST_IDENT_EXPR,  // unresolved_decl_ref_expr
  AST_INTEGER_LITERAL,  // integer_literal_expr
  AST_FLOAT_LITERAL,  // float_literal_expr
  AST_STRING_LITERAL,  // string_literal_expr
  AST_REGEX_LITERAL,  // regex_literal_expr
  AST_BOOL_LITERAL,  // boolean_literal_expr
  AST_NIL_LITERAL,  // nil_literal_expr
  AST_CALL_EXPR,
  AST_MEMBER_EXPR,  // unresolved_dot_expr
  AST_SUBSCRIPT_EXPR,
  AST_BINARY_EXPR,
  AST_UNARY_EXPR,  // prefix_unary_expr
  AST_CONSUME_EXPR,
  AST_ASSIGN_EXPR,
  AST_PAREN_EXPR,
  AST_ARRAY_LITERAL,  // array_expr
  AST_DICT_LITERAL,  // dictionary_expr
  AST_TUPLE_EXPR,
  AST_CLOSURE_EXPR,
  AST_CLOSURE_CAPTURE,
  AST_TRY_EXPR,
  AST_AWAIT_EXPR,
  AST_CAST_EXPR,
  AST_OPTIONAL_CHAIN,  // optional_chain_expr
  AST_FORCE_UNWRAP,  // force_value_expr
  AST_TERNARY_EXPR,
  AST_INOUT_EXPR,
  AST_IF_EXPR,
  AST_CATCH_CLAUSE,
  AST_KEY_PATH_EXPR,
  AST_MACRO_EXPANSION,  // macro_expansion_expr
  AST_AVAILABILITY_EXPR,

  // ── Types ───────────────────────────────────────────────────────
  AST_TYPE_IDENT,
  AST_TYPE_OPTIONAL,
  AST_TYPE_ARRAY,
  AST_TYPE_DICT,
  AST_TYPE_TUPLE,
  AST_TYPE_FUNC,  // type_function
  AST_TYPE_GENERIC,
  AST_TYPE_INOUT,
  AST_TYPE_SOME,
  AST_TYPE_ANY,
  AST_TYPE_COMPOSITION,

  // ── Patterns ────────────────────────────────────────────────────
  AST_PATTERN_ENUM,
  AST_PATTERN_TUPLE,
  AST_PATTERN_TYPE,
  AST_PATTERN_GUARD,
  AST_PATTERN_RANGE,
  AST_PATTERN_WILDCARD,  // pattern_any
  AST_PATTERN_VALUE_BINDING,  // pattern_binding
  AST_OPTIONAL_BINDING,  // optional_binding_cond

  // ── Declarations ────────────────────────────────────────────────
  AST_MACRO_DECL,

  AST__COUNT
