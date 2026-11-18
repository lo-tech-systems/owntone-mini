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

#ifndef __UPNP_HTTP_H__
#define __UPNP_HTTP_H__

#include <stdbool.h>
#include <stddef.h>
#include <event2/event.h>

#include "upnp_parse.h"

// The network side of following a renderer: a minimal HTTP/1.1 client for
// its description and subscriptions, a listener for the events it sends, and
// the SSDP search. libevent's own HTTP server cannot take NOTIFY, so this is
// written on bufferevents. Everything runs on the one event base given and
// must be called on its thread. Nothing here logs; failures are reported to
// the caller with a reason.

#define UPNP_HTTP_TIMEOUT_S      5
#define UPNP_HTTP_HEAD_MAX       4096
#define UPNP_NOTIFY_BODY_MAX     16384
#define UPNP_NOTIFY_CONNS_MAX    8

// status is the response status, or -1 when there was none (head NULL, err
// says why). With want_body false the response is complete at the head, and
// body is NULL.
typedef void (*upnp_http_done_cb)(int status, const struct upnp_http_head *head,
				  const char *body, size_t body_len, const char *err, void *arg);

struct upnp_http_req;

// One request and one response with "Connection: close". extra_headers, if
// not NULL, is a block of lines each ending in CRLF. Returns NULL if the
// request could not be started, and then cb is not called.
struct upnp_http_req *
upnp_http_request(struct event_base *evbase, const char *method, const char *host, unsigned short port,
		  const char *path, const char *extra_headers, bool want_body, size_t max_body,
		  upnp_http_done_cb cb, void *arg);

// cb is never called after this. A request whose cb is running is left alone.
void
upnp_http_cancel(struct upnp_http_req *req);

// Cancels every request in flight
void
upnp_http_cancel_all(void);

// Asked for each new connection before anything is read from it: false
// closes it without a reply
typedef bool (*upnp_peer_ok_cb)(const char *peer_ip, void *arg);

// Called for each complete NOTIFY; returns the status to answer (200, 404 or
// 412). The body is only valid during the call.
typedef int (*upnp_notify_cb)(const char *peer_ip, const struct upnp_http_head *head,
			      const char *body, size_t body_len, void *arg);

// Listens on 0.0.0.0:port. 0 or -1 (errno set).
int
upnp_http_listen(struct event_base *evbase, unsigned short port, upnp_peer_ok_cb peer_cb, upnp_notify_cb notify_cb, void *arg);

// Closes the listener and every connection on it
void
upnp_http_listen_stop(void);

// A non-blocking UDP socket for SSDP searches, multicast TTL 2 and loop on.
// The descriptor or -1.
int
upnp_ssdp_open(void);

// Sends an M-SEARCH for st out of the interface that has local_ip, to the
// SSDP group or, if to_ip is not NULL, to to_ip:to_port alone. 0 or -1.
int
upnp_ssdp_search(int fd, const char *local_ip, const char *st, const char *to_ip, unsigned short to_port);

// The local address the system would use to reach peer_ip. 0 or -1.
int
upnp_local_ip_for(const char *peer_ip, char *out, size_t outlen);

#endif /* !__UPNP_HTTP_H__ */
