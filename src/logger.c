/*
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "logger.h"

#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h> // for isprint()
#include <inttypes.h>
#include <pthread.h>

#include <event2/event.h>

#include <libavutil/log.h>

#include "owntone_config.h"
#include "misc.h"

#define LOGGER_REPEAT_MAX 10

/* Forward declaration: LOGGER_CHECK_ERR (below) expands to a call to this at
 * every use site, several of which now come before its definition further
 * down the file. */
static void vlogger_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* We need our own check to avoid nested locking or recursive calls */
#define LOGGER_CHECK_ERR(f) \
  do { int lerr; lerr = f; if (lerr != 0) { \
      vlogger_fatal("%s failed at line %d, err %d (%s)\n", #f, __LINE__, \
                    lerr, strerror(lerr)); \
      abort(); \
    } } while(0)

static pthread_mutex_t logger_lck;
static int logger_initialized;
static int logdomains;
static int threshold;
static int console = 1;
static uint32_t logger_repeat_counter;
static uint32_t logger_last_hash;
static char *logfilename;
static FILE *logfile;
static char *labels[] = { "config", "-", "-", "httpd", "-", "main", "mdns", "misc", "-", "-", "xcode", "event", "-", "-", "ffmpeg", "-", "player", "raop", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "web", "airplay", "-" };
static char *severities[] = { "FATAL", "LOG", "WARN", "INFO", "DEBUG", "SPAM" };
static char *format_labels[] = { "default", "logfmt" };

enum format {
  L_FMT_DEFAULT = 0,
  L_FMT_LOGFMT = 1,
};
static enum format format = L_FMT_DEFAULT;

/* ---------------------------- Async log writer ----------------------------
 *
 * DPRINTF() et al render a complete line (timestamped at the call site) and
 * hand it to a bounded queue; a dedicated "logwriter" thread is the only
 * thread that ever does file/console I/O, so a caller -- in particular the
 * player thread -- never blocks on a slow log sink (SD-card write-back
 * under memory pressure, mainly).
 *
 * The queue is bounded by entry count (256), not bytes: a regular log line
 * is well under 200 bytes, but DHEXDUMP() renders as one single, much
 * larger entry, so a fixed per-slot byte size would either truncate long
 * entries or have to be sized for the worst case and waste memory the rest
 * of the time. Each entry is heap-allocated at exactly the rendered line's
 * length and freed by the writer once written. When the queue is full,
 * entries are dropped and counted; the writer reports the drop count once
 * it catches up.
 *
 * The writer thread is started explicitly, from main() after daemonize()
 * (see logger_async_start()) -- logger_init() itself runs twice before the
 * fork (once with no settings, again after the config file is read), and a
 * thread started that early would not survive the fork. Until the writer
 * is started, and if it fails to start, every line falls back to the
 * synchronous path logging has always used (see logger_dispatch()).
 *
 * logfile (the FILE*) has a single owner once the writer is running: only
 * the writer thread opens, writes, reopens (SIGHUP/logrotate) or closes it.
 * Producers never touch it directly, which is what lets logger_dispatch()
 * enqueue without ever holding a lock across I/O.
 */

#define LOGGER_QUEUE_CAPACITY 256
#define LOGGER_STOP_TIMEOUT_MS 1500

struct logger_queue_entry
{
  char *line;
  size_t len;
};

static struct logger_queue_entry logger_queue[LOGGER_QUEUE_CAPACITY];
static int logger_queue_head;   /* next slot to fill */
static int logger_queue_tail;   /* next slot to drain */
static int logger_queue_count;
static uint64_t logger_queue_dropped;
static int logger_reopen_pending;
static int logger_writer_stop;

/* Guards the queue bookkeeping above, plus logger_reopen_pending and
 * logger_writer_stop. Never held across I/O -- the writer thread always
 * unlocks before it does any fwrite()/fopen()/fclose(). */
static pthread_mutex_t logger_queue_lck;
static pthread_cond_t logger_queue_cond;

static pthread_t logger_writer_tid;
/* Read/written with __atomic builtins (plain int, lock-free everywhere we
 * build) rather than under logger_queue_lck, so logger_dispatch() can check
 * it on every single log call without taking a second lock on top of
 * logger_lck, which the caller already holds by that point. */
