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

// Checks the rules of the buffered anchor state machine with no daemon, no
// event loop and no framework. From the top of the source tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -I src/outputs -o /tmp/airplay_anchor_test src/outputs/tests/airplay_anchor_test.c src/outputs/airplay_anchor.c
//   /tmp/airplay_anchor_test
//
// Exits 0 when every case passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing case too). The program is also
// listed under check_PROGRAMS in src/Makefile.am, so "make check" builds it
// (it is not run by "make check"), and a plain "make" or "make install" does
// not build it.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../airplay_anchor.h"

#define RETRIES_MAX 12

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


/* ----------------------------- Transition rows ---------------------------- */

struct tcase
{
  const char *name;
  enum airplay_anchor_state from;
  enum airplay_anchor_event ev;
  int ctx_retries;
  enum airplay_anchor_hold_reason ctx_reason;
  enum airplay_anchor_hold_reason ctx_held_for;
  int rc;
  enum airplay_anchor_state to;
  unsigned actions;
  int retries;
  enum airplay_anchor_hold_reason reason;
};

#define NONE_R   ANCHOR_HOLD_NONE
#define OWN_R    ANCHOR_HOLD_OWN_AGE
#define SIB_R    ANCHOR_HOLD_SIBLING
#define A_FLUSH  (ANCHOR_ACT_TIMER_DEL | ANCHOR_ACT_LINE_RESET_IF_UNHELD)

