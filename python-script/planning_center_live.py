"""
Planning Center Live for OBS
============================

OBS Python script (Tools -> Scripts) that reads the Planning Center Services
API directly (Personal Access Token, no browser login, no cookies) and serves
switchable overlays (lower third countdown, big countdown, run sheet,
service countdown, clock) to an OBS browser source.

It can also run standalone for testing:  python planning_center_live.py --demo

(c) meyer.productions - MIT License
"""

import base64
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import obspython as obs  # only available inside OBS
except ImportError:  # standalone mode
    obs = None

VERSION = "1.0.0"
API_BASE = "https://api.planningcenteronline.com/services/v2"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# shared with the native plugin (data/overlay/overlay.html); a copy next to the script wins
OVERLAY_FILE = next((p for p in (os.path.join(SCRIPT_DIR, "overlay.html"),
                                 os.path.join(os.path.dirname(SCRIPT_DIR), "data", "overlay", "overlay.html"))
                     if os.path.isfile(p)), os.path.join(SCRIPT_DIR, "overlay.html"))

VIEWS = [
    ("lowerthird", "Lower Third – Countdown (Standard)"),
    ("countdown", "Grosser Countdown"),
    ("list", "Ablauf (Liste wie PCO Live)"),
    ("service", "Gottesdienst-Countdown / Gesamtzeit"),
    ("clock", "Uhrzeit + aktuelles Element"),
    ("off", "Ausgeblendet"),
]
VIEW_KEYS = [v[0] for v in VIEWS]

# Plans whose last service time ended less than this ago stay selected
# (so the overlay does not jump to next week's plan right after the service).
PLAN_STICKY_HOURS = 3


def log(msg):
    line = "[PCO Live] %s" % msg
    if obs is not None:
        obs.script_log(obs.LOG_INFO, line)
    else:
        print(line, flush=True)


def parse_ts(value):
    """ISO-8601 from the API -> epoch milliseconds (or None)."""
    if not value:
        return None
    try:
        dt = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return int(dt.timestamp() * 1000)
    except ValueError:
        return None


def now_ms():
    return int(time.time() * 1000)


# --------------------------------------------------------------------------
# Configuration (copied from OBS settings, read by the worker thread)
# --------------------------------------------------------------------------
class Config:
    def __init__(self):
        self.app_id = ""
        self.secret = ""
        self.service_type_id = ""
        self.plan_id = ""  # manual override, empty = automatic
        self.poll_seconds = 2
        self.port = 8765
        self.lan = False
        self.demo = False
        self.accent = "#e4002b"
        self.warn_seconds = 30
        self.show_next = True
        self.show_progress = True
        self.preservice_minutes = 30

    def has_credentials(self):
        return bool(self.app_id and self.secret)

    def overlay_config(self):
        return {
            "accent": self.accent,
            "warn_seconds": self.warn_seconds,
            "show_next": self.show_next,
            "show_progress": self.show_progress,
            "preservice_minutes": self.preservice_minutes,
            "lt_bottom": 16,
            "lt_side": 24,
            "lt_width": 100,
            "lt_height": 64,
            "lt_clock": True,
            "lang": "de",
        }


# --------------------------------------------------------------------------
# Planning Center API client
# --------------------------------------------------------------------------
class PcoError(Exception):
    pass


class PcoClient:
    def __init__(self, app_id, secret):
        token = base64.b64encode(("%s:%s" % (app_id, secret)).encode()).decode()
        self.headers = {
            "Authorization": "Basic " + token,
            "Accept": "application/json",
            "User-Agent": "planning-center-live-obs/" + VERSION,
        }

    def get(self, path, params=None):
        url = path if path.startswith("http") else API_BASE + path
        if params:
            url += ("&" if "?" in url else "?") + urllib.parse.urlencode(params)
        req = urllib.request.Request(url, headers=self.headers)
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            detail = ""
            try:
                body = json.loads(e.read().decode("utf-8"))
                errs = body.get("errors") or []
                if errs:
                    detail = errs[0].get("detail") or errs[0].get("title") or ""
            except Exception:
                pass
            if e.code == 401:
                raise PcoError("Zugangsdaten ungültig (401)")
            if e.code == 429:
                raise PcoError("Rate-Limit erreicht (429)")
            raise PcoError("HTTP %s %s" % (e.code, detail).strip())
        except urllib.error.URLError as e:
            raise PcoError("Keine Verbindung: %s" % e.reason)

    def get_all(self, path, params=None, limit=500):
        """Follows JSON:API pagination, returns (data, included)."""
        params = dict(params or {})
        params.setdefault("per_page", 100)
        body = self.get(path, params)
        data = list(body.get("data") or [])
        included = list(body.get("included") or [])
        nxt = (body.get("links") or {}).get("next")
        while nxt and len(data) < limit:
            body = self.get(nxt)
            data += body.get("data") or []
            included += body.get("included") or []
            nxt = (body.get("links") or {}).get("next")
        return data, included

    def service_types(self):
        data, _ = self.get_all("/service_types", {"order": "sequence"})
        return [(d["id"], d["attributes"].get("name") or d["id"]) for d in data]


