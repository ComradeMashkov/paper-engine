#pragma once
#include "paper/ui/ui.hpp"
namespace paper {
class Engine;
struct Input;
} // namespace paper
namespace paper::ui {
class ScopedClip {
  public:
    ScopedClip(Engine& engine, Rect clip);
    ~ScopedClip();
    ScopedClip(const ScopedClip&) = delete;
    ScopedClip& operator=(const ScopedClip&) = delete;

  private:
    Engine& engine_;
    std::optional<Rect> previous_;
};
[[nodiscard]] Context::Measure engineMeasure(Engine& engine);
inline constexpr float defaultWheelPixels = 40;
[[nodiscard]] std::vector<Input> engineInput(const paper::Input& input,
                                             float wheelPixels = defaultWheelPixels);
void paint(Engine& engine, std::span<const Draw> commands);
} // namespace paper::ui
