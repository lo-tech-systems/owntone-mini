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

// Checks the rules for the level a device is sent and for what a level it
// reports means, with no daemon and no framework. From the top of the source
// tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/level_rules_test src/outputs/tests/level_rules_test.c src/level_rules.c
//   /tmp/level_rules_test
//
// Exits 0 when every case passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing case too). Also listed under
// check_PROGRAMS and TESTS in src/Makefile.am.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../level_rules.h"

static int cases_run;
static int cases_failed;
static bool verbose;
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


/* ------------------------- The level put on the wire ---------------------- */

static void
level_send(void)
{
  static const struct {
    const char *name;
    bool silent;
    int volume;
    int sent;
    int to_send;
    bool stale;
  } rows[] = {
    { "quiet speaker, in step",                       false,   0,   0,   0, false },
    { "in step",                                      false,  30,  30,  30, false },
    { "level changed during the start",               false,  40,  30,  40, true  },
    { "unmuted during the start",                     false,  30,   0,  30, true  },
    { "top",                                          false, 100, 100, 100, false },
    { "silent, in step",                              true,   30,   0,   0, false },
    { "muted during the start",                       true,   30,  30,   0, true  },
    { "silent at 0",                                  true,    0,   0,   0, false },
    { "silent, wire at the lowest level",             true,    1,   1,   0, true  },
    { "silent at the top",                            true,  100,   0,   0, false },
  };
  size_t i;

  for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
    {
      case_begin(rows[i].name);
      CHECK(level_to_send(rows[i].silent, rows[i].volume) == rows[i].to_send);
      CHECK(level_wire_stale(rows[i].silent, rows[i].volume, rows[i].sent) == rows[i].stale);
      case_end();
    }
}


/* --------------------- What a level the device sends means ---------------- */

#define W      2000
#define NOW    100000
#define IN     (NOW - 1999)  // inside the window
#define EDGE   (NOW - 2000)  // just outside it
#define OUT    (NOW - 5000)
#define NEVER  0
#define LATER  (NOW + 1)     // a stamp later than now

static void
report_classify(void)
{
  static const struct {
    const char *name;
    bool asks;
    bool silent;
    int level;
    uint64_t sent;
    bool restores;
    uint64_t wake;
    enum level_report expect;
  } rows[] = {
    { "request on a silent speaker",                    true,  true,  30, IN,    false, IN,    LEVEL_REPORT_WAKE },
    { "request computed to 0 still wakes",              true,  true,   0, OUT,   false, NEVER, LEVEL_REPORT_WAKE },
    { "request on a silent speaker, nothing sent",      true,  true,  35, NEVER, false, NEVER, LEVEL_REPORT_WAKE },
    { "a press just after a resume is applied",         true,  false, 35, IN,    true,  NEVER, LEVEL_REPORT_RECORD },
    { "a press soon after a send is applied",           true,  false, 35, IN,    false, NEVER, LEVEL_REPORT_RECORD },
    { "a press to 0 is applied",                        true,  false,  0, OUT,   false, NEVER, LEVEL_REPORT_RECORD },
    { "silent: the answer at once",                     false, true,   0, NOW,   false, NEVER, LEVEL_REPORT_ABSORB },
    { "silent: -144 answered as 1",                     false, true,   1, IN,    false, NEVER, LEVEL_REPORT_ABSORB },
    { "silent: any answer in the window",               false, true,  30, IN,    false, NEVER, LEVEL_REPORT_ABSORB },
    { "silent: the answer rule before the wake mark",   false, true,  30, IN,    false, IN,    LEVEL_REPORT_ABSORB },
    { "silent: 0 at the window edge",                   false, true,   0, EDGE,  false, NEVER, LEVEL_REPORT_ABSORB },
    { "silent: 0 after the window",                     false, true,   0, OUT,   false, NEVER, LEVEL_REPORT_ABSORB },
    { "silent: 0 with nothing sent",                    false, true,   0, NEVER, false, NEVER, LEVEL_REPORT_ABSORB },
    { "silent: 0 before the wake mark",                 false, true,   0, OUT,   false, IN,    LEVEL_REPORT_ABSORB },
    { "silent: the lowest level wakes",                 false, true,   1, OUT,   false, NEVER, LEVEL_REPORT_WAKE },
    { "silent: a level wakes",                          false, true,  30, OUT,   false, NEVER, LEVEL_REPORT_WAKE },
    { "silent: a level wakes, nothing sent",            false, true,  30, NEVER, false, NEVER, LEVEL_REPORT_WAKE },
    { "silent: a level at the window edge wakes",       false, true,  30, EDGE,  false, NEVER, LEVEL_REPORT_WAKE },
    { "silent: a later step of a forwarded turn",       false, true,  30, OUT,   false, IN,    LEVEL_REPORT_ABSORB },
    { "silent: at the wake window edge",                false, true,  30, OUT,   false, EDGE,  LEVEL_REPORT_WAKE },
    { "silent: a new turn",                             false, true,  30, OUT,   false, OUT,   LEVEL_REPORT_WAKE },
    { "silent: settling needs not silent",              false, true,  30, OUT,   true,  NEVER, LEVEL_REPORT_WAKE },
    { "settling after a restore",                       false, false, 30, IN,    true,  NEVER, LEVEL_REPORT_ABSORB },
    { "settling: a level reached from silence",         false, false, 11, IN,    true,  NEVER, LEVEL_REPORT_ABSORB },
    { "settling: 0",                                    false, false,  0, IN,    true,  NEVER, LEVEL_REPORT_ABSORB },
    { "settling window edge",                           false, false, 30, EDGE,  true,  NEVER, LEVEL_REPORT_RECORD },
    { "after the settling window",                      false, false, 30, OUT,   true,  NEVER, LEVEL_REPORT_RECORD },
    { "the echo and answer rules decide",               false, false, 30, IN,    false, NEVER, LEVEL_REPORT_RECORD },
    { "nothing sent",                                   false, false, 30, NEVER, false, NEVER, LEVEL_REPORT_RECORD },
    { "no send, no window",                             false, false, 30, NEVER, true,  NEVER, LEVEL_REPORT_RECORD },
    { "the wake mark means nothing once not silent",    false, false, 30, OUT,   false, IN,    LEVEL_REPORT_RECORD },
    { "a stamp later than now is outside",              false, true,  30, LATER, false, NEVER, LEVEL_REPORT_WAKE },
  };
  size_t i;

  for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
    {
      case_begin(rows[i].name);
      CHECK(level_report_classify(rows[i].silent, rows[i].asks, rows[i].level, rows[i].sent, rows[i].restores,
				  rows[i].wake, NOW, W) == rows[i].expect);
      case_end();
    }
}


int
main(int argc, char **argv)
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  level_send();
  report_classify();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
