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

#include <string.h>

#include "output_phase.h"

// Rules every path below keeps:
//  - a device waiting for a PIN is never started by anything automatic, since
//    a new PIN start replaces the PIN on the receiver's screen; only the
//    user's own select, or the select that came with the PIN, starts it
//  - a stop is never cut short: a deselect while connecting lets the start
//    run on and stops it once it has a session
//  - after every report, a selected device with no session and no operation
//    is started again while the player plays

static void
result_init(const struct output_phase_input *in, struct output_phase_result *out)
{
  memset(out, 0, sizeof(*out));
  out->phase = in->phase;
  out->mid_phase = in->phase;
  out->next_op = OUTPUT_OP_NONE;
  out->reason = OUTPUT_FAILURE_NONE;
}

// The phase the event reached, before any next operation
static void
reach(struct output_phase_result *out, enum output_phase phase)
{
  out->mid_phase = phase;
  out->phase = phase;
}

static void
begin(struct output_phase_result *out, enum output_op op)
{
  out->next_op = op;
  out->phase = (op == OUTPUT_OP_STOP) ? OUTPUT_PHASE_STOPPING : OUTPUT_PHASE_CONNECTING;
}

// A start while the player is not playing only probes the device
static enum output_op
start_op(const struct output_phase_input *in)
{
  return in->playing ? OUTPUT_OP_START : OUTPUT_OP_PROBE;
}

static enum output_failure_reason
reason_or(enum output_failure_reason reason, enum output_failure_reason fallback)
{
  return (reason != OUTPUT_FAILURE_NONE) ? reason : fallback;
}

static bool
pin_pending_after(const struct output_phase_input *in, const struct output_phase_result *out)
{
  return (in->pin_pending || out->set_pin_pending) && !out->clear_pin_pending;
}

static bool
selected_after(const struct output_phase_input *in, const struct output_phase_result *out)
{
  return in->selected && !out->deselect;
}

// The stop has ended (or the start a deselect was waiting for ended with no
// session): a select or a backend-changing mode switch made meanwhile starts
// the device again, unless it waits for a PIN
static void
stopped_restart(const struct output_phase_input *in, struct output_phase_result *out)
{
  if (!in->restart_after_stop)
    return;

  out->clear_restart_after_stop = true;
  if (selected_after(in, out) && !pin_pending_after(in, out))
    begin(out, start_op(in));
}

// A start or a probe that ended
static void
report_start(const struct output_phase_input *in, enum output_report report, struct output_phase_result *out)
{
  switch (report)
    {
      case OUTPUT_REPORT_CONNECTED:
	reach(out, OUTPUT_PHASE_CONNECTED);
	out->clear_op = true;
	out->clear_pin_pending = true;
	break;

      case OUTPUT_REPORT_STREAMING:
	reach(out, OUTPUT_PHASE_STREAMING);
	out->clear_op = true;
	out->clear_pin_pending = true;
	break;

      case OUTPUT_REPORT_FAILED:
	reach(out, OUTPUT_PHASE_FAILED);
	out->clear_op = true;
	out->deselect = true;
	out->reason = reason_or(in->report_reason, OUTPUT_FAILURE_REFUSED);
	break;

      case OUTPUT_REPORT_PASSWORD:
	reach(out, OUTPUT_PHASE_PASSWORD);
	out->clear_op = true;
	out->deselect = true;
	out->set_pin_pending = true;
	out->reason = reason_or(in->report_reason, OUTPUT_FAILURE_PIN_REQUIRED);
	break;

      case OUTPUT_REPORT_STARTUP:
	// A step of the start moves nothing; a probe ends with a startup
	// report and no session
	if (in->has_session)
	  break;
	// fallthrough
      case OUTPUT_REPORT_STOPPED:
	reach(out, OUTPUT_PHASE_STOPPED);
	out->clear_op = true;
	out->clear_pin_pending = true;
	if (in->upgrade_after_probe)
	  out->clear_upgrade_after_probe = true;
	// A probe done while the player started playing, or after a real start
	// was asked for, is followed by the start
	if (in->selected && (in->upgrade_after_probe || in->playing))
	  begin(out, OUTPUT_OP_START);
	break;
    }
}

