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

// Checks which roles a device plays in a group of HomePods behind an Apple
// TV, with the Apple TV included and excluded, and how an enable request that
// carries the option is resolved, with no daemon and no framework. From the
// top of the source tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/tv_proxy_mode_test src/outputs/tests/tv_proxy_mode_test.c
//   /tmp/tv_proxy_mode_test
//
// Exits 0 when every case passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing case too). The program is also
// listed under check_PROGRAMS and TESTS in src/Makefile.am, so "make check"
// builds and runs it.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../outputs_group_flags.h"

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
roles_check(struct tv_proxy_roles r, bool follower, bool leader, bool leads_group)
{
  CHECK(r.follower == follower);
  CHECK(r.leader == leader);
  CHECK(r.leads_group == leads_group);
}

static void
ungrouped_device_has_no_roles(void)
{
  struct tv_proxy_flags f = { false, false, false, false };

  case_begin("ungrouped_device_has_no_roles");
  roles_check(tv_proxy_flags_resolve(f), false, false, false);
  case_end();
}

static void
stereo_pair_has_no_roles(void)
{
  // A pair's visible leader and its hidden member: grouped, no proxy tag
  struct tv_proxy_flags leader = { false, true, true, false };
  struct tv_proxy_flags member = { false, true, false, false };

  case_begin("stereo_pair_has_no_roles");
  roles_check(tv_proxy_flags_resolve(leader), false, false, false);
  roles_check(tv_proxy_flags_resolve(member), false, false, false);
  case_end();
}

static void
tv_included_gives_both_roles(void)
{
  struct tv_proxy_flags tv = { true, true, true, false };
  struct tv_proxy_flags pod = { true, true, false, false };

  case_begin("tv_included_gives_both_roles");
  roles_check(tv_proxy_flags_resolve(tv), false, true, true);
  roles_check(tv_proxy_flags_resolve(pod), true, false, false);
  case_end();
}

static void
tv_excluded_removes_both_roles_but_keeps_leadership(void)
{
  struct tv_proxy_flags tv = { true, true, true, true };
  struct tv_proxy_flags pod = { true, true, false, true };

  case_begin("tv_excluded_removes_both_roles_but_keeps_leadership");
  roles_check(tv_proxy_flags_resolve(tv), false, false, true);
  roles_check(tv_proxy_flags_resolve(pod), false, false, false);
  case_end();
}

static void
homepods_present_follows_leadership_not_the_exclusion(void)
{
  // A row reports has_homepods from leads_group: true for the Apple TV whether
  // or not it was left out of the session, false for everything else
  struct tv_proxy_flags tv = { true, true, true, false };
  struct tv_proxy_flags tv_left_out = { true, true, true, true };
  struct tv_proxy_flags pod = { true, true, false, false };
  struct tv_proxy_flags pair_leader = { false, true, true, false };
  struct tv_proxy_flags alone = { false, false, false, false };

  case_begin("homepods_present_follows_leadership_not_the_exclusion");
  CHECK(tv_proxy_flags_resolve(tv).leads_group);
  CHECK(tv_proxy_flags_resolve(tv_left_out).leads_group);
  CHECK(!tv_proxy_flags_resolve(pod).leads_group);
  CHECK(!tv_proxy_flags_resolve(pair_leader).leads_group);
  CHECK(!tv_proxy_flags_resolve(alone).leads_group);
  case_end();
}

static void
leaderless_group_member_has_no_roles(void)
{
  // The Apple TV is gone: the tag is cleared on the members, and the
  // exclusion, which belonged to the Apple TV, no longer applies
  struct tv_proxy_flags pod = { false, true, false, false };

  case_begin("leaderless_group_member_has_no_roles");
  roles_check(tv_proxy_flags_resolve(pod), false, false, false);
  case_end();
}

static void
roles_return_when_the_tv_is_included_again(void)
{
  struct tv_proxy_flags pod = { true, true, false, true };

  case_begin("roles_return_when_the_tv_is_included_again");
  roles_check(tv_proxy_flags_resolve(pod), false, false, false);
  pod.leader_speakers_only = false;
  roles_check(tv_proxy_flags_resolve(pod), true, false, false);
  case_end();
}

static void
enable_without_the_option_leaves_the_flag(void)
{
  struct tv_proxy_enable e;

  case_begin("enable_without_the_option_leaves_the_flag");
  e = tv_proxy_enable_resolve(true, -1, false, true);
  CHECK(e.refusal == 0 && !e.set_flag);
  e = tv_proxy_enable_resolve(true, -1, true, false);
  CHECK(e.refusal == 0 && !e.set_flag);
  case_end();
}

