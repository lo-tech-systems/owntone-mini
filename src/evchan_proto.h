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

#ifndef __EVCHAN_PROTO_H__
#define __EVCHAN_PROTO_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// The lines of the notifier socket, as JSON text. Every message is one JSON
// object on one line; this file builds the server's messages and reads the
// client's, with no socket, event loop or logger, so that the wording can be
// checked by a plain program. The builders return a malloc'd string with no
// trailing newline (NULL on failure); the caller frees it.

#define EVCHAN_PROTOCOL_VERSION 1

// Longest line a client may send, newline excluded
#define EVCHAN_LINE_MAX 4096

// Longest client name in a hello, in bytes
#define EVCHAN_CLIENT_MAX 64

// What the server can say it does, for the hello's capabilities array
enum evchan_proto_cap
{
  EVCHAN_CAP_CHANGED          = (1 << 0),
  EVCHAN_CAP_RECEIVER_COMMAND = (1 << 1),
  EVCHAN_CAP_DEVICE_VOLUME    = (1 << 2),
  EVCHAN_CAP_INCLUDE_TV       = (1 << 3),
  EVCHAN_CAP_START_VOLUME     = (1 << 4),
  EVCHAN_CAP_MUTE             = (1 << 5),
};

enum evchan_proto_event_kind
{
  EVCHAN_EVENT_RECEIVER_COMMAND,
  EVCHAN_EVENT_DEVICE_VOLUME,
};

enum evchan_proto_msg_type
{
  EVCHAN_MSG_HELLO,
  EVCHAN_MSG_PING,
  EVCHAN_MSG_GOODBYE,
  EVCHAN_MSG_UNKNOWN,   // an object with a string type that is not one of the above
};

struct evchan_proto_msg
{
  enum evchan_proto_msg_type type;

  // Hello: true when protocol is an integer and client, if present, is a
  // string of at most EVCHAN_CLIENT_MAX bytes. The protocol number itself is
  // not judged here.
  bool hello_valid;
  int protocol;
  char client[EVCHAN_CLIENT_MAX + 1];

  // Ping: the JSON text of the optional t member, to be echoed; NULL if absent
  char *t_json;
};

char *
evchan_proto_hello_json(uint64_t seq, const char *version, const char *boot, uint64_t uptime_ms, unsigned caps);

char *
evchan_proto_changed_json(uint64_t seq);

// For a receiver command, value is ignored and the event carries the protocol
// the command arrived on; for a device volume, value is the level as given.
// speaker_id is the output row the event is reported under and member_id the
// device it came from (the same when the device has its own row).
// speaker_name may be NULL (sent as null).
char *
evchan_proto_event_json(uint64_t seq, enum evchan_proto_event_kind kind, const char *verb, const char *value,
                        const char *speaker_id, const char *member_id, const char *speaker_name, uint64_t at_ms);

char *
evchan_proto_pong_json(uint64_t seq, const char *t_json);

char *
evchan_proto_error_json(uint64_t seq, const char *code, const char *text);

// Reads one client line (newline already removed). Returns 0 and fills msg for
// any JSON object with a string type; -1 for anything else, including a line
// longer than EVCHAN_LINE_MAX. On 0 the caller releases msg with
// evchan_proto_msg_free().
int
evchan_proto_parse_client_line(const char *line, size_t len, struct evchan_proto_msg *msg);

void
evchan_proto_msg_free(struct evchan_proto_msg *msg);

#endif /* !__EVCHAN_PROTO_H__ */
