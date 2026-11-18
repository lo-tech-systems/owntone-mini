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

// Checks the rule that moves a speaker's phase and operation, with no daemon
// and no framework: every row of the rule, reports of an older operation, the
// PIN rule over every input, and a random run of eight fake speakers whose
// reports arrive late and out of order. From the top of the source tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/output_phase_test src/outputs/tests/output_phase_test.c src/output_phase.c
//   /tmp/output_phase_test [seed]
//
// Exits 0 when every case passes, 1 otherwise. The random run prints its seed;
// give it back to repeat a run. Also run by "make check".

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../output_phase.h"

static int cases_run;
static int cases_failed;

static const char *
op_name(enum output_op op)
{
  switch (op)
    {
      case OUTPUT_OP_PROBE:     return "probe";
      case OUTPUT_OP_START:     return "start";
      case OUTPUT_OP_STOP:      return "stop";
      case OUTPUT_OP_AUTHORIZE: return "authorize";
      default:                  return "none";
    }
}

static bool
result_equal(const struct output_phase_result *a, const struct output_phase_result *b)
{
  return a->phase == b->phase && a->mid_phase == b->mid_phase && a->next_op == b->next_op && a->clear_op == b->clear_op
    && a->deselect == b->deselect
    && a->set_stop_after_start == b->set_stop_after_start && a->clear_stop_after_start == b->clear_stop_after_start
    && a->set_restart_after_stop == b->set_restart_after_stop && a->clear_restart_after_stop == b->clear_restart_after_stop
    && a->set_upgrade_after_probe == b->set_upgrade_after_probe && a->clear_upgrade_after_probe == b->clear_upgrade_after_probe
    && a->clear_then_select == b->clear_then_select
    && a->set_pin_pending == b->set_pin_pending && a->clear_pin_pending == b->clear_pin_pending
    && a->reason == b->reason && a->announce == b->announce;
}

static void
result_print(const char *label, const struct output_phase_result *r)
{
  printf("  %s: phase %s (via %s), next %s, clear_op %d, deselect %d, stop_after_start +%d -%d, restart +%d -%d, upgrade +%d -%d, "
         "then_select -%d, pin +%d -%d, reason %s, announce %d\n",
         label, output_phase_name(r->phase), output_phase_name(r->mid_phase), op_name(r->next_op), r->clear_op, r->deselect,
         r->set_stop_after_start, r->clear_stop_after_start, r->set_restart_after_stop, r->clear_restart_after_stop,
         r->set_upgrade_after_probe, r->clear_upgrade_after_probe, r->clear_then_select, r->set_pin_pending, r->clear_pin_pending,
         output_failure_name(r->reason), r->announce);
}

static void
check(const char *name, const struct output_phase_result *got, const struct output_phase_result *want)
{
  cases_run++;
  if (result_equal(got, want))
    return;

  cases_failed++;
  printf("FAIL %s\n", name);
  result_print("got ", got);
  result_print("want", want);
}

static void
check_report(const char *name, struct output_phase_input in, enum output_report report, struct output_phase_result want)
{
  struct output_phase_result got;

  in.report_current_gen = true;
  output_phase_on_report(&in, report, &got);
  check(name, &got, &want);
}

static void
check_request(const char *name, struct output_phase_input in, enum output_request req, struct output_phase_result want)
{
  struct output_phase_result got;

  output_phase_on_request(&in, req, &got);
  check(name, &got, &want);
}

#define P(x) OUTPUT_PHASE_##x
#define O(x) OUTPUT_OP_##x
#define R(x) OUTPUT_REPORT_##x
#define F(x) OUTPUT_FAILURE_##x
#define IN(...) ((struct output_phase_input){ __VA_ARGS__ })
#define WANT(...) ((struct output_phase_result){ __VA_ARGS__ })

