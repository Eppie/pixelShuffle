#!/usr/bin/env python3

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent
DEFAULT_BUILD_DIR = ROOT / "build"
DEFAULT_OUTPUT_DIR = ROOT / "variant_runs"
DEFAULT_VARIANTS = (
    "pngLAB",
    "pngLAB_clean",
    "pngLAB_spiral",
    "pngLAB_pretty",
    "pngLAB_blur_anneal",
    "pngLAB_coarse_patches",
    "pngLAB_particle_flow",
    "pngLAB_hilbert_sort",
    "pngLAB_wavefront_flow",
    "pngLAB_random_walk_relax",
    "pngLAB_transport_stream",
    "hungarian",
)


def resolve_image(value: str) -> Path:
    candidate = Path(value).expanduser()
    if candidate.exists():
        return candidate.resolve()

    images_dir = ROOT / "images"
    named_candidate = images_dir / value
    if named_candidate.exists():
        return named_candidate.resolve()

    png_candidate = images_dir / f"{value}.png"
    if png_candidate.exists():
        return png_candidate.resolve()

    raise FileNotFoundError(
        f"could not find image '{value}'. Pass a path, an images/ filename, or an images/ stem"
    )


def is_path_like(value: str) -> bool:
    return os.sep in value or (os.altsep is not None and os.altsep in value) or value.startswith(".")


def variant_label(value: str) -> str:
    return Path(value).name


def find_executable(variant: str, build_dir: Path) -> Path:
    raw = Path(variant).expanduser()
    candidates = []
    if raw.is_absolute() or is_path_like(variant):
        candidates.append(raw)
    else:
        candidates.extend((build_dir / variant, ROOT / variant))

    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()

    searched = ", ".join(str(candidate) for candidate in candidates)
    raise FileNotFoundError(f"could not find executable for {variant!r}; searched {searched}")


def configure_build_dir(build_dir: Path) -> None:
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build_dir)], check=True)


def build_variants(variants: list[str], build_dir: Path, config: str | None, skip_missing: bool) -> None:
    targets = [variant for variant in variants if not is_path_like(variant)]
    if not targets:
        return

    configure_build_dir(build_dir)
    for target in targets:
        command = ["cmake", "--build", str(build_dir)]
        if config:
            command.extend(["--config", config])
        command.extend(["--target", target])
        completed = subprocess.run(command, check=False)
        if completed.returncode != 0:
            if skip_missing:
                print(f"skipping build for {target}: cmake target is unavailable", file=sys.stderr)
                continue
            raise subprocess.CalledProcessError(completed.returncode, command)


def command_for_variant(
    executable: Path,
    label: str,
    palette: Path,
    source: Path,
    output_name: str,
    hungarian_extra_args: list[str],
) -> list[str]:
    command = [str(executable)]
    if label == "hungarian":
        command.extend(hungarian_extra_args)
    command.extend((str(palette), str(source), f"./{output_name}"))
    return command


