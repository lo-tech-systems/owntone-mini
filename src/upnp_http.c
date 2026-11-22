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
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>

#include "upnp_http.h"

#ifndef PACKAGE_VERSION
# define PACKAGE_VERSION "unknown"
#endif

#define SSDP_GROUP  "239.255.255.250"
#define SSDP_PORT   1900

// Room for chunk framing over the body's own cap
#define CHUNK_FRAMING_MAX  4096


/* -------------------------------- Client ---------------------------------- */

struct upnp_http_req {
  struct upnp_http_req *prev;
  struct upnp_http_req *next;

  struct bufferevent *bev;
  bool want_body;
  size_t max_body;
  upnp_http_done_cb cb;
  void *arg;

  bool head_done;
  struct upnp_http_head head;
  // Set while cb runs: the request is no longer in the list
  bool finishing;
};

static struct upnp_http_req *reqs;

static void
req_unlink(struct upnp_http_req *req)
{
  if (reqs == req)
    reqs = req->next;
  else if (req->prev)
    req->prev->next = req->next;
  if (req->next)
    req->next->prev = req->prev;
  req->prev = req->next = NULL;
}

static void
req_free(struct upnp_http_req *req)
{
  if (req->bev)
    {
      bufferevent_setcb(req->bev, NULL, NULL, NULL, NULL);
      bufferevent_free(req->bev);
    }
  free(req);
}

static void
req_finish(struct upnp_http_req *req, int status, const char *body, size_t body_len, const char *err)
{
  req_unlink(req);
  req->finishing = true;
  req->cb(status, (status >= 0) ? &req->head : NULL, body, body_len, err, req->arg);
  req_free(req);
}

static void
req_fail(struct upnp_http_req *req, const char *err)
{
  req_finish(req, -1, NULL, 0, err);
}

// The body so far, once complete: 1 done (*body, *len set; *owned to free),
// 0 more to come, -1 failed (*err set)
static int
body_ready(struct upnp_http_req *req, bool eof, char **body, size_t *len, bool *owned, const char **err)
{
  struct evbuffer *in = bufferevent_get_input(req->bev);
  size_t have = evbuffer_get_length(in);
  long n;
  char *copy;

  *owned = false;

  if (req->head.content_length >= 0)
    {
      if ((size_t)req->head.content_length > req->max_body)
	{
	  *err = "the body is over the limit";
	  return -1;
	}
      if (have < (size_t)req->head.content_length)
	{
	  if (eof)
	    {
	      *err = "the body was cut short";
	      return -1;
	    }
	  return 0;
	}
      *len = req->head.content_length;
      *body = (char *)evbuffer_pullup(in, *len);
      return 1;
    }

  if (req->head.chunked)
    {
      if (have > req->max_body + CHUNK_FRAMING_MAX)
	{
	  *err = "the body is over the limit";
	  return -1;
	}
      copy = malloc(have + 1);
      if (!copy)
	{
	  *err = "out of memory";
	  return -1;
	}
      evbuffer_copyout(in, copy, have);
      n = upnp_http_dechunk(copy, have);
      if (n == -2 && !eof)
	{
	  free(copy);
	  return 0;
	}
      if (n < 0 || (size_t)n > req->max_body)
	{
	  free(copy);
	  *err = (n == -2) ? "the body was cut short" : "the chunked body is malformed";
	  return -1;
	}
      *body = copy;
      *len = n;
      *owned = true;
      return 1;
    }

  // Neither length nor chunks: the body runs to the end of the connection
  if (have > req->max_body)
    {
      *err = "the body is over the limit";
      return -1;
    }
  if (!eof)
    return 0;
  *len = have;
  *body = (char *)evbuffer_pullup(in, have);
  return 1;
}