static void
test_report_table(void)
{
  // Probes
  check_report("probe ends, not playing",
    IN(.phase = P(CONNECTING), .op = O(PROBE), .selected = true),
    R(STARTUP),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED), .clear_op = true, .clear_pin_pending = true, .announce = true));
  check_report("probe ends with stopped, not playing",
    IN(.phase = P(CONNECTING), .op = O(PROBE), .selected = true),
    R(STOPPED),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED), .clear_op = true, .clear_pin_pending = true, .announce = true));
  check_report("probe ends while playing: the start follows",
    IN(.phase = P(CONNECTING), .op = O(PROBE), .selected = true, .playing = true),
    R(STARTUP),
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(START), .clear_op = true, .clear_pin_pending = true, .announce = true));
  check_report("probe ends with a start asked for",
    IN(.phase = P(CONNECTING), .op = O(PROBE), .selected = true, .upgrade_after_probe = true),
    R(STOPPED),
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(START), .clear_op = true, .clear_pin_pending = true,
         .clear_upgrade_after_probe = true, .announce = true));
  check_report("probe step with a session moves nothing",
    IN(.phase = P(CONNECTING), .op = O(PROBE), .selected = true, .has_session = true),
    R(STARTUP),
    WANT(.phase = P(CONNECTING), .mid_phase = P(CONNECTING)));

  // Starts
  check_report("start connects",
    IN(.phase = P(CONNECTING), .op = O(START), .selected = true, .playing = true, .has_session = true),
    R(CONNECTED),
    WANT(.phase = P(CONNECTED), .mid_phase = P(CONNECTED), .clear_op = true, .clear_pin_pending = true, .announce = true));
  check_report("start streams",
    IN(.phase = P(CONNECTING), .op = O(START), .selected = true, .playing = true, .has_session = true),
    R(STREAMING),
    WANT(.phase = P(STREAMING), .mid_phase = P(STREAMING), .clear_op = true, .clear_pin_pending = true, .announce = true));
  check_report("connected then streaming",
    IN(.phase = P(CONNECTED), .selected = true, .playing = true, .has_session = true),
    R(STREAMING),
    WANT(.phase = P(STREAMING), .mid_phase = P(STREAMING), .announce = true));
  check_report("start fails: deselected with the backend's reason",
    IN(.phase = P(CONNECTING), .op = O(START), .selected = true, .playing = true, .report_reason = F(UNREACHABLE)),
    R(FAILED),
    WANT(.phase = P(FAILED), .mid_phase = P(FAILED), .clear_op = true, .deselect = true, .reason = F(UNREACHABLE), .announce = true));
  check_report("start fails with no reason: refused",
    IN(.phase = P(CONNECTING), .op = O(START), .selected = true, .playing = true),
    R(FAILED),
    WANT(.phase = P(FAILED), .mid_phase = P(FAILED), .clear_op = true, .deselect = true, .reason = F(REFUSED), .announce = true));
  check_report("start needs a PIN",
    IN(.phase = P(CONNECTING), .op = O(START), .selected = true, .playing = true, .report_reason = F(PIN_REQUIRED)),
    R(PASSWORD),
    WANT(.phase = P(PASSWORD), .mid_phase = P(PASSWORD), .clear_op = true, .deselect = true, .set_pin_pending = true,
         .reason = F(PIN_REQUIRED), .announce = true));

  // A deselect waits for the start
  check_report("start a deselect waits for connects: stop it",
    IN(.phase = P(STOPPING), .op = O(START), .stop_after_start = true, .has_session = true, .playing = true),
    R(CONNECTED),
    WANT(.phase = P(STOPPING), .mid_phase = P(STOPPING), .next_op = O(STOP), .clear_op = true, .clear_stop_after_start = true,
         .clear_pin_pending = true));
  check_report("start a deselect waits for fails: cancelled",
    IN(.phase = P(STOPPING), .op = O(START), .stop_after_start = true, .playing = true, .report_reason = F(TIMEOUT)),
    R(FAILED),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED), .clear_op = true, .clear_stop_after_start = true, .clear_pin_pending = true,
         .reason = F(CANCELLED), .announce = true));
  check_report("start a deselect waits for wants a PIN: cancelled",
    IN(.phase = P(STOPPING), .op = O(START), .stop_after_start = true, .playing = true),
    R(PASSWORD),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED), .clear_op = true, .clear_stop_after_start = true, .clear_pin_pending = true,
         .reason = F(CANCELLED), .announce = true));
  check_report("probe a deselect waits for ends: cancelled",
    IN(.phase = P(STOPPING), .op = O(PROBE), .stop_after_start = true),
    R(STARTUP),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED), .clear_op = true, .clear_stop_after_start = true, .clear_pin_pending = true,
         .reason = F(CANCELLED), .announce = true));
  check_report("start a mode switch waits for fails: started on the new backend",
    IN(.phase = P(STOPPING), .op = O(START), .stop_after_start = true, .restart_after_stop = true, .selected = true, .playing = true),
    R(FAILED),
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(START), .clear_op = true, .clear_stop_after_start = true,
         .clear_restart_after_stop = true, .clear_pin_pending = true, .reason = F(CANCELLED), .announce = true));

  // Stops
  check_report("stop ends",
    IN(.phase = P(STOPPING), .op = O(STOP), .playing = true),
    R(STOPPED),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED), .clear_op = true, .announce = true));
  check_report("stop ends with a select made meanwhile: started",
    IN(.phase = P(STOPPING), .op = O(STOP), .restart_after_stop = true, .selected = true, .playing = true),
    R(STOPPED),
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(START), .clear_op = true, .clear_restart_after_stop = true, .announce = true));
  check_report("stop ends with a select made meanwhile, not playing: probed",
    IN(.phase = P(STOPPING), .op = O(STOP), .restart_after_stop = true, .selected = true),
    R(STOPPED),
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(PROBE), .clear_op = true, .clear_restart_after_stop = true, .announce = true));
  check_report("stop ends, a PIN pending: not started",
    IN(.phase = P(STOPPING), .op = O(STOP), .restart_after_stop = true, .selected = true, .playing = true, .pin_pending = true),
    R(STOPPED),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED), .clear_op = true, .clear_restart_after_stop = true, .announce = true));
  check_report("any report with no session ends the stop",
    IN(.phase = P(STOPPING), .op = O(STOP)),
    R(FAILED),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED), .clear_op = true, .announce = true));
  check_report("a level answer while stopping moves nothing",
    IN(.phase = P(STOPPING), .op = O(STOP), .has_session = true),
    R(CONNECTED),
    WANT(.phase = P(STOPPING), .mid_phase = P(STOPPING)));

  // Established sessions
  check_report("established session lost: selected kept",
    IN(.phase = P(STREAMING), .selected = true, .playing = true, .report_reason = F(TIMEOUT)),
    R(FAILED),
    WANT(.phase = P(FAILED), .mid_phase = P(FAILED), .reason = F(LOST), .announce = true));
  check_report("established session gives up anchors: its reason kept",
    IN(.phase = P(CONNECTED), .selected = true, .playing = true, .report_reason = F(NO_CLOCK)),
    R(FAILED),
    WANT(.phase = P(FAILED), .mid_phase = P(FAILED), .reason = F(NO_CLOCK), .announce = true));
  check_report("a level answer that changes nothing",
    IN(.phase = P(CONNECTED), .selected = true, .playing = true, .has_session = true),
    R(CONNECTED),
    WANT(.phase = P(CONNECTED), .mid_phase = P(CONNECTED)));
  check_report("a flush answer: streaming to connected",
    IN(.phase = P(STREAMING), .selected = true, .has_session = true),
    R(CONNECTED),
    WANT(.phase = P(CONNECTED), .mid_phase = P(CONNECTED), .announce = true));
  check_report("established session ends quietly while playing: started again",
    IN(.phase = P(STREAMING), .selected = true, .playing = true),
    R(STOPPED),
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(START), .announce = true));

  // Authorise
  check_report("authorise pairs",
    IN(.phase = P(CONNECTING), .op = O(AUTHORIZE), .pin_pending = true),
    R(STOPPED),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED), .clear_op = true, .clear_pin_pending = true, .announce = true));
  check_report("authorise pairs, then the select it carried",
    IN(.phase = P(CONNECTING), .op = O(AUTHORIZE), .pin_pending = true, .then_select = true, .playing = true),
    R(STOPPED),
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(START), .clear_op = true, .clear_pin_pending = true,
         .clear_then_select = true, .announce = true));
  check_report("authorise pairs, then the select it carried, not playing",
    IN(.phase = P(CONNECTING), .op = O(AUTHORIZE), .pin_pending = true, .then_select = true),
    R(STOPPED),
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(PROBE), .clear_op = true, .clear_pin_pending = true,
         .clear_then_select = true, .announce = true));
  check_report("wrong PIN",
    IN(.phase = P(CONNECTING), .op = O(AUTHORIZE), .pin_pending = true, .report_reason = F(PIN_INVALID)),
    R(PASSWORD),
    WANT(.phase = P(PASSWORD), .mid_phase = P(PASSWORD), .clear_op = true, .set_pin_pending = true, .reason = F(PIN_INVALID), .announce = true));
  check_report("wrong PIN with a select: the start shows a new PIN",
    IN(.phase = P(CONNECTING), .op = O(AUTHORIZE), .pin_pending = true, .then_select = true, .playing = true, .report_reason = F(PIN_INVALID)),
    R(PASSWORD),
    WANT(.phase = P(CONNECTING), .mid_phase = P(PASSWORD), .next_op = O(START), .clear_op = true, .clear_then_select = true,
         .set_pin_pending = true, .reason = F(PIN_INVALID), .announce = true));
  check_report("authorise timed out with a select: no new start",
    IN(.phase = P(CONNECTING), .op = O(AUTHORIZE), .pin_pending = true, .then_select = true, .playing = true, .report_reason = F(TIMEOUT)),
    R(PASSWORD),
    WANT(.phase = P(PASSWORD), .mid_phase = P(PASSWORD), .clear_op = true, .clear_then_select = true, .set_pin_pending = true,
         .reason = F(TIMEOUT), .announce = true));
  check_report("authorise fails with no reason: a wrong PIN",
    IN(.phase = P(CONNECTING), .op = O(AUTHORIZE), .pin_pending = true),
    R(FAILED),
    WANT(.phase = P(PASSWORD), .mid_phase = P(PASSWORD), .clear_op = true, .set_pin_pending = true, .reason = F(PIN_INVALID), .announce = true));

  // The post-report rule
  check_report("a selected device with nothing in flight is started",
    IN(.phase = P(STOPPED), .selected = true, .playing = true),
    R(STOPPED),
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(START), .announce = true));
  check_report("but not while it waits for a PIN",
    IN(.phase = P(STOPPED), .selected = true, .playing = true, .pin_pending = true),
    R(STOPPED),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED)));
  check_report("nor when the player is not playing",
    IN(.phase = P(STOPPED), .selected = true),
    R(STOPPED),
    WANT(.phase = P(STOPPED), .mid_phase = P(STOPPED)));
  check_report("a failed device is left to its retry",
    IN(.phase = P(FAILED), .selected = true, .playing = true),
    R(STOPPED),
    WANT(.phase = P(FAILED), .mid_phase = P(FAILED)));
  check_report("a report for a session nothing here started",
    IN(.phase = P(STOPPED), .selected = true, .has_session = true),
    R(CONNECTED),
    WANT(.phase = P(CONNECTED), .mid_phase = P(CONNECTED), .announce = true));
}

