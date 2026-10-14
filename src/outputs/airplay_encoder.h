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

#ifndef __AIRPLAY_ENCODER_H__
#define __AIRPLAY_ENCODER_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "misc.h"           // struct media_quality
#include "airplay_common.h" // enum airplay_buffered_kind

struct airplay_encoder;

// One encoded codec frame handed back to the player thread. Singly linked,
// FIFO order == encode order.
struct airplay_encoded_frame
{
  uint8_t *data;
  size_t len;
  struct airplay_encoded_frame *next;
};

// Spawns the encoder thread, which makes the transcode contexts itself so the
// caller never waits for them. PCM written before they are ready queues. A
// failure to make them is therefore not reported here but through
// airplay_encoder_failed(). quality is the ams input quality (post
// quality-subscription). Returns 0 on success, -1 on error.
int
airplay_encoder_start(struct airplay_encoder **enc, enum airplay_buffered_kind kind, struct media_quality *quality);

// Signals the encoder to stop. With wait set the caller is blocked until the
// thread has exited and freed everything (only for shutdown, when no audio is
// playing, worst case one in-flight quantum encode, ~30 ms for 5.1 upmix);
// otherwise the thread is left to finish its current quantum and to free its
// state, including queued PCM and unclaimed frames, on its own, and the caller
// returns at once. *enc is NULL on return either way, and the caller must not
// use the encoder again. Safe to call with *enc == NULL.
void
airplay_encoder_stop(struct airplay_encoder **enc, bool wait);

// Blocks until every encoder thread that was started has finished freeing
// itself, or for a couple of seconds at most. For shutdown, after all
// encoders have been stopped, so that no encoder thread runs on while the
// rest of the program is torn down.
void
airplay_encoder_wait_all(void);

// Player thread. Copies PCM into the in-queue; never blocks and never drops
// audio to catch up. The queue is only bounded as a memory backstop against
// sustained overload: past that point pcm_write() stops queuing and the
// encoder is marked failed (airplay_encoder_failed()) so the caller tears
// its sessions down. samples must describe buf per quality given at start.
void
airplay_encoder_pcm_write(struct airplay_encoder *enc, uint8_t *buf, size_t bufsize, int samples);

// Player thread. Detaches and returns the entire ready list (FIFO), or NULL.
// Caller must send/free each frame via airplay_encoder_frame_free().
struct airplay_encoded_frame *
airplay_encoder_frames_get(struct airplay_encoder *enc);

void
airplay_encoder_frame_free(struct airplay_encoded_frame *frame);

// Player thread. Discards all queued PCM and all ready frames, and marks any
// in-flight quantum stale (its frames will be discarded when the worker posts
// them). Does not block, does not reset the transcode context (parity with
// FLUSHBUFFERED, which also keeps encoder state).
void
airplay_encoder_flush(struct airplay_encoder *enc);

// True if the encoder hit a fatal encode error; player must fail the
// sessions on this ams (deferred). Sticky until stop.
bool
airplay_encoder_failed(struct airplay_encoder *enc);

// Samples written but not yet returned as frames (in-queue only). Used by
// timestamp_set() for buffered ams.
uint32_t
airplay_encoder_pending_samples(struct airplay_encoder *enc);

#endif /* !__AIRPLAY_ENCODER_H__ */