static void
req_progress(struct upnp_http_req *req, bool eof)
{
  struct evbuffer *in = bufferevent_get_input(req->bev);
  size_t have = evbuffer_get_length(in);
  size_t n;
  const char *err = NULL;
  char *body = NULL;
  size_t len = 0;
  bool owned;
  int ret;

  if (!req->head_done)
    {
      n = (have < UPNP_HTTP_HEAD_MAX) ? have : UPNP_HTTP_HEAD_MAX;
      ret = upnp_http_head_parse((const char *)evbuffer_pullup(in, n), n, UPNP_HTTP_HEAD_MAX, &req->head);
      if (ret < 0 || (ret == 0 && eof))
	{
	  req_fail(req, (ret < 0) ? "the response head is malformed" : "the connection closed before a response");
	  return;
	}
      if (ret == 0)
	return;

      req->head_done = true;
      evbuffer_drain(in, req->head.head_len);
      if (req->head.status < 200)
	{
	  req_fail(req, "an interim response is not expected");
	  return;
	}

      // A renderer may hold the connection open after the answer to a
      // subscribe, so that answer is complete here
      if (!req->want_body)
	{
	  req_finish(req, req->head.status, NULL, 0, NULL);
	  return;
	}
    }

  ret = body_ready(req, eof, &body, &len, &owned, &err);
  if (ret < 0)
    req_fail(req, err);
  else if (ret > 0)
    {
      req_finish(req, req->head.status, body, len, NULL);
      if (owned)
	free(body);
    }
}

static void
req_read_cb(struct bufferevent *bev, void *arg)
{
  req_progress(arg, false);
}

static void
req_event_cb(struct bufferevent *bev, short what, void *arg)
{
  struct upnp_http_req *req = arg;
  static char err[96];

  if (what & BEV_EVENT_CONNECTED)
    return;

  if (what & BEV_EVENT_EOF)
    {
      req_progress(req, true);
      return;
    }

  if (what & BEV_EVENT_TIMEOUT)
    req_fail(req, "timed out");
  else
    {
      snprintf(err, sizeof(err), "%s", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
      req_fail(req, err);
    }
}

struct upnp_http_req *
upnp_http_request(struct event_base *evbase, const char *method, const char *host, unsigned short port,
		  const char *path, const char *extra_headers, bool want_body, size_t max_body,
		  upnp_http_done_cb cb, void *arg)
{
  struct upnp_http_req *req;
  struct sockaddr_in sin;
  struct timeval tv = { UPNP_HTTP_TIMEOUT_S, 0 };

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &sin.sin_addr) != 1)
    return NULL;

  req = calloc(1, sizeof(*req));
  if (!req)
    return NULL;

  req->want_body = want_body;
  req->max_body = max_body;
  req->cb = cb;
  req->arg = arg;

  req->bev = bufferevent_socket_new(evbase, -1, BEV_OPT_CLOSE_ON_FREE);
  if (!req->bev)
    goto error;

  bufferevent_setcb(req->bev, req_read_cb, NULL, req_event_cb, req);
  bufferevent_set_timeouts(req->bev, &tv, &tv);

  if (evbuffer_add_printf(bufferevent_get_output(req->bev),
			  "%s %s HTTP/1.1\r\nHOST: %s:%u\r\n%sContent-Length: 0\r\nConnection: close\r\n\r\n",
			  method, path, host, port, extra_headers ? extra_headers : "") < 0)
    goto error;

  if (bufferevent_enable(req->bev, EV_READ | EV_WRITE) < 0)
    goto error;
  if (bufferevent_socket_connect(req->bev, (struct sockaddr *)&sin, sizeof(sin)) < 0)
    goto error;

  req->next = reqs;
  if (reqs)
    reqs->prev = req;
  reqs = req;

  return req;

 error:
  req_free(req);
  return NULL;
}

void
upnp_http_cancel(struct upnp_http_req *req)
{
  if (!req || req->finishing)
    return;

  req_unlink(req);
  req_free(req);
}

void
upnp_http_cancel_all(void)
{
  while (reqs)
    upnp_http_cancel(reqs);
}


/* ------------------------------- Listener --------------------------------- */

struct notify_conn {
  struct notify_conn *prev;
  struct notify_conn *next;

  struct bufferevent *bev;
  struct event *deadline;
  char peer[INET_ADDRSTRLEN];
  bool head_done;
  struct upnp_http_head head;
};

static struct evconnlistener *listener;
static struct event_base *listen_evbase;
static upnp_peer_ok_cb peer_ok;
static upnp_notify_cb notify;
static void *listen_arg;
static struct notify_conn *conns;
static int nconns;

