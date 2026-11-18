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

#ifndef __UPNP_SUB_H__
#define __UPNP_SUB_H__

#include <stdbool.h>
#include <stdint.h>

// The life of one speaker's subscription to its renderer's volume events:
// search, describe, subscribe, renew, back off, unsubscribe. Pure: it holds
// no sockets or timers and never reads the clock. The caller feeds it events
// with the time, and performs the actions it returns.
//
// An entry has at most one request in flight (describe, subscribe, renew or
// unsubscribe). Searches are shared by every entry and are not counted. An
// unsubscribe marked detached belongs to no entry: its answer is not fed back.

enum upnp_sub_state {
  UPNP_SUB_IDLE,
  UPNP_SUB_SEARCHING,      // waiting for a search to find the renderer
  UPNP_SUB_DESCRIBING,     // fetching the description
  UPNP_SUB_SUBSCRIBING,
  UPNP_SUB_FOLLOWING,
  UPNP_SUB_RENEWING,
  UPNP_SUB_BACKOFF,        // waiting to search or subscribe again
  UPNP_SUB_UNSUBSCRIBING,  // on the way to idle
};

enum upnp_sub_event {
  UPNP_EV_ENABLE,
  UPNP_EV_DISABLE,
  UPNP_EV_ADDRESS_CHANGED,
  UPNP_EV_SSDP_MATCH,          // args.location
  UPNP_EV_SEARCH_DONE_NO_MATCH,
  UPNP_EV_DESCRIBED,           // args.event_url
  UPNP_EV_DESCRIBE_FAILED,
  UPNP_EV_SUB_OK,              // args.sid, args.timeout_s
  UPNP_EV_SUB_REFUSED,         // args.status
  UPNP_EV_NET_ERROR,
  UPNP_EV_TIMER,               // the timer armed by UPNP_ACT_ARM_TIMER is due
  UPNP_EV_UNSUB_DONE,
  UPNP_EV_NOTIFY,              // an event arrived on the subscription
};

// What the rows show about following a speaker's own level
enum upnp_follow_state {
  UPNP_FOLLOW_NONE,
  UPNP_FOLLOW_SEARCHING,
  UPNP_FOLLOW_UPNP,
};

struct upnp_sub_args {
  const char *location;
  const char *event_url;
  const char *sid;
  long timeout_s;          // as struct upnp_http_head: -1 absent, 0 infinite
  int status;
};

#define UPNP_ACT_WANT_SEARCH      (1u << 0)
#define UPNP_ACT_GET_DESCRIPTION  (1u << 1)  // of s->location
#define UPNP_ACT_SUBSCRIBE        (1u << 2)  // to s->event_url
#define UPNP_ACT_RENEW            (1u << 3)  // s->sid at s->event_url
#define UPNP_ACT_UNSUBSCRIBE      (1u << 4)  // act->unsub_sid at s->event_url
#define UPNP_ACT_CANCEL_REQUEST   (1u << 5)  // the entry's request in flight; before any new one
#define UPNP_ACT_ARM_TIMER        (1u << 6)  // at act->timer_at_ms, replacing any armed
#define UPNP_ACT_DISARM_TIMER     (1u << 7)
#define UPNP_ACT_FOLLOW_STATE     (1u << 8)  // publish act->follow
#define UPNP_ACT_FREE             (1u << 9)  // the entry may be freed; nothing is in flight
#define UPNP_ACT_WARN_DROPPED     (1u << 10) // renewal refused with no event since the subscribe
#define UPNP_ACT_SEARCH_GAVE_UP   (1u << 11) // a search found nothing; act->retry_s until the next

struct upnp_sub_actions {
  unsigned flags;
  bool unsub_detached;     // the unsubscribe belongs to no entry
  char unsub_sid[96];
  uint64_t timer_at_ms;
  enum upnp_follow_state follow;
  long retry_s;
};

struct upnp_sub {
  enum upnp_sub_state state;
  bool backoff_search;     // in backoff: search when it ends, rather than subscribe
  int search_step;
  int error_step;
  int refused_in_row;      // fresh subscribes refused in a row
  bool reenable;
  bool searched_once;
  bool notified;           // an event since the subscribe
  enum upnp_follow_state published;

  char location[256];
  char event_url[256];
  char sid[96];
  char prev_sid[96];
  uint64_t prev_sid_until_ms;
  long granted_s;
};

void
upnp_sub_init(struct upnp_sub *s);

// Runs one event. act is filled in (cleared first). args may be NULL for
// events that take none.
void
upnp_sub_step(struct upnp_sub *s, enum upnp_sub_event ev, const struct upnp_sub_args *args, uint64_t now_ms, struct upnp_sub_actions *act);

// True if an event with this SID belongs to the subscription: the current
// SID, the one it replaced within the last 30 s, or any SID while the first
// subscribe is still unanswered (a renderer may send the first event before
// the answer is read)
bool
upnp_sub_sid_ok(const struct upnp_sub *s, const char *sid, uint64_t now_ms);

// True while the renderer may send events to this entry
bool
upnp_sub_has_subscription(const struct upnp_sub *s);

// The renewal interval for a granted timeout
long
upnp_sub_renew_s(long timeout_s);

const char *
upnp_sub_state_name(enum upnp_sub_state state);

#endif /* !__UPNP_SUB_H__ */
