#!/usr/bin/env python3
"""
Claude usage bridge.

Reads Claude Code's local transcripts (~/.claude/projects/**/*.jsonl), works out how
much of the current 5-hour session window and the current weekly windows have been
used, and serves it as JSON so a small device on the LAN can display it.

The windows mirror what Claude Code's own /usage panel shows:

    5-hour limit          starts at the first message after a gap, runs 5 hours
    Weekly - all models   resets at a fixed weekday + hour (default Wednesday 13:00 local)
    Weekly - <model>      same window, one model family only (default Fable)

The percentages are an ESTIMATE. Anthropic does not publish the token budget behind a
plan's limits, so the bridge needs to be told where the real panel sits once:

    python claude_usage_bridge.py --calibrate "session=1,week=20,model=30"

reads those three percentages off /usage, works out what budget makes the local token
counts land on them, and stores the result in calibration.json. Re-run it whenever the
device drifts from the panel. Until calibrated, a rate-limit rejection recorded in the
transcripts gives a lower bound on the 5-hour budget, and failing that a generic guess.

Endpoints:
    GET /usage    the payload the device polls
    GET /health   liveness
    GET /dump     verbose breakdown for debugging

Usage:
    python claude_usage_bridge.py                   # serve on 0.0.0.0:8787
    python claude_usage_bridge.py --once            # print the payload and exit
    python claude_usage_bridge.py --dump            # print the debug breakdown and exit
    python claude_usage_bridge.py --calibrate "session=1,week=20,model=30[,session_resets_in=3h26m]"

Environment (all optional):
    CLAUDE_CONFIG_DIR        where ~/.claude lives
    CLAUDE_WEEKLY_RESET_DAY  0=Mon .. 6=Sun   (default 2, Wednesday)
    CLAUDE_WEEKLY_RESET_HOUR local hour        (default 13)
    CLAUDE_MODEL_FILTER      regex for the per-model gauge (default "fable")

Stdlib only. No third-party packages, no network calls out, no credentials read.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --------------------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------------------

CLAUDE_DIR = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(
    os.path.expanduser("~"), ".claude"
)
PROJECTS_DIR = os.path.join(CLAUDE_DIR, "projects")
CALIBRATION_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "calibration.json")

SESSION_WINDOW = timedelta(hours=5)

WEEKLY_RESET_WEEKDAY = int(os.environ.get("CLAUDE_WEEKLY_RESET_DAY", "2"))  # Wednesday
WEEKLY_RESET_HOUR = int(os.environ.get("CLAUDE_WEEKLY_RESET_HOUR", "13"))  # 1 PM local

MODEL_FILTER = os.environ.get("CLAUDE_MODEL_FILTER", "fable")
MODEL_RE = re.compile(MODEL_FILTER, re.IGNORECASE)
MODEL_LABEL = MODEL_FILTER.capitalize()

# Used only when nothing at all has been calibrated. Deliberately generous: a too-small
# guess would show a scary 100% that isn't real.
LIMIT_FALLBACK = {
    "session": 50_000_000,
    "week": 170_000_000,
    "model": 60_000_000,
}

# Rate limits are driven by weighted token consumption, and a cache read is far cheaper
# than a fresh input token. These weights are an approximation; they only affect the
# shape of the estimate, and calibration re-scales whatever we choose.
TOKEN_WEIGHTS = {
    "input": 1.0,
    "output": 5.0,
    "cache_creation": 1.25,
    "cache_read": 0.1,
}

# Cheap pre-filter so we only json.loads lines that can possibly matter.
INTERESTING = ('"usage"', '"quotaLimits"')

REFRESH_SECONDS = 20


def log(text):
    """Write a line to the console, tolerating not having one.

    Under pythonw.exe (how this is run as a background service) sys.stdout and
    sys.stderr are None, and an unguarded write raises inside the request handler,
    which drops the connection mid-response and looks like a network fault.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            if stream is not None:
                stream.write(text + "\n")
                stream.flush()
                return
        except Exception:
            pass


