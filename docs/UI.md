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
release over the captured control. Sliders keep dragging outside their bounds.
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
