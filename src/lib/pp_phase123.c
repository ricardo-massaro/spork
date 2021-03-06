/* pp_phase123.c
 *
 * Translation phases 1, 2 and 3.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>

#include "preprocessor.h"
#include "input.h"
#include "pp_token.h"
#include "punct.h"

#define ERR_ERROR                    -1
#define ERR_OUT_OF_MEMORY            -2
#define ERR_UNTERMINATED_HEADER      -3
#define ERR_UNTERMINATED_STRING      -4
#define ERR_UNTERMINATED_CHAR_CONST  -5
#define ERR_UNTERMINATED_COMMENT     -6
#define ERR_INVALID_ESCAPE_SEQUENCE  -7

#define set_error sp_set_pp_error

#define IS_SPACE(c)      ((c) == ' ' || (c) == '\r' || (c) == '\n' || (c) == '\t')
#define IS_ALPHA(c)      (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z') || (c) == '_')
#define IS_DIGIT(c)      ((c) >= '0' && (c) <= '9')
#define IS_ALNUM(c)      (IS_ALPHA(c) || IS_DIGIT(c))
#define IS_OCT_DIGIT(c)  ((c) >= '0' && (c) <= '7')
#define IS_HEX_DIGIT(c)  (((c) >= '0' && (c) <= '9') || ((c) >= 'A' && (c) <= 'F') || ((c) >= 'a' && (c) <= 'f'))

#define CUR   ((in->pos   < in->size) ? (int)in->data[in->pos  ] : -1)
#define NEXT  ((in->pos+1 < in->size) ? (int)in->data[in->pos+1] : -1)
#define NEXT2 ((in->pos+2 < in->size) ? (int)in->data[in->pos+2] : -1)

#define ADVANCE()        (in->pos++)
#define CUR_POS          (in->pos)
#define SET_POS(p)       (in->pos = (p))
#define CUR_IN_POS(in)   ((in)->pos)
#define SET_IN_POS(in,p) ((in)->pos = (p))

static bool skip_bs_newline(struct sp_input *in)
{
  //printf("=== skip_bs_newline ===\n");
  bool skipped = false;
  while (CUR == '\\') {
    size_t rewind_pos = CUR_POS;
    ADVANCE();
    while (IS_SPACE(CUR) && CUR != '\n')
      ADVANCE();
    if (CUR == '\n') {
      skipped = true;
      ADVANCE();
      continue;
    }
    SET_POS(rewind_pos);
    break;
  }
  return skipped;
}

static bool skip_spaces(struct sp_input *in)
{
  bool skipped = false;
  
  //printf("=== skip_spaces ===\n");
  while (IS_SPACE(CUR) && CUR != '\n') {
    skipped = true;
    do
      ADVANCE();
    while (IS_SPACE(CUR) && CUR != '\n');
    skip_bs_newline(in);
  }
  return skipped;
}

static bool skip_comments(struct sp_input *in, int *err)
{
  bool skipped = false;
  
  while (CUR == '/') {
    if (NEXT == '\\') {
      size_t rewind_pos = CUR_POS;
      ADVANCE();
      if (! skip_bs_newline(in) || (CUR != '/' && CUR != '*')) {
        SET_POS(rewind_pos);
        return false;
      }
    } else if (NEXT == '/' || NEXT == '*') {
      ADVANCE();
    } else
      break;

    skipped = true;

    // multi-line
    if (CUR == '*') {
      ADVANCE();
      while (true) {
        if (CUR < 0) {
          *err = ERR_UNTERMINATED_COMMENT;
          return false;
        }
        if (CUR == '*') {
          ADVANCE();
          if (CUR == '\\')
            skip_bs_newline(in);
          if (CUR == '/') {
            ADVANCE();
            break;
          }
          if (CUR != '*')
            ADVANCE();
          continue;
        }
        ADVANCE();
      }
      while (skip_bs_newline(in) || skip_spaces(in))
        ;
      continue;
    }

    // single-line
    ADVANCE();
    while (true) {
      while (CUR >= 0 && CUR != '\n' && CUR != '\\')
        ADVANCE();
      if (CUR < 0 || CUR == '\n')
        break;
      if (CUR == '\\') {
        if (! skip_bs_newline(in))
          ADVANCE();
      }
    }

    while (skip_bs_newline(in) || skip_spaces(in))
      ;
  }
  return skipped;
}

static int read_header(struct sp_input *in, struct sp_buffer *buf)
{
  char end_char = (CUR == '<') ? '>' : '"';
  
  buf->size = 0;
  if (sp_buf_add_byte(buf, CUR) < 0)
    return ERR_OUT_OF_MEMORY;
  while (true) {
    ADVANCE();
    if (CUR == '\\')
      skip_bs_newline(in);
    if (CUR < 0 || CUR == '\n')
      return ERR_UNTERMINATED_HEADER;
    if (CUR == end_char)
      break;
    if (sp_buf_add_byte(buf, CUR) < 0)
      return ERR_OUT_OF_MEMORY;
  }
  if (sp_buf_add_byte(buf, CUR) < 0)
    return ERR_OUT_OF_MEMORY;
  ADVANCE();
  if (sp_buf_add_byte(buf, '\0') < 0)
    return ERR_OUT_OF_MEMORY;
  return TOK_PP_HEADER_NAME;
}

static int read_string(struct sp_input *in, struct sp_buffer *buf)
{
  buf->size = 0;
  if (sp_buf_add_byte(buf, CUR) < 0)
    return ERR_OUT_OF_MEMORY;
  if (CUR == 'L') {
    ADVANCE();
    if (CUR == '\\')
      skip_bs_newline(in);
    if (sp_buf_add_byte(buf, CUR) < 0)
      return ERR_OUT_OF_MEMORY;
  }
  while (true) {
    ADVANCE();
    if (CUR == '\\') {
      if (! skip_bs_newline(in)) {
        // actual backslash
        if (sp_buf_add_byte(buf, CUR) < 0)
          return ERR_OUT_OF_MEMORY;
        ADVANCE();
        if (CUR < 0)
          return ERR_UNTERMINATED_STRING;
        if (CUR == '\\')
          skip_bs_newline(in);
        
        if (sp_buf_add_byte(buf, CUR) < 0)
          return ERR_OUT_OF_MEMORY;
        ADVANCE();
        if (CUR == '\\')
          skip_bs_newline(in);
        if (CUR < 0)
          return ERR_UNTERMINATED_STRING;
      }
    }
    if (CUR == '\n' || CUR < 0)
      return ERR_UNTERMINATED_STRING;
    if (CUR == '"') {
      if (sp_buf_add_byte(buf, CUR) < 0)
        return ERR_OUT_OF_MEMORY;
      ADVANCE();
      break;
    }
    if (sp_buf_add_byte(buf, CUR) < 0)
      return ERR_OUT_OF_MEMORY;
  }
  if (sp_buf_add_byte(buf, '\0') < 0)
    return ERR_OUT_OF_MEMORY;
  return TOK_PP_STRING;
}

static int read_chars_in_set(struct sp_input *in, struct sp_buffer *buf, const char *set, int min, int max)
{
  int n = 0;
  while (true) {
    if (CUR == '\\' && ! skip_bs_newline(in))
      break;
    if (CUR < 0)
      break;
    //printf("testing '%c' against set '%s'\n", CUR, set);
    if (CUR != '\0' && strchr(set, CUR) != NULL) {
      if (sp_buf_add_byte(buf, CUR) < 0)
        return ERR_OUT_OF_MEMORY;
      ADVANCE();
      n++;
      if (max >= 0 && n >= max)
        break;
      continue;
    }
    //printf("test FAILED!\n");
    break;
  }

  if (n >= min && (max < 0 || n <= max))
    return 0;
  //printf("expected %d-%d chars in set '%s', found %d\n", min, max, set, n);
  return ERR_INVALID_ESCAPE_SEQUENCE;
}

static int read_char_const(struct sp_input *in, struct sp_buffer *buf)
{
  buf->size = 0;
  if (sp_buf_add_byte(buf, CUR) < 0)
    return ERR_OUT_OF_MEMORY;
  if (CUR == 'L') {
    ADVANCE();
    if (CUR == '\\')
      skip_bs_newline(in);
    if (sp_buf_add_byte(buf, CUR) < 0)
      return ERR_OUT_OF_MEMORY;
  }
  while (true) {
    ADVANCE();
    if (CUR == '\\') {
      if (! skip_bs_newline(in)) {
        // actual backspace
        if (sp_buf_add_byte(buf, CUR) < 0)
          return ERR_OUT_OF_MEMORY;
        ADVANCE();
        if (CUR < 0)
          return ERR_UNTERMINATED_STRING;
        if (CUR == '\\')
          skip_bs_newline(in);
        if (CUR != '\0' && strchr("'\"?\\abfnrtv", CUR) != NULL) {
          // simple
          if (sp_buf_add_byte(buf, CUR) < 0)
            return ERR_OUT_OF_MEMORY;
          ADVANCE();
        } else if (IS_OCT_DIGIT(CUR)) {
          // oct
          if (sp_buf_add_byte(buf, CUR) < 0)
            return ERR_OUT_OF_MEMORY;
          ADVANCE();
          int err = read_chars_in_set(in, buf, "01234567", 0, 2);
          if (err < 0)
            return err;
        } else if (CUR == 'x') {
          // hex
          if (sp_buf_add_byte(buf, CUR) < 0)
            return ERR_OUT_OF_MEMORY;
          ADVANCE();
          int err = read_chars_in_set(in, buf, "0123456789abcdefABCDEF", 1, -1);
          if (err < 0)
            return err;
        } else if (CUR == 'u') {
          // \uXXXX
          if (sp_buf_add_byte(buf, CUR) < 0)
            return ERR_OUT_OF_MEMORY;
          ADVANCE();
          int err = read_chars_in_set(in, buf, "0123456789abcdefABCDEF", 4, 4);
          if (err < 0)
            return err;
        } else if (CUR == 'U') {
          // \UXXXXXXXX
          if (sp_buf_add_byte(buf, CUR) < 0)
            return ERR_OUT_OF_MEMORY;
          ADVANCE();
          int err = read_chars_in_set(in, buf, "0123456789abcdefABCDEF", 8, 8);
          if (err < 0)
            return err;
        } else
          return ERR_INVALID_ESCAPE_SEQUENCE;
      }
    }
    if (CUR == '\n' || CUR < 0)
      return ERR_UNTERMINATED_STRING;
    if (CUR == '\'') {
      if (sp_buf_add_byte(buf, CUR) < 0)
        return ERR_OUT_OF_MEMORY;
      ADVANCE();
      break;
    }
    if (sp_buf_add_byte(buf, CUR) < 0)
      return ERR_OUT_OF_MEMORY;
  }
  if (sp_buf_add_byte(buf, '\0') < 0)
    return ERR_OUT_OF_MEMORY;
  return TOK_PP_CHAR_CONST;
}

static int read_number(struct sp_input *in, struct sp_buffer *buf)
{
  buf->size = 0;
  if (sp_buf_add_byte(buf, CUR) < 0)
    return ERR_OUT_OF_MEMORY;
  ADVANCE();
  while (true) {
    if (CUR == '\\' && ! skip_bs_newline(in))
      break;

    // [eEpP][+-]
    if (CUR == 'e' || CUR == 'E' || CUR == 'p' || CUR == 'P') {
      char exp = CUR;
      size_t rewind_pos = CUR_POS;
      ADVANCE();
      if (CUR == '\\' && ! skip_bs_newline(in)) {
        SET_POS(rewind_pos);
        break;
      }
      if (CUR == '-' || CUR == '+') {
        if (sp_buf_add_byte(buf, exp) < 0 || sp_buf_add_byte(buf, CUR) < 0)
          return ERR_OUT_OF_MEMORY;
        ADVANCE();
        continue;
      }
    }

    // [0-9A-Za-z_.]
    if (IS_DIGIT(CUR) || IS_ALPHA(CUR) || CUR == '.') {
      if (sp_buf_add_byte(buf, CUR) < 0)
        return ERR_OUT_OF_MEMORY;
      ADVANCE();
      continue;
    }
    
    break;
  }
  if (sp_buf_add_byte(buf, '\0') < 0)
    return ERR_OUT_OF_MEMORY;
  return TOK_PP_NUMBER;
}

static int read_ident(struct sp_input *in, struct sp_buffer *buf)
{
  buf->size = 0;
  if (sp_buf_add_byte(buf, CUR) < 0)
    return ERR_OUT_OF_MEMORY;
  ADVANCE();
  while (true) {
    if (CUR == '\\' && ! skip_bs_newline(in))
      break;
    if (! IS_ALNUM(CUR))
      break;
    if (sp_buf_add_byte(buf, CUR) < 0)
      return ERR_OUT_OF_MEMORY;
    ADVANCE();
  }
  if (sp_buf_add_byte(buf, '\0') < 0)
    return ERR_OUT_OF_MEMORY;
  return TOK_PP_IDENTIFIER;
}

static int read_token(struct sp_input *in, struct sp_buffer *buf, size_t *pos, bool parse_header)
{
  int err = 0;
  
  if (CUR < 0) {
    *pos = in->size;
    return TOK_PP_EOF;
  }

  skip_bs_newline(in);

  /* comment */
  if (skip_comments(in, &err))
    goto skip_spaces;
  if (err)
    return err;

  /* spaces/newlines */
  if (skip_spaces(in)) {
  skip_spaces:;
    bool got_newline = false;

    *pos = CUR_POS;
    while (skip_spaces(in) || skip_bs_newline(in))
      ;
    do {
      if (CUR == '\n') {
        *pos = CUR_POS;
        ADVANCE();
        got_newline = true;
      }
    } while (skip_spaces(in) || skip_bs_newline(in) || (got_newline && CUR == '\n'));
    if (got_newline) {
      if (skip_comments(in, &err))
        goto skip_newlines;
    } else {
      if (skip_comments(in, &err))
        goto skip_spaces;
    }
    if (err)
      return err;
    return (got_newline) ? '\n' : ' ';
  }

  /* newlines */
  if (CUR == '\n') {
  skip_newlines:
    do {
      if (CUR == '\n') {
        *pos = CUR_POS;
        ADVANCE();
      }
    } while (skip_spaces(in) || skip_bs_newline(in) || CUR == '\n');
    if (skip_comments(in, &err))
      goto skip_newlines;
    if (err)
      return err;
    return '\n';
  }

  /* <header> or "header" */
  if (parse_header && (CUR == '<' || CUR == '"')) {
    *pos = CUR_POS;
    return read_header(in, buf);
  }

  /* "string" */
  if (CUR == '"') {
    *pos = CUR_POS;
    return read_string(in, buf);
  }
  if (CUR == 'L') {
    int rewind_pos = CUR_POS;
    ADVANCE();
    if (CUR == '"' || (CUR == '\\' && skip_bs_newline(in) && CUR == '"')) {
      SET_POS(rewind_pos);
      *pos = CUR_POS;
      return read_string(in, buf);
    }
    SET_POS(rewind_pos);
  }

  /* number */
  if (IS_DIGIT(CUR)) {
    *pos = CUR_POS;
    return read_number(in, buf);
  }
  if (CUR == '.') {
    int rewind_pos = CUR_POS;
    ADVANCE();
    if (IS_DIGIT(CUR) || (CUR == '\\' && skip_bs_newline(in) && IS_DIGIT(CUR))) {
      SET_POS(rewind_pos);
      *pos = CUR_POS;
      return read_number(in, buf);
    }
    SET_POS(rewind_pos);
  }

  /* character constant */
  if (CUR == '\'') {
    *pos = CUR_POS;
    return read_char_const(in, buf);
  }
  if (CUR == 'L') {
    int rewind_pos = CUR_POS;
    ADVANCE();
    if (CUR == '\'' || (CUR == '\\' && skip_bs_newline(in) && CUR == '\'')) {
      SET_POS(rewind_pos);
      *pos = CUR_POS;
      return read_char_const(in, buf);
    }
    SET_POS(rewind_pos);
  }
  
  /* identifier */
  if (IS_ALPHA(CUR)) {
    *pos = CUR_POS;
    return read_ident(in, buf);
  }

  /* punctuation */
  buf->size = 0;
  // ensure we have enough space for any punctuation
  if (sp_buf_add_string(buf, "1234") < 0)
    return ERR_OUT_OF_MEMORY;
  int start_pos = CUR_POS;
  for (int try_size = 3; try_size > 0; try_size--) {
    buf->size = 0;
    for (int i = 0; i < try_size; i++) {
      sp_buf_add_byte(buf, CUR);
      ADVANCE();
      if (CUR == '\\')
        skip_bs_newline(in);
    }
    sp_buf_add_byte(buf, '\0');
    if (sp_get_punct_id(buf->p) >= 0) {
      *pos = start_pos;
      return TOK_PP_PUNCT;
    }
    SET_POS(start_pos);
  }
  
  int c = CUR;
  *pos = CUR_POS;
  ADVANCE();
  return c;
}

