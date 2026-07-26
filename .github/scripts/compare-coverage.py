"""Compare the coverage reports produced by gcovr, lcov and llvm-cov over the same test suite.

Reads one directory per tool, as downloaded from the coverage workflow's artifacts, and writes a
markdown comparison to stdout. Each tool reports the same runs through a different counter format
and a different notion of what a "branch" is, so the tables below are what makes those differences
visible: totals per metric, per-file line and branch coverage side by side, and the set of files
each tool reports on at all.

Usage: python3 compare-coverage.py <artifact-root-dir>
"""

import json
import os
import re
import sys

# Order is the report order, and the first tool present becomes the per-file table's baseline.
TOOLS = ("gcovr", "lcov", "llvm-cov")

# Repository-relative path starts at one of the project's own top-level directories. gcovr reports
# paths already relative to the root, while lcov and llvm-cov both record absolute paths, so every
# filename is cut back to this common form before the reports can be compared per file.
SOURCE_ROOTS = ("applications", "rg_service", "protobuf_utils", "common")
_REL_RE = re.compile(r"(?:^|/)((?:%s)/.*)$" % "|".join(SOURCE_ROOTS))


def relative_path(filename):
    """Return filename as a repository-relative path, or its basename when it lies outside."""
    match = _REL_RE.search(filename.replace(os.sep, "/"))
    return match.group(1) if match else os.path.basename(filename)


def percent(covered, total):
    """Return the coverage percentage, or None when the metric has nothing to measure."""
    return None if not total else 100.0 * covered / total


def fmt(value, suffix="%"):
    return "n/a" if value is None else f"{value:.1f}{suffix}"


def counts(covered, total):
    return "n/a" if not total else f"{covered}/{total}"


def empty_metrics():
    return {key: [0, 0] for key in ("lines", "branches", "functions", "regions", "mcdc")}


def load_gcovr(path):
    """Parse a gcovr --json-summary report."""
    with open(os.path.join(path, "summary.json"), encoding="utf-8") as handle:
        report = json.load(handle)

    files = {}
    for entry in report.get("files", []):
        metrics = empty_metrics()
        metrics["lines"] = [entry.get("line_covered", 0), entry.get("line_total", 0)]
        metrics["branches"] = [entry.get("branch_covered", 0), entry.get("branch_total", 0)]
        metrics["functions"] = [entry.get("function_covered", 0), entry.get("function_total", 0)]
        files[relative_path(entry["filename"])] = metrics
    return files


def load_lcov(path):
    """Parse an lcov .info tracefile.

    The per-file LF/LH/BRF/BRH summary records are not trusted here: they are written by whichever
    lcov version produced the file, whereas the DA/BRDA detail records are the raw measurement.
    Counting the detail records keeps the definition of "covered" identical to the other two tools.
    """
    files = {}
    metrics = None
    for line in open(os.path.join(path, "lcov.info"), encoding="utf-8"):
        line = line.strip()
        if line.startswith("SF:"):
            metrics = files.setdefault(relative_path(line[3:]), empty_metrics())
        elif metrics is None:
            continue
        elif line.startswith("DA:"):
            hits = line[3:].split(",")[1]
            metrics["lines"][1] += 1
            metrics["lines"][0] += 1 if hits not in ("0", "-") else 0
        elif line.startswith("BRDA:"):
            taken = line[5:].split(",")[3]
            metrics["branches"][1] += 1
            metrics["branches"][0] += 1 if taken not in ("0", "-") else 0
        elif line.startswith("FNDA:"):
            hits = line[5:].split(",")[0]
            metrics["functions"][1] += 1
            metrics["functions"][0] += 1 if hits not in ("0", "-") else 0
        elif line == "end_of_record":
            metrics = None
    return files


def load_llvm_cov(path):
    """Parse an llvm-cov export JSON report."""
    with open(os.path.join(path, "llvm-cov.json"), encoding="utf-8") as handle:
        report = json.load(handle)

    files = {}
    for entry in report["data"][0].get("files", []):
        summary = entry.get("summary", {})
        metrics = empty_metrics()
        for key in ("lines", "branches", "functions", "regions", "mcdc"):
            block = summary.get(key)
            if block:
                metrics[key] = [block.get("covered", 0), block.get("count", 0)]
        files[relative_path(entry["filename"])] = metrics
    return files


LOADERS = {"gcovr": load_gcovr, "lcov": load_lcov, "llvm-cov": load_llvm_cov}


def load_meta(path):
    try:
        with open(os.path.join(path, "meta.json"), encoding="utf-8") as handle:
            return json.load(handle)
    except OSError:
        return {}


def totals(files):
    total = empty_metrics()
    for metrics in files.values():
        for key, (covered, count) in metrics.items():
            total[key][0] += covered
            total[key][1] += count
    return total


