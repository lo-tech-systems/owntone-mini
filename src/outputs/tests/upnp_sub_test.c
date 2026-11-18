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

// Checks the life of a subscription to a renderer's volume events, driving
// the state machine with events and a clock of its own. From the top of the
// source tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/upnp_sub_test src/outputs/tests/upnp_sub_test.c src/upnp_sub.c
//   /tmp/upnp_sub_test
//
// Exits 0 when every case passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing case too). Also listed under
// check_PROGRAMS and TESTS in src/Makefile.am.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../upnp_sub.h"

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

#define LOCATION   "http://192.168.1.20:49152/description.xml"
#define EVENT_URL  "http://192.168.1.20:49152/upnp/event/rendercontrol1"
#define SID1       "uuid:11111111-1dd2-11b2-8000-0123456789ab"
#define SID2       "uuid:22222222-1dd2-11b2-8000-0123456789ab"

static struct upnp_sub s;
static struct upnp_sub_actions act;
static uint64_t now;

static void
step(enum upnp_sub_event ev, const struct upnp_sub_args *args)
{
  upnp_sub_step(&s, ev, args, now, &act);
}

static void
step_match(void)
{
  struct upnp_sub_args a = { .location = LOCATION };
  step(UPNP_EV_SSDP_MATCH, &a);
}

static void
step_described(void)
{
  struct upnp_sub_args a = { .event_url = EVENT_URL };
  step(UPNP_EV_DESCRIBED, &a);
}

static void
step_sub_ok(const char *sid, long timeout_s)
{
  struct upnp_sub_args a = { .sid = sid, .timeout_s = timeout_s };
  step(UPNP_EV_SUB_OK, &a);
}

static void
step_refused(int status)
{
  struct upnp_sub_args a = { .status = status };
  step(UPNP_EV_SUB_REFUSED, &a);
}

static bool
has(unsigned flag)
{
  return (act.flags & flag) != 0;
}

static long
timer_s(void)
{
  return has(UPNP_ACT_ARM_TIMER) ? (long)((act.timer_at_ms - now) / 1000) : -1;
}

// A fresh entry taken to following with SID1, granted 1800 s
static void
to_following(void)
{
  upnp_sub_init(&s);
  now = 1000000;
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step_described();
  step_sub_ok(SID1, 1800);
}


/* -------------------------------- Cases ----------------------------------- */

static void
first_search_to_following(void)
{
  case_begin("first_search_to_following");
  upnp_sub_init(&s);
  now = 1000;

  step(UPNP_EV_ENABLE, NULL);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  CHECK(has(UPNP_ACT_WANT_SEARCH));
  CHECK(has(UPNP_ACT_FOLLOW_STATE) && act.follow == UPNP_FOLLOW_SEARCHING);

  step_match();
  CHECK(s.state == UPNP_SUB_DESCRIBING);
  CHECK(act.flags == UPNP_ACT_GET_DESCRIPTION);
  CHECK(strcmp(s.location, LOCATION) == 0);

  step_described();
  CHECK(s.state == UPNP_SUB_SUBSCRIBING);
  CHECK(act.flags == UPNP_ACT_SUBSCRIBE);
  CHECK(strcmp(s.event_url, EVENT_URL) == 0);
  CHECK(upnp_sub_has_subscription(&s));

  step_sub_ok(SID1, 1800);
  CHECK(s.state == UPNP_SUB_FOLLOWING);
  CHECK(timer_s() == 900);
  CHECK(has(UPNP_ACT_FOLLOW_STATE) && act.follow == UPNP_FOLLOW_UPNP);
  CHECK(strcmp(s.sid, SID1) == 0);

  // Renewal, then following again with no state published
  now += 900000;
  step(UPNP_EV_TIMER, NULL);
  CHECK(s.state == UPNP_SUB_RENEWING);
  CHECK(act.flags == UPNP_ACT_RENEW);
  step_sub_ok(SID1, 1800);
  CHECK(s.state == UPNP_SUB_FOLLOWING);
  CHECK(timer_s() == 900);
  CHECK(!has(UPNP_ACT_FOLLOW_STATE));
  case_end();
}

