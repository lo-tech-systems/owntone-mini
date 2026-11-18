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

#ifndef __LEVEL_RULES_H__
#define __LEVEL_RULES_H__

#include <stdbool.h>
#include <stdint.h>

// The rules for the level a device is sent and for what a level it reports
// means. No I/O, no logging and no state.

// The level due on the wire: 0 while the device is silent (muted, or held by
// sending it 0), otherwise its own level
int
level_to_send(bool silent, int volume);

// True when the level last put on the wire is not the level due now, so it
// must be sent again
bool
level_wire_stale(bool silent, int volume, int volume_sent);

enum level_report
{
  LEVEL_REPORT_RECORD,  // the device's level: recorded as it always was
  LEVEL_REPORT_ABSORB,  // not the device's level: an answer to a level sent, or a step of a turn already handled
  LEVEL_REPORT_WAKE,    // a person turned the silent device's own control
};

// What a level the device sent means. silent: the device is muted or held by
// sending it 0. asks: the device asks for the level (a DACP volume button)
// rather than reporting one it is at; a request is never an answer. level is
// on our scale. sent_ms: when a level was last put on the wire (0: never);
// sent_restores: that level ended a silence; wake_sent_ms: when a report from
// the silent device was last forwarded as a wake (0: none). A time is inside
// the window when it is set and now_ms is less than window_ms after it.
enum level_report
level_report_classify(bool silent, bool asks, int level,
                      uint64_t sent_ms, bool sent_restores, uint64_t wake_sent_ms,
                      uint64_t now_ms, uint64_t window_ms);

#endif /* !__LEVEL_RULES_H__ */
