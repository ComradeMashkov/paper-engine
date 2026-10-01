#include "audio_bank_editor.hpp"
#include "paper/audio/audio.hpp"
#include "paper/audio/bank.hpp"
#include "paper/resources/resource_store.hpp"
#include "property_form.hpp"
#include <QCloseEvent>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include <QVBoxLayout>
namespace paper::editor {
namespace {
constexpr qint64 maximumDocumentBytes = 4 * 1024 * 1024;
QString pathText(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}
QByteArray read(const std::filesystem::path& path) {
    QFile file(pathText(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumDocumentBytes)
        throw std::runtime_error("Cannot read audio bank or document budget exceeded");
    const auto result = file.readAll();
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error("Cannot read audio bank");
    return result;
}
} // namespace
AudioBankEditor::AudioBankEditor(std::filesystem::path file, std::filesystem::path assets,
                                 QWidget* parent)
    : QDialog(parent), assets_(std::filesystem::canonical(assets)) {
    ResourceStore files(assets_);
    file_ = files.resolve(std::filesystem::canonical(file).lexically_relative(assets_));
    baseline_ = read(file_);
    if (QString::fromUtf8(baseline_).toUtf8() != baseline_)
        throw std::invalid_argument("Audio bank must be valid UTF-8");
    setWindowTitle(tr("Audio Bank — ") + pathText(file_.filename()));
    setObjectName("audioBankEditor");
    constexpr int width = 800, height = 600;
    resize(width, height);
    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(
        tr("Create sounds, bind sources to scene nodes and define acoustic "
           "zones. Apply Properties commits one Undo step; Save checks every WAV variant."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    source_ = new QPlainTextEdit;
    source_->setObjectName("audioBankSource");
    source_->setPlainText(QString::fromUtf8(baseline_));
    displayedBaseline_ = source_->toPlainText();
    auto* tabs = new QTabWidget;
    auto* design = new QWidget;
    auto* designLayout = new QVBoxLayout(design);
    auto* tools = new QHBoxLayout;
    kind_ = new QComboBox;
    kind_->setObjectName("audioEntryKind");
    kind_->addItems({"sounds", "sources", "zones"});
    tools->addWidget(kind_);
    for (const auto& [name, title, action] :
         std::initializer_list<std::tuple<const char*, QString, std::function<void()>>>{
             {"addAudioEntry", tr("Add"), [this] { add(); }},
             {"deleteAudioEntry", tr("Delete"), [this] { remove(); }},
             {"undoAudioEntry", tr("Undo"),
              [this] {
                  if (form_->isEnabled() && form_->dirty())
                      inspect();
                  else
                      source_->undo();
              }},
             {"redoAudioEntry", tr("Redo"), [this] { source_->redo(); }}}) {
        auto* button = new QPushButton(title);
        button->setObjectName(name);
        tools->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, action] { report(action); });
    }
    designLayout->addLayout(tools);
    auto* audition = new QPushButton(tr("Audition Selected"));
    audition->setObjectName("auditionAudio");
    tools->addWidget(audition);
    auto* stop = new QPushButton(tr("Stop"));
    stop->setObjectName("stopAudioAudition");
    tools->addWidget(stop);
    connect(audition, &QPushButton::clicked, this, [this] {
        try {
            this->audition();
            status_->setText(tr("Playing selected sound; Stop ends the audition"));
        } catch (const std::exception& e) {
            status_->setText(QString::fromUtf8(e.what()));
        }
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        stopAudition();
        status_->setText(tr("Audition stopped"));
    });
    auto* choose = new QPushButton(tr("Choose WAV…"));
    choose->setObjectName("chooseAudioWav");
    tools->addWidget(choose);
    connect(choose, &QPushButton::clicked, this, [this] { report([this] { chooseWav(); }); });
    auto* split = new QSplitter;
    entries_ = new QTreeWidget;
    entries_->setObjectName("audioEntries");
    entries_->setHeaderLabel(tr("Sounds / Sources / Zones"));
    split->addWidget(entries_);
    form_ = new PropertyForm;
    form_->setObjectName("audioProperties");
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(form_);
    split->addWidget(scroll);
    split->setStretchFactor(1, 1);
    designLayout->addWidget(split);
    auto* apply = new QPushButton(tr("Apply Properties"));
    apply->setObjectName("applyAudioProperties");
    designLayout->addWidget(apply);
    tabs->addTab(design, tr("Structure"));
    tabs->addTab(source_, tr("Source"));
    layout->addWidget(tabs);
    status_ = new QLabel;
    status_->setObjectName("audioBankStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* check = new QPushButton(tr("Validate"));
    check->setObjectName("validateAudioBank");
    layout->addWidget(check);
    auto* saveButton = new QPushButton(tr("Save"));
    saveButton->setObjectName("saveAudioBank");
    layout->addWidget(saveButton);
    auto* closeButton = new QPushButton(tr("Close"));
    layout->addWidget(closeButton);
    connect(check, &QPushButton::clicked, this, [this] {
        report(
            [this] {
                if (form_->isEnabled() && form_->dirty())
                    this->apply();
                validate();
            },
            true);
    });
    connect(saveButton, &QPushButton::clicked, this, [this] { report([this] { save(); }, true); });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(source_, &QPlainTextEdit::textChanged, this, [this] { rebuild(); });
    connect(entries_, &QTreeWidget::currentItemChanged, this, [this] {
        const auto next =
            entries_->currentItem()
                ? entries_->currentItem()->data(0, Qt::UserRole).toString().toStdString()
                : "";
        if (form_->isEnabled() && form_->dirty()) {
            try {
                this->apply();
            } catch (const std::exception& e) {
                status_->setText(QString::fromUtf8(e.what()));
                QSignalBlocker blocked(entries_);
                for (QTreeWidgetItemIterator i(entries_); *i; ++i)
                    if ((*i)->data(0, Qt::UserRole).toString().toStdString() == selected_)
                        entries_->setCurrentItem(*i);
                return;
            }
        }
        selected_ = next;
        inspect();
        QSignalBlocker blocked(entries_);
        for (QTreeWidgetItemIterator i(entries_); *i; ++i)
            if ((*i)->data(0, Qt::UserRole).toString().toStdString() == next)
                entries_->setCurrentItem(*i);
    });
    connect(apply, &QPushButton::clicked, this, [this] { report([this] { this->apply(); }); });
    for (auto* button : findChildren<QPushButton*>())
        button->setAutoDefault(false);
    apply->setDefault(true);
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this,
            [this] { report([this] { save(); }, true); });
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index) {
        if (index == 1 && form_->isEnabled() && form_->dirty()) {
            try {
                this->apply();
            } catch (const std::exception& e) {
                status_->setText(QString::fromUtf8(e.what()));
                QSignalBlocker blocked(tabs);
                tabs->setCurrentIndex(0);
            }
        }
    });
    rebuild();
}
void AudioBankEditor::sceneNodes(std::vector<std::string> nodes) {
    nodes_ = std::move(nodes);
    hasNodes_ = true;
    if (!form_->dirty())
        inspect();
}
void AudioBankEditor::preview() {
    if (!previewChanged)
        return;
    try {
        previewChanged(
            parseAudioBank(source_->toPlainText().toUtf8().toStdString(), file_.string()));
    } catch (const std::exception&) {
        previewChanged(std::nullopt);
    }
}
void AudioBankEditor::audition() {
    if (form_->isEnabled() && form_->dirty())
        apply();
    const auto bank = parseAudioBank(source_->toPlainText().toUtf8().toStdString(), file_.string());
    const auto slash = selected_.find('/');
    if (slash == std::string::npos)
        throw std::runtime_error("Select a sound, source or ambience zone");
    const auto key = selected_.substr(0, slash), id = selected_.substr(slash + 1);
    std::string sound = id;
    if (key == "sources") {
        const auto found = std::ranges::find(bank.sources, id, &AudioSourceDefinition::id);
        if (found == bank.sources.end())
            throw std::runtime_error("Missing source selection");
        sound = found->sound;
    } else if (key == "zones") {
        const auto found = std::ranges::find(bank.zones, id, &AcousticZone::id);
        if (found == bank.zones.end())
            throw std::runtime_error("Missing zone selection");
        sound = found->ambience;
    }
    const auto definition = std::ranges::find(bank.sounds, sound, &SoundDefinition::id);
    if (definition == bank.sounds.end())
        throw std::runtime_error("Selection has no sound to audition");
    auto audio = std::make_unique<Audio>(false);
    audio->openBank(assets_, bank);
    if (!audio->available())
        throw std::runtime_error("Audio device is unavailable");
    AudioScene scene;
    scene.active = true;
    scene.reverbWet = 0;
    audio->update(scene);
    SoundPlacement placement;
    placement.preview = true;
    if (definition->loop)
        audio->loop(0, sound, placement);
    else
        audio->play(sound, placement);
    audition_ = std::move(audio);
}
void AudioBankEditor::rebuild() {
    stopAudition();
    try {
        const auto bank =
            parseAudioBank(source_->toPlainText().toUtf8().toStdString(), file_.string());
        structure_ = content::parse(writeAudioBank(bank), file_.string());
        QSignalBlocker blocked(entries_);
        entries_->clear();
        for (const auto key : {"sounds", "sources", "zones"}) {
            auto* group = new QTreeWidgetItem(entries_, {tr(key)});
            for (const auto& entry : structure_.at("bank").at(key)) {
                const auto id = entry.at("id").get<std::string>();
                auto* item = new QTreeWidgetItem(group, {QString::fromStdString(id)});
                const auto selection = std::string(key) + "/" + id;
                item->setData(0, Qt::UserRole, QString::fromStdString(selection));
                if (selection == selected_)
                    entries_->setCurrentItem(item);
            }
        }
        entries_->expandAll();
        entries_->setEnabled(true);
        inspect();
        status_->setText(tr("Definitions are valid. Save validates WAV files and scene bindings."));
    } catch (const std::exception& e) {
        structure_ = {};
        entries_->setEnabled(false);
        form_->setEnabled(false);
        status_->setText(QString::fromUtf8(e.what()));
    }
    preview();
}
void AudioBankEditor::inspect() {
    form_->setEnabled(false);
    if (!structure_.is_object() || selected_.empty())
        return;
    const auto slash = selected_.find('/');
    if (slash == std::string::npos)
        return;
    const auto key = selected_.substr(0, slash), id = selected_.substr(slash + 1);
    const auto& entries = structure_.at("bank").at(key);
    const auto found =
        std::ranges::find_if(entries, [&](const auto& e) { return e.at("id") == id; });
    if (found == entries.end())
        return;
    PropertyForm::Choices choices{{"bus", {"ambience", "effects", "interface", "voices"}},
                                  {"sound", {}},
                                  {"ambience", {""}},
                                  {"node", {""}}};
    for (const auto& s : structure_.at("bank").at("sounds")) {
        choices["sound"].push_back(QString::fromStdString(s.at("id").get<std::string>()));
        if (s.at("loop").get<bool>())
            choices["ambience"].push_back(QString::fromStdString(s.at("id").get<std::string>()));
    }
    for (const auto& n : nodes_)
        choices["node"].push_back(QString::fromStdString(n));
    form_->setValue(*found, std::move(choices));
    if (auto* file = form_->findChild<QLineEdit*>("file"))
        file->setToolTip(
            tr("WAV stem relative to audio/. Variants are stem-1.wav, stem-2.wav, etc."));
    form_->setEnabled(true);
}
void AudioBankEditor::replace(ContentValue value, std::string selected) {
    if (value == structure_)
        return;
    const auto encoded = content::encode(value);
    (void)parseAudioBank(encoded, file_.string());
    if (QString::fromStdString(encoded) == source_->toPlainText())
        return;
    selected_ = std::move(selected);
    auto cursor = source_->textCursor();
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(QString::fromStdString(encoded));
    cursor.endEditBlock();
}
void AudioBankEditor::apply() {
    if (!structure_.is_object() || !form_->isEnabled())
        throw std::runtime_error("Select an audio entry");
    const auto slash = selected_.find('/');
    const auto key = selected_.substr(0, slash), id = selected_.substr(slash + 1);
    auto candidate = structure_;
    auto edited = form_->value();
    const auto next = edited.at("id").get<std::string>();
    for (auto& e : candidate["bank"][key])
        if (e.at("id") == id)
            e = edited;
    if (key == "sounds" && next != id) {
        for (auto& s : candidate["bank"]["sources"])
            if (s.at("sound") == id)
                s["sound"] = next;
        for (auto& z : candidate["bank"]["zones"])
            if (z.at("ambience") == id)
                z["ambience"] = next;
    }
    replace(std::move(candidate), key + "/" + next);
}
void AudioBankEditor::add() {
    if (form_->isEnabled() && form_->dirty())
        apply();
    if (!structure_.is_object())
        throw std::runtime_error("Repair audio source first");
    auto bank = parseAudioBank(source_->toPlainText().toUtf8().toStdString(), file_.string());
    const auto key = kind_->currentText().toStdString(),
               id = "audio." + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    if (key == "sounds") {
        bank.sounds.push_back({id, "sound", AudioBus::Effects, 1, 1, 0, 0});
    } else if (key == "sources") {
        if (bank.sounds.empty())
            throw std::runtime_error("Create a sound before binding a source");
        AudioSourceDefinition source;
        source.id = id;
        source.sound = bank.sounds.front().id;
        bank.sources.push_back(source);
    } else {
        AcousticZone zone;
        zone.id = id;
        zone.bounds.half = {1, 1, 1};
        bank.zones.push_back(zone);
    }
    replace(content::parse(writeAudioBank(bank), file_.string()), key + "/" + id);
}
void AudioBankEditor::chooseWav() {
    if (!selected_.starts_with("sounds/") || !form_->isEnabled())
        throw std::runtime_error("Select a sound first");
    const auto choice =
        QFileDialog::getOpenFileName(this, tr("First WAV Variant"), pathText(assets_ / "audio"),
                                     tr("First WAV variant (*-1.wav)"));
    if (choice.isEmpty())
        return;
    const auto root = std::filesystem::canonical(assets_ / "audio");
    ResourceStore files(root);
    auto file =
        files
            .resolve(std::filesystem::canonical(std::filesystem::path(choice.toStdU16String()))
                         .lexically_relative(root))
            .lexically_relative(root);
    auto stem = file.replace_extension().generic_string();
    constexpr size_t variantSuffixCharacters = 2;
    if (!stem.ends_with("-1"))
        throw std::runtime_error("Choose the first numbered WAV variant (-1.wav)");
    stem.resize(stem.size() - variantSuffixCharacters);
    form_->findChild<QLineEdit*>("file")->setText(QString::fromStdString(stem));
    apply();
}
void AudioBankEditor::remove() {
    if (!structure_.is_object() || selected_.empty())
        return;
    const auto slash = selected_.find('/');
    if (slash == std::string::npos)
        return;
    const auto key = selected_.substr(0, slash), id = selected_.substr(slash + 1);
    auto candidate = structure_;
    std::erase_if(candidate["bank"][key].elements(),
                  [&](const auto& e) { return e.at("id") == id; });
    replace(std::move(candidate), "");
}
void AudioBankEditor::validate() const {
    const auto source = source_->toPlainText().toUtf8();
    if (source.size() > maximumDocumentBytes)
        throw std::invalid_argument("Audio bank document exceeds budget");
    const auto bank = parseAudioBank(
        std::string_view(source.constData(), static_cast<size_t>(source.size())), file_.string());
    const auto nodes = currentSceneNodes ? currentSceneNodes() : nodes_;
    if (hasNodes_ || currentSceneNodes)
        for (const auto& binding : bank.sources)
            if (!binding.node.empty() && std::ranges::find(nodes, binding.node) == nodes.end())
                throw std::runtime_error("Unknown scene node binding: " + binding.node);
    (void)loadSoundBank(assets_, bank);
}
std::string AudioBankEditor::snapshot() {
    if (form_->isEnabled() && form_->dirty())
        apply();
    validate();
    if (read(file_) != baseline_)
        throw std::runtime_error("Audio bank changed externally; reopen before Play");
    return source_->toPlainText().toUtf8().toStdString();
}
void AudioBankEditor::save() {
    if (form_->isEnabled() && form_->dirty())
        apply();
    validate();
    if (read(file_) != baseline_)
        throw std::runtime_error("Audio bank changed externally; reopen before saving");
    if (source_->toPlainText() == displayedBaseline_)
        return;
    auto source = source_->toPlainText().toUtf8();
    if (baseline_.contains("\r\n"))
        source.replace("\n", "\r\n");
    QSaveFile file(pathText(file_));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(source) != source.size() ||
        read(file_) != baseline_ || !file.commit())
        throw std::runtime_error("Cannot atomically save audio bank");
    baseline_ = source;
    displayedBaseline_ = source_->toPlainText();
    source_->document()->setModified(false);
}
void AudioBankEditor::report(const std::function<void()>& operation, bool verified) {
    try {
        operation();
        status_->setText(verified ? tr("Audio bank, WAV assets and scene bindings are valid")
                                  : tr("Audio bank updated"));
    } catch (const std::exception& e) {
        status_->setText(QString::fromUtf8(e.what()));
    }
}
bool AudioBankEditor::discardOrSave() {
    if (source_->toPlainText() == displayedBaseline_ && (!form_->isEnabled() || !form_->dirty()))
        return true;
    const auto choice = QMessageBox::question(
        this, tr("Unsaved Audio Bank"), tr("Save audio bank changes before closing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice == QMessageBox::Discard)
        return true;
    if (choice != QMessageBox::Save)
        return false;
    try {
        save();
        return true;
    } catch (const std::exception& e) {
        status_->setText(QString::fromUtf8(e.what()));
        return false;
    }
}
void AudioBankEditor::closeEvent(QCloseEvent* event) {
    if (discardOrSave()) {
        stopAudition();
        event->accept();
        if (closed)
            closed();
    } else
        event->ignore();
}
void AudioBankEditor::reject() {
    if (discardOrSave()) {
        stopAudition();
        QDialog::reject();
        if (closed)
            closed();
    }
}
} // namespace paper::editor