// An authorise that ended. The select that came with the PIN survives only a
// PIN the receiver refused, which the user is waiting to see replaced by a new
// one; every other end of the authorise clears it, and one that timed out is
// not started again, since the PIN on screen may be the one being typed
static void
report_authorize(const struct output_phase_input *in, enum output_report report, struct output_phase_result *out)
{
  switch (report)
    {
      case OUTPUT_REPORT_CONNECTED:
      case OUTPUT_REPORT_STREAMING:
	reach(out, (report == OUTPUT_REPORT_CONNECTED) ? OUTPUT_PHASE_CONNECTED : OUTPUT_PHASE_STREAMING);
	out->clear_op = true;
	out->clear_pin_pending = true;
	out->clear_then_select = in->then_select;
	break;

      case OUTPUT_REPORT_STARTUP:
	if (in->has_session)
	  break;
	// fallthrough
      case OUTPUT_REPORT_STOPPED:
	// Pairing done
	reach(out, OUTPUT_PHASE_STOPPED);
	out->clear_op = true;
	out->clear_pin_pending = true;
	if (in->then_select)
	  {
	    out->clear_then_select = true;
	    begin(out, start_op(in));
	  }
	break;

      case OUTPUT_REPORT_FAILED:
      case OUTPUT_REPORT_PASSWORD:
	reach(out, OUTPUT_PHASE_PASSWORD);
	out->clear_op = true;
	out->set_pin_pending = true;
	out->reason = reason_or(in->report_reason, OUTPUT_FAILURE_PIN_INVALID);
	if (in->then_select)
	  {
	    out->clear_then_select = true;
	    // The start the request asked for, which shows a new PIN
	    if (out->reason == OUTPUT_FAILURE_PIN_INVALID)
	      begin(out, start_op(in));
	  }
	break;
    }
}

// A report while a deselect waits for the start in flight
static void
report_stop_after_start(const struct output_phase_input *in, enum output_report report, struct output_phase_result *out)
{
  switch (report)
    {
      case OUTPUT_REPORT_STARTUP:
	if (in->has_session)
	  break;
	// fallthrough
      case OUTPUT_REPORT_STOPPED:
      case OUTPUT_REPORT_FAILED:
      case OUTPUT_REPORT_PASSWORD:
	// The start ended with nothing to stop
	reach(out, OUTPUT_PHASE_STOPPED);
	out->clear_op = true;
	out->clear_stop_after_start = true;
	out->clear_pin_pending = true;
	out->reason = OUTPUT_FAILURE_CANCELLED;
	stopped_restart(in, out);
	break;

      case OUTPUT_REPORT_CONNECTED:
      case OUTPUT_REPORT_STREAMING:
	// The start succeeded: now it is stopped
	if (!in->has_session)
	  break;
	out->clear_op = true;
	out->clear_stop_after_start = true;
	out->clear_pin_pending = true;
	begin(out, OUTPUT_OP_STOP);
	break;
    }
}

// The reason an established session's failure carries: a reason that says
// something about the receiver is kept, the ways a connection can end are
// all a lost session
static enum output_failure_reason
lost_reason(enum output_failure_reason reason)
{
  switch (reason)
    {
      case OUTPUT_FAILURE_NONE:
      case OUTPUT_FAILURE_REFUSED:
      case OUTPUT_FAILURE_UNREACHABLE:
      case OUTPUT_FAILURE_TIMEOUT:
	return OUTPUT_FAILURE_LOST;
      default:
	return reason;
    }
}

