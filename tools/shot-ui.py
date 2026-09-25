#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Screenshot the web UI with SYNTHETIC data, for the documentation.

Never point a screenshot at a real board. The status page prints the address it
is on, the network it joined and the UPS's identity, and all three would end up
committed and published. This serves the real index.html against a canned
/api/status instead, so the picture is of the actual page and the data in it
belongs to nobody.

Addresses come from 192.0.2.0/24, which RFC 5737 reserves for documentation --
the same range tools/publish.sh allows and every other private range it
refuses.

    tools/shot-ui.py            writes docs/img/ui-*.png
"""
import http.server, json, os, socketserver, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WWW  = os.path.join(ROOT, "firmware/components/webui/www")
OUT  = os.path.join(ROOT, "docs/img")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
PORT = 8731

# A healthy UPS on mains, mid-charge history, an update available so the
# firmware card has something to show. Nothing here is a real device.
STATUS = {
    "board": "ups-adaptor Rev A",
    "image": "ups-adaptor-reva",
    "version": "v0.05",
    "uptime_s": 98430,
    "last_reset": "power-on",
    "crashes": 0,
    "heap_free": 221040, "heap_low": 210472,
    "wifi": {"state": 3, "ip": "192.0.2.42", "ap": "", "ssid": "HomeNet", "rssi": -52,
             "host": "ups-adaptor-2A1F", "join": 0, "join_ip": "", "join_err": ""},
    "nut": {"port": 3493, "name": "ups", "clients": 2},
    "ups": {
        "attached": True, "mfr": "CPS", "model": "EC850LCD",
        "vid": "0764", "pid": "0501", "status": 1,
        "battery_charge": 100.0, "battery_runtime": 2050.0,
        "battery_voltage": None, "input_voltage": 120.0, "output_voltage": 120.0,
        "load": 19.0, "input_frequency": None,
        "realpower": None, "realpower_nom": 510.0, "apparentpower": None,
        "lowbatt_limit": 10.0, "battery_mfr_date": None,
        "caps": 15, "beeper": 2, "test_result": 1,
    },
    "vbus_fault": False,
    "ota": {"state": 2, "latest": "v0.06", "progress": 0, "pending": False, "dev": False,
            "auto": True, "checked": True, "error": "", "refused": ""},
}

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=WWW, **kw)
    def log_message(self, *a):
        pass
    def do_GET(self):
        if self.path.startswith("/api/status"):
            body = json.dumps(STATUS).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith("/api/"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok":true}')
            return
        if self.path == "/":
            self.path = "/index.html"
        return super().do_GET()

def shot(name, width, height, extra=()):
    path = os.path.join(OUT, name)
    cmd = [CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
           *( ["--force-device-scale-factor=2"] if os.environ.get("SHOT_2X","1")=="1" else [] ),
                      "--virtual-time-budget=4000",
           "--window-size=%d,%d" % (width, height),
           "--screenshot=" + path,
           *extra,
           "http://127.0.0.1:%d/" % PORT]
    subprocess.run(cmd, capture_output=True)
    ok = os.path.exists(path)
    print("   %-22s %s" % (name, "%d bytes" % os.path.getsize(path) if ok else "FAILED"))
    return ok

def main():
    if not os.path.exists(CHROME):
        print("Chrome not found at", CHROME); return 1
    os.makedirs(OUT, exist_ok=True)
    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.TCPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    time.sleep(0.4)
    print("== serving %s with a canned /api/status on :%d" % (WWW, PORT))
    # Sizes may be overridden for diagnosis: shot-ui.py NAME WIDTH HEIGHT
    ok = True
    if len(sys.argv) == 4:
        ok &= shot(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
    else:
        ok &= shot("ui-desktop.png", 1100, 1500)
        # No phone shot yet: at a 430px viewport the page overflows to the
        # right and the value column is cut off, at scale factor 1 and 2 alike.
        # Reproduce with:  tools/shot-ui.py probe.png 430 1200
    srv.shutdown()
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
