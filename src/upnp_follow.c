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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_EVENTFD
# include <sys/eventfd.h>
#endif

#include <event2/event.h>

#include "upnp_follow.h"
#include "upnp_http.h"
#include "upnp_parse.h"
#include "upnp_sub.h"
#include "commands.h"
#include "evchan.h"
#include "listener.h"
#include "logger.h"
#include "misc.h"
#include "owntone_config.h"
#include "player.h"

#define ST_RC                   "urn:schemas-upnp-org:service:RenderingControl:1"
#define DEFAULT_EVENT_PORT      3690
// A burst of speaker changes becomes one look at the list
#define SETTLE_MS               2000
// Searches wanted close together become one
#define SEARCH_GATHER_MS        100
// A search is sent twice, then replies are read until MX has passed
#define SEARCH_REPEAT_MS        250
#define SEARCH_LISTEN_MS        2750
#define SEARCH_LOCAL_MAX        8
// Events close together become one report
#define COALESCE_MS             100
#define DESCRIPTION_MAX         65536
// How long a stopping daemon waits for its unsubscribes to be answered
#define SHUTDOWN_WAIT_MS        1000

enum req_kind {
  REQ_DESCRIBE,
  REQ_SUBSCRIBE,
  REQ_RENEW,
  REQ_UNSUBSCRIBE,
};

static const char *req_kind_names[] = { "description", "subscribe", "renewal", "unsubscribe" };

struct follow_entry {
  struct follow_entry *next;

  uint64_t id;
  char name[255];           // as the rows show it, for the log
  char config_name[255];    // the key in airplay_devices
  char address[16];

  struct upnp_sub sub;
  struct upnp_http_req *req;
  enum req_kind req_kind;
  struct event *timer;      // renewal or backoff, as the state machine asks
  struct event *coalesce;
  struct upnp_rc_state known;

  bool want_search;
  bool in_search;
  bool location_warned;
  bool seen;
};

// Written by the player thread (via listener_cb) and the main thread
static atomic_bool enabled;

// Set before the other threads exist and not changed until they are joined
static struct commands_base *cmdbase;
static struct event_base *evbase;
static pthread_t tid_upnp;
static int poke_fd = -1;
static int poke_rfd = -1;

// The upnp thread only
static struct event *poke_ev;
static struct event *settle_ev;
static struct event *search_ev;
static struct event *ssdp_ev;
static struct event *shutdown_ev;
static int ssdp_fd = -1;
static bool listening;
static bool shut_down;
static int shutdown_pending;
static struct follow_entry *entries;

static struct {
  enum { SEARCH_IDLE, SEARCH_GATHER, SEARCH_REPEAT, SEARCH_LISTEN } phase;
  char local[SEARCH_LOCAL_MAX][INET_ADDRSTRLEN];
  int nlocal;
} search;

// Filled by the player thread while the upnp thread waits for it
struct speaker_seen {
  uint64_t id;
  char name[255];
  char config_name[255];
  char address[16];
};

struct speaker_list {
  struct speaker_seen *items;
  int n;
  int cap;
};


/* ------------------------------- Helpers ---------------------------------- */

static uint64_t
monotonic_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void
timer_in_ms(struct event *ev, uint64_t ms)
{
  struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000) };

  evtimer_add(ev, &tv);
}

static struct follow_entry *
entry_find(uint64_t id)
{
  struct follow_entry *e;

  for (e = entries; e; e = e->next)
    if (e->id == id)
      return e;

  return NULL;
}

static void
entry_free(struct follow_entry *e)
{
  struct follow_entry **pp;

  for (pp = &entries; *pp; pp = &(*pp)->next)
    {
      if (*pp == e)
	{
	  *pp = e->next;
	  break;
	}
    }

  if (e->req)
    upnp_http_cancel(e->req);
  if (e->timer)
    event_free(e->timer);
  if (e->coalesce)
    event_free(e->coalesce);
  free(e);
}

static enum output_volume_follow
follow_state_map(enum upnp_follow_state s)
{
  switch (s)
    {
      case UPNP_FOLLOW_SEARCHING: return OUTPUT_VOLUME_FOLLOW_SEARCHING;
      case UPNP_FOLLOW_UPNP:      return OUTPUT_VOLUME_FOLLOW_UPNP;
      default:                    return OUTPUT_VOLUME_FOLLOW_NONE;
    }
}

static bool entry_step(struct follow_entry *e, enum upnp_sub_event ev, const struct upnp_sub_args *args);
static void search_want(struct follow_entry *e);


/* ------------------------------- Requests --------------------------------- */

