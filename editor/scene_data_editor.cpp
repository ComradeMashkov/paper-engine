#include "scene_data_editor.hpp"
#include "paper/authoring/document.hpp"
#include "paper/content/document.hpp"
#include "property_form.hpp"
#include <QCloseEvent>
#include <QComboBox>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <array>

namespace paper::editor {
namespace {
QString text(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
constexpr int dialogWidth = 1050, dialogHeight = 720;
constexpr double initialLightRangeMeters = 10, initialLightAttenuationPerMeterSquared = .1;
const std::array<std::pair<const char*, const char*>, 4> sections{
    {{"rooms", "Rooms"},
     {"lights", "Lights"},
     {"spawns", "Spawns"},
     {"zones", "Transitions / Triggers"}}};
ContentValue defaults(std::string_view section) {
    const auto xyz = ContentValue::array({0.0, 0.0, 0.0});
    const ContentValue bounds{
        {"center", xyz}, {"half", ContentValue::array({1.0, 1.0, 1.0})}, {"yaw", 0.0}};
    if (section == "rooms")
        return {{"id", ""}, {"label", "Room"}, {"floorY", 0.0}, {"bounds", bounds}};
    if (section == "spawns")
        return {{"id", ""}, {"position", xyz}, {"yaw", 0.0}};
    if (section == "zones")
        return {{"id", ""}, {"bounds", bounds}, {"targetSpawn", ""}};
    return {{"id", ""},
            {"position", xyz},
            {"color", ContentValue::array({1.0, 1.0, 1.0})},
            {"intensity", 1.0},
            {"range", initialLightRangeMeters},
            {"attenuation", initialLightAttenuationPerMeterSquared}};
}
} // namespace
SceneDataEditor::SceneDataEditor(std::string source,
                                 std::function<bool(const ContentValue&, const std::string&)> apply,
                                 QWidget* parent)
    : QDialog(parent), apply_(std::move(apply)), saved_(std::move(source)) {
    setWindowTitle(tr("Scene Data"));
    resize(dialogWidth, dialogHeight);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Positions, bounds and range: metres. Yaw: radians. "
                                    "Transition destination may be empty for a story trigger.")));
    auto* tabs = new QTabWidget;
    auto* split = new QSplitter;
    auto* hierarchy = new QWidget;
    auto* left = new QVBoxLayout(hierarchy);
    palette_ = new QComboBox;
    palette_->setObjectName("sceneDataPalette");
    for (const auto& [key, label] : sections)
        palette_->addItem(tr(label), key);
    left->addWidget(palette_);
    tree_ = new QTreeWidget;
    tree_->setObjectName("sceneDataHierarchy");
    tree_->setHeaderLabel(tr("Scene entries"));
    left->addWidget(tree_);
    split->addWidget(hierarchy);
    auto* right = new QWidget;
    auto* properties = new QVBoxLayout(right);
    form_ = new PropertyForm;
    form_->setObjectName("sceneDataProperties");
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(form_);
    properties->addWidget(scroll);
    split->addWidget(right);
    tabs->addTab(split, tr("Properties"));
    source_ = new QPlainTextEdit;
    source_->setObjectName("sceneDataSource");
    source_->setPlainText(text(saved_));
    tabs->addTab(source_, tr("Source"));
    layout->addWidget(tabs);
    status_ = new QLabel;
    status_->setObjectName("sceneDataStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QHBoxLayout;
    for (const auto& [name, label, action] :
         std::initializer_list<std::tuple<const char*, const char*, std::function<void()>>>{
             {"addSceneData", "Add", [this] { add(); }},
             {"duplicateSceneData", "Duplicate", [this] { duplicate(); }},
             {"deleteSceneData", "Delete", [this] { remove(); }},
             {"applySceneData", "Apply to Scene", [this] { this->apply(); }}}) {
        auto* button = new QPushButton(tr(label));
        button->setObjectName(name);
        button->setAutoDefault(false);
        buttons->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, action] { report(action); });
    }
    auto* close = new QPushButton(tr("Close"));
    close->setAutoDefault(false);
    buttons->addWidget(close);
    connect(close, &QPushButton::clicked, this, &SceneDataEditor::reject);
    layout->addLayout(buttons);
    connect(source_, &QPlainTextEdit::textChanged, this, [this] { rebuild(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] {
        auto* item = tree_->currentItem();
        const auto nextSection = item ? item->data(0, Qt::UserRole).toString().toStdString() : "";
        const auto next = item ? item->data(0, Qt::UserRole + 1).toString().toStdString() : "";
        if (form_->isEnabled() && form_->dirty()) {
            try {
                publish(draft());
            } catch (const std::exception& e) {
                status_->setText(text(e.what()));
                QSignalBlocker block(tree_);
                for (auto* old :
                     tree_->findItems(text(selected_), Qt::MatchExactly | Qt::MatchRecursive))
                    if (old->data(0, Qt::UserRole).toString() == text(section_))
                        tree_->setCurrentItem(old);
                return;
            }
        }
        section_ = nextSection;
        selected_ = next;
        inspect();
    });
    rebuild();
}
void SceneDataEditor::choices(std::vector<std::string> spawns) {
    spawns_ = std::move(spawns);
    inspect();
}
void SceneDataEditor::restoreSource(const std::string& source) {
    source_->setPlainText(text(source));
}
void SceneDataEditor::rebuild() {
    try {
        value_ = content::parse(source_->toPlainText().toUtf8().toStdString(), "scene draft");
        const auto& scene = value_.at("scene");
        QSignalBlocker block(tree_);
        tree_->clear();
        for (const auto& [key, label] : sections) {
            auto* group = new QTreeWidgetItem(tree_, {tr(label)});
            if (!scene.contains(key))
                continue;
            for (const auto& entry : scene.at(key)) {
                const auto id = entry.at("id").get<std::string>();
                auto* item = new QTreeWidgetItem(group, {text(id)});
                item->setData(0, Qt::UserRole, key);
                item->setData(0, Qt::UserRole + 1, text(id));
                if (section_ == key && selected_ == id)
                    tree_->setCurrentItem(item);
            }
        }
        tree_->expandAll();
        inspect();
        status_->clear();
    } catch (const std::exception& e) {
        value_ = {};
        form_->setEnabled(false);
        status_->setText(text(e.what()));
    }
}
void SceneDataEditor::inspect() {
    form_->setEnabled(false);
    if (!value_.is_object() || section_.empty() || selected_.empty())
        return;
    for (const auto& entry : value_.at("scene").at(section_))
        if (entry.at("id") == selected_) {
            auto value = defaults(section_);
            value.overlay(entry);
            // Avoid adding a legacy yaw when the authored bounds use a quaternion.
            if (value.contains("bounds") && value.at("bounds").contains("rotation"))
                value["bounds"].erase("yaw");
            PropertyForm::Choices choices;
            if (section_ == "zones") {
                choices["targetSpawn"] = {""};
                for (const auto& id : spawns_)
                    choices["targetSpawn"].push_back(text(id));
            }
            form_->setValue(value, std::move(choices));
            form_->setEnabled(true);
            return;
        }
}
ContentValue SceneDataEditor::draft() const {
    auto value = value_;
    if (form_->isEnabled() && form_->dirty()) {
        const auto edited = form_->value();
        for (auto& entry : value["scene"][section_])
            if (entry.at("id") == selected_) {
                entry = edited;
                break;
            }
    }
    return value;
}
std::string SceneDataEditor::recoverySource() const {
    if (!form_->isEnabled() || !form_->dirty()) {
        auto normalized = text(saved_);
        normalized.replace("\r\n", "\n");
        return source_->toPlainText() == normalized ? saved_
                                                    : source_->toPlainText().toUtf8().toStdString();
    }
    const auto value = draft();
    try {
        authoring::SceneDocument document({}, source_->toPlainText().toUtf8().toStdString());
        document.replaceData(value);
        return document.serialized();
    } catch (const std::exception&) {
        return content::encode(value);
    }
}
void SceneDataEditor::publish(ContentValue value) {
    const auto source = source_->toPlainText().toUtf8().toStdString();
    authoring::SceneDocument document({}, source);
    document.replaceData(value);
    const auto encoded = document.serialized();
    source_->setPlainText(text(encoded));
}
void SceneDataEditor::apply() {
    const auto value = draft();
    authoring::SceneDocument document({}, source_->toPlainText().toUtf8().toStdString());
    document.replaceData(value);
    const auto encoded = document.serialized();
    if (!apply_(value, encoded))
        throw std::runtime_error("Scene validation failed; draft remains editable");
    saved_ = encoded;
    selected_ = form_->isEnabled() ? form_->value().at("id").get<std::string>() : selected_;
    source_->setPlainText(text(encoded));
    rebuild();
    status_->setText(
        staged_ ? tr("Recovery draft staged; the complete project is validated after all drafts.")
                : tr("Applied to scene; save the project to write authored files."));
}
void SceneDataEditor::add() {
    auto value = draft();
    section_ = palette_->currentData().toString().toStdString();
    auto entry = defaults(section_);
    selected_ = section_ + "." + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    entry["id"] = selected_;
    if (!value["scene"].contains(section_))
        value["scene"][section_] = ContentValue::array();
    value["scene"][section_].push_back(entry);
    publish(std::move(value));
}
void SceneDataEditor::duplicate() {
    auto value = draft();
    if (section_.empty() || selected_.empty())
        return;
    for (const auto& entry : value.at("scene").at(section_))
        if (entry.at("id") == selected_) {
            auto copy = entry;
            selected_ += "." + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            copy["id"] = selected_;
            value["scene"][section_].push_back(copy);
            publish(std::move(value));
            return;
        }
}
void SceneDataEditor::remove() {
    auto value = draft();
    if (section_.empty() || selected_.empty())
        return;
    std::erase_if(value["scene"][section_].elements(),
                  [this](const auto& entry) { return entry.at("id") == selected_; });
    selected_.clear();
    publish(std::move(value));
}
void SceneDataEditor::report(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::exception& e) {
        status_->setText(text(e.what()));
    }
}
bool SceneDataEditor::confirm() {
    if (recoverySource() == saved_)
        return true;
    const auto answer = QMessageBox::question(
        this, tr("Unapplied Scene Data"), tr("Apply scene changes before closing?"),
        QMessageBox::Apply | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Discard)
        return true;
    if (answer != QMessageBox::Apply)
        return false;
    try {
        apply();
        return true;
    } catch (const std::exception& e) {
        status_->setText(text(e.what()));
        return false;
    }
}
void SceneDataEditor::reject() {
    if (confirm())
        QDialog::reject();
}
void SceneDataEditor::closeEvent(QCloseEvent* event) {
    if (confirm())
        event->accept();
    else
        event->ignore();
}
} // namespace paper::editor
