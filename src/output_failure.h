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

#ifndef __OUTPUT_FAILURE_H__
#define __OUTPUT_FAILURE_H__

#include <stdbool.h>

// Why the last start of a device failed, or why it waits for a PIN. The
// backend records the reason at the failure site and writes it to the device
// when it reports OUTPUT_STATE_FAILED or OUTPUT_STATE_PASSWORD; any other
// report clears it. A reason recorded at a failure site wins over the one
// read from the report's state.
enum output_failure_reason
{
  OUTPUT_FAILURE_NONE         = 0,
  OUTPUT_FAILURE_CAPACITY     = 1,
  // The PIN start was sent, so the PIN is on the receiver's screen
  OUTPUT_FAILURE_PIN_REQUIRED,
  // Pair setup with the PIN given was refused
  OUTPUT_FAILURE_PIN_INVALID,
  // A negative answer that is not about authentication
  OUTPUT_FAILURE_REFUSED,
  // No connection, or no answer before the receiver had answered anything
  OUTPUT_FAILURE_UNREACHABLE,
  // No answer after the receiver had answered, or pair setup took too long
  OUTPUT_FAILURE_TIMEOUT,
  // Anchors given up while the receiver sent no delay request to our clock
  OUTPUT_FAILURE_NO_CLOCK,
  // A start that ended because it was no longer wanted
  OUTPUT_FAILURE_CANCELLED,
  // An established session failed with no operation in flight
  OUTPUT_FAILURE_LOST,
};

// The name clients see, "none" for anything unknown
static inline const char *
output_failure_name(enum output_failure_reason reason)
{
  switch (reason)
    {
      case OUTPUT_FAILURE_CAPACITY:     return "capacity";
      case OUTPUT_FAILURE_PIN_REQUIRED: return "pin_required";
      case OUTPUT_FAILURE_PIN_INVALID:  return "pin_invalid";
      case OUTPUT_FAILURE_REFUSED:      return "refused";
      case OUTPUT_FAILURE_UNREACHABLE:  return "unreachable";
      case OUTPUT_FAILURE_TIMEOUT:      return "timeout";
      case OUTPUT_FAILURE_NO_CLOCK:     return "no_clock";
      case OUTPUT_FAILURE_CANCELLED:    return "cancelled";
      case OUTPUT_FAILURE_LOST:         return "lost";
      default:                          return "none";
    }
}

// The reason a backend reports with a FAILED or PASSWORD state. A reason
// recorded at a failure site wins: an authorise whose pair setup timed out is
// reported in the password state, but the receiver never judged the PIN.
// Otherwise a password state is a PIN asked for, or a PIN refused when the
// session was an authorise, and a failure with no recorded reason was refused
static inline enum output_failure_reason
output_failure_reported(enum output_failure_reason recorded, bool password, bool pin_attempt)
{
  if (recorded != OUTPUT_FAILURE_NONE)
    return recorded;
  if (password)
    return pin_attempt ? OUTPUT_FAILURE_PIN_INVALID : OUTPUT_FAILURE_PIN_REQUIRED;

  return OUTPUT_FAILURE_REFUSED;
}

#endif /* !__OUTPUT_FAILURE_H__ */