static void
test_request_table(void)
{
  check_request("select while playing",
    IN(.phase = P(STOPPED), .playing = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(START), .announce = true));
  check_request("select while not playing",
    IN(.phase = P(STOPPED)), OUTPUT_REQ_SELECT,
    WANT(.phase = P(CONNECTING), .mid_phase = P(STOPPED), .next_op = O(PROBE), .announce = true));
  check_request("select of a failed device",
    IN(.phase = P(FAILED), .playing = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(CONNECTING), .mid_phase = P(FAILED), .next_op = O(START), .announce = true));
  check_request("select during a probe once playing",
    IN(.phase = P(CONNECTING), .op = O(PROBE), .selected = true, .playing = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(CONNECTING), .mid_phase = P(CONNECTING), .set_upgrade_after_probe = true));
  check_request("select while connecting",
    IN(.phase = P(CONNECTING), .op = O(START), .selected = true, .playing = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(CONNECTING), .mid_phase = P(CONNECTING)));
  check_request("select while streaming",
    IN(.phase = P(STREAMING), .selected = true, .has_session = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(STREAMING), .mid_phase = P(STREAMING)));
  check_request("deselect while connecting: the start runs on",
    IN(.phase = P(CONNECTING), .op = O(START), .selected = true, .playing = true), OUTPUT_REQ_DESELECT,
    WANT(.phase = P(STOPPING), .mid_phase = P(STOPPING), .set_stop_after_start = true, .announce = true));
  check_request("deselect during a probe with a start asked for",
    IN(.phase = P(CONNECTING), .op = O(PROBE), .selected = true, .upgrade_after_probe = true), OUTPUT_REQ_DESELECT,
    WANT(.phase = P(STOPPING), .mid_phase = P(STOPPING), .set_stop_after_start = true, .clear_upgrade_after_probe = true, .announce = true));
  check_request("select while a deselect waits for the start",
    IN(.phase = P(STOPPING), .op = O(START), .stop_after_start = true, .playing = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(CONNECTING), .mid_phase = P(CONNECTING), .clear_stop_after_start = true, .announce = true));
  check_request("select while a mode switch waits for the start",
    IN(.phase = P(STOPPING), .op = O(START), .stop_after_start = true, .restart_after_stop = true, .playing = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(STOPPING), .mid_phase = P(STOPPING)));
  check_request("deselect while streaming",
    IN(.phase = P(STREAMING), .selected = true, .has_session = true), OUTPUT_REQ_DESELECT,
    WANT(.phase = P(STOPPING), .mid_phase = P(STREAMING), .next_op = O(STOP), .announce = true));
  check_request("select while stopping",
    IN(.phase = P(STOPPING), .op = O(STOP), .has_session = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(STOPPING), .mid_phase = P(STOPPING), .set_restart_after_stop = true));
  check_request("deselect while stopping clears the restart and the PIN's select",
    IN(.phase = P(STOPPING), .op = O(STOP), .has_session = true, .restart_after_stop = true, .then_select = true), OUTPUT_REQ_DESELECT,
    WANT(.phase = P(STOPPING), .mid_phase = P(STOPPING), .clear_restart_after_stop = true, .clear_then_select = true));
  check_request("deselect during an authorise clears only its select",
    IN(.phase = P(CONNECTING), .op = O(AUTHORIZE), .then_select = true, .pin_pending = true), OUTPUT_REQ_DESELECT,
    WANT(.phase = P(CONNECTING), .mid_phase = P(CONNECTING), .clear_then_select = true));
  check_request("PIN given",
    IN(.phase = P(PASSWORD), .pin_pending = true), OUTPUT_REQ_AUTHORIZE,
    WANT(.phase = P(CONNECTING), .mid_phase = P(PASSWORD), .next_op = O(AUTHORIZE), .announce = true));
  check_request("PIN given in a start's retry gap: refused as busy",
    IN(.phase = P(CONNECTING), .op = O(START), .selected = true, .playing = true), OUTPUT_REQ_AUTHORIZE,
    WANT(.phase = P(CONNECTING), .mid_phase = P(CONNECTING)));
  check_request("PIN given while an authorise is out: refused as busy",
    IN(.phase = P(CONNECTING), .op = O(AUTHORIZE), .pin_pending = true), OUTPUT_REQ_AUTHORIZE,
    WANT(.phase = P(CONNECTING), .mid_phase = P(CONNECTING)));
  check_request("PIN given to a connected device: refused as busy",
    IN(.phase = P(CONNECTED), .has_session = true), OUTPUT_REQ_AUTHORIZE,
    WANT(.phase = P(CONNECTED), .mid_phase = P(CONNECTED)));
  check_request("automatic select of a device waiting for a PIN: ignored",
    IN(.phase = P(PASSWORD), .pin_pending = true, .playing = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(PASSWORD), .mid_phase = P(PASSWORD)));
  check_request("the user's select of a device waiting for a PIN: a new PIN start",
    IN(.phase = P(PASSWORD), .pin_pending = true, .playing = true, .explicit_request = true), OUTPUT_REQ_SELECT,
    WANT(.phase = P(CONNECTING), .mid_phase = P(PASSWORD), .next_op = O(START), .announce = true));
  check_request("backend change while connecting",
    IN(.phase = P(CONNECTING), .op = O(START), .selected = true, .playing = true, .backend_changes = true), OUTPUT_REQ_MODE_SWITCH,
    WANT(.phase = P(STOPPING), .mid_phase = P(STOPPING), .set_stop_after_start = true, .set_restart_after_stop = true, .announce = true));
  check_request("backend change while streaming",
    IN(.phase = P(STREAMING), .selected = true, .has_session = true, .backend_changes = true), OUTPUT_REQ_MODE_SWITCH,
    WANT(.phase = P(STOPPING), .mid_phase = P(STREAMING), .next_op = O(STOP), .set_restart_after_stop = true, .announce = true));
  check_request("backend change while stopping",
    IN(.phase = P(STOPPING), .op = O(STOP), .has_session = true, .backend_changes = true), OUTPUT_REQ_MODE_SWITCH,
    WANT(.phase = P(STOPPING), .mid_phase = P(STOPPING), .set_restart_after_stop = true));
  check_request("mode switch on the same backend",
    IN(.phase = P(STREAMING), .selected = true, .has_session = true), OUTPUT_REQ_MODE_SWITCH,
    WANT(.phase = P(STREAMING), .mid_phase = P(STREAMING)));
}

