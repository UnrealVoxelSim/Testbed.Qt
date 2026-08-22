# UnrealVoxelSim.Testbed.Qt

Interactive, non-production host for evaluating UnrealVoxelSim domains without Unreal Engine.

The current testbed composes the chunked voxel field, solid domain, renderer-neutral greedy mesher, event dispatcher,
and a Qt 6 OpenGL widget. It keeps a bounded set of camera-relative logical mesh tiles resident, captures authoritative
solid snapshots on the UI/simulation thread, meshes snapshots asynchronously, uploads only current results, and
frustum-culls tile bounds before drawing.

## Controls

- Hold the right mouse button and drag to look around.
- Use W/A/S/D to move, and Q/E to move vertically.
- Hold the left mouse button to apply the selected Fill or Erase brush.
- Fill targets the empty cell adjacent to the selected face; Erase targets the hit solid cell.
- Select Dirt, Grass, or Stone independently from brush mode and size.
- Set the camera-relative rendering distance from the toolbar in world cells.

The diagnostic bar reports rolling FPS, resident and visible tiles, queued/in-flight rebuilds, draw calls, and triangle
count. The testbed requests an uncapped OpenGL swap interval and continuously schedules frames so the counter reflects
the presenter's practical throughput; the window compositor or graphics driver may still impose a platform cap.
Rendering meshes are reconstructible derived state and are never persisted or treated as simulation authority.

Windows builds disable shared-library generation, use vcpkg's `x64-windows-static` triplet, and select the static MSVC
runtime. Qt and its Windows platform plugin are linked into the executable; running the testbed does not require adjacent
Qt or MSVC runtime DLLs.