static const struct tcase tcases[] =
{
  // name, from, event, ctx budget, ctx reason, ctx held_for, rc, to, actions, budget, reason
  { "t01_none_attach",                         ANCHOR_NONE,       ANCHOR_EV_ATTACH,   12, NONE_R, NONE_R, 0, ANCHOR_PENDING,    0,                                                   12, NONE_R },
  { "t02_none_held",                           ANCHOR_NONE,       ANCHOR_EV_HELD,     12, NONE_R, OWN_R,  -1, ANCHOR_NONE,      0,                                                   12, NONE_R },
  { "t03_none_sent",                           ANCHOR_NONE,       ANCHOR_EV_SENT,     12, NONE_R, NONE_R, -1, ANCHOR_NONE,      0,                                                   12, NONE_R },
  { "t04_none_accepted",                       ANCHOR_NONE,       ANCHOR_EV_ACCEPTED, 12, NONE_R, NONE_R, -1, ANCHOR_NONE,      0,                                                   12, NONE_R },
  { "t05_none_rejected",                       ANCHOR_NONE,       ANCHOR_EV_REJECTED, 12, NONE_R, NONE_R, -1, ANCHOR_NONE,      0,                                                   12, NONE_R },
  { "t06_none_flush",                          ANCHOR_NONE,       ANCHOR_EV_FLUSH,    12, NONE_R, NONE_R, -1, ANCHOR_NONE,      0,                                                   12, NONE_R },

  { "t07_pending_attach",                      ANCHOR_PENDING,    ANCHOR_EV_ATTACH,   12, NONE_R, NONE_R, -1, ANCHOR_PENDING,   0,                                                   12, NONE_R },
  { "t08_pending_held_sibling",                ANCHOR_PENDING,    ANCHOR_EV_HELD,     12, NONE_R, SIB_R,  0, ANCHOR_PENDING,    0,                                                   12, SIB_R  },
  { "t08x_pending_held_no_reason",             ANCHOR_PENDING,    ANCHOR_EV_HELD,     12, NONE_R, NONE_R, -1, ANCHOR_PENDING,   0,                                                   12, NONE_R },
  { "t09_pending_held_own_age_from_none",      ANCHOR_PENDING,    ANCHOR_EV_HELD,     12, NONE_R, OWN_R,  0, ANCHOR_PENDING,    0,                                                   12, OWN_R  },
  { "t10_pending_held_own_age_keeps_sibling",  ANCHOR_PENDING,    ANCHOR_EV_HELD,     12, SIB_R,  OWN_R,  0, ANCHOR_PENDING,    0,                                                   12, SIB_R  },
  { "t11_pending_sent",                        ANCHOR_PENDING,    ANCHOR_EV_SENT,     12, OWN_R,  NONE_R, 0, ANCHOR_IN_FLIGHT,  ANCHOR_ACT_MARK_SENT,                                12, NONE_R },
  { "t12_pending_accepted_ignored",            ANCHOR_PENDING,    ANCHOR_EV_ACCEPTED, 5,  NONE_R, NONE_R, 0, ANCHOR_PENDING,    ANCHOR_ACT_IGNORED,                                  5,  NONE_R },
  { "t13_pending_rejected_ignored",            ANCHOR_PENDING,    ANCHOR_EV_REJECTED, 5,  NONE_R, NONE_R, 0, ANCHOR_PENDING,    ANCHOR_ACT_IGNORED,                                  5,  NONE_R },
  { "t14_pending_flush",                       ANCHOR_PENDING,    ANCHOR_EV_FLUSH,    5,  SIB_R,  NONE_R, 0, ANCHOR_PENDING,    A_FLUSH,                                             5,  NONE_R },

  { "t15_in_flight_attach",                    ANCHOR_IN_FLIGHT,  ANCHOR_EV_ATTACH,   12, NONE_R, NONE_R, -1, ANCHOR_IN_FLIGHT, 0,                                                   12, NONE_R },
  { "t16_in_flight_held",                      ANCHOR_IN_FLIGHT,  ANCHOR_EV_HELD,     12, NONE_R, OWN_R,  -1, ANCHOR_IN_FLIGHT, 0,                                                   12, NONE_R },
  { "t17_in_flight_sent",                      ANCHOR_IN_FLIGHT,  ANCHOR_EV_SENT,     12, NONE_R, NONE_R, -1, ANCHOR_IN_FLIGHT, 0,                                                   12, NONE_R },
  { "t18_in_flight_accepted",                  ANCHOR_IN_FLIGHT,  ANCHOR_EV_ACCEPTED, 3,  NONE_R, NONE_R, 0, ANCHOR_CONFIRMED,  ANCHOR_ACT_TIMER_DEL,                                RETRIES_MAX, NONE_R },
  { "t19_in_flight_rejected_retry",            ANCHOR_IN_FLIGHT,  ANCHOR_EV_REJECTED, 12, NONE_R, NONE_R, 0, ANCHOR_RETRY_WAIT, ANCHOR_ACT_TIMER_ARM | ANCHOR_ACT_LINE_DROP_IF_UNHELD, 11, NONE_R },
  { "t19_in_flight_rejected_retry_last",       ANCHOR_IN_FLIGHT,  ANCHOR_EV_REJECTED, 1,  NONE_R, NONE_R, 0, ANCHOR_RETRY_WAIT, ANCHOR_ACT_TIMER_ARM | ANCHOR_ACT_LINE_DROP_IF_UNHELD, 0,  NONE_R },
  { "t20_in_flight_rejected_final",            ANCHOR_IN_FLIGHT,  ANCHOR_EV_REJECTED, 0,  NONE_R, NONE_R, 0, ANCHOR_NONE,       ANCHOR_ACT_ABORT,                                    0,  NONE_R },
  { "t21_in_flight_flush",                     ANCHOR_IN_FLIGHT,  ANCHOR_EV_FLUSH,    7,  NONE_R, NONE_R, 0, ANCHOR_PENDING,    A_FLUSH,                                             7,  NONE_R },

  { "t22_retry_wait_attach",                   ANCHOR_RETRY_WAIT, ANCHOR_EV_ATTACH,   9,  NONE_R, NONE_R, -1, ANCHOR_RETRY_WAIT, 0,                                                  9,  NONE_R },
  { "t23_retry_wait_held",                     ANCHOR_RETRY_WAIT, ANCHOR_EV_HELD,     9,  NONE_R, OWN_R,  -1, ANCHOR_RETRY_WAIT, 0,                                                  9,  NONE_R },
  { "t24_retry_wait_sent",                     ANCHOR_RETRY_WAIT, ANCHOR_EV_SENT,     9,  NONE_R, NONE_R, 0, ANCHOR_IN_FLIGHT,  ANCHOR_ACT_MARK_SENT,                                9,  NONE_R },
  { "t25_retry_wait_accepted_ignored",         ANCHOR_RETRY_WAIT, ANCHOR_EV_ACCEPTED, 9,  NONE_R, NONE_R, 0, ANCHOR_RETRY_WAIT, ANCHOR_ACT_IGNORED,                                  9,  NONE_R },
  { "t26_retry_wait_rejected_ignored",         ANCHOR_RETRY_WAIT, ANCHOR_EV_REJECTED, 9,  NONE_R, NONE_R, 0, ANCHOR_RETRY_WAIT, ANCHOR_ACT_IGNORED,                                  9,  NONE_R },
  { "t27_retry_wait_flush",                    ANCHOR_RETRY_WAIT, ANCHOR_EV_FLUSH,    9,  NONE_R, NONE_R, 0, ANCHOR_PENDING,    A_FLUSH,                                             9,  NONE_R },

  { "t28_confirmed_attach",                    ANCHOR_CONFIRMED,  ANCHOR_EV_ATTACH,   12, NONE_R, NONE_R, -1, ANCHOR_CONFIRMED, 0,                                                   12, NONE_R },
  { "t29_confirmed_held",                      ANCHOR_CONFIRMED,  ANCHOR_EV_HELD,     12, NONE_R, OWN_R,  -1, ANCHOR_CONFIRMED, 0,                                                   12, NONE_R },
  { "t30_confirmed_sent",                      ANCHOR_CONFIRMED,  ANCHOR_EV_SENT,     12, NONE_R, NONE_R, -1, ANCHOR_CONFIRMED, 0,                                                   12, NONE_R },
  { "t31_confirmed_accepted_ignored",          ANCHOR_CONFIRMED,  ANCHOR_EV_ACCEPTED, 12, NONE_R, NONE_R, 0, ANCHOR_CONFIRMED,  ANCHOR_ACT_IGNORED,                                  12, NONE_R },
  { "t32_confirmed_rejected_ignored",          ANCHOR_CONFIRMED,  ANCHOR_EV_REJECTED, 12, NONE_R, NONE_R, 0, ANCHOR_CONFIRMED,  ANCHOR_ACT_IGNORED,                                  12, NONE_R },
  { "t33_confirmed_flush",                     ANCHOR_CONFIRMED,  ANCHOR_EV_FLUSH,    12, NONE_R, NONE_R, 0, ANCHOR_PENDING,    A_FLUSH,                                             12, NONE_R },
};

