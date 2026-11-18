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

// Checks the rules for taking and giving up the slots that hold a backend's
// report, with no daemon and no framework. From the top of the source tree:
//
//   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/outputs_callbacks_test src/outputs/tests/outputs_callbacks_test.c
//   /tmp/outputs_callbacks_test
//
// Exits 0 when every case passes, 1 otherwise. Also run by "make check".

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// What outputs.h gives the registry
struct output_device;
enum output_device_state { STATE_STOPPED = 0, STATE_CONNECTED = 2 };
typedef void (*output_status_cb)(struct output_device *device, enum output_device_state status);

#include "../../outputs_callbacks.h"

#define N 6
#define DEV_A 1001
#define DEV_B 2002

static struct outputs_callback_register reg[N];
static int cases_run;
static int cases_failed;

static void cb_start(struct output_device *device, enum output_device_state status) { (void)device; (void)status; }
static void cb_level(struct output_device *device, enum output_device_state status) { (void)device; (void)status; }
static void cb_stop(struct output_device *device, enum output_device_state status) { (void)device; (void)status; }

static void
expect(const char *name, bool ok)
{
  cases_run++;
  if (ok)
    return;

  cases_failed++;
  printf("FAIL %s\n", name);
}

static int
used(uint64_t device_id)
{
  int i, n = 0;

  for (i = 0; i < N; i++)
    if (reg[i].cb && reg[i].reg_device_id == device_id)
      n++;

  return n;
}

static int
add(uint64_t device_id, output_status_cb cb, uint32_t gen, bool op_slot, enum outputs_callback_add_case *how)
{
  int conflict;

  return outputs_callbacks_add(reg, N, cb, device_id, gen, op_slot, how, &conflict);
}

static void
mark_ready(int id, uint64_t device_id)
{
  reg[id].ready = true;
  reg[id].device_id = device_id;
  reg[id].state = STATE_CONNECTED;
}

static void
reset(void)
{
  memset(reg, 0, sizeof(reg));
}

// Case (i): the same operation and generation reuses the slot
static void
test_in_place(void)
{
  enum outputs_callback_add_case how;
  int a, b;

  reset();
  a = add(DEV_A, cb_start, 0, true, &how);
  b = add(DEV_A, cb_stop, 0, true, &how);
  expect("in place: same slot", a == b && a >= 0);
  expect("in place: reported", how == OUTPUTS_CALLBACK_IN_PLACE);
  expect("in place: new callback", reg[a].cb == cb_stop);
  expect("in place: one slot", used(DEV_A) == 1);

  // Declined after an in-place reuse: the slot stays, the backend still
  // holds it
  outputs_callbacks_settle(reg, N, DEV_A, b, false, false);
  expect("in place, declined: kept", used(DEV_A) == 1);
}

// Case (ii): a level slot is waiting, an operation begins: two slots, and
// the level slot goes once the backend has taken the operation's id
static void
test_new_operation_over_level(void)
{
  enum outputs_callback_add_case how;
  int level, op, released;

  reset();
  level = add(DEV_A, cb_level, 0, false, &how);
  expect("level: fresh", how == OUTPUTS_CALLBACK_FRESH);
  op = add(DEV_A, cb_stop, 1, true, &how);
  expect("operation over a level: fresh, no complaint", how == OUTPUTS_CALLBACK_FRESH && op != level);
  expect("operation over a level: two slots", used(DEV_A) == 2);

  released = outputs_callbacks_settle(reg, N, DEV_A, op, true, true);
  expect("taken: the level slot given up", released == 1 && used(DEV_A) == 1 && reg[op].cb == cb_stop);
}

// Case (ii), declined: the new slot goes and the older one is kept
static void
test_declined(void)
{
  enum outputs_callback_add_case how;
  int start, level, released;

  reset();
  start = add(DEV_A, cb_start, 0, true, &how);
  level = add(DEV_A, cb_level, 0, false, &how);
  expect("level during a start: its own slot", level != start && how == OUTPUTS_CALLBACK_FRESH);

  released = outputs_callbacks_settle(reg, N, DEV_A, level, true, false);
  expect("declined: the new slot given up", released == 1 && !reg[level].cb);
  expect("declined: the start's slot kept", reg[start].cb == cb_start && used(DEV_A) == 1);
}