static bool next_char_is_lparen(struct sp_input *in)
{
  size_t rewind_pos = CUR_POS;
  if (CUR == '\\')
    skip_bs_newline(in);
  if (CUR == '(') {
    SET_POS(rewind_pos);
    return true;
  }
  SET_POS(rewind_pos);
  return false;
}

bool sp_next_pp_ph3_char_is_lparen(struct sp_preprocessor *pp)
{
  return next_char_is_lparen(pp->in);
}

static void get_input_location(struct sp_preprocessor *pp, size_t pos, struct sp_src_loc *loc)
{
  size_t start_pos;
  uint16_t cur_file_id = sp_get_input_file_id(pp->in);

  // start from last known position if possible
  if (pp->tok_loc.pos != (size_t) -1 && pp->tok_loc.loc.file_id == cur_file_id && pp->tok_loc.pos <= pos) {
    //printf("<cur %zu>", pos - pp->tok_loc.pos);
    start_pos = pp->tok_loc.pos;
    *loc = pp->tok_loc.loc;
  } else if (pp->last_tok_loc.pos != (size_t) -1 && pp->last_tok_loc.loc.file_id == cur_file_id && pp->last_tok_loc.pos <= pos) {
    //printf("<last %zu>", pos - pp->last_tok_loc.pos);
    start_pos = pp->last_tok_loc.pos;
    *loc = pp->last_tok_loc.loc;
  } else {
    //printf("\n\n*********** starting from beginning ***************\n\n");
    start_pos = 0;
    loc->file_id = cur_file_id;
    loc->line = 1;
    loc->col = 1;
  }

  // advance to requested position
  if (pos > pp->in->size)
    pos = pp->in->size;
  for (size_t p = start_pos; p < pos; p++) {
    if (pp->in->data[p] == '\n') {
      loc->line++;
      loc->col = 1;
    } else
      loc->col++;
  }

  // save current position as last known position
  pp->last_tok_loc = pp->tok_loc;
  pp->tok_loc.pos = pos;
  pp->tok_loc.loc = *loc;
}

