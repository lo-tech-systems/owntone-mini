/*
 * Copyright (C) 2016-2019 Espen Jürgensen <espenjurgensen@gmail.com>
 * Copyright (C) 2010-2011 Julien BLACHE <jb@jblache.org>
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


 * About player.c
 * --------------
 * The main tasks of the player are the following:
 * - handle playback commands, status checks and events from other threads
 * - receive audio from the input thread and to own the playback buffer
 * - feed the outputs at the appropriate rate (controlled by the playback timer)
 * - output device handling (outsourced to outputs.c)
 * - notify about playback status changes
 * - maintain the playback queue
 * 
 * The player thread should never be making operations that may block, since
 * that could block callers requesting status (effectively making the server
 * unresponsive) and it could also starve the outputs. In practice this rule is
 * not always obeyed, for instance some outputs do their setup in ways that
 * could block.
 *
 * Listener events
 * ---------------
 * Events will be signaled via listener_notify(). The following rules apply to
 * how this must be done in the code:
 * - always use status_update() to make sure the callbacks that the listener
 *   makes do not block the player thread, and to avoid any risk of deadlocks
 * - if the event is a result of an external command then trigger it when the
 *   command is completed, so generally in a bottom half
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#ifdef HAVE_TIMERFD
# include <sys/timerfd.h>
#else
# include <signal.h>
#endif

#include <event2/event.h>
#include <event2/buffer.h>

#include <gcrypt.h>

#include "queue.h"
#include "logger.h"
#include "owntone_config.h"
#include "misc.h"
#include "player.h"
#include "worker.h"
#include "listener.h"
#include "commands.h"
#include "evchan.h"
#include "mdns.h"
#include "memstats.h"

// Audio and metadata outputs
#include "outputs.h"
#include "outputs_group_flags.h"
#include "speakers_summary.h"
#include "upnp_parse.h"
#include "level_rules.h"

// Audio and metadata input
#include "input.h"


// The interval between each tick of the playback clock in ms. This means that
// we read 10 ms frames from the input and pass to the output, so the clock
// ticks 100 times a second. We use this value because most common sample rates
// are divisible by 100, and because it keeps delay low.
// TODO sample rates of 22050 might cause underruns, since we would be reading
// only 100 x 220 = 22000 samples each second.
#define PLAYER_TICK_INTERVAL 10

// For every tick_interval, we will read a frame from the input buffer and
// write it to the outputs. If the input is empty, we will try to catch up next
// tick. However, at some point we will owe the outputs so much data that we
// have to suspend playback and wait for the input to get its act together.
// (value is in milliseconds and should be low enough to avoid output underrun)
#define PLAYER_READ_BEHIND_MAX 1500

// Generally, an output must not block (for long) when outputs_write() is
// called. If an output does that anyway, the next tick event will be late, and
// by extension playback_cb(). We will try to catch up, but if the delay
// gets above this value, we will suspend playback and reset the output.
// (value is in milliseconds)
#define PLAYER_WRITE_BEHIND_MAX 1500

// If a speaker fails during playback we try to bring it back by reconnecting
// after this number of seconds. When this feature was added, we had an issue
// with Homepods and ATV4's dropping connections, so it is also a workaround.
#define PLAYER_SPEAKER_RESURRECT_TIME 5

// A device that asked for a PIN waits this many seconds for it, about as long
// as a receiver keeps the PIN on its screen. A wait that ends with an
// operation in flight is looked at again after the shorter time
#define PLAYER_PIN_WAIT_TIME 120
#define PLAYER_PIN_WAIT_RECHECK_TIME 10

// A TV proxy group leader (the Apple TV) that drops its session is
// reconnected after this many seconds, for at most this many consecutive
// attempts.
#define PLAYER_TV_PROXY_RECONNECT_TIME 3
#define PLAYER_TV_PROXY_RECONNECT_MAX 3

// Seconds between housekeeping ticks that expire removal-candidate devices.
#define PLAYER_DEVICE_GC_INTERVAL 30

// Default grace period (seconds) before a removal-candidate device is freed.
// Can be overridden via the "device_removal_grace_period" config key.
#define PLAYER_DEVICE_REMOVAL_GRACE_DEFAULT 180
#define PLAYER_DEVICE_REMOVAL_GRACE_MAX     3600

// A level a speaker reports itself is sent back to it once its reports have
// stopped for this long, for receivers that wait for the sender to apply the
// level. A report of the level last sent to the speaker, within the window,
// is the speaker confirming that send and is not sent back again.
#define PLAYER_VOLUME_ECHO_DELAY_MS 150
#define PLAYER_VOLUME_ECHO_WINDOW_MS 2000
// At most this many levels are sent back to one speaker in this many ms, in
// case speakers that set each other's level never settle
#define PLAYER_VOLUME_ECHO_MAX_SENDS 5
#define PLAYER_VOLUME_ECHO_MAX_PERIOD_MS 5000

// Number of empty-output GC checks before requesting mDNS rediscovery.
#define ZERO_OUTPUT_RESCAN_CHECKS 3

// Shorthand condition for outputs_start and outputs_device_start, both need to
// know if they should only probe the device, or fully start it.
#define PLAYER_ONLY_PROBE (player_state != PLAY_PLAYING)

// When HomePods are routed via an Apple TV ("TV proxy" topology), the Apple
// TV is the primary session: it owns pairing/PIN, artwork and now-playing,
// and every start path talks to it. The HomePod members are hidden followers
// that no start path may touch directly - tv_proxy_follower_skip() gates
// this. An Apple TV session that already exists when the HomePods connect is
// closed by the Apple TV a moment later, whereas one started after the
// HomePods are streaming joins cleanly, so the two are started in a specific
// order: the Apple TV is only probed first (outputs_device_start()), which
// still runs the pairing exchange so a missing PIN gates the whole group;
// once the probe succeeds, tv_proxy_followers_start() brings the HomePods up
// (or, if the probe completed before playback began, the followers are
// started from playback_start_bh once the player is playing);
// once any of them reports streaming, tv_proxy_leader_start() starts the
// Apple TV's real session. If no follower can be started at all, the Apple TV
// is started directly instead, as a fallback. A leader that then drops its
// session is reconnected automatically (see PLAYER_TV_PROXY_RECONNECT_TIME).
// Followers are stopped together with the leader, and volume commands go to
// the leader and then fan out to the followers.

//#define DEBUG_PLAYER 1

struct spk_enum
{
  spk_enum_cb cb;
  void *arg;
};

struct speaker_set_param
{
  uint64_t *device_ids;
};

struct speaker_attr_param
{
  uint64_t spk_id;

  int volume;
  const char *volstr;
  // A level the device reported that it is already playing at, so it is
  // never sent back to it (see volume_setraw_speaker())
  bool no_send_back;
  // The device asks for this level (a DACP volume button); it is applied and
  // sent
  bool asks;

  bool prevent_playback;
  bool busy;

  struct media_quality quality;
  enum media_format format;
  enum output_mode mode;
  int offset_ms;
  bool held;
  bool muted;

  int audio_fd;
  int metadata_fd;

  const char *pin;
};

struct volume_reported_param
{
  uint64_t id;
  char volstr[32];
  char verb[16];
};

struct speaker_get_param
{
  uint64_t spk_id;
  uint32_t active_remote;
  uint32_t index;
  const char *address;
  struct player_speaker_info *spk_info;
};

union player_arg
{
  struct output_device *device;
  uint32_t id;
  int intval;
};

struct player_source
{
  // Id of the file/item in the files database
  uint32_t id;

  // Item-Id of the file/item in the queue
  uint32_t item_id;

  // Length of the file/item in milliseconds, 0 for endless (unless the input
  // has given us a track length)
  uint32_t len_ms;

  // Set when opening the item based on initial track length (so is not changed
  // by later input track length metadata)
  bool is_seekable;

  // Quality of the source (sample rate etc.)
  struct media_quality quality;

  enum data_kind data_kind;
  enum media_kind media_kind;
  char *path;

  // This is the position (measured in samples) the session was at when we
  // started reading from the input source.
  uint64_t read_start;

  // This is the position (measured in samples) the session was at when we got
  // a EOF or error from the input.
  uint64_t read_end;

  // Same as the above, but added with samples equivalent to
  // outputs_buffer_duration_ms. When the session position reaches play_start it
  // means the media should actually be playing on your device.
  uint64_t play_start;
  uint64_t play_end;

  // When we receive a metadata update from the input it shouldn't be pushed to
  // clients until the speakers have reached the position that matches the
  // position the player was reading at when it got INPUT_FLAG_METADATA.
  uint64_t metadata_update;

  // The number of milliseconds into the media that we started
  uint32_t seek_ms;

  // This should at any time match the millisecond position of the media that is
  // coming out of your device. Will be 0 during initial buffering.
  uint32_t pos_ms;

  // How many samples the outputs buffer before playing (=delay)
  int output_buffer_samples;

  // Linked list, where next is the next item to play
  struct player_source *prev;
  struct player_source *next;
};

struct player_session
{
  uint8_t *buffer;
  size_t bufsize;

  // The time the playback session started
  struct timespec start_ts;

  // The time the first sample in the buffer should be played by the output,
  // without taking outputs_buffer_duration_ms into account.
  // It will be equal to:
  // pts = start_ts + ticks_elapsed * player_tick_interval
  struct timespec pts;

  // Equals current number of samples written to outputs
  uint32_t pos;

  // The player sources also have a quality property, but in some situations
  // they may get cleared. So we also save it here.
  struct media_quality quality;

  // We try to read a fixed number of bytes from the source each clock tick,
  // but if it gives us less we increase this correspondingly
  size_t read_deficit;
  size_t read_deficit_max;

  // We send metadata when we start a session, everytime we end a track and if
  // the input gives us a new metadata event. This value tracks if we have sent
  // the starting metadata.
  bool metadata_sent;

  // Pointer to the head of the two-way linked list of items from the queue that
  // we are either playing or going to play. When an item has been played it is
  // removed from the list. The playing_now and reading_now pointers will point
  // to items inside this list (or to NULL).
  struct player_source *source_list;

  // The item from the queue being played right now. It should only be NULL when
  // there is no playback.
  struct player_source *playing_now;

  // The item from the queue which the player is currently doing input_read()
  // for. So "reading" means reading from the input buffer - the source itself
  // may already be closed by the input module.
  struct player_source *reading_now;
};

static struct player_session pb_session;

struct event_base *evbase_player;

static int player_exit;
static pthread_t tid_player;
static struct commands_base *cmdbase;

// Keep track of how many outputs need to call back when flushing internally
// from the player thread (where we can't use player_playback_pause)
static int player_flush_pending;

// A playback stop, pause or flush command waits for the devices it flushed
// (see outputs_flush). pb_suspend()'s flush is made by no command, so it
// ends none
static bool player_flush_command;

// Config values and player settings category
static int speaker_autoselect;
static int clear_queue_on_stop_disabled;

// Player status
static enum play_status player_state;

// Playback timer
#ifdef HAVE_TIMERFD
static int pb_timer_fd;
#else
timer_t pb_timer;
#endif
static struct event *pb_timer_ev;

// Device housekeeping timer — expires removal-candidate devices
static struct event *device_gc_ev;
static _Atomic int device_removal_grace_secs;
static bool outputs_ever_seen;
static int zero_output_gc_checks;
static bool zero_output_rescan_attempted;

// Time between ticks, i.e. time between when playback_cb() is invoked
static struct timespec player_tick_interval;
// Timer resolution
static struct timespec player_timer_res;

// PLAYER_WRITE_BEHIND_MAX converted to clock ticks
static int pb_write_deficit_max;

// True if we are trying to recover from a major playback timer overrun (write problems)
static bool pb_write_recovery;

// When we receive track metadata from the input we have to wait until playback
// has reached the position before using it. We use this to record the update.
struct metadata_pending_register
{
  uint64_t pos;
  struct input_metadata *metadata;
} metadata_pending[16];


/* -------------------------------- Forwards -------------------------------- */

static void
pb_abort(void);

static int
pb_suspend(void);

static bool
speaker_visible(struct output_device *device);


/* ----------------------- Misc helpers and callbacks ----------------------- */

// This is just to be able to log the caller in a simple way
#define status_update(x, y) status_update_impl((x), (y), __func__)
static void
status_update_impl(enum play_status status, short listener_events, const char *caller)
{
  DPRINTF(E_DBG, L_PLAYER, "Status update - status: %d, events: %d, caller: %s\n", status, listener_events, caller);

  player_state = status;

  listener_notify(listener_events);
}

/* history_add, queue_item_next, queue_item_prev removed (no shuffle/repeat/consume) */


/* ------ All this is for dealing with metadata received from the input ----- */

static int
metadata_pending_add(struct input_metadata *metadata, uint64_t pos)
{
  int i;

  if (pos == 0)
    return -1; // Invalid position

  for (i = 0; i < ARRAY_SIZE(metadata_pending); i++)
    {
      if (metadata_pending[i].metadata == NULL)
	break;
    }

  if (i == ARRAY_SIZE(metadata_pending))
    {
      DPRINTF(E_LOG, L_PLAYER, "Error, too many pending metadata updates\n");
      return -1;
    }

  metadata_pending[i].pos = pos;
  metadata_pending[i].metadata = metadata;
  return 0;
}

static uint64_t
metadata_pending_next_pos(void)
{
  uint64_t next_pos;
  int i;

  for (i = 0, next_pos = 0; i < ARRAY_SIZE(metadata_pending); i++)
    {
      if (metadata_pending[i].metadata && (metadata_pending[i].pos < next_pos || !next_pos))
	next_pos = metadata_pending[i].pos;
    }

  return next_pos;
}

static int
metadata_finalize_cb(struct output_metadata *metadata)
{
  if (!pb_session.playing_now)
    {
      DPRINTF(E_WARN, L_PLAYER, "Aborting metadata_send(), playback stopped during metadata preparation\n");
      return -1;
    }
  else if (metadata->item_id != pb_session.playing_now->item_id)
    {
      DPRINTF(E_WARN, L_PLAYER, "Aborting metadata_send(), item_id changed during metadata preparation (%" PRIu32 " -> %" PRIu32 ")\n",
	metadata->item_id, pb_session.playing_now->item_id);
      return -1;
    }

  if (!metadata->pos_ms)
    metadata->pos_ms = pb_session.playing_now->pos_ms;
  if (!metadata->len_ms)
    metadata->len_ms = pb_session.playing_now->len_ms;
  if (!metadata->pts.tv_sec)
    metadata->pts = pb_session.pts;

  return 0;
}

static enum command_state
metadata_finalize(void *arg, int *retval)
{
  if (!pb_session.playing_now)
    return COMMAND_END; // Playback ended while we doing the metadata update

  outputs_metadata_send(pb_session.playing_now->item_id, false, metadata_finalize_cb);

  status_update(player_state, LISTENER_PLAYER);

  return COMMAND_END;
}

// Done in worker thread because we avoid blocking db updates in the player
static void
metadata_update_queue_cb(void *arg)
{
  struct input_metadata *metadata = *(struct input_metadata **)arg;
  struct db_queue_item *queue_item;
  int ret;

  queue_item = db_queue_fetch_byitemid(metadata->item_id);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! Could not update queue metadata, the item_id is unknown (%u)\n", metadata->item_id);
      input_metadata_free(metadata, 0);
      return;
    }

  // Update queue item if metadata changed
  if (metadata->artist || metadata->title || metadata->album || metadata->genre || metadata->artwork_url || metadata->len_ms)
    {
      // Since we won't be using the metadata struct values for anything else
      // than this we just swap pointers
      if (metadata->artist)
	swap_pointers(&queue_item->artist, &metadata->artist);
      if (metadata->title)
	swap_pointers(&queue_item->title, &metadata->title);
      if (metadata->album)
	swap_pointers(&queue_item->album, &metadata->album);
      if (metadata->genre)
	swap_pointers(&queue_item->genre, &metadata->genre);
      if (metadata->artwork_url)
	swap_pointers(&queue_item->artwork_url, &metadata->artwork_url);
      if (metadata->len_ms)
	queue_item->song_length = metadata->len_ms;

      ret = db_queue_item_update(queue_item);
      if (ret < 0)
	DPRINTF(E_LOG, L_PLAYER, "Database error while updating queue with new metadata\n");
    }

  free_queue_item(queue_item, 0);
  input_metadata_free(metadata, 0);

  // Now return to the player thread and run metadata_finalize
  commands_exec_async(cmdbase, metadata_finalize, NULL);
}


/* ----------- Audio source handling (interfaces with input module) --------- */

static void
source_free(struct player_source **ps)
{
  if (!(*ps))
    return;

  free((*ps)->path);
  free(*ps);

  *ps = NULL;
}

/*
 * Creates a new player source for the given queue item
 */
static struct player_source *
source_create(struct db_queue_item *queue_item, uint32_t seek_ms)
{
  struct player_source *ps;

  CHECK_NULL(L_PLAYER, ps = calloc(1, sizeof(struct player_source)));

  ps->id = queue_item->file_id;
  ps->item_id = queue_item->id;
  ps->data_kind = queue_item->data_kind;
  ps->media_kind = queue_item->media_kind;
  ps->len_ms = queue_item->song_length;
  ps->is_seekable = (queue_item->song_length > 0);
  ps->path = strdup(queue_item->path);
  ps->seek_ms = seek_ms;

  return ps;
}

static struct player_source *
source_next_create(struct player_source *current)
{
  struct player_source *ps;
  struct db_queue_item *queue_item;

  if (!current)
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! source_next_create called without a current source\n");
      return NULL;
    }

  // Single-item fifo queue: there is no next item
  queue_item = db_queue_fetch_next(current->item_id, 0);
  if (!queue_item)
    return NULL;

  ps = source_create(queue_item, 0);

  free_queue_item(queue_item, 0);

  return ps;
}

static void
source_stop(void)
{
  input_stop();
}

static int
source_start(struct player_source *ps)
{
  if (!ps)
    return 0;

  DPRINTF(E_DBG, L_PLAYER, "Opening track: '%s' (id=%d, seek=%d)\n", ps->path, ps->item_id, ps->seek_ms);

  input_flush(NULL);

  return input_seek(ps->item_id, (int)ps->seek_ms);
}

static void
source_next(struct player_source *ps)
{
  if (!ps)
    return;

  DPRINTF(E_DBG, L_PLAYER, "Opening next track: '%s' (id=%d)\n", ps->path, ps->item_id);

  input_start(ps->item_id);
}

static int
source_restart(struct player_source *ps)
{
  DPRINTF(E_DBG, L_PLAYER, "Restarting track: '%s' (id=%d, pos=%d)\n", ps->path, ps->item_id, ps->pos_ms);

  // Must be non-blocking, because otherwise we get a deadlock via the input
  // thread making a sync call to player_playback_start() -> pb_resume() ->
  // source_restart() -> input_resume()
  input_resume(ps->item_id, ps->pos_ms);

  return 0;
}

/* ------------------------ Playback session upkeep ------------------------- */

// The below update the playback session so it is always in mint condition. That
// is all they do, they should not do anything else. If you are looking for a
// place to add some non session actions, look further down at the events.

#ifdef DEBUG_PLAYER
static int debug_dump_counter = -1;

static int
source_print(char *line, size_t linesize, struct player_source *ps, const char *name)
{
  int pos = 0;

  if (ps)
    {
      pos += snprintf(line + pos, linesize - pos, "%s.path=%s; ", name, ps->path);
      pos += snprintf(line + pos, linesize - pos, "%s.quality=%d; ", name, ps->quality.sample_rate);
      pos += snprintf(line + pos, linesize - pos, "%s.item_id=%u; ", name, ps->item_id);
      pos += snprintf(line + pos, linesize - pos, "%s.read_start=%" PRIu64 "; ", name, ps->read_start);
      pos += snprintf(line + pos, linesize - pos, "%s.play_start=%" PRIu64 "; ", name, ps->play_start);
      pos += snprintf(line + pos, linesize - pos, "%s.read_end=%" PRIu64 "; ", name, ps->read_end);
      pos += snprintf(line + pos, linesize - pos, "%s.play_end=%" PRIu64 "; ", name, ps->play_end);
      pos += snprintf(line + pos, linesize - pos, "%s.metadata_update=%" PRIu64 "; ", name, ps->metadata_update);
      pos += snprintf(line + pos, linesize - pos, "%s.pos_ms=%u; ", name, ps->pos_ms);
      pos += snprintf(line + pos, linesize - pos, "%s.seek_ms=%u; ", name, ps->seek_ms);
    }
  else
    pos += snprintf(line + pos, linesize - pos, "%s=(null); ", name);

  return pos;
}

static void
session_dump(bool use_counter)
{
  struct player_source *ps;
  char line[4096];
  char label[32];
  int n;
  int pos = 0;

  if (use_counter)
    {
      debug_dump_counter++;
      if (debug_dump_counter % 100 != 0)
	return;
    }

  for (ps = pb_session.source_list, n = 0; ps; ps = ps->prev, n--)
    {
      pos = snprintf(line, sizeof(line), "pos=%d; ", pb_session.pos);
      if (ps == pb_session.playing_now && ps == pb_session.reading_now)
	snprintf(label, sizeof(label), "[%d][rp]", n);
      else if (ps == pb_session.playing_now)
	snprintf(label, sizeof(label), "[%d][p]", n);
      else if (ps == pb_session.reading_now)
	snprintf(label, sizeof(label), "[%d][r]", n);
      else
	snprintf(label, sizeof(label), "[%d][n]", n);

      pos += source_print(line + pos, sizeof(line) - pos, ps, label);

      DPRINTF(E_DBG, L_PLAYER, "%s\n", line);
    }
}
#endif

static void
session_update_play_eof(void)
{
  struct player_source *ps = pb_session.playing_now;

  pb_session.playing_now = pb_session.playing_now->next;

  // Remove the item we completed playing from source_list
  if (pb_session.playing_now)
    pb_session.playing_now->prev = NULL;
  else
    pb_session.source_list = NULL;

  // Free the removed item
  source_free(&ps);
}

static void
session_update_play_start(void)
{
  // Nothing to update right now
}

static void
session_update_read_next(struct player_source *ps)
{
  // Attach to linked source list
  ps->prev = pb_session.source_list;
  pb_session.source_list->next = ps;

  pb_session.source_list = ps;
}