// Case (iii): a second operation while one is in flight is a bug, and still
// gets a slot of its own
static void
test_second_operation(void)
{
  enum outputs_callback_add_case how;
  int first, second, conflict;

  reset();
  first = add(DEV_A, cb_start, 3, true, &how);
  second = outputs_callbacks_add(reg, N, cb_stop, DEV_A, 4, true, &how, &conflict);
  expect("second operation: reported", how == OUTPUTS_CALLBACK_SECOND_OP);
  expect("second operation: names the first", conflict == first);
  expect("second operation: a slot of its own", second >= 0 && second != first);

  // A level is never a second operation
  add(DEV_A, cb_level, 4, false, &how);
  expect("a level beside an operation: no complaint", how == OUTPUTS_CALLBACK_FRESH);
}

// A ready slot is never touched by a registration or a settle
static void
test_ready_survives(void)
{
  enum outputs_callback_add_case how;
  int start, again, released;

  reset();
  start = add(DEV_A, cb_start, 0, true, &how);
  mark_ready(start, DEV_A);

  again = add(DEV_A, cb_start, 0, true, &how);
  expect("ready: not reused", again != start && how == OUTPUTS_CALLBACK_FRESH);
  expect("ready: callback kept", reg[start].cb == cb_start && reg[start].ready);

  released = outputs_callbacks_settle(reg, N, DEV_A, again, true, true);
  expect("ready: not given up by a settle", released == 0 && reg[start].ready);

  again = add(DEV_A, cb_level, 0, false, &how);
  outputs_callbacks_settle(reg, N, DEV_A, again, true, false);
  expect("ready: not given up by a decline", reg[start].ready && reg[start].cb == cb_start);
}

// Another device's slots are left alone
static void
test_other_device(void)
{
  enum outputs_callback_add_case how;
  int a, b;

  reset();
  b = add(DEV_B, cb_level, 0, false, &how);
  a = add(DEV_A, cb_level, 0, false, &how);
  expect("other device: not reused", a != b);
  outputs_callbacks_settle(reg, N, DEV_A, a, true, true);
  expect("other device: not given up", reg[b].cb == cb_level && used(DEV_B) == 1);
}

static void
test_full(void)
{
  enum outputs_callback_add_case how;
  int i, id;

  reset();
  for (i = 0; i < N; i++)
    {
      id = add(DEV_B + (uint64_t)i, cb_level, 0, false, &how);
      mark_ready(id, DEV_B + (uint64_t)i);
    }

  id = add(DEV_A, cb_start, 0, true, &how);
  expect("full: -1", id == -1 && how == OUTPUTS_CALLBACK_FULL);
  outputs_callbacks_settle(reg, N, DEV_A, id, false, false);
  for (i = 0; i < N; i++)
    expect("full: nothing given up", reg[i].ready && reg[i].cb == cb_level);
}

// A report goes only into a slot its device took
static void
test_owner(void)
{
  enum outputs_callback_add_case how;
  int a;

  reset();
  a = add(DEV_A, cb_start, 0, true, &how);
  expect("owner: the device that took it", outputs_callbacks_owner(reg, N, a, DEV_A));
  expect("owner: another device dropped", !outputs_callbacks_owner(reg, N, a, DEV_B));
  expect("owner: a free slot", !outputs_callbacks_owner(reg, N, (a + 1) % N, DEV_A));
  expect("owner: out of range", !outputs_callbacks_owner(reg, N, N, DEV_A) && !outputs_callbacks_owner(reg, N, -1, DEV_A));

  // The id a retry still holds was given up and taken by another device
  outputs_callbacks_settle(reg, N, DEV_A, -1, false, true);
  a = add(DEV_B, cb_level, 0, false, &how);
  expect("owner: an id given up and taken again", !outputs_callbacks_owner(reg, N, a, DEV_A) && outputs_callbacks_owner(reg, N, a, DEV_B));
}

int
main(void)
{
  test_in_place();
  test_new_operation_over_level();
  test_declined();
  test_second_operation();
  test_ready_survives();
  test_other_device();
  test_full();
  test_owner();

  printf("%d cases, %d failed\n", cases_run, cases_failed);
  return cases_failed ? 1 : 0;
}
