# Pipe input

The pipe input is the only audio source. The server reads raw PCM audio from a
named pipe (FIFO) and streams it to the selected AirPlay outputs. Track
information and cover artwork can be supplied on a second, sibling pipe, or
through the HTTP API.

## The audio pipe

The FIFO path is the `misc/pipe_path` setting (see
[Settings](../json-api.md#settings)). The server does not create the FIFO: it
must exist, be a FIFO (not a regular file) and be readable by the server
process. Another program writes audio into it.

### Audio format

The stream is raw PCM with no header:

- Channels: 2 (stereo, interleaved). This is fixed.
- Sample rate: `player/pipe_sample_rate`, one of 44100, 48000, 88200 or
  96000 Hz. Default 44100.
- Bit depth: `player/pipe_bits_per_sample`, 16 or 32. Default 16.
- Samples are signed integers in the byte order of the machine the server runs
  on (little-endian on the usual hardware).

The sample rate and bit depth are read at startup, so changing them needs a
restart. Any other value makes the server refuse to start. Whatever writes into
the FIFO must produce exactly this format; the server cannot detect a mismatch.

### Starting and stopping playback

With `misc/pipe_autostart` enabled (the default), the server watches the FIFO.
When data arrives it stops any current playback and starts playing the pipe.
When the writer closes the FIFO (end of file) an autostarted playback stops,
and the server goes back to watching for new data.

With `pipe_autostart` disabled, nothing starts by itself. Start playback with
`PUT /api/player/play` (see [Player](../json-api.md#player)). A playback that
was started this way does not stop at end of file: when the FIFO has no data,
the input just waits for more, and it stops only on `PUT /api/player/stop` or a
read error. If the writer reopens the FIFO later, audio continues.

If the FIFO does not exist when the server starts, or is deleted and recreated
while the server runs, the server notices and re-attaches to the path. This
check runs every 30 seconds, so autostart can take up to that long to begin
working after the FIFO appears.

Changing `misc/pipe_path` or `misc/pipe_autostart` through the settings API
takes effect with `PUT /api/update`. The new path must already exist and be a
FIFO. See [Update](../json-api.md#update).

## The metadata pipe

For each playback the server also opens a second FIFO whose name is the audio
pipe path with `.metadata` appended. For `/tmp/owntone.fifo` that is
`/tmp/owntone.fifo.metadata`.

- It is opened when playback starts and closed when playback stops. It must
  already exist at that moment, and be a FIFO. If it does not exist, the
  session runs without metadata and the server does not retry until the next
  playback start.
- It is optional.
- On end of file the server reopens it, so a writer can come and go during a
  session.

### Format

The format is the one used by shairport-sync's metadata pipe: a stream of
XML-like items, each ending in `</item>`:

```
<item><type>636f7265</type><code>6d696e6d</code><length>5</length>
<data encoding="base64">
SGVsbG8=</data></item>
```

- `type` and `code` are four ASCII characters written as eight hex digits.
  `type` is normally `core` (`636f7265`) for track information or `ssnc`
  (`73736e63`) for control items. The server does not look at the type, only
  that it is present and non-zero. The meaning comes from `code`.
- `length` is ignored.
- `data`, when present, is base64. It may carry attributes on the opening tag.
- An item that cannot be parsed is skipped and reading continues. If more than
  1 MiB of unparsed data builds up in the buffer, it is discarded.

### Codes understood

| Code | Meaning | Payload |
| ---- | ------- | ------- |
| `minm` | Track title | text |
| `asar` | Artist | text |
| `asal` | Album | text |
| `asgn` | Genre | text |
| `prgr` | Progress | text, `start/current/end` as frame counts |
| `PICT` | Cover artwork | JPEG or PNG image bytes |
| `pvol` | Volume | text, shairport-sync style (see below) |
| `pfls` | Flush playback | none |
| `mdst` | Start of a metadata update | none |
| `mden` | End of a metadata update | none |

All other codes are ignored. An item that needs a payload but has none is
ignored.

**Progress.** `prgr` holds three frame counts separated by `/`: the start of
the track, the current position and the end. They are converted to
milliseconds using `pipe_sample_rate`. The position can be negative (the track
starts in the future). If the end is not after the start, the length is
treated as unknown. Any of the three being zero is rejected.

**Bracketing.** A sender can wrap one update in `mdst` and `mden`. Text,
progress and picture received between them are held and applied together when
`mden` arrives, so the receiver sees a track change and its artwork as one
event. Senders that never send `mdst` get each item applied as it arrives.
Bracketing is recommended: AirPlay receivers tend to attach artwork to the
first track item they see.

**Volume.** `pvol` is the shairport-sync string `airplay,0.00,0.00,0.00`,
where `airplay` is a level from -30.0 to 0.0, or -144 for mute. The level maps
to 0 to 100 percent and sets the player volume. A string that does not end in
`,0.00,0.00,0.00` is ignored.

**Flush.** `pfls` flushes playback (what shairport-sync sends on a seek or
pause).

### Artwork

- The picture must be JPEG (starts with bytes `FF D8`) or PNG (starts with
  `89 50`) and between 2 bytes and 1 MiB. Anything else is rejected with a log
  message.
- Keep pictures to about 48 KB or less. Receivers are known to render that
  size reliably, and the server does not resize or re-encode.
- Only one picture is held, in memory, for the current session. A new `PICT`
  replaces it. It is never written to disk. It is released when playback stops
  and the metadata pipe is closed.
- Title, artist, album, genre, progress and artwork are stored against the
  single queue item for the pipe. A field that an update leaves out keeps its
  previous value. The title is `Pipe` until one is received.

How the artwork then reaches receivers is described in
[Artwork](../artwork.md).

## Pushing metadata over the API

`PUT /api/metadata` sets the title, artist, album and an artwork file on the
same queue item and pushes it to the active outputs; see
[Metadata](../json-api.md#metadata). This is useful when the audio source
cannot write a metadata pipe.

Both routes write to the same queue item, and there is no priority between
them: the most recent write to a field wins. A later pipe update replaces what
the API set, and the other way round. Fields that an update omits are left as
they were.

## Example

This is only an example. Any program that writes the right raw PCM into the
FIFO will work.

Create the FIFO, and optionally the metadata FIFO, before starting playback:

```shell
mkfifo /tmp/owntone.fifo
mkfifo /tmp/owntone.fifo.metadata
```

Set `misc/pipe_path` to `/tmp/owntone.fifo` (and `PUT /api/update` if the
server is already running). Select an AirPlay output. Then write audio in the
configured format, here 44100 Hz, 16-bit, stereo, using `ffmpeg` to convert a
file:

```shell
ffmpeg -re -i song.flac -f s16le -ar 44100 -ac 2 - > /tmp/owntone.fifo
```

or copy a file that is already in that raw format:

```shell
cat song.raw > /tmp/owntone.fifo
```

With `pipe_autostart` on, playback begins when the first data arrives and stops
when the command finishes. To send a title over the metadata pipe, write to it
after playback has started, so that the server has opened it for reading:

```shell
printf '%s' '<item><type>636f7265</type><code>6d696e6d</code><length>5</length><data encoding="base64">SGVsbG8=</data></item>' > /tmp/owntone.fifo.metadata
```

This sets the title to `Hello`. To use the HTTP API instead:

```shell
curl -X PUT "http://localhost:3689/api/metadata" \
     -H "Content-Type: application/json" \
     -d '{"title":"Hello","artist":"Someone"}'
```
