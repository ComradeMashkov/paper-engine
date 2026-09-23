#include "paper/editor/application.hpp"
#include "paper/authoring/document.hpp"
#include "paper/content/document.hpp"
#include "viewport.hpp"
#include "workspace.hpp"
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QTreeWidget>
#include <QUndoStack>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <array>
#include <set>

namespace paper::editor {
namespace {
namespace fs = std::filesystem;
constexpr int initialWidth = 1400, initialHeight = 900, transformDecimals = 6;
constexpr int historyLimit = 128;
constexpr int hierarchyMinimumWidth = 240, inspectorMinimumWidth = 280;
constexpr int hierarchyInitialWidth = 280, inspectorInitialWidth = 310;
constexpr int resourcesInitialHeight = 200, problemsInitialHeight = 120;
QString text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
QString pathText(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}
fs::path filePath(const QString& path) {
    return fs::path(path.toStdU16String());
}
std::string bytes(const fs::path& path) {
    QFile file(pathText(path));
    if (!file.open(QIODevice::ReadOnly) ||
        file.size() > static_cast<qint64>(content::limits::fileBytes))
        throw std::runtime_error("Cannot read content file: " + path.string());
    const auto result = file.readAll();
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error(file.errorString().toStdString());
    return result.toStdString();
}
struct Project {
    fs::path file, root, manifest;
    std::string name;
    std::map<fs::path, std::shared_ptr<authoring::SceneDocument>> scenes;
    std::map<std::string, ContentValue, std::less<>> templates;
    std::map<std::string, std::string, std::less<>> labels;
    std::unique_ptr<QLockFile> lock;
    std::map<fs::path, std::string> sources;
    void verifySources() const {
        for (const auto& [path, original] : sources)
            if (bytes(path) != original)
                throw std::runtime_error("Project dependencies changed outside the editor; reopen "
                                         "the project before editing or saving");
        for (const auto& [path, scene] : scenes)
            if (bytes(scene->path()) != scene->original())
                throw std::runtime_error("Scene changed outside the editor; reopen the project "
                                         "before editing or saving");
    }
    ContentValue readSource(const fs::path& path) {
        auto source = bytes(path);
        sources.emplace(path, source);
        return content::parse(source, path.string());
    }
    explicit Project(const fs::path& path, bool acquireLock = true) {
        file = fs::canonical(path);
        const auto spec = readSource(file);
        if (spec.at("format") != "paper.project" || !spec.at("version").is_number_integer() ||
            spec.at("version") != 1)
            throw std::runtime_error("Unsupported Paper project version");
        ResourceStore projectFiles(file.parent_path());
        root = projectFiles.resolve(spec.at("assets").get<std::string>());
        manifest = spec.at("world").get<std::string>();
        name = spec.at("name").get<std::string>();
        if (acquireLock) {
            lock = std::make_unique<QLockFile>(pathText(file) + ".editor.lock");
            lock->setStaleLockTime(0);
            if (!lock->tryLock())
                throw std::runtime_error("Project is already open in another editor");
        }
        ResourceStore files(root);
        const auto world = readSource(files.resolve(manifest));
        for (const auto& relative : world.at("world").at("resources"))
            (void)readSource(files.resolve(relative.get<std::string>()));
        for (const auto& relative : world.at("world").at("scenes")) {
            const fs::path relativePath = relative.get<std::string>();
            const auto full = files.resolve(relativePath);
            scenes.emplace(relativePath,
                           std::make_shared<authoring::SceneDocument>(full, bytes(full)));
        }
        if (scenes.empty())
            throw std::runtime_error("Project has no scenes");
        for (const auto& relative : world.at("world").at("templates")) {
            const auto document = readSource(files.resolve(relative.get<std::string>()));
            for (const auto& value : document.at("templates").at("templates"))
                if (!templates.emplace(value.at("id").get<std::string>(), value).second)
                    throw std::runtime_error("Duplicate template ID");
        }
        if (spec.contains("strings")) {
            const auto strings = readSource(files.resolve(spec.at("strings").get<std::string>()));
            for (const auto& [id, value] : strings.at("strings").items())
                labels.emplace(id, value.get<std::string>());
        }
    }
    SceneDocuments snapshot() const {
        SceneDocuments result;
        for (const auto& [path, scene] : scenes)
            result.emplace(path, scene->data());
        return result;
    }
    ContentValue effective(const ContentValue& node) const {
        auto value = node.contains("template")
                         ? templates.at(node.at("template").get<std::string>())
                         : ContentValue::object();
        auto patch = node;
        if (patch.contains("remove")) {
            for (const auto& field : patch.at("remove"))
                value.erase(field.get<std::string>());
            patch.erase("remove");
        }
        value.overlay(patch);
        return value;
    }
};
struct PreviewResult {
    std::shared_ptr<ScenePackage> package;
    std::string error;
};
PreviewResult compile(const fs::path& root, const fs::path& manifest, SceneDocuments documents,
                      const Options& options) {
    try {
        std::vector<std::string> owned = options.materialNames;
        auto materials = options.materials ? options.materials() : std::vector<PaintedTexture>{};
        // Standalone editor discovers material names and uses neutral procedural previews.
        if (!options.materials) {
            std::set<std::string> names;
            const auto collect = [&](const auto& self, const ContentValue& value) -> void {
                if (value.is_object())
                    for (const auto& [key, child] : value.items()) {
                        if ((key == "material" || key == "replaceMaterial") && child.is_string())
                            names.insert(child.get<std::string>());
                        else
                            self(self, child);
                    }
                else if (value.is_array())
                    for (const auto& child : value)
                        self(self, child);
            };
            const auto world = content::read(root / manifest);
            ResourceStore files(root);
            for (const auto& path : world.at("world").at("resources"))
                collect(collect, content::read(files.resolve(path.get<std::string>())));
            for (const auto& name : names) {
                owned.push_back(name);
                PaintedTexture texture;
                texture.width = texture.height = 1;
                constexpr Pixel neutral{160, 160, 160, 255};
                texture.pixels.push_back(neutral);
                materials.push_back(std::move(texture));
            }
        }
        std::vector<std::string_view> names(owned.begin(), owned.end());
        return {loadScenes(root, std::move(materials), names, manifest, documents), {}};
    } catch (const std::exception& error) {
        return {{}, error.what()};
    }
}
class SceneCommand final : public QUndoCommand {
  public:
    SceneCommand(std::function<void(const ContentValue&)> apply, ContentValue before,
                 ContentValue after, QString title)
        : apply_(std::move(apply)), before_(std::move(before)), after_(std::move(after)) {
        setText(title);
    }
    void undo() override { apply_(before_); }
    void redo() override { apply_(after_); }

