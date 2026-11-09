# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
import unittest
from copy import deepcopy
from xml.etree import ElementTree as ET

import suite_results


class SuiteResultsTest(unittest.TestCase):
    def setUp(self):
        self.names = {"kwin-testFirst", "kwin-testSecond"}
        self.discovery = {
            "tests": [{"name": n, "properties": []} for n in sorted(self.names)]
        }
        self.xml = ET.Element("testsuite")
        for name in sorted(self.names):
            case = ET.SubElement(self.xml, "testcase", name=name, status="run")
            # The JUnit system-out only carries the loader trace on the runner.
            ET.SubElement(
                case, "system-out"
            ).text = "calling init: /build/bin/libkwin.so.6\n"
        self.console = (
            "Start  7: kwin-testFirst\n"
            "7: Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted, 1ms\n"
            "Start  9: kwin-testSecond\n"
            "9: Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted, 1ms\n"
        )

    def validate(self, libkwin_init="calling init: /build/bin/libkwin.so.6"):
        return suite_results.validate(
            self.discovery,
            self.xml,
            self.console,
            libkwin_init,
            expected=self.names,
        )

    def test_complete_inventory_passes(self):
        result = self.validate()
        self.assertEqual(result["suites"], 2)
        self.assertEqual(result["passed"], 6)

    def test_missing_or_disabled_suite_is_failure(self):
        for disabled in (False, True):
            with self.subTest(disabled=disabled):
                data = deepcopy(self.discovery)
                if disabled:
                    data["tests"][0]["properties"] = [
                        {"name": "DISABLED", "value": True}
                    ]
                else:
                    data["tests"].pop()
                with self.assertRaises(suite_results.ValidationError):
                    suite_results.validate(
                        data,
                        self.xml,
                        self.console,
                        "calling init: /build/bin/libkwin.so.6",
                        expected=self.names,
                    )

    def test_qtest_skip_is_failure_even_when_ctest_passed(self):
        self.console = self.console.replace(
            "7: Totals: 3 passed, 0 failed, 0 skipped",
            "7: Totals: 2 passed, 0 failed, 1 skipped",
        )
        with self.assertRaises(suite_results.ValidationError):
            self.validate()

    def test_retry_remains_visible_without_rejecting_final_pass(self):
        self.console = self.console.replace(
            "Start  7: kwin-testFirst\n",
            "Start  7: kwin-testFirst\n"
            "7: Totals: 2 passed, 1 failed, 0 skipped, 0 blacklisted, 1ms\n",
        )
        result = self.validate()
        self.assertEqual(result["failed_qtest_attempts"], 1)
        self.assertEqual(result["passed"], 6)

    def test_duplicate_or_missing_junit_case_is_failure(self):
        self.xml[1].set("name", self.xml[0].get("name"))
        with self.assertRaises(suite_results.ValidationError):
            self.validate()
        self.xml.remove(self.xml[1])
        with self.assertRaises(suite_results.ValidationError):
            self.validate()

    def test_ctest_failure_and_missing_qtest_results_are_failures(self):
        ET.SubElement(self.xml[0], "failure")
        with self.assertRaises(suite_results.ValidationError):
            self.validate()
        self.xml[0].remove(self.xml[0].find("failure"))
        self.console = self.console.replace(
            "7: Totals:", "7: no result here\n7: something:"
        )
        with self.assertRaises(suite_results.ValidationError):
            self.validate()

    def test_skipped_earlier_attempt_is_not_hidden_by_retry(self):
        self.console = self.console.replace(
            "Start  7: kwin-testFirst\n",
            "Start  7: kwin-testFirst\n"
            "7: Totals: 1 passed, 1 failed, 1 skipped, 0 blacklisted, 1ms\n",
        )
        with self.assertRaises(suite_results.ValidationError):
            self.validate()

    def test_empty_qtest_run_is_not_success(self):
        self.console = self.console.replace(
            "7: Totals: 3 passed", "7: Totals: 0 passed"
        )
        with self.assertRaises(suite_results.ValidationError):
            self.validate()

    def test_malformed_discovery_is_rejected(self):
        for data in (None, {"tests": [{"name": {}}, {"name": "kwin-testSecond"}]}):
            with (
                self.subTest(data=data),
                self.assertRaises(suite_results.ValidationError),
            ):
                suite_results.validate(
                    data,
                    self.xml,
                    self.console,
                    "calling init: /build/bin/libkwin.so.6",
                    expected=self.names,
                )

    def test_missing_built_libkwin_is_failure(self):
        with self.assertRaises(suite_results.ValidationError):
            self.validate(libkwin_init="")
        with self.assertRaises(suite_results.ValidationError):
            self.validate(libkwin_init="calling init: /usr/lib64/libkwin.so.6\n")

    def test_system_libkwin_trace_is_failure_even_with_built_one(self):
        trace = "calling init: /build/bin/libkwin.so.6\ncalling init: /usr/lib/libkwin.so.6\n"
        with self.assertRaises(suite_results.ValidationError):
            self.validate(libkwin_init=trace)

    def test_built_libkwin_trace_passes(self):
        result = self.validate(
            libkwin_init="     3 calling init: /build/bin/libkwin.so.6\n"
        )
        self.assertEqual(result["suites"], 2)


if __name__ == "__main__":
    unittest.main()
