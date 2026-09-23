# Changelog

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
