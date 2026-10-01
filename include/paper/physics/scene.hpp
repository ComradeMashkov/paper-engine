#pragma once
#include "paper/physics/world.hpp"
#include "paper/scenes/scene.hpp"
namespace paper {
using SceneColliders = std::map<std::string, ColliderId, std::less<>>;
// Adds a scene transactionally; caller owns returned IDs until removal/world destruction.
// Authored boxes retain their exact affine transform. Meshes use the static bind pose.
[[nodiscard]] SceneColliders addSceneColliders(PhysicsWorld& world, const ScenePackage& package,
                                               std::string_view scene);
} // namespace paper
