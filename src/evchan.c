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
#include <pwd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#ifdef HAVE_EVENTFD
# include <sys/eventfd.h>
#endif

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "evchan.h"
#include "commands.h"
#include "listener.h"
#include "logger.h"
#include "misc.h"
#include "owntone_config.h"

// The channel is a thread with an event loop of its own. Everything below that
// touches a connection, a timer or the listening socket runs on that thread;
// the other threads only set a flag, write to a descriptor or post a command.

#define EVCHAN_DEFAULT_PATH     "/run/owntone-mini/events.sock"
#define EVCHAN_MAX_CONNS        4
#define EVCHAN_SEND_CAP         (256 * 1024)
#define EVCHAN_HELLO_TIMEOUT_MS 5000
#define EVCHAN_IDLE_CLOSE_MS    30000
#define EVCHAN_SWEEP_MS         5000
#define EVCHAN_CLOSING_MAX_MS   1000
#define EVCHAN_SETTLE_MS        20
#define EVCHAN_ALLOWED_MAX      8
#define EVCHAN_PWBUF_SIZE       4096

// What this build can report. Grows as the kinds of message are implemented.
#define EVCHAN_CAPS             (EVCHAN_CAP_CHANGED | EVCHAN_CAP_RECEIVER_COMMAND | EVCHAN_CAP_DEVICE_VOLUME)

struct evchan_conn
{
  struct bufferevent *bev;       // owns the descriptor
  uid_t uid;
  pid_t pid;
  char client[EVCHAN_CLIENT_MAX + 1];
  bool hello_seen;
  bool closing;                  // error written, freed once it has drained
  const char *close_reason;
  uint64_t seq;                  // last sequence number sent
  uint64_t sent;
  uint64_t connected_at_ms;
  uint64_t last_rx_ms;
  uint64_t closing_at_ms;
  struct evchan_conn *next;
};

struct event_args
{
  enum evchan_proto_event_kind kind;
  uint64_t speaker_id;
  uint64_t member_id;
  char *name;
  char *verb;
  char *value;
  uint64_t at_ms;
};

// Written by the player thread (dirty, via listener_cb) and read by the
// notifier thread
static atomic_bool enabled;
static atomic_uint dirty;

// Set before the other threads exist and not changed until they are joined
static struct commands_base *cmdbase;
static struct event_base *evbase;
static pthread_t tid_evchan;
static int poke_fd = -1;         // written by the player thread
static int poke_rfd = -1;        // read by the notifier thread (the same descriptor with eventfd)
static char socket_path[108];
static char boot_id[17];
static uint64_t started_ms;

// Notifier thread only
static struct event *poke_ev;
static struct event *settle_ev;
static struct event *sweep_ev;
static struct event *listen_ev;
static int listen_fd = -1;
static bool socket_bound;
static struct evchan_conn *conns;
static int nconns;


/* ------------------------------- Helpers ---------------------------------- */

static uint64_t
monotonic_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// A new random id per daemon start, so a client can tell a restart from a
// dropped connection
static void
boot_id_make(void)
{
  uint8_t raw[8];
  ssize_t n = -1;
  int fd;
  int i;

  fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0)
    {
      n = read(fd, raw, sizeof(raw));
      close(fd);
    }

  // No random source: fall back to something that still differs per start
  if (n != (ssize_t)sizeof(raw))
    {
      uint64_t seed = monotonic_ms() ^ ((uint64_t)getpid() << 32) ^ (uint64_t)time(NULL);
      memcpy(raw, &seed, sizeof(raw));
    }

  for (i = 0; i < (int)sizeof(raw); i++)
    snprintf(boot_id + 2 * i, 3, "%02x", raw[i]);
}

// Best effort: the error is for a peer that is about to be closed and whose
// buffer is empty, so the line goes straight to the socket
static void
fd_send_error(int fd, uint64_t seq, const char *code, const char *text)
{
  char *json;
  char *line;
  int len;

  json = evchan_proto_error_json(seq, code, text);
  if (!json)
    return;

  len = asprintf(&line, "%s\n", json);
  free(json);
  if (len < 0)
    return;

  (void)send(fd, line, len, MSG_DONTWAIT | MSG_NOSIGNAL);
  free(line);
}