static int logger_writer_running;

static void
logger_write_line(const char *line, size_t len)
{
  if (logfile)
    fwrite(line, 1, len, logfile);
  if (console)
    fwrite(line, 1, len, stderr);
}

/* Render fmt/args into a freshly malloc'd, exactly-sized buffer. Returns
 * NULL (and leaves the caller to just drop the line) on a formatting error
 * or OOM -- there is no sane synchronous fallback for either, and the log
 * itself must not be what brings the process down. */
static char *
logger_render(size_t *out_len, const char *fmt, va_list args)
{
  va_list ap;
  int needed;
  char *buf;

  va_copy(ap, args);
  needed = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  if (needed < 0)
    return NULL;

  buf = malloc((size_t)needed + 1);
  if (!buf)
    return NULL;

  va_copy(ap, args);
  vsnprintf(buf, (size_t)needed + 1, fmt, ap);
  va_end(ap);

  if (out_len)
    *out_len = (size_t)needed;

  return buf;
}

/* Variadic convenience wrapper around logger_render() */
static char *
logger_strf(size_t *out_len, const char *fmt, ...)
{
  va_list ap;
  char *buf;

  va_start(ap, fmt);
  buf = logger_render(out_len, fmt, ap);
  va_end(ap);

  return buf;
}

static int
set_logdomains(char *domains)
{
  char *ptr;
  char *d;
  int i;

  logdomains = 0;

  while ((d = strtok_r(domains, " ,", &ptr)))
    {
      domains = NULL;

      for (i = 0; i < N_LOGDOMAINS; i++)
	{
	  if (strcmp(d, labels[i]) == 0)
	    {
	      logdomains |= (1 << i);
	      break;
	    }
	}

      if (i == N_LOGDOMAINS)
	{
	  fprintf(stderr, "Error: unknown log domain '%s'\n", d);
	  return -1;
	}
    }

  return 0;
}

static int
format_code_get(const char *label)
{
  int i;

  if (!label)
    return 0;

  for (i = 0; i < ARRAY_SIZE(format_labels); i++)
    {
      if (strcmp(label, format_labels[i]) == 0)
	return i;
    }

  return 0;
}

static int
repeat_count(const char *fmt)
{
  uint32_t hash;

  hash = djb_hash(fmt, strlen(fmt));

  if (hash == logger_last_hash)
    logger_repeat_counter++;
  else
    logger_repeat_counter = 0;

  logger_last_hash = hash;

  return logger_repeat_counter;
}

/* Push one already-rendered line onto the queue. Takes logger_queue_lck only
 * long enough to link the entry in or bump the drop counter -- no I/O here,
 * so this is safe to call while the caller still holds logger_lck (or,
 * transitively, any lock a DPRINTF() call site itself might hold). On drop,
 * frees the line itself since nothing else owns it. */
static void
logger_enqueue(char *line, size_t len)
{
  LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_queue_lck));

  if (logger_queue_count >= LOGGER_QUEUE_CAPACITY)
    {
      logger_queue_dropped++;
      LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_queue_lck));
      free(line);
      return;
    }

  logger_queue[logger_queue_head].line = line;
  logger_queue[logger_queue_head].len = len;
  logger_queue_head = (logger_queue_head + 1) % LOGGER_QUEUE_CAPACITY;
  logger_queue_count++;

  LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_queue_lck));

  LOGGER_CHECK_ERR(pthread_cond_signal(&logger_queue_cond));
}

/* Hand a fully rendered line to the writer thread, or -- if it isn't
 * running (not started yet, failed to start, or already stopped) -- write
 * it synchronously right here, exactly like every line was written before
 * the writer thread existed (one fwrite() per output, one fflush()). */
static void
logger_dispatch(char *line, size_t len)
{
  if (__atomic_load_n(&logger_writer_running, __ATOMIC_ACQUIRE))
    {
      logger_enqueue(line, len);
      return;
    }

  logger_write_line(line, len);
  if (logfile)
    fflush(logfile);
  free(line);
}

