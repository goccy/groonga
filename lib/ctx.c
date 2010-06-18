/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "groonga_in.h"
#include <string.h>
#include "token.h"
#include "ql.h"
#include "pat.h"
#include "module.h"
#include "snip.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif /* HAVE_NETINET_IN_H */

#define GRN_CTX_INITIALIZER(enc) \
  { GRN_SUCCESS, 0, enc, 0, GRN_LOG_NOTICE,\
      GRN_CTX_FIN, 0, 0, 0, 0, {0}, NULL, NULL, NULL, NULL, NULL }

#define GRN_CTX_CLOSED(ctx) ((ctx)->stat == GRN_CTX_FIN)

#ifdef USE_EXACT_ALLOC_COUNT
#define GRN_ADD_ALLOC_COUNT(count) \
{ \
  uint32_t alloced; \
  GRN_ATOMIC_ADD_EX(&alloc_count, count, alloced); \
}
#else /* USE_EXACT_ALLOC_COUNT */
#define GRN_ADD_ALLOC_COUNT(count) \
{ \
  alloc_count += count; \
}
#endif

grn_ctx grn_gctx = GRN_CTX_INITIALIZER(GRN_ENC_DEFAULT);
int grn_pagesize;
grn_critical_section grn_glock;
uint32_t grn_gtick;

#ifdef USE_UYIELD
int grn_uyield_count = 0;
#endif

/* fixme by 2038 */

grn_rc
grn_timeval_now(grn_ctx *ctx, grn_timeval *tv)
{
#ifdef WIN32
  time_t t;
  struct _timeb tb;
  time(&t);
  _ftime(&tb);
  tv->tv_sec = (int32_t) t;
  tv->tv_usec = tb.millitm * 1000;
  return GRN_SUCCESS;
#else /* WIN32 */
  struct timeval t;
  if (gettimeofday(&t, NULL)) {
    SERR("gettimeofday");
  } else {
    tv->tv_sec = (int32_t) t.tv_sec;
    tv->tv_usec = t.tv_usec;
  }
  return ctx->rc;
#endif /* WIN32 */
}

void
grn_time_now(grn_ctx *ctx, grn_obj *obj)
{
  grn_timeval tv;
  grn_timeval_now(ctx, &tv);
  GRN_TIME_SET(ctx, obj, GRN_TIME_PACK(tv.tv_sec, tv.tv_usec));
}

grn_rc
grn_timeval2str(grn_ctx *ctx, grn_timeval *tv, char *buf)
{
  struct tm *ltm;
#ifdef HAVE_LOCALTIME_R
  struct tm tm;
  time_t t = tv->tv_sec;
  ltm = localtime_r(&t, &tm);
#else /* HAVE_LOCALTIME_R */
  time_t tvsec = (time_t) tv->tv_sec;
  ltm = localtime(&tvsec);
#endif /* HAVE_LOCALTIME_R */
  if (!ltm) { SERR("localtime"); }
  snprintf(buf, GRN_TIMEVAL_STR_SIZE - 1, GRN_TIMEVAL_STR_FORMAT,
           ltm->tm_year + 1900, ltm->tm_mon + 1, ltm->tm_mday,
           ltm->tm_hour, ltm->tm_min, ltm->tm_sec, (int) tv->tv_usec);
  buf[GRN_TIMEVAL_STR_SIZE - 1] = '\0';
  return ctx->rc;
}

grn_rc
grn_str2timeval(const char *str, uint32_t str_len, grn_timeval *tv)
{
  struct tm tm;
  const char *r1, *r2, *rend = str + str_len;
  uint32_t uv;
  memset(&tm, 0, sizeof(struct tm));

  tm.tm_year = (int)grn_atoui(str, rend, &r1) - 1900;
  if ((r1 + 1) >= rend || (*r1 != '/' && *r1 != '-') ||
      tm.tm_year < 0) { return GRN_INVALID_ARGUMENT; }
  r1++;
  tm.tm_mon = (int)grn_atoui(r1, rend, &r1) - 1;
  if ((r1 + 1) >= rend || (*r1 != '/' && *r1 != '-') ||
      tm.tm_mon < 0 || tm.tm_mon >= 12) { return GRN_INVALID_ARGUMENT; }
  r1++;
  tm.tm_mday = (int)grn_atoui(r1, rend, &r1);
  if ((r1 + 1) >= rend || *r1 != ' ' ||
      tm.tm_mday < 1 || tm.tm_mday > 31) { return GRN_INVALID_ARGUMENT; }

  tm.tm_hour = (int)grn_atoui(++r1, rend, &r2);
  if ((r2 + 1) >= rend || r1 == r2 || *r2 != ':' ||
      tm.tm_hour < 0 || tm.tm_hour >= 24) {
    return GRN_INVALID_ARGUMENT;
  }
  r1 = r2 + 1;
  tm.tm_min = (int)grn_atoui(r1, rend, &r2);
  if ((r2 + 1) >= rend || r1 == r2 || *r2 != ':' ||
      tm.tm_min < 0 || tm.tm_min >= 60) {
    return GRN_INVALID_ARGUMENT;
  }
  r1 = r2 + 1;
  tm.tm_sec = (int)grn_atoui(r1, rend, &r2);
  if (r1 == r2 ||
      tm.tm_sec < 0 || tm.tm_sec > 61 /* leap 2sec */) {
    return GRN_INVALID_ARGUMENT;
  }
  r1 = r2;

  if ((tv->tv_sec = (int32_t) mktime(&tm)) == -1) { return GRN_INVALID_ARGUMENT; }
  if ((r1 + 1) < rend && *r1 == '.') { r1++; }
  uv = grn_atoi(r1, rend, &r2);
  while (r2 < r1 + 6) {
    uv *= 10;
    r2++;
  }
  if (uv >= 1000000) { return GRN_INVALID_ARGUMENT; }
  tv->tv_usec = uv;
  return GRN_SUCCESS;
}

#ifdef USE_FAIL_MALLOC
int grn_fmalloc_prob = 0;
char *grn_fmalloc_func = NULL;
char *grn_fmalloc_file = NULL;
int grn_fmalloc_line = 0;
#endif /* USE_FAIL_MALLOC */

#define GRN_CTX_SEGMENT_SIZE    (1<<22)
#define GRN_CTX_SEGMENT_MASK    (GRN_CTX_SEGMENT_SIZE - 1)

#define GRN_CTX_SEGMENT_WORD    (1<<31)
#define GRN_CTX_SEGMENT_VLEN    (1<<30)
#define GRN_CTX_SEGMENT_LIFO    (1<<29)
#define GRN_CTX_SEGMENT_DIRTY   (1<<28)

#ifdef USE_DYNAMIC_MALLOC_CHANGE
static void
grn_ctx_impl_init_malloc(grn_ctx *ctx)
{
#  ifdef USE_FAIL_MALLOC
  ctx->impl->malloc_func = grn_malloc_fail;
  ctx->impl->calloc_func = grn_calloc_fail;
  ctx->impl->realloc_func = grn_realloc_fail;
  ctx->impl->strdup_func = grn_strdup_fail;
#  else
  ctx->impl->malloc_func = grn_malloc_default;
  ctx->impl->calloc_func = grn_calloc_default;
  ctx->impl->realloc_func = grn_realloc_default;
  ctx->impl->strdup_func = grn_strdup_default;
#  endif
}
#endif

static void
grn_loader_init(grn_loader *loader)
{
  GRN_TEXT_INIT(&loader->values, 0);
  GRN_UINT32_INIT(&loader->level, GRN_OBJ_VECTOR);
  GRN_PTR_INIT(&loader->columns, GRN_OBJ_VECTOR, GRN_ID_NIL);
  loader->key_offset = -1;
  loader->table = NULL;
  loader->last = NULL;
  loader->ifexists = NULL;
  loader->values_size = 0;
  loader->nrecords = 0;
  loader->stat = GRN_LOADER_BEGIN;
}

void
grn_ctx_loader_clear(grn_ctx *ctx)
{
  grn_loader *loader = &ctx->impl->loader;
  grn_obj *v = (grn_obj *)(GRN_BULK_HEAD(&loader->values));
  grn_obj *ve = (grn_obj *)(GRN_BULK_CURR(&loader->values));
  grn_obj **p = (grn_obj **)GRN_BULK_HEAD(&loader->columns);
  uint32_t i = GRN_BULK_VSIZE(&loader->columns) / sizeof(grn_obj *);
  if (ctx->impl->db) { while (i--) { grn_obj_unlink(ctx, *p++); } }
  if (loader->ifexists) { grn_obj_unlink(ctx, loader->ifexists); }
  while (v < ve) { GRN_OBJ_FIN(ctx, v++); }
  GRN_OBJ_FIN(ctx, &loader->values);
  GRN_OBJ_FIN(ctx, &loader->level);
  GRN_OBJ_FIN(ctx, &loader->columns);
  grn_loader_init(loader);
}

#define IMPL_SIZE ((sizeof(struct _grn_ctx_impl) + (grn_pagesize - 1)) & ~(grn_pagesize - 1))