/* ------------------------------ Connections ------------------------------- */

static void
conn_free(struct evchan_conn *c, const char *reason)
{
  struct evchan_conn **pp;

  for (pp = &conns; *pp; pp = &(*pp)->next)
    if (*pp == c)
      {
	*pp = c->next;
	break;
      }
  nconns--;

  DPRINTF(E_LOG, L_EVCHAN, "Client closed (uid %u, %s, %" PRIu64 " messages sent)\n", (unsigned)c->uid, reason, c->sent);

  bufferevent_free(c->bev);
  free(c);
}

// Takes json. Returns 0 on success, -1 if the connection had to be closed for
// overflow, in which case c is gone.
static int
conn_send(struct evchan_conn *c, char *json)
{
  size_t len;

  if (!json)
    return 0;

  len = strlen(json) + 1;

  // A client that does not read must not make the daemon hold on to memory
  if (evbuffer_get_length(bufferevent_get_output(c->bev)) + len > EVCHAN_SEND_CAP)
    {
      free(json);
      conn_free(c, "overflow");
      return -1;
    }

  bufferevent_write(c->bev, json, len - 1);
  bufferevent_write(c->bev, "\n", 1);
  free(json);

  c->seq++;
  c->sent++;
  DPRINTF(E_DBG, L_EVCHAN, "Message %" PRIu64 " sent\n", c->seq);

  return 0;
}

// Writes one error and closes the connection once it has been sent
static int
conn_close_with_error(struct evchan_conn *c, const char *code, const char *text, const char *reason)
{
  if (conn_send(c, evchan_proto_error_json(c->seq + 1, code, text)) < 0)
    return -1;

  bufferevent_disable(c->bev, EV_READ);
  c->closing = true;
  c->close_reason = reason;
  c->closing_at_ms = monotonic_ms();

  return 0;
}

static int
handle_hello(struct evchan_conn *c, struct evchan_proto_msg *msg)
{
  uint64_t now = monotonic_ms();

  if (c->hello_seen)
    return conn_close_with_error(c, "bad_message", "hello already received", "bad_message");

  if (!msg->hello_valid)
    return conn_close_with_error(c, "bad_hello", "hello needs an integer protocol and a short client name", "bad_hello");

  if (msg->protocol != EVCHAN_PROTOCOL_VERSION)
    return conn_close_with_error(c, "protocol_unsupported", "only protocol 1 is supported", "protocol_unsupported");

  c->hello_seen = true;
  snprintf(c->client, sizeof(c->client), "%s", msg->client);

  DPRINTF(E_LOG, L_EVCHAN, "Client connected (uid %u pid %d) '%s'\n", (unsigned)c->uid, (int)c->pid, c->client);

  return conn_send(c, evchan_proto_hello_json(c->seq + 1, PACKAGE_VERSION, boot_id, now - started_ms, EVCHAN_CAPS));
}

// Returns -1 if the connection was freed
static int
conn_line(struct evchan_conn *c, const char *line, size_t len)
{
  struct evchan_proto_msg msg;
  int ret;

  if (evchan_proto_parse_client_line(line, len, &msg) < 0)
    return conn_close_with_error(c, c->hello_seen ? "bad_message" : "bad_hello", "not a message", c->hello_seen ? "bad_message" : "bad_hello");

  DPRINTF(E_DBG, L_EVCHAN, "Message received, type %d\n", (int)msg.type);

  if (msg.type == EVCHAN_MSG_HELLO)
    ret = handle_hello(c, &msg);
  else if (!c->hello_seen)
    ret = conn_close_with_error(c, "bad_hello", "the first message must be a hello", "bad_hello");
  else if (msg.type == EVCHAN_MSG_PING)
    ret = conn_send(c, evchan_proto_pong_json(c->seq + 1, msg.t_json));
  else if (msg.type == EVCHAN_MSG_GOODBYE)
    {
      conn_free(c, "goodbye");
      ret = -1;
    }
  else
    ret = 0; // unknown types are ignored, so a newer client can say more

  evchan_proto_msg_free(&msg);

  return ret;
}

