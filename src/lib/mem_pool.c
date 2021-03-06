/* mem_pool.c */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "mem_pool.h"

#define INITIAL_PAGE_SIZE (16*1024)

union align {
  double d;
  int i;
  uint32_t u32;
  void *p;
};

#define ALIGNMENT_SIZE   (sizeof(union align))
#define ALIGN(p)         (((size_t)(p) + ALIGNMENT_SIZE-1) & ~(ALIGNMENT_SIZE-1))

struct sp_mem_page {
  struct sp_mem_page *next;
  char *free;
  size_t free_size;
};

void sp_init_mem_pool(struct sp_mem_pool *p)
{
  p->page_list = NULL;
  p->page_size = INITIAL_PAGE_SIZE/2;
}

void sp_destroy_mem_pool(struct sp_mem_pool *p)
{
  struct sp_mem_page *page = p->page_list;
  while (page != NULL) {
    struct sp_mem_page *next = page->next;
    free(page);
    page = next;
  }
}

void sp_clear_mem_pool(struct sp_mem_pool *p)
{
  if (! p->page_list)
    return;

#if 1
  struct sp_mem_page *page = p->page_list->next;
  while (page != NULL) {
    struct sp_mem_page *next = page->next;
    free(page);
    page = next;
  }
  p->page_list->next = NULL;
  p->page_list->free = (char *)(p->page_list) + ALIGN(sizeof(struct sp_mem_page));
  p->page_list->free_size = p->page_size - ALIGN(sizeof(struct sp_mem_page));
#else
  sp_destroy_mem_pool(p);
  sp_init_mem_pool(p);
#endif
}

void *sp_malloc(struct sp_mem_pool *p, size_t size)
{
  if (! p)
    return malloc(size);

  if (size & (ALIGNMENT_SIZE-1))
    size = ALIGN(size);

  struct sp_mem_page *page;
 again:
  page = p->page_list;
  //printf("using page %p, page->free=%p\n", (void *) page, (page) ? (void *)page->free : NULL);
  if (page && page->free_size >= size) {
    void *ret = page->free;
    page->free += size;
    page->free_size -= size;
    //printf("-> sp_malloc(): allocated %zu bytes at %p (page->free=%p)\n", size, ret, (void*)page->free);
    return ret;
  }

  do {
    p->page_size *= 2;
  } while (p->page_size < size + ALIGN(sizeof(struct sp_mem_page)));

  //printf("-> sp_malloc(): [NEW PAGE] allocating %zu bytes for page\n", p->page_size);
  page = malloc(p->page_size);
  if (! page)
    return NULL;

  page->free = (char *)page + ALIGN(sizeof(struct sp_mem_page));

  //printf("size=%zu, align_size=%zu\n", sizeof(struct sp_mem_page), ALIGN(sizeof(struct sp_mem_page)));
  
  page->free_size = p->page_size - ALIGN(sizeof(struct sp_mem_page));
  page->next = p->page_list;
  p->page_list = page;
  goto again;
}

void *sp_realloc(struct sp_mem_pool *p, void *old_data, size_t size)
{
  if (! p) {
    if (old_data == NULL && size == 0)
      return NULL;
    return realloc(old_data, size);
  }

  if (size == 0)
    return NULL;
  
  void *new_data = sp_malloc(p, size);
  if (new_data && old_data)
    memcpy(new_data, old_data, size);
  return new_data;
}
