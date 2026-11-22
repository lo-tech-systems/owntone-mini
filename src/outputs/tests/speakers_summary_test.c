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

// Checks how the session states of the members of a row are reduced to the
// one the row shows, with no daemon and no framework. From the top of the
// source tree:
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

// A PIN asked for shows even while another member plays: the user has to act
static void
password_wins_over_everything(void)
{
  struct speakers_seen s = { .streaming = true, .starting = true, .stopping = true, .password = true, .failed = true };

  case_begin("password_wins_over_everything");
  CHECK(speakers_summary_resolve(s) == SPEAKERS_SUMMARY_PASSWORD);
  case_end();
}

static void
streaming_wins_over_the_rest(void)
{
  struct speakers_seen s = { .streaming = true, .starting = true, .stopping = true, .failed = true };

  case_begin("streaming_wins_over_the_rest");
  CHECK(speakers_summary_resolve(s) == SPEAKERS_SUMMARY_STREAMING);
  case_end();
}

static void
starting_wins_over_stopping_and_failed(void)
{
  struct speakers_seen s = { .starting = true, .stopping = true, .failed = true };

  case_begin("starting_wins_over_stopping_and_failed");
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
  CHECK(speakers_summary_resolve(with_password) == SPEAKERS_SUMMARY_PASSWORD);
  CHECK(speakers_summary_resolve(with_failed) == SPEAKERS_SUMMARY_STOPPING);
  CHECK(speakers_summary_resolve(with_streaming) == SPEAKERS_SUMMARY_STREAMING);
  case_end();
}

/* ------------------------------ Row members ------------------------------- */

static const struct row_member streaming_member = { .phase = OUTPUT_PHASE_STREAMING, .live = true, .selected = true };
static const struct row_member connecting_member = { .phase = OUTPUT_PHASE_CONNECTING, .live = true, .selected = true };
static const struct row_member retrying_member = { .phase = OUTPUT_PHASE_FAILED, .live = true, .selected = true,
                                                   .reason = OUTPUT_FAILURE_LOST };
static const struct row_member failed_member = { .phase = OUTPUT_PHASE_FAILED, .reason = OUTPUT_FAILURE_UNREACHABLE };
static const struct row_member stopped_member = { .phase = OUTPUT_PHASE_STOPPED };

static struct speakers_row_view
row_of(const struct row_member *members, int n)
{
  struct speakers_row row = { 0 };
  int i;

  for (i = 0; i < n; i++)
    speakers_row_add(&row, &members[i]);

  return speakers_row_resolve(&row);
}

// One member of a stereo pair is retried while the other plays: the pair
// plays, is on, and nothing is missing yet
static void
row_pair_one_retrying(void)
{
  struct row_member m[] = { streaming_member, retrying_member };
  struct speakers_row_view v = row_of(m, 2);

  case_begin("row_pair_one_retrying");
  CHECK(v.summary == SPEAKERS_SUMMARY_STREAMING);
  CHECK(v.selected);
  CHECK(v.members_missing == 0);
  CHECK(v.reason == OUTPUT_FAILURE_NONE);
  case_end();
}

// The retry was spent: the pair plays on with one member missing
static void
row_pair_one_given_up(void)
{
  struct row_member m[] = { streaming_member, failed_member };
  struct speakers_row_view v = row_of(m, 2);

  case_begin("row_pair_one_given_up");
  CHECK(v.summary == SPEAKERS_SUMMARY_STREAMING);
  CHECK(v.selected);
  CHECK(v.members_missing == 1);
  case_end();
}

// Both members wait for their retry: the row reads starting and stays on
static void
row_pair_both_retrying(void)
{
  struct row_member m[] = { retrying_member, retrying_member };
  struct speakers_row_view v = row_of(m, 2);

  case_begin("row_pair_both_retrying");
  CHECK(v.summary == SPEAKERS_SUMMARY_STARTING);
  CHECK(v.selected);
  CHECK(v.members_missing == 0);
  case_end();
}

// Nothing is live: the row fails, and nothing counts as missing
static void
row_pair_both_failed(void)
{
  struct row_member m[] = { failed_member, failed_member };
  struct speakers_row_view v = row_of(m, 2);

  case_begin("row_pair_both_failed");
  CHECK(v.summary == SPEAKERS_SUMMARY_FAILED);
  CHECK(!v.selected);
  CHECK(v.members_missing == 0);
  CHECK(v.reason == OUTPUT_FAILURE_UNREACHABLE);
  case_end();
}

