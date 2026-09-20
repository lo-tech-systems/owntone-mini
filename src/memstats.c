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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "logger.h"
#include "worker.h"
#include "player.h"
#include "outputs.h"
#include "memstats.h"

// How often the worker thread checks whether a line is due
#define MEMSTATS_TICK_SECONDS 60
// How often a line is actually logged, depending on playback state
#define MEMSTATS_INTERVAL_PLAYING 300
#define MEMSTATS_INTERVAL_IDLE 3600

static time_t memstats_last_logged;
static bool memstats_stopped = true;


// Looks up "key" (including its trailing colon) as a line prefix in fp and
// parses the kB figure that follows it, e.g. "VmRSS:   1234 kB". Returns 0
// and sets *val on success, -1 if the key isn't found or doesn't parse.
static int
proc_status_kb(FILE *fp, const char *key, unsigned long *val)
{
  char line[256];
  size_t keylen = strlen(key);

  rewind(fp);
  while (fgets(line, sizeof(line), fp))
    {
      if (strncmp(line, key, keylen) != 0)
	continue;

      if (sscanf(line + keylen, "%lu", val) == 1)
	return 0;

      break;
    }

  return -1;
}

// Counts the per-thread malloc arenas by asking glibc to dump its arena
// stats as XML into a memory-backed stream, then counting "<heap nr="
// occurrences. Returns -1 if the dump isn't available.
static int
memstats_arena_count(void)
{
  char *buf = NULL;
  size_t size = 0;
  FILE *stream;
  int count;
  char *p;

  stream = open_memstream(&buf, &size);
  if (!stream)
    return -1;

  if (malloc_info(0, stream) != 0)
    {
      fclose(stream);
      free(buf);
      return -1;
    }

  fclose(stream);

  count = 0;
  for (p = buf; (p = strstr(p, "<heap nr=")); p += strlen("<heap nr="))
    count++;

  free(buf);

  return count;
}

void
memstats_log(const char *reason)
{
  FILE *fp;
  unsigned long vm_rss_kb = 0;
  unsigned long vm_hwm_kb = 0;
  unsigned long vm_lck_kb = 0;
  unsigned long mem_available_kb = 0;
  unsigned long swap_total_kb = 0;
  unsigned long swap_free_kb = 0;
  unsigned long swap_used_mib;
  unsigned long heap_inuse_mib;
  unsigned long heap_held_mib;
  int arenas;
  int sessions;
  int enc_in_use;
  int enc_budget;
  struct mallinfo2 mi;

  fp = fopen("/proc/self/status", "r");
  if (fp)
    {
      proc_status_kb(fp, "VmRSS:", &vm_rss_kb);
      proc_status_kb(fp, "VmHWM:", &vm_hwm_kb);
      proc_status_kb(fp, "VmLck:", &vm_lck_kb);
      fclose(fp);
    }

  fp = fopen("/proc/meminfo", "r");
  if (fp)
    {
      proc_status_kb(fp, "MemAvailable:", &mem_available_kb);
      proc_status_kb(fp, "SwapTotal:", &swap_total_kb);
      proc_status_kb(fp, "SwapFree:", &swap_free_kb);
      fclose(fp);
    }

  swap_used_mib = (swap_total_kb > swap_free_kb) ? (swap_total_kb - swap_free_kb) / 1024 : 0;

  mi = mallinfo2();
  heap_inuse_mib = ((unsigned long)mi.uordblks + (unsigned long)mi.hblkhd) / 1048576;
  heap_held_mib = ((unsigned long)mi.arena + (unsigned long)mi.hblkhd) / 1048576;

  arenas = memstats_arena_count();

  sessions = outputs_sessions_count();

  airplay_encoder_budget_get(&enc_in_use, &enc_budget);

  DPRINTF(E_INFO, L_MAIN, "memory: rss %lu MiB (peak %lu, locked %lu), heap in-use %lu MiB held %lu MiB arenas %d, sessions %d, encoders %d/%d, system available %lu MiB, swap used %lu MiB%s%s\n",
          vm_rss_kb / 1024, vm_hwm_kb / 1024, vm_lck_kb / 1024,
          heap_inuse_mib, heap_held_mib, arenas,
          sessions, enc_in_use, enc_budget,
          mem_available_kb / 1024, swap_used_mib,
          reason ? " - " : "", reason ? reason : "");

  memstats_last_logged = time(NULL);
}

static void
memstats_tick_cb(void *arg)
{
  struct player_status status;
  bool playing;
  time_t now;
  int interval;

  if (memstats_stopped)
    return;

  playing = (player_get_status(&status) == 0 && status.status == PLAY_PLAYING);
  interval = playing ? MEMSTATS_INTERVAL_PLAYING : MEMSTATS_INTERVAL_IDLE;

  now = time(NULL);
  if (now - memstats_last_logged >= interval)
    {
      player_memstats_log();
      memstats_last_logged = now;
    }

  worker_execute(memstats_tick_cb, NULL, 0, MEMSTATS_TICK_SECONDS);
}

int
memstats_init(void)
{
  memstats_stopped = false;
  memstats_last_logged = 0;

  worker_execute(memstats_tick_cb, NULL, 0, MEMSTATS_TICK_SECONDS);

  return 0;
}

void
memstats_deinit(void)
{
  memstats_stopped = true;
}
