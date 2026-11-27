# SPDX-License-Identifier: MIT
"""Tests for scripts/syncval_report.py.

The raw fixture under tests/fixtures/syncval/ was produced by tests/vulkan-syncval-control on a
software Vulkan device. Every resource in it is a FrameLab-owned synthetic object. The NGX-shaped
logs below are assembled from those messages and from invented names. They contain no NVIDIA
runtime output.
"""
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import syncval_report as report  # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures" / "syncval"
CONTROL = FIXTURES / "control-lavapipe.log"
LEGACY = FIXTURES / "control-lavapipe-1.3.275.log"


def write(text: str, encoding: str = "utf-8") -> Path:
    handle = tempfile.NamedTemporaryFile("wb", suffix=".log", delete=False)
    handle.write(text.encode(encoding))
    handle.close()
    return Path(handle.name)


def quiet_main(args):
    with contextlib.redirect_stdout(io.StringIO()):
        return report.main(args)


def control_message(case: str) -> str:
    """Returns the raw callback text recorded for one control case."""
    lines = CONTROL.read_text(encoding="utf-8").splitlines()
    begin = lines.index(next(l for l in lines if l.startswith(f"SYNCVAL_CONTROL_BEGIN case={case} ")))
    end = next(i for i in range(begin, len(lines)) if lines[i].startswith("SYNCVAL_CONTROL_END "))
    return "\n".join(lines[begin + 1:end])


class ControlFixtureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.run_ = report.parse(CONTROL)
        cls.by_case = {d.case: d for d in cls.run_.diagnostics}

    def test_every_case_passed_and_counts_match_summary(self):
        verdicts = {c["case"]: c["verdict"] for c in self.run_.cases}
        self.assertEqual(len(verdicts), 14)
        self.assertEqual(set(verdicts.values()), {"PASS"})
        self.assertEqual(self.run_.issues, [])
        expected = {c["case"] for c in self.run_.cases if c["expect"] != "none"}
        self.assertEqual(set(self.by_case), expected)
        self.assertTrue(all(d.scope == "control:" + d.case for d in self.run_.diagnostics))

    def test_transition_without_destination_access(self):
        d = self.by_case["image-transition-clear/sync1-no-dst-access"]
        self.assertEqual((d.hazard, d.command, d.resource_type), ("WRITE_AFTER_WRITE", "vkCmdClearColorImage", "VkImage"))
        self.assertEqual(d.resource_name, "framelab.syncval.control.image")
        self.assertEqual((d.prior_kind, d.prior_command), ("layout-transition", "vkCmdPipelineBarrier"))
        self.assertEqual(d.prior_debug_region, "framelab.syncval.control")
        self.assertEqual((d.location, d.dependency), ("record", "none"))
        self.assertEqual(d.prior_access, "SYNC_IMAGE_LAYOUT_TRANSITION")
        self.assertIn("CLEAR", d.access)

    def test_transition_into_compute_scope(self):
        d = self.by_case["image-transition-clear/sync1-compute-write"]
        self.assertEqual(d.dependency, "access")
        self.assertIn("COMPUTE_SHADER", d.allowed)
        self.assertEqual(d.required_access, "VK_ACCESS_2_TRANSFER_WRITE_BIT")
        self.assertEqual(d.required_stage, "VK_PIPELINE_STAGE_2_CLEAR_BIT")
        self.assertIn("COMPUTE_SHADER", d.write_barriers)

    def test_sync2_copy_stage_misses_clear_stage(self):
        d = self.by_case["image-transition-clear/sync2-copy-stage"]
        self.assertEqual((d.dependency, d.prior_command), ("stage", "vkCmdPipelineBarrier2"))
        self.assertEqual(d.required_stage, "VK_PIPELINE_STAGE_2_CLEAR_BIT")
        self.assertIn("COPY", d.write_barriers)

    def test_record_and_submit_time_fills(self):
        record = self.by_case["buffer-fill-fill/no-barrier"]
        self.assertEqual((record.command, record.prior_command, record.location), ("vkCmdFillBuffer", "vkCmdFillBuffer", "record"))
        self.assertEqual(record.resource_name, "framelab.syncval.control.buffer")
        submit = self.by_case["buffer-fill-submit/queue-order-only"]
        self.assertEqual((submit.command, submit.prior_command), ("vkCmdFillBuffer", "vkCmdFillBuffer"))
        self.assertEqual((submit.location, submit.message_type), ("submit", "SubmitTimeError"))
        self.assertTrue(submit.prior_submitted)
        self.assertEqual(submit.debug_region, "framelab.syncval.control")
        self.assertNotEqual(record.signature(), submit.signature())

    def test_fill_stage_change_is_visible(self):
        d = self.by_case["buffer-fill-fill/sync2-copy-stage"]
        self.assertEqual((d.dependency, d.write_barriers, d.prior_command), ("none", "0", "vkCmdFillBuffer"))
        self.assertEqual(d.required_stage, "VK_PIPELINE_STAGE_2_CLEAR_BIT")
        self.assertNotIn("buffer-fill-fill/sync2-clear-stage", self.by_case)

    def test_json_output(self):
        out = write("")
        try:
            self.assertEqual(quiet_main([str(CONTROL), "--json", str(out)]), 0)
            payload = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(payload["counts"]["control"], len(self.by_case))
            self.assertEqual(sum(g["count"] for g in payload["groups"]), len(self.by_case))
        finally:
            out.unlink()


