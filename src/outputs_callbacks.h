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

#ifndef __OUTPUTS_CALLBACKS_H__
#define __OUTPUTS_CALLBACKS_H__

// The slots that hold a backend's report for the player until it is made, and
// the rules for taking and giving them up. Kept apart from outputs.c, and free
// of any device dereference, so the rules can be tested on their own.
// Include after outputs.h (or after defining output_status_cb and enum
// output_device_state).

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct outputs_callback_register
{
  output_status_cb cb;
  // Never dereferenced: the device may be gone when the report comes
  struct output_device *device;

  // We have received the callback with the result from the backend
  bool ready;

  // The device the backend reported for, and what it reported
  uint64_t device_id;
  enum output_device_state state;

  // Who took the slot and for which operation, set when it is taken: a
  // report for another device is dropped, and one for an older generation of
  // this device moves no phase
  uint64_t reg_device_id;
  uint32_t gen;
  // Taken by an operation (a start, probe, stop or authorise), not by a level,
  // a flush or a listener
  bool op_slot;
};

enum outputs_callback_add_case
{
  // A free slot was taken
  OUTPUTS_CALLBACK_FRESH,
  // The device's slot of the same operation and generation was reused
  OUTPUTS_CALLBACK_IN_PLACE,
  // A free slot was taken, but another operation of the device has not
  // reported yet, which one operation at a time never allows
  OUTPUTS_CALLBACK_SECOND_OP,
  // No slot was free
  OUTPUTS_CALLBACK_FULL,
};

// Takes a slot for a report the backend will make for a device. A ready slot
// is never touched. A slot not yet ready that belongs to the same operation
// and generation is reused; for anything else a free slot is taken and the
// device's other slots are left for outputs_callbacks_settle(). Returns the
// slot, or -1 when none is free. *conflict is the slot of the operation still
// in flight for OUTPUTS_CALLBACK_SECOND_OP, else -1
static inline int
outputs_callbacks_add(struct outputs_callback_register *reg, int n, output_status_cb cb, struct output_device *device,
		      uint64_t device_id, uint32_t gen, bool op_slot, enum outputs_callback_add_case *how, int *conflict)
{
  int free_id = -1;
  int i;

  *conflict = -1;

  for (i = 0; i < n; i++)
    {
      if (!reg[i].cb)
	{
	  if (free_id < 0)
	    free_id = i;
	  continue;
	}

      if (reg[i].ready || reg[i].reg_device_id != device_id)
	continue;

      if (reg[i].gen == gen && reg[i].op_slot == op_slot)
	{
	  reg[i].cb = cb;
	  reg[i].device = device;
	  *how = OUTPUTS_CALLBACK_IN_PLACE;
	  return i;
	}

      if (op_slot && reg[i].op_slot)
	*conflict = i;
    }

  if (free_id < 0)
    {
      *how = OUTPUTS_CALLBACK_FULL;
      return -1;
    }

  *how = (*conflict >= 0) ? OUTPUTS_CALLBACK_SECOND_OP : OUTPUTS_CALLBACK_FRESH;

  memset(&reg[free_id], 0, sizeof(reg[free_id]));
  reg[free_id].cb = cb;
  reg[free_id].device = device;
  reg[free_id].reg_device_id = device_id;
  reg[free_id].gen = gen;
  reg[free_id].op_slot = op_slot;
  return free_id;
}

// Settles a device's slots once the backend call is made. A backend session
// keeps one callback id, so when the backend took the new id, the device's
// other slots that are not ready can never be reported to and are given up.
// When it did not take it, a slot taken afresh for the call is given up and
// the others are kept; a slot reused in place stays. Returns the number of
// slots given up
static inline int
outputs_callbacks_settle(struct outputs_callback_register *reg, int n, uint64_t device_id, int new_id, bool fresh, bool taken)
{
  int released = 0;
  int i;

  if (!taken)
    {
      if (new_id >= 0 && fresh && !reg[new_id].ready)
	{
	  memset(&reg[new_id], 0, sizeof(reg[new_id]));
	  released++;
	}
      return released;
    }

  for (i = 0; i < n; i++)
    {
      if (i == new_id || !reg[i].cb || reg[i].ready || reg[i].reg_device_id != device_id)
	continue;

      memset(&reg[i], 0, sizeof(reg[i]));
      released++;
    }

  return released;
}

// Whether a report the backend makes for a device may go into the slot: the
// slot is taken, and by that device
static inline bool
outputs_callbacks_owner(const struct outputs_callback_register *reg, int n, int id, uint64_t device_id)
{
  return id >= 0 && id < n && reg[id].cb && reg[id].reg_device_id == device_id;
}

#endif /* !__OUTPUTS_CALLBACKS_H__ */
