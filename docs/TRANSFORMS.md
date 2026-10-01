# Scene transforms

DCMO 3 adds `rotation = [x, y, z, w]` unit quaternions, `scale = [x, y, z]`
positive axis scales, and an optional column-major `basis = [[x,y,z], ...]`.
Positions are in metres. Local points become `position + R * basis * S * point`.
`basis` normally stays identity; exact reparenting and hierarchy composition retain
shear there. The renderer, shadow pass, CPU picking and culling share this mapping.
Normals use the inverse transpose. Singular, reflected, non-finite and numerically
degenerate transforms are rejected before publication. Scene scale/coordinate
limits also apply to effective composed axes.

DCMO 2 remains readable and editable with yaw/scalar scale. A v2 file cannot
declare free-transform fields. Opening and Save never migrate authored content.
DCMO 3 also accepts legacy yaw/scalar scale; yaw and rotation cannot coexist on
an effective node. Template `remove` explicitly suppresses inherited yaw when
rotation overrides it. Parents remain static under the existing scene contract.

`MeshTransform::point`, `vector`, `normal`, `inversePoint`, `compose` and
`relativeTransform` preserve the exact affine mapping. Existing C++ aggregate
initializers with position/rotation/uniform scale remain source-compatible.
Shader model uniforms changed to seven float4 fields; rebuild CPU and shaders
together. Imported glTF retains its documented selected subset; this change
extends scene placement, not glTF import capabilities.

The editor Inspector exposes quaternion XYZW and scale XYZ on v3 scenes, while
v2 keeps its existing controls. The rotation tool selects a world X/Y/Z axis;
drag preview, snapped rotation, Undo and reparenting retain the complete mapping.

Explicit migration, including a reviewable diff and an untouched original:

```sh
python3 tools/migrate_transforms.py /path/to/project /path/to/new-project-copy
```

The destination must not exist or lie inside the source. Symlinks are rejected.
Native documents are canonically serialized in the new copy; comments remain in
the original and generated `transforms-v3.patch`. Spawn camera yaw is retained.
Review that diff, then open the copied project. Newer/unknown versions fail safely.

`paper_transforms_tests` contains independent analytic expectations for free
rotation, anisotropic normals, affine reparenting, picking distances and source
writer round trips. `paper_gpu_tests` exercises the corresponding shader ABI.
