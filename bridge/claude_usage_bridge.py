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
    GET /weather  current conditions, 5-day forecast and the local time (see below)
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
import urllib.parse

TOKEN_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "token.txt")
API_URL = "https://api.anthropic.com/v1/messages"
API_POLL_SECONDS = 60


CRED_FILES = [
    os.path.join(CLAUDE_DIR, ".credentials.json"),
    os.path.join(os.environ.get("LOCALAPPDATA", ""), "Claude", ".credentials.json"),
    os.path.join(os.environ.get("APPDATA", ""), "Claude", ".credentials.json"),
]


def read_oauth_token():
    """Locate the Claude OAuth access token as (token, source), or (None, None).

    Order: CLAUDE_CODE_OAUTH_TOKEN env var (what `claude setup-token` is for),
    bridge/token.txt, then Claude Code's own ~/.claude/.credentials.json.

    The source matters operationally, not just for debugging - see credential_status().
    The first two are tokens you minted deliberately and that nothing else rotates. The
    third is Claude Code's own working credential: it is refreshed roughly hourly and
    expires outright about a week after Claude Code was last run, so a panel resting on
    it goes dark during any quiet week. Callers re-read this per fetch; never cache it.
    """
    token = os.environ.get("CLAUDE_CODE_OAUTH_TOKEN", "").strip()
    if token:
        return token, "env"
    try:
        with open(TOKEN_FILE, "r", encoding="utf-8") as handle:
            token = handle.read().strip()
        if token:
            return token, "token.txt"
    except OSError:
        pass
    for path in CRED_FILES:
        if not path:
            continue
        try:
            with open(path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            continue
        oauth = data.get("claudeAiOauth") if isinstance(data, dict) else None
        if isinstance(oauth, dict) and oauth.get("accessToken"):
            return oauth["accessToken"], "credentials.json"
    return None, None


def _cred_file_expiry():
    """Seconds until Claude Code's own credential expires outright, or None.

    Reads refreshTokenExpiresAt from .credentials.json. The access token beside it
    rotates hourly and is not worth warning about; the refresh token is the real cliff.
    """
    for path in CRED_FILES:
        if not path:
            continue
        try:
            with open(path, "r", encoding="utf-8") as handle:
                oauth = (json.load(handle) or {}).get("claudeAiOauth") or {}
        except (OSError, ValueError, AttributeError):
            continue
        ms = oauth.get("refreshTokenExpiresAt")
        if isinstance(ms, (int, float)):
            return int(ms / 1000 - time.time())
    return None


# Warn once a credential is inside this window of expiring.
CRED_WARN_SECONDS = 2 * 24 * 3600


def credential_status():
    """Where the token came from, and whether it is about to strand the panel.

    Returned in /dump and logged at startup so the answer to "why did the gauges go
    blank" is available before they do, not only afterwards.
    """
    token, source = read_oauth_token()
    if not token:
        return {
            "source": None,
            "ok": False,
            "expires_in_days": None,
            "detail": "no token - run `claude setup-token` and save it to bridge/token.txt",
        }
    if source != "credentials.json":
        return {
            "source": source,
            "ok": True,
            "expires_in_days": None,
            "detail": "token you minted; nothing else rotates it",
        }
    left = _cred_file_expiry()
    if left is None:
        detail = "borrowing Claude Code's credential (expiry unknown)"
        return {"source": source, "ok": True, "expires_in_days": None, "detail": detail}
    days = round(left / 86400.0, 1)
    if left <= 0:
        return {
            "source": source,
            "ok": False,
            "expires_in_days": days,
            "detail": "Claude Code's credential has expired - run `claude setup-token` "
                      "and save it to bridge/token.txt",
        }
    detail = (
        "borrowing Claude Code's credential, expires in %.1f days; it renews only while "
        "you keep using Claude Code. Run `claude setup-token` into bridge/token.txt to "
        "stop depending on that." % days
    )
    return {
        "source": source,
        "ok": left > CRED_WARN_SECONDS,
        "expires_in_days": days,
        "detail": detail,
    }


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

    token, token_source = read_oauth_token()
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
            hint = (
                " - Claude Code's credential lapsed; run `claude setup-token` and save it "
                "to bridge/token.txt so the panel stops depending on it"
                if token_source == "credentials.json"
                else " - run `claude setup-token`"
            )
            _api_cache.update(
                at=time.time(), data=None,
                error="token rejected (%d, from %s)%s" % (exc.code, token_source, hint),
            )
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
        "credential": credential_status(),
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
# Weather + clock
#
# The panel knows neither where it is nor what time it is; the PC next to it knows
# both. GET /weather returns current conditions and a five-day forecast from
# Open-Meteo (free, no key, no account) for the house's coordinates, plus the local
# time so the panel can set its clock without NTP or a timezone string of its own.
#
# Location: bridge/weather_config.json if present, otherwise Home Assistant's own
# /api/config - it already knows where the house is, what zone it is in and whether
# it thinks in Fahrenheit. Weather is cached for ten minutes; the time is fresh on
# every request because the panel sets its clock from it.
# --------------------------------------------------------------------------------------

WEATHER_CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "weather_config.json")
OPEN_METEO_URL = "https://api.open-meteo.com/v1/forecast"
WEATHER_CACHE_SECONDS = 600
WEATHER_RETRY_SECONDS = 60
LOCATION_CACHE_SECONDS = 3600

