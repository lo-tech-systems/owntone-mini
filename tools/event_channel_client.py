#!/usr/bin/env python3
"""Talk to the owntone-mini notifier socket from a shell.

  event_channel_client.py check     connect, print the server's hello, list the outputs
  event_channel_client.py observe   print every message as it arrives
  event_channel_client.py enable TVID --speakers-only | --with-tv
                                    enable an Apple TV's group with or without the
                                    Apple TV and print its row until it streams or fails

The socket path comes from GET /api/config on the daemon's HTTP port unless
--socket is given (the enable command uses only the HTTP port). Python 3,
standard library only.

Exit status: 0 on success, 1 on any other failure, 2 when the server does not
speak protocol 1, 3 when the server closed an observed connection.
"""

import argparse
import json
import socket
import sys
import time
import urllib.error
import urllib.request

PROTOCOL = 1
PING_INTERVAL_S = 10
CLIENT_NAME = "event_channel_client"
ENABLE_TIMEOUT_S = 30

EXIT_FAIL = 1
EXIT_PROTOCOL = 2
EXIT_CLOSED = 3


def stamp():
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()) + ".%03d" % (int(time.time() * 1000) % 1000)


def http_get_json(base, path, timeout=5):
    with urllib.request.urlopen(base.rstrip("/") + path, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def socket_path(args):
    if args.socket:
        return args.socket

    try:
        config = http_get_json(args.url, "/api/config")
    except (urllib.error.URLError, OSError, ValueError) as err:
        sys.exit("could not read /api/config from %s: %s" % (args.url, err))

    path = config.get("event_socket")
    if not path or not config.get("event_channel"):
        sys.exit("the daemon has no event channel (event_channel: %r)" % config.get("event_channel"))

    return path


class Channel:
    """One connection: lines in, lines out."""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall(json.dumps(obj, separators=(",", ":")).encode("utf-8") + b"\n")

    def recv_line(self, timeout=None):
        """Next line as text, None on timeout, raises EOFError when the server closed."""
        while b"\n" not in self.buf:
            self.sock.settimeout(timeout)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                return None
            except ConnectionResetError:
                raise EOFError
            if not data:
                raise EOFError
            self.buf += data

        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("utf-8", "replace")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def hello(chan):
    """Sends the hello and returns the server's first message."""
    chan.send({"type": "hello", "protocol": PROTOCOL, "client": CLIENT_NAME})
    line = chan.recv_line(timeout=10)
    if line is None:
        sys.exit("no hello from the server within 10 s")
    return line


def describe_row(row):
    session = row.get("session")
    parts = ["%s  %s" % (row.get("id"), row.get("name"))]
    parts.append("selected" if row.get("selected") else "not selected")
    parts.append("has_homepods=%s" % json.dumps(row.get("has_homepods")))
    parts.append("session=%s" % (json.dumps(session, separators=(",", ":")) if session is not None else "null"))
    return "  " + "  ".join(parts)


def cmd_check(args):
    path = socket_path(args)

    try:
        chan = Channel(path)
    except OSError as err:
        print("could not connect to %s: %s" % (path, err), file=sys.stderr)
        return EXIT_FAIL

    try:
        line = hello(chan)
    except EOFError:
        print("the server closed the connection before its hello", file=sys.stderr)
        return EXIT_FAIL
    finally:
        chan.close()

    print(line)
    try:
        msg = json.loads(line)
    except ValueError:
        print("the server's first line is not JSON", file=sys.stderr)
        return EXIT_FAIL

    if msg.get("type") == "error" and msg.get("code") == "protocol_unsupported":
        return EXIT_PROTOCOL
    if msg.get("type") != "hello":
        print("expected a hello, got %r" % msg.get("type"), file=sys.stderr)
        return EXIT_FAIL
    if msg.get("protocol") != PROTOCOL:
        print("the server speaks protocol %r, this client speaks %d" % (msg.get("protocol"), PROTOCOL), file=sys.stderr)
        return EXIT_PROTOCOL

    try:
        outputs = http_get_json(args.url, "/api/outputs")
    except (urllib.error.URLError, OSError, ValueError) as err:
        print("could not read /api/outputs from %s: %s" % (args.url, err), file=sys.stderr)
        return EXIT_FAIL

    rows = outputs.get("outputs", [])
    print("%d output(s)" % len(rows))
    for row in rows:
        print(describe_row(row))

    return 0


def cmd_observe(args):
    path = socket_path(args)
    logfile = open(args.log, "a", encoding="utf-8") if args.log else None

    def emit(text, raw=None):
        print("%s %s" % (stamp(), text), flush=True)
        if logfile and raw is not None:
            logfile.write(raw + "\n")
            logfile.flush()

    try:
        chan = Channel(path)
    except OSError as err:
        print("could not connect to %s: %s" % (path, err), file=sys.stderr)
        return EXIT_FAIL

    try:
        line = hello(chan)
    except EOFError:
        emit("closed by the server before its hello")
        return EXIT_CLOSED

    emit(line, line)

    if args.stall:
        emit("stalling: no further reads")

    next_ping = time.monotonic() + PING_INTERVAL_S
    try:
        while True:
            if not args.no_ping and time.monotonic() >= next_ping:
                chan.send({"type": "ping", "t": int(time.time() * 1000)})
                next_ping = time.monotonic() + PING_INTERVAL_S

            if args.stall:
                time.sleep(0.5)
                continue

            wait = 1.0 if args.no_ping else max(0.0, min(1.0, next_ping - time.monotonic()))
            try:
                line = chan.recv_line(timeout=wait or 0.01)
            except EOFError:
                emit("closed by the server")
                return EXIT_CLOSED
            if line is not None:
                emit(line, line)
    except (BrokenPipeError, ConnectionResetError):
        emit("closed by the server")
        return EXIT_CLOSED
    except KeyboardInterrupt:
        try:
            chan.send({"type": "goodbye"})
        except OSError:
            pass
        return 0
    finally:
        chan.close()
        if logfile:
            logfile.close()


def http_put_json(base, path, body, timeout=120):
    """PUT a JSON body; returns (status, parsed reply or None)."""
    req = urllib.request.Request(base.rstrip("/") + path, data=json.dumps(body).encode("utf-8"),
                                 method="PUT", headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status, data = resp.status, resp.read()
    except urllib.error.HTTPError as err:
        status, data = err.code, err.read()
    try:
        return status, json.loads(data.decode("utf-8")) if data else None
    except ValueError:
        return status, None


def cmd_enable(args):
    body = {"selected": True, "include_tv": not args.speakers_only}
    status, reply = http_put_json(args.url, "/api/outputs/%s" % args.id, body)

    if status == 409:
        error = (reply or {}).get("error") or "reselect_required"
        if error == "group_not_present":
            print("409 conflict: %s (the Apple TV does not lead a group of HomePods just now; "
                  "try again when its HomePods are present)" % error, file=sys.stderr)
        else:
            print("409 conflict: %s (disable the output first, then enable it with the other option)"
                  % error, file=sys.stderr)
        return EXIT_FAIL
    if status >= 300:
        print("the enable failed: HTTP %d %s" % (status, json.dumps(reply) if reply else ""), file=sys.stderr)
        return EXIT_FAIL

    deadline = time.monotonic() + ENABLE_TIMEOUT_S
    last = None
    while True:
        try:
            row = http_get_json(args.url, "/api/outputs/%s" % args.id)
        except (urllib.error.URLError, OSError, ValueError) as err:
            print("could not read /api/outputs/%s: %s" % (args.id, err), file=sys.stderr)
            return EXIT_FAIL

        text = describe_row(row)
        if text != last:
            print("%s %s" % (stamp(), text), flush=True)
            last = text

        # The row shows what it plays: the HomePods when it was enabled without
        # the Apple TV
        session = (row.get("session") or {}).get("state")
        if session == "failed":
            return EXIT_FAIL
        if session == "streaming":
            return 0
        if time.monotonic() >= deadline:
            print("still not streaming after %d s" % ENABLE_TIMEOUT_S, file=sys.stderr)
            return EXIT_FAIL
        time.sleep(1)


def main():
    parser = argparse.ArgumentParser(description="Client for the owntone-mini notifier socket")
    parser.add_argument("--url", default="http://localhost:3689", help="base URL of the daemon's HTTP API (default %(default)s)")
    parser.add_argument("--socket", metavar="PATH", help="socket path (default: from /api/config)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("check", help="connect, print the hello, list the outputs")

    obs = sub.add_parser("observe", help="print every message as it arrives")
    obs.add_argument("--log", metavar="FILE", help="append each raw line to FILE")
    obs.add_argument("--no-ping", action="store_true", help="never send a ping (the server closes the connection after about 30 s)")
    obs.add_argument("--stall", action="store_true", help="stop reading after the hello and leave the connection open")

    ena = sub.add_parser("enable", help="enable an Apple TV's group with or without the Apple TV and watch it start")
    ena.add_argument("id", metavar="TVID", help="id of the Apple TV's output")
    mode = ena.add_mutually_exclusive_group(required=True)
    mode.add_argument("--speakers-only", action="store_true", help="play on the HomePods only")
    mode.add_argument("--with-tv", action="store_true", help="play on the Apple TV and the HomePods")

    args = parser.parse_args()

    if args.command == "check":
        return cmd_check(args)
    if args.command == "enable":
        return cmd_enable(args)
    return cmd_observe(args)


if __name__ == "__main__":
    sys.exit(main())
