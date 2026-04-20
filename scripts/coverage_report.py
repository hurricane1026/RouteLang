#!/usr/bin/env python3
"""Compute runtime coverage as the union across test binaries.

The default `llvm-cov report` aggregates over all `--object` binaries by
picking the first-listed binary's instantiation for each source line.
That's wrong for inline/template functions in headers: if binary A
instantiates `handle_jit_outcome<Loop>` for a loop type that never
exercises a branch, binary B's real coverage of the same branch is
invisible in the combined report.

This script instead exports per-binary JSON, walks the line segments,
and reports a line as covered if ANY binary hits it. That matches how
people read "overall coverage" and matches how coverage of library
code should be measured in multi-binary test suites.

Usage:
    coverage_report.py --profile PATH --threshold PCT BINARY [BINARY ...]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys


RUNTIME_SOURCES = ["include/rut/common/", "include/rut/runtime/", "src/runtime/"]
# Substring match used to re-filter the export output to the same scope
# as RUNTIME_SOURCES (llvm-cov export can leak transitively-referenced
# files into the output).
RUNTIME_SOURCE_SUFFIXES = RUNTIME_SOURCES

# Files excluded from the coverage bar:
#   - io_uring_backend / epoll_backend: require kernel features / perms
#     not available in CI
#   - socket.cc / access_log.cc / shard.h: thin wrappers or shard
#     bookkeeping that belong in a later test pass
#   - epoll_event_loop.h / iouring_event_loop.h: constructor-only headers
EXCLUDE_PATH_RE = re.compile(
    r"io_uring_backend|epoll_backend|socket\.cc|access_log\.cc|"
    r"shard\.h|epoll_event_loop\.h|iouring_event_loop\.h"
)


def per_binary_segments(profile: str, binary: str) -> dict:
    """Return llvm-cov JSON export for one binary restricted to runtime."""
    proc = subprocess.run(
        [
            "llvm-cov",
            "export",
            f"--instr-profile={profile}",
            f"--object={binary}",
            "--sources",
            *RUNTIME_SOURCES,
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(proc.stdout)


def merge_line_hits(binaries: list[str], profile: str) -> dict[str, dict[int, int]]:
    """For each runtime source file, max-merge per-line counts across binaries."""
    merged: dict[str, dict[int, int]] = {}
    for b in binaries:
        data = per_binary_segments(profile, b)
        for f in data["data"][0]["files"]:
            path = f["filename"]
            if "/tests/" in path or path.endswith("test.h"):
                continue
            # Limit to the runtime source prefixes. llvm-cov export
            # occasionally leaks transitively-referenced files here
            # (core/expected.h, placement_new.cc) that the CI report
            # mode would have filtered out via --sources. Keeping the
            # bar tight to the runtime matches the intent.
            if not any(s in path for s in RUNTIME_SOURCE_SUFFIXES):
                continue
            if EXCLUDE_PATH_RE.search(path):
                continue
            lines = merged.setdefault(path, {})
            for seg in f.get("segments", []):
                # seg: [line, col, count, hasCount, isRegionEntry, isGapRegion]
                if len(seg) < 6 or not seg[3] or seg[5]:
                    continue
                line = seg[0]
                cnt = seg[2]
                # Keep the highest hit count across binaries; a line is
                # "covered" iff at least one binary reached it.
                if line not in lines or cnt > lines[line]:
                    lines[line] = cnt
    return merged


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True, help="merged .profdata path")
    ap.add_argument("--threshold", type=float, required=True, help="fail below this line pct")
    ap.add_argument("binaries", nargs="+")
    args = ap.parse_args()

    merged = merge_line_hits(args.binaries, args.profile)

    per_file_stats = []
    total = 0
    covered = 0
    for path, lines in merged.items():
        c = sum(1 for v in lines.values() if v > 0)
        t = len(lines)
        total += t
        covered += c
        per_file_stats.append((t - c, t, c, path))

    # Pretty-print a sorted list: worst files first.
    per_file_stats.sort(key=lambda x: (-x[0], x[3]))
    print(f"{'Missed':>7} {'Total':>7} {'Cover':>7}  File")
    for missed, t, c, path in per_file_stats:
        pct = 100.0 * c / t if t else 100.0
        print(f"{missed:>7} {t:>7}  {pct:>5.1f}%  {path}")
    pct = 100.0 * covered / total if total else 100.0
    print(f"\nTOTAL: {covered}/{total} lines covered = {pct:.2f}%")

    if pct < args.threshold:
        print(f"ERROR: line coverage {pct:.2f}% is below {args.threshold}% threshold")
        return 1
    print(f"Coverage OK: {pct:.2f}% >= {args.threshold}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