static void
session_update_read_eof(void)
{
  pb_session.reading_now->read_end = pb_session.pos;
  pb_session.reading_now->play_end = pb_session.pos + pb_session.reading_now->output_buffer_samples;

  pb_session.reading_now = pb_session.reading_now->next;

  // There is nothing else to play
  if (!pb_session.reading_now)
    return;

  // We inherit this because the input will only notify on quality changes, not
  // if it is the same as the previous track
  pb_session.reading_now->quality = pb_session.reading_now->prev->quality;
  pb_session.reading_now->output_buffer_samples = pb_session.reading_now->prev->output_buffer_samples;

  pb_session.reading_now->read_start = pb_session.pos;
  pb_session.reading_now->play_start = pb_session.pos + pb_session.reading_now->output_buffer_samples;
}

static void
session_update_read_start(uint32_t seek_ms)
{
  pb_session.reading_now = pb_session.source_list;

  // There is nothing to play
  if (!pb_session.reading_now)
    return;

  pb_session.reading_now->pos_ms     = seek_ms;
  pb_session.reading_now->seek_ms    = seek_ms;
  pb_session.reading_now->read_start = pb_session.pos;

  pb_session.playing_now = pb_session.reading_now;
}

static inline void
session_update_read(int nsamples)
{
  uint32_t step_ms;

  // Did we just complete our first read? Then set the start timestamp
  if (pb_session.start_ts.tv_sec == 0)
    {
      // TODO raop/airplay ntp uses CLOCK_MONOTONIC, so this is just for testing PTP
      clock_gettime_with_res(CLOCK_MONOTONIC, &pb_session.start_ts, &player_timer_res);
      pb_session.pts = pb_session.start_ts;
    }

  // Advance position
  pb_session.pos += nsamples;

  // Need to know sample rate to calculate pos_ms step
  if (!pb_session.playing_now->quality.sample_rate)
    return;

  step_ms = (1000 * nsamples) / pb_session.playing_now->quality.sample_rate;

  // After we have started playing we also must calculate new pos_ms
  if (pb_session.pos > pb_session.playing_now->play_start)
    pb_session.playing_now->pos_ms += step_ms;
}

static void
session_update_read_quality(struct media_quality *quality)
{
  int samples_per_read;

  if (quality_is_equal(quality, &pb_session.quality))
    goto out;

  pb_session.quality = *quality;
  pb_session.reading_now->quality = *quality;

  samples_per_read = ((uint64_t)quality->sample_rate * (player_tick_interval.tv_nsec / 1000000)) / 1000;
  pb_session.reading_now->output_buffer_samples = outputs_buffer_duration_ms_get() * quality->sample_rate / 1000;

  pb_session.bufsize = STOB(samples_per_read, quality->bits_per_sample, quality->channels);
  pb_session.read_deficit_max = STOB(((uint64_t)quality->sample_rate * PLAYER_READ_BEHIND_MAX) / 1000, quality->bits_per_sample, quality->channels);

  DPRINTF(E_DBG, L_PLAYER, "New session values (q=%d/%d/%d, spr=%d, bufsize=%zu)\n",
    quality->sample_rate, quality->bits_per_sample, quality->channels, samples_per_read, pb_session.bufsize);

  if (pb_session.buffer)
    pb_session.buffer = realloc(pb_session.buffer, pb_session.bufsize);
  else
    pb_session.buffer = malloc(pb_session.bufsize);

  CHECK_NULL(L_PLAYER, pb_session.buffer);

  // Maybe we should actually adjust play_start and play_end of all items in the
  // source list when the quality changes?
  pb_session.reading_now->play_start = pb_session.reading_now->read_start + pb_session.reading_now->output_buffer_samples;
  if (pb_session.reading_now->prev)
    pb_session.reading_now->prev->play_end = pb_session.reading_now->play_start;

 out:
  free(quality);
}

static void
session_update_read_ts(struct timespec *ts)
{
  pb_session.pts = *ts;

  free(ts);
}

static void
session_update_read_metadata(void)
{
  if (!pb_session.reading_now)
    return;

  // Sets when to trigger the next event_play_metadata()
  pb_session.reading_now->metadata_update = metadata_pending_next_pos();
}

static void
session_update_play_metadata(struct input_metadata *metadata)
{
  if (metadata->pos_is_updated)
    pb_session.playing_now->pos_ms = metadata->pos_ms;
  if (metadata->len_ms)
    pb_session.playing_now->len_ms = metadata->len_ms;
}

static void
session_restart(void)
{
  pb_session.start_ts.tv_sec = 0;
  pb_session.start_ts.tv_nsec = 0;
  pb_session.pts.tv_sec = 0;
  pb_session.pts.tv_nsec = 0;
  pb_session.read_deficit = 0;
  pb_session.metadata_sent = 0;
}

static void
session_stop(void)
{
  struct player_source *ps;

  free(pb_session.buffer);
  pb_session.buffer = NULL;

  for (ps = pb_session.source_list; pb_session.source_list; ps = pb_session.source_list)
    {
      pb_session.source_list = ps->prev;
      source_free(&ps);
    }

  memset(&pb_session, 0, sizeof(struct player_session));
}

static void
session_start(struct player_source *ps)
{
  session_stop();

  pb_session.source_list = ps;
}


/* ------------------------- Playback event handlers ------------------------ */

static void
event_read_quality(struct media_quality *quality)
{
  DPRINTF(E_DBG, L_PLAYER, "event_read_quality()\n");

  session_update_read_quality(quality);
}

static void
event_read_ts(struct timespec *ts)
{
  DPRINTF(E_DBG, L_PLAYER, "event_read_ts()\n");

  session_update_read_ts(ts);
}

// Stuff to do when read of current track ends
static void
event_read_eof()
{
  DPRINTF(E_DBG, L_PLAYER, "event_read_eof()\n");

  session_update_read_eof();
}

static void
event_read_error()
{
  DPRINTF(E_DBG, L_PLAYER, "event_read_error()\n");

  db_queue_delete_byitemid(pb_session.reading_now->item_id);

  event_read_eof();
}

// Kicks of input reading of next source (async)
static void
event_read_start_next()
{
  DPRINTF(E_DBG, L_PLAYER, "event_read_start_next()\n");

  struct player_source *ps = source_next_create(pb_session.source_list);
  if (!ps)
    return;

  // Attaches next item to pb_session.source_list
  session_update_read_next(ps);

  // Starts the input read loop
  source_next(pb_session.source_list);
}

static void
event_read_metadata(struct input_metadata *metadata)
{
  uint64_t delay;
  int ret;

  DPRINTF(E_DBG, L_PLAYER, "event_read_metadata()\n");

  // Add the metadata to the register of pending events with a trigger position
  // that corresponds to outputs_buffer_duration_ms into the future. If we have
  // received a negative position we assume the metadata needs to be delayed
  // until the position is 0.
  if (metadata->pos_is_updated && metadata->pos_ms < 0)
    {
      delay = pb_session.reading_now->output_buffer_samples + (-metadata->pos_ms) * (uint64_t)pb_session.reading_now->quality.sample_rate / 1000;
      metadata->pos_ms = 0;
    }
  else
    delay = pb_session.reading_now->output_buffer_samples;

  ret = metadata_pending_add(metadata, pb_session.pos + delay);
  if (ret < 0)
    {
      input_metadata_free(metadata, 0);
      return;
    }

  session_update_read_metadata();
}

static void
event_play_end()
{
  DPRINTF(E_DBG, L_PLAYER, "event_play_end()\n");

  pb_abort();
}

// Stuff to do when playback of current track ends
static void
event_play_eof()
{
  DPRINTF(E_DBG, L_PLAYER, "event_play_eof()\n");

  if (pb_session.playing_now->next)
    outputs_metadata_send(pb_session.playing_now->next->item_id, false, metadata_finalize_cb);

  session_update_play_eof();
}

static void
event_play_start()
{
  DPRINTF(E_DBG, L_PLAYER, "event_play_start()\n");

  if (!pb_session.metadata_sent)
    {
      outputs_metadata_send(pb_session.playing_now->item_id, true, metadata_finalize_cb);
      pb_session.metadata_sent = 1;
    }

  session_update_play_start();

  status_update(PLAY_PLAYING, LISTENER_PLAYER);
}

static void
event_play_metadata()
{
  int i;

  DPRINTF(E_DBG, L_PLAYER, "event_play_metadata()\n");

  for (i = 0; i < ARRAY_SIZE(metadata_pending); i++)
    {
      // Proces all events with position from metadata_update (included) to
      // current read position (excluded)
      if (!(metadata_pending[i].pos >= pb_session.playing_now->metadata_update && metadata_pending[i].pos < pb_session.pos))
	continue;

      // Just in case
      if (!metadata_pending[i].metadata)
	continue;

      session_update_play_metadata(metadata_pending[i].metadata);

      // Triggers an async chain of metadata update, first worker will do an
      // update of the db, then the player will update outputs, where the worker
      // may be called by the output, and then player sends status_update
      worker_execute(metadata_update_queue_cb, &(metadata_pending[i].metadata), sizeof(metadata_pending[i].metadata), 0);

      memset(&metadata_pending[i], 0, sizeof(struct metadata_pending_register));
    }

  // Set trigger (playing_now->metadata_update) to next pending metadata
  session_update_read_metadata();
}

// Checks if the new playback position requires change of play status, plus
// calls session_update_read that updates playback position
static inline void
event_read(int nsamples)
{
  // Shouldn't happen, playing_now must be set during playback, but check anyway
  if (!pb_session.playing_now)
    return;

  if (pb_session.playing_now->play_end != 0 && pb_session.pos + nsamples >= pb_session.playing_now->play_end)
    {
      event_play_eof();

      if (!pb_session.playing_now)
	{
	  event_play_end();
	  return;
	}
    }

  session_update_read(nsamples);

  // Check if the playback position passed the play_start position
  if (pb_session.pos > pb_session.playing_now->play_start && pb_session.pos <= pb_session.playing_now->play_start + nsamples)
    event_play_start();

  if (pb_session.playing_now->metadata_update == 0)
    return;

  // Check if the playback position passed an input metadata update. The event
  // must process all metadata updates in the read interval.
  if (pb_session.pos > pb_session.playing_now->metadata_update && pb_session.pos <= pb_session.playing_now->metadata_update + nsamples)
    event_play_metadata();
}


/* ---- Main playback stuff: Start, read, write and playback timer event ---- */

// Returns -1 on error or bytes read (possibly 0)
static inline int
source_read(int *nbytes, int *nsamples, uint8_t *buf, int len)
{
  short flag;
  void *flagdata;

  // We can get into this condition if a) we finished reading, but are still
  // playing (playing_now is non-null), or b) the calling loop tries to catch up
  // with an overrun or a deficit, but playback ended in the first iteration (in
  // which case playing_now is null). Note that a) also can occur if the input
  // starts with input_write(NULL, NULL, INPUT_FLAG_ERROR).
  if (!pb_session.reading_now)
    {
      // This is only for case a). If we are in case b) the session was zeroed,
      // which means nsamples will become zero. Quality can be zero if the input
      // failed immediately, i.e. never passed INPUT_FLAG_QUALITY.
      *nbytes = len;
      *nsamples = (len != 0) ? BTOS(*nbytes, pb_session.quality.bits_per_sample, pb_session.quality.channels) : 0;

      // In case a) this advances playback position and possibly ends playback,
      // i.e. sets playing_now to null
      event_read(*nsamples);
      if (!pb_session.playing_now)
	{
	  *nbytes = 0;
	  *nsamples = 0;
	  return 0;
	}

      // Stream silence if playback didn't end yet
      memset(buf, 0, len);
      return 0;
    }

  *nsamples = 0;
  *nbytes = input_read(buf, len, &flag, &flagdata);
  if ((*nbytes < 0) || (flag == INPUT_FLAG_ERROR))
    {
      DPRINTF(E_LOG, L_PLAYER, "Error reading source '%s' (id=%d)\n", pb_session.reading_now->path, pb_session.reading_now->id);
      event_read_error();
      return -1;
    }
  else if (flag == INPUT_FLAG_START_NEXT)
    {
      event_read_start_next();
    }
  else if (flag == INPUT_FLAG_EOF)
    {
      event_read_eof();
    }
  else if (flag == INPUT_FLAG_METADATA)
    {
      event_read_metadata((struct input_metadata *)flagdata);
    }
  else if (flag == INPUT_FLAG_QUALITY)
    {
      event_read_quality((struct media_quality *)flagdata);
    }
  else if (flag == INPUT_FLAG_SYNC)
    {
      event_read_ts((struct timespec *)flagdata);
    }

  if (*nbytes == 0 || pb_session.quality.channels == 0)
    {
      event_read(0); // This will set start_ts even if source isn't open yet
      return 0;
    }

  *nsamples = BTOS(*nbytes, pb_session.quality.bits_per_sample, pb_session.quality.channels);

  event_read(*nsamples);

  return 0;
}

// Player-thread tick jitter: a fixed-size histogram of how late each
// playback tick fires relative to the nominal player_tick_interval.
// Updated only from playback_cb(), which runs solely on the player thread
// (see the note in that function), so plain non-atomic state is fine here.
// Summarised to the log and reset every PB_JITTER_LOG_INTERVAL_SEC, rather
// than kept forever, so this stays fixed-size and cheap - no heap
// allocation, no unbounded growth over time.
#define PB_JITTER_NBUCKETS 11
#define PB_JITTER_LOG_INTERVAL_SEC 300

static const uint32_t pb_jitter_edges_ms[PB_JITTER_NBUCKETS - 1] =
  { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000 };
static const struct timespec pb_jitter_log_interval = { PB_JITTER_LOG_INTERVAL_SEC, 0 };

static uint64_t pb_jitter_buckets[PB_JITTER_NBUCKETS];
static uint64_t pb_jitter_ticks;
static uint64_t pb_jitter_max_us;
static struct timespec pb_jitter_last_tick;
static struct timespec pb_jitter_last_log;

// The player thread's CPU clock at the start of the previous tick, taken
// together with pb_jitter_last_tick. One clock read per tick; it is compared
// only when a late-tick warning is about to be logged.
static struct timespec pb_cpu_last_tick;

static uint64_t
timespec_ms(struct timespec ts)
{
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Time the calling thread has spent runnable but not on a cpu, in nanoseconds,
// from the scheduler's per-thread statistics. Returns -1 if they cannot be
// read. Only called when a late-tick warning is logged.
static int
pb_run_delay_get(uint64_t *wait_ns)
{
  unsigned long long run_ns;
  unsigned long long delay_ns;
  char buf[128];
  int fd;
  int n;

  fd = open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;

  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;

  buf[n] = '\0';
  if (sscanf(buf, "%llu %llu", &run_ns, &delay_ns) != 2)
    return -1;

  *wait_ns = delay_ns;
  return 0;
}

// Describes the span since the previous tick for a late-tick warning: how long
// it was, how much of it the player thread spent on a cpu, and how much longer
// the thread has waited for a cpu since the previous warning. A thread that was
// busy shows a cpu time close to the span; one that was kept off the cpu shows
// a small cpu time and a run delay that accounts for the lateness.
static void
pb_delay_detail(char *buf, size_t len, unsigned int now_s, unsigned int last_emit_s)
{
  static uint64_t last_wait_ns;
  static bool last_wait_valid;
  struct timespec wall;
  struct timespec cpu;
  uint64_t wait_ns;
  int n;

  if (pb_jitter_last_tick.tv_sec == 0 && pb_jitter_last_tick.tv_nsec == 0)
    {
      snprintf(buf, len, "no earlier tick to compare with");
      return;
    }

  clock_gettime(CLOCK_MONOTONIC, &wall);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);

  n = snprintf(buf, len, "%" PRIu64 " ms since last tick, %" PRIu64 " ms of it on cpu",
               timespec_ms(timespec_sub(wall, pb_jitter_last_tick)), timespec_ms(timespec_sub(cpu, pb_cpu_last_tick)));

  if (pb_run_delay_get(&wait_ns) == 0)
    {
      if (last_wait_valid && last_emit_s != 0 && n > 0 && (size_t)n < len)
	snprintf(buf + n, len - n, "; waited %" PRIu64 " ms for a cpu over the last %u s", (wait_ns - last_wait_ns) / 1000000, now_s - last_emit_s);

      last_wait_ns = wait_ns;
      last_wait_valid = true;
    }
}

static void
playback_cb(int fd, short what, void *arg)
{
  struct timespec ts;
  uint64_t overrun;
  int nbytes;
  int nsamples;
  int i;
  int ret;

  // Check if we missed any timer expirations
  overrun = 0;
#ifdef HAVE_TIMERFD
  ret = read(fd, &overrun, sizeof(overrun));
  if (ret <= 0)
    DPRINTF(E_LOG, L_PLAYER, "Error reading timer\n");
  else if (overrun > 0)
    overrun--;
#else
  ret = timer_getoverrun(pb_timer);
  if (ret < 0)
    DPRINTF(E_LOG, L_PLAYER, "Error getting timer overrun\n");
  else
    overrun = ret;
#endif /* HAVE_TIMERFD */

  // We are too delayed, probably some output blocked: reset if first overrun or abort if second overrun.
  // Pipe sources with no active outputs will appear to overrun (nothing to write to), so skip this.
  if (overrun > pb_write_deficit_max && pb_session.playing_now->data_kind != DATA_KIND_PIPE)
    {
      if (pb_write_recovery)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Permanent output delay detected (behind=%" PRIu64 ", max=%d), aborting\n", overrun, pb_write_deficit_max);
	  pb_abort();
	  return;
	}

      DPRINTF(E_LOG, L_PLAYER, "Output delay detected (behind=%" PRIu64 ", max=%d), resetting all outputs\n", overrun, pb_write_deficit_max);
      pb_write_recovery = true;
      player_flush_pending = pb_suspend();
      // No devices to wait for, just set the restart cb right away. Otherwise
      // the trigger will be set by device_flush_cb.
      if (player_flush_pending == 0)
	input_buffer_full_cb(player_playback_start);

      return;
    }
  else
    {
      if (overrun > 1) // An overrun of 1 is no big deal
	{
	  // This fires once per catch-up, which can be as often as every tick
	  // under sustained output pressure, so it is throttled to one line
	  // per 5 seconds. Unlike DPRINTF_THROTTLED(), the summary line
	  // reports the worst overrun seen during the suppressed window
	  // rather than just a count, since that is the more useful number
	  // here -- so this call site keeps its own static state instead of
	  // using the generic macro. playback_cb() only ever runs on the
	  // player thread, so no locking is needed for it.
	  static unsigned int pb_delay_last_emit;
	  static unsigned int pb_delay_suppressed;
	  static uint64_t pb_delay_max_overrun;
	  unsigned int pb_delay_now;
	  char pb_delay_text[160];

	  // Gate on the same check DPRINTF() applies internally, so that when
	  // E_WARN/L_PLAYER is filtered out this costs nothing beyond that
	  // check -- and so the clock is never read for a line that will not
	  // be logged anyway.
	  if (logger_wants(E_WARN, L_PLAYER))
	    {
	      if (overrun > pb_delay_max_overrun)
		pb_delay_max_overrun = overrun;

	      // Monotonic, not time(NULL): a wall-clock step (NTP, DST, a
	      // manual clock set) must not be able to stall this throttle
	      // open or make it fire early.
	      pb_delay_now = logger_now_s();
	      // pb_delay_last_emit == 0 means "never emitted", not a real
	      // timestamp: the clock counts from boot, so without this check
	      // a first hit landing within 5 seconds of boot would otherwise
	      // be wrongly suppressed.
	      if (pb_delay_last_emit == 0 || pb_delay_now - pb_delay_last_emit >= 5)
		{
		  pb_delay_detail(pb_delay_text, sizeof(pb_delay_text), pb_delay_now, pb_delay_last_emit);

		  if (pb_delay_suppressed > 0)
		    DPRINTF(E_WARN, L_PLAYER, "Output delay detected: player is %" PRIu64 " ticks behind, catching up (max %" PRIu64 " ticks, %u similar suppressed; %s)\n",
			    overrun, pb_delay_max_overrun, pb_delay_suppressed, pb_delay_text);
		  else
		    DPRINTF(E_WARN, L_PLAYER, "Output delay detected: player is %" PRIu64 " ticks behind, catching up (%s)\n", overrun, pb_delay_text);

		  pb_delay_last_emit = pb_delay_now;
		  pb_delay_suppressed = 0;
		  pb_delay_max_overrun = 0;
		}
	      else
		pb_delay_suppressed++;
	    }
	}

      pb_write_recovery = false;
    }

  // Player-thread tick jitter measurement (fixed-size histogram, see the
  // static state above). Only reached for ticks that get past the delay
  // reset/abort handling above, i.e. "normal" ticks.
  clock_gettime(CLOCK_MONOTONIC, &ts);
  if (pb_jitter_last_tick.tv_sec == 0 && pb_jitter_last_tick.tv_nsec == 0)
    {
      // First tick seen (or just (re)started): nothing to compare against
      // yet, but seed the log window so it starts counting from here.
      pb_jitter_last_log = ts;
    }
  else
    {
      struct timespec delta;
      int64_t delta_us;
      int64_t tick_us;
      int64_t late_us;
      int bucket;

      delta = timespec_sub(ts, pb_jitter_last_tick);
      delta_us = (int64_t)delta.tv_sec * 1000000L + delta.tv_nsec / 1000L;
      tick_us = (int64_t)player_tick_interval.tv_sec * 1000000L + player_tick_interval.tv_nsec / 1000L;
      late_us = delta_us - tick_us;
      if (late_us < 0)
	late_us = 0;

      if ((uint64_t)late_us > pb_jitter_max_us)
	pb_jitter_max_us = (uint64_t)late_us;

      for (bucket = 0; bucket < PB_JITTER_NBUCKETS - 1; bucket++)
	if (late_us < (int64_t)pb_jitter_edges_ms[bucket] * 1000L)
	  break;

      pb_jitter_buckets[bucket]++;
      pb_jitter_ticks++;
    }
  pb_jitter_last_tick = ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &pb_cpu_last_tick);

  if (pb_jitter_ticks > 0 && timespec_cmp(timespec_sub(ts, pb_jitter_last_log), pb_jitter_log_interval) >= 0)
    {
      DPRINTF(E_INFO, L_PLAYER, "tick jitter (last %ds): ticks=%" PRIu64 " max=%.1fms buckets[<1,1-2,2-5,5-10,10-20,20-50,50-100,100-200,200-500,500-1k,>1k]="
	      "%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",
	      PB_JITTER_LOG_INTERVAL_SEC, pb_jitter_ticks, pb_jitter_max_us / 1000.0,
	      pb_jitter_buckets[0], pb_jitter_buckets[1], pb_jitter_buckets[2], pb_jitter_buckets[3], pb_jitter_buckets[4],
	      pb_jitter_buckets[5], pb_jitter_buckets[6], pb_jitter_buckets[7], pb_jitter_buckets[8], pb_jitter_buckets[9],
	      pb_jitter_buckets[10]);

      memset(pb_jitter_buckets, 0, sizeof(pb_jitter_buckets));
      pb_jitter_ticks = 0;
      pb_jitter_max_us = 0;
      pb_jitter_last_log = ts;
    }