static int next_token(struct sp_preprocessor *pp, struct sp_pp_token *tok, bool parse_header)
{
  size_t pos = 0;
  int type = read_token(pp->in, &pp->tmp_buf, &pos, parse_header);

  // error
  if (type < 0) {
    switch (type) {
    case ERR_ERROR:                    return set_error(pp, "internal error");
    case ERR_OUT_OF_MEMORY:            return set_error(pp, "out of memory");
    case ERR_UNTERMINATED_STRING:      return set_error(pp, "unterminated string");
    case ERR_UNTERMINATED_HEADER:      return set_error(pp, "unterminated header name");
    case ERR_UNTERMINATED_CHAR_CONST:  return set_error(pp, "unterminated character constant");
    case ERR_UNTERMINATED_COMMENT:     return set_error(pp, "unterminated comment");
    case ERR_INVALID_ESCAPE_SEQUENCE:  return set_error(pp, "invalid escape sequence");
    }
    return set_error(pp, "internal error");
  }

  get_input_location(pp, pos, &tok->loc);
  tok->macro_dead = false;
  tok->paste_dead = false;

  // EOF
  if (type == TOK_PP_EOF) {
    tok->type = TOK_PP_EOF;
    return 0;
  }
  
  // space
  if (type == ' ') {
    tok->type = TOK_PP_SPACE;
    return 0;
  }

  // newline
  if (type == '\n') {
    tok->type = TOK_PP_NEWLINE;
    return 0;
  }

  // other
  if (type < 256) {
    tok->type = TOK_PP_OTHER;
    tok->data.other = type;
    return 0;
  }

  // punct
  if (type == TOK_PP_PUNCT) {
    tok->type = TOK_PP_PUNCT;
    tok->data.punct_id = sp_get_punct_id(pp->tmp_buf.p);
    return 0;
  }

  // other tokens
  sp_string_id str_id = sp_add_string(&pp->token_strings, pp->tmp_buf.p);
  if (str_id < 0)
    return set_error(pp, "out of memory");
  tok->type = type;
  tok->data.str_id = str_id;
  return 0;
}