static void entry_req_cb(int status, const struct upnp_http_head *head, const char *body, size_t body_len, const char *err, void *arg);

// A request that could not even be started fails a moment later, so the
// state machine is never stepped from inside its own actions
static void
start_failed_cb(evutil_socket_t fd, short what, void *arg)
{
  uint64_t *id = arg;
  struct follow_entry *e = entry_find(*id);

  free(id);
  if (e && !e->req)
    entry_step(e, UPNP_EV_NET_ERROR, NULL);
}

static void
entry_request(struct follow_entry *e, enum req_kind kind, const char *url, const char *method, const char *headers, bool want_body)
{
  struct timeval now = { 0, 0 };
  char host[16];
  char path[256];
  unsigned short port;
  uint64_t *id;

  e->req_kind = kind;
  e->req = NULL;
  if (upnp_url_split(url, host, sizeof(host), &port, path, sizeof(path)) == 0)
    e->req = upnp_http_request(evbase, method, host, port, path, headers, want_body, DESCRIPTION_MAX, entry_req_cb, e);

  if (e->req)
    return;

  DPRINTF(E_WARN, L_UPNP, "Could not start the %s request to '%s' at %s\n", req_kind_names[kind], e->name, url);
  CHECK_NULL(L_UPNP, id = malloc(sizeof(*id)));
  *id = e->id;
  if (event_base_once(evbase, -1, EV_TIMEOUT, start_failed_cb, id, &now) < 0)
    free(id);
}

static void
entry_subscribe(struct follow_entry *e, bool renew)
{
  char host[16];
  char path[256];
  char local_ip[INET_ADDRSTRLEN];
  char headers[512];
  unsigned short port;

  if (renew)
    snprintf(headers, sizeof(headers), "SID: %s\r\nTIMEOUT: Second-1800\r\n", e->sub.sid);
  else
    {
      // The address the renderer can reach us on, which on a host with more
      // than one interface depends on where the renderer is
      if (upnp_url_split(e->sub.event_url, host, sizeof(host), &port, path, sizeof(path)) < 0
	  || upnp_local_ip_for(host, local_ip, sizeof(local_ip)) < 0)
	local_ip[0] = '\0';
      snprintf(headers, sizeof(headers), "CALLBACK: <http://%s:%d/upnp/%016" PRIx64 ">\r\nNT: upnp:event\r\nTIMEOUT: Second-1800\r\n",
	       local_ip, config_get_int("upnp_event_port", DEFAULT_EVENT_PORT), e->id);
    }

  entry_request(e, renew ? REQ_RENEW : REQ_SUBSCRIBE, e->sub.event_url, "SUBSCRIBE", headers, false);
}

static void
detached_unsub_cb(int status, const struct upnp_http_head *head, const char *body, size_t body_len, const char *err, void *arg)
{
  char *what = arg;

  DPRINTF(E_DBG, L_UPNP, "Unsubscribe from %s answered %d%s%s\n", what, status, err ? ": " : "", err ? err : "");
  free(what);
}

// An unsubscribe that belongs to no entry: from an old address, or for a
// subscription granted after the entry had stopped wanting it
static void
detached_unsubscribe(const char *url, const char *sid, const char *name)
{
  char host[16];
  char path[256];
  char headers[160];
  unsigned short port;
  char *what;

  if (upnp_url_split(url, host, sizeof(host), &port, path, sizeof(path)) < 0)
    return;

  snprintf(headers, sizeof(headers), "SID: %s\r\n", sid);
  CHECK_NULL(L_UPNP, what = strdup(name));
  if (!upnp_http_request(evbase, "UNSUBSCRIBE", host, port, path, headers, false, 0, detached_unsub_cb, what))
    free(what);
  else
    DPRINTF(E_INFO, L_UPNP, "Unsubscribing from '%s' at %s\n", name, url);
}

static void
describe_done(struct follow_entry *e, int status, const char *body, size_t body_len, const char *err)
{
  struct upnp_rc_service rc;
  struct upnp_sub_args args = { 0 };
  int ret;

  if (status != 200)
    {
      DPRINTF(E_INFO, L_UPNP, "Could not read the description of '%s' at %s: %s\n", e->name, e->sub.location,
	      (status < 0) ? err : "refused");
      entry_step(e, UPNP_EV_DESCRIBE_FAILED, NULL);
      return;
    }

  ret = upnp_description_parse(body, body_len, e->sub.location, &rc);
  if (ret < 0)
    {
      DPRINTF(E_INFO, L_UPNP, "The description of '%s' at %s %s\n", e->name, e->sub.location,
	      (ret == -1) ? "has no RenderingControl" : "could not be read");
      entry_step(e, UPNP_EV_DESCRIBE_FAILED, NULL);
      return;
    }

  DPRINTF(E_INFO, L_UPNP, "Found a renderer on '%s' (%s): %s %s, events at %s\n",
	  e->name, e->address, rc.manufacturer, rc.model, rc.event_url);

  args.event_url = rc.event_url;
  entry_step(e, UPNP_EV_DESCRIBED, &args);
}

