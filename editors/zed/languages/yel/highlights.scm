; yel2's highlights. A later pattern wins over an earlier one: the general first.

(identifier) @variable

; a name written in capitals (LIMIT, S64-MAX) is a constant's
((identifier) @constant
  (#match? @constant "^[A-Z][A-Z0-9_-]*$"))

(comment) @comment

; ---- keywords

[
  "on"
  "key"
  "style"
  "func"
  "async"
  "extern"
  "let"
  "const"
  "type"
  "distinct"
  "module"
  "view"
  "widget"
  "package"
  "from"
  "include"
  "record"
  "resource"
  "variant"
  "enum"
  "flags"
  "readonly"
] @keyword

[
  "if"
  "else"
  "match"
  "for"
  "in"
  "loop"
  "return"
  "break"
  "continue"
  "wait"
  "start"
  "yield"
] @keyword

[
  "as"
  "as?"
  "is"
] @keyword

; ---- literals

(boolean) @boolean
(directive) @constant.builtin
(regex) @string.regex
(number) @number
(char) @string.special

[
  (string)
  (c_string)
  (multiline_string)
  (attribute_string)
] @string

(escape_sequence) @string.escape
(format_spec) @string.special

(interpolation
  [
    "{"
    "}"
  ] @punctuation.special)

; ---- types

(primitive_type) @type.builtin
(never_type) @type.builtin
(type_identifier) @type

(scoped_type
  module: (identifier) @namespace)

(record_declaration name: (identifier) @type)
(variant_declaration name: (identifier) @type)
(enum_declaration name: (identifier) @type)
(flags_declaration name: (identifier) @type)
(type_alias name: (identifier) @type)
(view_declaration name: (identifier) @type)

(variant_case name: (identifier) @constructor)
(enum_case name: (identifier) @constructor)

; ---- declarations and names

(function_declaration name: (identifier) @function)
(const_declaration name: (identifier) @constant)
(module_declaration name: (identifier) @namespace)
(include_name name: (identifier) @namespace)
(include_name alias: (identifier) @namespace)
(package_name) @namespace

(parameter name: (identifier) @variable.parameter)
(closure_parameter name: (identifier) @variable.parameter)

(field_declaration name: (identifier) @property)
(field_type name: (identifier) @property)
(field_initializer name: (identifier) @property)
(field_pattern name: (identifier) @property)
(member_expression member: (identifier) @property)

(call_expression function: (identifier) @function)
(call_expression
  function: (member_expression member: (identifier) @function.method))

(case_pattern name: (identifier) @constructor)
(case_pattern name: (member_expression member: (identifier) @constructor))
(wildcard_pattern) @variable.special

(attribute "@" @attribute)
(attribute name: (identifier) @attribute)
(attribute_argument name: (identifier) @attribute)

; ---- punctuation

[
  "="
  "+="
  "-="
  "*="
  "/="
  "%="
  "=="
  "!="
  "<"
  "<="
  ">"
  ">="
  "+"
  "-"
  "*"
  "/"
  "%"
  "!"
  "&&"
  "||"
  "&"
  "|"
  "^"
  "<<"
  ">>"
  ".."
  "..="
  "..."
  "->"
  "=>"
] @operator

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  ","
  ";"
  ":"
  "."
] @punctuation.delimiter

; a type's angle brackets (list<T>, func<T>(...)) are brackets, not comparisons
(generic_type
  [
    "<"
    ">"
  ] @punctuation.bracket)

(type_parameters
  [
    "<"
    ">"
  ] @punctuation.bracket)

; ---- views: element tags, fields, attributes, handlers (on.event: { … }, a callback's on.name)

(view_tag) @tag
(view_field name: (identifier) @property)
(view_field derived: "let" name: (identifier) @variable)
(view_attribute name: (identifier) @property)
(view_attribute namespace: "on" name: (identifier) @function)
(view_field callback: "on" name: (identifier) @function)
(widget_declaration name: (identifier) @type)
(widget_declaration base: (identifier) @type)
(view_for pattern: (identifier) @variable)