static void
conn_read_cb(struct bufferevent *bev, void *arg)
{
  struct evchan_conn *c = arg;
  struct evbuffer *in = bufferevent_get_input(bev);
  char *line;
  size_t len;
  int ret;

  while (!c->closing && (line = evbuffer_readln(in, &len, EVBUFFER_EOL_LF)))
    {
      c->last_rx_ms = monotonic_ms();

      if (len > EVCHAN_LINE_MAX)
	{
	  free(line);
	  conn_close_with_error(c, "line_too_long", "a line may be at most 4096 bytes", "line_too_long");
	  return;
	}

      ret = conn_line(c, line, len);
      free(line);
      if (ret < 0)
	return;
    }

  // No newline yet and already more than a line's worth: it will never fit
  if (!c->closing && evbuffer_get_length(in) > EVCHAN_LINE_MAX)
    conn_close_with_error(c, "line_too_long", "a line may be at most 4096 bytes", "line_too_long");
}

static void
conn_write_cb(struct bufferevent *bev, void *arg)
{
  struct evchan_conn *c = arg;

  if (c->closing && evbuffer_get_length(bufferevent_get_output(bev)) == 0)
    conn_free(c, c->close_reason);
}

static void
conn_event_cb(struct bufferevent *bev, short what, void *arg)
{
  struct evchan_conn *c = arg;

  if (what & BEV_EVENT_EOF)
    conn_free(c, "eof");
  else if (what & BEV_EVENT_ERROR)
    conn_free(c, "error");
}

static struct evchan_conn *
conn_new(int fd, uid_t uid, pid_t pid)
{
  struct evchan_conn *c;

  c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;

  c->bev = bufferevent_socket_new(evbase, fd, BEV_OPT_CLOSE_ON_FREE);
  if (!c->bev)
    {
      free(c);
      return NULL;
    }

  c->uid = uid;
  c->pid = pid;
  c->connected_at_ms = c->last_rx_ms = monotonic_ms();

  bufferevent_setcb(c->bev, conn_read_cb, conn_write_cb, conn_event_cb, c);
  bufferevent_enable(c->bev, EV_READ | EV_WRITE);

  c->next = conns;
  conns = c;
  nconns++;

  return c;
}

static void
broadcast_changed(void)
{
  struct evchan_conn *c;
  struct evchan_conn *next;

  for (c = conns; c; c = next)
    {
      next = c->next;
      if (c->hello_seen && !c->closing)
	conn_send(c, evchan_proto_changed_json(c->seq + 1));
    }
}


/* ----------------------------- Notifier loop ------------------------------ */

// The configured names are looked up at every connection, so a user created
// after the daemon started is accepted from then on
static bool
peer_allowed(uid_t uid)
{
  struct passwd pwd;
  struct passwd *res;
  const char *name;
  char buf[EVCHAN_PWBUF_SIZE];
  int count;
  int i;

  if (uid == 0 || uid == geteuid())
    return true;

  count = config_get_strlist_count("event_socket_allowed_users");
  if (count > EVCHAN_ALLOWED_MAX)
    count = EVCHAN_ALLOWED_MAX;

  for (i = 0; i < count; i++)
    {
      name = config_get_strlist_item("event_socket_allowed_users", i);
      if (!name)
	continue;

      if (getpwnam_r(name, &pwd, buf, sizeof(buf), &res) == 0 && res && res->pw_uid == uid)
	return true;
    }

  return false;
}

static void
accept_cb(evutil_socket_t fd, short what, void *arg)
{
  struct ucred cred;
  socklen_t cred_len;
  struct evchan_conn *c;
  int cfd;

  for (;;)
    {
      cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (cfd < 0)
	{
	  if (errno == EINTR || errno == ECONNABORTED)
	    continue;
	  if (errno != EAGAIN && errno != EWOULDBLOCK)
	    DPRINTF_THROTTLED(60, E_LOG, L_EVCHAN, "Could not accept a connection: %s", strerror(errno));
	  return;
	}

      // Checked before anything is read from the peer
      cred_len = sizeof(cred);
      if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0)
	{
	  close(cfd);
	  continue;
	}

      if (!peer_allowed(cred.uid))
	{
	  fd_send_error(cfd, 1, "not_allowed", "this user may not connect");
	  DPRINTF_THROTTLED(60, E_LOG, L_EVCHAN, "Refused connection from uid %u: not allowed", (unsigned)cred.uid);
	  close(cfd);
	  continue;
	}

      if (nconns >= EVCHAN_MAX_CONNS)
	{
	  fd_send_error(cfd, 1, "too_many_clients", "too many clients are connected");
	  DPRINTF_THROTTLED(60, E_LOG, L_EVCHAN, "Refused connection from uid %u: too many clients", (unsigned)cred.uid);
	  close(cfd);
	  continue;
	}

      c = conn_new(cfd, cred.uid, cred.pid);
      if (!c)
	close(cfd);
    }
}

