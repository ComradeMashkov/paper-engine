#pragma once
#include "paper/render/scene.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace paper::editor {
struct Options {
    std::string title = "Paper Editor";
    std::filesystem::path project, shaders;
    // A host may provide its procedural material palette without engine -> game dependencies.
    std::function<std::vector<PaintedTexture>()> materials;
    std::vector<std::string> materialNames;
};
int run(int argc, char** argv, Options options = {});
} // namespace paper::editor
