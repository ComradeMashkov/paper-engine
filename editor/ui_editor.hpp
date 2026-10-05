#pragma once
#include "paper/ui/document.hpp"
#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
class QPlainTextEdit;
class QTreeWidget;
class QComboBox;
class QLabel;
class QCheckBox;
namespace paper::editor {
class PropertyForm;
class UiPreview final : public QWidget {
  public:
    explicit UiPreview(QWidget* parent = nullptr);
    void document(const ui::Document& document);
    void selection(std::string id);
    void interactive(bool value);
    [[nodiscard]] const ui::Context& context() const { return *context_; }
    std::function<void(std::string)> selected;
    std::function<void(const ui::Action&)> action;

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    bool event(QEvent*) override;

  private:
    void input(ui::Input event);
    std::unique_ptr<ui::Context> context_;
    std::vector<std::string> order_;
    std::string selection_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    bool interactive_ = false;
};
class UiEditor final : public QDialog {
  public:
    UiEditor(std::filesystem::path file, std::filesystem::path assets, QWidget* parent = nullptr);
    void validate() const;
    void save();
    [[nodiscard]] std::string snapshot();
    [[nodiscard]] std::string recoverySource() const;
    void restoreSource(const std::string& source);
    void acceptSaved(const std::string& source);
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }
    [[nodiscard]] std::string original() const { return baseline_.toStdString(); }

  protected:
    void closeEvent(QCloseEvent*) override;
    void reject() override;

  private:
    void rebuild();
    void inspect();
    void apply();
    void add();
    void remove();
    void duplicate();
    void reorder(int delta);
    void replace(ContentValue value, std::string selected);
    void report(const std::function<void()>& operation);
    bool discardOrSave();
    std::filesystem::path file_, assets_;
    QByteArray baseline_;
    QString displayedBaseline_;
    ContentValue value_;
    QPlainTextEdit* source_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QComboBox* palette_ = nullptr;
    PropertyForm* form_ = nullptr;
    UiPreview* preview_ = nullptr;
    QLabel* status_ = nullptr;
    std::string selected_;
    bool selectionKnown_ = false;
};
} // namespace paper::editor