static void
search_backoff_and_searching_once(void)
{
  case_begin("search_backoff_and_searching_once");
  upnp_sub_init(&s);
  now = 1000;

  step(UPNP_EV_ENABLE, NULL);
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  CHECK(s.state == UPNP_SUB_BACKOFF);
  CHECK(timer_s() == 60);
  CHECK(has(UPNP_ACT_SEARCH_GAVE_UP) && act.retry_s == 60);
  CHECK(has(UPNP_ACT_FOLLOW_STATE) && act.follow == UPNP_FOLLOW_NONE);

  step(UPNP_EV_TIMER, NULL);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  CHECK(has(UPNP_ACT_WANT_SEARCH));
  // A re-search from none publishes nothing
  CHECK(!has(UPNP_ACT_FOLLOW_STATE));

  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  CHECK(timer_s() == 300);
  CHECK(!has(UPNP_ACT_FOLLOW_STATE));
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  CHECK(timer_s() == 1800);
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  CHECK(timer_s() == 3600);
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  CHECK(timer_s() == 3600);

  // A reply to another entry's search is taken while waiting
  step_match();
  CHECK(s.state == UPNP_SUB_DESCRIBING);
  CHECK(has(UPNP_ACT_GET_DESCRIPTION));
  CHECK(has(UPNP_ACT_DISARM_TIMER));

  // Found: the next search, after a later loss, starts from the first wait
  step_described();
  step_sub_ok(SID1, 1800);
  CHECK(s.search_step == 0);
  case_end();
}

static void
address_change_resets_search(void)
{
  case_begin("address_change_resets_search");
  upnp_sub_init(&s);
  now = 1000;

  step(UPNP_EV_ENABLE, NULL);
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  CHECK(timer_s() == 300);

  step(UPNP_EV_ADDRESS_CHANGED, NULL);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  CHECK(has(UPNP_ACT_WANT_SEARCH));
  CHECK(has(UPNP_ACT_DISARM_TIMER));
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  CHECK(timer_s() == 60);
  case_end();
}

static void
address_change_while_following(void)
{
  case_begin("address_change_while_following");
  to_following();

  step(UPNP_EV_ADDRESS_CHANGED, NULL);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  CHECK(has(UPNP_ACT_UNSUBSCRIBE) && act.unsub_detached);
  CHECK(strcmp(act.unsub_sid, SID1) == 0);
  CHECK(has(UPNP_ACT_WANT_SEARCH));
  CHECK(has(UPNP_ACT_DISARM_TIMER));
  CHECK(!has(UPNP_ACT_CANCEL_REQUEST));
  CHECK(!upnp_sub_sid_ok(&s, SID1, now));
  // The old event URL is kept for the unsubscribe
  CHECK(strcmp(s.event_url, EVENT_URL) == 0);

  // While renewing the renewal is cancelled first
  to_following();
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_ADDRESS_CHANGED, NULL);
  CHECK(has(UPNP_ACT_CANCEL_REQUEST));
  CHECK(has(UPNP_ACT_UNSUBSCRIBE) && act.unsub_detached);

  // While describing or subscribing, only cancelled
  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step(UPNP_EV_ADDRESS_CHANGED, NULL);
  CHECK(has(UPNP_ACT_CANCEL_REQUEST));
  CHECK(!has(UPNP_ACT_UNSUBSCRIBE));
  CHECK(s.state == UPNP_SUB_SEARCHING);
  step_match();
  step_described();
  step(UPNP_EV_ADDRESS_CHANGED, NULL);
  CHECK(has(UPNP_ACT_CANCEL_REQUEST));
  CHECK(!has(UPNP_ACT_UNSUBSCRIBE));
  case_end();
}