class LegacyFormatTest(unittest.TestCase):
    """Layer 1.3.275 output: older message format and older vkCmdFillBuffer stage classification."""

    @classmethod
    def setUpClass(cls):
        cls.run_ = report.parse(LEGACY)
        cls.by_case = {d.case: d for d in cls.run_.diagnostics}

    def test_cases_and_issues(self):
        self.assertEqual({c["verdict"] for c in self.run_.cases}, {"PASS"})
        self.assertEqual(len(self.run_.cases), 14)
        self.assertTrue(all(d.format == "legacy" for d in self.run_.diagnostics))
        self.assertEqual(self.run_.issues,
                         ["pre-1.4 message format: barrier scopes are reported without the missing access or stage"])

    def test_record_time_fields(self):
        d = self.by_case["image-transition-clear/sync1-compute-write"]
        self.assertEqual(d.message_id, "SYNC-HAZARD-WRITE-AFTER-WRITE")
        self.assertEqual((d.command, d.resource_type, d.resource_name),
                         ("vkCmdClearColorImage", "VkImage", "framelab.syncval.control.image"))
        self.assertEqual((d.prior_kind, d.prior_command), ("layout-transition", "vkCmdPipelineBarrier"))
        self.assertEqual((d.access, d.write_barriers), ("SYNC_CLEAR_TRANSFER_WRITE", "SYNC_COMPUTE_SHADER_SHADER_STORAGE_WRITE"))
        self.assertEqual((d.location, d.dependency), ("record", "insufficient"))
        self.assertEqual(self.by_case["image-transition-clear/sync1-no-dst-access"].dependency, "none")

    def test_old_fill_classification(self):
        d = self.by_case["buffer-fill-fill/sync2-clear-stage"]
        self.assertEqual((d.command, d.access, d.prior_access), ("vkCmdFillBuffer", "SYNC_COPY_TRANSFER_WRITE", "SYNC_COPY_TRANSFER_WRITE"))
        self.assertNotIn("buffer-fill-fill/sync2-copy-stage", self.by_case)

    def test_submit_time_fields(self):
        d = self.by_case["buffer-fill-submit/queue-order-only"]
        self.assertEqual((d.command, d.prior_command, d.location), ("vkCmdFillBuffer", "vkCmdFillBuffer", "submit"))
        self.assertTrue(d.prior_submitted)
        self.assertIsNone(d.resource_type)


