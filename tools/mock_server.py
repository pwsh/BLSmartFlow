#!/usr/bin/env python3
"""
BLSmartFlow 2.0 — API mock server (stdlib only).

Implements every /api/* route of docs/REWORK-SPEC.md section 9 against an
in-memory config (section 5) plus a simulated printer and fan controller
(section 6), so src/www/index.html can be developed without hardware.

    python3 tools/mock_server.py [--port 8080] [--ap] [--offline] [--auth user:pass]

  --ap       simulate AP / provisioning mode (device.apMode = true, no STA)
  --offline  the printer never connects (temps/counters null, lastUpdateSec null,
             effectiveMode stale)
  --auth     require HTTP basic auth on every route (as web.authEnabled does)
"""

import argparse
import base64
import copy
import json
import math
import os
import queue
import random
import threading
import time
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INDEX = os.path.join(ROOT, "src", "www", "index.html")

CHIP_ID = "a1b2c3"
FW = "2.0.0"
BUILD = "2026-08-25 12:00:00"
MASK = "********"
SECRET_PATHS = (("wifi", "password"), ("printer", "accessCode"),
                ("mqtt", "password"), ("web", "password"))

# --------------------------------------------------------------------------
# configuration (spec section 5 defaults)
# --------------------------------------------------------------------------


def default_config():
    return {
        "version": 2,
        "wifi": {"ssid": "Holzhouse", "password": "hunter2hunter2", "bssid": "a4:2b:b0:11:22:33",
                 "lockBssid": False, "hostname": "blsmartflow"},
        "printer": {"ip": "10.0.1.42", "accessCode": "12345678", "serial": "01P00A000000001",
                    "model": "auto"},
        "fan": {
            "curve": [{"temp": 0, "speed": 0}, {"temp": 50, "speed": 0},
                      {"temp": 180, "speed": 50}, {"temp": 245, "speed": 80},
                      {"temp": 350, "speed": 100}],
            "source": "nozzle", "mode": "auto", "manualSpeed": 50, "minSpeed": 0,
            "kickStart": True, "kickMs": 500, "hysteresis": 2.0, "rampRate": 0,
            "pwmFreq": 25000, "pwmInvert": False, "output1": True, "output2": True,
            "onlyWhilePrinting": False, "cooldownMin": 10,
            "staleSec": 120, "staleMode": "off", "staleSpeed": 0,
        },
        "mqtt": {"enabled": False, "host": "", "port": 1883, "user": "", "password": "",
                 "baseTopic": "", "haDiscovery": True, "haPrefix": "homeassistant",
                 "publishIntervalSec": 10},
        "web": {"authEnabled": False, "user": "admin", "password": ""},
        "debug": {"serial": True, "mqttDump": False},
        "ssdp": {"enabled": True},
    }


def mask_config(cfg):
    out = copy.deepcopy(cfg)
    for sec, key in SECRET_PATHS:
        if out.get(sec, {}).get(key):
            out[sec][key] = MASK
    return out


def deep_merge(dst, src, path=()):
    """Merge partial config into dst. A value of only '*' keeps the old secret."""
    changed = []
    for k, v in src.items():
        p = path + (k,)
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            changed += deep_merge(dst[k], v, p)
        else:
            if isinstance(v, str) and v and set(v) == {"*"}:
                continue                       # masked secret -> leave unchanged
            if dst.get(k) != v:
                changed.append(".".join(p))
            dst[k] = v
    return changed


def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def validate_curve(points):
    """Returns (points, error). Mirrors curveValidate() in curve.h: temps are clamped
    to 0-400 C and speeds to 0-100 %, so only a malformed or wrongly sized list is a
    hard error - out-of-range values come back adjusted with a 200."""
    if not isinstance(points, list):
        return None, "points must be an array"
    if len(points) < 2:
        return None, "curve needs at least 2 points"
    if len(points) > 16:
        return None, "curve accepts at most 16 points"
    out = []
    for p in points:
        if not isinstance(p, dict) or "temp" not in p or "speed" not in p:
            return None, "each point needs temp and speed"
        if isinstance(p["temp"], bool) or isinstance(p["speed"], bool):
            return None, "temp and speed must be numbers"
        try:
            t = float(p["temp"])
            s = float(p["speed"])
        except (TypeError, ValueError):
            return None, "temp and speed must be numbers"
        out.append({"temp": round(clamp(t, 0.0, 400.0)),
                    "speed": int(round(clamp(s, 0.0, 100.0)))})
    out.sort(key=lambda p: p["temp"])
    dedup = []
    for p in out:                              # equal temps: keep the last one
        if dedup and dedup[-1]["temp"] == p["temp"]:
            dedup[-1] = p
        else:
            dedup.append(p)
    if len(dedup) < 2:
        return None, "curve needs at least 2 points with distinct temperatures"
    return dedup, None


