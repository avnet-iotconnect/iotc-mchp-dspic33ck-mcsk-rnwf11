# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Avnet
# -----------------------------------------------------------------------------
# MCSK BLDC dashboard - API Lambda.
#
# Same overall shape as the nxp-frdm-imx-95 "cockpit" (iotc-python-lite-sdk-demos
# repo, portal/lambda_function.py): a single Lambda behind an HTTP API serves
# the static dashboard page AND proxies /IOTCONNECT REST calls for it. One
# real difference from that reference: the imx95 cockpit is a single-tenant
# deployment for one company's fixed demo account, so its solution key lives
# once in Secrets Manager, set by whoever deploys it. This project is a
# quickstart lots of different people each build against their OWN
# /IOTCONNECT account, so the solution key/env/pf are NOT baked into this
# Lambda at all - the browser's login form collects them (once per browser
# session, alongside the /IOTCONNECT email/password) and resends them with
# every request. That means this Lambda needs no secret of its own and no
# Secrets Manager setup - deploy.sh's job shrinks to "stand up the compute",
# and anyone can point their own browser at one shared deployment. The
# trade-off: unlike the bearer token (exchanged once at login, then used
# alone), the solution key has to travel on every request after login too,
# same as it sits in the browser's sessionStorage for the life of the tab -
# see the login handling in dashboard.html.
#
# Routes (HTTP API, payload v2):
#   GET  /  /dashboard.html          serves the page (same origin as the API,
#                                     so the browser's fetch() calls need no CORS)
#   POST /api/dash/login             {email,password,solutionKey,env,pf} -> {token}
#   GET  /api/dash/bootstrap         devices + their command GUIDs
#   GET  /api/dash/telemetry?guid=   latest scalar values + the current/most
#                                     recent scope capture, reassembled from
#                                     its chunks (see reconstruct_capture())
#   POST /api/dash/command           {deviceGuid,commandGuid,value} -> send
# All routes past /login also require the X-Iotc-Solution-Key/X-Iotc-Env/
# X-Iotc-Pf headers (see api() in dashboard.html) - there's no server-side
# fallback for them.
# -----------------------------------------------------------------------------
import json
import os
import traceback

from iotc_client import Client, IoTConnectError

# Optional: set to this project's dspic33MC device-template GUID (Settings ->
# Device -> Templates in the /IOTCONNECT console, after uploading
# templates/dspic33MC-template.json) to hide any other devices on the account.
# Left unset, every device the logged-in user can see is offered. This is the
# only deployment-level (as opposed to per-user) setting left, and it's not a
# secret - it just narrows a dropdown.
TEMPLATE_GUID = os.environ.get("TEMPLATE_GUID", "")

# discover() is a live HTTP round trip to discovery.iotconnect.io that returns
# the same URLs for as long as a (solution_key, env, pf) triple is valid - but
# every telemetry poll (every 3s from the browser - see dashboard.html) used to
# call it fresh, hammering that endpoint for no reason and, on real hardware,
# eventually drawing throttling errors from it. Cached per Lambda execution
# environment (persists across warm invocations, gone on a cold start - which
# is fine, it's just an optimization, not a correctness requirement).
_discovery_cache = {}


def iotc_client(solution_key, env, pf, user_token=None):
    if not (solution_key and env):
        raise IoTConnectError("missing solution key or environment")
    pf = pf or "aws"
    c = Client(solution_key=solution_key, env=env, pf=pf)
    cache_key = (solution_key, env, pf)
    if cache_key in _discovery_cache:
        c.urls = _discovery_cache[cache_key]
    else:
        c.discover()
        _discovery_cache[cache_key] = c.urls
    if user_token:
        c.token = user_token
    return c


def telemetry_base(c):
    return (c.urls.get("telemetryBaseUrl")
            or c.urls["deviceBaseUrl"].replace("device", "telemetry"))


def resp(code, body, ctype="application/json"):
    return {"statusCode": code,
            "headers": {"Content-Type": ctype,
                        "Access-Control-Allow-Origin": "*",
                        "Access-Control-Allow-Headers":
                            "Content-Type, Authorization, X-Iotc-Solution-Key, X-Iotc-Env, X-Iotc-Pf"},
            "body": body if isinstance(body, str) else json.dumps(body)}