static void
entry_req_cb(int status, const struct upnp_http_head *head, const char *body, size_t body_len, const char *err, void *arg)
{
  struct follow_entry *e = arg;
  struct upnp_sub_args args = { 0 };
  enum req_kind kind = e->req_kind;

  e->req = NULL;

  if (kind == REQ_DESCRIBE)
    {
      describe_done(e, status, body, body_len, err);
      return;
    }

  if (kind == REQ_UNSUBSCRIBE)
    {
      DPRINTF(E_INFO, L_UPNP, "Unsubscribed from '%s'%s%s\n", e->name, (status == 200) ? "" : ": ",
	      (status == 200) ? "" : (status < 0) ? err : "refused");
      entry_step(e, (status == 200) ? UPNP_EV_UNSUB_DONE : UPNP_EV_NET_ERROR, NULL);
      return;
    }

  if (status == 200)
    {
      args.sid = head->sid;
      args.timeout_s = head->timeout_s;
      if (kind == REQ_SUBSCRIBE)
	DPRINTF(E_INFO, L_UPNP, "Subscribed to '%s': %s, for %ld s\n", e->name, head->sid[0] ? head->sid : "(no SID)", head->timeout_s);
      else
	DPRINTF(E_DBG, L_UPNP, "Renewed the subscription to '%s' for %ld s\n", e->name, head->timeout_s);
      entry_step(e, UPNP_EV_SUB_OK, &args);
    }
  else if (status > 0)
    {
      DPRINTF((kind == REQ_RENEW && (status == 412 || status == 404)) ? E_INFO : E_WARN, L_UPNP,
	      "'%s' answered the %s with %d%s\n", e->name, req_kind_names[kind], status,
	      (kind == REQ_RENEW && (status == 412 || status == 404)) ? ", subscribing again" : "");
      args.status = status;
      entry_step(e, UPNP_EV_SUB_REFUSED, &args);
    }
  else
    {
      DPRINTF(E_WARN, L_UPNP, "The %s to '%s' failed: %s\n", req_kind_names[kind], e->name, err);
      entry_step(e, UPNP_EV_NET_ERROR, NULL);
    }
}


/* ---------------------------- State machine ------------------------------- */

static void
entry_timer_cb(evutil_socket_t fd, short what, void *arg)
{
  entry_step(arg, UPNP_EV_TIMER, NULL);
}

// Performs what the state machine asks. Returns true if the entry was freed.
static bool
entry_apply(struct follow_entry *e, const struct upnp_sub_actions *act)
{
  uint64_t now;
  char headers[160];

  if ((act->flags & UPNP_ACT_CANCEL_REQUEST) && e->req)
    {
      upnp_http_cancel(e->req);
      e->req = NULL;
    }

  if (act->flags & UPNP_ACT_DISARM_TIMER)
    evtimer_del(e->timer);
  if (act->flags & UPNP_ACT_ARM_TIMER)
    {
      now = monotonic_ms();
      timer_in_ms(e->timer, (act->timer_at_ms > now) ? act->timer_at_ms - now : 0);
    }

  if (act->flags & UPNP_ACT_UNSUBSCRIBE)
    {
      if (act->unsub_detached)
	detached_unsubscribe(e->sub.event_url, act->unsub_sid, e->name);
      else
	{
	  snprintf(headers, sizeof(headers), "SID: %s\r\n", act->unsub_sid);
	  DPRINTF(E_INFO, L_UPNP, "Unsubscribing from '%s'\n", e->name);
	  entry_request(e, REQ_UNSUBSCRIBE, e->sub.event_url, "UNSUBSCRIBE", headers, false);
	}
    }

  if (act->flags & UPNP_ACT_GET_DESCRIPTION)
    entry_request(e, REQ_DESCRIBE, e->sub.location, "GET", NULL, true);
  if (act->flags & UPNP_ACT_SUBSCRIBE)
    entry_subscribe(e, false);
  if (act->flags & UPNP_ACT_RENEW)
    entry_subscribe(e, true);

  if (act->flags & UPNP_ACT_WARN_DROPPED)
    DPRINTF(E_WARN, L_UPNP, "'%s' dropped the subscription before sending any event: can it reach port %d here?\n",
	    e->name, config_get_int("upnp_event_port", DEFAULT_EVENT_PORT));
  if (act->flags & UPNP_ACT_SEARCH_GAVE_UP)
    DPRINTF(E_INFO, L_UPNP, "No UPnP renderer answered for '%s' (%s), looking again in %ld s\n", e->name, e->address, act->retry_s);

  if (act->flags & UPNP_ACT_FOLLOW_STATE)
    player_speaker_follow_state_set(e->id, follow_state_map(act->follow));

  if (act->flags & UPNP_ACT_WANT_SEARCH)
    search_want(e);

  if (act->flags & UPNP_ACT_FREE)
    {
      DPRINTF(E_DBG, L_UPNP, "No longer following '%s'\n", e->name);
      entry_free(e);
      return true;
    }

  return false;
}