#ifdef DEBUG_PLAYER
  session_dump(true);
#endif

  // The pessimistic approach: Assume you won't get anything, then anything that
  // comes your way is a positive surprise.
  pb_session.read_deficit += (1 + overrun) * pb_session.bufsize;

  // If there was an overrun, we will try to read/write a corresponding number
  // of times so we catch up. The read from the input is non-blocking, so it
  // should not bring us further behind, even if there is no data.
  for (i = 1 + overrun; i > 0; i--)
    {
      ret = source_read(&nbytes, &nsamples, pb_session.buffer, pb_session.bufsize);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Error reading from source\n");
	  pb_session.read_deficit -= pb_session.bufsize;
	  break;
	}
      if (nbytes == 0)
	{
	  break;
	}

      pb_session.read_deficit -= nbytes;

      outputs_write(pb_session.buffer, nbytes, nsamples, &pb_session.quality, &pb_session.pts);

      if (nbytes < pb_session.bufsize)
	{
	  // How much the number of samples we got corresponds to in time (nanoseconds)
	  ts.tv_sec = 0;
	  ts.tv_nsec = 1000000000UL * (uint64_t)nsamples / pb_session.quality.sample_rate;

	  // Can fire on every tick while the source is running dry, so throttled
	  // to one line per 5 seconds (see DPRINTF_THROTTLED() in logger.h).
	  DPRINTF_THROTTLED(5, E_DBG, L_PLAYER, "Incomplete read, wanted %zu, got %d (samples=%d/time=%lu), deficit %zu", pb_session.bufsize, nbytes, nsamples, ts.tv_nsec, pb_session.read_deficit);

	  pb_session.pts = timespec_add(pb_session.pts, ts);
	}
      else
	{
	  // We got a full frame, so that means we can also advance the presentation timestamp by a full tick
	  pb_session.pts = timespec_add(pb_session.pts, player_tick_interval);

	  // It is going well, lets take another round to repay our debt
	  if (i == 1 && pb_session.read_deficit > pb_session.bufsize)
	    i = 2;
	}
    }

  if (pb_session.read_deficit_max && pb_session.read_deficit > pb_session.read_deficit_max
      && pb_session.playing_now->data_kind != DATA_KIND_PIPE)
    {
      DPRINTF(E_LOG, L_PLAYER, "Source is not providing sufficient data, temporarily suspending playback (deficit=%zu/%zu bytes)\n",
	pb_session.read_deficit, pb_session.read_deficit_max);

      player_flush_pending = pb_suspend();
      // No devices to wait for, just set the restart cb right away. Otherwise
      // the trigger will be set by device_flush_cb.
      if (player_flush_pending == 0)
	input_buffer_full_cb(player_playback_start);
    }
}


/* ----------------- Output device handling (add/remove etc) ---------------- */

static int
speaker_visible_count(void)
{
  struct output_device *device;
  int visible;

  visible = 0;
  for (device = outputs_list(); device; device = device->next)
    {
      if (speaker_visible(device))
	visible++;
    }

  return visible;
}

static void
zero_output_recovery_check(bool count_towards_rescan)
{
  int visible;

  visible = speaker_visible_count();
  DPRINTF(E_SPAM, L_PLAYER, "Zero-output recovery check: visible=%d ever_seen=%u checks=%d attempted=%u\n",
          visible, outputs_ever_seen, zero_output_gc_checks, zero_output_rescan_attempted);

  if (visible > 0)
    {
      if (!outputs_ever_seen || zero_output_gc_checks || zero_output_rescan_attempted)
	DPRINTF(E_DBG, L_PLAYER, "Visible outputs present; arming zero-output recovery for future empty episodes\n");

      outputs_ever_seen = true;
      zero_output_gc_checks = 0;
      zero_output_rescan_attempted = false;
      return;
    }

  if (!outputs_ever_seen)
    return;

  if (zero_output_rescan_attempted)
    {
      DPRINTF(E_SPAM, L_PLAYER, "Zero-output recovery already attempted; waiting for outputs to reappear\n");
      return;
    }

  if (!count_towards_rescan)
    {
      DPRINTF(E_DBG, L_PLAYER, "Zero visible outputs observed after previous discovery; waiting for GC threshold\n");
      return;
    }

  zero_output_gc_checks++;
  DPRINTF(E_DBG, L_PLAYER, "Zero outputs observed after previous discovery: check %d/%d\n",
          zero_output_gc_checks, ZERO_OUTPUT_RESCAN_CHECKS);

  if (zero_output_gc_checks < ZERO_OUTPUT_RESCAN_CHECKS)
    return;

  DPRINTF(E_INFO, L_PLAYER, "No visible outputs after previous discovery; requesting mDNS rescan\n");
  if (mdns_rescan_request())
    {
      zero_output_gc_checks = 0;
      zero_output_rescan_attempted = true;
    }
  else
    {
      DPRINTF(E_WARN, L_PLAYER, "mDNS rescan request could not be enqueued; will retry after the next threshold window\n");
      zero_output_gc_checks = 0;
    }
}

static enum command_state
device_add(void *arg, int *retval)
{
  union player_arg *cmdarg = arg;
  struct output_device *device = cmdarg->device;
  bool new_deselect;

  // Never turn on new devices during playback
  new_deselect = (player_state == PLAY_PLAYING);

  device = outputs_device_add(device, new_deselect);
  if (!device)
    {
      *retval = -1;
      return COMMAND_END;
    }

  status_update(player_state, LISTENER_SPEAKER | LISTENER_VOLUME);
  zero_output_recovery_check(false);

  *retval = 0;
  return COMMAND_END;
}

// Housekeeping timer callback: runs every PLAYER_DEVICE_GC_INTERVAL seconds in
// the player thread. Frees removal-candidate device slots whose grace period
// has expired and fires LISTENER_SPEAKER if the output list changed.
static void
device_gc_cb(int fd, short event, void *arg)
{
  struct timeval tv = { PLAYER_DEVICE_GC_INTERVAL, 0 };
  bool any_changed;

  any_changed = outputs_device_gc(device_removal_grace_secs);
  if (any_changed)
    status_update(player_state, LISTENER_SPEAKER | LISTENER_VOLUME);

  zero_output_recovery_check(true);

  evtimer_add(device_gc_ev, &tv);
}

static enum command_state
device_remove_family(void *arg, int *retval)
{
  union player_arg *cmdarg = arg;
  struct output_device *remove;
  struct output_device *device;
  int ret;

  remove = cmdarg->device;
  DPRINTF(E_DBG, L_PLAYER,
          "device_remove_family: remove=%p id=%" PRIu64 " type=%s name='%s' v4_port=%d v6_port=%d advertised=%u\n",
          remove, remove->id, remove->type_name ? remove->type_name : "(null)",
          remove->name ? remove->name : "(null)", remove->v4_port, remove->v6_port,
          remove->advertised);

  device = outputs_device_get(remove->id);
  if (!device)
    {
      DPRINTF(E_WARN, L_PLAYER, "The %s device '%s' stopped advertising, but not in our list\n", remove->type_name, remove->name);

      outputs_device_free(remove);
      *retval = 0;
      return COMMAND_END;
    }

  DPRINTF(E_DBG, L_PLAYER,
          "device_remove_family: canonical=%p id=%" PRIu64 " name='%s' type=%s session=%p supported_modes=0x%x cand_raop=%p cand_airplay2=%p\n",
          device, device->id, device->name ? device->name : "(null)",
          device->type_name ? device->type_name : "(null)", device->session,
          device->supported_modes, device->candidate_raop, device->candidate_airplay2);

  // Delegate to outputs.c which understands the candidate model. Returns 0
  // (candidate deferred to GC) or -1 (legacy non-multi-protocol type).
  ret = outputs_device_protocol_remove(device, remove);
  DPRINTF(E_DBG, L_PLAYER,
          "device_remove_family: outputs_device_protocol_remove returned %d for canonical id=%" PRIu64 " name='%s'\n",
          ret, device->id, device->name ? device->name : "(null)");
  if (ret < 0)
    {
      // Not a known multi-protocol type (e.g. legacy local audio):
      // fall back to the direct address-removal path.
      if (remove->v4_port && device->v4_address)
        {
          free(device->v4_address);
          device->v4_address = NULL;
          device->v4_port = 0;
        }
      if (remove->v6_port && device->v6_address)
        {
          free(device->v6_address);
          device->v6_address = NULL;
          device->v6_port = 0;
        }
      if (!device->v4_address && !device->v6_address)
        {
          device->advertised = 0;
          // If there is a session we keep the device until the backend gives
          // us a failure callback; outputs.c will then remove it.
          if (!device->session)
            outputs_device_remove(device);
        }
    }
  else if (ret == 1)
    {
      // All candidates gone, no active session: remove canonical device
      outputs_device_remove(device);
    }

  outputs_device_free(remove);

  status_update(player_state, LISTENER_SPEAKER | LISTENER_VOLUME);
  zero_output_recovery_check(false);

  *retval = 0;
  return COMMAND_END;
}


/* -------- Output device callbacks executed in the player thread ----------- */

static void
tv_proxy_followers_start(struct output_device *leader);
static struct output_device *
tv_proxy_leader_of(struct output_device *follower);
static void
tv_proxy_leader_start(struct output_device *follower);
static bool
speaker_group_sibling_held(struct output_device *device);
static void
device_op_cb(struct output_device *device, enum output_device_state status);
static void
device_streaming_cb(struct output_device *device, enum output_device_state status);
static void
device_volume_cb(struct output_device *device, enum output_device_state status);
static void
device_flush_cb(struct output_device *device, enum output_device_state status);
static void
device_level_edge_cb(struct output_device *device, enum output_device_state status);
static void
device_volume_echo_cb(struct output_device *device, enum output_device_state status);
static int
device_start(struct output_device *device, bool only_probe, bool explicit_request);
static void
device_request(struct output_device *device, enum output_request req, bool explicit_request, bool backend_changes);

// A selected device whose session failed is started again by a timer of its
// own. The timer's argument is a copy of the device id, so a device freed in
// the meantime is found missing rather than read; the timer is freed when it
// fires, when the device is deselected and with the device
static void
resurrect_cb(int fd, short what, void *arg)
{
  uint64_t *id = arg;
  struct output_device *device;

  device = outputs_device_get(*id);
  if (!device)
    return;

  event_free(device->resurrect_ev);
  device->resurrect_ev = NULL;
  free(id);

  DPRINTF(E_DBG, L_PLAYER, "Speaker resurrect: '%s' (id=%" PRIu64 ")\n", device->name, device->id);

  // Not while it waits for a PIN (a start would replace the PIN on screen),
  // and not when nothing plays: every source is a live pipe, which plays on
  // when its speakers fail, so there is no suspended playback to resume
  if (!device->selected || device->pin_pending || device->busy || device->prevent_playback || player_state != PLAY_PLAYING)
    return;

  device_request(device, OUTPUT_REQ_SELECT, false, false);
  status_update(player_state, LISTENER_SPEAKER | LISTENER_VOLUME);
}

static void
resurrect_arm(struct output_device *device, int seconds)
{
  struct timeval tv = { seconds, 0 };
  uint64_t *id;

  if (!device->resurrect_ev)
    {
      CHECK_NULL(L_PLAYER, id = malloc(sizeof(*id)));
      *id = device->id;
      CHECK_NULL(L_PLAYER, device->resurrect_ev = evtimer_new(evbase_player, resurrect_cb, id));
    }

  evtimer_add(device->resurrect_ev, &tv);
}

static void
resurrect_cancel(struct output_device *device)
{
  if (!device->resurrect_ev)
    return;

  free(event_get_callback_arg(device->resurrect_ev));
  event_free(device->resurrect_ev);
  device->resurrect_ev = NULL;
}

static void
pin_wait_arm(struct output_device *device, int seconds);

// The wait for a PIN has run its time. The timer is built as the resurrect
// timer is: a copy of the device id, freed when it fires, when the wait ends
// and with the device
static void
pin_wait_cb(int fd, short what, void *arg)
{
  uint64_t *id = arg;
  struct output_device *device;

  device = outputs_device_get(*id);
  if (!device)
    return;

  event_free(device->pin_wait_ev);
  device->pin_wait_ev = NULL;
  free(id);

  if (!device->pin_pending)
    return;

  device_request(device, OUTPUT_REQ_PIN_EXPIRED, false, false);
  if (device->pin_pending)
    {
      // An operation is in flight, whose report decides
      pin_wait_arm(device, PLAYER_PIN_WAIT_RECHECK_TIME);
      return;
    }

  DPRINTF(E_INFO, L_PLAYER, "'%s' no longer waits for a PIN: none was given in %d s\n", device->name, PLAYER_PIN_WAIT_TIME);
  status_update(player_state, LISTENER_SPEAKER);
}

static void
pin_wait_arm(struct output_device *device, int seconds)
{
  struct timeval tv = { seconds, 0 };
  uint64_t *id;

  if (!device->pin_wait_ev)
    {
      CHECK_NULL(L_PLAYER, id = malloc(sizeof(*id)));
      *id = device->id;
      CHECK_NULL(L_PLAYER, device->pin_wait_ev = evtimer_new(evbase_player, pin_wait_cb, id));
    }

  evtimer_add(device->pin_wait_ev, &tv);
}

static void
pin_wait_cancel(struct output_device *device)
{
  if (!device->pin_wait_ev)
    return;

  free(event_get_callback_arg(device->pin_wait_ev));
  event_free(device->pin_wait_ev);
  device->pin_wait_ev = NULL;
}

// The handlers below hold what a backend report means to the player. Every
// callback given to outputs leads to device_report(), which moves the device's
// phase by the phase rule and then calls the handler for what reported: the
// operation that ended, or the level, flush or session the report came from.
// A handler takes the value the command holds so far (0 when there is none)
// and returns the value it stands for.

// A report while the device streams, or while it is idle with a session: a
// failure is recovered from where it can be, streaming starts the members of a
// TV proxy group that wait for it
static int
handle_streaming(struct output_device *device, enum output_device_state status, int retval)
{
  if (!device)
    {
      DPRINTF(E_LOG, L_PLAYER, "Output device disappeared during streaming!\n");
    }
  else if (status == OUTPUT_STATE_FAILED)
    {
      struct output_device *leader = NULL;

      DPRINTF(E_WARN, L_PLAYER, "The %s device '%s' failed\n", device->type_name, device->name);

      // The device can fail outside of playback, e.g. if it disconnects after a
      // flush command
      if (player_state != PLAY_PLAYING)
	goto out;

      // For live pipe sources keep playing even with no outputs, so that a
      // client enabling an output gets a real stream start instead of a probe.
      if (outputs_sessions_count() == 0 && pb_session.playing_now->data_kind != DATA_KIND_PIPE)
	pb_suspend();

      if (outputs_device_is_tv_proxy_follower(device))
	leader = tv_proxy_leader_of(device);

      // A follower that fails while none of its siblings ever came up either
      // leaves the leader with no follower to wait for (see
      // outputs_device_start()); fall back to starting the Apple TV directly
      // rather than stalling the group indefinitely. The same sibling scan
      // also covers the case where the leader's own session is already up
      // but audio-suppressed (its followers render the audio): if every
      // follower is now gone, that session must be replaced with a real,
      // direct one instead of silently doing nothing.
      if (leader && leader->selected)
	{
	  struct output_device *cur;
	  const char *group_id = outputs_device_group_id(device);
	  bool sibling_up = false;

	  for (cur = outputs_list(); group_id && cur; cur = cur->next)
	    {
	      const char *cur_gid;

	      if (cur == device || !outputs_device_is_tv_proxy_follower(cur))
		continue;

	      cur_gid = outputs_device_group_id(cur);
	      if (cur_gid && strcmp(cur_gid, group_id) == 0 && cur->session)
		{
		  sibling_up = true;
		  break;
		}
	    }

	  if (!leader->session && !sibling_up)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "TV proxy: followers of '%s' are unavailable, starting the Apple TV directly\n",
	              outputs_device_display_name(leader));

	      leader->tv_proxy_start_direct = 1;
	      device_start(leader, false, false);
	    }
	  else if (leader->session && !sibling_up && leader->tv_proxy_audio_suppress)
	    {
	      DPRINTF(E_LOG, L_PLAYER, "TV proxy: followers of '%s' are gone, restarting the Apple TV directly\n",
	              outputs_device_display_name(leader));

	      // The control-only session is stopped, and the start that follows
	      // the stop is a direct one
	      leader->tv_proxy_start_direct = 1;
	      leader->restart_after_stop = 1;
	      outputs_device_stop(leader, device_op_cb);
	    }
	}

      // A TV proxy group leader that drops its session is reconnected
      // directly, bounded by PLAYER_TV_PROXY_RECONNECT_MAX; the counter is
      // reset below once the leader streams, so the retries are bounded per
      // outage.
      if (outputs_device_is_tv_proxy_leader(device) && device->selected
          && device->tv_proxy_reconnects < PLAYER_TV_PROXY_RECONNECT_MAX)
	{
	  device->tv_proxy_reconnects++;
	  DPRINTF(E_LOG, L_PLAYER, "TV proxy: Apple TV '%s' dropped its session, reconnecting in %d sec (attempt %u of %d)\n",
	          outputs_device_display_name(device), PLAYER_TV_PROXY_RECONNECT_TIME, device->tv_proxy_reconnects, PLAYER_TV_PROXY_RECONNECT_MAX);
	  resurrect_arm(device, PLAYER_TV_PROXY_RECONNECT_TIME);
	  goto out;
	}

      if (!device->resurrect)
	goto out;

      DPRINTF(E_LOG, L_PLAYER, "Attempting reconnection in %d sec to the %s device '%s'\n", PLAYER_SPEAKER_RESURRECT_TIME, device->type_name, device->name);

      resurrect_arm(device, PLAYER_SPEAKER_RESURRECT_TIME);
    }
  else if (status == OUTPUT_STATE_STOPPED)
    {
      DPRINTF(E_INFO, L_PLAYER, "The %s device '%s' stopped\n", device->type_name, device->name);
    }
  else
    {
      DPRINTF(E_DBG, L_PLAYER, "Callback from %s device %s to device_streaming_cb (status %d)\n", device->type_name, device->name, status);

      if (status == OUTPUT_STATE_STREAMING)
	{
	  // Audio is flowing, so the leader's reconnect budget starts afresh
	  device->tv_proxy_reconnects = 0;
	  tv_proxy_followers_start(device);
	  tv_proxy_leader_start(device);
	}
    }

 out:
  return retval;
}

// The answer to a level sent by a volume command
static int
handle_volume(struct output_device *device, enum output_device_state status, int retval)
{
  if (status == OUTPUT_STATE_FAILED)
    DPRINTF(E_LOG, L_PLAYER, "The %s device '%s' failed during execution of volume command\n", device->type_name, device->name);
  else
    DPRINTF(E_DBG, L_PLAYER, "Callback from %s device %s to device_volume_cb (status %d)\n", device->type_name, device->name, status);

  // If a failure occurred when setting the volume, and we also don't have other
  // active sessions, then we suspend playback (but not for live pipe sources,
  // and not when nothing plays)
  if (outputs_sessions_count() == 0 && pb_session.playing_now && pb_session.playing_now->data_kind != DATA_KIND_PIPE)
    pb_suspend();

  return retval;
}

// Answer to the level a mute or a hold put on the wire (0, or the device's own
// level when the silence ends). Not part of a command; a failure is the
// streaming handler's, as it would have been had the send not been made
static int
handle_level_edge(struct output_device *device, enum output_device_state status, int retval)
{
  if (status == OUTPUT_STATE_FAILED || status == OUTPUT_STATE_STOPPED)
    {
      DPRINTF(E_INFO, L_PLAYER, "Level edge: send to '%s' ended with state %d\n", device->name, status);
      return handle_streaming(device, status, retval);
    }

  DPRINTF(E_DBG, L_PLAYER, "Level edge: '%s' accepted %d\n", device->name, device->volume_sent);
  return retval;
}

// A flushed device's answer. The command, if any, ends when the last device it
// flushed has reported, whatever the report, see flush_done_cb()
static int
handle_flush(struct output_device *device, enum output_device_state status, int retval)
{
  if (status == OUTPUT_STATE_FAILED)
    {
      DPRINTF(E_LOG, L_PLAYER, "The %s device '%s' failed during execution of flush command\n", device->type_name, device->name);
      return retval;
    }

  DPRINTF(E_DBG, L_PLAYER, "Callback from %s device %s to device_flush_cb (status %d)\n", device->type_name, device->name, status);

  // Used by pb_suspend - is basically the bottom half
  if (player_flush_pending > 0)
    {
      player_flush_pending--;
      if (player_flush_pending == 0)
	input_buffer_full_cb(player_playback_start);
    }

  outputs_device_stop_delayed(device, device_streaming_cb);

  return retval;
}

static void
device_to_speaker_info(struct player_speaker_info *spk, struct output_device *device, uint32_t index);

