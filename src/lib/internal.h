/* internal.h */

#ifndef INTERNAL_H_FILE
#define INTERNAL_H_FILE

#include "spork.h"
#include "mem_pool.h"
#include "string_tab.h"

#define ARRAY_SIZE(a)  ((int)(sizeof(a)/sizeof((a)[0])))
#define UNUSED(v)      ((void)(v))

struct sp_src_loc {
  uint16_t line;
  uint16_t col;
  uint16_t file_id;
};

#define sp_make_src_loc(f, l, c) ((struct sp_src_loc) { .file_id = (f), .line = (l), .col = (c) })

uint32_t sp_hash(const void *data, size_t len);
int sp_utf8_len(char *str, size_t size);
void sp_dump_string(const char *str);
void sp_dump_char(char c);
void dump_mem(const void *data, size_t len);

#endif /* INTERNAL_H_FILE */