static void
conn_free(struct notify_conn *c)
{
  if (conns == c)
    conns = c->next;
  else if (c->prev)
    c->prev->next = c->next;
  if (c->next)
    c->next->prev = c->prev;
  nconns--;

  if (c->deadline)
    event_free(c->deadline);
  if (c->bev)
    {
      bufferevent_setcb(c->bev, NULL, NULL, NULL, NULL);
      bufferevent_free(c->bev);
    }
  free(c);
}

static const char *
reason(int status)
{
  switch (status)
    {
      case 200: return "OK";
      case 400: return "Bad Request";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 412: return "Precondition Failed";
      default:  return "Error";
    }
}

static void
conn_written_cb(struct bufferevent *bev, void *arg)
{
  conn_free(arg);
}

static void
conn_event_cb(struct bufferevent *bev, short what, void *arg)
{
  conn_free(arg);
}

static void
conn_reply(struct notify_conn *c, int status)
{
  bufferevent_disable(c->bev, EV_READ);
  bufferevent_setcb(c->bev, NULL, conn_written_cb, conn_event_cb, c);
  evbuffer_add_printf(bufferevent_get_output(c->bev),
		      "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, reason(status));
}

static void
conn_read_cb(struct bufferevent *bev, void *arg)
{
  struct notify_conn *c = arg;
  struct evbuffer *in = bufferevent_get_input(bev);
  size_t have = evbuffer_get_length(in);
  size_t n;
  char *copy;
  const char *body = NULL;
  size_t len = 0;
  long dn;
  int ret;
  int status;

  if (!c->head_done)
    {
      n = (have < UPNP_HTTP_HEAD_MAX) ? have : UPNP_HTTP_HEAD_MAX;
      ret = upnp_http_head_parse((const char *)evbuffer_pullup(in, n), n, UPNP_HTTP_HEAD_MAX, &c->head);
      if (ret < 0)
	{
	  conn_free(c);
	  return;
	}
      if (ret == 0)
	return;

      c->head_done = true;
      evbuffer_drain(in, c->head.head_len);

      if (c->head.status != 0 || strcmp(c->head.method, "NOTIFY") != 0)
	{
	  conn_reply(c, 405);
	  return;
	}
      have = evbuffer_get_length(in);
    }

  if (c->head.content_length >= 0)
    {
      if (c->head.content_length > UPNP_NOTIFY_BODY_MAX)
	{
	  conn_free(c);
	  return;
	}
      if (have < (size_t)c->head.content_length)
	return;

      len = c->head.content_length;
      body = (const char *)evbuffer_pullup(in, len);
      status = notify(c->peer, &c->head, body, len, listen_arg);
      conn_reply(c, status);
      return;
    }

  if (c->head.chunked)
    {
      if (have > UPNP_NOTIFY_BODY_MAX + CHUNK_FRAMING_MAX)
	{
	  conn_free(c);
	  return;
	}
      copy = malloc(have + 1);
      if (!copy)
	{
	  conn_free(c);
	  return;
	}
      evbuffer_copyout(in, copy, have);
      dn = upnp_http_dechunk(copy, have);
      if (dn == -2)
	{
	  free(copy);
	  return;
	}
      if (dn < 0 || dn > UPNP_NOTIFY_BODY_MAX)
	{
	  free(copy);
	  conn_free(c);
	  return;
	}
      status = notify(c->peer, &c->head, copy, dn, listen_arg);
      free(copy);
      conn_reply(c, status);
      return;
    }

  // No body is announced: the event carries nothing
  status = notify(c->peer, &c->head, "", 0, listen_arg);
  conn_reply(c, status);
}

static void
conn_deadline_cb(evutil_socket_t fd, short what, void *arg)
{
  conn_free(arg);
}

static void
accept_cb(struct evconnlistener *l, evutil_socket_t fd, struct sockaddr *sa, int socklen, void *arg)
{
  struct notify_conn *c;
  struct timeval tv = { UPNP_HTTP_TIMEOUT_S, 0 };
  char peer[INET_ADDRSTRLEN];

  if (sa->sa_family != AF_INET || !inet_ntop(AF_INET, &((struct sockaddr_in *)sa)->sin_addr, peer, sizeof(peer)))
    {
      close(fd);
      return;
    }

  // Nothing is read from a peer we hold no subscription with
  if (!peer_ok(peer, listen_arg) || nconns >= UPNP_NOTIFY_CONNS_MAX)
    {
      close(fd);
      return;
    }

  c = calloc(1, sizeof(*c));
  if (!c)
    {
      close(fd);
      return;
    }

  c->next = conns;
  if (conns)
    conns->prev = c;
  conns = c;
  nconns++;
  memcpy(c->peer, peer, sizeof(c->peer));

  c->bev = bufferevent_socket_new(listen_evbase, fd, BEV_OPT_CLOSE_ON_FREE);
  c->deadline = evtimer_new(listen_evbase, conn_deadline_cb, c);
  if (!c->bev || !c->deadline)
    {
      if (!c->bev)
	close(fd);
      conn_free(c);
      return;
    }

  bufferevent_setcb(c->bev, conn_read_cb, NULL, conn_event_cb, c);
  bufferevent_enable(c->bev, EV_READ);
  // The whole request, however slowly it trickles in
  evtimer_add(c->deadline, &tv);
}

int
upnp_http_listen(struct event_base *evbase, unsigned short port, upnp_peer_ok_cb peer_cb, upnp_notify_cb notify_cb, void *arg)
{
  struct sockaddr_in sin;

  if (listener)
    return 0;

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = htonl(INADDR_ANY);

  listen_evbase = evbase;
  peer_ok = peer_cb;
  notify = notify_cb;
  listen_arg = arg;

  listener = evconnlistener_new_bind(evbase, accept_cb, NULL,
				     LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_EXEC,
				     16, (struct sockaddr *)&sin, sizeof(sin));

  return listener ? 0 : -1;
}

void
upnp_http_listen_stop(void)
{
  if (listener)
    evconnlistener_free(listener);
  listener = NULL;

  while (conns)
    conn_free(conns);
}


/* --------------------------------- SSDP ----------------------------------- */

int
upnp_ssdp_open(void)
{
  unsigned char ttl = 2;
  unsigned char loop = 1;
  int fd;

  fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0
      || setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0)
    {
      close(fd);
      return -1;
    }

  return fd;
}

