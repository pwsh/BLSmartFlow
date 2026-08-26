#!/usr/bin/env python3
"""
BLSmartFlow 2.0 — API mock server (stdlib only).

Implements every /api/* route of docs/REWORK-SPEC.md section 9 against an
in-memory config (section 5) plus a simulated printer and fan controller
(section 6), so src/www/index.html can be developed without hardware. That
includes the post-print cool-down session of section 17: sessions run against the
same chamber model, and every M106 the firmware would publish is logged as
"printer <- M106 ...".

    python3 tools/mock_server.py [--port 8080] [--ap] [--offline] [--auth user:pass] [--door]

  --ap       simulate AP / provisioning mode (device.apMode = true, no STA)
  --offline  the printer never connects (temps/counters null, lastUpdateSec null,
             effectiveMode stale)
  --auth     require HTTP basic auth on every route (as web.authEnabled does)
  --door     start with the door reported open (toggle later with
             POST /mock/door {"open":true|false} or {"toggle":true}); note that
             doorKnown stays false until the first toggle, exactly as on hardware
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
FILAMENT_DB_H = os.path.join(ROOT, "src", "blflow", "filament_db.h")
AMS_FIXTURE = os.path.join(ROOT, "test", "fixtures", "x1c_ams_trays.json")

CHIP_ID = "a1b2c3"
FW = "2.0.3"
BUILD = "2026-08-26 12:00:00"
MASK = "********"
SECRET_PATHS = (("wifi", "password"), ("printer", "accessCode"),
                ("mqtt", "password"), ("web", "password"))

# --------------------------------------------------------------------------
# filament (spec section 16)
# --------------------------------------------------------------------------
# The mock reads the *generated header* rather than carrying its own copy of the
# Filament Field Guide: one source of truth means the UI sees exactly the table
# the firmware serves, and a regenerated database needs no change here.

FIL_RE = re.compile(
    r'^\s*\{"([^"]*)",\s*"([^"]*)",\s*(FCLASS_\w+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),'
    r'\s*(\d+),\s*(\d+),\s*0x([0-9A-Fa-f]+),\s*(\d+),\s*(\d+)\},\s*$')
BAMBU_RE = re.compile(r'^\s*\{"([A-Z0-9]+)",\s*"([^"]*)"\},\s*$')

VENT_NAMES = ("optional", "recommended", "required")
LEVEL_NAMES = ("none", "low", "moderate", "high")
FIL_ENCLOSURE, FIL_HEATED, FIL_OPEN_COOL, FIL_HARDENED = 1, 2, 4, 8
TEMP_NA, PARTCOOL_NA = -1, 255


def load_filament_db(path=FILAMENT_DB_H):
    """Parses src/blflow/filament_db.h back into dicts."""
    fils, bambu = [], {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                m = FIL_RE.match(line)
                if m:
                    fils.append({
                        "id": m.group(1), "name": m.group(2),
                        "cls": m.group(3)[len("FCLASS_"):].lower().replace("_", "-"),
                        "cMin": int(m.group(4)), "cRec": int(m.group(5)), "cMax": int(m.group(6)),
                        "cool": int(m.group(7)), "vent": int(m.group(8)),
                        "flags": int(m.group(9), 16),
                        "voc": int(m.group(10)), "part": int(m.group(11))})
                    continue
                m = BAMBU_RE.match(line)
                if m:
                    bambu[m.group(1)] = m.group(2)
    except OSError:
        pass
    return fils, bambu


FILAMENTS, BAMBU_IDS = load_filament_db()
BY_ID = {f["id"]: f for f in FILAMENTS}

# Mirrors fmdetail::baseMap() in filament_match.h.
BASE_MAP = {
    "PLA": "pla", "PETG": "petg", "PCTG": "pctg", "ABS": "abs", "ASA": "asa", "PC": "pc",
    "PA": "pa", "PAHT": "pa", "PA6": "pa6", "PA12": "pa12", "PA66": "pa66", "PPA": "ppa",
    "TPU": "tpu", "TPE": "tpe", "PVA": "pva", "BVOH": "bvoh", "HIPS": "hips", "PET": "pet",
    "PPS": "pps", "PP": "pp", "PE": "pe", "EVA": "eva", "PHA": "pha", "PMMA": "pmma",
    "PVB": "pvb", "PBT": "pbt", "PPSU": "ppsu", "PEEK": "peek", "PEKK": "pekk",
    "PEI": "pei-ultem",
}
PREFIX_MAP = [("GFA", "PLA"), ("GFL", "PLA"), ("GFB", "ABS"), ("GFC", "PC"), ("GFG", "PETG"),
              ("GFN", "PA"), ("GFP", "PP"), ("GFT", "PPS"), ("GFU", "TPU")]


def _norm(s):
    return " ".join(str(s or "").replace("_", " ").upper().split())


def _support_pair(t):
    if "PLA" in t:
        return "pla"
    if "PA/PET" in t or "PET" in t:
        return "pa"
    if "ABS" in t or "ASA" in t:
        return "abs"
    if "SUPPORT W" in t:
        return "pla"
    if "SUPPORT G" in t:
        return "pa"
    if "PA" in t:
        return "pa"
    return ""


def _identify_type(t):
    """(id, family) for one normalised type string; id is '' when unmatched."""
    if not t:
        return "", ""
    if t.startswith("SUPPORT"):
        paired = _support_pair(t)
        return (paired if paired in BY_ID else ""), t
    base, _, mod = t.partition("-")
    base_id = BASE_MAP.get(base)
    if not base_id:
        return "", t
    if mod in ("CF", "GF"):
        variant = "%s-%s" % (base_id, mod.lower())
        if variant in BY_ID:
            return variant, t
    return (base_id if base_id in BY_ID else ""), t


def identify(tray_type, sub_brands, tray_idx):
    """Mirrors filamentIdentify() in filament_match.h (spec 16.2)."""
    t = _norm(tray_type)
    fid, family = _identify_type(t)
    if fid:
        return fid, family
    fallback = (fid, family)
    by_idx = BAMBU_IDS.get(_norm(tray_idx), "")
    if by_idx:
        fid, family = _identify_type(_norm(by_idx))
        if fid:
            return fid, family
        if not fallback[1]:
            fallback = (fid, family)
    sb = _norm(sub_brands).split(" ")[0]
    if sb:
        fid, family = _identify_type(sb)
        if fid:
            return fid, family
    idx = _norm(tray_idx)
    for prefix, typ in PREFIX_MAP:
        if idx.startswith(prefix):
            fid, family = _identify_type(typ)
            if fid:
                return fid, family
            break
    return fallback


def active_tray(tray_now):
    """spec 16.2 step 5: tray_now -> (source, ams, slot)."""
    if tray_now is None or tray_now < 0 or tray_now == 255:
        return "none", -1, -1
    if tray_now == 254:
        return "external", -1, 254
    if tray_now >= 128:
        return "ams", tray_now, 0
    return "ams", tray_now // 4, tray_now % 4


def effective_profile(info, fil_cfg, fan_cfg):
    """Mirrors filamentEffective() (spec 16.3)."""
    e = {"chamberTarget": int(fan_cfg["chamberTarget"]),
         "cooldownTarget": int(fan_cfg["cooldownTarget"]),
         "ventFloor": 0, "postPrintCooling": "fast", "overridden": False}
    keep_cool = False
    if not fil_cfg.get("auto", True):
        return e, keep_cool
    if info:
        keep_cool = bool(info["flags"] & FIL_OPEN_COOL) or (
            info["cRec"] != TEMP_NA and info["cRec"] < 35)
        target = info["cMax"] if keep_cool else info["cRec"]
        if target == TEMP_NA:
            target = info["cRec"] if keep_cool else info["cMax"]
        if target != TEMP_NA:
            e["chamberTarget"] = int(clamp(target, 20, 80))
        if info["cool"] != PARTCOOL_NA and info["cool"] < 50:
            e["postPrintCooling"] = "gentle"
        vf = fil_cfg.get("ventFloor", {})
        e["ventFloor"] = int(vf.get(VENT_NAMES[info["vent"]], 0))
    for star in (True, False):
        for r in fil_cfg.get("overrides", []):
            rid = r.get("id") or ""
            if (rid == "*") != star:
                continue
            if not star and (not info or rid != info["id"]):
                continue
            if r.get("chamberTarget") is not None:
                e["chamberTarget"] = int(clamp(r["chamberTarget"], 20, 80))
                e["overridden"] = True
            if r.get("cooldownTarget") is not None:
                e["cooldownTarget"] = int(clamp(r["cooldownTarget"], 15, 60))
                e["overridden"] = True
            if r.get("ventFloor") is not None:
                e["ventFloor"] = int(clamp(r["ventFloor"], 0, 100))
                e["overridden"] = True
            if r.get("postPrintCooling") in ("fast", "gentle"):
                e["postPrintCooling"] = r["postPrintCooling"]
                e["overridden"] = True
    return e, keep_cool


def load_ams_fixture():
    """The fake AMS is the real X1C capture in test/fixtures/x1c_ams_trays.json."""
    try:
        with open(AMS_FIXTURE, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError):
        return {}, None, 0
    pr = doc.get("print", {})
    ams = pr.get("ams", {})
    trays = {}
    for unit in ams.get("ams", []):
        uid = int(unit.get("id", 0))
        for tr in unit.get("tray", []):
            trays[(uid, int(tr.get("id", 0)))] = {
                "type": tr.get("tray_type", ""), "subBrand": tr.get("tray_sub_brands", ""),
                "idx": tr.get("tray_info_idx", ""), "color": tr.get("tray_color", "")}
    vt = pr.get("vt_tray")
    external = None
    if vt:
        external = {"type": vt.get("tray_type", ""), "subBrand": vt.get("tray_sub_brands", ""),
                    "idx": vt.get("tray_info_idx", ""), "color": vt.get("tray_color", "")}
    try:
        now = int(ams.get("tray_now", "255"))
    except ValueError:
        now = 255
    return trays, external, now


# --------------------------------------------------------------------------
# configuration (spec section 5 defaults)
# --------------------------------------------------------------------------


def default_config():
    return {
        "version": 2,
        "wifi": {"ssid": "Workshop-WiFi", "password": "correct-horse-battery", "bssid": "02:00:5e:10:20:30",
                 "lockBssid": False, "hostname": "blsmartflow"},
        "printer": {"ip": "192.168.1.42", "accessCode": "12345678", "serial": "01P00A000000001",
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
            # thermal states (spec 15.2)
            "doorMode": "ignore", "doorSpeed": 0, "doorResumeSec": 5,
            "preheatMode": "off", "preheatSpeed": 0,
            "chamberTarget": 45, "cooldownTarget": 35,
            "kp": 8.0, "ki": 0.02, "thermostatPeriodSec": 5, "ambientTemp": 25,
        },
        "mqtt": {"enabled": False, "host": "", "port": 1883, "user": "", "password": "",
                 "baseTopic": "", "haDiscovery": True, "haPrefix": "homeassistant",
                 "publishIntervalSec": 10},
        "web": {"authEnabled": False, "user": "admin", "password": ""},
        "debug": {"serial": True, "mqttDump": False},
        "ssdp": {"enabled": True},
        # filament-aware cooling (spec 16.3)
        "filament": {"auto": True, "manualId": "",
                     "ventFloor": {"optional": 0, "recommended": 0, "required": 10},
                     "overrides": []},
        # post-print cool-down (spec 17.1): the session is on, borrowing the
        # printer's own fans is opt-in because it is the one thing here that
        # sends commands to the printer.
        "cooldown": {"enabled": True, "target": 35, "usePrinterFans": False,
                     "auxSpeed": 100, "chamberFanSpeed": 100, "maxMinutes": 30,
                     "gentleFromFilament": True, "ownFan": "thermostat"},
        # learned cooling constants: 5 closed buckets then 5 open ones, null = unmeasured
        "thermal": {"k": [None] * 10, "samples": 0},
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
    f["mode"] = _enum(f.get("mode"), ("auto", "manual", "off", "chamber"), "auto")
    f["staleMode"] = _enum(f.get("staleMode"), ("hold", "off", "fixed"), "off")
    # thermal states (spec 15.2)
    for k in ("doorSpeed", "preheatSpeed"):
        _clamp_num(f, k, 0, 100, int)
    _clamp_num(f, "doorResumeSec", 0, 300, int)
    _clamp_num(f, "chamberTarget", 20, 80, int)
    _clamp_num(f, "cooldownTarget", 15, 60, int)
    _clamp_num(f, "kp", 0.0, 50.0, float)
    _clamp_num(f, "ki", 0.0, 1.0, float)
    _clamp_num(f, "thermostatPeriodSec", 1, 60, int)
    _clamp_num(f, "ambientTemp", 0, 40, int)
    f["doorMode"] = _enum(f.get("doorMode"), ("ignore", "off", "fixed"), "ignore")
    f["preheatMode"] = _enum(f.get("preheatMode"), ("ignore", "off", "fixed"), "off")
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

    fil = cfg.setdefault("filament", {})
    fil.setdefault("auto", True)
    fil.setdefault("manualId", "")
    if fil["manualId"] and fil["manualId"] not in BY_ID:
        fil["manualId"] = ""
    vf = fil.setdefault("ventFloor", {})
    for k in VENT_NAMES:
        vf[k] = int(clamp(int(vf.get(k, 0) or 0), 0, 100))
    ov = []
    for r in (fil.get("overrides") or [])[:12]:
        if not isinstance(r, dict) or not r.get("id"):
            continue
        ov.append({"id": str(r["id"])[:23],
                   "chamberTarget": None if r.get("chamberTarget") is None
                   else int(clamp(int(r["chamberTarget"]), 20, 80)),
                   "cooldownTarget": None if r.get("cooldownTarget") is None
                   else int(clamp(int(r["cooldownTarget"]), 15, 60)),
                   "ventFloor": None if r.get("ventFloor") is None
                   else int(clamp(int(r["ventFloor"]), 0, 100)),
                   "postPrintCooling": r.get("postPrintCooling")
                   if r.get("postPrintCooling") in ("fast", "gentle") else None})
    fil["overrides"] = ov

    cd = cfg.setdefault("cooldown", {})
    _clamp_num(cd, "target", 15, 60, int)
    _clamp_num(cd, "auxSpeed", 0, 100, int)
    _clamp_num(cd, "chamberFanSpeed", 0, 100, int)
    _clamp_num(cd, "maxMinutes", 1, 240, int)
    cd["ownFan"] = _enum(cd.get("ownFan"), ("thermostat", "max", "curve"), "thermostat")

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

# A trimmed stg_cur table; the firmware carries the full 0..77 list. Only the
# codes this simulation actually walks through are needed here.
STAGES = {-2: "offline", -1: "idle", 0: "printing", 1: "auto_bed_leveling",
          2: "heatbed_preheating", 7: "heating_hotend", 8: "calibrating_extrusion",
          9: "scanning_bed_surface", 14: "cleaning_nozzle_tip", 29: "cooling_chamber",
          49: "heating_chamber", 255: "idle"}

PAUSE_STAGES = {5, 6, 16, 17, 20, 21, 23, 26, 27, 28, 30, 32, 33, 34, 35}
PREHEAT_STAGES = {2, 7, 49, 54, 58, 63, 64}
COOLING_STAGES = {29, 50, 69}

# (gcode_state, seconds, stg_cur) - one loop of the fake print. The phase the UI
# shows is *derived* from these plus the temperatures, exactly as the firmware
# derives it, so the mock exercises the same rules.
SCENARIO = [("IDLE", 60, -1), ("PREPARE", 18, 2), ("RUNNING", 150, 0),
            ("FINISH", 90, 29)]

# Chamber model (spec 15.6): dT/dt = heatIn - k*(T - ambient), k in 1/min.
# The fan and an open door both raise k; the heated bed is the only heat source.
# Tuned so the fake print actually walks through the phases: the chamber crosses
# its target about half a minute in, settles near 50 C with the fan off, and can
# be pulled back to the low forties by the thermostat.
K_BASE, K_FAN, K_DOOR = 0.25, 0.55, 0.60
# The printer's own aux and chamber fans (spec 17): they sit inside the enclosure
# and move considerably more air through it than an external duct fan, which is
# the whole reason for borrowing them.
K_PRINTER_FAN = 0.90
CHAMBER_HEAT_C_PER_SEC = 0.20
COOLDOWN_SAMPLE_SEC = 5.0
COOLDOWN_REASSERT_SEC = 30.0
COOLDOWN_LINK_GRACE_SEC = 30.0


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
        self.c_target = 0.0                    # printer's own chamber set point (0 = none)
        # door (home_flag bit 23 - the front-door plunger switch; there is no lid
        # sensor). doorKnown stays false until an EDGE is seen, because on some
        # X1C units the bit is stuck at "open" for the whole session.
        self.door_open = bool(getattr(args, "door", False))
        self.door_known = False
        self.door_edges = 0
        self.last_door_open = 0.0
        self.last_door_close = 0.0
        # filament / AMS (spec 16): the fake AMS is the captured X1C block, with
        # tray_now pointing at slot 0 (ABS) unless --filament says otherwise.
        self.trays, self.vt_tray, self.tray_now = load_ams_fixture()
        if getattr(args, "filament", None):
            src, ams, slot = active_tray(self.tray_now)
            key = (ams, slot) if src == "ams" else None
            tray = self.trays.get(key) if key else self.vt_tray
            if tray:
                tray["type"] = args.filament.upper()
                tray["idx"] = ""            # a type with no Bambu id: third-party spool
                tray["subBrand"] = ""
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
        self.setpoint = None
        self.pi_integral = 0.0
        self.pi_setpoint = None
        self.pi_last = 0.0
        self.pi_out = 0.0
        self.door_rule_until = 0.0             # doorResumeSec anti-flap deadline
        # post-print cool-down session (spec 17.2)
        self.cd_active = False
        self.cd_manual = False
        self.cd_started = 0.0
        self.cd_start_chamber = None
        self.cd_target = 35
        self.cd_reason = None
        self.cd_sent_on = False
        self.cd_ever_sent = False
        self.cd_aux = 0
        self.cd_chamber_fan = 0
        self.cd_last_send = 0.0
        self.cd_at_target = 0
        self.cd_link_lost_at = None
        self.cd_material = None
        self.cd_prev_phase = None
        self.cd_last_step = 0.0
        self.cd_command = None                 # "start" / "stop", latched by the API
        # cooling-rate learning (spec 15.4)
        self.win = None                        # dict(start, t0, last, tlast, out, door)
        self.rate_c_per_min = None
        self.k_closed = [None] * 5
        self.k_open = [None] * 5
        self.k_samples = 0
        self.last_tick = time.monotonic()
        # wifi scan state
        self.scan_started = 0.0
        self.scan_done_at = 0.0
        self.mqtt_ext_connected = False
        self.restart_count = 0

    # ---------------- printer ----------------
    def state_name(self):
        return SCENARIO[self.phase_i][0]

    def stage(self):
        return SCENARIO[self.phase_i][2]

    def phase(self):
        """Mirrors reportPhase() in printer_parse.h - first rule that matches."""
        if not self.printer_connected or not self.ever_updated:
            return "offline"
        gs, stage = self.state_name(), self.stage()
        if gs == "PAUSE" or stage in PAUSE_STAGES:
            return "paused"
        warming = gs == "RUNNING" and (
            (self.b_target > 0 and self.bed < self.b_target - 3) or
            (self.c_target > 0 and self.chamber < self.c_target - 2))
        if stage in PREHEAT_STAGES or warming:
            return "preheat"
        if stage in COOLING_STAGES:
            return "cooling"
        if gs in ("RUNNING", "PREPARE", "SLICING"):
            return "printing"
        if gs == "FINISH":
            return "finished"
        if gs == "FAILED":
            return "failed"
        return "idle"

    def printing(self):
        """spec 15.1: onlyWhilePrinting gates on the phase, not on gcode_state."""
        return self.phase() in ("preheat", "printing", "paused")

    # ---------------- door ----------------
    def door(self):
        """The door state the control logic may act on: closed until an edge has
        proved the switch reports at all (spec 15.1)."""
        return self.door_known and self.door_open

    def set_door(self, want_open):
        """A door change is an edge, and the first edge is what makes the state
        trustworthy. The initial state (including --door) is not an edge."""
        want_open = bool(want_open)
        if want_open == self.door_open:
            return False
        now = time.monotonic()
        self.door_open = want_open
        self.door_known = True
        self.door_edges += 1
        if want_open:
            self.last_door_open = now
        else:
            self.last_door_close = now
            self.door_rule_until = now + self.cfg["fan"]["doorResumeSec"]
        BUS.log("I", "door %s" % ("opened" if want_open else "closed"))
        return True

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
        name, dur, _stage = SCENARIO[self.phase_i]
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
            self.n_target, self.b_target, self.c_target = 220.0, 100.0, 32.0
        elif name == "RUNNING":
            self.n_target, self.b_target, self.c_target = 220.0, 100.0, 32.0
            frac = self.phase_t / dur
            self.progress = clamp(frac * 100.0, 0, 100)
            self.layer = int(self.progress / 100.0 * self.total_layers)
        else:
            self.n_target, self.b_target, self.c_target = 0.0, 0.0, 0.0
        self.nozzle = self._approach(self.nozzle, self.n_target if self.n_target else 24.0,
                                     9.0 if self.n_target else 2.0, dt)
        self.nozzle += random.uniform(-0.25, 0.25)
        # A deliberately slow bed, so the "RUNNING but still below target" branch
        # of the preheat rule is actually visible in the UI.
        self.bed = self._approach(self.bed, self.b_target if self.b_target else 23.5,
                                  1.2 if self.b_target else 0.35, dt)
        self.bed += random.uniform(-0.08, 0.08)
        self.tick_chamber(dt)

    def tick_chamber(self, dt):
        """Newtonian model: the bed heats the chamber, the fan and an open door
        carry the heat away. This is what makes the thermostat worth testing."""
        amb = float(self.cfg["fan"]["ambientTemp"])
        out = getattr(self, "effective_output", self.output)
        k = K_BASE + K_FAN * clamp(out, 0, 100) / 100.0 + (K_DOOR if self.door() else 0.0)
        if self.cd_sent_on:
            # spec 17: M106 P2/P3 are actually moving air inside the enclosure
            k += K_PRINTER_FAN * (self.cd_aux + self.cd_chamber_fan) / 200.0
        heat = CHAMBER_HEAT_C_PER_SEC if self.b_target > 0 else 0.0
        self.chamber += (heat - k / 60.0 * (self.chamber - amb)) * dt
        self.chamber = clamp(self.chamber, amb - 1.0, 70.0)

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
        stage = self.stage()
        if name == "IDLE":                     # nothing loaded -> no job counters
            return dict(unknown, stage=stage, stageText=STAGES.get(stage, "idle"))
        return {"stage": stage, "stageText": STAGES.get(stage, "idle"),
                "progress": int(self.progress), "remainingMin": self.remaining_min(),
                "layer": self.layer, "totalLayers": self.total_layers}

    def temps(self):
        if not self.printer_connected:
            return {"nozzle": None, "nozzleTarget": None, "bed": None,
                    "bedTarget": None, "chamber": None, "chamberTarget": None}
        return {"nozzle": round(self.nozzle, 1), "nozzleTarget": round(self.n_target),
                "bed": round(self.bed, 1), "bedTarget": round(self.b_target),
                "chamber": round(self.chamber, 1),
                # null rather than 0 when the printer has no chamber heater
                "chamberTarget": round(self.c_target) if self.c_target else None}

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

    # ---------------- filament (spec section 16) ----------------
    def active_tray_record(self):
        """(source, ams, slot, tray dict or None)."""
        if not self.printer_connected:
            return "none", -1, -1, None
        src, ams, slot = active_tray(self.tray_now)
        if src == "ams":
            return src, ams, slot, self.trays.get((ams, slot))
        if src == "external":
            return src, ams, slot, self.vt_tray
        return src, ams, slot, None

    def filament(self):
        """The `filament` status block plus the effective profile the fan uses."""
        fil_cfg, fan_cfg = self.cfg["filament"], self.cfg["fan"]
        src, ams, slot, tray = self.active_tray_record()
        fid, family = ("", "")
        if tray and (tray.get("type") or tray.get("idx")):
            fid, family = identify(tray.get("type"), tray.get("subBrand"), tray.get("idx"))
        else:
            tray = None
        info = BY_ID.get(fid)
        if not info and fil_cfg.get("manualId"):
            info = BY_ID.get(fil_cfg["manualId"])
            if info:
                fid = info["id"]
                family = family or info["name"]
                if tray is None:
                    src, ams, slot = "manual", -1, -1
        eff, _keep = effective_profile(info, fil_cfg, fan_cfg)

        def entry(a, sl, t):
            i, _f = identify(t.get("type"), t.get("subBrand"), t.get("idx"))
            return {"ams": a, "slot": sl, "type": t.get("type") or None,
                    "subBrand": t.get("subBrand") or None, "idx": t.get("idx") or None,
                    "color": t.get("color") or None, "id": i or None}

        trays = [entry(a, sl, t) for (a, sl), t in sorted(self.trays.items())
                 if t.get("type") or t.get("idx")]
        if self.vt_tray and (self.vt_tray.get("type") or self.vt_tray.get("idx")):
            trays.append(entry(-1, 254, self.vt_tray))
        return {
            "source": src, "auto": bool(fil_cfg.get("auto", True)),
            "tray": ({"ams": ams, "slot": slot, "type": tray.get("type") or None,
                      "subBrand": tray.get("subBrand") or None,
                      "idx": tray.get("idx") or None,
                      "color": tray.get("color") or None} if tray else None),
            "id": fid or None,
            "name": info["name"] if info else None,
            "family": family or None,
            "profile": ({"chamberRec": None if info["cRec"] == TEMP_NA else info["cRec"],
                         "chamberMax": None if info["cMax"] == TEMP_NA else info["cMax"],
                         "partCoolRec": None if info["cool"] == PARTCOOL_NA else info["cool"],
                         "vent": VENT_NAMES[info["vent"]],
                         "openForCooling": bool(info["flags"] & FIL_OPEN_COOL),
                         "heatedRequired": bool(info["flags"] & FIL_HEATED)} if info else None),
            "effective": eff,
            "trays": trays,
        }

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

        phase = self.phase()
        # spec 16.3: the filament decides the set points unless it is switched off
        eff = self.filament()["effective"]
        recent_print = (not self.printing() and self.print_end_at
                        and now - self.print_end_at < f["cooldownMin"] * 60)
        chamber_cool = self.chamber <= eff["cooldownTarget"]

        # 3. door: nothing to exhaust while the printer is open, and the
        #    rule stays armed for doorResumeSec after it closes. During a
        #    cool-down an open door helps, so the rule is skipped there.
        door_active = self.door() or (self.door_known and now < self.door_rule_until)
        door_rule = (f["doorMode"] != "ignore" and self.door_known and door_active
                     and phase not in ("finished", "cooling", "idle"))
        # 4. preheat: the fan would be fighting the heaters.
        preheat_rule = f["preheatMode"] != "ignore" and phase == "preheat"

        # spec 17.2: while a session runs it owns the device's own fan, unless
        # ownFan is "curve" (= do not interfere)
        cd_max = self.cd_active and self.cfg["cooldown"]["ownFan"] == "max"
        cd_pi = (self.cd_active and self.cfg["cooldown"]["ownFan"] == "thermostat"
                 and self.chamber is not None)

        # 5. chamber thermostat
        thermostat = ((f["mode"] == "chamber" or cd_pi) and st is not None
                      and self.chamber is not None)
        cooling_phase = phase in ("finished", "cooling") or (phase == "idle" and recent_print)
        sp = None
        if cd_pi:
            sp = float(self.cd_target)
        elif thermostat:
            if phase in ("preheat", "printing", "paused"):
                sp = float(eff["chamberTarget"])
            elif cooling_phase:
                sp = float(eff["cooldownTarget"])

        if f["mode"] == "off":
            mode, tgt = "off", 0.0
        elif f["mode"] == "manual":
            mode, tgt = "manual", float(f["manualSpeed"])
        elif st is None or not self.printer_connected or stale_age > f["staleSec"]:
            mode = "stale"
            sm = f["staleMode"]
            tgt = self.output if sm == "hold" else (float(f["staleSpeed"]) if sm == "fixed" else 0.0)
        elif door_rule:
            mode = "door"
            tgt = float(f["doorSpeed"]) if f["doorMode"] == "fixed" else 0.0
        elif preheat_rule:
            mode = "preheat"
            tgt = float(f["preheatSpeed"]) if f["preheatMode"] == "fixed" else 0.0
        elif cd_max:
            mode, tgt = "cooldown", 100.0
        elif cd_pi:
            mode, tgt = "cooldown", self._thermostat(sp, f, now)
        elif thermostat and sp is None:
            mode, tgt = "idle", 0.0
        elif thermostat:
            mode = "cooldown" if cooling_phase else "chamber"
            tgt = self._thermostat(sp, f, now)
        elif f["onlyWhilePrinting"] and not self.printing():
            # the window ends at cooldownTarget or cooldownMin, whichever first
            if recent_print and not chamber_cool:
                mode = "cooldown"
                tgt = self._curve_target(st, f)
            else:
                mode, tgt = "idle", 0.0
        else:
            mode, tgt = "auto", self._curve_target(st, f)

        # spec 16.3: gentle post-print cooling, then the ventilation floor.
        if eff["postPrintCooling"] == "gentle" and mode == "cooldown":
            tgt = 0.0 if self.chamber > eff["chamberTarget"] - 10 else min(tgt, 50.0)
        vent_floor_applies = (eff["ventFloor"] > 0 and self.printing()
                              and f["mode"] not in ("off", "manual")
                              and mode not in ("door", "preheat", "stale"))
        if vent_floor_applies:
            tgt = max(tgt, float(eff["ventFloor"]))

        if mode not in ("chamber", "cooldown") or sp is None:
            self.pi_integral, self.pi_setpoint, self.pi_out, self.pi_last = 0.0, None, 0.0, 0.0
        self.setpoint = sp if mode in ("chamber", "cooldown") and sp is not None else None
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

    def _thermostat(self, sp, f, now):
        """Same PI step as thermostat.h: anti-windup clamp plus conditional
        integration, frozen while the door is open or the output is saturated."""
        if self.pi_setpoint is None or abs(sp - self.pi_setpoint) > 0.01:
            self.pi_integral, self.pi_setpoint, self.pi_last = 0.0, sp, 0.0
        period = f["thermostatPeriodSec"]
        if self.pi_last and now - self.pi_last < period:
            return self.pi_out
        dt = period if not self.pi_last else now - self.pi_last
        self.pi_last = now
        kp, ki = float(f["kp"]), float(f["ki"])
        e = self.chamber - sp
        integral = self.pi_integral
        if ki > 0 and dt > 0 and not self.door():
            lim = 100.0 / ki
            nxt = clamp(integral + e * dt, -lim, lim)
            held, trial = kp * e + ki * integral, kp * e + ki * nxt
            if not ((held >= 100 and trial > held) or (held <= 0 and trial < held)):
                integral = nxt
        self.pi_integral = integral
        self.pi_out = clamp(kp * e + ki * integral, 0.0, 100.0)
        return self.pi_out

    # ---------------- cooling-rate learning (spec 15.4) ----------------
    MIN_WINDOW_SEC = 60.0

    def tick_thermal(self, now):
        """Passive: a window is >= 60 s of steady output, unchanged door and no
        heater running. k = -(dT/dt)/(T - ambient), blended into the bucket."""
        amb = float(self.cfg["fan"]["ambientTemp"])
        out = getattr(self, "effective_output", self.output)
        heater = self.b_target > 0 or self.n_target > 0
        if heater or not self.printer_connected:
            self.win, self.rate_c_per_min = None, None
            return
        door = self.door()          # an unproven switch counts as closed
        w = self.win
        if w and (abs(out - w["out"]) > 5 or door != w["door"]):
            w = None
        if not w:
            self.win = {"start": now, "t0": self.chamber, "last": now,
                        "tlast": self.chamber, "out": out, "door": door}
            self.rate_c_per_min = None
            return
        w["last"], w["tlast"] = now, self.chamber
        span = w["last"] - w["start"]
        self.rate_c_per_min = (round((w["tlast"] - w["t0"]) / span * 60.0, 2)
                               if span >= 20 else None)
        if span < self.MIN_WINDOW_SEC:
            return
        d_t = w["tlast"] - w["t0"]
        lift = (w["t0"] + w["tlast"]) / 2.0 - amb
        rate = d_t / span * 60.0
        if abs(d_t) >= 0.5 and lift >= 3.0 and rate < 0:
            k = -rate / lift
            bucket = int(clamp((out + 12.5) // 25, 0, 4))
            table = self.k_open if door else self.k_closed
            table[bucket] = k if table[bucket] is None else table[bucket] + 0.3 * (k - table[bucket])
            self.k_samples += 1
            BUS.log("I", "thermal: k=%.3f /min at %d%% door %s"
                    % (k, bucket * 25, "open" if door else "closed"))
            self.win = None
        elif span >= self.MIN_WINDOW_SEC * 5:
            self.win = None

    # ---------------- post-print cool-down (spec 17.2) ----------------
    def cd_gcode_safe(self):
        return self.printer_connected and self.state_name() in ("FINISH", "IDLE")

    def cd_gcode_busy(self):
        return self.printer_connected and self.state_name() in (
            "RUNNING", "PAUSE", "PREPARE", "SLICING")

    def cd_send_fans(self, aux, chamber):
        """The mock's stand-in for printerLinkSendGcode(): it logs the exact line
        the firmware would publish as a `gcode_line` request."""
        BUS.log("I", "printer <- M106 P2 S%d M106 P3 S%d"
                % (round(aux * 255 / 100.0), round(chamber * 255 / 100.0)))

    def cd_stop(self, reason, send_stop):
        self.cd_active = False
        self.cd_reason = reason
        if send_stop and self.cd_ever_sent and self.cd_gcode_safe():
            self.cd_send_fans(0, 0)
        self.cd_sent_on = False
        self.cd_aux = self.cd_chamber_fan = 0
        BUS.log("I", "cooldown: finished (%s)" % reason)

    def tick_cooldown(self, now):
        """Mirrors cooldownStep() in cooldown_logic.h - one sample every 5 s, plus
        an immediate pass whenever the API has latched a command."""
        cmd, self.cd_command = self.cd_command, None
        if cmd is None and self.cd_last_step and now - self.cd_last_step < COOLDOWN_SAMPLE_SEC:
            return
        self.cd_last_step = now

        c = self.cfg["cooldown"]
        eff = self.filament()["effective"]
        phase = self.phase()
        prev = self.cd_prev_phase if self.cd_prev_phase is not None else phase
        self.cd_prev_phase = phase
        busy = self.printing() or self.cd_gcode_busy()
        online = self.printer_connected and self.ever_updated

        if not self.cd_active:
            start = False
            if cmd == "start" and not busy:
                start, self.cd_manual = True, True
            elif c["enabled"] and cmd != "stop" and not busy:
                # the edge, not the level: a session the user stopped must not
                # restart while the printer sits at FINISH
                start = (phase in ("finished", "cooling")
                         and prev not in ("finished", "cooling"))
                self.cd_manual = False
            if not start:
                return
            self.cd_active = True
            self.cd_started = now
            self.cd_start_chamber = self.chamber
            self.cd_target = int(eff["cooldownTarget"] if self.cfg["filament"]["auto"]
                                 else c["target"])
            self.cd_reason = None
            self.cd_sent_on = self.cd_ever_sent = False
            self.cd_aux = self.cd_chamber_fan = 0
            self.cd_last_send = now
            self.cd_at_target = 0
            self.cd_link_lost_at = None
            self.cd_material = self.filament()["id"] or None
            BUS.log("I", "cooldown: started, chamber %.1f -> %d C"
                    % (self.chamber, self.cd_target))

        if busy:                                  # the print owns its fans again
            return self.cd_stop("newJob", False)
        if cmd == "stop":
            return self.cd_stop("stopped", True)
        if not c["enabled"] and not self.cd_manual:
            return self.cd_stop("disabled", True)
        if not online:
            if self.cd_link_lost_at is None:
                self.cd_link_lost_at = now
            if now - self.cd_link_lost_at > COOLDOWN_LINK_GRACE_SEC:
                return self.cd_stop("linkLost", False)
        else:
            self.cd_link_lost_at = None
        if now - self.cd_started >= c["maxMinutes"] * 60:
            return self.cd_stop("timeout", True)
        if self.chamber is not None and self.chamber <= self.cd_target:
            self.cd_at_target += 1
        else:
            self.cd_at_target = 0
        if self.cd_at_target >= 2:
            return self.cd_stop("target", True)

        allowed = c["usePrinterFans"] and online and self.cd_gcode_safe()
        gentle = False
        if allowed and c["gentleFromFilament"] and eff["postPrintCooling"] == "gentle":
            if self.chamber is None or self.chamber >= eff["chamberTarget"] - 10:
                allowed = False
            else:
                gentle = True
        if allowed:
            aux = c["auxSpeed"] // 2 if gentle else c["auxSpeed"]
            cha = c["chamberFanSpeed"] // 2 if gentle else c["chamberFanSpeed"]
            changed = (not self.cd_sent_on or aux != self.cd_aux
                       or cha != self.cd_chamber_fan)
            due = self.cd_sent_on and now - self.cd_last_send >= COOLDOWN_REASSERT_SEC
            if changed or due:
                self.cd_send_fans(aux, cha)
                self.cd_sent_on = self.cd_ever_sent = True
                self.cd_aux, self.cd_chamber_fan = aux, cha
                self.cd_last_send = now
        elif self.cd_sent_on:
            if self.cd_gcode_safe():
                self.cd_send_fans(0, 0)
            self.cd_sent_on = False
            self.cd_aux = self.cd_chamber_fan = 0

    def cd_status(self):
        now = time.monotonic()
        c = self.cfg["cooldown"]
        return {
            "active": self.cd_active,
            "reason": self.cd_reason,
            "target": self.cd_target if self.cd_active else c["target"],
            "chamber": round(self.chamber, 1) if self.chamber is not None else None,
            "startChamber": round(self.cd_start_chamber, 1)
            if self.cd_start_chamber is not None else None,
            "elapsedSec": int(now - self.cd_started) if self.cd_active else 0,
            "maxSec": int(c["maxMinutes"]) * 60,
            "printerFans": {"aux": self.cd_aux, "chamber": self.cd_chamber_fan,
                            "sent": self.cd_sent_on},
            "ownFan": c["ownFan"],
            "material": self.cd_material if self.cd_active else None,
        }

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
                           "ip": "192.168.4.1" if ap else "192.168.1.50", "apMode": ap},
                "wifi": {"connected": connected,
                         # empty while in AP mode / not associated
                         "ssid": self.cfg["wifi"]["ssid"] if connected else "",
                         "bssid": "02:00:5E:10:20:30" if connected else "",
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
                    "task": "Bracket_v3.3mf" if self.printer_connected else "",
                    "phase": self.phase(),
                    # null until an edge has proved the switch reports at all
                    "doorOpen": self.door_open if self.door_known else None,
                    "doorKnown": self.door_known, "doorEdgeCount": self.door_edges,
                    "printError": 0,
                    "wifiSignal": "-45dBm" if self.printer_connected else "",
                    "temps": self.temps(),
                    "fans": {"part": out, "aux": 0, "chamber": 40, "heatbreak": 100}
                    if self.printer_connected else
                    {"part": 0, "aux": 0, "chamber": 0, "heatbreak": 0},
                },
                "fan": {"output": out, "target": round(self.target), "mode": f["mode"],
                        "effectiveMode": self.eff_mode, "source": f["source"],
                        "sourceTemp": self.source_temp(),
                        "setpoint": self.setpoint, "chamberTarget": f["chamberTarget"],
                        "cooldownTarget": f["cooldownTarget"],
                        "manualSpeed": f["manualSpeed"],
                        "manualExpiresSec": max(0, int(self.manual_expires - now))
                        if self.manual_expires else 0,
                        "pwmDuty": self.pwm_duty(), "output1": f["output1"],
                        "output2": f["output2"]},
                "filament": self.filament(),
                # post-print cool-down session (spec 17.3)
                "cooldown": self.cd_status(),
                # NaN is never emitted: an unmeasured bucket is JSON null
                "thermal": {"rateCPerMin": self.rate_c_per_min,
                            "kClosed": [round(k, 3) if k is not None else None
                                        for k in self.k_closed],
                            "kOpen": [round(k, 3) if k is not None else None
                                      for k in self.k_open],
                            "samples": self.k_samples},
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
                # before the fan: a running session decides what the fan does
                self.tick_cooldown(now)
                self.tick_fan(dt)
                self.tick_thermal(now)
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
    {"ssid": "Workshop-WiFi", "bssid": "02:00:5E:10:20:30", "rssi": -47, "channel": 6, "secure": True},
    {"ssid": "Home-Network", "bssid": "02:00:5E:10:20:31", "rssi": -68, "channel": 11, "secure": True},
    {"ssid": "Garage-IoT", "bssid": "02:00:5E:10:20:32", "rssi": -74, "channel": 1, "secure": True},
    {"ssid": "Guest", "bssid": "02:00:5E:10:20:33", "rssi": -79, "channel": 1, "secure": False},
    {"ssid": "Cafe-Free", "bssid": "02:00:5E:10:20:34", "rssi": -88, "channel": 13, "secure": False},
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
            if p == "/api/filaments":
                return self._send({
                    "count": len(FILAMENTS),
                    "source": "https://pwsh.github.io/filament-field-guide",
                    "licence": "CC BY 4.0",
                    "filaments": [{"id": x["id"], "name": x["name"], "cls": x["cls"],
                                   "cMin": None if x["cMin"] == TEMP_NA else x["cMin"],
                                   "cRec": None if x["cRec"] == TEMP_NA else x["cRec"],
                                   "cMax": None if x["cMax"] == TEMP_NA else x["cMax"],
                                   "cool": None if x["cool"] == PARTCOOL_NA else x["cool"],
                                   "vent": VENT_NAMES[x["vent"]], "flags": x["flags"],
                                   "voc": LEVEL_NAMES[x["voc"]],
                                   "part": LEVEL_NAMES[x["part"]]} for x in FILAMENTS]})
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
            if p == "/api/cooldown":
                b = self._json_body()
                if not isinstance(b.get("start"), bool):
                    return self._err('expected {"start":true|false}')
                with sim.lock:
                    if b["start"] and sim.printing():
                        return self._err("printer is busy")
                    sim.cd_command = "start" if b["start"] else "stop"
                    sim.tick_cooldown(time.monotonic())
                    st = sim.cd_status()
                return self._send({"ok": True, "cooldown": st})
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
            if p == "/mock/door":
                # Not part of the device API: the mock's stand-in for someone
                # opening the printer, so the door rules can be demonstrated.
                b = self._json_body()
                with sim.lock:
                    want = (not sim.door_open) if b.get("toggle") else bool(b.get("open"))
                    changed = sim.set_door(want)
                    st = sim.status()["printer"]
                return self._send({"ok": True, "changed": changed,
                                   "doorOpen": st["doorOpen"],
                                   "doorKnown": st["doorKnown"],
                                   "doorEdgeCount": st["doorEdgeCount"]})
            if p == "/mock/tray":
                # Not part of the device API: stands in for the printer switching
                # to another tray, so the filament card can be exercised.
                b = self._json_body()
                with sim.lock:
                    try:
                        sim.tray_now = int(b.get("now", 255))
                    except (TypeError, ValueError):
                        return self._err("now must be 0..15, 254 or 255")
                    fil = sim.filament()
                BUS.log("I", "mock: tray_now=%d (%s)" % (sim.tray_now, fil["name"] or "unknown"))
                return self._send({"ok": True, "trayNow": sim.tray_now, "filament": fil})
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
        if mode not in (None, "auto", "chamber", "manual", "off"):
            return self._err("mode must be auto, chamber, manual or off")
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
    ap.add_argument("--door", action="store_true",
                    help="start with the door reported open (POST /mock/door toggles it)")
    ap.add_argument("--filament", default="", metavar="TYPE",
                    help="override the loaded tray's material, e.g. --filament ABS")
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
                             ("door-open", args.door),
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