static bool skip_hex_quad(const char **pstr)
{
  const char *str = *pstr;
  if (IS_HEX_DIGIT(str[0]) && IS_HEX_DIGIT(str[1]) && IS_HEX_DIGIT(str[2]) && IS_HEX_DIGIT(str[3])) {
    (*pstr) += 4;
    return true;
  }
  return false;
}

static bool skip_escape_sequence(const char **pstr)
{
  const char *str = *pstr;
  if (*str++ != '\\')
    return false;
  
  if (*str == '\'' || *str == '"' || *str == '?' || *str == '\\' || *str == 'a' || *str == 'b'
      || *str == 'f' || *str == 'n' || *str == 'r' || *str == 't' || *str == 'v') {
    str++;
  } else if (IS_OCT_DIGIT(*str)) {
    int n = 0;
    do {
      n++;
      str++;
    } while (IS_OCT_DIGIT(*str) && n < 3);
  } else if (*str == 'x') {
    str++;
    if (! IS_HEX_DIGIT(*str))
      return false;
    do {
      str++;
    } while (IS_HEX_DIGIT(*str));
  } else if (*str == 'u') {
    str++;
    if (! skip_hex_quad(&str))
      return false;
  } else if (*str == 'U') {
    str++;
    if (! skip_hex_quad(&str) || ! skip_hex_quad(&str))
      return false;
  } else {
    return false;
  }
  
  *pstr = str;
  return true;
}

