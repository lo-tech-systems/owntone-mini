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

#include <stdio.h>
#include <string.h>

#include "upnp_sub.h"

// Waits after a search that found nothing; the last repeats
static const long search_backoff_s[] = { 60, 300, 1800, 3600 };
// Waits after a failed request; once the last has passed, search again, as
// the address may now belong to something else
static const long error_backoff_s[] = { 5, 30, 120, 600 };

#define N_SEARCH_STEPS  (int)(sizeof(search_backoff_s) / sizeof(search_backoff_s[0]))
#define N_ERROR_STEPS   (int)(sizeof(error_backoff_s) / sizeof(error_backoff_s[0]))

// A renewal is not left later than this, whatever is granted
#define RENEW_FLOOR_S          30
// How often to renew a subscription granted for ever, in case it is not
#define RENEW_INFINITE_S       900
// Taken as granted when a renderer answers with no TIMEOUT
#define TIMEOUT_ABSENT_S       300
// How long events on a replaced subscription are still taken
#define PREV_SID_MS            30000

void
upnp_sub_init(struct upnp_sub *s)
{
  memset(s, 0, sizeof(*s));
  s->state = UPNP_SUB_IDLE;
  s->published = UPNP_FOLLOW_NONE;
}

long
upnp_sub_renew_s(long timeout_s)
{
  if (timeout_s == 0)
    return RENEW_INFINITE_S;
  if (timeout_s < 0)
    timeout_s = TIMEOUT_ABSENT_S;

  return (timeout_s / 2 > RENEW_FLOOR_S) ? timeout_s / 2 : RENEW_FLOOR_S;
}

bool
upnp_sub_sid_ok(const struct upnp_sub *s, const char *sid, uint64_t now_ms)
{
  if (!sid || !sid[0])
    return false;
  if (s->sid[0] && strcmp(s->sid, sid) == 0)
    return true;
  if (s->prev_sid[0] && now_ms < s->prev_sid_until_ms && strcmp(s->prev_sid, sid) == 0)
    return true;

  return s->state == UPNP_SUB_SUBSCRIBING && !s->sid[0];
}

bool
upnp_sub_has_subscription(const struct upnp_sub *s)
{
  switch (s->state)
    {
      case UPNP_SUB_SUBSCRIBING:
      case UPNP_SUB_FOLLOWING:
      case UPNP_SUB_RENEWING:
      case UPNP_SUB_UNSUBSCRIBING:
	return true;
      default:
	return false;
    }
}

const char *
upnp_sub_state_name(enum upnp_sub_state state)
{
  static const char *names[] = {
    "idle", "searching", "describing", "subscribing", "following", "renewing", "backoff", "unsubscribing",
  };

  return ((unsigned)state < sizeof(names) / sizeof(names[0])) ? names[state] : "?";
}


/* ------------------------------- Helpers ---------------------------------- */

static void
publish(struct upnp_sub *s, struct upnp_sub_actions *act, enum upnp_follow_state follow)
{
  if (s->published == follow)
    return;

  s->published = follow;
  act->flags |= UPNP_ACT_FOLLOW_STATE;
  act->follow = follow;
}

static void
arm(struct upnp_sub_actions *act, uint64_t now_ms, long secs)
{
  act->flags |= UPNP_ACT_ARM_TIMER;
  act->flags &= ~UPNP_ACT_DISARM_TIMER;
  act->timer_at_ms = now_ms + (uint64_t)secs * 1000;
}

static void
disarm(struct upnp_sub_actions *act)
{
  act->flags |= UPNP_ACT_DISARM_TIMER;
  act->flags &= ~UPNP_ACT_ARM_TIMER;
}

static void
unsubscribe(struct upnp_sub_actions *act, const char *sid, bool detached)
{
  act->flags |= UPNP_ACT_UNSUBSCRIBE;
  act->unsub_detached = detached;
  snprintf(act->unsub_sid, sizeof(act->unsub_sid), "%s", sid);
}

// Forgets the subscription, keeping its SID a while for events still on the way
static void
sid_retire(struct upnp_sub *s, uint64_t now_ms)
{
  if (!s->sid[0])
    return;

  memcpy(s->prev_sid, s->sid, sizeof(s->prev_sid));
  s->prev_sid_until_ms = now_ms + PREV_SID_MS;
  s->sid[0] = '\0';
}

static void
to_searching(struct upnp_sub *s, struct upnp_sub_actions *act, bool reset)
{
  s->state = UPNP_SUB_SEARCHING;
  s->refused_in_row = 0;
  if (reset)
    {
      s->search_step = 0;
      s->error_step = 0;
    }
  act->flags |= UPNP_ACT_WANT_SEARCH;
}

static void
to_subscribing(struct upnp_sub *s, struct upnp_sub_actions *act)
{
  s->state = UPNP_SUB_SUBSCRIBING;
  s->notified = false;
  act->flags |= UPNP_ACT_SUBSCRIBE;
}