# --- telemetry reconstruction -------------------------------------------------
# The firmware (iotconnect_rnwf11.c) publishes two kinds of telemetry:
#  - plain scalars (run/st/sec/rpm/spd/ic/im/duty/vdc, osc_ch/osc_rate/osc_len) -
#    one value per attribute, handled the same way the imx95 cockpit handles
#    ALL of its telemetry: keep only the newest row per attribute name.
#  - a scope capture, published as a run of small CHUNKS (osc_seq/osc_n/osc_t/
#    osc_v), because a whole capture is far bigger than one /IOTCONNECT message
#    (see the README's "Software Oscilloscope" section) - the imx95 cockpit
#    never had this problem (its one large payload, the RAG chunk preview, goes
#    through a device file upload instead of telemetry - see its
#    stage_and_send()/rag-chunks).
#
# GET /Telemetry/device/{guid} - what collapse_latest() below reads - was also
# the FIRST thing this file tried for the chunks, through two different
# row-order-guessing reconstruction algorithms, both wrong: verified against
# real hardware (see dashboard/README.md's history if curious), that endpoint
# returns the single LATEST value per attribute ONLY, never more - one row per
# attribute, always, no matter how many times it changed. No reconstruction
# algorithm could ever have worked against it; the four chunk fields overwrite
# each other in that view before a poll ever sees more than the last one.
#
# The real fix is a different endpoint entirely - see fetch_scope_history()
# below - confirmed from Avnet's own reference REST client
# (avnet-iotconnect/iotc-python-rest-api, src/.../telemetry.py), not
# discovered by trial and error against this project's own guesses.
SCOPE_ATTRS = ("osc_seq", "osc_n", "osc_t", "osc_v")


def collapse_latest(rows):
    """rows -> (values, stamps, newest_ts) - newest row per attribute name,
    skipping the scope-chunk attributes (see SCOPE_ATTRS above, and the module
    comment above this function for why those specifically are excluded). Same
    rule the imx95 cockpit uses for ALL its telemetry (its Lambda's /telemetry
    route): the API can return more than one row per attribute in arbitrary
    order, and without this the UI flip-flops between a current and a stale
    value."""
    values, stamp, newest = {}, {}, None
    for r in rows:
        k = r.get("attributeName")
        if k in SCOPE_ATTRS:
            continue
        ts = r.get("deviceUpdatedDate") or ""
        if k and (k not in stamp or ts >= stamp[k]):
            values[k] = r.get("attributeValue")
            stamp[k] = ts
        if ts and (newest is None or ts > newest):
            newest = ts
    return values, stamp, newest


def parse_array(text):
    """'[0,500,1000]' -> [0, 500, 1000] - the bracketed-array-as-text format
    the firmware publishes osc_t/osc_v in (see the main README's "Software
    Oscilloscope" section)."""
    text = (text or "").strip()
    if text.startswith("[") and text.endswith("]"):
        text = text[1:-1]
    return [int(x) for x in text.split(",") if x.strip()]


# "%Y-%m-%d %H:%M:%S" (space, no "T", no milliseconds, seconds precision) -
# not guessed: this is the exact format Avnet's own iotc-python-rest-api uses
# (util.to_api_datetime()) for this same endpoint. Goes into a URL PATH
# segment (not a query string), so it still needs percent-encoding - the
# space alone is not a legal raw path character.
API_DATETIME_FORMAT = "%Y-%m-%d %H:%M:%S"
# Wide enough to comfortably cover the slowest realistic capture (a 1000-sample
# capture can take ~20-40s to publish all its chunks - see the README) plus
# clock skew and polling lag; narrow enough to keep the response small. Not a
# hard limit like the reference client's 7-day cap - just this dashboard's own
# "how far back is a capture still 'current'" choice.
SCOPE_HISTORY_LOOKBACK_MIN = 10