static void
sweep_cb(evutil_socket_t fd, short what, void *arg)
{
  struct timeval tv = { EVCHAN_SWEEP_MS / 1000, 0 };
  struct evchan_conn *c;
  struct evchan_conn *next;
  uint64_t now = monotonic_ms();

  for (c = conns; c; c = next)
    {
      next = c->next;

      if (c->closing)
	{
	  if (now - c->closing_at_ms > EVCHAN_CLOSING_MAX_MS)
	    conn_free(c, c->close_reason);
	}
      else if (!c->hello_seen && now - c->connected_at_ms > EVCHAN_HELLO_TIMEOUT_MS)
	conn_close_with_error(c, "hello_timeout", "no hello within 5 seconds", "hello_timeout");
      else if (now - c->last_rx_ms > EVCHAN_IDLE_CLOSE_MS)
	conn_free(c, "ping_timeout");
    }

  evtimer_add(sweep_ev, &tv);
}

// A burst of changes becomes one message: the poke arms a short timer, and the
// message goes out when it fires. Nothing is lost, the client reads the full
// list of outputs after each one.
static void
poke_cb(evutil_socket_t fd, short what, void *arg)
{
  struct timeval tv = { 0, EVCHAN_SETTLE_MS * 1000 };
  char buf[64];
  unsigned bits;

  // Empties the eventfd (or pipe), so the next write wakes the loop again
  while (read(fd, buf, sizeof(buf)) > 0)
    ;

  bits = atomic_exchange_explicit(&dirty, 0, memory_order_acquire);
  if (bits && !evtimer_pending(settle_ev, NULL))
    evtimer_add(settle_ev, &tv);
}

static void
settle_cb(evutil_socket_t fd, short what, void *arg)
{
  broadcast_changed();
}

// Thread: notifier
static enum command_state
event_exec(void *arg, int *retval)
{
  struct event_args *args = arg;
  struct evchan_conn *c;
  struct evchan_conn *next;
  char id[24];
  char member[24];
  char *json;
  bool sent = false;

  *retval = 0;

  snprintf(id, sizeof(id), "%" PRIu64, args->speaker_id);
  snprintf(member, sizeof(member), "%" PRIu64, args->member_id);

  for (c = conns; c; c = next)
    {
      next = c->next;
      if (!c->hello_seen || c->closing)
	continue;

      json = evchan_proto_event_json(c->seq + 1, args->kind, args->verb, args->value, id, member, args->name, args->at_ms);
      if (conn_send(c, json) == 0)
	sent = true;
    }

  if (!sent)
    DPRINTF(E_INFO, L_EVCHAN, "Event dropped, no client connected (%s)\n", args->verb);

  free(args->name);
  free(args->verb);
  free(args->value);

  // The command facility frees args itself
  return COMMAND_END;
}

