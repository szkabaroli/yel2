// yel's external scanner: a C template's text (c"..."), up to its end, an escape, or a { that
// starts an interpolation: one before a name or a "(" (any other { is C's, part of the text). Its
// line breaks are text too: a template spans lines, as yelc's lexer reads one

#include "tree_sitter/parser.h"

#include <stdbool.h>

// ERROR_SENTINEL: never in the grammar, so valid only while the parser recovers from an error,
// where nothing is a template's text
enum TokenType { C_STRING_CONTENT, ERROR_SENTINEL };

void *tree_sitter_yel_external_scanner_create(void) { return NULL; }

void tree_sitter_yel_external_scanner_destroy(void *payload) { (void)payload; }

unsigned tree_sitter_yel_external_scanner_serialize(void *payload, char *buffer) {
  (void)payload;
  (void)buffer;
  return 0;
}

void tree_sitter_yel_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  (void)payload;
  (void)buffer;
  (void)length;
}

static bool starts_name(int32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '(';
}

bool tree_sitter_yel_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  (void)payload;
  if (valid_symbols[ERROR_SENTINEL] || !valid_symbols[C_STRING_CONTENT]) return false;
  bool any = false;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == 0 || c == '"' || c == '\\') break;
    if (c == '{') {
      // a { before a name or ( is an interpolation's: the text ends before it
      lexer->mark_end(lexer);
      lexer->advance(lexer, false);
      if (starts_name(lexer->lookahead)) {
        if (!any) return false;
        lexer->result_symbol = C_STRING_CONTENT;
        return true;
      }
      any = true;
      continue;
    }
    lexer->advance(lexer, false);
    any = true;
  }
  if (!any) return false;
  lexer->mark_end(lexer);
  lexer->result_symbol = C_STRING_CONTENT;
  return true;
}