// Every combination of the inputs a report depends on
#define FLAG_BITS 8

static struct output_phase_input
combo(int phase, int op, unsigned bits, bool pin_pending)
{
  struct output_phase_input in = { 0 };

  in.phase = phase;
  in.op = op;
  in.selected = bits & 1;
  in.playing = bits & 2;
  in.has_session = bits & 4;
  in.stop_after_start = bits & 8;
  in.restart_after_stop = bits & 16;
  in.upgrade_after_probe = bits & 32;
  in.then_select = bits & 64;
  in.report_reason = (bits & 128) ? OUTPUT_FAILURE_TIMEOUT : OUTPUT_FAILURE_PIN_INVALID;
  in.pin_pending = pin_pending;
  return in;
}

static void
fail_combo(const char *what, const struct output_phase_input *in, int event, const struct output_phase_result *out)
{
  cases_failed++;
  printf("FAIL %s: phase %s op %s event %d selected %d playing %d session %d sas %d ras %d uap %d ts %d pin %d\n", what,
         output_phase_name(in->phase), op_name(in->op), event, in->selected, in->playing, in->has_session, in->stop_after_start,
         in->restart_after_stop, in->upgrade_after_probe, in->then_select, in->pin_pending);
  result_print("got", out);
}

// Each report repeated as one of an older operation: nothing moves
static void
test_stale_generation(void)
{
  struct output_phase_input in;
  struct output_phase_result out;
  int phase, op, report;
  unsigned bits;

  for (phase = OUTPUT_PHASE_STOPPED; phase <= OUTPUT_PHASE_PASSWORD; phase++)
    for (op = OUTPUT_OP_NONE; op <= OUTPUT_OP_AUTHORIZE; op++)
      for (report = OUTPUT_REPORT_STOPPED; report <= OUTPUT_REPORT_PASSWORD; report++)
	for (bits = 0; bits < (1u << FLAG_BITS); bits++)
	  {
	    in = combo(phase, op, bits, bits & 1);
	    in.report_current_gen = false;
	    output_phase_on_report(&in, report, &out);

	    cases_run++;
	    if (out.mid_phase != in.phase || out.clear_op || out.deselect || out.reason != OUTPUT_FAILURE_NONE
		|| out.set_pin_pending || out.clear_pin_pending || out.clear_then_select
		|| out.set_stop_after_start || out.clear_stop_after_start || out.clear_restart_after_stop)
	      fail_combo("an older report moved something", &in, report, &out);
	  }
}

