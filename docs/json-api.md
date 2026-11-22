---
hide:
  - navigation
---

# API Endpoint Reference

This pared-back build exposes a small HTTP API for:

- starting and stopping FIFO-backed playback
- discovering and controlling AirPlay and AirPlay 2 outputs
- reading and updating a small set of runtime settings
- retrieving basic server status

All endpoints are rooted at `/api`. Requests and responses use JSON.

## Access control

Access to the API is decided by the address the request comes from.

- The `trusted_networks` setting in the configuration file lists the networks
  that are trusted. It is a list whose entries can be `lan` (the default, any
  local network address), `localhost`, `any`, `none`, or an address prefix
  such as `192.168.1`. Requests from a trusted address need no credentials.
- Requests from any other address get `403 Forbidden`, unless
  `admin_password` is set in the configuration file. In that case the server
  asks for HTTP basic authentication with the user name `admin` and that
  password, and answers `401 Unauthorized` until valid credentials are sent.
- Responses carry an `Access-Control-Allow-Origin` header set from the
  `allow_origin` configuration value, which defaults to `*`. An empty value
  turns the header off. CORS preflight (`OPTIONS`) requests are answered when
  it is set.
- The server listens on the port given by `port` in the configuration file,
  `3689` by default.

## Errors

A request to a path or method that is not listed here gets `400 Bad Request`.
Errors are returned as an HTML page, except where an endpoint below says its
error body is JSON, in the form `{"error": "message"}`. The status codes each
endpoint can return are listed in its section.

| Code | Meaning |
| ---- | ------- |
| 400 | Invalid or missing request body, bad value, unknown output id, or unknown path |
| 401 | Credentials needed (see above) |
| 403 | Address not trusted and no `admin_password` set |
| 404 | Unknown setting |
| 409 | An output's `include_tv` option cannot be changed while it is selected (`{"error": "reselect_required"}`), `include_tv: false` was sent for an Apple TV that does not lead a group of HomePods at the moment (`{"error": "group_not_present"}`), or a PIN was sent while the output has a session or an operation in flight (`{"error": "busy"}`) |
| 500 | The server could not carry out the request |
| 503 | Not sent by the output endpoints: a buffered output refused for want of encoder capacity is reported as its session state |

## Player

| Method | Endpoint | Description |
| ------ | -------- | ----------- |
| PUT | `/api/player/play` | Start or resume playback |
| PUT | `/api/player/stop` | Stop playback |

### Start playback

**Endpoint**

```http
PUT /api/player/play
```

**Query parameters**

| Parameter | Type | Value |
| --------- | ---- | ----- |
| `item_id` | integer | Optional queue item id to start |
| `position` | integer | Optional queue position to start |

If no query parameter is provided, playback starts or resumes the current source.

**Response**

Returns HTTP `204 No Content` on success.

Returns HTTP `400 Bad Request` if `item_id` or `position` is not a number or
does not match a queue item, and HTTP `500 Internal Server Error` if playback
could not be started.

**Example**

```shell
curl -X PUT "http://localhost:3689/api/player/play"
```

### Stop playback

**Endpoint**

```http
PUT /api/player/stop
```

**Response**

Returns HTTP `204 No Content` on success, or HTTP `500 Internal Server Error`
if playback could not be stopped.

**Example**

```shell
curl -X PUT "http://localhost:3689/api/player/stop"
```

## Metadata

Push track metadata (title, artist, album, artwork) to all active AirPlay outputs.
This is useful when the audio source does not embed metadata (e.g. raw PCM on a pipe).

| Method | Endpoint | Description |
|--------|----------|-------------|
| PUT | `/api/metadata` | Push track metadata to all active outputs |

### Example: push text metadata

```
PUT /api/metadata
```

**Body parameters**

| Parameter | Type | Description |
|-----------|------|-------------|
| title | string | Track title |
| artist | string | Artist name |
| album | string | Album name |
| artwork_url | string | Local artwork path as `file:/absolute/path.jpg` or `.png` |

All fields are optional; omitted fields are not changed.

This endpoint reads the file straight from disk, unlike the pipe metadata
path (which carries artwork as bytes and keeps them in memory for the
session). If used, the file should already respect the same size contract as
the pipe path, at most about 48 KB, since that is the size an AirPlay
receiver is known to render reliably. This endpoint does not resize or
re-encode what it is given.

