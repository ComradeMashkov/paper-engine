#include "audio_bank_editor.hpp"
#include "paper/audio/audio.hpp"
#include "paper/audio/bank.hpp"
#include "paper/resources/resource_store.hpp"
#include <QCloseEvent>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QTextDocument>
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
    auto* hint = new QLabel(tr("Edit sound definitions, source bindings and acoustic zones in "
                               "TOML. Validate checks every declared WAV variant before Save."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    source_ = new QPlainTextEdit;
    source_->setObjectName("audioBankSource");
    source_->setPlainText(QString::fromUtf8(baseline_));
    displayedBaseline_ = source_->toPlainText();
    layout->addWidget(source_);
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
    connect(check, &QPushButton::clicked, this, [this] { report([this] { validate(); }); });
    connect(saveButton, &QPushButton::clicked, this, [this] { report([this] { save(); }); });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
}
void AudioBankEditor::validate() const {
    const auto source = source_->toPlainText().toUtf8();
    if (source.size() > maximumDocumentBytes)
        throw std::invalid_argument("Audio bank document exceeds budget");
    const auto bank = parseAudioBank(
        std::string_view(source.constData(), static_cast<size_t>(source.size())), file_.string());
    (void)loadSoundBank(assets_, bank);
}
void AudioBankEditor::save() {
    validate();
    if (read(file_) != baseline_)
        throw std::runtime_error("Audio bank changed externally; reopen before saving");
    if (source_->toPlainText() == displayedBaseline_)
        return;
    auto source = source_->toPlainText().toUtf8();
    if (baseline_.contains("\r\n"))
        source.replace("\n", "\r\n");
    QSaveFile file(pathText(file_));
    if (!file.open(QIODevice::WriteOnly) || file.write(source) != source.size() || !file.commit())
        throw std::runtime_error("Cannot atomically save audio bank");
    baseline_ = source;
    displayedBaseline_ = source_->toPlainText();
    source_->document()->setModified(false);
}
void AudioBankEditor::report(const std::function<void()>& operation) {
    try {
        operation();
        status_->setText(tr("Audio bank and WAV assets are valid"));
    } catch (const std::exception& e) {
        status_->setText(QString::fromUtf8(e.what()));
    }
}
bool AudioBankEditor::discardOrSave() {
    if (source_->toPlainText() == displayedBaseline_)
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
    if (discardOrSave())
        event->accept();
    else
        event->ignore();
}
void AudioBankEditor::reject() {
    if (discardOrSave())
        QDialog::reject();
}
} // namespace paper::editor
