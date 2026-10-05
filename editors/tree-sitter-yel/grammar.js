/**
 * @file yel2's grammar for Tree-sitter: what Zed highlights, outlines and indents by. It follows
 * compiler/parser.yel: declarations in yel's member syntax (name: func(...) -> T { ... }), types,
 * statements, expressions by yel's precedence, patterns, and strings with {expr} interpolation (a
 * C template's only before a name or a "("). A view's body is kept as a tree of tokens.
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// yel's binary operators, loosest first (compiler/parser.yel's levels; `is` is a comparison's)
const PREC = {
  or: 1,
  and: 2,
  compare: 3,
  bit_or: 4,
  bit_xor: 5,
  bit_and: 6,
  shift: 7,
  add: 8,
  multiply: 9,
  cast: 10,
  unary: 11,
  postfix: 12,
};

// words yel takes as keywords only where they start what they name (let start = ..., module: func,
// a case named yield): elsewhere they are names. Any keyword is a name written %name (%in)
// every keyword, a member's or a case's name after . or before ( (Kind.if(c), if(x) => ...)
const KEYWORDS = ["if", "else", "match", "for", "in", "return", "break", "continue", "let", "as", "is", "true", "false"];

const CONTEXTUAL = [
  "start", "wait", "yield", "module", "view", "package", "from", "include", "extern", "async",
  "loop", "distinct", "const", "type", "readonly", "never", "resource", "record", "variant",
  "enum", "flags", "on", "key", "style",
];

/** items separated by commas, a comma after the last allowed */
function commaSep(rule) {
  return optional(commaSep1(rule));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(",", rule)), optional(","));
}

