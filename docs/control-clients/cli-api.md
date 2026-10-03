# Controlling owntone-mini from the command line

owntone-mini is controlled over its HTTP JSON API, so `curl` is all you need.
This page shows the common operations. The full reference, including every
field and status code, is in the [JSON API docs](../json-api.md).

The examples assume the default port, `3689`, and run on the same machine as
the server. Change `localhost` and the port to match your setup (`port` in the
configuration file).

Only addresses listed in `trusted_networks` can use the API without a
password. The default is `lan`. A client outside the trusted networks gets
`403 Forbidden`, unless `admin_password` is set, in which case use
`curl -u admin:PASSWORD`.

## List outputs

```shell
curl http://localhost:3689/api/outputs
```

Each output has an `id`, a `name`, whether it is `selected`, its `volume`, and
the `mode` and `offset_ms` currently set. Use the `id` in the commands below.

## Enable or disable an output

```shell
curl -X PUT http://localhost:3689/api/outputs/1 \
  -H "Content-Type: application/json" \
  -d '{"selected":true}'
```

Send `{"selected":false}` to disable it. To make a list of outputs the only
enabled ones in one request:

```shell
curl -X PUT http://localhost:3689/api/outputs/set \
  -H "Content-Type: application/json" \
  -d '{"outputs":["1","2"]}'
```

## Set the volume

```shell
curl -X PUT http://localhost:3689/api/outputs/1 \
  -H "Content-Type: application/json" \
  -d '{"volume":40}'
```

Volume is a percentage from 0 to 100, set per output.

## Set a speaker's mode or offset

```shell
curl -X PUT http://localhost:3689/api/outputs/1 \
  -H "Content-Type: application/json" \
  -d '{"mode":"airplay2_buffered","offset_ms":-120}'
```

`mode` must be one of the names in the output's `supported_modes`, or `auto`.
`offset_ms` shifts the speaker's timing, from -2000 to 2000.

## Start and stop playback

```shell
curl -X PUT http://localhost:3689/api/player/play
curl -X PUT http://localhost:3689/api/player/stop
```

This starts and stops playback of the audio coming in on the pipe. Both return
`204 No Content` on success.

## Push metadata

```shell
curl -X PUT http://localhost:3689/api/metadata \
  -H "Content-Type: application/json" \
  -d '{"title":"My Track","artist":"My Artist","album":"My Album"}'
```

Fields you leave out are not changed. `artwork_url` can be given as
`file:/absolute/path.jpg`.

## Read or change a setting

```shell
curl http://localhost:3689/api/settings/player/start_buffer_ms

curl -X PUT http://localhost:3689/api/settings/player/start_buffer_ms \
  -H "Content-Type: application/json" \
  -d '{"value":1500}'
```

The reply to a change says whether a restart is needed for it to take effect:
`{"restart_required":true}`.