static void
to_error_backoff(struct upnp_sub *s, struct upnp_sub_actions *act, uint64_t now_ms)
{
  int step = (s->error_step < N_ERROR_STEPS) ? s->error_step : N_ERROR_STEPS - 1;

  s->state = UPNP_SUB_BACKOFF;
  s->backoff_search = false;
  arm(act, now_ms, error_backoff_s[step]);
  s->error_step++;
}

static void
sid_forget(struct upnp_sub *s)
{
  s->sid[0] = '\0';
  s->prev_sid[0] = '\0';
}

static void
to_idle(struct upnp_sub *s, struct upnp_sub_actions *act)
{
  sid_forget(s);
  s->state = UPNP_SUB_IDLE;
  disarm(act);
  publish(s, act, UPNP_FOLLOW_NONE);
  act->flags |= UPNP_ACT_FREE;
}

// Leaves an entry that may hold a live subscription: it is cancelled if
// unanswered, unsubscribed if granted
static void
disable(struct upnp_sub *s, struct upnp_sub_actions *act)
{
  switch (s->state)
    {
      case UPNP_SUB_IDLE:
	act->flags |= UPNP_ACT_FREE;
	return;

      case UPNP_SUB_UNSUBSCRIBING:
	s->reenable = false;
	return;

      case UPNP_SUB_DESCRIBING:
      case UPNP_SUB_SUBSCRIBING:
	act->flags |= UPNP_ACT_CANCEL_REQUEST;
	to_idle(s, act);
	return;

      case UPNP_SUB_RENEWING:
	act->flags |= UPNP_ACT_CANCEL_REQUEST;
	/* FALLTHROUGH */
      case UPNP_SUB_FOLLOWING:
	s->state = UPNP_SUB_UNSUBSCRIBING;
	s->reenable = false;
	disarm(act);
	unsubscribe(act, s->sid, false);
	return;

      case UPNP_SUB_SEARCHING:
      case UPNP_SUB_BACKOFF:
	to_idle(s, act);
	return;
    }
}

static void
address_changed(struct upnp_sub *s, struct upnp_sub_actions *act)
{
  switch (s->state)
    {
      case UPNP_SUB_IDLE:
      case UPNP_SUB_UNSUBSCRIBING:
	return;

      case UPNP_SUB_RENEWING:
	act->flags |= UPNP_ACT_CANCEL_REQUEST;
	/* FALLTHROUGH */
      case UPNP_SUB_FOLLOWING:
	// To the old address, which the event URL still holds
	unsubscribe(act, s->sid, true);
	sid_forget(s);
	break;

      case UPNP_SUB_DESCRIBING:
      case UPNP_SUB_SUBSCRIBING:
	act->flags |= UPNP_ACT_CANCEL_REQUEST;
	break;

      case UPNP_SUB_SEARCHING:
      case UPNP_SUB_BACKOFF:
	break;
    }

  disarm(act);
  to_searching(s, act, true);
}

static bool
has_sid(const struct upnp_sub_args *args)
{
  return args && args->sid && args->sid[0];
}

static void
following(struct upnp_sub *s, long timeout_s, struct upnp_sub_actions *act, uint64_t now_ms)
{
  s->granted_s = timeout_s;
  s->state = UPNP_SUB_FOLLOWING;
  s->error_step = 0;
  s->refused_in_row = 0;
  arm(act, now_ms, upnp_sub_renew_s(timeout_s));
  publish(s, act, UPNP_FOLLOW_UPNP);
}


/* ---------------------------- State machine ------------------------------- */