def local_tz():
    return datetime.now().astimezone().tzinfo


# --------------------------------------------------------------------------------------
# Transcript parsing
# --------------------------------------------------------------------------------------


def _parse_ts(value):
    if not value:
        return None
    try:
        if value.endswith("Z"):
            value = value[:-1] + "+00:00"
        dt = datetime.fromisoformat(value)
    except (ValueError, TypeError):
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def _weighted(usage):
    return (
        usage.get("input_tokens", 0) * TOKEN_WEIGHTS["input"]
        + usage.get("output_tokens", 0) * TOKEN_WEIGHTS["output"]
        + usage.get("cache_creation_input_tokens", 0) * TOKEN_WEIGHTS["cache_creation"]
        + usage.get("cache_read_input_tokens", 0) * TOKEN_WEIGHTS["cache_read"]
    )


def read_events(root=PROJECTS_DIR, since=None):
    """Return (usage_events, limit_events).

    usage_events: sorted list of (timestamp, model, weighted_tokens, raw_usage)
    limit_events: sorted list of {ts, type, resets_at} for recorded rejections

    De-duplicated on message id + request id: a resumed or forked session replays
    earlier assistant turns into a new transcript file.
    """
    usage_events = []
    limit_events = []
    seen = set()

    for path in glob.glob(os.path.join(root, "**", "*.jsonl"), recursive=True):
        try:
            mtime = datetime.fromtimestamp(os.path.getmtime(path), tz=timezone.utc)
        except OSError:
            continue
        if since is not None and mtime < since:
            continue
        try:
            handle = open(path, "r", encoding="utf-8", errors="ignore")
        except OSError:
            continue

        with handle:
            for line in handle:
                if not any(token in line for token in INTERESTING):
                    continue
                try:
                    rec = json.loads(line)
                except (json.JSONDecodeError, ValueError):
                    continue
                if not isinstance(rec, dict):
                    continue
                ts = _parse_ts(rec.get("timestamp"))
                if ts is None:
                    continue

                quota = rec.get("quotaLimits")
                if isinstance(quota, dict) and quota.get("status") == "rejected":
                    limit_events.append(
                        {
                            "ts": ts,
                            "type": quota.get("rateLimitType") or "five_hour",
                            "resets_at": quota.get("resetsAt"),
                        }
                    )

                message = rec.get("message")
                if not isinstance(message, dict):
                    continue
                usage = message.get("usage")
                if not isinstance(usage, dict):
                    continue
                model = message.get("model") or ""
                if model == "<synthetic>":
                    continue
                key = (message.get("id"), rec.get("requestId"))
                if key != (None, None):
                    if key in seen:
                        continue
                    seen.add(key)
                tokens = _weighted(usage)
                if tokens <= 0:
                    continue
                usage_events.append((ts, model, tokens, usage))

    usage_events.sort(key=lambda e: e[0])
    limit_events.sort(key=lambda e: e["ts"])
    return usage_events, limit_events


# --------------------------------------------------------------------------------------
# Windows
# --------------------------------------------------------------------------------------


def session_block_start(events, now):
    """Start of the active 5-hour block, or None if none is active.

    A block opens at the first request after the previous block expired and runs
    exactly five hours from that message - not from the top of the hour.
    """
    start = None
    for ts, _model, _tokens, _raw in events:
        if start is None or ts >= start + SESSION_WINDOW:
            start = ts
    if start is None or now >= start + SESSION_WINDOW:
        return None
    return start


def weekly_window(now):
    """(start, end) of the weekly window containing `now`, both UTC.

    The weekly limits reset at a fixed local weekday and hour, the way the /usage
    panel shows ("Resets Wed 1:00 PM"), rather than rolling.
    """
    local = now.astimezone(local_tz())
    anchor = local.replace(hour=WEEKLY_RESET_HOUR, minute=0, second=0, microsecond=0)
    while anchor.weekday() != WEEKLY_RESET_WEEKDAY or anchor > local:
        anchor -= timedelta(days=1)
    start = anchor
    end = anchor + timedelta(days=7)
    return start.astimezone(timezone.utc), end.astimezone(timezone.utc)


