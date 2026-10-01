#include "../../editor/audio_bank_editor.hpp"
#include "../../editor/project_migration.hpp"
#include "../../editor/property_form.hpp"
#include "../../editor/ui_editor.hpp"
#include "paper/content/document.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <fstream>
#include <iostream>
using namespace paper;
namespace fs = std::filesystem;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        auto check = [](bool value, const char* message) {
            if (!value)
                throw std::runtime_error(message);
        };
        auto rejects = [](auto operation) {
            try {
                operation();
                return false;
            } catch (const std::exception&) {
                return true;
            }
        };
        QTemporaryDir temporary;
        const auto root = fs::path(temporary.path().toStdString());
        auto write = [](const fs::path& path, const std::string& bytes) {
            std::ofstream(path, std::ios::binary) << bytes;
        };
        auto click = [&](QWidget& widget, const char* name) {
            auto* button = widget.findChild<QPushButton*>(name);
            check(button && button->isEnabled(), name);
            button->click();
        };
        ui::Document document;
        document.root.id = "root";
        document.viewport = {320, 240};
        document.root.layout.padding = {7, 7, 7, 7};
        document.root.layout.gap = 5;
        document.root.layout.scroll = true;
        for (auto kind : {ui::Kind::Label, ui::Kind::Button, ui::Kind::Toggle, ui::Kind::Slider,
                          ui::Kind::List}) {
            ui::Node node;
            node.id = "component." + std::to_string(static_cast<int>(kind));
            node.kind = kind;
            node.text = "Пример UI";
            node.layout.height.preferred = 32;
            if (kind == ui::Kind::List) {
                node.items = {"One", "Two"};
                node.value = -1;
            }
            document.root.children.push_back(node);
        }
        document.theme.accent = {10, 20, 30};
        const auto encoded = ui::writeDocument(document);
        check(ui::documentValue(ui::parseDocument(encoded)) == ui::documentValue(document),
              "UI asset round trip includes every component/theme/layout");
        auto bad = ui::documentValue(document);
        bad["ui"]["nodes"][1]["parent"] = "missing";
        check(rejects([&] { (void)ui::readDocument(bad); }), "missing UI parent rejected");
        bad = ui::documentValue(document);
        bad["ui"]["nodes"][1]["parent"] = "component.1";
        check(rejects([&] { (void)ui::readDocument(bad); }), "disconnected UI cycle rejected");
        bad = ui::documentValue(document);
        bad["ui"]["nodes"][0]["unknown"] = true;
        check(rejects([&] { (void)ui::readDocument(bad); }), "unknown UI fields rejected");
        bad = ui::documentValue(document);
        bad["ui"]["viewport"][0] = 99999;
        check(rejects([&] { (void)ui::readDocument(bad); }), "unbounded UI viewport rejected");
        editor::PropertyForm form;
        const ContentValue precise{{"number", .123456789},
                                   {"items", ContentValue::array({""})},
                                   {"width", ContentValue{{"maximum", "unlimited"}}}};
        form.setValue(precise);
        check(!form.dirty() && form.value() == precise,
              "untouched property forms retain exact numbers and empty list items");
        auto* maximum = form.findChild<QDoubleSpinBox*>("width.maximum");
        maximum->setValue(90);
        check(form.value().at("width").at("maximum").get<double>() == 90,
              "finite layout maximum can replace unlimited");
        form.setValue(ContentValue{{"half", ContentValue::array({1, 2, 3})}});
        form.findChild<QDoubleSpinBox*>("half.0")->setValue(2.5);
        check(form.value().at("half")[0].get<double>() == 2.5,
              "authored integral geometry can be edited in fractional metres");
        const auto file = root / "screen.pui";
        write(file, "# authored UI\n" + encoded);
        editor::UiEditor uiEditor(file, root);
        auto* source = uiEditor.findChild<QPlainTextEdit*>("uiSource");
        auto* preview = dynamic_cast<editor::UiPreview*>(uiEditor.findChild<QWidget*>("uiPreview"));
        check(preview && !preview->context().draw().empty(),
              "preview renders the common layout commands");
        uiEditor.save();
        check(content::read(file) == ui::documentValue(document), "no-op Save keeps UI semantics");
        auto* palette = uiEditor.findChild<QComboBox*>("uiPalette");
        palette->setCurrentIndex(2);
        click(uiEditor, "addUiNode");
        auto* label = uiEditor.findChild<QLineEdit*>("text");
        check(label, "selected component Inspector exists");
        label->setText("Play now");
        click(uiEditor, "applyUiProperties");
        check(ui::parseDocument(source->toPlainText().toStdString()).root.children.back().text ==
                  "Play now",
              "structured text edit updates the UI asset");
        click(uiEditor, "duplicateUiNode");
        check(ui::parseDocument(source->toPlainText().toStdString()).root.children.size() == 7,
              "duplicate creates a stable separate UI node");
        uiEditor.save();
        click(uiEditor, "undoUi");
        check(ui::parseDocument(source->toPlainText().toStdString()).root.children.size() == 6,
              "UI Undo survives Save");
        click(uiEditor, "redoUi");
        auto* hierarchy = uiEditor.findChild<QTreeWidget*>("uiHierarchy");
        auto* rootItem = hierarchy->topLevelItem(1);
        hierarchy->setCurrentItem(rootItem->child(rootItem->childCount() - 1));
        click(uiEditor, "deleteUiNode");
        check(ui::parseDocument(source->toPlainText().toStdString()).root.children.size() == 6,
              "Delete removes the selected duplicate after Redo");
        uiEditor.save();
        rootItem = hierarchy->topLevelItem(1);
        hierarchy->setCurrentItem(rootItem->child(rootItem->childCount() - 1));
        const auto beforeDraft = source->toPlainText();
        uiEditor.findChild<QComboBox*>("kind")->setCurrentText("slider");
        uiEditor.findChild<QDoubleSpinBox*>("step")->setValue(0);
        click(uiEditor, "applyUiProperties");
        check(source->toPlainText() == beforeDraft,
              "invalid property drafts retain the last authored UI");
        click(uiEditor, "undoUi");
        check(uiEditor.findChild<QComboBox*>("kind")->currentText() == "button",
              "Undo discards an unapplied invalid draft");
        uiEditor.findChild<QLineEdit*>("text")->setText("Final caption");
        uiEditor.save();
        check(ui::loadDocument(file).root.children.back().text == "Final caption",
              "Save applies pending component properties");
        hierarchy->setCurrentItem(hierarchy->topLevelItem(0));
        uiEditor.findChild<QDoubleSpinBox*>("viewport.0")->setValue(512);
        click(uiEditor, "applyUiProperties");
        uiEditor.save();
        check(preview->width() == 512 && ui::loadDocument(file).viewport.x == 512,
              "viewport editing relayouts and persists the preview size");
        uiEditor.findChild<QDoubleSpinBox*>("viewport.0")->setValue(0);
        hierarchy->setCurrentItem(hierarchy->topLevelItem(1));
        check(hierarchy->currentItem() == hierarchy->topLevelItem(0),
              "invalid theme draft preserves hierarchy selection");
        click(uiEditor, "undoUi");
        auto namedTheme = document;
        namedTheme.root.id = "@theme";
        const auto namedFile = root / "named.pui";
        write(namedFile, ui::writeDocument(namedTheme));
        editor::UiEditor namedEditor(namedFile, root);
        check(namedEditor.findChild<QLineEdit*>("id")->text() == "@theme",
              "component IDs cannot collide with the editor theme entry");
        const auto authored = source->toPlainText();
        if (argc == 2) {
            uiEditor.show();
            QApplication::processEvents();
            check(uiEditor.grab().save(QString::fromUtf8(argv[1])),
                  "capture UI editor for visual inspection");
        }
        preview->interactive(true);
        const auto bounds = preview->context().box("component.3").bounds;
        const QPointF position(bounds.x + 8, bounds.y + 8);
        QMouseEvent down(QEvent::MouseButtonPress, position, position, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease, position, position, Qt::LeftButton, Qt::NoButton,
                       Qt::NoModifier);
        QApplication::sendEvent(preview, &down);
        QApplication::sendEvent(preview, &up);
        check(preview->context().node("component.3").checked && source->toPlainText() == authored,
              "interactive toggle preview never modifies authored state");
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(preview, &tab);
        check(!preview->context().focus().empty(),
              "preview routes Tab into shared focus navigation");
        const auto saved = source->toPlainText().toStdString();
        source->setPlainText("invalid");
        check(rejects([&] { uiEditor.save(); }), "invalid UI edits cannot replace saved content");
        source->setPlainText(QString::fromStdString(saved));
        write(file, "# external\n" + saved);
        check(rejects([&] { uiEditor.save(); }), "external UI changes cannot be overwritten");
        const auto bankFile = root / "audio.pabank";
        write(bankFile, writeAudioBank({}));
        editor::AudioBankEditor audio(bankFile, root);
        audio.sceneNodes({"node.one"});
        auto* audioKind = audio.findChild<QComboBox*>("audioEntryKind");
        click(audio, "addAudioEntry");
        auto* soundId = audio.findChild<QLineEdit*>("id");
        soundId->setText("beep");
        auto* stem = audio.findChild<QLineEdit*>("file");
        stem->setText("beep");
        click(audio, "applyAudioProperties");
        audioKind->setCurrentIndex(1);
        click(audio, "addAudioEntry");
        auto* node = audio.findChild<QComboBox*>("node");
        check(node && node->findText("node.one") >= 0, "audio source binding offers scene nodes");
        node->setCurrentText("node.one");
        auto* offset = audio.findChild<QDoubleSpinBox*>("offset.1");
        offset->setValue(2.5);
        click(audio, "applyAudioProperties");
        audioKind->setCurrentIndex(2);
        click(audio, "addAudioEntry");
        auto* wet = audio.findChild<QDoubleSpinBox*>("wet");
        wet->setValue(.4);
        click(audio, "applyAudioProperties");
        auto* audioSource = audio.findChild<QPlainTextEdit*>("audioBankSource");
        auto bank = parseAudioBank(audioSource->toPlainText().toStdString());
        check(bank.sources.front().node == "node.one" && bank.sources.front().offset.y == 2.5f &&
                  std::abs(bank.zones.front().wet - .4f) < .0001f,
              "structured bank stores sources and acoustic parameters");
        bool overlay = false;
        audio.previewChanged = [&](std::optional<AudioBankDefinition> state) {
            overlay = state && state->zones.size() == 1;
        };
        audio.preview();
        check(overlay, "authored bank publishes a bounded viewport preview");
        fs::create_directory(root / "audio");
        std::string wav = "RIFF";
        auto word = [&](uint32_t v, int bytes) {
            for (int i = 0; i < bytes; ++i)
                wav.push_back(static_cast<char>(v >> (8 * i)));
        };
        word(36 + 1024, 4);
        wav += "WAVEfmt ";
        word(16, 4);
        word(1, 2);
        word(1, 2);
        word(44100, 4);
        word(88200, 4);
        word(2, 2);
        word(16, 2);
        wav += "data";
        word(1024, 4);
        for (int sample = 0; sample < 512; ++sample)
            word(0, 2);
        write(root / "audio" / "beep-1.wav", wav);
        auto* audioEntries = audio.findChild<QTreeWidget*>("audioEntries");
        audioEntries->setCurrentItem(audioEntries->topLevelItem(0)->child(0));
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        audio.audition();
        check(audio.isAuditioning(), "audition opens a validated dummy audio device");
        audio.stopAudition();
        check(!audio.isAuditioning(), "Stop joins and releases the audition stream");
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audioEntries->setCurrentItem(audioEntries->topLevelItem(2)->child(0));
        audio.save();
        if (argc == 2) {
            audio.show();
            QApplication::processEvents();
            check(audio.grab().save(QString::fromUtf8(argv[1]) + ".audio.png"),
                  "capture audio editor for visual inspection");
        }
        audio.currentSceneNodes = [] { return std::vector<std::string>{}; };
        check(rejects([&] { audio.validate(); }),
              "validation rejects bindings to nodes removed since the bank was opened");
        audio.currentSceneNodes = {};
        click(audio, "undoAudioEntry");
        check(parseAudioBank(audioSource->toPlainText().toStdString()).zones.front().wet != .4f,
              "audio property Undo survives Save");
        click(audio, "redoAudioEntry");
        const auto sourceProject = root / "original";
        fs::create_directory(sourceProject);
        fs::copy(PAPER_EXAMPLE_PROJECT, sourceProject, fs::copy_options::recursive);
        const auto original = content::read(sourceProject / "assets/boxes.dcscene");
        check(ui::loadDocument(sourceProject / "assets/settings.pui").root.children.size() == 5,
              "shipped UI example is loadable");
        editor::AudioBankEditor exampleBank(sourceProject / "assets/example.pabank",
                                            sourceProject / "assets");
        exampleBank.sceneNodes({"box.parent"});
        exampleBank.validate();
        auto edited = original;
        edited["scene"]["nodes"][0]["yaw"] = .5;
        const auto destination = root / "migrated";
        const auto project = editor::migrateProject(sourceProject / "boxes.paperproject",
                                                    destination, {{"boxes.dcscene", edited}});
        const auto scene = content::read(destination / "assets/boxes.dcscene");
        check(fs::exists(project) && scene.at("version") == 3 &&
                  scene.at("scene").at("nodes")[0].contains("rotation") &&
                  content::read(sourceProject / "assets/boxes.dcscene") == original,
              "editor migration captures unsaved overrides and preserves originals");
        check(rejects([&] {
                  (void)editor::migrateProject(sourceProject / "boxes.paperproject", destination);
              }),
              "existing migration destination protected");
        const auto invalidDestination = root / "failed";
        check(rejects([&] {
                  (void)editor::migrateProject(
                      sourceProject / "boxes.paperproject", invalidDestination, {},
                      [](const auto&) { throw std::runtime_error("validation failed"); });
              }) &&
                  !fs::exists(invalidDestination),
              "failed project validation leaves no published copy");
        fs::create_symlink(sourceProject / "assets/boxes.dcscene",
                           sourceProject / "assets/link.dcscene");
        check(rejects([&] {
                  (void)editor::migrateProject(sourceProject / "boxes.paperproject",
                                               root / "linked");
              }),
              "symlink migration rejected");
        std::cout << "Editor component checks passed: UI documents/design/input/persistence, "
                     "structured audio/scene binding, project-copy migration\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
