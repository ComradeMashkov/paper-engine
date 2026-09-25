# Qt scene editor — 2026-09-23

The editor now supports a complete existing-scene editing cycle. macOS Qt 6.9.0
integration tests have executed native rendering, dock visibility, window resize,
create/edit/duplicate/delete/reparent, undo/redo across Save, resource placement
and reopening saved projects on temporary copies. This is an early authoring tool,
not a completed Unity/Unreal equivalent. Human visual/usability acceptance, other
platforms and recovery remain outstanding.

## Workspace and controls

- Left: scene selector, searchable hierarchy with readable labels and stable IDs.
- Right: label/localization key, parent, resource, local XYZ in metres, yaw in degrees,
  uniform scale, shadow flag and reset of transform overrides to the template.
- The editor interface defaults to English; project labels/localized game content are
  displayed as authored. Editor translations can be added later.
- Bottom: Project browser with actual asset folders, category groups/filter, search
  across names/IDs/paths within the selected subtree, file paths and a distinct
  Placeable marker for declared resources. Double-click a resource or Add to Scene
  to instantiate; double-click a declared scene file to switch scenes. Open Folder
  reveals its folder in the OS file manager; Float / Dock opens a separate window.
  Refresh scans files asynchronously, without following symlinks, with a visible
  100,000-entry index limit. Categories classify files; they do not guarantee import
  support. No import, file editing, moving or deletion is offered yet.
- Object menu/toolbar: empty object, resource instance, duplicate subtree with new
  IDs and remapped internal parents, delete subtree. Duplicate refuses known host
  bindings (actions, state, item instances, legacy doors and visibility expressions).
  Engine validation cannot discover arbitrary references from host scripts.
- View menu: Frame Selected, refresh, panel toggles and Restore Panel Layout.
  Layout/settings are local QSettings state. `PAPER_EDITOR_SETTINGS_DIR` isolates QA.
- Q/W/E/R in the focused viewport select selection/move/yaw/scale tools. Left-click
  picks a mesh; a selected object has a bounds outline. Drag colored world axes to
  move, the screen ring horizontally to rotate around Y, or the diagonal handle to
  scale uniformly. One drag is one undo command; Escape or lost focus cancels it.
- Snap uses 0.5 metres, 15 degrees or 0.1 relative scale. Optional X-ray grid shows
  the world ground plane. Overlays are drawn after the scene and do not depth-test.
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
Reparent preserves world position/yaw/scale and uses runtime cycle/static-parent
validation. Gizmo preview updates instance transforms in memory, including children;
only release commits to the document. Invalid changes leave it unchanged.

`loadScenes` validates candidate documents before commands are accepted and before
saving. Initial load and command/save validation are synchronous and rebuild the
whole package; large-project latency still needs profiling. Undo/refresh rebuilds
run in one background worker with revision checks and coalescing. An already
published revision is not republished over a later drag. The last valid preview
remains visible during validation; gizmo editing waits for the matching package.

The host supplies its thread-safe procedural material factory through `Options`;
the standalone editor uses neutral materials. No game session, scripts, story
visibility, audio or Play mode runs in Scene view. Yaw and uniform scale reflect
the current runtime schema; full XYZ rotation/nonuniform scale are not offered.

## Save contract

- No-op saves do not rewrite files. Changed fields use original source ranges;
  structural edits retain existing node source blocks and format only new nodes.
  Comments, templates, IDs and other scene sections survive. The writer reparses
  output and requires exact semantic equality before producing replacement text.
- Structural edits require explicit `[[scene.nodes]]` tables (or `nodes = []`).
  Unsupported/ambiguous layouts fail without touching disk. Inline tables allow
  existing-value replacement, but not field/structural insertion or deletion.
- Unknown versions are rejected; opening does not migrate data. Current native
  formats stay TOML/DCMO 2; glTF/GLB is the external model-format exception.
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
  of visible docks, then edits/saves/reopens. Passed with Boxes and a host project.
  The test is deliberately outside automatic CTest because it needs a native desktop.
- macOS Release builds of editor/authoring targets. Detailed host verification is
  recorded in the consuming project's development log.

Native desktop screenshot automation timed out, so these programmatic checks are
not a claim of human visual acceptance or comprehensive pointer/keyboard testing.
Still required: direct manipulation review, Retina/multiple displays, dock dragging,
minimize/restore, keyboard-only/IME, external-edit/write-failure injection, performance
and user sessions; then Windows. The game and unrelated test binaries were not run.

Next work: autosave/recovery and Save All journal, import/reimport and thumbnails,
component/light/room/spawn authoring, script-aware reference tools, multi-selection,
local axes, copy/paste, scene creation, Play/Stop, incremental validation and packaging.
Review Qt deployment/licensing before distributing a package.

## References

- [Qt QSaveFile](https://doc.qt.io/qt-6/qsavefile.html)
- [Qt QLockFile](https://doc.qt.io/qt-6/qlockfile.html)
- [Qt QUndoStack](https://doc.qt.io/qt-6/qundostack.html)
- [Qt QWidget](https://doc.qt.io/qt-6/qwidget.html)
- [SDL external windows](https://wiki.libsdl.org/SDL3/SDL_CreateWindowWithProperties)
- SDL 3.4.12 Cocoa/Metal source inspected locally; the adapter patch is our source
  integration, not a claim that Qt officially guarantees this SDL GPU combination.