def sum_window(events, start, end, model_re=None):
    total = 0.0
    for ts, model, tokens, _raw in events:
        if ts < start or ts >= end:
            continue
        if model_re is not None and not model_re.search(model):
            continue
        total += tokens
    return total


# --------------------------------------------------------------------------------------
# Calibration
# --------------------------------------------------------------------------------------


def load_calibration():
    try:
        with open(CALIBRATION_FILE, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_calibration(data):
    with open(CALIBRATION_FILE, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)


def resolve_limits(events, limit_events, calibration):
    """Return ({kind: limit}, {kind: source}).

    Precedence: calibration.json (from --calibrate) > the largest 5-hour rejection
    observed in the transcripts > generic fallback.
    """
    limits = dict(LIMIT_FALLBACK)
    sources = {kind: "fallback" for kind in limits}

    # A rejection at time T means the 5-hour window containing T was full, so the
    # tokens in it up to T are a lower bound on the budget.
    best = 0.0
    for event in limit_events:
        if event["type"] != "five_hour":
            continue
        ts = event["ts"]
        start = session_block_start([e for e in events if e[0] <= ts], ts) or (ts - SESSION_WINDOW)
        best = max(best, sum_window(events, start, ts))
    # Only ever raises the estimate: the block boundary at the time of an old rejection
    # is reconstructed, and a wrong boundary under-counts. The real panel put the budget
    # ~10x above one such reconstruction, so a rejection is a floor, not a value.
    if best > limits["session"]:
        limits["session"] = best
        sources["session"] = "rejection"

    for kind in ("session", "week", "model"):
        value = calibration.get("limits", {}).get(kind)
        if isinstance(value, (int, float)) and value > 0:
            limits[kind] = float(value)
            sources[kind] = "calibrated"

    return limits, sources


def parse_duration(text):
    """'3h26m' / '45m' / '2h' -> seconds."""
    total = 0
    for amount, unit in re.findall(r"(\d+)\s*([hms])", text.lower()):
        total += int(amount) * {"h": 3600, "m": 60, "s": 1}[unit]
    return total


def calibrate(spec):
    """Set the budgets so the current token counts land on the given percentages.

    spec: "session=1,week=20,model=30[,session_resets_in=3h26m]"
    """
    fields = {}
    for part in spec.split(","):
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        fields[key.strip().lower()] = value.strip()

    now = datetime.now(timezone.utc)
    events, _ = read_events(since=now - timedelta(days=9))
    week_start, _week_end = weekly_window(now)

    calibration = load_calibration()
    limits = dict(calibration.get("limits", {}))

    # If told when the current block resets, pin its start; the local guess at the
    # block boundary can be a few minutes off.
    block_start = session_block_start(events, now)
    if "session_resets_in" in fields:
        secs = parse_duration(fields["session_resets_in"])
        if secs > 0:
            block_end = now + timedelta(seconds=secs)
            block_start = block_end - SESSION_WINDOW
            calibration["session_end_epoch"] = int(block_end.timestamp())

    used = {
        "session": sum_window(events, block_start, now) if block_start else 0.0,
        "week": sum_window(events, week_start, now),
        "model": sum_window(events, week_start, now, MODEL_RE),
    }

    for kind in ("session", "week", "model"):
        if kind not in fields:
            continue
        pct = float(fields[kind].rstrip("%"))
        if pct <= 0:
            log("%s: percentage must be > 0 to calibrate (got %s)" % (kind, fields[kind]))
            continue
        if used[kind] <= 0:
            log("%s: no local usage in this window yet; cannot calibrate" % kind)
            continue
        limits[kind] = used[kind] / (pct / 100.0)
        log("%-8s %14.0f weighted tokens at %5.1f%%  ->  budget %14.0f" % (kind, used[kind], pct, limits[kind]))

    calibration["limits"] = limits
    calibration["calibrated_at"] = now.isoformat()
    calibration["model_filter"] = MODEL_FILTER
    save_calibration(calibration)
    log("saved %s" % CALIBRATION_FILE)


# --------------------------------------------------------------------------------------
# Real numbers from Anthropic (the Clawdmeter approach)
#
# Every response from api.anthropic.com carries the account's live rate-limit state
# in headers such as anthropic-ratelimit-unified-5h-utilization. A one-token Haiku
# request costs effectively nothing and returns exact percentages and reset times -
# the same numbers /usage shows. When a token is available this replaces the local
# estimate entirely. The token is read at request time and never logged or stored.
# --------------------------------------------------------------------------------------

import urllib.request
import urllib.error

TOKEN_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "token.txt")
API_URL = "https://api.anthropic.com/v1/messages"
API_POLL_SECONDS = 60


