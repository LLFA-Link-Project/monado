# SPDX-License-Identifier: BSL-1.0
import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("latency", pathlib.Path(__file__).parents[1] / "scripts/analyze-drm-latency.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class AnalysisTest(unittest.TestCase):
    def test_pose_clock_annotations_and_receipt_age(self):
        text = "arpara pose timing: requested_ns=3000000 receipt_ns=1000000 query_ns=2000000 fusion_ns=500000"
        result = module.analyze_pose(text)
        self.assertEqual(result["host_receipt_age"]["p50_ms"], 1)
        self.assertEqual(result["requested_future_horizon"]["p95_ms"], 1)
        self.assertEqual(result["receipt_minus_integrated_device_time"]["max_ms"], .5)
    def test_missing_gpu_and_unmapped_clock_are_not_measurements(self):
        rows = [{"event": "flip_receipt", "frame_id": 1, "monotonic_ns": 123, "value": 1}]
        result = module.analyze(rows)
        self.assertIsNone(result["gpu_end_to_flip"])
        self.assertIsNone(result["flip_interval"])
        self.assertEqual(result["unmapped_flip_receipts"], 1)

    def test_sequence_wrap_gaps_signed_error_and_percentiles(self):
        rows = []
        for i, seq in enumerate((0xfffffffe, 0xffffffff, 1)):
            for event, ns, value in (("desired", (i + 1) * 10000000 + 1000000, 0),
                                     ("flip", (i + 1) * 10000000, seq)):
                rows.append(dict(event=event, frame_id=i, monotonic_ns=ns, value=value))
        result = module.analyze(rows)
        self.assertEqual(result["vblanks_without_update"], 1)
        self.assertEqual(result["late_desired_over_0_5ms"], 0)
        self.assertEqual(result["flip_minus_desired"]["p99_ms"], -1)
        self.assertEqual(result["flip_interval"]["p50_ms"], 10)
        self.assertEqual(result["update_fps"], 100)


if __name__ == "__main__":
    unittest.main()
