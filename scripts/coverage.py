#!/usr/bin/env python3
"""Aggregate gcov line coverage over the C sources into one report.

Run after building with `-fprofile-arcs -ftest-coverage` and executing the unit
and/or integration tests (see `make coverage`). Reads the .gcda/.gcno produced
under build/ and prints an overall line-coverage percentage plus a per-file
table. Uses plain `gcov` (ships with gcc) — no lcov/gcovr dependency.
"""
import os
import re
import subprocess
import sys

BUILD = "build"


def sources():
    out = subprocess.check_output(["find", "src", "-name", "*.c"], text=True)
    return sorted(out.split())


def file_coverage(src):
    """(pct, n_lines, ran) for `src`, or (0.0, 0, False) if it was never run."""
    objdir = os.path.join(BUILD, os.path.dirname(src))
    gcda = os.path.join(objdir, os.path.basename(src)[:-2] + ".gcda")
    if not os.path.exists(gcda):
        return 0.0, 0, False
    out = subprocess.run(["gcov", "-n", "-o", objdir, src],
                         capture_output=True, text=True).stdout
    m = re.search(r"Lines executed:([\d.]+)% of (\d+)", out)
    if not m:
        return 0.0, 0, False
    return float(m.group(1)), int(m.group(2)), True


def main():
    rows, tot_ex, tot = [], 0, 0
    for s in sources():
        pct, n, ran = file_coverage(s)
        if ran:
            ex = round(pct * n / 100)
            tot_ex += ex
            tot += n
        rows.append((pct, n, s, ran))

    if tot == 0:
        print("no coverage data found — build with -fprofile-arcs "
              "-ftest-coverage and run the tests first (see `make coverage`)")
        return 1

    overall = 100 * tot_ex / tot
    print("\n=== line coverage (gcov) ===")
    print(f"OVERALL: {overall:.1f}%  ({tot_ex}/{tot} lines across "
          f"{sum(1 for r in rows if r[3])} exercised files)\n")
    print(f"  {'cov':>6}  {'lines':>6}  file")
    for pct, n, s, ran in sorted(rows, key=lambda r: (r[0], -r[1])):
        note = "" if ran else "  (never run)"
        print(f"  {pct:5.1f}%  {n:6d}  {s}{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())