def _enum(v, allowed, default):
    return v if isinstance(v, str) and v in allowed else default


def _clamp_num(d, key, lo, hi, cast):
    if key not in d:
        return
    try:
        d[key] = cast(clamp(cast(d[key]), lo, hi))
    except (TypeError, ValueError):
        pass


def validate_config(cfg):
    """Clamp and normalise a whole config in place, exactly like Config::validate()."""
    f = cfg.setdefault("fan", {})
    for k in ("manualSpeed", "minSpeed", "staleSpeed"):
        _clamp_num(f, k, 0, 100, int)
    _clamp_num(f, "kickMs", 0, 5000, int)
    _clamp_num(f, "hysteresis", 0.0, 50.0, float)
    _clamp_num(f, "rampRate", 0.0, 1000.0, float)
    _clamp_num(f, "pwmFreq", 500, 40000, int)
    _clamp_num(f, "cooldownMin", 0, 1440, int)
    _clamp_num(f, "staleSec", 10, 3600, int)
    f["source"] = _enum(f.get("source"), ("nozzle", "bed", "chamber", "max"), "nozzle")
    f["mode"] = _enum(f.get("mode"), ("auto", "manual", "off"), "auto")
    f["staleMode"] = _enum(f.get("staleMode"), ("hold", "off", "fixed"), "off")
    pts, _err = validate_curve(f.get("curve"))
    if pts:
        f["curve"] = pts
    else:                                      # unusable -> fall back to the default curve
        f["curve"] = default_config()["fan"]["curve"]

    pr = cfg.setdefault("printer", {})
    pr["model"] = _enum(pr.get("model"), ("auto", "x1", "p1", "a1", "h2d"), "auto")
    if isinstance(pr.get("serial"), str):
        pr["serial"] = pr["serial"].upper()

    w = cfg.setdefault("wifi", {})
    if "hostname" in w:
        h = re.sub(r"[^a-z0-9-]", "", str(w["hostname"]).lower())[:32]
        w["hostname"] = h or "blsmartflow"

    m = cfg.setdefault("mqtt", {})
    _clamp_num(m, "publishIntervalSec", 1, 3600, int)
    if isinstance(m.get("baseTopic"), str):
        m["baseTopic"] = m["baseTopic"].rstrip("/")
    return cfg


def obfuscate(v):
    """Legacy 1.x helper: everything but the last 3 characters becomes '*'."""
    v = str(v or "")
    return ("*" * (len(v) - 3) + v[-3:]) if len(v) > 3 else v


def interpolate(points, temp):
    if not points:
        return 0.0
    if temp <= points[0]["temp"]:
        return float(points[0]["speed"])
    if temp >= points[-1]["temp"]:
        return float(points[-1]["speed"])
    for a, b in zip(points, points[1:]):
        if a["temp"] <= temp <= b["temp"]:
            span = b["temp"] - a["temp"]
            if span <= 0:
                return float(b["speed"])
            f = (temp - a["temp"]) / span
            return a["speed"] + f * (b["speed"] - a["speed"])
    return float(points[-1]["speed"])


# --------------------------------------------------------------------------
# log ring buffer + SSE fan-out
# --------------------------------------------------------------------------

T_START = time.monotonic()


class Bus:
    def __init__(self):
        self.lock = threading.Lock()
        self.lines = []
        self.clients = []

    def log(self, level, msg):
        # same shape as the device: "[   1234] [E] message" (uptime in ms, level letter)
        line = "[%7d] [%s] %s" % (int((time.monotonic() - T_START) * 1000), level, msg)
        with self.lock:
            self.lines.append(line)
            del self.lines[:-64]
            clients = list(self.clients)
        print(line, flush=True)
        for q in clients:
            try:
                q.put_nowait(("log", line))
            except queue.Full:
                pass

    def push_status(self, status):
        with self.lock:
            clients = list(self.clients)
        for q in clients:
            try:
                q.put_nowait(("status", status))
            except queue.Full:
                pass

    def subscribe(self):
        q = queue.Queue(maxsize=64)
        with self.lock:
            self.clients.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)

    def snapshot(self):
        with self.lock:
            return list(self.lines)


BUS = Bus()

# --------------------------------------------------------------------------
# simulator: printer + fan controller
# --------------------------------------------------------------------------

STAGES = {-1: "idle", 0: "printing", 1: "auto bed levelling", 2: "heatbed preheating",
          8: "calibrating extrusion", 9: "scanning bed surface", 11: "calibrating motor noise",
          14: "cleaning nozzle tip", 20: "cooling chamber"}

