/*
 * Copyright (c) 1999-2001
 *      The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */
/* Idea: clear on page alloc rather than individual alloc
   Turns out not so good (on lcc at least, seems a wash on mudlle):
   logically should be bad for small regions (less than a few pages)
*/
#undef PRECLEAR
#undef REGION_PROFILE
#include "stats.c"
#include "regions.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RPAGESIZE (1UL << RPAGELOG)
#define RMAXMAP  (1UL << (RMAXMEMLOG - RMAPLOG - RPAGELOG))
#define K 4
//#define MAXPAGE (1UL << (RMAXMEMLOG - RPAGELOG - RMAPLOG))
#define RMAXPAGE  (1UL << RMAPLOG)


//#define PAGENB(x) (((__rcintptr)(x) >> RPAGELOG) & ((1ULL << RMAXMEMLOG) - 1))
#define MAPNB(x) (((__rcintptr)(x) >> (RPAGELOG + RMAPLOG)) & ((1UL << (RMAXMEMLOG - RMAPLOG - RPAGELOG)) - 1))
#define PAGENB(x) ((__rcintptr)(x) >> RPAGELOG) & ((1UL << RMAPLOG) - 1)


#define ALIGN(x, n) (((x) + ((n) - 1)) & ~((n) - 1))
#define PALIGN(x, n) ((void *)ALIGN((__rcintptr)(x), n))
#ifdef __GNUC__
#define RALIGNMENT __alignof(unsigned long)
#define PTRALIGNMENT __alignof(void *)
#define ALIGNMENT_LONG __alignof(unsigned long)
#else
#define RALIGNMENT 4
#define PTRALIGNMENT 4
#define ALIGNMENT_LONG 4
#endif

typedef unsigned long __rcintptr;

struct ablock {
  char *base, *allocfrom;
};

struct allocator {
  struct ablock page;
  struct ablock superpage;
  struct ablock hyperpage;
  struct page *pages;
  struct page *bigpages;
};

struct region_ {
  struct allocator normal;
  region parent, sibling, children;
};

nomem_handler nomem_h;

region permanent;

static inline void clear(void *start, __rcintptr size)
{
  long *clear, *clearend;

  clear = (long *)start;
  clearend = (long *)((char *)start + size);
  do *clear++ = 0;
  while (clear < clearend) ;
}

#ifdef PRECLEAR
#define preclear clear
#define postclear(s, e)
#else
#define preclear(s, e)
#define postclear clear
#endif

#include "pages.c"
#include "alloc.c"
#include "serialize.c"

static void nochildren(region r)
{
  if (r->children)
    abort();
}

static void unlink_region(region r)
{
  region *scan;

  scan = &r->parent->children;
  while (*scan != r)
    scan = &(*scan)->sibling;
  *scan = (*scan)->sibling;
}

static void link_region(region r, region parent)
{
  r->sibling = parent->children;
  r->parent = parent;
  parent->children = r;
}

static int rstart = 0;

void initregion(region r)
{
  char *first =
    (char *)r - rstart - offsetof(struct page, previous);

  /* Start using page with region header as a pointer-containing page */
  r->normal.page.base = first;
  r->normal.page.allocfrom = (char *)(r + 1);

  /* Guarantee failure for all other blocks */
  r->normal.superpage.allocfrom = (char *)(K * RPAGESIZE + 1);
  r->normal.hyperpage.allocfrom = (char *)(K * K * RPAGESIZE + 1);

  /* Remember that r owns this page. */
  r->normal.pages = (struct page *)first;
  set_region(r->normal.pages, 1, r);
}

region __newregion(void)
{
  return newsubregion(permanent);
}

region newsubregion(region parent)
{
  char *first;
  region r;

  first = (char *)alloc_single_page(NULL);
  preclear(first + offsetof(struct page, pagecount), RPAGESIZE - offsetof(struct page, pagecount));

  /* stagger regions across cache lines a bit */
 rstart = 0; /* was 64, but changed to 0 to allow serialization/deserialization */
#if RPAGESIZE < 1024
#error RPAGESIZE must be at least 1024, or change the next if.
#endif
  if (rstart >= 16 * 64) rstart = 0;
  r = (region)(first + rstart + offsetof(struct page, previous));
  postclear(r, sizeof *r);
  initregion(r);
  ((struct page *) first)->available = r->normal.page.allocfrom;
  if (parent)
    link_region(r, parent);

  return r;
}

/* inline */ char *__rc_rstralloc(region r, size_t size)
{
  void *mem, *dummy;

  record_alloc(size);

  qalloc(r, &r->normal, &dummy, 0, 1, &mem, size, RALIGNMENT, 0);

  return mem;
}

inline char *__rc_rstralloc0(region r, size_t size)
{
  char *mem;

  mem = __rc_rstralloc(r, size);
  clear(mem, size);

  return mem;
}

char *__rc_rstrdup(region r, const char *s)
{
  char *news = __rc_rstralloc(r, strlen(s) + 1);

  strcpy(news, s);

  return news;
}

