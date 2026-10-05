#pragma once
#include "paper/render/scene.hpp"
#include "paper/scenes/scene.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace paper::editor {
struct TextAssetType {
    std::string name, extension, initialSource;
    // Host-owned syntax/domain checks; the engine never embeds a game's VM.
    std::function<void(const std::filesystem::path&, const std::filesystem::path&,
                       std::string_view)>
        validate;
};
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
    ContentValue nodePropertyDefaults = ContentValue::object();
    std::map<std::string, std::vector<std::string>, std::less<>> nodePropertyChoices;
    std::function<void(const ScenePackage&)> validateScenes;
    std::function<void(const ScenePackage&, const std::map<std::filesystem::path, std::string>&)>
        validateContent;
    std::vector<TextAssetType> textAssets;
    // Localization-aware hosts can supply a valid initial room label for new scenes.
    std::string initialRoomLabel;
};
int run(int argc, char** argv, Options options = {});
} // namespace paper::editor