# WMO weather interpretation codes -> (text, glyph family). The panel draws the nine
# glyph families itself and never sees a WMO code.
WMO_CODES = {
    0: ("Clear", "sun"),
    1: ("Mostly clear", "sun"),
    2: ("Partly cloudy", "partly"),
    3: ("Overcast", "cloud"),
    45: ("Fog", "fog"),
    48: ("Freezing fog", "fog"),
    51: ("Light drizzle", "rain"),
    53: ("Drizzle", "rain"),
    55: ("Heavy drizzle", "rain"),
    56: ("Freezing drizzle", "rain"),
    57: ("Freezing drizzle", "rain"),
    61: ("Light rain", "rain"),
    63: ("Rain", "rain"),
    65: ("Heavy rain", "rain"),
    66: ("Freezing rain", "rain"),
    67: ("Freezing rain", "rain"),
    71: ("Light snow", "snow"),
    73: ("Snow", "snow"),
    75: ("Heavy snow", "snow"),
    77: ("Snow grains", "snow"),
    80: ("Light showers", "rain"),
    81: ("Showers", "rain"),
    82: ("Heavy showers", "rain"),
    85: ("Snow showers", "snow"),
    86: ("Heavy snow showers", "snow"),
    95: ("Thunderstorm", "storm"),
    96: ("Thunderstorm, hail", "storm"),
    99: ("Thunderstorm, hail", "storm"),
}


def describe_wmo(code, is_day=True):
    try:
        text, icon = WMO_CODES[int(code)]
    except (KeyError, TypeError, ValueError):
        text, icon = "Unknown", "cloud"
    if not is_day:
        icon = {"sun": "moon", "partly": "partly-night"}.get(icon, icon)
    return text, icon


_location_cache = {"at": 0.0, "data": None}


def weather_location():
    """{latitude, longitude, timezone, units, name, source} or None if nobody knows."""
    cfg = {}
    try:
        with open(WEATHER_CONFIG_FILE, "r", encoding="utf-8") as handle:
            cfg = json.load(handle)
    except (OSError, ValueError):
        cfg = {}
    if not isinstance(cfg, dict):
        cfg = {}
    if cfg.get("latitude") is not None and cfg.get("longitude") is not None:
        try:
            return {
                "latitude": float(cfg["latitude"]),
                "longitude": float(cfg["longitude"]),
                "timezone": cfg.get("timezone") or None,
                "units": "c" if str(cfg.get("units", "f")).lower().startswith("c") else "f",
                "name": str(cfg.get("name") or "Home"),
                "source": "weather_config.json",
            }
        except (TypeError, ValueError):
            log("[weather] weather_config.json has a bad latitude/longitude")

    if time.time() - _location_cache["at"] < LOCATION_CACHE_SECONDS:
        return _location_cache["data"]

    loc = None
    ha = ha_config()
    if ha is not None:
        try:
            c = ha_request(ha, "GET", "/api/config") or {}
            if c.get("latitude") is not None and c.get("longitude") is not None:
                unit = str((c.get("unit_system") or {}).get("temperature", "F"))
                loc = {
                    "latitude": float(c["latitude"]),
                    "longitude": float(c["longitude"]),
                    "timezone": c.get("time_zone") or None,
                    "units": "c" if unit.upper().endswith("C") else "f",
                    "name": str(c.get("location_name") or "Home"),
                    "source": "home assistant",
                }
        except (urllib.error.URLError, OSError, ValueError, TypeError, AttributeError) as exc:
            log("[weather] Home Assistant location lookup failed: %s" % exc)
    _location_cache.update(at=time.time(), data=loc)
    return loc


def local_clock(tz_name):
    """The panel's clock, as {epoch, utc_offset, tz}. The panel keeps epoch+utc_offset
    as its system time and formats it as UTC, so DST is entirely this function's
    problem, re-evaluated every time the panel asks."""
    now = datetime.now(timezone.utc)
    tz = None
    if tz_name:
        try:
            from zoneinfo import ZoneInfo

            tz = ZoneInfo(tz_name)
        except Exception:
            # No tz database on this box (Windows without the tzdata package). The
            # PC sits next to the panel, so its own zone is the next best thing.
            tz = None
    local = now.astimezone(tz) if tz else now.astimezone()
    offset = local.utcoffset() or timedelta(0)
    return {
        "epoch": int(now.timestamp()),
        "utc_offset": int(offset.total_seconds()),
        "tz": tz_name if tz else str(local.tzname()),
        "local": local.strftime("%Y-%m-%d %H:%M:%S"),
    }


