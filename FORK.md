# Fork Relationship: owntone-mini vs OwnTone

owntone-mini is a fork of [OwnTone](https://github.com/owntone/owntone-server).
This file documents what was removed, what was changed, and the rationale.

---

## Scope

owntone-mini has a single purpose: stream audio from a named pipe (FIFO) to one or more
AirPlay 1 and/or AirPlay 2 speakers, controlled by an HTTP JSON API.

Everything outside that scope has been removed.

owntone-mini is an independent open-source project. It is not affiliated with, authorised, sponsored or endorsed by Apple Inc. AirPlay, Apple TV, HomePod and iTunes are trademarks of Apple Inc., registered in the U.S. and other countries.

---

## What was removed

### Features

| Removed feature | Rationale |
|----------------|-----------|
| Web interface | Not needed for headless API-only use |
| Library scanning and SQLite database | No file library; queue is a single in-memory pipe item |
| Spotify / LastFM / Chromecast / MPD integrations | Out of scope |
| Smart playlist parser (flex/bison) | No library |
| ALSA / PulseAudio output | Audio stays inside the pipeline; AirPlay is the only output |
| Artwork cache | The cache used SQLite. Artwork now comes from the picture sent on the pipe metadata input, which is held in memory for the session and never written to disk (a `file:` URI given through `PUT /api/metadata` is still read directly) |
| HTTP push notifications (websockets) | No web clients |

### Dependencies

| Removed dependency | Reason removed |
|--------------------|---------------|
| SQLite3 | Queue and speaker state are now in-memory |
| libconfuse | Config is now a JSON file (`owntone-settings.json`) |
| libcurl | Was used for artwork download; removed with cache |
| libxml2 | Was used for smart playlist parsing |
| libunistring | Was used in `unicode_fixup_string`; no library scanning means no charset fixup |
| inotify | Was used to watch the library directory |
| libmount | Was used to detect mount events |
| flex / bison | Were used for the smart playlist parser |

### Source files deleted

```
src/db_init.c / src/db_init.h
src/db_upgrade.c / src/db_upgrade.h
src/cache.c / src/cache.h
src/library.c / src/library.h
src/settings.c / src/settings.h
src/parsers/smartpl_lexer.l
src/parsers/smartpl_parser.y
sqlext/sqlext.c / sqlext/Makefile.am
```

### Source files replaced by minimal shims

These keep their names, so the rest of the code builds unchanged, but the
upstream implementations were removed and replaced with small versions written
for owntone-mini:

| File | What the shim does |
|------|--------------------|
| `src/db.c` / `src/db.h` | Only `db_speaker_save()`, which saves a device's AirPlay pairing key and learned buffered-transport capability in the settings file |
| `src/conffile.c` / `src/conffile.h` | Compatibility layer so the `cfg_get*` calls read from the JSON settings (see `src/owntone_config.c`) |
| `src/artwork.c` / `src/artwork.h` | Serves artwork from the picture held in memory by the pipe input; no cache |
| `src/dmap_common.c` / `src/dmap_common.h` | Inline DMAP text encoding of the current track for AirPlay 1 metadata |

---

## What was changed

### New files

| File | Purpose |
|------|---------|
| `src/queue.c` / `src/queue.h` | In-memory single-item queue (replaces SQLite-backed `db_queue_*`) |
| `src/owntone_config.c` / `src/owntone_config.h` | JSON settings reader and writer (replaces the libconfuse parser that was in `conffile.c`) |
| `src/outputs/airplay_buffered.c` / `.h` | AirPlay 2 buffered-audio (stream type 103) transport and ChaCha20-Poly1305 framing; caps the per-session send backlog and fails the session if a receiver stops draining it |
| `src/memstats.c` / `.h` | Periodic memory-usage logging (resident/peak/locked size, heap in-use and held, malloc arenas, sessions, encoder budget, system memory and swap); also runs a heap trim on its tick |
| `src/outputs/airplay_encoder.c` / `.h` | Threaded per-transform audio encoder feeding the buffered transport; absorbs a stall by draining its backlog instead of dropping audio, warning once it falls about a second behind, and failing the session only past a five-second hard limit |
| `src/outputs/airplay_common.h` | Shared definitions for the buffered/encoder units |
| `src/outputs/airplay_mrp.c` / `.h` | MediaRemote now-playing client for Apple TV: registers the session and pushes track metadata, artwork and playback state over the AirPlay control channel (see `docs/audio-outputs/airplay.md`) |
| `src/outputs/airplay_crypto.c` / `.h` | ChaCha20-Poly1305 primitives shared by the realtime and buffered AirPlay 2 senders, adapted from the upstream AirPlay output |

### Modified files (summary)

| File | Change |
|------|--------|
| `src/player.c` / `src/player.h` | Removed seek, shuffle, repeat, multi-queue, verification kickoff; restarts a TV proxy leader (Apple TV) directly, replacing its audio-suppressed session, if every HomePod follower in its group drops; starts a TV proxy leader's deferred HomePod followers once playback begins; fans per-speaker volume out to every member of an AirPlay 2 stereo group (with or without an Apple TV leader) at one absolute level |
| `src/transcode.c` / `src/transcode.h` | Reduced to encode-only path (PCM → ALAC/PCM16); removed file decode, seeking, metadata extraction. Extended with AirPlay 2 encode profiles (48 kHz AAC stereo, AAC 5.1, 24-bit ALAC), ffmpeg surround-upmix filters, and CPU-class AAC coder selection |
| `src/outputs.c` | Removed XCODE_PCM24/32/UNKNOWN dead references; adds per-output protocol mode selection, buffered-audio capability and mode handling, a device removal grace period for transient mDNS dropouts, and live offset changes; defers the end-of-session heap trim until after the session is freed; tracks whether a TV proxy leader's session should be sent no audio, and whether its follower start was deferred until playback begins |
| `src/misc.c` / `src/misc.h` | Removed: `unicode_fixup_string`, `two_str_hash`, `keyval_sort`, `linear_regression`, `m_readfile`, `atrim`; removed libunistring includes |
| `src/listener.h` | Reduced to 3 event types: PLAYER, VOLUME, SPEAKER |
| `src/logger.c` / `src/logger.h` | Removed unused log domains; removed `logger_alsa`; log lines are handed to a dedicated writer thread through a bounded queue, so the audio threads never wait on the log file (synchronous until the thread starts; lines are dropped and counted if the queue fills), and repetitive lines are throttled at the call site |
| `src/input.c` | Pipe input buffer capacity is a duration (3 s) computed from the stream's sample rate and bit depth, instead of a fixed byte count sized for one format |
| `src/main.c` | Starts from the JSON settings file (creates it if missing, checks its access at startup); reports as `owntone-mini`; locks process memory once daemonized and privileges are dropped, so the audio threads are not delayed by page faults under memory pressure; starts the log writer thread at the same point; logs memory statistics |
| `src/outputs/raop.c` | AirPlay 1 sender. Fixed `raop_metadata_prepare` to build the DMAP text buffer and load file artwork; per-device password from the settings file; bounded backoff retry on soft start failures; applies offset changes to live sessions; sends true mute for volume 0 |
| `src/outputs/airplay.c` | Substantially extended: AirPlay 2 buffered-audio output (RTP type 0x67 / stream type 103) with ChaCha20-Poly1305 framing; PTP-timed playback to HomePod stereo pairs (SETPEERS peer setup, timing-anchor handling); stream-type and audio-format selection/capability negotiation; 5.1 surround to a standalone Apple TV; connection retry/backoff; skips sending audio to a TV proxy leader whose HomePod followers are already rendering it; plus the `airplay_metadata_prepare` DMAP/artwork fix |
| `src/ptpd.c` | PTP grandmaster/announce settings for prompt receiver lock; see also the vendored libairptp changes under Licence |
| `src/inputs/pipe.c` | Rewritten as the only input. Reads metadata items without libxml2, applies a metadata bundle as one update, holds the current picture in memory for the session, keeps its autostart watch on the FIFO and re-arms it, owns the metadata watch on the pipe thread, reloads its settings live, and logs feed-rate diagnostics |
| `src/httpd_jsonapi.c` | Largely rewritten around the pipe-only scope. Serves the player, outputs, settings, config and metadata endpoints; adds per-output protocol mode, offset and buffered-audio capability, runtime-settable settings written back to the JSON file, and a restart-required status. `PUT /api/metadata` is new |
| `src/httpd.c` | Reduced to the JSON API and DACP; evaluates peer trust when a request arrives; accepts receiver volume reports over DACP |
| `src/httpd_dacp.c` | Rewritten to the few DACP requests AirPlay receivers send back (`setproperty` for volume and device volume, `volumeup`, `volumedown`), which update the speaker volume held by the player |
| `src/mdns_avahi.c` | Logs mDNS changes once instead of on every announcement; supports one in-process rediscovery when all AirPlay outputs have disappeared |
| `owntone.service.in` | Freezes glibc's malloc thresholds (`MALLOC_MMAP_THRESHOLD_`, `MALLOC_TRIM_THRESHOLD_`) low, alongside the existing `MALLOC_ARENA_MAX=2`, so heap freed after a large transient allocation is handed back to the OS instead of being retained by the allocator |
| `configure.ac` | Removed: LIBCURL, LIBXML2, INOTIFY, libunistring, AM_ICONV, sqlext |
| `Makefile.am` | Removed `sqlext` from SUBDIRS |

---

## Upstream

https://github.com/owntone/owntone-server

## Licence

owntone-mini is licensed under the GNU General Public License, version 2 or
later, the same licence as OwnTone.

- The original copyright notices are retained in full in each file.
- Files modified for owntone-mini say so in their header.
- Modifications and new files are Copyright (C) 2026 James Pearce.

Vendored components keep their own licences unchanged: `src/pair_ap` (MIT),
`src/libairptp` (MIT) and `src/evrtsp` (BSD-3-Clause). `src/pair_ap` has no
changes. The fork makes small changes to two of them:

| Component | Change |
|-----------|--------|
| `src/libairptp` | Announce interval changed to 0.25 s and sent at that rate, so AirPlay 2 receivers lock to the grandmaster promptly (`airptp_internal.h`); clearer PTP port-bind error messages (`airptp.c`); a failed `getaddrinfo()` in `utils_net_sockaddr_get()` now returns an error without calling `freeaddrinfo()` on an unset pointer (`utils.c`); comment added in `ptp_msg_handle.c`, no code change |
| `src/evrtsp` | Added the `SETRATEANCHORTIME` and `FLUSHBUFFERED` RTSP methods (`evrtsp.h`, `rtsp.c`) |
