# Shared UI and layout

`Paper::UI` is independent of SDL, Qt and any host game. It provides panels,
labels, buttons, toggles, sliders, lists, tooltips and a shared theme. Stable
string IDs identify nodes and returned actions. A host supplies exact text
measurement in logical pixels; drawing and input use the same coordinates.

`Context::setTree` validates IDs, enums, finite constraints and widget ranges
before publishing a replacement. Changes to the tree cancel pointer capture;
keyboard focus survives only on an enabled interactive node with the same ID.
`layout(viewport)` publishes geometry transactionally and cancels stale drags.
Call layout when content or the viewport changes. Failed tree/layout preparation
preserves the previously published state. A hidden root produces no actions or
drawing. Limits are 4096 nodes, 64 levels and 100,000 rows per list.

Panels choose row/column layout, padding, gap and cross-axis alignment. Dimensions
have minimum/preferred/maximum sizes and grow/shrink weights. Distribution
redistributes remaining space when an item reaches a bound. Minimum-size overflow
is clipped instead of overlapping. Scrollable panels retain content sizes; wheel
and keyboard focus reveal content. Lists draw only visible rows. Scroll indicators
show position; scrolling is controlled through wheel and keyboard input.

Labels/buttons/toggles wrap UTF-8 text using the host's actual font measurement.
Automatic column height includes wrapping; an explicit preferred height remains
an authored constraint. Text and nested content respect intersected clip bounds.
Sliders clamp/quantize pointer and keyboard values, including exact endpoints.
Lists return selection actions. `setValue` and `setChecked` update values without
rebuilding a tree; the host owns persistence and reactions to actions.

Pointer capture lasts until release/cancel/focus loss. Buttons activate only on
release over the captured control. Sliders keep dragging outside their bounds,
preserve off-centre thumb grabs and apply the final release coordinate.
Tab/Shift-Tab navigate enabled controls; Enter/Space activate buttons/toggles.
Arrow/Home/End keys adjust sliders and lists. Repeated discrete activation is
suppressed. Escape, resize, debug input capture and lost focus cancel dragging.

`Paper::UIRender` connects the generic layer to `Engine`:

```cpp
paper::ui::Context ui(paper::ui::engineMeasure(engine));
paper::ui::Node panel;
panel.id = "settings";
panel.layout.gap = 8;
paper::ui::Node sound;
sound.id = "sound";
sound.kind = paper::ui::Kind::Toggle;
sound.text = "Sound";
sound.checked = true;
panel.children.push_back(sound);
ui.setTree(std::move(panel));
ui.layout({0, 0, 400, 200});
// In the host frame:
auto events = paper::ui::engineInput(engine.input());
auto actions = ui.input(events);
ui.advance(elapsedSeconds);
paper::ui::paint(engine, ui.draw());
```

The adapter preserves ordered pointer/key edges, Shift and key repeat. SDL wheel
units map to configurable logical pixels. The renderer intersects each command
with the host's existing clip and restores that clip on exit, including errors.
Engine input retains aggregate relative camera motion while exposing ordered
motion/wheel edges. Host game menus are migrated by the host, not by this library.

## Assets and visual authoring

`paper/ui/document.hpp` provides `Document`, `parseDocument`, `loadDocument`,
`readDocument`, `documentValue` and `writeDocument`. `.pui` is TOML `paper.ui`
version 1. `[ui]` declares `root`, `viewport` (logical width/height), optional
`theme`, and ordered `[[ui.nodes]]` records. IDs are unique; `parent` references a
panel. Exactly one root has an empty/omitted parent. Missing parents, cycles,
unknown keys, malformed values and unsupported versions fail before publication.

Nodes support `panel`, `label`, `button`, `toggle`, `slider`, `list`; text, tooltip,
enabled/visible/checked, value/range/step, items and row/column layout. `width` and
`height` tables declare minimum/preferred/maximum/grow/shrink; `maximum = "unlimited"`
is explicit. Padding order is left/top/right/bottom. Theme colors use RGBA bytes.
Assets cap at 4 MiB, 4096 nodes, 64 hierarchy levels, 100,000 list items, an 8192-pixel
viewport axis and 128-pixel fonts. `examples/boxes/assets/settings.pui` is a complete
six-kind example. Hosts load the tree/theme and choose their current viewport:

```cpp
auto screen = paper::ui::loadDocument(assets / "settings.pui");
paper::ui::Context ui(paper::ui::engineMeasure(engine), screen.theme);
ui.setTree(std::move(screen.root));
ui.layout({0, 0, canvasWidth, canvasHeight});
```

File → New UI / Edit UI and Project → double-click `.pui` open a nonmodal design
window. The palette adds components; hierarchy, Inspector, Duplicate/Delete and
Up/Down edit the tree. Parent choices reparent panels and controls; validation
rejects cycles and children on leaf controls. Renaming an ID updates child parents
and the root reference. Theme / Viewport edits appearance and preview size.

The preview draws the shared `Context` commands with Qt font measurement. Clicking
in Design selects a visible component; Interact routes pointer, wheel and keyboard
input into that same context. Preview actions/slider/toggle/list changes are isolated
from authored values. Runtime uses the host's configured font and action handlers.

Apply Properties commits one Undo step. Pending properties apply on selection
change, Source switch, Save or Play; invalid drafts remain editable. Source and structure
share Undo/Redo across Save. Structured changes use canonical TOML; source edits and
no-op saves preserve their text/comments. Save/Ctrl+S validates actual preview
layout, checks external changes and atomically replaces the file. Close offers
Save/Discard/Cancel. Play includes the open document's validated draft in its
isolated asset copy. The preview is authoring data, not host-game execution.
