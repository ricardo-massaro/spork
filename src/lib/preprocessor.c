/* preprocessor.c */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>

#include "preprocessor.h"
#include "hashtable.h"
#include "input.h"
#include "ast.h"

#define IS_SPACE(c) ((c) == ' ' || (c) == '\r' || (c) == '\n' || (c) == '\t')
#define IS_ALPHA(c) (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z') || (c) == '_')
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define IS_ALNUM(c) (IS_ALPHA(c) || IS_DIGIT(c))

enum pp_directive_type {
  PP_DIR_if,
  PP_DIR_ifdef,
  PP_DIR_ifndef,
  PP_DIR_elif,
  PP_DIR_else,
  PP_DIR_endif,
  PP_DIR_include,
  PP_DIR_define,
  PP_DIR_undef,
  PP_DIR_line,
  PP_DIR_error,
  PP_DIR_pragma,
};

#define ADD_PP_DIR(dir) { PP_DIR_ ## dir, # dir }
static const struct pp_directive {
  enum pp_directive_type type;
  const char *name;
} pp_directives[] = {
  ADD_PP_DIR(if),
  ADD_PP_DIR(ifdef),
  ADD_PP_DIR(ifndef),
  ADD_PP_DIR(elif),
  ADD_PP_DIR(else),
  ADD_PP_DIR(endif),
  ADD_PP_DIR(include),
  ADD_PP_DIR(define),
  ADD_PP_DIR(undef),
  ADD_PP_DIR(line),
  ADD_PP_DIR(error),
  ADD_PP_DIR(pragma),
};

void sp_init_preprocessor(struct sp_preprocessor *pp, struct sp_program *prog, struct sp_mem_pool *pool)
{
  pp->prog = prog;
  pp->pool = pool;
  pp->in = NULL;
  pp->ast = NULL;
  pp->expanding_macro = NULL;
  pp->next_expanded_token = NULL;
  pp->expanding_args = NULL;
  pp->loc = sp_make_src_loc(0,0,0);
  pp->at_newline = false;
  sp_init_ht(&pp->macros, pool);
  sp_init_buffer(&pp->tmp_buf, pool);
  sp_init_mem_pool(&pp->macro_exp_pool);
}

void sp_destroy_preprocessor(struct sp_preprocessor *pp)
{
  sp_destroy_mem_pool(&pp->macro_exp_pool);
}

void sp_set_preprocessor_io(struct sp_preprocessor *pp, struct sp_input *in, struct sp_ast *ast)
{
  pp->in = in;
  pp->ast = ast;
  pp->loc = sp_make_src_loc(sp_get_input_file_id(in), 1, 0);
  pp->at_newline = true;
}