class TraceShapeTest(unittest.TestCase):
    def trace(self, frames, outside=""):
        body = []
        message = control_message("image-transition-clear/sync1-no-dst-access")
        for frame, count in enumerate(frames):
            body.append(f"NGX_FG_NATIVE_BEGIN frame={frame} diagnostics=0")
            body += [message] * count
            body.append(f"NGX_FG_NATIVE_END frame={frame} result=0x1")
        if outside:
            body.append(outside)
        total = sum(frames) + (1 if outside else 0)
        body.append(f"SAMPLE_VALIDATION_SUMMARY errors={total} warnings=0")
        return "\n".join(body) + "\n"

    def test_windows_and_grouping(self):
        path = write(self.trace([5, 3]))
        try:
            run = report.parse(path)
        finally:
            path.unlink()
        self.assertEqual([c["inside"] for c in run.ngx_calls], [5, 3])
        self.assertEqual([c["result"] for c in run.ngx_calls], ["0x1", "0x1"])
        self.assertEqual(report.scope_counts(run), {"inside_ngx": 8, "outside": 0, "control": 0})
        groups = report.group(run)
        self.assertEqual(len(groups), 1)
        self.assertEqual(sorted({d.frame for d in next(iter(groups.values()))}), [0, 1])
        self.assertEqual(run.issues, [])

    def test_outside_window_and_interleaved_output(self):
        outside = control_message("buffer-fill-fill/no-barrier")
        text = self.trace([1], outside=outside).replace(
            "NGX_FG_NATIVE_END", "FRAMEINPUTS_CONSUMED source_frame=0\nNGX_FG_NATIVE_END")
        path = write(text)
        try:
            run = report.parse(path)
        finally:
            path.unlink()
        self.assertEqual(report.scope_counts(run)["outside"], 1)
        self.assertEqual(run.ngx_calls[0]["inside"], 1)
        self.assertEqual(run.issues, [])

    def test_utf16_log_and_summary_mismatch(self):
        text = self.trace([2]).replace("errors=2", "errors=3")
        path = write("﻿" + text, "utf-16-le")
        try:
            run = report.parse(path)
        finally:
            path.unlink()
        self.assertEqual(len(run.diagnostics), 2)
        self.assertTrue(any("reports 3 errors but 2" in issue for issue in run.issues))

    def test_incomplete_window(self):
        path = write("NGX_FG_NATIVE_BEGIN frame=0 diagnostics=0\n")
        try:
            run = report.parse(path)
        finally:
            path.unlink()
        self.assertIn("incomplete trace: an NGX evaluation window was not closed", run.issues)

    def test_sanitized_summary_lines(self):
        text = ("NGX_FG_NATIVE_BEGIN frame=0 diagnostics=0\n"
                "vkCmdClearColorImage: WRITE_AFTER_WRITE VkImage [address redacted][example.private] "
                "previous=vkCmdPipelineBarrier[example.Evaluate]\n"
                "vkCmdFillBuffer: WRITE_AFTER_WRITE VkBuffer [address redacted][example.private]\n"
                "NGX_FG_NATIVE_END frame=0 result=0x1\n")
        path = write(text)
        try:
            run = report.parse(path)
        finally:
            path.unlink()
        clear, fill = run.diagnostics
        self.assertEqual((clear.format, clear.resource_name), ("summary", "example.private"))
        self.assertEqual((clear.prior_command, clear.prior_debug_region), ("vkCmdPipelineBarrier", "example.Evaluate"))
        self.assertIsNone(fill.prior_command)
        self.assertEqual(run.ngx_calls[0]["inside"], 2)
        self.assertTrue(any("sanitized summary lines" in issue for issue in run.issues))

    def test_duplicate_limit_is_reported(self):
        message = control_message("buffer-fill-fill/no-barrier").replace(
            "SAMPLE_VALIDATION severity=4096 ",
            "SAMPLE_VALIDATION severity=4096 (Warning - This VUID has now been reported 10 times, which is the "
            "duplicate_message_limit value, this will be the last time reporting it).\n", 1)
        path = write(message + "\n")
        try:
            run = report.parse(path)
        finally:
            path.unlink()
        self.assertTrue(run.diagnostics[0].at_duplicate_limit)
        self.assertEqual(run.diagnostics[0].command, "vkCmdFillBuffer")
        self.assertTrue(any("duplicate-message limit" in issue for issue in run.issues))


class CompareTest(unittest.TestCase):
    def test_compare_reports_removed_signature(self):
        clear = control_message("image-transition-clear/sync1-no-dst-access")
        fill = control_message("buffer-fill-submit/queue-order-only")
        base, candidate = write(clear + "\n" + fill + "\n"), write(clear + "\n")
        try:
            text, changed = report.compare(report.parse(base), report.parse(candidate))
            self.assertTrue(changed)
            self.assertIn("| 1 | 0 |", text)
            self.assertEqual(quiet_main(["--compare", str(base), str(base), "--fail-on-change"]), 0)
            self.assertEqual(quiet_main(["--compare", str(base), str(candidate), "--fail-on-change"]), 1)
            self.assertEqual(quiet_main([str(candidate), "--fail-on-diagnostics"]), 1)
        finally:
            base.unlink()
            candidate.unlink()


if __name__ == "__main__":
    unittest.main()
