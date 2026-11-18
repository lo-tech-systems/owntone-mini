/*
 * Copyright (C) 2021 Espen Jürgensen <espenjurgensen@gmail.com>
 *
 * Modified for owntone-mini. Modifications Copyright (C) 2026 James Pearce
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
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <math.h>
#include <sys/socket.h>

#include <pthread.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <plist/plist.h>

#include "owntone_config.h"
#include "airplay_events.h"
#include "airplay_remote_level.h"
#include "commands.h"
#include "evchan.h"
#include "misc.h"
#include "logger.h"
#include "pair_ap/pair.h"
#include "player.h"
#include "plist_wrap.h"

#define RTSP_VERSION "RTSP/1.0"

// The longest event message accepted, headers and body; more unread data than
// this means the connection is not carrying RTSP messages
#define AIRPLAY_EVENTS_MESSAGE_MAX (64 * 1024)

struct airplay_events_client
{
  char *name;
  // The output device this event channel belongs to, so a command sent by
  // the device (e.g. pause) can be reported against that output alone
  uint64_t device_id;
  // The output row this device is listed as, and its name: what a command is
  // reported under (the same as the device unless it is a hidden group member)
  uint64_t row_id;
  char *row_name;
  int fd;
  struct event *listener;
  struct pair_cipher_context *cipher_ctx;

  struct evbuffer *incoming;
  struct evbuffer *pending;

  struct airplay_events_client *next;
};

struct rtsp_message
{
  int content_length;
  char *content_type;
  char *first_line;
  int cseq;

  const uint8_t *body;
  size_t bodylen;

  const uint8_t *data;
  size_t datalen;
};


// What the events thread needs to make a connection to a receiver's event
// port. All of it is owned by the command that carries it, and freed by it.
struct listen_args
{
  char *name;
  uint64_t device_id;
  uint64_t row_id;
  char *row_name;
  char *address;
  unsigned short port;
  uint8_t *key;
  size_t key_len;
};


static pthread_t thread_id;
static struct event_base *evbase;
static struct commands_base *cmdbase;
static struct airplay_events_client *airplay_events_clients;

// Forwards
static void
incoming_cb(int fd, short what, void *arg);


/* ---------------------------- Client handling ----------------------------- */

static void
client_free(struct airplay_events_client *client)
{
  if (!client)
    return;

  if (client->listener)
    event_free(client->listener);

  // client->fd is owned by this client from the moment client_add() is
  // called (see client_add() below), whether or not client_add() ends up
  // succeeding, so it is always safe - and always necessary - to close it
  // here. A default-initialised value of -1 (rather than the calloc'd 0,
  // which is itself a valid fd) means a client that never actually got a
  // socket assigned will not close an fd it never owned.
  if (client->fd >= 0)
    close(client->fd);

  evbuffer_free(client->incoming);
  evbuffer_free(client->pending);

  free(client->name);
  free(client->row_name);
  pair_cipher_free(client->cipher_ctx);

  free(client);
}

static void
client_remove(struct airplay_events_client *client)
{
  struct airplay_events_client *iter;

  if (client == airplay_events_clients)
    airplay_events_clients = client->next;
  else
    {
      for (iter = airplay_events_clients; iter && (iter->next != client); iter = iter->next)
	; /* EMPTY */

      if (iter)
	iter->next = client->next;
    }

  client_free(client);
}

static int
client_add(const char *name, uint64_t device_id, uint64_t row_id, const char *row_name, int fd, const uint8_t *key, size_t key_len)
{
  struct airplay_events_client *client;

  CHECK_NULL(L_AIRPLAY, client = calloc(1, sizeof(struct airplay_events_client)));
  client->fd = -1; // Not yet owned; see client_free()
  client->device_id = device_id;
  client->row_id = row_id;

  CHECK_NULL(L_AIRPLAY, client->name = strdup(name));
  CHECK_NULL(L_AIRPLAY, client->row_name = strdup(row_name));
  CHECK_NULL(L_AIRPLAY, client->incoming = evbuffer_new());
  CHECK_NULL(L_AIRPLAY, client->pending = evbuffer_new());

  // From here on, client_add() owns fd on every path, success or failure:
  // either it is closed via client_free() below, or it is closed later via
  // client_remove()/client_free() once the client is live. The caller must
  // not close fd itself after calling client_add().
  client->fd = fd;

  client->listener = event_new(evbase, fd, EV_READ | EV_PERSIST, incoming_cb, client);
  if (!client->listener)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not listen for AirPlay events from '%s', invalid fd or out of memory\n", name);
      goto error;
    }

  client->cipher_ctx = pair_cipher_new(PAIR_CLIENT_HOMEKIT_NORMAL, 1, key, key_len);
  if (!client->cipher_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not listen for AirPlay events from '%s': Could not create ciphering context\n", name);
      goto error;
    }

  event_add(client->listener, NULL);

  client->next = airplay_events_clients;
  airplay_events_clients = client;

  return 0;

 error:
  client_free(client);
  return -1;
}