static void
renewal_refused_then_resubscribed(void)
{
  case_begin("renewal_refused_then_resubscribed");
  to_following();
  step(UPNP_EV_NOTIFY, NULL);

  now += 900000;
  step(UPNP_EV_TIMER, NULL);
  step_refused(412);
  CHECK(s.state == UPNP_SUB_SUBSCRIBING);
  CHECK(act.flags == UPNP_ACT_SUBSCRIBE);
  // The old SID is still taken for a while
  CHECK(upnp_sub_sid_ok(&s, SID1, now + 29000));

  step_sub_ok(SID2, 1800);
  CHECK(s.state == UPNP_SUB_FOLLOWING);
  CHECK(upnp_sub_sid_ok(&s, SID2, now));
  CHECK(upnp_sub_sid_ok(&s, SID1, now + 29000));
  CHECK(!upnp_sub_sid_ok(&s, SID1, now + 31000));
  CHECK(!has(UPNP_ACT_FOLLOW_STATE));

  // 404 is taken the same way
  now += 900000;
  step(UPNP_EV_TIMER, NULL);
  step_refused(404);
  CHECK(s.state == UPNP_SUB_SUBSCRIBING);
  // No event since the last subscribe: the renderer may have dropped us
  CHECK(has(UPNP_ACT_WARN_DROPPED));
  case_end();
}

static void
renewal_refused_twice(void)
{
  case_begin("renewal_refused_twice");
  to_following();
  step(UPNP_EV_NOTIFY, NULL);
  step(UPNP_EV_TIMER, NULL);
  step_refused(412);
  CHECK(!has(UPNP_ACT_WARN_DROPPED));
  // The fresh subscribe is refused as well: wait
  step_refused(412);
  CHECK(s.state == UPNP_SUB_BACKOFF);
  CHECK(timer_s() == 5);
  // ... then subscribe, which is refused again: look for the renderer again
  step(UPNP_EV_TIMER, NULL);
  CHECK(s.state == UPNP_SUB_SUBSCRIBING);
  CHECK(act.flags == UPNP_ACT_SUBSCRIBE);
  step_refused(412);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  CHECK(has(UPNP_ACT_WANT_SEARCH));
  case_end();
}

static void
fresh_subscribe_refused_twice(void)
{
  case_begin("fresh_subscribe_refused_twice");
  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step_described();
  step_refused(412);
  CHECK(s.state == UPNP_SUB_BACKOFF);
  step(UPNP_EV_TIMER, NULL);
  CHECK(s.state == UPNP_SUB_SUBSCRIBING);
  step_refused(412);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  CHECK(has(UPNP_ACT_WANT_SEARCH));

  // A success between them resets the count
  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step_described();
  step_refused(412);
  step(UPNP_EV_TIMER, NULL);
  step_sub_ok(SID1, 1800);
  step(UPNP_EV_ADDRESS_CHANGED, NULL);
  step_match();
  step_described();
  step_refused(412);
  CHECK(s.state == UPNP_SUB_BACKOFF);
  case_end();
}

static void
error_ladder_to_search(void)
{
  case_begin("error_ladder_to_search");
  to_following();

  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_NET_ERROR, NULL);
  CHECK(s.state == UPNP_SUB_BACKOFF);
  CHECK(timer_s() == 5);
  step(UPNP_EV_TIMER, NULL);
  CHECK(s.state == UPNP_SUB_SUBSCRIBING);
  step_refused(503);
  CHECK(timer_s() == 30);
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_NET_ERROR, NULL);
  CHECK(timer_s() == 120);
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_NET_ERROR, NULL);
  CHECK(timer_s() == 600);
  // After the longest wait, search
  step(UPNP_EV_TIMER, NULL);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  CHECK(has(UPNP_ACT_WANT_SEARCH));
  // A row once followed shows none if the search then finds nothing
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  CHECK(has(UPNP_ACT_FOLLOW_STATE) && act.follow == UPNP_FOLLOW_NONE);

  // A success resets the ladder
  to_following();
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_NET_ERROR, NULL);
  step(UPNP_EV_TIMER, NULL);
  step_sub_ok(SID2, 1800);
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_NET_ERROR, NULL);
  CHECK(timer_s() == 5);
  case_end();
}

static void
describe_failure_searches_again(void)
{
  case_begin("describe_failure_searches_again");
  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step(UPNP_EV_DESCRIBE_FAILED, NULL);
  CHECK(s.state == UPNP_SUB_BACKOFF);
  CHECK(timer_s() == 5);
  step(UPNP_EV_TIMER, NULL);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  step_match();
  step(UPNP_EV_NET_ERROR, NULL);
  CHECK(timer_s() == 30);
  case_end();
}

