#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Extract optics without rewriting numeric JSON tokens or publishing identity data."""
import argparse
import hashlib
import json
import pathlib
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("source", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
p.add_argument("--private-copy", type=pathlib.Path)
a = p.parse_args()
raw = a.source.read_bytes()
text = raw.decode("utf-8")
root = json.loads(text)
if root.get("manufacturer") != "arpara" or root.get("device_class") != "hmd":
    raise SystemExit("Expected an arpara HMD calibration")
# The source's optical subtree has no extrinsics or device identifiers.
allowed_eye_keys = {"distortion", "distortion_red", "distortion_blue", "intrinsics",
                    "grow_for_undistort", "undistort_r2_cutoff"}
if any(set(eye) - allowed_eye_keys for eye in root["tracking_to_eye_transform"]):
    raise SystemExit("Unexpected eye fields: review before extracting")
for eye in root["tracking_to_eye_transform"]:
    for name in ("distortion", "distortion_red", "distortion_blue"):
        if name in eye and set(eye[name]) - {"type", "coeffs", "center_x", "center_y"}:
            raise SystemExit("Unexpected distortion fields: review before extracting")
allowed_device_keys = {"eye_target_height_in_pixels", "eye_target_width_in_pixels",
                       "first_eye", "last_eye", "num_windows", "physical_aspect_x_over_y"}
if set(root["device"]) - allowed_device_keys:
    raise SystemExit("Unexpected device fields: review before extracting")
def original_value(key):
    match = re.search(r'"' + re.escape(key) + r'"\s*:\s*', text)
    _, length = json.JSONDecoder().raw_decode(text[match.end():])
    return text[match.end():match.end() + length]
result = '{\n  "source_sha256": ' + json.dumps(hashlib.sha256(raw).hexdigest())
result += ',\n  "device": ' + original_value("device")
result += ',\n  "tracking_to_eye_transform": ' + original_value("tracking_to_eye_transform") + '\n}\n'
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(result)
if a.private_copy:
    a.private_copy.parent.mkdir(parents=True, exist_ok=True)
    a.private_copy.write_bytes(raw)
