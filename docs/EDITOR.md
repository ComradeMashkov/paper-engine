# Qt scene editor — 2026-10-01

The editor now supports a complete existing-scene editing cycle. macOS Qt 6.9.0
integration tests have executed native rendering, dock visibility, window resize,
create/edit/duplicate/delete/reparent, undo/redo across Save, resource placement
and reopening saved projects on temporary copies. Free transforms and collider
Inspector editing passed on DCMO 3, including an ASan/UBSan run. Resource browser
filters and isolated Play/Stop/close also passed with a test-only child process.
This is an early authoring tool,
not a completed Unity/Unreal equivalent. Human visual/usability acceptance, other
platforms and recovery remain outstanding.

## Workspace and controls

- Left: scene selector, searchable hierarchy with readable labels and stable IDs.
- Right: label/localization key, parent, resource, local XYZ in metres, yaw in degrees,
  uniform scale for v2; local rotation XYZ in degrees, quaternion XYZW and scale XYZ
  for explicitly migrated v3 scenes. Degree controls apply Y × X × Z rotation.
  Collision shape/category/mask, local collider bounds, shadow flag and template
  reset are available. Enabling bounds collision initially fits the resource geometry;
  Bounds edits center, half extents and orientation independently of render geometry.
- The editor interface defaults to English; project labels/localized game content are
  displayed as authored. Editor translations can be added later.
- Bottom: Project browser with actual asset folders, category groups/filter, search
  across names/IDs/paths within the selected subtree, file paths and a distinct
  Placeable marker for declared resources. Double-click a resource or Add to Scene
  to instantiate; double-click a declared scene file to switch scenes. Open Folder
  reveals its folder in the OS file manager; Float / Dock opens a separate window.
  Refresh scans files asynchronously, without following symlinks, with a visible
  100,000-entry index limit. Categories classify files; they do not guarantee import
  support. Double-click `.pui` or `.pabank` to edit that asset. File has New/Edit UI
  and New/Edit Audio Bank actions; new files must lie inside project assets and
  cannot overwrite existing files. Import, moving and file deletion are not offered.
- Object menu/toolbar: empty object, resource instance, duplicate subtree with new
  IDs and remapped internal parents, delete subtree. Duplicate refuses known host
  bindings (actions, state, item instances, legacy doors and visibility expressions).
  Engine validation cannot discover arbitrary references from host scripts.
- View menu: Frame Selected, refresh, panel toggles and Restore Panel Layout.
  Layout/settings are local QSettings state. `PAPER_EDITOR_SETTINGS_DIR` isolates QA.
- Q/W/E/R in the focused viewport select selection/move/rotation/scale tools. Left-click
  picks a mesh; a selected object has a bounds outline. Drag colored world axes to
  move, the screen ring horizontally to rotate around the selected world X/Y/Z axis
  (Y only in v2), or the diagonal handle to
  scale uniformly. One drag is one undo command; Escape or lost focus cancels it.
- Snap uses 0.5 metres, 15 degrees or 0.1 relative scale. Optional X-ray grid shows
  the world ground plane. Overlays are drawn after the scene and do not depth-test.
- Collisions in the scene toolbar draws green collider bounds and exact triangles
  for the selected mesh collider. Opening an audio bank draws blue oriented acoustic
  zones and pink source positions/ranges; Hide Audio Overlays clears them. Node-bound
  sources use full world transforms and are filtered to the selected scene.
- RMB look + WASD/QE fly, Shift accelerates, MMB pans, Alt+LMB orbits selection,
  wheel moves forward/back, F frames selection. Mouse capture releases on Escape,
  lost focus and hiding. Delete/Backspace and Cmd/Ctrl+D are scoped to hierarchy
  and viewport so typing in an inspector field does not delete/duplicate objects.

## Architecture and boundaries

Qt 6 Widgets owns windows, menus, docks, focus and the event loop. `Viewport` embeds
SDL3 GPU rendering without per-frame CPU readback. Cocoa integration preserves the
Qt root content view and responder chain, attaches Metal to the supplied child view,
and reads that view's actual pixel bounds. The pinned SDL archive stays unchanged;
checked build-tree substitutions are reapplied on configure. A native assertion
rejects a wrapper that replaces Qt's view hierarchy. Qt owns process signals.
Win32/X11 adapters are present but unverified; native Wayland is unsupported.

