#pragma once
#include "paper/core/content_value.hpp"
#include "paper/render/scene.hpp"
namespace paper {
// Scene units: metres; quaternion arrays use XYZW. Positive scale preserves winding.
[[nodiscard]] Rotation3 readRotation(const ContentValue& value);
[[nodiscard]] MeshTransform readTransform(const ContentValue& node);
[[nodiscard]] ContentValue writeTransform(const MeshTransform& transform);
[[nodiscard]] MeshTransform relativeTransform(const MeshTransform& parent,
                                              const MeshTransform& world);
} // namespace paper
