#pragma once
#include "paper/audio/audio.hpp"
#include "paper/audio/bank.hpp"
#include <QDialog>
#include <filesystem>
#include <functional>
class QPlainTextEdit;
class QLabel;
class QCloseEvent;
class QTreeWidget;
class QComboBox;
class QTabWidget;
namespace paper::editor {
class PropertyForm;
// Source-preserving editing with strict definitions, PCM validation and atomic saving.
class AudioBankEditor final : public QDialog {
  public:
    AudioBankEditor(std::filesystem::path file, std::filesystem::path assets,
                    QWidget* parent = nullptr);
    void validate() const;
    void save();
    [[nodiscard]] std::string snapshot();
    [[nodiscard]] std::string recoverySource() const;
    void restoreSource(const std::string& source);
    void acceptSaved(const std::string& source);
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }
    [[nodiscard]] std::string original() const { return baseline_.toStdString(); }
    void sceneNodes(std::vector<std::string> nodes);
    std::function<std::vector<std::string>()> currentSceneNodes;
    std::function<void(std::optional<AudioBankDefinition>)> previewChanged;
    std::function<void()> closed;
    void preview();
    void audition();
    void stopAudition() { audition_.reset(); }
    [[nodiscard]] bool isAuditioning() const { return audition_ && audition_->available(); }

  protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

  private:
    bool discardOrSave();
    void report(const std::function<void()>& operation, bool verified = false);
    void rebuild();
    void inspect();
    void apply();
    void add();
    void chooseWav();
    void remove();
    void replace(ContentValue value, std::string selected);
    std::filesystem::path file_, assets_;
    QByteArray baseline_;
    QString displayedBaseline_;
    QPlainTextEdit* source_ = nullptr;
    QLabel* status_ = nullptr;
    QTreeWidget* entries_ = nullptr;
    QComboBox* kind_ = nullptr;
    PropertyForm* form_ = nullptr;
    ContentValue structure_;
    std::vector<std::string> nodes_;
    std::string selected_;
    bool hasNodes_ = false;
    std::unique_ptr<Audio> audition_;
};
} // namespace paper::editor