static void
grn_ctx_impl_init(grn_ctx *ctx)
{

  grn_io_mapinfo mi;
  if (!(ctx->impl = grn_io_anon_map(ctx, &mi, IMPL_SIZE))) {
    ctx->impl = NULL;
    return;
  }
#ifdef USE_DYNAMIC_MALLOC_CHANGE
  grn_ctx_impl_init_malloc(ctx);
#endif
  ctx->impl->encoding = ctx->encoding;
  ctx->impl->lifoseg = -1;
  ctx->impl->currseg = -1;
  if (!(ctx->impl->values = grn_array_create(ctx, NULL, sizeof(grn_tmp_db_obj),
                                             GRN_ARRAY_TINY))) {
    grn_io_anon_unmap(ctx, &mi, IMPL_SIZE);
    ctx->impl = NULL;
    return;
  }
  CRITICAL_SECTION_INIT(ctx->impl->lock);
  ctx->impl->db = NULL;

  ctx->impl->expr_vars = grn_hash_create(ctx, NULL, sizeof(grn_id), sizeof(grn_obj *), 0);
  ctx->impl->stack_curr = 0;
  ctx->impl->qe_next = NULL;
  ctx->impl->parser = NULL;

  GRN_TEXT_INIT(&ctx->impl->names, GRN_OBJ_VECTOR);
  GRN_UINT32_INIT(&ctx->impl->levels, GRN_OBJ_VECTOR);

  ctx->impl->phs = NIL;
  ctx->impl->code = NIL;
  ctx->impl->dump = NIL;
  ctx->impl->op = GRN_OP_T0LVL;
  ctx->impl->args = NIL;
  ctx->impl->envir = NIL;
  ctx->impl->value = NIL;
  ctx->impl->ncells = 0;
  ctx->impl->n_entries = 0;
  ctx->impl->seqno = 0;
  ctx->impl->lseqno = 0;
  ctx->impl->nbinds = 0;
  ctx->impl->nunbinds = 0;
  ctx->impl->feed_mode = grn_ql_atonce;
  ctx->impl->cur = NULL;
  ctx->impl->str_end = NULL;
  ctx->impl->batchmode = 0;
  ctx->impl->gc_verbose = 0;
  ctx->impl->inbuf = NULL;
  ctx->impl->co.mode = 0;
  ctx->impl->co.func = NULL;
  ctx->impl->objects = NULL;
  ctx->impl->symbols = NULL;
  ctx->impl->com = NULL;
  ctx->impl->outbuf = grn_obj_open(ctx, GRN_BULK, 0, 0);
  ctx->impl->output = NULL /* grn_ctx_concat_func */;
  ctx->impl->data.ptr = NULL;
  ctx->impl->tv.tv_sec = 0;
  ctx->impl->tv.tv_usec = 0;
  GRN_TEXT_INIT(&ctx->impl->subbuf, 0);
  ctx->impl->edge = NULL;
  grn_loader_init(&ctx->impl->loader);
  ctx->impl->module_path = NULL;
}

void
grn_ctx_set_next_expr(grn_ctx *ctx, grn_obj *expr)
{
  ctx->impl->qe_next = expr;
}

void
grn_ctx_impl_err(grn_ctx *ctx)
{
  if (ctx->impl) {
    ctx->impl->cur = ctx->impl->str_end;
    ctx->impl->op = GRN_OP_ERR0;
  }
}

static void
grn_ctx_ql_init(grn_ctx *ctx, int flags)
{
  if (!ctx->impl) {
    grn_ctx_impl_init(ctx);
    if (ERRP(ctx, GRN_ERROR)) { return; }
  }
  if (flags & GRN_CTX_BATCH_MODE) { ctx->impl->batchmode = 1; }
  if ((ctx->impl->objects = grn_array_create(ctx, NULL, sizeof(grn_cell),
                                             GRN_ARRAY_TINY))) {
    if ((ctx->impl->symbols = grn_hash_create(ctx, NULL, GRN_TABLE_MAX_KEY_SIZE,
                                              sizeof(grn_cell),
                                              GRN_OBJ_KEY_VAR_SIZE|GRN_HASH_TINY))) {
      if (!ERRP(ctx, GRN_ERROR)) {
        grn_ql_init_globals(ctx);
        if (!ERRP(ctx, GRN_ERROR)) {
          return;
        }
      }
      grn_hash_close(ctx, ctx->impl->symbols);
      ctx->impl->symbols = NULL;
    } else {
      MERR("ctx->impl->symbols init failed");
    }
    grn_array_close(ctx, ctx->impl->objects);
    ctx->impl->objects = NULL;
  } else {
    MERR("ctx->impl->objects init failed");
  }
}

grn_rc
grn_ctx_init(grn_ctx *ctx, int flags)
{
  if (!ctx) { return GRN_INVALID_ARGUMENT; }
  // if (ctx->stat != GRN_CTX_FIN) { return GRN_INVALID_ARGUMENT; }
  ERRCLR(ctx);
  ctx->flags = flags;
  ctx->stat = GRN_QL_WAIT_EXPR;
  ctx->encoding = grn_gctx.encoding;
  ctx->seqno = 0;
  ctx->seqno2 = 0;
  ctx->subno = 0;
  ctx->impl = NULL;
  if (flags & GRN_CTX_USE_QL) {
    grn_ctx_ql_init(ctx, flags);
    if (ERRP(ctx, GRN_ERROR)) { return ctx->rc; }
  }
  ctx->user_data.ptr = NULL;
  CRITICAL_SECTION_ENTER(grn_glock);
  ctx->next = grn_gctx.next;
  ctx->prev = &grn_gctx;
  grn_gctx.next->prev = ctx;
  grn_gctx.next = ctx;
  CRITICAL_SECTION_LEAVE(grn_glock);
  return ctx->rc;
}

grn_ctx *
grn_ctx_open(int flags)
{
  grn_ctx *ctx = GRN_GMALLOCN(grn_ctx, 1);
  if (ctx) {
    grn_ctx_init(ctx, flags|GRN_CTX_ALLOCATED);
    if (ERRP(ctx, GRN_ERROR)) {
      grn_ctx_fin(ctx);
      GRN_GFREE(ctx);
      ctx = NULL;
    }
  }
  return ctx;
}

grn_rc
grn_ctx_fin(grn_ctx *ctx)
{
  grn_rc rc = GRN_SUCCESS;
  if (!ctx) { return GRN_INVALID_ARGUMENT; }
  if (ctx->stat == GRN_CTX_FIN) { return GRN_INVALID_ARGUMENT; }
  if (!(ctx->flags & GRN_CTX_ALLOCATED)) {
    CRITICAL_SECTION_ENTER(grn_glock);
    ctx->next->prev = ctx->prev;
    ctx->prev->next = ctx->next;
    CRITICAL_SECTION_LEAVE(grn_glock);
  }
  if (ctx->impl) {
    grn_ctx_loader_clear(ctx);
    if (ctx->impl->objects) {
      grn_cell *o;
      GRN_ARRAY_EACH(ctx, ctx->impl->objects, 0, 0, id, &o, {
        grn_cell_clear(ctx, o);
      });
      grn_array_close(ctx, ctx->impl->objects);
    }
    if (ctx->impl->parser) {
      grn_expr_parser_close(ctx);
    }
    if (ctx->impl->values) {
      grn_tmp_db_obj *o;
      GRN_ARRAY_EACH(ctx, ctx->impl->values, 0, 0, id, &o, {
        grn_obj_close(ctx, (grn_obj *)o->obj);
      });
      grn_array_close(ctx, ctx->impl->values);
    }
    if (ctx->impl->symbols) {
      grn_hash_close(ctx, ctx->impl->symbols);
    }
    if (ctx->impl->com) {
      if (ctx->stat != GRN_CTX_QUIT) {
        int flags;
        char *str;
        unsigned int str_len;
        grn_ctx_send(ctx, "quit", 4, GRN_CTX_HEAD);
        grn_ctx_recv(ctx, &str, &str_len, &flags);
      }
      grn_ctx_send(ctx, "ACK", 3, GRN_CTX_HEAD);
      rc = grn_com_close(ctx, ctx->impl->com);
    }
    GRN_OBJ_FIN(ctx, &ctx->impl->names);
    GRN_OBJ_FIN(ctx, &ctx->impl->levels);
    rc = grn_obj_close(ctx, ctx->impl->outbuf);
    rc = grn_bulk_fin(ctx, &ctx->impl->subbuf);
    {
      grn_hash **vp;
      grn_obj *value;
      GRN_HASH_EACH(ctx, ctx->impl->expr_vars, eid, NULL, NULL, &vp, {
        if (*vp) {
          GRN_HASH_EACH(ctx, *vp, id, NULL, NULL, &value, {
            GRN_OBJ_FIN(ctx, value);
          });
        }
        grn_hash_close(ctx, *vp);
      });
    }
    grn_hash_close(ctx, ctx->impl->expr_vars);
    {
      int i;
      grn_io_mapinfo *mi;
      for (i = 0, mi = ctx->impl->segs; i < GRN_CTX_N_SEGMENTS; i++, mi++) {
        if (mi->map) {
          //GRN_LOG(ctx, GRN_LOG_NOTICE, "unmap in ctx_fin(%d,%d,%d)", i, (mi->count & GRN_CTX_SEGMENT_MASK), mi->nref);
          if (mi->count & GRN_CTX_SEGMENT_VLEN) {
            grn_io_anon_unmap(ctx, mi, mi->nref * grn_pagesize);
          } else {
            grn_io_anon_unmap(ctx, mi, GRN_CTX_SEGMENT_SIZE);
          }
        }
      }
    }
    {
      grn_io_mapinfo mi;
      mi.map = (void *)ctx->impl;
      grn_io_anon_unmap(ctx, &mi, IMPL_SIZE);
    }
    CRITICAL_SECTION_FIN(ctx->impl->lock);
  }
  ctx->stat = GRN_CTX_FIN;
  return rc;
}

grn_timeval grn_starttime;
const char *grn_log_path = GROONGA_LOG_PATH;
const char *grn_qlog_path = NULL;

static FILE *default_logger_fp = NULL;
static FILE *default_logger_qlog_fp = NULL;
static grn_critical_section grn_logger_lock;