def run_variant(
    executable: Path,
    label: str,
    palette: Path,
    source: Path,
    output_root: Path,
    clean: bool,
    dry_run: bool,
    hungarian_extra_args: list[str],
) -> dict[str, object]:
    run_dir = output_root / label
    if clean and run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    output_name = f"{label}.png"
    command = command_for_variant(
        executable=executable,
        label=label,
        palette=palette,
        source=source,
        output_name=output_name,
        hungarian_extra_args=hungarian_extra_args,
    )

    result: dict[str, object] = {
        "variant": label,
        "executable": str(executable),
        "command": command,
        "working_directory": str(run_dir),
        "output_file": str(run_dir / output_name),
    }

    printable_command = " ".join(shlex.quote(part) for part in command)
    print(f"{label}: {printable_command}")
    if dry_run:
        result.update({"returncode": None, "elapsed_s": None, "status": "dry-run"})
        return result

    started_at = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=run_dir,
        capture_output=True,
        text=True,
        check=False,
    )
    elapsed_s = time.perf_counter() - started_at

    stdout_log = run_dir / "stdout.log"
    stderr_log = run_dir / "stderr.log"
    stdout_log.write_text(completed.stdout)
    stderr_log.write_text(completed.stderr)

    output_file = run_dir / output_name
    result.update(
        {
            "returncode": completed.returncode,
            "elapsed_s": elapsed_s,
            "stdout_log": str(stdout_log),
            "stderr_log": str(stderr_log),
            "output_exists": output_file.exists(),
            "status": "ok" if completed.returncode == 0 and output_file.exists() else "failed",
        }
    )

    if completed.returncode != 0:
        print(f"{label}: failed with exit code {completed.returncode}; see {stderr_log}", file=sys.stderr)
    elif not output_file.exists():
        print(f"{label}: completed but did not write {output_file}", file=sys.stderr)
    else:
        print(f"{label}: wrote {output_file} in {elapsed_s:.2f}s")

    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the image-producing pixelShuffle variants against one palette/target image pair."
    )
    parser.add_argument("palette", help="Palette image path, images/ filename, or images/ stem")
    parser.add_argument("source", help="Source/target image path, images/ filename, or images/ stem")
    parser.add_argument(
        "--variants",
        nargs="+",
        default=list(DEFAULT_VARIANTS),
        help=f"Variant executable names or paths to run. Default: {' '.join(DEFAULT_VARIANTS)}",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=DEFAULT_BUILD_DIR,
        help="CMake build directory containing variant binaries",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="Configure the build directory if needed, then build selected named variants before running",
    )
    parser.add_argument(
        "--config",
        default=None,
        help="Optional CMake configuration to pass to cmake --build for multi-config generators",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help="Root directory for variant run outputs",
    )
    parser.add_argument(
        "--name",
        default=None,
        help="Output run name. Defaults to <palette-stem>TO<source-stem>",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Delete each selected variant's output directory before running it",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands without running them",
    )
    parser.add_argument(
        "--skip-missing",
        action="store_true",
        help="Skip variants whose executables are not present instead of failing",
    )
    parser.add_argument(
        "--hungarian-no-dither",
        action="store_true",
        help="Run hungarian with --no-dither",
    )
    parser.add_argument(
        "--hungarian-no-intermediate",
        action="store_true",
        help="Run hungarian with --no-intermediate",
    )
    parser.add_argument(
        "--hungarian-extra-args",
        default="",
        help='Extra options inserted before hungarian image args, for example --hungarian-extra-args="--patch-size 64"',
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    palette = resolve_image(args.palette)
    source = resolve_image(args.source)
    build_dir = args.build_dir.expanduser().resolve()

    variants = [variant_label(variant) if is_path_like(variant) else variant for variant in args.variants]
    if args.build:
        build_variants(args.variants, build_dir=build_dir, config=args.config, skip_missing=args.skip_missing)

    executables: list[tuple[str, Path]] = []
    missing_errors: list[str] = []
    for requested_variant in args.variants:
        label = variant_label(requested_variant)
        try:
            executables.append((label, find_executable(requested_variant, build_dir=build_dir)))
        except FileNotFoundError as exc:
            if args.skip_missing:
                print(f"skipping {label}: {exc}", file=sys.stderr)
            else:
                missing_errors.append(str(exc))

    if missing_errors:
        print("error: missing variant executables:", file=sys.stderr)
        for error in missing_errors:
            print(f"  {error}", file=sys.stderr)
        print("hint: pass --build to compile CMake targets first, or --skip-missing to run the available subset", file=sys.stderr)
        return 1

    if not executables:
        print("error: no variants to run", file=sys.stderr)
        return 1

    run_name = args.name or f"{palette.stem}TO{source.stem}"
    output_root = (args.output_dir.expanduser().resolve() / run_name)
    output_root.mkdir(parents=True, exist_ok=True)

    hungarian_extra_args = []
    if args.hungarian_no_dither:
        hungarian_extra_args.append("--no-dither")
    if args.hungarian_no_intermediate:
        hungarian_extra_args.append("--no-intermediate")
    hungarian_extra_args.extend(shlex.split(args.hungarian_extra_args))
    results = []
    for label, executable in executables:
        results.append(
            run_variant(
                executable=executable,
                label=label,
                palette=palette,
                source=source,
                output_root=output_root,
                clean=args.clean,
                dry_run=args.dry_run,
                hungarian_extra_args=hungarian_extra_args,
            )
        )

    summary = {
        "palette": str(palette),
        "source": str(source),
        "output_root": str(output_root),
        "variants": variants,
        "results": results,
    }
    summary_path = output_root / "summary.json"
    if not args.dry_run:
        summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
        print(f"wrote {summary_path}")

    failures = [result for result in results if result.get("status") == "failed"]
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
