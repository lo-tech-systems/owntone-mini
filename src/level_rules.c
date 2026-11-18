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

#include "level_rules.h"

int
level_to_send(bool silent, int volume)
{
  return silent ? 0 : volume;
}

bool
level_wire_stale(bool silent, int volume, int volume_sent)
{
  return level_to_send(silent, volume) != volume_sent;
}

static bool
within(uint64_t at_ms, uint64_t now_ms, uint64_t window_ms)
{
  return at_ms != 0 && now_ms >= at_ms && now_ms - at_ms < window_ms;
}

enum level_report
level_report_classify(bool silent, bool asks, int level,
                      uint64_t sent_ms, bool sent_restores, uint64_t wake_sent_ms,
                      uint64_t now_ms, uint64_t window_ms)
{
  // A request: no answer rule applies
  if (asks)
    return silent ? LEVEL_REPORT_WAKE : LEVEL_REPORT_RECORD;

  if (silent)
    {
      // The answer to our 0, whatever its level
      if (within(sent_ms, now_ms, window_ms))
	return LEVEL_REPORT_ABSORB;
      // Where we put it
      if (level == 0)
	return LEVEL_REPORT_ABSORB;
      // A later step of a turn already forwarded
      if (within(wake_sent_ms, now_ms, window_ms))
	return LEVEL_REPORT_ABSORB;

      return LEVEL_REPORT_WAKE;
    }

  // Settling after the level was put back
  if (sent_restores && within(sent_ms, now_ms, window_ms))
    return LEVEL_REPORT_ABSORB;

  return LEVEL_REPORT_RECORD;
}