def fetch_scope_history(c, duid):
    """The real fix for the oscilloscope panel: GET
    /Telemetry/attribute-history/device/{duid}/from/{from}/to/{to} - a
    genuine history endpoint, unlike /Telemetry/device/{guid} (see the module
    comment above). Confirmed from Avnet's own iotc-python-rest-api reference
    client, not discovered by trial and error: each returned item is one raw
    device MESSAGE, not one attribute - {"dTime": ..., "attr": {every
    attribute that message set: value, ...}}. Since the firmware publishes
    osc_seq/osc_n/osc_t/osc_v together in a single MQTT message (see
    IOTC_RNWF11_BuildScopeChunk()), a scope chunk's four fields already arrive
    bundled together in one "attr" dict here - no cross-row grouping needed at
    all, unlike the two abandoned approaches against the other endpoint.

    Returns (feed, raw) - feed is the list of {"dTime", "attr"} items (or []),
    raw is the whole parsed response body, kept only so the caller can surface
    it as a debug aid if the envelope shape here ever turns out to be wrong
    too (unconfirmed against a real response as of this writing - only the
    URL/format and the per-item "attr" shape come from the reference client;
    the top-level envelope key ("feed", nested under "data", or a bare list)
    is this function's own best guess among what /IOTCONNECT's other
    endpoints do, handled defensively rather than assumed).
    """
    import datetime
    import urllib.parse
    now = datetime.datetime.now(datetime.timezone.utc)
    from_str = (now - datetime.timedelta(minutes=SCOPE_HISTORY_LOOKBACK_MIN)).strftime(API_DATETIME_FORMAT)
    to_str = now.strftime(API_DATETIME_FORMAT)
    path = "/Telemetry/attribute-history/device/%s/from/%s/to/%s" % (
        urllib.parse.quote(duid, safe=""), urllib.parse.quote(from_str, safe=""),
        urllib.parse.quote(to_str, safe=""))
    raw = c._req("GET", telemetry_base(c) + path)
    feed = None
    if isinstance(raw, list):
        feed = raw
    elif isinstance(raw, dict):
        if isinstance(raw.get("feed"), list):
            feed = raw["feed"]
        elif isinstance(raw.get("data"), dict) and isinstance(raw["data"].get("feed"), list):
            feed = raw["data"]["feed"]
        elif isinstance(raw.get("data"), list):
            feed = raw["data"]
    return feed or [], raw


def reconstruct_capture(feed):
    """Scope-chunk history feed -> the most recent capture, or None if there
    are none. feed is fetch_scope_history()'s return - a list of
    {"dTime": ..., "attr": {...}} messages. Filters for messages whose "attr"
    already contains all four chunk fields together (see fetch_scope_history()
    for why that's already true per-message, no grouping needed), sorts by
    dTime, then walks them in order: a new capture starts whenever osc_seq
    repeats or restarts at 1 (the device can only be mid-capture on one at a
    time, so a second "1" always means a new capture began), keeping only the
    most recent one - a still-filling capture is returned with complete=false
    and whatever chunks have arrived so far.

    Also returns channel/rate_us/length - which osc_ch/osc_rate/osc_len were
    active for THIS capture, read from the settings messages
    ({"osc_ch":...,"osc_rate":...,"osc_len":...}, published on their own
    every 10s by IOTC_RNWF11_PublishScopeSettings() - separate messages from
    the chunk ones) in this SAME feed, picking whichever preceded the
    capture's first chunk. Deliberately NOT sourced from the separately-
    polled/latched scalar telemetry the rest of the dashboard uses: that
    value only updates a couple of polls after actually changing (by design -
    see latch() in dashboard.html, meant for smoothing noisy sensor readings,
    not a setting a command changes instantly), and worse, it reflects
    whatever the CURRENT setting is, which can already differ from whichever
    channel this particular (possibly earlier) capture was actually taken on
    - pairing "now" metadata with "then" data is a real mismatch, not just a
    display lag, and this avoids it entirely by deriving both from the same
    history read.
    """
    msgs = [(item.get("dTime") or "", item.get("attr") or {})
           for item in feed if isinstance(item.get("attr"), dict)
           and all(k in item["attr"] for k in SCOPE_ATTRS)]
    msgs.sort(key=lambda x: x[0])
    settings_msgs = sorted(
        ((item.get("dTime") or "", item.get("attr") or {}) for item in feed
         if isinstance(item.get("attr"), dict) and "osc_ch" in item["attr"]),
        key=lambda x: x[0])

    captures = []
    cap_cur = None
    for ts, f in msgs:
        # A single malformed message (a value that isn't valid JSON-ish text,
        # etc.) should drop that one chunk, not crash the whole telemetry
        # response - the regular motor telemetry in the same response has
        # nothing to do with the scope.
        try:
            seq, n = int(f["osc_seq"]), int(f["osc_n"])
            t_vals, v_vals = parse_array(f["osc_t"]), parse_array(f["osc_v"])
        except (TypeError, ValueError):
            continue
        if cap_cur is None or seq in cap_cur["chunks"] or (seq == 1 and cap_cur["chunks"]):
            cap_cur = {"n": n, "chunks": {}, "start_ts": ts}
            captures.append(cap_cur)
        cap_cur["n"] = n
        cap_cur["chunks"][seq] = (t_vals, v_vals)
    if not captures:
        return None

    cap = captures[-1]  # most recent capture, complete or still filling

    # If this capture's OPENING chunk (osc_seq=1) has already aged out of the
    # rolling lookback window, what's left here is a shrinking fragment of a
    # capture we can no longer vouch for - not a legitimate still-filling
    # capture, since a real one always starts at chunk 1. Returning that
    # fragment used to be exactly what made a finished capture visibly lose
    # its early samples and then disappear over a few polls, even though
    # dashboard.html's S.lastCapture logic was designed to keep showing the
    # last REAL capture until a genuinely new one replaced it - that logic
    # only helps if we stop handing it degrading truthy data. Treating this
    # the same as "no capture in the window" (None) lets that existing
    # frontend stickiness do its job instead of being undermined here.
    if 1 not in cap["chunks"]:
        return None

    t, v = [], []
    for seq in sorted(cap["chunks"]):
        ct, cv = cap["chunks"][seq]
        t += ct
        v += cv

    # The settings in effect when this capture started: the latest settings
    # message at or before its first chunk, falling back to the earliest
    # settings message available if none precede it (e.g. the settings
    # publish that covers it fell just outside the lookback window).
    channel = rate_us = length = None
    prior = [f for ts, f in settings_msgs if ts <= cap["start_ts"]]
    settings = prior[-1] if prior else (settings_msgs[0][1] if settings_msgs else None)
    if settings:
        try:
            channel = int(settings["osc_ch"])
            rate_us = int(settings["osc_rate"])
            length = int(settings["osc_len"])
        except (TypeError, ValueError, KeyError):
            pass

    return {"n": cap["n"], "seq_seen": len(cap["chunks"]),
           "complete": sorted(cap["chunks"]) == list(range(1, cap["n"] + 1)),
           "t": t, "v": v, "channel": channel, "rate_us": rate_us, "length": length}


