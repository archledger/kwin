# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the required CTest inventory and QTest results, including skips."""

import argparse
import json
import re
import sys
from pathlib import Path
from xml.etree import ElementTree as ET

REQUIRED = {
    "kwin-test" + name
    for name in (
        "X11Window",
        "DecorationInput",
        "DontCrashEmptyDeco",
        "DontCrashAuroraeDestroyDeco",
        "Tiles",
        "XdgShellWindow",
        "QuickTiling",
        "MoveResize",
        "FractionalScaling",
        "OutputChanges",
        "Maximized",
        "ScreenChanges",
        "OffscreenQuickView",
        "InternalWindow",
        "TabBox",
        "Placement",
        "TransientPlacement",
        "StartupDefaults",
        "Rect",
        "RectF",
        "Fifo",
        "HangDetectorShutdown",
        "WindowThumbnailSource",
        "LibinputConnection",
        "TabletModeStartup",
    )
} | {"kwayland-testWaylandSurface"}


class ValidationError(ValueError):
    pass


def validate(
    discovery: dict,
    junit: ET.Element,
    console: str,
    libkwin_init: str,
    expected=REQUIRED,
) -> dict:
    if not isinstance(discovery, dict):
        raise ValidationError("Invalid CTest discovery data")
    tests = discovery.get("tests")
    if not isinstance(tests, list) or not all(isinstance(test, dict) for test in tests):
        raise ValidationError("Invalid CTest discovery data")
    names = [test.get("name") for test in tests]
    if (
        not all(isinstance(name, str) for name in names)
        or len(names) != len(expected)
        or set(names) != set(expected)
    ):
        raise ValidationError("Missing, duplicate or unexpected required suites")
    for test in tests:
        properties = test.get("properties", [])
        if not isinstance(properties, list) or not all(
            isinstance(p, dict) for p in properties
        ):
            raise ValidationError("Invalid CTest properties")
        for prop in properties:
            if prop.get("name") == "DISABLED" and str(
                prop.get("value")
            ).lower() not in {"false", "off", "0"}:
                raise ValidationError("Required suite is disabled")
    cases = list(junit.iter("testcase"))
    if len(cases) != len(expected) or {case.get("name") for case in cases} != set(
        expected
    ):
        raise ValidationError(
            "JUnit does not contain every required suite exactly once"
        )
    for case in cases:
        if case.get("status") != "run" or any(
            case.find(tag) is not None for tag in ("failure", "error", "skipped")
        ):
            raise ValidationError("Required CTest suite failed or did not run")
    # ctest -V prefixes every line of a test's output with its test number, and
    # the JUnit system-out only carries the loader trace, so the per-suite QTest
    # results are parsed from the console log instead.
    start = re.compile(r"^\s*Start\s+(\d+): (\S+)", re.MULTILINE)
    numbers = {int(number): name for number, name in start.findall(console)}
    if not numbers:
        raise ValidationError("Console log has no test start markers")
    duplicate_names = len(numbers) != len(set(numbers.values()))
    if duplicate_names or set(numbers.values()) != set(expected):
        raise ValidationError("Console log does not cover every required suite")
    per_suite: dict[str, list[tuple[int, ...]]] = {name: [] for name in expected}
    prefixed = re.compile(
        r"^(\d+): Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted",
        re.MULTILINE,
    )
    for number, *counts in prefixed.findall(console):
        name = numbers.get(int(number))
        if name in per_suite:
            per_suite[name].append(tuple(int(value) for value in counts))
    passed = 0
    failed_attempts = 0
    for name, attempts in per_suite.items():
        if not attempts:
            raise ValidationError(f"Required suite {name} has no QTest result")
        failed_attempts += sum(row[1] != 0 for row in attempts)
        if any(row[2] or row[3] for row in attempts):
            raise ValidationError(
                f"Required suite {name} skipped or blacklisted cases in an attempt"
            )
        success, failed, skipped, blacklisted = attempts[-1]
        if not success or failed or skipped or blacklisted:
            raise ValidationError(
                f"Required suite {name} ended with failed, skipped or blacklisted cases"
            )
        passed += success
    if "calling init: /build/bin/libkwin.so" not in libkwin_init:
        raise ValidationError("Built libkwin was not loaded by the test suites")
    if re.search(r"calling init: /usr/lib(64)?/libkwin\.so", libkwin_init):
        raise ValidationError("Fedora's libkwin was loaded by the test suites")
    return {
        "suites": len(cases),
        "passed": passed,
        "failed_qtest_attempts": failed_attempts,
    }


def read(path: Path) -> str:
    if path.stat().st_size > 64 * 1024 * 1024:
        raise ValidationError("Result file exceeds the size limit")
    return path.read_text()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--discovery", type=Path, required=True)
    parser.add_argument("--junit", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--libkwin-init", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = validate(
            json.loads(read(args.discovery)),
            ET.fromstring(read(args.junit)),
            read(args.log),
            read(args.libkwin_init),
        )
    except (ValueError, OSError, ET.ParseError) as error:
        print(f"Test-result validation failed: {error}", file=sys.stderr)
        return 1
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
