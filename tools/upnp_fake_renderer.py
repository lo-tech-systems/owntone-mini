#!/usr/bin/env python3
"""A stand-in for a UPnP renderer's RenderingControl, for checking owntone-mini's
volume following without a speaker.

  upnp_fake_renderer.py [--addr 127.0.0.1] [--http-port 49152] [--ssdp-port 1900] [--multicast]

It answers M-SEARCH on addr:ssdp-port (and, with --multicast, on the SSDP group),
serves a description, takes SUBSCRIBE, renewals and UNSUBSCRIBE, and sends
events in the form an Edifier MS50A uses: the whole state after a subscribe,
then the changed values. Control requests on the same HTTP port drive it:

  GET /ctl/set?volume=N&mute=0|1   change the level and send an event to each subscriber
  GET /ctl/notify?kind=K           send one event and report how it was answered;
                                   K is plain, chunked, bogus_sid or oversize
  GET /ctl/flood?n=N               open N idle connections to the subscriber and
                                   report which were closed at once
  GET /ctl/mode?renew412=0|1&holdopen=0|1&slow=0|1
                                   refuse renewals, hold a subscribe's answer open
                                   with no length, or answer the description late
  GET /ctl/state                   subscriptions, renewals and unsubscribes so far

Each answer is JSON. Python 3, standard library only. Used by the upnp_http_check
program built under "make check".
"""

import argparse
import json
import socket
import struct
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

SSDP_GROUP = "239.255.255.250"
ST_RC = "urn:schemas-upnp-org:service:RenderingControl:1"

