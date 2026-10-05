#pragma once
#include "paper/core/math3d.hpp"
#include "paper/render/draw_types.hpp"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace paper::ui {
enum class Kind { Panel, Label, Button, Toggle, Slider, List };
enum class Direction { Row, Column };
enum class Align { Start, Center, End, Stretch };
struct Dimension {
    float minimum = 0, preferred = 0, maximum = std::numeric_limits<float>::infinity(), grow = 0,
          shrink = 1;
};
struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;
};
struct Layout {
    Dimension width, height;
    Insets padding;
    float gap = 0;
    Direction direction = Direction::Column;
    Align align = Align::Stretch;
    bool scroll = false;
};
struct Node {
    std::string id, text, tooltip;
    Kind kind = Kind::Panel;
    Layout layout;
    bool enabled = true, visible = true, checked = false;
    double value = 0, minimum = 0, maximum = 1, step = .01;
    std::vector<std::string> items;
    std::vector<Node> children;
};
struct Theme {
    Color background{35, 35, 38}, surface{55, 55, 60}, hover{75, 75, 82}, pressed{90, 90, 100},
        accent{115, 175, 240}, text{235, 235, 238}, disabled{130, 130, 135}, focus{245, 200, 90};
    float fontPixels = 18, lineHeight = 24, inset = 6, thumbPixels = 12, scrollbarPixels = 6,
          tooltipDelaySeconds = .6f;
};
enum class InputType { Move, Down, Up, Wheel, Key, Cancel, FocusLost };
enum class Key { Tab, Enter, Space, Left, Right, Up, Down, Home, End, Escape };
struct Input {
    InputType type;
    Vec2 position;
    float wheel = 0;
    Key key = Key::Tab;
    bool shift = false, repeat = false;
};
enum class ActionType { Activate, Value, Toggle, Selection };
struct Action {
    std::string id;
    ActionType type;
    double value = 0;
};
struct Box {
    Rect bounds, clip;
    std::string parent;
};
struct Draw {
    enum class Type { Fill, Outline, Text };
    Type type;
    Rect bounds, clip;
    Color color;
    std::string text;
    float fontPixels = 0;
};
// Logical pixel units. The host supplies exact text measurement and input in the same units.
class Context {
  public:
    using Measure = std::function<float(std::string_view, float)>;
    explicit Context(Measure measure, Theme theme = {});
    void setTree(Node root);
    void setValue(std::string_view id, double value);
    void setChecked(std::string_view id, bool checked);
    void layout(Rect viewport);
    [[nodiscard]] std::vector<Action> input(std::span<const Input> events);
    void advance(float seconds);
    [[nodiscard]] std::vector<Draw> draw() const;
    [[nodiscard]] const Box& box(std::string_view id) const;
    [[nodiscard]] const Node& node(std::string_view id) const;
    [[nodiscard]] const std::string& focus() const noexcept { return focus_; }
    [[nodiscard]] const std::string& capture() const noexcept { return capture_; }

  private:
    void index(Node& node, size_t depth, std::map<std::string, Node*, std::less<>>& candidate);
    void arrange(const Node& node, Rect bounds, Rect clip, const std::string& parent, bool enabled);
    void cancel();
    std::string hit(Vec2 point) const;
    void ensureVisible(std::string_view id);
    void focusNext(bool backwards);
    void activate(Node& node, std::vector<Action>& result);
    void slider(Node& node, Vec2 pointer, std::vector<Action>& result);
    Vec2 preferred(const Node& node,
                   float availableWidth = std::numeric_limits<float>::infinity()) const;
    float measured(std::string_view text) const;
    Measure measure_;
    Theme theme_;
    std::unique_ptr<Node> root_;
    Rect viewport_{};
    std::map<std::string, Node*, std::less<>> nodes_;
    std::map<std::string, Box, std::less<>> boxes_;
    std::map<std::string, bool, std::less<>> enabled_;
    std::map<std::string, float, std::less<>> scroll_, extent_;
    std::vector<std::string> order_, tabOrder_;
    std::string focus_, capture_, hover_;
    Vec2 pointer_{};
    float hoverSeconds_ = 0, sliderGrabOffset_ = 0;
    bool laidOut_ = false;
};
} // namespace paper::ui
