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
#include <stdlib.h>
#include <string.h>
#include <json.h>

#include "evchan_proto.h"

static const struct { unsigned bit; const char *name; } cap_names[] =
{
  { EVCHAN_CAP_CHANGED,          "changed" },
  { EVCHAN_CAP_RECEIVER_COMMAND, "receiver_command" },
  { EVCHAN_CAP_DEVICE_VOLUME,    "device_volume" },
  { EVCHAN_CAP_INCLUDE_TV,       "include_tv" },
  { EVCHAN_CAP_START_VOLUME,     "start_volume" },
  { EVCHAN_CAP_MUTE,             "mute" },
};

// Takes the object, returns its text and releases it
static char *
object_finish(json_object *obj)
{
  const char *str;
  char *out;

  str = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
  out = str ? strdup(str) : NULL;
  json_object_put(obj);

  return out;
}

static json_object *
message_new(const char *type, uint64_t seq)
{
  json_object *obj = json_object_new_object();

  if (!obj)
    return NULL;

  json_object_object_add(obj, "type", json_object_new_string(type));
  json_object_object_add(obj, "seq", json_object_new_int64((int64_t)seq));

  return obj;
}

char *
evchan_proto_hello_json(uint64_t seq, const char *version, const char *boot, uint64_t uptime_ms, unsigned caps)
{
  json_object *obj;
  json_object *jcaps;
  size_t i;

  obj = message_new("hello", seq);
  jcaps = json_object_new_array();
  if (!obj || !jcaps)
    {
      json_object_put(obj);
      json_object_put(jcaps);
      return NULL;
    }

  for (i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++)
    if (caps & cap_names[i].bit)
      json_object_array_add(jcaps, json_object_new_string(cap_names[i].name));

  json_object_object_add(obj, "protocol", json_object_new_int(EVCHAN_PROTOCOL_VERSION));
  json_object_object_add(obj, "version", json_object_new_string(version));
  json_object_object_add(obj, "boot", json_object_new_string(boot));
  json_object_object_add(obj, "uptime_ms", json_object_new_int64((int64_t)uptime_ms));
  json_object_object_add(obj, "capabilities", jcaps);
  json_object_object_add(obj, "ping_interval_ms", json_object_new_int(10000));
  json_object_object_add(obj, "idle_close_ms", json_object_new_int(30000));
  json_object_object_add(obj, "max_line", json_object_new_int(EVCHAN_LINE_MAX));

  return object_finish(obj);
}

char *
evchan_proto_changed_json(uint64_t seq)
{
  json_object *obj = message_new("changed", seq);

  if (!obj)
    return NULL;

  json_object_object_add(obj, "what", json_object_new_string("outputs"));

  return object_finish(obj);
}

char *
evchan_proto_event_json(uint64_t seq, enum evchan_proto_event_kind kind, const char *verb, const char *value, const char *protocol,
                        const char *speaker_id, const char *member_id, const char *speaker_name, uint64_t at_ms)
{
  json_object *obj;
  json_object *jspeaker;

  obj = message_new("event", seq);
  jspeaker = json_object_new_object();
  if (!obj || !jspeaker)
    {
      json_object_put(obj);
      json_object_put(jspeaker);
      return NULL;
    }

  json_object_object_add(jspeaker, "id", json_object_new_string(speaker_id));
  json_object_object_add(jspeaker, "member_id", json_object_new_string(member_id));
  json_object_object_add(jspeaker, "name", speaker_name ? json_object_new_string(speaker_name) : NULL);

  json_object_object_add(obj, "kind", json_object_new_string(kind == EVCHAN_EVENT_RECEIVER_COMMAND ? "receiver_command" : "device_volume"));
  json_object_object_add(obj, "verb", json_object_new_string(verb));
  if (kind == EVCHAN_EVENT_RECEIVER_COMMAND)
    json_object_object_add(obj, "protocol", json_object_new_string(protocol ? protocol : "unknown"));
  else
    json_object_object_add(obj, "value", json_object_new_string(value ? value : ""));
  json_object_object_add(obj, "speaker", jspeaker);
  json_object_object_add(obj, "at_ms", json_object_new_int64((int64_t)at_ms));

  return object_finish(obj);
}

char *
evchan_proto_pong_json(uint64_t seq, const char *t_json)
{
  json_object *obj = message_new("pong", seq);

  if (!obj)
    return NULL;

  // t_json came from this module's own parse, so it is valid JSON; a value
  // that does not parse back is left out rather than sent mangled
  if (t_json)
    {
      json_object *t = json_tokener_parse(t_json);
      if (t)
	json_object_object_add(obj, "t", t);
    }

  return object_finish(obj);
}

char *
evchan_proto_error_json(uint64_t seq, const char *code, const char *text)
{
  json_object *obj = message_new("error", seq);

  if (!obj)
    return NULL;

  json_object_object_add(obj, "code", json_object_new_string(code));
  json_object_object_add(obj, "text", json_object_new_string(text ? text : ""));

  return object_finish(obj);
}

int
evchan_proto_parse_client_line(const char *line, size_t len, struct evchan_proto_msg *msg)
{
  json_object *obj;
  json_object *val;
  const char *type;
  char *copy;
  int ret = -1;

  memset(msg, 0, sizeof(*msg));

  if (len > EVCHAN_LINE_MAX)
    return -1;

  // json_tokener_parse() needs a terminated string, and the line may be a view
  // into a larger buffer
  copy = strndup(line, len);
  if (!copy)
    return -1;

  obj = json_tokener_parse(copy);
  free(copy);
  if (!obj)
    return -1;

  if (!json_object_is_type(obj, json_type_object)
      || !json_object_object_get_ex(obj, "type", &val)
      || !json_object_is_type(val, json_type_string))
    goto out;

  type = json_object_get_string(val);

  if (strcmp(type, "hello") == 0)
    {
      msg->type = EVCHAN_MSG_HELLO;
      msg->hello_valid = true;

      if (json_object_object_get_ex(obj, "protocol", &val) && json_object_is_type(val, json_type_int))
	msg->protocol = json_object_get_int(val);
      else
	msg->hello_valid = false;

      if (json_object_object_get_ex(obj, "client", &val))
	{
	  if (json_object_is_type(val, json_type_string) && json_object_get_string_len(val) <= EVCHAN_CLIENT_MAX)
	    snprintf(msg->client, sizeof(msg->client), "%s", json_object_get_string(val));
	  else
	    msg->hello_valid = false;
	}
    }
  else if (strcmp(type, "ping") == 0)
    {
      msg->type = EVCHAN_MSG_PING;
      if (json_object_object_get_ex(obj, "t", &val))
	msg->t_json = strdup(json_object_to_json_string_ext(val, JSON_C_TO_STRING_PLAIN));
    }
  else if (strcmp(type, "goodbye") == 0)
    msg->type = EVCHAN_MSG_GOODBYE;
  else
    msg->type = EVCHAN_MSG_UNKNOWN;

  ret = 0;

 out:
  json_object_put(obj);
  return ret;
}

void
evchan_proto_msg_free(struct evchan_proto_msg *msg)
{
  free(msg->t_json);
  msg->t_json = NULL;
}
