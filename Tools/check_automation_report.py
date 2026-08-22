#!/usr/bin/env python3
"""Read a UE automation report and fail the task unless every test succeeded.

The editor's own exit code is not usable: it returns non-zero on a clean run,
and it terminates while the log is still flushing, so the last test or two are
routinely missing from the log even though they ran. The exported report is
written after the queue drains and is the only complete account of the run.
"""

import json
import sys
from collections import Counter
from pathlib import Path

PASSING_STATES = {"Success"}


def load_report(path: Path) -> dict:
    # The editor writes the report with a UTF-8 BOM.
    with path.open(encoding="utf-8-sig") as report_file:
        return json.load(report_file)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_automation_report.py <index.json>", file=sys.stderr)
        return 2

    report_path = Path(sys.argv[1])
    if not report_path.is_file():
        print(f"No automation report at {report_path}: the run did not complete.", file=sys.stderr)
        return 1

    tests = load_report(report_path).get("tests", [])
    if not tests:
        print("The automation report holds no tests.", file=sys.stderr)
        return 1

    states = Counter(test.get("state") for test in tests)
    failures = [test for test in tests if test.get("state") not in PASSING_STATES]

    for test in failures:
        print(f"FAILED  {test.get('fullTestPath')}  [{test.get('state')}]", file=sys.stderr)
        for entry in test.get("entries", []):
            if entry.get("event", {}).get("type") == "Error":
                print(f"        {entry['event'].get('message')}", file=sys.stderr)

    summary = ", ".join(f"{count} {state}" for state, count in sorted(states.items()))
    print(f"{len(tests)} automation tests: {summary}")
    if failures:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