static void
run_tcase(const struct tcase *tc)
{
  struct airplay_anchor_ctx ctx;
  struct airplay_anchor_result res;
  int rc;

  case_begin(tc->name);

  ctx.retries_left = tc->ctx_retries;
  ctx.hold_reason = tc->ctx_reason;
  ctx.held_for = tc->ctx_held_for;

  // Poison the result: the function must fill every field
  memset(&res, 0xa5, sizeof(res));

  rc = airplay_anchor_transition(tc->from, tc->ev, &ctx, RETRIES_MAX, &res);

  CHECK(rc == tc->rc);
  CHECK(res.state == tc->to);
  CHECK(res.actions == tc->actions);
  CHECK(res.retries_left == tc->retries);
  CHECK(res.hold_reason == tc->reason);

  case_end();
}


/* --------------------------------- Sweep ---------------------------------- */

// For every state and event: whether the pair is legal, and the states and
// actions a legal result may have. This is written out separately from the
// cases above so that a cell added or changed later is noticed.
#define N_STATES 5
#define N_EVENTS 6

static const bool legal[N_STATES][N_EVENTS] =
{
  // ATTACH HELD   SENT   ACCEPTED REJECTED FLUSH
  { true,  false, false, false,   false,   false },  // NONE
  { false, true,  true,  true,    true,    true  },  // PENDING
  { false, false, false, true,    true,    true  },  // IN_FLIGHT
  { false, false, true,  true,    true,    true  },  // RETRY_WAIT
  { false, false, false, true,    true,    true  },  // CONFIRMED
};

static unsigned
allowed_states(enum airplay_anchor_state from, enum airplay_anchor_event ev)
{
  switch (ev)
    {
      case ANCHOR_EV_ATTACH:
	return 1u << ANCHOR_PENDING;
      case ANCHOR_EV_HELD:
	return 1u << ANCHOR_PENDING;
      case ANCHOR_EV_SENT:
	return 1u << ANCHOR_IN_FLIGHT;
      case ANCHOR_EV_ACCEPTED:
	return (from == ANCHOR_IN_FLIGHT) ? 1u << ANCHOR_CONFIRMED : 1u << from;
      case ANCHOR_EV_REJECTED:
	return (from == ANCHOR_IN_FLIGHT) ? (1u << ANCHOR_RETRY_WAIT) | (1u << ANCHOR_NONE) : 1u << from;
      case ANCHOR_EV_FLUSH:
	return 1u << ANCHOR_PENDING;
    }

  return 0;
}