inline static 
char *internal_rstrextend(region r, const char *old, size_t newsize,
			  int needsclear)
{
  /* For now we don't attempt to extend the old storage area */
  void *newmem, *hdr;
  unsigned long *oldhdr, oldsize;

  record_alloc(newsize);

  qalloc(r, &r->normal, &hdr, sizeof(unsigned long), ALIGNMENT_LONG,
	 &newmem, newsize, RALIGNMENT, 0);

  /* If we don't do this we can't find the header: */
  hdr = (char *)newmem - sizeof(unsigned long);

  *(unsigned long *)hdr = newsize;

  if (old)
    {
      oldhdr = (unsigned long *)(old - ALIGNMENT_LONG);
      oldsize = *oldhdr;

      if (oldsize > newsize)
	oldsize = newsize;
      else if (needsclear)
	clear((char *)newmem + oldsize, newsize - oldsize);
      memcpy(newmem, old, oldsize);
    }
  else if (needsclear)
    clear(newmem, newsize);

  return newmem;
}

inline
char *__rc_rstrextend(region r, const char *old, size_t newsize)
{
  return internal_rstrextend(r, old, newsize, 0);
}

inline
char *__rc_rstrextend0(region r, const char *old, size_t newsize)
{
  return internal_rstrextend(r, old, newsize, 1);
}

inline void *__rc_typed_ralloc(region r, size_t size, type_t t)
{
  return __rc_rstralloc0(r, size);
}

void *__rc_ralloc_small0(region r, size_t size)
{
  char *mem2;

  mem2 = PALIGN(r->normal.page.allocfrom, RALIGNMENT);
  if (mem2 + size >= r->normal.page.base + RPAGESIZE)
    return __rc_typed_ralloc(r, size, 0);

  record_alloc(size);

  r->normal.page.allocfrom = mem2 + size;
  ((struct page *) r->normal.page.base)->available = r->normal.page.allocfrom;
  postclear(mem2, size);

  return mem2;
}

void *__rc_typed_rarrayextend(region r, void *old, size_t n, size_t size, type_t t)
{
  return __rc_rstrextend0(r, old, n * size);
}

void *__rc_typed_rarrayalloc(region r, size_t n, size_t size, type_t t)
{
  return __rc_typed_ralloc(r, n * size, t);
}

void typed_rarraycopy(void *to, void *from, size_t n, size_t size, type_t type)
{
  memcpy(to, from, n * size);
}

static void delregion(region r)
{
  nochildren(r);
  free_all_pages(r, &r->normal);
}

void __deleteregion(region r)
{
  unlink_region(r);
  delregion(r);
}

void __deleteregion_ptr(region *r)
{
  region tmp = *r;

  *r = NULL;
  __deleteregion(tmp);
}

void __deleteregion_array(int n, region *regions)
{
  int i;

  for (i = 0; i < n; i++)
    unlink_region(regions[i]);

  for (i = 0; i < n; i++)
    {
      delregion(regions[i]);
      regions[i] = NULL;
    }
}

region regionof(void *ptr)
{
  return *get_or_alloc_regionmap(MAPNB(ptr), PAGENB(ptr)); //__rcregionmap[(unsigned long)ptr >> RPAGELOG];
}

void region_init(void)
{
  static int initialized = 0;

  if ( initialized )
    return;

  else
    {
      /* rstart = -64; Save 64 bytes of memory! (sometimes ;-)) REMOVED aa */
      init_pages();
      permanent = __newregion();
      if (getenv("REGIONSTATS"))
	benchmark_init();
#ifdef DEBUG_RALLOC
      atexit(memusage);
#endif
    }
  initialized = 1;
}

nomem_handler set_nomem_handler(nomem_handler newhandler)
{
  nomem_handler oldh = nomem_h;

  nomem_h = newhandler;

  return oldh;
}

/*
int region_main(int argc, char **argv, char **envp);

int main(int argc, char **argv, char **envp)
{
  region_init();
  return region_main(argc, argv, envp);
}
*/

/* Debugging support */

static FILE *out;

static void printref(void *x)
{
  if (x >= (void *)__rcregionmap && x < (void *)&__rcregionmap[RMAXMAP])
    return;

#ifdef RCPAIRS
  if (x >= (void *)__rcregions && x < (void *)&__rcregions[MAXREGIONS])
    return;

#endif

  fprintf(out, "info symbol 0x%p\n", x);
}

void findrefs(region r, void *from, void *to)
{
  char *f;

  if (!out)
    out = fopen("/dev/tty", "w");

  for (f = PALIGN(from, PTRALIGNMENT); f < (char *)to; f += PTRALIGNMENT)
    if (regionof(*(void **)f) == r)
      printref(f);

  fflush(out);
}

#if defined(__GNUC__) && defined(sparc)
/* This code breaks some version of sun's cc at least */
extern void _DYNAMIC, _end;

void findgrefs(region r)
{
  findrefs(r, &_DYNAMIC, &_end);
}
#endif

void findrrefs(region r, region from)
{
  struct page *p;

  for (p = from->normal.pages; p; p = p->next)
    findrefs(r, (char *)&p->previous, (char *)p + RPAGESIZE);

  for (p = r->normal.bigpages; p; p = p->next)
    findrefs(r, (char *)&p->previous, (char *)p + p->pagecount * RPAGESIZE);
}

