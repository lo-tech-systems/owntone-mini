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

// Runs the network side of following a renderer's volume (search, describe,
// subscribe, events, renew, unsubscribe) against tools/upnp_fake_renderer.py,
// with no speaker and no daemon. Built by "make check" but not run by it, as
// it needs the fake running:
//
//   tools/upnp_fake_renderer.py --addr 127.0.0.1 --http-port 49152 --ssdp-port 1900 &
//   src/upnp_http_check [-v] [fake address] [http port] [ssdp port] [listen port]
//
// Exits 0 when every check passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing check too).

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <event2/event.h>

#include "../../upnp_parse.h"
#include "../../upnp_sub.h"
#include "../../upnp_http.h"

#define ST_RC     "urn:schemas-upnp-org:service:RenderingControl:1"
#define CB_PATH   "/upnp/0011223344556677"

static int checks_run;
static int checks_failed;
static bool verbose;

#define CHECK(name, cond) \
  do { \
    checks_run++; \
    if (!(cond)) \
      { \
	printf("FAIL %s: line %d: %s\n", name, __LINE__, #cond); \
	checks_failed++; \
      } \
    else if (verbose) \
      printf("PASS %s\n", name); \
  } while (0)

static struct event_base *evbase;
static const char *fake_ip = "127.0.0.1";
static unsigned short http_port = 49152;
static unsigned short ssdp_port = 1900;
static unsigned short listen_port = 3691;

static struct upnp_sub sub;
static struct upnp_rc_state known;
static int notifies;
static bool refuse_peers;

static uint64_t
now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
tick_cb(evutil_socket_t fd, short what, void *arg)
{
}

// Runs the loop until *flag is set or ms have passed
static bool
wait_for(bool *flag, int ms)
{
  struct timeval tv = { 0, 20000 };
  struct event *tick = evtimer_new(evbase, tick_cb, NULL);
  uint64_t end = now_ms() + ms;

  while (!*flag && now_ms() < end)
    {
      evtimer_add(tick, &tv);
      event_base_loop(evbase, EVLOOP_ONCE);
    }

  event_free(tick);
  return *flag;
}

static void
run_for(int ms)
{
  bool never = false;

  wait_for(&never, ms);
}


/* ------------------------------- Requests --------------------------------- */

struct result {
  bool done;
  int status;
  struct upnp_http_head head;
  char body[65536];
  size_t len;
  char err[96];
  uint64_t at_ms;
};

static void
result_cb(int status, const struct upnp_http_head *head, const char *body, size_t len, const char *err, void *arg)
{
  struct result *r = arg;

  r->done = true;
  r->status = status;
  r->at_ms = now_ms();
  if (head)
    r->head = *head;
  if (body && len < sizeof(r->body))
    {
      memcpy(r->body, body, len);
      r->len = len;
    }
  r->body[r->len] = '\0';
  snprintf(r->err, sizeof(r->err), "%s", err ? err : "");
}

static struct result *
request(const char *method, const char *path, const char *headers, bool want_body, int wait_ms)
{
  static struct result r;
  struct upnp_http_req *req;

  memset(&r, 0, sizeof(r));
  req = upnp_http_request(evbase, method, fake_ip, http_port, path, headers, want_body, sizeof(r.body) - 1, result_cb, &r);
  if (!req)
    {
      r.done = true;
      r.status = -1;
      snprintf(r.err, sizeof(r.err), "not started");
      return &r;
    }

  if (!wait_for(&r.done, wait_ms))
    {
      upnp_http_cancel(req);
      r.status = -2;
    }

  return &r;
}

// A control request to the fake; its JSON answer
static const char *
ctl(const char *path)
{
  struct result *r = request("GET", path, NULL, true, 15000);

  return (r->status == 200) ? r->body : "";
}

static void
sub_step(enum upnp_sub_event ev, const struct upnp_sub_args *args, struct upnp_sub_actions *act)
{
  upnp_sub_step(&sub, ev, args, now_ms(), act);
}

static bool
subscribe(const char *local_ip, char *sid, size_t sidlen, long *timeout_s)
{
  char headers[256];
  struct result *r;

  snprintf(headers, sizeof(headers), "CALLBACK: <http://%s:%u" CB_PATH ">\r\nNT: upnp:event\r\nTIMEOUT: Second-1800\r\n",
	   local_ip, listen_port);
  r = request("SUBSCRIBE", "/upnp/event/rendercontrol1", headers, false, 3000);
  if (r->status != 200 || !r->head.sid[0])
    return false;

  snprintf(sid, sidlen, "%s", r->head.sid);
  *timeout_s = r->head.timeout_s;
  return true;
}


/* ------------------------------- Listener --------------------------------- */

static bool
peer_cb(const char *peer, void *arg)
{
  return !refuse_peers && strcmp(peer, fake_ip) == 0;
}

static int
notify_cb(const char *peer, const struct upnp_http_head *head, const char *body, size_t len, void *arg)
{
  struct upnp_rc_state st;
  struct upnp_sub_actions act;

  if (strcmp(head->target, CB_PATH) != 0)
    return 404;
  if (!upnp_sub_sid_ok(&sub, head->sid, now_ms()))
    return 412;

  sub_step(UPNP_EV_NOTIFY, NULL, &act);
  if (upnp_notify_parse(body, len, &st) == 0)
    upnp_rc_state_merge(&known, &st);
  notifies++;
  return 200;
}


/* --------------------------------- SSDP ----------------------------------- */

static struct {
  bool got;
  char from[INET_ADDRSTRLEN];
  struct upnp_http_head head;
} ssdp_reply;

static void
ssdp_read_cb(evutil_socket_t fd, short what, void *arg)
{
  struct sockaddr_in from;
  socklen_t fromlen = sizeof(from);
  char buf[2048];
  ssize_t n;

  while ((n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen)) > 0)
    {
      if (upnp_http_head_parse(buf, n, sizeof(buf), &ssdp_reply.head) != 1 || ssdp_reply.head.status != 200)
	continue;
      inet_ntop(AF_INET, &from.sin_addr, ssdp_reply.from, sizeof(ssdp_reply.from));
      ssdp_reply.got = true;
    }
}


