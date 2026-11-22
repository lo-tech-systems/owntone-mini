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

#ifndef __OUTPUT_PHASE_H__
#define __OUTPUT_PHASE_H__

#include <stdbool.h>

#include "output_failure.h"

// The phase of a speaker as clients see it, the operation in flight on it,
// and the rule that moves both on a request or a backend report. Pure: no
// device, no clock, no log, so every row can be tested on its own.

enum output_phase
{
  OUTPUT_PHASE_STOPPED,
  OUTPUT_PHASE_CONNECTING,
  OUTPUT_PHASE_CONNECTED,
  OUTPUT_PHASE_STREAMING,
  OUTPUT_PHASE_STOPPING,
  OUTPUT_PHASE_FAILED,
  OUTPUT_PHASE_PASSWORD,
};

enum output_op
{
  OUTPUT_OP_NONE,
  OUTPUT_OP_PROBE,
  OUTPUT_OP_START,
  OUTPUT_OP_STOP,
  OUTPUT_OP_AUTHORIZE,
};

// What a backend reported, in the order of enum output_device_state
enum output_report
{
  OUTPUT_REPORT_STOPPED,
  OUTPUT_REPORT_STARTUP,
  OUTPUT_REPORT_CONNECTED,
  OUTPUT_REPORT_STREAMING,
  OUTPUT_REPORT_FAILED,
  OUTPUT_REPORT_PASSWORD,
};

enum output_request
{
  OUTPUT_REQ_SELECT,
  OUTPUT_REQ_DESELECT,
  OUTPUT_REQ_AUTHORIZE,
  OUTPUT_REQ_MODE_SWITCH,
  // The wait for a PIN has run its time; the PIN on the receiver is taken
  // as gone
  OUTPUT_REQ_PIN_EXPIRED,
};

struct output_phase_input
{
  enum output_phase phase;
  enum output_op op;
  // Reports only: the report's slot carries the device's current generation,
  // so it belongs to the operation in flight (or to none, when none is)
  bool report_current_gen;
  // Reports only: the reason the backend wrote with a FAILED or PASSWORD
  // report, NONE otherwise
  enum output_failure_reason report_reason;
  bool selected;
  // The player is playing
  bool playing;
  // The device has a session, after the report was applied
  bool has_session;
  bool stop_after_start;
  bool restart_after_stop;
  bool upgrade_after_probe;
  bool then_select;
  bool pin_pending;
  // Requests only: a select or deselect a user asked for, not an automatic
  // one
  bool explicit_request;
  // Mode switch only: the effective backend would change
  bool backend_changes;
  // Reports only: the player may retry a failure now (its budget and its
  // settings allow it); the rule adds whether the reason is worth retrying
  bool retry_allowed;
};

struct output_phase_result
{
  // The phase once the next operation, if any, has begun
  enum output_phase phase;
  // The phase the event itself reached, before the next operation began;
  // equal to phase when there is no next operation. Each of the two that
  // differs from the phase before is announced, in this order
  enum output_phase mid_phase;
  // The operation to begin now, NONE for none
  enum output_op next_op;
  // The operation in flight has ended
  bool clear_op;
  // A failed or refused start is deselected, unless it is retried
  bool deselect;
  // The failure is retried: the device stays selected and the player starts
  // it again after a delay
  bool retry;
  bool set_stop_after_start, clear_stop_after_start;
  bool set_restart_after_stop, clear_restart_after_stop;
  bool set_upgrade_after_probe, clear_upgrade_after_probe;
  bool clear_then_select;
  bool set_pin_pending, clear_pin_pending;
  // The reason the device is to carry, NONE when the event sets none
  enum output_failure_reason reason;
  // The phase, the selection or the PIN state changed, so clients are told
  bool announce;
};

void
output_phase_on_report(const struct output_phase_input *in, enum output_report report, struct output_phase_result *out);

void
output_phase_on_request(const struct output_phase_input *in, enum output_request req, struct output_phase_result *out);

// "stopped", "connecting", "connected", "streaming", "stopping", "failed",
// "password"
const char *
output_phase_name(enum output_phase phase);

#endif /* !__OUTPUT_PHASE_H__ */
