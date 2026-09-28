# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Avnet
# -----------------------------------------------------------------------------
# Minimal IOTCONNECT REST API client - unmodified copy of the same file used by
# the nxp-frdm-imx-95 "cockpit" portal (iotc-python-lite-sdk-demos repo). Kept
# identical rather than re-derived so this dashboard's Lambda inherits the same
# already-verified auth flow instead of a second, untested implementation.
#
# Auth flow (verified against awspocauth 2026-07-28, in the sibling repo):
#   1. GET  {auth}/Auth/basic-token          header Solution-Key  -> basic token
#   2. POST {auth}/Auth/login                Authorization: Basic <basic-token>,
#      body {username, password, solutionKey}                    -> bearer token
#   3. All other calls: Authorization: Bearer <access_token>
#
# Secrets (solution key) come from the environment / AWS Secrets Manager -
# never from this repo. See dashboard/README.md for what this project's
# deployment needs (its own solution key/env - NOT the imx95 one above).
# -----------------------------------------------------------------------------
import json
import os
import urllib.parse
import urllib.request

DISCOVERY = "https://discovery.iotconnect.io/api/uisdk/solutionkey/{key}/env/{env}?pf={pf}"


class IoTConnectError(RuntimeError):
    pass


class Client:
    def __init__(self, solution_key=None, env=None, pf=None):
        self.solution_key = solution_key or os.environ["IOTC_SOLUTION_KEY"]
        self.env = env or os.environ.get("IOTC_ENV", "poc")
        self.pf = pf or os.environ.get("IOTC_PF", "aws")
        self.urls = {}
        self.token = None

    # -- plumbing -------------------------------------------------------------
    def _req(self, method, url, body=None, headers=None, timeout=30):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Accept", "application/json")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        if self.token and "Authorization" not in (headers or {}):
            req.add_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                payload = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            raise IoTConnectError("%s %s -> HTTP %s: %s" % (
                method, url, e.code, e.read().decode("utf-8", "replace")[:400]))
        try:
            return json.loads(payload)
        except ValueError:
            raise IoTConnectError("%s %s -> non-JSON response: %s" % (method, url, payload[:200]))

    def discover(self):
        url = DISCOVERY.format(key=urllib.parse.quote(self.solution_key), env=self.env, pf=self.pf)
        out = self._req("GET", url)
        data = out.get("data") or {}
        if not data.get("authBaseUrl"):
            raise IoTConnectError("discovery failed: %s" % json.dumps(out)[:300])
        self.urls = data
        return data

    # -- auth -----------------------------------------------------------------
    def login(self, username, password):
        if not self.urls:
            self.discover()
        auth = self.urls["authBaseUrl"]
        basic = self._req("GET", auth + "/Auth/basic-token",
                          headers={"Solution-Key": self.solution_key})
        basic_token = basic.get("data")
        if not basic_token:
            raise IoTConnectError("basic-token failed: %s" % json.dumps(basic)[:300])
        out = self._req("POST", auth + "/Auth/login",
                        body={"username": username, "password": password,
                              "solutionKey": self.solution_key},
                        headers={"Authorization": "Basic " + basic_token,
                                 "Solution-Key": self.solution_key})
        data = out.get("data") or out  # login returns the token at top level
        self.token = data.get("access_token") or data.get("accessToken")
        if not self.token:
            raise IoTConnectError("login failed: %s" % json.dumps(out)[:300])
        return data

    # -- lookups ----------------------------------------------------------
    def entities(self):
        return self._req("GET", self.urls["entityBaseUrl"] + "/Entity/lookup")

    def roles(self):
        return self._req("GET", self.urls["entityBaseUrl"] + "/Role/lookup")

    def templates(self):
        return self._req("GET", self.urls["deviceBaseUrl"] + "/device-template/lookup")

    def devices(self):
        return self._req("GET", self.urls["deviceBaseUrl"] + "/Device/lookup")