`Paper::Authoring` is independent of Qt/SDL. `SceneDocument` holds original TOML and
raw authored values, not flattened runtime transforms. Commands capture authored
node snapshots and selection, with a 128-command project history. Undo returns to
the affected scene. Dirty compares against the latest saved snapshot. Template
inheritance and explicit `remove` lists survive editing; technical IDs are stable.
Reparent preserves the exact affine world transform, including shear in v3, and
uses runtime cycle/static-parent validation. Gizmo preview updates instance
transforms in memory, including children;
only release commits to the document. Invalid changes leave it unchanged.

`loadScenes` and physical collider construction validate candidate documents before
commands are accepted and before saving. Initial load and command/save validation
are synchronous and rebuild the whole package; large-project latency still needs
profiling. Undo/refresh rebuilds
run in one background worker with revision checks and coalescing. An already
published revision is not republished over a later drag. The last valid preview
remains visible during validation; gizmo editing waits for the matching package.

The host supplies its thread-safe procedural material factory through `Options`;
the standalone editor uses neutral materials. Scene view renders authored scene state;
hosts execute gameplay scripts and resolve story visibility in their runtime.
DCMO 2 retains yaw/uniform scale controls;
DCMO 3 supports free rotation and axis scale. File → Upgrade Project Copy for Free
Transforms creates and validates a separate v3 project, including unsaved scenes,
without changing the original. See [transforms](TRANSFORMS.md).

UI and audio bank editors are nonmodal component windows: scene navigation remains
available. Each has structured forms, a Source tab, shared Undo/Redo and guarded
atomic Save. Pending fields are applied and validated before Save or Play. Invalid
drafts remain editable; Undo first discards a pending form draft. Structured changes
canonicalize the asset; untouched/source-only edits preserve their text. Close offers
Save/Discard/Cancel. Scene Save All does not save these separate asset windows.

UI Design provides the six shared components, hierarchy, add/duplicate/delete,
parent/reorder, layout constraints, theme and viewport. Design clicks select nodes;
Interact exercises the engine's layout/draw/input state without changing authored
values. Audio editing exposes sounds, source bindings and acoustic zones, node/sound
choices and WAV selection. Audition Selected opens audio explicitly; Stop, applied edits,
closing or Play releases the audition stream. Opening a bank never plays sound.
See [UI](UI.md), [physics](PHYSICS.md) and [audio](AUDIO.md).

## Play / Stop and host contract

- F5 / Play launches a separate host game window; Shift+F5 / Stop ends it. The Start
  at selector lists spawns in the selected scene. No spawn or no configured runtime
  disables Play with a status explanation. The Console shows preparation, process
  output, launch failures and exit status. Its history is bounded to 1,000 blocks.
- All unsaved scene documents and the open UI/audio asset drafts are serialized into
  a temporary copy of the complete asset tree. The copy's world entrySpawn is changed
  to the selected spawn; original
  files are neither saved nor rewritten. Scene edits made during Play apply on the
  next run. Invalid component drafts or missing audio resources/bindings block Play.
  Undo, selection and dirty state remain authoring state. Hosts must load these assets
  through the shared APIs to display/use them; authoring does not install host menus.
- Preparation runs on a worker, checks opened source bytes and file inventory before
  and after copying, rejects symlinks/special files and caps content at 4 GiB and
  100,000 files. Stop cancels preparation; cancellation is checked between copies.
  This is a local consistency guard, not a transaction against arbitrary concurrent
  writers. Large-project copy/cleanup latency remains to be profiled.
- The host sets an absolute `Options::playExecutable` and optional structured
  `playArguments` in trusted C++ code. Project data cannot choose an executable or
  shell command. The editor appends `--paper-play <absolute session.paperplay>`.
  The standalone Paper editor has no game host by default; the DCMO adapter supplies
  its compiled runtime target and builds it together with the editor.
- `session.paperplay` is TOML: `format = "paper.play"`, integer `version = 1`, and
  `world` naming the asset-relative world manifest. Sibling `assets/` holds copied
  content; `state/` is the only runtime save/preferences/diagnostics directory.
  A host must explicitly honor these roots with no fallback to player data, start
  a fresh real session at the manifest entrySpawn, report errors to stderr and
  poll sibling `stop.request` in its main loop. `EngineConfig::exactAssetRoot`
  disables bundled/source asset fallback. It defaults to false for existing hosts.
- Stop writes the request, allows 3 seconds for exit, requests process termination,
  then kills only the owned child after a further second. Closing the editor waits
  asynchronously for the child before removing the snapshot. Repeat launches are
  disabled until completion. A forced exit may discard temporary runtime state.