/* -------------------------------- Ciphering ------------------------------- */

static int
buffer_decrypt(struct evbuffer *output, struct evbuffer *input, struct pair_cipher_context *cipher_ctx)
{
  uint8_t *in;
  size_t in_len;
  ssize_t bytes_decrypted;
  uint8_t *plain;
  size_t plain_len;

  in = evbuffer_pullup(input, -1);
  in_len = evbuffer_get_length(input);

  // Note that bytes_decrypted is not necessarily equal to plain_len
  bytes_decrypted = pair_decrypt(&plain, &plain_len, in, in_len, cipher_ctx);
  if (bytes_decrypted < 0)
    return -1;

  evbuffer_add(output, plain, plain_len);
  evbuffer_drain(input, bytes_decrypted);
  free(plain);
  return 0;
}

static int
buffer_encrypt(struct evbuffer *output, uint8_t *in, size_t in_len, struct pair_cipher_context *cipher_ctx)
{
  uint8_t *out;
  size_t out_len;
  int ret;

  ret = pair_encrypt(&out, &out_len, in, in_len, cipher_ctx);
  if (ret < 0)
    return -1;

  evbuffer_add(output, out, out_len);
  free(out);
  return 0;
}


/* --------------------- Message construction/parsing ----------------------- */

static void
response_headers_add(struct evbuffer *response, int cseq, size_t content_length, const char *content_type)
{
  evbuffer_add_printf(response, "%s 200 OK\r\n", RTSP_VERSION);
  evbuffer_add_printf(response, "Server: %s/1.0\r\n", PACKAGE_NAME);
  if (content_length)
    evbuffer_add_printf(response, "Content-Length: %zu\r\n", content_length);
  if (content_type)
    evbuffer_add_printf(response, "Content-Type: %s\r\n", content_type);
  if (cseq)
    evbuffer_add_printf(response, "CSeq: %d\r\n", cseq);
  evbuffer_add_printf(response, "\r\n");
}

static void
response_create_from_raw(struct evbuffer *response, uint8_t *body, size_t body_len, int cseq, const char *content_type)
{
  response_headers_add(response, cseq, body_len, content_type);

  if (body)
    evbuffer_add(response, body, body_len);
}

// The value of an RTSP header in the header block hdr (hdr_len bytes, ending
// with the blank line), as an integer, or -1 if the header is not there
static long
header_int_get(const uint8_t *hdr, size_t hdr_len, const char *name)
{
  size_t name_len = strlen(name);
  size_t i;

  for (i = 0; i + name_len + 1 < hdr_len; i++)
    {
      if ((i == 0 || hdr[i - 1] == '\n') && strncasecmp((const char *)hdr + i, name, name_len) == 0 && hdr[i + name_len] == ':')
	return strtol((const char *)hdr + i + name_len + 1, NULL, 10);
    }

  return -1;
}

// Finds the first whole RTSP message in in: its length including any body
// (from Content-Length) and its CSeq (0 if it has none). A message can arrive
// over several encrypted frames, each carrying at most 1024 bytes, so a long
// command (as iOS sends through a HomePod) is split. Returns 1 while the
// message is still incomplete, -1 if it cannot be an RTSP message.
static int
message_find(size_t *msg_len, int *cseq, const uint8_t *in, size_t in_len)
{
  size_t hdr_len;
  long content_length;

  for (hdr_len = 0; hdr_len + 4 <= in_len; hdr_len++)
    {
      if (memcmp(in + hdr_len, "\r\n\r\n", 4) == 0)
	break;
    }

  if (hdr_len + 4 > in_len)
    return 1;

  hdr_len += 4;

  content_length = header_int_get(in, hdr_len, "Content-Length");
  if (content_length < 0)
    content_length = 0;
  else if (content_length > AIRPLAY_EVENTS_MESSAGE_MAX)
    return -1;

  *cseq = (int)header_int_get(in, hdr_len, "CSeq");
  if (*cseq < 0)
    *cseq = 0;

  if (in_len < hdr_len + (size_t)content_length)
    return 1;

  *msg_len = hdr_len + (size_t)content_length;
  return 0;
}

