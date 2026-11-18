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

// Which kinds of session state were seen among the other devices of a group
struct speakers_seen
{
  bool streaming;
  bool starting;
  bool password;
  bool failed;
};

enum speakers_summary
{
  SPEAKERS_SUMMARY_STOPPED,
  SPEAKERS_SUMMARY_STARTING,
  SPEAKERS_SUMMARY_PASSWORD,
  SPEAKERS_SUMMARY_FAILED,
  SPEAKERS_SUMMARY_STREAMING,
};

// One state for the group: streaming if any streams, else starting, else
// password, else failed, else stopped
static inline enum speakers_summary
speakers_summary_resolve(struct speakers_seen seen)
{
  if (seen.streaming)
    return SPEAKERS_SUMMARY_STREAMING;
  if (seen.starting)
    return SPEAKERS_SUMMARY_STARTING;
  if (seen.password)
    return SPEAKERS_SUMMARY_PASSWORD;
  if (seen.failed)
    return SPEAKERS_SUMMARY_FAILED;

  return SPEAKERS_SUMMARY_STOPPED;
}

#endif /* !__SPEAKERS_SUMMARY_H__ */
