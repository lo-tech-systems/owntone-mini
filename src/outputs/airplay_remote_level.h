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

#ifndef __AIRPLAY_REMOTE_LEVEL_H__
#define __AIRPLAY_REMOTE_LEVEL_H__

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Whether value is a number rather than a NaN, judged from its bits. The
// release builds use -Ofast, whose -ffinite-math-only lets the compiler
// assume no value is ever a NaN: isnan() is then always false and a
// comparison that a NaN would fail can be rewritten as one it passes, so
// neither can be used to catch one.
static inline bool
airplay_remote_level_is_number(double value)
{
  uint64_t bits;

  memcpy(&bits, &value, sizeof(bits));
  return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000)
         || (bits & UINT64_C(0x000fffffffffffff)) == 0;
}

// An AirPlay 2 receiver that reports a turn of its own volume control on the
// event channel (command "dvlc") gives its level as a fraction of its range,
// 0 to 1. Levels are sent to receivers in dB, -30 to 0 for 1 to 100 and -144
// for 0, so this returns the reported level in dB on that scale, ready for
// the same conversion back to 0 to 100 as a level reported over DACP.
//
// A receiver can report the middle of its step (one set to 17 reports 0.175),
// so the level is the whole step at or just below the fraction, allowing for
// a fraction a little under a whole step. It is never under 1 for a fraction
// above 0, as the receiver is still playing; 0, less, or not a number is
// silent.
static inline double
airplay_remote_level_db(double fraction)
{
  double steps;

  if (!airplay_remote_level_is_number(fraction) || !(fraction > 0.0))
    return -144.0;

  steps = floor(fraction * 100.0 + 0.001);
  if (steps < 1.0)
    steps = 1.0;
  if (steps > 100.0)
    steps = 100.0;

  return -30.0 + 30.0 * steps / 100.0;
}

#endif /* !__AIRPLAY_REMOTE_LEVEL_H__ */
