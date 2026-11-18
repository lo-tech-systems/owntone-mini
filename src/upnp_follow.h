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

#ifndef __UPNP_FOLLOW_H__
#define __UPNP_FOLLOW_H__

// Following the level a speaker sets itself (its knob, buttons or app) when
// the speaker reports it only over UPnP: some AirPlay speakers built on a
// Linkplay module tell neither DACP nor the AirPlay event channel, but their
// UPnP RenderingControl sends an event for every change. Each AirPlay output
// is looked for with an SSDP search; one that answers from its own address is
// subscribed to, and the levels it reports are recorded by the player and
// passed to the event channel as device_volume "upnp.volume". Nothing is ever
// sent to a renderer but searches and subscriptions.
//
// It runs on a thread of its own and calls the player only through its
// commands. Settings: upnp_volume_follow (on/off, at once) and upnp_event_port
// (the port events arrive on, read at start); per device in airplay_devices,
// upnp_follow (false leaves the device alone) and upnp_volume_offset (learned).

// Thread: main. Starts the thread and registers for speaker changes. Must run
// before the player starts and after the settings are loaded. A failure is
// logged and leaves the feature off; it is not fatal.
int
upnp_follow_init(void);

// Thread: main, after the player has started. Opens the listener and the
// search socket and looks at the speakers already known.
void
upnp_follow_start(void);

// Thread: main. Unsubscribes from every renderer (waiting at most a second
// for the answers) and stops. Must run while the player is still running.
// Safe to call more than once.
void
upnp_follow_shutdown(void);

// Thread: main. Joins the thread. Must run after the player thread has been
// joined.
void
upnp_follow_deinit(void);

// Any thread. upnp_volume_follow or a device's upnp_follow has changed.
void
upnp_follow_settings_changed(void);

#endif /* !__UPNP_FOLLOW_H__ */