def rel_id(resource, name):
    try:
        rel = (resource.get("relationships") or {}).get(name) or {}
        data = rel.get("data")
        return data.get("id") if isinstance(data, dict) else None
    except AttributeError:
        return None


# --------------------------------------------------------------------------
# Worker: polls the API and builds the overlay state
# --------------------------------------------------------------------------
class Worker:
    def __init__(self):
        self.cfg = Config()
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.stop_flag = threading.Event()
        self.thread = None
        self.view = "lowerthird"
        self.state = {"ok": False, "error": "Noch keine Daten", "plan": None}
        # caches
        self._plan = None
        self._plan_checked = 0
        self._items = []
        self._items_loaded = 0
        self._plan_times = []
        self._live_include_ok = True
        self.service_types_cache = []

    # ----- lifecycle ------------------------------------------------------
    def start(self):
        if self.thread and self.thread.is_alive():
            return
        self.stop_flag.clear()
        self.thread = threading.Thread(target=self._run, name="pco-live", daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_flag.set()
        self.wake.set()
        if self.thread:
            self.thread.join(timeout=3)
        self.thread = None

    def reconfigure(self, cfg):
        with self.lock:
            changed = (
                cfg.app_id != self.cfg.app_id
                or cfg.secret != self.cfg.secret
                or cfg.service_type_id != self.cfg.service_type_id
                or cfg.plan_id != self.cfg.plan_id
                or cfg.demo != self.cfg.demo
            )
            self.cfg = cfg
            if changed:
                self._plan = None
                self._plan_checked = 0
                self._items = []
                self._items_loaded = 0
                self._plan_times = []
                self._live_include_ok = True
        self.wake.set()

    def set_view(self, view):
        if view == "next":
            idx = VIEW_KEYS.index(self.view) if self.view in VIEW_KEYS else -1
            view = VIEW_KEYS[(idx + 1) % len(VIEW_KEYS)]
        if view not in VIEW_KEYS:
            return False
        with self.lock:
            self.view = view
        log("Ansicht: %s" % view)
        return True

    def snapshot(self):
        with self.lock:
            s = dict(self.state)
            s["view"] = self.view
            s["now"] = now_ms()
            s["config"] = self.cfg.overlay_config()
            s["views"] = VIEW_KEYS
            return s

    def refresh(self):
        with self.lock:
            self._plan_checked = 0
            self._items_loaded = 0
        self.wake.set()

    # ----- loop -----------------------------------------------------------
    def _run(self):
        while not self.stop_flag.is_set():
            with self.lock:
                cfg = self.cfg
            try:
                if cfg.demo:
                    state = demo_state()
                elif not cfg.has_credentials():
                    state = {"ok": False, "error": "App ID / Secret fehlt", "plan": None}
                elif not cfg.service_type_id and not cfg.plan_id:
                    state = {"ok": False, "error": "Kein Service Type gewählt", "plan": None}
                else:
                    state = self._poll(cfg)
            except PcoError as e:
                state = dict(self.state)
                state["ok"] = False
                state["error"] = str(e)
                log("Fehler: %s" % e)
            except Exception as e:  # never kill the thread
                state = dict(self.state)
                state["ok"] = False
                state["error"] = "Interner Fehler: %s" % e
                log("Interner Fehler: %r" % e)
            with self.lock:
                self.state = state
            interval = max(1, int(cfg.poll_seconds or 2))
            if state.get("error") and "429" in state.get("error", ""):
                interval = 20
            self.wake.wait(interval)
            self.wake.clear()

    def _poll(self, cfg):
        client = PcoClient(cfg.app_id, cfg.secret)
        t = time.time()

        # 1) Plan selection (every 5 min, or immediately after config change)
        if self._plan is None or t - self._plan_checked > 300:
            self._plan = self._select_plan(client, cfg)
            self._plan_checked = t
            self._items_loaded = 0
            if self._plan:
                log("Plan: %s (%s, ID %s)" % (
                    self._plan["attributes"].get("title") or "-",
                    self._plan["attributes"].get("dates"),
                    self._plan["id"]))
        plan = self._plan
        if not plan:
            return {"ok": True, "error": None, "plan": None, "live": None, "items": []}

        st_id = cfg.service_type_id or rel_id(plan, "service_type")
        base = "/service_types/%s/plans/%s" % (st_id, plan["id"])

        # 2) Items + plan times (every 30 s; they rarely change during a service)
        if t - self._items_loaded > 30:
            items, _ = client.get_all(base + "/items")
            items.sort(key=lambda i: i["attributes"].get("sequence") or 0)
            self._items = items
            pts, _ = client.get_all(base + "/plan_times")
            self._plan_times = pts
            self._items_loaded = t

        # 3) Live status (every poll); a plan without LIVE session is not an error
        try:
            cur_it, next_it = self._live_item_times(client, base)
        except PcoError as e:
            if "404" not in str(e):
                raise
            cur_it, next_it = None, None

        return self._build_state(plan, cur_it, next_it)

    def _select_plan(self, client, cfg):
        if cfg.plan_id:
            if cfg.service_type_id:
                body = client.get("/service_types/%s/plans/%s" % (cfg.service_type_id, cfg.plan_id))
            else:
                body = client.get("/plans/%s" % cfg.plan_id)
            return body.get("data")
        base = "/service_types/%s/plans" % cfg.service_type_id
        # most recent past plan - still "current" shortly after the service
        past = client.get(base, {"filter": "past", "order": "-sort_date", "per_page": 1}).get("data") or []
        if past:
            last = parse_ts(past[0]["attributes"].get("last_time_at"))
            if last and now_ms() - last < PLAN_STICKY_HOURS * 3600 * 1000:
                return past[0]
        future = client.get(base, {"filter": "future", "order": "sort_date", "per_page": 1}).get("data") or []
        return future[0] if future else (past[0] if past else None)

    def _live_item_times(self, client, base):
        """Returns (current ItemTime, next ItemTime) or (None, None)."""
        if self._live_include_ok:
            try:
                body = client.get(base + "/live", {"include": "current_item_time,next_item_time"})
                live = body.get("data") or {}
                included = {(r["type"], r["id"]): r for r in body.get("included") or []}
                rels = live.get("relationships") or {}
                if "current_item_time" in rels or included:
                    cur_id = rel_id(live, "current_item_time")
                    nxt_id = rel_id(live, "next_item_time")
                    return (included.get(("ItemTime", cur_id)) if cur_id else None,
                            included.get(("ItemTime", nxt_id)) if nxt_id else None)
                # include did not deliver relationships -> use the edge endpoints
                self._live_include_ok = False
                self._live_id = live.get("id")
            except PcoError as e:
                if "400" not in str(e):
                    raise
                self._live_include_ok = False
                self._live_id = None
        live_id = getattr(self, "_live_id", None)
        if not live_id:
            live_id = (client.get(base + "/live").get("data") or {}).get("id")
            self._live_id = live_id
        if not live_id:
            return None, None
        cur = client.get(base + "/live/%s/current_item_time" % live_id).get("data")
        nxt = client.get(base + "/live/%s/next_item_time" % live_id).get("data")
        return cur or None, nxt or None

    # ----- state building -------------------------------------------------
    def _build_state(self, plan, cur_it, next_it):
        pa = plan["attributes"]
        items_by_id = {i["id"]: i for i in self._items}

        # service times (only "service" type, fallback to all)
        times = []
        for pt in self._plan_times:
            a = pt["attributes"]
            times.append({
                "id": pt["id"],
                "name": a.get("name") or "",
                "type": str(a.get("time_type") or ""),
                "starts_at": parse_ts(a.get("starts_at")),
                "ends_at": parse_ts(a.get("ends_at")),
                "live_starts_at": parse_ts(a.get("live_starts_at")),
                "live_ends_at": parse_ts(a.get("live_ends_at")),
            })
        services = [x for x in times if x["type"] == "service"] or times
        services.sort(key=lambda x: x["starts_at"] or 0)

        def item_view(item_time):
            if not item_time:
                return None
            item = items_by_id.get(rel_id(item_time, "item"))
            ia = item["attributes"] if item else {}
            ta = item_time["attributes"]
            length = ta.get("length") or ia.get("length") or 0
            return {
                "id": item["id"] if item else None,
                "title": ia.get("title") or "",
                "type": ia.get("item_type") or "",
                "description": ia.get("description") or "",
                "key": ia.get("key_name") or "",
                "length": int(length),
                "start": parse_ts(ta.get("live_start_at")),
                "end": parse_ts(ta.get("live_end_at")),
                "plan_time_id": rel_id(item_time, "plan_time"),
            }

        current = item_view(cur_it)
        nxt = item_view(next_it)

        # which service time is running / next?
        n = now_ms()
        running = None
        if current and current.get("plan_time_id"):
            running = next((s for s in services if s["id"] == current["plan_time_id"]), None)
        if not running:
            running = next((s for s in services if s["live_starts_at"] and not s["live_ends_at"]), None)
        upcoming = next((s for s in services if s["starts_at"] and s["starts_at"] > n), None)
        if not nxt and current:
            # fallback: following item in sequence
            ids = [i["id"] for i in self._items]
            if current["id"] in ids:
                for i in self._items[ids.index(current["id"]) + 1:]:
                    if i["attributes"].get("item_type") != "header":
                        nxt = {"id": i["id"], "title": i["attributes"].get("title") or "",
                               "type": i["attributes"].get("item_type") or "",
                               "length": int(i["attributes"].get("length") or 0)}
                        break

        # run sheet
        items = []
        cur_seq = None
        if current and current["id"] in items_by_id:
            cur_seq = items_by_id[current["id"]]["attributes"].get("sequence")
        service_start = (running or upcoming or (services[0] if services else {}) or {}).get("starts_at")
        # "pre" items run before the service start (PCO counts them backwards),
        # "during"/"post" items from the service start on.
        planned = service_start
        if planned is not None:
            pre_len = sum(int(i["attributes"].get("length") or 0) for i in self._items
                          if i["attributes"].get("service_position") == "pre"
                          and i["attributes"].get("item_type") != "header")
            planned -= pre_len * 1000
        for i in self._items:
            a = i["attributes"]
            seq = a.get("sequence") or 0
            if cur_seq is None:
                status = "upcoming"
            elif seq < cur_seq:
                status = "done"
            elif seq == cur_seq:
                status = "current"
            else:
                status = "upcoming"
            items.append({
                "id": i["id"],
                "title": a.get("title") or "",
                "type": a.get("item_type") or "",
                "length": int(a.get("length") or 0),
                "position": a.get("service_position") or "",
                "key": a.get("key_name") or "",
                "status": status,
                "planned_start": planned,
            })
            if planned is not None and a.get("item_type") != "header":
                planned += int(a.get("length") or 0) * 1000

        return {
            "ok": True,
            "error": None,
            "plan": {
                "id": plan["id"],
                "title": pa.get("title") or "",
                "series_title": pa.get("series_title") or "",
                "dates": pa.get("dates") or "",
                "total_length": pa.get("total_length") or 0,
            },
            "live": {
                "active": current is not None,
                "current": current,
                "next": nxt,
                "service": running,
                "upcoming_service": upcoming,
            },
            "items": items,
        }


# --------------------------------------------------------------------------
# Demo data (to design/test the overlays without credentials)
# --------------------------------------------------------------------------
_DEMO_T0 = time.time()


def demo_state():
    titles = [
        ("Pre-Service", "header", 0), ("Countdown-Video", "media", 300),
        ("Begrüssung", "item", 180), ("Lobpreis", "header", 0),
        ("Way Maker", "song", 330), ("Build My Life", "song", 300),
        ("Gebet", "item", 120), ("Predigt: Hoffnung, die trägt", "item", 1800),
        ("Abschlusslied", "song", 240), ("Segen", "item", 60),
    ]
    n = now_ms()
    cycle = 90  # demo: every item lasts 90 s
    elapsed = int(time.time() - _DEMO_T0)
    playable = [i for i, t in enumerate(titles) if t[1] != "header"]
    pre = 20  # 20 s pre-service countdown at start of demo
    service_start = int((_DEMO_T0 + pre) * 1000)
    items = []
    current = nxt = None
    cur_idx = None
    if elapsed >= pre:
        k = ((elapsed - pre) // cycle) % len(playable)
        cur_idx = playable[k]
        start = n - ((elapsed - pre) % cycle) * 1000
        t = titles[cur_idx]
        current = {"id": str(cur_idx), "title": t[0], "type": t[1], "description": "",
                   "key": "G" if t[1] == "song" else "", "length": 75, "start": start,
                   "end": None, "plan_time_id": "1"}
        n_idx = playable[(k + 1) % len(playable)]
        nxt = {"id": str(n_idx), "title": titles[n_idx][0], "type": titles[n_idx][1],
               "length": titles[n_idx][2]}
    planned = service_start
    for i, (title, typ, length) in enumerate(titles):
        status = "upcoming" if cur_idx is None else ("done" if i < cur_idx else "current" if i == cur_idx else "upcoming")
        items.append({"id": str(i), "title": title, "type": typ, "length": length, "position": "",
                      "key": "G" if typ == "song" else "", "status": status, "planned_start": planned})
        if typ != "header":
            planned += length * 1000
    service = {"id": "1", "name": "Celebration", "type": "service", "starts_at": service_start,
               "ends_at": service_start + 75 * 60000, "live_starts_at": service_start if current else None,
               "live_ends_at": None}
    return {
        "ok": True, "error": None,
        "plan": {"id": "demo", "title": "Demo-Plan", "series_title": "Herbstserie",
                 "dates": datetime.now().strftime("%d. %B %Y"), "total_length": 3600},
        "live": {"active": current is not None, "current": current, "next": nxt,
                 "service": service if current else None,
                 "upcoming_service": None if current else service},
        "items": items,
    }


# --------------------------------------------------------------------------
# HTTP server (overlay + JSON + view switching for Companion/Stream Deck)
# --------------------------------------------------------------------------
WORKER = Worker()


class Handler(BaseHTTPRequestHandler):
    server_version = "PCOLive/" + VERSION

    def log_message(self, fmt, *args):  # keep the OBS log clean
        pass

    def _send(self, code, body, ctype):
        data = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        path = url.path.rstrip("/") or "/"
        if path in ("/", "/overlay"):
            try:
                with open(OVERLAY_FILE, "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
            except OSError:
                self._send(500, "overlay.html nicht gefunden", "text/plain; charset=utf-8")
        elif path == "/state":
            self._send(200, json.dumps(WORKER.snapshot()), "application/json")
        elif path.startswith("/api/view/"):
            view = path[len("/api/view/"):]
            ok = WORKER.set_view(view)
            self._send(200 if ok else 400, json.dumps({"ok": ok, "view": WORKER.view}), "application/json")
        elif path == "/api/view":
            self._send(200, json.dumps({"view": WORKER.view, "views": VIEW_KEYS}), "application/json")
        elif path == "/api/refresh":
            WORKER.refresh()
            self._send(200, json.dumps({"ok": True}), "application/json")
        else:
            self._send(404, "not found", "text/plain")


class Server:
    def __init__(self):
        self.httpd = None
        self.thread = None
        self.bind = None
        self.error = None

    def ensure(self, port, lan):
        bind = ("0.0.0.0" if lan else "127.0.0.1", int(port))
        if self.httpd and self.bind == bind:
            return
        self.stop()
        try:
            ThreadingHTTPServer.allow_reuse_address = True
            self.httpd = ThreadingHTTPServer(bind, Handler)
            self.httpd.daemon_threads = True
            self.thread = threading.Thread(target=self.httpd.serve_forever, name="pco-http", daemon=True)
            self.thread.start()
            self.bind = bind
            self.error = None
            log("Overlay-Server läuft auf http://%s:%s/overlay" % ("127.0.0.1" if not lan else "<IP>", port))
        except OSError as e:
            self.httpd = None
            self.bind = None
            self.error = "Port %s nicht verfügbar: %s" % (port, e)
            log(self.error)

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
            if self.thread:
                self.thread.join(timeout=2)
        self.httpd = None
        self.thread = None
        self.bind = None


SERVER = Server()


# ==========================================================================
# OBS integration
# ==========================================================================
SOURCE_NAME = "Planning Center Live"
_hotkeys = {}
_settings = None


def script_description():
    return (
        "<h3>Planning Center Live %s</h3>"
        "<p>Liest den Live-Status direkt über die Planning-Center-API "
        "(Personal Access Token, kein Login/Cookies) und stellt Overlays als "
        "Browser-Quelle bereit: Lower Third mit Countdown, grosser Countdown, "
        "Ablauf, Gottesdienst-Countdown und Uhr.</p>"
        "<p>Token erstellen: <a href='https://api.planningcenteronline.com/oauth/applications'>"
        "api.planningcenteronline.com/oauth/applications</a> → Personal Access Tokens.</p>"
        "<p>meyer.productions</p>" % VERSION
    )


def script_defaults(settings):
    obs.obs_data_set_default_int(settings, "poll_seconds", 2)
    obs.obs_data_set_default_int(settings, "port", 8765)
    obs.obs_data_set_default_string(settings, "view", "lowerthird")
    obs.obs_data_set_default_string(settings, "accent", "#e4002b")
    obs.obs_data_set_default_int(settings, "warn_seconds", 30)
    obs.obs_data_set_default_bool(settings, "show_next", True)
    obs.obs_data_set_default_bool(settings, "show_progress", True)
    obs.obs_data_set_default_int(settings, "preservice_minutes", 30)


def _cfg_from_settings(settings):
    c = Config()
    g = obs.obs_data_get_string
    # empty fields fall back to .env next to the script (or one level up),
    # so the token does not have to be stored in the scene collection
    _load_dotenv()
    c.app_id = g(settings, "app_id").strip() or os.environ.get("PCO_APP_ID", "")
    c.secret = g(settings, "secret").strip() or os.environ.get("PCO_SECRET", "")
    c.service_type_id = g(settings, "service_type_id").strip() or os.environ.get("PCO_SERVICE_TYPE", "")
    c.plan_id = g(settings, "plan_id").strip()
    c.poll_seconds = obs.obs_data_get_int(settings, "poll_seconds")
    c.port = obs.obs_data_get_int(settings, "port")
    c.lan = obs.obs_data_get_bool(settings, "lan")
    c.demo = obs.obs_data_get_bool(settings, "demo")
    c.accent = g(settings, "accent") or "#e4002b"
    c.warn_seconds = obs.obs_data_get_int(settings, "warn_seconds")
    c.show_next = obs.obs_data_get_bool(settings, "show_next")
    c.show_progress = obs.obs_data_get_bool(settings, "show_progress")
    c.preservice_minutes = obs.obs_data_get_int(settings, "preservice_minutes")
    return c


def script_update(settings):
    global _settings
    _settings = settings
    cfg = _cfg_from_settings(settings)
    SERVER.ensure(cfg.port, cfg.lan)
    WORKER.reconfigure(cfg)
    view = obs.obs_data_get_string(settings, "view")
    if view and view != WORKER.view:
        WORKER.set_view(view)


def _load_service_types(props, prop):
    cfg = WORKER.cfg
    if not cfg.has_credentials():
        log("Zuerst App ID und Secret eintragen.")
        return False
    try:
        WORKER.service_types_cache = PcoClient(cfg.app_id, cfg.secret).service_types()
        log("%d Service Types geladen." % len(WORKER.service_types_cache))
    except PcoError as e:
        log("Service Types konnten nicht geladen werden: %s" % e)
        return False
    lst = obs.obs_properties_get(props, "service_type_id")
    _fill_service_types(lst)
    return True


def _fill_service_types(lst):
    obs.obs_property_list_clear(lst)
    obs.obs_property_list_add_string(lst, "– bitte wählen –", "")
    current = WORKER.cfg.service_type_id
    ids = [i for i, _ in WORKER.service_types_cache]
    for sid, name in WORKER.service_types_cache:
        obs.obs_property_list_add_string(lst, name, sid)
    if current and current not in ids:
        obs.obs_property_list_add_string(lst, "ID %s" % current, current)


def _create_source(props, prop):
    port = WORKER.cfg.port
    scene_src = obs.obs_frontend_get_current_scene()
    if not scene_src:
        return False
    scene = obs.obs_scene_from_source(scene_src)
    existing = obs.obs_get_source_by_name(SOURCE_NAME)
    if existing:
        settings = obs.obs_source_get_settings(existing)
        obs.obs_data_set_string(settings, "url", "http://127.0.0.1:%d/overlay" % port)
        obs.obs_source_update(existing, settings)
        obs.obs_data_release(settings)
        if not obs.obs_scene_find_source(scene, SOURCE_NAME):
            obs.obs_scene_add(scene, existing)
        obs.obs_source_release(existing)
        log("Browser-Quelle '%s' aktualisiert." % SOURCE_NAME)
    else:
        settings = obs.obs_data_create()
        obs.obs_data_set_string(settings, "url", "http://127.0.0.1:%d/overlay" % port)
        obs.obs_data_set_int(settings, "width", 1920)
        obs.obs_data_set_int(settings, "height", 1080)
        obs.obs_data_set_bool(settings, "shutdown", False)
        obs.obs_data_set_bool(settings, "restart_when_active", False)
        src = obs.obs_source_create("browser_source", SOURCE_NAME, settings, None)
        obs.obs_scene_add(scene, src)
        obs.obs_source_release(src)
        obs.obs_data_release(settings)
        log("Browser-Quelle '%s' erstellt." % SOURCE_NAME)
    obs.obs_source_release(scene_src)
    return False


def _refresh(props, prop):
    WORKER.refresh()
    s = WORKER.snapshot()
    plan = s.get("plan") or {}
    log("Status: %s | Plan: %s %s | Ansicht: %s" % (
        "OK" if s.get("ok") else ("Fehler: %s" % s.get("error")),
        plan.get("title") or "-", plan.get("dates") or "", s.get("view")))
    if SERVER.error:
        log(SERVER.error)
    return False


def script_properties():
    props = obs.obs_properties_create()

    obs.obs_properties_add_text(props, "app_id", "Application ID", obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_text(props, "secret", "Secret", obs.OBS_TEXT_PASSWORD)
    lst = obs.obs_properties_add_list(props, "service_type_id", "Service Type",
                                      obs.OBS_COMBO_TYPE_EDITABLE, obs.OBS_COMBO_FORMAT_STRING)
    if not WORKER.service_types_cache and WORKER.cfg.has_credentials():
        try:
            WORKER.service_types_cache = PcoClient(WORKER.cfg.app_id, WORKER.cfg.secret).service_types()
        except PcoError as e:
            log("Service Types: %s" % e)
    _fill_service_types(lst)
    obs.obs_properties_add_button(props, "load_types", "Service Types laden", _load_service_types)
    p = obs.obs_properties_add_text(props, "plan_id", "Plan-ID (leer = automatisch nächster Plan)",
                                    obs.OBS_TEXT_DEFAULT)
    obs.obs_property_set_long_description(p, "Nur ausfüllen, um einen bestimmten Plan zu erzwingen.")

    views = obs.obs_properties_add_list(props, "view", "Aktive Ansicht",
                                        obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_STRING)
    for key, label in VIEWS:
        obs.obs_property_list_add_string(views, label, key)

    obs.obs_properties_add_text(props, "accent", "Akzentfarbe (Hex, z. B. #e4002b)", obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_int(props, "warn_seconds", "Warnung ab Restzeit (Sek.)", 0, 600, 5)
    obs.obs_properties_add_bool(props, "show_next", "Nächstes Element anzeigen")
    obs.obs_properties_add_bool(props, "show_progress", "Fortschrittsbalken anzeigen")
    obs.obs_properties_add_int(props, "preservice_minutes",
                               "Countdown zum Beginn ab (Min. vorher)", 0, 240, 5)

    obs.obs_properties_add_int(props, "poll_seconds", "Abfrage-Intervall (Sek.)", 1, 30, 1)
    obs.obs_properties_add_int(props, "port", "Lokaler Port", 1024, 65535, 1)
    obs.obs_properties_add_bool(props, "lan", "Im Netzwerk freigeben (Companion/anderer PC)")
    obs.obs_properties_add_bool(props, "demo", "Demo-Modus (ohne Planning Center)")

    obs.obs_properties_add_button(props, "create_source", "Browser-Quelle in aktuelle Szene einfügen",
                                  _create_source)
    obs.obs_properties_add_button(props, "refresh", "Neu laden + Status ins Skript-Log", _refresh)
    return props


def _make_hotkey_cb(view):
    def cb(pressed):
        if pressed:
            WORKER.set_view(view)
            if _settings is not None:
                obs.obs_data_set_string(_settings, "view", WORKER.view)
    return cb


def script_load(settings):
    global _settings
    _settings = settings
    entries = [("next", "Planning Center: Nächste Ansicht")] + \
              [(k, "Planning Center: %s" % label) for k, label in VIEWS]
    for key, desc in entries:
        hk = obs.obs_hotkey_register_frontend("pco_live_view_" + key, desc, _make_hotkey_cb(key))
        arr = obs.obs_data_get_array(settings, "hotkey_" + key)
        obs.obs_hotkey_load(hk, arr)
        obs.obs_data_array_release(arr)
        _hotkeys[key] = hk
    script_update(settings)
    WORKER.start()


def script_save(settings):
    obs.obs_data_set_string(settings, "view", WORKER.view)
    for key, hk in _hotkeys.items():
        arr = obs.obs_hotkey_save(hk)
        obs.obs_data_set_array(settings, "hotkey_" + key, arr)
        obs.obs_data_array_release(arr)


def script_unload():
    WORKER.stop()
    SERVER.stop()


# ==========================================================================
# Standalone mode (testing without OBS)
# ==========================================================================
def _load_dotenv():
    """Loads KEY=VALUE lines from .env (next to the script or one level up) into os.environ."""
    for path in (os.path.join(SCRIPT_DIR, ".env"), os.path.join(os.path.dirname(SCRIPT_DIR), ".env")):
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8-sig") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, val = line.split("=", 1)
                os.environ.setdefault(key.strip(), val.strip().strip('"').strip("'"))
        return path
    return None


def _standalone():
    import argparse
    _load_dotenv()
    ap = argparse.ArgumentParser(description="Planning Center Live overlay server (ohne OBS)")
    ap.add_argument("--demo", action="store_true", help="Demo-Daten statt API")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--lan", action="store_true")
    ap.add_argument("--service-type", default=os.environ.get("PCO_SERVICE_TYPE", ""))
    ap.add_argument("--plan", default="")
    ap.add_argument("--list-service-types", action="store_true")
    ap.add_argument("--view", default="lowerthird", choices=VIEW_KEYS)
    args = ap.parse_args()

    cfg = Config()
    cfg.app_id = os.environ.get("PCO_APP_ID", "")
    cfg.secret = os.environ.get("PCO_SECRET", "")
    cfg.service_type_id = args.service_type
    cfg.plan_id = args.plan
    cfg.port = args.port
    cfg.lan = args.lan
    cfg.demo = args.demo

    if args.list_service_types:
        if not cfg.has_credentials():
            sys.exit("PCO_APP_ID / PCO_SECRET fehlen (.env oder Umgebungsvariablen).")
        try:
            for sid, name in PcoClient(cfg.app_id, cfg.secret).service_types():
                print("%s\t%s" % (sid, name))
        except PcoError as e:
            sys.exit("Fehler: %s" % e)
        return

    SERVER.ensure(cfg.port, cfg.lan)
    if SERVER.error:
        sys.exit(1)
    WORKER.reconfigure(cfg)
    WORKER.set_view(args.view)
    WORKER.start()
    print("Overlay: http://127.0.0.1:%d/overlay   (Strg+C beendet)" % cfg.port, flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    WORKER.stop()
    SERVER.stop()


if __name__ == "__main__" and obs is None:
    _standalone()