// Clients are told of a change to a device: its phase, its selection, its
// PIN state, its hold or its mute. The event carries the device's row as GET
// /api/outputs would give it at this instant, built here on the player
// thread, so an event and a read in the same turn agree; the moved device's
// own phase and reason go with it
static void
speaker_announce(struct output_device *device)
{
  struct player_speaker_info info;
  struct evchan_output_state st;
  struct output_device *row;

  if (evchan_enabled())
    {
      row = outputs_device_row(device);
      device_to_speaker_info(&info, row, 0);

      st.state = output_phase_name(info.phase);
      st.reason = output_failure_name(info.last_failure);
      st.member_state = output_phase_name(device->phase);
      st.member_reason = output_failure_name(device->last_failure);
      st.selected = info.selected;
      st.held = info.held;
      st.holdable = info.holdable;
      st.muted = info.muted;
      st.volume = info.absvol;
      st.pin_pending = info.pin_pending;

      evchan_event_output_state(row->id, device->id, info.name, &st);
    }

  status_update(player_state, LISTENER_SPEAKER | LISTENER_VOLUME);
}

// A device's phase changed: clients are told, once per change
static void
phase_announce_cb(struct output_device *device)
{
  speaker_announce(device);
}

// The last device a playback flush waited for has reported
static void
flush_done_cb(void)
{
  if (!player_flush_command)
    return;

  player_flush_command = false;
  commands_exec_end(cmdbase, 0);
}

// Makes a playback stop, pause or flush command wait for the devices still
// being flushed. Returns the command state for the caller to return
static enum command_state
flush_command_wait(int *retval)
{
  if (outputs_flush_waiting() == 0)
    {
      *retval = 0;
      return COMMAND_END;
    }

  // One event ends the command: the flush set emptying
  player_flush_command = true;
  *retval = 1;
  return COMMAND_PENDING;
}

// A stop that ended. A start the device was asked for meanwhile, or that a
// backend-changing mode switch wants, follows by the phase rule
static int
handle_shutdown(struct output_device *device, enum output_device_state status, int retval)
{
  DPRINTF(E_DBG, L_PLAYER, "Callback from %s device %s to device_shutdown_cb (status %d)\n", device->type_name, device->name, status);

  return retval;
}

// Starts the hidden followers of a TV proxy group once the leader (the Apple
// TV) has cleared pairing. Called from device_activate_cb() on any successful
// status of the leader (a probe completes with a non-failure status once
// pairing has succeeded), and again from device_streaming_cb() when the
// leader reports streaming, which restarts any follower that was lost in
// between. A follower that fails to start is logged and skipped - it must not
// affect the leader's session or the pending command. If no follower could be
// started at all, the leader's real session is started directly instead of
// leaving it waiting for one to stream (see outputs_device_start()). If the
// leader is probed before the player is playing, the group is marked
// deferred and started later from playback_start_bh() instead.
static void
tv_proxy_followers_start(struct output_device *leader)
{
  struct output_device *cur;
  const char *group_id;
  int ret;
  int up = 0;

  if (!outputs_device_is_tv_proxy_leader(leader))
    return;
  if (player_state != PLAY_PLAYING)
    {
      // The leader was probed before playback reached PLAY_PLAYING (the
      // probe callback runs before playback_start_bh). Record it so the
      // followers are started from the bottom half once the player is
      // playing, instead of being lost until a later enable.
      leader->tv_proxy_followers_deferred = 1;
      DPRINTF(E_DBG, L_PLAYER, "TV proxy: '%s' probed while not playing, followers deferred\n",
              outputs_device_display_name(leader));
      return;
    }

  leader->tv_proxy_followers_deferred = 0;

  group_id = outputs_device_group_id(leader);
  if (!group_id)
    return;

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid;

      if (cur == leader)
	continue;

      cur_gid = outputs_device_group_id(cur);
      if (!cur_gid || strcmp(cur_gid, group_id) != 0)
	continue;

      if (!outputs_device_is_tv_proxy_follower(cur))
	continue;
      if (!cur->selected)
	continue;
      if (cur->session)
	{
	  up++;
	  continue;
	}

      DPRINTF(E_INFO, L_PLAYER, "TV proxy: Apple TV '%s' up, starting follower '%s'\n",
              outputs_device_display_name(leader), outputs_device_display_name(cur));

      ret = device_start(cur, false, false);
      if (ret < 0)
	{
	  DPRINTF(E_WARN, L_PLAYER, "TV proxy: could not start follower '%s'\n", outputs_device_display_name(cur));
	  continue;
	}

      up++;
    }

  if (up > 0)
    return;

  DPRINTF(E_LOG, L_PLAYER, "TV proxy: no follower of '%s' could be started, starting the Apple TV directly\n",
          outputs_device_display_name(leader));

  if (!leader->session)
    {
      leader->tv_proxy_start_direct = 1;
      ret = device_start(leader, false, false);
      if (ret < 0)
	DPRINTF(E_WARN, L_PLAYER, "TV proxy: could not start Apple TV '%s' directly\n", outputs_device_display_name(leader));
    }
}

// Starts the deferred followers of every selected TV proxy leader whose probe
// completed while the player was not yet playing (see tv_proxy_followers_start).
// Called from playback_start_bh() once PLAY_PLAYING has been set, so a cold
// start completes on its own without waiting for a later enable. Starting a
// follower that already has a session is a no-op, and tv_proxy_followers_start()
// clears the deferred flag when it runs, so this cannot double-start.
static void
tv_proxy_groups_resume(void)
{
  struct output_device *device;

  for (device = outputs_list(); device; device = device->next)
    {
      if (!outputs_device_is_tv_proxy_leader(device))
	continue;
      if (!device->selected || !device->tv_proxy_followers_deferred)
	continue;

      DPRINTF(E_INFO, L_PLAYER, "TV proxy: playback started, starting the followers of '%s'\n",
              outputs_device_display_name(device));
      tv_proxy_followers_start(device);
    }
}

// Finds the leader of the TV proxy group that follower belongs to. Returns
// NULL if follower is not (or no longer) a TV proxy follower, or the group
// has no recognised leader.
static struct output_device *
tv_proxy_leader_of(struct output_device *follower)
{
  struct output_device *cur;
  const char *group_id;

  if (!outputs_device_is_tv_proxy_follower(follower))
    return NULL;

  group_id = outputs_device_group_id(follower);
  if (!group_id)
    return NULL;

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid;

      if (cur == follower || !outputs_device_is_tv_proxy_leader(cur))
	continue;

      cur_gid = outputs_device_group_id(cur);
      if (cur_gid && strcmp(cur_gid, group_id) == 0)
	return cur;
    }

  return NULL;
}

// Starts the Apple TV leading a TV proxy group once one of its HomePod
// followers is confirmed streaming. This is the normal way the leader's real
// session gets started - it is left merely probed until this point (see
// outputs_device_start()), because an Apple TV session that already exists
// when the HomePods connect is closed by the Apple TV a moment later.
static void
tv_proxy_leader_start(struct output_device *follower)
{
  struct output_device *leader;
  int ret;

  if (!outputs_device_is_tv_proxy_follower(follower))
    return;
  if (player_state != PLAY_PLAYING)
    return;

  leader = tv_proxy_leader_of(follower);
  if (!leader)
    return;
  if (!leader->selected)
    return;
  if (leader->session)
    return;

  DPRINTF(E_INFO, L_PLAYER, "TV proxy: follower '%s' streaming, starting Apple TV '%s'\n",
          outputs_device_display_name(follower), outputs_device_display_name(leader));

  ret = device_start(leader, false, false);
  if (ret < 0)
    DPRINTF(E_WARN, L_PLAYER, "TV proxy: could not start Apple TV '%s'\n", outputs_device_display_name(leader));
}

// A member of a held group that comes back (its session was set up again)
// plays, so the members still held are released, or the row would read not
// held while some of it is, and could not be resumed. Run as a command so it
// never replaces the callback of a device another command is waiting on.
static enum command_state
speaker_member_returned(void *arg, int *retval)
{
  uint64_t *id = arg;
  struct output_device *device;
  struct output_device *cur;
  const char *group_id;
  int ended = 0;

  *retval = 0;

  device = outputs_device_get(*id);
  if (!device || !device->session)
    return COMMAND_END;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    return COMMAND_END;

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (cur == device || !cur_gid || strcmp(cur_gid, group_id) != 0)
	continue;

      if (!cur->session || cur->hold_kind == OUTPUT_HOLD_NONE)
	continue;

      if (outputs_device_hold_set(cur, false, "a member came back", device_level_edge_cb) == 0)
	{
	  speaker_announce(cur);
	  ended++;
	}
    }

  if (ended)
    {
      DPRINTF(E_INFO, L_PLAYER, "'%s' came back while its group was held: hold ended for %d speaker(s)\n",
	      outputs_device_display_name(device), ended);
      // An async command has no bottom half
      status_update(player_state, LISTENER_SPEAKER);
    }

  return COMMAND_END;
}

// A start or a probe that ended. The phase rule has already deselected a
// failed start and recorded a PIN the receiver asks for. Returns -2 for a PIN
// asked for, -1 for a failure, else retval
static int
handle_activate(struct output_device *device, enum output_device_state status, int retval)
{
  DPRINTF(E_DBG, L_PLAYER, "Callback from %s device %s to device_activate_cb (status %d)\n", device->type_name, device->name, status);

  if (status == OUTPUT_STATE_PASSWORD)
    {
      DPRINTF(E_LOG, L_PLAYER, "The %s device '%s' requires a valid PIN or password\n", device->type_name, device->name);

      device->tv_proxy_followers_deferred = 0;
      return -2;
    }

  if (status == OUTPUT_STATE_FAILED)
    {
      DPRINTF(E_LOG, L_PLAYER, "The %s device '%s' failed to activate (%s)\n", device->type_name, device->name,
	      output_failure_name(device->last_failure));

      device->tv_proxy_followers_deferred = 0;
      return -1;
    }

  // A device deselected while it started is being stopped: nothing waits for
  // it
  if (!device->selected)
    return retval;

  // Any status reaching here other than PASSWORD/FAILED means the leader
  // cleared pairing - a probe completes with a startup-range status, a real
  // start with connected (or streaming) - so its hidden HomePod followers can
  // be brought online. This is a no-op unless device is a TV proxy group
  // leader. On a cold start the probe callback runs before the player reaches
  // PLAY_PLAYING, so tv_proxy_followers_start() records the group as deferred
  // and it is completed from playback_start_bh(); when the player is already
  // playing the followers start here directly.
  tv_proxy_followers_start(device);

  // A start that ended streaming does what streaming does
  if (status == OUTPUT_STATE_STREAMING)
    retval = handle_streaming(device, status, retval);

  // A member back in a held group: its siblings' hold is ended once no
  // command is waiting (see speaker_member_returned())
  if (device->session && outputs_device_group_id(device) && speaker_group_sibling_held(device))
    {
      uint64_t *id;

      CHECK_NULL(L_PLAYER, id = malloc(sizeof(*id)));
      *id = device->id;

      // On success the command frees id
      if (commands_exec_async(cmdbase, speaker_member_returned, id) < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Could not queue the end of the hold of the group of '%s'\n", device->name);
	  free(id);
	}
    }

  return retval;
}

// An authorise that ended: the PIN paired, or the receiver refused it (or the
// pairing timed out). What follows is the phase rule's
static int
handle_authorize(struct output_device *device, enum output_device_state status, int retval)
{
  if (status == OUTPUT_STATE_PASSWORD || status == OUTPUT_STATE_FAILED)
    DPRINTF(E_LOG, L_PLAYER, "The PIN for '%s' did not pair it (%s)\n", device->name, output_failure_name(device->last_failure));
  else
    DPRINTF(E_INFO, L_PLAYER, "'%s' paired with the PIN\n", device->name);

  return retval;
}

static int
handle_volume_echo(struct output_device *device, enum output_device_state status, int retval);
static void
speaker_enable_apply(struct output_device *device, int volume);


/* ---------------- The one way a speaker report is handled ----------------- */

// Every operation on a speaker (a start, a probe, a stop, an authorise) and
// every level, flush or session report reaches device_report(). It moves the
// device's phase and flags by the phase rule (output_phase.c), runs the
// handler for what reported, begins the operation the rule asks for, and has
// clients told of each change. No speaker command waits for a report.

// Where a report arrived: the callback of the operation in flight, or one of
// the callbacks a device with no operation reports to
enum report_kind
{
  REPORT_OP,
  REPORT_STREAMING,
  REPORT_VOLUME,
  REPORT_FLUSH,
  REPORT_LEVEL_EDGE,
  REPORT_VOLUME_ECHO,
};

static const char *
op_name(enum output_op op)
{
  switch (op)
    {
      case OUTPUT_OP_PROBE:     return "probe";
      case OUTPUT_OP_START:     return "start";
      case OUTPUT_OP_STOP:      return "stop";
      case OUTPUT_OP_AUTHORIZE: return "authorise";
      default:                  return "none";
    }
}

static enum output_report
report_of(enum output_device_state status)
{
  switch (status)
    {
      case OUTPUT_STATE_STARTUP:   return OUTPUT_REPORT_STARTUP;
      case OUTPUT_STATE_CONNECTED: return OUTPUT_REPORT_CONNECTED;
      case OUTPUT_STATE_STREAMING: return OUTPUT_REPORT_STREAMING;
      case OUTPUT_STATE_FAILED:    return OUTPUT_REPORT_FAILED;
      case OUTPUT_STATE_PASSWORD:  return OUTPUT_REPORT_PASSWORD;
      default:                     return OUTPUT_REPORT_STOPPED;
    }
}

// The facts the phase rule reads, from the device as it is now
static void
phase_input_fill(struct output_phase_input *in, struct output_device *device)
{
  memset(in, 0, sizeof(*in));
  in->phase = device->phase;
  in->op = device->op;
  in->selected = device->selected;
  in->playing = (player_state == PLAY_PLAYING);
  in->has_session = (device->session != NULL);
  in->stop_after_start = device->stop_after_start;
  in->restart_after_stop = device->restart_after_stop;
  in->upgrade_after_probe = device->upgrade_after_probe;
  in->then_select = device->then_select;
  in->pin_pending = device->pin_pending;
}

// Whether a start the phase rule asks for after a report is left to another
// path: an Apple TV left out of its group gets no session; a TV proxy
// follower is started by its leader's chain, unless the leader is selected and
// waits for no PIN; a leader whose followers are not streaming is started by
// them once one streams, unless the start follows its own stop (a restart).
// ended is the operation whose end asks for the start
static bool
start_left_to_others(struct output_device *device, enum output_op ended)
{
  struct output_device *leader;

  if (outputs_device_tv_proxy_excluded(device) || device->tv_proxy_followers_deferred)
    return true;

  if (outputs_device_is_tv_proxy_follower(device))
    {
      leader = tv_proxy_leader_of(device);
      return !leader || !leader->selected || leader->pin_pending;
    }

  if (outputs_device_is_tv_proxy_leader(device) && ended != OUTPUT_OP_STOP && !device->tv_proxy_start_direct
      && !outputs_device_tv_proxy_followers_streaming(device))
    return true;

  return false;
}

// The flags, the reason and the selection a phase rule result asks for
static void
flags_apply(struct output_device *device, const struct output_phase_result *res)
{
  if (res->set_stop_after_start && !device->stop_after_start)
    DPRINTF(E_INFO, L_PLAYER, "'%s' will stop once its start ends\n", device->name);
  if (res->set_restart_after_stop && !device->restart_after_stop)
    DPRINTF(E_INFO, L_PLAYER, "'%s' will start again once it has stopped\n", device->name);

  if (res->set_stop_after_start)
    device->stop_after_start = 1;
  if (res->clear_stop_after_start)
    device->stop_after_start = 0;
  if (res->set_restart_after_stop)
    device->restart_after_stop = 1;
  if (res->clear_restart_after_stop)
    device->restart_after_stop = 0;
  if (res->set_upgrade_after_probe)
    device->upgrade_after_probe = 1;
  if (res->clear_upgrade_after_probe)
    device->upgrade_after_probe = 0;
  // Each PIN asked for gets the whole wait
  if (res->set_pin_pending)
    {
      device->pin_pending = 1;
      pin_wait_arm(device, PLAYER_PIN_WAIT_TIME);
    }
  if (res->clear_pin_pending)
    {
      device->pin_pending = 0;
      pin_wait_cancel(device);
    }
  if (res->clear_then_select)
    device->then_select = 0;

  if (res->reason != OUTPUT_FAILURE_NONE)
    device->last_failure = res->reason;

  if (res->deselect && device->selected)
    outputs_device_deselect(device);
}

static void
operation_failed_at_once(struct output_device *device, enum output_op op);

// Begins the operation the phase rule asked for
static void
operation_start(struct output_device *device, enum output_op op, bool explicit_request)
{
  switch (op)
    {
      case OUTPUT_OP_START:
      case OUTPUT_OP_PROBE:
	device_start(device, op == OUTPUT_OP_PROBE, explicit_request);
	break;
      case OUTPUT_OP_STOP:
	outputs_device_stop(device, device_op_cb);
	break;
      default:
	break;
    }
}

// Sets the phase the event reached, then begins the next operation, which
// sets its own phase. Each phase change is announced as it is made; a change
// of selection or PIN state with no phase change is announced here
static void
result_phases_apply(struct output_device *device, const struct output_phase_result *res, enum output_op next, bool explicit_request)
{
  enum output_phase reached = res->mid_phase;
  bool announced;

  // A request can move the phase with no operation (a deselect while
  // connecting, a select that takes it back); with none begun the phase is the
  // one the event reached
  if (next == OUTPUT_OP_NONE && res->next_op == OUTPUT_OP_NONE)
    reached = res->phase;

  announced = outputs_device_phase_set(device, reached);

  if (next != OUTPUT_OP_NONE)
    {
      operation_start(device, next, explicit_request);
      announced = announced || (device->phase != reached);
    }

  if (res->announce && !announced)
    speaker_announce(device);
}

// Applies one report to a device: in is the rule's input, filled from the
// device and the report's slot
static void
report_apply(struct output_device *device, enum output_device_state status, struct output_phase_input *in, enum report_kind kind)
{
  struct output_phase_result res;
  enum output_op op = in->op;
  enum output_op next;
  bool current = in->report_current_gen;
  bool ended;
  bool then_select;
  int start_volume;

  in->report_reason = (status == OUTPUT_STATE_FAILED || status == OUTPUT_STATE_PASSWORD) ? device->last_failure : OUTPUT_FAILURE_NONE;
  output_phase_on_report(in, report_of(status), &res);

  DPRINTF(E_DBG, L_PLAYER, "'%s' %s -> %s (op %s, gen %u%s)\n", device->name, output_phase_name(in->phase), output_phase_name(res.phase),
	  op_name(op), device->op_gen, current ? "" : ", older operation");
  if (!current)
    DPRINTF(E_DBG, L_PLAYER, "Report %d from an older operation of '%s' moves nothing\n", status, device->name);

  ended = current && res.clear_op && (op != OUTPUT_OP_NONE);

  // The select that came with the PIN: the start the rule asks for after the
  // authorise is that select, of the device and its group
  then_select = ended && (op == OUTPUT_OP_AUTHORIZE) && in->then_select
                && (res.next_op == OUTPUT_OP_START || res.next_op == OUTPUT_OP_PROBE);
  start_volume = device->start_volume;

  flags_apply(device, &res);
  if (res.clear_op)
    device->op = OUTPUT_OP_NONE;

  next = res.next_op;
  if (then_select)
    next = OUTPUT_OP_NONE;
  else if ((next == OUTPUT_OP_START || next == OUTPUT_OP_PROBE) && start_left_to_others(device, ended ? op : OUTPUT_OP_NONE))
    {
      DPRINTF(E_DBG, L_PLAYER, "'%s' is not started after its report: it is started another way\n", device->name);
      next = OUTPUT_OP_NONE;
    }

  result_phases_apply(device, &res, next, false);

  if (then_select)
    {
      DPRINTF(E_INFO, L_PLAYER, "Starting '%s' as the PIN request asked\n", device->name);
      device->start_volume = -1;
      speaker_enable_apply(device, start_volume);
    }

  // The session's next report reaches the session's callback; a report that
  // was one step of an operation gives the operation a new slot
  if (device->session && device->op == OUTPUT_OP_NONE)
    outputs_device_cb_set(device, device_streaming_cb);
  else if (current && kind == REPORT_OP && device->session && device->op == op && !ended)
    outputs_device_op_cb_set(device, device_op_cb);

  if (ended)
    {
      switch (op)
	{
	  case OUTPUT_OP_START:
	  case OUTPUT_OP_PROBE:
	    handle_activate(device, status, 0);
	    break;
	  case OUTPUT_OP_STOP:
	    handle_shutdown(device, status, 0);
	    break;
	  case OUTPUT_OP_AUTHORIZE:
	    handle_authorize(device, status, 0);
	    break;
	  default:
	    break;
	}
    }
  else if (current || kind == REPORT_FLUSH)
    {
      switch (kind)
	{
	  case REPORT_STREAMING:
	    handle_streaming(device, status, 0);
	    break;
	  case REPORT_VOLUME:
	    handle_volume(device, status, 0);
	    break;
	  case REPORT_FLUSH:
	    handle_flush(device, status, 0);
	    break;
	  case REPORT_LEVEL_EDGE:
	    handle_level_edge(device, status, 0);
	    break;
	  case REPORT_VOLUME_ECHO:
	    handle_volume_echo(device, status, 0);
	    break;
	  case REPORT_OP:
	    break;
	}
    }

  // A start that ends after playback stopped or paused missed that flush; it
  // is flushed now, and its delayed stop follows the flush's answer
  if (ended && (op == OUTPUT_OP_START || op == OUTPUT_OP_PROBE) && device->session && device->op == OUTPUT_OP_NONE
      && player_state != PLAY_PLAYING)
    {
      DPRINTF(E_INFO, L_PLAYER, "'%s' started after playback stopped, flushing it\n", device->name);
      outputs_device_flush(device, device_flush_cb);
    }
}

static void
device_report(struct output_device *device, enum output_device_state status, enum report_kind kind)
{
  struct output_phase_input in;

  if (!device)
    {
      DPRINTF(E_WARN, L_PLAYER, "Report for device %" PRIu64 " which has gone\n", outputs_report_device_id());
      status_update(player_state, LISTENER_SPEAKER | LISTENER_VOLUME);
      return;
    }

  phase_input_fill(&in, device);
  in.report_current_gen = outputs_report_current_gen();
  report_apply(device, status, &in, kind);

  status_update(player_state, LISTENER_SPEAKER | LISTENER_VOLUME);
}

