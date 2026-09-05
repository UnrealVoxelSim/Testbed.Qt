# Headless simulation diagnostics - 2026-09-05

The headless executable was run against local checkouts of `Navigation.Voxel` and `Build.CMake` with the stress world. Each
population used 300 warm-up ticks followed by two measured samples of 300 ticks. The diagnostic columns report navigation
starts during the measured interval and the execution-state counts at its end.

| Configuration | Pawns | Median steps/s | Navigation starts | Planning | Following | Arrived | Unreachable |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| MSVC profiling | 1,000 | 359.05 | 927 | 10 | 986 | 0 | 4 |
| MSVC profiling | 5,000 | 153.22 | 3,124 | 243 | 4,191 | 3 | 563 |
| MSVC profiling | 10,000 | 96.88 | 3,454 | 2,446 | 6,937 | 9 | 608 |
| MSVC release | 10,000 | 87.65 | 1,194 | 3,938 | 5,702 | 5 | 355 |

The profiling sweep used three measured samples; the release comparison used two. The 10,000-pawn profiling run completes
only about 3.8 navigation starts per tick while 2,446 pawns remain in planning at the sample boundary. This confirms that
the workload is planner-backlog limited rather than simply spending all time on completed path searches. The release and
profiling runs vary substantially under VM scheduling, so repeated samples and state counters should be used together when
evaluating planner changes.

The planner now caches a blocked direct-path result for each request until the environment revision changes. Previously,
pending requests repeatedly walked the same blocked straight-line route on every tick. In a 20,000-pawn backlog run,
direct-path checks fell from about 1,121 calls per step to 20 calls per step after topology settled; measured throughput
rose from 39.8 to 70.1 steps/s in the corresponding VM runs. Requests whose tiles are not ready still retry, and topology
invalidation clears the cached result, so this does not alter reachability outcomes.

Path validation dependencies were changed from per-path ordered sets to sorted unique vectors. Invalidation only iterates
these dependencies, so the representation remains exact while avoiding one tree allocation per dependency during path
publication. The current VM runs do not establish a reliable throughput improvement from this change; it primarily reduces
allocation and memory overhead.

The request-index experiment was abandoned. A longer five-sample run with 500 warm-up ticks measured 143.16 steps/s
median at 10,000 pawns, with only 7 pawns still planning. This matches the earlier 142.77 steps/s baseline and shows that
the 97.12 steps/s result was VM and backlog variance, not an optimization.

An isolated budget experiment increased fine expansions per tick from 64 to 512 and component expansions from 4 to 32.
The 10,000-pawn profiling run reached 92.70 steps/s and still ended with 3,938 pawns planning, compared with 96.88
steps/s and 2,446 planning in the baseline run. Increasing the budgets therefore consumed more CPU without improving
queue drain in this workload; future work should reduce per-request overhead or improve search efficiency before raising
budgets.
