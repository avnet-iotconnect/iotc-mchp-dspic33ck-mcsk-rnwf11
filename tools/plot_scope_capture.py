#!/usr/bin/env python3
"""Plot a software-oscilloscope capture published by the dsPIC33CK firmware.

The firmware publishes one capture as a series of small chunks (see the
"Software Oscilloscope" section of the README), each one a telemetry message
carrying:

    osc_seq  1-based chunk number
    osc_n    total chunks in the capture
    osc_t    elapsed microseconds per sample, as a bracketed array string
    osc_v    channel value per sample, as a bracketed array string

This script rebuilds a capture from those chunks and plots it. It reads the
raw telemetry copied from the device in the /IOTCONNECT console, one JSON
object per line, in either of the two shapes the console gives:

    {"dt":"2026-09-21T16:46:51.2Z","d":[{"dt":"...","d":{"osc_seq":1,...}}]}
    {"time":"9/21/2026, 10:28:27 AM","reporting":{"osc_seq":1,...}}

Lines are put in time order using their timestamps (or, for the second shape,
which only has whole-second times, by detecting a newest-first listing), so
the order you paste them in doesn't matter. No credentials are needed:

    python3 tools/plot_scope_capture.py live_data.txt
    python3 tools/plot_scope_capture.py live_data.txt --out capture.png
    pbpaste | python3 tools/plot_scope_capture.py -

Lines that aren't JSON, or aren't oscilloscope chunks, are ignored, so it's
fine to paste a whole Live Data dump. If the input holds several captures, the
last complete one is plotted (--index picks another; --list shows them all).

Each channel is converted to engineering units using the board's actual
component values (bus voltage in V, duty in %, currents in A - see the constants
at the top of the file); pass --raw to plot the raw counts instead. The current
channels have no offset calibration in the firmware: capture with the motor
stopped and pass the mean raw value as --offset to zero them.

Requires matplotlib (pip install matplotlib).
"""
import argparse
import json
import re
import sys
from datetime import datetime

# --- Scaling to engineering units --------------------------------------
# Values come from the DM330031 LVMC board schematic and the firmware; see the
# README's "Software Oscilloscope" section for the derivations.
#
# ADC: 12-bit results left-justified in 16 bits (ADCON1H.FORM = 1), 3.3 V
# reference, so one 16-bit count is 3.3 V / 65536.
ADC_VREF = 3.3
ADC_LSB_V = ADC_VREF / 65536.0

# Ch 0, DC bus voltage: VDC -> R69 34k -> R77 34k -> node -> R87 3.3k -> AGND
# (sheet 3), node -> AN15. The firmware stores ADCBUF15 >> 1 as "vdc".
VBUS_DIVIDER = (34e3 + 34e3 + 3.3e3) / 3.3e3               # = 21.606
VBUS_VOLTS_PER_COUNT = ADC_LSB_V * 2 * VBUS_DIVIDER         # ~2.176 mV

# Ch 2-4, currents: 10 mOhm shunts (Rsh1/2/4, sheet 4) into the internal
# op-amps as a difference amplifier (sheet 2): Rin = 62R + 470R, Rf = 4.02k,
# biased to VREF ~1.65 V. Firmware reports them signed, 0 = mid-scale.
SHUNT_OHMS = 0.010
OPAMP_GAIN = 4020.0 / (62.0 + 470.0)                        # = 7.556
AMPS_PER_COUNT = ADC_LSB_V / (OPAMP_GAIN * SHUNT_OHMS)      # ~0.666 mA

# Ch 1, duty: raw PWM compare value out of the PWM period,
# LOOPTIME_TCY = (50 us * 200 MHz / 2) - 1.
PWM_PERIOD_COUNTS = 4999

# Must match the SCOPE_CHANNEL_* indices in scope_commands.h.
CHANNEL_NAMES = {
    0: "DC bus voltage (raw ADC >>1)",
    1: "PWM duty cycle (raw counts)",
    2: "Bus current (raw ADC)",
    3: "Phase A current (raw ADC)",
    4: "Phase B current (raw ADC)",
}


def convert(channel, values, offset=0):
    """Raw scope values -> (values in engineering units, axis label)."""
    if channel == 0:
        return [x * VBUS_VOLTS_PER_COUNT for x in values], "DC bus voltage (V)"
    if channel == 1:
        return [100.0 * x / PWM_PERIOD_COUNTS for x in values], "PWM duty cycle (%)"
    if channel in (2, 3, 4):
        name = CHANNEL_NAMES[channel].split(" (")[0]
        return [(x - offset) * AMPS_PER_COUNT for x in values], f"{name} (A)"
    return values, "Value (raw)"


def parse_array(text):
    """'[0,500,1000]' -> [0, 500, 1000]"""
    text = text.strip()
    if text.startswith("[") and text.endswith("]"):
        text = text[1:-1]
    return [int(x) for x in text.split(",") if x.strip()]


def parse_dt(text):
    """ISO-8601 timestamp with 1-7 fractional digits -> datetime (UTC)."""
    m = re.match(r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d+))?", text or "")
    if not m:
        return None
    frac = (m.group(2) or "0")[:6].ljust(6, "0")
    return datetime.strptime(m.group(1) + "." + frac, "%Y-%m-%dT%H:%M:%S.%f")