/* -------------------------------- Checks ---------------------------------- */

static int
waits_for_notifies(int before, int ms)
{
  uint64_t end = now_ms() + ms;

  while (notifies == before && now_ms() < end)
    run_for(20);

  return notifies - before;
}

int
main(int argc, char **argv)
{
  struct upnp_rc_service rc;
  struct upnp_sub_actions act;
  struct upnp_sub_args args;
  struct event *ssdp_ev;
  struct result *r;
  char host[16];
  char path[256];
  char local_ip[INET_ADDRSTRLEN];
  char sid[96];
  char sid2[96];
  char held_sid[96];
  char headers[160];
  unsigned short port;
  long timeout_s;
  uint64_t t0;
  int fd;
  int n;
  int i = 1;

  if (argc > i && strcmp(argv[i], "-v") == 0)
    {
      verbose = true;
      i++;
    }
  if (argc > i)
    fake_ip = argv[i++];
  if (argc > i)
    http_port = atoi(argv[i++]);
  if (argc > i)
    ssdp_port = atoi(argv[i++]);
  if (argc > i)
    listen_port = atoi(argv[i++]);

  evbase = event_base_new();
  upnp_sub_init(&sub);
  sub_step(UPNP_EV_ENABLE, NULL, &act);

  // Search, sent straight to the fake: the group itself is checked on hardware
  CHECK("local address", upnp_local_ip_for(fake_ip, local_ip, sizeof(local_ip)) == 0);
  fd = upnp_ssdp_open();
  CHECK("ssdp socket", fd >= 0);
  ssdp_ev = event_new(evbase, fd, EV_READ | EV_PERSIST, ssdp_read_cb, NULL);
  event_add(ssdp_ev, NULL);
  CHECK("search sent", upnp_ssdp_search(fd, local_ip, ST_RC, fake_ip, ssdp_port) == 0);
  CHECK("search answered", wait_for(&ssdp_reply.got, 3000));
  CHECK("reply from the renderer", strcmp(ssdp_reply.from, fake_ip) == 0);
  CHECK("reply location on the renderer",
	upnp_url_split(ssdp_reply.head.location, host, sizeof(host), &port, path, sizeof(path)) == 0 && strcmp(host, fake_ip) == 0);
  event_free(ssdp_ev);
  close(fd);

  args = (struct upnp_sub_args){ .location = ssdp_reply.head.location };
  sub_step(UPNP_EV_SSDP_MATCH, &args, &act);
  CHECK("describe wanted", act.flags & UPNP_ACT_GET_DESCRIPTION);

  // Describe
  r = request("GET", path, NULL, true, 3000);
  CHECK("description fetched", r->status == 200 && r->len > 0);
  CHECK("description parsed", upnp_description_parse(r->body, r->len, ssdp_reply.head.location, &rc) == 0);
  CHECK("event url", strstr(rc.event_url, "/upnp/event/rendercontrol1") != NULL);
  args = (struct upnp_sub_args){ .event_url = rc.event_url };
  sub_step(UPNP_EV_DESCRIBED, &args, &act);
  CHECK("subscribe wanted", act.flags & UPNP_ACT_SUBSCRIBE);

  // Subscribe, with the first event
  CHECK("listening", upnp_http_listen(evbase, listen_port, peer_cb, notify_cb, NULL) == 0);
  n = notifies;
  CHECK("subscribed", subscribe(local_ip, sid, sizeof(sid), &timeout_s));
  CHECK("timeout granted", timeout_s == 1800);
  args = (struct upnp_sub_args){ .sid = sid, .timeout_s = timeout_s };
  sub_step(UPNP_EV_SUB_OK, &args, &act);
  CHECK("following", sub.state == UPNP_SUB_FOLLOWING);
  CHECK("first event", waits_for_notifies(n, 2000) == 1);
  CHECK("first event state", known.has_volume && known.volume == 25 && known.has_mute && !known.mute);

  // Changes, and a change of mute alone merged into what is known
  CHECK("volume change answered", strstr(ctl("/ctl/set?volume=31"), "[200]") != NULL);
  CHECK("volume change read", known.volume == 31);
  CHECK("mute change answered", strstr(ctl("/ctl/set?mute=1"), "[200]") != NULL);
  CHECK("mute merged", known.volume == 31 && known.mute);
  ctl("/ctl/set?mute=0");

  // Chunked event taken
  n = notifies;
  CHECK("chunked answered", strstr(ctl("/ctl/notify?kind=chunked"), "[200]") != NULL);
  CHECK("chunked read", notifies == n + 1 && known.volume == 31);

  // A SID we do not hold is refused
  n = notifies;
  CHECK("wrong sid refused", strstr(ctl("/ctl/notify?kind=bogus_sid"), "[412]") != NULL);
  CHECK("wrong sid not read", notifies == n);

  // A body over the limit is closed without a reply
  CHECK("oversize closed", strstr(ctl("/ctl/notify?kind=oversize"), "[\"closed\"]") != NULL);
  CHECK("oversize not read", notifies == n);

  // A peer we hold nothing with is closed before anything is read
  refuse_peers = true;
  CHECK("wrong peer closed", strstr(ctl("/ctl/notify"), "[\"closed\"]") != NULL);
  refuse_peers = false;
  CHECK("wrong peer not read", notifies == n);

  // Eight idle connections are held, the ninth is closed at once
  CHECK("ninth connection closed",
	strstr(ctl("/ctl/flood?n=9"), "[\"open\", \"open\", \"open\", \"open\", \"open\", \"open\", \"open\", \"open\", \"closed\"]") != NULL);
  // ... and the held ones are closed by the deadline
  run_for(UPNP_HTTP_TIMEOUT_S * 1000 + 500);
  CHECK("events taken after the flood", strstr(ctl("/ctl/notify"), "[200]") != NULL);

  // Renew
  snprintf(headers, sizeof(headers), "SID: %s\r\nTIMEOUT: Second-1800\r\n", sid);
  sub_step(UPNP_EV_TIMER, NULL, &act);
  CHECK("renew wanted", act.flags & UPNP_ACT_RENEW);
  r = request("SUBSCRIBE", "/upnp/event/rendercontrol1", headers, false, 3000);
  CHECK("renewed", r->status == 200);
  args = (struct upnp_sub_args){ .sid = r->head.sid, .timeout_s = r->head.timeout_s };
  sub_step(UPNP_EV_SUB_OK, &args, &act);
  CHECK("following after renew", sub.state == UPNP_SUB_FOLLOWING);

  // A refused renewal, then a fresh subscribe
  ctl("/ctl/mode?renew412=1");
  sub_step(UPNP_EV_TIMER, NULL, &act);
  r = request("SUBSCRIBE", "/upnp/event/rendercontrol1", headers, false, 3000);
  CHECK("renewal refused", r->status == 412);
  args = (struct upnp_sub_args){ .status = r->status };
  sub_step(UPNP_EV_SUB_REFUSED, &args, &act);
  CHECK("resubscribe wanted", (act.flags & UPNP_ACT_SUBSCRIBE) && sub.state == UPNP_SUB_SUBSCRIBING);
  ctl("/ctl/mode?renew412=0");
  n = notifies;
  CHECK("resubscribed", subscribe(local_ip, sid2, sizeof(sid2), &timeout_s));
  CHECK("new sid", strcmp(sid, sid2) != 0);
  args = (struct upnp_sub_args){ .sid = sid2, .timeout_s = timeout_s };
  sub_step(UPNP_EV_SUB_OK, &args, &act);
  CHECK("first event after resubscribe", waits_for_notifies(n, 2000) == 1);
  CHECK("old subscription still taken a while", upnp_sub_sid_ok(&sub, sid, now_ms()));

  // A subscribe answered with no length on a connection held open
  ctl("/ctl/mode?holdopen=1");
  t0 = now_ms();
  CHECK("held-open answer taken", subscribe(local_ip, held_sid, sizeof(held_sid), &timeout_s));
  CHECK("held-open answer at once", now_ms() - t0 < 1000);
  ctl("/ctl/mode?holdopen=0");
  snprintf(headers, sizeof(headers), "SID: %s\r\n", held_sid);
  r = request("UNSUBSCRIBE", "/upnp/event/rendercontrol1", headers, false, 3000);
  CHECK("held-open unsubscribed", r->status == 200);

  // A request cancelled in flight never calls back
  {
    static struct result late;
    struct upnp_http_req *req;

    ctl("/ctl/mode?slow=1");
    memset(&late, 0, sizeof(late));
    req = upnp_http_request(evbase, "GET", fake_ip, http_port, path, NULL, true, sizeof(late.body) - 1, result_cb, &late);
    CHECK("slow request started", req != NULL);
    run_for(100);
    upnp_http_cancel(req);
    run_for(3500);
    CHECK("cancelled request silent", !late.done);
    ctl("/ctl/mode?slow=0");
  }

  // A refused port is a failure, not a hang
  {
    static struct result refused;
    struct upnp_http_req *req;

    memset(&refused, 0, sizeof(refused));
    req = upnp_http_request(evbase, "GET", fake_ip, 1, "/", NULL, true, 1024, result_cb, &refused);
    CHECK("refused connection reported", req == NULL || (wait_for(&refused.done, 3000) && refused.status == -1));
  }

  // Unsubscribe, answered within a second
  sub_step(UPNP_EV_DISABLE, NULL, &act);
  CHECK("unsubscribe wanted", (act.flags & UPNP_ACT_UNSUBSCRIBE) && strcmp(act.unsub_sid, sid2) == 0);
  snprintf(headers, sizeof(headers), "SID: %s\r\n", act.unsub_sid);
  t0 = now_ms();
  r = request("UNSUBSCRIBE", "/upnp/event/rendercontrol1", headers, false, 1000);
  CHECK("unsubscribed", r->status == 200 && r->at_ms - t0 < 1000);
  sub_step(UPNP_EV_UNSUB_DONE, NULL, &act);
  CHECK("free", act.flags & UPNP_ACT_FREE);
  {
    char state[4096];
    char *subs_end;

    snprintf(state, sizeof(state), "%s", ctl("/ctl/state"));
    subs_end = strstr(state, "\"renewals\"");
    if (subs_end)
      *subs_end = 0;
    CHECK("renderer forgot us", subs_end && !strstr(state, sid2) && strstr(subs_end + 1, sid2));
  }

  // Shutdown with requests still in flight
  ctl("/ctl/mode?slow=1");
  {
    static struct result pending;

    memset(&pending, 0, sizeof(pending));
    upnp_http_request(evbase, "GET", fake_ip, http_port, path, NULL, true, sizeof(pending.body) - 1, result_cb, &pending);
    run_for(100);
    upnp_http_cancel_all();
    upnp_http_listen_stop();
    run_for(3500);
    CHECK("nothing calls back after shutdown", !pending.done);
  }
  ctl("/ctl/mode?slow=0");

  event_base_free(evbase);

  printf("%d checks, %d failed\n", checks_run, checks_failed);

  return checks_failed ? 1 : 0;
}
