# Source provenance and third-party components

Initial engine sources, shaders and engine-only tests were extracted from
[Don't Cross Me Out](https://github.com/ComradeMashkov/dont-cross-me-out/tree/589ba8334bb2810fd6c7889f776ed4dbddbd97c9)
on 2026-09-23. The human-authored source history remains in that repository.
This repository starts a separate history with namespace/include/build changes,
a source-preserving authoring layer and the first Qt editor. Paper Engine is public; DCMO remains a separate host repository. Publication does
not select a new license for project code. Third-party licenses remain in force.

| Dependency | Pinned source | Notice |
| --- | --- | --- |
| SDL | 3.4.12, vendor/SDL-3.4.12.tar.gz | licenses/SDL-LICENSE.txt, zlib |
| toml++ | 3.4.0, vendor/tomlplusplus/toml.hpp | licenses/tomlplusplus-LICENSE.txt, MIT |
| cgltf | 1.15, vendor/cgltf.h | licenses/cgltf-LICENSE.txt, MIT |
| stb | f0569113c93ad095470c54bf34a17b36646bbbb5 | licenses/stb-LICENSE.txt; terms in headers |
| nlohmann/json | 3.12.0, external glTF test mutation only | licenses/nlohmann-json-LICENSE.txt, MIT |
| Qt | system Qt 6.9+ Widgets/Concurrent, dynamically linked for editor only | Not vendored; retain provider notices when distributing |

Archive/header hashes:

- SDL: b68381f06a7580e63400b3b6eb547ec57d8c3ebde70f9f40e0aba530ba05da27
- cgltf: e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec
- toml++: 6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e
- nlohmann/json: aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63

`cmake/PatchSDLCocoa.cmake` deliberately modifies only the extracted SDL build-tree
copy: external Cocoa windows retain the host root content view; Metal attaches to `sdlContentView` using its local bounds; pixel size is read
from that view's current bounds so Qt dock resizing is handled. The pinned archive
is unchanged. Exact source matching fails configure if the upstream implementation
changes. For ordinary game windows this is the same content view. The adapter's
native ownership, rendering, dock bounds and window resize passed isolated macOS
editor integration checks; broader focus/DPI/platform acceptance remains open.

Qt deployment, notices and license choice are a distribution gate; no editor
package is produced here. Reference: https://doc.qt.io/qt-6/licensing.html.