int
upnp_ssdp_search(int fd, const char *local_ip, const char *st, const char *to_ip, unsigned short to_port)
{
  struct sockaddr_in to;
  struct in_addr ifaddr;
  char msg[512];
  int n;

  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(to_ip ? to_port : SSDP_PORT);
  if (inet_pton(AF_INET, to_ip ? to_ip : SSDP_GROUP, &to.sin_addr) != 1)
    return -1;

  if (!to_ip)
    {
      if (inet_pton(AF_INET, local_ip, &ifaddr) != 1)
	return -1;
      if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0)
	return -1;
    }

  n = snprintf(msg, sizeof(msg),
	       "M-SEARCH * HTTP/1.1\r\n"
	       "HOST: " SSDP_GROUP ":%d\r\n"
	       "MAN: \"ssdp:discover\"\r\n"
	       "MX: 2\r\n"
	       "ST: %s\r\n"
	       "USER-AGENT: Linux/1 UPnP/1.1 owntone-mini/%s\r\n"
	       "\r\n", SSDP_PORT, st, PACKAGE_VERSION);
  if (n < 0 || (size_t)n >= sizeof(msg))
    return -1;

  return (sendto(fd, msg, n, 0, (struct sockaddr *)&to, sizeof(to)) == n) ? 0 : -1;
}

int
upnp_local_ip_for(const char *peer_ip, char *out, size_t outlen)
{
  struct sockaddr_in sin;
  socklen_t len = sizeof(sin);
  int fd;
  int ret = -1;

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(SSDP_PORT);
  if (inet_pton(AF_INET, peer_ip, &sin.sin_addr) != 1)
    return -1;

  // Connecting a datagram socket sends nothing, but picks the route
  fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0
      && getsockname(fd, (struct sockaddr *)&sin, &len) == 0
      && inet_ntop(AF_INET, &sin.sin_addr, out, outlen))
    ret = 0;

  close(fd);
  return ret;
}
