# Headless autonomous-navigation throughput — 2026-08-26

## Scenario

The optimized `windows-msvc-profiling` build ran `UnrealVoxelSim.Testbed.Qt.Headless` with the same `1024 x 1024 x 256`
stress-world bounds and autonomous pawn behavior as the Qt stress world. Every pawn repeatedly selects a reproducibly
random destination, including invalid and unreachable voxels. A fresh world was constructed for every population.

World construction and 1,000 warm-up simulation steps were excluded. Five consecutive samples of 500 fixed simulation
steps were timed with `std::chrono::steady_clock`. The executable contained all backend-neutral instrumentation and the
Tracy 0.13.1 on-demand client; no Tracy viewer was connected during this maximum-throughput run. There was no Qt event
loop, rendering, sleeping, simulation pacing, or world editing during measurement.

```text
UnrealVoxelSim.Testbed.Qt.Headless.exe --entities 0,100,1000,5000,10000,20000,30000 --warmup-steps 1000 --measurement-steps 500 --samples 5
```

## Environment

- CPU: AMD Ryzen 5 7600X, 6 cores / 12 logical processors
- Memory: 47.1 GiB visible to Windows
- OS: Windows 10 Enterprise 10.0.19045
- Compiler: MSVC 19.44.35223 x64
- Configuration: Release, static MSVC runtime, `UNREALVOXELSIM_ENABLE_PROFILING=ON`
- Tracy state during timing: on-demand client enabled, viewer disconnected

## Results

| Pawns | Minimum steps/s | Median steps/s | Maximum steps/s | Milliseconds at maximum |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 17,367.80 | 6,385,696.04 | 6,402,048.66 | 0.0002 |
| 100 | 1,960.90 | 3,688.91 | 4,630.99 | 0.216 |
| 1,000 | 1,068.27 | 1,165.12 | 1,424.48 | 0.702 |
| 5,000 | 214.63 | 237.11 | 258.26 | 3.872 |
| 10,000 | 132.87 | 142.77 | 151.07 | 6.619 |
| 20,000 | 106.23 | 114.52 | 123.27 | 8.112 |
| 30,000 | 79.59 | 92.32 | 96.33 | 10.381 |

The zero-pawn minimum includes completion of background navigation preparation that remained after warm-up; once that
bounded work completed, empty ticks became nearly no-ops. It should not be used as a population-scaling baseline.

At 10,000 pawns, every measured sample exceeded 100 steps/s. The maximum remained above 100 at 20,000 pawns, while
30,000 pawns reached 96.33 steps/s at best. From 10,000 through 30,000 pawns, step time rose from 6.619 ms to 10.381 ms;
the observed degradation was sub-linear over this interval rather than a sudden scaling collapse.

That flattening is not sufficient evidence of healthy navigation throughput. Fine-path and component-expansion work is
globally budgeted per tick, so increasing population can increase request latency or queue residence without adding the
same amount of CPU work. A subsequent benchmark must correlate steps/s with navigation completions, terminal outcomes,
queue depths, and request-latency percentiles before raising the supported-pawn conclusion above 10,000.

These numbers measure simulation throughput, not presentation FPS. They exclude rendering and world edits, and an
attached Tracy viewer would add capture overhead. They establish a reproducible simulation-only baseline for the next
profile-guided comparison; they do not demonstrate the full 100 FPS gameplay requirement by themselves.
