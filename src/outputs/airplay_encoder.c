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
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>

#include <event2/buffer.h>

#include "airplay_encoder.h"
#include "transcode.h"
#include "misc.h"
#include "logger.h"

// Input quantum: the encode feed size used by the buffered AirPlay 2
// transport, one RTP packet's worth of samples.
#define ENCODER_SAMPLES_PER_QUANTUM 352
// Log cadence for the periodic stats line, in encoded quanta.
#define ENCODER_STATS_INTERVAL_QUANTA 4096

// Backlog, in milliseconds of audio at the encoder's quality, above which a
// WARN is logged so a stall shows up in the log while it is still being
// absorbed. Rate-limited to ENCODER_STALL_WARN_INTERVAL_S per encoder.
#define ENCODER_STALL_WARN_MS 1000
#define ENCODER_STALL_WARN_INTERVAL_S 5

// Hard backstop on the in-queue, in milliseconds of audio at the encoder's
// quality. pcm_write() no longer drops audio to stay under a soft cap, so
// the queue is expected to grow while catching up after a stall; this is
// not a normal operating limit but a bound on memory under sustained
// overload (input arriving faster than the encoder can ever keep up, for
// as long as that lasts). Crossing it latches enc->failed, which trips the
// existing "Buffered encoder ... failed" teardown path upstream. The value
// sits comfortably above the largest legitimate catch-up backlog: the
// input buffer's capacity (3 s) plus the FIFO (well under 1 s), plus
// margin.
#define ENCODER_QUEUE_FAIL_MS 5000

struct airplay_encoder
{
  // --- immutable after start ---
  enum airplay_buffered_kind kind;
  struct media_quality quality;
  uint32_t quantum_bytes;     // bytes for one ENCODER_SAMPLES_PER_QUANTUM quantum
  pthread_t tid;

  // --- worker-thread-private (never touched by the caller after start) ---
  struct encode_ctx *encode_ctx;
  uint8_t *rawbuf; // quantum_bytes
  struct evbuffer *encoded_buffer;

  // --- shared, guarded by lock ---
  pthread_mutex_t lock;
  pthread_cond_t cond; // signaled on new PCM, stop and flush
  struct evbuffer *pcm;
  uint32_t pcm_samples;
  uint32_t flush_gen;
  bool stop;
  bool failed;
  struct airplay_encoded_frame *frames_head;
  struct airplay_encoded_frame *frames_tail;

  // stats
  uint32_t stat_quanta;
  uint32_t stat_frames;
  uint32_t stat_queue_highwater_samples;
  uint64_t stat_encode_ns_total;
  uint64_t stat_encode_ns_max;

  // Stall telemetry: rate-limits the backlog WARN in pcm_write(), and
  // tracks the peak backlog seen since the last one was logged.
  struct timespec stall_warn_last;
  bool stall_warned;
  uint32_t stall_warn_peak_samples;
};


/* ------------------------------- Helpers -------------------------------- */

static uint64_t
timespec_diff_ns(struct timespec *t0, struct timespec *t1)
{
  return (uint64_t)(t1->tv_sec - t0->tv_sec) * 1000000000UL + (uint64_t)(t1->tv_nsec - t0->tv_nsec);
}

// Values needed for the periodic stats line, snapshotted under the lock so
// the DPRINTF itself can run after it is released.
struct encoder_stats_snapshot
{
  uint32_t quanta;
  uint32_t frames;
  uint64_t avg_us;
  uint64_t max_us;
  uint32_t queue_hw_ms;
};

// Caller holds the lock. True (with *out filled in) every
// ENCODER_STATS_INTERVAL_QUANTA quanta, i.e. when the periodic debug stats
// line is due.
static bool
encoder_stats_snapshot_take(struct airplay_encoder *enc, struct encoder_stats_snapshot *out)
{
  if (enc->stat_quanta == 0 || enc->stat_quanta % ENCODER_STATS_INTERVAL_QUANTA != 0)
    return false;

  out->quanta = enc->stat_quanta;
  out->frames = enc->stat_frames;
  out->avg_us = (enc->stat_encode_ns_total / enc->stat_quanta) / 1000;
  out->max_us = enc->stat_encode_ns_max / 1000;
  out->queue_hw_ms = (uint32_t)((uint64_t)enc->stat_queue_highwater_samples * 1000 / enc->quality.sample_rate);

  return true;
}

