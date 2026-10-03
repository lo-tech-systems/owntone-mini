# Artwork

This build has no music library, artwork cache or online artwork lookup.
Cover artwork comes from one of two places, and is sent to AirPlay receivers
when the track information is pushed to them.

## Where artwork comes from

- **Pipe metadata (normal case).** A `PICT` item on the metadata pipe carries a
  JPEG or PNG picture. The server keeps the bytes in memory for the current
  session, never on disk, and replaces them when a new picture arrives. See
  [Pipe input](inputs/pipe.md#artwork) for the format, size limits and
  bundling of updates.
- **`PUT /api/metadata`.** The `artwork_url` parameter names a local `.jpg`,
  `.jpeg` or `.png` file as `file:/absolute/path`. The file is read from disk
  each time artwork is needed. See [Metadata](json-api.md#metadata).

If the current item has neither, no artwork is sent. The image is passed on
as it is: it is not resized or converted, so keep it to about 48 KB.

## How artwork reaches receivers

Each time track information is sent to the outputs, the server fetches the
current artwork once and then sends it in the way each receiver supports.

- **AirPlay 1 receivers.** Artwork is sent with the track text (DMAP
  metadata) in an RTSP `SET_PARAMETER` request, if the receiver advertises
  artwork support.
- **AirPlay 2 receivers.** The same kind of `SET_PARAMETER` artwork request is
  used for receivers that advertise artwork support.
- **Apple TV.** Receivers that support it also get a now-playing update over
  the AirPlay control channel, with the artwork attached. The artwork is sent
  in a second push shortly after the track change. See
  [Now Playing on Apple TV](audio-outputs/airplay.md#now-playing-on-apple-tv).

A receiver that does not advertise artwork support gets text only.
