#include "paper/authoring/document.hpp"
#include "paper/authoring/source.hpp"
#include "paper/content/document.hpp"
#include "paper/scenes/scene.hpp"
#include <fstream>
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
int main(int argc, char** argv) {
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
        SceneDocument structure("structure.dcscene",
                                source + "[scene.nodes.bounds]\ncenter=[0,0,0]\nhalf=[1,1,1]\n"
                                         "[[scene.rooms]]\nid='workshop'\nlabel='Workshop'\n");
        const auto originalNodes = structure.nodes();
        auto nodes = originalNodes;
        nodes.elements().push_back(ContentValue{
            {"id", "new.box"}, {"resource", "box"}, {"position", ContentValue::array({0, 0, 4})}});
        structure.replaceNodes(nodes);
        structure.setProperty("chair", "label", "Новое имя");
        auto structuralOutput = structure.serialized();
        check(paper::content::parse(structuralOutput, "created") == structure.data(),
              "Create and rename preserve nested tables and other scene sections");
        check(structuralOutput.find("# ID должен сохраниться") != std::string::npos &&
                  structuralOutput.find("# Авторский комментарий") != std::string::npos,
              "Structural edits preserve existing source comments");
        structure.acceptSaved(structuralOutput);
        structure.replaceNodes(originalNodes);
        structure.acceptSaved(structure.serialized());
        auto removed = originalNodes;
        removed.erase(0);
        structure.replaceNodes(removed);
        structure.acceptSaved(structure.serialized());
        structure.replaceNodes(originalNodes);
        check(paper::content::parse(structure.serialized(), "undo-delete") == structure.data(),
              "Undo deletion across save restores node order and nested values");
        structure.replaceNodes(ContentValue::array());
        structure.acceptSaved(structure.serialized());
        structure.replaceNodes(originalNodes);
        check(paper::content::parse(structure.serialized(), "from-empty") == structure.data(),
              "Create nodes in empty scene after save");
        const auto validNodes = structure.nodes();
        auto duplicate = validNodes;
        duplicate.elements().push_back(validNodes.at(0));
        rejects([&] { structure.replaceNodes(duplicate); });
        check(structure.nodes() == validNodes, "Invalid node replacement is atomic");
        const std::string completeSource = source + R"(
# Комната сохраняет комментарии
[[scene.rooms]]
id = 'room.main'
label = 'Главная'
floorY = 0.0
[scene.rooms.bounds]
center = [0, 0, 0]
half = [4, 3, 5] # metres
[[scene.lights]]
id = 'lamp'
position = [0, 2, 0]
intensity = 1.0 # authored light
[[scene.spawns]]
id = 'start'
position = [0, 1.7, 0]
[[scene.zones]]
id = 'exit'
targetSpawn = 'start'
[scene.zones.bounds]
center = [1, 1, 1]
half = [1, 1, 1]
)";
        SceneDocument complete("complete.dcscene", completeSource);
        auto completeData = complete.data();
        completeData["scene"]["rooms"][0]["bounds"]["half"][0] = 6.25;
        completeData["scene"]["lights"][0]["intensity"] = 2.5;
        completeData["scene"]["spawns"].push_back(
            {{"id", "annex.start"}, {"position", ContentValue::array({2, 1.7, 3})}});
        completeData["scene"]["zones"][0]["targetSpawn"] = "annex.start";
        completeData["scene"]["nodes"][0]["actions"] = "chair.actions";
        complete.replaceData(completeData);
        auto completeOutput = complete.serialized();
        check(paper::content::parse(completeOutput, "complete") == completeData &&
                  completeOutput.find("# metres") != std::string::npos &&
                  completeOutput.find("# authored light") != std::string::npos &&
                  completeOutput.find("label = 'Главная'") != std::string::npos,
              "All scene sections and gameplay overrides preserve comments and Unicode");
        complete.acceptSaved(completeOutput);
        complete.replaceSource(completeSource);
        check(complete.dirty() && complete.serialized() == completeSource,
              "Complete source Undo across Save restores exact original text");
        complete.acceptSaved(completeSource);
        complete.replaceSource("# source-only annotation\n" + completeSource);
        check(complete.dirty() && complete.serialized().starts_with("# source-only annotation"),
              "Comment-only source edits are dirty and persist");
        auto reordered = complete.data();
        reordered["scene"]["rooms"].push_back({{"id", "annex"}, {"label", "Annex"}});
        complete.replaceData(reordered);
        complete.acceptSaved(complete.serialized());
        std::reverse(reordered["scene"]["rooms"].begin(), reordered["scene"]["rooms"].end());
        complete.replaceData(reordered);
        check(paper::content::parse(complete.serialized(), "reordered") == reordered,
              "Reorder table arrays with nested tables");
        reordered["scene"]["rooms"] = ContentValue::array();
        complete.replaceData(reordered);
        complete.acceptSaved(complete.serialized());
        reordered["scene"]["rooms"].push_back({{"id", "new.room"}});
        complete.replaceData(reordered);
        check(paper::content::parse(complete.serialized(), "from-empty-sections") == reordered,
              "Remove and recreate non-node scene sections across Save");
        auto invalidVersion = complete.data();
        invalidVersion["version"] = 3;
        rejects([&] { complete.replaceData(invalidVersion); });
        const std::string worldSource =
            "# world comment\nformat='dcmo.world'\nversion=2\n"
            "[world]\nentrySpawn='start' # destination\n"
            "scenes=['boxes.dcscene']\nresources=['boxes.dcresources']\n"
            "templates=[]\n";
        auto authoredWorld = paper::content::parse(worldSource, "world");
        auto changedWorld = authoredWorld;
        changedWorld["world"]["scenes"].push_back("virtual.dcscene");
        const auto patchedWorld =
            paper::authoring::patchSource(worldSource, authoredWorld, changedWorld, "world");
        check(patchedWorld.find("# destination") != std::string::npos &&
                  paper::content::parse(patchedWorld, "world") == changedWorld,
              "World manifest editing preserves authored fields and comments");
        const std::filesystem::path root = argc > 1 ? argv[1] : PAPER_EXAMPLE_ASSETS;
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
        auto virtualScene = original;
        virtualScene["scene"]["id"] = "virtual";
        virtualScene["scene"]["nodes"] = ContentValue::array();
        virtualScene["scene"]["spawns"] = ContentValue::array();
        virtualScene["scene"]["rooms"] = ContentValue::array();
        const auto virtualPackage =
            paper::loadScenes(root, {material}, names, "world.dcworld",
                              {{"world.dcworld", changedWorld}, {"virtual.dcscene", virtualScene}});
        check(virtualPackage && !std::filesystem::exists(root / "virtual.dcscene"),
              "Compile virtual new scenes before their first save");
        if (argc > 2) {
            size_t checked = 0;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[2])) {
                if (entry.path().extension() != ".dcscene")
                    continue;
                std::ifstream input(entry.path(), std::ios::binary);
                const std::string text{std::istreambuf_iterator<char>(input), {}};
                SceneDocument gameScene(entry.path(), text);
                const auto baseline = gameScene.nodes();
                auto added = baseline;
                added.elements().push_back(
                    ContentValue{{"id", "editor.qa.created"}, {"label", "QA"}});
                gameScene.replaceNodes(added);
                gameScene.acceptSaved(gameScene.serialized());
                gameScene.replaceNodes(baseline);
                check(paper::content::parse(gameScene.serialized(), "undo-host") ==
                          paper::content::parse(text, "original-host"),
                      "Host scene structural undo must preserve all authored fields");
                ++checked;
            }
            check(checked > 0, "Expected copied host scenes");
            std::cout << "Copied host scene round trips passed: " << checked << '\n';
        }
        std::cout << "Authoring document invariants passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
