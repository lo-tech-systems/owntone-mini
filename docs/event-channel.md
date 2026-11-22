# Event channel

owntone-mini can tell a local supervisor, without being polled, that the list
of outputs has changed and what a receiver has asked for. The supervisor is
whatever program decides which outputs play: owntone-mini applies what it is
told and reports what happens, and does not act on a receiver's own commands.

The channel is a Unix socket on the machine the daemon runs on. It is off the
network, it is optional, and nothing in the daemon waits on it. With no client
connected, events are dropped.

## Turning it on and off

The channel is controlled by two settings (see
[Configuration](configuration.md)):

| Setting | Default | Meaning |
|---------|---------|---------|
| `event_socket` | `"/run/owntone-mini/events.sock"` | Path of the socket. An empty string turns the channel off. |
| `event_socket_allowed_users` | `["autostream"]` | Users that may connect, in addition to root and the user the daemon runs as, which are always allowed. The daemon checks the user id of each connecting process; any other user is sent `not_allowed` and closed. The names are looked up at each connection, so a user created after the daemon started is accepted from then on. |

Both are read at startup, so a change needs a restart. `GET /api/config`
reports `event_channel` (the protocol version, or `0` when the channel is off
or its socket could not be bound) and `event_socket` (the path, or `null`). The
socket is bound before the HTTP server starts, so the two agree from the first
request. A client that picks its event source from `event_channel` should read
it again each time it reconnects.

The daemon runs as an unprivileged user and cannot create the socket's
directory itself; the installed service file creates `/run/owntone-mini` before
the daemon starts. If the socket cannot be created, the daemon logs it and
carries on without the channel.

## Protocol

Messages are single JSON objects, one per line (terminated by `\n`, UTF-8, no
`\r`). Every message has a string `type`. A line from the client may be at most
4096 bytes. Messages from the server also carry `seq`, a number that counts up
from 1 on each connection and is on every message the server writes (`hello`,
`changed`, every `event`, `pong`, `error`). A `changed` that is folded into a
later one is never written and uses no number. A gap in `seq` therefore means
messages were lost, which the server never does on purpose: a client that sees
one reads `GET /api/outputs` again.

A client sends `hello` as its first line within 5 seconds of connecting. The
server allows four connections at once.

### From the client

| `type` | Fields | Notes |
|--------|--------|-------|
| `hello` | `protocol` (integer, `1`), `client` (string, at most 64 bytes) | First line. |
| `ping` | `t` (any value, optional) | Send one about every 10 seconds. |
| `goodbye` | none | Send before a clean close. |

### From the server

| `type` | Fields |
|--------|--------|
| `hello` | `protocol` (`1`), `version`, `boot` (16 hexadecimal digits, new on every start of the daemon), `uptime_ms`, `capabilities`, `ping_interval_ms` (`10000`), `idle_close_ms` (`30000`), `max_line` (`4096`) |
| `changed` | `what`: `"outputs"`. The list of outputs, or something in it, has changed. |
| `event` | `kind`, `at_ms`, `speaker` (`id`, `member_id` and `name`), and fields that depend on `kind` (see below). |
| `pong` | `t`, as the client sent it. |
| `error` | `code`, `text`. The server closes the connection after it. |

