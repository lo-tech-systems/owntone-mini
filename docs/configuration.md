# Configuration

owntone-mini is configured through a single JSON file. There is no separate
library or database configuration: the daemon reads audio from a named pipe
(FIFO) and sends it to AirPlay speakers.

## The settings file

- Default location: `$(sysconfdir)/owntone-settings.json`. When configured with
  `--sysconfdir=/etc` this is `/etc/owntone-settings.json`. Use `-c <file>` on
  the command line to read a different file.
- If the file does not exist, the daemon creates it with the built-in defaults
  at startup.
- If the file exists but lacks some keys (for example after an upgrade added
  new settings), the daemon adds the missing keys with their default values and
  writes the file back. Existing values are never changed by this.
- The file is plain JSON. Comments are not allowed. If the file cannot be
  parsed, the daemon refuses to start.
- The daemon runs as the user named in `uid`. When started as root it corrects
  the file's owner and mode (`0644`) to match that user, so that the API can
  write settings back.
- Settings are read once at startup, unless the tables below say otherwise.
  Some can also be changed while running through the JSON API (see
  [json-api.md](json-api.md)). A change made through the API is written back to
  the file immediately. Settings that need a restart are flagged in the table.
  Edit the file while the daemon is stopped, because the daemon rewrites the
  whole file whenever it saves a setting.
- The repository file `owntone-settings.json` is a reference template. It is
  not installed.

Example:

```json
{
  "uid": "owntone",
  "server_name": "owntone-mini",
  "loglevel": 3,
  "logfile": "/var/log/owntone.log",
  "pipe_path": "/run/autostream-pipes/autostream.fifo",
  "pipe_autostart": true,
  "pipe_sample_rate": 44100,
  "pipe_bits_per_sample": 16,
  "ipv6": true,
  "start_buffer_ms": 2250,
  "trusted_networks": ["lan"],
  "airplay_devices": {
    "Kitchen": { "max_volume": 8 },
    "Bedroom": { "nickname": "Bedroom HomePod", "reconnect": true }
  }
}
```

## Top-level settings

"API" means the setting can be read and changed with
`GET`/`PUT /api/settings/{category}/{option}` (the category is shown in
brackets). Values sent through the API are validated as shown. A value written
by hand into the file is read as is; where the daemon validates it at startup,
that is noted.

