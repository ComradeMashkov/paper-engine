#include "paper/ui/document.hpp"
#include "paper/ui/scroll_area.hpp"
#include "paper/ui/ui.hpp"
#include <iostream>
using namespace paper;
using namespace paper::ui;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        if (!ok) {
            ++failures;
            std::cerr << text << '\n';
        }
    };
    auto rejects = [](auto f) {
        try {
            f();
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    Context context(
        [](std::string_view text, float) { return static_cast<float>(text.size()) * 6; });
    Node root;
    root.id = "root";
    root.layout.direction = Direction::Column;
    root.layout.gap = 4;
    Node button;
    button.id = "button";
    button.kind = Kind::Button;
    button.text = "Run";
    button.tooltip = "Begin operation";
    button.layout.height.preferred = 40;
    Node toggle = button;
    toggle.id = "toggle";
    toggle.kind = Kind::Toggle;
    Node slider = button;
    slider.id = "slider";
    slider.kind = Kind::Slider;
    slider.minimum = 10;
    slider.maximum = 20;
    slider.value = 10;
    slider.step = .5;
    root.children = {button, toggle, slider};
    context.setTree(root);
    context.layout({0, 0, 200, 128});
    check(context.box("button").bounds.h == 40 && context.box("slider").bounds.y == 88,
          "column layout respects gaps and sizes");
    const auto send = [&](std::initializer_list<paper::ui::Input> events) {
        return context.input(std::span(events.begin(), events.size()));
    };
    check(send({{InputType::Down, {20, 20}}, {InputType::Up, {20, 20}}}).size() == 1,
          "pointer activation produces one action");
    check(
        send(
            {{InputType::Down, {20, 20}}, {InputType::Move, {250, 20}}, {InputType::Up, {250, 20}}})
            .empty(),
        "release outside captured button does not activate");
    (void)send({{InputType::Down, {20, 20}}, {InputType::FocusLost, {}}});
    check(context.capture().empty() && context.focus().empty(),
          "focus loss releases capture and keyboard focus");
    (void)send({{InputType::Down, {20, 60}}, {InputType::Up, {20, 60}}});
    check(context.node("toggle").checked, "toggle pointer interaction changes checked state");
    (void)send({{InputType::Key, {}, 0, Key::Space, false, true}});
    check(context.node("toggle").checked, "key repeat does not repeatedly toggle");
    (void)send({{InputType::Key, {}, 0, Key::Tab}});
    check(context.focus() == "slider", "Tab follows enabled control order");
    (void)send({{InputType::Key, {}, 0, Key::End}});
    check(context.node("slider").value == 20, "keyboard End reaches exact slider endpoint");
    (void)send({{InputType::Key, {}, 0, Key::Left}});
    check(context.node("slider").value == 19.5, "keyboard uses authored slider step");
    (void)send({{InputType::Down, {100, 108}}, {InputType::Move, {-100, 108}}});
    check(context.node("slider").value == 10 && !context.capture().empty(),
          "slider captures outside viewport and clamps value");
    context.layout({0, 0, 100, 128});
    check(context.capture().empty(), "resize cancels stale drag");
    {
        Context drag([](auto, float) { return 0.f; });
        Node control;
        control.id = "drag";
        control.kind = Kind::Slider;
        control.value = .5;
        control.step = .01;
        drag.setTree(control);
        drag.layout({0, 0, 200, 40});
        const std::array press{Input{InputType::Down, {105, 20}}};
        check(drag.input(press).empty() && drag.node("drag").value == .5,
              "grabbing a thumb off centre does not jump its value");
        const std::array release{Input{InputType::Up, {181, 20}}};
        const auto actions = drag.input(release);
        check(actions.size() == 1 && std::abs(actions[0].value - .93) < .001 &&
                  drag.capture().empty(),
              "release applies the final pointer with the original grab offset");
        (void)drag.input(press);
        const std::array cancel{Input{InputType::Cancel, {}}};
        (void)drag.input(cancel);
        const std::array outside{Input{InputType::Up, {500, 20}}};
        check(drag.input(outside).empty(), "cancel discards an unfinished slider drag");
    }
    root.layout.direction = Direction::Row;
    root.children = {button, toggle};
    for (auto& n : root.children) {
        n.layout.width = {30, 40, 100, 1, 1};
    }
    context.setTree(root);
    context.layout({0, 0, 144, 40});
    check(context.box("button").bounds.w == 70 && context.box("toggle").bounds.x == 74,
          "flex grow distributes available space");
    context.layout({0, 0, 44, 40});
    check(context.box("button").bounds.w == 30 && context.box("toggle").clip.w == 10,
          "minimum sizes overflow through clipping instead of overlap");
    Node list;
    list.id = "list";
    list.kind = Kind::List;
    list.value = 0;
    list.items = {"zero", "one", "two", "three", "four", "five"};
    list.layout.height.preferred = 60;
    root.children = {list};
    root.layout.direction = Direction::Column;
    context.setTree(root);
    context.layout({0, 0, 120, 60});
    (void)send({{InputType::Down, {20, 20}},
                {InputType::Up, {20, 20}},
                {InputType::Key, {}, 0, Key::End}});
    check(context.node("list").value == 5, "list keyboard navigation selects last row");
    const auto draw = context.draw();
    check(std::ranges::any_of(draw, [](const Draw& d) { return d.text == "five"; }),
          "list scroll reveals selected row");
    auto bad = root;
    bad.children.push_back(list);
    check(rejects([&] { context.setTree(bad); }),
          "duplicate tree IDs fail before replacing active tree");
    check(context.node("list").value == 5, "failed tree update preserves active state");
    bad = root;
    bad.children[0].layout.width.minimum = -1;
    check(rejects([&] { context.setTree(bad); }), "negative layout constraints rejected");
    root.children = {button};
    context.setTree(root);
    context.layout({0, 0, 200, 60});
    (void)send({{InputType::Move, {20, 20}}});
    context.advance(1);
    check(std::ranges::any_of(context.draw(),
                              [](const Draw& d) { return d.text == "Begin operation"; }),
          "tooltip obeys hover delay and renderer contract");
    button.layout.height.preferred = 0;
    button.text = "one two three four";
    root.children = {button};
    root.layout.scroll = true;
    context.setTree(root);
    context.layout({0, 0, 60, 100});
    check(context.box("button").bounds.h > 60, "automatic height accounts for wrapped text");
    button.layout.height.preferred = 40;
    button.text = "Run";
    toggle.enabled = false;
    root.children = {button, toggle, slider};
    context.setTree(root);
    context.layout({0, 0, 200, 40});
    (void)send({{InputType::Key, {}, 0, Key::Tab}, {InputType::Key, {}, 0, Key::Tab}});
    check(context.focus() == "slider" && context.box("slider").clip.h > 0,
          "Tab skips disabled controls and scrolls an ancestor to reveal focus");
    root.children = {button, toggle};
    root.layout.scroll = false;
    root.layout.direction = Direction::Row;
    root.layout.gap = 0;
    root.children[0].layout.width = {0, 10, 20, 1, 1};
    root.children[1].layout.width = {0, 10, 200, 1, 1};
    context.setTree(root);
    context.layout({0, 0, 100, 40});
    check(context.box("button").bounds.w == 20 && context.box("toggle").bounds.w == 80,
          "flex allocation redistributes space after a child reaches maximum");
    Context invalidMetrics([](std::string_view, float) { return -1.f; });
    check(rejects([&] { invalidMetrics.setTree(button); }),
          "negative text metrics rejected before publishing a tree");
    bool metricsValid = true;
    Context atomicLayout([&](std::string_view text, float) {
        return metricsValid ? static_cast<float>(text.size()) * 6 : -1.f;
    });
    atomicLayout.setTree(button);
    atomicLayout.layout({0, 0, 100, 40});
    metricsValid = false;
    check(rejects([&] { atomicLayout.layout({0, 0, 200, 40}); }) &&
              atomicLayout.box("button").bounds.w == 100,
          "failed relayout preserves previously published geometry");
    metricsValid = true;
    button.visible = false;
    atomicLayout.setTree(button);
    const std::array<paper::ui::Input, 1> emptyInput{{{InputType::Key, {}, 0, Key::Enter}}};
    check(atomicLayout.draw().empty() && atomicLayout.input(emptyInput).empty(),
          "hidden root accepts input without stale focus or draw commands");
    {
        Document authored;
        authored.viewport = {400, 300};
        authored.root.id = "canvas";
        authored.root.layout.padding = {10, 20, 0, 0};
        Node region;
        region.id = "preview";
        region.kind = Kind::Region;
        region.layout.bounds = Rect{30, 40, 100, 80};
        region.properties = {{"fontPixels", 25}, {"resource", "illustration.png"}};
        Node action = button;
        action.visible = true;
        action.id = "authored.action";
        action.layout.bounds = Rect{30, 40, 100, 80};
        authored.root.children = {action, region};
        auto restored = parseDocument(writeDocument(authored));
        check(documentValue(restored) == documentValue(authored),
              "Absolute regions and owning host properties survive native round-trip");
        context.setTree(restored.root);
        context.layout({0, 0, 400, 300});
        const auto b = context.box("preview").bounds;
        check(b.x == 40 && b.y == 60 && b.w == 100 && b.h == 80,
              "Authored coordinates are relative to padded parent bounds");
        const auto activation = send({{InputType::Down, {50, 70}}, {InputType::Up, {50, 70}}});
        check(activation.size() == 1 && activation.front().id == "authored.action",
              "Host preview regions do not intercept shared buttons");
        (void)send({{InputType::Down, {50, 70}}});
        restored.root.children[0].layout.bounds->x += 1;
        context.setTree(restored.root);
        check(context.capture().empty() && send({{InputType::Up, {50, 70}}}).empty(),
              "Replacing authored geometry cancels a stale release");
        restored.root.children[1].layout.bounds->w = -1;
        check(rejects([&] { (void)writeDocument(restored); }), "Reject negative authored extents");
        ScrollArea scroll;
        scroll.layout({20, 30, 100, 80}, 300, "paragraph");
        check(scroll.input({InputType::Wheel, {30, 40}, 50}) && scroll.offset() == 50,
              "Host scrolling uses logical pixels within its authored region");
        scroll.layout({20, 30, 100, 80}, 400, "paragraph");
        check(scroll.offset() == 50, "Reflow and content reload preserve the scroll position");
        check(!scroll.input({InputType::Wheel, {500, 500}, 40}),
              "Wheel outside a region cannot change its scroll position");
        check(scroll.input({InputType::Key, {}, 0, Key::PageDown}, true) && scroll.offset() == 130,
              "Keyboard paging reveals complete long content");
        scroll.layout({20, 30, 100, 80}, 100, "paragraph");
        check(scroll.offset() == 20, "Shorter translations clamp the existing scroll position");
        scroll.layout({20, 30, 100, 80}, 400, "another target");
        check(scroll.offset() == 0, "A different target does not inherit old scrolling");
        scroll.reveal({20, 230, 100, 20});
        check(scroll.offset() == 140, "Keyboard selection can reveal a clipped dynamic action");
        check(rejects([&] { scroll.layout({0, 0, 0, 80}, 300, "invalid"); }) &&
                  scroll.offset() == 140,
              "Rejected scroll layout preserves published state");
    }
    std::cout << "UI failures=" << failures << '\n';
    return failures ? 1 : 0;
}