static unsigned
allowed_actions(enum airplay_anchor_state from, enum airplay_anchor_event ev)
{
  switch (ev)
    {
      case ANCHOR_EV_ATTACH:
      case ANCHOR_EV_HELD:
	return 0;
      case ANCHOR_EV_SENT:
	return ANCHOR_ACT_MARK_SENT;
      case ANCHOR_EV_ACCEPTED:
	return (from == ANCHOR_IN_FLIGHT) ? ANCHOR_ACT_TIMER_DEL : ANCHOR_ACT_IGNORED;
      case ANCHOR_EV_REJECTED:
	return (from == ANCHOR_IN_FLIGHT) ? (ANCHOR_ACT_TIMER_ARM | ANCHOR_ACT_LINE_DROP_IF_UNHELD | ANCHOR_ACT_ABORT) : ANCHOR_ACT_IGNORED;
      case ANCHOR_EV_FLUSH:
	return A_FLUSH;
    }

  return 0;
}

static void
t_sweep_all_cells(void)
{
  static const int budgets[] = { 0, 1 };
  static const enum airplay_anchor_hold_reason reasons[] = { ANCHOR_HOLD_NONE, ANCHOR_HOLD_SIBLING };
  struct airplay_anchor_ctx ctx;
  struct airplay_anchor_result res;
  int from, ev, b, r, rc;

  case_begin("t_sweep_all_cells");

  for (from = 0; from < N_STATES; from++)
    for (ev = 0; ev < N_EVENTS; ev++)
      for (b = 0; b < 2; b++)
	for (r = 0; r < 2; r++)
	  {
	    ctx.retries_left = budgets[b];
	    ctx.hold_reason = reasons[r];
	    ctx.held_for = ANCHOR_HOLD_OWN_AGE;

	    rc = airplay_anchor_transition(from, ev, &ctx, RETRIES_MAX, &res);

	    CHECK((rc == 0) == legal[from][ev]);
	    CHECK(rc == 0 || rc == -1);

	    if (rc != 0)
	      {
		// An illegal pair changes nothing
		CHECK(res.state == (enum airplay_anchor_state)from);
		CHECK(res.actions == 0);
		CHECK(res.retries_left == ctx.retries_left);
		CHECK(res.hold_reason == ctx.hold_reason);
		continue;
	      }

	    CHECK(((1u << res.state) & allowed_states(from, ev)) != 0);
	    CHECK((res.actions & ~allowed_actions(from, ev)) == 0);

	    // The retry timer is armed only on entry to the retry wait
	    CHECK(((res.actions & ANCHOR_ACT_TIMER_ARM) != 0) == (res.state == ANCHOR_RETRY_WAIT && from != ANCHOR_RETRY_WAIT));

	    // The budget is never negative and never above the maximum
	    CHECK(res.retries_left >= 0 && res.retries_left <= RETRIES_MAX);

	    // The budget changes only on an accepted or rejected in-flight anchor
	    if (!(from == ANCHOR_IN_FLIGHT && (ev == ANCHOR_EV_ACCEPTED || ev == ANCHOR_EV_REJECTED)))
	      CHECK(res.retries_left == ctx.retries_left);
	  }

  case_end();
}


/* ------------------------------- Predicates ------------------------------- */

static void
p_holds_line(void)
{
  case_begin("p_holds_line");

  CHECK(!airplay_anchor_holds_line(ANCHOR_NONE));
  CHECK(!airplay_anchor_holds_line(ANCHOR_PENDING));
  CHECK(airplay_anchor_holds_line(ANCHOR_IN_FLIGHT));
  CHECK(!airplay_anchor_holds_line(ANCHOR_RETRY_WAIT));
  CHECK(airplay_anchor_holds_line(ANCHOR_CONFIRMED));

  case_end();
}

static void
p_within_hold_in_flight_inside(void)
{
  case_begin("p_within_hold_in_flight_inside");
  CHECK(airplay_anchor_within_hold(ANCHOR_IN_FLIGHT, 1000, 1150, 200));
  CHECK(airplay_anchor_within_hold(ANCHOR_IN_FLIGHT, 1000, 1000, 200));
  case_end();
}