// No lock required - operates only on the snapshot and the encoder's
// immutable kind.
static void
encoder_stats_log(struct encoder_stats_snapshot *s, enum airplay_buffered_kind kind)
{
  DPRINTF(E_DBG, L_AIRPLAY, "Encoder (kind %d): %u quanta, %u frames, avg %" PRIu64 " us, max %" PRIu64 " us, queue hw %u ms\n",
          kind, s->quanta, s->frames, s->avg_us, s->max_us, s->queue_hw_ms);
}

// Frees a detached frame list. The list must already be unlinked from the
// encoder, so no lock is required.
static void
encoder_frame_list_free(struct airplay_encoded_frame *frame)
{
  struct airplay_encoded_frame *next;

  for (; frame; frame = next)
    {
      next = frame->next;
      free(frame->data);
      free(frame);
    }
}


/* --------------------------- Worker thread body -------------------------- */

static void *
encoder_thread_run(void *arg)
{
  struct airplay_encoder *enc = arg;
  struct airplay_encoded_frame *frame;
  struct airplay_encoded_frame *frames_head;
  struct airplay_encoded_frame *frames_tail;
  struct airplay_encoded_frame *next;
  transcode_frame *xframe;
  struct encoder_stats_snapshot stats;
  struct timespec t0;
  struct timespec t1;
  uint32_t gen;
  int pkt_len;
  int len;

  thread_setname("apenc");

#if defined(__linux__)
  // Best effort: encode may lag (buffered outputs have seconds of anchor
  // lead); the player/RTSP thread must win CPU contention.
  //
  // This nice() value is RELATIVE to the process's current priority, not an
  // absolute one: it only matters together with whatever baseline the
  // owning systemd unit sets via Nice=. With the unit's Nice= at -12, this
  // thread lands at -2, i.e. still above default-priority (nice 0)
  // processes -- so lowering it here does not hand the CPU advantage back
  // to unrelated housekeeping work. If the unit's baseline Nice= is ever
  // changed, this thread's effective priority moves with it.
  errno = 0;
  if (nice(10) == -1 && errno != 0)
    DPRINTF(E_DBG, L_AIRPLAY, "Could not lower encoder thread priority: %s\n", strerror(errno));
#endif

  pthread_mutex_lock(&enc->lock);

  while (1)
    {
      while (!enc->stop && enc->pcm_samples < ENCODER_SAMPLES_PER_QUANTUM)
        pthread_cond_wait(&enc->cond, &enc->lock);

      if (enc->stop)
        break;

      gen = enc->flush_gen;
      evbuffer_remove(enc->pcm, enc->rawbuf, enc->quantum_bytes);
      enc->pcm_samples -= ENCODER_SAMPLES_PER_QUANTUM;

      pthread_mutex_unlock(&enc->lock);

      clock_gettime(CLOCK_MONOTONIC, &t0);

      len = -1;
      xframe = transcode_frame_new(enc->rawbuf, enc->quantum_bytes, ENCODER_SAMPLES_PER_QUANTUM, &enc->quality);
      if (xframe)
        {
          len = transcode_encode(enc->encoded_buffer, enc->encode_ctx, xframe, 0);
          transcode_frame_free(xframe);
        }

      clock_gettime(CLOCK_MONOTONIC, &t1);

      pthread_mutex_lock(&enc->lock);

      if (len < 0)
        {
          DPRINTF(E_LOG, L_AIRPLAY, "Encoder (kind %d): fatal encode error\n", enc->kind);
          enc->failed = true;
          break;
        }

      enc->stat_quanta++;
      enc->stat_encode_ns_total += timespec_diff_ns(&t0, &t1);
      if (timespec_diff_ns(&t0, &t1) > enc->stat_encode_ns_max)
        enc->stat_encode_ns_max = timespec_diff_ns(&t0, &t1);

      if (len == 0)
        {
          // Not enough input samples yet to complete an encoded frame (AAC
          // accumulates to a 1024-sample frame).
          if (encoder_stats_snapshot_take(enc, &stats))
            {
              pthread_mutex_unlock(&enc->lock);
              encoder_stats_log(&stats, enc->kind);
              pthread_mutex_lock(&enc->lock);
            }
          continue;
        }

      // One transcode_encode() call can flush more than one encoded frame
      // (e.g. two short ALAC frames for one longer input push); each must be
      // split out and delivered separately. Popping the per-packet size FIFO
      // must happen on this thread - it lives inside the encode context and
      // is filled by the same call that just ran.
      pthread_mutex_unlock(&enc->lock);

      frames_head = NULL;
      frames_tail = NULL;

      while (len > 0)
        {
          pkt_len = transcode_encode_packet_size_next(enc->encode_ctx);
          if (pkt_len <= 0 || pkt_len > len)
            pkt_len = len;

          frame = malloc(sizeof(struct airplay_encoded_frame));
          if (!frame)
            {
              evbuffer_drain(enc->encoded_buffer, len);
              // Partial chain is freed at the len < 0 merge point below.
              len = -1;
              break;
            }

          frame->data = malloc(pkt_len);
          if (!frame->data)
            {
              free(frame);
              evbuffer_drain(enc->encoded_buffer, len);
              // Partial chain is freed at the len < 0 merge point below.
              len = -1;
              break;
            }

          evbuffer_remove(enc->encoded_buffer, frame->data, pkt_len);
          frame->len = pkt_len;
          frame->next = NULL;

          if (frames_tail)
            frames_tail->next = frame;
          else
            frames_head = frame;
          frames_tail = frame;

          len -= pkt_len;
        }

      pthread_mutex_lock(&enc->lock);

      if (len < 0)
        {
          DPRINTF(E_LOG, L_AIRPLAY, "Encoder (kind %d): out of memory splitting encoded frames\n", enc->kind);
          // The chain is already freed on the paths that set len negative;
          // freeing it (again) here makes that invariant local to this
          // merge point instead of something every exit from the loop above
          // has to uphold - a no-op at runtime on today's paths.
          while (frames_head)
            {
              next = frames_head->next;
              free(frames_head->data);
              free(frames_head);
              frames_head = next;
            }
          frames_tail = NULL;
          enc->failed = true;
          break;
        }

      if (gen == enc->flush_gen)
        {
          for (frame = frames_head; frame; frame = frame->next)
            enc->stat_frames++;

          if (frames_head)
            {
              if (enc->frames_tail)
                enc->frames_tail->next = frames_head;
              else
                enc->frames_head = frames_head;
              enc->frames_tail = frames_tail;
            }
        }
      else
        {
          // Flushed while this quantum was in flight - discard, the player
          // no longer expects frames from before the flush generation.
          encoder_frame_list_free(frames_head);
        }

      if (encoder_stats_snapshot_take(enc, &stats))
        {
          pthread_mutex_unlock(&enc->lock);
          encoder_stats_log(&stats, enc->kind);
          pthread_mutex_lock(&enc->lock);
        }
    }

  pthread_mutex_unlock(&enc->lock);

  return NULL;
}


