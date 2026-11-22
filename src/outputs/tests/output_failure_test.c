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

// Checks the reason a backend reports with a failed or password state, and
// the names clients see, with no daemon and no framework. From the top of the
// source tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/output_failure_test src/outputs/tests/output_failure_test.c
//   /tmp/output_failure_test
//
// Exits 0 when every case passes, 1 otherwise. Also run by "make check".

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../output_failure.h"

static int cases_run;
static int cases_failed;

static void
check_reason(const char *what, enum output_failure_reason got, enum output_failure_reason want)
{
  cases_run++;
  if (got == want)
    return;

  cases_failed++;
  printf("FAIL %s: got %s, want %s\n", what, output_failure_name(got), output_failure_name(want));
}

static void
check_name(enum output_failure_reason reason, const char *want)
{
  cases_run++;
  if (strcmp(output_failure_name(reason), want) == 0)
    return;

  cases_failed++;
  printf("FAIL name of %d: got %s, want %s\n", (int)reason, output_failure_name(reason), want);
}

static void
test_reported(void)
{
  // A PIN start: the PIN is on screen
  check_reason("password, no attempt", output_failure_reported(OUTPUT_FAILURE_NONE, true, false), OUTPUT_FAILURE_PIN_REQUIRED);
  // An authorise the receiver refused
  check_reason("password, attempt", output_failure_reported(OUTPUT_FAILURE_NONE, true, true), OUTPUT_FAILURE_PIN_INVALID);
  // An authorise whose pair setup timed out in the queue is not a wrong PIN
  check_reason("password, attempt, timed out", output_failure_reported(OUTPUT_FAILURE_TIMEOUT, true, true), OUTPUT_FAILURE_TIMEOUT);
  check_reason("password, no attempt, timed out", output_failure_reported(OUTPUT_FAILURE_TIMEOUT, true, false), OUTPUT_FAILURE_TIMEOUT);
  // Failures keep the reason recorded at their site
  check_reason("failed, unreachable", output_failure_reported(OUTPUT_FAILURE_UNREACHABLE, false, false), OUTPUT_FAILURE_UNREACHABLE);
  check_reason("failed, capacity", output_failure_reported(OUTPUT_FAILURE_CAPACITY, false, false), OUTPUT_FAILURE_CAPACITY);
  check_reason("failed, no clock", output_failure_reported(OUTPUT_FAILURE_NO_CLOCK, false, true), OUTPUT_FAILURE_NO_CLOCK);
  check_reason("failed, cancelled", output_failure_reported(OUTPUT_FAILURE_CANCELLED, false, false), OUTPUT_FAILURE_CANCELLED);
  // A failure with no site of its own was refused, PIN or not
  check_reason("failed, nothing recorded", output_failure_reported(OUTPUT_FAILURE_NONE, false, false), OUTPUT_FAILURE_REFUSED);
  check_reason("failed, nothing recorded, attempt", output_failure_reported(OUTPUT_FAILURE_NONE, false, true), OUTPUT_FAILURE_REFUSED);
}

static void
test_names(void)
{
  check_name(OUTPUT_FAILURE_NONE, "none");
  check_name(OUTPUT_FAILURE_CAPACITY, "capacity");
  check_name(OUTPUT_FAILURE_PIN_REQUIRED, "pin_required");
  check_name(OUTPUT_FAILURE_PIN_INVALID, "pin_invalid");
  check_name(OUTPUT_FAILURE_REFUSED, "refused");
  check_name(OUTPUT_FAILURE_UNREACHABLE, "unreachable");
  check_name(OUTPUT_FAILURE_TIMEOUT, "timeout");
  check_name(OUTPUT_FAILURE_NO_CLOCK, "no_clock");
  check_name(OUTPUT_FAILURE_CANCELLED, "cancelled");
  check_name(OUTPUT_FAILURE_LOST, "lost");
  check_name((enum output_failure_reason)99, "none");
  // The value clients have always seen for a capacity failure is kept
  cases_run++;
  if (OUTPUT_FAILURE_CAPACITY != 1)
    {
      cases_failed++;
      printf("FAIL capacity is no longer 1\n");
    }
}

static void
check_retriable(enum output_failure_reason reason, bool want)
{
  cases_run++;
  if (output_failure_retriable(reason) == want)
    return;

  cases_failed++;
  printf("FAIL retriable %s: got %d, want %d\n", output_failure_name(reason), !want, want);
}

static void
test_retriable(void)
{
  // A connection or an answer that went wrong, or a session lost
  check_retriable(OUTPUT_FAILURE_NONE, true);
  check_retriable(OUTPUT_FAILURE_REFUSED, true);
  check_retriable(OUTPUT_FAILURE_UNREACHABLE, true);
  check_retriable(OUTPUT_FAILURE_TIMEOUT, true);
  check_retriable(OUTPUT_FAILURE_NO_CLOCK, true);
  check_retriable(OUTPUT_FAILURE_LOST, true);
  // A person, nobody, or our own encoder budget
  check_retriable(OUTPUT_FAILURE_PIN_REQUIRED, false);
  check_retriable(OUTPUT_FAILURE_PIN_INVALID, false);
  check_retriable(OUTPUT_FAILURE_CANCELLED, false);
  check_retriable(OUTPUT_FAILURE_CAPACITY, false);
}

int
main(void)
{
  test_reported();
  test_names();
  test_retriable();

  printf("%d cases, %d failed\n", cases_run, cases_failed);
  return cases_failed ? 1 : 0;
}
