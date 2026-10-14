# Developer Guide

This document covers building the firmware from source and reference material
not needed just to run the quickstart as-is. This includes the full C2D
command set, the oscilloscope's wire format, engineering-unit conversions,
and suggested capture settings. If you just want to run the demo, see
[README.md](README.md) instead. It uses the prebuilt
[`bin/quickstart/dspic33ck-lvmcb-rnwf11-iotconnect-demo.hex`](bin/quickstart/dspic33ck-lvmcb-rnwf11-iotconnect-demo.hex)
and doesn't require MPLAB X at all.

Everything below assumes you've already completed the README's hardware and
cloud setup (Steps 1-8: prerequisites, getting the quickstart source, cloud
account setup, importing the device template, generating/uploading the
device certificate, creating the device in /IOTCONNECT, mounting the
RNWF11, and connecting the motor). This guide picks up from there.

## Table of Contents

1. [Building From Source](#1-building-from-source)
2. [Motor Control Commands](#2-motor-control-commands)
3. [Software Oscilloscope](#3-software-oscilloscope)

## 1. Building From Source

Use this if you want to modify the firmware itself (motor control
parameters, the oscilloscope feature, telemetry fields, etc.) instead of
flashing the prebuilt binary.

### Requirements

[MPLAB X IDE](https://www.microchip.com/mplabx) 6.25 or later, with the
**XC-DSC** compiler (4.00 or later) and the `dsPIC33CK-MP_DFP` device pack.

> [!NOTE]
> This project pins `dsPIC33CK-MP_DFP` 1.15.423 in
> `firmware/dspic33ck256mp508_rnwf11_iotconnect.X/bldc.X/nbproject/configurations.xml`.
> If your installed pack is a different version, MPLAB X will prompt to
> resolve it on first open, accepting the update (or editing that pinned
> version to match what you have installed) is normally enough.

Building from source needs the full firmware tree, which the README's quick
path doesn't download (it only grabs the prebuilt `.hex` and provisioning
scripts). Clone the complete repository instead, including the `iotc-c-lib`
submodule (/IOTCONNECT's C SDK):

```bash
git clone --recurse-submodules https://github.com/avnet-iotconnect/iotc-mchp-dspic33ck-lvmcb.git
cd iotc-mchp-dspic33ck-lvmcb
```

Already cloned it without `--recurse-submodules`? Initialize the submodule separately:

```bash
git submodule update --init --recursive
```

### Configure and Build

Connect the board's power supply, and connect the included micro-USB cable
between your PC and the board's **PKOB4** port.

`provision_device_config.py`/`.ps1` resolves your device's /IOTCONNECT MQTT
connection info via /IOTCONNECT's discovery/identity API, then writes it
(along with your WiFi credentials) directly into
[`iotconnect/iotconnect_rnwf11_config.h`](firmware/dspic33ck256mp508_rnwf11_iotconnect.X/iotconnect/iotconnect_rnwf11_config.h)
so it's compiled into the firmware. It finds that file on its own, relative
to its own location, so there's nothing to copy-paste by hand.

**Linux:**
```bash
cd tools
```

Replace `MYSSID`/`MYPASSWORD` with your real WiFi credentials,
`MYCPID`/`MYENVIRONMENT` with the values under **Settings &rarr; Key
Vault** in the /IOTCONNECT console, and `MYUNIQUEID` with the same Unique ID
you used when generating the device certificate and creating the device in
/IOTCONNECT:
```bash
python3 provision_device_config.py --wifi-ssid MYSSID --wifi-password MYPASSWORD --cpid MYCPID --env MYENVIRONMENT --duid MYUNIQUEID
```
```bash
cd ..
```

**Windows (PowerShell):**
```powershell
Set-Location tools
```

Replace `MYSSID`/`MYPASSWORD` with your real WiFi credentials,
`MYCPID`/`MYENVIRONMENT` with the values under **Settings &rarr; Key
Vault** in the /IOTCONNECT console, and `MYUNIQUEID` with the same Unique ID
you used when generating the device certificate and creating the device in
/IOTCONNECT:
```powershell
.\provision_device_config.ps1 -WifiSsid MYSSID -WifiPassword MYPASSWORD -Cpid MYCPID -Env MYENVIRONMENT -Duid MYUNIQUEID
```
```powershell
Set-Location ..
```

> [!NOTE]
> Pass `--port`/`-Port` (e.g. `--port /dev/ttyACM0` or `-Port COM5`) to
> *also* push this same config live, over serial, into an already-flashed,
> already-running board's on-chip flash, useful for reconfiguring a board
> without rebuilding. It's optional here; omit it and the script only
> updates the header file above, to be compiled in by the next build.

In MPLAB X:

1. Open [`firmware/dspic33ck256mp508_rnwf11_iotconnect.X/bldc.X`](firmware/dspic33ck256mp508_rnwf11_iotconnect.X/bldc.X).
2. Clean and Build. The output `.hex` lands in
   `bldc.X/dist/default/production/`.
3. Program the board via the onboard debugger (**Make and Program Device**).

To watch the boot log, open a serial terminal at 115200 8-N-1.

> [!NOTE]
> The micro-USB connection can stay on the PKOB4 port on the board for connecting to the
> serial console. The firmware routes the serial communications through this port to
> prevent users from needing to swap their USB connection between ports on the board.

## 2. Motor Control Commands

The motor behavior is primarily driven by /IOTCONNECT C2D commands (sent from
the dashboard's buttons, or from the /IOTCONNECT console directly), but **SW1**
on the board itself can be used to toggle the motor power manually. SW1 is handled
in the motor control interrupt, so it responds immediately whatever else the firmware
is doing.

Commands that arrive while a scope capture is being published are queued (up to 4)
and run, in order, once the capture has finished, so they can't disturb the capture.
The exception is `motor-stop`, which stops the motor the moment it is received; its
acknowledgement is sent later with the rest of the queue.

| Command         | Parameter         | Effect                                   |
|-----------------|--------------------|-------------------------------------------|
| `motor-start`   | none               | Starts the motor (same effect as pressing SW1 while stopped) |
| `motor-stop`    | none               | Stops the motor (same effect as pressing SW1 while running)  |
| `motor-reverse` | none               | Reverses motor direction                        |
| `motor-speed`   | integer, `0`-`100` | Sets speed as a percent of max RPM (default 50%) |

A `motor-speed` value outside `0`-`100` is **rejected** (the ack comes back
failed, with a message giving the valid range) rather than silently clamped
to `0` or `100`. The dashboard's
speed field checks this client-side too, so an out-of-range value is caught
immediately with an on-screen error instead of a round trip to the device.

## 3. Software Oscilloscope

The firmware can capture one motor signal into a buffer at a configurable
rate and publish it to /IOTCONNECT as two parallel arrays (elapsed time and
value). It's a one-shot "single sweep" capture, not a continuous trigger.
The dashboard's oscilloscope panel is the recommended way to configure and
trigger a capture and see the result plotted.

The commands below are the
same ones its buttons send, given here for reference (and for sending
directly from the /IOTCONNECT console).

| Command          | Parameter           | Effect                                      |
|------------------|---------------------|----------------------------------------------|
| `scope-channel`  | integer, `0`-`4`    | Selects the signal to capture: `0`=DC bus voltage (default), `1`=PWM duty cycle, `2`=bus current, `3`=phase A current, `4`=phase B current |
| `scope-rate`     | integer, `50`-`3276750` (µs), **multiples of 50** | Sample period in microseconds; default 500µs (2kHz) |
| `scope-length`   | integer, `10`-`1000` | Number of samples per capture; default 50 (a 25ms window at the default rate) |
| `scope-capture`  | none                | Starts a new capture using the current channel/rate/length settings |

A value outside a command's range is **rejected** (the ack comes back failed,
with a message giving the valid range) rather than silently rounded to the
nearest limit.

`scope-rate` is a special case: the sampling clock is the motor control
loop's own 50µs tick, so only whole multiples of 50 are actually achievable.
A value in range but not a multiple of 50 isn't rejected. It's rounded to
the nearest 50µs (e.g. `80` becomes `100`), and the rounded value (not what
was requested) is what's reported back in telemetry and used to label any
capture taken at that rate.

The default rate/length are set to match expected conditions for the included motor,
but users are encouraged to tinker with different settings to achieve the captures they need.

Once a capture fills its buffer, the firmware publishes it (independent of
the normal 10-second telemetry cadence) as a series of **chunks**, sent back to
back, each carrying a slice of the two arrays:

```json
{"osc_seq": 1, "osc_n": 10, "osc_t": "[0,500,1000,1500,2000,2500]", "osc_v": "[4000,4037,4074,15,52,89]"}
{"osc_seq": 2, "osc_n": 10, "osc_t": "[3000,3500,4000,4500,5000,5500]", "osc_v": "[126,163,200,237,274,311]"}
...
```

`osc_seq` is the 1-based chunk number and `osc_n` the total chunk count, so the
capture is complete once `osc_seq` equals `osc_n`; concatenate the `osc_t` and
`osc_v` arrays in `osc_seq` order to rebuild it.

Both arrays are `STRING`-typed attributes carrying a bracketed array of numbers as text, meant
to be parsed back into numbers on the consuming end. `osc_t` is elapsed
microseconds since the capture started (`osc_rate * sample index`); `osc_v` is
the raw value of the selected channel at each point, in the same units as that
channel's regular telemetry field (`vdc`/`duty` for channels 0/1).

The current oscilloscope settings also publish as their own small message, on
the same 10-second cadence as regular telemetry:

```json
{"osc_ch": 0, "osc_rate": 500, "osc_len": 50}
```

They're a separate publish rather than merged into the main telemetry message
because the C2D command handlers for `scope-channel`/`scope-rate`/
`scope-length` publish this immediately on a successful change (not just on
the periodic cadence), so the dashboard's displayed capture metadata is never
stale. Merging it into the main telemetry message would have tied that
immediate publish to the full telemetry snapshot instead.

### Converting to Engineering Units

Values published by the device are raw; the dashboard converts them using this
board's actual component values (from the DM330031 schematic and the
firmware) before plotting:

| Channel | Conversion | Basis |
|---|---|---|
| 0, DC bus voltage | volts = value x 0.002176 | 34k + 34k + 3.3k divider (21.6:1), 3.3 V reference, 12-bit result left-justified in 16 bits and stored `>> 1` |
| 1, PWM duty | % = value / 4999 x 100 | PWM period of 4999 counts (50 us at 200 MHz) |
| 2, 3, 4, bus / phase A / phase B current | amps = (value - offset) x 0.000666 | 10 mOhm shunts, difference amplifier gain 4.02k / (62 + 470) = 7.56, 1.65 V bias, 3.3 V reference; the firmware reports these signed, 0 at mid-scale (about +/-21.8 A full scale, 10.7 mA per ADC step) |

The firmware does no current offset calibration, so with zero current the
current channels read a small non-zero value (amplifier and reference
tolerance). The dashboard plots the raw conversion as-is.

### Suggested Capture Settings

This kit's motor runs from 24 V with 2 pole pairs and a 4600 RPM maximum
(`bldc_main.h`), so the electrical frequency is `RPM / 60 x 2` and one electrical
cycle is `6e7 / (RPM x 2)` us. Six-step commutation changes step six times per
electrical cycle. The sample period floor is 50 us (the 20 kHz PWM), and a capture
holds 10 to 1000 samples. A few hundred samples is plenty for these windows (the
table uses 100). Choose the rate so the window covers about one to one and a half electrical cycles:

| Motor speed | Electrical cycle | One commutation step | Rate for phase / bus current | Window (100 samples) |
|---|---|---|---|---|
| 10% (460 RPM) | 65 ms | 10.9 ms | 700 us | 70 ms |
| 50% (2300 RPM) | 13 ms | 2.2 ms | 150 us | 15 ms |
| 100% (4600 RPM) | 6.5 ms | 1.1 ms | 100 us | 10 ms |

For what to look at:

| Goal | Channel | Rate | Length | Window | Notes |
|---|---|---|---|---|---|
| Commutation current shape | 3 or 4 (phase A/B), 2 (bus) | see table above | 100 | 1-1.5 electrical cycles | The phase shunts are in the low-side legs, so in six-step each one only carries current while its own low-side switch conducts. Expect pulses gated to the commutation sectors, not a sine wave. The bus channel is the DC-link return current. |
| Bus voltage ripple and commutation transients | 0 | 50 us | 100 | 5 ms | A stiff 24 V supply reads nearly flat: a few tens of mV of noise with occasional short excursions. |
| Bus sag / regeneration on start, stop, reverse | 0 | 50 ms | 100 | 5 s | Send `scope-capture`, then the motor command. |
| Speed-loop response (duty step) | 1 | 50 ms (20 ms for a faster look) | 100 | 5 s (2 s) | Send `scope-capture`, then `motor-speed`. Cloud latency is a second or two, so the step lands early in the window, not at time zero. |
| Start-up inrush | 2 | 20 ms | 100 | 2 s | Send `scope-capture`, then `motor-start`. |

The scope samples one channel per capture and has no trigger, so two channels
can't be compared for timing because two captures have no common time reference.
The ADC is sampled once per PWM period, so PWM switching ripple is not visible.

To look at a capture, send `scope-capture` from the dashboard (or the
/IOTCONNECT console) and watch it appear in the dashboard's oscilloscope
panel. It reassembles the chunks in `osc_seq` order and plots them
automatically once the capture is complete, with no manual copying of
anything.

> [!NOTE]
> Chunking exists because of two limits found on the RNWF11.
> It rejects any `AT+MQTTPUB` command line longer than **195 bytes** (including
> the topic and the trailing `\r\n`) with `"Invalid Parameter"`, and it rejects
> a publish sent while the previous one is still waiting for its `+MQTTPUBACK`
> (`"MQTT Error"`), so the firmware waits for each ack before sending the next
> message. That works out to roughly 4-5 samples per chunk which is about 10 chunks
> for the default 50 samples, about 20 for 100, and 200-330 for the
> 1000-sample maximum (each chunk is one /IOTCONNECT message). Each chunk takes
> about 60-130 ms to be acknowledged, so a 100-sample capture publishes in a
> couple of seconds and a 1000-sample one in roughly 20-40 s.
>
> The publish runs a few chunks at a time from the main loop rather than as one
> blocking call, so telemetry, SW1 and the motor control carry on normally
> while it goes. Queued commands wait for it to finish (except `motor-stop`,
> see above), and a capture is abandoned if the broker connection drops
> partway through.