DESCRIPTION = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<device>
<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
<friendlyName>Fake Renderer</friendlyName>
<manufacturer>Nobody</manufacturer>
<modelName>Fake RC</modelName>
<UDN>uuid:{udn}</UDN>
<serviceList>
<service>
<serviceType>urn:schemas-upnp-org:service:GroupRenderingControl:1</serviceType>
<eventSubURL>/upnp/event/group1</eventSubURL>
</service>
<service>
<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>
<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>
<controlURL>/upnp/control/rendercontrol1</controlURL>
<eventSubURL>/upnp/event/rendercontrol1</eventSubURL>
</service>
</serviceList>
</device>
</root>
"""


def escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def event_body(volume=None, mute=None):
    parts = []
    if mute is not None:
        parts.append('<Mute channel="Master" val="%d"/>' % (1 if mute else 0))
    if volume is not None:
        parts.append('<Volume channel="Master" val="%d"/>' % volume)
    inner = ('<Event xmlns="urn:schemas-upnp-org:metadata-1-0/RCS/">\r\n  <InstanceID val="0">\r\n  '
             + "\r\n  ".join(parts) + "\r\n  </InstanceID>\r\n</Event>")
    return ("<?xml version='1.0' encoding='UTF-8' ?>\r\n"
            '<e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0">\r\n  <e:property>\r\n'
            "    <LastChange>" + escape(inner) + "</LastChange>\r\n</e:property>\r\n</e:propertyset>\r\n")


class Renderer:
    def __init__(self, addr, http_port):
        self.addr = addr
        self.http_port = http_port
        self.udn = str(uuid.uuid4())
        self.lock = threading.Lock()
        self.volume = 25
        self.mute = False
        self.subs = {}            # sid -> {"callback": (host, port, path), "seq": n}
        self.renewals = 0
        self.unsubscribed = []
        self.renew412 = False
        self.holdopen = False
        self.slow = False

    def send_event(self, sid, body, kind="plain", timeout=3.0):
        """Sends one event to a subscriber; the status it answered, or "closed"."""
        with self.lock:
            sub = self.subs.get(sid)
            if not sub:
                return "no_subscription"
            seq = sub["seq"]
            sub["seq"] += 1
            host, port, path = sub["callback"]
        if kind == "bogus_sid":
            sid = "uuid:00000000-0000-0000-0000-000000000000"
        if kind == "oversize":
            body = body + "<!--" + "x" * 20000 + "-->"
        data = body.encode()
        head = ("NOTIFY %s HTTP/1.1\r\nHOST: %s:%d\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                "NT: upnp:event\r\nNTS: upnp:propchange\r\nSID: %s\r\nSEQ: %d\r\n" % (path, host, port, sid, seq))
        if kind == "chunked":
            mid = len(data) // 2
            chunks = [data[:mid], data[mid:]]
            payload = b"".join(b"%x\r\n%s\r\n" % (len(c), c) for c in chunks) + b"0\r\n\r\n"
            msg = (head + "TRANSFER-ENCODING: chunked\r\n\r\n").encode() + payload
        else:
            msg = (head + "CONTENT-LENGTH: %d\r\n\r\n" % len(data)).encode() + data
        try:
            s = socket.create_connection((host, port), timeout=timeout)
        except OSError as exc:
            return "connect_failed: %s" % exc
        try:
            s.settimeout(timeout)
            try:
                s.sendall(msg)
            except OSError:
                return "closed"
            reply = b""
            while b"\r\n\r\n" not in reply:
                try:
                    d = s.recv(4096)
                except OSError:
                    return "closed"
                if not d:
                    return "closed" if not reply else "partial"
                reply += d
            line = reply.split(b"\r\n", 1)[0].decode()
            return int(line.split()[1])
        finally:
            s.close()

    def notify_all(self, volume=None, mute=None):
        with self.lock:
            sids = list(self.subs)
        return [self.send_event(sid, event_body(volume, mute)) for sid in sids]

    def flood(self, n):
        """Opens n idle connections to the first subscriber's callback; for
        each, whether it was closed at once."""
        with self.lock:
            if not self.subs:
                return []
            host, port, _ = next(iter(self.subs.values()))["callback"]
        socks = []
        result = []
        for _ in range(n):
            s = socket.create_connection((host, port), timeout=2)
            socks.append(s)
            time.sleep(0.05)
        time.sleep(0.3)
        for s in socks:
            s.setblocking(False)
            try:
                d = s.recv(1)
                result.append("closed" if d == b"" else "data")
            except BlockingIOError:
                result.append("open")
            except OSError:
                result.append("closed")
        for s in socks:
            s.close()
        return result


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    renderer = None

    def log_message(self, *args):
        pass

    def reply_json(self, obj):
        data = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def do_GET(self):
        r = self.renderer
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}

        if url.path == "/description.xml":
            if r.slow:
                time.sleep(3)
            data = DESCRIPTION.format(udn=r.udn).encode()
            self.send_response(200)
            self.send_header("Content-Type", 'text/xml; charset="utf-8"')
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            try:
                self.end_headers()
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass  # a request cancelled while the answer was held back
            self.close_connection = True
        elif url.path == "/ctl/set":
            volume = int(q["volume"]) if "volume" in q else None
            mute = (q["mute"] == "1") if "mute" in q else None
            with r.lock:
                if volume is not None:
                    r.volume = volume
                if mute is not None:
                    r.mute = mute
            self.reply_json({"answers": r.notify_all(volume, mute)})
        elif url.path == "/ctl/notify":
            with r.lock:
                sids = list(r.subs)
                body = event_body(r.volume, r.mute)
            kind = q.get("kind", "plain")
            self.reply_json({"answers": [r.send_event(sid, body, kind) for sid in sids]})
        elif url.path == "/ctl/flood":
            self.reply_json({"result": r.flood(int(q.get("n", "9")))})
        elif url.path == "/ctl/mode":
            for key in ("renew412", "holdopen", "slow"):
                if key in q:
                    setattr(r, key, q[key] == "1")
            self.reply_json({"renew412": r.renew412, "holdopen": r.holdopen, "slow": r.slow})
        elif url.path == "/ctl/state":
            with r.lock:
                self.reply_json({"subscriptions": list(r.subs), "renewals": r.renewals,
                                 "unsubscribed": r.unsubscribed, "volume": r.volume})
        else:
            self.send_error(404)

    def do_SUBSCRIBE(self):
        r = self.renderer
        if self.path != "/upnp/event/rendercontrol1":
            self.send_error(404)
            return
        sid = self.headers.get("SID")
        timeout = "Second-1800"
        if sid:
            with r.lock:
                known = sid in r.subs
                if known and not r.renew412:
                    r.renewals += 1
            if not known or r.renew412:
                self.send_response(412)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        else:
            cb = (self.headers.get("CALLBACK") or "").strip("<> ")
            u = urlparse(cb)
            if self.headers.get("NT") != "upnp:event" or u.scheme != "http":
                self.send_response(412)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            sid = "uuid:" + str(uuid.uuid4())
            with r.lock:
                r.subs[sid] = {"callback": (u.hostname, u.port or 80, u.path or "/"), "seq": 0}

        if r.holdopen:
            # The answer with no length, and the connection left open a while
            self.wfile.write(("HTTP/1.1 200 OK\r\nSID: %s\r\nTIMEOUT: %s\r\n\r\n" % (sid, timeout)).encode())
            self.wfile.flush()
            time.sleep(6)
            self.close_connection = True
        else:
            self.send_response(200)
            self.send_header("SID", sid)
            self.send_header("TIMEOUT", timeout)
            self.send_header("Content-Length", "0")
            self.end_headers()

        # The whole state after a fresh subscribe, as a renderer does
        if self.headers.get("NT"):
            with r.lock:
                body = event_body(r.volume, r.mute)
            threading.Timer(0.05, r.send_event, (sid, body)).start()

    def do_UNSUBSCRIBE(self):
        r = self.renderer
        sid = self.headers.get("SID")
        with r.lock:
            known = sid in r.subs
            if known:
                del r.subs[sid]
                r.unsubscribed.append(sid)
        self.send_response(200 if known else 412)
        self.send_header("Content-Length", "0")
        self.end_headers()


def ssdp_loop(r, sock):
    while True:
        data, peer = sock.recvfrom(4096)
        if not data.startswith(b"M-SEARCH"):
            continue
        reply = ("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                 "LOCATION: http://%s:%d/description.xml\r\nSERVER: Linux/1 UPnP/1.0 fake/1\r\n"
                 "ST: %s\r\nUSN: uuid:%s::%s\r\n\r\n" % (r.addr, r.http_port, ST_RC, r.udn, ST_RC))
        sock.sendto(reply.encode(), peer)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--addr", default="127.0.0.1")
    ap.add_argument("--http-port", type=int, default=49152)
    ap.add_argument("--ssdp-port", type=int, default=1900)
    ap.add_argument("--multicast", action="store_true", help="also join the SSDP group")
    args = ap.parse_args()

    r = Renderer(args.addr, args.http_port)
    Handler.renderer = r

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0" if args.multicast else args.addr, args.ssdp_port))
    if args.multicast:
        mreq = struct.pack("4s4s", socket.inet_aton(SSDP_GROUP), socket.inet_aton("0.0.0.0"))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    threading.Thread(target=ssdp_loop, args=(r, sock), daemon=True).start()

    server = ThreadingHTTPServer((args.addr, args.http_port), Handler)
    server.daemon_threads = True
    print("fake renderer on %s: http %d, ssdp %d" % (args.addr, args.http_port, args.ssdp_port), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
