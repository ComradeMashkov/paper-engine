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
    // Trusted host configuration, never read executable paths from a project file.
    // Runtime accepts --paper-play <session.paperplay>; see docs/EDITOR.md.
    std::filesystem::path playExecutable;
    std::vector<std::string> playArguments;
};
int run(int argc, char** argv, Options options = {});
} // namespace paper::editor