static void
default_logger_func(int level, const char *time, const char *title,
                    const char *msg, const char *location, void *func_arg)
{
  const char slev[] = " EACewnid-";
  if (level > GRN_LOG_NONE) {
    if (grn_log_path) {
      CRITICAL_SECTION_ENTER(grn_logger_lock);
      if (!default_logger_fp) {
        default_logger_fp = fopen(grn_log_path, "a");
      }
      if (default_logger_fp) {
        if (location && *location) {
          fprintf(default_logger_fp, "%s|%c|%s %s %s\n",
                  time, *(slev + level), title, msg, location);
        } else {
          fprintf(default_logger_fp, "%s|%c|%s %s\n", time, *(slev + level), title, msg);
        }
        fflush(default_logger_fp);
      }
      CRITICAL_SECTION_LEAVE(grn_logger_lock);
    }
  } else {
    if (grn_qlog_path) {
      CRITICAL_SECTION_ENTER(grn_logger_lock);
      if (!default_logger_qlog_fp) {
        default_logger_qlog_fp = fopen(grn_qlog_path, "a");
      }
      if (default_logger_qlog_fp) {
        fprintf(default_logger_qlog_fp, "%s|%s\n", time, msg);
        fflush(default_logger_qlog_fp);
      }
      CRITICAL_SECTION_LEAVE(grn_logger_lock);
    }
  }
}

static grn_logger_info default_logger = {
  GRN_LOG_DEFAULT_LEVEL,
  GRN_LOG_TIME|GRN_LOG_MESSAGE,
  default_logger_func
};

static const grn_logger_info *grn_logger = &default_logger;

void
grn_log_reopen(grn_ctx *ctx)
{
  if (grn_logger != &default_logger) {
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
        "grn_log_reopen() is not implemented with a custom grn_logger.");
    return;
  }

  if (grn_log_path) {
    GRN_LOG(ctx, GRN_LOG_NOTICE, "log will be closed.");
    CRITICAL_SECTION_ENTER(grn_logger_lock);
    if (default_logger_fp) {
      fclose(default_logger_fp);
      default_logger_fp = NULL;
    }
    CRITICAL_SECTION_LEAVE(grn_logger_lock);
    GRN_LOG(ctx, GRN_LOG_NOTICE, "log opened.");
  }

  if (grn_qlog_path) {
    GRN_LOG(ctx, GRN_LOG_NONE, "query log will be closed.");
    CRITICAL_SECTION_ENTER(grn_logger_lock);
    if (default_logger_qlog_fp) {
      fclose(default_logger_qlog_fp);
      default_logger_qlog_fp = NULL;
    }
    CRITICAL_SECTION_LEAVE(grn_logger_lock);
    GRN_LOG(ctx, GRN_LOG_NONE, "query log opened.");
  }
}

static grn_obj grn_true_, grn_false_, grn_null_;
grn_obj *grn_true, *grn_false, *grn_null;

grn_rc
grn_init(void)
{
  grn_rc rc;
  grn_ctx *ctx = &grn_gctx;
  grn_logger = &default_logger;
  CRITICAL_SECTION_INIT(grn_glock);
  CRITICAL_SECTION_INIT(grn_logger_lock);
  grn_gtick = 0;
  grn_ql_init_const();
  ctx->next = ctx;
  ctx->prev = ctx;
  grn_ctx_init(ctx, 0);
  ctx->encoding = grn_strtoenc(GROONGA_DEFAULT_ENCODING);
  grn_true = &grn_true_;
  grn_false = &grn_false_;
  grn_null = &grn_null_;
  GRN_VOID_INIT(grn_true);
  GRN_VOID_INIT(grn_false);
  GRN_VOID_INIT(grn_null);
  grn_timeval_now(ctx, &grn_starttime);
#ifdef WIN32
  {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    grn_pagesize = si.dwAllocationGranularity;
  }
#else /* WIN32 */
  if ((grn_pagesize = sysconf(_SC_PAGESIZE)) == -1) {
    SERR("_SC_PAGESIZE");
    return ctx->rc;
  }
#endif /* WIN32 */
  if (grn_pagesize & (grn_pagesize - 1)) {
    GRN_LOG(ctx, GRN_LOG_CRIT, "pagesize=%x", grn_pagesize);
  }
  // expand_stack();
#ifdef USE_AIO
  if (getenv("GRN_DEBUG_PRINT")) {
    grn_debug_print = atoi(getenv("GRN_DEBUG_PRINT"));
  } else {
    grn_debug_print = 0;
  }
  if (getenv("GRN_AIO_ENABLED")) {
    grn_aio_enabled = atoi(getenv("GRN_AIO_ENABLED"));
  } else {
    grn_aio_enabled = 0;
  }
  if (grn_aio_enabled) {
    GRN_LOG(ctx, GRN_LOG_NOTICE, "AIO and DIO enabled");
    grn_cache_open();
  }
#endif /* USE_AIO */
#ifdef USE_FAIL_MALLOC
  if (getenv("GRN_FMALLOC_PROB")) {
    grn_fmalloc_prob = strtod(getenv("GRN_FMALLOC_PROB"), 0) * RAND_MAX;
    if (getenv("GRN_FMALLOC_SEED")) {
      srand((unsigned int)atoi(getenv("GRN_FMALLOC_SEED")));
    } else {
      srand((unsigned int)time(NULL));
    }
  }
  if (getenv("GRN_FMALLOC_FUNC")) {
    grn_fmalloc_func = getenv("GRN_FMALLOC_FUNC");
  }
  if (getenv("GRN_FMALLOC_FILE")) {
    grn_fmalloc_file = getenv("GRN_FMALLOC_FILE");
  }
  if (getenv("GRN_FMALLOC_LINE")) {
    grn_fmalloc_line = atoi(getenv("GRN_FMALLOC_LINE"));
  }
#endif /* USE_FAIL_MALLOC */
  if ((rc = grn_com_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_com_init failed (%d)", rc);
    return rc;
  }
  grn_ctx_ql_init(ctx, 0);
  if ((rc = ctx->rc)) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "gctx initialize failed (%d)", rc);
    return rc;
  }
  if ((rc = grn_io_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "io initialize failed (%d)", rc);
    return rc;
  }
  if ((rc = grn_modules_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "modules initialize failed (%d)", rc);
    return rc;
  }
  if ((rc = grn_token_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_token_init failed (%d)", rc);
    return rc;
  }
  /*
  if ((rc = grn_index_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "index initialize failed (%d)", rc);
    return rc;
  }
  */
  grn_cache_init();
  GRN_LOG(ctx, GRN_LOG_NOTICE, "grn_init");
  return rc;
}

grn_encoding
grn_get_default_encoding(void)
{
  return grn_gctx.encoding;
}

grn_rc
grn_set_default_encoding(grn_encoding encoding)
{
  switch (encoding) {
  case GRN_ENC_DEFAULT :
    grn_gctx.encoding = grn_strtoenc(GROONGA_DEFAULT_ENCODING);
    return GRN_SUCCESS;
  case GRN_ENC_NONE :
  case GRN_ENC_EUC_JP :
  case GRN_ENC_UTF8 :
  case GRN_ENC_SJIS :
  case GRN_ENC_LATIN1 :
  case GRN_ENC_KOI8R :
    grn_gctx.encoding = encoding;
    return GRN_SUCCESS;
  default :
    return GRN_INVALID_ARGUMENT;
  }
}

static int alloc_count = 0;

grn_rc
grn_fin(void)
{
  grn_ctx *ctx, *ctx_;
  if (grn_gctx.stat == GRN_CTX_FIN) { return GRN_INVALID_ARGUMENT; }
  for (ctx = grn_gctx.next; ctx != &grn_gctx; ctx = ctx_) {
    ctx_ = ctx->next;
    if (ctx->stat != GRN_CTX_FIN) { grn_ctx_fin(ctx); }
    if (ctx->flags & GRN_CTX_ALLOCATED) {
      ctx->next->prev = ctx->prev;
      ctx->prev->next = ctx->next;
      GRN_GFREE(ctx);
    }
  }
  grn_cache_fin();
  grn_token_fin();
  grn_modules_fin();
  grn_io_fin();
  grn_ctx_fin(ctx);
  grn_com_fin();
  GRN_LOG(ctx, GRN_LOG_NOTICE, "grn_fin (%d)", alloc_count);
  grn_logger_fin();
  CRITICAL_SECTION_FIN(grn_logger_lock);
  CRITICAL_SECTION_FIN(grn_glock);
  return GRN_SUCCESS;
}