// Nothing automatic starts a device that waits for a PIN; the one start
// allowed is the select that came with the PIN after the receiver refused it.
// Also: one operation at a time, and the phase that goes with each new one
static void
test_pin_never_automatic(void)
{
  struct output_phase_input in;
  struct output_phase_result out;
  int phase, op, report, req;
  unsigned bits;
  bool pin_after;

  for (phase = OUTPUT_PHASE_STOPPED; phase <= OUTPUT_PHASE_PASSWORD; phase++)
    for (op = OUTPUT_OP_NONE; op <= OUTPUT_OP_AUTHORIZE; op++)
      for (bits = 0; bits < (1u << FLAG_BITS); bits++)
	{
	  for (report = OUTPUT_REPORT_STOPPED; report <= OUTPUT_REPORT_PASSWORD; report++)
	    {
	      in = combo(phase, op, bits, true);
	      in.report_current_gen = true;
	      output_phase_on_report(&in, report, &out);

	      cases_run++;
	      pin_after = !out.clear_pin_pending;
	      if (pin_after && (out.next_op == OUTPUT_OP_START || out.next_op == OUTPUT_OP_PROBE)
		  && !(op == OUTPUT_OP_AUTHORIZE && in.then_select && out.reason == OUTPUT_FAILURE_PIN_INVALID))
		fail_combo("automatic start of a device waiting for a PIN", &in, report, &out);

	      cases_run++;
	      if (out.next_op != OUTPUT_OP_NONE && op != OUTPUT_OP_NONE && !out.clear_op)
		fail_combo("a second operation while one is in flight", &in, report, &out);

	      cases_run++;
	      if (out.next_op != OUTPUT_OP_NONE
		  && out.phase != ((out.next_op == OUTPUT_OP_STOP) ? OUTPUT_PHASE_STOPPING : OUTPUT_PHASE_CONNECTING))
		fail_combo("a new operation in the wrong phase", &in, report, &out);
	    }

	  for (req = OUTPUT_REQ_SELECT; req <= OUTPUT_REQ_MODE_SWITCH; req++)
	    {
	      in = combo(phase, op, bits, true);
	      in.backend_changes = bits & 128;
	      output_phase_on_request(&in, req, &out);

	      cases_run++;
	      if (req != OUTPUT_REQ_AUTHORIZE && (out.next_op == OUTPUT_OP_START || out.next_op == OUTPUT_OP_PROBE))
		fail_combo("automatic request started a device waiting for a PIN", &in, req, &out);

	      cases_run++;
	      if (out.next_op != OUTPUT_OP_NONE && op != OUTPUT_OP_NONE)
		fail_combo("a request began a second operation", &in, req, &out);
	    }
	}
}


/* ------------------------------ Random run -------------------------------- */

#define H_DEVICES 8
#define H_STEPS 20000
#define H_QUEUE 64

struct h_report
{
  enum output_report report;
  enum output_failure_reason reason;
  bool session_after;
  uint32_t gen;
};

struct h_device
{
  struct output_phase_input st;   // the persistent part: phase, op, flags
  uint32_t gen;
  bool session;
  struct h_report queue[H_QUEUE];
  int nqueue;
};

static uint64_t h_rng;
static struct h_device h_devices[H_DEVICES];
static bool h_playing;
static int h_failures;

static uint32_t
h_rand(uint32_t n)
{
  h_rng ^= h_rng << 13;
  h_rng ^= h_rng >> 7;
  h_rng ^= h_rng << 17;
  return (uint32_t)(h_rng % n);
}

static void
h_fail(int i, const char *what)
{
  struct h_device *d = &h_devices[i];

  if (h_failures++ < 10)
    printf("FAIL random run, device %d: %s (phase %s, op %s, selected %d, session %d, pin %d, queued %d)\n", i, what,
           output_phase_name(d->st.phase), op_name(d->st.op), d->st.selected, d->session, d->st.pin_pending, d->nqueue);
}

static void
h_push(struct h_device *d, enum output_report report, enum output_failure_reason reason, bool session_after)
{
  if (d->nqueue == H_QUEUE)
    return;

  d->queue[d->nqueue++] = (struct h_report){ report, reason, session_after, d->gen };
}

// The fake backend: the reports an operation will make, under its generation
static void
h_begin(struct h_device *d, enum output_op op)
{
  uint32_t r = h_rand(100);

  d->gen++;
  d->st.op = op;

  switch (op)
    {
      case OUTPUT_OP_START:
	d->session = true;
	if (r < 60)
	  {
	    h_push(d, OUTPUT_REPORT_CONNECTED, OUTPUT_FAILURE_NONE, true);
	    if (h_rand(2))
	      h_push(d, OUTPUT_REPORT_STREAMING, OUTPUT_FAILURE_NONE, true);
	  }
	else if (r < 80)
	  h_push(d, OUTPUT_REPORT_FAILED, h_rand(2) ? OUTPUT_FAILURE_UNREACHABLE : OUTPUT_FAILURE_TIMEOUT, false);
	else
	  h_push(d, OUTPUT_REPORT_PASSWORD, OUTPUT_FAILURE_PIN_REQUIRED, false);
	break;

      case OUTPUT_OP_PROBE:
	d->session = true;
	if (r < 70)
	  h_push(d, OUTPUT_REPORT_STARTUP, OUTPUT_FAILURE_NONE, false);
	else if (r < 85)
	  h_push(d, OUTPUT_REPORT_FAILED, OUTPUT_FAILURE_UNREACHABLE, false);
	else
	  h_push(d, OUTPUT_REPORT_PASSWORD, OUTPUT_FAILURE_PIN_REQUIRED, false);
	break;

      case OUTPUT_OP_STOP:
	h_push(d, OUTPUT_REPORT_STOPPED, OUTPUT_FAILURE_NONE, false);
	break;

      case OUTPUT_OP_AUTHORIZE:
	d->session = true;
	if (r < 50)
	  h_push(d, OUTPUT_REPORT_STOPPED, OUTPUT_FAILURE_NONE, false);
	else if (r < 80)
	  h_push(d, OUTPUT_REPORT_PASSWORD, OUTPUT_FAILURE_PIN_INVALID, false);
	else
	  h_push(d, OUTPUT_REPORT_PASSWORD, OUTPUT_FAILURE_TIMEOUT, false);
	break;

      case OUTPUT_OP_NONE:
	break;
    }
}

