# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import publisher_report as report


class SupervisorTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.state = Path(self.temp.name) / "state"
        self.run_id = "a" * 32
        self.directory = self.state / "runs" / self.run_id
        self.directory.mkdir(mode=0o700, parents=True)
        self.state.chmod(0o700)
        (self.state / "runs").chmod(0o700)
        self.environment = {
            "MONITOR_UNIT": "kwin-upstream-publisher.service",
            "MONITOR_INVOCATION_ID": self.run_id,
            "MONITOR_SERVICE_RESULT": "timeout",
        }
        self.env = patch.dict(os.environ, self.environment)
        self.env.start()
        self.addCleanup(self.env.stop)

    def previous(self, status="running", **extra):
        data = dict(run_id=self.run_id, status=status, **extra)
        (self.directory / "result.json").write_text(json.dumps(data))
        (self.state / "latest.json").write_text(json.dumps(data))

    def test_timeout_replaces_running_state_and_preserves_diagnostics(self):
        self.previous()
        (self.directory / "integration.json").write_text("retained diagnostic\n")
        with patch.object(report, "notify", return_value=True) as notify:
            self.assertEqual(report.service_failure(self.state), 0)
        latest = json.loads((self.state / "latest.json").read_text())
        self.assertEqual(latest["status"], "service_failed")
        self.assertEqual(latest["service_result"], "timeout")
        self.assertTrue(latest["alert_accepted"])
        self.assertEqual(notify.call_count, 1)
        self.assertEqual(
            (self.directory / "integration.json").read_text(), "retained diagnostic\n"
        )

    def test_startup_failure_without_any_python_state_is_reported(self):
        with patch.object(report, "notify", return_value=True):
            self.assertEqual(report.service_failure(self.state), 0)
        self.assertEqual(
            json.loads((self.state / "latest.json").read_text())["status"],
            "service_failed",
        )

    def test_already_reported_failure_and_repeated_reporter_do_not_send_again(self):
        self.previous("error", alert_accepted=True)
        with patch.object(report, "notify", side_effect=AssertionError("duplicate")):
            self.assertEqual(report.service_failure(self.state), 0)
            self.assertEqual(report.service_failure(self.state), 0)

    def test_an_older_failure_cannot_overwrite_a_new_run(self):
        self.previous()
        new = {"run_id": "b" * 32, "status": "running"}
        (self.state / "latest.json").write_text(json.dumps(new))
        with patch.object(report, "notify", return_value=True):
            self.assertEqual(report.service_failure(self.state), 0)
        self.assertEqual(json.loads((self.state / "latest.json").read_text()), new)
        failure = json.loads((self.directory / "supervisor.json").read_text())
        self.assertEqual(failure["run_id"], self.run_id)

    def test_unknown_unit_or_path_like_invocation_is_refused(self):
        for value in (
            {"MONITOR_UNIT": "other.service"},
            {"MONITOR_INVOCATION_ID": "../outside"},
        ):
            with (
                self.subTest(value=value),
                patch.dict(os.environ, value),
                patch.object(
                    report, "notify", side_effect=AssertionError("must refuse")
                ),
            ):
                self.assertEqual(report.service_failure(self.state), 1)

    def test_oversized_valid_newer_record_is_preserved_byte_for_byte(self):
        self.previous()
        # The diagnostics writer can represent a pre-fix oversized status file.
        latest = self.state / "latest.json"
        report.atomic_json(
            latest,
            {"run_id": "b" * 32, "status": "current", "diagnostics": "\x01" * 65536},
            max_bytes=None,
        )
        before = latest.read_bytes()
        self.assertGreater(len(before), report.MAX_RECORD_BYTES)
        with patch.object(report, "notify", return_value=True):
            self.assertEqual(report.service_failure(self.state), 0)
        self.assertEqual(latest.read_bytes(), before)

    def test_missing_and_unreadable_records_are_distinct(self):
        path = self.state / "sample.json"
        self.assertIsNone(report.read_record(path))
        for raw in (
            "not-json",
            "[]",
            "null",
            '{"note": "' + "x" * report.MAX_RECORD_BYTES + '"}',
        ):
            path.write_text(raw)
            self.assertIs(report.read_record(path), report.UNREADABLE)

    def test_largest_permitted_status_keeps_acknowledgement_readable(self):
        path = self.directory / "result.json"
        data = {
            "run_id": self.run_id,
            "status": "error",
            "alert_accepted": True,
            "message": "",
        }
        overhead = len((json.dumps(data, indent=2) + "\n").encode())
        data["message"] = "x" * (report.MAX_RECORD_BYTES - overhead)
        report.atomic_json(path, data)
        self.assertEqual(path.stat().st_size, report.MAX_RECORD_BYTES)
        self.assertEqual(report.read_record(path), data)
        with patch.object(
            report, "notify", side_effect=AssertionError("duplicate acknowledgement")
        ):
            self.assertEqual(report.service_failure(self.state), 0)
        before = path.read_bytes()
        data["message"] += "x"
        with self.assertRaises(ValueError):
            report.atomic_json(path, data)
        self.assertEqual(path.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
