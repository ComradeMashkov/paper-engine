#pragma once
#include "paper/editor/application.hpp"
#include <QDialog>
class QPlainTextEdit;
class QLabel;
namespace paper::editor {
class TextAssetEditor final : public QDialog {
  public:
    TextAssetEditor(std::filesystem::path file, std::filesystem::path assets, TextAssetType type,
                    QWidget* parent = nullptr);
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }
    [[nodiscard]] const std::string& original() const { return baseline_; }
    [[nodiscard]] std::string recoverySource() const;
    [[nodiscard]] std::string snapshot() const;
    void restoreSource(const std::string& source);
    void acceptSaved(const std::string& source);
    void save();
    // Workspace hosts route Save through complete cross-file validation and the journal.
    std::function<bool()> saveProject;

  protected:
    void closeEvent(QCloseEvent*) override;
    void reject() override;

  private:
    bool confirm();
    void report(const std::function<void()>& operation);
    std::filesystem::path file_, assets_;
    TextAssetType type_;
    std::string baseline_;
    QString displayedBaseline_;
    QPlainTextEdit* source_ = nullptr;
    QLabel* status_ = nullptr;
};
} // namespace paper::editor