static void
h_apply(int i, const struct output_phase_input *in, const struct output_phase_result *out, bool from_report)
{
  struct h_device *d = &h_devices[i];

  d->st.phase = out->phase;
  if (out->clear_op)
    d->st.op = OUTPUT_OP_NONE;
  if (out->deselect)
    d->st.selected = false;
  if (out->set_stop_after_start)    d->st.stop_after_start = true;
  if (out->clear_stop_after_start)  d->st.stop_after_start = false;
  if (out->set_restart_after_stop)  d->st.restart_after_stop = true;
  if (out->clear_restart_after_stop) d->st.restart_after_stop = false;
  if (out->set_upgrade_after_probe) d->st.upgrade_after_probe = true;
  if (out->clear_upgrade_after_probe) d->st.upgrade_after_probe = false;
  if (out->clear_then_select)       d->st.then_select = false;
  if (out->set_pin_pending)         d->st.pin_pending = true;
  if (out->clear_pin_pending)       d->st.pin_pending = false;

  if (from_report && d->st.pin_pending && (out->next_op == OUTPUT_OP_START || out->next_op == OUTPUT_OP_PROBE)
      && !(in->op == OUTPUT_OP_AUTHORIZE && in->then_select))
    h_fail(i, "a report started a device waiting for a PIN");

  if (out->next_op != OUTPUT_OP_NONE)
    {
      if (d->st.op != OUTPUT_OP_NONE)
	h_fail(i, "a second operation began");
      // The select that came with a PIN is the user's
      if (in->op == OUTPUT_OP_AUTHORIZE && in->then_select)
	d->st.selected = true;
      h_begin(d, out->next_op);
    }
}

static void
h_report(int i)
{
  struct h_device *d = &h_devices[i];
  struct output_phase_input in;
  struct output_phase_result out;
  struct h_report r;
  int idx = 0;
  int k;

  if (d->nqueue == 0)
    return;

  // Mostly the oldest, sometimes any report of an older operation
  if (h_rand(10) < 3)
    {
      k = (int)h_rand((uint32_t)d->nqueue);
      if (d->queue[k].gen != d->gen)
	idx = k;
    }

  r = d->queue[idx];
  memmove(&d->queue[idx], &d->queue[idx + 1], (size_t)(d->nqueue - idx - 1) * sizeof(r));
  d->nqueue--;

  if (r.gen == d->gen)
    d->session = r.session_after;

  in = d->st;
  in.report_current_gen = (r.gen == d->gen);
  in.report_reason = r.reason;
  in.playing = h_playing;
  in.has_session = d->session;
  output_phase_on_report(&in, r.report, &out);
  h_apply(i, &in, &out, true);

  if (d->st.selected && h_playing && !d->session && d->st.op == OUTPUT_OP_NONE && !d->st.pin_pending && d->st.phase == OUTPUT_PHASE_STOPPED)
    h_fail(i, "left selected while playing with no session and nothing in flight");
}

static void
h_request(int i)
{
  struct h_device *d = &h_devices[i];
  struct output_phase_input in;
  struct output_phase_result out;
  enum output_request req;
  bool explicit_request = h_rand(2);
  bool ignored;

  req = (enum output_request)h_rand(4);

  in = d->st;
  in.playing = h_playing;
  in.has_session = d->session;
  in.explicit_request = explicit_request;
  in.backend_changes = h_rand(2);
  if (req == OUTPUT_REQ_AUTHORIZE)
    in.then_select = h_rand(2);

  output_phase_on_request(&in, req, &out);

  ignored = (req == OUTPUT_REQ_SELECT && d->st.pin_pending && !explicit_request);
  if (req == OUTPUT_REQ_SELECT && !ignored)
    d->st.selected = true;
  if (req == OUTPUT_REQ_DESELECT)
    d->st.selected = false;
  if (req == OUTPUT_REQ_AUTHORIZE && out.next_op == OUTPUT_OP_AUTHORIZE)
    d->st.then_select = in.then_select;

  h_apply(i, &in, &out, false);
}

// A level answer, or the session lost, on a device with nothing in flight
static void
h_event(int i)
{
  struct h_device *d = &h_devices[i];

  if (!d->session || d->st.op != OUTPUT_OP_NONE)
    return;
  if (d->st.phase != OUTPUT_PHASE_CONNECTED && d->st.phase != OUTPUT_PHASE_STREAMING)
    return;

  if (h_rand(3) == 0)
    h_push(d, OUTPUT_REPORT_FAILED, OUTPUT_FAILURE_TIMEOUT, false);
  else
    h_push(d, (d->st.phase == OUTPUT_PHASE_CONNECTED) ? OUTPUT_REPORT_CONNECTED : OUTPUT_REPORT_STREAMING, OUTPUT_FAILURE_NONE, true);
}

static bool
h_has_current(const struct h_device *d)
{
  int k;

  for (k = 0; k < d->nqueue; k++)
    if (d->queue[k].gen == d->gen)
      return true;

  return false;
}

static void
h_check_all(void)
{
  int i;

  for (i = 0; i < H_DEVICES; i++)
    {
      struct h_device *d = &h_devices[i];

      if ((d->st.phase == OUTPUT_PHASE_CONNECTING || d->st.phase == OUTPUT_PHASE_STOPPING) && d->st.op == OUTPUT_OP_NONE)
	h_fail(i, "connecting or stopping with nothing in flight");
      if (d->st.op != OUTPUT_OP_NONE && !h_has_current(d))
	h_fail(i, "an operation that will never report");
    }
}