// One member asks for a PIN while the other plays: the PIN shows, and the
// member waiting for it is not counted as missing
static void
row_pair_pin_while_playing(void)
{
  struct row_member pin = { .phase = OUTPUT_PHASE_PASSWORD, .pin_pending = true, .reason = OUTPUT_FAILURE_PIN_REQUIRED };
  struct row_member m[] = { streaming_member, pin };
  struct speakers_row_view v = row_of(m, 2);

  case_begin("row_pair_pin_while_playing");
  CHECK(v.summary == SPEAKERS_SUMMARY_PASSWORD);
  CHECK(v.pin_pending);
  CHECK(v.reason == OUTPUT_FAILURE_PIN_REQUIRED);
  CHECK(v.members_missing == 0);
  case_end();
}

// An Apple TV included in its group whose control session failed while its
// HomePods play: the row plays
static void
row_tv_failed_homepods_play(void)
{
  struct row_member tv = failed_member;
  struct row_member m[3];
  struct speakers_row_view v;

  tv.is_row = true;
  m[0] = tv;
  m[1] = streaming_member;
  m[2] = streaming_member;
  v = row_of(m, 3);

  case_begin("row_tv_failed_homepods_play");
  CHECK(v.summary == SPEAKERS_SUMMARY_STREAMING);
  CHECK(v.members_missing == 1);
  case_end();
}

// An Apple TV whose start failed and was deselected, its HomePods selected and
// waiting for its chain: the row fails, though still shown selected
static void
row_tv_start_failed_followers_waiting(void)
{
  struct row_member tv = failed_member;
  struct row_member waiting = { .phase = OUTPUT_PHASE_STOPPED, .selected = true };
  struct row_member m[3];
  struct speakers_row_view v;

  tv.is_row = true;
  m[0] = tv;
  m[1] = waiting;
  m[2] = waiting;
  v = row_of(m, 3);

  case_begin("row_tv_start_failed_followers_waiting");
  CHECK(v.summary == SPEAKERS_SUMMARY_FAILED);
  CHECK(v.selected);
  CHECK(v.members_missing == 0);
  case_end();
}

// A retried member and one that is gone: the row reads starting, with the
// gone one counted
static void
row_retrying_and_gone(void)
{
  struct row_member m[] = { retrying_member, stopped_member };
  struct speakers_row_view v = row_of(m, 2);

  case_begin("row_retrying_and_gone");
  CHECK(v.summary == SPEAKERS_SUMMARY_STARTING);
  CHECK(v.members_missing == 1);
  case_end();
}

// A device in no group is its own row
static void
row_single_device(void)
{
  struct row_member m[] = { connecting_member };
  struct speakers_row_view v = row_of(m, 1);
  struct row_member f[] = { failed_member };
  struct speakers_row_view vf = row_of(f, 1);

  case_begin("row_single_device");
  CHECK(v.summary == SPEAKERS_SUMMARY_STARTING);
  CHECK(v.selected);
  CHECK(v.members_missing == 0);
  CHECK(vf.summary == SPEAKERS_SUMMARY_FAILED);
  CHECK(vf.reason == OUTPUT_FAILURE_UNREACHABLE);
  case_end();
}

// The row's own device gives the reason when it is in the winning phase,
// wherever it is in the list; else the first member in it does
static void
row_reason_precedence(void)
{
  struct row_member first = { .phase = OUTPUT_PHASE_FAILED, .reason = OUTPUT_FAILURE_TIMEOUT };
  struct row_member own = { .phase = OUTPUT_PHASE_FAILED, .reason = OUTPUT_FAILURE_UNREACHABLE, .is_row = true };
  struct row_member second = { .phase = OUTPUT_PHASE_FAILED, .reason = OUTPUT_FAILURE_REFUSED };
  struct row_member with_own[] = { first, own };
  struct row_member without_own[] = { first, second };

  case_begin("row_reason_precedence");
  CHECK(row_of(with_own, 2).reason == OUTPUT_FAILURE_UNREACHABLE);
  CHECK(row_of(without_own, 2).reason == OUTPUT_FAILURE_TIMEOUT);
  case_end();
}

int
main(int argc, char **argv)
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  nothing_seen_is_stopped();
  each_state_alone_maps_to_itself();
  password_wins_over_everything();
  streaming_wins_over_the_rest();
  starting_wins_over_stopping_and_failed();
  password_wins_over_failed();
  stopping_ranks_below_starting();
  row_pair_one_retrying();
  row_pair_one_given_up();
  row_pair_both_retrying();
  row_pair_both_failed();
  row_pair_pin_while_playing();
  row_tv_failed_homepods_play();
  row_tv_start_failed_followers_waiting();
  row_retrying_and_gone();
  row_single_device();
  row_reason_precedence();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
