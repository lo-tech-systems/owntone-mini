# owntone-mini

Minimal, personal, non-commercial fork of OwnTone, stripped back to provide
playback from a FIFO and extended to improve compatibility with HomePods and
Apple TV.

## Credits

- OwnTone and its contributors
- shairport-sync and nqptp (Mike Brady)
- pyatv
- openairplay
- Emanuele Cozzi - AirPlay 2 Internals
- music-assistant/airplay-cli - documentation and observed behaviour of the
  MediaRemote message set used for Apple TV Now Playing support (see
  docs/audio-outputs/airplay.md)

owntone-mini is an independent open-source project. It is not affiliated with,
authorised, sponsored or endorsed by Apple Inc. AirPlay, Apple TV, HomePod and
iTunes are trademarks of Apple Inc., registered in the U.S. and other
countries. No claim is made that operation with any device is authorised or
will continue to work. No Apple SDK, NDA-covered specification, or confidential
documentation was used in the development of features specific to this fork.

## Building and running

Dependencies (see configure.ac for minimum versions): a C compiler and the
autotools, zlib, libsodium, libgcrypt, libevent (with libevent_pthreads),
json-c, libplist, ffmpeg or libav (libavformat, libavcodec, libavutil,
libavfilter), and either Avahi or a Bonjour dns_sd library for mDNS.

    autoreconf -i && ./configure && make && sudo make install

Useful configure options:

- `--with-avahi` / `--without-avahi`: use Avahi for mDNS, or use the Bonjour
  dns_sd library instead
- `--with-libav`: use libav even if ffmpeg is present
- `--disable-preferairplay2`: do not prefer AirPlay 2 for devices that support
  both AirPlay 1 and 2
- `--with-user=USER`, `--with-group=GROUP`: user and group the service runs as
- `--disable-install-systemd`: do not install the systemd service file
- `--with-systemddir=DIR`: where to install the systemd service file

Settings are read from a JSON file, `owntone-settings.json`; see the
configuration page below.

## Documentation

- [Configuration](docs/configuration.md)
- [JSON API](docs/json-api.md)
- [Pipe input](docs/inputs/pipe.md)
- [AirPlay outputs](docs/audio-outputs/airplay.md)
- [Artwork](docs/artwork.md)
- [Command line and curl examples](docs/control-clients/cli-api.md)
- [Differences from OwnTone](FORK.md)