// Thread: notifier
static enum command_state
start_exec(void *arg, int *retval)
{
  struct sockaddr_un addr;
  struct timeval tv = { EVCHAN_SWEEP_MS / 1000, 0 };
  struct passwd pwd;
  struct passwd *res;
  struct stat st;
  const char *name;
  char buf[EVCHAN_PWBUF_SIZE];
  int count;
  int i;

  *retval = -1;

  // The allow-list is resolved at every connection, but say now which names
  // do not resolve yet
  count = config_get_strlist_count("event_socket_allowed_users");
  for (i = 0; i < count; i++)
    {
      name = config_get_strlist_item("event_socket_allowed_users", i);
      if (i >= EVCHAN_ALLOWED_MAX)
	{
	  DPRINTF(E_WARN, L_EVCHAN, "event_socket_allowed_users: more than %d users, '%s' ignored\n", EVCHAN_ALLOWED_MAX, name ? name : "");
	  continue;
	}
      if (name && (getpwnam_r(name, &pwd, buf, sizeof(buf), &res) != 0 || !res))
	DPRINTF(E_WARN, L_EVCHAN, "event_socket_allowed_users: unknown user '%s' for now\n", name);
    }

  // A socket left behind by a crash would stop the bind; anything else at the
  // path is not ours to remove
  if (lstat(socket_path, &st) == 0)
    {
      if (!S_ISSOCK(st.st_mode))
	{
	  DPRINTF(E_LOG, L_EVCHAN, "Event channel disabled: %s exists and is not a socket\n", socket_path);
	  return COMMAND_END;
	}
      unlink(socket_path);
    }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

  listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd < 0)
    goto error;

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    goto error;
  socket_bound = true;

  // Connecting needs write permission on the socket; who may connect is
  // decided by the peer's credentials, not by the mode
  if (chmod(socket_path, 0666) < 0 || listen(listen_fd, EVCHAN_MAX_CONNS) < 0)
    goto error;

  listen_ev = event_new(evbase, listen_fd, EV_READ | EV_PERSIST, accept_cb, NULL);
  sweep_ev = evtimer_new(evbase, sweep_cb, NULL);
  if (!listen_ev || !sweep_ev || event_add(listen_ev, NULL) < 0 || evtimer_add(sweep_ev, &tv) < 0)
    {
      errno = ENOMEM;
      goto error;
    }

  atomic_store_explicit(&enabled, true, memory_order_release);
  *retval = 0;

  DPRINTF(E_LOG, L_EVCHAN, "Event channel listening on %s\n", socket_path);

  return COMMAND_END;

 error:
  DPRINTF(E_LOG, L_EVCHAN, "Event channel disabled: could not create socket at %s: %s\n", socket_path, strerror(errno));

  if (listen_ev)
    event_free(listen_ev);
  if (sweep_ev)
    event_free(sweep_ev);
  listen_ev = NULL;
  sweep_ev = NULL;
  if (listen_fd >= 0)
    close(listen_fd);
  listen_fd = -1;
  if (socket_bound)
    unlink(socket_path);
  socket_bound = false;

  return COMMAND_END;
}

// Thread: notifier
static enum command_state
shutdown_exec(void *arg, int *retval)
{
  *retval = 0;

  // From here the player's callback does nothing
  atomic_store_explicit(&enabled, false, memory_order_release);

  while (conns)
    {
      // The client is told why, straight to the socket, because freeing the
      // connection discards whatever is still queued on it
      if (evbuffer_get_length(bufferevent_get_output(conns->bev)) == 0)
	fd_send_error(bufferevent_getfd(conns->bev), conns->seq + 1, "shutting_down", "the daemon is stopping");
      conn_free(conns, "shutting_down");
    }

  if (listen_ev)
    event_free(listen_ev);
  listen_ev = NULL;
  if (sweep_ev)
    event_free(sweep_ev);
  sweep_ev = NULL;

  if (listen_fd >= 0)
    close(listen_fd);
  listen_fd = -1;

  if (socket_bound)
    unlink(socket_path);
  socket_bound = false;

  return COMMAND_END;
}

static void *
evchan_thread(void *arg)
{
  thread_setname("evchan");

  event_base_dispatch(evbase);

  pthread_exit(NULL);
}


/* ------------------------- Player-thread callback ------------------------- */

// Thread: player. Must not block or allocate: records that outputs changed
// and wakes the notifier thread. The write's result is ignored on purpose:
// EAGAIN means the counter is already non-zero, so the wake-up is pending.
static void
listener_cb(short event_mask, void *ctx)
{
  uint64_t one = 1;

  if (!atomic_load_explicit(&enabled, memory_order_relaxed))
    return;

  atomic_fetch_or_explicit(&dirty, (unsigned)event_mask, memory_order_release);
  (void)!write(poke_fd, &one, sizeof(one));
}


/* ------------------------------- Interface -------------------------------- */

