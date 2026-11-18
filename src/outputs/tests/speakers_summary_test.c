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

// Checks how the session states of the other devices in a group are reduced
// to one, with no daemon and no framework. From the top of the source tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/speakers_summary_test src/outputs/tests/speakers_summary_test.c
//   /tmp/speakers_summary_test
//
// Exits 0 when every case passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing case too). The program is also
// listed under check_PROGRAMS and TESTS in src/Makefile.am, so "make check"
// builds and runs it.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../speakers_summary.h"

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
    printf("PASS %s\n", cur_name);
}

static void
nothing_seen_is_stopped(void)
{
  struct speakers_seen s = { 0 };

  case_begin("nothing_seen_is_stopped");
  CHECK(speakers_summary_resolve(s) == SPEAKERS_SUMMARY_STOPPED);
  case_end();
}

static void
each_state_alone_maps_to_itself(void)
{
  struct speakers_seen starting = { .starting = true };
  struct speakers_seen password = { .password = true };
  struct speakers_seen failed = { .failed = true };
  struct speakers_seen streaming = { .streaming = true };

  case_begin("each_state_alone_maps_to_itself");
  CHECK(speakers_summary_resolve(starting) == SPEAKERS_SUMMARY_STARTING);
  CHECK(speakers_summary_resolve(password) == SPEAKERS_SUMMARY_PASSWORD);
  CHECK(speakers_summary_resolve(failed) == SPEAKERS_SUMMARY_FAILED);
  CHECK(speakers_summary_resolve(streaming) == SPEAKERS_SUMMARY_STREAMING);
  case_end();
}

static void
streaming_wins_over_everything(void)
{
  struct speakers_seen s = { .streaming = true, .starting = true, .stopping = true, .password = true, .failed = true };

  case_begin("streaming_wins_over_everything");
  CHECK(speakers_summary_resolve(s) == SPEAKERS_SUMMARY_STREAMING);
  case_end();
}

static void
starting_wins_over_password_and_failed(void)
{
  struct speakers_seen s = { .starting = true, .stopping = true, .password = true, .failed = true };

  case_begin("starting_wins_over_password_and_failed");
  CHECK(speakers_summary_resolve(s) == SPEAKERS_SUMMARY_STARTING);
  case_end();
}

static void
password_wins_over_failed(void)
{
  struct speakers_seen s = { .password = true, .failed = true };

  case_begin("password_wins_over_failed");
  CHECK(speakers_summary_resolve(s) == SPEAKERS_SUMMARY_PASSWORD);
  case_end();
}

// A member being stopped: the group reads stopping, unless another member is
// still starting (a group with one member restarting reads connecting)
static void
stopping_ranks_below_starting(void)
{
  struct speakers_seen alone = { .stopping = true };
  struct speakers_seen with_starting = { .starting = true, .stopping = true };
  struct speakers_seen with_password = { .stopping = true, .password = true };
  struct speakers_seen with_failed = { .stopping = true, .failed = true };
  struct speakers_seen with_streaming = { .streaming = true, .stopping = true };

  case_begin("stopping_ranks_below_starting");
  CHECK(speakers_summary_resolve(alone) == SPEAKERS_SUMMARY_STOPPING);
  CHECK(speakers_summary_resolve(with_starting) == SPEAKERS_SUMMARY_STARTING);
  CHECK(speakers_summary_resolve(with_password) == SPEAKERS_SUMMARY_STOPPING);
  CHECK(speakers_summary_resolve(with_failed) == SPEAKERS_SUMMARY_STOPPING);
  CHECK(speakers_summary_resolve(with_streaming) == SPEAKERS_SUMMARY_STREAMING);
  case_end();
}

int
main(int argc, char **argv)
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  nothing_seen_is_stopped();
  each_state_alone_maps_to_itself();
  streaming_wins_over_everything();
  starting_wins_over_password_and_failed();
  password_wins_over_failed();
  stopping_ranks_below_starting();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