/* --------------------------------- API ----------------------------------- */

int
airplay_encoder_start(struct airplay_encoder **enc_p, enum airplay_buffered_kind kind, struct media_quality *quality)
{
  struct airplay_encoder *enc;
  struct decode_ctx *src_ctx;
  struct transcode_encode_setup_args encode_args = { 0 };
  enum transcode_profile profile;
  bool alac24 = (kind == AIRPLAY_BUFFERED_KIND_ALAC24);
  int ret;

  switch (kind)
    {
      case AIRPLAY_BUFFERED_KIND_ALAC24:          profile = XCODE_ALAC48K_24_STEREO; break;
      case AIRPLAY_BUFFERED_KIND_SURROUND_STEREO: profile = XCODE_AAC48K_51_STEREO;  break;
      case AIRPLAY_BUFFERED_KIND_SURROUND_UPMIX:  profile = XCODE_AAC48K_51_DECODE;  break;
      case AIRPLAY_BUFFERED_KIND_AAC_STEREO:      profile = XCODE_AAC48K_STEREO;     break;
      case AIRPLAY_BUFFERED_KIND_AAC44_STEREO:    profile = XCODE_AAC44K_STEREO;     break;
      case AIRPLAY_BUFFERED_KIND_NONE:
      default:                                    profile = XCODE_ALAC;             break;
    }

  src_ctx = transcode_decode_setup_raw(alac24 ? XCODE_PCM32 : XCODE_PCM16, quality);
  if (!src_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Encoder (kind %d): could not create decoding context\n", kind);
      return -1;
    }

  encode_args.profile = profile;
  encode_args.quality = quality;
  encode_args.src_ctx = src_ctx;

  enc = calloc(1, sizeof(struct airplay_encoder));
  if (!enc)
    {
      transcode_decode_cleanup(&src_ctx);
      return -1;
    }

  enc->encode_ctx = transcode_encode_setup(encode_args);
  transcode_decode_cleanup(&src_ctx);
  if (!enc->encode_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Encoder (kind %d): could not create encoding context\n", kind);
      free(enc);
      return -1;
    }

  enc->kind = kind;
  enc->quality = *quality;
  enc->quantum_bytes = STOB(ENCODER_SAMPLES_PER_QUANTUM, quality->bits_per_sample, quality->channels);

  enc->rawbuf = malloc(enc->quantum_bytes);
  enc->encoded_buffer = evbuffer_new();
  enc->pcm = evbuffer_new();
  if (!enc->rawbuf || !enc->encoded_buffer || !enc->pcm)
    {
      transcode_encode_cleanup(&enc->encode_ctx);
      free(enc->rawbuf);
      if (enc->encoded_buffer)
        evbuffer_free(enc->encoded_buffer);
      if (enc->pcm)
        evbuffer_free(enc->pcm);
      free(enc);
      return -1;
    }

  CHECK_ERR(L_AIRPLAY, mutex_init(&enc->lock));
  CHECK_ERR(L_AIRPLAY, pthread_cond_init(&enc->cond, NULL));

  ret = pthread_create(&enc->tid, NULL, encoder_thread_run, enc);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Encoder (kind %d): could not spawn thread: %s\n", kind, strerror(ret));
      transcode_encode_cleanup(&enc->encode_ctx);
      pthread_mutex_destroy(&enc->lock);
      pthread_cond_destroy(&enc->cond);
      free(enc->rawbuf);
      evbuffer_free(enc->encoded_buffer);
      evbuffer_free(enc->pcm);
      free(enc);
      return -1;
    }

  *enc_p = enc;
  return 0;
}