static int
body_find(uint8_t **body, size_t *body_len, uint8_t *in, size_t in_len)
{
  const char *plist_header = "bplist";
  size_t plist_header_len = strlen(plist_header);

  *body_len = in_len;
  for (*body = in; *body_len > plist_header_len; (*body)++, (*body_len)--)
    {
      if (memcmp(*body, plist_header, plist_header_len) == 0)
	return 0;
    }

  return -1;
}

// The four letter verb for a sendMediaRemoteCommand that carries its command
// as a number in modernMediaRemoteCommand instead of a string in value, or
// NULL if there is no such number. The numbers have the meanings receivers
// are reported to use; one with no verb here comes back as "cmd<number>", so
// it is reported as unhandled. The caller owns the result.
static char *
modern_command_verb(plist_t request)
{
  static const char *verbs[] = { "play", "paus", "plps", "stop", "nitm", "pitm" };
  plist_t item;
  uint64_t command;
  char buf[32];

  item = plist_dict_get_item(request, "modernMediaRemoteCommand");
  if (!item || plist_get_node_type(item) != PLIST_UINT)
    return NULL;

  plist_get_uint_val(item, &command);
  if (command < ARRAY_SIZE(verbs))
    return strdup(verbs[command]);

  snprintf(buf, sizeof(buf), "cmd%" PRIu64, command);
  return strdup(buf);
}

// *level is the "volume" a command carries (a "dvlc" reporting the
// receiver's own level), and *has_level says whether it carries one that is
// a number. A flag and not a NaN, which the release builds cannot test for
static int
rtsp_parse(char **verb, double *level, bool *has_level, uint8_t *in, size_t in_len)
{
  uint8_t *body;
  size_t body_len;
  plist_t request = NULL;
  plist_t item;
  char *type = NULL;
  char *value = NULL;
  bool known;
  int ret;

  *level = 0.0;
  *has_level = false;

  DHEXDUMP(E_DBG, L_AIRPLAY, in, in_len, "Incoming event\n");

  ret = body_find(&body, &body_len, in, in_len);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Could not parse incoming event, no plist body found\n");
      return -1;
    }

  plist_from_bin((char *)body, (uint32_t)body_len, &request);
  if (!request)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Could not parse incoming event plist\n");
      return -1;
    }

  item = plist_dict_get_item(request, "type");
  if (item)
    {
      plist_get_string_val(item, &type);
    }

  item = plist_dict_get_item(request, "value");
  if (item)
    {
      plist_get_string_val(item, &value);
    }

  // A receiver that has been told we take its commands may ask for a separate
  // remote control session, as an iPhone opens one. Its commands reach us on
  // this connection without it, so none is opened; the request is answered.
  // The misspelling is the receiver's.
  if (type && strcmp(type, "remoteConrolSessionCreate") == 0)
    {
      DPRINTF(E_INFO, L_AIRPLAY, "Receiver asked for a remote control session, which is not opened\n");
      goto error;
    }

  // Some receivers send the command as a number instead of a four letter value
  if (!value)
    value = modern_command_verb(request);

  if (!type || !value)
    {
      // Logged in full and at a level that stays visible: an event in a form
      // we cannot read is otherwise lost without a trace
      char *xml = wplist_to_xml(request);

      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay event in a form we cannot read (type=%s), full event follows:\n%s\n",
              type ? type : "(none)", xml ? xml : "(could not render event plist)");
      free(xml);
      goto error;
    }
  else if (strcmp(type, "sendMediaRemoteCommand") != 0)
    {
      DPRINTF(E_DBG, L_AIRPLAY, "Incoming event not of type sendMediaRemoteCommand\n");
      goto error;
    }

  DPRINTF(E_INFO, L_AIRPLAY, "Received event type '%s', value '%s'\n", type, value);

  item = plist_dict_get_item(request, "volume");
  if (item && plist_get_node_type(item) == PLIST_REAL)
    {
      plist_get_real_val(item, level);
      *has_level = airplay_remote_level_is_number(*level);
    }

  known = strcmp(value, "paus") == 0 || strcmp(value, "play") == 0 || strcmp(value, "plps") == 0 ||
          strcmp(value, "nitm") == 0 || strcmp(value, "pitm") == 0 ||
          (strcmp(value, "dvlc") == 0 && *has_level);
  if (!known)
    {
      // Receivers send command values we have no handling for, and the value
      // on its own rarely says what was meant by it. Dump the whole event: it
      // is the only way to work out what a device is asking for, and these are
      // rare enough that the log volume is not a concern. Logged at a level
      // that stays visible without turning diagnostics up.
      char *xml = wplist_to_xml(request);

      DPRINTF(E_LOG, L_AIRPLAY, "Unhandled media remote command '%s', full event follows:\n%s\n",
              value, xml ? xml : "(could not render event plist)");
      free(xml);
    }

  // The caller owns the verb
  *verb = value;

  free(type);
  plist_free(request);
  return 0;

 error:
  free(type);
  free(value);
  plist_free(request);
  return -1;
}


