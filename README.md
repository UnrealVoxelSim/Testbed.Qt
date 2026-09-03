# UnrealVoxelSim.Testbed.Qt

Interactive, non-production host for evaluating UnrealVoxelSim domains without Unreal Engine.

The testbed composes the chunked voxel field, solid domain, renderer-neutral greedy mesher, event dispatcher,
deterministic simulation, grounded movement, voxel navigation, path following, and a Qt 6 OpenGL widget. It keeps a
bounded set of camera-relative logical mesh tiles resident, captures authoritative solid snapshots on the UI/simulation
thread, meshes snapshots asynchronously, uploads only current results, and frustum-culls tile bounds before drawing.

Select the initial world from the command line, or switch worlds later from **File > World**:

```text
UnrealVoxelSim.Testbed.Qt.App.exe --world standard
UnrealVoxelSim.Testbed.Qt.App.exe --world stress
```

`standard` is the default interactive single-pawn world. `stress` uses `1024 x 1024 x 256` world bounds, a one-million
voxel ground plane, and 1,000 initial pawns. Its toolbar population control creates or deletes pawns immediately and
supports up to 36,864 simultaneous pawns in the configured spawn lattice.

## Controls

- Hold the right mouse button and drag to look around.
- Use W/A/S/D to move, and Q/E to move vertically.
- Select the Select tool and left-click a pawn to make it active.
- Hold the left mouse button to apply the selected Fill or Erase tool.
- Fill targets the empty cell adjacent to the selected face; Erase targets the hit solid cell.
- Select Navigate and left-click a visible voxel face to start navigation toward the adjacent empty cell.
- Select Dirt, Grass, Stone, Trunk, or Plank independently from brush mode and size.
- Set the camera-relative rendering distance from the toolbar in world cells.

Each stress-world pawn uses a reproducibly seeded random sequence: half of its goals select a voxel on the ground surface
and half select from the world's full vertical range, deliberately retaining invalid or unreachable destinations. After
arrival, failure, or cancellation, it waits 60 simulation ticks before starting another navigation execution.
Manual tools remain available for every world.

A manual or autonomous goal calls `Navigation.Api::INavigation::Start` synchronously; the Qt adapter never calls the
planner directly. Autonomous population and goal updates run as the first explicitly ordered simulation participant.

## Simulation composition

`UnrealVoxelSim.Composition.Game` constructs only the core domains and generic simulation pipeline. The
`UnrealVoxelSim.Testbed.Worlds` add-on owns world selection, terrain generation, pawn population, and autonomous pawn
decisions, and prepends its participant to the core sequence:

```text
autonomous pawn decisions -> voxel topology and planner advance -> following update -> movement update
```

Neither Qt nor a future engine adapter can reorder those phases. The fixed-step engine only invokes the pipeline and
does not know these domains or their order. Navigation topology is
prepared proactively for the initial world region. Later solid changes are delivered immediately to the
Navigation-owned invalidation adapter before topology and query work. Planning may deliberately remain pending for several 20 ms
simulation steps while deterministic work budgets are consumed; changing presentation speed does not change those
per-tick budgets.

The status bar reports rolling FPS, visible/total pawns, navigation-state counts, resident and visible tiles,
queued/in-flight rebuilds, draw calls, and triangle count. Visible pawns use one instanced draw call so stress results are
not dominated by per-pawn rendering submissions. The testbed requests an uncapped OpenGL swap interval and continuously
schedules frames so the counter reflects the presenter's practical throughput; the window compositor or graphics
driver may still impose a platform cap. Rendering meshes are reconstructible derived state and are never persisted or
treated as simulation authority.

Voxel faces use renderer-neutral surface and texture keys. The Qt presenter resolves those keys to PNG resources deployed
beside the executable in an OpenGL texture array; the voxel sampler and greedy mesher do not depend on Qt, PNG, or
OpenGL.

Windows builds disable shared-library generation, use vcpkg's `x64-windows-static` triplet, and select the static MSVC
runtime. Qt and its Windows platform plugin are linked into the executable; running the testbed does not require adjacent
Qt or MSVC runtime DLLs.

## Profiling

The `windows-msvc-profiling` and `linux-clang-profiling` presets build an optimized application with backend-neutral
instrumentation and Tracy's on-demand client. Ordinary debug and release presets do not fetch, compile, or link Tracy,
and their instrumentation macros compile away.

Start the instrumented Testbed, connect a separately installed Tracy Profiler viewer, then reproduce the workload. The
capture initially separates Qt update callbacks, simulation ticks and every simulation phase, render-frame terrain and
pawn work, and aggregate plots for population, navigation command volume, simulation backlog, tile rebuild queues,
visible pawns, and draw calls. Zones are intentionally batch-level; they do not add virtual calls per pawn, voxel, or
search expansion.

The same profiling build also produces `UnrealVoxelSim.Testbed.Qt.Headless`. It composes the stress simulation without
Qt or rendering and measures uncapped fixed-step throughput for fresh worlds at multiple populations. Setup and a
configurable warm-up are excluded; each result reports minimum, median, and maximum steps per second across repeated
samples:

```text
UnrealVoxelSim.Testbed.Qt.Headless.exe --world stress --entities 0,100,1000,5000,10000 --warmup-steps 200 --measurement-steps 200 --samples 3
```

The first recorded sustained sweep and its hardware/configuration notes are in
[`docs/performance/HeadlessNavigation-2026-08-26.md`](docs/performance/HeadlessNavigation-2026-08-26.md).