void
airplay_encoder_stop(struct airplay_encoder **enc_p)
{
  struct airplay_encoder *enc = *enc_p;
  uint64_t avg_us;
  uint64_t max_us;
  uint32_t queue_hw_ms;

  if (!enc)
    return;

  pthread_mutex_lock(&enc->lock);
  enc->stop = true;
  pthread_cond_signal(&enc->cond);
  pthread_mutex_unlock(&enc->lock);

  pthread_join(enc->tid, NULL);

  // The worker thread has exited, so enc is now only touched by this
  // (the caller's) thread and no lock is needed to read it.
  avg_us = enc->stat_quanta ? (enc->stat_encode_ns_total / enc->stat_quanta) / 1000 : 0;
  max_us = enc->stat_encode_ns_max / 1000;
  queue_hw_ms = (uint32_t)((uint64_t)enc->stat_queue_highwater_samples * 1000 / enc->quality.sample_rate);
  DPRINTF(E_INFO, L_AIRPLAY, "Encoder (kind %d) stopped: %u quanta, %u frames, avg %" PRIu64 " us, max %" PRIu64 " us, queue hw %u ms\n",
          enc->kind, enc->stat_quanta, enc->stat_frames, avg_us, max_us, queue_hw_ms);

  transcode_encode_cleanup(&enc->encode_ctx);
  free(enc->rawbuf);
  evbuffer_free(enc->encoded_buffer);
  evbuffer_free(enc->pcm);
  encoder_frame_list_free(enc->frames_head);
  pthread_mutex_destroy(&enc->lock);
  pthread_cond_destroy(&enc->cond);
  free(enc);

  *enc_p = NULL;
}

