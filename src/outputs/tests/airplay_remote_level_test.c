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

// Checks the level a receiver's report of its own volume ("dvlc", a fraction
// of its range) turns into, in dB and then back on the 0 to 100 scale the way
// a level reported over DACP is, with no daemon and no framework. From the top
// of the source tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/airplay_remote_level_test src/outputs/tests/airplay_remote_level_test.c -lm
//   /tmp/airplay_remote_level_test
//
// Exits 0 when every case passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing case too). The program is also
// listed under check_PROGRAMS and TESTS in src/Makefile.am, so "make check"
// builds and runs it.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../airplay_remote_level.h"

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

static bool
db_is(double fraction, double db)
{
  return fabs(airplay_remote_level_db(fraction) - db) < 1e-6;
}

// The level the daemon records for a reported dB value, as the AirPlay
// backend converts one with the speaker's full range in use: through the
// text the report is passed on as, in single precision, rounded to the
// nearest step and never below 1 above -30 dB
static int
level_of(double fraction)
{
  char volstr[32];
  float db;
  float volume;

  snprintf(volstr, sizeof(volstr), "%.6f", airplay_remote_level_db(fraction));
  db = (float)atof(volstr);
  if (db <= -30.0)
    return 0;

  volume = 100.0 * (db / 30.0 + 1.0);
  volume = (int)(volume + 0.5);
  return volume < 1 ? 1 : (volume > 100 ? 100 : (int)volume);
}

static void
reported_fractions(void)
{
  case_begin("reported_fractions");
  // What an Edifier MS50A reported while its knob was turned, beside the
  // level its UPnP renderer showed (one above ours): 12, 3, 18, 25 and 31
  CHECK(level_of(0.1150000254313151) == 11);
  CHECK(level_of(0.025000000000000001) == 2);
  CHECK(level_of(0.17499999999999999) == 17);
  CHECK(level_of(0.24500001271565755) == 24);
  CHECK(level_of(0.30499998728434247) == 30);
  // Set to 17, it reports 0.175: the level sent comes back unchanged
  CHECK(db_is(0.17499999999999999, -24.9));
  case_end();
}

static void
whole_steps(void)
{
  int i;

  case_begin("whole_steps");
  // A receiver that reports a whole step, a little under or exactly
  for (i = 1; i <= 100; i++)
    {
      CHECK(level_of(i / 100.0) == i);
      CHECK(level_of(i / 100.0 - 1e-9) == i);
    }
  case_end();
}

// A quiet NaN, made from its bits at run time: a NaN constant is undefined
// under -ffinite-math-only (part of -Ofast), and the compiler may drop the
// code that uses one, the rest of the program with it
static double
not_a_number(void)
{
  volatile uint64_t bits = UINT64_C(0x7ff8000000000000);
  uint64_t copy = bits;
  double value;

  memcpy(&value, &copy, sizeof(value));
  return value;
}

static void
ends(void)
{
  case_begin("ends");
  // Silent
  CHECK(db_is(0.0, -144.0));
  CHECK(db_is(-0.2, -144.0));
  CHECK(db_is(not_a_number(), -144.0));
  CHECK(!airplay_remote_level_is_number(not_a_number()));
  CHECK(airplay_remote_level_is_number(0.0));
  CHECK(airplay_remote_level_is_number(0.5));
  CHECK(level_of(0.0) == 0);
  // Still playing, however quietly
  CHECK(db_is(0.004, -29.7));
  CHECK(level_of(0.004) == 1);
  // The top, and beyond it
  CHECK(db_is(1.0, 0.0));
  CHECK(db_is(1.7, 0.0));
  CHECK(level_of(1.0) == 100);
  case_end();
}

int
main(int argc, char **argv)
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  reported_fractions();
  whole_steps();
  ends();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
