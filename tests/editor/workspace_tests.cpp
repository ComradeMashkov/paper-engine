#include "../../editor/viewport.hpp"
#include "../../editor/workspace.hpp"
#include "paper/content/document.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T* child(QObject& object, const char* name) {
    auto* result = object.findChild<T*>(name);
    check(result, "Missing workspace control");
    return result;
}
template <class F> void waitFor(F condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(condition(), "Editor did not finish rendering/validating within five seconds");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        check(argc == 2, "Pass an isolated Paper project path");
        QTemporaryDir temporary;
        check(temporary.isValid(), "Cannot allocate test workspace");
        const auto destination = std::filesystem::path(temporary.path().toStdString()) / "project";
        const auto supplied = std::filesystem::canonical(argv[1]);
        std::filesystem::copy(supplied.parent_path(), destination,
                              std::filesystem::copy_options::recursive);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QCoreApplication::setOrganizationName("PaperEditorTests");
        QCoreApplication::setApplicationName("Workspace");
        paper::editor::Options options;
        options.project = destination / supplied.filename();
        options.shaders = PAPER_TEST_SHADER_DIR;
        auto window = paper::editor::makeWorkspace(options);
        window->show();
        auto* tree = child<QTreeWidget>(*window, "sceneHierarchy");
        auto* problems = child<QListWidget>(*window, "projectProblems");
        auto* viewport = dynamic_cast<paper::editor::Viewport*>(window->centralWidget());
        check(viewport, "Missing native viewport");
        waitFor([&] { return viewport->renderedFrames() > 0 || problems->count() > 0; });
        if (problems->count())
            throw std::runtime_error(problems->item(0)->text().toStdString());
        for (const auto name : {"hierarchy", "inspector", "assets", "problems"}) {
            const auto* panel = child<QDockWidget>(*window, name);
            check(panel->isVisible() && panel->width() > 0 && panel->height() > 0,
                  "A workspace panel is hidden");
            check(!panel->geometry().intersects(viewport->geometry()),
                  "Viewport overlaps a dock panel");
        }
        const auto projectSpec = paper::content::read(options.project);
        const auto assetsRoot = destination / projectSpec.at("assets").get<std::string>();
        const auto scenePath =
            assetsRoot /
            child<QComboBox>(*window, "sceneList")->currentData().toString().toStdString();
        const auto count = [&] {
            return tree->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive).size();
        };
        const auto action = [&](const char* name) {
            auto* item = child<QAction>(*window, name);
            check(item->isEnabled(), "Expected enabled action");
            item->trigger();
        };
        const auto save = [&] {
            action("saveScene");
            check(problems->count() == 0, "Editing produced validation errors");
            return paper::content::read(scenePath);
        };
        const auto originalCount = count();
        check(originalCount > 0, "Expected populated fixture scene");
        action("createGroup");
        check(count() == originalCount + 1, "Create must add and select a node");
        const auto id = tree->currentItem()->data(0, Qt::UserRole).toString().toStdString();
        auto* label = child<QLineEdit>(*window, "nodeLabel");
        label->setText("Edited group");
        QMetaObject::invokeMethod(label, "editingFinished", Qt::DirectConnection);
        auto* x = child<QDoubleSpinBox>(*window, "transform0");
        x->setValue(7.25);
        QMetaObject::invokeMethod(x, "editingFinished", Qt::DirectConnection);
        auto persisted = save();
        auto node = [&](const paper::ContentValue& document, const std::string& key) {
            for (const auto& value : document.at("scene").at("nodes"))
                if (value.at("id") == key)
                    return value;
            throw std::runtime_error("Saved node is missing");
        };
        check(node(persisted, id).at("label") == "Edited group" &&
                  node(persisted, id).at("position").at(0) == 7.25,
              "Inspector edits must persist");
        action("duplicateNode");
        check(count() == originalCount + 2, "Duplicate must add a node");
        action("undo");
        check(count() == originalCount + 1, "Undo duplicate");
        action("redo");
        check(count() == originalCount + 2, "Redo duplicate");
        persisted = save();
        action("deleteNode");
        check(count() == originalCount + 1, "Delete selected object");
        save();
        action("undo");
        check(count() == originalCount + 2, "Undo deletion across save");
        save();
        const auto duplicateId =
            tree->currentItem()->data(0, Qt::UserRole).toString().toStdString();
        auto* parent = child<QComboBox>(*window, "nodeParent");
        const auto parentIndex = parent->findData(QString::fromStdString(id));
        parent->setCurrentIndex(parentIndex);
        QMetaObject::invokeMethod(parent, "activated", Qt::DirectConnection,
                                  Q_ARG(int, parentIndex));
        persisted = save();
        check(node(persisted, duplicateId).at("parent") == id, "Reparent must persist");
        parent->setCurrentIndex(0);
        QMetaObject::invokeMethod(parent, "activated", Qt::DirectConnection, Q_ARG(int, 0));
        persisted = save();
        check(!node(persisted, duplicateId).contains("parent"),
              "Detach must remove parent, not write an invalid empty ID");
        auto* resources = child<QTreeWidget>(*window, "resourceLibrary");
        check(resources->topLevelItemCount() > 0, "Expected resource library");
        resources->setCurrentItem(resources->topLevelItem(0));
        action("createResource");
        check(count() == originalCount + 3, "Resource must create a scene instance");
        const auto resourceId = tree->currentItem()->data(0, Qt::UserRole).toString().toStdString();
        persisted = save();
        const auto oldX = node(persisted, resourceId).at("position").at(0).get<double>();
        viewport->transformed(paper::editor::Viewport::Tool::Move, {1, 0, 0}, 0);
        persisted = save();
        check(std::abs(node(persisted, resourceId).at("position").at(0).get<double>() - oldX - 1) <
                  1e-5,
              "Viewport transform must persist");
        const auto beforeResize = viewport->renderedFrames();
        window->resize(1100, 760);
        waitFor([&] { return viewport->renderedFrames() > beforeResize; });
        check(problems->count() == 0, "Native resize must keep rendering");
        window->close();
        window.reset();
        // Reopen the saved project, not an in-memory document from the previous window.
        auto reopened = paper::editor::makeWorkspace(options);
        check(child<QTreeWidget>(*reopened, "sceneHierarchy")
                      ->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive)
                      .size() == originalCount + 3,
              "Reopen must retain authored objects");
        reopened.reset();
        std::cout
            << "Editor workspace checks passed: native rendering/docks/resize, create, inspector, "
               "duplicate, delete, undo/redo across save, reparent, resources, transform, reopen\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