- There is no Pause/Step, embedded Game view, runtime-to-scene Apply, live reload or
  editor-crash recovery/orphan reaping yet. The game host integration is compiled
  but has not been executed: the owner authorized only editor/authoring checks.

## Save contract

- No-op saves do not rewrite files. Changed fields use original source ranges;
  structural edits retain existing node source blocks and format only new nodes.
  Comments, templates, IDs and other scene sections survive. The writer reparses
  output and requires exact semantic equality before producing replacement text.
- Structural edits require explicit `[[scene.nodes]]` tables (or `nodes = []`).
  Unsupported/ambiguous layouts fail without touching disk. Inline tables allow
  existing-value replacement, but not field/structural insertion or deletion.
- Unknown versions are rejected; opening does not migrate data. Current native
  formats stay TOML/DCMO 2 or explicitly migrated DCMO 3; glTF/GLB is the external
  model-format exception.
- Descriptor, scenes, templates, manifests and localization source bytes are
  checked against their opening/saved snapshots. External changes block writing.
- QLockFile prevents cooperating editor instances. QSaveFile atomically replaces
  one file with direct-write fallback disabled. Failure retains dirty state.
  Save All is sequential, not a multi-file transaction. There is no autosave or
  crash recovery yet. A non-cooperating writer can race the final compare/rename.
- Source checking does not infer arbitrary game-script dependencies. Review host
  references when deleting authored nodes used by gameplay.

## Verified and remaining acceptance

Executed, with owner authorization and only on disposable copies:

- `paper_authoring_tests`: no-op byte identity, Unicode/comments, transform and
  metadata edits, create/delete/reorder and undo across Save, nested source tables,
  empty-scene transitions, invalid IDs, and unsaved transforms through the loader.
- `paper_editor_workspace_tests <copied-project.paperproject>`: creates another
  temporary copy itself, isolates settings, checks native GPU frames and nonoverlap
  of visible docks, then edits/saves/reopens. Passed with Boxes v2/v3 and a host
  project copy upgraded to v3. `--upgrade` exercises migration on the test's own copy.
  The test is deliberately outside automatic CTest because it needs a native desktop.
- `paper_editor_play_tests`: isolated synthetic files and a test-only child verify
  unsaved snapshot/media, source/save isolation, repeat, cancel, launch/content
  failures, changed files, symlinks and forced Stop. No game code is linked into
  `paper_editor_play_fixture`. Workspace tests also exercise the actual toolbar,
  spawn selector, dirty/Undo preservation and window close with that fixture.
- `paper_editor_component_tests`: versioned UI round trips and malformed documents,
  structured controls, actual preview layout/input, pending drafts, Undo across Save,
  atomic save/external-change guards, WAV/binding validation, dummy-device audition,
  shipped examples and transactional v3 project-copy migration.
- All 16 CTest cases passed in Release and ASan/UBSan, including Metal and the
  component editors. Release standalone editor and host game/editor builds passed.
  UI/audio forms were visually inspected from test-window captures.

These programmatic checks are not a claim of human visual acceptance or
comprehensive pointer/keyboard testing.
Still required: direct manipulation review, Retina/multiple displays, dock dragging,
minimize/restore, keyboard-only/IME, external-edit/write-failure injection, performance
and user sessions; then Windows. The game was not run.

Next work: autosave/recovery and Save All journal, import/reimport and thumbnails,
component/light/room/spawn authoring, script-aware reference tools, multi-selection,
local axes, copy/paste, scene creation, Play acceptance/Pause, incremental validation and packaging.
Review Qt deployment/licensing before distributing a package.

## References

- [Qt QProcess](https://doc.qt.io/qt-6/qprocess.html)
- [Unity Project window](https://docs.unity3d.com/Manual/ProjectView.html)
- [Qt QSaveFile](https://doc.qt.io/qt-6/qsavefile.html)
- [Qt QLockFile](https://doc.qt.io/qt-6/qlockfile.html)
- [Qt QUndoStack](https://doc.qt.io/qt-6/qundostack.html)
- [Qt QWidget](https://doc.qt.io/qt-6/qwidget.html)
- [SDL external windows](https://wiki.libsdl.org/SDL3/SDL_CreateWindowWithProperties)
- SDL 3.4.12 Cocoa/Metal source inspected locally; the adapter patch is our source
  integration, not a claim that Qt officially guarantees this SDL GPU combination.