**Response**

Returns HTTP `204 No Content` on success, or HTTP `400 Bad Request` if the body
is missing or is not valid JSON.

**Example**

```shell
curl -X PUT "http://localhost:3689/api/metadata" \
     -H "Content-Type: application/json" \
     -d '{"title":"My Track","artist":"My Artist","album":"My Album","artwork_url":"file:/var/music/cover.jpg"}'
```

## Outputs

| Method | Endpoint | Description |
| ------ | -------- | ----------- |
| GET | `/api/outputs` | List all discovered outputs |
| GET | `/api/outputs/{id}` | Get a single output |
| PUT | `/api/outputs/{id}` | Update a single output |
| PUT | `/api/outputs/set` | Replace the selected output set |

### Output object

| Key | Type | Value |
| --- | ---- | ----- |
| `id` | string | Output id |
| `name` | string | Display name |
| `type` | string | Output type |
| `selected` | boolean | `true` if the output is enabled |
| `has_password` | boolean | `true` if the output advertises password protection |
| `requires_auth` | boolean | `true` if the output currently requires authorization |
| `needs_auth_key` | boolean | `true` if the server does not have a valid stored auth key |
| `pin_pending` | boolean | `true` if the last attempt to enable the output stopped because the device wants a PIN or password. Submit it with `pin` in [Update an output](#update-an-output). Cleared when a later activation attempt gets past that step, when the output is enabled or disabled again, and 2 minutes after the PIN was asked for if none was given. For a group (a stereo pair, an Apple TV with its HomePods) it is true while any speaker of the group is waiting for one |
| `members_missing` | integer | How many speakers of the output's group are not playing or about to while another speaker of the group is; `0` otherwise, and always `0` for an output on its own. A client that wants the whole group back enables the output again: that starts only the missing speakers |
| `volume` | integer | Output volume in percent. While the output is muted, or held by sending it 0, it is the level the speaker returns to |
| `muted` | boolean | `true` while the output is muted with `muted` (see [Update an output](#update-an-output)): it is sent 0 and keeps `volume` |
| `offset_ms` | integer | Timing offset in milliseconds |
| `format` | string | Active audio format |
| `supported_formats` | array | Supported audio format strings |
| `mode` | string | Active protocol mode preference. One of `"auto"` (the default), `"raop"`, `"airplay2"`, `"airplay2_buffered"`, `"airplay2_buffered_24"`, `"airplay2_surround_stereo"`, or `"airplay2_surround_upmix"` |
| `supported_modes` | array | Concrete protocol modes available for this output; a subset of `["raop", "airplay2", "airplay2_buffered", "airplay2_buffered_24", "airplay2_surround_stereo", "airplay2_surround_upmix"]`. Reflects capability learned from the device itself, so it can grow after the output is first discovered. Never includes `"auto"`, which is always accepted. Empty if no protocol is known yet |
| `session` | object | What the output is playing on, see [Session](#session) |
| `has_homepods` | boolean | `true` only for an Apple TV that currently leads a group of HomePods which are present, which is exactly when it can be enabled with `include_tv: false` (see [Update an output](#update-an-output)). `false` for every other output |
| `volume_follow` | string | Whether the level the speaker sets itself (its knob, buttons or app) is followed through its UPnP renderer: `"upnp"` (followed), `"searching"` (the first search for a renderer is under way), `"none"` (no renderer found, the speaker is left out, or following is off) or `"unknown_scale"` (a renderer was found but its volume scale does not fit, so its reports are not taken). See [upnp_volume_follow](configuration.md#upnp_volume_follow) |

#### Session

The state of what the row plays. For an output on its own that is its own connection. For a group it is every speaker of the group, as one: a stereo pair's two speakers, or an Apple TV and the HomePods it leads (an Apple TV enabled with `include_tv: false` has no session of its own and is left out). `selected` and `pin_pending` are likewise the group's: the row is selected while any of its speakers is.

| Key | Type | Value |
| --- | ---- | ----- |
| `state` | string | `"stopped"` (no connection), `"connecting"` (a start, a probe or a PIN pairing is under way), `"connected"` (connected, audio not flowing yet; a buffered AirPlay 2 speaker stays here until it has accepted its start time), `"streaming"`, `"stopping"` (a stop is waiting for the receiver to end its session), `"failed"`, or `"password"` (the device wants a PIN or password). `"startup"` is no longer sent |
| `failure` | string | Why the state is `"failed"` or `"password"`, or `"cancelled"` on a `"stopped"` row whose start was ended by a disable; `"none"` otherwise. `"pin_required"` (the PIN start was sent, so the PIN is on the receiver's screen), `"pin_invalid"` (pair setup with the PIN given was refused), `"refused"` (the receiver answered no to something other than authentication, including a PIN start it refused outright), `"unreachable"` (no connection, or no answer before the receiver had answered anything), `"timeout"` (no answer after it had answered, or pair setup took too long), `"no_clock"` (buffered playback could not be started because the receiver never used our clock), `"capacity"` (the buffered-encode budget was used up), `"cancelled"` (a start ended because the output was disabled while it connected), `"lost"` (an established session failed with nothing in flight; the output is not deselected, and is retried if the speaker is one that reconnects). A PIN given with `pin` whose pair setup times out is `"password"` with `"timeout"`: the receiver never judged the PIN, the start is not repeated, and the next start shows a new PIN. Clients treat any other value as `"refused"` on a `"failed"` row and `"pin_required"` on a `"password"` row |
| `held` | boolean | `true` while the output is held with `hold` (see [Update an output](#update-an-output)): it paused itself and is paused in place (a buffered AirPlay 2 session) or sent 0 (any other). For a group it is `true` when every speaker of the group that has a session is held. Cleared when the hold ends or the session ends |
| `holdable` | boolean | `true` if `hold` would be accepted now: the output, or a speaker of its group that plays audio, has a connected or streaming AirPlay 2 or AirPlay 1 session that is not being stopped |

An output that is selected is not necessarily streaming: `selected` says what
was asked for, `session.state` what the device is doing. When playback starts
from the pipe, the player plays at once, with or without speakers, and each
selected speaker steps through `"connecting"`, `"connected"` and `"streaming"`, or
ends at `"failed"`, as it joins. `GET /api/player` reports `play` about the
start buffer (2.3 seconds) after the pipe opens, not when a speaker has
started. For a group, `session.state` is `"password"` if any speaker waits for
a PIN (the user has to act, so it shows even while another speaker plays),
otherwise `"streaming"` if any speaker streams, otherwise `"connecting"` if any
is connecting, connected or about to be retried, otherwise `"stopping"`,
`"failed"` or `"stopped"` in that order of precedence.

While the player plays, a speaker whose start fails or that is lost is
retried 2 seconds later, and if that fails too, 3 seconds after that, if it
is one that reconnects (the `reconnect` option, on unless set `false`). It
stays selected meanwhile and its row reads `"connecting"`. A failure for
`"pin_required"`, `"pin_invalid"`, `"cancelled"` or `"capacity"` is not
retried. When the retries are spent a failed start is deselected and a lost
session stays failed, and further recovery is the client's.

A pair is listed as one output; its second speaker is hidden. A group of
HomePods that sits behind an Apple TV is listed as the Apple TV; the HomePods
are hidden. Enabling, disabling and
setting the volume of the listed output applies to every output in its group.

A row used to carry `paused_by_device`. It has been removed: a pause from a
device is now only [reported](event-channel.md), and does not change the
output. A supervisor that wants the speaker to stay paused holds it with
`hold`.

The AirPlay 2 modes beyond plain `"airplay2"` (realtime) select a buffered
transport, where the receiver queues audio ahead of playout instead of
receiving it as it plays:

| Mode | Description |
| ---- | ----------- |
| `airplay2_buffered` | Buffered playback with 16-bit AAC-LC audio. Offered as a baseline for any AirPlay 2-capable output as soon as the output is discovered, since it is supported almost universally; the session falls back to realtime if the device turns out not to accept it |
| `airplay2_buffered_24` | Buffered playback with 24-bit ALAC (lossless). Only listed once learned from the device; only worth selecting when the source is genuinely higher than 16-bit |
| `airplay2_surround_stereo` | Buffered 5.1 output that maps a stereo source onto the front-left/front-right/LFE channels of a 5.1 stream, leaving the remaining channels silent. Only listed for a standalone Apple TV |
| `airplay2_surround_upmix` | Buffered 5.1 output that decodes and upmixes a stereo source across all six channels. Only listed for a standalone Apple TV |

`airplay2_surround_stereo` and `airplay2_surround_upmix` never appear in
`supported_modes` for a HomePod, a HomePod stereo pair, or a HomePod group
routed through an Apple TV; surround is offered only for a standalone Apple
TV. If either mode is set on an output that is not eligible, the output plays
over the realtime path instead and no error is returned.

Setting a mode that is valid but not in the output's `supported_modes` is
accepted with `204` but ignored, and `mode` stays as it was. A name that is not
in the list above gives `400`.

### List outputs

**Endpoint**

```http
GET /api/outputs
```

**Response**

| Key | Type | Value |
| --- | ---- | ----- |
| `outputs` | array | Array of [output](#output-object) objects |

**Example**

```shell
curl -X GET "http://localhost:3689/api/outputs"
```

```json
{
  "outputs": [
    {
      "id": "1",
      "name": "Speaker A",
      "type": "AirPlay 2",
      "selected": true,
      "has_password": false,
      "requires_auth": false,
      "needs_auth_key": false,
      "volume": 50,
      "offset_ms": 0,
      "format": "alac",
      "supported_formats": ["pcm", "alac"],
      "mode": "auto",
      "supported_modes": ["raop", "airplay2"],
      "session": {"state": "streaming", "failure": "none"},
      "has_homepods": false
    }
  ]
}
```

An Apple TV that leads two HomePods, enabled without the Apple TV, looks like
this once the HomePods are playing (other keys left out); the session is the
HomePods', not the Apple TV's:

```json
{
  "id": "186916976721924",
  "name": "TV A",
  "selected": true,
  "session": {"state": "streaming", "failure": "none"},
  "has_homepods": true
}
```

### Get an output

**Endpoint**

```http
GET /api/outputs/{id}
```

**Response**

Returns a single [output](#output-object) object, or HTTP `400 Bad Request` if
the id is not a number or no output has that id.

### Update an output

**Endpoint**

```http
PUT /api/outputs/{id}
Content-Type: application/json
```

**Request body**

All fields are optional. Any combination may be supplied in one request.

| Key | Type | Value |
| --- | ---- | ----- |
| `selected` | boolean | Enable or disable the output |
| `include_tv` | boolean | With `selected: true` on an Apple TV that leads a group of HomePods: `true` plays on the Apple TV and the HomePods (the default behaviour), `false` plays on the HomePods only and gives the Apple TV no session. Ignored for any other output, except that `false` is refused with `409` (see below) unless the output's `has_homepods` is `true`. When absent the option is left as it was |
| `volume` | integer | Set output volume, from `0` to `100`. Sent with `selected: true` it is also the level the speakers start at: it is set on the output, and on every other member of its group (a stereo pair, or the HomePods behind an Apple TV), before they are started, and is not limited by the volume of speakers already playing. `0` starts them silent. Without `selected: true` it sets the level of the output |
| `pin` | string | Submit a PIN for authorization, at most 15 characters. With `selected: true` in the same request the PIN and the start it unlocks are one request (see below) |
| `format` | string | Set the output audio format |
| `offset_ms` | integer | Set timing offset in milliseconds, from `-2000` to `2000` |
| `mode` | string | Set the protocol mode preference: `"auto"`, `"raop"`, `"airplay2"`, `"airplay2_buffered"`, `"airplay2_buffered_24"`, `"airplay2_surround_stereo"`, or `"airplay2_surround_upmix"` |
| `hold` | boolean | `true` holds a speaker that paused itself, `false` ends the hold. See below |
| `muted` | boolean | `true` mutes the output, `false` ends the mute. See below |

Fields are applied in this order: `offset_ms`, `mode` (unless the request has
`selected: false`), `muted`, `pin` together with `selected: true`, `selected`
(with `include_tv` and the start `volume`), `mode` (when the request has
`selected: false`), `volume`, `hold`, `pin` alone, `format`. If one fails, the
ones before it have already taken effect.

`pin` with `selected: true` is one request: the output is paired with the PIN
and, once paired, started with its group as an enable with the same
`include_tv` and `volume` would start it, `volume` being the level it starts
at; `selected`, `volume` and `pin` are not applied again. If the receiver
refuses the PIN the start is made anyway, which puts a new PIN on its screen.
A pairing that times out starts nothing, since the PIN on screen may be the one
being typed; the next enable shows a new PIN. Disabling the output during the
pairing takes the start away. A PIN that arrives while the output has a
session, or while a start, a stop or a pairing is in flight, is not used: the
request gets HTTP `409 Conflict` with body `{"error": "busy"}`, nothing is
started or paired, and the client can send the PIN again once the output
reads `"password"`.

A buffered AirPlay 2 receiver (an Apple TV, a HomePod) pauses itself when its
own pause is pressed, and its own play does not restart it. It keeps reading
audio until its buffer is full and then stops, and the session then fails.
`hold: true` pauses it in place with its buffer kept, so the members of a
stereo pair stop together, and stops sending it audio. The session stays up,
and the receiver is told that playback is paused. `hold: false` restarts it at the live
position: its buffer is flushed and it is given a new start time. A real-time
AirPlay 2 or an AirPlay 1 speaker is held by sending it 0 while its audio keeps
flowing, so it stays in time; `hold: false` sends it its own level again. For a
group (a stereo pair, or an Apple TV and its HomePods) every speaker of the
group with a session is held or restarted together. Holding a speaker that is
held, or ending the hold of one that is not, is accepted and changes nothing. A
request for an output that cannot be held (no session, a session that is still
starting or is being stopped, or a speaker of another kind) gets HTTP `409
Conflict` with body `{"error": "not_holdable"}`. The hold ends with the
session; a speaker of a held group whose session is set up again ends the hold
of the others.

`muted: true` sends the output 0 and keeps its level in `volume`; `muted:
false` sends that level again. The mute applies to every speaker of the
output's group and needs no session: a muted output that is started later
starts silent. While an output is muted or held by sending it 0, a level set on
it moves the level it returns to and nothing is sent, and the speaker's own
volume control is not recorded: a turn of it is reported once on the [event
channel](event-channel.md) as the receiver command `wake`, and nothing else
changes. Disabling the output ends its mute. `muted` on an output that is off
is kept for its next start and cleared when it is next switched off, so a later
`selected: true` without `muted` starts it silent; a supervisor sends `muted`
with every enable.

**Response**

Returns HTTP `204 No Content` when every field was accepted. It means the work
is in flight or done, not that it succeeded: no speaker write waits for the
speaker. The request answers within one turn of the player (in practice well
under 200 ms), and what then becomes of each speaker is its `session.state` and
`session.failure`, sent as an `output_state` event on the [event
channel](event-channel.md) and read with `GET /api/outputs`. A failure event can
arrive before the `204`. An enable of an output that is already connecting or
connected is accepted and changes nothing. `GET /api/config` lists
`async_outputs` in `api_features` for this behaviour.

An enable starts the output: `"connecting"`, then `"connected"` and
`"streaming"`, or `"failed"` with a reason and the output deselected, or
`"password"` with `pin_pending` (the PIN is on the receiver's screen). With no
playback it is only probed, and reads `"stopped"` and selected once the probe
is done. The start makes one attempt: the connection and each request up to
setting up the stream give up after 4 seconds, and a request after that after
15, so a start that is accepted ends within about 37 seconds at worst.

A disable of a connected or streaming output reads `"stopping"` at once and
`"stopped"` when the receiver has ended its session: an AirPlay 2 output is
sent `TEARDOWN` and stops once the receiver has replied, or after 1.5 seconds
without a reply; an AirPlay 1 output is given about 2 seconds when nothing else
was in flight on it, otherwise the timeout of the request already sent. A
disable while the output is connecting reads `"stopping"` at once; the start
runs on and the output is stopped once it has a session, or reads `"stopped"`
with `"cancelled"` when the start left none (a start still waiting to pair, or
between two attempts, is ended there). An enable while the output is stopping
starts it once the stop has ended. A mode change that moves the output to the
other protocol while it plays is a stop and a start on the new protocol.

The playback commands that flush the speakers (stop, pause and the pipe's
flush) still wait for the speakers they flushed, at most 4 seconds.

Returns HTTP `400 Bad Request` if the id is not a number, the body is missing
or not valid JSON, no output has that id, `volume` or `offset_ms` is out of
range (an enable with `volume` out of range starts nothing), the output could
not be given the format, `pin` is longer than 15 characters, or `mode` is not
one of the names listed above. The bodies `{"error": "pin_required"}` and
`{"error": "start_failed"}` are no longer sent: a PIN asked for and a failed
start are the output's state.

The `include_tv` option is fixed for as long as the output is selected. A
request that would change it while the output is selected gets HTTP `409
Conflict` with body `{"error": "reselect_required"}` and changes nothing:
disable the output, then enable it with the other option. Repeating the option
it already has is accepted. When the group is enabled without the Apple TV,
the HomePods are started, kept in sync, given their volume and metadata, and
restarted if one drops out exactly as a stereo pair is; the Apple TV shows
nothing. The row's state is that of the HomePods. Disabling the output stops
every speaker in the group.
If `include_tv: false` is sent for an Apple TV that does not lead a group of
HomePods at the moment, because its HomePods have not been discovered yet or
have been away for longer than the grace period, the request gets HTTP `409
Conflict` with body `{"error": "group_not_present"}` and nothing is selected or
started; the Apple TV is not given a session on its own. A client should wait
for the group to appear (the output list changes, and the row's `has_homepods`
becomes `true`) and send the request again. Once a group has been
enabled without its Apple TV, the Apple TV is not started by a later "play on
the selected outputs" request even if its HomePods go away, until the output
is enabled again.

The option is not changed by disabling, so a client that enables the output
should send `include_tv` every time. Rows do not
report the option. `PUT /api/outputs/set` does not take the
option and uses whatever was last set.

Enabling (`"selected":true`) a buffered AirPlay 2 output (`airplay2_buffered`,
`airplay2_buffered_24`, or one of the surround modes) ends `"failed"` with
`"capacity"` if admitting it would exceed the buffered-encode capacity budget
(see `buffered_encoder_budget` in [configuration.md](configuration.md)); the
request itself answers `204`, and HTTP `503` with `{"error":
"encoder_capacity"}` is no longer sent. Other outputs are unaffected; retry
once capacity frees up (another buffered output stops).

**Example**

```shell
curl -X PUT "http://localhost:3689/api/outputs/1" \
  -H "Content-Type: application/json" \
  -d "{\"selected\":true,\"volume\":60}"
```

### Replace selected outputs

**Endpoint**

```http
PUT /api/outputs/set
Content-Type: application/json
```

**Request body**

| Key | Type | Value |
| --- | ---- | ----- |
| `outputs` | array | Array of output id strings to select |

**Response**

Returns HTTP `204 No Content` within one turn of the player, without waiting
for any speaker (each reports its outcome as its state, as for [Update an
output](#update-an-output)), or HTTP `400 Bad Request` if the body
is missing, has no `outputs` array, or contains an id that is not a number.

**Example**

```shell
curl -X PUT "http://localhost:3689/api/outputs/set" \
  -H "Content-Type: application/json" \
  -d "{\"outputs\":[\"1\",\"2\"]}"
```

## Server Info

| Method | Endpoint | Description |
| ------ | -------- | ----------- |
| GET | `/api/config` | Get configuration information |
| GET | `/api/library` | Get basic health/status information |
| PUT | `/api/update` | Reload configuration, apply any changed pipe settings live, and refresh the in-memory pipe item |

### Config

**Endpoint**

```http
GET /api/config
```

**Response**

| Key | Type | Value |
| --- | ---- | ----- |
| `product_name` | string | Product/package name compiled into the server |
| `version` | string | Server version |
| `websocket_port` | integer | Always `0` in this build |
| `buildoptions` | array | Array of server feature strings |
| `restart_required` | boolean | `true` if a restart-required config change is pending |
| `live_offset` | boolean | Always `true` in this build. It means an output's `offset_ms` can be changed during playback |
| `event_channel` | integer | Version of the [event channel](event-channel.md) protocol, or `0` when the channel is off |
| `event_socket` | string or null | Path of the event channel's socket, `null` when the channel is off |
| `api_features` | array | Names of what this build's API does beyond the base. `"mute"`: [Update an output](#update-an-output) takes `muted` and each output row carries it. `"async_outputs"`: speaker writes answer within one turn of the player without waiting for the speaker, and outcomes follow as the output's state. `"output_state"`: the [event channel](event-channel.md) sends every output transition as an `output_state` event. A client treats a missing list as empty; the order is not significant |

**Example**

```shell
curl -X GET "http://localhost:3689/api/config"
```

```json
{
  "product_name": "owntone-mini",
  "websocket_port": 0,
  "version": "x.y.z",
  "buildoptions": [
    "ffmpeg",
    "Without Spotify",
    "Without LastFM",
    "Without Chromecast",
    "Without MPD",
    "Without websockets",
    "Without ALSA",
    "Without webinterface",
    "Regex"
  ],
  "restart_required": false,
  "live_offset": true,
  "event_channel": 1,
  "event_socket": "/run/owntone-mini/events.sock",
  "api_features": ["mute", "async_outputs", "output_state"]
}
```

### Library

This endpoint is retained as a lightweight health/status response.

**Endpoint**

```http
GET /api/library
```

**Response**

| Key | Type | Value |
| --- | ---- | ----- |
| `updating` | boolean | Always `false` in this build |
| `songs` | integer | Always `1` in this build |
| `Healthy` | boolean | Always `true` in this build |
| `restart_required` | boolean | `true` if a restart-required config change is pending |

**Example**

```json
{
  "updating": false,
  "songs": 1,
  "Healthy": true,
  "restart_required": false
}
```

### Update

Reloads configuration, applies any changed pipe settings live, and refreshes the in-memory synthetic pipe queue item.

If `misc/pipe_path` or `misc/pipe_autostart` changed on disk, the running pipe input is reconfigured immediately.
If there is no relevant change, this endpoint is effectively a no-op for the pipe runtime.

**Endpoint**

```http
PUT /api/update
```

**Response**

Returns HTTP `204 No Content` on success.

Returns HTTP `500 Internal Server Error` with a JSON error object if the
configuration cannot be reloaded or the updated pipe configuration cannot be
applied. In the second case the previous pipe path stays in use.

## Settings

| Method | Endpoint | Description |
| ------ | -------- | ----------- |
| GET | `/api/settings/{category}/{option}` | Read a setting |
| PUT | `/api/settings/{category}/{option}` | Change a setting |

There are no category-listing or option-listing endpoints in this build.

Category and option names are matched without regard to case, so
`/api/settings/Misc/LogLevel` reaches `misc/loglevel`. GET returns the name in
its canonical lower case form. Only letters, digits and underscores are allowed
in the path. An unknown setting gives HTTP `404 Not Found`.

### Option object

| Key | Type | Value |
| --- | ---- | ----- |
| `name` | string | Option name |
| `type` | integer | `0` = integer, `1` = boolean, `2` = string |
| `value` | varies | Current value in use. For `misc/pipe_path` this is the live path currently used by the running server, which may differ from the persisted on-disk value until `PUT /api/update` applies the change. |

### Supported settings

| Endpoint | Type | Notes |
| -------- | ---- | ----- |
| `/api/settings/misc/loglevel` | integer | Log level, applied to the running server at once. The running level is clamped to `0` to `5`; the number stored in the settings is the one sent |
| `/api/settings/misc/pipe_path` | string | Pipe/FIFO path. The provided path must already exist, be a FIFO, and be readable by the server. A successful PUT persists the value immediately; `PUT /api/update` makes it live. GET returns the live path currently in use. |
| `/api/settings/misc/pipe_autostart` | boolean | Whether the pipe input autostarts. Changes become live on `PUT /api/update`. |
| `/api/settings/misc/ipv6` | boolean | IPv6 enable/disable, restart required |
| `/api/settings/misc/user_agent` | string | User-Agent header sent on AirPlay RTSP requests, max 255 bytes, no control characters, restart required. Empty string resets to the built-in default (`<product>/<version>`). GET always returns the effective value, including the default when unset. |
| `/api/settings/player/start_buffer_ms` | integer | Start buffer in milliseconds, valid range `300` to `3500`, restart required |
| `/api/settings/player/uncompressed_alac` | boolean | ALAC output preference, restart required |
| `/api/settings/player/device_removal_grace_period` | integer | Seconds a disappeared AirPlay device is kept selectable, valid range `0` to `3600`; applied to the running player immediately on PUT, no restart required |
| `/api/settings/player/buffered_audio_enabled` | boolean | Prefer buffered AAC-LC over realtime for AirPlay 2 outputs left on `auto` (default `true`, gated per device on an advertised buffered format); when `false` they play realtime AirPlay 2. Takes effect on the next output start |
| `/api/settings/player/buffered_encoder_budget` | integer | Buffered-encode capacity budget override, valid range `0` to `64` (`0` = auto, CPU-based); takes effect on the next output activation |
| `/api/settings/player/tv_proxy_leader_audio_suppress` | boolean | Suppress audio to a TV proxy leader (e.g. Apple TV) whose HomePod followers are already streaming, keeping control/anchor/metadata/volume only (default `true`); takes effect on the next TV proxy leader start |
| `/api/settings/player/pipe_sample_rate` | integer | Pipe input sample rate, one of `44100`, `48000`, `88200`, `96000`; restart required |
| `/api/settings/player/pipe_bits_per_sample` | integer | Pipe input bit depth, `16` or `32`; restart required |
| `/api/settings/player/resample_quality` | string | Output resampler quality for format-mismatched paths, `high` or `standard` (default `standard`); unknown values fall back to `standard`; takes effect on the next playback session, no restart required |
| `/api/settings/player/device_volume_echo` | string | What to do with a level a speaker reports itself: `off` (record only), `reporter` (default, also send it back to that speaker) or `group` (send it to the speaker and its group); other values are refused; applied immediately, no restart required |
| `/api/settings/player/airplay_setpeers` | string | How AirPlay 2 receivers that use PTP are told about our clock: `auto` (default, `SETPEERSX` to a receiver that advertises support for it in its AirPlay features, `SETPEERS` to the others), `all` (`SETPEERSX` to every such receiver) or `off` (always `SETPEERS`); a receiver that refuses `SETPEERSX` is sent `SETPEERS` instead; other values are refused; takes effect on the next speaker start, no restart required |
| `/api/settings/player/upnp_volume_follow` | boolean | Follow the level a speaker sets itself through its UPnP renderer (default `true`); applied immediately, no restart required |
| `/api/settings/player/upnp_event_port` | integer | Port on which UPnP renderers send volume events, valid range `1024` to `65535` (default `3690`); restart required |

### Get a setting

**Endpoint**

```http
GET /api/settings/{category}/{option}
```

**Example**

```shell
curl -X GET "http://localhost:3689/api/settings/player/start_buffer_ms"
```

```json
{
  "name": "start_buffer_ms",
  "type": 0,
  "value": 2250
}
```

For `misc/pipe_path`, the response reflects the current live pipe path in use by the running process. If a new value has been written but `PUT /api/update` has not yet been called, GET continues to return the old live value.

### Change a setting

**Endpoint**

```http
PUT /api/settings/{category}/{option}
Content-Type: application/json
```

**Request body**

```json
{
  "value": 1500
}
```

**Response**

On success returns HTTP `200 OK` with:

```json
{
  "restart_required": true
}
```

`restart_required` reflects whether the server currently has any pending restart-required configuration changes.

For settings that can be applied live, such as `misc/pipe_path`, this field may still be `false` if there are no other pending restart-required changes.

For `misc/pipe_path`, the server validates the provided path before persisting it. The path must exist, be a FIFO, and be accessible to the server process.

`PUT /api/settings/misc/pipe_path` does not switch the running pipe immediately. The new value is persisted, and `PUT /api/update` applies it to the live runtime.

Status codes other than `200`:

| Code | When |
| ---- | ---- |
| 400 | The body is missing or not valid JSON, `value` is missing or not of the setting's type, or the value is rejected |
| 404 | No such setting |
| 500 | The setting could not be saved |

If `pipe_path` or `user_agent` validation fails, the 400 response has a JSON body like:

```json
{
  "error": "Invalid pipe_path: '/tmp/example.fifo' does not exist, is not a FIFO, or is not accessible"
}
```

**Examples**

```shell
curl -X PUT "http://localhost:3689/api/settings/misc/ipv6" \
  -H "Content-Type: application/json" \
  -d "{\"value\":false}"
```

```shell
curl -X PUT "http://localhost:3689/api/settings/player/start_buffer_ms" \
  -H "Content-Type: application/json" \
  -d "{\"value\":1500}"
```

```shell
curl -X PUT "http://localhost:3689/api/settings/misc/pipe_path" \
  -H "Content-Type: application/json" \
  -d "{\"value\":\"/tmp/owntone.fifo\"}"
```
