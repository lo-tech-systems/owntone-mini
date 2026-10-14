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

#include <stdint.h>
#include <stdbool.h>

#include "airplay_anchor.h"

// Every state and event pair is accounted for. A pair that cannot happen if
// the callers test the state before raising the event is illegal. An answer in
// a state other than in flight is not illegal: it is ignored, because
// responses are matched to requests by their order on the connection and a
// flush can overtake an anchor that is still unanswered.
int
airplay_anchor_transition(enum airplay_anchor_state from, enum airplay_anchor_event ev,
			  const struct airplay_anchor_ctx *ctx, int retries_max,
			  struct airplay_anchor_result *out)
{
  out->state = from;
  out->actions = 0;
  out->retries_left = ctx->retries_left;
  out->hold_reason = ctx->hold_reason;

  switch (from)
    {
      case ANCHOR_NONE:
	if (ev != ANCHOR_EV_ATTACH)
	  return -1;

	out->state = ANCHOR_PENDING;
	out->hold_reason = ANCHOR_HOLD_NONE;
	return 0;

      case ANCHOR_PENDING:
	switch (ev)
	  {
	    case ANCHOR_EV_HELD:
	      if (ctx->held_for == ANCHOR_HOLD_NONE)
		return -1;

	      // Once a sibling has been waited for, that stays the reason
	      if (ctx->held_for == ANCHOR_HOLD_SIBLING || ctx->hold_reason == ANCHOR_HOLD_SIBLING)
		out->hold_reason = ANCHOR_HOLD_SIBLING;
	      else
		out->hold_reason = ANCHOR_HOLD_OWN_AGE;
	      return 0;

	    case ANCHOR_EV_SENT:
	      out->state = ANCHOR_IN_FLIGHT;
	      out->actions = ANCHOR_ACT_MARK_SENT;
	      out->hold_reason = ANCHOR_HOLD_NONE;
	      return 0;

	    case ANCHOR_EV_ACCEPTED:
	    case ANCHOR_EV_REJECTED:
	      out->actions = ANCHOR_ACT_IGNORED;
	      return 0;

	    case ANCHOR_EV_FLUSH:
	      out->actions = ANCHOR_ACT_TIMER_DEL | ANCHOR_ACT_LINE_RESET_IF_UNHELD;
	      out->hold_reason = ANCHOR_HOLD_NONE;
	      return 0;

	    default:
	      return -1;
	  }

      case ANCHOR_IN_FLIGHT:
	switch (ev)
	  {
	    case ANCHOR_EV_ACCEPTED:
	      out->state = ANCHOR_CONFIRMED;
	      out->actions = ANCHOR_ACT_TIMER_DEL;
	      out->retries_left = retries_max;
	      return 0;

	    case ANCHOR_EV_REJECTED:
	      if (ctx->retries_left > 0)
		{
		  out->state = ANCHOR_RETRY_WAIT;
		  out->actions = ANCHOR_ACT_TIMER_ARM | ANCHOR_ACT_LINE_DROP_IF_UNHELD;
		  out->retries_left = ctx->retries_left - 1;
		}
	      else
		{
		  out->state = ANCHOR_NONE;
		  out->actions = ANCHOR_ACT_ABORT;
		}
	      return 0;

	    case ANCHOR_EV_FLUSH:
	      out->state = ANCHOR_PENDING;
	      out->actions = ANCHOR_ACT_TIMER_DEL | ANCHOR_ACT_LINE_RESET_IF_UNHELD;
	      out->hold_reason = ANCHOR_HOLD_NONE;
	      return 0;

	    default:
	      return -1;
	  }

      case ANCHOR_RETRY_WAIT:
	switch (ev)
	  {
	    case ANCHOR_EV_SENT:
	      out->state = ANCHOR_IN_FLIGHT;
	      out->actions = ANCHOR_ACT_MARK_SENT;
	      out->hold_reason = ANCHOR_HOLD_NONE;
	      return 0;

	    case ANCHOR_EV_ACCEPTED:
	    case ANCHOR_EV_REJECTED:
	      out->actions = ANCHOR_ACT_IGNORED;
	      return 0;

	    case ANCHOR_EV_FLUSH:
	      out->state = ANCHOR_PENDING;
	      out->actions = ANCHOR_ACT_TIMER_DEL | ANCHOR_ACT_LINE_RESET_IF_UNHELD;
	      out->hold_reason = ANCHOR_HOLD_NONE;
	      return 0;

	    default:
	      return -1;
	  }

      case ANCHOR_CONFIRMED:
	switch (ev)
	  {
	    case ANCHOR_EV_ACCEPTED:
	    case ANCHOR_EV_REJECTED:
	      out->actions = ANCHOR_ACT_IGNORED;
	      return 0;

	    case ANCHOR_EV_FLUSH:
	      out->state = ANCHOR_PENDING;
	      out->actions = ANCHOR_ACT_TIMER_DEL | ANCHOR_ACT_LINE_RESET_IF_UNHELD;
	      out->hold_reason = ANCHOR_HOLD_NONE;
	      return 0;

	    default:
	      return -1;
	  }

      default:
	return -1;
    }
}

bool
airplay_anchor_holds_line(enum airplay_anchor_state state)
{
  return (state == ANCHOR_IN_FLIGHT || state == ANCHOR_CONFIRMED);
}

// Unsigned arithmetic: the clock is read after the send time, on the same
// thread, so the difference cannot be negative
bool
airplay_anchor_within_hold(enum airplay_anchor_state state, uint64_t sent_ms, uint64_t now_ms, uint64_t hold_max_ms)
{
  return (state == ANCHOR_IN_FLIGHT && now_ms - sent_ms < hold_max_ms);
}

const char *
airplay_anchor_state_name(enum airplay_anchor_state state)
{
  switch (state)
    {
      case ANCHOR_NONE:       return "NONE";
      case ANCHOR_PENDING:    return "PENDING";
      case ANCHOR_IN_FLIGHT:  return "IN_FLIGHT";
      case ANCHOR_RETRY_WAIT: return "RETRY_WAIT";
      case ANCHOR_CONFIRMED:  return "CONFIRMED";
    }

  return "?";
}

const char *
airplay_anchor_event_name(enum airplay_anchor_event ev)
{
  switch (ev)
    {
      case ANCHOR_EV_ATTACH:   return "ATTACH";
      case ANCHOR_EV_HELD:     return "HELD";
      case ANCHOR_EV_SENT:     return "SENT";
      case ANCHOR_EV_ACCEPTED: return "ACCEPTED";
      case ANCHOR_EV_REJECTED: return "REJECTED";
      case ANCHOR_EV_FLUSH:    return "FLUSH";
    }

  return "?";
}