static bool
entry_step(struct follow_entry *e, enum upnp_sub_event ev, const struct upnp_sub_args *args)
{
  struct upnp_sub_actions act;
  enum upnp_sub_state before = e->sub.state;

  upnp_sub_step(&e->sub, ev, args, monotonic_ms(), &act);
  if (e->sub.state != before)
    DPRINTF(E_DBG, L_UPNP, "'%s': %s -> %s\n", e->name, upnp_sub_state_name(before), upnp_sub_state_name(e->sub.state));

  return entry_apply(e, &act);
}


/* --------------------------------- Search --------------------------------- */

static void
search_want(struct follow_entry *e)
{
  e->want_search = true;

  if (search.phase == SEARCH_IDLE)
    {
      search.phase = SEARCH_GATHER;
      timer_in_ms(search_ev, SEARCH_GATHER_MS);
    }
}

static void
search_send(void)
{
  int i;

  for (i = 0; i < search.nlocal; i++)
    {
      if (upnp_ssdp_search(ssdp_fd, search.local[i], ST_RC, NULL, 0) < 0)
	DPRINTF(E_WARN, L_UPNP, "Could not send a search from %s: %s\n", search.local[i], strerror(errno));
    }
}

static void
search_cb(evutil_socket_t fd, short what, void *arg)
{
  struct follow_entry *e;
  struct follow_entry *next;
  char local_ip[INET_ADDRSTRLEN];
  bool again = false;
  int i;

  switch (search.phase)
    {
      case SEARCH_GATHER:
	// One search per interface facing a speaker that is looked for
	search.nlocal = 0;
	for (e = entries; e; e = e->next)
	  {
	    if (!e->want_search)
	      continue;
	    e->want_search = false;
	    e->in_search = true;

	    if (upnp_local_ip_for(e->address, local_ip, sizeof(local_ip)) < 0)
	      continue;
	    for (i = 0; i < search.nlocal && strcmp(search.local[i], local_ip) != 0; i++)
	      ;
	    if (i == search.nlocal && search.nlocal < SEARCH_LOCAL_MAX)
	      snprintf(search.local[search.nlocal++], sizeof(search.local[0]), "%s", local_ip);
	  }

	DPRINTF(E_DBG, L_UPNP, "Searching for renderers from %d local address(es)\n", search.nlocal);
	search_send();
	search.phase = SEARCH_REPEAT;
	timer_in_ms(search_ev, SEARCH_REPEAT_MS);
	break;

      case SEARCH_REPEAT:
	search_send();
	search.phase = SEARCH_LISTEN;
	timer_in_ms(search_ev, SEARCH_LISTEN_MS);
	break;

      case SEARCH_LISTEN:
	search.phase = SEARCH_IDLE;
	for (e = entries; e; e = next)
	  {
	    next = e->next;
	    if (!e->in_search)
	      continue;
	    e->in_search = false;
	    if (e->sub.state == UPNP_SUB_SEARCHING)
	      entry_step(e, UPNP_EV_SEARCH_DONE_NO_MATCH, NULL);
	  }

	// Entries that wanted a search while this one was out
	for (e = entries; e; e = e->next)
	  again |= e->want_search;
	if (again)
	  {
	    search.phase = SEARCH_GATHER;
	    timer_in_ms(search_ev, SEARCH_GATHER_MS);
	  }
	break;

      case SEARCH_IDLE:
	break;
    }
}

