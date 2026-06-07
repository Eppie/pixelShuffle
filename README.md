# pixelShuffle
http://gfycat.com/DetailedElasticAlaskanhusky

## CPU counter instrumentation

Build an instrumented PMU binary against the local `cpu_counter` checkout:

```sh
cmake -S . -B build-pmu -DCMAKE_BUILD_TYPE=Release -DENABLE_CPU_COUNTERS=ON -DCPU_COUNTER_DIR=/Users/eppie/codex_projects/cpu_counter
cmake --build build-pmu -j 8
```

Use `-DPNGLAB_PMU_PROFILE=cache|branch|frontend|execution` to switch counter sets. The default chunk scope size is 100,000 candidates; override it with `-DPNGLAB_PMU_CHUNK_CANDIDATES=<N>`.

Run with sudo so Apple `kperf` can program hardware counters:

```sh
sudo env PERF_OUTPUT=/tmp/pixelshuffle-pmu.jsonl /usr/bin/time -lp ./build-pmu/pngLAB_bench_core images/mona.png images/gothic.png /tmp/pixelshuffle-pmu.png
/Users/eppie/codex_projects/cpu_counter/cpu_counter summary /tmp/pixelshuffle-pmu.jsonl
```