SCENARIO = [("IDLE", 12), ("PREPARE", 18), ("RUNNING", 150), ("FINISH", 12)]


class Sim:
    def __init__(self, args):
        self.args = args
        self.lock = threading.RLock()
        self.cfg = default_config()
        self.t0 = time.monotonic()
        self.phase_i = 0
        self.phase_t = 0.0
        self.nozzle, self.bed, self.chamber = 24.0, 23.5, 24.0
        self.n_target = self.b_target = 0.0
        self.progress = 0.0
        self.layer = 0
        self.total_layers = 210
        self.last_update = time.monotonic()
        self.ever_updated = False              # no report has ever arrived yet
        self.printer_connected = not args.offline
        # fan controller state
        self.output = 0.0
        self.target = 0.0
        self.held_temp = None
        self.eff_mode = "stale"
        self.manual_expires = 0.0
        self.kick_until = 0.0
        self.print_end_at = 0.0
        self.last_tick = time.monotonic()
        # wifi scan state
        self.scan_started = 0.0
        self.scan_done_at = 0.0
        self.mqtt_ext_connected = False
        self.restart_count = 0

    # ---------------- printer ----------------
    def state_name(self):
        return SCENARIO[self.phase_i][0]

    def printing(self):
        return self.state_name() in ("RUNNING", "PAUSE", "PREPARE", "SLICING")

    def _approach(self, cur, tgt, rate, dt):
        if tgt is None:
            return cur
        d = tgt - cur
        step = rate * dt
        if abs(d) <= step:
            return tgt
        return cur + math.copysign(step, d)

    def tick_printer(self, dt):
        if self.args.offline or self.args.ap:   # no LAN in AP mode -> no printer link
            self.printer_connected = False
            return
        self.printer_connected = True
        self.last_update = time.monotonic()
        self.ever_updated = True
        self.phase_t += dt
        name, dur = SCENARIO[self.phase_i]
        if self.phase_t >= dur:
            self.phase_t = 0.0
            self.phase_i = (self.phase_i + 1) % len(SCENARIO)
            name = self.state_name()
            BUS.log("I", "printer state -> %s" % name)
            if name == "PREPARE":
                self.progress, self.layer = 0.0, 0
            if name == "FINISH":
                self.progress, self.layer = 100.0, self.total_layers
                self.print_end_at = time.monotonic()
        if name == "PREPARE":
            self.n_target, self.b_target = 220.0, 60.0
        elif name == "RUNNING":
            self.n_target, self.b_target = 220.0, 60.0
            frac = self.phase_t / dur
            self.progress = clamp(frac * 100.0, 0, 100)
            self.layer = int(self.progress / 100.0 * self.total_layers)
        else:
            self.n_target, self.b_target = 0.0, 0.0
        self.nozzle = self._approach(self.nozzle, self.n_target if self.n_target else 24.0,
                                     9.0 if self.n_target else 2.0, dt)
        self.nozzle += random.uniform(-0.25, 0.25)
        self.bed = self._approach(self.bed, self.b_target if self.b_target else 23.5,
                                  1.2 if self.b_target else 0.35, dt)
        self.bed += random.uniform(-0.08, 0.08)
        ctarget = 42.0 if name == "RUNNING" else 25.0
        self.chamber = self._approach(self.chamber, ctarget, 0.35, dt)

    def remaining_min(self):
        if self.state_name() != "RUNNING":
            return None
        dur = SCENARIO[2][1]
        return int(round((dur - self.phase_t) / dur * 96))

    def last_update_sec(self):
        """null until the very first report ever arrives."""
        if not self.ever_updated:
            return None
        return int(time.monotonic() - self.last_update)

    def counters(self):
        """stage/progress/remainingMin/layer/totalLayers are null while unknown."""
        unknown = {"stage": None, "stageText": "", "progress": None,
                   "remainingMin": None, "layer": None, "totalLayers": None}
        if not (self.printer_connected and self.ever_updated):
            return unknown
        name = self.state_name()
        stage = 0 if name == "RUNNING" else (2 if name == "PREPARE" else -1)
        if name == "IDLE":                     # nothing loaded -> no job counters
            return dict(unknown, stage=stage, stageText=STAGES.get(stage, "idle"))
        return {"stage": stage, "stageText": STAGES.get(stage, "idle"),
                "progress": int(self.progress), "remainingMin": self.remaining_min(),
                "layer": self.layer, "totalLayers": self.total_layers}

    def temps(self):
        if not self.printer_connected:
            return {"nozzle": None, "nozzleTarget": None, "bed": None,
                    "bedTarget": None, "chamber": None}
        return {"nozzle": round(self.nozzle, 1), "nozzleTarget": round(self.n_target),
                "bed": round(self.bed, 1), "bedTarget": round(self.b_target),
                "chamber": round(self.chamber, 1)}

    def source_temp(self):
        t = self.temps()
        if t["nozzle"] is None:
            return None
        src = self.cfg["fan"]["source"]
        if src == "bed":
            return t["bed"]
        if src == "chamber":
            return t["chamber"]
        if src == "max":
            return max(t["nozzle"], t["bed"], t["chamber"])
        return t["nozzle"]

    # ---------------- fan (spec section 6) ----------------
    def tick_fan(self, dt):
        f = self.cfg["fan"]
        now = time.monotonic()
        st = self.source_temp()
        stale_age = now - self.last_update
        if self.manual_expires and now >= self.manual_expires:
            self.manual_expires = 0.0
            f["mode"] = "auto"
            BUS.log("I", "manual override expired -> auto")

        if f["mode"] == "off":
            mode, tgt = "off", 0.0
        elif f["mode"] == "manual":
            mode, tgt = "manual", float(f["manualSpeed"])
        elif st is None or not self.printer_connected or stale_age > f["staleSec"]:
            mode = "stale"
            sm = f["staleMode"]
            tgt = self.output if sm == "hold" else (float(f["staleSpeed"]) if sm == "fixed" else 0.0)
        elif f["onlyWhilePrinting"] and not self.printing():
            cd_left = (self.print_end_at + f["cooldownMin"] * 60) - now
            if cd_left > 0:
                mode = "cooldown"
                tgt = self._curve_target(st, f)
            else:
                mode, tgt = "idle", 0.0
        else:
            mode, tgt = "auto", self._curve_target(st, f)

        self.eff_mode = mode
        self.target = clamp(tgt, 0, 100)
        out = self.target
        if f["rampRate"] > 0:
            step = f["rampRate"] * dt
            d = self.target - self.output
            out = self.target if abs(d) <= step else self.output + math.copysign(step, d)
        if f["kickStart"] and self.output <= 0 and out > 0:
            self.kick_until = now + f["kickMs"] / 1000.0
        self.output = clamp(out, 0, 100)
        eff = self.output
        if 0 < eff < f["minSpeed"]:
            eff = 0.0
        if now < self.kick_until and eff > 0:
            eff = 100.0
        self.effective_output = eff

    def _curve_target(self, st, f):
        if self.held_temp is None or abs(st - self.held_temp) >= f["hysteresis"]:
            self.held_temp = st
        return interpolate(f["curve"], self.held_temp)

    def pwm_duty(self):
        d = int(round(getattr(self, "effective_output", self.output) * 255 / 100.0))
        return 255 - d if self.cfg["fan"]["pwmInvert"] else d

    # ---------------- status object (spec section 9) ----------------
    def status(self):
        with self.lock:
            now = time.monotonic()
            ap = self.args.ap
            f = self.cfg["fan"]
            out = round(getattr(self, "effective_output", self.output))
            connected = not ap
            cnt = self.counters()
            return {
                "device": {"fw": FW, "uptimeSec": int(now - self.t0),
                           "heapFree": 148000 + random.randint(-3000, 3000),
                           "heapMin": 121344, "chipId": CHIP_ID,
                           "hostname": self.cfg["wifi"]["hostname"],
                           "ip": "192.168.4.1" if ap else "10.0.1.57", "apMode": ap},
                "wifi": {"connected": connected,
                         # empty while in AP mode / not associated
                         "ssid": self.cfg["wifi"]["ssid"] if connected else "",
                         "bssid": "A4:2B:B0:11:22:33" if connected else "",
                         "rssi": 0 if ap else -58 + random.randint(-6, 6),
                         "channel": 0 if ap else 6},
                "printer": {
                    "configured": bool(self.cfg["printer"]["ip"] and self.cfg["printer"]["serial"]),
                    "connected": self.printer_connected,
                    "online": self.printer_connected and self.ever_updated
                    and (now - self.last_update) < f["staleSec"],
                    "lastUpdateSec": self.last_update_sec(),
                    "mqttState": 0 if self.printer_connected else -2,
                    "mqttStateText": "connected" if self.printer_connected else "connect failed",
                    "state": self.state_name() if self.printer_connected else "UNKNOWN",
                    "printing": self.printer_connected and self.printing(),
                    "stage": cnt["stage"], "stageText": cnt["stageText"],
                    "progress": cnt["progress"], "remainingMin": cnt["remainingMin"],
                    "layer": cnt["layer"], "totalLayers": cnt["totalLayers"],
                    "task": "Benchy.3mf" if self.printer_connected else "",
                    "doorOpen": False, "printError": 0,
                    "wifiSignal": "-45dBm" if self.printer_connected else "",
                    "temps": self.temps(),
                    "fans": {"part": out, "aux": 0, "chamber": 40, "heatbreak": 100}
                    if self.printer_connected else
                    {"part": 0, "aux": 0, "chamber": 0, "heatbreak": 0},
                },
                "fan": {"output": out, "target": round(self.target), "mode": f["mode"],
                        "effectiveMode": self.eff_mode, "source": f["source"],
                        "sourceTemp": self.source_temp(), "manualSpeed": f["manualSpeed"],
                        "manualExpiresSec": max(0, int(self.manual_expires - now))
                        if self.manual_expires else 0,
                        "pwmDuty": self.pwm_duty(), "output1": f["output1"],
                        "output2": f["output2"]},
                "mqttExt": {"enabled": self.cfg["mqtt"]["enabled"],
                            "connected": self.cfg["mqtt"]["enabled"] and self.mqtt_ext_connected},
            }

    def run(self):
        last_push = 0.0
        while True:
            now = time.monotonic()
            dt = now - self.last_tick
            self.last_tick = now
            with self.lock:
                self.tick_printer(dt)
                self.tick_fan(dt)
                if self.cfg["mqtt"]["enabled"] and not self.mqtt_ext_connected:
                    self.mqtt_ext_connected = True
                    BUS.log("I", "external MQTT connected to %s:%d" %
                            (self.cfg["mqtt"]["host"] or "broker.local", self.cfg["mqtt"]["port"]))
                elif not self.cfg["mqtt"]["enabled"]:
                    self.mqtt_ext_connected = False
            if now - last_push >= 1.0:
                last_push = now
                BUS.push_status(self.status())
                if random.random() < 0.12:
                    BUS.log(random.choice(["I", "I", "I", "W"]), random.choice([
                        "mqtt: report received (%d bytes)" % random.randint(600, 3800),
                        "fan: output %d%% (mode %s)" % (round(self.output), self.eff_mode),
                        "wifi: rssi %d dBm" % (-58 + random.randint(-8, 8)),
                        "heap: free %d bytes" % (148000 + random.randint(-4000, 4000)),
                    ]))
            time.sleep(0.2)