static void
ssdp_read_cb(evutil_socket_t fd, short what, void *arg)
{
  struct follow_entry *e;
  struct upnp_http_head head;
  struct upnp_sub_args args = { 0 };
  struct sockaddr_in from;
  socklen_t fromlen;
  char buf[2048];
  char src[INET_ADDRSTRLEN];
  char host[16];
  char path[256];
  unsigned short port;
  ssize_t n;

  for (;;)
    {
      fromlen = sizeof(from);
      n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
      if (n <= 0)
	return;

      if (upnp_http_head_parse(buf, n, sizeof(buf), &head) != 1 || head.status != 200 || !head.location[0])
	continue;
      if (!inet_ntop(AF_INET, &from.sin_addr, src, sizeof(src)))
	continue;

      // Only a speaker's own renderer, and only one describing itself
      for (e = entries; e && strcmp(e->address, src) != 0; e = e->next)
	;
      if (!e)
	continue;

      if (upnp_url_split(head.location, host, sizeof(host), &port, path, sizeof(path)) < 0 || strcmp(host, src) != 0)
	{
	  if (!e->location_warned)
	    DPRINTF(E_INFO, L_UPNP, "Ignoring the renderer that answered from '%s' (%s): its description is at %s\n",
		    e->name, src, head.location);
	  e->location_warned = true;
	  continue;
	}

      args.location = head.location;
      entry_step(e, UPNP_EV_SSDP_MATCH, &args);
    }
}


/* -------------------------------- Events ---------------------------------- */

static void
coalesce_cb(evutil_socket_t fd, short what, void *arg)
{
  struct follow_entry *e = arg;
  struct player_followed_report r;
  char value[8];

  if (!e->known.has_volume && !(e->known.has_mute && e->known.mute))
    return;

  memset(&r, 0, sizeof(r));
  r.id = e->id;
  r.raw = e->known.has_volume ? e->known.volume : 0;
  r.mute = e->known.has_mute && e->known.mute;

  player_volume_followed_report(&r);

  switch (r.result)
    {
      case FOLLOWED_RECORDED:
	DPRINTF(E_INFO, L_UPNP, "'%s' reports %d (raw %d%s, offset %d), recorded\n",
		e->name, r.pct, r.raw, r.mute ? ", muted" : "", r.offset);
	snprintf(value, sizeof(value), "%d", r.pct);
	evchan_event_device_volume(r.row_id, r.id, r.row_name, "upnp.volume", value);
	break;

      case FOLLOWED_CONFIRMATION:
	if (r.learned)
	  DPRINTF(E_INFO, L_UPNP, "'%s' scale offset learned: %d (sent %d, speaker %d)\n", e->name, r.offset, r.sent, r.raw);
	else
	  DPRINTF(E_DBG, L_UPNP, "'%s' answered %d with %d%s\n", e->name, r.sent, r.raw, r.mute ? ", muted" : "");
	break;

      case FOLLOWED_UNCHANGED:
	DPRINTF(E_DBG, L_UPNP, "'%s' reports %d (raw %d), unchanged\n", e->name, upnp_level_map(r.raw, r.mute, r.offset), r.raw);
	break;

      case FOLLOWED_HELD:
	DPRINTF(E_DBG, L_UPNP, "'%s' reports raw %d while a level it reported over DACP is sent back, dropped\n", e->name, r.raw);
	break;

      case FOLLOWED_SCALE_UNKNOWN:
	DPRINTF(E_DBG, L_UPNP, "'%s' reports raw %d on a scale not known, dropped\n", e->name, r.raw);
	break;

      case FOLLOWED_ABSORBED:
	DPRINTF(E_DBG, L_UPNP, "'%s' reports %d (raw %d%s) while silent or settling, dropped\n",
		e->name, r.pct, r.raw, r.mute ? ", muted" : "");
	break;

      case FOLLOWED_WAKE:
	DPRINTF(E_INFO, L_UPNP, "'%s' reports %d (raw %d) while silent: a wake\n", e->name, r.pct, r.raw);
	if (evchan_event_receiver_command(r.row_id, r.id, r.row_name, "wake", "upnp") < 0)
	  DPRINTF(E_DBG, L_UPNP, "Wake from '%s' not reported, no event channel\n", e->name);
	break;

      case FOLLOWED_GONE:
	break;
    }
}

static bool
peer_ok_cb(const char *peer_ip, void *arg)
{
  struct follow_entry *e;

  for (e = entries; e; e = e->next)
    {
      if (strcmp(e->address, peer_ip) == 0 && upnp_sub_has_subscription(&e->sub))
	return true;
    }

  DPRINTF(E_DBG, L_UPNP, "Refused a connection from %s, which holds no subscription\n", peer_ip);
  return false;
}