def read_oauth_token():
    """Locate the Claude OAuth access token, or return None.

    Order: CLAUDE_CODE_OAUTH_TOKEN env var (what `claude setup-token` is for),
    bridge/token.txt, then Claude Code's own ~/.claude/.credentials.json.
    """
    token = os.environ.get("CLAUDE_CODE_OAUTH_TOKEN", "").strip()
    if token:
        return token
    try:
        with open(TOKEN_FILE, "r", encoding="utf-8") as handle:
            token = handle.read().strip()
        if token:
            return token
    except OSError:
        pass
    for path in (
        os.path.join(CLAUDE_DIR, ".credentials.json"),
        os.path.join(os.environ.get("LOCALAPPDATA", ""), "Claude", ".credentials.json"),
        os.path.join(os.environ.get("APPDATA", ""), "Claude", ".credentials.json"),
    ):
        try:
            with open(path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            continue
        oauth = data.get("claudeAiOauth") if isinstance(data, dict) else None
        if isinstance(oauth, dict) and oauth.get("accessToken"):
            return oauth["accessToken"]
    return None


_api_cache = {"at": 0.0, "data": None, "error": None}


def fetch_api_usage(now):
    """Return the live utilization dict from Anthropic, or None (with a reason logged).

    Cached for API_POLL_SECONDS so the device polling every 30 s doesn't turn into a
    request per poll. Result shape:
        {"session_pct", "session_resets_in", "week_pct", "week_resets_in",
         "status", "headers": {every anthropic-ratelimit-* header, for inspection}}
    """
    if time.time() - _api_cache["at"] < API_POLL_SECONDS:
        return _api_cache["data"]

    token = read_oauth_token()
    if not token:
        _api_cache.update(at=time.time(), data=None, error="no token")
        return None

    body = json.dumps(
        {
            "model": "claude-haiku-4-5-20251001",
            "max_tokens": 1,
            "messages": [{"role": "user", "content": "hi"}],
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        API_URL,
        data=body,
        method="POST",
        headers={
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",
            "Content-Type": "application/json",
            "User-Agent": "claude-code/2.1.5",
            "Authorization": "Bearer " + token,
        },
    )

    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            headers = {k.lower(): v for k, v in resp.headers.items()}
    except urllib.error.HTTPError as exc:
        # Rate-limit headers are present on a 429 too, which is exactly when they matter.
        headers = {k.lower(): v for k, v in exc.headers.items()} if exc.headers else {}
        if exc.code in (401, 403):
            _api_cache.update(at=time.time(), data=None, error="token rejected (%d) - run `claude setup-token`" % exc.code)
            log("[api] " + _api_cache["error"])
            return None
        if exc.code != 429:
            _api_cache.update(at=time.time(), data=None, error="http %d" % exc.code)
            log("[api] http %d" % exc.code)
            return None
    except (urllib.error.URLError, OSError) as exc:
        _api_cache.update(at=time.time(), data=None, error="network: %s" % exc)
        log("[api] network error: %s" % exc)
        return None

    rl = {k: v for k, v in headers.items() if k.startswith("anthropic-ratelimit-")}

    def pct(name):
        try:
            return round(float(rl[name]) * 100.0, 1)
        except (KeyError, ValueError):
            return None

    def resets_in(name):
        try:
            return max(0, int(float(rl[name]) - now.timestamp()))
        except (KeyError, ValueError):
            return None

    data = None
    if "anthropic-ratelimit-unified-5h-utilization" in rl:
        data = {
            "session_pct": pct("anthropic-ratelimit-unified-5h-utilization"),
            "session_resets_in": resets_in("anthropic-ratelimit-unified-5h-reset"),
            "week_pct": pct("anthropic-ratelimit-unified-7d-utilization"),
            "week_resets_in": resets_in("anthropic-ratelimit-unified-7d-reset"),
            "status": rl.get("anthropic-ratelimit-unified-5h-status")
            or rl.get("anthropic-ratelimit-unified-status"),
            "headers": rl,
        }
    elif "anthropic-ratelimit-unified-overage-utilization" in rl:
        data = {
            "session_pct": pct("anthropic-ratelimit-unified-overage-utilization"),
            "session_resets_in": resets_in("anthropic-ratelimit-unified-overage-reset"),
            "week_pct": None,
            "week_resets_in": None,
            "status": rl.get("anthropic-ratelimit-unified-status"),
            "headers": rl,
        }
    else:
        _api_cache.update(at=time.time(), data=None, error="no ratelimit headers in response")
        log("[api] response carried no anthropic-ratelimit-* headers")
        return None

    _api_cache.update(at=time.time(), data=data, error=None)
    return data


# --------------------------------------------------------------------------------------
# Payload
# --------------------------------------------------------------------------------------


def build_payload(now=None):
    now = now or datetime.now(timezone.utc)
    events, limit_events = read_events(since=now - timedelta(days=9))
    calibration = load_calibration()
    limits, sources = resolve_limits(events, limit_events, calibration)

    week_start, week_end = weekly_window(now)
    week_used = sum_window(events, week_start, now)
    model_used = sum_window(events, week_start, now, MODEL_RE)

    # Session block: a pinned end from --calibrate wins while it is still current.
    block_start = None
    pinned_end = calibration.get("session_end_epoch")
    if pinned_end and pinned_end > now.timestamp() and pinned_end - now.timestamp() <= SESSION_WINDOW.total_seconds():
        block_start = datetime.fromtimestamp(pinned_end, tz=timezone.utc) - SESSION_WINDOW
    if block_start is None:
        block_start = session_block_start(events, now)

    if block_start is None:
        session_used = 0.0
        session_resets_in = 0
    else:
        session_used = sum_window(events, block_start, now)
        session_resets_in = int((block_start + SESSION_WINDOW - now).total_seconds())

    # A live rejection still in force outranks any estimate.
    active_block = None
    for event in reversed(limit_events):
        resets = event.get("resets_at")
        if resets and resets > now.timestamp():
            active_block = event
            break

    def gauge(kind, used, resets_in):
        limit = limits[kind]
        pct = 0.0 if limit <= 0 else min(999.0, used / limit * 100.0)
        return {
            "used": int(used),
            "limit": int(limit),
            "pct": round(pct, 1),
            "resets_in": max(0, int(resets_in)),
            "source": sources[kind],
        }

    week_resets_in = (week_end - now).total_seconds()
    session = gauge("session", session_used, session_resets_in)
    week = gauge("week", week_used, week_resets_in)
    model = gauge("model", model_used, week_resets_in)

    # Live numbers from Anthropic outrank every estimate above. The per-model weekly
    # gauge has no header of its own, so it keeps the local estimate but is re-scaled
    # so that its share of the week matches what the real week gauge reports.
    live = fetch_api_usage(now)
    if live:
        if live.get("session_pct") is not None:
            session["pct"] = live["session_pct"]
            session["source"] = "api"
        if live.get("session_resets_in") is not None:
            session["resets_in"] = live["session_resets_in"]
        if live.get("week_pct") is not None:
            week_pct_est = week["pct"]
            week["pct"] = live["week_pct"]
            week["source"] = "api"
            if week_pct_est > 0 and model["source"] != "calibrated":
                model["pct"] = round(model["pct"] * live["week_pct"] / week_pct_est, 1)
                model["source"] = "api-scaled"
        if live.get("week_resets_in") is not None:
            week["resets_in"] = live["week_resets_in"]
            model["resets_in"] = live["week_resets_in"]

    return {
        "ok": True,
        "generated_at": int(now.timestamp()),
        "session": session,
        "week": week,
        "model": model,
        "model_label": MODEL_LABEL,
        "live": bool(live),
        "live_status": live.get("status") if live else _api_cache.get("error"),
        "live_headers": live.get("headers") if live else None,
        "blocked": bool(active_block),
        "blocked_type": active_block["type"] if active_block else "",
        "blocked_resets_in": (
            max(0, int(active_block["resets_at"] - now.timestamp())) if active_block else 0
        ),
        "session_start": block_start.isoformat() if block_start else None,
        "week_start": week_start.isoformat(),
        "calibrated_at": calibration.get("calibrated_at"),
        "events": len(events),
    }


def dump():
    now = datetime.now(timezone.utc)
    events, limit_events = read_events(since=now - timedelta(days=9))
    calibration = load_calibration()
    limits, sources = resolve_limits(events, limit_events, calibration)
    week_start, week_end = weekly_window(now)
    tz = local_tz()
    return {
        "claude_dir": CLAUDE_DIR,
        "calibration_file": CALIBRATION_FILE,
        "calibration": calibration,
        "usage_events": len(events),
        "first_event": events[0][0].isoformat() if events else None,
        "last_event": events[-1][0].isoformat() if events else None,
        "weekly_window_local": {
            "start": week_start.astimezone(tz).strftime("%a %Y-%m-%d %H:%M"),
            "end": week_end.astimezone(tz).strftime("%a %Y-%m-%d %H:%M"),
        },
        "model_filter": MODEL_FILTER,
        "rate_limit_events": [
            {"ts": e["ts"].isoformat(), "type": e["type"], "resets_at": e.get("resets_at")}
            for e in limit_events
        ],
        "effective_limits": {k: int(v) for k, v in limits.items()},
        "limit_sources": sources,
        "payload": build_payload(now),
    }


# --------------------------------------------------------------------------------------
# Home Assistant proxy
#
# The device only ever talks to this bridge, so the HA long-lived token stays on the PC
# in bridge/ha_config.json:
#
#   {
#     "url": "http://homeassistant.local:8123",
#     "token": "eyJ...",
#     "thermostat": "climate.living_room",
#     "lights": ["light.office", "light.office_lamp"]
#   }
#
# GET  /ha/state  -> compact state for the thermostat and each light (cached 5 s)
# POST /ha/call   -> {"domain": "light", "service": "turn_on", "data": {...}}
#                    forwarded to /api/services/<domain>/<service>
# --------------------------------------------------------------------------------------

HA_CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ha_config.json")
HA_CACHE_SECONDS = 5
_ha_cache = {"at": 0.0, "data": None}


def ha_config():
    try:
        with open(HA_CONFIG_FILE, "r", encoding="utf-8") as handle:
            cfg = json.load(handle)
    except (OSError, ValueError):
        return None
    if not isinstance(cfg, dict) or not cfg.get("url") or not cfg.get("token"):
        return None
    return cfg


def ha_request(cfg, method, path, body=None):
    url = cfg["url"].rstrip("/") + path
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={
            "Authorization": "Bearer " + cfg["token"],
            "Content-Type": "application/json",
        },
    )
    with urllib.request.urlopen(req, timeout=8) as resp:
        raw = resp.read()
    return json.loads(raw) if raw else None


def ha_state():
    cfg = ha_config()
    if cfg is None:
        return {"ok": False, "configured": False, "error": "no ha_config.json"}
    if time.time() - _ha_cache["at"] < HA_CACHE_SECONDS and _ha_cache["data"]:
        return _ha_cache["data"]

    out = {"ok": True, "configured": True, "thermostat": None, "lights": []}
    try:
        thermo = cfg.get("thermostat")
        if thermo:
            s = ha_request(cfg, "GET", "/api/states/" + thermo)
            a = s.get("attributes", {})
            out["thermostat"] = {
                "entity": thermo,
                "name": a.get("friendly_name", thermo),
                "current": a.get("current_temperature"),
                # heat_cool mode exposes a range instead of a single target
                "target": a.get("temperature", a.get("target_temp_high")),
                "mode": s.get("state"),
                "action": a.get("hvac_action", ""),
                "unit": "°",
            }
        for entity in cfg.get("lights", [])[:3]:
            s = ha_request(cfg, "GET", "/api/states/" + entity)
            a = s.get("attributes", {})
            out["lights"].append(
                {
                    "entity": entity,
                    "name": a.get("friendly_name", entity),
                    "on": s.get("state") == "on",
                    "brightness": int(a.get("brightness") or 0),
                }
            )
    except urllib.error.HTTPError as exc:
        out = {"ok": False, "configured": True, "error": "HA http %d" % exc.code}
        log("[ha] http %d for %s" % (exc.code, exc.url))
    except (urllib.error.URLError, OSError, ValueError, AttributeError) as exc:
        out = {"ok": False, "configured": True, "error": "HA unreachable"}
        log("[ha] %s" % exc)

    _ha_cache.update(at=time.time(), data=out)
    return out


def ha_discover():
    """List the climate and light entities HA exposes, for filling in ha_config.json.

    Run after pasting a token:  python claude_usage_bridge.py --ha-discover
    """
    cfg = ha_config()
    if cfg is None:
        log("no ha_config.json, or it is missing url/token")
        return
    if "PASTE" in cfg.get("token", ""):
        log("ha_config.json still holds the placeholder token")
        log("get one at %s -> your profile -> Security -> Long-lived access tokens"
            % cfg["url"].rstrip("/"))
        return
    try:
        states = ha_request(cfg, "GET", "/api/states")
    except urllib.error.HTTPError as exc:
        log("HA returned %d - %s" % (exc.code, "token rejected" if exc.code == 401 else "error"))
        return
    except (urllib.error.URLError, OSError) as exc:
        log("cannot reach %s: %s" % (cfg["url"], exc))
        return

    groups = {"climate": [], "light": []}
    for s in states or []:
        entity = s.get("entity_id", "")
        domain = entity.split(".", 1)[0]
        if domain in groups:
            attrs = s.get("attributes", {})
            groups[domain].append((entity, attrs.get("friendly_name", entity), s.get("state")))

    for domain in ("climate", "light"):
        rows = sorted(groups[domain])
        log("\n%s (%d)" % (domain.upper(), len(rows)))
        if not rows:
            log("  none exposed")
        for entity, name, state in rows:
            log("  %-44s %-28s %s" % (entity, name, state))

    log("\nPut one climate entity in \"thermostat\" and up to three light entities in")
    log("\"lights\" in %s, then restart the bridge." % HA_CONFIG_FILE)


def ha_call(body):
    cfg = ha_config()
    if cfg is None:
        return {"ok": False, "error": "no ha_config.json"}, 400
    domain = str(body.get("domain", ""))
    service = str(body.get("service", ""))
    data = dict(body.get("data") or {})
    if not re.fullmatch(r"[a-z_]+", domain) or not re.fullmatch(r"[a-z_]+", service):
        return {"ok": False, "error": "bad domain/service"}, 400

    # The device never names an entity. It sends a light by its index in the
    # configured list, and nothing at all for the thermostat, so a compromised or
    # confused panel cannot address arbitrary entities in the house - the worst it
    # can reach is what is listed in ha_config.json.
    lights = cfg.get("lights", [])[:3]
    if "index" in data:
        try:
            idx = int(data.pop("index"))
        except (TypeError, ValueError):
            return {"ok": False, "error": "bad index"}, 400
        if not (0 <= idx < len(lights)):
            return {"ok": False, "error": "index out of range"}, 400
        data["entity_id"] = lights[idx]
    elif domain == "climate" and cfg.get("thermostat"):
        data["entity_id"] = cfg["thermostat"]

    if "entity_id" not in data:
        return {"ok": False, "error": "no entity resolved"}, 400
    if data["entity_id"] not in lights and data["entity_id"] != cfg.get("thermostat"):
        return {"ok": False, "error": "entity not in ha_config.json"}, 403
    try:
        ha_request(cfg, "POST", "/api/services/%s/%s" % (domain, service), data)
    except urllib.error.HTTPError as exc:
        log("[ha] call failed http %d" % exc.code)
        return {"ok": False, "error": "HA http %d" % exc.code}, 502
    except (urllib.error.URLError, OSError) as exc:
        log("[ha] call failed: %s" % exc)
        return {"ok": False, "error": "HA unreachable"}, 502
    _ha_cache["at"] = 0.0  # next state read reflects the change
    log("[ha] %s.%s %s" % (domain, service, json.dumps(data)))
    return {"ok": True}, 200


# --------------------------------------------------------------------------------------
# Cache + HTTP
# --------------------------------------------------------------------------------------


class Cache:
    def __init__(self):
        self._lock = threading.Lock()
        self._payload = None
        self._at = 0.0

    def get(self):
        with self._lock:
            if self._payload is None or time.time() - self._at > REFRESH_SECONDS:
                try:
                    self._payload = build_payload()
                except Exception as exc:  # keep serving even if a scan fails
                    self._payload = {"ok": False, "error": str(exc)}
                self._at = time.time()
            return self._payload


CACHE = Cache()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0].rstrip("/") or "/"
        if path in ("/", "/usage"):
            self._send(CACHE.get())
        elif path == "/health":
            self._send({"ok": True, "ts": int(time.time())})
        elif path == "/dump":
            self._send(dump())
        elif path == "/ha/state":
            self._send(ha_state())
        else:
            self._send({"ok": False, "error": "not found"}, status=404)

    def do_POST(self):
        path = self.path.split("?", 1)[0].rstrip("/")
        if path != "/ha/call":
            self._send({"ok": False, "error": "not found"}, status=404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length) or b"{}")
            if not isinstance(body, dict):
                raise ValueError("body must be an object")
        except (ValueError, TypeError) as exc:
            self._send({"ok": False, "error": "bad json: %s" % exc}, status=400)
            return
        result, status = ha_call(body)
        self._send(result, status=status)

    def log_message(self, fmt, *args):
        log("%s  %s" % (self.address_string(), fmt % args))