// A start or an authorise the backend refused at once ends as if it had
// reported a failure
static void
operation_failed_at_once(struct output_device *device, enum output_op op)
{
  struct output_phase_input in;

  if (device->last_failure == OUTPUT_FAILURE_NONE)
    device->last_failure = OUTPUT_FAILURE_UNREACHABLE;

  DPRINTF(E_WARN, L_PLAYER, "Could not begin the %s of '%s' (%s)\n", op_name(op), device->name, output_failure_name(device->last_failure));

  phase_input_fill(&in, device);
  in.phase = OUTPUT_PHASE_CONNECTING;
  in.op = op;
  in.report_current_gen = true;
  report_apply(device, OUTPUT_STATE_FAILED, &in, REPORT_OP);
}

// Starts or probes the device with the operation callback. A start the user
// asked for is explicit, which is what lets a device waiting for a PIN start
static int
device_start(struct output_device *device, bool only_probe, bool explicit_request)
{
  int ret;

  ret = outputs_device_start(device, device_op_cb, only_probe, explicit_request);
  if (ret < 0)
    operation_failed_at_once(device, only_probe ? OUTPUT_OP_PROBE : OUTPUT_OP_START);

  return ret;
}

// A request for the device through the phase rule: its flags, its phases and
// the operation the rule begins. The caller has selected or deselected it
static void
device_request(struct output_device *device, enum output_request req, bool explicit_request, bool backend_changes)
{
  struct output_phase_input in;
  struct output_phase_result res;

  phase_input_fill(&in, device);
  in.explicit_request = explicit_request;
  in.backend_changes = backend_changes;
  output_phase_on_request(&in, req, &res);

  if (req == OUTPUT_REQ_SELECT && device->pin_pending && !explicit_request)
    DPRINTF(E_INFO, L_PLAYER, "Not starting '%s': it waits for a PIN\n", device->name);
  else if (res.clear_pin_pending && req != OUTPUT_REQ_PIN_EXPIRED)
    DPRINTF(E_INFO, L_PLAYER, "The PIN '%s' asked for is stale: the user asked for it again\n", device->name);

  // A device no longer wanted is not brought back
  if (req == OUTPUT_REQ_DESELECT && !device->selected)
    resurrect_cancel(device);

  flags_apply(device, &res);
  result_phases_apply(device, &res, res.next_op, explicit_request);
}

static void
device_op_cb(struct output_device *device, enum output_device_state status)
{
  device_report(device, status, REPORT_OP);
}

static void
device_streaming_cb(struct output_device *device, enum output_device_state status)
{
  device_report(device, status, REPORT_STREAMING);
}

static void
device_volume_cb(struct output_device *device, enum output_device_state status)
{
  device_report(device, status, REPORT_VOLUME);
}

static void
device_flush_cb(struct output_device *device, enum output_device_state status)
{
  device_report(device, status, REPORT_FLUSH);
}

static void
device_level_edge_cb(struct output_device *device, enum output_device_state status)
{
  device_report(device, status, REPORT_LEVEL_EDGE);
}

static void
device_volume_echo_cb(struct output_device *device, enum output_device_state status)
{
  device_report(device, status, REPORT_VOLUME_ECHO);
}

/* ------------------------- Internal playback routines --------------------- */

static int
pb_timer_start(void)
{
  struct itimerspec tick;
  int ret;

  // The stop timers will be active if we have recently paused, but now that the
  // playback loop has been kicked off, we deactivate them
  outputs_stop_delayed_cancel();

  ret = event_add(pb_timer_ev, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not add playback timer\n");

      return -1;
    }

  tick.it_interval = player_tick_interval;
  tick.it_value = player_tick_interval;

#ifdef HAVE_TIMERFD
  ret = timerfd_settime(pb_timer_fd, 0, &tick, NULL);
#else
  ret = timer_settime(pb_timer, 0, &tick, NULL);
#endif
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not arm playback timer: %s\n", strerror(errno));

      return -1;
    }

  return 0;
}

static int
pb_timer_stop(void)
{
  struct itimerspec tick;
  int ret;

  event_del(pb_timer_ev);

  // The next tick is the first of a new run, not a late one
  pb_jitter_last_tick.tv_sec = 0;
  pb_jitter_last_tick.tv_nsec = 0;

  memset(&tick, 0, sizeof(struct itimerspec));

#ifdef HAVE_TIMERFD
  ret = timerfd_settime(pb_timer_fd, 0, &tick, NULL);
#else
  ret = timer_settime(pb_timer, 0, &tick, NULL);
#endif
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not disarm playback timer: %s\n", strerror(errno));
      return -1;
    }

  return 0;
}

// Initiates the session and starts the input source
static int
pb_session_start(struct db_queue_item *queue_item, uint32_t seek_ms)
{
  struct player_source *ps;
  uint32_t item_id;
  int ret;

  ps = source_create(queue_item, seek_ms);

  // Clears the session and attaches the new source as pb_session.source_list
  session_start(ps);

  // Sets of opening of the new source
  while ( (ret = source_start(ps)) < 0)
    {
      // Couldn't start requested item, skip to next and remove failed item from queue
      item_id = ps->item_id;
      ps = source_next_create(ps);

      // Will free pb_session.source_list so we don't memleak failed sources
      session_start(ps);
      db_queue_delete_byitemid(item_id);
    }

  session_update_read_start((uint32_t)ret);

  if (!pb_session.playing_now)
    return -1;

  return ret;
}

// Stops input source and stops read loop
static void
pb_session_pause(void)
{
  pb_timer_stop();

  source_stop();
}

// Stops input source and deallocates pb_session content
static void
pb_session_stop(void)
{
  pb_timer_stop();

  source_stop();

  session_stop();

  status_update(PLAY_STOPPED, LISTENER_PLAYER);
}

static void
pb_abort(void)
{
  // Immediate stop of all outputs
  outputs_stop(device_op_cb);
  outputs_metadata_purge();

  pb_session_stop();

  if (!clear_queue_on_stop_disabled)
    db_queue_clear(0);
}

// Restarts the input (in case it was closed during the pause), resets session
// start timestamp and deficits, which is necessary after pb_suspend.
static int
pb_resume(void)
{
  struct player_source *ps;
  int ret;

  // Before pb_resume() is called it is important that source_list is set to
  // have just one item, and that both reading_now and playing_now point to it.
  // Otherwise it would mean that the input is currently reading an item that is
  // not being played, which means asking the input to resume playing_now
  // will bring us into a situation where the order of data read by input_read()
  // from the data input_buffer will not the match the order of the source_list.
  ps = pb_session.source_list;
  if (!ps || ps != pb_session.playing_now)
    {
      DPRINTF(E_LOG, L_PLAYER, "Bug! pb_resume() called, but source list is invalid (%p, %p)\n", ps, pb_session.playing_now);
      pb_abort();
      return -1;
    }

  // In many cases the input will already be open, so this only has effect if
  // we are resuming after pause longer than INPUT_OPEN_TIMEOUT, or if the input
  // lost connection during the pause
  ret = source_restart(ps);
  if (ret < 0)
    {
      pb_abort();
      return -1;
    }

  session_restart();

  return 0;
}

// Temporarily suspends/resets playback, used when input buffer underruns or in
// case of problems writing to the outputs.
static int
pb_suspend(void)
{
  struct db_queue_item *queue_item;
  int flush_pending;
  int ret;

  // If ->next is set then suspend was called during a track change, which is a
  // tricky time. To simplify things, we reset the entire session, which also
  // means resetting the input, but still letting it proceed with the head of
  // source_list. Ideally, we instead want to resume with playing_now, because
  // with the current solution the user will loose a bit of audio. In practice,
  // that causes issues, because sometimes pb_suspend() is called because of an
  // output delay, which was caused by e.g. changing quality of the output
  // during track change. So going back to playing_now would make that repeat.
  // playing_now is NULL once session_update_play_eof() has advanced past the
  // last source, and pb_suspend() is reachable in that window from the output
  // write-failure and underrun paths.
  if (pb_session.playing_now && pb_session.playing_now->next)
    {
      // So we restart the session with the head source, not playing_now
      queue_item = db_queue_fetch_byitemid(pb_session.source_list->item_id);
      if (!queue_item)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Error suspending playback, could not retrieve queue item currently being played\n");
	  pb_abort();
	  return -1;
	}

      ret = pb_session_start(queue_item, 0);
      free_queue_item(queue_item, 0);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Error suspending playback, could not start session\n");
	  pb_abort();
	  return -1;
	}
    }

  flush_pending = outputs_flush(device_flush_cb);

  pb_timer_stop();

  status_update(PLAY_PAUSED, LISTENER_PLAYER);

  return flush_pending;
}


/* --------------- Actual commands, executed in the player thread ----------- */

static enum command_state
get_status(void *arg, int *retval)
{
  struct player_status *status = arg;

  memset(status, 0, sizeof(struct player_status));

  status->consume = 0;

  status->volume = outputs_volume_get();

  switch (player_state)
    {
      case PLAY_STOPPED:
	DPRINTF(E_DBG, L_PLAYER, "Player status: stopped\n");

	status->status  = PLAY_STOPPED;
	break;

      case PLAY_PAUSED:
	DPRINTF(E_DBG, L_PLAYER, "Player status: paused\n");

	status->status  = PLAY_PAUSED;
	status->id      = pb_session.playing_now->id;
	status->item_id = pb_session.playing_now->item_id;

	status->pos_ms  = pb_session.playing_now->pos_ms;
	status->len_ms  = pb_session.playing_now->len_ms;

	break;

      case PLAY_PLAYING:
	if (pb_session.playing_now->play_start == 0 || pb_session.pos < pb_session.playing_now->play_start)
	  {
	    DPRINTF(E_DBG, L_PLAYER, "Player status: playing (buffering)\n");

	    status->status = PLAY_PAUSED;
	  }
	else
	  {
	    DPRINTF(E_DBG, L_PLAYER, "Player status: playing\n");

	    status->status = PLAY_PLAYING;
	  }

	status->id      = pb_session.playing_now->id;
	status->item_id = pb_session.playing_now->item_id;

	status->pos_ms  = pb_session.playing_now->pos_ms;
	status->len_ms  = pb_session.playing_now->len_ms;

	break;
    }

  *retval = 0;
  return COMMAND_END;
}

static enum command_state
playback_stop(void *arg, int *retval)
{
  if (player_state == PLAY_STOPPED)
    {
      *retval = 0;
      return COMMAND_END;
    }

  // We may be restarting very soon, so we don't bring the devices to a full
  // stop just yet; this saves time when restarting, which is nicer for the user
  outputs_flush(device_flush_cb);
  outputs_metadata_purge();

  // Stops the input
  pb_session_stop();

  status_update(PLAY_STOPPED, LISTENER_PLAYER);

  // We're async if we need to flush devices
  return flush_command_wait(retval);
}

static enum command_state
playback_abort(void *arg, int *retval)
{
  pb_abort();

  *retval = 0;
  return COMMAND_END;
}

// With nothing selected and nothing started, start the first speaker that may be
// chosen automatically
static void
outputs_autoselect(void)
{
  struct output_device *device;

  for (device = outputs_list(); device; device = device->next)
    {
      if (device->selected || outputs_priority(device) == 0 || device->session || device->op != OUTPUT_OP_NONE)
	continue;

      outputs_device_select(device, -1);
      device_request(device, OUTPUT_REQ_SELECT, false, false);
      if (device->op == OUTPUT_OP_NONE)
	{
	  outputs_device_deselect(device);
	  continue;
	}

      DPRINTF(E_LOG, L_PLAYER, "Autoselected %s device '%s'\n", device->type_name, device->name);
      break;
    }
}

// Brings the speakers in line with their selection: a deselected speaker with
// a session or an operation in flight is stopped, a selected one is started
// (joined to the stream that plays, or probed when nothing plays), each
// through the phase rule. TV proxy followers wait for their leader's chain and
// an Apple TV left out of its group gets no session. Nothing waits for them
static void
speakers_join(void)
{
  struct output_device *device;
  bool any = false;

  for (device = outputs_list(); device; device = device->next)
    {
      if (!device->selected && (device->session || device->op != OUTPUT_OP_NONE))
	device_request(device, OUTPUT_REQ_DESELECT, false, false);
    }

  for (device = outputs_list(); device; device = device->next)
    {
      if (!device->selected)
	continue;

      if (outputs_device_is_tv_proxy_follower(device) || outputs_device_tv_proxy_excluded(device))
	continue;

      device_request(device, OUTPUT_REQ_SELECT, false, false);
      if (device->session || device->op != OUTPUT_OP_NONE)
	any = true;
    }

  // autoselect also applies if no devices were selected
  if (speaker_autoselect && outputs_sessions_count() == 0 && !any)
    outputs_autoselect();
}

static enum command_state
playback_start_bh(void *arg, int *retval)
{
  int ret;

  ret = pb_timer_start();
  if (ret < 0)
    goto error;

  // We also ask listeners to update speaker/volume state, since it is possible
  // some of the speakers we tried to start responded with failure
  status_update(PLAY_PLAYING, LISTENER_PLAYER | LISTENER_SPEAKER | LISTENER_VOLUME);

  // A pipe does not wait for its speakers, so they are started now that the
  // player is playing. A speaker that is already running is left alone.
  speakers_join();

  // Complete any TV proxy group whose leader was probed before the player
  // reached PLAY_PLAYING; its followers were deferred until this point.
  tv_proxy_groups_resume();

  *retval = 0;
  return COMMAND_END;

 error:
  pb_abort();
  *retval = -1;
  return COMMAND_END;
}

static enum command_state
playback_start_item(void *arg, int *retval)
{
  struct db_queue_item *queue_item = arg;
  uint32_t seek_ms;
  int ret;

  if (player_state == PLAY_PLAYING)
    {
      DPRINTF(E_DBG, L_PLAYER, "Player is already playing, ignoring call to playback start\n");

      status_update(player_state, LISTENER_PLAYER);

      *retval = 1; // Value greater 0 will prevent execution of the bottom half function
      return COMMAND_END;
    }

  if (player_state == PLAY_STOPPED && !queue_item)
    {
      DPRINTF(E_LOG, L_PLAYER, "Failed to start/resume playback, no queue item given\n");

      *retval = -1;
      return COMMAND_END;
    }

  if (!queue_item)
    {
      ret = pb_resume();
      if (ret < 0)
	{
	  *retval = -1;
	  return COMMAND_END;
	}
    }
  else
    {
      // Start playback for given queue item
      DPRINTF(E_DBG, L_PLAYER, "Start playback of '%s' (id=%d, item-id=%d)\n", queue_item->path, queue_item->file_id, queue_item->id);

      seek_ms = 0; /* pipe has no stored seek position */

      ret = pb_session_start(queue_item, seek_ms);
      if (ret < 0)
	{
	  *retval = -1;
	  return COMMAND_END;
	}
    }

  // Every source is a live pipe, which does not wait for its speakers: it
  // plays at once and they join from the bottom half
  *retval = 0;
  return COMMAND_END;
}

static enum command_state
playback_start_id(void *arg, int *retval)
{
  struct db_queue_item *queue_item = NULL;
  enum command_state cmd_state;

  *retval = -1;

  if (player_state == PLAY_STOPPED)
    {
      /* Pipe-only build: the single queue item is always at position 0 */
      queue_item = db_queue_fetch_bypos(0, 0);
      if (!queue_item)
        return COMMAND_END;
    }

  cmd_state = playback_start_item(queue_item, retval);

  free_queue_item(queue_item, 0);

  return cmd_state;
}

static enum command_state
playback_start(void *arg, int *retval)
{
  struct db_queue_item *queue_item = NULL;
  enum command_state cmd_state;

  *retval = -1;

  if (player_state == PLAY_STOPPED)
    {
      // Start playback of first item in queue
      queue_item = db_queue_fetch_bypos(0, 0);
      if (!queue_item)
	return COMMAND_END;
    }

  cmd_state = playback_start_item(queue_item, retval);

  free_queue_item(queue_item, 0);

  return cmd_state;
}


static enum command_state
playback_pause_bh(void *arg, int *retval)
{
  struct db_queue_item *queue_item;
  int ret;

  // outputs_flush() in playback_pause() may have a caused a failure callback
  // from the output, which in streaming_cb() can cause pb_abort()
  if (player_state == PLAY_STOPPED)
    {
      goto error;
    }

  queue_item = db_queue_fetch_byitemid(pb_session.playing_now->item_id);
  if (!queue_item)
    {
      DPRINTF(E_DBG, L_PLAYER, "Error pausing source, queue item has disappeared\n");
      goto error;
    }

  ret = pb_session_start(queue_item, pb_session.playing_now->pos_ms);
  free_queue_item(queue_item, 0);
  if (ret < 0)
    {
      DPRINTF(E_DBG, L_PLAYER, "Error pausing source, aborting playback\n");
      goto error;
    }

  status_update(PLAY_PAUSED, LISTENER_PLAYER);

  *retval = 0;
  return COMMAND_END;

 error:
  pb_abort();
  *retval = -1;
  return COMMAND_END;
}

static enum command_state
playback_pause(void *arg, int *retval)
{
  if (player_state == PLAY_STOPPED)
    {
      *retval = -1;
      return COMMAND_END;
    }

  if (player_state == PLAY_PAUSED)
    {
      *retval = 0;
      return COMMAND_END;
    }

  pb_session_pause();

  outputs_flush(device_flush_cb);
  outputs_metadata_purge();

  // We're async if we need to flush devices, otherwise the bottom half just
  // runs
  return flush_command_wait(retval);
}

static enum command_state
playback_flush(void *arg, int *retval)
{
  if (player_state == PLAY_STOPPED)
    {
      *retval = -1;
      return COMMAND_END;
    }

  if (player_state == PLAY_PAUSED)
    {
      *retval = 0;
      return COMMAND_END;
    }

  input_flush(NULL);

  outputs_flush(device_flush_cb);
  outputs_metadata_purge();

  return flush_command_wait(retval);
}

// Replaces the session facts of an Apple TV's row with those of the HomePods it
// leads, reduced to one phase: streaming if any streams, else connecting if any
// is connecting or connected, else stopping, else password, else failed, else
// stopped. A PIN is pending if any of them waits for one. A password or failed
// phase carries the reason of the first of them in that phase.
static void
homepods_session_of(struct player_speaker_info *spk, struct output_device *device)
{
  struct output_device *cur;
  const char *group_id;
  struct speakers_seen seen = { 0 };
  bool pin_pending = false;
  enum output_failure_reason password_reason = OUTPUT_FAILURE_NONE;
  enum output_failure_reason failed_reason = OUTPUT_FAILURE_NONE;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    return;

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (cur == device || !cur_gid || strcmp(cur_gid, group_id) != 0)
        continue;

      if (cur->pin_pending)
        pin_pending = true;
      switch (cur->phase)
        {
          case OUTPUT_PHASE_STREAMING:
            seen.streaming = true;
            break;
          case OUTPUT_PHASE_CONNECTING:
          case OUTPUT_PHASE_CONNECTED:
            seen.starting = true;
            break;
          case OUTPUT_PHASE_STOPPING:
            seen.stopping = true;
            break;
          case OUTPUT_PHASE_PASSWORD:
            seen.password = true;
            if (password_reason == OUTPUT_FAILURE_NONE)
              password_reason = cur->last_failure;
            break;
          case OUTPUT_PHASE_FAILED:
            seen.failed = true;
            if (failed_reason == OUTPUT_FAILURE_NONE)
              failed_reason = cur->last_failure;
            break;
          case OUTPUT_PHASE_STOPPED:
            break;
        }
    }

  spk->last_failure = OUTPUT_FAILURE_NONE;

  switch (speakers_summary_resolve(seen))
    {
      case SPEAKERS_SUMMARY_STREAMING:
        spk->phase = OUTPUT_PHASE_STREAMING;
        break;
      case SPEAKERS_SUMMARY_STARTING:
        spk->phase = OUTPUT_PHASE_CONNECTING;
        break;
      case SPEAKERS_SUMMARY_STOPPING:
        spk->phase = OUTPUT_PHASE_STOPPING;
        break;
      case SPEAKERS_SUMMARY_PASSWORD:
        spk->phase = OUTPUT_PHASE_PASSWORD;
        spk->last_failure = password_reason;
        break;
      case SPEAKERS_SUMMARY_FAILED:
        spk->phase = OUTPUT_PHASE_FAILED;
        spk->last_failure = failed_reason;
        break;
      case SPEAKERS_SUMMARY_STOPPED:
        spk->phase = OUTPUT_PHASE_STOPPED;
        break;
    }

  spk->pin_pending = pin_pending;
}

// Whether the speakers a row stands for are held: every member of its group
// that has a session is held, not counting an Apple TV whose HomePods play the
// audio. A device in no group is held when it is.
static bool
speaker_group_held(struct output_device *device)
{
  struct output_device *cur;
  const char *group_id;
  int held = 0;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    return device->held;

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (!cur_gid || strcmp(cur_gid, group_id) != 0 || !cur->session || cur->tv_proxy_audio_suppress)
	continue;

      if (!cur->held)
	return false;

      held++;
    }

  return held > 0;
}

// Whether another member of the device's group is held now
static bool
speaker_group_sibling_held(struct output_device *device)
{
  struct output_device *cur;
  const char *group_id;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    return false;

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (cur == device || !cur_gid || strcmp(cur_gid, group_id) != 0 || !cur->session)
	continue;

      if (cur->hold_kind != OUTPUT_HOLD_NONE)
	return true;
    }

  return false;
}

// Whether a hold of the row would be accepted: a member of its group that
// carries audio has a session that can be held
static bool
speaker_group_holdable(struct output_device *device)
{
  struct output_device *cur;
  const char *group_id;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    return outputs_device_holdable(device);

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (!cur_gid || strcmp(cur_gid, group_id) != 0 || cur->tv_proxy_audio_suppress)
	continue;

      if (outputs_device_holdable(cur))
	return true;
    }

  return false;
}

