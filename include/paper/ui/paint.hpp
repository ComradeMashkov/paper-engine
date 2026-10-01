#pragma once
#include "paper/ui/ui.hpp"
namespace paper {
class Engine;
struct Input;
} // namespace paper
namespace paper::ui {
[[nodiscard]] Context::Measure engineMeasure(Engine& engine);
inline constexpr float defaultWheelPixels = 40;
[[nodiscard]] std::vector<Input> engineInput(const paper::Input& input,
                                             float wheelPixels = defaultWheelPixels);
void paint(Engine& engine, std::span<const Draw> commands);
} // namespace paper::ui