  private:
    std::function<void(const ContentValue&)> apply_;
    ContentValue before_, after_;
};
class Window final : public QMainWindow {
  public:
    explicit Window(Options options) : options_(std::move(options)) {
        setWindowTitle(text(options_.title));
        resize(initialWidth, initialHeight);
        undo_.setUndoLimit(historyLimit);
        viewport_ = new Viewport(options_.shaders, this);
        setCentralWidget(viewport_);
        viewport_->selected = [this](std::string id) {
            const auto found = items_.find(id);
            if (id.empty()) {
                tree_->setCurrentItem(nullptr);
            } else if (found != items_.end()) {
                tree_->setCurrentItem(found->second);
                tree_->scrollToItem(found->second);
            }
        };
        viewport_->failed = [this](std::string error) {
            viewportError_ = tr("Ошибка 3D-вида: ") + text(error);
            problem(viewportError_);
        };
        viewport_->transformed = [this](Viewport::Tool tool, Vec3 delta, float value) {
            transform(tool, delta, value);
        };
        setDockNestingEnabled(true);
        auto* hierarchy = new QWidget;
        auto* layout = new QVBoxLayout(hierarchy);
        sceneList_ = new QComboBox;
        sceneList_->setObjectName("sceneList");
        sceneList_->setAccessibleName(tr("Сцена"));
        layout->addWidget(sceneList_);
        auto* search = hierarchySearch_ = new QLineEdit;
        search->setPlaceholderText(tr("Поиск объекта или ID"));
        layout->addWidget(search);
        tree_ = new QTreeWidget;
        tree_->setObjectName("sceneHierarchy");
        tree_->setAccessibleName(tr("Объекты сцены"));
        tree_->setAlternatingRowColors(true);
        tree_->setContextMenuPolicy(Qt::ActionsContextMenu);
        tree_->setMinimumWidth(hierarchyMinimumWidth);
        tree_->setHeaderLabels({tr("Объект"), tr("ID")});
        layout->addWidget(tree_);
        dock(tr("Объекты"), "hierarchy", hierarchy, Qt::LeftDockWidgetArea);
        auto* inspector = new QWidget;
        auto* form = new QFormLayout(inspector);
        selection_ = new QLabel(tr("Выберите объект"));
        selection_->setWordWrap(true);
        form->addRow(selection_);
        name_ = new QLineEdit;
        name_->setObjectName("nodeLabel");
        name_->setAccessibleName(tr("Имя объекта"));
        form->addRow(tr("Имя / ключ локализации"), name_);
        connect(name_, &QLineEdit::editingFinished, this,
                [this] { editProperty("label", name_->text().toStdString()); });
        parent_ = new QComboBox;
        parent_->setObjectName("nodeParent");
        parent_->setAccessibleName(tr("Родитель объекта"));
        form->addRow(tr("Родитель"), parent_);
        connect(parent_, &QComboBox::activated, this,
                [this] { reparent(parent_->currentData().toString().toStdString()); });
        resource_ = new QComboBox;
        resource_->setObjectName("nodeResource");
        resource_->setAccessibleName(tr("Ресурс объекта"));
        form->addRow(tr("Ресурс"), resource_);
        connect(resource_, &QComboBox::activated, this, [this] {
            editProperty("resource", resource_->currentData().toString().toStdString());
        });
        const std::array<QString, 5> labels{tr("X, м"), tr("Y, м"), tr("Z, м"), tr("Поворот Y, °"),
                                            tr("Масштаб")};
        for (size_t i = 0; i < fields_.size(); ++i) {
            auto* spin = fields_[i] = new QDoubleSpinBox;
            spin->setObjectName(QString("transform%1").arg(i));
            spin->setDecimals(transformDecimals);
            spin->setKeyboardTracking(false);
            spin->setEnabled(false);
            const double limit =
                i == yawField ? sceneLimits::coordinateMeters * units::degreesPerHalfTurn / pi3
                              : sceneLimits::coordinateMeters;
            spin->setRange(-limit, limit);
            if (i == scaleField)
                spin->setRange(sceneLimits::scaleMinimum, sceneLimits::scaleMaximum);
            constexpr double positionStepMeters = .1, rotationStepDegrees = 1;
            spin->setSingleStep(i == yawField ? rotationStepDegrees : positionStepMeters);
            spin->setAccessibleName(labels[i]);
            form->addRow(labels[i], spin);
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this, i] { change(i); });
        }
        auto* hint = new QLabel(tr("Локальные координаты относительно родителя. Изменение создаёт "
                                   "явное переопределение шаблона."));
        hint->setWordWrap(true);
        form->addRow(hint);
        shadow_ = new QCheckBox(tr("Отбрасывает тень"));
        connect(shadow_, &QCheckBox::clicked, this,
                [this](bool checked) { editProperty("shadow", checked); });
        form->addRow(shadow_);
        auto* reset = new QPushButton(tr("Сбросить трансформ к шаблону"));
        connect(reset, &QPushButton::clicked, this, [this] { resetTransform(); });
        form->addRow(reset);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(inspector);
        scroll->setMinimumWidth(inspectorMinimumWidth);
        dock(tr("Свойства"), "inspector", scroll, Qt::RightDockWidgetArea);
        auto* assets = new QWidget;
        auto* assetsLayout = new QVBoxLayout(assets);
        assetSearch_ = new QLineEdit;
        assetSearch_->setPlaceholderText(tr("Поиск ресурса…"));
        assetsLayout->addWidget(assetSearch_);
        assets_ = new QTreeWidget;
        assets_->setObjectName("resourceLibrary");
        assets_->setAccessibleName(tr("Библиотека ресурсов"));
        assets_->setHeaderLabels({tr("Ресурс"), tr("Тип")});
        assets_->setAlternatingRowColors(true);
        assetsLayout->addWidget(assets_);
        auto* instantiate = new QPushButton(tr("Добавить в сцену"));
        assetsLayout->addWidget(instantiate);
        connect(instantiate, &QPushButton::clicked, this, [this] { createResource(); });
        connect(assets_, &QTreeWidget::itemDoubleClicked, this, [this] { createResource(); });
        connect(assetSearch_, &QLineEdit::textChanged, this, [this](const QString& query) {
            for (int i = 0; i < assets_->topLevelItemCount(); ++i) {
                auto* item = assets_->topLevelItem(i);
                item->setHidden(!item->text(0).contains(query, Qt::CaseInsensitive));
            }
        });
        dock(tr("Ресурсы проекта"), "assets", assets, Qt::BottomDockWidgetArea);
        problems_ = new QListWidget;
        problems_->setObjectName("projectProblems");
        dock(tr("Проверка проекта"), "problems", problems_, Qt::BottomDockWidgetArea);
        auto* file = menuBar()->addMenu(tr("Файл"));
        auto* open = file->addAction(tr("Открыть проект…"));
        open->setShortcut(QKeySequence::Open);
        connect(open, &QAction::triggered, this, [this] {
            if (confirmChanges())
                chooseProject();
        });
        save_ = file->addAction(tr("Сохранить сцену"));
        save_->setObjectName("saveScene");
        save_->setShortcut(QKeySequence::Save);
        connect(save_, &QAction::triggered, this, [this] {
            if (current_)
                save(current_);
        });
        auto* saveAll = file->addAction(tr("Сохранить все сцены"));
        saveAll->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
        connect(saveAll, &QAction::triggered, this, [this] {
            if (project_)
                for (const auto& [path, doc] : project_->scenes)
                    if (!save(doc))
                        break;
        });
        auto* exit = file->addAction(tr("Закрыть"));
        exit->setShortcut(QKeySequence::Quit);
        connect(exit, &QAction::triggered, this, &QWidget::close);
        auto* edit = menuBar()->addMenu(tr("Правка"));
        auto* undo = undo_.createUndoAction(this, tr("Отменить"));
        undo->setObjectName("undo");
        undo->setShortcut(QKeySequence::Undo);
        edit->addAction(undo);
        auto* redo = undo_.createRedoAction(this, tr("Повторить"));
        redo->setObjectName("redo");
        redo->setShortcut(QKeySequence::Redo);
        edit->addAction(redo);
        auto* create = menuBar()->addMenu(tr("Объект"));
        auto* group = create->addAction(tr("Создать пустой объект"));
        group->setObjectName("createGroup");
        connect(group, &QAction::triggered, this, [this] { createNode(""); });
        auto* instance = create->addAction(tr("Добавить выбранный ресурс"));
        instance->setObjectName("createResource");
        connect(instance, &QAction::triggered, this, [this] { createResource(); });
        duplicate_ = create->addAction(tr("Дублировать"));
        duplicate_->setObjectName("duplicateNode");
        remove_ = create->addAction(tr("Удалить с дочерними объектами"));
        remove_->setObjectName("deleteNode");
        for (auto* action : {duplicate_, remove_}) {
            action->setShortcutContext(Qt::WidgetShortcut);
            tree_->addAction(action);
            viewport_->addAction(action);
        }
        duplicate_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
        remove_->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
        connect(duplicate_, &QAction::triggered, this, [this] { duplicateNode(); });
        connect(remove_, &QAction::triggered, this, [this] { deleteNode(); });
        auto* view = menuBar()->addMenu(tr("Вид"));
        auto* frame = view->addAction(tr("Приблизить выбранный объект"));
        frame->setShortcut(Qt::Key_F);
        frame->setShortcutContext(Qt::WidgetShortcut);
        viewport_->addAction(frame);
        tree_->addAction(frame);
        connect(frame, &QAction::triggered, this, [this] { viewport_->frame(selected_); });
        auto* refresh = view->addAction(tr("Обновить 3D-вид"));
        refresh->setShortcut(QKeySequence::Refresh);
        connect(refresh, &QAction::triggered, this, [this] {
            ++revision_;
            requestPreview();
        });
        for (auto* panel : findChildren<QDockWidget*>())
            view->addAction(panel->toggleViewAction());
        auto* resetLayout = view->addAction(tr("Восстановить расположение панелей"));
        connect(resetLayout, &QAction::triggered, this, [this] {
            restoreState(defaultLayout_);
            for (auto* dock : findChildren<QDockWidget*>())
                dock->show();
        });
        auto* toolbar = addToolBar(tr("Проект"));
        toolbar->setObjectName("projectToolbar");
        toolbar->addAction(open);
        toolbar->addAction(save_);
        toolbar->addSeparator();
        toolbar->addAction(undo);
        toolbar->addAction(redo);
        toolbar->addAction(frame);
        toolbar->addSeparator();
        toolbar->addAction(group);
        toolbar->addAction(duplicate_);
        toolbar->addAction(remove_);
        open->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
        save_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
        remove_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
        addToolBarBreak();
        auto* tools = addToolBar(tr("Инструменты сцены"));
        tools->setObjectName("sceneTools");
        auto* modes = new QActionGroup(this);
        const std::array<QString, 4> titles{tr("Выбор"), tr("Перемещение"), tr("Поворот Y"),
                                            tr("Масштаб")};
        const std::array<int, 4> shortcuts{Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R};
        for (size_t i = 0; i < titles.size(); ++i) {
            auto* action = tools->addAction(titles[i]);
            action->setCheckable(true);
            action->setChecked(i == 1);
            modes->addAction(action);
            // Fly-camera WASD is handled directly by the viewport while RMB is held.
            action->setToolTip(
                titles[i] +
                tr(" • клавиша %1 во viewport").arg(QKeySequence(shortcuts[i]).toString()));
            connect(action, &QAction::triggered, this,
                    [this, i] { viewport_->tool(static_cast<Viewport::Tool>(i)); });
        }
        toolActions_ = modes->actions();
        viewport_->toolRequested = [this](Viewport::Tool tool) {
            toolActions_.at(static_cast<int>(tool))->trigger();
        };
        tools->addSeparator();
        auto* grid = tools->addAction(tr("Сетка X-ray"));
        grid->setCheckable(true);
        grid->setChecked(false);
        connect(grid, &QAction::toggled, this, [this](bool checked) { viewport_->grid(checked); });
        auto* snap = tools->addAction(tr("Привязка: 0,5 м / 15° / 0,1"));
        snap->setCheckable(true);
        connect(snap, &QAction::toggled, this, [this](bool checked) { viewport_->snap(checked); });
        auto* navigation = new QLabel(tr("  ПКМ + WASD/QE — полёт • СКМ — сдвиг • F — фокус"));
        tools->addWidget(navigation);
        connect(sceneList_, &QComboBox::currentIndexChanged, this, [this] { selectScene(); });
        connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            selected_ = item ? item->data(0, Qt::UserRole).toString().toStdString() : "";
            inspect();
        });
        connect(search, &QLineEdit::textChanged, this, [this](const QString& query) {
            const auto filter = [&](const auto& self, QTreeWidgetItem* item) -> bool {
                bool visible = item->text(0).contains(query, Qt::CaseInsensitive) ||
                               item->text(1).contains(query, Qt::CaseInsensitive);
                for (int i = 0; i < item->childCount(); ++i)
                    visible = self(self, item->child(i)) || visible;
                item->setHidden(!visible);
                return visible;
            };
            for (int i = 0; i < tree_->topLevelItemCount(); ++i)
                filter(filter, tree_->topLevelItem(i));
            if (!query.isEmpty())
                tree_->expandAll();
        });
        connect(&worker_, &QFutureWatcher<PreviewResult>::finished, this, [this] {
            const auto result = worker_.result();
            if (runningRevision_ == revision_ && publishedRevision_ != revision_)
                publish(result);
            if (pendingPreview_) {
                pendingPreview_ = false;
                requestPreview();
            }
        });
        resizeDocks({findChild<QDockWidget*>("hierarchy"), findChild<QDockWidget*>("inspector")},
                    {hierarchyInitialWidth, inspectorInitialWidth}, Qt::Horizontal);
        resizeDocks({findChild<QDockWidget*>("assets"), findChild<QDockWidget*>("problems")},
                    {resourcesInitialHeight, problemsInitialHeight}, Qt::Vertical);
        splitDockWidget(findChild<QDockWidget*>("assets"), findChild<QDockWidget*>("problems"),
                        Qt::Horizontal);
        defaultLayout_ = saveState();
        QSettings settings;
        restoreGeometry(settings.value("window/geometry").toByteArray());
        restoreState(settings.value("window/layout").toByteArray());
        inspect();
        updateState();
        if (!options_.project.empty())
            openProject(options_.project);
    }

  protected:
    void closeEvent(QCloseEvent* event) override {
        if (!confirmChanges()) {
            event->ignore();
            return;
        }
        QSettings settings;
        settings.setValue("window/geometry", saveGeometry());
        settings.setValue("window/layout", saveState());
        event->accept();
    }

  private:
    static constexpr size_t yawField = 3, scaleField = 4, propertyCount = 5;
    void dock(QString title, QString name, QWidget* contents, Qt::DockWidgetArea area) {
        auto* panel = new QDockWidget(title, this);
        panel->setObjectName(name);
        panel->setWidget(contents);
        addDockWidget(area, panel);
    }
    void problem(const QString& error) {
        problems_->addItem(error);
        statusBar()->showMessage(error);
    }
    void chooseProject() {
        const auto path = QFileDialog::getOpenFileName(this, tr("Открыть проект"), {},
                                                       tr("Проект Paper (*.paperproject)"));
        if (!path.isEmpty())
            openProject(filePath(path));
    }
    void openProject(const fs::path& path) {
        try {
            const bool same = project_ && fs::canonical(path) == project_->file;
            auto candidate = std::make_unique<Project>(path, !same);
            // Validate before publishing project state; no Lua or game session is constructed.
            auto preview =
                compile(candidate->root, candidate->manifest, candidate->snapshot(), options_);
            if (!preview.package)
                throw std::runtime_error(preview.error);
            candidate->verifySources();
            if (same)
                candidate->lock = std::move(project_->lock);
            ++revision_;
            undo_.clear();
            project_ = std::move(candidate);
            package_ = std::move(preview.package);
            publishedRevision_ = revision_;
            problems_->clear();
            if (!viewportError_.isEmpty())
                problem(viewportError_);
            selected_.clear();
            {
                QSignalBlocker blocked(sceneList_);
                sceneList_->clear();
                for (const auto& [file, doc] : project_->scenes)
                    sceneList_->addItem(text(doc->data().at("scene").at("id").get<std::string>()),
                                        pathText(file));
            }
            populateAssets();
            selectScene();
            updateState();
        } catch (const std::exception& error) {
            problem(text(error.what()));
            QMessageBox::warning(this, tr("Проект не открыт"), text(error.what()));
        }
    }
    void selectScene() {
        if (!project_ || sceneList_->currentIndex() < 0)
            return;
        current_ = project_->scenes.at(filePath(sceneList_->currentData().toString()));
        selected_.clear();
        rebuildTree();
        if (package_)
            viewport_->scene(package_, current_->data().at("scene").at("id").get<std::string>(),
                             true);
        updateState();
    }
    void rebuildTree() {
        QSignalBlocker treeSignals(tree_);
        tree_->clear();
        items_.clear();
        for (const auto& node : current_->nodes()) {
            const auto id = node.at("id").get<std::string>();
            const auto effective = project_->effective(node);
            std::string label = effective.value("label", "");
            if (project_->labels.contains(label))
                label = project_->labels.at(label);
            if (label.empty())
                label = effective.value("kind", "Объект");
            auto* item = new QTreeWidgetItem({text(label), text(id)});
            item->setData(0, Qt::UserRole, text(id));
            items_[id] = item;
        }
        for (const auto& node : current_->nodes()) {
            auto* item = items_.at(node.at("id").get<std::string>());
            const auto parent = project_->effective(node).value("parent", "");
            if (!parent.empty() && items_.contains(parent))
                items_.at(parent)->addChild(item);
            else
                tree_->addTopLevelItem(item);
        }
        tree_->expandToDepth(0);
        tree_->resizeColumnToContents(0);
        if (items_.contains(selected_))
            tree_->setCurrentItem(items_.at(selected_));
        else
            selected_.clear();
        if (!hierarchySearch_->text().isEmpty())
            emit hierarchySearch_->textChanged(hierarchySearch_->text());
        inspect();
    }
    void inspect() {
        updating_ = true;
        const bool available = current_ && !selected_.empty();
        for (auto* field : fields_)
            field->setEnabled(available);
        selection_->setText(available ? text(selected_) : tr("Выберите объект"));
        viewport_->selection(selected_);
        name_->setEnabled(available);
        parent_->setEnabled(available);
        resource_->setEnabled(available);
        shadow_->setEnabled(available);
        duplicate_->setEnabled(available);
        remove_->setEnabled(available);
        name_->clear();
        parent_->clear();
        resource_->clear();
        if (available) {
            const auto effective = project_->effective(current_->node(selected_));
            name_->setText(text(effective.value("label", "")));
            parent_->addItem(tr("Корень сцены"), "");
            const auto children = descendants(selected_);
            for (const auto& node : current_->nodes()) {
                const auto id = node.at("id").get<std::string>();
                if (!children.contains(id))
                    parent_->addItem(text(id), text(id));
            }
            parent_->setCurrentIndex(parent_->findData(text(effective.value("parent", ""))));
            resource_->addItem(tr("Без геометрии"), "");
            for (const auto& [id, asset] : package_->assets)
                resource_->addItem(text(id), text(id));
            resource_->setCurrentIndex(resource_->findData(text(effective.value("resource", ""))));
            shadow_->setChecked(effective.value("shadow", true));
        }
        if (available) {
            const auto node = project_->effective(current_->node(selected_));
            const auto position = node.value("position", ContentValue::array({0, 0, 0}));
            for (size_t i = 0; i < yawField; ++i)
                fields_[i]->setValue(position.at(i).get<double>());
            fields_[yawField]->setValue(node.value("yaw", 0.0) * units::degreesPerHalfTurn / pi3);
            fields_[scaleField]->setValue(node.value("scale", 1.0));
            for (size_t i = 0; i < fields_.size(); ++i)
                displayed_[i] = fields_[i]->value();
        }
        updating_ = false;
    }
    void change(size_t component) {
        if (updating_ || !current_ || selected_.empty() ||
            fields_[component]->value() == displayed_[component])
            return;
        const auto name = component < yawField    ? "position"
                          : component == yawField ? "yaw"
                                                  : "scale";
        auto after =
            project_->effective(current_->node(selected_))
                .value(name, component < yawField ? ContentValue::array({0, 0, 0})
                                                  : ContentValue(component == scaleField ? 1 : 0));
        if (component < yawField)
            after[component] = fields_[component]->value();
        else
            after = fields_[component]->value() *
                    (component == yawField ? pi3 / units::degreesPerHalfTurn : 1);
        editProperty(name, after);
    }
    void populateAssets() {
        assets_->clear();
        if (!package_)
            return;
        for (const auto& [id, asset] : package_->assets) {
            auto* item = new QTreeWidgetItem(assets_, {text(id), asset->model       ? tr("Модель")
                                                                 : asset->billboard ? tr("Спрайт")
                                                                                    : tr("Меш")});
            item->setData(0, Qt::UserRole, text(id));
        }
        assets_->resizeColumnToContents(0);
        if (!assetSearch_->text().isEmpty())
            emit assetSearch_->textChanged(assetSearch_->text());
    }
    std::set<std::string> descendants(const std::string& id) const {
        std::set<std::string> result{id};
        bool added = true;
        while (added) {
            added = false;
            for (const auto& node : current_->nodes())
                if (result.contains(project_->effective(node).value("parent", "")) &&
                    result.insert(node.at("id").get<std::string>()).second)
                    added = true;
        }
        return result;
    }
    void commitNodes(ContentValue after, QString title, std::string selection) {
        if (!current_)
            return;
        const auto doc = current_;
        const auto before = doc->nodes();
        if (before == after) {
            inspect();
            return;
        }
        try {
            project_->verifySources();
            auto candidate = *doc;
            candidate.replaceNodes(after);
            (void)candidate.serialized();
            auto documents = project_->snapshot();
            for (const auto& [path, scene] : project_->scenes)
                if (scene == doc)
                    documents[path] = candidate.data();
            const auto checked =
                compile(project_->root, project_->manifest, std::move(documents), options_);
            if (!checked.package)
                throw std::runtime_error(checked.error);
            const auto oldSelection = selected_;
            undo_.push(new SceneCommand(
                [this, doc, initialPackage = checked.package](const ContentValue& state) mutable {
                    doc->replaceNodes(state.at("nodes"));
                    ++revision_;
                    // Commands remain attached to their scene when selection has changed.
                    for (const auto& [path, scene] : project_->scenes)
                        if (scene == doc) {
                            QSignalBlocker blocker(sceneList_);
                            sceneList_->setCurrentIndex(sceneList_->findData(pathText(path)));
                        }
                    current_ = doc;
                    selected_ = state.at("selection").get<std::string>();
                    rebuildTree();
                    updateState();
                    if (initialPackage) {
                        publish({std::move(initialPackage), {}});
                    } else
                        requestPreview();
                },
                ContentValue{{"nodes", before}, {"selection", oldSelection}},
                ContentValue{{"nodes", std::move(after)}, {"selection", selection}},
                std::move(title)));
        } catch (const std::exception& error) {
            problem(text(error.what()));
            inspect();
        }
    }
    void editProperty(std::string_view name, const ContentValue& value) {
        if (updating_ || !current_ || selected_.empty())
            return;
        try {
            auto candidate = *current_;
            setEffectiveProperty(candidate, name, value);
            commitNodes(candidate.nodes(), tr("Изменить %1").arg(text(name)), selected_);
        } catch (const std::exception& error) {
            problem(text(error.what()));
            inspect();
        }
    }
    void setEffectiveProperty(authoring::SceneDocument& candidate, std::string_view name,
                              const ContentValue& value) {
        if ((name != "parent" && name != "resource") || value != "") {
            candidate.setProperty(selected_, name, value);
            return;
        }
        auto nodes = candidate.nodes();
        for (auto& node : nodes)
            if (node.at("id") == selected_) {
                node.erase(name);
                auto removed = node.value("remove", ContentValue::array());
                const auto inherited =
                    node.contains("template")
                        ? project_->templates.at(node.at("template").get<std::string>())
                        : ContentValue::object();
                if (inherited.contains(name) &&
                    std::ranges::none_of(removed, [&](const auto& key) { return key == name; }))
                    removed.elements().push_back(std::string(name));
                if (!removed.empty())
                    node["remove"] = std::move(removed);
            }
        candidate.replaceNodes(std::move(nodes));
    }
    std::string newId() const {
        return "node." + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    }
    void createResource() {
        if (!assets_->currentItem()) {
            statusBar()->showMessage(tr("Выберите ресурс в библиотеке внизу окна"));
            return;
        }
        createNode(assets_->currentItem()->data(0, Qt::UserRole).toString().toStdString());
    }
    void createNode(const std::string& resource) {
        if (!current_)
            return;
        auto nodes = current_->nodes();
        const auto id = newId();
        const auto position = viewport_->insertionPoint();
        ContentValue node{{"id", id},
                          {"label", resource.empty() ? "Новый объект" : resource},
                          {"position", ContentValue::array({position.x, position.y, position.z})}};
        if (!resource.empty())
            node["resource"] = resource;
        nodes.elements().push_back(std::move(node));
        commitNodes(std::move(nodes), tr("Создать объект"), id);
    }
    void deleteNode() {
        if (!current_ || selected_.empty())
            return;
        const auto ids = descendants(selected_);
        auto nodes = current_->nodes();
        std::erase_if(nodes.elements(), [&](const auto& node) {
            return ids.contains(node.at("id").template get<std::string>());
        });
        commitNodes(std::move(nodes), tr("Удалить %1 объект(ов)").arg(ids.size()), "");
    }
    void duplicateNode() {
        if (!current_ || selected_.empty())
            return;
        const auto ids = descendants(selected_);
        std::map<std::string, std::string, std::less<>> remap;
        for (const auto& id : ids)
            remap[id] = newId();
        auto nodes = current_->nodes();
        for (const auto& original : current_->nodes()) {
            const auto id = original.at("id").get<std::string>();
            if (!ids.contains(id))
                continue;
            const auto effective = project_->effective(original);
            // Host game bindings have uniqueness rules the engine cannot infer.
            for (const auto field :
                 {"actions", "stateKey", "itemInstance", "legacyDoor", "visibleWhen"})
                if (effective.contains(field)) {
                    problem(tr("У объекта есть игровая привязка %1. Для независимой копии добавьте "
                               "его ресурс из библиотеки.")
                                .arg(text(field)));
                    return;
                }
            auto clone = original;
            clone["id"] = remap.at(id);
            const auto parent = effective.value("parent", "");
            if (remap.contains(parent))
                clone["parent"] = remap.at(parent);
            if (id == selected_) {
                auto pos = effective.value("position", ContentValue::array({0, 0, 0}));
                constexpr double duplicateOffsetMeters = .5;
                pos[0] = pos.at(0).get<double>() + duplicateOffsetMeters;
                clone["position"] = std::move(pos);
            }
            nodes.elements().push_back(std::move(clone));
        }
        commitNodes(std::move(nodes), tr("Дублировать объект"), remap.at(selected_));
    }
    const SceneNode* compiledNode(std::string_view id) const {
        if (package_)
            for (const auto& node : package_->nodes)
                if (node.id == id)
                    return &node;
        return nullptr;
    }
    void reparent(const std::string& parent) {
        if (!current_ || selected_.empty())
            return;
        try {
            const auto* node = compiledNode(selected_);
            if (!node)
                return;
            const auto* destination = compiledNode(parent);
            auto candidate = *current_;
            setEffectiveProperty(candidate, "parent", parent);
            const auto position =
                destination ? rotateY(node->transform.position - destination->transform.position,
                                      -destination->yaw) /
                                  destination->transform.scale
                            : node->transform.position;
            candidate.setProperty(selected_, "position",
                                  ContentValue::array({position.x, position.y, position.z}));
            candidate.setProperty(selected_, "yaw",
                                  node->yaw - (destination ? destination->yaw : 0));
            candidate.setProperty(selected_, "scale",
                                  node->transform.scale /
                                      (destination ? destination->transform.scale : 1));
            commitNodes(candidate.nodes(), tr("Изменить родителя"), selected_);
        } catch (const std::exception& error) {
            problem(text(error.what()));
            inspect();
        }
    }
    void transform(Viewport::Tool tool, Vec3 delta, float amount) {
        if (!current_ || selected_.empty())
            return;
        try {
            auto candidate = *current_;
            const auto effective = project_->effective(current_->node(selected_));
            if (tool == Viewport::Tool::Move) {
                const auto* parent = compiledNode(effective.value("parent", ""));
                if (parent)
                    delta = rotateY(delta, -parent->yaw) / parent->transform.scale;
                auto pos = effective.value("position", ContentValue::array({0, 0, 0}));
                pos[0] = pos.at(0).get<float>() + delta.x;
                pos[1] = pos.at(1).get<float>() + delta.y;
                constexpr size_t zComponent = 2;
                pos[zComponent] = pos.at(zComponent).get<float>() + delta.z;
                candidate.setProperty(selected_, "position", pos);
            } else if (tool == Viewport::Tool::Rotate)
                candidate.setProperty(selected_, "yaw", effective.value("yaw", 0.0) + amount);
            else if (tool == Viewport::Tool::Scale)
                candidate.setProperty(selected_, "scale", effective.value("scale", 1.0) * amount);
            commitNodes(candidate.nodes(), tr("Трансформировать объект"), selected_);
        } catch (const std::exception& error) {
            problem(text(error.what()));
            inspect();
        }
    }
    void resetTransform() {
        if (!current_ || selected_.empty())
            return;
        auto candidate = *current_;
        for (const auto key : {"position", "yaw", "scale"})
            candidate.setProperty(selected_, key, {});
        auto nodes = candidate.nodes();
        for (auto& node : nodes)
            if (node.at("id") == selected_ && node.contains("remove")) {
                std::erase_if(node["remove"].elements(), [](const ContentValue& name) {
                    return name == "position" || name == "yaw" || name == "scale";
                });
                if (node.at("remove").empty())
                    node.erase("remove");
            }
        candidate.replaceNodes(std::move(nodes));
        commitNodes(candidate.nodes(), tr("Сбросить трансформ"), selected_);
    }
    void requestPreview() {
        viewport_->editingEnabled(false);
        if (!project_)
            return;
        try {
            project_->verifySources();
        } catch (const std::exception& error) {
            problem(text(error.what()));
            return;
        }
        if (worker_.isRunning()) {
            pendingPreview_ = true;
            return;
        }
        runningRevision_ = revision_;
        statusBar()->showMessage(tr("Проверка сцены…"));
        worker_.setFuture(QtConcurrent::run(compile, project_->root, project_->manifest,
                                            project_->snapshot(), options_));
    }
    void publish(const PreviewResult& result) {
        if (!result.package) {
            problem(tr("Сцена требует исправления: ") + text(result.error));
            return;
        }
        viewport_->editingEnabled(true);
        package_ = result.package;
        publishedRevision_ = revision_;
        problems_->clear();
        if (!viewportError_.isEmpty())
            problem(viewportError_);
        if (current_)
            viewport_->scene(package_, current_->data().at("scene").at("id").get<std::string>(),
                             false);
        updateState();
    }
    bool save(const std::shared_ptr<authoring::SceneDocument>& document) {
        if (!document->dirty())
            return true;
        try {
            project_->verifySources();
            const auto checked =
                compile(project_->root, project_->manifest, project_->snapshot(), options_);
            if (!checked.package)
                throw std::runtime_error(checked.error);
            const auto output = document->serialized();
            QSaveFile file(pathText(document->path()));
            file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly))
                throw std::runtime_error(file.errorString().toStdString());
            if (file.write(output.data(), static_cast<qint64>(output.size())) !=
                static_cast<qint64>(output.size()))
                throw std::runtime_error(file.errorString().toStdString());
            // Compare immediately before atomic replacement. Cooperating editors share the project
            // lock.
            project_->verifySources();
            if (bytes(document->path()) != document->original()) {
                file.cancelWriting();
                throw std::runtime_error("Файл изменён вне редактора. Запись отменена; откройте "
                                         "проект заново после согласования изменений.");
            }
            if (!file.commit())
                throw std::runtime_error(file.errorString().toStdString());
            document->acceptSaved(output);
            publish(checked);
            updateState();
            return true;
        } catch (const std::exception& error) {
            problem(text(error.what()));
            QMessageBox::warning(this, tr("Сцена не сохранена"), text(error.what()));
            return false;
        }
    }
    bool confirmChanges() {
        if (!project_)
            return true;
        std::vector<std::shared_ptr<authoring::SceneDocument>> dirty;
        for (const auto& [path, doc] : project_->scenes)
            if (doc->dirty())
                dirty.push_back(doc);
        if (dirty.empty())
            return true;
        const auto answer = QMessageBox::question(
            this, tr("Несохранённые изменения"),
            tr("Сохранить изменённые сцены перед закрытием? Каждая сцена сохраняется отдельно."),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return false;
        if (answer == QMessageBox::Save)
            for (const auto& doc : dirty)
                if (!save(doc))
                    return false;
        return true;
    }
    void updateState() {
        const bool dirty = project_ && std::ranges::any_of(project_->scenes, [](const auto& item) {
                               return item.second->dirty();
                           });
        setWindowTitle(text(options_.title) + (project_ ? " — " + text(project_->name) : "") +
                       (dirty ? " *" : ""));
        save_->setEnabled(current_ && current_->dirty());
        if (project_)
            statusBar()->showMessage(dirty ? tr("Есть несохранённые изменения")
                                           : tr("Сцена сохранена • Правая кнопка: осмотр • Колесо: "
                                                "движение • F: выбранный объект"));
        else
            statusBar()->showMessage(tr("Откройте проект .paperproject"));
    }
    QByteArray defaultLayout_;
    QList<QAction*> toolActions_;
    QLineEdit* name_ = nullptr;
    QLineEdit* hierarchySearch_ = nullptr;
    QLineEdit* assetSearch_ = nullptr;
    QComboBox* parent_ = nullptr;
    QComboBox* resource_ = nullptr;
    QCheckBox* shadow_ = nullptr;
    QTreeWidget* assets_ = nullptr;
    QAction* duplicate_ = nullptr;
    QAction* remove_ = nullptr;
    QString viewportError_;
    Options options_;
    std::unique_ptr<Project> project_;
    std::shared_ptr<authoring::SceneDocument> current_;
    std::shared_ptr<ScenePackage> package_;
    Viewport* viewport_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QComboBox* sceneList_ = nullptr;
    QLabel* selection_ = nullptr;
    QListWidget* problems_ = nullptr;
    QAction* save_ = nullptr;
    std::array<QDoubleSpinBox*, propertyCount> fields_{};
    std::array<double, propertyCount> displayed_{};
    std::map<std::string, QTreeWidgetItem*, std::less<>> items_;
    std::string selected_;
    QUndoStack undo_;
    QFutureWatcher<PreviewResult> worker_;
    size_t revision_ = 0, runningRevision_ = 0, publishedRevision_ = 0;
    bool updating_ = false, pendingPreview_ = false;
};
} // namespace
std::unique_ptr<QMainWindow> makeWorkspace(Options options) {
    return std::make_unique<Window>(std::move(options));
}
int run(int argc, char** argv, Options options) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("PaperEngine");
    QCoreApplication::setApplicationName("PaperEditor");
    const auto settingsDirectory = qEnvironmentVariable("PAPER_EDITOR_SETTINGS_DIR");
    if (!settingsDirectory.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory);
    }
    if (app.arguments().size() > 1)
        options.project = filePath(app.arguments().at(1));
    auto window = makeWorkspace(std::move(options));
    window->show();
    return app.exec();
}
} // namespace paper::editor
