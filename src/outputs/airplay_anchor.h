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

#ifndef __AIRPLAY_ANCHOR_H__
#define __AIRPLAY_ANCHOR_H__

#include <stdint.h>
#include <stdbool.h>

// The anchor of a buffered AirPlay session, as a state machine. A buffered
// receiver plays nothing until it has accepted a SETRATEANCHORTIME, so a
// session owes one after it is set up and again after every flush; the request
// may be rejected and retried, and while it is unanswered the session may hold
// the shared write head. This file holds the states, the events and the
// transition rules with no dependency on the session, the event loop or the
// logger, so that the rules can be checked by a plain program.

enum airplay_anchor_state
{
  ANCHOR_NONE = 0,     // realtime session, buffered session not yet attached, or failed for good
  ANCHOR_PENDING,      // a fresh anchor is owed; sent once streaming and the receiver's clock peer is old enough
  ANCHOR_IN_FLIGHT,    // SETRATEANCHORTIME written, not yet answered
  ANCHOR_RETRY_WAIT,   // rejected, retry timer armed
  ANCHOR_CONFIRMED,    // accepted; delivery is governed by the anchor's frame and the write head
};

enum airplay_anchor_event
{
  ANCHOR_EV_ATTACH,    // the session was decided to be buffered
  ANCHOR_EV_HELD,      // a write tick found the anchor not yet due (reason in the context)
  ANCHOR_EV_SENT,      // the request was built; it is written as soon as the builder returns
  ANCHOR_EV_ACCEPTED,  // RTSP 200 and in-band status 0
  ANCHOR_EV_REJECTED,  // anything else answered (the retry budget is in the context)
  ANCHOR_EV_FLUSH,     // FLUSHBUFFERED or FLUSHBUFFERED (offset) answered
};

enum airplay_anchor_hold_reason
{
  ANCHOR_HOLD_NONE = 0,    // not held, or not pending
  ANCHOR_HOLD_OWN_AGE,     // held because the session's own clock peer is too young
  ANCHOR_HOLD_SIBLING,     // held, at least once, for a sibling's young clock peer
};

// What the caller knows that the transition depends on
struct airplay_anchor_ctx
{
  int retries_left;                              // current budget, for REJECTED
  enum airplay_anchor_hold_reason hold_reason;   // current qualifier, for HELD
  enum airplay_anchor_hold_reason held_for;      // this tick's reason, for HELD
};

// What the caller must do after a legal transition
#define ANCHOR_ACT_TIMER_ARM            (1 << 0)  // arm the retry timer for the retry wait
#define ANCHOR_ACT_TIMER_DEL            (1 << 1)  // delete the retry timer (a no-op if not armed)
#define ANCHOR_ACT_MARK_SENT            (1 << 2)  // record the send time, clear the given-up flag
#define ANCHOR_ACT_LINE_DROP_IF_UNHELD  (1 << 3)  // rejection: forget the shared line if no other session holds it
#define ANCHOR_ACT_LINE_RESET_IF_UNHELD (1 << 4)  // flush: if no other session holds the line, re-arm the marker, forget the line, flush the encoder
#define ANCHOR_ACT_ABORT                (1 << 5)  // no retries left: the caller ends the session
#define ANCHOR_ACT_IGNORED              (1 << 6)  // an answer that no outstanding anchor matches: log and do nothing else

struct airplay_anchor_result
{
  enum airplay_anchor_state state;               // the new state (the old one if illegal)
  unsigned actions;                              // ANCHOR_ACT_* bits (0 if illegal)
  int retries_left;                              // the new budget
  enum airplay_anchor_hold_reason hold_reason;   // the new qualifier
};

// Returns 0 and fills out for a legal transition; returns -1 for an illegal
// one, with out->state == from, out->actions == 0 and the context's budget and
// reason unchanged. retries_max is the budget a new anchor starts with.
int
airplay_anchor_transition(enum airplay_anchor_state from, enum airplay_anchor_event ev,
			  const struct airplay_anchor_ctx *ctx, int retries_max,
			  struct airplay_anchor_result *out);

// True for a state that holds the master session's shared line: an anchor
// accepted on it or one sent and not yet answered.
bool
airplay_anchor_holds_line(enum airplay_anchor_state state);

// True while an unanswered anchor sent at sent_ms may still hold the shared
// write head.
bool
airplay_anchor_within_hold(enum airplay_anchor_state state, uint64_t sent_ms, uint64_t now_ms, uint64_t hold_max_ms);

const char *
airplay_anchor_state_name(enum airplay_anchor_state state);

const char *
airplay_anchor_event_name(enum airplay_anchor_event ev);

#endif  /* !__AIRPLAY_ANCHOR_H__ */