void
upnp_sub_step(struct upnp_sub *s, enum upnp_sub_event ev, const struct upnp_sub_args *args, uint64_t now_ms, struct upnp_sub_actions *act)
{
  int step;

  memset(act, 0, sizeof(*act));

  // Wherever the entry is
  switch (ev)
    {
      case UPNP_EV_DISABLE:
	disable(s, act);
	return;

      case UPNP_EV_ADDRESS_CHANGED:
	address_changed(s, act);
	return;

      case UPNP_EV_NOTIFY:
	s->notified = true;
	return;

      default:
	break;
    }

  switch (s->state)
    {
      case UPNP_SUB_IDLE:
	if (ev == UPNP_EV_ENABLE)
	  {
	    to_searching(s, act, true);
	    if (!s->searched_once)
	      publish(s, act, UPNP_FOLLOW_SEARCHING);
	  }
	else if (ev == UPNP_EV_SUB_OK && args && args->sid && args->sid[0])
	  unsubscribe(act, args->sid, true);
	return;

      case UPNP_SUB_SEARCHING:
	if (ev == UPNP_EV_SSDP_MATCH && args && args->location)
	  {
	    snprintf(s->location, sizeof(s->location), "%s", args->location);
	    s->state = UPNP_SUB_DESCRIBING;
	    act->flags |= UPNP_ACT_GET_DESCRIPTION;
	  }
	else if (ev == UPNP_EV_SEARCH_DONE_NO_MATCH)
	  {
	    step = (s->search_step < N_SEARCH_STEPS) ? s->search_step : N_SEARCH_STEPS - 1;
	    s->state = UPNP_SUB_BACKOFF;
	    s->backoff_search = true;
	    arm(act, now_ms, search_backoff_s[step]);
	    s->search_step++;
	    s->searched_once = true;
	    act->flags |= UPNP_ACT_SEARCH_GAVE_UP;
	    act->retry_s = search_backoff_s[step];
	    publish(s, act, UPNP_FOLLOW_NONE);
	  }
	return;

      case UPNP_SUB_DESCRIBING:
	if (ev == UPNP_EV_DESCRIBED && args && args->event_url)
	  {
	    snprintf(s->event_url, sizeof(s->event_url), "%s", args->event_url);
	    s->searched_once = true;
	    s->search_step = 0;
	    s->refused_in_row = 0;
	    to_subscribing(s, act);
	  }
	else if (ev == UPNP_EV_DESCRIBE_FAILED || ev == UPNP_EV_NET_ERROR)
	  {
	    // Nothing to subscribe to yet: search again when the wait ends
	    to_error_backoff(s, act, now_ms);
	    s->backoff_search = true;
	  }
	return;

      case UPNP_SUB_SUBSCRIBING:
	if (ev == UPNP_EV_SUB_OK && has_sid(args))
	  {
	    snprintf(s->sid, sizeof(s->sid), "%s", args->sid);
	    following(s, args->timeout_s, act, now_ms);
	  }
	else if (ev == UPNP_EV_SUB_OK)
	  to_error_backoff(s, act, now_ms);
	else if (ev == UPNP_EV_SUB_REFUSED && args && args->status >= 400 && args->status < 500)
	  {
	    // The event URL is wrong, perhaps for another device now at the
	    // address: twice in a row and it is looked for again
	    s->refused_in_row++;
	    if (s->refused_in_row >= 2)
	      to_searching(s, act, true);
	    else
	      to_error_backoff(s, act, now_ms);
	  }
	else if (ev == UPNP_EV_SUB_REFUSED || ev == UPNP_EV_NET_ERROR)
	  to_error_backoff(s, act, now_ms);
	return;

      case UPNP_SUB_FOLLOWING:
	if (ev == UPNP_EV_TIMER)
	  {
	    s->state = UPNP_SUB_RENEWING;
	    act->flags |= UPNP_ACT_RENEW;
	  }
	return;

      case UPNP_SUB_RENEWING:
	if (ev == UPNP_EV_SUB_OK)
	  {
	    // A renewal keeps the SID, but take a new one if the renderer gives it
	    if (has_sid(args) && strcmp(args->sid, s->sid) != 0)
	      {
		sid_retire(s, now_ms);
		snprintf(s->sid, sizeof(s->sid), "%s", args->sid);
	      }
	    following(s, args ? args->timeout_s : -1, act, now_ms);
	  }
	else if (ev == UPNP_EV_SUB_REFUSED && args && (args->status == 412 || args->status == 404))
	  {
	    // The renderer has forgotten the subscription: subscribe afresh at
	    // once. Dropped with no event delivered suggests it could not reach us.
	    if (!s->notified)
	      act->flags |= UPNP_ACT_WARN_DROPPED;
	    sid_retire(s, now_ms);
	    s->notified = false;
	    s->refused_in_row = 0;
	    to_subscribing(s, act);
	  }
	else if (ev == UPNP_EV_SUB_REFUSED || ev == UPNP_EV_NET_ERROR)
	  {
	    sid_retire(s, now_ms);
	    to_error_backoff(s, act, now_ms);
	  }
	return;

      case UPNP_SUB_BACKOFF:
	if (ev == UPNP_EV_SSDP_MATCH && s->backoff_search && args && args->location)
	  {
	    // A reply to another entry's search: free, so take it
	    disarm(act);
	    snprintf(s->location, sizeof(s->location), "%s", args->location);
	    s->state = UPNP_SUB_DESCRIBING;
	    act->flags |= UPNP_ACT_GET_DESCRIPTION;
	  }
	else if (ev == UPNP_EV_TIMER)
	  {
	    if (s->backoff_search || !s->event_url[0])
	      to_searching(s, act, false);
	    else if (s->error_step >= N_ERROR_STEPS)
	      to_searching(s, act, true);
	    else
	      to_subscribing(s, act);
	  }
	return;

      case UPNP_SUB_UNSUBSCRIBING:
	if (ev == UPNP_EV_ENABLE)
	  s->reenable = true;
	else if (ev == UPNP_EV_UNSUB_DONE || ev == UPNP_EV_NET_ERROR || ev == UPNP_EV_SUB_REFUSED)
	  {
	    sid_forget(s);
	    if (s->reenable)
	      {
		s->reenable = false;
		to_searching(s, act, true);
	      }
	    else
	      to_idle(s, act);
	  }
	else if (ev == UPNP_EV_SUB_OK && args && args->sid && args->sid[0])
	  unsubscribe(act, args->sid, true);
	return;
    }
}
