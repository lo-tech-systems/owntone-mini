/*
 * db.c — compatibility shim for owntone-minimal
 *
 * Implements db_speaker_save_auth_key() and db_speaker_save_buffered_modes(),
 * the only db_* functions still called by airplay.c and raop.c after the
 * SQLite database was removed. Also used by outputs.c to persist a device's
 * buffered-transport capability.
 *
 * Minimal replacement for the OwnTone module of the same name, written for
 * owntone-mini.
 *
 * Copyright (C) 2026 James Pearce
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "outputs.h"        /* struct output_device (full definition) */
#include "owntone_config.h" /* config_set_device_str, config_set_device_int_deferred */
#include "db.h"

/* Persist the AirPlay pairing auth_key and the learned buffered-transport
 * capability bitmask for this device into the "airplay_devices" block of
 * owntone-settings.json so both survive a restart. Written now: the user has
 * just paired, so the key must not be lost. */
void
db_speaker_save_auth_key(struct output_device *device)
{
  if (!device)
    return;
  config_set_device_int_deferred(device->name, "buffered_modes", (int)device->buffered_modes);
  config_set_device_str(device->name, "auth_key", device->auth_key);
}

/* Persist the learned buffered-transport capability bitmask for this device.
 * The file is written a moment later by a worker thread, so the caller never
 * waits for it. */
void
db_speaker_save_buffered_modes(struct output_device *device)
{
  if (!device)
    return;
  config_set_device_int_deferred(device->name, "buffered_modes", (int)device->buffered_modes);
}