static void
device_to_speaker_info(struct player_speaker_info *spk, struct output_device *device, uint32_t index)
{
  struct output_device *row;

  memset(spk, 0, sizeof(struct player_speaker_info));
  spk->id = device->id;
  spk->index = index;
  spk->active_remote = (uint32_t)device->id;
  strncpy(spk->name, outputs_device_display_name(device), sizeof(spk->name));
  spk->name[sizeof(spk->name) - 1] = '\0';
  strncpy(spk->output_type, device->type_name, sizeof(spk->output_type));
  spk->output_type[sizeof(spk->output_type) - 1] = '\0';
  spk->relvol = device->relvol;
  spk->absvol = device->volume;
  // A row's mute is the row device's own flag; the group walk sets every
  // member alike
  spk->muted = device->muted;
  spk->offset_ms = device->offset_ms;

  spk->supported_formats = device->supported_formats;
  // Devices supporting more than one format should at least have default_format set
  if (device->selected_format != MEDIA_FORMAT_UNKNOWN)
    spk->format = device->selected_format;
  else if (device->default_format != MEDIA_FORMAT_UNKNOWN)
    spk->format = device->default_format;
  else
    spk->format = device->supported_formats;

  strncpy(spk->mode, output_mode_to_string(outputs_device_display_mode(device)), sizeof(spk->mode) - 1);
  spk->mode[sizeof(spk->mode) - 1] = '\0';
  spk->supported_modes = outputs_device_supported_modes(device);

  spk->selected = OUTPUTS_DEVICE_DISPLAY_SELECTED(device);

  spk->has_password = device->has_password;
  spk->has_video = device->has_video;
  spk->requires_auth = device->requires_auth;
  spk->needs_auth_key = (device->requires_auth && device->auth_key == NULL);
  spk->pin_pending = device->pin_pending;
  spk->prevent_playback = device->prevent_playback;
  spk->busy = device->busy;

  spk->phase = device->phase;
  spk->last_failure = device->last_failure;
  spk->held = speaker_group_held(device);
  spk->holdable = speaker_group_holdable(device);

  row = outputs_device_row(device);
  spk->row_id = row->id;
  strncpy(spk->row_name, outputs_device_display_name(row), sizeof(spk->row_name));
  spk->row_name[sizeof(spk->row_name) - 1] = '\0';

  spk->has_homepods = outputs_device_leads_tv_proxy_group(device);

  if (device->v4_address)
    snprintf(spk->v4_address, sizeof(spk->v4_address), "%s", device->v4_address);
  snprintf(spk->config_name, sizeof(spk->config_name), "%s", device->name ? device->name : "");
  spk->volume_follow = device->volume_follow;
  if (device->volume_follow == OUTPUT_VOLUME_FOLLOW_UPNP && device->upnp_scale_unknown)
    spk->volume_follow = OUTPUT_VOLUME_FOLLOW_UNKNOWN_SCALE;

  // An Apple TV enabled without itself plays nothing: its row shows what its
  // HomePods are doing
  if (spk->has_homepods && outputs_device_tv_proxy_excluded(device))
    homepods_session_of(spk, device);
}

static bool
speaker_visible(struct output_device *device)
{
  return !outputs_device_is_hidden(device);
}

static int
speaker_visible_index_match(uint32_t wanted_index, struct output_device **matched)
{
  struct output_device *device;
  uint32_t visible_index = 0;

  for (device = outputs_list(); device; device = device->next)
    {
      if (!speaker_visible(device))
        continue;

      if (visible_index == wanted_index)
        {
          *matched = device;
          return 0;
        }

      visible_index++;
    }

  return -1;
}

static bool
speaker_is_in_selected_set(struct output_device *device, uint64_t *ids, int nspk)
{
  struct output_device *selected_device;
  const char *selected_gid;
  const char *device_gid;
  int i;

  if (!ids)
    return false;

  device_gid = outputs_device_group_id(device);

  for (i = 1; i <= nspk; i++)
    {
      if (ids[i] == device->id)
        return true;

      selected_device = outputs_device_get(ids[i]);
      if (!selected_device)
        continue;

      selected_gid = outputs_device_group_id(selected_device);
      if (device_gid && selected_gid && strcmp(device_gid, selected_gid) == 0)
        return true;
    }

  return false;
}

// Returns true for devices a group start must pass over: TV proxy group
// followers (the HomePods), which must never be started directly - they are
// started by tv_proxy_followers_start() once the Apple TV's probe succeeds -
// and an Apple TV left out of its group, which is given no session at all.
static bool
group_start_skip(struct output_device *device)
{
  if (outputs_device_tv_proxy_excluded(device))
    {
      DPRINTF(E_DBG, L_PLAYER, "TV proxy: not starting Apple TV '%s', its group plays without it\n",
              outputs_device_display_name(device));
      return true;
    }

  if (!outputs_device_is_tv_proxy_follower(device))
    return false;

  DPRINTF(E_DBG, L_PLAYER, "TV proxy: deferring follower '%s' until the Apple TV has been probed\n",
          outputs_device_display_name(device));
  return true;
}

static void
speaker_group_select(struct output_device *device, int max_volume)
{
  struct output_device *cur;
  const char *group_id;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    {
      outputs_device_select(device, max_volume);
      return;
    }

  DPRINTF(E_INFO, L_PLAYER, "Expanding stereo enable/select from '%s' to group gid=%s\n",
          outputs_device_display_name(device), group_id);

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (cur_gid && strcmp(cur_gid, group_id) == 0)
        outputs_device_select(cur, max_volume);
    }
}

// Mutes one device or ends its mute; a change is announced. 1 when accepted
static int
speaker_mute_one(struct output_device *device, bool muted)
{
  bool before = device->muted;

  if (outputs_device_mute_set(device, muted, device_level_edge_cb) != 0)
    return 0;

  if (device->muted != before)
    speaker_announce(device);

  return 1;
}

// Mutes the device and every member of its group, or ends their mute, with or
// without a session: the flag outlives a session. Returns how many changed.
static int
speaker_group_mute_set(struct output_device *device, bool muted)
{
  struct output_device *cur;
  const char *group_id;
  int n = 0;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    return speaker_mute_one(device, muted);

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (cur_gid && strcmp(cur_gid, group_id) == 0)
	n += speaker_mute_one(cur, muted);
    }

  return n;
}

static void
speaker_group_deselect(struct output_device *device)
{
  struct output_device *cur;
  const char *group_id;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    {
      outputs_device_deselect(device);
      return;
    }

  DPRINTF(E_INFO, L_PLAYER, "Expanding stereo disable/deselect from '%s' to group gid=%s\n",
          outputs_device_display_name(device), group_id);

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (cur_gid && strcmp(cur_gid, group_id) == 0)
        outputs_device_deselect(cur);
    }
}

// Starts the device and every member of its group that a group start does not
// pass over, through the phase rule. explicit_request: the user asked for it
static void
speaker_group_start(struct output_device *device, bool explicit_request)
{
  struct output_device *cur;
  const char *group_id;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    {
      if (!group_start_skip(device))
	device_request(device, OUTPUT_REQ_SELECT, explicit_request, false);
      return;
    }

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (!cur_gid || strcmp(cur_gid, group_id) != 0)
        continue;

      if (group_start_skip(cur))
        continue;

      device_request(cur, OUTPUT_REQ_SELECT, explicit_request, false);
    }
}

// Stops the device and every member of its group, through the phase rule (a
// start in flight runs on and is stopped once it has ended). explicit_request:
// the user asked for it
static void
speaker_group_stop(struct output_device *device, bool explicit_request)
{
  struct output_device *cur;
  const char *group_id;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    {
      device_request(device, OUTPUT_REQ_DESELECT, explicit_request, false);
      return;
    }

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (!cur_gid || strcmp(cur_gid, group_id) != 0)
        continue;

      device_request(cur, OUTPUT_REQ_DESELECT, explicit_request, false);
    }
}

static int
speaker_group_volume_apply(struct output_device *device, int absvol, int relvol, output_status_cb cb)
{
  struct output_device *cur;
  const char *group_id;
  int target;
  int ret;
  int pending = 0;
  int successes = 0;

  // Volume commands go to the addressed device, then fan out to every other
  // member of its group (a HomePod stereo pair or a HomePod-behind-AppleTV
  // proxy group) so their cached volume (and hardware volume, where they
  // support it) stays in sync. When cb is NULL the value is only registered,
  // not sent - used for a level the devices report themselves (see the DACP
  // endpoint), where they already play at it and sending it back would only
  // cause churn.
  outputs_device_volume_register(device, absvol, relvol);
  if (cb)
    {
      ret = outputs_device_volume_set(device, cb);
      if (ret > 0)
        pending += ret;
      else if (ret == 0)
        successes++;
    }
  else
    successes++;

  group_id = outputs_device_group_id(device);
  if (!group_id)
    return (pending > 0) ? pending : (successes > 0 ? 0 : -1);

  // Bring every other member to the addressed device's resulting absolute
  // level (its relvol has already been folded in), so a relative command does
  // not resolve to a different absolute on each member.
  target = device->volume;

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      const char *cur_gid = outputs_device_group_id(cur);

      if (cur == device)
        continue;
      if (!cur_gid || strcmp(cur_gid, group_id) != 0)
        continue;

      outputs_device_volume_register(cur, target, -1);
      if (!cb)
        {
          DPRINTF(E_INFO, L_PLAYER, "Volume echo: recorded %d on group member '%s'\n", target, cur->name);
          successes++;
          continue;
        }

      ret = outputs_device_volume_set(cur, cb);
      if (ret > 0)
        {
          pending += ret;
          successes++;
        }
      else if (ret == 0)
        successes++;
    }

  return (pending > 0) ? pending : (successes > 0 ? 0 : -1);
}

static enum command_state
speaker_enumerate(void *arg, int *retval)
{
  struct spk_enum *spk_enum = arg;
  struct output_device *device;
  struct player_speaker_info spk;
  uint32_t i;

  for (device = outputs_list(), i = 0; device; device = device->next)
    {
      if (!speaker_visible(device))
        continue;

      device_to_speaker_info(&spk, device, i);
      spk_enum->cb(&spk, spk_enum->arg);
      i++;
    }

  *retval = 0;
  return COMMAND_END;
}

static enum command_state
speaker_get_byid(void *arg, int *retval)
{
  struct speaker_get_param *spk_param = arg;
  struct output_device *device;
  uint32_t i;

  for (device = outputs_list(), i = 0; device; device = device->next)
    {
      if (!speaker_visible(device))
        continue;

      if ((device->advertised || device->selected)
	  && device->id == spk_param->spk_id)
	{
	  device_to_speaker_info(spk_param->spk_info, device, i);
	  *retval = 0;
	  return COMMAND_END;
	}

      i++;
    }

  // No output device found with matching id
  *retval = -1;
  return COMMAND_END;
}

static enum command_state
speaker_get_byactiveremote(void *arg, int *retval)
{
  struct speaker_get_param *spk_param = arg;
  struct output_device *device;
  uint32_t i;

  // Hidden group members are addressed by their own Active-Remote as well,
  // so this lookup does not filter on visibility; only the index does.
  for (device = outputs_list(), i = 0; device; device = device->next)
    {
      if ((uint32_t)device->id == spk_param->active_remote)
	{
	  device_to_speaker_info(spk_param->spk_info, device, i);
	  *retval = 0;
	  return COMMAND_END;
	}

      if (speaker_visible(device))
        i++;
    }

  // No output device found with matching id
  *retval = -1;
  return COMMAND_END;
}

static enum command_state
speaker_get_byaddress(void *arg, int *retval)
{
  struct speaker_get_param *spk_param = arg;
  struct output_device *device;
  bool match_v4;
  bool match_v6;
  uint32_t i;

  for (device = outputs_list(), i = 0; device; device = device->next)
    {
      if (!speaker_visible(device))
        continue;

      match_v4 = device->v4_address && (strcmp(spk_param->address, device->v4_address) == 0);
      match_v6 = device->v6_address && (strcmp(spk_param->address, device->v6_address) == 0);
      if (match_v4 || match_v6)
	{
	  device_to_speaker_info(spk_param->spk_info, device, i);
	  *retval = 0;
	  return COMMAND_END;
	}

      i++;
    }

  *retval = -1;
  return COMMAND_END;
}

static enum command_state
speaker_get_byindex(void *arg, int *retval)
{
  struct speaker_get_param *spk_param = arg;
  struct output_device *device;
  int ret;

  ret = speaker_visible_index_match(spk_param->index, &device);
  if (ret == 0)
    {
      device_to_speaker_info(spk_param->spk_info, device, spk_param->index);
      *retval = 0;
      return COMMAND_END;
    }

  *retval = -1;
  return COMMAND_END;
}

static enum command_state
speaker_set(void *arg, int *retval)
{
  struct speaker_set_param *speaker_set_param = arg;
  struct output_device *device;
  uint64_t *ids;
  int max_volume;
  int nspk;
  int i;

  ids = speaker_set_param->device_ids;

  if (ids)
    nspk = ids[0];
  else
    nspk = 0;

  DPRINTF(E_DBG, L_PLAYER, "Speaker set: %d speakers\n", nspk);

  // Save the current master volume before we start selecting/unselecting, as
  // that will affect master volume. See comment in outputs_device_select() for
  // why we want to provide a max_volume.
  max_volume = (player_state != PLAY_STOPPED) ? outputs_volume_get() : -1;

  // Log a warning via outputs_device_get() if a speaker doesn't exist
  for (i = 1; i <= nspk; i++)
    outputs_device_get(ids[i]);

  // Stereo leaders are the visible control point for paired HomePods. If a
  // requested output belongs to an AirPlay 2 stereo group, expand selection to
  // every member sharing that group id.
  for (device = outputs_list(); device; device = device->next)
    {
      if (speaker_is_in_selected_set(device, ids, nspk))
        {
          outputs_device_select(device, max_volume);
        }
      else
        outputs_device_deselect(device);
    }

  // Stops first, then the starts, each through the phase rule; the speakers
  // given are the user's own selection
  for (device = outputs_list(); device; device = device->next)
    {
      if (!device->selected)
	device_request(device, OUTPUT_REQ_DESELECT, false, false);
    }

  for (device = outputs_list(); device; device = device->next)
    {
      if (!device->selected || group_start_skip(device))
	continue;

      device_request(device, OUTPUT_REQ_SELECT, true, false);
    }

  *retval = 0;
  return COMMAND_END;
}

struct speaker_enable_param
{
  uint64_t id;
  int include_tv;
  int volume;     // level to start at, or -1 for none
};

// Selects the device and its group and starts them as the user's own select,
// at the level given (-1 for none)
static void
speaker_enable_apply(struct output_device *device, int volume)
{
  int max_volume;

  // An explicit level is not capped to the master volume
  max_volume = (volume < 0 && player_state != PLAY_STOPPED) ? outputs_volume_get() : -1;

  speaker_group_select(device, max_volume);

  // Register the level on the device and every member of its group before the
  // start, so that the first volume the speakers are sent is this one. Nothing
  // is sent here: the start does that.
  if (volume >= 0)
    speaker_group_volume_apply(device, volume, -1, NULL);

  speaker_group_start(device, true);
}

static enum command_state
speaker_enable(void *arg, int *retval)
{
  struct speaker_enable_param *param = arg;
  struct output_device *device;
  struct tv_proxy_enable enable;

  *retval = -1;

  device = outputs_device_get(param->id);
  if (!device)
    return COMMAND_END;

  DPRINTF(E_DBG, L_PLAYER, "Speaker enable: '%s' (id=%" PRIu64 ")\n", outputs_device_display_name(device), param->id);

  // The option is fixed for the life of a selection: the Apple TV either has
  // a session or has none, and swapping that under a playing group is a stop
  // and a start, which the caller does as a disable and an enable.
  enable = tv_proxy_enable_resolve(outputs_device_leads_tv_proxy_group(device), param->include_tv,
                                   device->selected, device->tv_proxy_speakers_only);
  if (enable.refusal)
    {
      // Nothing has been selected or started
      *retval = enable.refusal;
      return COMMAND_END;
    }

  if (enable.set_flag)
    {
      device->tv_proxy_speakers_only = enable.flag;
      if (param->include_tv >= 0)
        DPRINTF(E_INFO, L_PLAYER, "Speaker enable: '%s' %s the Apple TV\n",
                outputs_device_display_name(device), enable.flag ? "without" : "with");
    }

  // Accepted: the outcome reaches clients as the device's phase
  speaker_enable_apply(device, param->volume);

  *retval = 0;
  return COMMAND_END;
}

static enum command_state
speaker_disable(void *arg, int *retval)
{
  uint64_t *id = arg;
  struct output_device *device;

  *retval = -1;

  device = outputs_device_get(*id);
  if (!device)
    return COMMAND_END;

  DPRINTF(E_DBG, L_PLAYER, "Speaker disable: '%s' (id=%" PRIu64 ")\n", outputs_device_display_name(device), *id);

  speaker_group_deselect(device);
  device->tv_proxy_start_direct = 0;

  speaker_group_stop(device, true);

  *retval = 0;
  return COMMAND_END;
}

static enum command_state
speaker_generic_bh(void *arg, int *retval)
{
  status_update(player_state, LISTENER_SPEAKER | LISTENER_VOLUME);
  return COMMAND_END;
}

/*
 * Airplay speakers can via DACP set the "busy" + "prevent-playback" properties,
 * which we handle below. We try to do this like iTunes, except we need to
 * unselect devices, since our clients don't understand the "grayed out" state:
 *
 *                           | Playing to 1 device                            | Playing to 2 devices
 * device-prevent-playback=1 | Playback stops, device selected but grayed out | Playback stops on device, continues on other device, device selected but grayed out
 * device-prevent-playback=0 | Playback does not resume, device not grayed    | Playback resumes on device, device not grayed
 * (device-busy does the same)
 *
 * device-prevent-playback=1 | (same)                                         | (same)
 * device-busy=1             | (no change)                                    | (no change)
 * device-prevent-playback=0 | Playback does not resume, device still grayed  | Playback does not resume, device still grayed
 * device-busy=0             | Playback does not resume, device not grayed    | Playback resumes on device, device not grayed
 * (same if vice versa, ie busy=1 first)
 *
 * Special cases:
 *  a) Denon Home 150: User deselects speaker, we send TEARDOWN, speaker then
 *     sends prevent-playback=1 and then shortly after prevent-play-playback=0.
 *     In this case we never want to resume playback.
 */

static enum command_state
speaker_format_set(void *arg, int *retval)
{
  struct speaker_attr_param *param = arg;
  struct output_device *device;

  if (param->format == MEDIA_FORMAT_UNKNOWN)
    goto error;

  device = outputs_device_get(param->spk_id);
  if (!device)
    goto error;

  if (!(param->format & device->supported_formats))
    goto error;

  device->selected_format = param->format;

  *retval = 0;
  return COMMAND_END;

 error:
  DPRINTF(E_LOG, L_PLAYER, "Error setting format '%s', device unknown or format unsupported\n", media_format_to_string(param->format));
  *retval = -1;
  return COMMAND_END;
}

static enum command_state
speaker_offset_ms_set(void *arg, int *retval)
{
  struct speaker_attr_param *param = arg;
  struct output_device *device;

  if (param->offset_ms < -2000 || param->offset_ms > 2000)
    goto error;

  device = outputs_device_get(param->spk_id);
  if (!device)
    goto error;

  // Clients routinely re-send the current offset alongside other output
  // fields (e.g. a volume change); an unchanged value must not disturb a
  // live session (the buffered path re-times with an audible flush).
  if (device->offset_ms == param->offset_ms)
    {
      *retval = 0;
      return COMMAND_END;
    }

  device->offset_ms = param->offset_ms;

  if (player_state == PLAY_PAUSED)
    {
      // No live session to refresh; stop so the new offset is picked up
      // when playback resumes, which starts the device again
      device_request(device, OUTPUT_REQ_DESELECT, false, false);
    }
  else
    {
      // Refresh any live realtime session so the new offset applies to the
      // next sync packet; a no-op for backends without live offset support
      outputs_device_offset_set(device);
    }

  *retval = 0;
  return COMMAND_END;

 error:
  DPRINTF(E_LOG, L_PLAYER, "Error setting offset_ms %d, outside of supported range -2000 to 2000\n", param->offset_ms);
  *retval = -1;
  return COMMAND_END;
}

// Holds one device or ends its hold; a change is announced. 1 when accepted
static int
speaker_hold_one(struct output_device *device, bool held)
{
  bool before = device->held;

  if (outputs_device_hold_set(device, held, "by request", device_level_edge_cb) != 0)
    return 0;

  if (device->held != before)
    speaker_announce(device);

  return 1;
}

// Holds the speakers of a row, or ends their hold: the device and, for a group
// (a stereo pair, an Apple TV and its HomePods), every member with a session,
// so the members stop and restart together. -1 for an unknown device, -2 when
// none of them could be held.
static enum command_state
speaker_hold_set(void *arg, int *retval)
{
  struct speaker_attr_param *param = arg;
  struct output_device *device;
  struct output_device *cur;
  const char *group_id;
  int accepted = 0;

  device = outputs_device_get(param->spk_id);
  if (!device)
    {
      *retval = -1;
      return COMMAND_END;
    }

  group_id = outputs_device_group_id(device);
  if (!group_id)
    accepted += speaker_hold_one(device, param->held);
  else
    {
      for (cur = outputs_list(); cur; cur = cur->next)
	{
	  const char *cur_gid = outputs_device_group_id(cur);

	  if (!cur_gid || strcmp(cur_gid, group_id) != 0 || !cur->session)
	    continue;

	  accepted += speaker_hold_one(cur, param->held);
	}
    }

  DPRINTF(E_INFO, L_PLAYER, "%s '%s': %d speaker(s)\n", param->held ? "Hold" : "Hold ended for",
          outputs_device_display_name(device), accepted);

  *retval = (accepted > 0) ? 0 : -2;
  return COMMAND_END;
}

