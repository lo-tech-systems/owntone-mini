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

// Logs the process and system memory figures as a single line at E_INFO,
// domain L_MAIN, from the worker thread. Must be called on the player thread,
// which is where the session count and the encoder budget are read. "reason"
// is appended to the line (e.g. "last session ended") and must stay valid, so
// pass a string literal.
void
memstats_log_async(const char *reason);

// Starts the periodic tick that logs the figures every 5 minutes while
// playing and every hour while idle. Runs on the worker thread, which only asks
// the player thread for the session count and the encoder budget.
int
memstats_init(void);

void
memstats_deinit(void);

#endif /* !__MEMSTATS_H__ */