grn_rc
grn_ctx_connect(grn_ctx *ctx, const char *host, int port, int flags)
{
  GRN_API_ENTER;
  if (!ctx->impl) { grn_ctx_impl_init(ctx); }
  if (!ctx->impl) { goto exit; }
  {
    grn_com *com = grn_com_copen(ctx, NULL, host, port);
    if (com) {
      ctx->impl->com = com;
      return GRN_SUCCESS;
    }
  }
exit :
  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_ctx_close(grn_ctx *ctx)
{
  grn_rc rc = grn_ctx_fin(ctx);
  GRN_GFREE(ctx);
  return rc;
}

grn_content_type
grn_get_ctype(grn_obj *var)
{
  grn_content_type ct = GRN_CONTENT_JSON;
  if (var->header.domain == GRN_DB_INT32) {
    ct = GRN_INT32_VALUE(var);
  } else if (GRN_TEXT_LEN(var)) {
    switch (*(GRN_TEXT_VALUE(var))) {
    case 't' :
    case 'T' :
      ct = GRN_CONTENT_TSV;
      break;
    case 'j' :
    case 'J' :
      ct = GRN_CONTENT_JSON;
      break;
    case 'x' :
    case 'X' :
      ct = GRN_CONTENT_XML;
      break;
    }
  }
  return ct;
}

static void
get_content_mime_type(grn_ctx *ctx, const char *p, const char *pe)
{
  if (p + 2 <= pe) {
    switch (*p) {
    case 'c' :
      if (p + 3 == pe && !memcmp(p, "css", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "text/css";
      }
      break;
    case 'g' :
      if (p + 3 == pe && !memcmp(p, "gif", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "image/gif";
      }
      break;
    case 'h' :
      if (p + 4 == pe && !memcmp(p, "html", 4)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "text/html";
      }
      break;
    case 'j' :
      if (!memcmp(p, "js", 2)) {
        if (p + 2 == pe) {
          ctx->impl->output_type = GRN_CONTENT_NONE;
          ctx->impl->mime_type = "text/javascript";
        } else if (p + 4 == pe && !memcmp(p + 2, "on", 2)) {
          ctx->impl->output_type = GRN_CONTENT_JSON;
          ctx->impl->mime_type = "application/json";
        }
      } else if (p + 3 == pe && !memcmp(p, "jpg", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "image/jpeg";
      }
      break;
    case 'p' :
      if (p + 3 == pe && !memcmp(p, "png", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "image/png";
      }
      break;
    case 't' :
      if (p + 3 == pe && !memcmp(p, "txt", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "text/plain";
      } else if (p + 3 == pe && !memcmp(p, "tsv", 3)) {
        ctx->impl->output_type = GRN_CONTENT_TSV;
        ctx->impl->mime_type = "text/plain";
      }
      break;
    case 'x':
      if (p + 3 == pe && !memcmp(p, "xml", 3)) {
        ctx->impl->output_type = GRN_CONTENT_XML;
        ctx->impl->mime_type = "text/xml";
      }
      break;
    }
  }
}

static void
grn_str_get_mime_type(grn_ctx *ctx, const char *p, const char *pe,
                     const char **key_end, const char **filename_end)
{
  const char *pd = NULL;
  for (; p < pe && *p != '?' && *p != '#'; p++) {
    if (*p == '.') { pd = p; }
  }
  *filename_end = p;
  if (pd && pd < p) {
    get_content_mime_type(ctx, pd + 1, p);
    *key_end = pd;
  } else {
    *key_end = pe;
  }
}

#define INDEX_HTML "index.html"
#define OUTPUT_TYPE "output_type"
#define EXPR_MISSING "expr_missing"
#define OUTPUT_TYPE_LEN (sizeof(OUTPUT_TYPE) - 1)

grn_obj *
grn_ctx_qe_exec_uri(grn_ctx *ctx, const char *path, uint32_t path_len)
{
  grn_obj buf, *expr, *val;
  const char *p = path, *e = path + path_len, *v, *key_end, *filename_end;
  GRN_TEXT_INIT(&buf, 0);
  p = grn_text_urldec(ctx, &buf, p, e, '?');
  if (!GRN_TEXT_LEN(&buf)) { GRN_TEXT_SETS(ctx, &buf, INDEX_HTML); }
  v = GRN_TEXT_VALUE(&buf);
  grn_str_get_mime_type(ctx, v, GRN_BULK_CURR(&buf), &key_end, &filename_end);
  if ((GRN_TEXT_LEN(&buf) >= 2 && v[0] == 'd' && v[1] == '/') &&
      (expr = grn_ctx_get(ctx, v + 2, key_end - (v + 2)))) {
    while (p < e) {
      int l;
      GRN_BULK_REWIND(&buf);
      p = grn_text_cgidec(ctx, &buf, p, e, '=');
      v = GRN_TEXT_VALUE(&buf);
      l = GRN_TEXT_LEN(&buf);
      if (l == OUTPUT_TYPE_LEN && !memcmp(v, OUTPUT_TYPE, OUTPUT_TYPE_LEN)) {
        GRN_BULK_REWIND(&buf);
        p = grn_text_cgidec(ctx, &buf, p, e, '&');
        v = GRN_TEXT_VALUE(&buf);
        get_content_mime_type(ctx, v, GRN_BULK_CURR(&buf));
      } else {
        if (!(val = grn_expr_get_or_add_var(ctx, expr, v, l))) {
          val = &buf;
        }
        grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
        p = grn_text_cgidec(ctx, val, p, e, '&');
      }
    }
    ctx->impl->curr_expr = expr;
    grn_expr_exec(ctx, expr, 0);
  } else if ((expr = grn_ctx_get(ctx, GRN_EXPR_MISSING_NAME,
                                 strlen(GRN_EXPR_MISSING_NAME)))) {
    if ((val = grn_expr_get_var_by_offset(ctx, expr, 0))) {
      grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
      GRN_TEXT_SET(ctx, val, v, filename_end - v);
    }
    ctx->impl->curr_expr = expr;
    grn_expr_exec(ctx, expr, 0);
  }
  GRN_OBJ_FIN(ctx, &buf);
  return expr;
}

grn_obj *
grn_ctx_qe_exec(grn_ctx *ctx, const char *str, uint32_t str_len)
{
  char tok_type;
  int offset = 0;
  grn_obj buf, *expr = NULL, *val = NULL;
  const char *p = str, *e = str + str_len, *v;
  GRN_TEXT_INIT(&buf, 0);
  p = grn_text_unesc_tok(ctx, &buf, p, e, &tok_type);
  if ((expr = grn_ctx_get(ctx, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf)))) {
    while (p < e) {
      GRN_BULK_REWIND(&buf);
      p = grn_text_unesc_tok(ctx, &buf, p, e, &tok_type);
      v = GRN_TEXT_VALUE(&buf);
      switch (tok_type) {
      case GRN_TOK_VOID :
        p = e;
        break;
      case GRN_TOK_SYMBOL :
        if (GRN_TEXT_LEN(&buf) > 2 && v[0] == '-' && v[1] == '-') {
          int l = GRN_TEXT_LEN(&buf) - 2;
          v += 2;
          if (l == OUTPUT_TYPE_LEN && !memcmp(v, OUTPUT_TYPE, OUTPUT_TYPE_LEN)) {
            GRN_BULK_REWIND(&buf);
            p = grn_text_unesc_tok(ctx, &buf, p, e, &tok_type);
            v = GRN_TEXT_VALUE(&buf);
            get_content_mime_type(ctx, v, GRN_BULK_CURR(&buf));
          } else if ((val = grn_expr_get_or_add_var(ctx, expr, v, l))) {
            grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
            p = grn_text_unesc_tok(ctx, val, p, e, &tok_type);
          } else {
            p = e;
          }
          break;
        }
        // fallthru
      case GRN_TOK_STRING :
      case GRN_TOK_QUOTE :
        if ((val = grn_expr_get_var_by_offset(ctx, expr, offset++))) {
          grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
          GRN_TEXT_PUT(ctx, val, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf));
        } else {
          p = e;
        }
        break;
      }
    }
    ctx->impl->curr_expr = expr;
    grn_expr_exec(ctx, expr, 0);
  }
  GRN_OBJ_FIN(ctx, &buf);
  return expr;
}

grn_rc
grn_ctx_sendv(grn_ctx *ctx, int argc, char **argv, int flags)
{
  grn_obj buf;
  GRN_TEXT_INIT(&buf, 0);
  while (argc--) {
    // todo : encode into json like syntax
    GRN_TEXT_PUTS(ctx, &buf, *argv);
    argv++;
    if (argc) { GRN_TEXT_PUTC(ctx, &buf, ' '); }
  }
  grn_ctx_send(ctx, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf), flags);
  GRN_OBJ_FIN(ctx, &buf);
  return ctx->rc;
}

unsigned int
grn_ctx_send(grn_ctx *ctx, const char *str, unsigned int str_len, int flags)
{
  if (!ctx) { return 0; }
  GRN_API_ENTER;
  if (ctx->impl) {
    if (ctx->impl->com) {
      grn_rc rc;
      grn_com_header sheader;
      if ((flags & GRN_CTX_MORE)) { flags |= GRN_CTX_QUIET; }
      if (ctx->stat == GRN_CTX_QUIT) { flags |= GRN_CTX_QUIT; }
      sheader.proto = GRN_COM_PROTO_GQTP;
      sheader.qtype = 0;
      sheader.keylen = 0;
      sheader.level = 0;
      sheader.flags = flags;
      sheader.status = 0;
      sheader.opaque = 0;
      sheader.cas = 0;
      if ((rc = grn_com_send(ctx, ctx->impl->com, &sheader, (char *)str, str_len, 0))) {
        ERR(rc, "grn_com_send failed");
      }
      goto exit;
    } else {
      grn_obj *expr;
      if (ctx->impl->qe_next) {
        grn_obj *val;
        expr = ctx->impl->qe_next;
        ctx->impl->qe_next = NULL;
        if ((val = grn_expr_get_var_by_offset(ctx, expr, 0))) {
          grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
          GRN_TEXT_PUT(ctx, val, str, str_len);
        }
        grn_expr_exec(ctx, expr, 0);
      } else {
        ctx->impl->mime_type = "application/json";
        ctx->impl->output_type = GRN_CONTENT_JSON;
        ctx->impl->opened = 1;
        grn_timeval_now(ctx, &ctx->impl->tv);
        GRN_LOG(ctx, GRN_LOG_NONE, "%08x|>%.*s", (intptr_t)ctx, str_len, str);
        if (str_len && *str == '/') {
          expr = grn_ctx_qe_exec_uri(ctx, str + 1, str_len - 1);
        } else {
          expr = grn_ctx_qe_exec(ctx, str, str_len);
        }
      }
      if (ctx->stat == GRN_CTX_QUITTING) { ctx->stat = GRN_CTX_QUIT; }
      if (!ERRP(ctx, GRN_CRIT)) {
        if (!(flags & GRN_CTX_QUIET) && ctx->impl->output) {
          ctx->impl->output(ctx, 0, ctx->impl->data.ptr);
        }
      }
      if (expr) { grn_expr_clear_vars(ctx, expr); }
      if (!ctx->impl->qe_next) {
        uint64_t et;
        grn_timeval tv;
        grn_timeval_now(ctx, &tv);
        et = (tv.tv_sec - ctx->impl->tv.tv_sec) * GRN_TIME_USEC_PER_SEC
          + (tv.tv_usec - ctx->impl->tv.tv_usec);
        GRN_LOG(ctx, GRN_LOG_NONE, "%08x|<%012zu rc=%d", (intptr_t)ctx, et, ctx->rc);
      }
      goto exit;
    }
  }
  ERR(GRN_INVALID_ARGUMENT, "invalid ctx assigned");
exit :
  GRN_API_RETURN(0);
}

unsigned
grn_ctx_recv(grn_ctx *ctx, char **str, unsigned int *str_len, int *flags)
{
  if (!ctx) { return GRN_INVALID_ARGUMENT; }
  if (ctx->stat == GRN_CTX_QUIT) {
    *str = NULL;
    *str_len = 0;
    *flags = GRN_CTX_QUIT;
    return 0;
  }
  GRN_API_ENTER;
  if (ctx->impl) {
    if (ctx->impl->com) {
      grn_com_header header;
      if (grn_com_recv(ctx, ctx->impl->com, &header, ctx->impl->outbuf)) {
        *str = NULL;
        *str_len = 0;
        *flags = 0;
      } else {
        *str = GRN_BULK_HEAD(ctx->impl->outbuf);
        *str_len = GRN_BULK_VSIZE(ctx->impl->outbuf);
        if (header.flags & GRN_CTX_QUIT) {
          ctx->stat = GRN_CTX_QUIT;
          *flags = GRN_CTX_QUIT;
        } else {
          *flags = (header.flags & GRN_CTX_TAIL) ? 0 : GRN_CTX_MORE;
        }
      }
      if (ctx->rc) {
        ERR(ctx->rc, "grn_com_recv failed!");
      }
      goto exit;
    } else {
      grn_obj *buf = ctx->impl->outbuf;
      unsigned int head = 0, tail = GRN_BULK_VSIZE(buf);
      /*
      unsigned int *offsets = (unsigned int *) GRN_BULK_HEAD(&ctx->impl->subbuf);
      int npackets = GRN_BULK_VSIZE(&ctx->impl->subbuf) / sizeof(unsigned int);
      if (npackets < ctx->impl->bufcur) {
        ERR(GRN_INVALID_ARGUMENT, "invalid argument");
        goto exit;
      }
      head = ctx->impl->bufcur ? offsets[ctx->impl->bufcur - 1] : 0;
      tail = ctx->impl->bufcur < npackets ? offsets[ctx->impl->bufcur] : GRN_BULK_VSIZE(buf);
      *flags = ctx->impl->bufcur++ < npackets ? GRN_CTX_MORE : 0;
      */
      *str = GRN_BULK_HEAD(buf) + head;
      *str_len = tail - head;
      GRN_BULK_REWIND(ctx->impl->outbuf);
      goto exit;
    }
  }
  ERR(GRN_INVALID_ARGUMENT, "invalid ctx assigned");
exit :
  GRN_API_RETURN(0);
}

void
grn_ctx_concat_func(grn_ctx *ctx, int flags, void *dummy)
{
  if (ctx && ctx->impl && (flags & GRN_CTX_MORE)) {
    unsigned int size = GRN_BULK_VSIZE(ctx->impl->outbuf);
    grn_bulk_write(ctx, &ctx->impl->subbuf, (char *) &size, sizeof(unsigned int));
  }
}

void
grn_ctx_stream_out_func(grn_ctx *ctx, int flags, void *stream)
{
  if (ctx && ctx->impl) {
    grn_obj *buf = ctx->impl->outbuf;
    uint32_t size = GRN_BULK_VSIZE(buf);
    if (size) {
      if (fwrite(GRN_BULK_HEAD(buf), 1, size, (FILE *)stream)) {
        fputc('\n', (FILE *)stream);
        fflush((FILE *)stream);
      }
      GRN_BULK_REWIND(buf);
    }
  }
}

void
grn_ctx_recv_handler_set(grn_ctx *ctx, void (*func)(grn_ctx *, int, void *), void *func_arg)
{
  if (ctx && ctx->impl) {
    ctx->impl->output = func;
    ctx->impl->data.ptr = func_arg;
  }
}

grn_rc
grn_ctx_info_get(grn_ctx *ctx, grn_ctx_info *info)
{
  if (!ctx || !ctx->impl) { return GRN_INVALID_ARGUMENT; }
  if (ctx->impl->com) {
    info->fd = ctx->impl->com->fd;
    info->com_status = ctx->impl->com_status;
    info->outbuf = ctx->impl->outbuf;
    info->stat = ctx->stat;
  } else {
    info->fd = -1;
    info->com_status = 0;
    info->outbuf = ctx->impl->outbuf;
    info->stat = ctx->stat;
  }
  return GRN_SUCCESS;
}


grn_cell *
grn_get(const char *key)
{
  grn_cell *obj;
  if (!grn_gctx.impl || !grn_gctx.impl->symbols ||
      !grn_hash_add(&grn_gctx, grn_gctx.impl->symbols, key, strlen(key),
                    (void **) &obj, NULL)) {
    GRN_LOG(&grn_gctx, GRN_LOG_WARNING, "grn_get(%s) failed", key);
    return F;
  }
  if (!obj->header.impl_flags) {
    obj->header.impl_flags |= GRN_CELL_SYMBOL;
    obj->header.type = GRN_VOID;
  }
  return obj;
}

grn_cell *
grn_at(const char *key)
{
  grn_cell *obj;
  if (!grn_gctx.impl || grn_gctx.impl->symbols ||
      !grn_hash_get(&grn_gctx, grn_gctx.impl->symbols,
                   key, strlen(key), (void **) &obj)) {
    return F;
  }
  return obj;
}

grn_rc
grn_del(const char *key)
{
  if (!grn_gctx.impl || !grn_gctx.impl->symbols) {
    GRN_LOG(&grn_gctx, GRN_LOG_WARNING, "grn_del(%s) failed", key);
    return GRN_INVALID_ARGUMENT;
  }
  return grn_hash_delete(&grn_gctx, grn_gctx.impl->symbols, key, strlen(key), NULL);
}

typedef struct _grn_cache_entry grn_cache_entry;

typedef struct {
  grn_cache_entry *next;
  grn_cache_entry *prev;
  grn_hash *hash;
  grn_mutex mutex;
  uint32_t max_nentries;
} grn_cache;

struct _grn_cache_entry {
  grn_cache_entry *next;
  grn_cache_entry *prev;
  grn_obj *value;
  grn_timeval tv;
  grn_id id;
  uint32_t nref;
};

static grn_cache grn_gcache;

void
grn_cache_init(void)
{
  grn_gcache.next = (grn_cache_entry *) &grn_gcache;
  grn_gcache.prev = (grn_cache_entry *) &grn_gcache;
  grn_gcache.hash = grn_hash_create(&grn_gctx, NULL, GRN_TABLE_MAX_KEY_SIZE,
                                    sizeof(grn_cache_entry), GRN_OBJ_KEY_VAR_SIZE);
  MUTEX_INIT(grn_gcache.mutex);
  grn_gcache.max_nentries = 100;
}

uint32_t *
grn_cach_max_nentries(void)
{
  return &grn_gcache.max_nentries;
}

static void
grn_cache_expire_entry(grn_cache_entry *ce)
{
  if (!ce->nref) {
    ce->prev->next = ce->next;
    ce->next->prev = ce->prev;
    grn_obj_close(&grn_gctx, ce->value);
    grn_hash_delete_by_id(&grn_gctx, grn_gcache.hash, ce->id, NULL);
  }
}

grn_obj *
grn_cache_fetch(grn_ctx *ctx, const char *str, uint32_t str_len)
{
  grn_cache_entry *ce;
  grn_obj *obj = NULL;
  if (!ctx->impl || !ctx->impl->db) { return obj; }
  MUTEX_LOCK(grn_gcache.mutex);
  if (grn_hash_get(&grn_gctx, grn_gcache.hash, str, str_len, (void **)&ce)) {
    if (ce->tv.tv_sec <= grn_db_lastmod(ctx->impl->db)) {
      grn_cache_expire_entry(ce);
      goto exit;
    }
    ce->nref++;
    obj = ce->value;
    ce->prev->next = ce->next;
    ce->next->prev = ce->prev;
    {
      grn_cache_entry *ce0 = (grn_cache_entry *)&grn_gcache;
      ce->next = ce0->next;
      ce->prev = ce0;
      ce0->next->prev = ce;
      ce0->next = ce;
    }
  }
exit :
  MUTEX_UNLOCK(grn_gcache.mutex);
  return obj;
}

void
grn_cache_unref(const char *str, uint32_t str_len)
{
  grn_cache_entry *ce;
  grn_ctx *ctx = &grn_gctx;
  MUTEX_LOCK(grn_gcache.mutex);
  if (grn_hash_get(ctx, grn_gcache.hash, str, str_len, (void **)&ce)) {
    if (ce->nref) { ce->nref--; }
  }
  MUTEX_UNLOCK(grn_gcache.mutex);
}

void
grn_cache_update(grn_ctx *ctx, const char *str, uint32_t str_len, grn_obj *value)
{
  grn_id id;
  int added = 0;
  grn_cache_entry *ce;
  grn_rc rc = GRN_SUCCESS;
  grn_obj *old = NULL, *obj;
  if (!ctx->impl || !grn_gcache.max_nentries) { return; }
  if (!(obj = grn_obj_open(&grn_gctx, GRN_BULK, 0, GRN_DB_TEXT))) { return; }
  GRN_TEXT_PUT(&grn_gctx, obj, GRN_TEXT_VALUE(value), GRN_TEXT_LEN(value));
  MUTEX_LOCK(grn_gcache.mutex);
  if ((id = grn_hash_add(&grn_gctx, grn_gcache.hash, str, str_len, (void **)&ce, &added))) {
    if (!added) {
      if (ce->nref) {
        rc = GRN_RESOURCE_BUSY;
        goto exit;
      }
      old = ce->value;
      ce->prev->next = ce->next;
      ce->next->prev = ce->prev;
    }
    ce->id = id;
    ce->value = obj;
    ce->tv = ctx->impl->tv;
    ce->nref = 0;
    {
      grn_cache_entry *ce0 = (grn_cache_entry *)&grn_gcache;
      ce->next = ce0->next;
      ce->prev = ce0;
      ce0->next->prev = ce;
      ce0->next = ce;
    }
    if (GRN_HASH_SIZE(grn_gcache.hash) > grn_gcache.max_nentries) {
      grn_cache_expire_entry(grn_gcache.prev);
    }
  } else {
    rc = GRN_NO_MEMORY_AVAILABLE;
  }
exit :
  MUTEX_UNLOCK(grn_gcache.mutex);
  if (rc) { grn_obj_close(&grn_gctx, obj); }
  if (old) { grn_obj_close(&grn_gctx, old); }
}

void
grn_cache_expire(int32_t size)
{
  grn_cache_entry *ce0 = (grn_cache_entry *)&grn_gcache;
  MUTEX_LOCK(grn_gcache.mutex);
  while (ce0 != ce0->prev && size--) { grn_cache_expire_entry(ce0->prev); }
  MUTEX_UNLOCK(grn_gcache.mutex);
}

void
grn_cache_fin(void)
{
  grn_ctx *ctx = &grn_gctx;
  grn_cache_entry *vp;
  GRN_HASH_EACH(ctx, grn_gcache.hash, id, NULL, NULL, &vp, {
    grn_obj_close(ctx, vp->value);
  });
  grn_hash_close(ctx, grn_gcache.hash);
  MUTEX_FIN(grn_gcache.mutex);
}

/**** memory allocation ****/

#define ALIGN_SIZE (1<<3)
#define ALIGN_MASK (ALIGN_SIZE-1)
#define GRN_CTX_ALLOC_CLEAR 1

void *
grn_ctx_alloc(grn_ctx *ctx, size_t size, int flags,
              const char* file, int line, const char *func)
{
  void *res = NULL;
  if (!ctx) { return res; }
  if (!ctx->impl) {
    grn_ctx_impl_init(ctx);
    if (ERRP(ctx, GRN_ERROR)) { return res; }
  }
  CRITICAL_SECTION_ENTER(ctx->impl->lock);
  {
    int32_t i;
    int32_t *header;
    grn_io_mapinfo *mi;
    size = ((size + ALIGN_MASK) & ~ALIGN_MASK) + ALIGN_SIZE;
    if (size > GRN_CTX_SEGMENT_SIZE) {
      uint64_t npages = (size + (grn_pagesize - 1)) / grn_pagesize;
      if (npages >= (1LL<<32)) {
        MERR("too long request size=%zu", size);
        goto exit;
      }
      for (i = 0, mi = ctx->impl->segs;; i++, mi++) {
        if (i >= GRN_CTX_N_SEGMENTS) {
          MERR("all segments are full");
          goto exit;
        }
        if (!mi->map) { break; }
      }
      if (!grn_io_anon_map(ctx, mi, npages * grn_pagesize)) { goto exit; }
      //GRN_LOG(ctx, GRN_LOG_NOTICE, "map i=%d (%d)", i, npages * grn_pagesize);
      mi->nref = (uint32_t) npages;
      mi->count = GRN_CTX_SEGMENT_VLEN;
      ctx->impl->currseg = -1;
      header = mi->map;
      header[0] = i;
      header[1] = (int32_t) size;
    } else {
      i = ctx->impl->currseg;
      mi = &ctx->impl->segs[i];
      if (i < 0 || size + mi->nref > GRN_CTX_SEGMENT_SIZE) {
        for (i = 0, mi = ctx->impl->segs;; i++, mi++) {
          if (i >= GRN_CTX_N_SEGMENTS) {
            MERR("all segments are full");
            goto exit;
          }
          if (!mi->map) { break; }
        }
        if (!grn_io_anon_map(ctx, mi, GRN_CTX_SEGMENT_SIZE)) { goto exit; }
        //GRN_LOG(ctx, GRN_LOG_NOTICE, "map i=%d", i);
        mi->nref = 0;
        mi->count = GRN_CTX_SEGMENT_WORD;
        ctx->impl->currseg = i;
      }
      header = (int32_t *)((byte *)mi->map + mi->nref);
      mi->nref += size;
      mi->count++;
      header[0] = i;
      header[1] = (int32_t) size;
      if ((flags & GRN_CTX_ALLOC_CLEAR) && (mi->count & GRN_CTX_SEGMENT_DIRTY)) {
        memset(&header[2], 0, size);
      }
    }
    /*
    {
      char g = (ctx == &grn_gctx) ? 'g' : ' ';
      GRN_LOG(ctx, GRN_LOG_NOTICE, "+%c(%p) %s:%d(%s) (%d:%d)%p mi(%d:%d)", g, ctx, file, line, func, header[0], header[1], &header[2], mi->nref, (mi->count & GRN_CTX_SEGMENT_MASK));
    }
    */
    res = &header[2];
  }
exit :
  CRITICAL_SECTION_LEAVE(ctx->impl->lock);
  return res;
}

void *
grn_ctx_malloc(grn_ctx *ctx, size_t size,
              const char* file, int line, const char *func)
{
  return grn_ctx_alloc(ctx, size, 0, file, line, func);
}

void *
grn_ctx_calloc(grn_ctx *ctx, size_t size,
              const char* file, int line, const char *func)
{
  return grn_ctx_alloc(ctx, size, GRN_CTX_ALLOC_CLEAR, file, line, func);
}

void *
grn_ctx_realloc(grn_ctx *ctx, void *ptr, size_t size,
                const char* file, int line, const char *func)
{
  void *res = NULL;
  if (size) {
    /* todo : expand if possible */
    res = grn_ctx_alloc(ctx, size, 0, file, line, func);
    if (res && ptr) {
      int32_t *header = &((int32_t *)ptr)[-2];
      size_t size_ = header[1];
      memcpy(res, ptr, size_ > size ? size : size_);
      grn_ctx_free(ctx, ptr, file, line, func);
    }
  } else {
    grn_ctx_free(ctx, ptr, file, line, func);
  }
  return res;
}

char *
grn_ctx_strdup(grn_ctx *ctx, const char *s, const char* file, int line, const char *func)
{
  void *res = NULL;
  if (s) {
    size_t size = strlen(s) + 1;
    if ((res = grn_ctx_alloc(ctx, size, 0, file, line, func))) {
      memcpy(res, s, size);
    }
  }
  return res;
}

void
grn_ctx_free(grn_ctx *ctx, void *ptr,
             const char* file, int line, const char *func)
{
  if (!ctx) { return; }
  if (!ctx->impl) {
    ERR(GRN_INVALID_ARGUMENT,"ctx without impl passed.");
    return;
  }
  CRITICAL_SECTION_ENTER(ctx->impl->lock);
  if (ptr) {
    int32_t *header = &((int32_t *)ptr)[-2];

    if (header[0] >= GRN_CTX_N_SEGMENTS) {
      ERR(GRN_INVALID_ARGUMENT,"invalid ptr passed. ptr=%p seg=%zu", ptr, *header);
      goto exit;
    }
    /*
    {
      int32_t i = header[0];
      char c = 'X', g = (ctx == &grn_gctx) ? 'g' : ' ';
      grn_io_mapinfo *mi = &ctx->impl->segs[i];
      if (!(mi->count & GRN_CTX_SEGMENT_VLEN) &&
          mi->map <= (void *)header && (char *)header < ((char *)mi->map + GRN_CTX_SEGMENT_SIZE)) { c = '-'; }
      GRN_LOG(ctx, GRN_LOG_NOTICE, "%c%c(%p) %s:%d(%s) (%d:%d)%p mi(%d:%d)", c, g, ctx, file, line, func, header[0], header[1], &header[2], mi->nref, (mi->count & GRN_CTX_SEGMENT_MASK));
    }
    */
    {
      int32_t i = header[0];
      grn_io_mapinfo *mi = &ctx->impl->segs[i];
      if (mi->count & GRN_CTX_SEGMENT_VLEN) {
        if (mi->map != header) {
          ERR(GRN_INVALID_ARGUMENT,"invalid ptr passed.. ptr=%p seg=%d", ptr, i);
          goto exit;
        }
        //GRN_LOG(ctx, GRN_LOG_NOTICE, "umap i=%d (%d)", i, mi->nref * grn_pagesize);
        grn_io_anon_unmap(ctx, mi, mi->nref * grn_pagesize);
        mi->map = NULL;
      } else {
        if (!mi->map) {
          ERR(GRN_INVALID_ARGUMENT,"invalid ptr passed... ptr=%p seg=%d", ptr, i);
          goto exit;
        }
        mi->count--;
        if (!(mi->count & GRN_CTX_SEGMENT_MASK)) {
          //GRN_LOG(ctx, GRN_LOG_NOTICE, "umap i=%d", i);
          if (i == ctx->impl->currseg) {
            mi->count |= GRN_CTX_SEGMENT_DIRTY;
            mi->nref = 0;
          } else {
            grn_io_anon_unmap(ctx, mi, GRN_CTX_SEGMENT_SIZE);
            mi->map = NULL;
          }
        }
      }
    }
  }
exit :
  CRITICAL_SECTION_LEAVE(ctx->impl->lock);
}

#define DB_P(s) ((s) && (s)->header.type == GRN_DB)

grn_rc
grn_ctx_use(grn_ctx *ctx, grn_obj *db)
{
  GRN_API_ENTER;
  if (db && !DB_P(db)) {
    ctx->rc = GRN_INVALID_ARGUMENT;
  } else {
    if (!ctx->impl) { grn_ctx_impl_init(ctx); }
    if (!ctx->rc) {
      ctx->impl->db = db;
      if (db) {
        grn_obj buf;
        if (ctx->impl->symbols) { grn_ql_def_db_funcs(ctx); }
        GRN_TEXT_INIT(&buf, 0);
        grn_obj_get_info(ctx, db, GRN_INFO_ENCODING, &buf);
        ctx->encoding = *(grn_encoding *)GRN_BULK_HEAD(&buf);
        grn_obj_close(ctx, &buf);
      }
    }
  }
  GRN_API_RETURN(ctx->rc);
}

void *
grn_ctx_alloc_lifo(grn_ctx *ctx, size_t size,
                   const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  if (!ctx->impl) {
    grn_ctx_impl_init(ctx);
    if (ERRP(ctx, GRN_ERROR)) { return NULL; }
  }
  {
    int32_t i = ctx->impl->lifoseg;
    grn_io_mapinfo *mi = &ctx->impl->segs[i];
    if (size > GRN_CTX_SEGMENT_SIZE) {
      uint64_t npages = (size + (grn_pagesize - 1)) / grn_pagesize;
      if (npages >= (1LL<<32)) {
        MERR("too long request size=%zu", size);
        return NULL;
      }
      for (;;) {
        if (++i >= GRN_CTX_N_SEGMENTS) {
          MERR("all segments are full");
          return NULL;
        }
        mi++;
        if (!mi->map) { break; }
      }
      if (!grn_io_anon_map(ctx, mi, npages * grn_pagesize)) { return NULL; }
      mi->nref = (uint32_t) npages;
      mi->count = GRN_CTX_SEGMENT_VLEN|GRN_CTX_SEGMENT_LIFO;
      ctx->impl->lifoseg = i;
      return mi->map;
    } else {
      size = (size + ALIGN_MASK) & ~ALIGN_MASK;
      if (i < 0 || (mi->count & GRN_CTX_SEGMENT_VLEN) || size + mi->nref > GRN_CTX_SEGMENT_SIZE) {
        for (;;) {
          if (++i >= GRN_CTX_N_SEGMENTS) {
            MERR("all segments are full");
            return NULL;
          }
          if (!(++mi)->map) { break; }
        }
        if (!grn_io_anon_map(ctx, mi, GRN_CTX_SEGMENT_SIZE)) { return NULL; }
        mi->nref = 0;
        mi->count = GRN_CTX_SEGMENT_WORD|GRN_CTX_SEGMENT_LIFO;
        ctx->impl->lifoseg = i;
      }
      {
        uint32_t u = mi->nref;
        mi->nref += size;
        return (byte *)mi->map + u;
      }
    }
  }
}

void
grn_ctx_free_lifo(grn_ctx *ctx, void *ptr,
                  const char* file, int line, const char *func)
{
  if (!ctx) { return; }
  if (!ctx->impl) {
    ERR(GRN_INVALID_ARGUMENT,"ctx without impl passed.");
    return;
  }
  {
    int32_t i = ctx->impl->lifoseg, done = 0;
    grn_io_mapinfo *mi = &ctx->impl->segs[i];
    if (i < 0) {
      ERR(GRN_INVALID_ARGUMENT, "lifo buffer is void");
      return;
    }
    for (; i >= 0; i--, mi--) {
      if (!(mi->count & GRN_CTX_SEGMENT_LIFO)) { continue; }
      if (done) { break; }
      if (mi->count & GRN_CTX_SEGMENT_VLEN) {
        if (mi->map == ptr) { done = 1; }
        grn_io_anon_unmap(ctx, mi, mi->nref * grn_pagesize);
        mi->map = NULL;
      } else {
        if (mi->map == ptr) {
          done = 1;
        } else {
          if (mi->map < ptr && ptr < (void *)((byte*)mi->map + mi->nref)) {
            mi->nref = (uint32_t) ((uintptr_t)ptr - (uintptr_t)mi->map);
            break;
          }
        }
        grn_io_anon_unmap(ctx, mi, GRN_CTX_SEGMENT_SIZE);
        mi->map = NULL;
      }
    }
    ctx->impl->lifoseg = i;
  }
}

#if USE_DYNAMIC_MALLOC_CHANGE
grn_malloc_func
grn_ctx_get_malloc(grn_ctx *ctx)
{
  if (!ctx || !ctx->impl) { return NULL; }
  return ctx->impl->malloc_func;
}

void
grn_ctx_set_malloc(grn_ctx *ctx, grn_malloc_func malloc_func)
{
  if (!ctx || !ctx->impl) { return; }
  ctx->impl->malloc_func = malloc_func;
}

grn_calloc_func
grn_ctx_get_calloc(grn_ctx *ctx)
{
  if (!ctx || !ctx->impl) { return NULL; }
  return ctx->impl->calloc_func;
}

void
grn_ctx_set_calloc(grn_ctx *ctx, grn_calloc_func calloc_func)
{
  if (!ctx || !ctx->impl) { return; }
  ctx->impl->calloc_func = calloc_func;
}

grn_realloc_func
grn_ctx_get_realloc(grn_ctx *ctx)
{
  if (!ctx || !ctx->impl) { return NULL; }
  return ctx->impl->realloc_func;
}

void
grn_ctx_set_realloc(grn_ctx *ctx, grn_realloc_func realloc_func)
{
  if (!ctx || !ctx->impl) { return; }
  ctx->impl->realloc_func = realloc_func;
}

grn_strdup_func
grn_ctx_get_strdup(grn_ctx *ctx)
{
  if (!ctx || !ctx->impl) { return NULL; }
  return ctx->impl->strdup_func;
}

void
grn_ctx_set_strdup(grn_ctx *ctx, grn_strdup_func strdup_func)
{
  if (!ctx || !ctx->impl) { return; }
  ctx->impl->strdup_func = strdup_func;
}

void *
grn_malloc(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->malloc_func) {
    return ctx->impl->malloc_func(ctx, size, file, line, func);
  } else {
    return grn_malloc_default(ctx, size, file, line, func);
  }
}

void *
grn_calloc(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->calloc_func) {
    return ctx->impl->calloc_func(ctx, size, file, line, func);
  } else {
    return grn_calloc_default(ctx, size, file, line, func);
  }
}

void *
grn_realloc(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->realloc_func) {
    return ctx->impl->realloc_func(ctx, ptr, size, file, line, func);
  } else {
    return grn_realloc_default(ctx, ptr, size, file, line, func);
  }
}