static void
subscribe_answer_without_sid(void)
{
  case_begin("subscribe_answer_without_sid");
  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step_described();
  step_sub_ok("", 1800);
  CHECK(s.state == UPNP_SUB_BACKOFF);
  CHECK(timer_s() == 5);
  case_end();
}

static void
timeouts(void)
{
  case_begin("timeouts");
  CHECK(upnp_sub_renew_s(1800) == 900);
  CHECK(upnp_sub_renew_s(60) == 30);
  CHECK(upnp_sub_renew_s(10) == 30);
  CHECK(upnp_sub_renew_s(0) == 900);
  CHECK(upnp_sub_renew_s(-1) == 150);

  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step_described();
  step_sub_ok(SID1, 0);
  CHECK(timer_s() == 900);
  step(UPNP_EV_TIMER, NULL);
  step_sub_ok(SID1, -1);
  CHECK(timer_s() == 150);
  step(UPNP_EV_TIMER, NULL);
  step_sub_ok(SID1, 20);
  CHECK(timer_s() == 30);
  // A renewal answered with a new SID takes it
  step(UPNP_EV_TIMER, NULL);
  step_sub_ok(SID2, 1800);
  CHECK(upnp_sub_sid_ok(&s, SID2, now));
  CHECK(upnp_sub_sid_ok(&s, SID1, now));
  // ... and one with none keeps the old
  step(UPNP_EV_TIMER, NULL);
  step_sub_ok(NULL, 1800);
  CHECK(s.state == UPNP_SUB_FOLLOWING);
  CHECK(upnp_sub_sid_ok(&s, SID2, now));
  case_end();
}

static void
sid_checks(void)
{
  case_begin("sid_checks");
  upnp_sub_init(&s);
  CHECK(!upnp_sub_sid_ok(&s, SID1, now));
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step_described();
  // The first event may come before the subscribe's answer is read
  CHECK(upnp_sub_sid_ok(&s, SID1, now));
  CHECK(!upnp_sub_sid_ok(&s, "", now));
  CHECK(!upnp_sub_sid_ok(&s, NULL, now));
  step_sub_ok(SID1, 1800);
  CHECK(upnp_sub_sid_ok(&s, SID1, now));
  CHECK(!upnp_sub_sid_ok(&s, SID2, now));
  case_end();
}

static void
disable_from_each_state(void)
{
  case_begin("disable_from_each_state");

  // Searching and backoff: freed at once
  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step(UPNP_EV_DISABLE, NULL);
  CHECK(s.state == UPNP_SUB_IDLE);
  CHECK(has(UPNP_ACT_FREE));
  CHECK(has(UPNP_ACT_FOLLOW_STATE) && act.follow == UPNP_FOLLOW_NONE);

  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  step(UPNP_EV_DISABLE, NULL);
  CHECK(has(UPNP_ACT_FREE));
  CHECK(has(UPNP_ACT_DISARM_TIMER));
  CHECK(!has(UPNP_ACT_FOLLOW_STATE));

  // Describing and subscribing: the request is cancelled, then freed
  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step(UPNP_EV_DISABLE, NULL);
  CHECK(has(UPNP_ACT_CANCEL_REQUEST) && has(UPNP_ACT_FREE));
  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step_match();
  step_described();
  step(UPNP_EV_DISABLE, NULL);
  CHECK(has(UPNP_ACT_CANCEL_REQUEST) && has(UPNP_ACT_FREE));
  CHECK(!has(UPNP_ACT_UNSUBSCRIBE));

  // Following: unsubscribed, freed only when that is answered
  to_following();
  step(UPNP_EV_DISABLE, NULL);
  CHECK(s.state == UPNP_SUB_UNSUBSCRIBING);
  CHECK(has(UPNP_ACT_UNSUBSCRIBE) && !act.unsub_detached);
  CHECK(strcmp(act.unsub_sid, SID1) == 0);
  CHECK(has(UPNP_ACT_DISARM_TIMER));
  CHECK(!has(UPNP_ACT_FREE));
  CHECK(!has(UPNP_ACT_CANCEL_REQUEST));
  step(UPNP_EV_UNSUB_DONE, NULL);
  CHECK(s.state == UPNP_SUB_IDLE);
  CHECK(has(UPNP_ACT_FREE));
  CHECK(has(UPNP_ACT_FOLLOW_STATE) && act.follow == UPNP_FOLLOW_NONE);

  // Renewing: the renewal is cancelled, then unsubscribed
  to_following();
  step(UPNP_EV_TIMER, NULL);
  step(UPNP_EV_DISABLE, NULL);
  CHECK(has(UPNP_ACT_CANCEL_REQUEST) && has(UPNP_ACT_UNSUBSCRIBE));
  CHECK(s.state == UPNP_SUB_UNSUBSCRIBING);
  // A failed unsubscribe ends the same way
  step(UPNP_EV_NET_ERROR, NULL);
  CHECK(has(UPNP_ACT_FREE));

  // Idle: freed
  upnp_sub_init(&s);
  step(UPNP_EV_DISABLE, NULL);
  CHECK(has(UPNP_ACT_FREE));
  case_end();
}

