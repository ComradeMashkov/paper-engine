# Changelog

## [0.15.0-alpha.1+build.19] - 2026-10-05

- Add an owning categorized string-table reader shared by runtime hosts and editor labels.
  Category names never change complete text IDs; duplicate IDs across categories,
  empty text and nested/non-string entries are rejected. Flat tables remain readable.
- Keep native writer semantics and editor source/save/recovery paths. No game or Lua
  dependencies were introduced.
- Verification: Release host build and the existing document tests compiled; grouped
  Unicode/writer and rejection regressions added. Full final verification is recorded
  in the subsequent task entry; no platform or human acceptance is implied here.

## [0.14.0-alpha.1+build.18] - 2026-10-05

- Provide `Paper::AudioCore` for bank definitions, bindings, acoustic zones and PCM mixing without SDL/Qt. Keep the existing `Paper::Audio` facade and functionality for device/WAV consumers. Headless game sessions can validate editable audio without acquiring a platform dependency.
- Verification: all engine targets built, and all 18 standalone CTest cases passed, in Release and ASan/UBSan including Metal, storage/recovery, authoring, audio and UI. Version, numeric ownership and whitespace checks passed.
- Compatibility: additive CMake target; the existing facade, audio bank version 2 and content formats are unchanged.

## [0.13.1-alpha.1+build.17] - 2026-10-05

- Keep a slider's grab offset when its thumb is pressed off centre; apply the final release coordinate before ending capture. Preserve track-click snapping, quantization and cancellation.
- Verification: all engine targets built in Release and ASan/UBSan. Shared UI tests passed in both configurations, including off-centre grabs, final release and cancellation.

## [0.13.0-alpha.1+build.16] - 2026-10-05

- Author spatial and nonspatial source modes in audio bank version 2; continue reading version 1 with its original spatial defaults. Expose the mode through the existing structured source editor.
- Replace a decoded bank through the live SDL stream lock without reopening the device. Preserve mute and bus levels; reject malformed replacements before publication. Expose bank replacement and complete binding frames through the runtime facade.
- Verification: audio-bank and editor-component tests passed in Release and ASan/UBSan, including version compatibility, shared placements and dummy-device replacement failure. All engine targets built in both configurations; no game was launched.
- Compatibility: writers emit bank version 2, which older engines cannot read. Version 1 input remains supported. Already queued PCM can finish within device latency after a successful replacement.

## [0.12.0-alpha.1+build.15] - 2026-10-05

- Add filtered feet-origin capsule placement queries for saved-player validation and moving collider clearance. Permit support contact within an explicit metre tolerance, reject malformed geometry and stale collider IDs, and retain owner-thread enforcement.
- Verification: physics tests passed in Release and ASan/UBSan, including low ceilings, support contact, reciprocal layers and moved/removed colliders. Version, numeric ownership and whitespace checks passed.
- Compatibility: additive Qt/SDL-independent physics API; content and save formats are unchanged.

## [0.11.1-alpha.1+build.14] - 2026-10-05

- Initialize missing interaction bounds from the selected resource when a host component is attached; preserve authored bounds.
- Keep large host object-property forms inside a resizable scroll area so every field and Apply remain reachable. This was found while connecting DCMO door/item authoring.
- Verification: complete scene/editor integration tests passed in Release and ASan/UBSan on temporary projects, including scrolling and applying a host component. Metadata, numeric ownership and whitespace checks passed.

## [0.11.0-alpha.1+build.13] - 2026-10-05

- Create scenes as unsaved drafts and journal their first save with the world manifest. Author rooms, lights, spawns, transitions/trigger volumes and project entry points with complete-project validation and Undo across Save.
- Preserve untouched TOML/comments in every scene section; retain source-only edits. Display scene data overlays and edit inherited object properties with host-registered game fields and choices.
- Add guarded UTF-8 host-source editors, Find, validation and Undo. Save from these editors uses the project journal and host cross-file validation; include source drafts in recovery and isolated Play. The engine contains no game VM.
- Recover new scenes and world-manifest drafts; reopen invalid scene drafts for staged repair. Compile and migrate virtual new scenes before any authored file exists. Validate Save Scene against the exact disk candidate rather than unrelated unsaved documents.
- Verification on macOS: 18 CTest cases passed in Release and ASan/UBSan, including Metal, cold recovery, invalid pending forms, cross-scene save guards and host-source validation. Native Boxes workspace and isolated Play lifecycle checks passed on temporary copies. Version/numeric ownership and whitespace checks passed.
- Compatibility: DCMO 2/3 remain supported; additions to the host editor API are source-compatible. Host triggers/sequences are authored as source; visual scripting graphs and real-game Play acceptance remain outside these engine checks.