static int
notify_cb(const char *peer_ip, const struct upnp_http_head *head, const char *body, size_t body_len, void *arg)
{
  struct follow_entry *e;
  struct upnp_rc_state st;
  uint64_t id;
  char *end;

  if (strncmp(head->target, "/upnp/", 6) != 0 || strlen(head->target) != 6 + 16)
    return 404;
  id = strtoull(head->target + 6, &end, 16);
  if (*end != '\0')
    return 404;

  e = entry_find(id);
  if (!e || strcmp(e->address, peer_ip) != 0)
    {
      DPRINTF(E_DBG, L_UPNP, "Event from %s for %s, which is not followed there\n", peer_ip, head->target);
      return 404;
    }

  if (!upnp_sub_sid_ok(&e->sub, head->sid, monotonic_ms()))
    {
      DPRINTF(E_DBG, L_UPNP, "Event from '%s' on a subscription not held (%s)\n", e->name, head->sid);
      return 412;
    }

  entry_step(e, UPNP_EV_NOTIFY, NULL);
  if (e->sub.state == UPNP_SUB_UNSUBSCRIBING)
    return 200;

  if (upnp_notify_parse(body, body_len, &st) < 0 || (!st.has_volume && !st.has_mute))
    {
      DPRINTF(E_SPAM, L_UPNP, "Event from '%s' with no Master level\n", e->name);
      return 200;
    }

  DPRINTF(E_DBG, L_UPNP, "Event from '%s': volume %d%s, mute %d%s\n", e->name,
	  st.volume, st.has_volume ? "" : " (absent)", st.mute, st.has_mute ? "" : " (absent)");

  upnp_rc_state_merge(&e->known, &st);
  timer_in_ms(e->coalesce, COALESCE_MS);
  return 200;
}


/* ------------------------------- Speakers --------------------------------- */

// Thread: player
static void
speaker_seen_cb(struct player_speaker_info *spk, void *arg)
{
  struct speaker_list *list = arg;
  struct speaker_seen *items;

  if (list->n == list->cap)
    {
      items = realloc(list->items, (list->cap + 16) * sizeof(*items));
      if (!items)
	return;
      list->items = items;
      list->cap += 16;
    }

  items = &list->items[list->n++];
  items->id = spk->id;
  snprintf(items->name, sizeof(items->name), "%s", spk->name);
  snprintf(items->config_name, sizeof(items->config_name), "%s", spk->config_name);
  snprintf(items->address, sizeof(items->address), "%s", spk->v4_address);
}

static struct follow_entry *
entry_new(const struct speaker_seen *spk)
{
  struct follow_entry *e;

  CHECK_NULL(L_UPNP, e = calloc(1, sizeof(*e)));
  CHECK_NULL(L_UPNP, e->timer = evtimer_new(evbase, entry_timer_cb, e));
  CHECK_NULL(L_UPNP, e->coalesce = evtimer_new(evbase, coalesce_cb, e));

  e->id = spk->id;
  upnp_sub_init(&e->sub);
  e->next = entries;
  entries = e;

  return e;
}

// Compares the speakers the player knows with the entries, and enables,
// moves or disables entries to match
static void
speakers_reconcile(void)
{
  struct speaker_list list = { 0 };
  struct speaker_seen *spk;
  struct follow_entry *e;
  struct follow_entry *next;
  bool on;
  int i;

  if (shut_down)
    return;

  on = listening && config_get_bool("upnp_volume_follow", true);
  if (on)
    player_speaker_enumerate(speaker_seen_cb, &list);

  for (e = entries; e; e = e->next)
    e->seen = false;

  for (i = 0; i < list.n; i++)
    {
      spk = &list.items[i];
      if (!spk->address[0] || !config_get_device_bool(spk->config_name, "upnp_follow", true))
	continue;

      e = entry_find(spk->id);
      if (!e)
	{
	  e = entry_new(spk);
	  e->seen = true;
	  snprintf(e->name, sizeof(e->name), "%s", spk->name);
	  snprintf(e->config_name, sizeof(e->config_name), "%s", spk->config_name);
	  snprintf(e->address, sizeof(e->address), "%s", spk->address);
	  entry_step(e, UPNP_EV_ENABLE, NULL);
	  continue;
	}

      e->seen = true;
      snprintf(e->name, sizeof(e->name), "%s", spk->name);
      if (strcmp(e->address, spk->address) != 0)
	{
	  DPRINTF(E_INFO, L_UPNP, "'%s' moved from %s to %s\n", e->name, e->address, spk->address);
	  // The old address is still in the event URL for the unsubscribe
	  entry_step(e, UPNP_EV_ADDRESS_CHANGED, NULL);
	  snprintf(e->address, sizeof(e->address), "%s", spk->address);
	  e->location_warned = false;
	}
      if (e->sub.state == UPNP_SUB_UNSUBSCRIBING)
	entry_step(e, UPNP_EV_ENABLE, NULL);
    }

  free(list.items);

  for (e = entries; e; e = next)
    {
      next = e->next;
      if (!e->seen)
	entry_step(e, UPNP_EV_DISABLE, NULL);
    }
}