/* Drain whatever is currently queued, writing each line synchronously on
 * the calling thread, then one fflush(). Used at shutdown, once the writer
 * is confirmed stopped, and from the fatal-error path (see vlogger_fatal()),
 * where the writer thread itself may still be alive and running.
 *
 * Uses pthread_mutex_trylock() rather than a blocking lock (LOGGER_CHECK_ERR
 * or otherwise): this can be reached from inside a failed LOGGER_CHECK_ERR
 * on logger_queue_lck itself (LOGGER_CHECK_ERR -> vlogger_fatal() -> here),
 * in which case the calling thread already holds the lock. logger_queue_lck
 * is a PTHREAD_MUTEX_ERRORCHECK mutex (see mutex_init() in misc.c), so on
 * glibc, trylock() tells the three cases apart without ever blocking:
 *   - 0: acquired normally -- drain, then unlock.
 *   - EDEADLK: already held by this very thread -- drain without taking or
 *     releasing the lock (a second, blocking lock attempt here would either
 *     report EDEADLK right back into another LOGGER_CHECK_ERR/abort(), or,
 *     without ERRORCHECK, genuinely deadlock). This is safe because every
 *     LOGGER_CHECK_ERR-wrapped logger_queue_lck operation is itself just the
 *     lock, unlock or cond_wait call -- none of them sit in the middle of a
 *     queue mutation -- so whatever queue mutation this thread was doing
 *     under the lock had either already finished, or not yet started, at
 *     the point the wrapped call failed and landed us here. The queue is
 *     never torn when we walk it unlocked below.
 *   - EBUSY: held by another thread (normally the writer, doing genuine
 *     work) -- skip the drain rather than walk the queue unlocked.
 */
static void
logger_drain_sync(void)
{
  struct logger_queue_entry batch[LOGGER_QUEUE_CAPACITY];
  int n = 0;
  int i;
  int ret;
  int self_owned;

  ret = pthread_mutex_trylock(&logger_queue_lck);
  if (ret == EBUSY)
    return;

  self_owned = (ret == EDEADLK);
  if (ret != 0 && !self_owned)
    return; /* unexpected error -- nothing safe to do but leave the queue alone */

  while (logger_queue_count > 0 && n < LOGGER_QUEUE_CAPACITY)
    {
      batch[n++] = logger_queue[logger_queue_tail];
      logger_queue_tail = (logger_queue_tail + 1) % LOGGER_QUEUE_CAPACITY;
      logger_queue_count--;
    }

  if (!self_owned)
    pthread_mutex_unlock(&logger_queue_lck);

  for (i = 0; i < n; i++)
    {
      logger_write_line(batch[i].line, batch[i].len);
      free(batch[i].line);
    }

  if (n > 0 && logfile)
    fflush(logfile);
}

static void
logger_write_with_label(int severity, int domain, const char *content)
{
  char stamp[32];
  char thread_nametid[32];
  time_t t;
  struct tm timebuf;
  char logfmt_msg[1024];
  char *line;
  size_t len;
  int ret;

  thread_getnametid(thread_nametid, sizeof(thread_nametid));
  t = time(NULL);

  if (format == L_FMT_LOGFMT)
    {
      ret = strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S%z", localtime_r(&t, &timebuf));
      if (ret == 0)
	stamp[0] = '\0';

      strncpy(logfmt_msg, content, sizeof(logfmt_msg));
      logfmt_msg[sizeof(logfmt_msg) - 1] = '\0';
      safe_snreplace(logfmt_msg, sizeof(logfmt_msg), "\n", " ");
      safe_snreplace(logfmt_msg, sizeof(logfmt_msg), "\"", "\\\"");

      line = logger_strf(&len, "time=%s level=%s thread=\"%s\" component=%s msg=\"%s\"\n", stamp, severities[severity], thread_nametid,
          labels[domain], logfmt_msg);
    }
  else
    {
      ret = strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime_r(&t, &timebuf));
      if (ret == 0)
	stamp[0] = '\0';

      line = logger_strf(&len, "[%s] [%5s] [%16s] %8s: %s", stamp, severities[severity], thread_nametid, labels[domain], content);
    }

  if (!line)
    return; /* OOM or formatting error rendering the label -- drop the line rather than crash */

  logger_dispatch(line, len);
}

/* Same as logger_write_with_label(), but always writes and flushes the line
 * synchronously on the calling thread, bypassing the queue regardless of
 * whether the writer thread is running. Used only by vlogger_fatal(): the
 * writer may still be alive and writing its own lines concurrently (they
 * can interleave with this one), but abort() follows right behind this
 * call, so the fatal line itself has to be on disk before we get there
 * rather than left queued for a writer that might not run again. */
