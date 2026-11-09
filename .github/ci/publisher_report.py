# SPDX-FileCopyrightText: 2026 Wisbendji Fimerlus <archledger236@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
"""Private records and an independent systemd failure reporter for KWin updates."""

import argparse
import fcntl
import json
import os
import re
import stat
import tempfile
import urllib.request
from contextlib import contextmanager
from datetime import datetime, timezone
from http.client import HTTPException
from pathlib import Path

MAX_RECORD_BYTES = 131072
UNREADABLE = object()


def atomic_json(path: Path, value: dict, *, max_bytes=MAX_RECORD_BYTES):
    payload = (json.dumps(value, indent=2) + "\n").encode("utf-8")
    if max_bytes is not None and len(payload) > max_bytes:
        raise ValueError("Publisher status exceeds the serialized record limit")
    fd, temporary = tempfile.mkstemp(prefix="." + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def private_directory(path: Path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_mode & 0o077
    ):
        raise ValueError(
            "State directory must be private and owned by the current user"
        )


@contextmanager
def record_lock(path):
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "a") as lock:
        if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode):
            raise ValueError("Invalid publisher lock")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_args, **_kwargs):
        return None


def notify(run_id: str, *, test: bool = False) -> bool:
    """Server acceptance is not proof of device delivery. Never expose the topic."""
    try:
        path = Path.home() / ".config/paseo-health/ntfy-topic"
        with path.open() as stream:
            topic = stream.read(202).strip()
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,200}", topic):
            return False
        title = ("TEST: " if test else "") + "KWin upstream publisher needs attention"
        body = (
            f"Stable-update publisher {'notification test' if test else 'failed'} on minihost.\n"
            f"Run: {run_id}\nInspect ~/.local/state/kwin-upstream-publisher/ "
            "(latest.json and last-service-failure.json) and "
            "journalctl --user -u kwin-upstream-publisher.service.\n"
            "No source merge or package installation was performed."
        )
        request = urllib.request.Request(
            "https://ntfy.sh/" + topic,
            data=body.encode(),
            method="POST",
            headers={"Title": title, "Tags": "warning", "Priority": "default"},
        )
        opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), NoRedirect()
        )
        with opener.open(request, timeout=10) as response:
            data = json.loads(response.read(4096))
            return (
                response.status == 200
                and isinstance(data, dict)
                and data.get("event") == "message"
                and isinstance(data.get("id"), str)
            )
    except (OSError, ValueError, HTTPException):
        return False


def read_record(path):
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_RECORD_BYTES:
                return UNREADABLE
            payload = stream.read(MAX_RECORD_BYTES + 1)
            if len(payload) > MAX_RECORD_BYTES:
                return UNREADABLE
            value = json.loads(payload)
            return value if isinstance(value, dict) else UNREADABLE
    except FileNotFoundError:
        return None
    except (OSError, ValueError):
        return UNREADABLE


def save_failure(state, directory, record):
    # The original result and integration diagnostics remain intact.
    atomic_json(directory / "supervisor.json", record)
    atomic_json(state / "last-service-failure.json", record)
    try:
        with record_lock(state / "run.lock"):
            latest = read_record(state / "latest.json")
            if latest is None or (
                isinstance(latest, dict) and latest.get("run_id") == record["run_id"]
            ):
                atomic_json(state / "latest.json", record)
    except BlockingIOError:
        pass  # A newer source invocation owns latest.json.


def service_failure(
    state: Path, expected_unit="kwin-upstream-publisher.service"
) -> int:
    run_id = os.environ.get("MONITOR_INVOCATION_ID", "")
    result = os.environ.get("MONITOR_SERVICE_RESULT", "")
    if (
        os.environ.get("MONITOR_UNIT") != expected_unit
        or not re.fullmatch(r"[0-9a-f]{32}", run_id)
        or not re.fullmatch(r"[a-z-]{1,40}", result)
        or result == "success"
    ):
        print(json.dumps({"status": "invalid_supervisor_context"}))
        return 1
    try:
        private_directory(state)
        private_directory(state / "runs")
        directory = state / "runs" / run_id
        private_directory(directory)
        with record_lock(state / "supervisor.lock"):
            old = read_record(directory / "supervisor.json")
            previous = read_record(directory / "result.json")
            readable = isinstance(previous, dict)
            previous = previous if readable else {}
            prior_status = previous.get("status")
            if not isinstance(prior_status, str) or len(prior_status) > 40:
                prior_status = None
            accepted = any(
                isinstance(item, dict)
                and item.get("run_id") == run_id
                and item.get("alert_accepted") is True
                for item in (old, previous)
            )
            record = {
                "run_id": run_id,
                "status": "service_failed",
                "service_result": result,
                "time": datetime.now(timezone.utc).isoformat(),
                "prior_status": prior_status,
                "prior_record_readable": readable,
                "alert_accepted": accepted,
            }
            save_failure(state, directory, record)
            if not accepted:
                record["alert_accepted"] = notify(run_id)
                save_failure(state, directory, record)
            print(json.dumps(record))
            return 0 if record["alert_accepted"] else 1
    except BlockingIOError:
        print(json.dumps({"status": "reporter_busy", "run_id": run_id}))
        return 0
    except (OSError, ValueError):
        print(
            json.dumps(
                {
                    "status": "reporter_storage_failed",
                    "run_id": run_id,
                    "alert_accepted": notify(run_id),
                }
            )
        )
        return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--state",
        type=Path,
        default=Path.home() / ".local/state/kwin-upstream-publisher",
    )
    parser.add_argument("--expected-unit", default="kwin-upstream-publisher.service")
    args = parser.parse_args()
    return service_failure(args.state, args.expected_unit)


if __name__ == "__main__":
    raise SystemExit(main())