char *
grn_strdup(grn_ctx *ctx, const char *string, const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->strdup_func) {
    return ctx->impl->strdup_func(ctx, string, file, line, func);
  } else {
    return grn_strdup_default(ctx, string, file, line, func);
  }
}
#endif

void *
grn_malloc_default(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  {
    void *res = malloc(size);
    if (res) {
      GRN_ADD_ALLOC_COUNT(1);
    } else {
      if (!(res = malloc(size))) {
        MERR("malloc fail (%d)=%p (%s:%d) <%d>", size, res, file, line, alloc_count);
      } else {
        GRN_ADD_ALLOC_COUNT(1);
      }
    }
    return res;
  }
}

void *
grn_calloc_default(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  {
    void *res = calloc(size, 1);
    if (res) {
      GRN_ADD_ALLOC_COUNT(1);
    } else {
      if (!(res = calloc(size, 1))) {
        MERR("calloc fail (%d)=%p (%s:%d) <%d>", size, res, file, line, alloc_count);
      } else {
        GRN_ADD_ALLOC_COUNT(1);
      }
    }
    return res;
  }
}

void
grn_free_default(grn_ctx *ctx, void *ptr, const char* file, int line, const char *func)
{
  if (!ctx) { return; }
  {
    free(ptr);
    if (ptr) {
      GRN_ADD_ALLOC_COUNT(-1);
    } else {
      GRN_LOG(ctx, GRN_LOG_ALERT, "free fail (%p) (%s:%d) <%d>", ptr, file, line, alloc_count);
    }
  }
}

