# MCSK BLDC dashboard

A live web dashboard for this board: regular motor telemetry, motor/scope
controls, and the software oscilloscope's most recent capture, auto-updating
every 3 seconds - no console, no serial terminal, no manual copying of
anything. This is now the recommended way to use this project's telemetry and
oscilloscope features; the earlier "paste Live Data lines into a local
Python script" workflow has been retired in its favor.

Same overall shape as the `nxp-frdm-imx-95` "cockpit" in the
`iotc-python-lite-sdk-demos` repo (`portal/`): one AWS Lambda serves the static
page *and* proxies /IOTCONNECT REST calls for it. This dashboard is the
plain-telemetry two-thirds of that design - it does not need that demo's
file-upload machinery (their RAG document previews are megabytes of text; this
project's biggest payload, a 1000-sample scope capture, is designed to fit
inside telemetry - see the main README's "Software Oscilloscope" section).

**One real difference from that reference:** the imx95 cockpit is a
single-tenant deployment for one company's fixed demo account, so its solution
key lives once in Secrets Manager, set by whoever deploys it. This project is
a quickstart lots of different people each build against their *own*
/IOTCONNECT account - so instead of baking one solution key into the Lambda,
the login form collects the solution key and environment alongside the
/IOTCONNECT email/password (platform is fixed to `aws` in the page itself,
not asked for - this project's solution is always AWS-hosted, see the main
README), and the browser resends all of it with every request. That means **this Lambda holds no /IOTCONNECT secret of its own** -
one shared deployment works for anyone with this repo and their own
/IOTCONNECT login, and there's nothing for whoever deploys it to configure
per user. See "Design notes" below for what that trades away.

```
 browser (dashboard.html)
   |  fetch("api/dash/...") with the signed-in user's own solution
   |  key/env/pf (headers) and bearer token -- same origin, no CORS needed
   v
 Lambda (lambda_function.py)  -- holds no /IOTCONNECT secret of its own
   v
 /IOTCONNECT REST API (login, Telemetry, template-command)
```

## What each panel shows

| Panel | Source |
|---|---|
| Motor telemetry (gauge + tiles) | `GET Telemetry/device/{guid}`, newest row per attribute (`run/st/sec/rpm/spd/vdc`; `duty` too, via the gauge). `ic`/`im` (requested/measured current) aren't shown - always 0 in this build, see the main README's telemetry field notes for why. |
| Motor control | `POST template-command/device/{guid}/send` (`motor-start/stop/reverse/speed`) |
| Software oscilloscope | `GET Telemetry/attribute-history/device/{duid}/from/{from}/to/{to}` - see `fetch_scope_history()`/`reconstruct_capture()` in `lambda_function.py`, and "How the oscilloscope panel reads capture data" below |

There's no separate "last command" panel - the Activity log (right-hand rail)
already shows every command this browser has sent, which covers it without
needing a `last_cmd` telemetry field from the firmware. (If that field ever
gets added for some other reason, it's still just another attribute
`collapse_latest()` would pick up for free - nothing here forecloses it.)

## How the oscilloscope panel reads capture data

Motor telemetry (`GET Telemetry/device/{guid}`) is a **"latest value" /
device-shadow style endpoint**: exactly one row per attribute, no history -
fine for the telemetry panel, but not enough to reassemble a whole scope
capture out of 12+ chunks, each updating `osc_seq`/`osc_n`/`osc_t`/`osc_v`.
The oscilloscope panel instead reads `Telemetry/attribute-history/device/
{duid}/from/{from}/to/{to}` (`duid` = device uniqueId, not guid; `from`/`to`
are `"%Y-%m-%d %H:%M:%S"` UTC, as URL path segments) - a genuine history read,
confirmed from Avnet's own reference client
([avnet-iotconnect/iotc-python-rest-api](https://github.com/avnet-iotconnect/iotc-python-rest-api),
`src/avnet/iotconnect/restapi/lib/telemetry.py`). Each row there is one raw
device *message* (`{"dTime": ..., "attr": {every attribute that message
set}}`), so a scope chunk's four fields already arrive bundled together in one
row - no cross-row grouping needed. See `fetch_scope_history()`/
`reconstruct_capture()` in `lambda_function.py`.

### Which channel/rate a capture is labeled with

The chart's title and y-axis need to know which channel/rate a *specific*
capture was taken on. `reconstruct_capture()` gets this from the settings
messages (`{"osc_ch", "osc_rate", "osc_len"}`, published separately by
`IOTC_RNWF11_PublishScopeSettings()`) in that same history feed - whichever
one precedes the capture's first chunk - rather than the separately-polled
`osc_ch`/`osc_rate` telemetry the rest of the dashboard uses, which reflects
whatever the *current* setting is and can differ from whichever channel an
already-completed capture was actually taken on. Both the data and its label
come from the same read, so they can't drift apart; `v.osc_ch` is kept only as
a fallback for the rare case where no settings message falls inside the
lookback window. `iotconnect_rnwf11.c` also publishes settings immediately
whenever `scope-channel`/`scope-rate`/`scope-length` changes them, not just on
the periodic 10-second tick, so a label is never waiting on that cadence.

## Deploying

You'll need your own AWS account/credentials - this can't be deployed from
here. There's no /IOTCONNECT secret to configure at deploy time (see "one real
difference" above) - `./deploy.sh` just stands up the compute, and is safe to
re-run:

```
./deploy.sh
```

Then open the URL it prints and sign in - see "Signing in" below for what to
enter. Deploy it once; anyone with the URL and their own /IOTCONNECT login can
use it.

The manual steps, if you'd rather do (or understand) them by hand:

1. **IAM role.** Create an execution role for the Lambda - just the standard
   `AWSLambdaBasicExecutionRole` managed policy (logs only; no secret to read
   any more). See `aws-deploy-policy.json` for the *deploying user's* own
   policy, not this role.
2. **Zip & create the Lambda.** `zip -j lambda.zip lambda_function.py iotc_client.py dashboard.html`
   (flat - all three files at the zip root, `dashboard.html` right next to the
   `.py` files, matching how `lambda_handler()` opens it), then create a
   Python 3.12 function from it. Optional env var: `TEMPLATE_GUID` (this
   project's `dspic33MC` device-template GUID, from Settings -> Device ->
   Templates in the /IOTCONNECT console after uploading
   `templates/dspic33MC-template.json` - narrows every signed-in user's
   device picker to just this template; leave unset to show every device
   they can see).
3. **HTTP API.** Create an API Gateway HTTP API, Lambda integration, a
   `$default` route (quick-create does this). Open the API's invoke URL.

To update after a code change: re-zip the same three files and
`aws lambda update-function-code --function-name <name> --zip-file fileb://lambda.zip`
(or just re-run `./deploy.sh`).

### Signing in

Each person who opens the dashboard signs in with **their own** /IOTCONNECT
solution key, environment, email and password (platform isn't asked for -
it's fixed to `aws` in `dashboard.html`'s `PLATFORM` constant, since this
project's solution is always AWS-hosted; change that one constant if that's
ever not true for you). `env` is the same value
`tools/provision_device_config.py --env` uses for *this* project's solution
(Settings -> Key Value in the console). The **solution key** is a separate
value this dashboard needs that the provisioning script doesn't -
`iotc_client.py`'s discovery call names it `solutionkey`
(`discovery.iotconnect.io/api/uisdk/solutionkey/{key}/...`), the same term the
imx95 cockpit's login screen uses. It's a different value from the script's
`--cpid` - look for a field literally labeled "Solution Key" in the console.
Check "Remember the solution
key/environment on this device" to skip retyping those two next time - email
and password are never remembered, and are asked for fresh every session.

## Files

| File | What it is |
|---|---|
| `lambda_function.py` | The backend: serves `dashboard.html` at `/`, plus `/api/dash/login`, `/bootstrap`, `/telemetry`, `/command`. |
| `iotc_client.py` | /IOTCONNECT REST client - unmodified copy of the same file from the `iotc-python-lite-sdk-demos` cockpit (trimmed to the lookups this dashboard actually calls). |
| `dashboard.html` | The page: login, motor telemetry, motor control, oscilloscope (chart + controls), activity log. Single file, no build step, no external JS dependencies. |
| `aws-deploy-policy.json` | IAM policy for the *deploying user* (not the Lambda's own execution role - see step 2 above). |
| `deploy.sh` | Creates/updates the IAM role, Lambda, and HTTP API - see "Deploying" above. |

## Design notes

- **A finished capture is "sticky" client-side, on purpose.** The backend's
  history read is a rolling window (`now - SCOPE_HISTORY_LOOKBACK_MIN` to
  `now`, not "since the capture started" - see `fetch_scope_history()`), so a
  capture's own chunks eventually age out of it. `refresh()` in
  `dashboard.html` keeps the last real capture (`S.lastCapture`) displayed
  regardless of what a later poll's window returns; only a genuinely new
  capture ever replaces it - the same way a real oscilloscope keeps its last
  trace on screen instead of blanking it once some unrelated internal window
  expires.
- **Per-user credentials, not a server-held secret.** Each browser sends its
  own solution key/env/pf as headers (`X-Iotc-Solution-Key` etc. - see `api()`
  in `dashboard.html`) on every request after login, not just at login, since
  the Lambda has nowhere else to get them from (unlike the imx95 cockpit,
  where a fixed solution key already lived in Secrets Manager and only the
  bearer token needed to travel per-request). The trade-off: the solution key
  sits in `sessionStorage` (cleared when the tab closes) for as long as
  someone's signed in, and in `localStorage` (survives closing the browser)
  if they checked "remember" - a bigger footprint than the bearer token alone
  would have needed, on a device you control, which is the honest exchange
  for not fixing this dashboard to one /IOTCONNECT account at deploy time. The
  password itself is never persisted anywhere, in either mode - only the
  short-lived token it's exchanged for.
- **Polling, not push.** Same as the reference cockpit: `setInterval(refresh, 3000)`,
  with client-side "latching" (`latch()` in `dashboard.html`) so an
  eventually-consistent REST read doesn't flicker the UI between a current and
  a stale value - a novel value needs 2 consecutive polls to be shown, a
  *reversion* to a value shown earlier needs 5.
- **Oscilloscope chart** is hand-rolled inline SVG (no charting library): a 2px
  line, hairline recessive gridlines, and a pointermove crosshair + tooltip.
  Built against this project's own [dataviz skill](../tools) conventions -
  single series, so no legend; text stays in text-token colors, never the
  series color. Unit conversion (`convert()` in `dashboard.html`) uses the same
  schematic-derived scale factors documented in the main README's "Software
  Oscilloscope" section - if the board's actual component values there ever
  change, change them here too.
- **Dark is the default**, on the reasoning that a hardware cockpit is
  normally viewed in a dim room; light is a real, fully supported theme too,
  reachable via the manual toggle (top-right, persisted via
  `data-theme`/`localStorage`). Built on the CSS custom-property pattern the
  dataviz skill's reference palette specifies - the accent/status colors are
  that skill's validated default instance, swap them for this project's own
  brand colors if it has one (only the `:root` custom properties need to
  change).
