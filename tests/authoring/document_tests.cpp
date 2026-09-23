#include "paper/authoring/document.hpp"
#include "paper/content/document.hpp"
#include "paper/scenes/scene.hpp"
#include <iostream>
#include <stdexcept>
using paper::ContentValue;
using paper::authoring::SceneDocument;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F&& action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "Expected rejection");
}
const std::string source = R"(format = 'dcmo.scene'
version = 2
# Авторский комментарий
[scene]
id = 'room'
[[scene.nodes]]
id = 'chair' # ID должен сохраниться
template = 'furniture.chair'
position = [1, 2, 3] # meters
label = 'Стул'
[[scene.nodes]]
id = 'table'
kind = 'decoration'
)";
} // namespace
int main() {
    try {
        SceneDocument scene("room.dcscene", source);
        check(!scene.dirty() && scene.serialized() == source, "No-op must preserve every byte");
        const auto position = scene.property("chair", "position");
        scene.setProperty("chair", "position", ContentValue::array({4.25, 2, 3}));
        auto output = scene.serialized();
        check(output.find("# Авторский комментарий") != std::string::npos &&
                  output.find("# meters") != std::string::npos &&
                  output.find("template = 'furniture.chair'") != std::string::npos &&
                  output.find("label = 'Стул'") != std::string::npos,
              "Preserve comments, Unicode and inheritance");
        check(paper::content::parse(output, "edited") == scene.data(),
              "Saved semantics match document");
        scene.setProperty("chair", "position", position);
        check(scene.serialized() == source && !scene.dirty(), "Undo restores original bytes");
        scene.setProperty("chair", "scale", 1.5);
        scene.setProperty("chair", "yaw", .5);
        output = scene.serialized();
        scene.acceptSaved(output);
        check(!scene.dirty(), "Save establishes a new dirty baseline");
        scene.setProperty("chair", "scale", {});
        scene.setProperty("chair", "yaw", {});
        check(paper::content::parse(scene.serialized(), "undo") ==
                  paper::content::parse(source, "original"),
              "Undo across save removes inserted overrides");
        check(scene.node("chair").at("template") == "furniture.chair", "Template remains authored");
        const auto snapshot = scene.data();
        rejects([&] { scene.setProperty("chair", "scale", 0); });
        rejects([&] { scene.setProperty("chair", "position", ContentValue::array({1, 2})); });
        rejects([&] { scene.setProperty("missing", "yaw", 0); });
        rejects([&] { scene.setProperty("chair", "id", "changed"); });
        check(scene.data() == snapshot, "Invalid edits do not mutate document");
        const std::string inlineSource = "format='dcmo.scene'\nversion=2\n[scene]\nid='room'"
                                         "\nnodes=[{id='chair', label='Стул', yaw=0.0}]\n";
        SceneDocument inlined("inline.dcscene", inlineSource);
        inlined.setProperty("chair", "yaw", .25);
        check(paper::content::parse(inlined.serialized(), "inline") == inlined.data(),
              "UTF-8 source columns in inline table");
        inlined.setProperty("chair", "scale", 2);
        rejects([&] { (void)inlined.serialized(); });
        check(inlined.original() == inlineSource, "Unsupported layout leaves original untouched");
        const std::filesystem::path root = PAPER_EXAMPLE_ASSETS;
        const auto original = paper::content::read(root / "boxes.dcscene");
        auto edited = original;
        edited["scene"]["nodes"][0]["position"] = ContentValue::array({5, 0, 0});
        paper::PaintedTexture material;
        material.width = material.height = 1;
        material.pixels.push_back({});
        const std::array<std::string_view, 1> names{"neutral"};
        const auto package = paper::loadScenes(root, {material}, names, "world.dcworld",
                                               {{"boxes.dcscene", edited}});
        const auto child = std::ranges::find(package->nodes, "box.child", &paper::SceneNode::id);
        check(child != package->nodes.end() && child->transform.position.x == 7,
              "Unsaved local transforms compile into world transforms");
        check(paper::content::read(root / "boxes.dcscene") == original,
              "Preview does not write authored files");
        std::cout << "Authoring document invariants passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
