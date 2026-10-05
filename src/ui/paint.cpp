#include "paper/ui/paint.hpp"
#include "paper/engine.hpp"
#include <limits>
namespace paper::ui {
ScopedClip::ScopedClip(Engine& engine, Rect clip) : engine_(engine), previous_(engine.clipRect()) {
    if (previous_) {
        const auto p = *previous_;
        const float x = std::max(clip.x, p.x), y = std::max(clip.y, p.y);
        clip = {x, y, std::max(0.f, std::min(clip.x + clip.w, p.x + p.w) - x),
                std::max(0.f, std::min(clip.y + clip.h, p.y + p.h) - y)};
    }
    if (!engine_.setClipRect(clip))
        throw std::runtime_error("Cannot apply UI clipping");
}
ScopedClip::~ScopedClip() {
    (void)engine_.setClipRect(previous_);
}
namespace {
int pixels(float value) {
    if (!std::isfinite(value) || value <= 0 ||
        value >= static_cast<float>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Invalid UI font size");
    return static_cast<int>(std::ceil(value));
}
} // namespace
Context::Measure engineMeasure(Engine& engine) {
    return
        [&engine](std::string_view text, float size) { return engine.measure(text, pixels(size)); };
}
std::vector<Input> engineInput(const paper::Input& frame, float wheelPixels) {
    if (!std::isfinite(wheelPixels) || wheelPixels < 0)
        throw std::invalid_argument("Invalid wheel pixel scale");
    std::vector<Input> result;
    if (frame.focusLost || frame.debugCaptured) {
        result.push_back({InputType::FocusLost, {}});
        return result;
    }
    if (frame.canvasChanged || frame.windowModeChanged)
        result.push_back({InputType::Cancel, {}});
    const auto key = [](SDL_Keycode value) -> std::optional<Key> {
        switch (value) {
        case SDLK_TAB:
            return Key::Tab;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return Key::Enter;
        case SDLK_SPACE:
            return Key::Space;
        case SDLK_LEFT:
            return Key::Left;
        case SDLK_RIGHT:
            return Key::Right;
        case SDLK_UP:
            return Key::Up;
        case SDLK_DOWN:
            return Key::Down;
        case SDLK_HOME:
            return Key::Home;
        case SDLK_END:
            return Key::End;
        case SDLK_ESCAPE:
            return Key::Escape;
        case SDLK_PAGEUP:
            return Key::PageUp;
        case SDLK_PAGEDOWN:
            return Key::PageDown;
        default:
            return {};
        }
    };
    for (const auto& e : frame.events) {
        const Vec2 p{e.x, e.y};
        if (e.click)
            result.push_back({InputType::Down, p});
        else if (e.released)
            result.push_back({InputType::Up, p});
        else if (e.wheelY != 0)
            result.push_back({InputType::Wheel, p, -e.wheelY * wheelPixels});
        else if (e.dx != 0 || e.dy != 0)
            result.push_back({InputType::Move, p});
        if (const auto k = key(e.repeatedKey ? e.repeatedKey : e.key))
            result.push_back({InputType::Key,
                              {},
                              0,
                              *k,
                              e.held[SDL_SCANCODE_LSHIFT] || e.held[SDL_SCANCODE_RSHIFT],
                              e.repeatedKey != 0});
    }
    result.push_back({InputType::Move, {frame.x, frame.y}});
    return result;
}
void paint(Engine& engine, std::span<const Draw> commands) {
    struct Restore {
        Engine& engine;
        std::optional<Rect> previous;
        ~Restore() { (void)engine.setClipRect(previous); }
    } restore{engine, engine.clipRect()};
    for (const auto& c : commands) {
        if (c.clip.w <= 0 || c.clip.h <= 0)
            continue;
        auto clip = c.clip;
        if (restore.previous) {
            const auto previous = *restore.previous;
            const float x = std::max(clip.x, previous.x), y = std::max(clip.y, previous.y);
            clip = {x, y, std::max(0.f, std::min(clip.x + clip.w, previous.x + previous.w) - x),
                    std::max(0.f, std::min(clip.y + clip.h, previous.y + previous.h) - y)};
        }
        if (clip.w <= 0 || clip.h <= 0)
            continue;
        if (!engine.setClipRect(clip))
            throw std::runtime_error("Cannot apply UI clipping");
        switch (c.type) {
        case Draw::Type::Fill:
            engine.rect(c.bounds, c.color);
            break;
        case Draw::Type::Outline:
            engine.outline(c.bounds, c.color);
            break;
        case Draw::Type::Text:
            engine.text(c.text, c.bounds.x, c.bounds.y, pixels(c.fontPixels), c.color);
            break;
        }
    }
}
} // namespace paper::ui
