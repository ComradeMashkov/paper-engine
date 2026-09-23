# First editor slice — 2026-09-23

This is an implementation/build milestone. No application, editor, test executable
or CTest has been run for this work. Human usability and native viewport acceptance
are explicitly outstanding. The code is not a completed Unity/Unreal equivalent.

## Ownership and behavior

Qt 6 Widgets owns the main window, menus, docks, controls, focus and native event
loop. `Viewport` embeds the existing SDL3 GPU renderer, without per-frame CPU image
readback. Cocoa, Win32 and X11 adapters are present; only Cocoa compilation is
verified. Native Wayland embedding is unsupported and reports an error. Qt owns
the window/view; GPU resources and the SDL wrapper are released before QWidget.
SDL's Cocoa Metal attachment requires the bounded patch described in THIRD_PARTY.md.
Qt's responder chain is restored after SDL installs its notification observers;
Qt dispatches events and the editor does not run a second SDL event pump.

`SceneDocument` stores raw authored values and the original TOML source. It does
not serialize a flattened runtime `ScenePackage`. Each transform command captures
node ID, property, before and after values. The project owns a bounded 128-command
undo stack. Dirty is a comparison to the last saved authored snapshot, including
undo across save. A new property is an explicit template override; removing a
property during undo restores inheritance. Other scene fields remain untouched.

`loadScenes` accepts in-memory document overrides and performs its usual resource,
reference, bounds and hierarchy validation. Background rebuilds keep the previous
valid preview; revisions prevent stale worker results from replacing newer edits.
Only one rebuild runs at a time, with subsequent requests coalesced. The current
implementation recompiles the package, rather than incrementally updating the
renderer; initial project loading and save validation remain synchronous.

The inspector exposes local XYZ in metres, Y rotation in degrees (stored in
radians) and uniform scale using engine limits. It preserves unedited components
at their original precision; six-decimal UI display alone never marks the file
dirty. The hierarchy uses readable labels plus IDs and supports search. Right-drag
looks around, the wheel moves along the view direction, F frames selection, and
left-click selects a mesh. Escape, lost focus and hiding release mouse capture.
Layout is local QSettings state; no workspace preference is written to assets.

The standalone host uses neutral procedural materials; a game host supplies its
own material factory and names through `Options`, called on background workers
as well as during load/save, so it must be thread-safe and independent of SDL/UI.
DCMO's host supplies its existing art library. Preview uses neutral illumination,
no game session, scripts, playback, story visibility, game postprocessing or audio.
It is an authoring view, not proof of exact in-game appearance.

## Save contract

- No-op save does not touch the source file.
- Existing transform values are replaced by source ranges; new overrides are
  inserted after the node ID. Comments, IDs, templates and unrelated text remain.
- The writer parses its result and requires semantic equality to the document.
  Unsupported layouts fail before replacement. Inline node tables can replace
  existing values but cannot insert/remove fields in this slice.
- Unknown format versions are rejected; opening never migrates files.
- Before save, all edited scenes are validated through the runtime loader. The
  descriptor, scene, template, resource-manifest and localization source bytes are
  compared against their opening/saved snapshots. External edits block saving.
- `QLockFile` prevents cooperating editors opening the same project twice.
  `QSaveFile` replaces one file atomically, with direct-write fallback disabled.
  Read/write/commit errors keep dirty state and are shown in Problems/a dialog.
- This is not filesystem compare-and-swap against arbitrary external writers:
  a non-cooperating process can race the final comparison/rename. There is no
  multi-file atomic transaction, autosave or crash recovery. Closing several
  dirty scenes saves individually, explicitly described in the confirmation.
- Reopen the same project to reload agreed external changes; its existing lock is
  transferred only after the replacement project validates. Cancel preserves work.

Source-preserving writer tests cover Unicode/comments, inheritance, no-op and
undo across save, malformed transforms, unsupported layouts and compiling an
unsaved parent transform without writing the source. They were compiled, not run.

## Next acceptance steps

After the owner permits execution:

1. Run authoring/resource tests and open the independent Boxes example, then DCMO.
2. Verify Metal/Retina drawing stays inside the viewport; resize docks/window,
   move across displays, minimize/restore, use menus/text input and close repeatedly.
3. Exercise pick → inspect → edit → undo/redo → save → reopen, including inherited
   transforms, non-ASCII paths, external file edits, a second editor and read-only files.
4. Repeat on Windows; measure package rebuild latency/memory on actual author tasks.
5. Review Qt deployment/licensing before distributing an editor package.

Next authoring work: selection outlines and spatial proxies, orbit/pan/fly and
gizmos, create/delete/duplicate/reparent with reference checks, resource browser,
async opening/import progress, document tabs, schema-driven diagnostics, autosave
and recovery. The engine's inherited scene schema still needs generic host
extension fields before treating it as a general-purpose engine API.

## Primary references used for this design

- [Qt QSaveFile](https://doc.qt.io/qt-6/qsavefile.html): atomic replacement and direct-write fallback.
- [Qt QLockFile](https://doc.qt.io/qt-6/qlockfile.html): cooperating process locks.
- [Qt QUndoStack](https://doc.qt.io/qt-6/qundostack.html): command ownership and undo history.
- [Qt QWidget](https://doc.qt.io/qt-6/qwidget.html): native widgets and embedding constraints.
- [SDL_CreateWindowWithProperties](https://wiki.libsdl.org/SDL3/SDL_CreateWindowWithProperties): external Cocoa/Win32/X11 handles.
- The pinned SDL 3.4.12 Cocoa/Metal implementation was inspected locally; its
  external-view mismatch is a source finding, not a claim that Qt officially supports
  this complete SDL GPU integration without platform testing.
