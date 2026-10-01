#include "paper/ui/ui.hpp"
#include "paper/core/text_layout.hpp"
#include <iomanip>
#include <set>
#include <sstream>
namespace paper::ui {
namespace {
constexpr size_t maximumNodes = 4096, maximumDepth = 64, maximumListItems = 100000;
constexpr int displayDigits = 6;
constexpr float insetSides = 2, centreFraction = .5f;
Rect intersect(Rect a, Rect b) {
    const float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.f, std::min(a.x + a.w, b.x + b.w) - x),
            std::max(0.f, std::min(a.y + a.h, b.y + b.h) - y)};
}
bool rectangle(Rect r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) &&
           r.w >= 0 && r.h >= 0 && std::isfinite(r.x + r.w) && std::isfinite(r.y + r.h);
}
bool interactive(Kind kind) {
    return kind == Kind::Button || kind == Kind::Toggle || kind == Kind::Slider ||
           kind == Kind::List;
}
void require(bool ok, const char* message) {
    if (!ok)
        throw std::invalid_argument(message);
}
float bounded(float value, const Dimension& dimension) {
    return std::clamp(value, dimension.minimum, dimension.maximum);
}
std::string valueText(double value) {
    std::ostringstream s;
    s << std::setprecision(displayDigits) << value;
    return s.str();
}
} // namespace
Context::Context(Measure measure, Theme theme) : measure_(std::move(measure)), theme_(theme) {
    require(bool(measure_), "UI requires exact host text measurement");
    for (float v : {theme.fontPixels, theme.lineHeight, theme.thumbPixels, theme.scrollbarPixels})
        require(std::isfinite(v) && v > 0, "Invalid UI typography/geometry");
    require(std::isfinite(theme.inset) && theme.inset >= 0 &&
                std::isfinite(theme.tooltipDelaySeconds) && theme.tooltipDelaySeconds >= 0,
            "Invalid UI theme");
}
void Context::index(Node& n, size_t depth, std::map<std::string, Node*, std::less<>>& candidate) {
    require(depth < maximumDepth && candidate.size() < maximumNodes && !n.id.empty() &&
                candidate.emplace(n.id, &n).second,
            "Invalid/duplicate UI ID or tree budget");
    for (const auto d : {n.layout.width, n.layout.height})
        require(std::isfinite(d.minimum) && d.minimum >= 0 && std::isfinite(d.preferred) &&
                    d.preferred >= 0 && !std::isnan(d.maximum) && d.maximum >= d.minimum &&
                    std::isfinite(d.grow) && d.grow >= 0 && std::isfinite(d.shrink) &&
                    d.shrink >= 0,
                "Invalid UI size constraints");
    for (float v : {n.layout.padding.left, n.layout.padding.top, n.layout.padding.right,
                    n.layout.padding.bottom, n.layout.gap})
        require(std::isfinite(v) && v >= 0, "Invalid UI spacing");
    require(n.kind >= Kind::Panel && n.kind <= Kind::List && n.layout.direction >= Direction::Row &&
                n.layout.direction <= Direction::Column && n.layout.align >= Align::Start &&
                n.layout.align <= Align::Stretch,
            "Invalid UI enum");
    require(n.items.size() <= maximumListItems, "UI list exceeds item budget");
    require(n.kind == Kind::Panel || n.children.empty(), "Only panels have authored children");
    if (n.kind == Kind::Slider)
        require(std::isfinite(n.minimum) && std::isfinite(n.maximum) && std::isfinite(n.value) &&
                    std::isfinite(n.step) && n.maximum > n.minimum && n.step > 0 &&
                    n.value >= n.minimum && n.value <= n.maximum &&
                    std::isfinite(n.maximum - n.minimum),
                "Invalid slider range/value");
    if (n.kind == Kind::List)
        require(std::isfinite(n.value) && n.value == std::floor(n.value) && n.value >= -1 &&
                    (n.value < static_cast<double>(n.items.size()) ||
                     (n.items.empty() && n.value == -1)),
                "Invalid list selection");
    for (auto& child : n.children)
        index(child, depth + 1, candidate);
}
void Context::setTree(Node root) {
    Context candidate(measure_, theme_);
    candidate.root_ = std::make_unique<Node>(std::move(root));
    candidate.index(*candidate.root_, 0, candidate.nodes_);
    (void)candidate.preferred(*candidate.root_);
    candidate.focus_ = focus_;
    candidate.scroll_ = scroll_;
    candidate.pointer_ = pointer_;
    candidate.viewport_ = viewport_;
    std::erase_if(candidate.scroll_,
                  [&](const auto& entry) { return !candidate.nodes_.contains(entry.first); });
    if (!candidate.nodes_.contains(candidate.focus_))
        candidate.focus_.clear();
    if (laidOut_)
        candidate.layout(viewport_);
    *this = std::move(candidate);
}
const Node& Context::node(std::string_view id) const {
    return *nodes_.at(std::string(id));
}
const Box& Context::box(std::string_view id) const {
    return boxes_.at(std::string(id));
}
void Context::setValue(std::string_view id, double value) {
    auto& n = *nodes_.at(std::string(id));
    require(std::isfinite(value), "Non-finite UI value");
    if (n.kind == Kind::Slider) {
        require(value >= n.minimum && value <= n.maximum, "Slider value outside range");
        n.value = value;
    } else {
        require(n.kind == Kind::List && value == std::floor(value) && value >= -1 &&
                    value < static_cast<double>(n.items.size()),
                "Invalid list selection");
        n.value = value;
    }
}
void Context::setChecked(std::string_view id, bool checked) {
    auto& n = *nodes_.at(std::string(id));
    require(n.kind == Kind::Toggle, "Expected toggle");
    n.checked = checked;
}
float Context::measured(std::string_view text) const {
    const float width = measure_(text, theme_.fontPixels);
    require(std::isfinite(width) && width >= 0, "Invalid font measurement");
    return width;
}
Vec2 Context::preferred(const Node& n, float availableWidth) const {
    float w = 0, h = 0;
    if (n.kind == Kind::Panel) {
        size_t visible = 0;
        for (const auto& child : n.children)
            if (child.visible) {
                const auto size = preferred(child);
                if (n.layout.direction == Direction::Row) {
                    w += size.x;
                    h = std::max(h, size.y);
                } else {
                    h += size.y;
                    w = std::max(w, size.x);
                }
                ++visible;
            }
        if (visible > 0) {
            if (n.layout.direction == Direction::Row)
                w += n.layout.gap * (visible - 1);
            else
                h += n.layout.gap * (visible - 1);
        }
    } else {
        w = measured(n.text) + insetSides * theme_.inset;
        h = theme_.lineHeight + insetSides * theme_.inset;
        if (n.kind == Kind::Toggle)
            w += theme_.lineHeight;
        if (n.kind != Kind::List && std::isfinite(availableWidth) &&
            n.layout.height.preferred == 0) {
            const float width =
                std::max(0.f, bounded(availableWidth, n.layout.width) - insetSides * theme_.inset -
                                  n.layout.padding.left - n.layout.padding.right -
                                  (n.kind == Kind::Toggle ? theme_.lineHeight + theme_.inset : 0));
            if (width > 0)
                h = std::max(size_t{1},
                             wrapText(n.text, width,
                                      [&](std::string_view text) { return measured(text); })
                                 .size()) *
                        theme_.lineHeight +
                    insetSides * theme_.inset;
        }
        if (n.kind == Kind::List) {
            h = std::max(1.f, static_cast<float>(n.items.size())) * theme_.lineHeight +
                insetSides * theme_.inset;
            for (const auto& item : n.items)
                w = std::max(w, measured(item) + insetSides * theme_.inset);
        }
    }
    require(std::isfinite(w) && std::isfinite(h) && w >= 0 && h >= 0,
            "Invalid text metrics/layout extent"); // numbers: inset on both sides of a widget.
    w += n.layout.padding.left + n.layout.padding.right;
    h += n.layout.padding.top + n.layout.padding.bottom;
    return {
        bounded(n.layout.width.preferred > 0 ? n.layout.width.preferred : w, n.layout.width),
        bounded(n.layout.height.preferred > 0 ? n.layout.height.preferred : h, n.layout.height)};
}
void Context::cancel() {
    capture_.clear();
}
void Context::layout(Rect viewport) {
    require(root_ && rectangle(viewport), "Invalid UI viewport/tree");
    Context prepared(measure_, theme_);
    prepared.nodes_ = nodes_;
    prepared.scroll_ = scroll_;
    prepared.focus_ = focus_;
    (void)prepared.preferred(*root_, viewport.w);
    prepared.arrange(*root_, viewport, viewport, "", true);
    if (!prepared.enabled_.contains(prepared.focus_) || !prepared.enabled_.at(prepared.focus_) ||
        !interactive(prepared.nodes_.at(prepared.focus_)->kind))
        prepared.focus_.clear();
    const auto hover = prepared.hit(pointer_);
    if (hover_ != hover)
        hoverSeconds_ = 0;
    hover_ = hover;
    viewport_ = viewport;
    boxes_ = std::move(prepared.boxes_);
    enabled_ = std::move(prepared.enabled_);
    scroll_ = std::move(prepared.scroll_);
    extent_ = std::move(prepared.extent_);
    order_ = std::move(prepared.order_);
    tabOrder_ = std::move(prepared.tabOrder_);
    focus_ = std::move(prepared.focus_);
    cancel();
    laidOut_ = true;
}
void Context::arrange(const Node& n, Rect bounds, Rect clip, const std::string& parent,
                      bool parentEnabled) {
    if (!n.visible)
        return;
    require(rectangle(bounds), "Layout arithmetic overflow");
    clip = intersect(clip, bounds);
    boxes_[n.id] = {bounds, clip, parent};
    order_.push_back(n.id);
    enabled_[n.id] = parentEnabled && n.enabled;
    if (interactive(n.kind) && enabled_[n.id])
        tabOrder_.push_back(n.id);
    if (n.kind == Kind::List) {
        extent_[n.id] = std::max(0.f, static_cast<float>(n.items.size()) * theme_.lineHeight -
                                          (bounds.h - insetSides * theme_.inset));
        scroll_[n.id] = std::clamp(scroll_[n.id], 0.f, extent_[n.id]);
    } // numbers: inset on both ends of list viewport.
    if (n.kind != Kind::Panel)
        return;
    const auto& l = n.layout;
    const bool row = l.direction == Direction::Row;
    Rect area{bounds.x + l.padding.left, bounds.y + l.padding.top,
              std::max(0.f, bounds.w - l.padding.left - l.padding.right),
              std::max(0.f, bounds.h - l.padding.top - l.padding.bottom)};
    clip = intersect(clip, area);
    std::vector<const Node*> children;
    std::vector<float> sizes;
    float used = 0;
    for (auto& child : n.children)
        if (child.visible) {
            children.push_back(&child);
            const auto size =
                preferred(child, row ? std::numeric_limits<float>::infinity() : area.w);
            sizes.push_back(row ? size.x : size.y);
            used += sizes.back();
        }
    if (!children.empty())
        used += l.gap * (children.size() - 1);
    const float available = row ? area.w : area.h;
    float remainder = available - used;
    if (!l.scroll)
        for (size_t pass = 0;
             pass < children.size() && std::abs(remainder) > geometryTolerance::normalizedLength;
             ++pass) {
            float weights = 0;
            for (size_t k = 0; k < children.size(); ++k) {
                const auto d = row ? children[k]->layout.width : children[k]->layout.height;
                if (remainder > 0 ? sizes[k] < d.maximum : sizes[k] > d.minimum)
                    weights += remainder > 0 ? d.grow : d.shrink * sizes[k];
            }
            if (weights <= 0)
                break;
            float taken = 0;
            for (size_t k = 0; k < children.size(); ++k) {
                const auto d = row ? children[k]->layout.width : children[k]->layout.height;
                if (remainder > 0 ? sizes[k] >= d.maximum : sizes[k] <= d.minimum)
                    continue;
                const auto weight = remainder > 0 ? d.grow : d.shrink * sizes[k];
                const auto next = bounded(sizes[k] + remainder * weight / weights, d);
                taken += next - sizes[k];
                sizes[k] = next;
            }
            remainder -= taken;
        }
    used = children.empty() ? 0 : l.gap * (children.size() - 1);
    for (float size : sizes)
        used += size;
    extent_[n.id] = l.scroll ? std::max(0.f, used - available) : 0;
    scroll_[n.id] = std::clamp(scroll_[n.id], 0.f, extent_[n.id]);
    float at = (row ? area.x : area.y) - scroll_[n.id];
    for (size_t k = 0; k < children.size(); ++k) {
        auto& child = *children[k];
        const auto size = preferred(child, row ? sizes[k] : area.w);
        const auto d = row ? child.layout.height : child.layout.width;
        const float crossAvailable = row ? area.h : area.w;
        const float cross =
            bounded(l.align == Align::Stretch ? crossAvailable : (row ? size.y : size.x), d);
        float offset = 0;
        if (l.align == Align::Center)
            offset = (crossAvailable - cross) * centreFraction;
        else if (l.align == Align::End)
            offset = crossAvailable -
                     cross; // numbers: center alignment halves the remaining cross-axis space.
        Rect childBounds = row ? Rect{at, area.y + offset, sizes[k], cross}
                               : Rect{area.x + offset, at, cross, sizes[k]};
        arrange(child, childBounds, clip, n.id, enabled_[n.id]);
        at += sizes[k] + l.gap;
    }
}
std::string Context::hit(Vec2 point) const {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        const auto& b = boxes_.at(*it);
        if (b.clip.w > 0 && b.clip.h > 0 && b.clip.contains(point.x, point.y) &&
            b.bounds.contains(point.x, point.y))
            return *it;
    }
    return {};
}
void Context::ensureVisible(std::string_view id) {
    auto child = std::string(id);
    while (boxes_.contains(child)) {
        const auto current = boxes_.at(child);
        const auto parent = current.parent;
        if (parent.empty())
            break;
        auto& n = *nodes_.at(parent);
        if (n.layout.scroll) {
            const auto b = boxes_.at(parent).bounds;
            const bool row = n.layout.direction == Direction::Row;
            const float start = row ? current.bounds.x : current.bounds.y,
                        end = start + (row ? current.bounds.w : current.bounds.h),
                        low = (row ? b.x : b.y) +
                              (row ? n.layout.padding.left : n.layout.padding.top),
                        high = (row ? b.x + b.w : b.y + b.h) -
                               (row ? n.layout.padding.right : n.layout.padding.bottom);
            if (start < low)
                scroll_[parent] += start - low;
            else if (end > high)
                scroll_[parent] += end - high;
            layout(viewport_);
        }
        child = parent;
    }
}
void Context::focusNext(bool backwards) {
    if (tabOrder_.empty()) {
        focus_.clear();
        return;
    }
    auto it = std::ranges::find(tabOrder_, focus_);
    size_t index = it == tabOrder_.end() ? (backwards ? tabOrder_.size() - 1 : 0)
                                         : static_cast<size_t>(it - tabOrder_.begin());
    if (it != tabOrder_.end())
        index = backwards ? (index + tabOrder_.size() - 1) % tabOrder_.size()
                          : (index + 1) % tabOrder_.size();
    focus_ = tabOrder_[index];
    ensureVisible(focus_);
}
void Context::activate(Node& n, std::vector<Action>& result) {
    if (n.kind == Kind::Toggle) {
        n.checked = !n.checked;
        result.push_back({n.id, ActionType::Toggle, n.checked ? 1.0 : 0.0});
    } else if (n.kind == Kind::Button)
        result.push_back({n.id, ActionType::Activate});
}
void Context::slider(Node& n, Vec2 pointer, std::vector<Action>& result) {
    const auto b = boxes_.at(n.id).bounds;
    const float width = b.w - insetSides * theme_.inset - theme_.thumbPixels;
    if (width <= 0)
        return; // numbers: inset at both ends of slider.
    const double fraction = std::clamp(
        (pointer.x - b.x - theme_.inset - theme_.thumbPixels * centreFraction) / width, 0.f,
        1.f); // numbers: pointer tracks thumb centre.
    const double value =
        fraction <= 0 ? n.minimum
        : fraction >= 1
            ? n.maximum
            : std::clamp(n.minimum +
                             std::round(fraction * (n.maximum - n.minimum) / n.step) * n.step,
                         n.minimum, n.maximum);
    if (value != n.value) {
        n.value = value;
        result.push_back({n.id, ActionType::Value, value});
    }
}
std::vector<Action> Context::input(std::span<const Input> events) {
    require(root_ && laidOut_, "UI must be laid out before input");
    std::vector<Action> result;
    for (const auto& e : events) {
        require(std::isfinite(e.position.x) && std::isfinite(e.position.y) &&
                    std::isfinite(e.wheel),
                "Non-finite UI input");
        if (e.type == InputType::FocusLost || e.type == InputType::Cancel) {
            cancel();
            if (e.type == InputType::FocusLost) {
                focus_.clear();
                hover_.clear();
                hoverSeconds_ = 0;
            }
            continue;
        }
        if (e.type != InputType::Key) {
            pointer_ = e.position;
            const auto hovered = hit(pointer_);
            if (hover_ != hovered) {
                hover_ = hovered;
                hoverSeconds_ = 0;
            }
        }
        if (e.type == InputType::Down) {
            cancel();
            if (!hover_.empty() && enabled_.at(hover_) && interactive(nodes_.at(hover_)->kind)) {
                focus_ = capture_ = hover_;
                auto& n = *nodes_.at(capture_);
                if (n.kind == Kind::Slider)
                    slider(n, pointer_, result);
            }
        } else if (e.type == InputType::Move && !capture_.empty()) {
            auto& n = *nodes_.at(capture_);
            if (n.kind == Kind::Slider)
                slider(n, pointer_, result);
        } else if (e.type == InputType::Up && !capture_.empty()) {
            const auto id = capture_;
            cancel();
            if (hover_ == id && enabled_.at(id)) {
                auto& n = *nodes_.at(id);
                activate(n, result);
                if (n.kind == Kind::List) {
                    const auto row = static_cast<long long>(std::floor(
                        (pointer_.y - boxes_.at(id).bounds.y - theme_.inset + scroll_[id]) /
                        theme_.lineHeight));
                    if (row >= 0 && static_cast<size_t>(row) < n.items.size() && n.value != row) {
                        n.value = static_cast<double>(row);
                        result.push_back({id, ActionType::Selection, n.value});
                    }
                }
            }
        } else if (e.type == InputType::Wheel) {
            auto id = hover_;
            while (!id.empty()) {
                const auto& n = *nodes_.at(id);
                if (enabled_.at(id) && (n.layout.scroll || n.kind == Kind::List) &&
                    extent_[id] > 0) {
                    scroll_[id] = std::clamp(scroll_[id] + e.wheel, 0.f, extent_[id]);
                    layout(viewport_);
                    break;
                }
                id = boxes_.at(id).parent;
            }
        } else if (e.type == InputType::Key) {
            if (e.key == Key::Escape) {
                cancel();
                continue;
            }
            if (e.key == Key::Tab) {
                cancel();
                focusNext(e.shift);
                continue;
            }
            if (focus_.empty() || !enabled_.at(focus_))
                continue;
            auto& n = *nodes_.at(focus_);
            if ((e.key == Key::Enter || e.key == Key::Space) && !e.repeat)
                activate(n, result);
            if (n.kind == Kind::Slider) {
                double value = n.value;
                if (e.key == Key::Home)
                    value = n.minimum;
                else if (e.key == Key::End)
                    value = n.maximum;
                else if (e.key == Key::Right || e.key == Key::Up)
                    value += n.step;
                else if (e.key == Key::Left || e.key == Key::Down)
                    value -= n.step;
                value = std::clamp(value, n.minimum, n.maximum);
                if (value != n.value) {
                    n.value = value;
                    result.push_back({n.id, ActionType::Value, value});
                }
            }
            if (n.kind == Kind::List && !n.items.empty() &&
                (e.key == Key::Home || e.key == Key::End || e.key == Key::Down ||
                 e.key == Key::Up)) {
                double value = n.value;
                if (e.key == Key::Home)
                    value = 0;
                else if (e.key == Key::End)
                    value = static_cast<double>(n.items.size() - 1);
                else if (e.key == Key::Down)
                    value += 1;
                else if (e.key == Key::Up)
                    value -= 1;
                value = std::clamp(value, 0.0, static_cast<double>(n.items.size() - 1));
                if (value != n.value) {
                    n.value = value;
                    result.push_back({n.id, ActionType::Selection, value});
                }
                const auto b = boxes_.at(n.id).bounds;
                const float low = static_cast<float>(value) * theme_.lineHeight,
                            high = low + theme_.lineHeight,
                            visible = std::max(0.f, b.h - insetSides * theme_.inset);
                if (low < scroll_[n.id])
                    scroll_[n.id] = low;
                else if (high > scroll_[n.id] + visible)
                    scroll_[n.id] = high - visible;
                scroll_[n.id] = std::clamp(scroll_[n.id], 0.f, extent_[n.id]);
            } // numbers: list content excludes inset at both ends.
        }
    }
    return result;
}
void Context::advance(float seconds) {
    require(std::isfinite(seconds) && seconds >= 0, "Invalid UI elapsed time");
    hoverSeconds_ = std::min(theme_.tooltipDelaySeconds, hoverSeconds_ + seconds);
}
std::vector<Draw> Context::draw() const {
    std::vector<Draw> result;
    const auto add = [&](Draw::Type type, Rect bounds, Rect clip, Color color,
                         std::string text = {}, float font = 0) {
        if (clip.w > 0 && clip.h > 0)
            result.push_back({type, bounds, clip, color, std::move(text), font});
    };
    for (const auto& id : order_) {
        const auto& n = *nodes_.at(id);
        const auto& box = boxes_.at(id);
        const auto b = box.bounds;
        const bool enabled = enabled_.at(id);
        const auto foreground = enabled ? theme_.text : theme_.disabled;
        if (n.kind != Kind::Label) {
            const auto color = enabled && capture_ == id ? theme_.pressed
                               : enabled && hover_ == id ? theme_.hover
                               : n.kind == Kind::Panel   ? theme_.background
                                                         : theme_.surface;
            add(Draw::Type::Fill, b, box.clip, color);
        }
        if (focus_ == id)
            add(Draw::Type::Outline, b, box.clip, theme_.focus);
        Rect textBounds{
            b.x + theme_.inset, b.y + theme_.inset, std::max(0.f, b.w - insetSides * theme_.inset),
            std::max(0.f, b.h - insetSides * theme_.inset)}; // numbers: inset around content.
        if (n.kind == Kind::Toggle) {
            Rect mark{textBounds.x, textBounds.y, theme_.lineHeight, theme_.lineHeight};
            add(n.checked ? Draw::Type::Fill : Draw::Type::Outline, mark, box.clip,
                enabled ? theme_.accent : theme_.disabled);
            textBounds.x += theme_.lineHeight + theme_.inset;
            textBounds.w = std::max(0.f, textBounds.w - theme_.lineHeight - theme_.inset);
        }
        if (n.kind == Kind::Slider) {
            const double fraction = (n.value - n.minimum) / (n.maximum - n.minimum);
            Rect thumb{b.x + theme_.inset +
                           static_cast<float>(fraction) *
                               std::max(0.f, b.w - insetSides * theme_.inset - theme_.thumbPixels),
                       b.y + theme_.inset, theme_.thumbPixels,
                       std::max(0.f, b.h - insetSides * theme_.inset)};
            add(Draw::Type::Fill, thumb, box.clip, enabled ? theme_.accent : theme_.disabled);
        } // numbers: inset around slider thumb.
        if (n.kind == Kind::List) {
            const auto contentClip = intersect(textBounds, box.clip);
            const auto first = static_cast<size_t>(std::clamp(
                std::floor((contentClip.y - textBounds.y + scroll_.at(id)) / theme_.lineHeight),
                0.f, static_cast<float>(n.items.size())));
            const auto last = static_cast<size_t>(std::clamp(
                std::ceil((contentClip.y + contentClip.h - textBounds.y + scroll_.at(id)) /
                          theme_.lineHeight),
                0.f, static_cast<float>(n.items.size())));
            for (size_t row = first; row < last; ++row) {
                Rect rect{textBounds.x, textBounds.y + row * theme_.lineHeight - scroll_.at(id),
                          textBounds.w, theme_.lineHeight};
                if (n.value == static_cast<double>(row))
                    add(Draw::Type::Fill, rect, contentClip, theme_.accent);
                add(Draw::Type::Text, rect, contentClip, foreground, n.items[row],
                    theme_.fontPixels);
            }
        } else if (n.kind != Kind::Panel && textBounds.w > 0) {
            const auto text = n.kind == Kind::Slider ? n.text + " " + valueText(n.value) : n.text;
            const auto lines = wrapText(text, textBounds.w, [&](std::string_view value) {
                const auto width = measure_(value, theme_.fontPixels);
                require(std::isfinite(width) && width >= 0, "Invalid font measurement");
                return width;
            });
            float y = textBounds.y;
            for (const auto& line : lines) {
                add(Draw::Type::Text, {textBounds.x, y, textBounds.w, theme_.lineHeight}, box.clip,
                    foreground, line, theme_.fontPixels);
                y += theme_.lineHeight;
            }
        }
    }
    for (const auto& id : order_)
        if (extent_.contains(id) && extent_.at(id) > 0) {
            const auto& n = *nodes_.at(id);
            const auto& b = boxes_.at(id);
            const bool row = n.kind == Kind::Panel && n.layout.direction == Direction::Row;
            Rect track =
                row ? Rect{b.clip.x, b.clip.y + std::max(0.f, b.clip.h - theme_.scrollbarPixels),
                           b.clip.w, theme_.scrollbarPixels}
                    : Rect{b.clip.x + std::max(0.f, b.clip.w - theme_.scrollbarPixels), b.clip.y,
                           theme_.scrollbarPixels, b.clip.h};
            const float length = row ? track.w : track.h;
            const float thumb = std::min(
                length, std::max(theme_.thumbPixels, length * length / (length + extent_.at(id))));
            const float offset = (length - thumb) * scroll_.at(id) / extent_.at(id);
            add(Draw::Type::Fill, track, b.clip, theme_.disabled);
            Rect indicator = row ? Rect{track.x + offset, track.y, thumb, track.h}
                                 : Rect{track.x, track.y + offset, track.w, thumb};
            add(Draw::Type::Fill, indicator, b.clip, theme_.accent);
        }
    if (!hover_.empty() && hoverSeconds_ >= theme_.tooltipDelaySeconds &&
        !nodes_.at(hover_)->tooltip.empty()) {
        const auto& text = nodes_.at(hover_)->tooltip;
        const float width = std::min(viewport_.w, measured(text) + insetSides * theme_.inset);
        const auto lines = wrapText(text, std::max(0.f, width - insetSides * theme_.inset),
                                    [&](std::string_view s) { return measured(s); });
        const float height = lines.size() * theme_.lineHeight +
                             insetSides * theme_.inset; // numbers: inset on both sides of tooltip.
        Rect b{std::clamp(pointer_.x, viewport_.x,
                          std::max(viewport_.x, viewport_.x + viewport_.w - width)),
               std::clamp(pointer_.y + theme_.lineHeight, viewport_.y,
                          std::max(viewport_.y, viewport_.y + viewport_.h - height)),
               width, height};
        add(Draw::Type::Fill, b, viewport_, theme_.surface);
        float y = b.y + theme_.inset;
        for (const auto& line : lines) {
            add(Draw::Type::Text, {b.x + theme_.inset, y, width, theme_.lineHeight},
                intersect(b, viewport_), theme_.text, line, theme_.fontPixels);
            y += theme_.lineHeight;
        }
    }
    return result;
}
} // namespace paper::ui