static void
test_harness_random(uint64_t seed)
{
  int step, i, round;
  uint32_t r;
  bool busy;

  printf("random run seed %llu\n", (unsigned long long)seed);
  h_rng = seed ? seed : 1;
  memset(h_devices, 0, sizeof(h_devices));
  h_playing = false;
  h_failures = 0;

  for (step = 0; step < H_STEPS; step++)
    {
      i = (int)h_rand(H_DEVICES);
      r = h_rand(100);

      if (r < 55)
	h_report(i);
      else if (r < 85)
	h_request(i);
      else if (r < 95)
	h_event(i);
      else
	h_playing = !h_playing;

      h_check_all();
    }

  // Every operation ends once the backends have said everything, and every
  // stopping device reaches stopped
  h_playing = false;
  for (round = 0; round < 10000; round++)
    {
      busy = false;
      for (i = 0; i < H_DEVICES; i++)
	if (h_devices[i].nqueue)
	  {
	    h_report(i);
	    busy = true;
	  }
      h_check_all();
      if (!busy)
	break;
    }

  for (i = 0; i < H_DEVICES; i++)
    {
      if (h_devices[i].st.op != OUTPUT_OP_NONE)
	h_fail(i, "an operation did not end");
      if (h_devices[i].st.phase == OUTPUT_PHASE_STOPPING || h_devices[i].st.phase == OUTPUT_PHASE_CONNECTING)
	h_fail(i, "did not settle");
    }

  cases_run++;
  if (h_failures)
    cases_failed++;
}

static void
test_names(void)
{
  static const char *names[] = { "stopped", "connecting", "connected", "streaming", "stopping", "failed", "password" };
  int phase;

  for (phase = OUTPUT_PHASE_STOPPED; phase <= OUTPUT_PHASE_PASSWORD; phase++)
    {
      cases_run++;
      if (strcmp(output_phase_name(phase), names[phase]) != 0)
	{
	  cases_failed++;
	  printf("FAIL name of phase %d: %s\n", phase, output_phase_name(phase));
	}
    }
}

/* ------------- Sequences as the player's report handler makes them ---------- */

// One device driven the way the player applies the rule: flags and the
// selection first, then the phase the event reached, then the next operation,
// which sets its own phase. Every phase change is announced, and a change of
// selection or PIN state with no phase change too; the log is what a client
// would see, one entry per announcement
struct seq_device
{
  struct output_phase_input in;
  enum output_failure_reason reason;
  char log[512];
};

static void
seq_announce(struct seq_device *d)
{
  char entry[64];

  snprintf(entry, sizeof(entry), "%s%s%s%s%s", d->log[0] ? "," : "", output_phase_name(d->in.phase),
           (d->in.phase == P(FAILED) || d->in.phase == P(PASSWORD) || d->reason == F(CANCELLED)) ? ":" : "",
           (d->in.phase == P(FAILED) || d->in.phase == P(PASSWORD) || d->reason == F(CANCELLED)) ? output_failure_name(d->reason) : "",
           d->in.pin_pending ? "+pin" : "");
  strncat(d->log, entry, sizeof(d->log) - strlen(d->log) - 1);
}

static void
seq_apply(struct seq_device *d, const struct output_phase_result *r, enum output_op ended)
{
  enum output_phase before = d->in.phase;
  enum output_phase reached = r->mid_phase;
  bool announced = false;

  if (r->set_stop_after_start) d->in.stop_after_start = true;
  if (r->clear_stop_after_start) d->in.stop_after_start = false;
  if (r->set_restart_after_stop) d->in.restart_after_stop = true;
  if (r->clear_restart_after_stop) d->in.restart_after_stop = false;
  if (r->set_upgrade_after_probe) d->in.upgrade_after_probe = true;
  if (r->clear_upgrade_after_probe) d->in.upgrade_after_probe = false;
  if (r->set_pin_pending) d->in.pin_pending = true;
  if (r->clear_pin_pending) d->in.pin_pending = false;
  if (r->reason != F(NONE)) d->reason = r->reason;
  if (r->deselect) d->in.selected = false;
  if (r->clear_op) d->in.op = O(NONE);

  // The select that came with the PIN selects the device before its start
  if (ended == O(AUTHORIZE) && d->in.then_select && (r->next_op == O(START) || r->next_op == O(PROBE)))
    d->in.selected = true;
  if (r->clear_then_select) d->in.then_select = false;

  if (r->next_op == O(NONE))
    reached = r->phase;

  if (reached != before)
    {
      d->in.phase = reached;
      seq_announce(d);
      announced = true;
    }

  if (r->next_op != O(NONE))
    {
      d->in.op = r->next_op;
      if (r->phase != d->in.phase)
	{
	  d->in.phase = r->phase;
	  seq_announce(d);
	  announced = true;
	}
    }

  if (r->announce && !announced)
    seq_announce(d);
}

static void
seq_request(struct seq_device *d, enum output_request req, bool explicit_request)
{
  struct output_phase_result r;

  d->in.explicit_request = explicit_request;
  output_phase_on_request(&d->in, req, &r);
  d->in.explicit_request = false;
  seq_apply(d, &r, O(NONE));
}

static void
seq_report(struct seq_device *d, enum output_report report, enum output_failure_reason reason, bool has_session, bool current)
{
  struct output_phase_result r;
  enum output_op op = d->in.op;

  d->in.report_current_gen = current;
  d->in.report_reason = reason;
  d->in.has_session = has_session;
  output_phase_on_report(&d->in, report, &r);
  seq_apply(d, &r, (current && r.clear_op) ? op : O(NONE));
}

static void
seq_check(const char *name, const struct seq_device *d, const char *want)
{
  cases_run++;
  if (strcmp(d->log, want) == 0)
    return;

  cases_failed++;
  printf("FAIL %s\n  got  %s\n  want %s\n", name, d->log, want);
}

// A device that asked for a PIN, as the PIN request finds it
static void
seq_pin_device(struct seq_device *d, bool then_select)
{
  memset(d, 0, sizeof(*d));
  d->in.phase = P(PASSWORD);
  d->in.pin_pending = true;
  d->in.playing = true;
  d->reason = F(PIN_REQUIRED);
  d->in.then_select = then_select;
}