def read_records(stream):
    """Yield (datetime or None, fields dict) for every telemetry JSON line."""
    for line in stream:
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(obj.get("d"), list):          # raw: {"dt":..,"d":[{"dt":..,"d":{..}}]}
            for entry in obj["d"]:
                if isinstance(entry, dict) and isinstance(entry.get("d"), dict):
                    yield parse_dt(entry.get("dt") or obj.get("dt")), entry["d"]
        elif isinstance(obj.get("reporting"), dict):  # console: {"time":..,"reporting":{..}}
            yield None, obj["reporting"]


def build_captures(records):
    """Group chunk records into captures, in chronological order.

    Each capture is a dict with 'n', 'chunks' ({seq: (t, v)}), 'complete' and
    'channel' (the osc_ch reported most recently before the capture, or None).
    """
    records = list(records)
    if records and all(dt is not None for dt, _ in records):
        records.sort(key=lambda r: r[0])            # stable: keeps ties in input order
    else:
        # No usable timestamps: the console lists newest first, so a capture's
        # chunks run n..1 - detect that from the chunks and flip to chronological.
        first = [r for _, r in records if "osc_seq" in r]
        if len(first) > 1 and int(first[0]["osc_seq"]) > int(first[-1]["osc_seq"]):
            records.reverse()

    captures = []
    current = None
    channel = None
    for _, r in records:
        if "osc_ch" in r:
            channel = int(r["osc_ch"])
        if not ("osc_seq" in r and "osc_t" in r and "osc_v" in r):
            continue
        seq, n = int(r["osc_seq"]), int(r["osc_n"])
        # A new capture starts when seq 1 arrives again or a seq repeats.
        if current is None or seq in current["chunks"] or (seq == 1 and current["chunks"]):
            current = {"n": n, "chunks": {}, "channel": channel}
            captures.append(current)
        current["n"] = n
        current["chunks"][seq] = (parse_array(r["osc_t"]), parse_array(r["osc_v"]))
    for c in captures:
        c["complete"] = sorted(c["chunks"]) == list(range(1, c["n"] + 1))
    return captures


def flatten(capture):
    t, v = [], []
    for seq in sorted(capture["chunks"]):
        ct, cv = capture["chunks"][seq]
        t += ct
        v += cv
    return t, v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="file of Live Data lines, or - for stdin")
    ap.add_argument("--out", help="save the plot to this image file instead of opening a window")
    ap.add_argument("--index", type=int, default=-1, help="which capture to plot (default -1 = last complete one)")
    ap.add_argument("--channel", type=int, help="channel number, if not present in the input as osc_ch")
    ap.add_argument("--raw", action="store_true", help="plot raw counts instead of converting to volts / amps / percent")
    ap.add_argument("--offset", type=int, default=0,
                    help="current channels: raw counts to subtract as the zero-current offset "
                         "(capture with the motor stopped and use its mean; the firmware does no offset calibration)")
    ap.add_argument("--list", action="store_true", help="list the captures found and exit")
    args = ap.parse_args()

    stream = sys.stdin if args.input == "-" else open(args.input, encoding="utf-8")
    captures = build_captures(read_records(stream))

    if not captures:
        sys.exit("No oscilloscope chunks (osc_seq/osc_t/osc_v) found in the input.")

    if args.list:
        for i, c in enumerate(captures):
            t, v = flatten(c)
            state = "complete" if c["complete"] else f"INCOMPLETE ({len(c['chunks'])}/{c['n']} chunks)"
            print(f"[{i}] {len(v)} samples, {state}")
        return

    complete = [c for c in captures if c["complete"]]
    if args.index == -1:
        if not complete:
            sys.exit("Found capture chunks but none form a complete capture (use --list).")
        capture = complete[-1]
    else:
        capture = captures[args.index]
        if not capture["complete"]:
            print("warning: this capture is incomplete - missing chunks", file=sys.stderr)

    channel = args.channel if args.channel is not None else capture["channel"]
    t, v = flatten(capture)
    if len(t) != len(v):
        sys.exit(f"osc_t has {len(t)} points but osc_v has {len(v)} - corrupted capture")

    import matplotlib
    if args.out:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    ylabel = CHANNEL_NAMES.get(channel, "Value (raw)")
    if not args.raw:
        v, ylabel = convert(channel, v, args.offset)

    period_us = (t[1] - t[0]) if len(t) > 1 else 0
    fig, ax = plt.subplots(figsize=(10, 4.5))
    # Pick a readable time unit for the span of the capture.
    span = max(t) if t else 0
    unit, div = ("s", 1e6) if span >= 2e6 else ("ms", 1e3) if span >= 2e3 else ("us", 1.0)
    ax.plot([x / div for x in t], v, marker=".", linewidth=1)
    ax.set_xlabel(f"Time ({unit})")
    ax.set_ylabel(ylabel)
    ax.set_title(f"Scope capture - {len(v)} samples, {period_us} us/sample"
                 f" ({1e6 / period_us:g} Hz)" if period_us else "Scope capture")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    if args.out:
        fig.savefig(args.out, dpi=120)
        print(f"saved {args.out}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
