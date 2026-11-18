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
from 1 on each connection and is meant only for matching log lines.

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
`receiver_command`, `device_volume`, `include_tv` and `start_volume`.
`start_volume` means an enable that carries a `volume` starts the speakers at
that level (see [Update an output](json-api.md#update-an-output)).

`changed` carries no detail. The client reads `GET /api/outputs` to find out
what changed. At most one `changed` is sent per 20 ms on a connection, so a
burst of changes arrives as one message and nothing is lost.

`event` has two kinds. `at_ms` is the daemon's monotonic clock in
milliseconds, on the same scale as `uptime_ms` in the hello, so a client can
put an event in order with the hello.

| `kind` | Extra fields | Meaning |
|--------|--------------|---------|
| `receiver_command` | `verb` (the receiver's own word, unchanged: for example `paus`, `play`, `nitm`, `pitm`), `protocol` (`"airplay2_events"`) | A speaker or Apple TV sent a command on its events connection. The daemon does not pause, resume or deselect anything because of it. The receiver still gets its normal reply. |
| `device_volume` | `verb` (`dmcp.device-volume`, `dmcp.volume`, `volumeup` or `volumedown`), `value` (the raw value for `dmcp.device-volume`, a percentage as a decimal string for the others) | A receiver set its volume through the control interface. The volume has already been applied when the event is sent. |

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

`error` codes: `not_allowed`, `too_many_clients`, `hello_timeout`,
`bad_hello`, `protocol_unsupported`, `bad_message`, `line_too_long` and
`shutting_down`.

Example:

```json
{"type":"hello","protocol":1,"client":"supervisor 1.0"}
{"type":"hello","seq":1,"protocol":1,"version":"1.6.0","boot":"3f9a1c2e77b04d15","uptime_ms":1843,"capabilities":["changed","receiver_command","device_volume","include_tv","start_volume"],"ping_interval_ms":10000,"idle_close_ms":30000,"max_line":4096}
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
  through `startup`, `connected` and `streaming`, or `failed`, and each step
  is announced with `changed`.
- Enabling one output with `PUT /api/outputs/{id}` still waits for the
  speaker's answer, and other requests to the player wait behind it. A start
  makes one attempt: the connection and each request up to setting up the
  stream give up after 4 seconds, and a request after that after 15, so a
  refusal comes back at once from a speaker that is off, within about 4
  seconds from one that does not answer, and within about 30 seconds at
  worst from one that answers part of the way and then stalls. The one
  retry is a stereo pair's pair-setup rejection just after a stop, tried up
  to twice more 3 seconds apart. A client that sees a timeout while a start
  is in progress should treat the request as pending and wait for the next
  `changed`, not conclude that the daemon is down. Starting a speaker again
  after a failure is the supervisor's job.
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
playing. Deciding what a command means is left to the supervisor. The
`paused_by_device` field has been removed from the output object.

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
