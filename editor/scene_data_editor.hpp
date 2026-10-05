#pragma once
#include "paper/core/content_value.hpp"
#include <QDialog>
#include <functional>
#include <string>

class QPlainTextEdit;
class QTreeWidget;
class QComboBox;
class QLabel;
namespace paper::editor {
class PropertyForm;
class SceneDataEditor final : public QDialog {
  public:
    SceneDataEditor(std::string source,
                    std::function<bool(const ContentValue&, const std::string&)> apply,
                    QWidget* parent = nullptr);
    [[nodiscard]] std::string recoverySource() const;
    void restoreSource(const std::string& source);
    void choices(std::vector<std::string> spawns);
    void stagedRecovery() { staged_ = true; }

  protected:
    void closeEvent(QCloseEvent*) override;
    void reject() override;

  private:
    void rebuild();
    void inspect();
    ContentValue draft() const;
    void publish(ContentValue value);
    void apply();
    void add();
    void duplicate();
    void remove();
    bool confirm();
    void report(const std::function<void()>& operation);
    std::function<bool(const ContentValue&, const std::string&)> apply_;
    std::string saved_, selected_, section_;
    ContentValue value_;
    std::vector<std::string> spawns_;
    QPlainTextEdit* source_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QComboBox* palette_ = nullptr;
    PropertyForm* form_ = nullptr;
    QLabel* status_ = nullptr;
    bool staged_ = false;
};
} // namespace paper::editor
