#include "resource_browser.hpp"
#include <QComboBox>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <map>

namespace paper::editor {
namespace {
constexpr int pathRole = Qt::UserRole + 1;
// Editor index budget, not an import limit. Prevent an accidentally selected huge
// content directory from consuming unlimited memory; report truncation explicitly.
constexpr size_t indexEntryLimit = 100000;
const QStringList categories{"Models",    "Meshes",  "Sprites",   "Images", "Audio", "Scenes",
                             "Templates", "Scripts", "Libraries", "Fonts",  "Other"};
QString categoryFor(const QString& suffix) {
    if (QStringList{"gltf", "glb", "obj", "fbx"}.contains(suffix))
        return "Models";
    if (QStringList{"png", "jpg", "jpeg", "webp", "bmp", "tga", "svg"}.contains(suffix))
        return "Images";
    if (QStringList{"wav", "ogg", "mp3", "flac"}.contains(suffix))
        return "Audio";
    if (suffix == "dcscene" || suffix == "dcworld")
        return "Scenes";
    if (suffix == "dctemplates")
        return "Templates";
    if (suffix == "lua")
        return "Scripts";
    if (suffix == "dcresources")
        return "Libraries";
    if (suffix == "ttf" || suffix == "otf")
        return "Fonts";
    return "Other";
}
ResourceIndex scan(const QString& root) {
    ResourceIndex result;
    QDir base(root);
    QDirIterator entries(root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        entries.next();
        const auto info = entries.fileInfo();
        // Never traverse links or expose files outside the content root.
        if (info.isSymLink())
            continue;
        const auto relative = base.relativeFilePath(info.filePath());
        if (info.isDir())
            result.folders.push_back(relative);
        else
            result.files.push_back(
                {info.fileName(), categoryFor(info.suffix().toLower()), relative, {}});
        if (result.files.size() + static_cast<size_t>(result.folders.size()) >= indexEntryLimit) {
            result.error = "Index limited to 100,000 entries. Choose a smaller project asset root.";
            break;
        }
    }
    return result;
}
} // namespace
ResourceBrowser::ResourceBrowser(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout;
    search_ = new QLineEdit;
    search_->setObjectName("resourceSearch");
    search_->setPlaceholderText(tr("Search name, ID or path in selected folder…"));
    search_->setClearButtonEnabled(true);
    category_ = new QComboBox;
    category_->setObjectName("resourceCategory");
    category_->setAccessibleName(tr("Resource category"));
    category_->addItem(tr("All categories"), "");
    for (const auto& category : categories)
        category_->addItem(category, category);
    refresh_ = new QPushButton(tr("Refresh"));
    refresh_->setObjectName("refreshResources");
    auto* detached = new QPushButton(tr("Float / Dock"));
    detached->setObjectName("floatResources");
    detached->setToolTip(tr("Toggle floating Project window"));
    controls->addWidget(search_, 1);
    controls->addWidget(category_);
    controls->addWidget(refresh_);
    controls->addWidget(detached);
    layout->addLayout(controls);
    breadcrumb_ = new QLabel(tr("Open a project"));
    breadcrumb_->setObjectName("resourceBreadcrumb");
    layout->addWidget(breadcrumb_);
    auto* split = new QSplitter;
    folders_ = new QTreeWidget;
    folders_->setObjectName("projectFolders");
    folders_->setHeaderLabel(tr("Project folders"));
    files_ = new QTreeWidget;
    files_->setObjectName("resourceLibrary");
    files_->setHeaderLabels({tr("Name / Resource"), tr("Path"), tr("Use")});
    files_->setAlternatingRowColors(true);
    files_->setRootIsDecorated(true);
    files_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    split->addWidget(folders_);
    split->addWidget(files_);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    auto* footer = new QHBoxLayout;
    details_ = new QLabel;
    details_->setWordWrap(true);
    details_->setTextFormat(Qt::PlainText);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    add_ = new QPushButton(tr("Add to Scene"));
    add_->setObjectName("instantiateResource");
    auto* reveal = new QPushButton(tr("Open Folder"));
    reveal->setObjectName("openResourceFolder");
    footer->addWidget(details_, 1);
    footer->addWidget(add_);
    footer->addWidget(reveal);
    layout->addLayout(footer);
    connect(search_, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(category_, &QComboBox::currentIndexChanged, this, [this] { rebuild(); });
    connect(folders_, &QTreeWidget::currentItemChanged, this, [this] {
        folder_ = folders_->currentItem() ? folders_->currentItem()->data(0, pathRole).toString()
                                          : QString{};
        rebuild();
    });
    connect(files_, &QTreeWidget::currentItemChanged, this, [this] { selection(); });
    connect(files_, &QTreeWidget::itemDoubleClicked, this, [this] { activate(); });
    connect(add_, &QPushButton::clicked, this, [this] {
        const auto id = selectedResource();
        if (!id.empty() && instantiate)
            instantiate(id);
    });
    connect(reveal, &QPushButton::clicked, this, [this] {
        if (root_.isEmpty())
            return;
        const auto* item = files_->currentItem();
        const auto relative = item ? QFileInfo(item->data(0, pathRole).toString()).path() : folder_;
        const auto directory = QDir(root_).filePath(relative);
        if (QFileInfo(directory).isDir())
            QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    connect(detached, &QPushButton::clicked, this, [this] {
        if (floatPanel)
            floatPanel();
    });
    connect(refresh_, &QPushButton::clicked, this, [this] { refresh(); });
    connect(&worker_, &QFutureWatcher<ResourceIndex>::finished, this, [this] {
        if (runningRoot_ == root_ && !pending_) {
            index_ = worker_.result();
            folders();
            rebuild();
            setProperty("indexing", false);
            refresh_->setEnabled(true);
        }
        if (pending_) {
            pending_ = false;
            refresh();
        }
    });
    selection();
}
void ResourceBrowser::project(const std::filesystem::path& root,
                              std::vector<ResourceEntry> resources) {
    root_ = QString::fromStdU16String(root.u16string());
    folder_.clear();
    resources_ = std::move(resources);
    index_ = {};
    folders();
    rebuild();
    refresh();
}
void ResourceBrowser::refresh() {
    if (root_.isEmpty())
        return;
    setProperty("indexing", true);
    refresh_->setEnabled(false);
    if (worker_.isRunning()) {
        pending_ = true;
        return;
    }
    runningRoot_ = root_;
    worker_.setFuture(QtConcurrent::run(scan, root_));
}
void ResourceBrowser::folders() {
    QSignalBlocker blocker(folders_);
    folders_->clear();
    std::map<QString, QTreeWidgetItem*> items;
    auto* root = new QTreeWidgetItem(folders_, {tr("All Assets")});
    root->setData(0, pathRole, "");
    root->setToolTip(0, root_);
    root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
    items[""] = root;
    auto paths = index_.folders;
    for (const auto& resource : resources_)
        paths.push_back(QFileInfo(resource.path).path());
    // Add parents first, including manifests before the asynchronous file scan completes.
    for (auto path : paths) {
        QString prefix;
        for (const auto& part : path.split('/', Qt::SkipEmptyParts)) {
            if (part == ".")
                continue;
            const auto next = prefix.isEmpty() ? part : prefix + '/' + part;
            if (!items.contains(next)) {
                auto* item = new QTreeWidgetItem(items.at(prefix), {part});
                item->setData(0, pathRole, next);
                item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
                items[next] = item;
            }
            prefix = next;
        }
    }
    folders_->sortItems(0, Qt::AscendingOrder);
    root->setExpanded(true);
    if (!items.contains(folder_))
        folder_.clear();
    folders_->setCurrentItem(items.at(folder_));
}
void ResourceBrowser::rebuild() {
    const auto selected = selectedResource();
    files_->clear();
    std::map<QString, QTreeWidgetItem*> groups;
    const auto query = search_->text().trimmed();
    const auto category = category_->currentData().toString();
    const auto append = [&](const ResourceEntry& entry) {
        if ((!category.isEmpty() && category != entry.category) ||
            (!folder_.isEmpty() && !entry.path.startsWith(folder_ + '/')) ||
            (!query.isEmpty() && !entry.name.contains(query, Qt::CaseInsensitive) &&
             !entry.path.contains(query, Qt::CaseInsensitive)))
            return;
        if (!groups.contains(entry.category)) {
            auto* group = new QTreeWidgetItem(files_, {entry.category});
            group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
            groups[entry.category] = group;
        }
        auto* item = new QTreeWidgetItem(
            groups.at(entry.category),
            {entry.name, entry.path, entry.resource.isEmpty() ? tr("File") : tr("Placeable")});
        item->setData(0, Qt::UserRole, entry.resource);
        item->setData(0, pathRole, entry.path);
        item->setToolTip(0, entry.resource.isEmpty()
                                ? tr("Project file; import and file editing are not available yet")
                                : tr("Double-click to add an instance to the current scene"));
        item->setIcon(0, style()->standardIcon(entry.resource.isEmpty()
                                                   ? QStyle::SP_FileIcon
                                                   : QStyle::SP_FileDialogDetailedView));
        if (!selected.empty() && entry.resource.toStdString() == selected)
            files_->setCurrentItem(item);
    };
    for (const auto& entry : resources_)
        append(entry);
    for (const auto& entry : index_.files)
        append(entry);
    for (const auto& [categoryName, group] : groups)
        group->setText(0, categoryName + QString(" (%1)").arg(group->childCount()));
    files_->sortItems(0, Qt::AscendingOrder);
    files_->expandAll();
    breadcrumb_->setText(tr("Assets / ") + folder_ + tr("  • includes subfolders"));
    breadcrumb_->setToolTip(root_);
    selection();
}
std::string ResourceBrowser::selectedResource() const {
    const auto* item = files_->currentItem();
    return item ? item->data(0, Qt::UserRole).toString().toStdString() : "";
}
void ResourceBrowser::selection() {
    const auto* item = files_->currentItem();
    add_->setEnabled(!selectedResource().empty());
    details_->setText(item                     ? item->data(0, pathRole).toString()
                      : index_.error.isEmpty() ? tr("Select a folder, category or resource")
                                               : index_.error);
}
void ResourceBrowser::activate() {
    const auto id = selectedResource();
    if (!id.empty() && instantiate) {
        instantiate(id);
        return;
    }
    const auto* item = files_->currentItem();
    if (!item)
        return;
    const auto path = item->data(0, pathRole).toString();
    if (path.endsWith(".dcscene", Qt::CaseInsensitive) && openScene)
        openScene(std::filesystem::path(path.toStdU16String()));
}
} // namespace paper::editor
