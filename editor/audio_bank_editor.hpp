#pragma once
#include <QDialog>
#include <filesystem>
#include <functional>
class QPlainTextEdit;
class QLabel;
class QCloseEvent;
namespace paper::editor {
// Source-preserving editing with strict definitions, PCM validation and atomic saving.
class AudioBankEditor final : public QDialog {
  public:
    AudioBankEditor(std::filesystem::path file, std::filesystem::path assets,
                    QWidget* parent = nullptr);
    void validate() const;
    void save();

  protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

  private:
    bool discardOrSave();
    void report(const std::function<void()>& operation);
    std::filesystem::path file_, assets_;
    QByteArray baseline_;
    QString displayedBaseline_;
    QPlainTextEdit* source_ = nullptr;
    QLabel* status_ = nullptr;
};
} // namespace paper::editor