static void
logger_write_with_label_sync(int severity, int domain, const char *content)
{
  char stamp[32];
  char thread_nametid[32];
  time_t t;
  struct tm timebuf;
  char *line;
  size_t len;
  int ret;

  thread_getnametid(thread_nametid, sizeof(thread_nametid));
  t = time(NULL);
  ret = strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime_r(&t, &timebuf));
  if (ret == 0)
    stamp[0] = '\0';

  line = logger_strf(&len, "[%s] [%5s] [%16s] %8s: %s", stamp, severities[severity], thread_nametid, labels[domain], content);
  if (!line)
    return;

  logger_write_line(line, len);
  if (logfile)
    fflush(logfile);

  free(line);
}

static void
vlogger_writer(int severity, int domain, const char *fmt, va_list args, int sync)
{
  va_list ap;
  char content[2048];
  int ret;

  va_copy(ap, args);
  ret = vsnprintf(content, sizeof(content), fmt, ap);
  if (ret < 0)
    strcpy(content, "(LOGGING SKIPPED - error printing log message)\n");
  else if (ret >= sizeof(content))
    strcpy(content + sizeof(content) - 8, "...\n");
  va_end(ap);

  ret = repeat_count(content);
  if (ret == LOGGER_REPEAT_MAX)
    strcpy(content, "(LOGGING SKIPPED - above log message is repeating)\n");
  else if (ret > LOGGER_REPEAT_MAX)
    return;

  if (sync)
    logger_write_with_label_sync(severity, domain, content);
  else
    logger_write_with_label(severity, domain, content);
}

static void
vlogger_fatal(const char *fmt, ...)
{
  va_list ap;

  /* abort() follows immediately after this (see LOGGER_CHECK_ERR). The
   * writer thread is not asked to stop here -- that's optional and this
   * doesn't wait for it either way -- it may still be alive and mid-batch,
   * so this is a best-effort grab of whatever is queued right now, written
   * out on this thread too in case the writer never gets another turn
   * before abort(). The two can interleave -- the writer's own writes and
   * this drain both do plain fwrite()s of complete, already-rendered lines,
   * so at worst lines land out of order, never corrupted. */
  logger_drain_sync();

  va_start(ap, fmt);
  vlogger_writer(E_FATAL, L_MISC, fmt, ap, 1);
  va_end(ap);
}

static void
vlogger(int severity, int domain, const char *fmt, va_list args)
{

  if(! logger_initialized)
    {
      /* lock not initialized, use stderr */
      vlogger_writer(severity, domain, fmt, args, 0);
      return;
    }

  if (!((1 << domain) & logdomains) || (severity > threshold))
    return;

  LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_lck));

  if (!logfile && !console)
    {
      LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_lck));
      return;
    }

  vlogger_writer(severity, domain, fmt, args, 0);

  LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_lck));
}

/* Sane upper bound on how much of a buffer hexdump() will actually render.
 * data_len is caller-supplied (a length field off the wire, in some call
 * sites) and feeds directly into the capacity arithmetic below; without a
 * cap, a bogus or corrupt length could overflow that arithmetic on a
 * 32-bit size_t well before it overflowed the allocation itself. This is
 * also just a reasonable ceiling for a debug dump either way. */
#define HEXDUMP_MAX_BYTES 4096

