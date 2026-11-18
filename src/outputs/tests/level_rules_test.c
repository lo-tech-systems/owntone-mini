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

// Checks the rules for the level a device is sent, with no daemon and no
// framework. From the top of the source tree:
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


int
main(int argc, char **argv)
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  level_send();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