void *
grn_realloc_default(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line, const char *func)
{
  void *res;
  if (!ctx) { return NULL; }
  if (size) {
    if (!(res = realloc(ptr, size))) {
      if (!(res = realloc(ptr, size))) {
        MERR("realloc fail (%p,%zu)=%p (%s:%d) <%d>", ptr, size, res, file, line, alloc_count);
        return NULL;
      }
    }
    if (!ptr) { GRN_ADD_ALLOC_COUNT(1); }
  } else {
    if (!ptr) { return NULL; }
    GRN_ADD_ALLOC_COUNT(-1);
#if defined __FreeBSD__
    free(ptr);
    return NULL;
#else /* __FreeBSD__ */
    res = realloc(ptr, size);
    if (res) {
      GRN_LOG(ctx, GRN_LOG_ALERT, "realloc(%p,%zu)=%p (%s:%d) <%d>", ptr, size, res, file, line, alloc_count);
      // grn_free(ctx, res, file, line);
    }
#endif /* __FreeBSD__ */
  }
  return res;
}

int
grn_alloc_count(void)
{
  return alloc_count;
}

char *
grn_strdup_default(grn_ctx *ctx, const char *s, const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  {
    char *res = strdup(s);
    if (res) {
      GRN_ADD_ALLOC_COUNT(1);
    } else {
      if (!(res = strdup(s))) {
        MERR("strdup(%p)=%p (%s:%d) <%d>", s, res, file, line, alloc_count);
      }
    }
    return res;
  }
}

