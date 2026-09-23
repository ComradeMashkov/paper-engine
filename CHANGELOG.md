# Changelog

## [0.2.0-alpha.1+build.3] - 2026-09-23

- Add a usable scene workspace: searchable resources, creation, subtree duplication
  and deletion, names/resource/shadow inspector, world-preserving reparent, transform
  reset, panel-layout reset and sequential Save All. Keep host bindings out of copies.
- Add bounds selection, world move/yaw/uniform-scale handles, snap, optional X-ray
  grid, fly/pan/orbit navigation and one undo command per committed drag.
- Preserve authored TOML through structural edits, including nested tables, empty
  scenes and undo after saving. Validate candidates before accepting edits.
- Verification: authoring tests passed, including three copied host scene round trips;
  native Qt/GPU workspace checks passed on temporary Boxes and host-project copies
  through edit/save/reopen and resize. macOS editor/test targets compiled. Desktop
  screenshot automation timed out; human visual acceptance remains open.
- Limits: synchronous full-package command validation, yaw/uniform scale, existing
  resources only; no recovery, scene creation, import UI, Play mode or packaging.
  Windows/Linux and game execution were not tested. See docs/EDITOR.md.

## [0.1.1-alpha.1+build.2] - 2026-09-23

- Keep the host Qt content view intact when wrapping a Cocoa viewport with SDL;
  apply the checked patch on every configure and assert native view ownership.
  Build native macOS editor app bundles and let Qt own process signal handling.
- Isolate QA settings through PAPER_EDITOR_SETTINGS_DIR, accept a copied fixture
  root in authoring tests, and give the Boxes example a valid spawn room.
- Verification: macOS editor compiled; authoring document tests passed on copied
  Boxes assets. Native UI automation timed out; visual acceptance is still pending.
  Game and other test executables were not run.

## [0.1.0-alpha.1+build.1] - 2026-09-23

- Establish Paper Engine as an independent public repository with `paper` APIs and `Paper::…`
  CMake targets, extracted from DCMO commit 589ba8334bb2810fd6c7889f776ed4dbddbd97c9.
  Keep pinned dependencies, notices, engine tests and shaders with the engine;
  preserve DCMO 2 TOML and external glTF/GLB format compatibility.
- Add optional Qt 6 Widgets editor, host material interface, standalone Boxes example,
  native SDL GPU viewport, hierarchy/search, transform inspector, undo/redo, source-
  preserving scene writer, per-document dirty state and guarded atomic file saving.
- Compile unsaved documents through the existing scene validator, retain the last
  valid preview and discard stale background rebuild results. Keep game/session/Lua
  out of the authoring process. Add bounded SDL Cocoa external-view build-tree patch.
- Verification: macOS Release standalone engine/editor and eight test executables
  compiled; numeric/source/scene metadata checks performed. Binaries, CTest, GPU
  execution and user flows were not run. Inherited particle-test aggregate warning
  remains; no runtime acceptance or packaging is claimed.
- Limitations: first transform-editing slice only; no gizmos, object creation/removal,
  recovery/autosave or transactional Save All. Qt/native DPI, resize and focus still
  need manual evaluation; Windows/X11 are unverified, native Wayland unsupported.