/* --------------------------- Message handling ----------------------------- */

// Thread: airplay events. A receiver reporting a turn of its own volume
// control. The level is recorded like one a speaker reports over DACP, but is
// not sent back (the receiver is already playing at it), and the player then
// reports it to the local supervisor as a device volume. Queued for the
// player without waiting, as the player may be waiting for this thread to
// stop.
static void
handle_volume(struct airplay_events_client *client, double level)
{
  char volstr[32];

  snprintf(volstr, sizeof(volstr), "%.6f", airplay_remote_level_db(level));

  DPRINTF(E_INFO, L_AIRPLAY, "'%s' reports its own volume, %.4f of its range (%s dB)\n", client->name, level, volstr);

  if (player_volume_reported_speaker(client->device_id, volstr, "dvlc") < 0)
    DPRINTF(E_LOG, L_AIRPLAY, "Could not queue the volume '%s' reported\n", client->name);
}

// Thread: airplay events. A receiver's command is reported to the local
// supervisor and never acted on here; the supervisor decides what a pause or
// play from a device means for the outputs. A volume the receiver reports is
// handled by handle_volume().
static void
handle_event(struct airplay_events_client *client, const char *verb, double level, bool has_level)
{
  if (strcmp(verb, "dvlc") == 0 && has_level)
    {
      handle_volume(client, level);
      return;
    }

  if (evchan_event_receiver_command(client->row_id, client->device_id, client->row_name, verb, "airplay2_events") < 0)
    DPRINTF(E_DBG, L_AIRPLAY, "Receiver command '%s' from '%s' not reported, no event channel\n", verb, client->name);
}

static int
respond(struct airplay_events_client *client, int cseq)
{
  struct evbuffer *response;
  struct evbuffer *encrypted;
  uint8_t *plain;
  size_t plain_len;
  int ret;

  CHECK_NULL(L_AIRPLAY, response = evbuffer_new());
  response_create_from_raw(response, NULL, 0, cseq, NULL);

  plain = evbuffer_pullup(response, -1);
  plain_len = evbuffer_get_length(response);

  CHECK_NULL(L_AIRPLAY, encrypted = evbuffer_new());

  ret = buffer_encrypt(encrypted, plain, plain_len, client->cipher_ctx);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Could not encrypt AirPlay event data response: %s\n", pair_cipher_errmsg(client->cipher_ctx));
      return -1;
    }

  do
    {
      ret = evbuffer_write(encrypted, client->fd);
      if (ret <= 0)
        goto error;
    } while (evbuffer_get_length(encrypted) > 0);

  evbuffer_free(encrypted);
  evbuffer_free(response);
  return 0;

 error:
  evbuffer_free(encrypted);
  evbuffer_free(response);
  return -1;
}

static void
incoming_cb(int fd, short what, void *arg)
{
  struct airplay_events_client *client = arg;
  char *verb;
  double level;
  bool has_level;
  uint8_t *plain;
  size_t plain_len;
  size_t msg_len;
  int cseq;
  int ret;

  DPRINTF(E_DBG, L_AIRPLAY, "AirPlay event from '%s'\n", client->name);

  ret = evbuffer_read(client->incoming, fd, -1);
  if (ret == 0)
    {
      DPRINTF(E_DBG, L_AIRPLAY, "'%s' disconnected from the event channel\n", client->name);
      goto disconnect;
    }
  else if (ret < 0)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "AirPlay event connection to '%s' returned an error\n", client->name);
      goto disconnect;
    }

  ret = buffer_decrypt(client->pending, client->incoming, client->cipher_ctx);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Could not decrypt incoming AirPlay event data: %s\n", pair_cipher_errmsg(client->cipher_ctx));
      goto disconnect;
    }

  // Each whole message is handled in turn; an incomplete one stays pending
  // until the rest of it arrives
  while ((plain_len = evbuffer_get_length(client->pending)) > 0)
    {
      plain = evbuffer_pullup(client->pending, -1);

      ret = message_find(&msg_len, &cseq, plain, plain_len);
      if (ret == 1 && plain_len <= AIRPLAY_EVENTS_MESSAGE_MAX)
	{
	  DPRINTF(E_SPAM, L_AIRPLAY, "Incomplete RTSP event message, waiting for more data\n");
	  return;
	}
      else if (ret != 0)
	{
	  DPRINTF(E_WARN, L_AIRPLAY, "Data from '%s' on the event channel is not an RTSP message\n", client->name);
	  goto disconnect;
	}

      verb = NULL;
      ret = rtsp_parse(&verb, &level, &has_level, plain, msg_len);

      evbuffer_drain(client->pending, msg_len);

      // A message we cannot use is still answered: the receiver waits for the
      // answer to every request it sends, and one left unanswered (for example
      // a remoteConrolSessionCreate) can stop it answering us
      if (ret == 0)
	{
	  handle_event(client, verb, level, has_level);
	  free(verb);
	}

      ret = respond(client, cseq);
      if (ret < 0)
	{
	  DPRINTF(E_WARN, L_AIRPLAY, "Could not send AirPlay event response\n");
	  goto disconnect;
	}
    }

  return;

 disconnect:
  client_remove(client);
  return;
}

