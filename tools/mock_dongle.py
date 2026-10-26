#!/usr/bin/env python3
"""Mock Saab Dash dongle: speaks protocol v2 (PROTOCOL.md) so the app can be tested without hardware.

Standard library only. Serves the app like the dongle does, streams simulated values over
WebSocket /ws, answers /api/info and accepts /api/app and /api/ota uploads.

    python3 tools/mock_dongle.py            # http://localhost:8080
    python3 tools/mock_dongle.py --port 9000

Open the printed LAN address on an iPhone on the same Wi-Fi to test the real phone.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import shutil
import socket
import struct
import tempfile
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MSG_DATA, MSG_DTC, MSG_LOG = 0x01, 0x02, 0x03
FW_VERSION = "0.0.0-mock"


class Car:
    """Simulated T8 on E85, same driving loop as the app's demo mode."""

    def __init__(self):
        self.t0 = time.time()
        self.coolant = 62.0
        self.dtcs = []          # [(code, status)]
        self.seq = 0
        self.slide = 0
        self.lock = threading.Lock()

    def data(self):
        t = time.time() - self.t0
        cyc = t % 16
        load = 0 if cyc < 3 else (cyc - 3) / 5 if cyc < 8 else -0.2 if cyc < 10 else 0.25
        rpm = (850 + 30 * math.sin(t * 3) if cyc < 3 else 1800 + 4300 * (cyc - 3) / 5 if cyc < 8
               else 2600 - 400 * (cyc - 8) if cyc < 10 else 2200 + 50 * math.sin(t))
        boost = -0.62 + 0.03 * math.sin(t * 2) if load <= 0 else min(1.45, -0.4 + 2.1 * load) + 0.03 * math.sin(t * 9)
        self.coolant = min(91.0, self.coolant + 0.005)
        speed = 0 if cyc < 3 else min(160, 40 + 18 * (cyc - 3) + (30 if cyc > 8 else 0))
        req = max(90, round(120 + 1050 * max(0, load)))
        act = round(req * (0.96 + 0.03 * math.sin(t * 4)))
        afr = 9.83 + (-1.6 if load > 0.6 else 0) + 0.08 * math.sin(t * 5)
        with self.lock:
            if not self.dtcs and 30 < t < 31:
                self.dtcs = [(0x0171, 0x48)]
            flags = 1 | (2 if 20 < t < 28 else 0) | (4 if self.dtcs else 0) | (8 if self.slide == 5 else 0)
            n_dtc = len(self.dtcs)
        self.seq = (self.seq + 1) & 0xFF
        u8 = lambda v: max(0, min(254, round(v)))
        return struct.pack("<BBhHBBBBHHHBBBB", 1, flags, round(boost * 1000), round(rpm), u8(speed),
                           u8(self.coolant + 40), u8(24 + 10 * max(0, load) + 40), u8(14.1 * 10),
                           req, act, round(afr * 100), 78, 54, n_dtc, self.seq)

    def dtc(self):
        with self.lock:
            out = struct.pack("<BB", 1, len(self.dtcs))
            for code, status in self.dtcs:
                out += struct.pack(">HB", code, status)
        return out


class Dongle:
    def __init__(self, www):
        self.www = www
        self.car = Car()
        self.clients = set()
        self.clients_lock = threading.Lock()
        self.rebooting_until = 0.0
        self.started = time.time()

    def broadcast(self, payload):
        with self.clients_lock:
            dead = []
            for c in self.clients:
                try:
                    c.send_frame(payload)
                except OSError:
                    dead.append(c)
            for c in dead:
                self.clients.discard(c)

    def log(self, text):
        print("LOG:", text)
        self.broadcast(bytes([MSG_LOG]) + text.encode())

    def stream(self):
        last_dtc = None
        while True:
            time.sleep(0.05)
            if time.time() < self.rebooting_until:
                continue
            self.broadcast(bytes([MSG_DATA]) + self.car.data())
            dtc = self.car.dtc()
            if dtc != last_dtc:
                last_dtc = dtc
                self.broadcast(bytes([MSG_DTC]) + dtc)

    def command(self, client, b):
        if not b:
            return
        if b[0] == 0x01:
            with self.car.lock:
                self.car.dtcs = []
            self.log("Felkoder raderade")
        elif b[0] == 0x02:
            client.send_frame(bytes([MSG_DTC]) + self.car.dtc())
            self.log("Felkoder lästa")
        elif b[0] == 0x10 and len(b) > 1:
            self.car.slide = b[1]
            self.log(f"Aktiv sida {b[1]}" + (" (diagnossession på)" if b[1] == 5 else ""))
        elif b[0] == 0x30:
            self.log(f"konsol: {b[1:].decode(errors='replace')} -> ok (mock)")

    def reboot(self, seconds=6):
        self.rebooting_until = time.time() + seconds
        with self.clients_lock:
            for c in list(self.clients):
                c.close()
            self.clients.clear()
        print(f"Simulerar omstart i {seconds} s")