static void
hexdump(int severity, int domain, const unsigned char *data, int len, const char *heading)
{
  int i;
  int full_len;
  int truncated;
  unsigned char buff[17];
  const unsigned char *pc = data;
  char *content;
  size_t cap;
  size_t pos;

  if (len <= 0)
    return;

  full_len = len;
  truncated = (len > HEXDUMP_MAX_BYTES);
  if (truncated)
    len = HEXDUMP_MAX_BYTES;

  /* Render the whole dump -- heading plus every hex row -- into one buffer
   * and hand it to logger_write_with_label() as a single entry, instead of
   * one queue push per row: the queue is bounded by entry count, and a big
   * hexdump would otherwise crowd out ordinary log lines, or itself come
   * out partly dropped and looking corrupted. The size below is a generous
   * upper bound on what the loop can produce per row (actual usage is well
   * under half of it), computed once rather than grown incrementally; len
   * is capped above, so this can't overflow size_t even on a 32-bit build.
   */
  cap = (heading ? strlen(heading) : 0) + (size_t)((len + 15) / 16) * 128 + 192;
  content = malloc(cap);
  if (!content)
    return;

  pos = 0;
  if (heading)
    pos += snprintf(content + pos, cap - pos, "%s", heading);

  for (i = 0; i < len; i++)
    {
      if ((i % 16) == 0)
	{
	  if (i != 0)
	    pos += snprintf(content + pos, cap - pos, "  %s\n", buff);

	  pos += snprintf(content + pos, cap - pos, " %04x ", i);
	}

      pos += snprintf(content + pos, cap - pos, " %02x", pc[i]);

      if (isprint(pc[i]))
	buff[i % 16] = pc[i];
      else
	buff[i % 16] = '.';

      buff[(i % 16) + 1] = '\0';
    }

  while ((i % 16) != 0)
    {
      pos += snprintf(content + pos, cap - pos, "   ");
      i++;
    }

  pos += snprintf(content + pos, cap - pos, "  %s\n", buff);

  if (truncated)
    pos += snprintf(content + pos, cap - pos, "  ... (truncated, showing first %d of %d bytes)\n", len, full_len);

  // Mirrors vlogger(): DHEXDUMP() callers don't hold logger_lck themselves,
  // so take it here around the dispatch, same as every DPRINTF() line gets.
  // This only matters for the synchronous fallback in logger_dispatch()
  // (before the writer thread starts, or if it never does) -- once the
  // writer is running this just serialises the (lock-free-of-I/O) push
  // onto the queue, same as any other line, never held across I/O.
  if (logger_initialized)
    {
      LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_lck));
      logger_write_with_label(severity, domain, content);
      LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_lck));
    }
  else
    logger_write_with_label(severity, domain, content);

  free(content);
}

void
DPRINTF(int severity, int domain, const char *fmt, ...)
{
  va_list ap;

  // If domain and severity do not match the current log configuration, return early to
  // save some unnecessary code execution (tiny performance gain)
  if (logger_initialized && (!((1 << domain) & logdomains) || (severity > threshold)))
    return;

  va_start(ap, fmt);
  vlogger(severity, domain, fmt, ap);
  va_end(ap);
}

void
DVPRINTF(int severity, int domain, const char *fmt, va_list ap)
{
  // If domain and severity do not match the current log configuration, return early to
  // safe some unnecessary code execution (tiny performance gain)
  if (logger_initialized && (!((1 << domain) & logdomains) || (severity > threshold)))
    return;

  vlogger(severity, domain, fmt, ap);
}

void
DHEXDUMP(int severity, int domain, const unsigned char *data, int data_len, const char *heading)
{
  // If domain and severity do not match the current log configuration, return early to
  // save some unnecessary code execution (tiny performance gain)
  if (logger_initialized && (!((1 << domain) & logdomains) || (severity > threshold)))
    return;

  hexdump(severity, domain, data, data_len, heading);
}

int
logger_wants(int severity, int domain)
{
  if (!logger_initialized)
    return 1;

  return ((1 << domain) & logdomains) && (severity <= threshold);
}

unsigned int
logger_now_s(void)
{
  struct timespec ts;

#ifdef CLOCK_MONOTONIC_COARSE
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts);
#endif

  return (unsigned int)ts.tv_sec;
}

void
logger_ffmpeg(void *ptr, int level, const char *fmt, va_list ap)
{
  int severity;

  if (level <= AV_LOG_FATAL)
    severity = E_LOG;
  else if (level <= AV_LOG_WARNING)
    severity = E_WARN;
  else if (level <= AV_LOG_VERBOSE)
    severity = E_DBG;
  else if (level <= AV_LOG_DEBUG)
    severity = E_SPAM;
  else
    severity = E_SPAM;

  vlogger(severity, L_FFMPEG, fmt, ap);
}

void
logger_libevent(int severity, const char *msg)
{
  switch (severity)
    {
      case EVENT_LOG_DEBUG:
	severity = E_DBG;
	break;

      case EVENT_LOG_ERR:
	severity = E_LOG;
	break;

      case EVENT_LOG_WARN:
	severity = E_WARN;
	break;

      case EVENT_LOG_MSG:
	severity = E_INFO;
	break;

      default:
	severity = E_LOG;
	break;
    }

  DPRINTF(severity, L_EVENT, "%s\n", msg);
}