#ifdef USE_FAIL_MALLOC
int
grn_fail_malloc_check(size_t size, const char *file, int line, const char *func)
{
  if ((grn_fmalloc_file && strcmp(file, grn_fmalloc_file)) ||
      (grn_fmalloc_line && line != grn_fmalloc_line) ||
      (grn_fmalloc_func && strcmp(func, grn_fmalloc_func))) {
    return 1;
  }
  if (grn_fmalloc_prob && grn_fmalloc_prob >= rand()) {
    return 0;
  }
  return 1;
}

void *
grn_malloc_fail(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(size, file, line, func)) {
    return grn_malloc_default(ctx, size, file, line, func);
  } else {
    MERR("fail_malloc (%d) (%s:%d@%s) <%d>", size, file, line, func, alloc_count);
    return NULL;
  }
}

void *
grn_calloc_fail(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(size, file, line, func)) {
    return grn_calloc_default(ctx, size, file, line, func);
  } else {
    MERR("fail_calloc (%d) (%s:%d@%s) <%d>", size, file, line, func, alloc_count);
    return NULL;
  }
}

void *
grn_realloc_fail(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line,
                 const char *func)
{
  if (grn_fail_malloc_check(size, file, line, func)) {
    return grn_realloc_default(ctx, ptr, size, file, line, func);
  } else {
    MERR("fail_realloc (%p,%zu) (%s:%d@%s) <%d>", ptr, size, file, line, func, alloc_count);
    return NULL;
  }
}