void
airplay_encoder_pcm_write(struct airplay_encoder *enc, uint8_t *buf, size_t bufsize, int samples)
{
  struct timespec now;
  uint32_t backlog_ms;
  uint32_t warn_peak_ms = 0;
  bool log_failed = false;
  bool log_stall = false;

  pthread_mutex_lock(&enc->lock);

  // Once failed, the encoder thread has (or is about to have) torn itself
  // down and the caller is tearing down the sessions above us; there is no
  // point queuing more PCM that will never be encoded.
  if (enc->failed)
    {
      pthread_mutex_unlock(&enc->lock);
      return;
    }

  evbuffer_add(enc->pcm, buf, bufsize);
  enc->pcm_samples += samples;
  if (enc->pcm_samples > enc->stat_queue_highwater_samples)
    enc->stat_queue_highwater_samples = enc->pcm_samples;

  backlog_ms = (uint32_t)((uint64_t)enc->pcm_samples * 1000 / enc->quality.sample_rate);

  if (backlog_ms > ENCODER_QUEUE_FAIL_MS)
    {
      // Sustained overload, not a stall we can catch up from: latch failed
      // so the caller tears the sessions down instead of the queue growing
      // without bound. Log once, on the transition.
      enc->failed = true;
      log_failed = true;
    }
  else if (backlog_ms > ENCODER_STALL_WARN_MS)
    {
      if (enc->pcm_samples > enc->stall_warn_peak_samples)
        enc->stall_warn_peak_samples = enc->pcm_samples;

      // clock_gettime() is only worth its cost while we're actually in (or
      // considering warning about) a stall, which should be rare.
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (!enc->stall_warned || timespec_diff_ns(&enc->stall_warn_last, &now) >= (uint64_t)ENCODER_STALL_WARN_INTERVAL_S * 1000000000UL)
        {
          log_stall = true;
          warn_peak_ms = (uint32_t)((uint64_t)enc->stall_warn_peak_samples * 1000 / enc->quality.sample_rate);
          enc->stall_warned = true;
          enc->stall_warn_last = now;
          enc->stall_warn_peak_samples = 0;
        }
    }
  else
    {
      // Backlog is back under the warn threshold: re-arm, so a later,
      // distinct stall warns right away (subject to the interval below)
      // instead of being silenced by this one's rate limit.
      enc->stall_warned = false;
      enc->stall_warn_peak_samples = 0;
    }

  pthread_cond_signal(&enc->cond);
  pthread_mutex_unlock(&enc->lock);

  if (log_failed)
    DPRINTF(E_LOG, L_AIRPLAY, "Encoder (kind %d): PCM backlog %u ms exceeds the hard limit, failing\n", enc->kind, backlog_ms);
  else if (log_stall)
    DPRINTF(E_WARN, L_AIRPLAY, "Encoder (kind %d): PCM backlog %u ms, catching up\n", enc->kind, warn_peak_ms);
}

struct airplay_encoded_frame *
airplay_encoder_frames_get(struct airplay_encoder *enc)
{
  struct airplay_encoded_frame *frames;

  pthread_mutex_lock(&enc->lock);

  frames = enc->frames_head;
  enc->frames_head = NULL;
  enc->frames_tail = NULL;

  pthread_mutex_unlock(&enc->lock);

  return frames;
}

void
airplay_encoder_frame_free(struct airplay_encoded_frame *frame)
{
  if (!frame)
    return;

  free(frame->data);
  free(frame);
}

void
airplay_encoder_flush(struct airplay_encoder *enc)
{
  pthread_mutex_lock(&enc->lock);

  enc->flush_gen++;
  evbuffer_drain(enc->pcm, -1);
  enc->pcm_samples = 0;

  encoder_frame_list_free(enc->frames_head);
  enc->frames_head = NULL;
  enc->frames_tail = NULL;

  // The queue is empty, so any earlier stall is over; a later one is a
  // distinct event and should warn on its own terms.
  enc->stall_warned = false;
  memset(&enc->stall_warn_last, 0, sizeof(enc->stall_warn_last));
  enc->stall_warn_peak_samples = 0;

  pthread_mutex_unlock(&enc->lock);
}

bool
airplay_encoder_failed(struct airplay_encoder *enc)
{
  bool failed;

  pthread_mutex_lock(&enc->lock);
  failed = enc->failed;
  pthread_mutex_unlock(&enc->lock);

  return failed;
}

uint32_t
airplay_encoder_pending_samples(struct airplay_encoder *enc)
{
  uint32_t samples;

  pthread_mutex_lock(&enc->lock);
  samples = enc->pcm_samples;
  pthread_mutex_unlock(&enc->lock);

  return samples;
}