def fetch_open_meteo(loc):
    imperial = loc["units"] == "f"
    params = {
        "latitude": "%.4f" % loc["latitude"],
        "longitude": "%.4f" % loc["longitude"],
        "current": "temperature_2m,relative_humidity_2m,apparent_temperature,"
                   "weather_code,wind_speed_10m,is_day",
        "daily": "weather_code,temperature_2m_max,temperature_2m_min,"
                 "precipitation_probability_max",
        "temperature_unit": "fahrenheit" if imperial else "celsius",
        "wind_speed_unit": "mph" if imperial else "kmh",
        "timezone": loc.get("timezone") or "auto",
        "forecast_days": "5",
    }
    req = urllib.request.Request(
        OPEN_METEO_URL + "?" + urllib.parse.urlencode(params),
        headers={"User-Agent": "claude-desk-panel"},
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        raw = json.loads(resp.read())

    def whole(value):
        return None if value is None else int(round(float(value)))

    cur = raw.get("current") or {}
    is_day = bool(cur.get("is_day", 1))
    text, icon = describe_wmo(cur.get("weather_code"), is_day)
    out = {
        "location": loc["name"],
        "unit": "F" if imperial else "C",
        "wind_unit": "mph" if imperial else "km/h",
        "current": {
            "temp": whole(cur.get("temperature_2m")),
            "feels": whole(cur.get("apparent_temperature")),
            "humidity": whole(cur.get("relative_humidity_2m")),
            "wind": whole(cur.get("wind_speed_10m")),
            "code": cur.get("weather_code"),
            "icon": icon,
            "text": text,
            "is_day": is_day,
        },
        "daily": [],
        "timezone": raw.get("timezone"),
        "utc_offset": raw.get("utc_offset_seconds"),
    }

    daily = raw.get("daily") or {}
    dates = daily.get("time") or []
    codes = daily.get("weather_code") or []
    highs = daily.get("temperature_2m_max") or []
    lows = daily.get("temperature_2m_min") or []
    pops = daily.get("precipitation_probability_max") or []

    def at(seq, i):
        return seq[i] if i < len(seq) else None

    for i, date in enumerate(dates[:5]):
        text, icon = describe_wmo(at(codes, i), True)
        try:
            day = datetime.strptime(date, "%Y-%m-%d").strftime("%a")
        except (ValueError, TypeError):
            day = str(date)[-5:]
        out["daily"].append(
            {
                "day": day,
                "date": date,
                "code": at(codes, i),
                "icon": icon,
                "text": text,
                "hi": whole(at(highs, i)),
                "lo": whole(at(lows, i)),
                "precip": whole(at(pops, i)) or 0,
            }
        )
    return out


_weather_cache = {"at": 0.0, "fetched_at": 0.0, "data": None, "error": None}


def weather_payload():
    loc = weather_location()
    if loc is None:
        return {
            "ok": False,
            "configured": False,
            "error": "no location - add bridge/weather_config.json or set one in Home Assistant",
            "time": local_clock(None),
        }

    if time.time() - _weather_cache["at"] >= WEATHER_CACHE_SECONDS:
        try:
            data = fetch_open_meteo(loc)
            _weather_cache.update(at=time.time(), fetched_at=time.time(), data=data, error=None)
            log("[weather] %s: %s %s%s, %d-day forecast" % (
                loc["name"], data["current"]["text"], data["current"]["temp"],
                data["unit"], len(data["daily"])))
        except (urllib.error.URLError, OSError, ValueError, KeyError, TypeError) as exc:
            # Keep serving the last good forecast; try again sooner than the full cache life.
            _weather_cache.update(
                at=time.time() - WEATHER_CACHE_SECONDS + WEATHER_RETRY_SECONDS,
                error="weather fetch failed: %s" % exc,
            )
            log("[weather] %s" % _weather_cache["error"])

    clock = local_clock(loc.get("timezone"))
    data = _weather_cache["data"]
    if data is None:
        return {
            "ok": False,
            "configured": True,
            "error": _weather_cache["error"] or "no data yet",
            "time": clock,
        }

    out = dict(data)
    out.update(
        ok=True,
        configured=True,
        time=clock,
        fetched_at=int(_weather_cache["fetched_at"]),
        stale=bool(_weather_cache["error"]),
        error=_weather_cache["error"],
        source="open-meteo, location from " + loc["source"],
    )
    return out


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
        elif path == "/weather":
            self._send(weather_payload())
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
    parser.add_argument("--weather", action="store_true", help="print the /weather payload and exit")
    args = parser.parse_args()

    if args.ha_discover:
        ha_discover()
        return
    if args.weather:
        print(json.dumps(weather_payload(), indent=2))
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
    loc = weather_location()
    if loc:
        log("  weather for %s (%.3f, %.3f) from %s" % (loc["name"], loc["latitude"], loc["longitude"], loc["source"]))
    else:
        log("  weather: no location - add weather_config.json or configure Home Assistant")
    cred = credential_status()
    log("  credential: %s" % cred["detail"])
    if not cred["ok"]:
        log("  WARNING: the usage gauges will blank when this lapses")
    log("  reading transcripts from: %s" % PROJECTS_DIR)
    log("  weekly reset: weekday %d at %02d:00 local; per-model gauge: %s" % (WEEKLY_RESET_WEEKDAY, WEEKLY_RESET_HOUR, MODEL_LABEL))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("stopping")
        server.shutdown()


if __name__ == "__main__":
    main()
