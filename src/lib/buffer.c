/* buffer.c */

#include <limits.h>
#include <string.h>

#include "buffer.h"

void sp_init_buffer(struct sp_buffer *buf, struct sp_mem_pool *pool)
{
  buf->pool = pool;
  buf->p = NULL;
  buf->size = 0;
  buf->cap = 0;
}

void sp_destroy_buffer(struct sp_buffer *buf)
{
  if (buf->p != NULL)
    sp_free(buf->pool, buf->p);
  buf->p = NULL;
  buf->size = 0;
  buf->cap = 0;
}

int sp_buf_grow(struct sp_buffer *buf, size_t add_size)
{
  size_t new_size = buf->size + add_size;
  if (new_size > INT_MAX || new_size < (size_t)buf->size)
    return -1;
  if ((int)new_size > buf->cap) {
    size_t new_cap = ((new_size + 1024 - 1) / 1024) * 1024;
    if (new_cap > INT_MAX || new_cap < new_size)
      return -1;
    void *new_p = sp_realloc(buf->pool, buf->p, new_cap);
    if (new_p == NULL)
      return -1;
    buf->p = new_p;
    buf->cap = (int) new_cap;
  }
  
  buf->size = (int) new_size;
  return 0;
}

int sp_buf_shrink_to_fit(struct sp_buffer *buf)
{
  if (buf->size == 0) {
    if (buf->p) {
      sp_free(buf->pool, buf->p);
      buf->p = NULL;
      buf->cap = 0;
    }
    return 0;
  }
  
  void *new_p = sp_realloc(buf->pool, buf->p, buf->size);
  if (new_p == NULL)
    return -1;
  buf->p = new_p;
  return 0;
}

int sp_buf_add_string(struct sp_buffer *buf, const char *str)
{
  return sp_buf_add_data(buf, str, strlen(str));
}

int sp_buf_add_data(struct sp_buffer *buf, const void *data, size_t size)
{
  int pos = buf->size;
  if (sp_buf_grow(buf, size) < 0)
    return -1;
  memcpy(buf->p + pos, data, size);
  return pos;
}

int sp_buf_add_byte(struct sp_buffer *buf, uint8_t c)
{
  int pos = buf->size;
  if (sp_buf_grow(buf, 1) < 0)
    return -1;
  buf->p[pos++] = c;
  return pos;
}

int sp_buf_add_u16(struct sp_buffer *buf, uint16_t c)
{
  int pos = buf->size;
  if (sp_buf_grow(buf, 2) < 0)
    return -1;
  buf->p[pos++] = c & 0xff;
  buf->p[pos++] = (c >> 8) & 0xff;
  return pos;
}
