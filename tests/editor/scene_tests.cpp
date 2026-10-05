#include "../../editor/project_storage.hpp"
#include "../../editor/resource_browser.hpp"
#include "../../editor/scene_data_editor.hpp"
#include "../../editor/text_asset_editor.hpp"
#include "../../editor/workspace.hpp"
#include "paper/content/document.hpp"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace paper;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T* child(QObject& object, const char* name) {
    auto* value = object.findChild<T*>(name);
    check(value, name);
    return value;
}
std::string bytes(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
void click(QObject& object, const char* name) {
    child<QPushButton>(object, name)->click();
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        check(temporary.isValid(), "Temporary directory");
        const auto root = fs::path(temporary.path().toStdString()) / "project";
        fs::copy(PAPER_EXAMPLE_PROJECT, root, fs::copy_options::recursive);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
        QCoreApplication::setOrganizationName("PaperEditorTests");
        QCoreApplication::setApplicationName("SceneAuthoring");
        editor::Options options;
        options.project = root / "boxes.paperproject";
        const auto assets = root / "assets", world = assets / "world.dcworld";
        editor::TextAssetType type{"Sequence", ".txt", "",
                                   [](const auto&, const auto&, std::string_view source) {
                                       if (!source.starts_with("ok"))
                                           throw std::runtime_error("Invalid sequence syntax");
                                   }};
        options.textAssets.push_back(type);
        options.nodePropertyDefaults = {
            {"kind", ""},          {"detail", 0},
            {"stateKey", ""},      {"visibleWhen", ""},
            {"actions", ""},       {"itemInstance", ""},
            {"openAngle", 0.0},    {"openOffset", ContentValue::array({0.0, 0.0, 0.0})},
            {"legacyDoor", -1},    {"inspectResource", ""},
            {"inspectScale", 1.5}, {"animation", -1},
            {"pose", 0},           {"poses", ContentValue::array()}};
        options.nodePropertyChoices["kind"] = {"", "Inspectable"};
        options.validateContent = [](const auto&, const std::map<fs::path, std::string>& sources) {
            for (const auto& [path, source] : sources)
                if (path.extension() == ".txt" &&
                    source.find("domain-invalid") != std::string::npos)
                    throw std::runtime_error("Unknown sequence reference");
        };
        const auto originalWorld = bytes(world);
        auto window = editor::makeWorkspace(options);
        const auto action = [&](const char* name) {
            auto* item = child<QAction>(*window, name);
            check(item->isEnabled(), name);
            item->trigger();
            const auto* problems = child<QListWidget>(*window, "projectProblems");
            if (problems->count())
                throw std::runtime_error(problems->item(0)->text().toStdString());
        };
        QTimer::singleShot(0, [] {
            auto* dialog = dynamic_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                dialog->setTextValue("annex");
                dialog->accept();
            }
        });
        action("newScene");
        check(child<QComboBox>(*window, "sceneList")->count() == 2, "New scene in workspace");
        check(bytes(world) == originalWorld && !fs::exists(assets / "annex.dcscene"),
              "New scene is an unsaved project draft");
        action("autosaveNow");
        editor::ProjectStorage storage(options.project);
        auto recovery = storage.recovery();
        check(std::ranges::any_of(recovery,
                                  [&](const auto& edit) {
                                      return edit.file == assets / "annex.dcscene" && !edit.before;
                                  }),
              "New scene recovery retains absent-file baseline");
        window.reset();
        QTimer::singleShot(0, [] {
            if (auto* message = dynamic_cast<QMessageBox*>(QApplication::activeModalWidget()))
                message->button(QMessageBox::Yes)->click();
        });
        window = editor::makeWorkspace(options);
        if (child<QListWidget>(*window, "projectProblems")->count())
            throw std::runtime_error(
                child<QListWidget>(*window, "projectProblems")->item(0)->text().toStdString());
        check(child<QComboBox>(*window, "sceneList")->count() == 2 &&
                  !fs::exists(assets / "annex.dcscene"),
              "Cold recovery restores new scene as draft");
        action("saveAll");
        check(fs::exists(assets / "annex.dcscene") &&
                  content::read(world).at("world").at("scenes").size() == 2,
              "Save All persists scene and manifest together");
        action("undo");
        action("saveAll");
        check(content::read(world).at("world").at("scenes").size() == 1 &&
                  fs::exists(assets / "annex.dcscene"),
              "Undo across Save removes reference, retains asset");
        action("redo");
        action("saveAll");
        auto* scenes = child<QComboBox>(*window, "sceneList");
        scenes->setCurrentIndex(scenes->findData("annex.dcscene"));
        std::string callbackError;
        QTimer::singleShot(0, [&] {
            auto* dialog =
                dynamic_cast<editor::SceneDataEditor*>(QApplication::activeModalWidget());
            try {
                check(dialog, "Scene data dialog");
                child<QComboBox>(*dialog, "sceneDataPalette")->setCurrentIndex(1);
                click(*dialog, "addSceneData");
                child<QDoubleSpinBox>(*dialog, "intensity")->setValue(2.5);
                click(*dialog, "applySceneData");
                check(child<QLabel>(*dialog, "sceneDataStatus")->text().startsWith("Applied"),
                      "Apply validates complete candidate");
            } catch (const std::exception& e) {
                callbackError = e.what();
            }
            if (dialog)
                dialog->done(QDialog::Rejected);
        });
        action("editSceneData");
        check(callbackError.empty(), callbackError.c_str());
        action("saveAll");
        check(content::read(assets / "annex.dcscene").at("scene").at("lights")[0].at("intensity") ==
                  2.5,
              "Typed light properties persist");
        action("undo");
        action("saveAll");
        check(content::read(assets / "annex.dcscene").at("scene").at("lights").empty(),
              "Scene data Undo survives Save");
        action("redo");
        action("saveAll");
        const auto sceneBeforeInvalid = bytes(assets / "annex.dcscene");
        QTimer::singleShot(0, [&] {
            auto* dialog =
                dynamic_cast<editor::SceneDataEditor*>(QApplication::activeModalWidget());
            try {
                check(dialog, "Scene data dialog for recovery");
                auto* hierarchy = child<QTreeWidget>(*dialog, "sceneDataHierarchy");
                hierarchy->setCurrentItem(hierarchy->topLevelItem(0)->child(0));
                child<QDoubleSpinBox>(*dialog, "bounds.half.0")->setValue(-2);
                check(content::parse(dialog->recoverySource(), "pending")
                              .at("scene")
                              .at("rooms")[0]
                              .at("bounds")
                              .at("half")[0]
                              .get<double>() == -2,
                      "Dialog captures invalid form");
                child<QAction>(*window, "autosaveNow")->trigger();
            } catch (const std::exception& e) {
                callbackError = e.what();
            }
            if (dialog)
                dialog->done(QDialog::Rejected);
        });
        action("editSceneData");
        check(callbackError.empty(), callbackError.c_str());
        recovery = storage.recovery();
        if (recovery.empty())
            throw std::runtime_error("Invalid recovery snapshot is empty");
        check(std::ranges::any_of(recovery,
                                  [&](const editor::StoredEdit& edit) {
                                      return edit.file == assets / "annex.dcscene" &&
                                             content::parse(edit.after, "draft")
                                                     .at("scene")
                                                     .at("rooms")[0]
                                                     .at("bounds")
                                                     .at("half")[0]
                                                     .get<double>() == -2;
                                  }),
              "Unapplied invalid scene fields are recovered without publication");
        check(bytes(assets / "annex.dcscene") == sceneBeforeInvalid,
              "Snapshot leaves scene untouched");
        window.reset();
        QTimer::singleShot(0, [&] {
            if (auto* message = dynamic_cast<QMessageBox*>(QApplication::activeModalWidget()))
                message->button(QMessageBox::Yes)->click();
            QTimer::singleShot(0, [&] {
                auto* dialog =
                    dynamic_cast<editor::SceneDataEditor*>(QApplication::activeModalWidget());
                try {
                    check(dialog, "Invalid recovery is repairable");
                    dialog->restoreSource("# repaired draft\n" + sceneBeforeInvalid);
                    click(*dialog, "applySceneData");
                } catch (const std::exception& e) {
                    callbackError = e.what();
                }
                if (dialog)
                    dialog->done(QDialog::Rejected);
            });
        });
        window = editor::makeWorkspace(options);
        check(callbackError.empty(), callbackError.c_str());
        action("saveAll");
        check(bytes(assets / "annex.dcscene").starts_with("# repaired draft"),
              "Repaired recovery and comment-only changes persist");
        const auto editSource = [&](const fs::path& file, const ContentValue& value) {
            auto* list = child<QComboBox>(*window, "sceneList");
            list->setCurrentIndex(list->findData(QString::fromStdString(file.generic_string())));
            QTimer::singleShot(0, [&] {
                auto* dialog =
                    dynamic_cast<editor::SceneDataEditor*>(QApplication::activeModalWidget());
                try {
                    check(dialog, "Cross-scene editing dialog");
                    dialog->restoreSource(content::encode(value));
                    click(*dialog, "applySceneData");
                    check(child<QLabel>(*dialog, "sceneDataStatus")->text().startsWith("Applied"),
                          "Cross-scene candidate applies");
                } catch (const std::exception& e) {
                    callbackError = e.what();
                }
                if (dialog)
                    dialog->done(QDialog::Rejected);
            });
            action("editSceneData");
            check(callbackError.empty(), callbackError.c_str());
        };
        auto boxes = content::read(assets / "boxes.dcscene");
        auto extraSpawn = boxes.at("scene").at("spawns")[0];
        extraSpawn["id"] = "pending.destination";
        boxes["scene"]["spawns"].push_back(extraSpawn);
        editSource("boxes.dcscene", boxes);
        auto annex = content::read(assets / "annex.dcscene");
        annex["scene"]["zones"].push_back(
            {{"id", "annex.exit"},
             {"targetSpawn", "pending.destination"},
             {"bounds", annex.at("scene").at("rooms")[0].at("bounds")}});
        editSource("annex.dcscene", annex);
        QTimer::singleShot(0, [] {
            if (auto* message = dynamic_cast<QMessageBox*>(QApplication::activeModalWidget()))
                message->button(QMessageBox::Ok)->click();
        });
        child<QAction>(*window, "saveScene")->trigger();
        check(content::read(assets / "annex.dcscene").at("scene").at("zones").empty(),
              "Save Scene rejects references available only in another unsaved draft");
        child<QAction>(*window, "saveAll")->trigger();
        check(child<QListWidget>(*window, "projectProblems")->count() == 0 &&
                  content::read(assets / "annex.dcscene").at("scene").at("zones").size() == 1,
              "Save All validates and commits related scene drafts together");
        const auto workspaceFile = assets / "workspace.txt";
        std::ofstream(workspaceFile) << "ok original\n";
        auto* browser =
            dynamic_cast<editor::ResourceBrowser*>(child<QWidget>(*window, "projectBrowser"));
        check(browser, "Project browser");
        browser->openAsset("workspace.txt");
        auto* workspaceText = dynamic_cast<editor::TextAssetEditor*>(
            child<QPlainTextEdit>(*window, "textAssetSource")->window());
        check(workspaceText, "Host text asset opens from browser");
        workspaceText->restoreSource("ok domain-invalid\n");
        QTimer::singleShot(0, [] {
            if (auto* message = dynamic_cast<QMessageBox*>(QApplication::activeModalWidget()))
                message->button(QMessageBox::Ok)->click();
        });
        bool projectRejected = false;
        try {
            workspaceText->save();
        } catch (const std::exception&) {
            projectRejected = true;
        }
        check(projectRejected && bytes(workspaceFile) == "ok original\n",
              "Individual host source Save uses complete project validation");
        workspaceText->restoreSource("ok valid reference\n");
        workspaceText->save();
        check(bytes(workspaceFile) == "ok valid reference\n",
              "Host source Save commits through journal");
        scenes = child<QComboBox>(*window, "sceneList");
        scenes->setCurrentIndex(scenes->findData("boxes.dcscene"));
        auto* objects = child<QTreeWidget>(*window, "sceneHierarchy");
        objects->setCurrentItem(objects->topLevelItem(0));
        QTimer::singleShot(0, [&] {
            auto* dialog = dynamic_cast<QDialog*>(QApplication::activeModalWidget());
            try {
                check(dialog, "Host object properties dialog");
                dialog->resize(dialog->width(), 300);
                QApplication::processEvents();
                auto* scroll = child<QScrollArea>(*dialog, "objectPropertyScroll");
                check(scroll->widgetResizable() && scroll->viewport()->height() <
                                                       scroll->widget()->minimumSizeHint().height(),
                      "Large host forms remain reachable through scrolling");
                child<QComboBox>(*dialog, "kind")->setCurrentText("Inspectable");
                click(*dialog, "applyObjectProperties");
                if (dialog->isVisible())
                    throw std::runtime_error(
                        child<QLabel>(*dialog, "objectPropertyStatus")->text().toStdString());
            } catch (const std::exception& e) {
                callbackError = e.what();
                if (dialog)
                    dialog->done(QDialog::Rejected);
            }
        });
        action("editObjectProperties");
        check(callbackError.empty(), callbackError.c_str());
        action("saveAll");
        const auto textFile = assets / "sequence.txt";
        std::ofstream(textFile) << "ok original\n";
        editor::TextAssetEditor text(textFile, assets, type);
        text.restoreSource("ok edited\n");
        text.save();
        child<QPlainTextEdit>(text, "textAssetSource")->undo();
        check(text.snapshot() == "ok original\n", "Text Undo survives Save");
        text.restoreSource("invalid draft");
        bool rejected = false;
        try {
            (void)text.snapshot();
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected && text.recoverySource() == "invalid draft",
              "Invalid script draft remains recoverable");
        text.restoreSource("ok later\n");
        std::ofstream(textFile) << "ok external\n";
        rejected = false;
        try {
            text.save();
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected && bytes(textFile) == "ok external\n",
              "Text Save protects external changes");
        std::cout << "Complete scene authoring and recovery invariants passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