static bool check_pp_char_const(const char *str)
{
  if (*str == 'L')
    str++;
  if (*str != '\'')
    return false;
  str++;
  while (*str != '\'') {
    if (*str == '\n')
      return false;
    if (*str == '\\') {
      if (! skip_escape_sequence(&str))
        return false;
    } else {
      str++;
    }
  }
  str++;
  return (*str == '\0');
}

static bool check_string(const char *str)
{
  if (*str == 'L')
    str++;
  if (*str != '"')
    return false;
  str++;
  while (*str != '"') {
    if (*str == '\n')
      return false;
    if (*str == '\\') {
      if (! skip_escape_sequence(&str))
        return false;
    } else {
      str++;
    }
  }
  str++;
  return (*str == '\0');
}

static bool check_pp_number(const char *str)
{
  while (*str != '\0') {
    if (*str == 'e' || *str == 'E' || *str == 'p' || *str == 'P') {
      if (str[1] == '-')
        str++;
      str++;
    } else if (*str == '.' || IS_DIGIT(*str) || IS_ALPHA(*str)) {
      str++;
    } else if (*str == '\\' && str[1] == 'u') {
      str += 2;
      if (! skip_hex_quad(&str))
        return false;
    } else if (*str == '\\' && str[1] == 'U') {
      str += 2;
      if (! skip_hex_quad(&str) || ! skip_hex_quad(&str))
        return false;
    } else {
      return false;
    }
  }
  return true;
}