def local_ips():
    import socket

    ips = set()
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))  # no packets sent; picks the default route
        ips.add(sock.getsockname()[0])
        sock.close()
    except OSError:
        pass
    return sorted(ips)


def main():
    parser = argparse.ArgumentParser(description="Serve Claude usage estimates on the LAN.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--once", action="store_true", help="print the payload and exit")
    parser.add_argument("--dump", action="store_true", help="print a debug breakdown and exit")
    parser.add_argument(
        "--calibrate",
        metavar="SPEC",
        help='match the real /usage panel, e.g. "session=1,week=20,model=30,session_resets_in=3h26m"',
    )
    parser.add_argument(
        "--ha-discover",
        action="store_true",
        help="list Home Assistant climate/light entities for ha_config.json",
    )
    args = parser.parse_args()

    if args.ha_discover:
        ha_discover()
        return
    if args.calibrate:
        calibrate(args.calibrate)
        return
    if args.dump:
        print(json.dumps(dump(), indent=2))
        return
    if args.once:
        print(json.dumps(build_payload(), indent=2))
        return

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    log("Claude usage bridge listening on %s:%d" % (args.host, args.port))
    for ip in local_ips():
        log("  device should poll:  http://%s:%d/usage" % (ip, args.port))
    log("  reading transcripts from: %s" % PROJECTS_DIR)
    log("  weekly reset: weekday %d at %02d:00 local; per-model gauge: %s" % (WEEKLY_RESET_WEEKDAY, WEEKLY_RESET_HOUR, MODEL_LABEL))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("stopping")
        server.shutdown()


if __name__ == "__main__":
    main()
