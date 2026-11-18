/*
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
 */

/*
 * Minimal DACP control receiver.
 *
 * AirPlay receivers discover the sender's classic remote-control identity
 * (the DACP-ID header we send them) and browse mDNS for a matching
 * "iTunes_Ctrl_<DACP-ID>" _dacp._tcp service to learn where to send that
 * control protocol's HTTP requests. The one we actually care about is a
 * volume change made on the receiver itself (e.g. with an Apple TV remote
 * or a HomePod's own controls), reported via a setproperty request or a
 * volumeup/volumedown request. This module implements just enough of the
 * protocol to receive and log those requests and apply the resulting
 * volume to the player; nothing else of DACP (now playing, queue browsing,
 * playback control, etc) is implemented.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "httpd_internal.h"
#include "evchan.h"
#include "logger.h"
#include "misc.h"
#include "player.h"

#define DACP_VOLUME_STEP 5


/* --------------------------------- HELPERS --------------------------------- */

// Finds the speaker a request is about via its Active-Remote header, which
// carries the id the receiver was given (as a header on its own outgoing
// requests) to identify which of possibly several sessions it belongs to.
static int
speaker_get(struct player_speaker_info *speaker, struct httpd_request *hreq)
{
  const char *remote;
  uint32_t active_remote;

  remote = httpd_header_find(hreq->in_headers, "Active-Remote");
  if (!remote || safe_atou32(remote, &active_remote) < 0)
    {
      DPRINTF(E_LOG, L_DACP, "DACP request from %s has invalid Active-Remote: '%s'\n", (hreq->peer_address ? hreq->peer_address : "?"), remote ? remote : "(none)");
      return -1;
    }

  if (player_speaker_get_byactiveremote(speaker, active_remote) < 0)
    {
      DPRINTF(E_LOG, L_DACP, "DACP request from %s has unknown Active-Remote: '%s'\n", (hreq->peer_address ? hreq->peer_address : "?"), remote);
      return -1;
    }

  return 0;
}

static int
clamp_pct(int value)
{
  return value < 0 ? 0 : (value > 100 ? 100 : value);
}

// Reports a volume the device set, after it has been applied. It is reported
// under the output row that stands for the speaker (the speaker itself unless it
// is a hidden member of a group), with the speaker's own id alongside. When the
// speaker is not known the id from the request is used, and 0 stands for all
// speakers. The report is dropped when no supervisor is listening, and posting
// never waits.
static void
volume_report(const struct player_speaker_info *speaker, uint64_t id, const char *verb, const char *value)
{
  int ret;

  if (speaker)
    ret = evchan_event_device_volume(speaker->row_id, speaker->id, speaker->row_name, verb, value);
  else
    ret = evchan_event_device_volume(id, id, NULL, verb, value);

  if (ret < 0)
    DPRINTF(E_DBG, L_DACP, "Volume '%s' %s not reported, no event channel\n", verb, value);
}

static void
volume_report_pct(const struct player_speaker_info *speaker, uint64_t id, const char *verb, int pct)
{
  char value[16];

  snprintf(value, sizeof(value), "%d", pct);
  volume_report(speaker, id, verb, value);
}

// Reports a command the receiver sent (or a wake its volume buttons made)
// under the output row that stands for it, as an AirPlay 2 receiver's
// commands are reported. Dropped when no supervisor is listening.
static void
receiver_command_post(const struct player_speaker_info *speaker, const char *verb)
{
  if (evchan_event_receiver_command(speaker->row_id, speaker->id, speaker->row_name, verb, "dacp") < 0)
    DPRINTF(E_DBG, L_DACP, "Receiver command '%s' from '%s' not reported, no event channel\n", verb, speaker->name);
}


/* ------------------------------- SETPROPERTY -------------------------------- */

struct setproperty_ctx {
  struct httpd_request *hreq;
  bool bad_active_remote;
};

static void
setproperty_devicevolume(struct setproperty_ctx *ctx, const char *key, const char *value)
{
  struct player_speaker_info speaker;
  int ret;

  if (speaker_get(&speaker, ctx->hreq) < 0)
    {
      ctx->bad_active_remote = true;
      return;
    }

  DPRINTF(E_INFO, L_DACP, "DACP: '%s' reports volume %s -> speaker '%s'\n", key, value, speaker.name);

  ret = player_volume_setraw_speaker(speaker.id, value);
  if (ret == 2)
    receiver_command_post(&speaker, "wake");
  else if (ret != 1)
    volume_report(&speaker, 0, key, value); // Recorded, or an error, reported as it always was
}

static void
setproperty_volume(struct setproperty_ctx *ctx, const char *key, const char *value)
{
  struct player_speaker_info speaker;
  const char *speaker_id_param;
  const struct player_speaker_info *known = NULL;
  uint64_t speaker_id;
  int volume;

  volume = clamp_pct(atoi(value));

  speaker_id_param = httpd_query_value_find(ctx->hreq->query, "speaker-id");
  if (speaker_id_param && safe_atou64(speaker_id_param, &speaker_id) == 0)
    {
      if (player_speaker_get_byid(&speaker, speaker_id) == 0)
	{
	  DPRINTF(E_INFO, L_DACP, "DACP: '%s' reports volume %s -> speaker '%s'\n", key, value, speaker.name);
	  known = &speaker;
	}
      else
	DPRINTF(E_INFO, L_DACP, "DACP: '%s' reports volume %s -> speaker '%" PRIu64 "'\n", key, value, speaker_id);

      player_volume_setabs_speaker(speaker_id, volume);

      volume_report_pct(known, speaker_id, key, volume);
      return;
    }

  DPRINTF(E_INFO, L_DACP, "DACP: '%s' reports volume %s -> speaker '%s'\n", key, value, "(all)");

  player_volume_set(volume);

  volume_report_pct(NULL, 0, key, volume);
}