static void
p_within_hold_in_flight_at_bound(void)
{
  case_begin("p_within_hold_in_flight_at_bound");
  CHECK(!airplay_anchor_within_hold(ANCHOR_IN_FLIGHT, 1000, 1200, 200));
  CHECK(!airplay_anchor_within_hold(ANCHOR_IN_FLIGHT, 1000, 1500, 200));
  case_end();
}

static void
p_within_hold_other_states(void)
{
  case_begin("p_within_hold_other_states");
  CHECK(!airplay_anchor_within_hold(ANCHOR_NONE, 1000, 1100, 200));
  CHECK(!airplay_anchor_within_hold(ANCHOR_PENDING, 1000, 1100, 200));
  CHECK(!airplay_anchor_within_hold(ANCHOR_RETRY_WAIT, 1000, 1100, 200));
  CHECK(!airplay_anchor_within_hold(ANCHOR_CONFIRMED, 1000, 1100, 200));
  case_end();
}

static void
p_within_hold_wrap(void)
{
  case_begin("p_within_hold_wrap");
  // Sent just below the largest value, read just after the counter wrapped
  CHECK(airplay_anchor_within_hold(ANCHOR_IN_FLIGHT, UINT64_MAX - 50, 100, 200));
  CHECK(!airplay_anchor_within_hold(ANCHOR_IN_FLIGHT, UINT64_MAX - 50, 200, 200));
  case_end();
}

static void
p_names(void)
{
  case_begin("p_names");
  CHECK(strcmp(airplay_anchor_state_name(ANCHOR_IN_FLIGHT), "IN_FLIGHT") == 0);
  CHECK(strcmp(airplay_anchor_event_name(ANCHOR_EV_REJECTED), "REJECTED") == 0);
  CHECK(strcmp(airplay_anchor_state_name((enum airplay_anchor_state)99), "?") == 0);
  CHECK(strcmp(airplay_anchor_event_name((enum airplay_anchor_event)99), "?") == 0);
  CHECK(ANCHOR_NONE == 0);
  case_end();
}


/* ---------------------------- Scripted sequences --------------------------- */

#define SIM_SESSIONS 4

struct sim_session
{
  enum airplay_anchor_state state;
  int retries;
  enum airplay_anchor_hold_reason reason;
  bool live;
  bool timer_armed;
  uint64_t sent_ms;
  int sends;
};

// Several sessions on one shared line. step() applies a transition the way the
// daemon's writer does and mirrors the timer, the budget, the reason and the
// shared line, so that a sequence can be written as events and expectations.
struct sim
{
  struct sim_session s[SIM_SESSIONS];
  bool line_mapped;
  int line_drops;
  int retries_max;
  // A session that must be in a given state after every step, or -1
  int watch_idx;
  enum airplay_anchor_state watch_state;
};

static void
sim_init(struct sim *m, int retries_max)
{
  int i;

  memset(m, 0, sizeof(*m));
  m->retries_max = retries_max;
  m->watch_idx = -1;
  for (i = 0; i < SIM_SESSIONS; i++)
    {
      m->s[i].state = ANCHOR_NONE;
      m->s[i].retries = retries_max;
      m->s[i].live = true;
    }
}

static int
sim_holders(const struct sim *m, int except)
{
  int i, n = 0;

  for (i = 0; i < SIM_SESSIONS; i++)
    {
      if (i == except || !m->s[i].live)
	continue;
      if (airplay_anchor_holds_line(m->s[i].state))
	n++;
    }

  return n;
}