/* Reopen logfilename, executed by the writer thread. Called with no lock
 * held (fopen()/fclose() are I/O). Enqueues its own status line the normal
 * way (logger_write_with_label()), so it goes out in the writer's very next
 * pass rather than needing special-cased direct output here. */
static void
logger_writer_reopen(void)
{
  FILE *fp;
  char msg[1024];
  int open_errno;

  fp = fopen(logfilename, "a");
  if (!fp)
    {
      open_errno = errno;
      snprintf(msg, sizeof(msg), "Could not reopen logfile '%s': %s\n", logfilename, strerror(open_errno));
      logger_write_with_label(E_LOG, L_MAIN, msg);
      return;
    }

  if (logfile)
    fclose(logfile);
  logfile = fp;

  snprintf(msg, sizeof(msg), "%s version %s started new logfile '%s'\n", PACKAGE_NAME, PACKAGE_VERSION, logfilename);
  logger_write_with_label(E_LOG, L_MAIN, msg);
}

void
logger_reinit(void)
{
  FILE *fp;
  char msg[1024];
  int open_errno;

  if (!logfile)
    return;

  if (!__atomic_load_n(&logger_writer_running, __ATOMIC_ACQUIRE))
    {
      /* No writer thread (not started yet, or it failed to start) -- no
       * other thread can be touching logfile, so swap it right here,
       * exactly as before there was a writer thread at all. */
      LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_lck));

      fp = fopen(logfilename, "a");
      if (!fp)
	{
	  open_errno = errno;
	  snprintf(msg, sizeof(msg), "Could not reopen logfile '%s': %s\n",
		   logfilename, strerror(open_errno));
	  logger_write_with_label(E_LOG, L_MAIN, msg);
	}
      else
	{
	  fclose(logfile);
	  logfile = fp;
	  snprintf(msg, sizeof(msg), "%s version %s started new logfile '%s'\n",
		   PACKAGE_NAME, PACKAGE_VERSION, logfilename);
	  logger_write_with_label(E_LOG, L_MAIN, msg);
	}

      LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_lck));
      return;
    }

  /* The writer thread owns logfile once it's running -- it may be mid-batch
   * on it right now, outside any lock by design. Doing the fopen()/fclose()
   * swap from here (the signal-handling thread) would race that unlocked
   * I/O, so just request it; the writer performs the swap itself, between
   * batches, next time it wakes up. */
  LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_queue_lck));
  logger_reopen_pending = 1;
  LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_queue_lck));
  LOGGER_CHECK_ERR(pthread_cond_signal(&logger_queue_cond));
}


int
logger_severity(void)
{
  return threshold;
}

void
logger_severity_set(int severity)
{
  // Follow the requested level, clamping to the nearest supported severity
  // rather than ignoring an out-of-range request. Ignoring it would leave the
  // running threshold at its old value while the persisted config holds the
  // requested number - a silent divergence between what is configured and
  // what is actually applied.
  if (severity < E_FATAL)
    severity = E_FATAL;
  else if (severity > E_SPAM)
    severity = E_SPAM;

  LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_lck));
  threshold = severity;
  LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_lck));
}

/* The functions below are used at init time with a single thread running */
void
logger_domains(void)
{
  int i;

  fprintf(stdout, "%s", labels[0]);

  for (i = 1; i < N_LOGDOMAINS; i++)
    fprintf(stdout, ", %s", labels[i]);

  fprintf(stdout, "\n");
}

void
logger_detach(void)
{
  console = 0;
}