module.exports = grammar({
  name: "yel",

  extras: ($) => [/\s/, $.comment],

  // a C template's text: src/scanner.c (its { an interpolation's only before a name or a "(")
  externals: ($) => [$.c_string_content, $._error_sentinel],

  word: ($) => $.identifier,

  conflicts: ($) => [
    [$.closure_parameter, $.field_shorthand],
    [$._pattern, $._expression],
    [$.record_pattern, $.record_expression],
    [$.scoped_type, $._expression],
    [$.list_pattern, $.list_expression],
    [$._block_statement, $._expression],
    [$._type, $.union_type],
    [$._type_member, $._literal],
    [$.unit_type, $.unit_expression],
    [$.anonymous_record_type, $.block],
    [$.negative_number, $._literal],
    [$.field_type, $.closure_parameter],
    [$._name, $.yield_expression],
    [$._type_member, $._pattern],
    [$._name, $.start_expression],
    [$._name, $.wait_expression],
    [$._expression, $.start_expression],
    [$.never_type, $._type_name],
    [$._name, $._type_name],
    [$._name, $.never_type, $._type_name],
    [$._name, $._type_name, $.yield_expression],
    [$._type_name, $.function_type],
    [$._expression, $.field_shorthand],
  ],

  rules: {
    source_file: ($) => repeat($._declaration),

    _declaration: ($) =>
      choice(
        $.package_declaration,
        $.include_declaration,
        $.function_declaration,
        $.record_declaration,
        $.variant_declaration,
        $.enum_declaration,
        $.flags_declaration,
        $.module_declaration,
        $.const_declaration,
        $.global_declaration,
        $.type_alias,
        $.view_declaration,
      ),

    comment: (_) => token(seq("//", /[^\n]*/)),

    // a name; %name, a keyword's word as a name (as WIT writes one: %in, %record)
    identifier: (_) => /%?[A-Za-z_][A-Za-z0-9_]*(-[A-Za-z0-9_]+)*/,

    // a name: an identifier, or a contextual keyword where it names something
    _name: ($) => choice($.identifier, alias(choice(...CONTEXTUAL), $.identifier)),

    // ---- declarations

    // @(private), @(private="file"), @c(name = "...", gives = "s"), @primitive
    attribute: ($) =>
      seq(
        "@",
        choice(
          seq(field("name", $._name), optional($.attribute_arguments)),
          $.attribute_arguments,
        ),
      ),

    attribute_arguments: ($) => seq("(", commaSep($.attribute_argument), ")"),

    attribute_argument: ($) =>
      seq(field("name", $._name), optional(seq("=", field("value", choice($.attribute_string, $.number, $.boolean, $._name))))),

    // an attribute's string: C, as written (its braces no interpolation's)
    attribute_string: (_) => token(seq('"', repeat(choice(/[^"\\]/, /\\./)), '"')),

    package_declaration: ($) => seq("package", field("name", $.package_name), ";"),

    // ns:name, ns:name@1.2.3
    package_name: (_) => token(/[a-z][a-z0-9-]*:[a-z][a-z0-9-]*(@[0-9][0-9A-Za-z.+-]*)?/),

    include_declaration: ($) =>
      seq("from", field("source", $.string), "include", commaSep1($.include_name), ";"),

    include_name: ($) =>
      seq(field("name", $._name), optional(seq("as", field("alias", $._name)))),

    function_declaration: ($) =>
      seq(
        repeat($.attribute),
        field("name", $._name),
        ":",
        repeat(choice("async", "extern")),
        "func",
        optional(field("type_parameters", $.type_parameters)),
        field("parameters", $.parameters),
        optional(seq("->", field("result", $._type))),
        choice(field("body", $.block), ";"),
      ),

    type_parameters: ($) => seq("<", commaSep1($.identifier), ">"),

    parameters: ($) => seq("(", commaSep($.parameter), ")"),

    // name: T, or name: T = default (a literal, or a directive: #caller-location)
    parameter: ($) =>
      seq(field("name", $._name), ":", field("type", $._type), optional(seq("=", field("default", $._expression)))),

    record_declaration: ($) =>
      seq(
        repeat($.attribute),
        choice("record", "resource"),
        field("name", $.identifier),
        field("body", $.field_list),
      ),

    field_list: ($) => seq("{", commaSep($.field_declaration), "}"),

    field_declaration: ($) =>
      seq(repeat($.attribute), field("name", $._name), ":", field("type", $._type)),

    variant_declaration: ($) =>
      seq(
        repeat($.attribute),
        "variant",
        field("name", $.identifier),
        "{",
        commaSep($.variant_case),
        "}",
      ),

    variant_case: ($) =>
      seq(field("name", $._name), optional(seq("(", field("payload", $._type), ")"))),

    enum_declaration: ($) =>
      seq(repeat($.attribute), "enum", field("name", $.identifier), "{", commaSep($.enum_case), "}"),

    flags_declaration: ($) =>
      seq(repeat($.attribute), "flags", field("name", $.identifier), "{", commaSep($.enum_case), "}"),

    enum_case: ($) => field("name", $._name),

    module_declaration: ($) =>
      seq(
        repeat($.attribute),
        "module",
        field("name", $.identifier),
        field("body", $.declaration_list),
      ),

    declaration_list: ($) => seq("{", repeat($._declaration), "}"),

    const_declaration: ($) =>
      seq(
        repeat($.attribute),
        "const",
        field("name", $.identifier),
        optional(seq(":", field("type", $._type))),
        "=",
        field("value", $._expression),
        ";",
      ),

    global_declaration: ($) =>
      seq(
        repeat($.attribute),
        "let",
        field("name", $.identifier),
        ":",
        field("type", $._type),
        optional(seq("=", field("value", $._expression))),
        ";",
      ),

    type_alias: ($) =>
      seq(
        repeat($.attribute),
        "type",
        field("name", $.identifier),
        "=",
        optional("distinct"),
        field("type", $._type),
        ";",
      ),

    // view Counter { fields; its element }: yel's UI views (compiler/parser.yel's view-decl)
    view_declaration: ($) =>
      seq(repeat($.attribute), "view", field("name", $._name), field("body", $.view_body)),

    view_body: ($) => seq("{", repeat(choice($.view_field, $.view_element)), "}"),

    // @(private) name: T = value; (a field: a prop, its value to start with)
    view_field: ($) =>
      seq(
        repeat($.attribute),
        field("name", $._name),
        ":",
        field("type", $._type),
        "=",
        field("value", $._expression),
        ";",
      ),

    // Tag { items }
    view_element: ($) => seq(field("tag", $._view_tag), "{", repeat($._view_item), "}"),

    // an element's tag (VStack, Text: a name, as a leaf of its own)
    _view_tag: ($) =>
      choice(alias($.identifier, $.view_tag), alias(choice(...CONTEXTUAL), $.view_tag)),

    // text, an element, an attribute, a handler, if, for (a comma after one allowed)
    _view_item: ($) =>
      seq(
        choice(
          $.string,
          $.multiline_string,
          $.view_element,
          $.view_attribute,
          $.view_handler,
          $.view_if,
          $.view_for,
        ),
        optional(","),
      ),

    // name: value, style.name: value
    view_attribute: ($) =>
      seq(optional(seq("style", ".")), field("name", $._name), ":", field("value", $._expression)),

    // on event { statements }
    view_handler: ($) => seq("on", field("event", $._name), field("body", $.block)),

    // if cond { items } else { items }, else if ...
    view_if: ($) =>
      seq(
        "if",
        field("condition", $._expression),
        "{",
        repeat($._view_item),
        "}",
        optional(seq("else", choice($.view_if, seq("{", repeat($._view_item), "}")))),
      ),

    // for item in items key(expr) { items }: a row for each item, kept by its key
    view_for: ($) =>
      seq(
        "for",
        field("pattern", $._name),
        "in",
        field("over", $._expression),
        "key",
        "(",
        field("key", $._expression),
        ")",
        "{",
        repeat($._view_item),
        "}",
      ),

    // ---- types

    _type: ($) => choice($.union_type, $._type_member),

    union_type: ($) => prec.left(seq($._type_member, repeat1(seq("|", $._type_member)))),

    _type_member: ($) =>
      choice(
        $.primitive_type,
        $._type_name,
        $.scoped_type,
        $.generic_type,
        $.function_type,
        $.unit_type,
        $.anonymous_record_type,
        $.readonly_type,
        $.range_type,
        $.string,
        $.number,
        $.negative_number,
        $.never_type,
      ),

    primitive_type: (_) =>
      choice(
        "bool",
        "s8",
        "s16",
        "s32",
        "s64",
        "u8",
        "u16",
        "u32",
        "u64",
        "f32",
        "f64",
        "char",
        "string",
      ),

    never_type: (_) => "never",

    // a type's name (an identifier, as a type)
    _type_name: ($) =>
      choice(
        alias($.identifier, $.type_identifier),
        alias(choice(...CONTEXTUAL), $.type_identifier),
      ),

    // types.descriptor, json.value
    scoped_type: ($) =>
      seq(field("module", $._name), ".", field("name", $._type_name)),

    // list<T>, list<T, 3>, map<K, V>, result<_, string>, option<T>, stream<u8>
    generic_type: ($) =>
      prec(
        1,
        seq(
          field("name", choice($._type_name, $.scoped_type)),
          "<",
          commaSep1(choice($._type, "_", "?")),
          ">",
        ),
      ),

    function_type: ($) =>
      prec.right(
        seq(
          optional("async"),
          "func",
          "(",
          commaSep($._type),
          ")",
          optional(seq("->", field("result", $._type_member))),
        ),
      ),

    unit_type: (_) => seq("(", ")"),

    anonymous_record_type: ($) => seq("{", commaSep($.field_type), "}"),

    field_type: ($) => seq(field("name", $._name), ":", field("type", $._type)),

    readonly_type: ($) => prec(2, seq($._type_member, "&", "readonly")),

    range_type: ($) =>
      prec(1, seq(choice($.number, $.char, $.negative_number), choice("..", "..="), choice($.number, $.char, $.negative_number))),

    negative_number: ($) => seq("-", $.number),

    // ---- statements

    block: ($) => seq("{", repeat($._statement), optional(field("value", $._expression)), "}"),

    _statement: ($) =>
      choice(
        $.let_statement,
        $.assignment_statement,
        $.return_statement,
        $.break_statement,
        $.continue_statement,
        $.for_statement,
        $.wait_for_statement,
        $.loop_statement,
        $.expression_statement,
        $._block_statement,
      ),

    // an if, a match or a block that starts a statement is the statement (no ; after it)
    _block_statement: ($) => choice($.if_expression, $.match_expression, $.block),

    let_statement: ($) =>
      seq(
        "let",
        field("pattern", $._pattern),
        optional(seq(":", field("type", $._type))),
        optional(seq("=", field("value", $._expression))),
        ";",
      ),

    assignment_statement: ($) =>
      seq(
        field("target", $._expression),
        field("operator", choice("=", "+=", "-=", "*=", "/=", "%=")),
        field("value", $._expression),
        ";",
      ),

    return_statement: ($) => seq("return", optional($._expression), ";"),

    break_statement: (_) => seq("break", ";"),

    continue_statement: (_) => seq("continue", ";"),

    for_statement: ($) =>
      seq("for", field("pattern", $._pattern), "in", field("over", $._iterable), field("body", $.block)),

    wait_for_statement: ($) =>
      seq(
        "wait",
        "for",
        field("pattern", $._pattern),
        "in",
        field("over", $._expression),
        field("body", $.block),
      ),

    _iterable: ($) => choice($.range_expression, $._expression),

    range_expression: ($) =>
      prec.left(seq(field("start", $._expression), choice("..", "..="), field("end", $._expression))),

    loop_statement: ($) => seq("loop", field("body", $.block)),

    expression_statement: ($) => seq($._expression, ";"),

    // ---- expressions

    _expression: ($) =>
      choice(
        $._name,
        $._literal,
        $.directive,
        $.binary_expression,
        $.unary_expression,
        $.cast_expression,
        $.is_expression,
        $.call_expression,
        $.member_expression,
        $.index_expression,
        $.parenthesized_expression,
        $.tuple_expression,
        $.unit_expression,
        $.list_expression,
        $.record_expression,
        $.closure_expression,
        $.block,
        $.if_expression,
        $.match_expression,
        $.wait_expression,
        $.start_expression,
        $.yield_expression,
      ),

    _literal: ($) =>
      choice($.number, $.string, $.c_string, $.multiline_string, $.char, $.boolean, $.regex),

    // /pattern/flags: a regex literal (where a value starts: else a / divides), as
    // tree-sitter-javascript reads one
    regex: ($) =>
      seq(
        "/",
        field("pattern", $.regex_pattern),
        token.immediate(prec(1, "/")),
        optional(field("flags", $.regex_flags)),
      ),

    regex_pattern: (_) =>
      token.immediate(
        prec(
          -1,
          repeat1(choice(seq("[", repeat(choice(seq("\\", /./), /[^\]\n\\]/)), "]"), seq("\\", /./), /[^/\\\[\n]/)),
        ),
      ),

    regex_flags: (_) => token.immediate(/[a-z]+/),

    boolean: (_) => choice("true", "false"),

    // #file, #line, #caller-location: what the compiler knows (a call of one, #caller-expression(x),
    // is a call_expression)
    directive: (_) => token(/#[a-zA-Z][a-zA-Z0-9_-]*/),

    number: (_) =>
      token(
        choice(
          /0x[0-9a-fA-F_]+/,
          /0o[0-7_]+/,
          /0b[01_]+/,
          /[0-9][0-9_]*(\.[0-9][0-9_]*)?([eE][+-]?[0-9]+)?/,
        ),
      ),

    char: (_) => token(seq("'", choice(/[^'\\\n]/, /\\[^\n]/, /\\u\{[0-9a-fA-F]+\}/), "'")),

    escape_sequence: (_) => token.immediate(choice(/\\[^u\n]/, /\\u\{[0-9a-fA-F]+\}/)),

    string: ($) =>
      seq(
        '"',
        repeat(choice($.string_content, $.escape_sequence, $.interpolation)),
        token.immediate('"'),
      ),

    string_content: (_) => token.immediate(prec(1, /[^"\\{\n]+/)),

    // c"...": a { starts an interpolation only before a name or a (
    c_string: ($) =>
      seq(
        'c"',
        repeat(choice($.c_string_content, $.escape_sequence, $.interpolation)),
        token.immediate('"'),
      ),

    multiline_string: ($) =>
      seq(
        '"""',
        repeat(choice($.multiline_string_content, $._multiline_quotes, $.escape_sequence, $.interpolation)),
        token.immediate('"""'),
      ),

    multiline_string_content: (_) => token.immediate(prec(1, /[^"\\{]+/)),

    // one or two quotes in a multiline string's text (three end it: below their precedence)
    _multiline_quotes: ($) => alias(token.immediate(prec(-1, /"{1,2}/)), $.multiline_string_content),

    // {expr} in a string, a format after a colon ({x:>8}, {n:08x})
    interpolation: ($) =>
      seq(
        token.immediate("{"),
        field("value", $._expression),
        optional(seq(":", field("format", alias(/[^}\n]*/, $.format_spec)))),
        "}",
      ),

    binary_expression: ($) => {
      const table = [
        [PREC.or, "||"],
        [PREC.and, "&&"],
        [PREC.compare, choice("==", "!=", "<", "<=", ">", ">=")],
        [PREC.bit_or, "|"],
        [PREC.bit_xor, "^"],
        [PREC.bit_and, "&"],
        [PREC.shift, choice("<<", ">>")],
        [PREC.add, choice("+", "-")],
        [PREC.multiply, choice("*", "/", "%")],
      ];
      return choice(
        ...table.map(([precedence, operator]) =>
          prec.left(
            /** @type {number} */ (precedence),
            seq(
              field("left", $._expression),
              field("operator", /** @type {RuleOrLiteral} */ (operator)),
              field("right", $._expression),
            ),
          ),
        ),
      );
    },

    unary_expression: ($) =>
      prec(PREC.unary, seq(field("operator", choice("-", "!")), field("value", $._expression))),

    // x as T, x as? T
    cast_expression: ($) =>
      prec.left(
        PREC.cast,
        seq(field("value", $._expression), choice("as", "as?"), field("type", $._type_member)),
      ),

    is_expression: ($) =>
      prec.left(PREC.compare, seq(field("value", $._expression), "is", field("type", $._type))),

    call_expression: ($) =>
      prec(PREC.postfix, seq(field("function", $._expression), field("arguments", $.arguments))),

    arguments: ($) => seq(token.immediate("("), commaSep($._expression), ")"),

    // x.name, t.0
    member_expression: ($) =>
      prec(
        PREC.postfix,
        seq(
          field("object", $._expression),
          ".",
          field("member", choice($._name, $.number, alias(choice(...KEYWORDS), $.identifier))),
        ),
      ),

    index_expression: ($) =>
      prec(PREC.postfix, seq(field("object", $._expression), token.immediate("["), field("index", $._expression), "]")),

    parenthesized_expression: ($) => seq("(", $._expression, ")"),

    // (a, b), (x,)
    tuple_expression: ($) =>
      seq("(", $._expression, ",", optional(seq($._expression, repeat(seq(",", $._expression)), optional(","))), ")"),

    unit_expression: (_) => seq("(", ")"),

    list_expression: ($) => seq("[", commaSep($._expression), "]"),

    // { x: 1, y }, as a record's literal (or an anonymous record's)
    record_expression: ($) =>
      seq("{", commaSep1(choice($.field_initializer, $.field_shorthand)), "}"),

    field_initializer: ($) => seq(field("name", $._name), ":", field("value", $._expression)),

    field_shorthand: ($) => field("name", $._name),

    // { params -> body }, { -> body }
    closure_expression: ($) =>
      seq(
        "{",
        optional(commaSep1($.closure_parameter)),
        "->",
        repeat($._statement),
        optional(field("value", $._expression)),
        "}",
      ),

    closure_parameter: ($) =>
      seq(field("name", $._name), optional(seq(":", field("type", $._type)))),

    if_expression: ($) =>
      prec.right(
        seq(
          "if",
          field("condition", $._expression),
          field("consequence", $.block),
          optional(seq("else", field("alternative", choice($.if_expression, $.block)))),
        ),
      ),

    match_expression: ($) =>
      seq("match", field("value", $._expression), "{", repeat($.match_arm), "}"),

    match_arm: ($) =>
      prec.right(
        seq(
          field("pattern", $._match_pattern),
          optional(seq("if", field("guard", $._expression))),
          "=>",
          field("value", $._expression),
          optional(","),
        ),
      ),

    // wait f(x), wait fut, wait a | b, wait { a: x, b: y }
    wait_expression: ($) => prec.dynamic(1, prec.right(seq("wait", field("value", $._expression)))),

    // start f(x): a call begun (start before anything else is a name's: if start { ... })
    start_expression: ($) => seq("start", field("value", $.call_expression)),

    yield_expression: ($) => prec.right(seq("yield", optional(field("value", $._expression)))),

    // ---- patterns

    _match_pattern: ($) => choice($._pattern, $.or_pattern, $.typed_pattern),

    or_pattern: ($) => prec.left(seq($._pattern, repeat1(seq("|", $._pattern)))),

    // let n: s32 => (a race's arm: the value of that type)
    typed_pattern: ($) => seq("let", field("name", $._pattern), ":", field("type", $._type)),

    _pattern: ($) =>
      choice(
        $._name,
        // (let func = ...: func a binding's name)
        alias("func", $.identifier),
        $.binding_pattern,
        $.wildcard_pattern,
        $._literal,
        $.negative_number,
        $.case_pattern,
        $.tuple_pattern,
        $.record_pattern,
        $.list_pattern,
        $.range_pattern,
        $.unit_expression,
      ),

    wildcard_pattern: (_) => "_",

    // let x, let { x, y }: the names a match arm binds
    binding_pattern: ($) => prec(1, seq("let", field("pattern", $._pattern))),

    // some(x), circle(r), Shape.circle(r), Shape.dot, types.error-code.io (a bare name is an
    // identifier: a case or a constant in a match, a binding in a let)
    case_pattern: ($) =>
      prec(
        1,
        choice(
          seq(
            field("name", choice($._name, $.member_expression, alias(choice(...KEYWORDS), $.identifier))),
            token.immediate("("),
            commaSep($._match_pattern),
            ")",
          ),
          field("name", $.member_expression),
        ),
      ),

    tuple_pattern: ($) => seq("(", $._match_pattern, ",", commaSep(choice($._match_pattern, $.rest_pattern)), ")"),

    record_pattern: ($) => seq("{", commaSep1(choice($.field_pattern, $.field_shorthand, $.rest_pattern)), "}"),

    // ...rest: the fields (or items) not named
    rest_pattern: ($) => seq("...", optional(field("name", $._name))),

    field_pattern: ($) => seq(field("name", $._name), ":", field("pattern", $._match_pattern)),

    list_pattern: ($) => seq("[", commaSep(choice($._match_pattern, $.rest_pattern)), "]"),

    range_pattern: ($) =>
      seq(
        choice($.number, $.char, $.negative_number),
        choice("..", "..="),
        choice($.number, $.char, $.negative_number),
      ),
  },
});
