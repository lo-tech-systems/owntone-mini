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

#ifndef __AIRPLAY_BUFFERED_H__
#define __AIRPLAY_BUFFERED_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct event_base;

struct airplay_buffered_stream;

// Called once on the event base when the connection attempt has an outcome.
// The callback may free the stream.
typedef void (*airplay_buffered_connect_cb)(void *arg, bool connected);

// Starts the connection to address:data_port and sets up the ChaCha20-Poly1305
// cipher for the type-103 buffered-audio TCP transport. The connect is not
// waited for: on return *bs is set, owns the socket/evbuffer/cipher, and the
// attempt is under way. connect_cb is called from the event base, exactly
// once, when the attempt has succeeded, failed or timed out; it is never
// called from inside this function. Returns 0 if the attempt was started, -1
// (with *bs untouched and no callback to come) if the socket could not be made
// or the attempt could not be started. name is the receiver, for log lines.
int
airplay_buffered_start(struct airplay_buffered_stream **bs, struct event_base *evbase, const char *name, const char *address, unsigned short data_port, const uint8_t *shk, size_t shk_len, uint32_t format_id, airplay_buffered_connect_cb connect_cb, void *connect_cb_arg);

// Frames, encrypts and queues one AAC frame for asynchronous transmission.
// Never blocks the calling thread. Returns 0 on success, -1 if the stream
// has failed or its connection is not up (caller should treat this as a
// session failure).
int
airplay_buffered_write(struct airplay_buffered_stream *bs, const uint8_t *frame, size_t len, uint32_t seqnum, uint32_t rtptime, bool marker);

// Returns the number of bytes currently queued for transmission (i.e. not
// yet written to the socket).
size_t
airplay_buffered_pending(struct airplay_buffered_stream *bs);

// Closes the socket and frees all resources owned by bs.
void
airplay_buffered_stop(struct airplay_buffered_stream *bs);

#endif  /* !__AIRPLAY_BUFFERED_H__ */
