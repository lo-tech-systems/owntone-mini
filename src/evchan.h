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

#ifndef __EVCHAN_H__
#define __EVCHAN_H__

#include <stdbool.h>
#include <stdint.h>

#include "evchan_proto.h"

// The notifier: a local Unix socket on which a supervisor learns, without
// polling, that the list of outputs has changed and what the receivers have
// told us. It runs on a thread of its own and never calls into the player.
//
// Whether the channel exists is decided once, in evchan_init(), from the
// event_socket setting. When it is off there is no thread and every posting
// function returns -1 at once.

// Thread: main. Decides from settings whether the channel is enabled and, if
// so, starts its thread. Must run before the player and the threads that post
// events are created, and after the settings are loaded. Failure to start the
// thread leaves the channel disabled; it is not fatal.
int
evchan_init(void);

// Thread: main. Binds the socket and starts accepting. A failure is logged and
// leaves the thread running without a listener: posted events are dropped.
void
evchan_start(void);

// Thread: main. Tells every client the daemon is stopping, closes them and the
// listener and removes the socket. The thread keeps running and drops later
// posts. Safe to call more than once.
void
evchan_shutdown(void);

// Thread: main. Stops and joins the thread. Must run after every thread that
// posts events, and the player thread, has been joined.
void
evchan_deinit(void);

// True once the socket is listening. Safe from any thread.
bool
evchan_enabled(void);

// The configured socket path. Fixed at evchan_init(); safe from any thread.
const char *
evchan_socket_path(void);

// Any thread but the player. Reports a receiver command or a device volume to
// the connected supervisor, or drops it when none is connected. Never blocks:
// one write to the notifier's command pipe, which a running loop drains.
// Returns 0 if handed over, -1 if the channel is disabled.
//
// speaker_id is the id of the output row the event is reported under: for a
// device that has no row of its own (a hidden HomePod behind an Apple TV, the
// second speaker of a stereo pair) that is the visible output standing for it.
// member_id is the id of the device the event actually came from, the same as
// speaker_id when the device has its own row; it is carried for logs only.
int
evchan_event_receiver_command(uint64_t speaker_id, uint64_t member_id, const char *name, const char *verb);

int
evchan_event_device_volume(uint64_t speaker_id, uint64_t member_id, const char *name, const char *verb, const char *value);

#endif /* !__EVCHAN_H__ */