# the ESP32 radio is 2.4 GHz only, so a scan never returns a 5 GHz channel
NETWORKS = [
    {"ssid": "Holzhouse", "bssid": "A4:2B:B0:11:22:33", "rssi": -47, "channel": 6, "secure": True},
    {"ssid": "FRITZ!Box 7590 QT", "bssid": "3C:A6:2F:AA:BB:CC", "rssi": -68, "channel": 11, "secure": True},
    {"ssid": "Printer-Farm", "bssid": "DE:AD:BE:EF:00:12", "rssi": -74, "channel": 1, "secure": True},
    {"ssid": "Gastnetz", "bssid": "DE:AD:BE:EF:00:13", "rssi": -79, "channel": 1, "secure": False},
    {"ssid": "Vodafone-Hotspot", "bssid": "00:11:22:33:44:55", "rssi": -88, "channel": 13, "secure": False},
]

# --------------------------------------------------------------------------
# HTTP layer
# --------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "BLSmartFlowMock/2.0"

    # ---- helpers ----
    def log_message(self, fmt, *a):            # quieter than the default
        pass

    def _auth_ok(self):
        want = self.server.args.auth
        if not want or self.server.args.ap:    # AP mode is always open (spec 9)
            return True
        hdr = self.headers.get("Authorization", "")
        if hdr.startswith("Basic "):
            try:
                got = base64.b64decode(hdr[6:]).decode("utf-8", "replace")
            except Exception:
                got = ""
            if got == want:
                return True
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="BLSmartFlow"')
        body = json.dumps({"error": "unauthorized"}).encode()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        return False

    def _send(self, obj, code=200, headers=None):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _err(self, msg, code=400):
        self._send({"error": msg}, code)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def _json_body(self):
        raw = self._body()
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8", "replace"))

    def _form(self):
        """Legacy routes are form-encoded; query-string args are accepted too."""
        raw = self._body().decode("utf-8", "replace")
        args = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
        args.update(parse_qs(raw, keep_blank_values=True))
        return {k: v[0] for k, v in args.items()}

    def _query(self):
        return {k: v[0] for k, v in
                parse_qs(urlsplit(self.path).query, keep_blank_values=True).items()}

    # ---- routing ----
    def do_GET(self):
        if not self._auth_ok():
            return
        p = self.path.split("?")[0]
        sim = self.server.sim
        try:
            if p in ("/", "/index.html"):
                return self._serve_index()
            if p == "/api/status":
                return self._send(sim.status())
            if p == "/api/config":
                return self._send(mask_config(sim.cfg))
            if p == "/api/curve":
                return self._send({"points": sim.cfg["fan"]["curve"],
                                   "source": sim.cfg["fan"]["source"]})
            if p == "/api/log":
                return self._send({"lines": BUS.snapshot()})
            if p == "/api/info":
                return self._send({
                    "fw": FW, "build": BUILD, "chipId": CHIP_ID, "sdk": "v5.5.1-esp32",
                    "flashSize": 4194304, "sketchSize": 1123456,
                    "freeSketchSpace": 1966080, "partition": "app0",
                    "resetReason": "POWERON"})
            if p == "/api/wifi/scan":
                return self._wifi_scan()
            if p == "/api/backup":
                body = json.dumps(sim.cfg, indent=2).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Disposition",
                                 'attachment; filename="blsmartflow-%s.json"' % CHIP_ID)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                return self.wfile.write(body)
            if p == "/api/events":
                return self._sse()
            if p == "/sensorData":            # legacy 1.x
                st = sim.source_temp()
                return self._send({"temp": round(float(st), 2) if st is not None else 0,
                                   "speed": int(round(getattr(sim, "effective_output",
                                                              sim.output)))})
            if p == "/getFanConfig":          # legacy 1.x
                return self._send({"points": sim.cfg["fan"]["curve"],
                                   "source": sim.cfg["fan"]["source"]})
            if p == "/getOptions":            # legacy 1.x
                c, f = sim.cfg, sim.cfg["fan"]
                return self._send({
                    "firmwareversion": FW,
                    "ip": c["printer"]["ip"],
                    "code": obfuscate(c["printer"]["accessCode"]),
                    "id": obfuscate(c["printer"]["serial"]),
                    "staticfans": f["mode"] == "manual",
                    "staticfanspeed": f["manualSpeed"],
                    "debuging": c["debug"]["serial"],
                    "debugingchange": False,   # 1.x-only flag, no 2.0 equivalent
                    "mqttdebug": c["debug"]["mqttDump"]})
            return self._err("not found: %s" % p, 404)
        except Exception as e:                # never take the mock down
            BUS.log("E", "GET %s failed: %s" % (p, e))
            return self._err(str(e), 500)

    def do_POST(self):
        if not self._auth_ok():
            return
        p = self.path.split("?")[0]
        sim = self.server.sim
        try:
            if p == "/api/config":
                return self._post_config()
            if p == "/api/fan":
                return self._post_fan()
            if p == "/api/wifi":
                return self._post_wifi()
            if p == "/api/restart":
                BUS.log("W", "restart requested (mock: not restarting)")
                return self._send({"ok": True})
            if p == "/api/factoryreset":
                b = self._json_body()
                if not b.get("confirm"):
                    return self._err("confirm:true required")
                BUS.log("W", "factory reset requested (mock: config reset to defaults)")
                with sim.lock:
                    sim.cfg = default_config()
                return self._send({"ok": True})
            if p == "/api/restore":
                b = self._json_body()
                if not isinstance(b, dict):
                    return self._err("body must be an object")
                with sim.lock:
                    cfg = default_config()     # a restore replaces the whole config
                    deep_merge(cfg, b)
                    validate_config(cfg)
                    sim.cfg = cfg
                    sim.held_temp = None
                    sim.restart_count += 1
                BUS.log("I", "config restored from backup, saved")
                BUS.log("W", "restarting now (mock: not restarting)")
                return self._send({"ok": True})
            if p in ("/api/update", "/update"):
                return self._ota()
            if p == "/submitOptions":         # legacy 1.x form route
                return self._submit_options()
            if p == "/updateFanConfig":       # legacy 1.x form route
                return self._update_fan_config()
            return self._err("not found: %s" % p, 404)
        except json.JSONDecodeError as e:
            return self._err("invalid JSON: %s" % e)
        except Exception as e:
            BUS.log("E", "POST %s failed: %s" % (p, e))
            return self._err(str(e), 500)

    def do_PUT(self):
        if not self._auth_ok():
            return
        p = self.path.split("?")[0]
        sim = self.server.sim
        try:
            if p == "/api/curve":
                b = self._json_body()
                pts, err = validate_curve(b.get("points"))
                if err:
                    return self._err(err)
                with sim.lock:
                    sim.cfg["fan"]["curve"] = pts
                    sim.held_temp = None
                BUS.log("I", "fan curve saved (%d points)" % len(pts))
                return self._send({"ok": True, "points": pts})
            return self._err("not found: %s" % p, 404)
        except json.JSONDecodeError as e:
            return self._err("invalid JSON: %s" % e)
        except Exception as e:
            return self._err(str(e), 500)

    # ---- route bodies ----
    def _serve_index(self):
        try:
            with open(INDEX, "rb") as fh:
                body = fh.read()
        except OSError:
            body = b"<h1>src/www/index.html missing</h1>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _post_config(self):
        sim = self.server.sim
        b = self._json_body()
        if not isinstance(b, dict):
            return self._err("body must be an object")
        if isinstance(b.get("printer"), dict) and "accessCode" in b["printer"]:
            ac = b["printer"]["accessCode"]
            masked = isinstance(ac, str) and ac and set(ac) == {"*"}
            if not masked and len(str(ac)) != 8:
                return self._err("access code must be exactly 8 characters")
        if isinstance(b.get("fan"), dict) and "curve" in b["fan"]:
            pts, err = validate_curve(b["fan"]["curve"])
            if err:
                return self._err(err)
            b["fan"]["curve"] = pts
        with sim.lock:
            changed = deep_merge(sim.cfg, b)
            validate_config(sim.cfg)
            sim.held_temp = None
            # web.* applies to the very next request, so only wifi.* needs a restart
            restart = any(c.startswith("wifi.") for c in changed)
            cfg = mask_config(sim.cfg)
        BUS.log("I", "config saved: %s" % (", ".join(changed) or "no changes"))
        return self._send({"ok": True, "restartRequired": restart, "config": cfg})

    def _post_fan(self):
        sim = self.server.sim
        b = self._json_body()
        mode = b.get("mode")
        if mode not in (None, "auto", "manual", "off"):
            return self._err("mode must be auto, manual or off")
        speed = b.get("speed")
        dur = int(b.get("durationSec") or 0)
        with sim.lock:
            f = sim.cfg["fan"]
            if speed is not None:
                try:
                    speed = float(speed)
                except (TypeError, ValueError):
                    return self._err("speed must be 0..100")
                if isinstance(b.get("speed"), bool) or not (0 <= speed <= 100):
                    return self._err("speed must be 0..100")   # no clamping
                f["manualSpeed"] = int(round(speed))
            if mode:
                f["mode"] = mode
            sim.manual_expires = (time.monotonic() + dur) if (mode == "manual" and dur > 0) else 0.0
            sim.tick_fan(0.0)
            st = sim.status()["fan"]
        BUS.log("I", "fan: mode=%s speed=%s duration=%ds" % (f["mode"], f["manualSpeed"], dur))
        return self._send({"ok": True, "fan": st})

    def _post_wifi(self):
        sim = self.server.sim
        b = self._json_body()
        if not b.get("ssid"):
            return self._err("ssid must not be empty")
        with sim.lock:
            deep_merge(sim.cfg["wifi"], {k: v for k, v in b.items()
                                         if k in ("ssid", "password", "bssid",
                                                  "lockBssid", "hostname")})
            validate_config(sim.cfg)
            sim.restart_count += 1
        delay = 1.0 if self.server.args.ap else 1.5
        BUS.log("I", "wifi credentials saved for '%s'" % b["ssid"])
        BUS.log("W", "restarting in %.1f s (mock: not restarting)" % delay)
        return self._send({"ok": True, "restartRequired": True})

    SCAN_CACHE_SEC = 20.0                      # a cached result older than this is dropped
    SCAN_DURATION_SEC = 2.5                    # the radio needs ~3 s for a full 2.4 GHz sweep

    def _wifi_scan(self):
        sim = self.server.sim
        now = time.monotonic()
        force = self._query().get("force", "0") not in ("0", "", "false", "no")
        with sim.lock:
            if force:                          # explicit refresh: drop the cache, scan again
                sim.scan_done_at = 0.0
                sim.scan_started = now
                BUS.log("I", "wifi: forced scan started")
                return self._send({"scanning": True}, 202)
            if sim.scan_done_at and now - sim.scan_done_at < self.SCAN_CACHE_SEC:
                nets = sorted(NETWORKS, key=lambda n: -n["rssi"])
                nets = [dict(n, rssi=n["rssi"] + random.randint(-3, 3)) for n in nets]
                return self._send({"networks": nets})
            if not sim.scan_started or now - sim.scan_started > 10.0:
                sim.scan_started = now
                BUS.log("I", "wifi: async scan started")
            if now - sim.scan_started < self.SCAN_DURATION_SEC:
                return self._send({"scanning": True}, 202)
            sim.scan_done_at = now
            sim.scan_started = 0.0
            BUS.log("I", "wifi: scan finished, %d networks" % len(NETWORKS))
            nets = sorted(NETWORKS, key=lambda n: -n["rssi"])
            return self._send({"networks": nets})

    def _submit_options(self):
        """Legacy 1.x form: ip, code, serial, staticfan, staticfanspeed, debuging, mqttdebug.
        Values that still contain a '*' come from the obfuscated /getOptions payload and
        are left untouched, exactly like the 1.x handler."""
        sim = self.server.sim
        a = self._form()
        with sim.lock:
            pr, f, dbg = sim.cfg["printer"], sim.cfg["fan"], sim.cfg["debug"]
            if "ip" in a and "*" not in a["ip"]:
                pr["ip"] = a["ip"]
            if "code" in a and "*" not in a["code"]:
                pr["accessCode"] = a["code"]
            if "serial" in a and "*" not in a["serial"]:
                pr["serial"] = a["serial"].upper()
            if "staticfan" in a:
                f["mode"] = "manual" if a["staticfan"] == "on" else "auto"
            if "staticfanspeed" in a:
                try:
                    f["manualSpeed"] = int(float(a["staticfanspeed"]))
                except ValueError:
                    pass
            if "debuging" in a:
                dbg["serial"] = a["debuging"] == "on"
            if "mqttdebug" in a:
                dbg["mqttDump"] = a["mqttdebug"] == "on"
            validate_config(sim.cfg)
        BUS.log("I", "legacy /submitOptions applied: %s" % ", ".join(sorted(a)))
        return self._send({"ok": True})

    def _update_fan_config(self):
        """Legacy 1.x form: points = {"points":[...]} or a bare [...] array."""
        sim = self.server.sim
        raw = self._form().get("points")
        if raw is None:
            return self._err("missing 'points' form field")
        try:
            doc = json.loads(raw)
        except json.JSONDecodeError as e:
            return self._err("invalid JSON in 'points': %s" % e)
        pts = doc.get("points") if isinstance(doc, dict) else doc
        pts, err = validate_curve(pts)
        if err:
            return self._err(err)
        with sim.lock:
            sim.cfg["fan"]["curve"] = pts
            sim.held_temp = None
        BUS.log("I", "legacy /updateFanConfig saved %d points" % len(pts))
        return self._send({"ok": True, "points": pts})

    def _ota(self):
        n = int(self.headers.get("Content-Length") or 0)
        ctype = self.headers.get("Content-Type", "")
        if "multipart/form-data" not in ctype:
            self._body()
            return self._err("expected a multipart/form-data upload")
        read = 0
        head = b""
        while read < n:                        # stream it away like the ESP does
            chunk = self.rfile.read(min(65536, n - read))
            if not chunk:
                break
            if len(head) < 4096:
                head += chunk[:4096]
            read += len(chunk)
        if b"filename=" not in head:            # any field name is accepted
            return self._err("no file in the upload")
        if read < 1024:
            return self._err("firmware image too small (%d bytes)" % read)
        BUS.log("I", "OTA: received %d bytes, flashing (mock)" % read)
        return self._send({"ok": True})

    def _sse(self):
        sim = self.server.sim
        q = BUS.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        try:
            self._sse_send("status", json.dumps(sim.status()))
            for line in BUS.snapshot()[-10:]:
                self._sse_send("log", line)
            while True:
                try:
                    kind, payload = q.get(timeout=5)
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
                    continue
                self._sse_send(kind, payload if isinstance(payload, str) else json.dumps(payload))
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            BUS.unsubscribe(q)
        self.close_connection = True

    def _sse_send(self, event, data):
        out = "event: %s\ndata: %s\n\n" % (event, data.replace("\n", "\ndata: "))
        self.wfile.write(out.encode())
        self.wfile.flush()


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description="BLSmartFlow 2.0 API mock")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--ap", action="store_true", help="simulate AP / provisioning mode")
    ap.add_argument("--offline", action="store_true", help="printer never connects")
    ap.add_argument("--auth", default="", metavar="USER:PASS", help="require basic auth")
    args = ap.parse_args()

    sim = Sim(args)
    if args.auth:
        sim.cfg["web"]["authEnabled"] = True
        sim.cfg["web"]["user"] = args.auth.split(":")[0]
        sim.cfg["web"]["password"] = args.auth.split(":", 1)[1]
    try:
        srv = Server((args.host, args.port), Handler)
    except OSError as e:
        raise SystemExit("cannot bind port %d: %s (try --port 8081)" % (args.port, e))
    srv.sim, srv.args = sim, args
    threading.Thread(target=sim.run, daemon=True).start()
    modes = [m for m, on in (("AP", args.ap), ("offline-printer", args.offline),
                             ("auth", bool(args.auth))) if on] or ["normal"]
    print("BLSmartFlow mock API on http://localhost:%d/  (%s)  serving %s  — Ctrl-C to stop"
          % (args.port, ", ".join(modes), os.path.relpath(INDEX, os.getcwd())), flush=True)
    BUS.log("I", "mock server started")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye", flush=True)


if __name__ == "__main__":
    main()
