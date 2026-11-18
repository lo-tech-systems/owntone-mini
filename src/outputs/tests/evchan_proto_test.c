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

// Checks the wording of the notifier socket's messages and the reading of a
// client's lines with no daemon, no socket and no framework. From the top of
// the source tree (json-c's include and library paths as the platform needs):
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/evchan_proto_test src/outputs/tests/evchan_proto_test.c src/evchan_proto.c -ljson-c
//   /tmp/evchan_proto_test
//
// Exits 0 when every case passes, 1 otherwise, printing one line per failure
// and a count (with -v, one line per passing case too). The program is also
// listed under check_PROGRAMS and TESTS in src/Makefile.am, so "make check"
// builds and runs it.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json.h>

#include "../../evchan_proto.h"

static int cases_run;
static int cases_failed;

// Print the name of every passing case as well
static bool verbose;

// The case being run, for the failure lines
static const char *cur_name;
static bool cur_failed;

#define CHECK(cond) \
  do { \
    if (!(cond)) \
      { \
	printf("FAIL %s: line %d: %s\n", cur_name, __LINE__, #cond); \
	cur_failed = true; \
      } \
  } while (0)

static void
case_begin(const char *name)
{
  cur_name = name;
  cur_failed = false;
}

static void
case_end(void)
{
  cases_run++;
  if (cur_failed)
    cases_failed++;
  else if (verbose)
    printf("PASS %s\n", cur_name);
}

// Parses a built message, taking ownership of the text. The caller releases
// the result with json_object_put().
static json_object *
parse_owned(char *text)
{
  json_object *obj = NULL;

  CHECK(text != NULL);
  if (text)
    {
      CHECK(strchr(text, '\n') == NULL);
      obj = json_tokener_parse(text);
      free(text);
    }
  CHECK(obj != NULL);

  return obj;
}

static json_object *
member(json_object *obj, const char *key)
{
  json_object *val = NULL;

  if (!obj || !json_object_object_get_ex(obj, key, &val))
    return NULL;

  return val;
}

static bool
str_is(json_object *obj, const char *key, const char *expect)
{
  json_object *val = member(obj, key);

  return val && json_object_is_type(val, json_type_string) && strcmp(json_object_get_string(val), expect) == 0;
}

static bool
int_is(json_object *obj, const char *key, int64_t expect)
{
  json_object *val = member(obj, key);

  return val && json_object_is_type(val, json_type_int) && json_object_get_int64(val) == expect;
}

static bool
array_has(json_object *obj, const char *key, const char *item)
{
  json_object *arr = member(obj, key);
  size_t i;

  if (!arr || !json_object_is_type(arr, json_type_array))
    return false;

  for (i = 0; i < json_object_array_length(arr); i++)
    if (strcmp(json_object_get_string(json_object_array_get_idx(arr, i)), item) == 0)
      return true;

  return false;
}

static size_t
array_len(json_object *obj, const char *key)
{
  json_object *arr = member(obj, key);

  return (arr && json_object_is_type(arr, json_type_array)) ? json_object_array_length(arr) : 0;
}


/* ------------------------------ Server messages --------------------------- */

#define ALL_CAPS (EVCHAN_CAP_CHANGED | EVCHAN_CAP_RECEIVER_COMMAND | EVCHAN_CAP_DEVICE_VOLUME | EVCHAN_CAP_INCLUDE_TV)

static void
hello_has_protocol_and_all_capabilities(void)
{
  json_object *obj;

  case_begin("hello_has_protocol_and_all_capabilities");
  obj = parse_owned(evchan_proto_hello_json(1, "1.6.0", "3f9a1c2e77b04d15", 1843, ALL_CAPS));
  CHECK(str_is(obj, "type", "hello"));
  CHECK(int_is(obj, "seq", 1));
  CHECK(int_is(obj, "protocol", 1));
  CHECK(str_is(obj, "version", "1.6.0"));
  CHECK(str_is(obj, "boot", "3f9a1c2e77b04d15"));
  CHECK(int_is(obj, "uptime_ms", 1843));
  CHECK(array_len(obj, "capabilities") == 4);
  CHECK(array_has(obj, "capabilities", "changed"));
  CHECK(array_has(obj, "capabilities", "receiver_command"));
  CHECK(array_has(obj, "capabilities", "device_volume"));
  CHECK(array_has(obj, "capabilities", "include_tv"));
  CHECK(int_is(obj, "ping_interval_ms", 10000));
  CHECK(int_is(obj, "idle_close_ms", 30000));
  CHECK(int_is(obj, "max_line", 4096));
  json_object_put(obj);
  case_end();
}

static void
hello_lists_only_the_capabilities_given(void)
{
  json_object *obj;

  case_begin("hello_lists_only_the_capabilities_given");
  obj = parse_owned(evchan_proto_hello_json(1, "1.6.0", "0011223344556677", 0, EVCHAN_CAP_CHANGED));
  CHECK(array_len(obj, "capabilities") == 1);
  CHECK(array_has(obj, "capabilities", "changed"));
  json_object_put(obj);

  obj = parse_owned(evchan_proto_hello_json(1, "1.6.0", "0011223344556677", 0, 0));
  CHECK(array_len(obj, "capabilities") == 0);
  CHECK(member(obj, "capabilities") != NULL);
  json_object_put(obj);
  case_end();
}

static void
changed_message(void)
{
  json_object *obj;

  case_begin("changed_message");
  obj = parse_owned(evchan_proto_changed_json(7));
  CHECK(str_is(obj, "type", "changed"));
  CHECK(int_is(obj, "seq", 7));
  CHECK(str_is(obj, "what", "outputs"));
  json_object_put(obj);
  case_end();
}

static void
event_receiver_command_with_name(void)
{
  json_object *obj;
  json_object *speaker;

  case_begin("event_receiver_command_with_name");
  obj = parse_owned(evchan_proto_event_json(3, EVCHAN_EVENT_RECEIVER_COMMAND, "paus", NULL, "9988776655443322", "9988776655443399", "Speaker One", 190511));
  CHECK(str_is(obj, "type", "event"));
  CHECK(int_is(obj, "seq", 3));
  CHECK(str_is(obj, "kind", "receiver_command"));
  CHECK(str_is(obj, "verb", "paus"));
  CHECK(str_is(obj, "protocol", "airplay2_events"));
  CHECK(member(obj, "value") == NULL);
  CHECK(int_is(obj, "at_ms", 190511));
  speaker = member(obj, "speaker");
  CHECK(speaker && str_is(speaker, "id", "9988776655443322"));
  CHECK(speaker && str_is(speaker, "member_id", "9988776655443399"));
  CHECK(speaker && str_is(speaker, "name", "Speaker One"));
  json_object_put(obj);
  case_end();
}

static void
event_without_a_name_has_null_name(void)
{
  json_object *obj;
  json_object *speaker;

  case_begin("event_without_a_name_has_null_name");
  obj = parse_owned(evchan_proto_event_json(4, EVCHAN_EVENT_RECEIVER_COMMAND, "play", NULL, "12", "12", NULL, 5));
  speaker = member(obj, "speaker");
  CHECK(speaker != NULL);
  CHECK(speaker && json_object_object_get_ex(speaker, "name", NULL));
  CHECK(speaker && member(speaker, "name") == NULL);
  CHECK(speaker && str_is(speaker, "id", "12"));
  json_object_put(obj);
  case_end();
}

static void
event_device_volume(void)
{
  json_object *obj;
  json_object *speaker;

  case_begin("event_device_volume");
  obj = parse_owned(evchan_proto_event_json(5, EVCHAN_EVENT_DEVICE_VOLUME, "volumeup", "45", "8734567012345678", "8734567012345678", "Speaker Two", 191002));
  CHECK(str_is(obj, "kind", "device_volume"));
  CHECK(str_is(obj, "verb", "volumeup"));
  CHECK(str_is(obj, "value", "45"));
  CHECK(member(obj, "protocol") == NULL);
  speaker = member(obj, "speaker");
  CHECK(speaker && str_is(speaker, "id", "8734567012345678"));
  CHECK(speaker && str_is(speaker, "member_id", "8734567012345678"));
  json_object_put(obj);
  case_end();
}

static void
event_device_volume_all_speakers_id(void)
{
  json_object *obj;
  json_object *speaker;

  case_begin("event_device_volume_all_speakers_id");
  obj = parse_owned(evchan_proto_event_json(6, EVCHAN_EVENT_DEVICE_VOLUME, "dmcp.volume", "30", "0", "0", NULL, 1));
  CHECK(str_is(obj, "verb", "dmcp.volume"));
  speaker = member(obj, "speaker");
  CHECK(speaker && str_is(speaker, "id", "0"));
  CHECK(speaker && member(speaker, "name") == NULL);
  json_object_put(obj);

  // The raw verb and value are passed on as given, characters needing escapes included
  obj = parse_owned(evchan_proto_event_json(6, EVCHAN_EVENT_DEVICE_VOLUME, "dmcp.device-volume", "-30.0\"x\\y", "7", "7", "A \"quoted\" name", 1));
  CHECK(str_is(obj, "value", "-30.0\"x\\y"));
  speaker = member(obj, "speaker");
  CHECK(speaker && str_is(speaker, "name", "A \"quoted\" name"));
  json_object_put(obj);
  case_end();
}

static void
error_message(void)
{
  json_object *obj;

  case_begin("error_message");
  obj = parse_owned(evchan_proto_error_json(1, "not_allowed", "this user may not connect"));
  CHECK(str_is(obj, "type", "error"));
  CHECK(int_is(obj, "seq", 1));
  CHECK(str_is(obj, "code", "not_allowed"));
  CHECK(str_is(obj, "text", "this user may not connect"));
  json_object_put(obj);
  case_end();
}

static void
pong_echoes_t(void)
{
  json_object *obj;

  case_begin("pong_echoes_t");
  obj = parse_owned(evchan_proto_pong_json(9, "123456"));
  CHECK(str_is(obj, "type", "pong"));
  CHECK(int_is(obj, "seq", 9));
  CHECK(int_is(obj, "t", 123456));
  json_object_put(obj);

  obj = parse_owned(evchan_proto_pong_json(9, "\"tick\""));
  CHECK(str_is(obj, "t", "tick"));
  json_object_put(obj);

  obj = parse_owned(evchan_proto_pong_json(9, "{\"a\":[1,2]}"));
  CHECK(member(obj, "t") != NULL && json_object_is_type(member(obj, "t"), json_type_object));
  json_object_put(obj);

  obj = parse_owned(evchan_proto_pong_json(9, NULL));
  CHECK(member(obj, "t") == NULL);
  json_object_put(obj);
  case_end();
}


/* ------------------------------ Client lines ------------------------------ */

static int
parse(const char *line, struct evchan_proto_msg *msg)
{
  return evchan_proto_parse_client_line(line, strlen(line), msg);
}

static void
parse_hello_good(void)
{
  struct evchan_proto_msg msg;

  case_begin("parse_hello_good");
  CHECK(parse("{\"type\":\"hello\",\"protocol\":1,\"client\":\"autostream 0.8.0\"}", &msg) == 0);
  CHECK(msg.type == EVCHAN_MSG_HELLO);
  CHECK(msg.hello_valid);
  CHECK(msg.protocol == 1);
  CHECK(strcmp(msg.client, "autostream 0.8.0") == 0);
  evchan_proto_msg_free(&msg);

  // The client name is optional
  CHECK(parse("{\"type\":\"hello\",\"protocol\":1}", &msg) == 0);
  CHECK(msg.hello_valid);
  CHECK(msg.client[0] == '\0');
  evchan_proto_msg_free(&msg);
  case_end();
}

static void
parse_hello_wrong_protocol_is_valid_but_not_one(void)
{
  struct evchan_proto_msg msg;

  case_begin("parse_hello_wrong_protocol_is_valid_but_not_one");
  CHECK(parse("{\"type\":\"hello\",\"protocol\":2,\"client\":\"x\"}", &msg) == 0);
  CHECK(msg.type == EVCHAN_MSG_HELLO);
  CHECK(msg.hello_valid);
  CHECK(msg.protocol == 2);
  evchan_proto_msg_free(&msg);
  case_end();
}

static void
parse_hello_missing_or_mistyped_protocol(void)
{
  struct evchan_proto_msg msg;

  case_begin("parse_hello_missing_or_mistyped_protocol");
  CHECK(parse("{\"type\":\"hello\",\"client\":\"x\"}", &msg) == 0);
  CHECK(msg.type == EVCHAN_MSG_HELLO);
  CHECK(!msg.hello_valid);
  evchan_proto_msg_free(&msg);

  CHECK(parse("{\"type\":\"hello\",\"protocol\":\"1\",\"client\":\"x\"}", &msg) == 0);
  CHECK(!msg.hello_valid);
  evchan_proto_msg_free(&msg);

  CHECK(parse("{\"type\":\"hello\",\"protocol\":1,\"client\":7}", &msg) == 0);
  CHECK(!msg.hello_valid);
  evchan_proto_msg_free(&msg);
  case_end();
}

static void
parse_hello_client_length_limit(void)
{
  struct evchan_proto_msg msg;
  char line[256];
  char name[EVCHAN_CLIENT_MAX + 2];

  case_begin("parse_hello_client_length_limit");
  memset(name, 'a', EVCHAN_CLIENT_MAX);
  name[EVCHAN_CLIENT_MAX] = '\0';
  snprintf(line, sizeof(line), "{\"type\":\"hello\",\"protocol\":1,\"client\":\"%s\"}", name);
  CHECK(parse(line, &msg) == 0);
  CHECK(msg.hello_valid);
  CHECK(strcmp(msg.client, name) == 0);
  evchan_proto_msg_free(&msg);

  name[EVCHAN_CLIENT_MAX] = 'a';
  name[EVCHAN_CLIENT_MAX + 1] = '\0';
  snprintf(line, sizeof(line), "{\"type\":\"hello\",\"protocol\":1,\"client\":\"%s\"}", name);
  CHECK(parse(line, &msg) == 0);
  CHECK(!msg.hello_valid);
  evchan_proto_msg_free(&msg);
  case_end();
}

static void
parse_ping(void)
{
  struct evchan_proto_msg msg;

  case_begin("parse_ping");
  CHECK(parse("{\"type\":\"ping\",\"t\":12345}", &msg) == 0);
  CHECK(msg.type == EVCHAN_MSG_PING);
  CHECK(msg.t_json && strcmp(msg.t_json, "12345") == 0);
  evchan_proto_msg_free(&msg);

  CHECK(parse("{\"type\":\"ping\",\"t\":\"abc\"}", &msg) == 0);
  CHECK(msg.t_json && strcmp(msg.t_json, "\"abc\"") == 0);
  evchan_proto_msg_free(&msg);

  CHECK(parse("{\"type\":\"ping\"}", &msg) == 0);
  CHECK(msg.type == EVCHAN_MSG_PING);
  CHECK(msg.t_json == NULL);
  evchan_proto_msg_free(&msg);
  case_end();
}

static void
parse_goodbye(void)
{
  struct evchan_proto_msg msg;

  case_begin("parse_goodbye");
  CHECK(parse("{\"type\":\"goodbye\"}", &msg) == 0);
  CHECK(msg.type == EVCHAN_MSG_GOODBYE);
  evchan_proto_msg_free(&msg);
  case_end();
}

static void
parse_unknown_type(void)
{
  struct evchan_proto_msg msg;

  case_begin("parse_unknown_type");
  CHECK(parse("{\"type\":\"subscribe\",\"what\":\"outputs\"}", &msg) == 0);
  CHECK(msg.type == EVCHAN_MSG_UNKNOWN);
  evchan_proto_msg_free(&msg);
  case_end();
}

static void
parse_rejects_what_is_not_a_message(void)
{
  struct evchan_proto_msg msg;
  char *big;

  case_begin("parse_rejects_what_is_not_a_message");
  CHECK(parse("[1,2,3]", &msg) < 0);
  CHECK(parse("\"hello\"", &msg) < 0);
  CHECK(parse("42", &msg) < 0);
  CHECK(parse("null", &msg) < 0);
  CHECK(parse("not json", &msg) < 0);
  CHECK(parse("", &msg) < 0);
  CHECK(parse("{\"type\":\"hello\"", &msg) < 0);

  // An object that has no type, or a type that is not a string
  CHECK(parse("{\"protocol\":1}", &msg) < 0);
  CHECK(parse("{\"type\":5}", &msg) < 0);
  CHECK(parse("{\"type\":null}", &msg) < 0);

  // Longer than a client may send, even though it is valid JSON
  big = malloc(EVCHAN_LINE_MAX + 64);
  CHECK(big != NULL);
  if (big)
    {
      size_t n = snprintf(big, EVCHAN_LINE_MAX + 64, "{\"type\":\"ping\",\"t\":\"");
      memset(big + n, 'x', EVCHAN_LINE_MAX);
      memcpy(big + n + EVCHAN_LINE_MAX, "\"}", 2);
      CHECK(evchan_proto_parse_client_line(big, n + EVCHAN_LINE_MAX + 2, &msg) < 0);
      free(big);
    }
  case_end();
}

static void
parse_reads_only_the_length_given(void)
{
  struct evchan_proto_msg msg;
  const char *buf = "{\"type\":\"goodbye\"}{\"type\":\"junk";

  case_begin("parse_reads_only_the_length_given");
  CHECK(evchan_proto_parse_client_line(buf, 18, &msg) == 0);
  CHECK(msg.type == EVCHAN_MSG_GOODBYE);
  evchan_proto_msg_free(&msg);
  case_end();
}


int
main(int argc, char **argv)
{
  verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

  hello_has_protocol_and_all_capabilities();
  hello_lists_only_the_capabilities_given();
  changed_message();
  event_receiver_command_with_name();
  event_without_a_name_has_null_name();
  event_device_volume();
  event_device_volume_all_speakers_id();
  error_message();
  pong_echoes_t();

  parse_hello_good();
  parse_hello_wrong_protocol_is_valid_but_not_one();
  parse_hello_missing_or_mistyped_protocol();
  parse_hello_client_length_limit();
  parse_ping();
  parse_goodbye();
  parse_unknown_type();
  parse_rejects_what_is_not_a_message();
  parse_reads_only_the_length_given();

  printf("%d cases, %d failed\n", cases_run, cases_failed);

  return cases_failed ? 1 : 0;
}