`capabilities` lists what this build can report: `changed`,
`receiver_command`, `device_volume`, `include_tv`, `start_volume`, `mute` and
`output_state`. `output_state` means every transition of an output is sent as
an `output_state` event (see below); it also means that speaker writes
(`PUT /api/outputs/{id}` and `PUT /api/outputs/set`) answer without waiting for
the speaker, the outcome following as the event, so a client may take both from
this one name.
`start_volume` means an enable that carries a `volume` starts the speakers at
that level (see [Update an output](json-api.md#update-an-output)). `mute` means
an output can be muted with `muted`, and a muted speaker, or one held by sending
it 0, reports a turn of its own volume control as the receiver command `wake`
(see below).

`changed` carries no detail. The client reads `GET /api/outputs` to find out
what changed. At most one `changed` is sent per 20 ms on a connection, so a
burst of changes arrives as one message and nothing is lost.

`event` has three kinds: the two below, and `output_state` in its own section.
`at_ms` is the daemon's monotonic clock in
milliseconds, on the same scale as `uptime_ms` in the hello, so a client can
put an event in order with the hello.

| `kind` | Extra fields | Meaning |
|--------|--------------|---------|
| `receiver_command` | `verb` (the receiver's own word, unchanged: for example `paus`, `play`, `nitm`, `pitm`; or `wake`), `protocol` (`"airplay2_events"`, `"dacp"` or `"upnp"`) | A speaker or Apple TV sent a command. With `"airplay2_events"` it came on an AirPlay 2 receiver's events connection. With `"dacp"` an AirPlay 1 receiver sent its own pause, play, play/pause or mute toggle as a DACP request, reported as `paus`, `play`, `plps` or `mute`. Verb `wake` means a turn of a silent speaker's own volume control (it is muted, or held by sending it 0) was read: from `dvlc` (`"airplay2_events"`), from DACP `setproperty`, `volumeup` or `volumedown` (`"dacp"`), or from a UPnP renderer's level (`"upnp"`). It is sent once per turn, and nothing is changed because of it. The daemon does not pause, resume or deselect anything because of a command. The receiver still gets its normal reply. |
| `device_volume` | `verb` (`dmcp.device-volume`, `dmcp.volume`, `volumeup`, `volumedown`, `upnp.volume` or `dvlc`), `value` (the raw value for `dmcp.device-volume`, the level in dB on the same scale for `dvlc`, a percentage as a decimal string for the others) | A receiver set its volume through the control interface; for `upnp.volume`, a speaker's UPnP renderer reported a level set on the speaker itself (see [upnp_volume_follow](configuration.md#upnp_volume_follow)); for `dvlc`, an AirPlay 2 speaker reported a turn of its own volume control on its events connection (see `remote_commands` in [the per-speaker settings](configuration.md)). The volume has already been recorded when the event is sent. A `dvlc` level is not sent back to the speaker, which is already playing at it. A silent speaker's own levels are not recorded and not reported as `device_volume`. |

`speaker.id` is the id of the output the event is reported under, as a string,
or `"0"` when a `dmcp.volume` applies to every speaker. For a device the daemon knows it is
the id of a row of `GET /api/outputs`: a HomePod hidden behind an Apple TV is reported
under the Apple TV, and the second speaker of a stereo pair under the visible
speaker of the pair. `speaker.name` is the name of that output, or `null` when
the name is not known.

`speaker.member_id` is the id of the device the event actually came from. It is
the same as `speaker.id` when the device has a row of its own, and differs only
for a hidden member of a group. It is there for logs; a client should act on
`speaker.id`.

### The `output_state` event

One event per transition per device: every phase change of a device, once, and
every change of its selection, its PIN state, its hold or its mute. Member
order as written:

```json
{"type":"event","seq":118,"kind":"output_state",
 "speaker":{"id":"74291837465512","member_id":"74291837465513","name":"Speaker B (Stereo)"},
 "state":"streaming","reason":"none",
 "member_state":"connecting","member_reason":"none",
 "selected":true,"held":false,"holdable":true,"muted":false,"volume":42,"pin_pending":false,
 "members_missing":0,"at_ms":5532104}
```

| Member | Type | Value |
|--------|------|-------|
| `speaker.id` | string | The row id, as `GET /api/outputs` lists it |
| `speaker.member_id` | string | The device that moved; the same as `speaker.id` for a device with its own row |
| `speaker.name` | string or null | The row's name |
| `state` | string | The row's `session.state` at this instant, exactly as `GET /api/outputs` would give it: `stopped`, `connecting`, `connected`, `streaming`, `stopping`, `failed` or `password`. For a group (a stereo pair, an Apple TV with its HomePods) that is the summary of every speaker of the group |
| `reason` | string | The row's `session.failure` at this instant (see [the JSON API](json-api.md#session)) |
| `member_state`, `member_reason` | string | The moved device's own state and reason, for logs |
| `selected`, `held`, `holdable`, `muted`, `volume`, `pin_pending`, `members_missing` | | The row's values at this instant, as `GET /api/outputs` gives them |

A client applies `state` and the row values to the row named by
`speaker.id`, never `member_state`. The values are taken on the player thread
at the moment of the transition, so an event and a read made in the same turn
agree. A level change sends no `output_state` (the `changed` message and
`device_volume` cover it), and a report that changes nothing sends nothing.
Events are not folded together, and those of one device arrive in the order
its transitions happened. The two events of a stereo pair are next to each
other only when one request moved both members (a hold, a mute, the
`stopping` of a disable); each member's own connection steps arrive as its
receiver answers. No event is sent while the daemon stops.

A client that lost events (a gap in `seq`, or a reconnect) reads
`GET /api/outputs` again; `changed` keeps being sent as before.

Sequences a client should expect for one output:

| Situation | Events, in order |
|-----------|------------------|
| Enable while playing, success | `connecting`; `connected`; `streaming` (a buffered AirPlay 2 receiver once it has accepted its start time) |
| Enable while not playing | `connecting` (a probe); `stopped` with `selected` true; later `connecting`, `connected`, `streaming` when playback starts |
| Start fails | `connecting`; `failed` with the reason and `selected` false |
| PIN needed | `connecting`; `password` with `pin_required` and `pin_pending` true (the PIN is on the receiver's screen when this is sent) |
| Disable of a connected or streaming output | `stopping`; `stopped` within 1.5 seconds and one turn (AirPlay 2) or about 2 seconds (AirPlay 1) |
| Disable while connecting | `stopping` at once; then `stopped` with `cancelled` when the start left no session, or, when it succeeded, `stopped` after its `TEARDOWN` |
| Enable while stopping | nothing new; after `stopped`, `connecting` and the rest |
| Wrong PIN with `selected: true` in the same request | `password` with `pin_invalid` and `pin_pending` true (the pairing ended); `connecting` (the start the request asked for, which sends a new PIN start); `password` with `pin_required` and `pin_pending` true (the new PIN is on screen). The third event, arriving while the client already shows the output as waiting for a PIN, is a refresh of the prompt |
| Wrong PIN without `selected` | `password` with `pin_invalid`; the next enable shows a new PIN |
| Right PIN with `selected: true` | `connecting` (the pairing); `stopped` with `pin_pending` false (pairing done); `connecting`; `connected`; `streaming` |
| Pairing with a PIN timed out | `password` with `timeout`, `pin_pending` true and `selected` false; nothing is started, and the next enable shows a new PIN |
| No PIN given within 2 minutes of the request, or the output disabled while it waits | `stopped` with `pin_pending` false. Enabling the output while it waits sends a new PIN start: `connecting` with `pin_pending` false |
| Established session fails | `failed` with `lost` (or the receiver's reason); the output is not deselected; a later `connecting` if the speaker is brought back |
| Hold or resume, mute or unmute | the same `state` with the new `held` or `muted` |
| Mode change to the other protocol while playing | `stopping`; `stopped`; `connecting`; `connected`; `streaming` |

A `failed` event can arrive before the `204` of the request that caused it.
`selected` follows `GET /api/outputs`, which does not show an output as
selected while its row reads `failed`.

`error` codes: `not_allowed`, `too_many_clients`, `hello_timeout`,
`bad_hello`, `protocol_unsupported`, `bad_message`, `line_too_long` and
`shutting_down`.

Example:

```json
{"type":"hello","protocol":1,"client":"supervisor 1.0"}
{"type":"hello","seq":1,"protocol":1,"version":"1.6.0","boot":"3f9a1c2e77b04d15","uptime_ms":1843,"capabilities":["changed","receiver_command","device_volume","include_tv","start_volume","mute","output_state"],"ping_interval_ms":10000,"idle_close_ms":30000,"max_line":4096}
{"type":"changed","seq":2,"what":"outputs"}
{"type":"event","seq":3,"kind":"receiver_command","verb":"paus","protocol":"airplay2_events","speaker":{"id":"9988776655443322","member_id":"9988776655443399","name":"TV A"},"at_ms":190511}
{"type":"event","seq":4,"kind":"device_volume","verb":"volumeup","value":"45","speaker":{"id":"8734567012345678","member_id":"8734567012345678","name":"Speaker A"},"at_ms":191002}
```

### Rules

- A client reads `GET /api/outputs` after `hello`, and again after every
  `changed`. When it reconnects it reads the list again and carries on; events
  that happened while it was away are not replayed.
- The server closes a connection that has sent nothing for 30 seconds, so a
  client pings about every 10 seconds. It also closes one that falls so far
  behind that more than 256 KB is waiting to be sent, and one that sends a
  line longer than 4096 bytes.
- When the daemon stops, a client that has nothing waiting to be sent gets
  `error` with code `shutting_down` and then the connection closes; any other
  client is simply closed.
- Starting playback from the pipe never waits for a speaker. The player is
  playing as soon as the pipe is open, and the selected speakers join it as
  each one connects, so a speaker that is switched off cannot hold up the
  others, the player's state or other requests. Each speaker's row steps
  through `connecting`, `connected` and `streaming`, or `failed`, and each step
  is announced with `changed` and an `output_state` event.
- No speaker write waits for the speaker: `PUT /api/outputs/{id}` and
  `PUT /api/outputs/set` answer within one turn of the player, and other
  requests are not held up by a speaker. A start makes one attempt: the
  connection and each request up to setting up the stream give up after 4
  seconds, and a request after that after 15, so a start ends in a final
  state within about 37 seconds at worst. The one retry is a stereo pair's
  pair-setup rejection just after a stop, tried up to twice more 3 seconds
  apart. Starting a speaker again after a failure is the supervisor's job,
  except that a speaker that drops a session it had is brought back by the
  daemon after a few seconds, as before.
- The playback commands that flush the speakers (stop, pause, the pipe's
  flush) still wait for those speakers, at most 4 seconds.
- `boot` tells a restart of the daemon from a dropped connection. `uptime_ms`
  says how far device discovery is likely to have got.
- Fields may be added to a message without changing the protocol version.
  Both sides ignore fields and message types they do not know. A change to
  the meaning of a field, or to the framing, is a new protocol version.

## What the daemon no longer does

A receiver can send `Pause`, `Play`, next and previous on its events
connection, for example when an Apple TV goes into standby or its remote's
pause button is pressed. The daemon used to answer a pause by deselecting the
output and flagging it as `paused_by_device`. It now only reports the command
as a `receiver_command` event, and the output stays selected and keeps
playing. Deciding what a command means is left to the supervisor, which can
hold a speaker that paused itself and later restart it with the output's
`hold` field (see [the JSON API](json-api.md#update-an-output)). The
`paused_by_device` field has been removed from the output object.

An Apple TV's remote sends `paus` and `play` from most screens, and the toggle
`plps` (play or pause, whichever applies) while its full-screen now-playing
view is up. It also sends `play` on its own when that view opens.

A volume set by a receiver is still applied by the daemon, and is now also
reported as a `device_volume` event.

## Trying it by hand

`tools/event_channel_client.py` is a small client that uses only the Python 3
standard library.

```shell
python3 tools/event_channel_client.py check      # connect, show the hello, list the outputs
python3 tools/event_channel_client.py observe    # print every message as it arrives
```

`observe` pings on its own every 10 seconds; add `--no-ping` to see the server
close an idle connection, `--stall` to stop reading after the hello and see a
slow client dropped, and `--log FILE` to append each raw line to a file. The
socket path is read from `GET /api/config` unless `--socket PATH` is given.

The same tool enables an Apple TV's group with or without the Apple TV and
prints its row until it is streaming or has failed:

```shell
python3 tools/event_channel_client.py enable TVID --speakers-only
python3 tools/event_channel_client.py enable TVID --with-tv
```

It reports a `409` if the option differs from the one the output is already
selected with, or if `--speakers-only` is asked of an Apple TV that is not
leading a group of HomePods (see [JSON API](json-api.md#update-an-output)).
