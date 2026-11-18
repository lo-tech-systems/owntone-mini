/*
 * Copyright (C) 2026 James Pearce
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

// Checks the worker pools with real threads and no daemon: the pairing thread
// runs its jobs one after another in the order given, under its own name, and
// keeps running them while every general worker is held. The logger and the
// thread naming helper are replaced by the stubs below. The program is listed
// under check_PROGRAMS and TESTS in src/Makefile.am, so "make check" builds
// and runs it.
//
// Exits 0 when every case passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing case too).

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <event2/thread.h>

#include "../../worker.h"

static int cases_run;
static int cases_failed;

// Print the name of every passing case as well
static bool verbose;

// The case being run, for the failure lines
static const char *cur_name;
static bool cur_failed;

#define CHECK(cond) \
  do { \
    if (!(cond)) \
      { \
	printf("FAIL %s: line %d: %s\n", cur_name, __LINE__, #cond); \
	cur_failed = true; \
      } \
  } while (0)

static void
case_begin(const char *name)
{
  cur_name = name;
  cur_failed = false;
}

static void
case_end(void)
{
  cases_run++;
  if (cur_failed)
    cases_failed++;
  else if (verbose)
    printf("pass %s\n", cur_name);
}


/* ------------------------------- Stubs ------------------------------------ */

void
DPRINTF(int severity, int domain, const char *fmt, ...)
{
  va_list ap;

  if (!verbose)
    return;

  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

void
thread_setname(const char *name)
{
  pthread_setname_np(pthread_self(), name);
}


/* ------------------------------ Job records ------------------------------- */

#define NJOBS 32

// How long a case waits for the threads before it counts as failed
#define WAIT_SEC 5

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static int ran[NJOBS]; // The job numbers, in the order the jobs ran
static int nran;
static pthread_t ran_on[NJOBS];
static char ran_name[NJOBS][16];

static int held; // General workers currently held
static bool release_held;

struct job_arg
{
  int n;
};

static void
deadline_get(struct timespec *ts)
{
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += WAIT_SEC;
}

// Waits under the lock until *counter reaches want, false if it never does
static bool
wait_for(int *counter, int want)
{
  struct timespec ts;
  int ret = 0;

  deadline_get(&ts);

  pthread_mutex_lock(&lock);
  while (*counter < want && ret != ETIMEDOUT)
    ret = pthread_cond_timedwait(&cond, &lock, &ts);
  ret = (*counter >= want);
  pthread_mutex_unlock(&lock);

  return ret;
}

static void
record_cb(void *arg)
{
  struct job_arg *ja = arg;

  pthread_mutex_lock(&lock);
  if (nran < NJOBS)
    {
      ran[nran] = ja->n;
      ran_on[nran] = pthread_self();
      pthread_getname_np(pthread_self(), ran_name[nran], sizeof(ran_name[nran]));
      nran++;
    }
  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&lock);
}

// Holds a general worker until release_held is set
static void
hold_cb(void *arg)
{
  struct timespec ts;
  int ret = 0;

  deadline_get(&ts);
  ts.tv_sec += WAIT_SEC;

  pthread_mutex_lock(&lock);
  held++;
  pthread_cond_broadcast(&cond);
  while (!release_held && ret != ETIMEDOUT)
    ret = pthread_cond_timedwait(&cond, &lock, &ts);
  held--;
  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&lock);
}

static void
records_reset(void)
{
  pthread_mutex_lock(&lock);
  nran = 0;
  memset(ran, 0, sizeof(ran));
  memset(ran_name, 0, sizeof(ran_name));
  pthread_mutex_unlock(&lock);
}


/* -------------------------------- Cases ----------------------------------- */

static void
test_pairing_runs_in_order_on_one_named_thread(void)
{
  struct job_arg ja;
  int i;

  case_begin("pairing_runs_in_order_on_one_named_thread");
  records_reset();

  for (i = 0; i < NJOBS; i++)
    {
      ja.n = i;
      CHECK(worker_pairing_execute(record_cb, &ja, sizeof(ja)) == 0);
    }

  CHECK(wait_for(&nran, NJOBS));

  pthread_mutex_lock(&lock);
  for (i = 0; i < nran; i++)
    {
      CHECK(ran[i] == i);
      CHECK(pthread_equal(ran_on[i], ran_on[0]));
      CHECK(strcmp(ran_name[i], "pairing") == 0);
    }
  pthread_mutex_unlock(&lock);

  case_end();
}

static void
test_argument_is_copied(void)
{
  struct job_arg ja;

  case_begin("argument_is_copied");
  records_reset();

  ja.n = 7;
  CHECK(worker_pairing_execute(record_cb, &ja, sizeof(ja)) == 0);
  ja.n = 99; // The job must still see the value at the call

  CHECK(wait_for(&nran, 1));

  pthread_mutex_lock(&lock);
  CHECK(ran[0] == 7);
  pthread_mutex_unlock(&lock);

  case_end();
}

static void
test_general_pool_keeps_its_name(void)
{
  struct job_arg ja = { 0 };

  case_begin("general_pool_keeps_its_name");
  records_reset();

  CHECK(worker_execute(record_cb, &ja, sizeof(ja), 0) == 0);
  CHECK(wait_for(&nran, 1));

  pthread_mutex_lock(&lock);
  CHECK(strcmp(ran_name[0], "worker") == 0);
  pthread_mutex_unlock(&lock);

  case_end();
}

static void
test_pairing_runs_while_workers_are_held(void)
{
  struct job_arg ja;
  int i;

  case_begin("pairing_runs_while_workers_are_held");
  records_reset();

  pthread_mutex_lock(&lock);
  release_held = false;
  pthread_mutex_unlock(&lock);

  // One per general worker; however the pool spreads them, at least one
  // worker is held and the rest of the jobs wait behind it
  for (i = 0; i < 4; i++)
    CHECK(worker_execute(hold_cb, NULL, 0, 0) == 0);

  CHECK(wait_for(&held, 1));

  for (i = 0; i < 4; i++)
    {
      ja.n = i;
      CHECK(worker_pairing_execute(record_cb, &ja, sizeof(ja)) == 0);
    }

  CHECK(wait_for(&nran, 4));

  pthread_mutex_lock(&lock);
  CHECK(held > 0); // The pairing jobs ran while a worker was still held
  for (i = 0; i < nran; i++)
    CHECK(strcmp(ran_name[i], "pairing") == 0);
  release_held = true;
  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&lock);

  // Let the held workers go before the pools are stopped
  pthread_mutex_lock(&lock);
  while (held > 0)
    pthread_cond_wait(&cond, &lock);
  pthread_mutex_unlock(&lock);

  case_end();
}

static void
test_nothing_is_taken_without_the_pools(void)
{
  struct job_arg ja = { 0 };

  case_begin("nothing_is_taken_without_the_pools");

  CHECK(worker_pairing_execute(record_cb, &ja, sizeof(ja)) < 0);
  CHECK(worker_execute(record_cb, &ja, sizeof(ja), 0) < 0);

  case_end();
}

int
main(int argc, char *argv[])
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  if (evthread_use_pthreads() < 0 || worker_init() < 0)
    {
      printf("FAIL could not start the worker pools\n");
      return 1;
    }

  test_pairing_runs_in_order_on_one_named_thread();
  test_argument_is_copied();
  test_general_pool_keeps_its_name();
  test_pairing_runs_while_workers_are_held();

  worker_deinit();
  worker_deinit(); // A second call finds nothing to stop

  test_nothing_is_taken_without_the_pools();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