// Returns the action set, or -1 for an illegal event
static int
step(struct sim *m, int idx, enum airplay_anchor_event ev, enum airplay_anchor_hold_reason held_for)
{
  struct sim_session *s = &m->s[idx];
  struct airplay_anchor_ctx ctx;
  struct airplay_anchor_result res;
  int i;

  ctx.retries_left = s->retries;
  ctx.hold_reason = s->reason;
  ctx.held_for = held_for;

  if (airplay_anchor_transition(s->state, ev, &ctx, m->retries_max, &res) < 0)
    {
      CHECK(res.state == s->state);
      CHECK(res.actions == 0);
      return -1;
    }

  // The retry timer is one-shot: a retry is sent because it fired
  if (s->state == ANCHOR_RETRY_WAIT && ev == ANCHOR_EV_SENT)
    s->timer_armed = false;

  s->state = res.state;
  s->retries = res.retries_left;
  s->reason = res.hold_reason;

  if (res.actions & ANCHOR_ACT_MARK_SENT)
    {
      s->sent_ms = 0;
      s->sends++;
      // The request builder creates the shared line when it is not mapped
      m->line_mapped = true;
    }
  if (res.actions & ANCHOR_ACT_TIMER_DEL)
    s->timer_armed = false;
  if (res.actions & ANCHOR_ACT_TIMER_ARM)
    s->timer_armed = true;
  if (res.actions & (ANCHOR_ACT_LINE_DROP_IF_UNHELD | ANCHOR_ACT_LINE_RESET_IF_UNHELD))
    {
      if (sim_holders(m, idx) == 0)
	{
	  if (m->line_mapped)
	    m->line_drops++;
	  m->line_mapped = false;
	}
    }

  // The timer is armed if and only if the session waits for a retry
  for (i = 0; i < SIM_SESSIONS; i++)
    CHECK(m->s[i].timer_armed == (m->s[i].state == ANCHOR_RETRY_WAIT));

  CHECK(s->retries >= 0 && s->retries <= m->retries_max);

  if (m->watch_idx >= 0)
    CHECK(m->s[m->watch_idx].state == m->watch_state);

  return (int)res.actions;
}

#define STEP(m, i, ev, held) \
  do { if (step((m), (i), (ev), (held)) < 0) { printf("FAIL %s: line %d: illegal event %s in a sequence\n", cur_name, __LINE__, airplay_anchor_event_name(ev)); cur_failed = true; } } while (0)

#define A 0
#define B 1
#define C 2

