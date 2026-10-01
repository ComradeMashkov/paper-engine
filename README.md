# Paper Engine

C++20 engine and an early Qt Widgets scene editor, extracted from
[Don't Cross Me Out](https://github.com/ComradeMashkov/dont-cross-me-out).
Public headers live in `include/paper/`, C++ APIs use `paper`, CMake consumers link
`Paper::…`. The engine contains no game code, assets, Lua VM or game session.

The editor provides hierarchy, inspector, resource placement, create/delete/duplicate,
reparent, world transform gizmos, camera controls, undo/redo and guarded TOML saving.
Native Qt/GPU integration and editing/saving/reopening were tested on temporary
macOS project copies. The Project browser has folders, category/search filters and
a floating window.
Host-configured Play/Stop uses isolated snapshots and temporary saves. Its process
lifecycle is tested with a fixture; real-game Play acceptance, human usability,
recovery and import UI are still outstanding. No game logic runs in Scene view. See [the editor contract](docs/EDITOR.md).

## Build

Requires CMake 3.24+, a C++20 compiler and the native platform SDK. SDL 3.4.12,
Jolt Physics 5.6.0, toml++ 3.4.0, cgltf 1.15 and stb are pinned in `vendor`. Windows/Linux additionally
require DXC (`dxc` in PATH or `PAPER_DXC`). Editor builds require Qt 6.9+ Widgets
and Concurrent; Qt 6.9.0/macOS builds and isolated editor integration tests have passed.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPAPER_BUILD_EDITOR=ON -DPAPER_BUILD_TESTS=ON
cmake --build build --parallel 4
```

For a library/game build omit `PAPER_BUILD_EDITOR`; Qt is optional. Test targets
are opt-in and include an offscreen GPU executable that needs a GPU when run.
The build does not execute tests. Running binaries and CTest needs separate owner
authorization in this workspace. The editor binary is `build/editor/paper_editor`;
on macOS it points into the native `paper_editor.app` development bundle. The included project is `examples/boxes/boxes.paperproject`. Pass its path as the
first argument when manually evaluating the editor.

## Consume from another repository

Pin this repository as a Git submodule, initialize it recursively, then:

```cmake
add_subdirectory(engine)
add_executable(my_game main.cpp)
target_link_libraries(my_game PRIVATE Paper::Runtime Paper::Resources)
```

Targets: `Paper::Core`, `Content`, `Render`, `Resources`, `GPU`, `Audio`,
`Physics`, `UI`, `UIRender`, `Diagnostics`, `Runtime`, `Authoring`; `Editor` exists when explicitly enabled.
`Authoring` uses content/resources and toml++, without Qt/SDL devices. The public API is
source-based and pre-1.0; no stable binary ABI is promised. `VERSION` identifies
the engine independently of the game's version.

The DCMO 2 TOML envelope and `.dcworld/.dcscene/.dcresources/.dctemplates` extensions
remain readable/editable. DCMO 3 adds free rotation and axis scale through explicit
migration; see [transforms](docs/TRANSFORMS.md). Their historical names are file-format identifiers, not
C++ namespaces. `SceneNode` still carries existing authoring fields such as
`legacyDoor`, `actions` and `itemInstance`; rules interpreting them stay in the
host. A generic extension/component schema is future work, not an invisible data
migration. glTF/GLB remains an external model import standard. There is no native
JSON reader or legacy scene-import fallback.

Shared engine systems: [movement and collisions](docs/PHYSICS.md),
[audio banks and acoustic zones](docs/AUDIO.md), [UI and layout](docs/UI.md).

[Provenance and third-party notices](THIRD_PARTY.md),
[contribution rules](CONTRIBUTING.md), [version history](CHANGELOG.md).
