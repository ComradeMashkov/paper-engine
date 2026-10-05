#pragma once
#include "paper/ui/ui.hpp"

namespace paper::ui {
// Host-painted content can share the same bounded scrolling contract as widgets.
// The host retains this value between frames; no content pointers are stored.
class ScrollArea {
  public:
    void layout(Rect viewport, float contentHeight, std::string target) {
        if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
            !std::isfinite(viewport.w) || !std::isfinite(viewport.h) || viewport.w <= 0 ||
            viewport.h <= 0 || !std::isfinite(contentHeight) || contentHeight < 0)
            throw std::invalid_argument("Invalid host scroll area");
        if (target_ != target)
            offset_ = 0;
        target_ = std::move(target);
        viewport_ = viewport;
        maximum_ = std::max(0.f, contentHeight - viewport.h);
        offset_ = std::clamp(offset_, 0.f, maximum_);
    }
    [[nodiscard]] bool input(const Input& event, bool keyboard = false) {
        if (maximum_ <= 0)
            return false;
        float next = offset_;
        if (event.type == InputType::Wheel &&
            viewport_.contains(event.position.x, event.position.y))
            next += event.wheel;
        else if (keyboard && event.type == InputType::Key) {
            if (event.key == Key::PageUp)
                next -= viewport_.h;
            else if (event.key == Key::PageDown)
                next += viewport_.h;
            else if (event.key == Key::Home)
                next = 0;
            else if (event.key == Key::End)
                next = maximum_;
            else
                return false;
        } else
            return false;
        const auto previous = offset_;
        offset_ = std::clamp(next, 0.f, maximum_);
        return previous != offset_;
    }
    void reveal(Rect content) {
        if (content.y < viewport_.y + offset_)
            offset_ = content.y - viewport_.y;
        else if (content.y + content.h > viewport_.y + viewport_.h + offset_)
            offset_ = content.y + content.h - viewport_.y - viewport_.h;
        offset_ = std::clamp(offset_, 0.f, maximum_);
    }
    [[nodiscard]] float offset() const { return offset_; }
    [[nodiscard]] float maximum() const { return maximum_; }
    [[nodiscard]] Rect viewport() const { return viewport_; }

  private:
    Rect viewport_{};
    float offset_ = 0, maximum_ = 0;
    std::string target_;
};
} // namespace paper::ui