// Mutes the speakers of a row, or ends their mute. -1 for an unknown device.
static enum command_state
speaker_mute_set(void *arg, int *retval)
{
  struct speaker_attr_param *param = arg;
  struct output_device *device;
  int n;

  device = outputs_device_get(param->spk_id);
  if (!device)
    {
      *retval = -1;
      return COMMAND_END;
    }

  n = speaker_group_mute_set(device, param->muted);

  DPRINTF(E_INFO, L_PLAYER, "%s '%s': %d speaker(s)\n", param->muted ? "Mute" : "Mute ended for",
	  outputs_device_display_name(device), n);

  *retval = 0;
  return COMMAND_END;
}

static enum command_state
speaker_mode_set(void *arg, int *retval)
{
  struct speaker_attr_param *param = arg;
  struct output_device *device;
  int ret;

  device = outputs_device_get(param->spk_id);
  if (!device)
    goto error;

  ret = outputs_device_mode_set(device, param->mode);
  if (ret < 0)
    goto error; // Invalid mode value

  // The backend changes under a session or a start in flight: the device is
  // stopped and started again on the new backend, by the phase rule
  if (ret == 1)
    device_request(device, OUTPUT_REQ_MODE_SWITCH, false, true);

  *retval = 0;
  return COMMAND_END;

 error:
  DPRINTF(E_LOG, L_PLAYER, "Error setting mode '%s', device unknown or mode invalid\n",
          output_mode_to_string(param->mode));
  *retval = -1;
  return COMMAND_END;
}

// Begins an authorise with the PIN. then_select: the request also asked for
// the device to be started, at volume (-1 for none), once the PIN has paired
// it, or with a new PIN start when the receiver refuses this one; enable,
// when given, is the include_tv option that start takes. A PIN that arrives
// while the device has a session or an operation in flight is refused with -6
// and changes nothing; whatever is in flight reports as usual
static int
authorize_begin(struct output_device *device, const char *pin, bool then_select, int volume,
                const struct tv_proxy_enable *enable)
{
  struct output_phase_input in;
  struct output_phase_result res;
  int ret;

  phase_input_fill(&in, device);
  output_phase_on_request(&in, OUTPUT_REQ_AUTHORIZE, &res);
  if (res.next_op != OUTPUT_OP_AUTHORIZE)
    {
      DPRINTF(E_INFO, L_PLAYER, "PIN for '%s' refused: it has a session or an operation in flight (%s)\n",
	      device->name, op_name(device->op));
      return -6;
    }

  if (enable && enable->set_flag)
    device->tv_proxy_speakers_only = enable->flag;

  flags_apply(device, &res);

  device->then_select = then_select;
  device->start_volume = then_select ? volume : -1;

  // The PIN is used inside this call (the backend builds its first pairing
  // request at once), so it is never kept
  ret = outputs_device_authorize(device, pin, device_op_cb);
  if (ret < 0)
    operation_failed_at_once(device, OUTPUT_OP_AUTHORIZE);
  else if (ret == 0)
    device->then_select = 0;

  return 0;
}

static enum command_state
speaker_authorize(void *arg, int *retval)
{
  struct speaker_attr_param *param = arg;
  struct output_device *device;

  *retval = -1;

  device = outputs_device_get(param->spk_id);
  if (!device)
    return COMMAND_END;

  *retval = authorize_begin(device, param->pin, false, -1, NULL);
  return COMMAND_END;
}

struct speaker_authorize_select_param
{
  uint64_t id;
  const char *pin;
  int include_tv;
  int volume;     // level to start at, or -1 for none
};

// A PIN and the select it unlocks, in one request: the include_tv option is
// judged now, as an enable would judge it, and the start follows the pairing
static enum command_state
speaker_authorize_then_select(void *arg, int *retval)
{
  struct speaker_authorize_select_param *param = arg;
  struct output_device *device;
  struct tv_proxy_enable enable;

  *retval = -1;

  device = outputs_device_get(param->id);
  if (!device)
    return COMMAND_END;

  enable = tv_proxy_enable_resolve(outputs_device_leads_tv_proxy_group(device), param->include_tv,
                                   device->selected, device->tv_proxy_speakers_only);
  if (enable.refusal)
    {
      *retval = enable.refusal;
      return COMMAND_END;
    }

  *retval = authorize_begin(device, param->pin, true, param->volume, &enable);
  return COMMAND_END;
}

static enum command_state
speaker_start_all(void *arg, int *retval)
{
  outputs_stop_delayed_cancel();

  speakers_join();

  *retval = 0;
  return COMMAND_END;
}


// The volume commands register the level and send it to every device with
// no operation in flight (a starting device takes it from its start's level
// row, a stopping one keeps it for its next start). They answer at once: the
// devices' answers reach the report handler, and no command waits for them.

static enum command_state
volume_set(void *arg, int *retval)
{
  union player_arg *cmdarg = arg;

  outputs_volume_set(cmdarg->intval, device_volume_cb);

  *retval = 0;
  return COMMAND_END;
}

static enum command_state
volume_setrel_speaker(void *arg, int *retval)
{
  struct speaker_attr_param *vol_param = arg;
  struct output_device *device;

  device = outputs_device_get(vol_param->spk_id);
  if (!device)
    {
      DPRINTF(E_WARN, L_PLAYER, "Could not set volume for speaker id %" PRIu64 ", speaker disappeared\n", vol_param->spk_id);
      *retval = -1;
      return COMMAND_END;
    }

  *retval = (speaker_group_volume_apply(device, -1, vol_param->volume, device_volume_cb) < 0) ? -1 : 0;
  return COMMAND_END;
}

static enum command_state
volume_setabs_speaker(void *arg, int *retval)
{
  struct speaker_attr_param *vol_param = arg;
  struct output_device *device;

  device = outputs_device_get(vol_param->spk_id);
  if (!device)
    {
      DPRINTF(E_WARN, L_PLAYER, "Could not set volume for speaker id %" PRIu64 ", speaker disappeared\n", vol_param->spk_id);
      *retval = -1;
      return COMMAND_END;
    }

  *retval = (speaker_group_volume_apply(device, vol_param->volume, -1, device_volume_cb) < 0) ? -1 : 0;
  return COMMAND_END;
}

/* ---------------- Sending a speaker's reported level back ----------------- */

// Some receivers only report the level their own buttons ask for and wait for
// the sender to apply it, while HomePods change themselves. Which speakers
// need the level sent back, and what the others do when it is, is not yet
// known, so the "device_volume_echo" setting chooses: "off" records the level
// only, "reporter" also sends it back to the reporting speaker, and "group"
// sends it to the reporting speaker and every member of its group.

enum volume_echo_mode
{
  VOLUME_ECHO_OFF,
  VOLUME_ECHO_REPORTER,
  VOLUME_ECHO_GROUP,
};

static const char *volume_echo_mode_names[] = { "off", "reporter", "group" };

static uint64_t
monotonic_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Read on every report, so a change through the settings API applies at once
static enum volume_echo_mode
volume_echo_mode_get(void)
{
  char value[16];
  size_t i;

  config_get_str_copy("device_volume_echo", "reporter", value, sizeof(value));

  for (i = 0; i < ARRAY_SIZE(volume_echo_mode_names); i++)
    {
      if (strcmp(value, volume_echo_mode_names[i]) == 0)
        return (enum volume_echo_mode)i;
    }

  DPRINTF(E_WARN, L_PLAYER, "Unknown device_volume_echo '%s', using 'reporter'\n", value);
  return VOLUME_ECHO_REPORTER;
}

static const char *
volume_echo_role(struct output_device *device)
{
  if (outputs_device_is_tv_proxy_leader(device))
    return "TV proxy leader";
  if (outputs_device_is_tv_proxy_follower(device))
    return "TV proxy follower";
  if (outputs_device_group_id(device))
    return "group member";

  return "ungrouped";
}

// Completion of a level sent back to a speaker. The send is not part of a
// command, so unlike a volume command's answer this ends none; a failure goes
// to the streaming handler, which would have had it had the send not been made.
static int
handle_volume_echo(struct output_device *device, enum output_device_state status, int retval)
{
  if (status == OUTPUT_STATE_FAILED || status == OUTPUT_STATE_STOPPED)
    {
      DPRINTF(E_INFO, L_PLAYER, "Volume echo: send to '%s' ended with state %d\n", device->name, status);
      return handle_streaming(device, status, retval);
    }

  DPRINTF(E_INFO, L_PLAYER, "Volume echo: '%s' accepted %d after %" PRIu64 " ms\n",
          device->name, device->volume_sent, monotonic_ms() - device->volume_sent_ms);

  return retval;
}

static void
volume_echo_send_one(struct output_device *device, const char *why)
{
  uint64_t now;
  int ret;

  // A session still starting has the start's callback registered, which a
  // send would replace even if the backend then declines it, so the start
  // would never report back
  if (!device->session || device->state < OUTPUT_STATE_CONNECTED)
    {
      DPRINTF(E_INFO, L_PLAYER, "Volume echo: not sending %d to %s '%s' (%s): %s\n",
              device->volume, why, device->name, volume_echo_role(device),
              device->session ? "still starting" : "no session");
      return;
    }

  if (outputs_device_silent(device))
    {
      DPRINTF(E_INFO, L_PLAYER, "Volume echo: not sending %d to %s '%s' (%s): silent, level kept\n",
              device->volume, why, device->name, volume_echo_role(device));
      return;
    }

  now = monotonic_ms();
  if (now - device->volume_echo_since_ms >= PLAYER_VOLUME_ECHO_MAX_PERIOD_MS)
    {
      device->volume_echo_since_ms = now;
      device->volume_echo_count = 0;
    }

  if (device->volume_echo_count >= PLAYER_VOLUME_ECHO_MAX_SENDS)
    {
      DPRINTF(E_LOG, L_PLAYER, "Volume echo: not sending %d to %s '%s' (%s): already sent %d in %d ms\n",
              device->volume, why, device->name, volume_echo_role(device),
              device->volume_echo_count, PLAYER_VOLUME_ECHO_MAX_PERIOD_MS);
      return;
    }

  ret = outputs_device_volume_set(device, device_volume_echo_cb);
  if (ret > 0)
    device->volume_echo_count++;

  DPRINTF(E_INFO, L_PLAYER, "Volume echo: sending %d to %s '%s' (%s): %s\n",
          device->volume, why, device->name, volume_echo_role(device),
          (ret > 0) ? "sent" : (ret == 0) ? "declined by the backend" : "failed");
}

// Queued from the timer as a command, so it never runs while another command
// is waiting on a speaker: a send would replace that command's callback and
// leave its caller blocked
static enum command_state
volume_echo_send(void *arg, int *retval)
{
  uint64_t *id = arg;
  struct output_device *device;
  struct output_device *cur;
  enum volume_echo_mode mode;
  const char *group_id;
  const char *cur_gid;

  *retval = 0;

  device = outputs_device_get(*id);
  if (!device)
    {
      DPRINTF(E_INFO, L_PLAYER, "Volume echo: speaker %" PRIu64 " has gone, nothing sent\n", *id);
      return COMMAND_END;
    }

  mode = volume_echo_mode_get();
  if (mode == VOLUME_ECHO_OFF)
    {
      DPRINTF(E_INFO, L_PLAYER, "Volume echo: mode is off, nothing sent to '%s'\n", device->name);
      return COMMAND_END;
    }

  volume_echo_send_one(device, "reporter");

  group_id = outputs_device_group_id(device);
  if (mode != VOLUME_ECHO_GROUP || !group_id)
    return COMMAND_END;

  for (cur = outputs_list(); cur; cur = cur->next)
    {
      if (cur == device)
        continue;

      cur_gid = outputs_device_group_id(cur);
      if (!cur_gid || strcmp(cur_gid, group_id) != 0)
        continue;

      volume_echo_send_one(cur, "group member");
    }

  return COMMAND_END;
}

static void
volume_echo_timer_cb(int fd, short what, void *arg)
{
  struct output_device *device = arg;
  uint64_t *id;

  DPRINTF(E_INFO, L_PLAYER, "Volume echo: no report from '%s' for %d ms, queueing the send of %d\n",
          device->name, PLAYER_VOLUME_ECHO_DELAY_MS, device->volume);

  CHECK_NULL(L_PLAYER, id = malloc(sizeof(*id)));
  *id = device->id;

  // On success the command frees id
  if (commands_exec_async(cmdbase, volume_echo_send, id) < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Volume echo: could not queue the send to '%s'\n", device->name);
      free(id);
    }
}

// Decides, for a level the speaker has just reported, whether to send it back
// once the reports stop. The timer lives on the device, so it is freed with it.
static void
volume_echo_schedule(struct output_device *device, const char *volstr, int volume)
{
  struct timeval delay = { 0, PLAYER_VOLUME_ECHO_DELAY_MS * 1000 };
  enum volume_echo_mode mode;
  uint64_t since_sent = 0;
  bool restarted;

  mode = volume_echo_mode_get();

  if (device->volume_sent_ms)
    since_sent = monotonic_ms() - device->volume_sent_ms;

  if (device->volume_sent_ms)
    DPRINTF(E_INFO, L_PLAYER, "Volume echo: '%s' (%s, group %s) reports %s = %d, %" PRIu64 " ms after %d was last sent to it, mode %s\n",
            device->name, volume_echo_role(device), outputs_device_group_id(device) ? outputs_device_group_id(device) : "none",
            volstr, volume, since_sent, device->volume_sent, volume_echo_mode_names[mode]);
  else
    DPRINTF(E_INFO, L_PLAYER, "Volume echo: '%s' (%s, group %s) reports %s = %d, nothing sent to it yet, mode %s\n",
            device->name, volume_echo_role(device), outputs_device_group_id(device) ? outputs_device_group_id(device) : "none",
            volstr, volume, volume_echo_mode_names[mode]);

  if (mode == VOLUME_ECHO_OFF)
    {
      if (device->volume_echo_timer)
        evtimer_del(device->volume_echo_timer);
      return;
    }

  if (device->volume_sent_ms && since_sent < PLAYER_VOLUME_ECHO_WINDOW_MS && volume == device->volume_sent)
    {
      DPRINTF(E_INFO, L_PLAYER, "Volume echo: '%s' is confirming the level sent to it, not sending it back\n", device->name);
      if (device->volume_echo_timer)
        evtimer_del(device->volume_echo_timer);
      return;
    }

  // 0 is sent as -144 (off), so a muted speaker is left alone rather than
  // being told again, and the lowest level, which reports at -30, is not
  // turned into a mute
  if (volume == 0)
    {
      DPRINTF(E_INFO, L_PLAYER, "Volume echo: '%s' reports 0, not sending it back\n", device->name);
      if (device->volume_echo_timer)
        evtimer_del(device->volume_echo_timer);
      return;
    }

  if (!device->volume_echo_timer)
    CHECK_NULL(L_PLAYER, device->volume_echo_timer = evtimer_new(evbase_player, volume_echo_timer_cb, device));

  restarted = evtimer_pending(device->volume_echo_timer, NULL);
  evtimer_add(device->volume_echo_timer, &delay);

  DPRINTF(E_INFO, L_PLAYER, "Volume echo: '%s' send-back %s, due in %d ms\n",
          device->name, restarted ? "restarted" : "armed", PLAYER_VOLUME_ECHO_DELAY_MS);
}

// What a level the device sent means, given its silence and the levels sent to
// it lately. The rules are in level_rules.c; this is their one caller.
static enum level_report
speaker_report_classify(struct output_device *device, int level, bool asks)
{
  return level_report_classify(outputs_device_silent(device), asks, level,
                               device->volume_sent_ms, device->volume_sent_restores, device->wake_sent_ms,
                               monotonic_ms(), PLAYER_VOLUME_ECHO_WINDOW_MS);
}

// One command for every level a device reports and every level it asks for.
// Results: 0 recorded, 1 not the device's level (it is silent, or its level
// was just put back), 2 a wake, -1 error. No answer to an asked level is waited for.
static enum command_state
volume_setraw_speaker(void *arg, int *retval)
{
  struct speaker_attr_param *vol_param = arg;
  struct output_device *device;
  int volume;

  device = outputs_device_get(vol_param->spk_id);
  if (!device)
    {
      DPRINTF(E_WARN, L_PLAYER, "Could not set volume for speaker id %" PRIu64 ", speaker disappeared\n", vol_param->spk_id);
      *retval = -1;
      return COMMAND_END;
    }

  if (vol_param->volstr)
    {
      volume = outputs_device_volume_to_pct(device, vol_param->volstr); // Only converts
      if (volume < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Could not parse volume '%s' in update_volume() for speaker '%s'\n", vol_param->volstr, device->name);
	  *retval = -1;
	  return COMMAND_END;
	}
    }
  else
    volume = vol_param->volume; // A request, already on our scale

  switch (speaker_report_classify(device, volume, vol_param->asks))
    {
      case LEVEL_REPORT_ABSORB:
	if (outputs_device_silent(device))
	  DPRINTF(E_DBG, L_PLAYER, "'%s' reports %d while silent: not its level\n", device->name, volume);
	else
	  DPRINTF(E_DBG, L_PLAYER, "'%s' reports %d just after its level was put back: not a change\n", device->name, volume);
	*retval = 1;
	return COMMAND_END;

      case LEVEL_REPORT_WAKE:
	device->wake_sent_ms = monotonic_ms();
	DPRINTF(E_INFO, L_PLAYER, "'%s' woke: its own control %s %d while silent\n",
		device->name, vol_param->asks ? "asks for" : "reports", volume);
	*retval = 2;
	return COMMAND_END;

      case LEVEL_REPORT_RECORD:
	break;
    }

  if (vol_param->asks)
    {
      // The device asks for the level: applied on its group and sent, as a
      // level set through the API is, without waiting for the answers
      *retval = (speaker_group_volume_apply(device, volume, -1, device_volume_cb) < 0) ? -1 : 0;
      return COMMAND_END;
    }

  // A raw volume is the device reporting its own level (see the DACP
  // endpoint and the AirPlay event channel). It is recorded on the whole group
  // at once, so clients see it straight away, and may be sent back once the
  // reports stop, unless the device said it is already playing at it.
  device->volume_reported_ms = monotonic_ms();
  if (!vol_param->no_send_back)
    volume_echo_schedule(device, vol_param->volstr, volume);
  speaker_group_volume_apply(device, volume, -1, NULL);

  *retval = 0;

  return COMMAND_END;
}

// A level the device reported it is already playing at, queued by a thread
// that must not wait for the player (see player_volume_reported_speaker()).
// Async commands have no bottom half, so clients are told here, and the
// supervisor once the level is recorded.
static enum command_state
volume_reported_speaker(void *arg, int *retval)
{
  struct volume_reported_param *param = arg;
  struct speaker_attr_param vol_param;
  struct output_device *device;
  struct output_device *row;

  memset(&vol_param, 0, sizeof(vol_param));
  vol_param.spk_id = param->id;
  vol_param.volstr = param->volstr;
  vol_param.no_send_back = true;

  volume_setraw_speaker(&vol_param, retval);
  if (*retval < 0 || *retval == 1)
    return COMMAND_END; // Nothing recorded, nothing to tell

  device = outputs_device_get(param->id);
  if (!device)
    return COMMAND_END;

  row = outputs_device_row(device);

  // A wake changes no row, so there is no status update
  if (*retval == 2)
    {
      if (evchan_event_receiver_command(row->id, device->id, outputs_device_display_name(row), "wake", "airplay2_events") < 0)
	DPRINTF(E_DBG, L_PLAYER, "Wake from '%s' not reported, no event channel\n", device->name);
      return COMMAND_END;
    }

  status_update(player_state, LISTENER_VOLUME);

  if (evchan_event_device_volume(row->id, device->id, outputs_device_display_name(row), param->verb, param->volstr) < 0)
    DPRINTF(E_DBG, L_PLAYER, "Volume '%s' from '%s' not reported, no event channel\n", param->verb, device->name);

  return COMMAND_END;
}

// A level from the device's UPnP renderer. The renderer answers every level
// sent to the device too, at its own scale, which is ours plus an offset
// learned from those answers. Nothing is sent to the device from here.
static enum command_state
volume_followed_report(void *arg, int *retval)
{
  struct player_followed_report *r = arg;
  struct output_device *device;
  struct output_device *row;
  uint64_t now = monotonic_ms();
  bool was_unknown;
  enum upnp_echo echo;
  int offset;

  // Always 0, so the bottom half runs; it looks at the result
  *retval = 0;

  device = outputs_device_get(r->id);
  if (!device)
    {
      r->result = FOLLOWED_GONE;
      return COMMAND_END;
    }

  row = outputs_device_row(device);
  r->row_id = row->id;
  snprintf(r->row_name, sizeof(r->row_name), "%s", outputs_device_display_name(row));
  r->sent = device->volume_sent;

  if (!device->upnp_offset_loaded)
    {
      device->upnp_offset = config_get_device_int(device->name, "upnp_volume_offset", 0);
      device->upnp_offset_loaded = true;
    }
  r->offset = device->upnp_offset;

  // A level the device has just reported over DACP is waiting to be sent
  // back, or has only just been: the renderer may still hold the old one,
  // which must not be recorded over it. The send-back's own answer then
  // arrives as an answer to a level sent.
  if ((device->volume_echo_timer && evtimer_pending(device->volume_echo_timer, NULL))
      || (device->volume_reported_ms && now - device->volume_reported_ms < PLAYER_VOLUME_ECHO_DELAY_MS + 100))
    {
      r->result = FOLLOWED_HELD;
      return COMMAND_END;
    }

  // The renderer's first report soon after a level is sent is its answer to
  // it. Reports after that are the device's own changes (a turn of the knob
  // straight after a level was sent), so they are not read as answers: taken
  // as one, a new level looks like a scale that is not ours.
  if (upnp_report_is_answer(device->volume_sent_ms, device->volume_answered, now, PLAYER_VOLUME_ECHO_WINDOW_MS))
    {
      device->volume_answered = true;
      was_unknown = device->upnp_scale_unknown;
      // The first report after a silence ended may be a level the knob reached
      // before the device applied ours, which would read as a wrong scale, so
      // nothing is learned from it
      echo = device->volume_sent_restores ? UPNP_ECHO_NO_LEARN : upnp_level_echo(r->raw, r->mute, device->volume_sent, &offset);
      switch (echo)
	{
	  case UPNP_ECHO_LEARN:
	    device->upnp_scale_unknown = false;
	    if (was_unknown)
	      {
		DPRINTF(E_LOG, L_PLAYER, "'%s' answered %d with %d: its scale fits again, following its level\n",
			device->name, device->volume_sent, r->raw);
		r->follow_changed = true;
	      }
	    if (offset != device->upnp_offset)
	      {
		device->upnp_offset = offset;
		config_set_device_int_deferred(device->name, "upnp_volume_offset", offset);
		r->offset = offset;
		r->learned = true;
	      }
	    break;

	  case UPNP_ECHO_NO_LEARN:
	    break;

	  case UPNP_ECHO_SCALE_UNKNOWN:
	    if (!was_unknown)
	      {
		DPRINTF(E_WARN, L_PLAYER, "'%s' answered %d with %d: its scale is not ours, not following its level\n",
			device->name, device->volume_sent, r->raw);
		r->follow_changed = true;
	      }
	    device->upnp_scale_unknown = true;
	    break;
	}

      r->result = FOLLOWED_CONFIRMATION;
      return COMMAND_END;
    }

  if (device->upnp_scale_unknown)
    {
      r->result = FOLLOWED_SCALE_UNKNOWN;
      return COMMAND_END;
    }

  r->pct = upnp_level_map(r->raw, r->mute, device->upnp_offset);

  switch (speaker_report_classify(device, r->pct, false))
    {
      case LEVEL_REPORT_ABSORB:
	r->result = FOLLOWED_ABSORBED;
	return COMMAND_END;

      case LEVEL_REPORT_WAKE:
	device->wake_sent_ms = now;
	DPRINTF(E_INFO, L_PLAYER, "'%s' woke: its own control reports %d while silent\n", device->name, r->pct);
	r->result = FOLLOWED_WAKE;
	return COMMAND_END;

      case LEVEL_REPORT_RECORD:
	break;
    }

  if (r->pct == device->volume)
    {
      r->result = FOLLOWED_UNCHANGED;
      return COMMAND_END;
    }

  // Recorded on the whole group, never sent: the device is already playing at it
  speaker_group_volume_apply(device, r->pct, -1, NULL);
  r->result = FOLLOWED_RECORDED;
  return COMMAND_END;
}