static void
setproperty_cb(const char *key, const char *value, void *arg)
{
  struct setproperty_ctx *ctx = arg;

  if (!value)
    return;

  if (strcmp(key, "dmcp.device-volume") == 0)
    setproperty_devicevolume(ctx, key, value);
  else if (strcmp(key, "dmcp.volume") == 0)
    setproperty_volume(ctx, key, value);
  else
    DPRINTF(E_INFO, L_DACP, "DACP setproperty '%s=%s' ignored\n", key, value);
}

static int
dacp_reply_setproperty(struct httpd_request *hreq)
{
  struct setproperty_ctx ctx = { .hreq = hreq, .bad_active_remote = false };

  httpd_query_iterate(hreq->query, setproperty_cb, &ctx);

  if (ctx.bad_active_remote)
    {
      httpd_send_error(hreq, HTTP_BADREQUEST, "Bad Request");
      return -1;
    }

  httpd_send_reply(hreq, HTTP_NOCONTENT, "No Content", HTTPD_SEND_NO_GZIP);
  return 0;
}


/* --------------------------------- VOLUME ----------------------------------- */

static int
dacp_reply_volumeup(struct httpd_request *hreq)
{
  struct player_speaker_info speaker;
  int volume;
  int ret;

  if (speaker_get(&speaker, hreq) < 0)
    {
      httpd_send_error(hreq, HTTP_BADREQUEST, "Bad Request");
      return -1;
    }

  volume = clamp_pct(speaker.absvol + DACP_VOLUME_STEP);

  DPRINTF(E_INFO, L_DACP, "DACP: '%s' reports volume %d -> speaker '%s'\n", "volumeup", volume, speaker.name);

  // The level is worked out from the kept one, so a silent speaker wakes
  // at its own level
  ret = player_volume_asked_speaker(speaker.id, volume);
  if (ret == 2)
    receiver_command_post(&speaker, "wake");
  else
    volume_report_pct(&speaker, 0, "volumeup", volume);

  httpd_send_reply(hreq, HTTP_NOCONTENT, "No Content", HTTPD_SEND_NO_GZIP);
  return 0;
}

static int
dacp_reply_volumedown(struct httpd_request *hreq)
{
  struct player_speaker_info speaker;
  int volume;
  int ret;

  if (speaker_get(&speaker, hreq) < 0)
    {
      httpd_send_error(hreq, HTTP_BADREQUEST, "Bad Request");
      return -1;
    }

  volume = clamp_pct(speaker.absvol - DACP_VOLUME_STEP);

  DPRINTF(E_INFO, L_DACP, "DACP: '%s' reports volume %d -> speaker '%s'\n", "volumedown", volume, speaker.name);

  // The level is worked out from the kept one, so a silent speaker wakes
  // at its own level
  ret = player_volume_asked_speaker(speaker.id, volume);
  if (ret == 2)
    receiver_command_post(&speaker, "wake");
  else
    volume_report_pct(&speaker, 0, "volumedown", volume);

  httpd_send_reply(hreq, HTTP_NOCONTENT, "No Content", HTTPD_SEND_NO_GZIP);
  return 0;
}


/* --------------------------------- CATCH-ALL -------------------------------- */

// Anything else under /ctrl-int/ (session setup, now-playing polling, queue
// browsing etc) gets a harmless empty reply, so a receiver that tries the
// rest of DACP doesn't see what looks like a broken service.
static int
dacp_reply_notfound(struct httpd_request *hreq)
{
  httpd_send_reply(hreq, HTTP_NOCONTENT, "No Content", HTTPD_SEND_NO_GZIP);
  return 0;
}

static struct httpd_uri_map dacp_handlers[] =
  {
    { HTTPD_METHOD_GET | HTTPD_METHOD_POST, "^/ctrl-int/[[:digit:]]+/setproperty$", dacp_reply_setproperty },
    { HTTPD_METHOD_GET | HTTPD_METHOD_POST, "^/ctrl-int/[[:digit:]]+/volumeup$",    dacp_reply_volumeup },
    { HTTPD_METHOD_GET | HTTPD_METHOD_POST, "^/ctrl-int/[[:digit:]]+/volumedown$",  dacp_reply_volumedown },
    { HTTPD_METHOD_GET | HTTPD_METHOD_POST, "^/ctrl-int/",                          dacp_reply_notfound },

    { 0, NULL, NULL }
  };


/* --------------------------------- DACP ------------------------------------- */

static void
dacp_request(struct httpd_request *hreq)
{
  const char *remote;
  const char *query;

  remote = httpd_header_find(hreq->in_headers, "Active-Remote");
  query = strchr(hreq->uri, '?');

  // Log every request a receiver makes here: this module exists to receive
  // what receivers report back, so each report is worth a line.
  DPRINTF(E_INFO, L_DACP, "DACP request from %s: %s%s%s (Active-Remote %s)\n",
	  (hreq->peer_address ? hreq->peer_address : "?"), hreq->path, query ? "?" : "", query ? query + 1 : "",
	  remote ? remote : "(none)");

  if (!hreq->handler)
    {
      httpd_send_error(hreq, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  hreq->handler(hreq);
}

static int
dacp_init(void)
{
  return 0;
}

static void
dacp_deinit(void)
{
}

struct httpd_module httpd_dacp =
{
  .name = "DACP",
  .type = MODULE_DACP,
  .logdomain = L_DACP,
  .subpaths = { "/ctrl-int/", NULL },
  .handlers = dacp_handlers,
  .init = dacp_init,
  .deinit = dacp_deinit,
  .request = dacp_request,
};