def make_handler(dongle):
    class Handler(SimpleHTTPRequestHandler):
        protocol_version = "HTTP/1.1"   # WebSocket upgrade needs a 1.1 status line

        def __init__(self, *a, **kw):
            super().__init__(*a, directory=dongle.www, **kw)

        def log_message(self, fmt, *args):
            if "/api/" in self.path or self.command != "GET":
                super().log_message(fmt, *args)

        def end_headers(self):
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.split("?")[0] == "/ws":
                return self.websocket()
            if self.path.split("?")[0] == "/api/info":
                if time.time() < dongle.rebooting_until:
                    self.close_connection = True
                    return
                return self.json({"device": "saab-dash", "fw": FW_VERSION, "app": app_build(dongle.www),
                                  "ip": local_ip(), "rssi": -58, "uptime": int(time.time() - dongle.started), "vbat": 14.1})
            return super().do_GET()

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n)
            if self.path == "/api/app":
                if b"<html" not in body[:4096].lower():
                    return self.json({"ok": False, "error": "inte en html-fil"}, 400)
                tmp = os.path.join(dongle.www, "index.html.new")
                with open(tmp, "wb") as f:
                    f.write(body)
                os.replace(tmp, os.path.join(dongle.www, "index.html"))   # atomic swap, like the firmware
                dongle.log(f"Ny app sparad ({len(body)} byte, build {app_build(dongle.www)})")
                return self.json({"ok": True})
            if self.path == "/api/ota":
                if len(body) < 1024:
                    return self.json({"ok": False, "error": "för liten fil"}, 400)
                dongle.log(f"Firmware mottagen ({len(body)} byte), startar om")
                self.json({"ok": True})
                threading.Timer(0.3, dongle.reboot).start()
                return
            self.json({"ok": False, "error": "okänd sökväg"}, 404)

        # --- minimal RFC 6455 server side ---
        def websocket(self):
            if time.time() < dongle.rebooting_until:
                self.send_error(503)
                return
            key = self.headers.get("Sec-WebSocket-Key")
            if not key:
                self.send_error(400)
                return
            accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            super().end_headers()
            self.wfile.flush()
            self.close_connection = True
            client = WsClient(self.connection)
            client.send_frame(bytes([MSG_DTC]) + dongle.car.dtc())
            with dongle.clients_lock:
                dongle.clients.add(client)
            print("WebSocket ansluten:", self.client_address[0])
            try:
                while True:
                    msg = client.recv()
                    if msg is None:
                        break
                    dongle.command(client, msg)
            finally:
                with dongle.clients_lock:
                    dongle.clients.discard(client)
                print("WebSocket stängd:", self.client_address[0])

    return Handler


class WsClient:
    def __init__(self, sock):
        self.sock = sock
        self.wlock = threading.Lock()

    def send_frame(self, payload, opcode=0x2):
        n = len(payload)
        head = bytes([0x80 | opcode]) + (bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n)
                                         if n < 65536 else bytes([127]) + struct.pack(">Q", n))
        with self.wlock:
            self.sock.sendall(head + payload)

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError
            buf += chunk
        return buf

    def recv(self):
        try:
            while True:
                b0, b1 = self._read(2)
                op, n = b0 & 0x0F, b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self._read(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._read(8))[0]
                mask = self._read(4) if b1 & 0x80 else b"\0\0\0\0"
                data = bytes(c ^ mask[i % 4] for i, c in enumerate(self._read(n)))
                if op == 0x8:
                    return None
                if op == 0x9:
                    self.send_frame(data, 0xA)
                    continue
                if op in (0x1, 0x2):
                    return data
        except (ConnectionError, OSError):
            return None

    def close(self):
        try:
            self.send_frame(b"", 0x8)
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def app_build(www):
    try:
        with open(os.path.join(www, "index.html"), encoding="utf-8", errors="replace") as f:
            for line in f:
                if "const BUILD = '" in line:
                    return line.split("'")[1]
    except OSError:
        pass
    return "?"


def local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    # the mock's "flash storage": a temp copy of the built app, so uploads never touch the repo
    www = tempfile.mkdtemp(prefix="saab-dash-www-")
    for name in ("index.html", "manifest.json"):
        shutil.copy(os.path.join(REPO, name), www)
    shutil.copytree(os.path.join(REPO, "icons"), os.path.join(www, "icons"))
    dongle = Dongle(www)
    threading.Thread(target=dongle.stream, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(dongle))
    srv.daemon_threads = True
    print(f"Mock-dongel igång: http://localhost:{args.port}  (iPhone på samma Wi-Fi: http://{local_ip()}:{args.port})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