static void
reenable_while_unsubscribing(void)
{
  case_begin("reenable_while_unsubscribing");
  to_following();
  step(UPNP_EV_DISABLE, NULL);
  step(UPNP_EV_ENABLE, NULL);
  CHECK(act.flags == 0);
  CHECK(s.state == UPNP_SUB_UNSUBSCRIBING);
  // Ignored while leaving
  step(UPNP_EV_ADDRESS_CHANGED, NULL);
  CHECK(act.flags == 0);
  step(UPNP_EV_UNSUB_DONE, NULL);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  CHECK(has(UPNP_ACT_WANT_SEARCH));
  CHECK(!has(UPNP_ACT_FREE));

  // Enabled then disabled again: freed
  to_following();
  step(UPNP_EV_DISABLE, NULL);
  step(UPNP_EV_ENABLE, NULL);
  step(UPNP_EV_DISABLE, NULL);
  step(UPNP_EV_UNSUB_DONE, NULL);
  CHECK(has(UPNP_ACT_FREE));
  case_end();
}

static void
late_subscribe_answer_is_unsubscribed(void)
{
  case_begin("late_subscribe_answer_is_unsubscribed");
  to_following();
  step(UPNP_EV_DISABLE, NULL);
  step_sub_ok(SID2, 1800);
  CHECK(has(UPNP_ACT_UNSUBSCRIBE) && act.unsub_detached);
  CHECK(strcmp(act.unsub_sid, SID2) == 0);
  CHECK(s.state == UPNP_SUB_UNSUBSCRIBING);

  upnp_sub_init(&s);
  step_sub_ok(SID2, 1800);
  CHECK(has(UPNP_ACT_UNSUBSCRIBE) && act.unsub_detached);
  CHECK(s.state == UPNP_SUB_IDLE);
  case_end();
}

static void
stray_events_are_ignored(void)
{
  case_begin("stray_events_are_ignored");
  to_following();
  step_match();
  CHECK(act.flags == 0);
  step(UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
  CHECK(act.flags == 0);
  step(UPNP_EV_ENABLE, NULL);
  CHECK(act.flags == 0);
  step(UPNP_EV_UNSUB_DONE, NULL);
  CHECK(act.flags == 0);
  step(UPNP_EV_NOTIFY, NULL);
  CHECK(act.flags == 0);
  CHECK(s.state == UPNP_SUB_FOLLOWING);

  upnp_sub_init(&s);
  step(UPNP_EV_ENABLE, NULL);
  step(UPNP_EV_TIMER, NULL);
  CHECK(act.flags == 0);
  CHECK(s.state == UPNP_SUB_SEARCHING);
  case_end();
}

int
main(int argc, char **argv)
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  first_search_to_following();
  search_backoff_and_searching_once();
  address_change_resets_search();
  address_change_while_following();
  renewal_refused_then_resubscribed();
  renewal_refused_twice();
  fresh_subscribe_refused_twice();
  error_ladder_to_search();
  describe_failure_searches_again();
  subscribe_answer_without_sid();
  timeouts();
  sid_checks();
  disable_from_each_state();
  reenable_while_unsubscribing();
  late_subscribe_answer_is_unsubscribed();
  stray_events_are_ignored();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