static int set_error_at(struct sp_preprocessor *pp, struct sp_src_loc loc, char *fmt, ...)
{
  char str[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(str, sizeof(str), fmt, ap);
  va_end(ap);

  sp_set_error(pp->prog, "%s:%d:%d: %s", sp_get_ast_file_name(pp->ast, loc.file_id), loc.line, loc.col, str);
  return -1;
}

static int set_error(struct sp_preprocessor *pp, char *fmt, ...)
{
  char str[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(str, sizeof(str), fmt, ap);
  va_end(ap);

  sp_set_error(pp->prog, "%s:%d:%d: %s", sp_get_ast_file_name(pp->ast, pp->tok.loc.file_id), pp->tok.loc.line, pp->tok.loc.col, str);
  return -1;
}

void sp_dump_macros(struct sp_preprocessor *pp)
{
  const char *macro = NULL;
  size_t macro_len;
  while (sp_next_ht_key(&pp->macros, (const void **) &macro, &macro_len)) {
    printf("#define %s", macro);
    struct sp_macro_def *def = sp_get_ht_value(&pp->macros, macro, strlen(macro)+1);
    if (def->is_function) {
      printf("(");
      for (struct sp_token *t = def->params; t != NULL; t = t->next) {
        printf("%s", sp_dump_token(pp->ast, t));
        if (t->next)
          printf(", ");
      }
      printf(") ");
    }
    for (struct sp_token *t = def->body; t != NULL; t = t->next)
      printf(" %s", sp_dump_token(pp->ast, t));
    printf("\n");
  }
}

/*
 * ==========================================================
 * Phases 1-2
 * ==========================================================
 */

#define read_byte(pp)      sp_input_next_byte((pp)->in)
#define unread_byte(pp, b) sp_input_unget_byte((pp)->in, b)

#define GET_BYTE(c, pp) do { c = get_byte(pp); if (c < 0) goto eof; } while (0)

static int get_byte(struct sp_preprocessor *pp)
{
  int c = sp_input_next_byte(pp->in);
  if (c < 0)
    return -1;
  if (c == '\n') {
    pp->loc.line++;
    pp->loc.col = 1;
  } else
    pp->loc.col++;
  return c;
}

static int next_byte(struct sp_preprocessor *pp, int *ret)
{
  int c;
  
  // skip spaces, comments and \\ at the end of the line
  while (true) {
    GET_BYTE(c, pp);
    if (c == '\n')
      break;
    if (IS_SPACE(c))
      continue;

    // backslash
    if (c == '\\') {
      do {
        GET_BYTE(c, pp);
      } while (c != '\n' && IS_SPACE(c));
      if (c == '\n')
        continue;
      if (c >= 0)
        unread_byte(pp, c);
      c = '\\';  // put '\' back
      break;
    }

    // comment
    if (c == '/') {
      int next;
      GET_BYTE(next, pp);
      if (next == '*') {
        while (true) {
          do {
            GET_BYTE(next, pp);
          } while (next != '*');
          GET_BYTE(next, pp);
          if (next == '/')
            break;
        }
        continue;
      } else if (next == '/') {
        while (true) {
          GET_BYTE(next, pp);
          //printf("out"); sp_dump_char(next);
          if (next == '\n')
            break;
          if (next == '\\') {
            do {
              GET_BYTE(next, pp);
              //printf("in "); sp_dump_char(next);
            } while (next == '\\' || (next != '\n' && IS_SPACE(next)));
          }
        }
        c = next;
        break;
      } else {
        if (next >= 0)
          unread_byte(pp, next);
      }
    }
    
    break;
  }

  //printf("[%c]\n", c);
  *ret = c;
  return 0;

 eof:
  *ret = -1;
  return 0;
}

/*
 * ==========================================================
 * Phase 3
 * ==========================================================
 */

static int next_token(struct sp_preprocessor *pp, bool parse_header)
{
  int c = 0;
  if (next_byte(pp, &c) < 0)
    return -1;
  if (c < 0) {
    pp->tok.type = TOK_EOF;
    pp->tok.loc = pp->loc;
    return 0;
  }
  
  pp->tok.loc = pp->loc;

  // newline
  if (c == '\n') {
    pp->tok.type = TOK_PP_NEWLINE;
    pp->tok.loc = pp->loc;
    return 0;
  }
  
  // header
  if (c == '<' && parse_header) {
    pp->tmp_buf.size = 0;
    while (1) {
      c = read_byte(pp);
      if (c < 0)
        return set_error(pp, "unterminated string");
      if (c == '>')
        break;
      if (sp_buf_add_byte(&pp->tmp_buf, c) < 0)
        return set_error(pp, "out of memory");
    }
    if (sp_buf_add_byte(&pp->tmp_buf, '\0') < 0)
      return set_error(pp, "out of memory");
    sp_string_id str_id = sp_add_string(&pp->ast->strings, pp->tmp_buf.p);
    if (str_id < 0)
      return set_error(pp, "out of memory");
    pp->tok.type = TOK_PP_HEADER_NAME;
    pp->tok.data.str_id = str_id;
    return 0;
  }
  
  // string
  if (c == '"') {
    pp->tmp_buf.size = 0;
    while (1) {
      c = read_byte(pp);
      if (c < 0)
        return set_error(pp, "unterminated string");
      if (c == '"')
        break;
      if (c == '\\') {
        int next = read_byte(pp);
        if (next < 0)
          return set_error(pp, "unterminated string");
        switch (next) {
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '\'': c = '\''; break;
        case 'e': c = '\x1b'; break;
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        default:
          return set_error_at(pp, pp->loc, "bad escape sequence");
        }
      }
      if (sp_buf_add_byte(&pp->tmp_buf, c) < 0)
        return set_error(pp, "out of memory");
    }

    if (sp_buf_add_byte(&pp->tmp_buf, '\0') < 0)
      return set_error(pp, "out of memory");
    sp_string_id str_id = sp_add_string(&pp->ast->strings, pp->tmp_buf.p);
    if (str_id < 0)
      return set_error(pp, "out of memory");
    pp->tok.type = TOK_STRING;
    pp->tok.data.str_id = str_id;
    return 0;
  }

  // TODO: char constant (TOK_PP_CHAR_CONST)
  
  // number
  if (IS_DIGIT(c)) {
    pp->tmp_buf.size = 0;
    int got_point = 0;
    while (IS_DIGIT(c) || c == '.') {
      if (c == '.') {
        if (got_point)
          break;
        got_point = 1;
      }
      if (sp_buf_add_byte(&pp->tmp_buf, c) < 0)
        return set_error(pp, "out of memory");
      c = read_byte(pp);
    }
    if (c >= 0)
      unread_byte(pp, c);

    if (sp_buf_add_byte(&pp->tmp_buf, '\0') < 0)
      return set_error(pp, "out of memory");
    
    char *end = NULL;
    double num = strtod(pp->tmp_buf.p, &end);
    if (pp->tmp_buf.p == end)
      return set_error(pp, "invalid number");
    pp->tok.type = TOK_PP_NUMBER;
    pp->tok.data.d = num;
    return 0;
  }

  // identifier
  if (IS_ALPHA(c)) {
    pp->tmp_buf.size = 0;
    while (IS_ALNUM(c)) {
      if (sp_buf_add_byte(&pp->tmp_buf, c) < 0)
        return set_error(pp, "out of memory");
      c = read_byte(pp);
    }
    if (c >= 0)
      unread_byte(pp, c);

    if (sp_buf_add_byte(&pp->tmp_buf, '\0') < 0)
      return set_error(pp, "out of memory");
    sp_string_id ident_id = sp_add_string(&pp->ast->strings, pp->tmp_buf.p);
    if (ident_id < 0)
      return set_error(pp, "out of memory");
    pp->tok.type = TOK_IDENTIFIER;
    pp->tok.data.str_id = ident_id;
    return 0;
  }

  // handle '...'
  if (c == '.') {
    int c2 = read_byte(pp);
    if (c2 == '.') {
      int c3 = read_byte(pp);
      if (c3 == '.') {
        pp->tok.type = TOK_PUNCT;
        pp->tok.data.punct_id = sp_get_punct_id("...");
        return 0;
      }
      if (c3 >= 0)
        unread_byte(pp, c3);
    }
    if (c2 >= 0)
      unread_byte(pp, c2);
  }

  // punctuation
  struct sp_src_loc start_loc = pp->loc;
  char punct_name[4] = { c };
  int punct_len = 1;
  while (1) {
    punct_name[punct_len] = '\0';
    if (sp_get_punct_id(punct_name) < 0) {
      punct_name[--punct_len] = '\0';
      unread_byte(pp, c);
      break;
    }

    if (punct_len == sizeof(punct_name)-1)
      break;
    c = read_byte(pp);
    if (c < 0)
      break;
    punct_name[punct_len++] = c;
  }
  if (punct_len > 0) {
    pp->tok.type = TOK_PUNCT;
    pp->tok.data.punct_id = sp_get_punct_id(punct_name);
    return 0;
  }

  if (c >= 32 && c < 128)
    set_error_at(pp, start_loc, "invalid character: '%c'", c);
  else
    set_error_at(pp, start_loc, "invalid byte: 0x%02x", c);
  return -1;
}

/*
 * ==========================================================
 * Phase 4
 * ==========================================================
 */

#define NEXT_TOKEN()   do { if (next_token(pp, false) < 0) return -1; } while (0)

#define IS_TOK_TYPE(t)         (pp->tok.type == t)
#define IS_EOF()               IS_TOK_TYPE(TOK_EOF)
#define IS_NEWLINE()           IS_TOK_TYPE(TOK_PP_NEWLINE)
#define IS_PP_HEADER_NAME()    IS_TOK_TYPE(TOK_PP_HEADER_NAME)
#define IS_STRING()            IS_TOK_TYPE(TOK_STRING)
#define IS_IDENTIFIER()        IS_TOK_TYPE(TOK_IDENTIFIER)
#define IS_PUNCT(id)           (IS_TOK_TYPE(TOK_PUNCT) && pp->tok.data.punct_id == (id))

static bool find_pp_directive(const char *string, enum pp_directive_type *ret)
{
  for (int i = 0; i < ARRAY_SIZE(pp_directives); i++) {
    if (strcmp(string, pp_directives[i].name) == 0) {
      *ret = pp_directives[i].type;
      return true;
    }
  }
  return false;
}

static int process_include(struct sp_preprocessor *pp)
{
  if (next_token(pp, true) < 0)
    return -1;
  
  if (! IS_STRING() && ! IS_PP_HEADER_NAME())   // TODO: allow macro expansion
    return set_error(pp, "bad include file name: '%s'", sp_dump_token(pp->ast, &pp->tok));
  
  const char *filename = sp_get_token_string(pp->ast, &pp->tok);
  NEXT_TOKEN();
  if (! IS_NEWLINE())
    return set_error(pp, "unexpected input after include file name: '%s'", sp_dump_token(pp->ast, &pp->tok));
  
  const char *base_filename = sp_get_ast_file_name(pp->ast, sp_get_input_file_id(pp->in));
  sp_string_id file_id = sp_add_ast_file_name(pp->ast, filename);
  if (file_id < 0)
    return set_error(pp, "out of memory");

  // TODO: search file in pre-defined directories
  struct sp_input *in = sp_open_input_file(pp->pool, filename, (uint16_t) file_id, base_filename);
  if (! in)
    return set_error(pp, "can't open '%s'", filename);
  in->next = pp->in;
  pp->in = in;

  return 0;
}

static int read_macro_body(struct sp_preprocessor *pp, struct sp_macro_def *macro_def)
{
  struct sp_token *list = NULL;
  struct sp_token **cur = &list;

  bool is_variadic = false;
  for (struct sp_token *t = macro_def->params; t != NULL; t = t->next) {
    if (t->type == TOK_PUNCT && t->data.punct_id == PUNCT_ELLIPSIS) {
      is_variadic = true;
      break;
    }
  }

  while (true) {
    NEXT_TOKEN();
    if (IS_NEWLINE())
      break;
    if ((! is_variadic) && IS_IDENTIFIER()) {
      const char *ident = sp_get_token_string(pp->ast, &pp->tok);
      if (strcmp(ident, "__VA_ARGS__") == 0)
        return set_error(pp, "__VA_ARGS__ is only allowed in variadic macros");
    }
    *cur = sp_malloc(pp->pool, sizeof(struct sp_token));
    if (! *cur)
      return set_error(pp, "out of memory");
    **cur = pp->tok;
    (*cur)->next = NULL;
    cur = &(*cur)->next;
  }

  // check if all # and ## are valid
  if (list && tok_is_punct(list, PUNCT_HASHES))
    return set_error(pp, "## is not allowed at the start of macro body");
  for (struct sp_token *t = list; t != NULL; t = t->next) {
    if (tok_is_punct(t, PUNCT_HASHES)) {
      if (! t->next)
        return set_error(pp, "## is not allowed at the end of macro body");
    }
  
    if (tok_is_punct(t, '#')) {
      bool next_is_argument = false;
      if (t->next && tok_is_identifier(t->next)) {
        for (struct sp_token *param = macro_def->params; param != NULL; param = param->next) {
          if (tok_is_identifier(param) && t->next->data.str_id == param->data.str_id) {
            next_is_argument = true;
            break;
          }
        }
      }
      if (! next_is_argument)
        return set_error(pp, "# must be followed by a parameter name");
    }
  }

  macro_def->body = list;
  return 0;
}

static int read_macro_params(struct sp_preprocessor *pp, struct sp_macro_def *macro_def)
{
  struct sp_token *list = NULL;
  struct sp_token **cur = &list;

  NEXT_TOKEN();
  if (! IS_PUNCT('('))
    return set_error(pp, "expected '('");

  while (true) {
    NEXT_TOKEN();
    if (IS_EOF() || IS_NEWLINE())
      return set_error(pp, "unterminated macro parameter list");
    if (IS_PUNCT(')') && list == NULL)
      break;
    if (! IS_IDENTIFIER() && ! IS_PUNCT(PUNCT_ELLIPSIS))
      return set_error(pp, "invalid macro parameter: '%s'", sp_dump_token(pp->ast, &pp->tok));
    if (IS_IDENTIFIER()) {
      // check if there's a parameter with the same name
      for (struct sp_token *t = list; t != NULL; t = t->next) {
        if (tok_is_identifier(t) && t->data.str_id == pp->tok.data.str_id)
          return set_error(pp, "duplicate parameter name: '%s'", sp_dump_token(pp->ast, t));
      }
    }
    
    *cur = sp_malloc(pp->pool, sizeof(struct sp_token));
    if (! *cur) {
      set_error(pp, "out of memory");
      return -1;
    }
    **cur = pp->tok;
    (*cur)->next = NULL;
    cur = &(*cur)->next;

    bool was_ellipsis = IS_PUNCT(PUNCT_ELLIPSIS);
    NEXT_TOKEN();
    if (IS_EOF() || IS_NEWLINE())
      return set_error(pp, "unterminated macro parameter list");
    if (IS_PUNCT(')'))
      break;
    if (was_ellipsis)
      return set_error(pp, "expected ')'");
    if (IS_PUNCT(','))
      continue;
    return set_error(pp, "expected ',' or ')'");
  }

  macro_def->params = list;
  return 0;
}

static int process_undef(struct sp_preprocessor *pp)
{
  NEXT_TOKEN();
  if (IS_NEWLINE())
    return set_error(pp, "macro name required");
  if (! IS_IDENTIFIER())
    return set_error(pp, "macro name must be an identifier, found '%s'", sp_dump_token(pp->ast, &pp->tok));
  
  const char *macro_name = sp_get_token_string(pp->ast, &pp->tok);
  sp_delete_ht_entry(&pp->macros, macro_name, strlen(macro_name)+1);
  return 0;
}

static int process_define(struct sp_preprocessor *pp)
{
  NEXT_TOKEN();
  if (IS_NEWLINE())
    return set_error(pp, "macro name required");
  if (! IS_IDENTIFIER())
    return set_error(pp, "macro name must be an identifier, found '%s'", sp_dump_token(pp->ast, &pp->tok));
  
  const char *macro_name = sp_get_token_string(pp->ast, &pp->tok);
  struct sp_macro_def *macro_def = sp_malloc(pp->pool, sizeof(struct sp_macro_def));
  if (! macro_def)
    return set_error(pp, "out of memory");
  macro_def->name_id = pp->tok.data.str_id;
  
  int c = read_byte(pp);
  macro_def->is_function = (c == '(');
  if (c >= 0)
    unread_byte(pp, c);

  if (! macro_def->is_function)
    macro_def->params = NULL;
  else {
    if (read_macro_params(pp, macro_def) < 0)
      return -1;
  }
  if (read_macro_body(pp, macro_def) < 0)
    return -1;
  if (sp_add_ht_entry(&pp->macros, macro_name, strlen(macro_name)+1, macro_def) < 0)
    return set_error(pp, "out of memory");
  return 0;
}

static int process_pp_directive(struct sp_preprocessor *pp)
{
  NEXT_TOKEN();
  if (IS_NEWLINE())
    return 0;
  
  if (! IS_IDENTIFIER())
    return set_error(pp, "invalid input after '#': '%s'", sp_dump_token(pp->ast, &pp->tok));
  
  const char *directive_name = sp_get_token_string(pp->ast, &pp->tok);
  enum pp_directive_type directive;
  if (! find_pp_directive(directive_name, &directive))
    return set_error(pp, "invalid preprocessor directive: '#%s'", directive_name);
  
  switch (directive) {
  case PP_DIR_include: return process_include(pp);
  case PP_DIR_define:  return process_define(pp);
  case PP_DIR_undef:   return process_undef(pp);

  case PP_DIR_if:
  case PP_DIR_ifdef:
  case PP_DIR_ifndef:
  case PP_DIR_elif:
  case PP_DIR_else:
  case PP_DIR_endif:
  case PP_DIR_line:
  case PP_DIR_error:
  case PP_DIR_pragma:
    return set_error(pp, "preprocessor directive is not implemented: '#%s'", directive_name);
  }
  return set_error(pp, "invalid preprocessor directive: '#%s'", directive_name);
}

static int expand_macro(struct sp_preprocessor *pp)
{
  if (! pp->next_expanded_token) {
    pp->expanding_macro = NULL;
    pp->expanding_args = NULL;
    return 1;
  }
  if (pp->expanding_macro->is_function)
    return set_error(pp, "function-macro expansion not implemented!");
  pp->tok = *pp->next_expanded_token;
  pp->next_expanded_token = pp->tok.next;
  return 0;
}

static int setup_macro_expansion(struct sp_preprocessor *pp, struct sp_macro_def *macro)
{
  if (! macro->is_function) {
    pp->expanding_macro = macro;
    pp->next_expanded_token = macro->body;
    return expand_macro(pp);
  }

  sp_clear_mem_pool(&pp->macro_exp_pool);
  //pp->expanding_args = ...read macro args using pp->macro_exp_pool...;
  pp->expanding_macro = macro;
  pp->next_expanded_token = macro->body;
  return set_error(pp, "function-macro expansion not implemented!");
}

static int next_expanded_token(struct sp_preprocessor *pp)
{
  if (pp->expanding_macro) {
    int exp = expand_macro(pp);
    if (exp <= 0)
      return exp;
  }

  while (true) {
    NEXT_TOKEN();
    if (IS_NEWLINE()) {
      pp->at_newline = true;
      while (IS_NEWLINE())
        if (next_token(pp, false) < 0)
          return -1;
    }
    
    if (IS_EOF()) {
      if (! pp->in->next)
        return 0;
      struct sp_input *next = pp->in->next;
      sp_close_input(pp->in);
      pp->in = next;
      pp->at_newline = true;
      continue;
    }
  
    if (IS_PUNCT('#')) {
      if (! pp->at_newline)
        return set_error(pp, "invalid '#'");
      if (process_pp_directive(pp) < 0)
        return -1;
      pp->at_newline = true;
      continue;
    }

    pp->at_newline = false;

    if (tok_is_identifier(&pp->tok)) {
      const char *macro_name = sp_get_token_string(pp->ast, &pp->tok);
      struct sp_macro_def *macro = sp_get_ht_value(&pp->macros, macro_name, strlen(macro_name)+1);
      if (macro) {
        bool expand_macro = true;
        if (macro->is_function) {
          int c = 0;
          if (next_byte(pp, &c) < 0)
            return -1;
          if (c != '(')
            expand_macro = false;
          if (c >= 0)
            unread_byte(pp, c);
        }

        if (expand_macro) {
          int exp = setup_macro_expansion(pp, macro);
          if (exp <= 0)
            return exp;
          continue;
        }
      }
    }
    
    return 0;
  }
}

int sp_read_token(struct sp_preprocessor *pp, struct sp_token *tok)
{
  if (next_expanded_token(pp) < 0)
    return -1;
  *tok = pp->tok;
  return 0;
}