// Brings a session from attach to an accepted anchor
static void
sim_confirm(struct sim *m, int idx)
{
  STEP(m, idx, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(m, idx, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(m, idx, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
}

static void
s_cold_start_both_accept(void)
{
  struct sim m;

  case_begin("s_cold_start_both_accept");
  sim_init(&m, RETRIES_MAX);

  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_HELD, ANCHOR_HOLD_OWN_AGE);
  STEP(&m, B, ANCHOR_EV_HELD, ANCHOR_HOLD_OWN_AGE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);

  CHECK(m.s[A].state == ANCHOR_CONFIRMED);
  CHECK(m.s[B].state == ANCHOR_CONFIRMED);
  CHECK(m.line_mapped);
  CHECK(m.s[A].retries == RETRIES_MAX);
  CHECK(m.s[B].retries == RETRIES_MAX);
  CHECK(m.line_drops == 0);

  case_end();
}

static void
s_rejection_while_sibling_in_flight_keeps_line(void)
{
  struct sim m;

  case_begin("s_rejection_while_sibling_in_flight_keeps_line");
  sim_init(&m, RETRIES_MAX);

  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);

  CHECK(m.s[A].state == ANCHOR_RETRY_WAIT);
  CHECK(m.s[A].timer_armed);
  CHECK(m.line_mapped);

  STEP(&m, B, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);

  CHECK(m.s[A].state == ANCHOR_CONFIRMED);
  CHECK(m.s[B].state == ANCHOR_CONFIRMED);
  CHECK(m.line_drops == 0);
  CHECK(m.line_mapped);

  case_end();
}

static void
s_both_rejected(void)
{
  struct sim m;

  case_begin("s_both_rejected");
  sim_init(&m, RETRIES_MAX);

  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);

  // B still holds the line, so it is kept
  STEP(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(m.line_mapped);

  // A waits for its retry and holds nothing, so the line is dropped
  STEP(&m, B, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(!m.line_mapped);
  CHECK(m.line_drops == 1);

  // The next send creates it again
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  CHECK(m.line_mapped);
  STEP(&m, B, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);

  CHECK(m.s[A].state == ANCHOR_CONFIRMED);
  CHECK(m.s[B].state == ANCHOR_CONFIRMED);
  CHECK(m.line_mapped);

  case_end();
}

static void
s_retry_budget_exhausted(void)
{
  struct sim m;
  int ret;

  case_begin("s_retry_budget_exhausted");
  sim_init(&m, 3);

  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(m.s[A].retries == 2);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(m.s[A].retries == 1);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(m.s[A].retries == 0);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);

  ret = step(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(ret >= 0 && (ret & ANCHOR_ACT_ABORT));
  CHECK(m.s[A].state == ANCHOR_NONE);
  CHECK(!m.s[A].timer_armed);
  CHECK(m.s[A].sends == 3 + 1);

  case_end();
}

static void
s_answer_after_flush_ignored(void)
{
  struct sim m;
  int ret;

  case_begin("s_answer_after_flush_ignored");
  sim_init(&m, RETRIES_MAX);

  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);

  CHECK(m.s[A].state == ANCHOR_PENDING);
  CHECK(!m.s[A].timer_armed);
  // Nobody else holds the line, so the flush reset it
  CHECK(!m.line_mapped);

  ret = step(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
  CHECK(ret >= 0 && (ret & ANCHOR_ACT_IGNORED));
  CHECK(m.s[A].state == ANCHOR_PENDING);

  ret = step(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(ret >= 0 && (ret & ANCHOR_ACT_IGNORED));
  CHECK(m.s[A].state == ANCHOR_PENDING);
  CHECK(m.s[A].retries == RETRIES_MAX);

  case_end();
}

static void
s_flush_while_in_flight_with_confirmed_sibling(void)
{
  struct sim m;

  case_begin("s_flush_while_in_flight_with_confirmed_sibling");
  sim_init(&m, RETRIES_MAX);

  sim_confirm(&m, A);
  STEP(&m, B, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);

  STEP(&m, B, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);
  CHECK(m.s[B].state == ANCHOR_PENDING);
  CHECK(m.line_mapped);

  // B is pending and holds nothing
  STEP(&m, A, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);
  CHECK(m.s[A].state == ANCHOR_PENDING);
  CHECK(!m.line_mapped);

  case_end();
}

static void
s_offset_flush_with_sibling_in_flight_keeps_line(void)
{
  struct sim m;

  case_begin("s_offset_flush_with_sibling_in_flight_keeps_line");
  sim_init(&m, RETRIES_MAX);

  sim_confirm(&m, A);
  STEP(&m, B, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);

  STEP(&m, A, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);
  CHECK(m.line_mapped);
  CHECK(m.line_drops == 0);

  STEP(&m, B, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);

  CHECK(m.s[A].state == ANCHOR_CONFIRMED);
  CHECK(m.s[B].state == ANCHOR_CONFIRMED);
  CHECK(m.line_mapped);
  CHECK(m.line_drops == 0);

  case_end();
}

static void
s_flush_in_retry_wait_cancels_timer(void)
{
  struct sim m;

  case_begin("s_flush_in_retry_wait_cancels_timer");
  sim_init(&m, RETRIES_MAX);

  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(m.s[A].timer_armed);

  STEP(&m, A, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);
  CHECK(m.s[A].state == ANCHOR_PENDING);
  CHECK(!m.s[A].timer_armed);
  // A flush does not give the budget back
  CHECK(m.s[A].retries == RETRIES_MAX - 1);

  case_end();
}

static void
s_failed_session_never_holds(void)
{
  struct sim m;

  case_begin("s_failed_session_never_holds");
  sim_init(&m, RETRIES_MAX);

  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);

  // B fails: no event, the session just stops counting
  m.s[B].live = false;

  STEP(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(!m.line_mapped);

  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
  CHECK(m.line_mapped);

  // B is still in flight as far as its own state goes, but does not hold the line
  CHECK(m.s[B].state == ANCHOR_IN_FLIGHT);
  STEP(&m, A, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);
  CHECK(!m.line_mapped);

  case_end();
}

static void
s_hold_bound_expiry(void)
{
  struct sim m;

  case_begin("s_hold_bound_expiry");
  sim_init(&m, RETRIES_MAX);

  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  m.s[A].sent_ms = 1000;

  CHECK(airplay_anchor_within_hold(m.s[A].state, m.s[A].sent_ms, 1150, 200));
  CHECK(!airplay_anchor_within_hold(m.s[A].state, m.s[A].sent_ms, 1200, 200));

  STEP(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
  CHECK(!airplay_anchor_within_hold(m.s[A].state, m.s[A].sent_ms, 1250, 200));
  CHECK(airplay_anchor_holds_line(m.s[A].state));

  case_end();
}

static void
s_held_reason_sticky(void)
{
  struct sim m;

  case_begin("s_held_reason_sticky");
  sim_init(&m, RETRIES_MAX);

  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  CHECK(m.s[A].reason == ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_HELD, ANCHOR_HOLD_OWN_AGE);
  CHECK(m.s[A].reason == ANCHOR_HOLD_OWN_AGE);
  STEP(&m, A, ANCHOR_EV_HELD, ANCHOR_HOLD_SIBLING);
  CHECK(m.s[A].reason == ANCHOR_HOLD_SIBLING);
  STEP(&m, A, ANCHOR_EV_HELD, ANCHOR_HOLD_OWN_AGE);
  CHECK(m.s[A].reason == ANCHOR_HOLD_SIBLING);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  CHECK(m.s[A].reason == ANCHOR_HOLD_NONE);

  STEP(&m, B, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_HELD, ANCHOR_HOLD_SIBLING);
  CHECK(m.s[B].reason == ANCHOR_HOLD_SIBLING);
  STEP(&m, B, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);
  CHECK(m.s[B].reason == ANCHOR_HOLD_NONE);

  case_end();
}

static void
s_misattributed_answer_chain(void)
{
  struct sim m;
  int ret;

  case_begin("s_misattributed_answer_chain");
  sim_init(&m, RETRIES_MAX);

  // An anchor is overtaken by a flush, a new anchor is sent, and the old
  // anchor's late rejection is taken for the new anchor's
  STEP(&m, A, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_REJECTED, ANCHOR_HOLD_NONE);
  CHECK(m.s[A].state == ANCHOR_RETRY_WAIT);
  CHECK(m.s[A].timer_armed);

  // The real answer to the new anchor is ignored
  ret = step(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
  CHECK(ret >= 0 && (ret & ANCHOR_ACT_IGNORED));

  // The retry is sent and accepted
  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);

  // step() has checked after every event that the timer was armed only in the
  // retry wait, so a confirmed session never has a retry pending
  CHECK(m.s[A].state == ANCHOR_CONFIRMED);
  CHECK(!m.s[A].timer_armed);

  case_end();
}

static void
s_group_pause_resume(void)
{
  struct sim m;

  case_begin("s_group_pause_resume");
  sim_init(&m, RETRIES_MAX);

  sim_confirm(&m, A);
  sim_confirm(&m, B);

  STEP(&m, A, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);
  CHECK(m.line_mapped);
  STEP(&m, B, ANCHOR_EV_FLUSH, ANCHOR_HOLD_NONE);
  CHECK(!m.line_mapped);

  STEP(&m, A, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  CHECK(m.line_mapped);
  STEP(&m, B, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, A, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);
  STEP(&m, B, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);

  CHECK(m.s[A].state == ANCHOR_CONFIRMED);
  CHECK(m.s[B].state == ANCHOR_CONFIRMED);
  CHECK(m.line_mapped);

  case_end();
}

static void
s_join_while_playing(void)
{
  struct sim m;

  case_begin("s_join_while_playing");
  sim_init(&m, RETRIES_MAX);

  sim_confirm(&m, A);
  m.watch_idx = A;
  m.watch_state = ANCHOR_CONFIRMED;
  m.line_drops = 0;

  STEP(&m, C, ANCHOR_EV_ATTACH, ANCHOR_HOLD_NONE);
  STEP(&m, C, ANCHOR_EV_HELD, ANCHOR_HOLD_OWN_AGE);
  STEP(&m, C, ANCHOR_EV_SENT, ANCHOR_HOLD_NONE);
  STEP(&m, C, ANCHOR_EV_ACCEPTED, ANCHOR_HOLD_NONE);

  CHECK(m.s[C].state == ANCHOR_CONFIRMED);
  CHECK(m.line_mapped);
  CHECK(m.line_drops == 0);

  case_end();
}


int
main(int argc, char **argv)
{
  size_t i;

  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  for (i = 0; i < sizeof(tcases) / sizeof(tcases[0]); i++)
    run_tcase(&tcases[i]);

  t_sweep_all_cells();

  p_holds_line();
  p_within_hold_in_flight_inside();
  p_within_hold_in_flight_at_bound();
  p_within_hold_other_states();
  p_within_hold_wrap();
  p_names();

  s_cold_start_both_accept();
  s_rejection_while_sibling_in_flight_keeps_line();
  s_both_rejected();
  s_retry_budget_exhausted();
  s_answer_after_flush_ignored();
  s_flush_while_in_flight_with_confirmed_sibling();
  s_offset_flush_with_sibling_in_flight_keeps_line();
  s_flush_in_retry_wait_cancels_timer();
  s_failed_session_never_holds();
  s_hold_bound_expiry();
  s_held_reason_sticky();
  s_misattributed_answer_chain();
  s_group_pause_resume();
  s_join_while_playing();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