static void
settle_cb(evutil_socket_t fd, short what, void *arg)
{
  speakers_reconcile();
}

static void
poke_cb(evutil_socket_t fd, short what, void *arg)
{
  char buf[64];

  while (read(fd, buf, sizeof(buf)) > 0)
    ;

  if (!evtimer_pending(settle_ev, NULL))
    timer_in_ms(settle_ev, SETTLE_MS);
}

// Thread: player. Must not block or allocate.
static void
listener_cb(short event_mask, void *ctx)
{
  uint64_t one = 1;

  if (!atomic_load_explicit(&enabled, memory_order_relaxed))
    return;

  (void)!write(poke_fd, &one, sizeof(one));
}


/* -------------------------------- Commands -------------------------------- */

static enum command_state
start_exec(void *arg, int *retval)
{
  int port;

  *retval = 0;

  port = config_get_int("upnp_event_port", DEFAULT_EVENT_PORT);
  if (port < 1 || port > 65535 || upnp_http_listen(evbase, port, peer_ok_cb, notify_cb, NULL) < 0)
    {
      DPRINTF(E_WARN, L_UPNP, "Following speaker levels over UPnP is off: could not listen on port %d: %s\n", port, strerror(errno));
      return COMMAND_END;
    }

  ssdp_fd = upnp_ssdp_open();
  if (ssdp_fd < 0)
    {
      DPRINTF(E_WARN, L_UPNP, "Following speaker levels over UPnP is off: could not open the search socket: %s\n", strerror(errno));
      upnp_http_listen_stop();
      return COMMAND_END;
    }

  CHECK_NULL(L_UPNP, ssdp_ev = event_new(evbase, ssdp_fd, EV_READ | EV_PERSIST, ssdp_read_cb, NULL));
  event_add(ssdp_ev, NULL);

  listening = true;
  atomic_store_explicit(&enabled, true, memory_order_release);

  DPRINTF(E_INFO, L_UPNP, "Listening for renderer events on port %d%s\n", port,
	  config_get_bool("upnp_volume_follow", true) ? "" : " (following is off in the settings)");

  // The speakers already found, on the loop rather than in this command
  timer_in_ms(settle_ev, 0);
  return COMMAND_END;
}

static enum command_state
settings_changed_exec(void *arg, int *retval)
{
  *retval = 0;

  if (!shut_down)
    timer_in_ms(settle_ev, 0);

  return COMMAND_END;
}

static void
shutdown_finish(void)
{
  if (shutdown_ev)
    event_free(shutdown_ev);
  shutdown_ev = NULL;

  upnp_http_cancel_all();
  commands_exec_end(cmdbase, 0);
}

static void
shutdown_unsub_cb(int status, const struct upnp_http_head *head, const char *body, size_t body_len, const char *err, void *arg)
{
  char *name = arg;

  DPRINTF(E_INFO, L_UPNP, "Unsubscribed from '%s'%s%s\n", name, (status == 200) ? "" : ": ",
	  (status == 200) ? "" : (status < 0) ? err : "refused");
  free(name);

  if (--shutdown_pending == 0 && shutdown_ev)
    shutdown_finish();
}

static void
shutdown_timer_cb(evutil_socket_t fd, short what, void *arg)
{
  DPRINTF(E_LOG, L_UPNP, "%d unsubscribe(s) not answered in %d ms\n", shutdown_pending, SHUTDOWN_WAIT_MS);
  shutdown_finish();
}