## [0.10.0-alpha.1+build.12] - 2026-10-05

- Journal Save All across scene documents and open UI/audio banks before atomically replacing files. Validate every source before writing; retain interrupted intent for restart or explicit retry.
- Capture separate automatic recovery snapshots every 30 seconds, including unapplied and invalid UI/audio property drafts. Offer Restore/Discard/Cancel; restore scenes as one Undo command and leave authored files untouched.
- Bound journal decoding, reject duplicate targets, escaping/hidden paths and symlinks, and retain externally conflicting recovery data. Keep local recovery state out of Git and Play snapshots.
- Verification on macOS: storage, component and authoring CTest cases passed in Release and ASan/UBSan, including an actual writer-process crash. Native copied-Boxes workspace checks passed through Save/reopen and isolated Play/Stop. Version, numeric ownership and whitespace checks passed.
- Limits: individual file replacements become visible sequentially. The journal restores consistency after process failure; this does not provide isolation from non-cooperating writers or a filesystem-wide power-loss transaction.

## [0.9.0-alpha.1+build.11] - 2026-10-01

- Expose collider bounds editing, initial fitting, category/mask authoring and
  scene overlays; show exact triangles for the selected mesh collider.
- Add local X/Y/Z rotation in degrees alongside quaternion/axis-scale controls.
  Upgrade project copies to DCMO 3 in the editor, including unsaved scenes,
  template removals and full project validation before publishing the copy.
- Add structured audio sound/source/zone forms, scene-node choices, WAV selection,
  explicit audition/Stop and source/range/oriented-zone overlays in Scene view.
- Add versioned `.pui` assets and visual UI authoring: component hierarchy,
  properties, reparent/reorder/duplicate/delete, theme/viewport and isolated
  interactive preview using the shared layout, drawing and input system.
- Preserve no-op/source edits; publish structured edits as single Undo steps,
  retain exact untouched values and guard atomic saves against external changes.
  Keep component windows nonmodal and add browser/menu actions and Boxes examples.
- Include validated unsaved UI/audio drafts in isolated Play snapshots without
  saving originals; validate source bindings against the current scene documents.
- Verification on macOS: all 16 CTest cases passed in Release and ASan/UBSan,
  including Metal. Native Boxes v2/v3 and an upgraded host project copy passed;
  native v3 also passed under ASan/UBSan. Isolated Play lifecycle checks passed.
  UI/audio test-window captures were visually inspected. Host game/editor builds
  passed; the game was not launched.

## [0.8.1-alpha.1+build.10] - 2026-10-01

- Fix collider source serialization and preserve nested-table comments, disable,
  Undo and Save semantics. Retain pending collider shape choices in the Inspector.
- Fix a temporary-lifetime error during world-axis rotation, preserve editable
  axis-scale magnitudes and validate free transforms inherited from v3 templates.
- Reject overflowing collider coordinates and triangle budgets before Jolt calls;
  validate authored audio loops and zone ambience against their shared capacity.
- Enable dependency RTTI for UBSan builds and add independent affine GPU,
  v3 Inspector/template, collider authoring and capacity regressions.
- Verification on macOS: all 15 CTest cases passed in Release and ASan/UBSan,
  including Metal GPU and offscreen audio-bank editing. Native v2/v3 workspace
  and isolated Play lifecycle tests passed; the v3 workspace also passed under
  ASan/UBSan. Engine/editor and host application targets compiled. The real game
  was not launched; other platforms and human visual acceptance remain unverified.

## [0.8.0-alpha.1+build.9] - 2026-10-01

- Add shared panels, labels, buttons, toggles, sliders, lists and tooltips with a
  host-measured, device-independent row/column layout system.
- Add constrained grow/shrink allocation, alignment, padding, wrapping, nested
  clipping, scrolling, focus navigation, pointer capture and bounded tree validation.
- Publish tree/layout changes transactionally; retain the previous valid state
  on failure. Render only visible list rows and preserve host clipping.
- Add Engine drawing/input adapters with ordered wheel/motion/key edges while
  retaining aggregate relative camera motion. Host menu adoption stays explicit.
- Verification: Release UI/runtime/editor compiled; layout, wrap, constraints,
  capture, focus, keyboard/list/slider, disabled/hidden states, rollback and
  ordered host-input adapter regressions passed. See docs/UI.md for usage.