// A report on a connected or streaming device with no operation in flight:
// an answer to a level or a flush, or the session's end. A session that
// fails stays selected; it is not deselected for a loss
static void
report_established(const struct output_phase_input *in, enum output_report report, struct output_phase_result *out)
{
  switch (report)
    {
      case OUTPUT_REPORT_CONNECTED:
	reach(out, OUTPUT_PHASE_CONNECTED);
	break;

      case OUTPUT_REPORT_STREAMING:
	reach(out, OUTPUT_PHASE_STREAMING);
	break;

      case OUTPUT_REPORT_FAILED:
	reach(out, OUTPUT_PHASE_FAILED);
	out->reason = lost_reason(in->report_reason);
	break;

      case OUTPUT_REPORT_PASSWORD:
	reach(out, OUTPUT_PHASE_PASSWORD);
	out->set_pin_pending = true;
	out->reason = reason_or(in->report_reason, OUTPUT_FAILURE_PIN_REQUIRED);
	break;

      case OUTPUT_REPORT_STARTUP:
      case OUTPUT_REPORT_STOPPED:
	if (!in->has_session)
	  reach(out, OUTPUT_PHASE_STOPPED);
	break;
    }
}

// The rule that keeps a start from being lost: a selected device that has no
// session and nothing in flight is started while the player plays
static void
post_report(const struct output_phase_input *in, struct output_phase_result *out)
{
  bool op_after;

  op_after = (in->op != OUTPUT_OP_NONE) && !out->clear_op;
  if (op_after || out->next_op != OUTPUT_OP_NONE)
    return;

  if (out->mid_phase != OUTPUT_PHASE_STOPPED || in->has_session || !in->playing)
    return;

  if (!selected_after(in, out) || pin_pending_after(in, out))
    return;

  begin(out, OUTPUT_OP_START);
}

static void
finish(const struct output_phase_input *in, struct output_phase_result *out)
{
  bool pin_before = in->pin_pending;
  bool pin_after = pin_pending_after(in, out);

  out->announce = (out->phase != in->phase) || (out->mid_phase != in->phase) || (out->deselect && in->selected) || (pin_before != pin_after);
}

void
output_phase_on_report(const struct output_phase_input *in, enum output_report report, struct output_phase_result *out)
{
  result_init(in, out);

  // A report of an older operation moves nothing; the device may still be
  // left without a session, which the post-report rule sees to
  if (!in->report_current_gen)
    goto out;

  switch (in->phase)
    {
      case OUTPUT_PHASE_CONNECTING:
	if (in->op == OUTPUT_OP_AUTHORIZE)
	  report_authorize(in, report, out);
	else
	  report_start(in, report, out);
	break;

      case OUTPUT_PHASE_STOPPING:
	if (in->stop_after_start)
	  report_stop_after_start(in, report, out);
	else if (report == OUTPUT_REPORT_STOPPED || !in->has_session)
	  {
	    reach(out, OUTPUT_PHASE_STOPPED);
	    out->clear_op = (in->op != OUTPUT_OP_NONE);
	    stopped_restart(in, out);
	  }
	break;

      case OUTPUT_PHASE_CONNECTED:
      case OUTPUT_PHASE_STREAMING:
	report_established(in, report, out);
	break;

      case OUTPUT_PHASE_STOPPED:
      case OUTPUT_PHASE_FAILED:
      case OUTPUT_PHASE_PASSWORD:
	// A session nothing here started, made known by its report
	if (in->has_session && report == OUTPUT_REPORT_CONNECTED)
	  reach(out, OUTPUT_PHASE_CONNECTED);
	else if (in->has_session && report == OUTPUT_REPORT_STREAMING)
	  reach(out, OUTPUT_PHASE_STREAMING);
	break;
    }

 out:
  post_report(in, out);
  finish(in, out);
}

