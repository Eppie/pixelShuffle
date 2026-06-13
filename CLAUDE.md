# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

pixelShuffle rearranges the **exact** pixels of a *palette* image to approximate a *target* image. It is a constrained optimization: the output is a permutation of the palette's pixel multiset (no color is invented), chosen to minimize per-pixel color distance to the target. Palette and target must contain the same number of pixels. The programs emit a final PNG plus, by default, one PNG frame per iteration; `run.sh`/`generateAll.sh` stitch those frames into a webm.

CLI for every `pngLAB*` binary: `./pngLAB <palette.png> <target.png> <output.png>`.

## Build

CMake is the source of truth. The root `Makefile` is CMake-generated — never edit it. The repo is configured **in-source** (`CMakeCache.txt` lives at the repo root, binaries land at the repo root), and `run_variants.py` additionally uses a separate `build/` tree.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8                      # all targets
cmake --build build --target pngLAB          # one target
```

`Debug` build type drops to `-O0`; anything else uses `-O3`. `COMMON_FLAGS` includes `-march=native -mtune=native`, so binaries are not portable across machines.

## Run

```sh
./pngLAB images/mona.png images/gothic.png out.png         # single binary
./run.sh LAB mona gothic                                   # build webm animation (needs png2yuv/vpxenc/optipng)
python3 run_variants.py mona gothic --build                # build + run every variant, into variant_runs/<paletteTOtarget>/
hungarian images/mona.png images/gothic.png out.png [--no-dither --patch-size N --no-intermediate]
```

`run_variants.py` accepts a path, an `images/` filename, or a bare stem (`mona` → `images/mona.png`).

## Core algorithm (`pngLAB.cpp`)

The canonical pipeline, shared by all `pngLAB*` variants:

1. `readPNGFile` (in `pngReadWrite.h`) loads palette + target as RGBA rows.
2. `seedFromExactPalettePixels` tiles the palette's pixels across the target's dimensions — this is the initial permutation.
3. `imageToLab` converts to LAB via RGB→XYZ→LAB; all distance math runs in LAB. `Color` is a 16-byte/4-float vector-aligned struct (4th lane unused, kept for SIMD).
4. `processPNGFile` runs two optimization phases over candidate pixel *pairs*:
   - **Ordered loop** (`kOrderedLoopCount`): deterministic pair traversal driven by precomputed `AdvanceTables` (per-step next/carry index tables in `KernelTables`).
   - **Random loop** (`kRandomLoopCount`): random pairs from `xorshift64star` (fixed seed `kInitialRandomSeed` → reproducible runs).
   - Each candidate calls `shouldSwapImpl`: swap iff `swapCost < keepCost`, where cost is the sum of squared LAB diffs (`pixelDiffValue`). Greedy — a swap only commits if it lowers total distance.
5. `labToImage` converts back and `writePNGFile` emits the result (and per-iteration frames under `ANIMATION`).

Image dimensions are **global ints** (`sWidth/sHeight/dWidth/dHeight`, defined in `pngReadWrite.h`), not passed around — many functions read them directly.

### Compile-time feature flags (set via `target_compile_definitions` in CMakeLists.txt)

These `#define`s reshape the same source into different binaries:

- `OUTPUT` — print per-iteration diff/swap stats.
- `ANIMATION` — write a PNG per iteration; also disables PNG compression for speed.
- `PROFILE_STATS` — enable `ProfileStats` sampled software timing (`profile_stats.h`, ns + throughput per region, no privileges needed); used by `pngLAB_bench_core` (kernel only) and `pngLAB_bench_e2e` (+ I/O + animation).
- `PNG_LAB_ENABLE_NEON` — ARM64 NEON swap kernel. Selected at runtime only when the working set fits `kNeonWorkingSetLimitBytes` (8 MiB); CMake option `ENABLE_NEON_SWAP_KERNEL` (default ON on arm64).
- `SHOULD_SWAP_AB_HARNESS` — builds `shouldSwap_ab_harness`, which runs the full candidate stream comparing scalar vs NEON swap decisions and reports the first numeric/decision drift. Nonzero exit on decision drift.
- `PNGLAB_RANDOM_MODE_{GLOBAL,LOCAL}` — global random pairs vs locality-aware per-tile chunks (`-DPNGLAB_RANDOM_MODE=local`, tuned by `PNGLAB_LOCAL_RANDOM_TILE_SIZE` / `PNGLAB_LOCAL_RANDOM_GLOBAL_CHUNK_INTERVAL`). **Changes output quality** — diff the result against `global` before adopting.

## Variants

The `pngLAB_*` files are **not** forks of `pngLAB.cpp`. They fall into two families that each reimplement the pixel-rearrangement objective with a different strategy and data model:

- **`pngLAB.cpp`** (standalone): greedy pairwise-swap over `struct Color`, full RGB↔Lab round-trip. Shares nothing extractable with the others.
- **Gen-1 `{pngLAB_clean, pngLAB_spiral, pngLAB_pretty}`**: camelCase rewrites of the round-trip swap approach. `clean` and `pretty` share their identical Lab↔RGB color core — `struct LabColor`, the `xyzToRgb`/`rgbToXyz`/`xyzToLab`/`labToXyz` converters, `rgbaToLab`, `advanceOrderedState` — via **`lab_roundtrip.h`**; `spiral`'s converters differ textually, so it keeps its own.
- **Gen-2 `{pngLAB_blur_anneal, pngLAB_coarse_patches, pngLAB_particle_flow, pngLAB_hilbert_sort, pngLAB_wavefront_flow, pngLAB_random_walk_relax, pngLAB_transport_stream}`**: an assignment/transport family over `struct LabColor`. Their byte-identical shared scaffolding — `struct Geometry`/`LabColor`/`Pixel`, `srgbToLinear`/`rgbaToLab`, `imageToLab`, `squaredLabDistance`, `quantize`/`colorKey`/`labLess` — lives in **`lab_grid.h`**; each `.cpp` keeps only its unique optimization strategy. Pieces that genuinely differ between these variants (the `OwnedRows` RAII wrapper, `XorShift64Star` + its per-variant seed, `totalCost`) deliberately stay per-`.cpp`.

All variants share PNG I/O via `pngReadWrite.h` (whose globals/functions are `inline` for ODR-safety across TUs). `hungarian.cpp` is a separate approach: OpenMP-parallel patch-based assignment (Hungarian algorithm), built only when OpenMP is found.

## PMU profiling (Apple Silicon)

`ENABLE_CPU_COUNTERS` instruments named scopes (`PNGLAB_PMU_SCOPE`) against an external `cpu_counter` checkout (`perf.h`); requires `-DCPU_COUNTER_DIR=...`. Select counter sets with `-DPNGLAB_PMU_PROFILE=cache|branch|frontend|execution`. Must run under `sudo` so Apple `kperf` can program counters. See README.md for the full invocation. The `profile` CMake target runs `pngLAB` under valgrind callgrind.
