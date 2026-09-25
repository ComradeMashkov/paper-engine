#pragma once
#include <QFutureWatcher>
#include <QWidget>
#include <filesystem>
#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace paper::editor {
struct ResourceEntry {
    QString name, category, path, resource;
};
struct ResourceIndex {
    std::vector<ResourceEntry> files;
    QStringList folders;
    QString error;
};
class ResourceBrowser final : public QWidget {
  public:
    explicit ResourceBrowser(QWidget* parent = nullptr);
    void project(const std::filesystem::path& root, std::vector<ResourceEntry> resources);
    [[nodiscard]] std::string selectedResource() const;
    std::function<void(std::string)> instantiate;
    std::function<void(std::filesystem::path)> openScene;
    std::function<void()> floatPanel;

  private:
    void refresh();
    void rebuild();
    void folders();
    void selection();
    void activate();
    QString root_, folder_;
    std::vector<ResourceEntry> resources_;
    ResourceIndex index_;
    QFutureWatcher<ResourceIndex> worker_;
    bool pending_ = false;
    QString runningRoot_;
    QTreeWidget *folders_ = nullptr, *files_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* category_ = nullptr;
    QLabel *breadcrumb_ = nullptr, *details_ = nullptr;
    QPushButton *add_ = nullptr, *refresh_ = nullptr;
};
} // namespace paper::editor