void
output_phase_on_request(const struct output_phase_input *in, enum output_request req, struct output_phase_result *out)
{
  result_init(in, out);

  switch (req)
    {
      case OUTPUT_REQ_SELECT:
	if (in->pin_pending && !in->explicit_request)
	  break;

	switch (in->phase)
	  {
	    case OUTPUT_PHASE_STOPPED:
	    case OUTPUT_PHASE_FAILED:
	    case OUTPUT_PHASE_PASSWORD:
	      if (in->op == OUTPUT_OP_NONE)
		begin(out, start_op(in));
	      break;
	    case OUTPUT_PHASE_CONNECTING:
	      if (in->op == OUTPUT_OP_PROBE && in->playing && !in->upgrade_after_probe)
		out->set_upgrade_after_probe = true;
	      break;
	    case OUTPUT_PHASE_STOPPING:
	      // The start a deselect was waiting for is wanted again, unless a
	      // mode switch is waiting to stop it and start the device on the
	      // other backend; a stop already out runs to its end and the
	      // device starts after it
	      if (in->stop_after_start && !in->restart_after_stop)
		{
		  reach(out, OUTPUT_PHASE_CONNECTING);
		  out->clear_stop_after_start = true;
		}
	      else if (!in->restart_after_stop)
		out->set_restart_after_stop = true;
	      break;
	    case OUTPUT_PHASE_CONNECTED:
	    case OUTPUT_PHASE_STREAMING:
	      break;
	  }
	break;

      case OUTPUT_REQ_DESELECT:
	out->clear_then_select = in->then_select;

	switch (in->phase)
	  {
	    case OUTPUT_PHASE_CONNECTING:
	      // An authorise is not stopped: the keys it makes are kept, and
	      // the select it may carry is gone
	      if (in->op == OUTPUT_OP_AUTHORIZE)
		break;
	      reach(out, OUTPUT_PHASE_STOPPING);
	      out->set_stop_after_start = true;
	      out->clear_upgrade_after_probe = in->upgrade_after_probe;
	      break;
	    case OUTPUT_PHASE_CONNECTED:
	    case OUTPUT_PHASE_STREAMING:
	      if (in->op == OUTPUT_OP_NONE)
		begin(out, OUTPUT_OP_STOP);
	      break;
	    case OUTPUT_PHASE_STOPPING:
	      out->clear_restart_after_stop = in->restart_after_stop;
	      break;
	    case OUTPUT_PHASE_STOPPED:
	    case OUTPUT_PHASE_FAILED:
	    case OUTPUT_PHASE_PASSWORD:
	      break;
	  }
	break;

      case OUTPUT_REQ_AUTHORIZE:
	// Nothing else may be in flight: a start between its retries still
	// holds the report it will make
	if (in->op != OUTPUT_OP_NONE || in->has_session)
	  break;

	if (in->phase == OUTPUT_PHASE_STOPPED || in->phase == OUTPUT_PHASE_FAILED || in->phase == OUTPUT_PHASE_PASSWORD)
	  begin(out, OUTPUT_OP_AUTHORIZE);
	break;

      case OUTPUT_REQ_MODE_SWITCH:
	if (!in->backend_changes)
	  break;

	switch (in->phase)
	  {
	    case OUTPUT_PHASE_CONNECTING:
	      if (in->op == OUTPUT_OP_AUTHORIZE)
		break;
	      // The start runs on, is stopped, and the device starts again on
	      // the new backend
	      reach(out, OUTPUT_PHASE_STOPPING);
	      out->set_stop_after_start = true;
	      out->set_restart_after_stop = !in->restart_after_stop;
	      break;
	    case OUTPUT_PHASE_CONNECTED:
	    case OUTPUT_PHASE_STREAMING:
	      if (in->op != OUTPUT_OP_NONE)
		break;
	      begin(out, OUTPUT_OP_STOP);
	      out->set_restart_after_stop = !in->restart_after_stop;
	      break;
	    case OUTPUT_PHASE_STOPPING:
	      out->set_restart_after_stop = !in->restart_after_stop;
	      break;
	    case OUTPUT_PHASE_STOPPED:
	    case OUTPUT_PHASE_FAILED:
	    case OUTPUT_PHASE_PASSWORD:
	      break;
	  }
	break;
    }

  finish(in, out);
}

const char *
output_phase_name(enum output_phase phase)
{
  switch (phase)
    {
      case OUTPUT_PHASE_CONNECTING: return "connecting";
      case OUTPUT_PHASE_CONNECTED:  return "connected";
      case OUTPUT_PHASE_STREAMING:  return "streaming";
      case OUTPUT_PHASE_STOPPING:   return "stopping";
      case OUTPUT_PHASE_FAILED:     return "failed";
      case OUTPUT_PHASE_PASSWORD:   return "password";
      default:                      return "stopped";
    }
}