static void
test_sequences(void)
{
  struct seq_device d;

  // The right PIN with selected true: the stopped between the two
  // operations is announced, with the PIN no longer pending
  seq_pin_device(&d, true);
  seq_request(&d, OUTPUT_REQ_AUTHORIZE, false);
  seq_report(&d, R(STOPPED), F(NONE), false, true);
  seq_report(&d, R(CONNECTED), F(NONE), true, true);
  seq_report(&d, R(STREAMING), F(NONE), true, true);
  seq_check("right PIN with select", &d, "connecting+pin,stopped,connecting,connected,streaming");

  // A wrong PIN with selected true: the refusal, then the start the request
  // asked for, which shows a new PIN
  seq_pin_device(&d, true);
  seq_request(&d, OUTPUT_REQ_AUTHORIZE, false);
  seq_report(&d, R(PASSWORD), F(PIN_INVALID), false, true);
  seq_report(&d, R(PASSWORD), F(PIN_REQUIRED), false, true);
  seq_check("wrong PIN with select", &d, "connecting+pin,password:pin_invalid+pin,connecting+pin,password:pin_required+pin");

  // A wrong PIN alone: the refusal, and nothing started
  seq_pin_device(&d, false);
  seq_request(&d, OUTPUT_REQ_AUTHORIZE, false);
  seq_report(&d, R(PASSWORD), F(PIN_INVALID), false, true);
  seq_check("wrong PIN alone", &d, "connecting+pin,password:pin_invalid+pin");

  // An authorise that timed out: timeout, not a refused PIN, and no new PIN
  // start, since the PIN on screen may be the one being typed
  seq_pin_device(&d, true);
  seq_request(&d, OUTPUT_REQ_AUTHORIZE, false);
  seq_report(&d, R(PASSWORD), F(TIMEOUT), false, true);
  seq_check("authorise timed out", &d, "connecting+pin,password:timeout+pin");
  cases_run++;
  if (d.in.then_select || d.in.op != O(NONE) || d.in.selected)
    {
      cases_failed++;
      printf("FAIL authorise timed out: the select survived\n");
    }

  // A deselect during the authorise takes the select away; the pairing ends
  // stopped and nothing starts
  seq_pin_device(&d, true);
  seq_request(&d, OUTPUT_REQ_AUTHORIZE, false);
  seq_request(&d, OUTPUT_REQ_DESELECT, false);
  seq_report(&d, R(STOPPED), F(NONE), false, true);
  seq_check("deselect during the authorise", &d, "connecting+pin,stopped");

  // Nothing automatic starts a device that waits for a PIN; the user's own
  // select does
  seq_pin_device(&d, false);
  seq_request(&d, OUTPUT_REQ_SELECT, false);
  seq_check("automatic select of a PIN device", &d, "");
  seq_request(&d, OUTPUT_REQ_SELECT, true);
  seq_check("user's select of a PIN device", &d, "connecting+pin");

  // A deselect while connecting: stopping at once, the start runs on and is
  // stopped once it has a session, then stopped
  memset(&d, 0, sizeof(d));
  d.in.playing = true;
  d.in.selected = true;
  seq_request(&d, OUTPUT_REQ_SELECT, true);
  d.in.selected = false;
  seq_request(&d, OUTPUT_REQ_DESELECT, false);
  seq_report(&d, R(CONNECTED), F(NONE), true, true);
  seq_report(&d, R(STOPPED), F(NONE), false, true);
  seq_check("deselect while connecting, start succeeds", &d, "connecting,stopping,stopped");

  // The same, the start leaving no session: stopped and cancelled
  memset(&d, 0, sizeof(d));
  d.in.playing = true;
  d.in.selected = true;
  seq_request(&d, OUTPUT_REQ_SELECT, true);
  d.in.selected = false;
  seq_request(&d, OUTPUT_REQ_DESELECT, false);
  seq_report(&d, R(FAILED), F(UNREACHABLE), false, true);
  seq_check("deselect while connecting, start fails", &d, "connecting,stopping,stopped:cancelled");

  // A select during a stop: nothing new until the stop ends, then the start
  memset(&d, 0, sizeof(d));
  d.in.playing = true;
  d.in.phase = P(STREAMING);
  d.in.has_session = true;
  seq_request(&d, OUTPUT_REQ_DESELECT, false);
  d.in.selected = true;
  seq_request(&d, OUTPUT_REQ_SELECT, true);
  seq_report(&d, R(STOPPED), F(NONE), false, true);
  seq_check("select during a stop", &d, "stopping,stopped,connecting");

  // A probe that ends while the player plays is followed by the start, and
  // both phases are announced
  memset(&d, 0, sizeof(d));
  d.in.selected = true;
  seq_request(&d, OUTPUT_REQ_SELECT, true);
  d.in.playing = true;
  seq_report(&d, R(STARTUP), F(NONE), false, true);
  seq_check("probe then start", &d, "connecting,stopped,connecting");

  // A report from an older operation moves nothing
  memset(&d, 0, sizeof(d));
  d.in.playing = true;
  d.in.phase = P(STOPPING);
  d.in.op = O(STOP);
  d.in.has_session = true;
  seq_report(&d, R(FAILED), F(UNREACHABLE), true, false);
  seq_check("stale report", &d, "");

  // An established session that fails stays selected, reads lost
  memset(&d, 0, sizeof(d));
  d.in.playing = true;
  d.in.selected = true;
  d.in.phase = P(STREAMING);
  seq_report(&d, R(FAILED), F(TIMEOUT), false, true);
  seq_check("established session lost", &d, "failed:lost");
  cases_run++;
  if (!d.in.selected)
    {
      cases_failed++;
      printf("FAIL established session lost: deselected\n");
    }
}

int
main(int argc, char *argv[])
{
  uint64_t seed;
  int run;

  seed = (argc > 1) ? strtoull(argv[1], NULL, 10) : (uint64_t)time(NULL);

  test_sequences();
  test_report_table();
  test_request_table();
  test_stale_generation();
  test_pin_never_automatic();
  test_names();

  // The given (or time's) seed, then a few fixed ones so every run covers them
  test_harness_random(seed);
  for (run = 1; run <= 4; run++)
    test_harness_random((uint64_t)run * 2654435761u);

  printf("%d cases, %d failed\n", cases_run, cases_failed);
  return cases_failed ? 1 : 0;
}
