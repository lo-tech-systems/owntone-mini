
#ifndef __LOGGER_H__
#define __LOGGER_H__

#include <stdarg.h>
#include <time.h>

/* Log domains */
#define L_CONF        0
#define L_HTTPD       3
#define L_MAIN        5
#define L_MDNS        6
#define L_MISC        7
#define L_XCODE       10
/* libevent logging */
#define L_EVENT       11
#define L_FFMPEG      14
#define L_PLAYER      16
#define L_RAOP        17
#define L_WEB         29
#define L_AIRPLAY     30

#define N_LOGDOMAINS  32

/* Severities */
#define E_FATAL   0
#define E_LOG     1
#define E_WARN    2
#define E_INFO    3
#define E_DBG     4
#define E_SPAM    5



void
DPRINTF(int severity, int domain, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

void
DVPRINTF(int severity, int domain, const char *fmt, va_list ap);

void
DHEXDUMP(int severity, int domain, const unsigned char *data, int data_len, const char *heading);

/* Cheap severity/domain pre-filter, exposed so DPRINTF_THROTTLED() below can
 * bail out before touching its timing/atomic state -- mirrors the early-out
 * DPRINTF() already does internally, so a filtered-out call costs no more
 * than a plain DPRINTF() call. */
int
logger_wants(int severity, int domain);

/* Monotonic seconds (truncated to 32 bits), for timing throttle windows.
 * Unlike time(), this can't be stepped backwards or forwards by an NTP
 * step, DST change or manual clock set, which could otherwise defeat a
 * throttle (stall it open, or make it fire early). Uses
 * CLOCK_MONOTONIC_COARSE where available -- plenty of resolution for a
 * multi-second window, and cheaper than the full CLOCK_MONOTONIC lookup. */
unsigned int
logger_now_s(void);

void
logger_ffmpeg(void *ptr, int level, const char *fmt, va_list ap);

void
logger_libevent(int severity, const char *msg);

void
logger_reinit(void);

int
logger_severity(void);

void
logger_severity_set(int severity);

void
logger_domains(void);

void
logger_detach(void);

int
logger_init(char *file, char *domains, int severity, char *logformat);

void
logger_deinit(void);

/* Starts the dedicated log writer thread; until this is called (or if it
 * fails), DPRINTF() etc. write synchronously, same as always. Must be
 * called after the process has forked/daemonized -- see the call site in
 * main() for why. Returns 0 on success, -1 otherwise. */
int
logger_async_start(void);


/* Rate-limit a log line to at most one emission per interval_s seconds from
 * this exact call site: the counters below are static, so there is one
 * instance per textual macro invocation, not per format string, domain, or
 * thread. The first hit logs immediately; further hits within the window
 * are counted and dropped; the next line emitted once the window has
 * elapsed carries a "(N similar suppressed)" suffix, so a burst is visible
 * in the log instead of silently vanishing between two identical lines.
 *
 * fmt must NOT end in '\n' -- the macro appends it (and, when relevant,
 * the suppression suffix ahead of it).
 *
 * The per-call-site counters are updated with __atomic builtins (on plain
 * "unsigned int", which is lock-free on every target this project builds
 * for) rather than a mutex, because a call site can legitimately be
 * reached from more than one thread. At worst a race lets one extra line
 * through, or delays a suppression summary by one interval -- never
 * undefined behaviour. Seconds are truncated to 32 bits, which only
 * matters once every few decades of continuous uptime.
 *
 * dt_thr_last starts out zero-initialized, same as logger_now_s()'s clock
 * itself starts counting from at boot, so a bare "elapsed < interval_s"
 * check would wrongly suppress a call site's first hit if that happened to
 * land within interval_s of boot. dt_thr_last == 0 is therefore treated as
 * "never emitted yet" rather than a real timestamp, so the first hit always
 * goes through regardless of how soon after boot it is.
 */
#define DPRINTF_THROTTLED(interval_s, severity, domain, fmt, ...) \
  do { \
    static unsigned int dt_thr_last; \
    static unsigned int dt_thr_suppressed; \
    unsigned int dt_thr_now; \
    unsigned int dt_thr_prev; \
    unsigned int dt_thr_n; \
    if (logger_wants((severity), (domain))) \
      { \
	dt_thr_now = logger_now_s(); \
	dt_thr_prev = __atomic_load_n(&dt_thr_last, __ATOMIC_RELAXED); \
	if (dt_thr_prev != 0 && dt_thr_now - dt_thr_prev < (unsigned int)(interval_s)) \
	  __atomic_fetch_add(&dt_thr_suppressed, 1, __ATOMIC_RELAXED); \
	else if (!__atomic_compare_exchange_n(&dt_thr_last, &dt_thr_prev, dt_thr_now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) \
	  __atomic_fetch_add(&dt_thr_suppressed, 1, __ATOMIC_RELAXED); \
	else \
	  { \
	    dt_thr_n = __atomic_exchange_n(&dt_thr_suppressed, 0, __ATOMIC_RELAXED); \
	    if (dt_thr_n > 0) \
	      DPRINTF((severity), (domain), fmt " (%u similar suppressed)\n", ##__VA_ARGS__, dt_thr_n); \
	    else \
	      DPRINTF((severity), (domain), fmt "\n", ##__VA_ARGS__); \
	  } \
      } \
  } while (0)


#endif /* !__LOGGER_H__ */
