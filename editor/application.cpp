#include "paper/editor/application.hpp"
#include "paper/authoring/document.hpp"
#include "paper/content/document.hpp"
#include "viewport.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <array>
#include <set>

namespace paper::editor {
namespace {
namespace fs = std::filesystem;
constexpr int initialWidth = 1400, initialHeight = 900, transformDecimals = 6;
constexpr int historyLimit = 128;
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
class PropertyCommand final : public QUndoCommand {
  public:
    PropertyCommand(std::function<void(const ContentValue&)> apply, ContentValue before,
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
            if (found != items_.end()) {
                tree_->setCurrentItem(found->second);
                tree_->scrollToItem(found->second);
            }
        };
        viewport_->failed = [this](std::string error) {
            viewportError_ = tr("Ошибка 3D-вида: ") + text(error);
            problem(viewportError_);
        };
        auto* hierarchy = new QWidget;
        auto* layout = new QVBoxLayout(hierarchy);
        sceneList_ = new QComboBox;
        sceneList_->setAccessibleName(tr("Сцена"));
        layout->addWidget(sceneList_);
        auto* search = new QLineEdit;
        search->setPlaceholderText(tr("Поиск объекта или ID"));
        layout->addWidget(search);
        tree_ = new QTreeWidget;
        tree_->setHeaderLabels({tr("Объект"), tr("ID")});
        layout->addWidget(tree_);
        dock(tr("Объекты"), "hierarchy", hierarchy, Qt::LeftDockWidgetArea);
        auto* inspector = new QWidget;
        auto* form = new QFormLayout(inspector);
        selection_ = new QLabel(tr("Выберите объект"));
        selection_->setWordWrap(true);
        form->addRow(selection_);
        const std::array<QString, 5> labels{tr("X, м"), tr("Y, м"), tr("Z, м"), tr("Поворот Y, °"),
                                            tr("Масштаб")};
        for (size_t i = 0; i < fields_.size(); ++i) {
            auto* spin = fields_[i] = new QDoubleSpinBox;
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
        dock(tr("Свойства"), "inspector", inspector, Qt::RightDockWidgetArea);
        problems_ = new QListWidget;
        dock(tr("Проверка проекта"), "problems", problems_, Qt::BottomDockWidgetArea);
        auto* file = menuBar()->addMenu(tr("Файл"));
        auto* open = file->addAction(tr("Открыть проект…"));
        open->setShortcut(QKeySequence::Open);
        connect(open, &QAction::triggered, this, [this] {
            if (confirmChanges())
                chooseProject();
        });
        save_ = file->addAction(tr("Сохранить сцену"));
        save_->setShortcut(QKeySequence::Save);
        connect(save_, &QAction::triggered, this, [this] {
            if (current_)
                save(current_);
        });
        auto* exit = file->addAction(tr("Закрыть"));
        exit->setShortcut(QKeySequence::Quit);
        connect(exit, &QAction::triggered, this, &QWidget::close);
        auto* edit = menuBar()->addMenu(tr("Правка"));
        auto* undo = undo_.createUndoAction(this, tr("Отменить"));
        undo->setShortcut(QKeySequence::Undo);
        edit->addAction(undo);
        auto* redo = undo_.createRedoAction(this, tr("Повторить"));
        redo->setShortcut(QKeySequence::Redo);
        edit->addAction(redo);
        auto* view = menuBar()->addMenu(tr("Вид"));
        auto* frame = view->addAction(tr("Приблизить выбранный объект"));
        frame->setShortcut(Qt::Key_F);
        frame->setShortcutContext(Qt::WidgetShortcut);
        viewport_->addAction(frame);
        connect(frame, &QAction::triggered, this, [this] { viewport_->frame(selected_); });
        auto* refresh = view->addAction(tr("Обновить 3D-вид"));
        refresh->setShortcut(QKeySequence::Refresh);
        connect(refresh, &QAction::triggered, this, [this] { requestPreview(); });
        for (auto* panel : findChildren<QDockWidget*>())
            view->addAction(panel->toggleViewAction());
        auto* toolbar = addToolBar(tr("Проект"));
        toolbar->setObjectName("projectToolbar");
        toolbar->addAction(open);
        toolbar->addAction(save_);
        toolbar->addSeparator();
        toolbar->addAction(undo);
        toolbar->addAction(redo);
        toolbar->addAction(frame);
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
            if (runningRevision_ == revision_)
                publish(result);
            if (pendingPreview_) {
                pendingPreview_ = false;
                requestPreview();
            }
        });
        QSettings settings;
        restoreGeometry(settings.value("window/geometry").toByteArray());
        restoreState(settings.value("window/layout").toByteArray());
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
        inspect();
        if (package_)
            viewport_->scene(package_, current_->data().at("scene").at("id").get<std::string>(),
                             true);
        updateState();
    }
    void inspect() {
        updating_ = true;
        const bool available = current_ && !selected_.empty();
        for (auto* field : fields_)
            field->setEnabled(available);
        selection_->setText(available ? text(selected_) : tr("Выберите объект"));
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
        const auto before = current_->property(selected_, name);
        auto after =
            project_->effective(current_->node(selected_))
                .value(name, component < yawField ? ContentValue::array({0, 0, 0})
                                                  : ContentValue(component == scaleField ? 1 : 0));
        if (component < yawField)
            after[component] = fields_[component]->value();
        else
            after = fields_[component]->value() *
                    (component == yawField ? pi3 / units::degreesPerHalfTurn : 1);
        const auto doc = current_;
        const auto id = selected_;
        try {
            // Validate before Qt takes ownership; exceptions must not escape an event handler.
            auto validated = *doc;
            validated.setProperty(id, name, after);
            undo_.push(new PropertyCommand(
                [this, doc, id, name](const auto& value) {
                    doc->setProperty(id, name, value);
                    ++revision_;
                    inspect();
                    updateState();
                    requestPreview();
                },
                before, std::move(after), tr("Изменить %1: %2").arg(text(name), text(id))));
        } catch (const std::exception& error) {
            problem(text(error.what()));
            inspect();
        }
    }
    void requestPreview() {
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
        package_ = result.package;
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
    size_t revision_ = 0, runningRevision_ = 0;
    bool updating_ = false, pendingPreview_ = false;
};
} // namespace
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
    Window window(std::move(options));
    window.show();
    return app.exec();
}
} // namespace paper::editor