# --- routes --------------------------------------------------------------
def dash(event, path, method, headers, body_raw):
    token = headers.get("authorization", "")
    token = token[7:] if token.lower().startswith("bearer ") else ""
    solution_key = headers.get("x-iotc-solution-key", "")
    env = headers.get("x-iotc-env", "")
    pf = headers.get("x-iotc-pf", "")
    qs = event.get("queryStringParameters") or {}

    if path.endswith("/login"):
        try:
            b = json.loads(body_raw or "{}")
        except ValueError:
            return resp(400, {"error": "bad json"})
        email, password = (b.get("email") or "").strip(), b.get("password") or ""
        solution_key, env, pf = (b.get("solutionKey") or "").strip(), (b.get("env") or "").strip(), (b.get("pf") or "aws").strip()
        if not email or not password:
            return resp(400, {"error": "email and password are required"})
        if not solution_key or not env:
            return resp(400, {"error": "solution key and environment are required"})
        try:
            c = iotc_client(solution_key, env, pf)
            data = c.login(email, password)
        except IoTConnectError:
            return resp(401, {"error": "Sign-in failed - check your /IOTCONNECT solution key, "
                                       "environment, email and password."})
        except Exception as e:  # noqa: BLE001 - same reasoning as the catch-all below
            traceback.print_exc()
            return resp(500, {"error": "%s: %s" % (type(e).__name__, str(e)[:300])})
        return resp(200, {"token": data.get("access_token") or data.get("accessToken")})

    if not token:
        return resp(401, {"error": "not signed in"})
    if not solution_key or not env:
        return resp(401, {"error": "missing solution key or environment - please sign in again"})

    try:
        c = iotc_client(solution_key, env, pf, token)
        if path.endswith("/bootstrap"):
            devs = c.devices().get("data", [])
            if TEMPLATE_GUID:
                devs = [d for d in devs if d.get("deviceTemplateGuid") == TEMPLATE_GUID]
            devs = devs[:50]
            out = {"devices": [{"guid": d["guid"], "uniqueId": d["uniqueId"],
                                "templateGuid": d.get("deviceTemplateGuid")} for d in devs],
                   "commandsByTemplate": {}}
            for tpl in {d.get("deviceTemplateGuid") for d in devs if d.get("deviceTemplateGuid")}:
                try:
                    cmds = c._req("GET", c.urls["deviceBaseUrl"] + "/template-command/%s/lookup" % tpl)
                    out["commandsByTemplate"][tpl] = {x.get("command"): x.get("guid")
                                                      for x in cmds.get("data", []) if x.get("command")}
                except Exception as e:  # noqa: BLE001 - leave this template's commands empty
                    print("template-command lookup failed for %s: %s" % (tpl, e))
                    out["commandsByTemplate"][tpl] = {}
            return resp(200, out)

        if path.endswith("/telemetry"):
            guid = qs.get("guid") or ""
            duid = qs.get("duid") or ""
            if not guid:
                return resp(400, {"error": "guid is required"})
            # Latest-value read (confirmed: this is genuinely a "latest value
            # only" endpoint, never history - see the module comment above
            # reconstruct_capture()) - fine for these, they're only ever
            # displayed as their current value anyway.
            rows = c._req("GET", telemetry_base(c) + "/Telemetry/device/" + guid).get("data", [])
            values, stamp, newest = collapse_latest(rows)
            age = None
            if newest:
                try:
                    import datetime
                    t0 = datetime.datetime.strptime(newest[:19], "%Y-%m-%dT%H:%M:%S").replace(
                        tzinfo=datetime.timezone.utc)
                    age = (datetime.datetime.now(datetime.timezone.utc) - t0).total_seconds()
                except ValueError:
                    pass

            # Real history read for the scope capture (see fetch_scope_history()) -
            # needs duid (uniqueId), not guid; dashboard.html sends both.
            capture = None
            scope_feed = []
            scope_raw = None
            if duid:
                scope_feed, scope_raw = fetch_scope_history(c, duid)
                capture = reconstruct_capture(scope_feed)

            # TEMPORARY: the envelope shape (top-level "feed" vs nested under
            # "data" vs a bare list) isn't confirmed against a real response
            # yet - only the URL format and per-item "attr" shape come from
            # Avnet's reference client (see fetch_scope_history()). If capture
            # is still null, scope_raw shows exactly what came back so the next
            # fix is grounded in the real shape instead of another guess.
            # Remove once the oscilloscope panel is confirmed working.
            scope_debug = {"duid": duid, "feed_items": len(scope_feed),
                           "feed_items_with_all_4_fields":
                               sum(1 for item in scope_feed if isinstance(item.get("attr"), dict)
                                  and all(k in item["attr"] for k in SCOPE_ATTRS)),
                           "raw_response_if_feed_empty": scope_raw if not scope_feed else None,
                           "sample_feed_items": scope_feed[:3]}
            return resp(200, {"values": values, "stamps": stamp, "age_s": age,
                              "capture": capture, "scope_debug": scope_debug})

        if path.endswith("/command"):
            b = json.loads(body_raw or "{}")
            src = ((event.get("requestContext") or {}).get("http") or {}).get("sourceIp", "?")
            print("dashboard command from %s: device=%s cmd=%s value=%r" % (
                src, (b.get("deviceGuid") or "")[-8:], (b.get("commandGuid") or "")[-8:],
                (b.get("value") or "")[:60]))
            c._req("POST", c.urls["deviceBaseUrl"] + "/template-command/device/%s/send"
                   % b["deviceGuid"],
                   body={"commandGuid": b["commandGuid"], "parameterValue": b.get("value", ""),
                         "gatewayGuid": ""})
            return resp(200, {"sent": True})

        return resp(404, {"error": "unknown route"})
    except IoTConnectError as e:
        return resp(502, {"error": str(e)[:300]})
    except KeyError as e:
        return resp(400, {"error": "missing field: %s" % e})
    except Exception as e:  # noqa: BLE001 - last resort: a real error message beats
        # an opaque bare 500 from API Gateway with no body. The full traceback
        # still goes to CloudWatch either way; this just also gets a short
        # version to the browser so a failure like this is diagnosable from the
        # Network tab alone, the same way IoTConnectError/KeyError already are.
        traceback.print_exc()
        return resp(500, {"error": "%s: %s" % (type(e).__name__, str(e)[:300])})


def lambda_handler(event, context):
    rc = event.get("requestContext", {})
    http = rc.get("http", {})
    method, path = http.get("method", ""), http.get("path", "")

    if method == "OPTIONS":
        return resp(200, "")

    if method == "GET" and path in ("/", "/dashboard", "/dashboard.html"):
        try:
            page = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "dashboard.html")).read()
            return resp(200, page, "text/html; charset=utf-8")
        except OSError:
            return resp(500, {"error": "dashboard.html missing from deployment"})

    if "/api/dash/" in path:
        body_raw = event.get("body") or ""
        hdrs = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
        return dash(event, path, method, hdrs, body_raw)

    return resp(404, {"error": "not found"})