int
logger_init(char *file, char *domains, int severity, char *logformat)
{
  int ret;

  if ((sizeof(labels) / sizeof(labels[0])) != N_LOGDOMAINS)
    {
      fprintf(stderr, "WARNING: log domains do not match\n");

      return -1;
    }

  console = 1;
  // Clamp a persisted severity into range so a bad stored value cannot be
  // applied raw at startup (a negative threshold would suppress all output).
  threshold = (severity < E_FATAL) ? E_FATAL : (severity > E_SPAM ? E_SPAM : severity);
  format = format_code_get(logformat);

  if (domains)
    {
      ret = set_logdomains(domains);
      if (ret < 0)
	return ret;
    }
  else
    logdomains = ~0;

  if (!file)
    return 0;

  logfile = fopen(file, "a");
  if (!logfile)
    {
      fprintf(stderr, "Could not open logfile %s: %s\n", file, strerror(errno));

      return -1;
    }

  ret = fchown(fileno(logfile), runas_uid, 0);
  if (ret < 0)
    fprintf(stderr, "Failed to set ownership on logfile: %s\n", strerror(errno));

  ret = fchmod(fileno(logfile), 0644);
  if (ret < 0)
    fprintf(stderr, "Failed to set permissions on logfile: %s\n", strerror(errno));

  /* Own a copy of the path; the caller's string may live in the config JSON
   * tree, which is freed and rebuilt on config_reload() */
  logfilename = strdup(file);
  if (!logfilename)
    {
      fprintf(stderr, "Out of memory for logfile name\n");

      fclose(logfile);
      logfile = NULL;
      return -1;
    }

  /* logging w/o locks before initialized complete */
  CHECK_ERR(L_MISC, mutex_init(&logger_lck));

  CHECK_ERR(L_MISC, mutex_init(&logger_queue_lck));
  CHECK_ERR(L_MISC, pthread_cond_init(&logger_queue_cond, NULL));

  logger_queue_head = 0;
  logger_queue_tail = 0;
  logger_queue_count = 0;
  logger_queue_dropped = 0;
  logger_reopen_pending = 0;
  logger_writer_stop = 0;
  __atomic_store_n(&logger_writer_running, 0, __ATOMIC_RELAXED);

  logger_initialized = 1;

  return 0;
}

static void *
logger_writer_main(void *arg)
{
  struct logger_queue_entry batch[LOGGER_QUEUE_CAPACITY];
  char dropmsg[128];
  int dropmsg_len;
  int n;
  int i;
  int reopen;
  int stop_now;
  uint64_t dropped;

  thread_setname("logwriter");

  for (;;)
    {
      LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_queue_lck));

      while (logger_queue_count == 0 && !logger_reopen_pending && !logger_writer_stop)
	LOGGER_CHECK_ERR(pthread_cond_wait(&logger_queue_cond, &logger_queue_lck));

      reopen = logger_reopen_pending;
      logger_reopen_pending = 0;

      /* Drain whatever was queued *before* acting on a pending reopen, so
       * those lines still go to the old file -- the new one only carries
       * the reopen confirmation onward, which lands via the normal queue
       * on the very next pass since it wakes the loop immediately. */
      n = 0;
      while (logger_queue_count > 0 && n < LOGGER_QUEUE_CAPACITY)
	{
	  batch[n++] = logger_queue[logger_queue_tail];
	  logger_queue_tail = (logger_queue_tail + 1) % LOGGER_QUEUE_CAPACITY;
	  logger_queue_count--;
	}

      dropped = logger_queue_dropped;
      logger_queue_dropped = 0;

      stop_now = logger_writer_stop;

      LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_queue_lck));

      /* Everything from here on runs without the queue lock -- this is the
       * only thread doing file I/O, so nothing else needs it. */

      for (i = 0; i < n; i++)
	{
	  logger_write_line(batch[i].line, batch[i].len);
	  free(batch[i].line);
	}

      if (dropped > 0)
	{
	  dropmsg_len = snprintf(dropmsg, sizeof(dropmsg), "[logger] dropped %" PRIu64 " lines while the log sink was slow\n", dropped);
	  logger_write_line(dropmsg, (size_t)dropmsg_len);
	}

      if ((n > 0 || dropped > 0) && logfile)
	fflush(logfile); /* one fflush() per batch, not per line */

      if (reopen)
	logger_writer_reopen();

      if (stop_now)
	{
	  LOGGER_CHECK_ERR(pthread_mutex_lock(&logger_queue_lck));
	  if (logger_queue_count == 0 && !logger_reopen_pending)
	    {
	      LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_queue_lck));
	      break;
	    }
	  LOGGER_CHECK_ERR(pthread_mutex_unlock(&logger_queue_lck));
	  /* else: something arrived in this same window (e.g. the reopen
	   * confirmation line) -- go round once more to drain it. */
	}
    }

  __atomic_store_n(&logger_writer_running, 0, __ATOMIC_RELEASE);

  return NULL;
}