| Key | Type | Default | Valid values | Description | API |
|-----|------|---------|--------------|-------------|-----|
| `uid` | string | `"owntone"` | An existing system user | User the daemon runs as after startup. The daemon will not start if the user does not exist. | No |
| `server_name` | string | `"owntone-mini"` | Any string | Name the daemon uses to identify itself to AirPlay receivers. | No |
| `loglevel` | integer | `3` | 0 to 5 (0 fatal, 1 log, 2 warning, 3 info, 4 debug, 5 spam). Out-of-range values are clamped. | Log verbosity. The `-d` command line option overrides it. | Yes (`misc`) |
| `logfile` | string | `"/var/log/owntone.log"` | A writable path | Log file. If the key is absent, no log file is used. | No |
| `logformat` | string | `"default"` | `default`, `logfmt` | Log line format. Not written to the file by default; add it by hand if wanted. The `--logformat` command line option overrides it. | No |
| `pipe_path` | string | `"/run/autostream-pipes/autostream.fifo"` | Path to an existing FIFO | The named pipe audio is read from. Via the API the path must exist, be a FIFO and be accessible, otherwise the change is rejected. Applied without a restart by `PUT /api/update` (see [Pipe input](inputs/pipe.md)). | Yes (`misc`) |
| `pipe_autostart` | boolean | `true` | `true`, `false` | Start playback automatically when audio data appears on the pipe. Applied without a restart by `PUT /api/update`. | Yes (`misc`) |
| `pipe_sample_rate` | integer | `44100` | `44100`, `48000`, `88200`, `96000` | Sample rate of the audio written to the pipe, in Hz. An invalid value in the file stops startup. Restart required. | Yes (`player`) |
| `pipe_bits_per_sample` | integer | `16` | `16`, `32` | Sample size of the audio written to the pipe. An invalid value in the file stops startup. Restart required. | Yes (`player`) |
| `resample_quality` | string | `"standard"` | `standard`, `high` | See [resample_quality](#resample_quality). | Yes (`player`) |
| `ipv6` | boolean | `true` | `true`, `false` | Listen on IPv6 as well as IPv4, and use IPv6 for AirPlay and mDNS. The daemon adds a missing key as `true` at startup. Restart required. | Yes (`misc`) |
| `start_buffer_ms` | integer | `2250` | 300 to 3500 via the API | Milliseconds of audio the outputs buffer before playback starts. The default matches what AirPlay receivers expect, and a different value is logged as a warning. Some receivers ignore it. Restart required. | Yes (`player`) |
| `uncompressed_alac` | boolean | `true` | `true`, `false` | Use uncompressed ALAC framing on the realtime AirPlay path, and use lossless ALAC instead of AAC on buffered AirPlay 2 receivers that support it. Restart required. | Yes (`player`) |
| `buffered_audio_enabled` | boolean | `true` | `true`, `false` | See [buffered_audio_enabled](#buffered_audio_enabled). | Yes (`player`) |
| `buffered_encoder_budget` | integer | `0` | 0 to 64 via the API | See [buffered_encoder_budget](#buffered_encoder_budget). | Yes (`player`) |
| `tv_proxy_leader_audio_suppress` | boolean | `true` | `true`, `false` | See [tv_proxy_leader_audio_suppress](#tv_proxy_leader_audio_suppress). | Yes (`player`) |
| `device_removal_grace_period` | integer | `180` | 0 to 3600 (seconds) | How long a speaker that has disappeared from the network is kept before it is removed. A value outside the range in the file is replaced by 180 with a warning. | Yes (`player`) |
| `speaker_autoselect` | boolean | `false` | `true`, `false` | If playback is started with no speaker selected, start on every available speaker. Not written to the file by default. | No |
| `clear_queue_on_stop_disable` | boolean | `false` | `true`, `false` | Stopping playback normally clears the queue. Set `true` to keep the queue. Not written to the file by default. | No |
| `high_resolution_clock` | boolean | `true` | `true`, `false` | Set `false` on a system whose timer resolution is too coarse for the player tick, which makes the player use a longer tick interval. Not written to the file by default. | No |
| `airplay_timing_port` | integer | `0` | A free UDP port, or `0` | Fixed local port for AirPlay timing. `0` lets the system choose. Useful when a firewall needs a known port. Restart required. | No |
| `airplay_control_port` | integer | `0` | A free UDP port, or `0` | Fixed local port for AirPlay control. `0` lets the system choose. Restart required. | No |
| `user_agent` | string | Derived from the build (`<product>/<version>`) | Up to 255 bytes, no control characters | `User-Agent` header sent on AirPlay RTSP requests. Useful when a receiver behaves differently depending on the sender. Not written to the file by default. An empty string sent through the API removes the key and restores the derived value, and GET always reports the effective value. Restart required. | Yes (`misc`) |
| `port` | integer | `3689` | A free TCP port | Port for the HTTP JSON API. Not written to the file by default. Restart required. | No |
| `bind_address` | string | Not set (all addresses) | An IP address, or `::` | Address the HTTP, AirPlay and PTP services bind to. When set, mDNS discovery is also limited to the interface that has this address. `::` is the same as not setting it. Not written to the file by default. | No |
| `allow_origin` | string | `"*"` | An origin, `*`, or empty | Value of the `Access-Control-Allow-Origin` header on API responses. An empty string turns the header off. Not written to the file by default. | No |
| `admin_password` | string | Not set | Any string | Password for HTTP basic auth (user `admin`) for clients outside `trusted_networks`. Not written to the file by default. | No |
| `trusted_networks` | list of strings | `["lan"]` | See [Access control](#access-control) | Which clients may use the API without a password. | No |
| `airplay_devices` | object | `{}` | See [Per-device settings](#per-device-settings) | Settings for individual speakers, keyed by device name. | No |

Keys marked "Not written to the file by default" are read by the daemon if you
add them, but the daemon does not create them.

### buffered_audio_enabled

When `true`, an AirPlay 2 output left on the `auto` mode preference prefers
buffered AAC-LC playback (`airplay2_buffered`) over realtime, but only on a
device that has advertised an actual AAC bufferStream format (learned from the
receiver's info response), not merely the mDNS "SupportsBufferedAudio" flag. A
device that has not advertised a concrete buffered format falls back to `raop`
(AirPlay 1), and realtime `airplay2` is used only as a last resort for a
receiver that supports nothing else. When `false`, `auto` never uses buffered
(`raop`, then realtime `airplay2`).

The default is `true` so that outputs left on `auto` benefit from buffered
timing without manual configuration. Because the buffered path is still gated
per device on an advertised format, enabling it globally does not force
buffered onto a receiver that cannot play it.

This setting only affects outputs left on `auto`. An output with an explicit
`mode` selection (via the [output API](json-api.md#update-an-output)), such as
`airplay2_buffered_24` or one of the surround modes, always uses that mode
regardless of this setting. A change through the API takes effect on the next
output start.

### buffered_encoder_budget

Expert capacity-tuning setting for buffered AirPlay 2 outputs. Each active
buffered output (AAC-LC, ALAC 48k/24, or one of the 5.1 surround modes) costs
CPU on a dedicated encode thread. A new buffered output is refused rather than
started if it would push the total cost over budget, so that outputs that are
already playing are never starved.

`0` (auto) scores the host's core count and clock speed at startup and derives
a budget from that. Set a positive value to override the computed budget, for
example if the detected value is too conservative or too generous for your
hardware, or to cap the number of concurrent buffered outputs. A rejected
activation returns HTTP 503 with `{"error": "encoder_capacity"}` (see
[json-api.md](json-api.md)). A change through the API takes effect on the next
output activation.

### tv_proxy_leader_audio_suppress

When an Apple TV is leading a HomePod group, its own session exists only for
RTSP control, the shared anchor, metadata and volume. The HomePods render the
audio. When `true`, that session is never sent an audio frame, which avoids
needless encode and write work and the reconnect churn that comes from data the
Apple TV does not need. When `false`, the Apple TV also receives audio.

This only affects a TV proxy leader started because its followers are already
streaming. A direct start (no follower available) always sends audio to the
Apple TV regardless of this setting. A change through the API takes effect on
the next TV proxy leader start.

### resample_quality

Controls the resampler used for any output whose format does not match the pipe
input (for example an AirPlay 1 receiver at 44.1kHz/16-bit fed from a
48kHz/32-bit pipe). `standard` leaves libavfilter's automatically inserted
resample stage at its default settings. `high` requests the soxr resampler at
VHQ precision with dither, at a modest CPU cost. Outputs whose format already
matches the pipe (for example a 48kHz buffered AirPlay 2 receiver) never
insert a resample stage and are unaffected either way. An unrecognised value is
treated as `standard`. A change through the API takes effect on the next
playback session.

## Access control

Four settings decide who can reach the HTTP JSON API and how.

**`trusted_networks`** is a list of strings. A client whose address matches any
entry is trusted and needs no password. The entries are checked in order, and
these values are understood:

- `lan`: any address on a subnet of a local network interface (the default).
- `localhost`: the loopback addresses (`127.0.0.1` and `::1`).
- `any`: every client. Only use this on a network you fully control.
- An address prefix such as `192.168.1.` or `10.0.`: any peer whose address
  starts with that text.
- `none`, or an empty string: stops the check at that entry, so entries after
  it are ignored and the client is not trusted.

**`admin_password`**: a client that is not trusted is asked for HTTP basic
authentication with user name `admin` and this password. If no password is
set, requests from untrusted clients are refused with 403 Forbidden.

**`allow_origin`**: sent as the CORS `Access-Control-Allow-Origin` header on
API responses, so that a web page served from another origin can call the API.
The default `*` allows any origin. Set a specific origin such as
`"http://192.168.1.20:8080"` to restrict it, or an empty string to omit the
header.

**`port`** and **`bind_address`**: the API listens on TCP port `port` (default
3689). With no `bind_address` it listens on all IPv4 addresses, and on all IPv6
addresses too if `ipv6` is `true`. Set `bind_address` to a single IP address to
listen only there. The same setting restricts the address used for AirPlay
sockets and PTP, and limits mDNS discovery to the interface that owns the
address.

## Per-device settings

Settings for individual speakers go under `airplay_devices`, in an object
keyed by the device name as the speaker announces it over mDNS (for example
`"Living Room"`). A device with no entry uses the defaults below. Values must
have the right JSON type to be read, otherwise the default applies.

```json
"airplay_devices": {
  "Jared's Room": { "max_volume": 3 },
  "Office": { "exclude": true }
}
```

| Key | Type | Default | Valid values | Description |
|-----|------|---------|--------------|-------------|
| `max_volume` | integer | `11` | 1 to 11 | Highest volume this speaker is allowed to reach. 100% on the daemon's volume scale maps to this level of the speaker's own range (11 is the full range). A value outside 1 to 11 is logged and replaced by 11. |
| `exclude` | boolean | `false` | `true`, `false` | Ignore this speaker completely. It will not appear as an output. |
| `permanent` | boolean | `false` | `true`, `false` | Keep the speaker listed after it disappears from the network, instead of removing it. |
| `exclusive` | boolean | `false` | `true`, `false` | Read by the discovery code, but exclusive mode is never switched on in this build, so the setting currently has no effect. |
| `airplay2_disable` | boolean | `false` | `true`, `false` | Do not use AirPlay 2 for this speaker. It can still be used over AirPlay 1. |
| `raop_disable` | boolean | `false` | `true`, `false` | Do not use AirPlay 1 (RAOP) for this speaker. It can still be used over AirPlay 2. |
| `ptp_disable` | boolean | `false` | `true`, `false` | Do not use PTP clock sync with this AirPlay 2 speaker, so it falls back to NTP timing. |
| `nowplaying_disable` | boolean | `false` | `true`, `false` | Do not request the now-playing metadata channel from this Apple TV class receiver. |
| `nickname` | string | Not set | Any string | Name to show for the speaker instead of the one it announces. The key in `airplay_devices` must still be the announced name. |
| `password` | string | Not set | Any string | Password for a password-protected speaker. |
| `reconnect` | boolean | Not set (automatic) | `true`, `false` | Try to reconnect automatically if the speaker drops the connection. Left unset, reconnect is on for Apple TV and HomePod devices and off for others. `null` counts as unset. |
| `auth_key` | string | Not set | Written by the daemon | Pairing key for a speaker that uses AirPlay 2 pairing, saved so that it survives a restart. Do not edit by hand. Deleting it makes the speaker pair again. |
| `buffered_modes` | integer | `0` | Written by the daemon | Bit mask of the buffered AirPlay 2 formats the speaker has been seen to support, saved so that it survives a restart. Do not edit by hand. |

The daemon creates the `airplay_devices` entry for a speaker, and the
`auth_key` and `buffered_modes` keys in it, when it learns them.
