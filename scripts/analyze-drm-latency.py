#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Summarize DRM frame observations; missing GPU timestamps stay unmeasured."""
import argparse
import csv
import json
import math
import re
from collections import defaultdict


def stats(values):
    if not values:
        return None
    values = sorted(values)
    return {"count": len(values), "mean_ms": sum(values) / len(values) / 1e6,
            **{name: values[max(0, math.ceil(len(values) * p) - 1)] / 1e6
               for name, p in (("p50_ms", .5), ("p95_ms", .95), ("p99_ms", .99), ("max_ms", 1))}}


def analyze(rows):
    frames = defaultdict(dict)
    for row in rows:
        frames[int(row["frame_id"])][row["event"]] = (int(row["monotonic_ns"]), int(row["value"]))
    pairs = {"pre_frame_wait": ("pre_frame_wait_begin", "pre_frame_wait_end"),
             "resource_wait": ("resource_wait_begin", "resource_wait_end"),
             "queue_idle_wait": ("queue_idle_begin", "queue_idle_end"),
             "submit_cpu": ("submit_begin", "submit_end"),
             "submit_to_gpu_end": ("submit_begin", "gpu_end"),
             "gpu_end_to_flip": ("gpu_end", "flip"),
             "commit_to_flip": ("commit_begin", "flip"),
             "present_request_to_commit": ("present_request", "commit_begin"),
             "pose_fetch_to_flip": ("pose_return", "flip"),
             "submit_to_flip": ("submit_begin", "flip"),
             "flip_minus_desired": ("desired", "flip"),
             "flip_minus_display_estimate": ("display_estimate", "flip")}
    result = {name: stats([f[b][0] - f[a][0] for f in frames.values() if a in f and b in f])
              for name, (a, b) in pairs.items()}
    result["gpu_duration"] = stats([f["gpu_duration"][1] for f in frames.values() if "gpu_duration" in f])
    flips = sorted((f["flip"][0], f["flip"][1]) for f in frames.values() if "flip" in f)
    intervals = [b[0] - a[0] for a, b in zip(flips, flips[1:])]
    gaps = [((b[1] - a[1]) & 0xffffffff) for a, b in zip(flips, flips[1:])]
    result["flip_interval"] = stats(intervals)
    result["update_fps"] = len(intervals) * 1e9 / sum(intervals) if intervals and sum(intervals) else None
    result["vblanks_without_update"] = sum(g - 1 for g in gaps if 0 < g < 0x80000000)
    result["late_desired_over_0_5ms"] = sum(f["flip"][0] > f["desired"][0] + 500000
                                            for f in frames.values() if "flip" in f and "desired" in f)
    events = sorted([(f["submit_end"][0], 1) for f in frames.values() if "submit_end" in f and "flip" in f]
                    + [(f["flip"][0], -1) for f in frames.values() if "submit_end" in f and "flip" in f])
    pending = peak = 0
    for _, delta in events:
        pending += delta
        peak = max(peak, pending)
    result["max_submitted_not_flipped_complete_frames"] = peak
    result["unmapped_flip_receipts"] = sum("flip_receipt" in f for f in frames.values())
    result["frames_requested_without_flip"] = sum("present_request" in f and "flip" not in f and "flip_receipt" not in f
                                                  for f in frames.values())
    result["notes"] = ["null means unavailable; no clock-domain substitution",
                       "flip is a KMS event, not photon emission or application frame freshness",
                       "sequence gaps count unchanged scanout vblanks, not dropped application frames",
                       "gpu_duration covers compositor query commands, not whole application GPU work"]
    return result


def timeline(rows):
    # Host-mapped absolute observations only. Durations/estimates aren't GPU completion.
    stages = {"begin": "#666", "pose_return": "#955", "submit_begin": "#06a",
              "gpu_start": "#198", "gpu_end": "#080", "commit_begin": "#d80", "flip": "#905"}
    frames = defaultdict(list)
    for row in rows:
        if row["event"] in stages:
            frames[int(row["frame_id"])].append((int(row["monotonic_ns"]), row["event"]))
    selected = sorted(frames.items())[:80]
    timestamps = [ns for _, events in selected for ns, _ in events]
    if not timestamps:
        return "<p>No host-mapped timing observations.</p>"
    first, last = min(timestamps), max(timestamps)
    width = 1400
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{len(selected)*24+45}">']
    for i, (frame, events) in enumerate(selected):
        y = 35 + i * 24
        svg.append(f'<text x="5" y="{y+4}" font-size="12">{frame}</text>')
        for ns, stage in events:
            x = 90 + (ns - first) * (width - 110) / max(1, last - first)
            svg.append(f'<circle cx="{x}" cy="{y}" r="4" fill="{stages[stage]}">'
                       f'<title>frame {frame}: {stage}, +{(ns-first)/1e6:.3f} ms</title></circle>')
    svg.append('</svg>')
    legend = ', '.join(stages)
    return '<!doctype html><meta charset="utf-8"><title>DRM frame timeline</title>' \
           + f'<p>First 80 frames. Hover for observations. Order: {legend}. KMS flip is not photon emission.</p>' \
           + ''.join(svg)


def analyze_pose(text):
    samples = [tuple(map(int, match)) for match in re.findall(
        r"arpara pose timing: requested_ns=(\d+) receipt_ns=(\d+) query_ns=(\d+) fusion_ns=(\d+)", text)]
    samples = [s for s in samples if s[1] > 0]
    return {"host_receipt_age": stats([query - receipt for _, receipt, query, _ in samples]),
            "requested_future_horizon": stats([requested - query for requested, _, query, _ in samples]),
            "receipt_minus_integrated_device_time": stats([receipt - fusion for _, receipt, _, fusion in samples]),
            "note": "host receipt age excludes USB/device latency; fusion clock is anchored and integrated, not synchronized capture time"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("--pose-log")
    parser.add_argument("--html", help="write a scrollable host-clock timeline")
    args = parser.parse_args()
    with open(args.trace, newline="") as stream:
        rows = list(csv.DictReader(stream))
        result = analyze(rows)
    if args.html:
        with open(args.html, "w") as stream:
            stream.write(timeline(rows))
    if args.pose_log:
        with open(args.pose_log) as stream:
            result["pose"] = analyze_pose(stream.read())
    print(json.dumps(result, indent=2))