int
logger_async_start(void)
{
  int ret;

  if (!logger_initialized)
    return -1;

  if (__atomic_load_n(&logger_writer_running, __ATOMIC_ACQUIRE))
    return 0;

  logger_writer_stop = 0;

  ret = pthread_create(&logger_writer_tid, NULL, logger_writer_main, NULL);
  if (ret != 0)
    {
      DPRINTF(E_WARN, L_MAIN, "Could not start log writer thread: %s; logging stays synchronous\n", strerror(ret));
      return -1;
    }

  __atomic_store_n(&logger_writer_running, 1, __ATOMIC_RELEASE);

  return 0;
}

/* Ask the writer thread to stop and wait for it, bounded, so a wedged log
 * sink cannot hang shutdown. Polls logger_writer_running rather than a
 * plain pthread_join() so the wait has a deadline; mirrors the approach
 * used for the equivalent thread in the sibling monitor process. Returns 1
 * if the writer is confirmed stopped (joined, or was never started) and it
 * is therefore safe to close logfile; 0 if the bounded wait expired and the
 * writer was detached instead, in which case it may still be using logfile
 * and the caller must leave it alone. */
static int
logger_async_stop(void)
{
  struct timespec deadline;
  struct timespec now;

  if (!__atomic_load_n(&logger_writer_running, __ATOMIC_ACQUIRE))
    return 1;

  pthread_mutex_lock(&logger_queue_lck);
  logger_writer_stop = 1;
  pthread_cond_broadcast(&logger_queue_cond);
  pthread_mutex_unlock(&logger_queue_lck);

  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += LOGGER_STOP_TIMEOUT_MS / 1000;
  deadline.tv_nsec += (long)(LOGGER_STOP_TIMEOUT_MS % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L)
    {
      deadline.tv_nsec -= 1000000000L;
      deadline.tv_sec += 1;
    }

  while (__atomic_load_n(&logger_writer_running, __ATOMIC_ACQUIRE))
    {
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
	break;

      usleep(5000);
    }

  if (!__atomic_load_n(&logger_writer_running, __ATOMIC_ACQUIRE))
    {
      pthread_join(logger_writer_tid, NULL);
      return 1;
    }

  /* The sink is still stalled after the bounded wait (e.g. a wedged SD-card
   * write). Detach rather than block shutdown on it indefinitely; whatever
   * it hasn't flushed yet is lost -- the same drop-and-count trade-off the
   * writer already accepts under load, just pushed to its worst case. */
  pthread_detach(logger_writer_tid);
  return 0;
}

void
logger_deinit(void)
{
  /* logger_queue_lck/cond only exist once logger_initialized is set (they
   * are created together in logger_init(), right after logger_lck) -- guard
   * all of the writer-thread teardown on that, same as the original code
   * guarded logger_lck's destruction. */
  if (logger_initialized && logger_async_stop())
    {
      /* The writer is confirmed gone (or never started), so nothing else
       * can be touching logfile, logfilename or the queue now. */
      logger_drain_sync();

      if (logfile)
	{
	  fclose(logfile);
	  logfile = NULL;
	}

      free(logfilename);
      logfilename = NULL;

      CHECK_ERR(L_MISC, pthread_mutex_destroy(&logger_queue_lck));
      CHECK_ERR(L_MISC, pthread_cond_destroy(&logger_queue_cond));
    }
  /* else: either never initialized (nothing to do), or the writer is still
   * running after the bounded stop wait. It remains the sole owner of
   * logfile -- and of logfilename, which logger_writer_reopen() may still
   * fopen() if a reopen is (or becomes) pending -- plus the queue lock and
   * cond, so leave all of that alone rather than free or close it out from
   * under a thread that might still be using it. logfilename (and logfile)
   * are leaked on this path: the process is exiting either way, and that
   * beats a use-after-free in the writer thread. */

  if(logger_initialized)
    {
      /* logging w/o locks to stderr now */
      logger_initialized = 0;
      console = 1;
      CHECK_ERR(L_MISC, pthread_mutex_destroy(&logger_lck));
    }
}
