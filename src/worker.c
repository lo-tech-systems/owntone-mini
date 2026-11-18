/*
 * Copyright (C) 2014 Espen Jürgensen <espenjurgensen@gmail.com>
 *
 * Modified for owntone-mini. Modifications Copyright (C) 2026 James Pearce
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

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/queue.h>

#include <unistd.h>

#include <pthread.h>

#include <event2/event.h>

#include "logger.h"
#include "worker.h"
#include "evthr.h"
#include "misc.h"

#define THREADPOOL_NTHREADS 4

// Pair setup computations have a pool of their own, so a start that needs one
// never waits behind API requests or artwork on the general pool. One thread,
// so pairing never takes more than one core from the audio threads.
#define PAIRING_NTHREADS 1

static struct evthr_pool *worker_threadpool;
static struct evthr_pool *pairing_threadpool;
static char pairing_thread_name[] = "pairing";
static __thread struct evthr *worker_thr;


/* ----------------------------- CALLBACK EXECUTION ------------------------- */
/*                                 Worker threads                             */

struct worker_arg
{
  void (*cb)(void *);
  void *cb_arg;
  int delay;
  struct event *timer;
  LIST_ENTRY(worker_arg) next;
};

// The delayed tasks waiting on this thread's timers, let go when the thread
// stops so a task still waiting at shutdown is not lost with its timer
static __thread LIST_HEAD(worker_delayed, worker_arg) worker_delayed;

static void
worker_arg_free(struct worker_arg *cmdarg)
{
  if (cmdarg->timer)
    event_free(cmdarg->timer);
  free(cmdarg->cb_arg);
  free(cmdarg);
}

static void
execute_cb(int fd, short what, void *arg)
{
  struct worker_arg *cmdarg = arg;

  LIST_REMOVE(cmdarg, next);

  cmdarg->cb(cmdarg->cb_arg);

  worker_arg_free(cmdarg);
}

static void
execute(struct evthr *thr, void *arg, void *shared)
{
  struct worker_arg *cmdarg = arg;
  struct timeval tv = { cmdarg->delay, 0 };
  struct event_base *evbase;

  if (cmdarg->delay)
    {
      evbase = evthr_get_base(thr);
      cmdarg->timer = evtimer_new(evbase, execute_cb, cmdarg);
      LIST_INSERT_HEAD(&worker_delayed, cmdarg, next);
      evtimer_add(cmdarg->timer, &tv);
      return;
    }

  cmdarg->cb(cmdarg->cb_arg);
  worker_arg_free(cmdarg);
}

// The pool's shared argument is the thread name, NULL for the general pool
static void
init_cb(struct evthr *thr, void *shared)
{
  worker_thr = thr;

  thread_setname(shared ? shared : "worker");

#if defined(__linux__)
  // The pool carries work the audio threads must outrank (pairing
  // computations, artwork preparation, API requests).
  //
  // This nice() value is RELATIVE to the process's current priority, not an
  // absolute one: it only matters together with whatever baseline the
  // owning systemd unit sets via Nice=. With the unit's Nice= at -12, these
  // threads land at -2, i.e. still above default-priority (nice 0)
  // processes. If the unit's baseline Nice= is ever changed, their effective
  // priority moves with it.
  errno = 0;
  if (nice(10) == -1 && errno != 0)
    DPRINTF(E_DBG, L_MAIN, "Could not lower worker thread priority: %s\n", strerror(errno));
#endif
}

// Runs on the thread once its loop has ended, before its event base is freed
static void
exit_cb(struct evthr *thr, void *shared)
{
  struct worker_arg *cmdarg;

  while ((cmdarg = LIST_FIRST(&worker_delayed)))
    {
      LIST_REMOVE(cmdarg, next);
      worker_arg_free(cmdarg);
    }

  worker_thr = NULL;
}


/* ---------------------------- Our worker API  --------------------------- */

static int
execute_on(struct evthr_pool *pool, void (*cb)(void *), void *cb_arg, size_t arg_size, int delay)
{
  struct worker_arg *cmdarg;
  void *argcpy;

  // Not started, or already stopped
  if (!pool)
    return -1;

  cmdarg = calloc(1, sizeof(struct worker_arg));
  if (!cmdarg)
    {
      DPRINTF(E_LOG, L_MAIN, "Could not allocate worker_arg\n");
      return -1;
    }

  if (arg_size > 0)
    {
      argcpy = malloc(arg_size);
      if (!argcpy)
	{
	  DPRINTF(E_LOG, L_MAIN, "Out of memory\n");
	  free(cmdarg);
	  return -1;
	}

      memcpy(argcpy, cb_arg, arg_size);
    }
  else
    argcpy = NULL;

  cmdarg->cb = cb;
  cmdarg->cb_arg = argcpy;
  cmdarg->delay = delay;

  if (evthr_pool_defer(pool, execute, cmdarg) != EVTHR_RES_OK)
    {
      free(argcpy);
      free(cmdarg);
      return -1;
    }

  return 0;
}

int
worker_execute(void (*cb)(void *), void *cb_arg, size_t arg_size, int delay)
{
  return execute_on(worker_threadpool, cb, cb_arg, arg_size, delay);
}

int
worker_pairing_execute(void (*cb)(void *), void *cb_arg, size_t arg_size)
{
  return execute_on(pairing_threadpool, cb, cb_arg, arg_size, 0);
}

struct event_base *
worker_evbase_get(void)
{
  return evthr_get_base(worker_thr);
}

int
worker_init(void)
{
  int ret;

  worker_threadpool = evthr_pool_wexit_new(THREADPOOL_NTHREADS, init_cb, exit_cb, NULL);
  if (!worker_threadpool)
    {
      DPRINTF(E_LOG, L_MAIN, "Could not create worker thread pool\n");
      goto error;
    }

  ret = evthr_pool_start(worker_threadpool);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MAIN, "Could not spawn worker threads\n");
      goto error;
    }

  pairing_threadpool = evthr_pool_wexit_new(PAIRING_NTHREADS, init_cb, exit_cb, pairing_thread_name);
  if (!pairing_threadpool)
    {
      DPRINTF(E_LOG, L_MAIN, "Could not create pairing thread pool\n");
      goto error;
    }

  ret = evthr_pool_start(pairing_threadpool);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MAIN, "Could not spawn pairing thread\n");
      goto error;
    }

  return 0;
  
 error:
  worker_deinit();
  return -1;
}

// Either pool may be NULL here, after a failed worker_init()
void
worker_deinit(void)
{
  if (pairing_threadpool)
    {
      evthr_pool_stop(pairing_threadpool);
      evthr_pool_free(pairing_threadpool);
      pairing_threadpool = NULL;
    }

  if (worker_threadpool)
    {
      evthr_pool_stop(worker_threadpool);
      evthr_pool_free(worker_threadpool);
      worker_threadpool = NULL;
    }
}
