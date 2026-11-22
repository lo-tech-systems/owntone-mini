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

#ifndef __SPEAKERS_SUMMARY_H__
#define __SPEAKERS_SUMMARY_H__

#include <stdbool.h>

#include "output_failure.h"
#include "output_phase.h"

// Which kinds of session state were seen among the members of a row
struct speakers_seen
{
  bool streaming;
  bool starting;
  bool password;
  bool failed;
  bool stopping;
};

enum speakers_summary
{
  SPEAKERS_SUMMARY_STOPPED,
  SPEAKERS_SUMMARY_STARTING,
  SPEAKERS_SUMMARY_PASSWORD,
  SPEAKERS_SUMMARY_FAILED,
  SPEAKERS_SUMMARY_STREAMING,
  SPEAKERS_SUMMARY_STOPPING,
};

// One state for the group: password if any member asks for a PIN (the user
// has to act, so it shows even while another member plays), else streaming if
// any streams, else starting, else stopping, else failed, else stopped.
// Stopping ranks below starting, so a group with one member restarting reads
// starting
static inline enum speakers_summary
speakers_summary_resolve(struct speakers_seen seen)
{
  if (seen.password)
    return SPEAKERS_SUMMARY_PASSWORD;
  if (seen.streaming)
    return SPEAKERS_SUMMARY_STREAMING;
  if (seen.starting)
    return SPEAKERS_SUMMARY_STARTING;
  if (seen.stopping)
    return SPEAKERS_SUMMARY_STOPPING;
  if (seen.failed)
    return SPEAKERS_SUMMARY_FAILED;

  return SPEAKERS_SUMMARY_STOPPED;
}

// One member of a row as the row reads it. A device in no group is the only
// member of its row
struct row_member
{
  enum output_phase phase;
  // It plays or is about to: a session starting, connected or streaming, or a
  // failure whose retry is waiting
  bool live;
  // Shown selected, or kept selected for its retry
  bool selected;
  bool pin_pending;
  // The reason it carries (read in the password and failed phases)
  enum output_failure_reason reason;
  // It is the row's own device, whose reason is preferred
  bool is_row;
};

// The members of a row so far
struct speakers_row
{
  struct speakers_seen seen;
  bool selected;
  bool pin_pending;
  bool any_live;
  // Members neither live nor waiting for a PIN
  int not_live;
  enum output_failure_reason password_reason;
  enum output_failure_reason failed_reason;
  bool password_reason_is_row;
  bool failed_reason_is_row;
};

// What the row shows, from all its members
struct speakers_row_view
{
  enum speakers_summary summary;
  // The reason of the winning password or failed phase, NONE otherwise
  enum output_failure_reason reason;
  bool selected;
  bool pin_pending;
  // Members not playing while another member is; 0 when none is
  int members_missing;
};

static inline void
speakers_row_reason_take(enum output_failure_reason *reason, bool *is_row, const struct row_member *m)
{
  // The first member's reason, unless the row's own device has one
  if (*reason == OUTPUT_FAILURE_NONE || (m->is_row && !*is_row))
    {
      *reason = m->reason;
      *is_row = m->is_row;
    }
}

static inline void
speakers_row_add(struct speakers_row *row, const struct row_member *m)
{
  switch (m->phase)
    {
      case OUTPUT_PHASE_STREAMING:
        row->seen.streaming = true;
        break;
      case OUTPUT_PHASE_CONNECTING:
      case OUTPUT_PHASE_CONNECTED:
        row->seen.starting = true;
        break;
      case OUTPUT_PHASE_STOPPING:
        row->seen.stopping = true;
        break;
      case OUTPUT_PHASE_PASSWORD:
        row->seen.password = true;
        speakers_row_reason_take(&row->password_reason, &row->password_reason_is_row, m);
        break;
      case OUTPUT_PHASE_FAILED:
        // A failure whose retry is waiting is about to start again
        if (m->live)
          row->seen.starting = true;
        else
          {
            row->seen.failed = true;
            speakers_row_reason_take(&row->failed_reason, &row->failed_reason_is_row, m);
          }
        break;
      case OUTPUT_PHASE_STOPPED:
        break;
    }

  row->selected = row->selected || m->selected;
  row->pin_pending = row->pin_pending || m->pin_pending;
  row->any_live = row->any_live || m->live;
  if (!m->live && !m->pin_pending)
    row->not_live++;
}

static inline struct speakers_row_view
speakers_row_resolve(const struct speakers_row *row)
{
  struct speakers_row_view view = { 0 };

  view.summary = speakers_summary_resolve(row->seen);
  if (view.summary == SPEAKERS_SUMMARY_PASSWORD)
    view.reason = row->password_reason;
  else if (view.summary == SPEAKERS_SUMMARY_FAILED)
    view.reason = row->failed_reason;
  view.selected = row->selected;
  view.pin_pending = row->pin_pending;
  view.members_missing = row->any_live ? row->not_live : 0;

  return view;
}

#endif /* !__SPEAKERS_SUMMARY_H__ */
