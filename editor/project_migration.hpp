#pragma once
#include "paper/scenes/scene.hpp"
#include <functional>
namespace paper::editor {
// Copies only the descriptor and asset tree, never the host source/build directory.
// Publishes a new destination after conversion and optional full host validation.
[[nodiscard]] std::filesystem::path
migrateProject(const std::filesystem::path& descriptor, const std::filesystem::path& destination,
               const SceneDocuments& overrides = {},
               const std::function<void(const std::filesystem::path&)>& validate = {});
} // namespace paper::editor