static enum command_state
shutdown_exec(void *arg, int *retval)
{
  struct follow_entry *e;
  char host[16];
  char path[256];
  char headers[160];
  unsigned short port;
  char *name;

  *retval = 0;

  if (shut_down)
    return COMMAND_END;
  shut_down = true;

  // From here the player's callback does nothing
  atomic_store_explicit(&enabled, false, memory_order_release);

  if (poke_ev)
    event_free(poke_ev);
  poke_ev = NULL;
  if (settle_ev)
    event_free(settle_ev);
  settle_ev = NULL;
  if (search_ev)
    event_free(search_ev);
  search_ev = NULL;
  if (ssdp_ev)
    event_free(ssdp_ev);
  ssdp_ev = NULL;
  if (ssdp_fd >= 0)
    close(ssdp_fd);
  ssdp_fd = -1;

  upnp_http_listen_stop();
  upnp_http_cancel_all();

  // Every renderer holding a subscription is told, so it stops sending
  shutdown_pending = 0;
  while (entries)
    {
      e = entries;
      if (e->sub.sid[0] && e->sub.state != UPNP_SUB_SUBSCRIBING
	  && upnp_url_split(e->sub.event_url, host, sizeof(host), &port, path, sizeof(path)) == 0)
	{
	  snprintf(headers, sizeof(headers), "SID: %s\r\n", e->sub.sid);
	  CHECK_NULL(L_UPNP, name = strdup(e->name));
	  if (upnp_http_request(evbase, "UNSUBSCRIBE", host, port, path, headers, false, 0, shutdown_unsub_cb, name))
	    shutdown_pending++;
	  else
	    free(name);
	}

      // Its request was cancelled above
      e->req = NULL;
      entry_free(e);
    }

  if (shutdown_pending == 0)
    return COMMAND_END;

  CHECK_NULL(L_UPNP, shutdown_ev = evtimer_new(evbase, shutdown_timer_cb, NULL));
  timer_in_ms(shutdown_ev, SHUTDOWN_WAIT_MS);

  // One pending event: whichever of the last answer and the timer comes first
  *retval = 1;
  return COMMAND_PENDING;
}

static void *
upnp_thread(void *arg)
{
  thread_setname("upnp");

  event_base_dispatch(evbase);

  pthread_exit(NULL);
}


/* ------------------------------- Interface -------------------------------- */

static void
resources_free(void)
{
  if (poke_ev)
    event_free(poke_ev);
  if (settle_ev)
    event_free(settle_ev);
  if (search_ev)
    event_free(search_ev);
  poke_ev = NULL;
  settle_ev = NULL;
  search_ev = NULL;

  if (poke_rfd >= 0)
    close(poke_rfd);
  if (poke_fd >= 0 && poke_fd != poke_rfd)
    close(poke_fd);
  poke_rfd = -1;
  poke_fd = -1;

  if (evbase)
    event_base_free(evbase);
  evbase = NULL;
}

// Thread: main
int
upnp_follow_init(void)
{
  int ret;

#ifdef HAVE_EVENTFD
  poke_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  poke_rfd = poke_fd;
#else
  {
    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0)
      {
	poke_rfd = fds[0];
	poke_fd = fds[1];
      }
  }
#endif
  if (poke_fd < 0)
    {
      DPRINTF(E_LOG, L_UPNP, "Following speaker levels over UPnP is off: could not create the wake-up descriptor: %s\n", strerror(errno));
      return -1;
    }

  evbase = event_base_new();
  if (!evbase)
    goto error;

  poke_ev = event_new(evbase, poke_rfd, EV_READ | EV_PERSIST, poke_cb, NULL);
  settle_ev = evtimer_new(evbase, settle_cb, NULL);
  search_ev = evtimer_new(evbase, search_cb, NULL);
  cmdbase = commands_base_new(evbase, NULL);
  if (!poke_ev || !settle_ev || !search_ev || !cmdbase || event_add(poke_ev, NULL) < 0)
    goto error;

  // Before the player thread exists: the listener list is not locked
  if (listener_add(listener_cb, LISTENER_SPEAKER, NULL) < 0)
    goto error;

  ret = pthread_create(&tid_upnp, NULL, upnp_thread, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_UPNP, "Following speaker levels over UPnP is off: could not start the thread: %s\n", strerror(ret));
      listener_remove(listener_cb);
      goto error;
    }

  return 0;

 error:
  if (cmdbase)
    commands_base_free(cmdbase);
  cmdbase = NULL;
  resources_free();
  return -1;
}

// Thread: main
void
upnp_follow_start(void)
{
  if (!cmdbase)
    return;

  commands_exec_sync(cmdbase, start_exec, NULL, NULL);
}

// Thread: main
void
upnp_follow_shutdown(void)
{
  if (!cmdbase)
    return;

  commands_exec_sync(cmdbase, shutdown_exec, NULL, NULL);
}

// Thread: main
void
upnp_follow_deinit(void)
{
  if (!cmdbase)
    return;

  // Normally done already; this covers a start-up that failed part way
  upnp_follow_shutdown();

  commands_base_destroy(cmdbase);
  cmdbase = NULL;

  if (pthread_join(tid_upnp, NULL) != 0)
    DPRINTF(E_LOG, L_UPNP, "Could not join the UPnP thread: %s\n", strerror(errno));

  // Safe because the only caller of listener_notify is the player thread,
  // which has been joined
  listener_remove(listener_cb);

  resources_free();
}

// Any thread
void
upnp_follow_settings_changed(void)
{
  if (!cmdbase)
    return;

  commands_exec_async(cmdbase, settings_changed_exec, NULL);
}
