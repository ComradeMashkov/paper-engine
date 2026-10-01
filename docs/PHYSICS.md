# Movement and collision

`Paper::Physics` provides `PhysicsWorld`, a Jolt-backed static collision world,
and `CharacterController`, a swept virtual capsule with gravity, jumping, step
climbing, floor snapping, slope limits and crouch/stand clearance. Distances are
metres, velocities metres/second, angles radians, elapsed time seconds. A frame
is split into bounded substeps; invalid inputs and excessive elapsed time fail
explicitly. Settings are per character, rather than host-game constants.

Bodies, queries and characters use symmetric 32-bit category/mask filtering:
both `(a.category & b.mask)` and `(b.category & a.mask)` must be nonzero.
Stable collider IDs survive pose updates and are never reused. Boxes and static
triangle meshes bake axis scale/shear into their shapes; `move` updates only the
rigid position/rotation. Recreate a collider when changing its baked geometry.
Use triangle winding consistently; meshes represent surfaces, not solid volumes.

DCMO 3 accepts `collision = { shape="bounds", category=1, mask=4294967295 }` or
`shape="mesh"`. Boolean collision remains compatible in v2 and v3. The editor
Inspector exposes shape and decimal bitsets on v3 scenes. Mesh collision requires
a static, non-billboard geometry resource and uses its bind pose. Character and
render poses share the full world transform. Call `addSceneColliders` for the
selected scene; installation rolls back on failure. Remove the returned IDs when
unloading, or destroy the world. Dynamic gameplay visibility/state is resolved by
the host before installation; the engine does not interpret host state keys.

Construct, query, mutate and destroy on one owner thread. The world is not a
rigid-body dynamics simulator; characters query terrain and do not collide with
other virtual characters. Character instances retain world state until destroyed.
World initialization owns the process-wide Jolt factory. No SDL, Qt or host-game
code is needed by the physics API. Pinned Jolt sources/license are tracked;
SIMD extensions and GPU compute backends are disabled for portable builds.

Defaults cap a frame at 0.25 s with 1/60 s substeps. At most 256 substeps are
accepted. API coordinates/ray lengths are bounded at 10,000 m, commanded velocity
at 1,000 m/s, capsule height at 1,000 m and collider triangles at 50,000. World
capacities are explicit. Large worlds should use local coordinates.

Regression cases cover stairs, walkable/steep slopes, jump, low ceilings, sliding,
thin-wall sweeps, layers, invalid settings, stale IDs and transactional scene load.