static int
event_post(enum evchan_proto_event_kind kind, uint64_t speaker_id, uint64_t member_id, const char *name, const char *verb, const char *value)
{
  struct event_args *args;

  if (!cmdbase)
    return -1;

  CHECK_NULL(L_EVCHAN, args = calloc(1, sizeof(*args)));
  args->kind = kind;
  args->speaker_id = speaker_id;
  args->member_id = member_id;
  args->name = name ? strdup(name) : NULL;
  args->verb = strdup(verb ? verb : "");
  args->value = value ? strdup(value) : NULL;
  args->at_ms = monotonic_ms() - started_ms;

  if (commands_exec_async(cmdbase, event_exec, args) < 0)
    {
      free(args->name);
      free(args->verb);
      free(args->value);
      free(args);
      return -1;
    }

  return 0;
}

int
evchan_event_receiver_command(uint64_t speaker_id, uint64_t member_id, const char *name, const char *verb)
{
  return event_post(EVCHAN_EVENT_RECEIVER_COMMAND, speaker_id, member_id, name, verb, NULL);
}

int
evchan_event_device_volume(uint64_t speaker_id, uint64_t member_id, const char *name, const char *verb, const char *value)
{
  return event_post(EVCHAN_EVENT_DEVICE_VOLUME, speaker_id, member_id, name, verb, value);
}

bool
evchan_enabled(void)
{
  return atomic_load_explicit(&enabled, memory_order_acquire);
}

const char *
evchan_socket_path(void)
{
  return socket_path;
}

static void
resources_free(void)
{
  if (poke_ev)
    event_free(poke_ev);
  if (settle_ev)
    event_free(settle_ev);
  poke_ev = NULL;
  settle_ev = NULL;

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
evchan_init(void)
{
  const char *path;
  int ret;

  path = config_get_str("event_socket", EVCHAN_DEFAULT_PATH);
  if (!path || path[0] == '\0')
    {
      DPRINTF(E_LOG, L_EVCHAN, "Event channel disabled: no socket path is configured\n");
      return 0;
    }
  if (path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
    {
      DPRINTF(E_LOG, L_EVCHAN, "Event channel disabled: the socket path must be absolute and shorter than %zu bytes\n", sizeof(((struct sockaddr_un *)0)->sun_path));
      return 0;
    }
  snprintf(socket_path, sizeof(socket_path), "%s", path);

  boot_id_make();
  started_ms = monotonic_ms();

  // The player thread writes to this descriptor from its listener callback,
  // so a full counter must never make the write wait
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
      DPRINTF(E_LOG, L_EVCHAN, "Event channel disabled: could not create the wake-up descriptor: %s\n", strerror(errno));
      return -1;
    }

  evbase = event_base_new();
  if (!evbase)
    goto error;

  poke_ev = event_new(evbase, poke_rfd, EV_READ | EV_PERSIST, poke_cb, NULL);
  settle_ev = evtimer_new(evbase, settle_cb, NULL);
  cmdbase = commands_base_new(evbase, NULL);
  if (!poke_ev || !settle_ev || !cmdbase || event_add(poke_ev, NULL) < 0)
    goto error;

  // Before the player thread exists: the listener list is not locked
  if (listener_add(listener_cb, LISTENER_SPEAKER | LISTENER_VOLUME, NULL) < 0)
    goto error;

  ret = pthread_create(&tid_evchan, NULL, evchan_thread, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_EVCHAN, "Event channel disabled: could not start the thread: %s\n", strerror(ret));
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
evchan_start(void)
{
  if (!cmdbase)
    return;

  commands_exec_sync(cmdbase, start_exec, NULL, NULL);
}

// Thread: main
void
evchan_shutdown(void)
{
  if (!cmdbase)
    return;

  commands_exec_sync(cmdbase, shutdown_exec, NULL, NULL);
}

// Thread: main
void
evchan_deinit(void)
{
  if (!cmdbase)
    return;

  // Normally done already; this covers a start-up that failed part way
  evchan_shutdown();

  commands_base_destroy(cmdbase);
  cmdbase = NULL;

  if (pthread_join(tid_evchan, NULL) != 0)
    DPRINTF(E_LOG, L_EVCHAN, "Could not join the event channel thread: %s\n", strerror(errno));

  // Safe because the only caller of listener_notify is the player thread,
  // which has been joined
  listener_remove(listener_cb);

  resources_free();
}