static void
enable_sets_the_flag_from_the_option(void)
{
  struct tv_proxy_enable e;

  case_begin("enable_sets_the_flag_from_the_option");
  e = tv_proxy_enable_resolve(true, 0, false, false);
  CHECK(e.refusal == 0 && e.set_flag && e.flag);
  e = tv_proxy_enable_resolve(true, 1, false, true);
  CHECK(e.refusal == 0 && e.set_flag && !e.flag);
  // Repeating the current option on a selected output is not a change
  e = tv_proxy_enable_resolve(true, 0, true, true);
  CHECK(e.refusal == 0 && e.set_flag && e.flag);
  case_end();
}

static void
enable_refuses_a_change_while_selected(void)
{
  struct tv_proxy_enable e;

  case_begin("enable_refuses_a_change_while_selected");
  e = tv_proxy_enable_resolve(true, 0, true, false);
  CHECK(e.refusal == -4 && !e.set_flag);
  e = tv_proxy_enable_resolve(true, 1, true, true);
  CHECK(e.refusal == -4 && !e.set_flag);
  case_end();
}

static void
enable_refuses_speakers_only_without_a_group(void)
{
  struct tv_proxy_enable e;

  case_begin("enable_refuses_speakers_only_without_a_group");
  e = tv_proxy_enable_resolve(false, 0, false, false);
  CHECK(e.refusal == -5 && !e.set_flag);
  // Whatever the output's state, and a stale exclusion stays as it was
  e = tv_proxy_enable_resolve(false, 0, true, true);
  CHECK(e.refusal == -5 && !e.set_flag);
  case_end();
}

static void
enable_without_a_group_clears_the_exclusion(void)
{
  struct tv_proxy_enable e;

  case_begin("enable_without_a_group_clears_the_exclusion");
  e = tv_proxy_enable_resolve(false, -1, false, true);
  CHECK(e.refusal == 0 && e.set_flag && !e.flag);
  e = tv_proxy_enable_resolve(false, 1, true, true);
  CHECK(e.refusal == 0 && e.set_flag && !e.flag);
  case_end();
}

static void
pair_memory_needs_every_member_on_the_issued_group(void)
{
  struct stereo_leader_memory m;

  case_begin("pair_memory_needs_every_member_on_the_issued_group");

  // Both members on the group we issued, the last leader present
  m = (struct stereo_leader_memory){ false, true, 2, 2 };
  CHECK(stereo_leader_memory_applies(m));

  // One of the two on it: another sender's group or a stale advertisement
  m = (struct stereo_leader_memory){ false, true, 2, 1 };
  CHECK(!stereo_leader_memory_applies(m));

  // Neither on it
  m = (struct stereo_leader_memory){ false, true, 2, 0 };
  CHECK(!stereo_leader_memory_applies(m));

  // A pair with one member present, which is on it
  m = (struct stereo_leader_memory){ false, true, 1, 1 };
  CHECK(stereo_leader_memory_applies(m));

  case_end();
}

static void
pair_memory_never_overrides_a_known_leader(void)
{
  struct stereo_leader_memory m;

  case_begin("pair_memory_never_overrides_a_known_leader");

  m = (struct stereo_leader_memory){ true, true, 2, 2 };
  CHECK(!stereo_leader_memory_applies(m));
  m = (struct stereo_leader_memory){ true, false, 0, 0 };
  CHECK(!stereo_leader_memory_applies(m));

  case_end();
}

static void
pair_memory_needs_the_last_leader_present(void)
{
  struct stereo_leader_memory m;

  case_begin("pair_memory_needs_the_last_leader_present");

  // The member that led last has gone, or never had a session of ours
  m = (struct stereo_leader_memory){ false, false, 2, 2 };
  CHECK(!stereo_leader_memory_applies(m));
  m = (struct stereo_leader_memory){ false, false, 2, 0 };
  CHECK(!stereo_leader_memory_applies(m));

  // No members at all
  m = (struct stereo_leader_memory){ false, true, 0, 0 };
  CHECK(!stereo_leader_memory_applies(m));

  case_end();
}

int
main(int argc, char **argv)
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  ungrouped_device_has_no_roles();
  stereo_pair_has_no_roles();
  tv_included_gives_both_roles();
  tv_excluded_removes_both_roles_but_keeps_leadership();
  homepods_present_follows_leadership_not_the_exclusion();
  leaderless_group_member_has_no_roles();
  roles_return_when_the_tv_is_included_again();
  enable_without_the_option_leaves_the_flag();
  enable_sets_the_flag_from_the_option();
  enable_refuses_a_change_while_selected();
  enable_refuses_speakers_only_without_a_group();
  enable_without_a_group_clears_the_exclusion();
  pair_memory_needs_every_member_on_the_issued_group();
  pair_memory_never_overrides_a_known_leader();
  pair_memory_needs_the_last_leader_present();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
