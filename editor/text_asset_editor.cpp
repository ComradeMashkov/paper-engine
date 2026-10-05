#include "text_asset_editor.hpp"
#include "paper/content/document.hpp"
#include "paper/resources/resource_store.hpp"
#include <QCloseEvent>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QTextDocument>
#include <QVBoxLayout>

namespace paper::editor {
namespace {
QString text(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
QString pathText(const std::filesystem::path& p) {
    return QString::fromStdU16String(p.u16string());
}
std::string read(const std::filesystem::path& path) {
    QFile file(pathText(path));
    if (!file.open(QIODevice::ReadOnly) ||
        file.size() > static_cast<qint64>(content::limits::fileBytes))
        throw std::runtime_error("Cannot read bounded text asset");
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || QString::fromUtf8(bytes).toUtf8() != bytes)
        throw std::runtime_error("Text asset must be valid UTF-8");
    return bytes.toStdString();
}
constexpr int editorWidth = 1000, editorHeight = 720;
} // namespace
TextAssetEditor::TextAssetEditor(std::filesystem::path file, std::filesystem::path assets,
                                 TextAssetType type, QWidget* parent)
    : QDialog(parent), assets_(std::filesystem::canonical(assets)), type_(std::move(type)) {
    ResourceStore files(assets_);
    file_ = files.resolve(std::filesystem::weakly_canonical(file).lexically_relative(assets_));
    if (pathText(file_.extension()).compare(text(type_.extension), Qt::CaseInsensitive) != 0 ||
        !type_.validate)
        throw std::runtime_error("Unregistered text asset type");
    baseline_ = read(file_);
    setWindowTitle(text(type_.name) + " — " + pathText(file_.filename()));
    resize(editorWidth, editorHeight);
    auto* layout = new QVBoxLayout(this);
    source_ = new QPlainTextEdit;
    source_->setObjectName("textAssetSource");
    source_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    source_->setLineWrapMode(QPlainTextEdit::NoWrap);
    source_->setPlainText(text(baseline_));
    displayedBaseline_ = source_->toPlainText();
    layout->addWidget(source_);
    auto* search = new QLineEdit;
    search->setObjectName("textAssetSearch");
    search->setPlaceholderText(tr("Find text"));
    layout->addWidget(search);
    connect(search, &QLineEdit::returnPressed, this, [this, search] {
        if (!source_->find(search->text())) {
            auto cursor = source_->textCursor();
            cursor.movePosition(QTextCursor::Start);
            source_->setTextCursor(cursor);
            source_->find(search->text());
        }
    });
    status_ = new QLabel;
    status_->setObjectName("textAssetStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QHBoxLayout;
    for (const auto& [name, label, operation] :
         std::initializer_list<std::tuple<const char*, const char*, std::function<void()>>>{
             {"validateTextAsset", "Validate",
              [this] {
                  (void)snapshot();
                  status_->setText(tr("Validation passed"));
              }},
             {"saveTextAsset", "Save", [this] { save(); }},
             {"undoTextAsset", "Undo", [this] { source_->undo(); }},
             {"redoTextAsset", "Redo", [this] { source_->redo(); }}}) {
        auto* button = new QPushButton(tr(label));
        button->setObjectName(name);
        button->setAutoDefault(false);
        buttons->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, operation] { report(operation); });
    }
    auto* close = new QPushButton(tr("Close"));
    close->setAutoDefault(false);
    buttons->addWidget(close);
    connect(close, &QPushButton::clicked, this, &TextAssetEditor::reject);
    layout->addLayout(buttons);
    auto* shortcut = new QShortcut(QKeySequence::Save, this);
    connect(shortcut, &QShortcut::activated, this, [this] { report([this] { save(); }); });
    connect(source_, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        const auto cursor = source_->textCursor();
        status_->setText(tr("Line %1, column %2")
                             .arg(cursor.blockNumber() + 1)
                             .arg(cursor.positionInBlock() + 1));
    });
}
std::string TextAssetEditor::recoverySource() const {
    return source_->toPlainText() == displayedBaseline_
               ? baseline_
               : source_->toPlainText().toUtf8().toStdString();
}
std::string TextAssetEditor::snapshot() const {
    if (read(file_) != baseline_)
        throw std::runtime_error("Text asset changed externally; reopen before saving or Play");
    const auto source = recoverySource();
    type_.validate(assets_, file_.lexically_relative(assets_), source);
    return source;
}
void TextAssetEditor::restoreSource(const std::string& source) {
    auto cursor = source_->textCursor();
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(text(source));
    cursor.endEditBlock();
}
void TextAssetEditor::acceptSaved(const std::string& source) {
    baseline_ = source;
    QTextDocument document;
    document.setPlainText(text(source));
    displayedBaseline_ = document.toPlainText();
    source_->document()->setModified(source_->toPlainText() != displayedBaseline_);
}
void TextAssetEditor::save() {
    if (saveProject) {
        if (!saveProject())
            throw std::runtime_error("Project validation or save failed; source draft retained");
        status_->setText(tr("Saved"));
        return;
    }
    const auto source = snapshot();
    if (source == baseline_)
        return;
    QSaveFile file(pathText(file_));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(source.data(), static_cast<qint64>(source.size())) !=
            static_cast<qint64>(source.size()) ||
        read(file_) != baseline_ || !file.commit())
        throw std::runtime_error("Cannot atomically save text asset");
    acceptSaved(source);
    status_->setText(tr("Saved"));
}
void TextAssetEditor::report(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::exception& e) {
        status_->setText(text(e.what()));
    }
}
bool TextAssetEditor::confirm() {
    if (recoverySource() == baseline_)
        return true;
    const auto answer = QMessageBox::question(
        this, tr("Unsaved Text Asset"), tr("Save changes before closing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Discard)
        return true;
    if (answer != QMessageBox::Save)
        return false;
    try {
        save();
        return true;
    } catch (const std::exception& e) {
        status_->setText(text(e.what()));
        return false;
    }
}
void TextAssetEditor::reject() {
    if (confirm())
        QDialog::reject();
}
void TextAssetEditor::closeEvent(QCloseEvent* event) {
    if (confirm())
        event->accept();
    else
        event->ignore();
}
} // namespace paper::editor