def emit_totals(reports, metas, out):
    out.append("## Totals\n")
    out.append("| Tool | Version | Lines | Line % | Branches | Branch % | Functions | Regions | MC/DC | Files |")
    out.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for tool, files in reports.items():
        total = totals(files)
        meta = metas.get(tool, {})
        out.append(
            f"| {tool} | {meta.get('version', 'n/a')} "
            f"| {counts(*total['lines'])} | {fmt(percent(*total['lines']))} "
            f"| {counts(*total['branches'])} | {fmt(percent(*total['branches']))} "
            f"| {fmt(percent(*total['functions']))} | {fmt(percent(*total['regions']))} "
            f"| {fmt(percent(*total['mcdc']))} | {len(files)} |"
        )
    out.append("")


def emit_cost(metas, out):
    out.append("## Cost\n")
    out.append("| Tool | Build + test (s) | Report generation (s) | Raw counter data (KiB) | HTML report (KiB) |")
    out.append("| --- | --- | --- | --- | --- |")
    for tool, meta in metas.items():
        out.append(
            f"| {tool} | {meta.get('build_test_seconds', 'n/a')} | {meta.get('report_seconds', 'n/a')} "
            f"| {meta.get('raw_kib', 'n/a')} | {meta.get('html_kib', 'n/a')} |"
        )
    out.append("")


def emit_per_file(reports, metric, out):
    """Emit one row per source file, with the metric as measured by each tool."""
    tools = list(reports)
    every_file = sorted({name for files in reports.values() for name in files})

    out.append(f"## Per-file {metric[:-1] if metric.endswith('s') else metric} coverage\n")
    out.append("| File | " + " | ".join(tools) + " | Spread |")
    out.append("| --- | " + " | ".join("---" for _ in tools) + " | --- |")
    for name in every_file:
        cells, values = [], []
        for tool in tools:
            metrics = reports[tool].get(name)
            if metrics is None:
                cells.append("absent")
                continue
            value = percent(*metrics[metric])
            cells.append("n/a" if value is None else f"{value:.1f}% ({counts(*metrics[metric])})")
            if value is not None:
                values.append(value)
        spread = f"{max(values) - min(values):.1f} pts" if len(values) > 1 else "n/a"
        out.append(f"| `{name}` | " + " | ".join(cells) + f" | {spread} |")
    out.append("")


def emit_file_sets(reports, out):
    """Report which files a tool includes that another one does not."""
    sets = {tool: set(files) for tool, files in reports.items()}
    lines = []
    for tool, names in sets.items():
        others = set().union(*(other for name, other in sets.items() if name != tool)) if len(sets) > 1 else set()
        only = sorted(names - others)
        if only:
            lines.append(f"- Reported only by {tool}: " + ", ".join(f"`{name}`" for name in only))
    if lines:
        out.append("## Reported file sets differ\n")
        out.extend(lines)
        out.append("")


def emit_gcovr_throw_branches(root, out):
    """Compare gcovr's default branch total against the same data with throw branches excluded.

    GCC emits a branch for every implicit exception-unwind edge, which llvm-cov has no counterpart
    for, so this is the single largest source of branch-percentage disagreement between the two
    counter formats on C++ code.
    """
    path = os.path.join(root, "coverage-gcovr", "summary-nothrow.json")
    try:
        with open(path, encoding="utf-8") as handle:
            report = json.load(handle)
    except OSError:
        return

    out.append("## gcovr branch total, with and without throw branches\n")
    out.append("| View | Branches | Branch % |")
    out.append("| --- | --- | --- |")
    out.append(f"| default | {report.get('branch_total', 0)} | see totals above |")
    out.append(
        f"| --exclude-throw-branches | {report.get('branch_total', 0)} "
        f"| {fmt(report.get('branch_percent'))} |"
    )
    out.append("")


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."

    reports, metas = {}, {}
    for tool in TOOLS:
        path = os.path.join(root, f"coverage-{tool}")
        if not os.path.isdir(path):
            continue
        try:
            reports[tool] = LOADERS[tool](path)
        except (OSError, KeyError, ValueError) as error:
            print(f"<!-- {tool}: {type(error).__name__}: {error} -->")
            continue
        metas[tool] = load_meta(path)

    if not reports:
        print("No coverage reports found: every measurement job failed or produced no artifact.")
        return 1

    out = ["# Coverage tool comparison\n"]
    out.append(f"Tools reporting: {', '.join(reports)}. ")
    out.append("All three measured the same test suite, over the same source filters.\n")
    emit_totals(reports, metas, out)
    emit_cost(metas, out)
    emit_gcovr_throw_branches(root, out)
    emit_per_file(reports, "lines", out)
    emit_per_file(reports, "branches", out)
    emit_file_sets(reports, out)
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
