#include "paper/authoring/document.hpp"
#include "paper/content/document.hpp"
#include <iostream>
using namespace paper;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) {
            ++failures;
            std::cerr << message << '\n';
        }
    };
    const std::string source =
        "format='dcmo.scene'\nversion=3\n[scene]\nid='room'\n[[scene.nodes]]\nid='box'\n# outside "
        "property\n[scene.nodes.collision]\nshape='bounds' # authored shape\ncategory=1 # authored "
        "category\nmask=1\n";
    authoring::SceneDocument doc("scene.dcscene", source);
    const auto original = doc.property("box", "collision");
    doc.setProperty("box", "collision",
                    ContentValue{{"shape", "mesh"}, {"category", 4}, {"mask", 4294967295u}});
    auto saved = doc.serialized();
    check(content::parse(saved, "collider fixture") == doc.data() &&
              saved.find("# authored category") != std::string::npos,
          "nested collider edits preserve semantics/comments");
    doc.setProperty("box", "collision", original);
    check(doc.serialized() == source, "Undo restores original collider bytes");
    doc.setProperty("box", "collision", false);
    saved = doc.serialized();
    check(content::parse(saved, "collider fixture") == doc.data() &&
              saved.find("# outside property") != std::string::npos,
          "disabling nested collider removes only its property table");
    doc.acceptSaved(saved);
    doc.setProperty("box", "collision", original);
    saved = doc.serialized();
    check(content::parse(saved, "collider fixture") == doc.data(),
          "inline collider insertion round trips after Save");
    authoring::SceneDocument inlineDoc("scene.dcscene", saved);
    inlineDoc.setProperty("box", "collision", false);
    check(content::parse(inlineDoc.serialized(), "inline collider fixture") == inlineDoc.data(),
          "inline collider can change back to boolean");
    std::cout << "collision authoring failures=" << failures << '\n';
    return failures ? 1 : 0;
}