## [0.7.0-alpha.1+build.8] - 2026-10-01

- Add editable TOML audio banks, validated source instances and oriented acoustic
  zones with priority, boundary fade, ambience and stable reverb profiles.
- Resolve source offsets through full scene transforms and submit bounded loop/scene
  snapshots under one stream lock; keep allocation and I/O outside the PCM callback.
- Validate all declared WAV variants and PCM budgets before replacing live audio.
  Add optional runtime bank configuration and a source-preserving bank editor with
  strict validation, Undo/Redo, atomic Save and external-change protection.
- Verification: Release audio/runtime/editor compiled; bank/source/zone, loop-capacity,
  strict WAV/dummy-device replacement, echo-decay and offscreen editor-save tests passed.
- Limits: banks reload explicitly; no editor audition or automatic live reload.

## [0.6.0-alpha.1+build.7] - 2026-10-01

- Add a pinned Jolt-backed collision world and swept character capsules with stairs,
  slope limits, gravity, jump, floor snap, crouching and stand-up clearance.
- Add symmetric 32-bit collision layers, stable handles and bounded owner-thread APIs.
- Bind exact box/static mesh geometry from DCMO 3 scenes transactionally; expose
  shape/category/mask in the editor through its validated Undo pipeline.
- Verification: Release engine/editor targets compiled; physics regressions passed
  for stairs, slopes, jump, ceilings, sliding, thin-wall sweep, layers, invalid
  settings, capacity and scene-loader rollback. Existing authoring tests passed.
- Limits: static terrain and virtual characters; no rigid-body dynamics or
  collisions between virtual characters. See docs/PHYSICS.md for API bounds.

## [0.5.0-alpha.1+build.6] - 2026-10-01

- Add DCMO 3 quaternion rotation, axis scale and exact affine hierarchy transforms.
  Keep DCMO 2 readable/editable and provide an explicit migration on a new copy.
- Share full transforms between scene placement, picking, bounds, normals, rendering
  and shadows; add editor quaternion/axis controls and world-axis rotation.
- Preserve shear through reparenting; reject singular or invalid transforms.
- Compatibility: rebuild shaders with the changed model-uniform ABI. glTF import
  retains its existing subset. Migration is explicit and leaves originals intact.
- Verification: Release libraries/editor compiled; transform, authoring and resource
  tests passed; Metal GPU shader tests passed; migration copy/diff guards checked.

## [0.4.0-alpha.1+build.5] - 2026-09-25

- Add host-configured Play/Stop, scene start-point selection and a bounded Console.
  Prepare unsaved scenes and media in an isolated temporary snapshot on a worker;
  launch the trusted host executable with structured arguments and no shell.
- Add preparation cancellation, launch/exit diagnostics, cooperative Stop with
  terminate/kill deadlines, repeat protection and asynchronous close/cleanup.
  Keep authoring edits/Undo separate from runtime state and temporary persistence.
- Add opt-in exactAssetRoot to runtime configuration, preventing Play snapshots
  from silently falling back to installed/source content. Existing hosts retain
  asset discovery; standalone editor Play is disabled without a configured host.
- Verification: macOS host editor/runtime and standalone editor targets compiled;
  isolated Qt/GPU workspace tests passed with Boxes and host project copies, including
  unsaved Play, selected spawn, Stop and close with a test-only child process.
  Play lifecycle tests passed for repeat, cancellation, failures, external changes,
  symlinks, media isolation and forced termination. Metadata/static checks passed.
- Limits: the real game was not launched by owner instruction. Human visual acceptance
  and other platforms remain unverified; no Pause, live reload or embedded Game view.
  Snapshots reject symlinks and cap copied content at 4 GiB / 100,000 files.

## [0.3.0-alpha.1+build.4] - 2026-09-25

- Add a Project browser with actual folders, grouped resource/file categories,
  combined search and type filters, folder reveal, scene navigation and an explicit
  floating-window toggle. Distinguish placeable definitions from source files.
- Default the entire editor interface to English; preserve authored project text.
- Verification: macOS editor and workspace targets built; isolated native Qt/GPU
  workspace tests passed with Boxes and the host project, including folder/category/
  search filters, detach/reattach, resource placement, save/reopen and resize.
  Version and numeric ownership checks passed.
- Limits: no import/reimport, thumbnails or file operations beyond folder reveal.
  The asynchronous index ignores symlinks and reports a 100,000-entry cap.
  Human visual acceptance and Windows/Linux remain unverified.

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