static bool check_identifier(const char *str)
{
  if (IS_DIGIT(*str))
    return false;

  while (*str != '\0') {
    if (IS_ALNUM(*str)) {
      str++;
    } else if (*str == '\\' && str[1] == 'u') {
      str += 2;
      if (! skip_hex_quad(&str))
        return false;
    } else if (*str == '\\' && str[1] == 'U') {
      str += 2;
      if (! skip_hex_quad(&str))
        return false;
    } else {
      return false;
    }
  }
  return true;
}

int sp_string_to_pp_token(struct sp_preprocessor *pp, const char *str, struct sp_pp_token *ret)
{
  int punct_id = sp_get_punct_id(str);
  if (punct_id >= 0) {
    ret->type = TOK_PP_PUNCT;
    ret->data.punct_id = punct_id;
    return 0;
  }

  if (IS_DIGIT(str[0]) || (str[0] == '.' && IS_DIGIT(str[1]))) {
    if (! check_pp_number(str))
      goto err;
    ret->type = TOK_PP_NUMBER;
    ret->data.str_id = sp_add_string(&pp->token_strings, str);
    if (ret->data.str_id < 0)
      return set_error(pp, "out of memory");
    return 0;
  }

  if (str[0] == '\'' || (str[0] == 'L' && str[1] == '\'')) {
    if (! check_pp_char_const(str))
      goto err;
    ret->type = TOK_PP_CHAR_CONST;
    ret->data.str_id = sp_add_string(&pp->token_strings, str);
    if (ret->data.str_id < 0)
      return set_error(pp, "out of memory");
    return 0;
  }
  
  if (str[0] == '"' || (str[0] == 'L' && str[1] == '"')) {
    if (! check_string(str))
      goto err;
    ret->type = TOK_PP_STRING;
    ret->data.str_id = sp_add_string(&pp->token_strings, str);
    if (ret->data.str_id < 0)
      return set_error(pp, "out of memory");
    return 0;
  }

  if (IS_ALPHA(str[0])) {
    if (! check_identifier(str))
      goto err;
    ret->type = TOK_PP_IDENTIFIER;
    ret->data.str_id = sp_add_string(&pp->token_strings, str);
    if (ret->data.str_id < 0)
      return set_error(pp, "out of memory");
    return 0;
  }

  if (strlen(str) == 1) {
    ret->type = TOK_PP_OTHER;
    ret->data.other = str[0];
    return 0;
  }

 err:
  set_error(pp, "pasting doesn't produce valid token: '%s'", str);
  return -1;
}

int sp_next_pp_ph3_token(struct sp_preprocessor *pp, bool parse_header)
{
  return next_token(pp, &pp->tok, parse_header);
}

int sp_peek_nonblank_pp_ph3_token(struct sp_preprocessor *pp, struct sp_pp_token *next, bool parse_header)
{
  int rewind_pos = CUR_IN_POS(pp->in);
  do {
    if (next_token(pp, next, parse_header) < 0)
      goto err;
  } while (next->type == TOK_PP_SPACE || next->type == TOK_PP_NEWLINE);
  SET_IN_POS(pp->in, rewind_pos);
  return 0;
  
 err:
  SET_IN_POS(pp->in, rewind_pos);
  return -1;
}
