#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Embed a sanitized optical calibration: no runtime home-directory dependency."""
import hashlib
import json
import pathlib
import sys
source, output = map(pathlib.Path, sys.argv[1:])
raw = source.read_bytes()
root = json.loads(raw)
# Whitelist optical fields even if a local full device JSON is supplied.
keys = {"distortion", "distortion_red", "distortion_blue", "intrinsics",
        "grow_for_undistort", "undistort_r2_cutoff"}
data = {"device": {k: v for k, v in root["device"].items() if k in {
    "eye_target_height_in_pixels", "eye_target_width_in_pixels", "first_eye",
    "last_eye", "num_windows", "physical_aspect_x_over_y"}},
    "tracking_to_eye_transform": [{k: v for k, v in eye.items() if k in keys}
                                  for eye in root["tracking_to_eye_transform"]]}
for eye in data["tracking_to_eye_transform"]:
    for name in ("distortion", "distortion_red", "distortion_blue"):
        if name in eye:
            eye[name] = {k: v for k, v in eye[name].items()
                         if k in {"type", "coeffs", "center_x", "center_y"}}
sha = root.get("source_sha256", hashlib.sha256(raw).hexdigest())
if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
    raise SystemExit("Invalid calibration provenance digest")
encoded = json.dumps(json.dumps(data, ensure_ascii=True, separators=(",", ":")))
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('// Generated from calibration JSON; do not edit.\n#pragma once\n'
                  + 'static const char arpara_optics_json[] = ' + encoded + ';\n'
                  + 'static const char arpara_optics_source_sha256[] = "' + sha + '";\n')
