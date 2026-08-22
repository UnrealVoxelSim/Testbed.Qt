# UnrealVoxelSim.Testbed.Qt

Interactive, non-production host for evaluating UnrealVoxelSim domains without Unreal Engine.

The current testbed composes the chunked voxel field, solid domain, renderer-neutral greedy mesher, event dispatcher,
deterministic simulation, grounded movement, voxel navigation, path following, one pawn, and a Qt 6 OpenGL widget. It
keeps a bounded set of camera-relative logical mesh tiles resident, captures authoritative solid snapshots on the
UI/simulation thread, meshes snapshots asynchronously, uploads only current results, and frustum-culls tile bounds
before drawing.

## Controls

- Hold the right mouse button and drag to look around.
- Use W/A/S/D to move, and Q/E to move vertically.
- Hold the left mouse button to apply the selected Fill or Erase brush.
- Fill targets the empty cell adjacent to the selected face; Erase targets the hit solid cell.
- Select Navigate and left-click a visible voxel face to queue a goal in the adjacent empty cell for the pawn.
- Select Dirt, Grass, or Stone independently from brush mode and size.
- Set the camera-relative rendering distance from the toolbar in world cells.

The status bar reports the pawn's navigation execution state: planning, following, replanning, arrived, unreachable, or
cancelled. A navigation click creates a stamped `Navigation.Api::Start` for the current simulation tick and submits it to
the navigation-specific command sink; the Qt adapter never calls the planner or movement implementation directly.

## Simulation composition

The testbed is the composition root and explicitly owns this per-tick order:

```text
solid commands -> queued solid-change events -> navigation commands -> topology update
-> planner advance -> following update -> movement update -> queued post-movement events
```

The fixed-step engine only invokes the pipeline and does not know these domains or their order. Navigation topology is
prepared proactively for the initial world region. Later solid changes flow through `Voxel.Solid.Navigation` into
planner invalidation before topology and query work. Planning may deliberately remain pending for several 20 ms
simulation steps while deterministic work budgets are consumed; changing presentation speed does not change those
per-tick budgets.

The diagnostic bar reports rolling FPS, resident and visible tiles, queued/in-flight rebuilds, draw calls, and triangle
count. The testbed requests an uncapped OpenGL swap interval and continuously schedules frames so the counter reflects
the presenter's practical throughput; the window compositor or graphics driver may still impose a platform cap.
Rendering meshes are reconstructible derived state and are never persisted or treated as simulation authority.

Windows builds disable shared-library generation, use vcpkg's `x64-windows-static` triplet, and select the static MSVC
runtime. Qt and its Windows platform plugin are linked into the executable; running the testbed does not require adjacent
Qt or MSVC runtime DLLs.
