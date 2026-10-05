(function_declaration
  name: (identifier) @name
  ":" @context
  "func" @context) @item

(record_declaration
  ["record" "resource"] @context
  name: (identifier) @name) @item

(field_declaration
  name: (identifier) @name) @item

(variant_declaration
  "variant" @context
  name: (identifier) @name) @item

(enum_declaration
  "enum" @context
  name: (identifier) @name) @item

(flags_declaration
  "flags" @context
  name: (identifier) @name) @item

(module_declaration
  "module" @context
  name: (identifier) @name) @item

(const_declaration
  "const" @context
  name: (identifier) @name) @item

(global_declaration
  "let" @context
  name: (identifier) @name) @item

(type_alias
  "type" @context
  name: (identifier) @name) @item

(view_declaration
  "view" @context
  name: (identifier) @name) @item