/* -------------------- Event loop (thread: airplay events ) ---------------- */

static void *
airplay_events(void *arg)
{

  thread_setname("airplay events");

  event_base_dispatch(evbase);

  pthread_exit(NULL);
}


/* ------------------------------- Interface -------------------------------- */

/* Thread: airplay events */
static enum command_state
listen_exec(void *arg, int *retval)
{
  struct listen_args *args = arg;
  int fd;
  int ret;

  *retval = -1;

  fd = net_connect(args->address, args->port, SOCK_STREAM, "AirPlay events");
  if (fd < 0)
    {
      DPRINTF(E_WARN, L_AIRPLAY, "Could not connect to '%s' events port %u, proceeding anyway\n", args->name, args->port);
      goto out;
    }

  // client_add() takes ownership of fd on both success and failure, closing
  // it itself if it fails, so it must not also be closed here.
  ret = client_add(args->name, args->device_id, args->row_id, args->row_name, fd, args->key, args->key_len);
  if (ret < 0)
    goto out;

  *retval = 0;

 out:
  free(args->name);
  free(args->row_name);
  free(args->address);
  free(args->key);

  // The command facility frees args itself
  return COMMAND_END;
}

// The connection is made, and the client added, on the events thread, so the
// caller (the player thread) never waits for the receiver. Everything the
// events thread needs is copied here, nothing points back at the caller's
// session. Returns 0 if the connection was handed over, the outcome of the
// connection itself is logged by the events thread.
int
airplay_events_listen(const char *name, uint64_t device_id, uint64_t row_id, const char *row_name, const char *address, unsigned short port, const uint8_t *key, size_t key_len)
{
  struct listen_args *args;
  int ret;

  if (!cmdbase)
    return -1;

  CHECK_NULL(L_AIRPLAY, args = calloc(1, sizeof(struct listen_args)));
  CHECK_NULL(L_AIRPLAY, args->name = strdup(name));
  CHECK_NULL(L_AIRPLAY, args->row_name = strdup(row_name));
  CHECK_NULL(L_AIRPLAY, args->address = strdup(address));
  CHECK_NULL(L_AIRPLAY, args->key = malloc(key_len));
  memcpy(args->key, key, key_len);
  args->key_len = key_len;
  args->device_id = device_id;
  args->row_id = row_id;
  args->port = port;

  ret = commands_exec_async(cmdbase, listen_exec, args);
  if (ret < 0)
    {
      free(args->name);
      free(args->row_name);
      free(args->address);
      free(args->key);
      free(args);
      return -1;
    }

  return 0;
}

/* Thread: main */
int
airplay_events_init(void)
{
  int ret;

  CHECK_NULL(L_AIRPLAY, evbase = event_base_new());
  CHECK_NULL(L_AIRPLAY, cmdbase = commands_base_new(evbase, NULL));

  ret = pthread_create(&thread_id, NULL, airplay_events, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not spawn AirPlay events thread: %s\n", strerror(errno));

      goto error;
    }

  return 0;

 error:
  airplay_events_deinit();
  return -1;
}

/* Thread: main */
void
airplay_events_deinit(void)
{
  int ret;

  commands_base_destroy(cmdbase);
  cmdbase = NULL;

  ret = pthread_join(thread_id, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not join AirPlay events thread: %s\n", strerror(errno));
      return;
    }

  while (airplay_events_clients)
    {
      client_remove(airplay_events_clients);
    }

  event_base_free(evbase);
}
