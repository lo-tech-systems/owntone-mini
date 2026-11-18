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

#ifndef __UPNP_PARSE_H__
#define __UPNP_PARSE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Reading what a UPnP renderer sends: SSDP replies, HTTP message heads, the
// device description and RenderingControl events, and the rules that turn a
// renderer's volume into the player's level. No I/O, no logging and no state:
// every function reads only the length it is given.

// The longest header line and the most header lines a head may have
#define UPNP_HEAD_LINE_MAX   1024
#define UPNP_HEAD_LINES_MAX  64

// An HTTP message head: the start line and the headers up to the blank line
struct upnp_http_head {
  char method[16];        // request: "NOTIFY"; empty for a response
  char target[128];       // request target
  int status;             // response status; 0 for a request
  char sid[96];           // SID
  char nt[32];            // NT
  char nts[32];           // NTS
  char location[256];     // LOCATION (SSDP)
  char st[128];           // ST (SSDP)
  long timeout_s;         // TIMEOUT: Second-N; -1 absent, 0 "infinite"
  long content_length;    // -1 absent
  bool chunked;           // Transfer-Encoding: chunked
  size_t head_len;        // bytes up to and including the blank line
};

// Parses the head at the start of buf. Returns 1 when a complete head was
// parsed, 0 when more bytes are needed, -1 when it is malformed or no blank
// line comes within max bytes. Serves SSDP replies over UDP too.
int
upnp_http_head_parse(const char *buf, size_t len, size_t max, struct upnp_http_head *out);

// What a renderer's description says about its RenderingControl service
struct upnp_rc_service {
  char event_url[256];    // absolute, resolved against URLBase or LOCATION
  char control_url[256];
  char manufacturer[64];
  char model[64];
  char friendly_name[64];
};

// Returns 0 when a RenderingControl service (any version) was found, -1 when
// there is none, -2 when the description is malformed or its event URL is on
// a host other than the LOCATION's
int
upnp_description_parse(const char *xml, size_t len, const char *location, struct upnp_rc_service *out);

// The Master volume and mute of InstanceID 0, as far as an event carries them
struct upnp_rc_state {
  bool has_volume;
  int volume;
  bool has_mute;
  bool mute;
};

// Reads a RenderingControl event body: LastChange is unescaped (once or
// twice), then the Master Volume and Mute of InstanceID 0 are read. Other
// elements and instances, and the second speaker's Slave entries, are
// ignored. Returns 0 when the body has a LastChange (whether or not it has a
// Master level), -1 otherwise.
int
upnp_notify_parse(const char *body, size_t len, struct upnp_rc_state *out);

// Merges an event into the last known state: an event may carry only Volume
// or only Mute
void
upnp_rc_state_merge(struct upnp_rc_state *known, const struct upnp_rc_state *notify);

// Splits "http://a.b.c.d[:port]/path". Only http and a dotted-quad host are
// accepted. Returns 0 or -1.
int
upnp_url_split(const char *url, char *host, size_t hostlen, unsigned short *port, char *path, size_t pathlen);

// Removes chunked framing from a body in place. Returns the body's length,
// -2 when the last chunk has not arrived yet (buf is unchanged), or -1 when
// the framing is malformed (buf is unchanged).
long
upnp_http_dechunk(char *buf, size_t len);

// The player's level, 0 to 100, for a renderer's volume. Mute and raw 0 are
// 0; otherwise raw less the offset, never below 1.
int
upnp_level_map(int raw, bool mute, int offset);

enum upnp_echo {
  UPNP_ECHO_LEARN,          // the renderer's answer to a level sent; *offset set
  UPNP_ECHO_NO_LEARN,       // a mute, or a level sent too near either end to learn from
  UPNP_ECHO_SCALE_UNKNOWN,  // too far from the level sent: not pct plus a small offset
};

// Classifies an event that arrives soon after a level was sent to the
// renderer, which is taken to be the renderer's answer to it
enum upnp_echo
upnp_level_echo(int raw, bool mute, int sent, int *offset);

// Whether a renderer's report is its answer to the last level sent: it is
// the first report since that level was sent (at sent_ms, 0 if never), within
// window_ms of it. Later reports are the device's own changes, such as a turn
// of its knob straight after a level was sent.
bool
upnp_report_is_answer(uint64_t sent_ms, bool answered, uint64_t now_ms, uint64_t window_ms);

#endif /* !__UPNP_PARSE_H__ */