char *
grn_strdup_fail(grn_ctx *ctx, const char *s, const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(strlen(s), file, line, func)) {
    return grn_strdup_default(ctx, s, file, line, func);
  } else {
    MERR("fail_strdup(%p) (%s:%d@%s) <%d>", s, file, line, func, alloc_count);
    return NULL;
  }
}
#endif /* USE_FAIL_MALLOC */

grn_cell *
grn_cell_new(grn_ctx *ctx)
{
  grn_cell *o = NULL;
  if (ctx && ctx->impl) {
    grn_array_add(ctx, ctx->impl->objects, (void **)&o);
    if (o) {
      o->header.impl_flags = 0;
      ctx->impl->n_entries++;
    }
  }
  return o;
}

grn_cell *
grn_cell_cons(grn_ctx *ctx, grn_cell *a, grn_cell *b)
{
  if (!ctx) { return NULL; }
  {
    grn_cell *o;
    GRN_CELL_NEW(ctx, o);
    if (o) {
      o->header.type = GRN_CELL_LIST;
      o->header.impl_flags = 0;
      o->u.l.car = a;
      o->u.l.cdr = b;
    }
    return o;
  }
}

grn_cell *
grn_cell_alloc(grn_ctx *ctx, uint32_t size)
{
  if (!ctx) { return NULL; }
  {
    void *value = GRN_MALLOC(size + 1);
    if (value) {
      grn_cell *o = grn_cell_new(ctx);
      if (!ERRP(ctx, GRN_ERROR)) {
        o->header.impl_flags = GRN_OBJ_ALLOCATED;
        o->header.type = GRN_CELL_STR;
        o->u.b.size = size;
        o->u.b.value = value;
        return o;
      }
      GRN_FREE(value);
    } else {
      MERR("malloc(%d) failed", size + 1);
    }
    return NULL;
  }
}

void
grn_cell_clear(grn_ctx *ctx, grn_cell *o)
{
  if (!ctx || !ctx->impl) { return; }
  if (o->header.impl_flags & GRN_OBJ_ALLOCATED) {
    switch (o->header.type) {
    case GRN_SNIP :
      if (o->u.p.value) { grn_snip_close(ctx, (grn_snip *)o->u.p.value); }
      break;
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_NO_KEY :
      grn_obj_close(ctx, grn_ctx_at(ctx, o->u.o.id));
      break;
    case GRN_CELL_STR :
      if (o->u.b.value) {
        GRN_FREE(o->u.b.value);
      }
      break;
    case GRN_QUERY :
      if (o->u.p.value) { grn_query_close(ctx, (grn_query *)o->u.p.value); }
      break;
    case GRN_UVECTOR :
    case GRN_VECTOR :
      if (o->u.p.value) { grn_obj_close(ctx, o->u.p.value); }
      break;
    case GRN_PATSNIP :
      grn_obj_patsnip_spec_close(ctx, (patsnip_spec *)o->u.p.value);
      break;
    default :
      GRN_LOG(ctx, GRN_LOG_WARNING, "obj_clear: invalid type(%x)", o->header.type);
      break;
    }
    o->header.impl_flags &= ~GRN_OBJ_ALLOCATED;
  }
}

/* don't handle error inside logger functions */

void
grn_ctx_log(grn_ctx *ctx, char *fmt, ...)
{
  va_list argp;
  va_start(argp, fmt);
  vsnprintf(ctx->errbuf, GRN_CTX_MSGSIZE - 1, fmt, argp);
  va_end(argp);
  ctx->errbuf[GRN_CTX_MSGSIZE - 1] = '\0';
}

void
grn_logger_fin(void)
{
  CRITICAL_SECTION_ENTER(grn_logger_lock);
  if (default_logger_fp) {
    fclose(default_logger_fp);
    default_logger_fp = NULL;
  }
  CRITICAL_SECTION_LEAVE(grn_logger_lock);
}

grn_rc
grn_logger_info_set(grn_ctx *ctx, const grn_logger_info *info)
{
  if (info) {
    grn_logger = info;
  } else {
    grn_logger = &default_logger;
  }
  return GRN_SUCCESS;
}

int
grn_logger_pass(grn_ctx *ctx, grn_log_level level)
{
  return level <= grn_logger->max_level;
}

#define TBUFSIZE GRN_TIMEVAL_STR_SIZE
#define MBUFSIZE 0x1000
#define LBUFSIZE 0x400

void
grn_logger_put(grn_ctx *ctx, grn_log_level level,
               const char *file, int line, const char *func, const char *fmt, ...)
{
  if (level <= grn_logger->max_level) {
    char tbuf[TBUFSIZE];
    char mbuf[MBUFSIZE];
    char lbuf[LBUFSIZE];
    tbuf[0] = '\0';
    if (grn_logger->flags & GRN_LOG_TIME) {
      grn_timeval tv;
      grn_timeval_now(ctx, &tv);
      grn_timeval2str(ctx, &tv, tbuf);
    }
    if (grn_logger->flags & GRN_LOG_MESSAGE) {
      va_list argp;
      va_start(argp, fmt);
      vsnprintf(mbuf, MBUFSIZE - 1, fmt, argp);
      va_end(argp);
      mbuf[MBUFSIZE - 1] = '\0';
    } else {
      mbuf[0] = '\0';
    }
    if (grn_logger->flags & GRN_LOG_LOCATION) {
      snprintf(lbuf, LBUFSIZE - 1, "%d %s:%d %s()", getpid(), file, line, func);
      lbuf[LBUFSIZE - 1] = '\0';
    } else {
      lbuf[0] = '\0';
    }
    if (grn_logger->func) {
      grn_logger->func(level, tbuf, "", mbuf, lbuf, grn_logger->func_arg);
    } else {
      default_logger_func(level, tbuf, "", mbuf, lbuf, grn_logger->func_arg);
    }
  }
}

void
grn_assert(grn_ctx *ctx, int cond, const char* file, int line, const char* func)
{
  if (!cond) {
    GRN_LOG(ctx, GRN_LOG_WARNING, "ASSERT fail on %s %s:%d", func, file, line);
  }
}

const char *
grn_get_version(void)
{
  return GROONGA_VERSION;
}

const char *
grn_get_package(void)
{
  return PACKAGE;
}

#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
static int segv_received = 0;
static void
segv_handler(int signal_number, siginfo_t *info, void *context)
{
  grn_ctx *ctx = &grn_gctx;

  if (segv_received) {
    GRN_LOG(ctx, GRN_LOG_CRIT, "SEGV received in SEGV handler.");
    exit(EXIT_FAILURE);
  }
  segv_received = 1;

  GRN_LOG(ctx, GRN_LOG_CRIT, "-- CRASHED!!! --");
#ifdef HAVE_EXECINFO_H
#  define N_TRACE_LEVEL 1024
  {
    static void *trace[N_TRACE_LEVEL];
    int n = backtrace(trace, N_TRACE_LEVEL);
    char **symbols = backtrace_symbols(trace, n);
    int i;

    if (symbols) {
      for (i = 0; i < n; i++) {
        GRN_LOG(ctx, GRN_LOG_CRIT, "%s", symbols[i]);
      }
      free(symbols);
    }
  }
#else
  GRN_LOG(ctx, GRN_LOG_CRIT, "backtrace() isn't available.");
#endif
  GRN_LOG(ctx, GRN_LOG_CRIT, "----------------");
  abort();
}
#endif /* defined(HAVE_SIGNAL_H) && !defined(WIN32) */

grn_rc
grn_set_segv_handler(void)
{
  grn_rc rc = GRN_SUCCESS;
#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
  grn_ctx *ctx = &grn_gctx;
  struct sigaction action;

  sigemptyset(&action.sa_mask);
  action.sa_sigaction = segv_handler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;

  if (sigaction(SIGSEGV, &action, NULL)) {
    SERR("failed to set SIGSEGV action");
    rc = ctx->rc;
  };
#endif
  return rc;
}

#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
static struct sigaction old_int_handler;
static void
int_handler(int signal_number, siginfo_t *info, void *context)
{
  grn_gctx.stat = GRN_CTX_QUIT;
  sigaction(signal_number, &old_int_handler, NULL);
}

static struct sigaction old_term_handler;
static void
term_handler(int signal_number, siginfo_t *info, void *context)
{
  grn_gctx.stat = GRN_CTX_QUIT;
  sigaction(signal_number, &old_term_handler, NULL);
}
#endif /* defined(HAVE_SIGNAL_H) && !defined(WIN32) */

grn_rc
grn_set_int_handler(void)
{
  grn_rc rc = GRN_SUCCESS;
#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
  grn_ctx *ctx = &grn_gctx;
  struct sigaction action;

  sigemptyset(&action.sa_mask);
  action.sa_sigaction = int_handler;
  action.sa_flags = SA_SIGINFO;

  if (sigaction(SIGINT, &action, &old_int_handler)) {
    SERR("failed to set SIGINT action");
    rc = ctx->rc;
  }
#endif
  return rc;
}

grn_rc
grn_set_term_handler(void)
{
  grn_rc rc = GRN_SUCCESS;
#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
  grn_ctx *ctx = &grn_gctx;
  struct sigaction action;

  sigemptyset(&action.sa_mask);
  action.sa_sigaction = term_handler;
  action.sa_flags = SA_SIGINFO;

  if (sigaction(SIGINT, &action, &old_term_handler)) {
    SERR("failed to set SIGTERM action");
    rc = ctx->rc;
  }
#endif
  return rc;
}