static enum command_state
volume_followed_bh(void *arg, int *retval)
{
  struct player_followed_report *r = arg;

  if (r->result == FOLLOWED_RECORDED)
    status_update(player_state, LISTENER_VOLUME);
  if (r->follow_changed)
    status_update(player_state, LISTENER_SPEAKER);

  return COMMAND_END;
}

struct follow_state_param {
  uint64_t id;
  enum output_volume_follow state;
  bool changed;
};

static enum command_state
speaker_follow_state_set(void *arg, int *retval)
{
  struct follow_state_param *p = arg;
  struct output_device *device;

  device = outputs_device_get(p->id);
  if (!device)
    {
      *retval = -1;
      return COMMAND_END;
    }

  p->changed = (device->volume_follow != p->state);
  device->volume_follow = p->state;

  *retval = 0;
  return COMMAND_END;
}

static enum command_state
speaker_follow_state_bh(void *arg, int *retval)
{
  struct follow_state_param *p = arg;

  if (p->changed)
    status_update(player_state, LISTENER_SPEAKER);

  return COMMAND_END;
}

static enum command_state
volume_generic_bh(void *arg, int *retval)
{
  status_update(player_state, LISTENER_VOLUME);
  return COMMAND_END;
}

static enum command_state
input_config_reload_cmd(void *arg, int *retval)
{
  *retval = input_config_reload();
  return COMMAND_END;
}

struct memstats_sample
{
  int sessions;
  int enc_in_use;
  int enc_budget;
};

static enum command_state
memstats_sample_cmd(void *arg, int *retval)
{
  struct memstats_sample *sample = arg;

  sample->sessions = outputs_sessions_count();
  airplay_encoder_budget_get(&sample->enc_in_use, &sample->enc_budget);

#ifdef STORM_DEBUG
  {
    struct output_device *device;
    uint64_t now = outputs_monotonic_ms();

    // Every operation ends within its backend's bounds, far below this
    for (device = outputs_list(); device; device = device->next)
      if (device->op != OUTPUT_OP_NONE && now - device->op_started_ms > 120000)
	DPRINTF(E_LOG, L_PLAYER, "Bug! Operation %s on '%s' has been in flight for %" PRIu64 " s\n",
		op_name(device->op), device->name, (now - device->op_started_ms) / 1000);
  }
#endif
  *retval = 0;
  return COMMAND_END;
}

// Backends can have async teardowns (e.g. RTSP session close) that complete
// via callbacks dispatched on the player event loop. Tearing outputs down
// from another thread would race those callbacks over the same session
// objects, so this runs outputs_deinit() as a command on the player thread,
// serializing it with any in-flight teardown callbacks on that loop.
static enum command_state
outputs_deinit_cmd(void *arg, int *retval)
{
  outputs_deinit();
  *retval = 0;
  return COMMAND_END;
}

/* ------------------------------- Player API ------------------------------- */

int
player_get_status(struct player_status *status)
{
  int ret;

  ret = commands_exec_sync(cmdbase, get_status, NULL, status);
  return ret;
}

int
player_input_config_reload(void)
{
  return commands_exec_sync(cmdbase, input_config_reload_cmd, NULL, NULL);
}

int
player_memstats_sample(int *sessions, int *enc_in_use, int *enc_budget)
{
  struct memstats_sample sample = { 0 };
  int ret;

  ret = commands_exec_sync(cmdbase, memstats_sample_cmd, NULL, &sample);
  if (ret < 0)
    return ret;

  *sessions = sample.sessions;
  *enc_in_use = sample.enc_in_use;
  *enc_budget = sample.enc_budget;
  return 0;
}


/* ------------------------------ Thread: httpd ----------------------------- */

/*
 * Starts/resumes playback
 *
 * Depending on the player state, this will either resume playing the current
 * item (player is paused) or begin playing the queue from the beginning.
 *
 * If shuffle is set, the queue is reshuffled prior to starting playback.
 *
 * @return 0 if successful, -1 if an error occurred
 */
int
player_playback_start(void)
{
  int ret;

  ret = commands_exec_sync(cmdbase, playback_start, playback_start_bh, NULL);

  return ret;
}

/*
 * Starts/resumes playback of the given queue_item
 *
 * If shuffle is set, the queue is reshuffled prior to starting playback.
 *
 * If a pointer is given as argument "itemid", its value will be set to the playing item id.
 *
 * @param queue_item to start playing
 * @return 0 if successful, -1 if an error occurred
 */
int
player_playback_start_byitem(struct db_queue_item *queue_item)
{
  int ret;

  ret = commands_exec_sync(cmdbase, playback_start_item, playback_start_bh, queue_item);

  return ret;
}

int
player_playback_start_byid(uint32_t id)
{
  union player_arg cmdarg;
  int ret;

  cmdarg.id = id;

  ret = commands_exec_sync(cmdbase, playback_start_id, playback_start_bh, &cmdarg);

  return ret;
}

int
player_playback_stop(void)
{
  int ret;

  ret = commands_exec_sync(cmdbase, playback_stop, NULL, NULL);
  return ret;
}

int
player_playback_abort(void)
{
  int ret;

  ret = commands_exec_sync(cmdbase, playback_abort, NULL, NULL);
  return ret;
}

int
player_playback_pause(void)
{
  int ret;

  ret = commands_exec_sync(cmdbase, playback_pause, playback_pause_bh, NULL);
  return ret;
}

/**
 * Flushes outputs and input buffer, but does not stop the input read loop. Used
 * by the pipe input when a track change is registered. Flushing outputs will
 * stop them, so the command is two-step, i.e. it starts them again.
 *
 * @return Returns 0 on success and a negative value on error
 */

int
player_playback_flush(void)
{
  int ret;

  ret = commands_exec_sync(cmdbase, playback_flush, NULL, NULL);
  if (ret < 0)
    return ret;

  ret = commands_exec_sync(cmdbase, speaker_start_all, NULL, NULL);
  return ret;
}

void
player_speaker_enumerate(spk_enum_cb cb, void *arg)
{
  struct spk_enum spk_enum;

  spk_enum.cb = cb;
  spk_enum.arg = arg;

  commands_exec_sync(cmdbase, speaker_enumerate, NULL, &spk_enum);
}

int
player_speaker_set(uint64_t *ids)
{
  struct speaker_set_param speaker_set_param;
  int ret;

  speaker_set_param.device_ids = ids;

  ret = commands_exec_sync(cmdbase, speaker_set, speaker_generic_bh, &speaker_set_param);

  return ret;
}

int
player_speaker_get_byid(struct player_speaker_info *spk, uint64_t id)
{
  struct speaker_get_param param;
  int ret;

  param.spk_id = id;
  param.spk_info = spk;

  ret = commands_exec_sync(cmdbase, speaker_get_byid, NULL, &param);
  return ret;
}

int
player_speaker_get_byactiveremote(struct player_speaker_info *spk, uint32_t active_remote)
{
  struct speaker_get_param param;
  int ret;

  param.active_remote = active_remote;
  param.spk_info = spk;

  ret = commands_exec_sync(cmdbase, speaker_get_byactiveremote, NULL, &param);
  return ret;
}

int
player_speaker_get_byaddress(struct player_speaker_info *spk, const char *address)
{
  struct speaker_get_param param;
  int ret;

  param.address = address;
  param.spk_info = spk;

  ret = commands_exec_sync(cmdbase, speaker_get_byaddress, NULL, &param);
  return ret;
}

int
player_speaker_get_byindex(struct player_speaker_info *spk, uint32_t index)
{
  struct speaker_get_param param;
  int ret;

  param.index = index;
  param.spk_info = spk;

  ret = commands_exec_sync(cmdbase, speaker_get_byindex, NULL, &param);
  return ret;
}

int
player_speaker_enable(uint64_t id, int include_tv, int volume)
{
  struct speaker_enable_param param;
  int ret;

  param.id = id;
  param.include_tv = include_tv;
  param.volume = volume;

  ret = commands_exec_sync(cmdbase, speaker_enable, speaker_generic_bh, &param);

  return ret;
}

int
player_speaker_disable(uint64_t id)
{
  int ret;

  ret = commands_exec_sync(cmdbase, speaker_disable, speaker_generic_bh, &id);

  return ret;
}

int
player_speaker_authorize(uint64_t id, const char *pin)
{
  struct speaker_attr_param param;

  param.spk_id = id;
  param.pin = pin;

  return commands_exec_sync(cmdbase, speaker_authorize, speaker_generic_bh, &param);
}

int
player_speaker_authorize_then_select(uint64_t id, const char *pin, int include_tv, int volume)
{
  struct speaker_authorize_select_param param;

  param.id = id;
  param.pin = pin;
  param.include_tv = include_tv;
  param.volume = volume;

  return commands_exec_sync(cmdbase, speaker_authorize_then_select, speaker_generic_bh, &param);
}

int
player_speaker_format_set(uint64_t id, enum media_format format)
{
  struct speaker_attr_param param;

  param.spk_id = id;
  param.format = format;

  return commands_exec_sync(cmdbase, speaker_format_set, speaker_generic_bh, &param);
}

int
player_speaker_offset_ms_set(uint64_t id, int offset_ms)
{
  struct speaker_attr_param param;

  param.spk_id = id;
  param.offset_ms = offset_ms;

  return commands_exec_sync(cmdbase, speaker_offset_ms_set, speaker_generic_bh, &param);
}

int
player_speaker_hold_set(uint64_t id, bool held)
{
  struct speaker_attr_param param;

  param.spk_id = id;
  param.held = held;

  return commands_exec_sync(cmdbase, speaker_hold_set, speaker_generic_bh, &param);
}

int
player_speaker_mute_set(uint64_t id, bool muted)
{
  struct speaker_attr_param param;

  memset(&param, 0, sizeof(param));
  param.spk_id = id;
  param.muted = muted;

  return commands_exec_sync(cmdbase, speaker_mute_set, speaker_generic_bh, &param);
}

int
player_speaker_mode_set(uint64_t id, enum output_mode mode)
{
  struct speaker_attr_param param;

  param.spk_id = id;
  param.mode   = mode;

  return commands_exec_sync(cmdbase, speaker_mode_set, speaker_generic_bh, &param);
}


int
player_volume_set(int vol)
{
  union player_arg cmdarg;
  int ret;

  if (vol < 0 || vol > 100)
    {
      DPRINTF(E_LOG, L_PLAYER, "Volume (%d) for player_volume_set is out of range\n", vol);
      return -1;
    }

  cmdarg.intval = vol;

  ret = commands_exec_sync(cmdbase, volume_set, volume_generic_bh, &cmdarg);

  return ret;
}

int
player_volume_setrel_speaker(uint64_t id, int relvol)
{
  struct speaker_attr_param vol_param;
  int ret;

  if (relvol < 0 || relvol > 100)
    {
      DPRINTF(E_LOG, L_PLAYER, "Volume (%d) for player_volume_setrel_speaker is out of range\n", relvol);
      return -1;
    }

  vol_param.spk_id = id;
  vol_param.volume = relvol;

  ret = commands_exec_sync(cmdbase, volume_setrel_speaker, volume_generic_bh, &vol_param);

  return ret;
}

int
player_volume_setabs_speaker(uint64_t id, int vol)
{
  struct speaker_attr_param vol_param;
  int ret;

  if (vol < 0 || vol > 100)
    {
      DPRINTF(E_LOG, L_PLAYER, "Volume (%d) for player_volume_setabs_speaker is out of range\n", vol);
      return -1;
    }

  vol_param.spk_id = id;
  vol_param.volume = vol;

  ret = commands_exec_sync(cmdbase, volume_setabs_speaker, volume_generic_bh, &vol_param);

  return ret;
}

int
player_volume_setraw_speaker(uint64_t id, const char *volstr)
{
  struct speaker_attr_param vol_param;
  int ret;

  vol_param.spk_id = id;
  vol_param.volstr  = volstr;
  vol_param.no_send_back = false;
  vol_param.asks = false;

  ret = commands_exec_sync(cmdbase, volume_setraw_speaker, volume_generic_bh, &vol_param);

  return ret;
}

int
player_volume_asked_speaker(uint64_t id, int vol)
{
  struct speaker_attr_param vol_param;

  if (vol < 0 || vol > 100)
    {
      DPRINTF(E_LOG, L_PLAYER, "Volume (%d) for player_volume_asked_speaker is out of range\n", vol);
      return -1;
    }

  memset(&vol_param, 0, sizeof(vol_param));
  vol_param.spk_id = id;
  vol_param.volume = vol;
  vol_param.asks = true;

  return commands_exec_sync(cmdbase, volume_setraw_speaker, volume_generic_bh, &vol_param);
}

int
player_volume_reported_speaker(uint64_t id, const char *volstr, const char *verb)
{
  struct volume_reported_param *param;

  CHECK_NULL(L_PLAYER, param = calloc(1, sizeof(*param)));
  param->id = id;
  snprintf(param->volstr, sizeof(param->volstr), "%s", volstr);
  snprintf(param->verb, sizeof(param->verb), "%s", verb);

  // On success the command frees param
  if (commands_exec_async(cmdbase, volume_reported_speaker, param) < 0)
    {
      free(param);
      return -1;
    }

  return 0;
}

int
player_volume_followed_report(struct player_followed_report *r)
{
  return commands_exec_sync(cmdbase, volume_followed_report, volume_followed_bh, r);
}

int
player_speaker_follow_state_set(uint64_t id, enum output_volume_follow state)
{
  struct follow_state_param p = { .id = id, .state = state };

  return commands_exec_sync(cmdbase, speaker_follow_state_set, speaker_follow_state_bh, &p);
}

/* ------------------- Non-blocking commands used by mDNS ------------------- */

int
player_device_add(void *device)
{
  union player_arg *cmdarg;
  int ret;

  cmdarg = calloc(1, sizeof(union player_arg));
  if (!cmdarg)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not allocate player_command\n");
      return -1;
    }

  cmdarg->device = device;

  ret = commands_exec_async(cmdbase, device_add, cmdarg);
  return ret;
}

int
player_device_remove(void *device)
{
  union player_arg *cmdarg;
  int ret;

  cmdarg = calloc(1, sizeof(union player_arg));
  if (!cmdarg)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not allocate player_command\n");
      return -1;
    }

  cmdarg->device = device;

  ret = commands_exec_async(cmdbase, device_remove_family, cmdarg);
  return ret;
}


/* ---------------------------- Thread: player ------------------------------ */

static void *
player(void *arg)
{
  thread_setname("player");

  event_base_dispatch(evbase_player);

  if (!player_exit)
    DPRINTF(E_LOG, L_PLAYER, "Player event loop terminated ahead of time!\n");

  pthread_exit(NULL);
}


/* ----------------------------- Thread: main ------------------------------- */

int
player_init(void)
{
  uint64_t interval;
  int ret;

  speaker_autoselect = config_get_bool("speaker_autoselect", false);
  clear_queue_on_stop_disabled = config_get_bool("clear_queue_on_stop_disable", false);
  device_removal_grace_secs = config_get_int("device_removal_grace_period", PLAYER_DEVICE_REMOVAL_GRACE_DEFAULT);
  if (device_removal_grace_secs < 0 || device_removal_grace_secs > PLAYER_DEVICE_REMOVAL_GRACE_MAX)
    {
      DPRINTF(E_WARN, L_PLAYER,
              "device_removal_grace_period must be 0..%d; using default %d\n",
              PLAYER_DEVICE_REMOVAL_GRACE_MAX, PLAYER_DEVICE_REMOVAL_GRACE_DEFAULT);
      device_removal_grace_secs = PLAYER_DEVICE_REMOVAL_GRACE_DEFAULT;
    }

  db_queue_set_pipe(config_get_str("pipe_path", NULL));

  player_state = PLAY_STOPPED;

  // Determine if the resolution of the system timer is > or < the size
  // of an audio packet. NOTE: this assumes the system clock resolution
  // is less than one second.
  if (clock_getres(CLOCK_MONOTONIC, &player_timer_res) < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not get the system timer resolution.\n");
      return -1;
    }

  if (!config_get_bool("high_resolution_clock", true))
    {
      DPRINTF(E_INFO, L_PLAYER, "High resolution clock not enabled on this system (res is %ld)\n", player_timer_res.tv_nsec);
      player_timer_res.tv_nsec = 10 * PLAYER_TICK_INTERVAL * 1000000;
    }

  // Set the tick interval for the playback timer
  interval = MAX(player_timer_res.tv_nsec, PLAYER_TICK_INTERVAL * 1000000);
  player_tick_interval.tv_nsec = interval;

  pb_write_deficit_max = (PLAYER_WRITE_BEHIND_MAX * 1000000 / interval);

  // Create the playback timer
#ifdef HAVE_TIMERFD
  pb_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  ret = pb_timer_fd;
#else
  ret = timer_create(CLOCK_MONOTONIC, NULL, &pb_timer);
#endif
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not create playback timer: %s\n", strerror(errno));
      return -1;
    }

  CHECK_NULL(L_PLAYER, evbase_player = event_base_new());
#ifdef HAVE_TIMERFD
  CHECK_NULL(L_PLAYER, pb_timer_ev = event_new(evbase_player, pb_timer_fd, EV_READ | EV_PERSIST, playback_cb, NULL));
#else
  CHECK_NULL(L_PLAYER, pb_timer_ev = event_new(evbase_player, SIGALRM, EV_SIGNAL | EV_PERSIST, playback_cb, NULL));
#endif
  CHECK_NULL(L_PLAYER, cmdbase = commands_base_new(evbase_player, NULL));

  CHECK_NULL(L_PLAYER, device_gc_ev = evtimer_new(evbase_player, device_gc_cb, NULL));
  {
    struct timeval gc_tv = { PLAYER_DEVICE_GC_INTERVAL, 0 };
    evtimer_add(device_gc_ev, &gc_tv);
  }

  ret = outputs_init();
  outputs_flush_done_cb_set(flush_done_cb);
  outputs_phase_cb_set(phase_announce_cb);
  outputs_stop_cb_set(device_op_cb);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_PLAYER, "Output initiation failed\n");
      goto error_evbase_free;
    }

  ret = input_init();
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_PLAYER, "Input initiation failed\n");
      goto error_outputs_deinit;
    }

  ret = pthread_create(&tid_player, NULL, player, NULL);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_PLAYER, "Could not spawn player thread: %s\n", strerror(errno));
      goto error_input_deinit;
    }

  return 0;

 error_input_deinit:
  input_deinit();
 error_outputs_deinit:
  outputs_deinit();
 error_evbase_free:
  commands_base_free(cmdbase);
  event_free(pb_timer_ev);
  event_free(device_gc_ev);
  event_base_free(evbase_player);
#ifdef HAVE_TIMERFD
  close(pb_timer_fd);
#else
  timer_delete(pb_timer);
#endif
  return -1;
}

void
player_set_device_removal_grace_secs(int secs)
{
  device_removal_grace_secs = secs;
}

void
player_deinit(void)
{
  int ret;

  // First stop the inputs from generating any new work -- a pipe watch that
  // fires after the abort below would start playback into a player that is
  // being torn down, posting commands into bases that are about to be freed.
  // Everything is still alive at this point, so the quiesce is safe, and the
  // abort then drains whatever was already queued.
  input_quiesce();

  player_playback_abort();

  // Stop the GC timer before tearing down output backends. The callback
  // traverses the device list; backends must not be freed under it.
  event_del(device_gc_ev);

#ifdef HAVE_TIMERFD
  close(pb_timer_fd);
#else
  timer_delete(pb_timer);
#endif

  input_deinit();

  // Run on the player thread rather than calling outputs_deinit() directly
  // here: backend session teardown started by player_playback_abort() above
  // is asynchronous and completes via callbacks on the player event loop.
  // Freeing backend state from this (main) thread concurrently with those
  // callbacks races over shared session objects, so marshal it onto the
  // player thread and let it serialize with the loop that owns the data.
  commands_exec_sync(cmdbase, outputs_deinit_cmd, NULL, NULL);

  player_exit = 1;
  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_player, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not join player thread: %s\n", strerror(errno));

      return;
    }

  event_free(pb_timer_ev);
  event_free(device_gc_ev);
  event_base_free(evbase_player);
}
