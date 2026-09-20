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

#ifndef __MEMSTATS_H__
#define __MEMSTATS_H__

// Gathers process and system memory figures and logs them as a single line
// at E_INFO, domain L_MAIN. Must be called on the player thread, since it
// calls outputs_sessions_count(). "reason", if non-NULL, is appended to the
// line (e.g. "last session ended"); pass NULL for the periodic tick.
void
memstats_log(const char *reason);

// Starts the periodic tick that logs memstats_log() every 5 minutes while
// playing and every hour while idle. Runs on the worker thread, which hops
// onto the player thread for the actual logging.
int
memstats_init(void);

void
memstats_deinit(void);

#endif /* !__MEMSTATS_H__ */
