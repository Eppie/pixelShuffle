#!/usr/bin/env python3

import argparse
import hashlib
import json
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent
DEFAULT_BUILD_DIR = ROOT / "build"
DEFAULT_OUTPUT_DIR = DEFAULT_BUILD_DIR / "benchmarks"

PROFILE_PREFIX = "PROFILE_STATS_JSON "
REAL_RE = re.compile(r"^real\s+([0-9]+(?:\.[0-9]+)?)$")
INSTRUCTIONS_RE = re.compile(r"^\s*([0-9]+)\s+instructions retired$")
CYCLES_RE = re.compile(r"^\s*([0-9]+)\s+cycles elapsed$")


@dataclass(frozen=True)
class CaseConfig:
    name: str
    target: str
    palette: Path
    source: Path
    default_runs: int


CASE_CONFIGS = {
    "small-core": CaseConfig(
        name="small-core",
        target="pngLAB_bench_core",
        palette=ROOT / "images" / "mona.png",
        source=ROOT / "images" / "gothic.png",
        default_runs=5,
    ),
    "small-e2e": CaseConfig(
        name="small-e2e",
        target="pngLAB_bench_e2e",
        palette=ROOT / "images" / "mona.png",
        source=ROOT / "images" / "gothic.png",
        default_runs=5,
    ),
    "big-core": CaseConfig(
        name="big-core",
        target="pngLAB_bench_core",
        palette=ROOT / "images" / "monaBig.png",
        source=ROOT / "images" / "gothicBig.png",
        default_runs=3,
    ),
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_profile_stats(stderr_text: str) -> dict[str, Any]:
    for line in reversed(stderr_text.splitlines()):
        if line.startswith(PROFILE_PREFIX):
            return json.loads(line[len(PROFILE_PREFIX):])
    raise RuntimeError("PROFILE_STATS_JSON line not found in stderr output")


def parse_time_metrics(stderr_text: str) -> dict[str, Any]:
    metrics: dict[str, Any] = {}
    for line in stderr_text.splitlines():
        real_match = REAL_RE.match(line)
        if real_match:
            metrics["real_s"] = float(real_match.group(1))
            continue

        instructions_match = INSTRUCTIONS_RE.match(line)
        if instructions_match:
            metrics["instructions_retired"] = int(instructions_match.group(1))
            continue

        cycles_match = CYCLES_RE.match(line)
        if cycles_match:
            metrics["cycles_elapsed"] = int(cycles_match.group(1))

    if "real_s" not in metrics:
        raise RuntimeError("Failed to parse wall time from /usr/bin/time output")
    return metrics


def event_metric(events: dict[str, Any], name: str, field: str) -> int | None:
    event = events.get(name)
    if not event:
        return None
    value = event.get(field)
    if value is None:
        return None
    return int(value)


def safe_divide(numerator: float | int | None, denominator: float | int | None) -> float | None:
    if numerator is None or denominator in (None, 0):
        return None
    return float(numerator) / float(denominator)


def compute_run_metrics(profile: dict[str, Any], timing: dict[str, Any]) -> dict[str, Any]:
    process_ns = event_metric(profile, "process_png_file", "ns")
    process_ops = event_metric(profile, "process_png_file", "ops")
    write_png_ns = event_metric(profile, "write_png", "ns")
    write_png_ops = event_metric(profile, "write_png", "ops")
    lab_to_image_ns = event_metric(profile, "lab_to_image", "ns")
    lab_to_image_ops = event_metric(profile, "lab_to_image", "ops")
    instructions = timing.get("instructions_retired")
    cycles = timing.get("cycles_elapsed")

    metrics = {
        "wall_time_s": timing["real_s"],
        "process_ns": process_ns,
        "process_ops": process_ops,
        "ns_per_candidate": safe_divide(process_ns, process_ops),
        "candidate_ops_per_s": safe_divide((process_ops or 0) * 1_000_000_000, process_ns),
        "instructions_retired": instructions,
        "cycles_elapsed": cycles,
        "instructions_per_candidate": safe_divide(instructions, process_ops),
        "cycles_per_candidate": safe_divide(cycles, process_ops),
        "ipc": safe_divide(instructions, cycles),
        "write_png_ns_per_pixel": safe_divide(write_png_ns, write_png_ops),
        "lab_to_image_ns_per_pixel": safe_divide(lab_to_image_ns, lab_to_image_ops),
    }
    return metrics


def median_or_none(values: list[float | None]) -> float | None:
    present = [value for value in values if value is not None]
    if not present:
        return None
    return statistics.median(present)


def summarise_runs(case_name: str, warmup_runs: int, results: list[dict[str, Any]]) -> dict[str, Any]:
    measured = [result for result in results if not result["is_warmup"]]
    summary = {
        "case": case_name,
        "warmup_runs": warmup_runs,
        "total_runs": len(results),
        "measured_runs": len(measured),
        "measured_run_ids": [result["run_id"] for result in measured],
        "output_sha256_values": sorted({result["output_sha256"] for result in measured}),
        "stdout_sha256_values": sorted({result["stdout_sha256"] for result in measured}),
        "median": {},
    }

    median_fields = [
        "wall_time_s",
        "ns_per_candidate",
        "candidate_ops_per_s",
        "instructions_retired",
        "cycles_elapsed",
        "instructions_per_candidate",
        "cycles_per_candidate",
        "ipc",
        "write_png_ns_per_pixel",
        "lab_to_image_ns_per_pixel",
    ]

    for field in median_fields:
        summary["median"][field] = median_or_none([result[field] for result in measured])

    return summary


def run_case(case: CaseConfig, total_runs: int, warmup_runs: int, output_root: Path, build_dir: Path) -> dict[str, Any]:
    case_root = output_root / case.name
    case_root.mkdir(parents=True, exist_ok=True)

    binary = build_dir / case.target
    if not binary.exists():
        raise FileNotFoundError(f"Benchmark target not found: {binary}")

    all_results: list[dict[str, Any]] = []

    for run_index in range(total_runs):
        run_id = f"run-{run_index:02d}"
        run_root = case_root / run_id
        run_root.mkdir(parents=True, exist_ok=True)

        stdout_path = run_root / "stdout.log"
        stderr_path = run_root / "stderr.log"
        output_path = run_root / "out.png"
        output_arg = output_path.name
        result_path = run_root / "result.json"

        command = [
            "/usr/bin/time",
            "-lp",
            str(binary),
            str(case.palette),
            str(case.source),
            output_arg,
        ]

        completed = subprocess.run(
            command,
            cwd=run_root,
            capture_output=True,
            text=True,
            check=False,
        )

        stdout_path.write_text(completed.stdout)
        stderr_path.write_text(completed.stderr)

        if completed.returncode != 0:
            raise RuntimeError(
                f"{case.name} {run_id} failed with exit code {completed.returncode}. "
                f"See {stderr_path}"
            )

        profile = parse_profile_stats(completed.stderr)
        timing = parse_time_metrics(completed.stderr)
        metrics = compute_run_metrics(profile, timing)

        result = {
            "case": case.name,
            "target": case.target,
            "palette": str(case.palette),
            "source": str(case.source),
            "run_id": run_id,
            "run_index": run_index,
            "is_warmup": run_index < warmup_runs,
            "stdout_log": str(stdout_path),
            "stderr_log": str(stderr_path),
            "output_file": str(output_path),
            "output_sha256": sha256_file(output_path),
            "stdout_sha256": sha256_file(stdout_path),
            "profile_stats": profile,
            **metrics,
        }

        result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        all_results.append(result)

    summary = summarise_runs(case.name, warmup_runs, all_results)
    summary["results"] = [result["run_id"] for result in all_results]
    (case_root / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run reproducible pngLAB benchmark cases")
    parser.add_argument(
        "--cases",
        nargs="+",
        choices=sorted(CASE_CONFIGS.keys()),
        default=["small-core", "small-e2e"],
        help="Benchmark cases to run",
    )
    parser.add_argument(
        "--warmup-runs",
        type=int,
        default=1,
        help="How many initial runs per case to mark as warm-up and exclude from medians",
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=None,
        help="Override the total run count for every selected case",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help="Directory where per-run artifacts and JSON results will be written",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=DEFAULT_BUILD_DIR,
        help="Directory containing the benchmark binaries to run",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.warmup_runs < 0:
        raise ValueError("--warmup-runs must be non-negative")

    build_dir = args.build_dir.resolve()
    output_root = args.output_dir.resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    overall_summary = {"build_dir": str(build_dir), "cases": [], "output_root": str(output_root)}

    for case_name in args.cases:
        case = CASE_CONFIGS[case_name]
        total_runs = args.runs if args.runs is not None else case.default_runs
        if total_runs <= args.warmup_runs:
            raise ValueError(
                f"Case {case_name} needs total runs greater than warmup runs "
                f"({total_runs} <= {args.warmup_runs})"
            )

        summary = run_case(case, total_runs=total_runs, warmup_runs=args.warmup_runs, output_root=output_root, build_dir=build_dir)
        overall_summary["cases"].append(summary)

        median = summary["median"]
        print(
            f"{case_name}: "
            f"wall={median['wall_time_s']:.3f}s, "
            f"ns/candidate={median['ns_per_candidate']:.4f}, "
            f"candidate/s={median['candidate_ops_per_s']:.2f}, "
            f"cycles/candidate={median['cycles_per_candidate']:.4f}, "
            f"instructions/candidate={median['instructions_per_candidate']:.4f}, "
            f"ipc={median['ipc']:.4f}"
        )

    summary_path = output_root / "summary.json"
    summary_path.write_text(json.dumps(overall_summary, indent=2, sort_keys=True) + "\n")
    print(f"wrote {summary_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise
