#include "paper/editor/application.hpp"
#include "audio_bank_editor.hpp"
#include "paper/authoring/document.hpp"
#include "paper/authoring/source.hpp"
#include "paper/content/document.hpp"
#include "paper/content/strings.hpp"
#include "paper/physics/scene.hpp"
#include "paper/scenes/transforms.hpp"
#include "play_controller.hpp"
#include "project_migration.hpp"
#include "project_storage.hpp"
#include "property_form.hpp"
#include "resource_browser.hpp"
#include "scene_data_editor.hpp"
#include "text_asset_editor.hpp"
#include "ui_editor.hpp"
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
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
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
constexpr int resourcesInitialHeight = 280, problemsInitialHeight = 120;
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
    std::unique_ptr<ProjectStorage> storage;
    std::map<fs::path, std::string> sources;
    ContentValue world;
    std::map<fs::path, std::shared_ptr<authoring::SceneDocument>> knownScenes;
    std::set<fs::path> newScenes;
    fs::path worldPath() const { return fs::weakly_canonical(root / manifest); }
    std::string worldSource() const {
        const auto& baseline = sources.at(worldPath());
        return authoring::patchSource(baseline, content::parse(baseline, manifest.string()), world,
                                      manifest.string());
    }
    bool dirty() const {
        return worldSource() != sources.at(worldPath()) ||
               std::ranges::any_of(scenes, [this](const auto& entry) {
                   return entry.second->dirty() || newScenes.contains(entry.first);
               });
    }
    std::optional<std::string> baseline(const fs::path& path) const {
        return newScenes.contains(path) ? std::nullopt : std::optional(scenes.at(path)->original());
    }
    void verifySources() const {
        for (const auto& [path, original] : sources)
            if (bytes(path) != original)
                throw std::runtime_error("Project dependencies changed outside the editor; reopen "
                                         "the project before editing or saving");
        for (const auto& [path, scene] : scenes)
            if ((newScenes.contains(path) && fs::exists(scene->path())) ||
                (!newScenes.contains(path) && bytes(scene->path()) != scene->original()))
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
        storage = std::make_unique<ProjectStorage>(file);
        if (acquireLock)
            storage->finishSave();
        ResourceStore files(root);
        world = readSource(files.resolve(manifest));
        for (const auto& relative : world.at("world").at("resources"))
            (void)readSource(files.resolve(relative.get<std::string>()));
        for (const auto& relative : world.at("world").at("scenes")) {
            const fs::path relativePath = relative.get<std::string>();
            const auto full = files.resolve(relativePath);
            scenes.emplace(relativePath,
                           std::make_shared<authoring::SceneDocument>(full, bytes(full)));
        }
        knownScenes = scenes;
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
            labels = content::stringTable(strings.at("strings"));
        }
    }
    SceneDocuments snapshot() const {
        SceneDocuments result;
        result.emplace(manifest, world);
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
            const auto world = documents.contains(manifest) ? documents.at(manifest)
                                                            : content::read(root / manifest);
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
        auto package = loadScenes(root, std::move(materials), names, manifest, documents);
        std::set<std::string> collisionScenes;
        for (const auto& node : package->nodes)
            if (node.collidable)
                collisionScenes.insert(node.scene);
        for (const auto& scene : collisionScenes) {
            PhysicsLimits limits;
            limits.bodies =
                static_cast<unsigned>(std::ranges::count_if(package->nodes, [&](const auto& node) {
                    return node.collidable && node.scene == scene;
                }));
            PhysicsWorld world(limits);
            (void)addSceneColliders(world, *package, scene);
        }
        if (options.validateScenes)
            options.validateScenes(*package);
        return {std::move(package), {}};
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
            viewportError_ = tr("Viewport error: ") + text(error);
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
        sceneList_->setAccessibleName(tr("Scene"));
        layout->addWidget(sceneList_);
        auto* search = hierarchySearch_ = new QLineEdit;
        search->setPlaceholderText(tr("Search object or ID"));
        layout->addWidget(search);
        tree_ = new QTreeWidget;
        tree_->setObjectName("sceneHierarchy");
        tree_->setAccessibleName(tr("Scene hierarchy"));
        tree_->setAlternatingRowColors(true);
        tree_->setContextMenuPolicy(Qt::ActionsContextMenu);
        tree_->setMinimumWidth(hierarchyMinimumWidth);
        tree_->setHeaderLabels({tr("Object"), tr("ID")});
        layout->addWidget(tree_);
        dock(tr("Hierarchy"), "hierarchy", hierarchy, Qt::LeftDockWidgetArea);
        auto* inspector = new QWidget;
        auto* form = new QFormLayout(inspector);
        selection_ = new QLabel(tr("Select an object"));
        selection_->setWordWrap(true);
        form->addRow(selection_);
        name_ = new QLineEdit;
        name_->setObjectName("nodeLabel");
        name_->setAccessibleName(tr("Object name"));
        form->addRow(tr("Name / Localization key"), name_);
        connect(name_, &QLineEdit::editingFinished, this,
                [this] { editProperty("label", name_->text().toStdString()); });
        parent_ = new QComboBox;
        parent_->setObjectName("nodeParent");
        parent_->setAccessibleName(tr("Object parent"));
        form->addRow(tr("Parent"), parent_);
        connect(parent_, &QComboBox::activated, this,
                [this] { reparent(parent_->currentData().toString().toStdString()); });
        resource_ = new QComboBox;
        resource_->setObjectName("nodeResource");
        resource_->setAccessibleName(tr("Object resource"));
        form->addRow(tr("Resource"), resource_);
        connect(resource_, &QComboBox::activated, this, [this] {
            editProperty("resource", resource_->currentData().toString().toStdString());
        });
        const std::array<QString, 12> labels{
            tr("X, m"),          tr("Y, m"),         tr("Z, m"),         tr("Y rotation, °"),
            tr("Uniform scale"), tr("Quaternion X"), tr("Quaternion Y"), tr("Quaternion Z"),
            tr("Quaternion W"),  tr("Scale X"),      tr("Scale Y"),      tr("Scale Z")};
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
            if (i == scaleField || i >= axisScaleField)
                spin->setRange(sceneLimits::scaleMinimum, sceneLimits::scaleMaximum);
            constexpr double positionStepMeters = .1, rotationStepDegrees = 1;
            spin->setSingleStep(i == yawField ? rotationStepDegrees : positionStepMeters);
            if (i >= quaternionField && i < axisScaleField) {
                constexpr double componentStep = .01;
                spin->setRange(-1, 1);
                spin->setSingleStep(componentStep);
            }
            spin->setAccessibleName(labels[i]);
            form->addRow(labels[i], spin);
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this, i] { change(i); });
        }
        const std::array<QString, 3> angleLabels{tr("Local pitch X, °"), tr("Local yaw Y, °"),
                                                 tr("Local roll Z, °")};
        for (size_t i = 0; i < euler_.size(); ++i) {
            auto* field = euler_[i] = new QDoubleSpinBox;
            field->setObjectName(QString("rotationDegrees%1").arg(i));
            field->setDecimals(transformDecimals);
            constexpr double fullTurnDegrees = units::degreesPerHalfTurn * 2;
            field->setRange(-fullTurnDegrees, fullTurnDegrees);
            field->setKeyboardTracking(false);
            field->setToolTip(tr("Local rotation in degrees, applied in Y × X × Z order. "
                                 "Quaternion storage retains free rotation."));
            form->addRow(angleLabels[i], field);
            connect(field, &QDoubleSpinBox::editingFinished, this, [this] { changeEuler(); });
        }
        auto* hint = new QLabel(tr("Coordinates are local to the parent. Editing creates "
                                   "an explicit template override."));
        hint->setWordWrap(true);
        form->addRow(hint);
        shadow_ = new QCheckBox(tr("Cast shadows"));
        connect(shadow_, &QCheckBox::clicked, this,
                [this](bool checked) { editProperty("shadow", checked); });
        form->addRow(shadow_);
        collision_ = new QCheckBox(tr("Collision"));
        collision_->setObjectName("nodeCollision");
        collisionShape_ = new QComboBox;
        collisionShape_->addItems({tr("Bounds"), tr("Static mesh")});
        collisionShape_->setObjectName("collisionShape");
        collisionCategory_ = new QLineEdit;
        collisionCategory_->setObjectName("collisionCategory");
        collisionMask_ = new QLineEdit;
        collisionMask_->setObjectName("collisionMask");
        form->addRow(collision_);
        form->addRow(tr("Collider shape"), collisionShape_);
        form->addRow(tr("Category bits"), collisionCategory_);
        form->addRow(tr("Mask bits"), collisionMask_);
        boundsButton_ = new QPushButton(tr("Edit Collider Bounds…"));
        boundsButton_->setObjectName("editColliderBounds");
        form->addRow(boundsButton_);
        connect(boundsButton_, &QPushButton::clicked, this, [this] { editBounds(); });
        connect(collision_, &QCheckBox::clicked, this, [this](bool checked) {
            if (checked)
                editCollision();
            else
                editProperty("collision", false);
        });
        connect(collisionShape_, &QComboBox::activated, this, [this] { editCollision(); });
        for (auto* field : {collisionCategory_, collisionMask_})
            connect(field, &QLineEdit::editingFinished, this, [this] { editCollision(); });
        auto* reset = new QPushButton(tr("Reset Transform to Template"));
        connect(reset, &QPushButton::clicked, this, [this] { resetTransform(); });
        form->addRow(reset);
        auto* objectProperties = new QPushButton(tr("Edit Object Properties…"));
        objectProperties->setObjectName("objectProperties");
        connect(objectProperties, &QPushButton::clicked, this, [this] { editObjectProperties(); });
        form->addRow(objectProperties);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(inspector);
        scroll->setMinimumWidth(inspectorMinimumWidth);
        dock(tr("Inspector"), "inspector", scroll, Qt::RightDockWidgetArea);
        assets_ = new ResourceBrowser;
        assets_->setObjectName("projectBrowser");
        assets_->instantiate = [this](const std::string& id) { createNode(id); };
        assets_->openScene = [this](const fs::path& path) {
            const auto index = sceneList_->findData(pathText(path));
            if (index >= 0)
                sceneList_->setCurrentIndex(index);
            else
                statusBar()->showMessage(tr("This scene is not included in the project manifest"));
        };
        assets_->floatPanel = [this] {
            auto* panel = findChild<QDockWidget*>("assets");
            panel->setFloating(!panel->isFloating());
            panel->show();
            panel->raise();
        };
        assets_->openAsset = [this](const fs::path& path) { editAsset(project_->root / path); };
        for (const auto& type : options_.textAssets)
            assets_->editableExtensions.push_back(text(type.extension));
        dock(tr("Project"), "assets", assets_, Qt::BottomDockWidgetArea);
        problems_ = new QListWidget;
        problems_->setObjectName("projectProblems");
        dock(tr("Problems"), "problems", problems_, Qt::BottomDockWidgetArea);
        console_ = new QPlainTextEdit;
        console_->setObjectName("playConsole");
        console_->setReadOnly(true);
        constexpr int consoleLineLimit = 1000;
        console_->setMaximumBlockCount(consoleLineLimit);
        dock(tr("Console"), "console", console_, Qt::BottomDockWidgetArea);
        play_ = new PlayController(this);
        play_->setObjectName("playController");
        play_->output = [this](const QString& value) { console_->appendPlainText(value); };
        play_->changed = [this](PlayController::State) {
            updatePlay();
            if (!play_->active() && closeAfterStop_)
                QTimer::singleShot(0, this, &QWidget::close);
        };
        auto* file = menuBar()->addMenu(tr("File"));
        auto* open = open_ = file->addAction(tr("Open Project…"));
        open->setShortcut(QKeySequence::Open);
        connect(open, &QAction::triggered, this, [this] {
            if (!play_->active() && confirmChanges())
                chooseProject();
        });
        save_ = file->addAction(tr("Save Scene"));
        save_->setObjectName("saveScene");
        save_->setShortcut(QKeySequence::Save);
        connect(save_, &QAction::triggered, this, [this] {
            if (current_)
                save(current_);
        });
        auto* newScene = file->addAction(tr("New Scene…"));
        newScene->setObjectName("newScene");
        connect(newScene, &QAction::triggered, this, [this] { createScene(); });
        auto* sceneData = file->addAction(tr("Edit Scene Data…"));
        sceneData->setObjectName("editSceneData");
        connect(sceneData, &QAction::triggered, this, [this] { editSceneData(); });
        auto* entrySpawn = file->addAction(tr("Set Project Entry Spawn…"));
        entrySpawn->setObjectName("setEntrySpawn");
        connect(entrySpawn, &QAction::triggered, this, [this] {
            if (!project_ || !package_)
                return;
            QStringList spawns;
            for (const auto& spawn : package_->spawns)
                spawns.push_back(text(spawn.id));
            bool accepted = false;
            const auto selected = QInputDialog::getItem(
                this, tr("Project Entry Spawn"), tr("Spawn"), spawns,
                std::max(0, static_cast<int>(spawns.indexOf(text(package_->entrySpawn)))), false,
                &accepted);
            if (accepted) {
                auto state = projectState();
                state["world"]["world"]["entrySpawn"] = selected.toStdString();
                commitProject(std::move(state), tr("Set Entry Spawn"));
            }
        });
        auto* gameplay = file->addAction(tr("Edit Object Properties…"));
        gameplay->setObjectName("editObjectProperties");
        connect(gameplay, &QAction::triggered, this, [this] { editObjectProperties(); });
        auto* saveAll = file->addAction(tr("Save All"));
        saveAll->setObjectName("saveAll");
        saveAll->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
        connect(saveAll, &QAction::triggered, this, [this] {
            if (project_)
                saveBatch(nullptr);
        });
        auto* finishSave = file->addAction(tr("Finish Interrupted Save"));
        finishSave->setObjectName("finishSave");
        connect(finishSave, &QAction::triggered, this, [this] {
            if (!project_)
                return;
            try {
                if (!project_->storage->finishSave())
                    return;
                for (const auto& [path, doc] : project_->scenes) {
                    doc->rebaseSaved(bytes(doc->path()));
                    project_->newScenes.erase(path);
                }
                for (auto& [path, source] : project_->sources)
                    source = bytes(path);
                const auto reconcile = [](auto* editor) {
                    if (!editor || !editor->isVisible())
                        return;
                    const auto draft = editor->recoverySource();
                    editor->acceptSaved(bytes(editor->file()));
                    editor->restoreSource(draft);
                };
                reconcile(uiEditor_.get());
                reconcile(audioEditor_.get());
                for (auto& [path, editor] : textEditors_)
                    reconcile(editor.get());
                recoveryDeferred_ = false;
                requestPreview();
                updateState();
                autosaveProject();
            } catch (const std::exception& e) {
                problem(text(e.what()));
            }
        });
        auto* autosave = file->addAction(tr("Create Recovery Snapshot"));
        autosave->setObjectName("autosaveNow");
        connect(autosave, &QAction::triggered, this, [this] { autosaveProject(); });
        auto* restore = file->addAction(tr("Restore Recovery Snapshot…"));
        restore->setObjectName("restoreRecovery");
        connect(restore, &QAction::triggered, this, [this] { restoreRecovery(); });
        auto* discardRecovery = file->addAction(tr("Discard Recovery Snapshot…"));
        discardRecovery->setObjectName("discardRecovery");
        connect(discardRecovery, &QAction::triggered, this, [this] {
            if (project_ && QMessageBox::question(this, tr("Discard Recovery"),
                                                  tr("Remove the recovery snapshot? Authored files "
                                                     "and current drafts are unchanged."),
                                                  QMessageBox::Discard | QMessageBox::Cancel,
                                                  QMessageBox::Cancel) == QMessageBox::Discard) {
                try {
                    project_->storage->discardRecovery();
                    recoveryDeferred_ = false;
                } catch (const std::exception& e) {
                    problem(text(e.what()));
                }
            }
        });
        auto* exit = file->addAction(tr("Close"));
        exit->setShortcut(QKeySequence::Quit);
        connect(exit, &QAction::triggered, this, &QWidget::close);
        auto* audioBank = file->addAction(tr("Edit Audio Bank…"));
        audioBank->setObjectName("editAudioBank");
        connect(audioBank, &QAction::triggered, this, [this] {
            if (!project_) {
                problem(tr("Open a project before editing its audio bank"));
                return;
            }
            const auto file =
                QFileDialog::getOpenFileName(this, tr("Open Audio Bank"), pathText(project_->root),
                                             tr("Audio Bank (*.pabank *.toml)"));
            if (file.isEmpty())
                return;
            editAsset(filePath(file));
        });
        auto* uiFile = file->addAction(tr("Edit UI…"));
        uiFile->setObjectName("editUi");
        connect(uiFile, &QAction::triggered, this, [this] {
            if (!project_)
                return;
            const auto path = QFileDialog::getOpenFileName(
                this, tr("Open UI"), pathText(project_->root), tr("Paper UI (*.pui)"));
            if (!path.isEmpty())
                editAsset(filePath(path));
        });
        for (const auto& [name, title, extension] :
             std::initializer_list<std::tuple<const char*, QString, const char*>>{
                 {"newUi", tr("New UI…"), ".pui"},
                 {"newAudioBank", tr("New Audio Bank…"), ".pabank"}}) {
            auto* action = file->addAction(title);
            action->setObjectName(name);
            connect(action, &QAction::triggered, this,
                    [this, extension, title] { newAsset(extension, title); });
        }
        for (const auto& type : options_.textAssets) {
            auto* action = file->addAction(tr("Edit %1…").arg(text(type.name)));
            action->setObjectName("editTextAsset" + text(type.extension));
            connect(action, &QAction::triggered, this, [this, type] {
                if (!project_)
                    return;
                const auto chosen = QFileDialog::getOpenFileName(
                    this, text(type.name), pathText(project_->root),
                    text(type.name) + " (*" + text(type.extension) + ")");
                if (!chosen.isEmpty())
                    editAsset(filePath(chosen));
            });
        }
        auto* upgrade = file->addAction(tr("Upgrade Project Copy for Free Transforms…"));
        upgrade->setObjectName("upgradeProject");
        connect(upgrade, &QAction::triggered, this, [this] {
            if (!project_ || play_->active())
                return;
            const auto choice = QFileDialog::getSaveFileName(
                this, tr("New Project Folder"),
                pathText(project_->file.parent_path().parent_path() /
                         (project_->file.parent_path().filename().string() + "-v3")),
                tr("Project folder"));
            if (choice.isEmpty())
                return;
            try {
                // Resolve asset Save/Discard before copying, so later saves cannot leave
                // the new project with older UI or audio content.
                if (!closeAssetEditors())
                    return;
                project_->verifySources();
                const auto file =
                    migrateProject(project_->file, filePath(choice), project_->snapshot(),
                                   [this](const fs::path& file) {
                                       Project candidate(file, false);
                                       const auto checked =
                                           compile(candidate.root, candidate.manifest,
                                                   candidate.snapshot(), options_);
                                       if (!checked.package)
                                           throw std::runtime_error(checked.error);
                                       project_->verifySources();
                                   });
                openProject(file);
            } catch (const std::exception& e) {
                problem(text(e.what()));
            }
        });
        auto* edit = menuBar()->addMenu(tr("Edit"));
        auto* undo = undo_.createUndoAction(this, tr("Undo"));
        undo->setObjectName("undo");
        undo->setShortcut(QKeySequence::Undo);
        edit->addAction(undo);
        auto* redo = undo_.createRedoAction(this, tr("Redo"));
        redo->setObjectName("redo");
        redo->setShortcut(QKeySequence::Redo);
        edit->addAction(redo);
        auto* create = menuBar()->addMenu(tr("Object"));
        auto* group = create->addAction(tr("Create Empty"));
        group->setObjectName("createGroup");
        connect(group, &QAction::triggered, this, [this] { createNode(""); });
        auto* instance = create->addAction(tr("Add Selected Resource"));
        instance->setObjectName("createResource");
        connect(instance, &QAction::triggered, this, [this] { createResource(); });
        duplicate_ = create->addAction(tr("Duplicate"));
        duplicate_->setObjectName("duplicateNode");
        remove_ = create->addAction(tr("Delete with Children"));
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
        auto* view = menuBar()->addMenu(tr("View"));
        auto* frame = view->addAction(tr("Frame Selected"));
        frame->setShortcut(Qt::Key_F);
        frame->setShortcutContext(Qt::WidgetShortcut);
        viewport_->addAction(frame);
        tree_->addAction(frame);
        connect(frame, &QAction::triggered, this, [this] { viewport_->frame(selected_); });
        auto* refresh = view->addAction(tr("Refresh Viewport"));
        refresh->setShortcut(QKeySequence::Refresh);
        connect(refresh, &QAction::triggered, this, [this] {
            ++revision_;
            requestPreview();
        });
        for (auto* panel : findChildren<QDockWidget*>())
            view->addAction(panel->toggleViewAction());
        auto* resetLayout = view->addAction(tr("Reset Panel Layout"));
        connect(resetLayout, &QAction::triggered, this, [this] {
            restoreState(defaultLayout_);
            for (auto* dock : findChildren<QDockWidget*>())
                dock->show();
        });
        auto* toolbar = addToolBar(tr("Project"));
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
        auto* playMenu = menuBar()->addMenu(tr("Play"));
        playAction_ = playMenu->addAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play"));
        playAction_->setObjectName("play");
        playAction_->setShortcut(Qt::Key_F5);
        playAction_->setToolTip(tr(
            "Play unsaved scenes in a separate game window (F5). Changes apply on the next Play."));
        stopAction_ = playMenu->addAction(style()->standardIcon(QStyle::SP_MediaStop), tr("Stop"));
        stopAction_->setObjectName("stop");
        stopAction_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
        connect(playAction_, &QAction::triggered, this, [this] { startPlay(); });
        connect(stopAction_, &QAction::triggered, this, [this] { play_->stop(); });
        auto* playback = addToolBar(tr("Play Controls"));
        playback->setObjectName("playToolbar");
        playback->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        playback->addAction(playAction_);
        playback->addAction(stopAction_);
        playback->addWidget(new QLabel(tr("  Start at: ")));
        spawn_ = new QComboBox;
        spawn_->setObjectName("playSpawn");
        spawn_->setAccessibleName(tr("Play start point"));
        spawn_->setToolTip(tr("Start point in the selected scene"));
        playback->addWidget(spawn_);
        playStatus_ = new QLabel;
        playStatus_->setObjectName("playStatus");
        statusBar()->addPermanentWidget(playStatus_);
        connect(spawn_, &QComboBox::currentIndexChanged, this, [this] { updatePlay(); });
        addToolBarBreak();
        auto* tools = addToolBar(tr("Scene Tools"));
        tools->setObjectName("sceneTools");
        auto* modes = new QActionGroup(this);
        const std::array<QString, 4> titles{tr("Select"), tr("Move"), tr("Rotate"), tr("Scale")};
        auto* rotationAxis = new QComboBox;
        rotationAxis_ = rotationAxis;
        rotationAxis->setObjectName("worldRotationAxis");
        rotationAxis->setAccessibleName(tr("World rotation axis"));
        rotationAxis->addItems({tr("Rotate X"), tr("Rotate Y"), tr("Rotate Z")});
        rotationAxis->setCurrentIndex(1);
        tools->addWidget(rotationAxis);
        connect(rotationAxis, &QComboBox::currentIndexChanged, this, [this](int index) {
            constexpr std::array<Vec3, 3> axes{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
            viewport_->rotationAxis(axes.at(static_cast<size_t>(index)));
        });
        auto* colliders = tools->addAction(tr("Collisions"));
        colliders->setObjectName("showCollisions");
        colliders->setCheckable(true);
        colliders->setToolTip(
            tr("Show collider bounds; selected mesh colliders show their triangles"));
        connect(colliders, &QAction::toggled, viewport_, &Viewport::collisions);
        auto* sceneOverlays = tools->addAction(tr("Scene Data"));
        sceneOverlays->setObjectName("showSceneData");
        sceneOverlays->setCheckable(true);
        sceneOverlays->setToolTip(
            tr("Show rooms, transition volumes, spawn directions and lights"));
        connect(sceneOverlays, &QAction::toggled, viewport_, &Viewport::sceneData);
        auto* hideAudio = tools->addAction(tr("Hide Audio Overlays"));
        hideAudio->setObjectName("hideAudioOverlays");
        connect(hideAudio, &QAction::triggered, this, [this] { viewport_->audio(std::nullopt); });
        const std::array<int, 4> shortcuts{Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R};
        for (size_t i = 0; i < titles.size(); ++i) {
            auto* action = tools->addAction(titles[i]);
            action->setCheckable(true);
            action->setChecked(i == 1);
            modes->addAction(action);
            // Fly-camera WASD is handled directly by the viewport while RMB is held.
            action->setToolTip(titles[i] +
                               tr(" • %1 in viewport").arg(QKeySequence(shortcuts[i]).toString()));
            connect(action, &QAction::triggered, this,
                    [this, i] { viewport_->tool(static_cast<Viewport::Tool>(i)); });
        }
        toolActions_ = modes->actions();
        viewport_->toolRequested = [this](Viewport::Tool tool) {
            toolActions_.at(static_cast<int>(tool))->trigger();
        };
        tools->addSeparator();
        auto* grid = tools->addAction(tr("X-ray Grid"));
        grid->setCheckable(true);
        grid->setChecked(false);
        connect(grid, &QAction::toggled, this, [this](bool checked) { viewport_->grid(checked); });
        auto* snap = tools->addAction(tr("Snap: 0.5 m / 15° / 0.1"));
        snap->setCheckable(true);
        connect(snap, &QAction::toggled, this, [this](bool checked) { viewport_->snap(checked); });
        auto* navigation = new QLabel(tr("  RMB + WASD/QE: Fly • MMB: Pan • F: Frame"));
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
        tabifyDockWidget(findChild<QDockWidget*>("problems"), findChild<QDockWidget*>("console"));
        findChild<QDockWidget*>("problems")->raise();
        defaultLayout_ = saveState();
        QSettings settings;
        restoreGeometry(settings.value("window/geometry").toByteArray());
        restoreState(settings.value("window/layout").toByteArray());
        inspect();
        updateState();
        if (!options_.project.empty())
            openProject(options_.project);
        autosaveTimer_.setInterval(autosaveMilliseconds);
        connect(&autosaveTimer_, &QTimer::timeout, this, [this] { autosaveProject(); });
        autosaveTimer_.start();
    }

    ~Window() override {
        play_->changed = {};
        play_->output = {};
    }

  protected:
    void closeEvent(QCloseEvent* event) override {
        if (!closeAssetEditors()) {
            event->ignore();
            return;
        }
        if (!closeAfterStop_ && !confirmChanges()) {
            event->ignore();
            return;
        }
        if (play_->active()) {
            closeAfterStop_ = true;
            play_->stop();
            setEnabled(false);
            event->ignore();
            return;
        }
        if (project_ && !recoveryDeferred_) {
            try {
                project_->storage->discardRecovery();
            } catch (const std::exception& e) {
                problem(text(e.what()));
                event->ignore();
                return;
            }
        }
        QSettings settings;
        settings.setValue("window/geometry", saveGeometry());
        settings.setValue("window/layout", saveState());
        event->accept();
    }

  private:
    void editAsset(const fs::path& path) {
        if (!project_)
            return;
        try {
            const auto registered =
                std::ranges::find_if(options_.textAssets, [&](const auto& type) {
                    return pathText(path.extension())
                               .compare(text(type.extension), Qt::CaseInsensitive) == 0;
                });
            if (registered != options_.textAssets.end()) {
                const auto canonical = fs::canonical(path);
                auto& editor = textEditors_[canonical];
                if (!editor || !editor->isVisible()) {
                    editor = std::make_unique<TextAssetEditor>(canonical, project_->root,
                                                               *registered, this);
                    editor->saveProject = [this] { return saveBatch(nullptr); };
                }
                editor->show();
                editor->raise();
                return;
            }
            if (pathText(path.extension()).compare(".pui", Qt::CaseInsensitive) == 0) {
                if (uiEditor_ && !uiEditor_->close())
                    return;
                uiEditor_ = std::make_unique<UiEditor>(path, project_->root, this);
                uiEditor_->show();
                uiEditor_->raise();
            } else {
                if (audioEditor_ && !audioEditor_->close())
                    return;
                audioEditor_ = std::make_unique<AudioBankEditor>(path, project_->root, this);
                auto& dialog = *audioEditor_;
                std::vector<std::string> nodes;
                for (const auto& node : package_->nodes)
                    nodes.push_back(node.id);
                dialog.sceneNodes(std::move(nodes));
                dialog.currentSceneNodes = [this] {
                    std::vector<std::string> catalog;
                    for (const auto& [scenePath, document] : project_->scenes)
                        for (const auto& node : document->nodes())
                            catalog.push_back(node.at("id").get<std::string>());
                    return catalog;
                };
                dialog.previewChanged = [this](std::optional<AudioBankDefinition> bank) {
                    viewport_->audio(std::move(bank));
                };
                dialog.closed = [this, path] {
                    try {
                        viewport_->audio(loadAudioBank(path));
                        populateAssets();
                    } catch (const std::exception& e) {
                        viewport_->audio(std::nullopt);
                        problem(text(e.what()));
                    }
                };
                dialog.preview();
                dialog.show();
                dialog.raise();
            }
            populateAssets();
        } catch (const std::exception& e) {
            viewport_->audio(std::nullopt);
            problem(text(e.what()));
        }
    }
    bool closeAssetEditors() {
        for (QDialog* dialog :
             {static_cast<QDialog*>(uiEditor_.get()), static_cast<QDialog*>(audioEditor_.get())})
            if (dialog && dialog->isVisible() && !dialog->close())
                return false;
        for (auto& [path, editor] : textEditors_)
            if (editor && editor->isVisible() && !editor->close())
                return false;
        textEditors_.clear();
        uiEditor_.reset();
        audioEditor_.reset();
        return true;
    }
    ContentValue defaultBounds() const {
        Box3 bounds{{}, {1, 1, 1}};
        const auto node = std::ranges::find(package_->nodes, selected_, &SceneNode::id);
        if (node != package_->nodes.end() && node->asset) {
            if (node->asset->mesh)
                bounds = meshBounds(*node->asset->mesh);
            else if (node->asset->model) {
                bool found = false;
                for (const auto& instance : node->asset->model->sample()) {
                    const auto part = worldBounds(meshBounds(*instance.mesh), instance.transform);
                    bounds = found ? unionBounds(bounds, part) : part;
                    found = true;
                }
            }
        }
        for (float* extent : {&bounds.half.x, &bounds.half.y, &bounds.half.z})
            *extent = std::max(*extent, sceneLimits::minimumExtentMeters);
        return ContentValue{
            {"center", ContentValue::array({bounds.center.x, bounds.center.y, bounds.center.z})},
            {"half", ContentValue::array({bounds.half.x, bounds.half.y, bounds.half.z})}};
    }
    void editBounds() {
        if (!current_ || selected_.empty())
            return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Collider Bounds"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new PropertyForm;
        const auto effective = project_->effective(current_->node(selected_));
        ContentValue value;
        if (effective.contains("bounds"))
            value = effective.at("bounds");
        else
            value = defaultBounds();
        if (!value.contains("yaw") && !value.contains("rotation"))
            value["yaw"] = 0.0;
        if (value.contains("yaw"))
            value["yaw"] = value.at("yaw").get<double>();
        if (current_->data().at("version") == content::limits::sceneVersion) {
            const auto q = value.contains("rotation")
                               ? readRotation(value.at("rotation"))
                               : Rotation3::axisAngle({0, 1, 0}, value.at("yaw").get<float>());
            value.erase("yaw");
            value["rotation"] = ContentValue::array({q.x, q.y, q.z, q.w});
        }
        form->setValue(value);
        layout->addWidget(new QLabel(tr("Local center and half extents in metres; rotation uses "
                                        "quaternion XYZW or legacy yaw radians.")));
        layout->addWidget(form);
        auto* status = new QLabel;
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* apply = new QPushButton(tr("Apply"));
        apply->setObjectName("applyColliderBounds");
        layout->addWidget(apply);
        connect(apply, &QPushButton::clicked, &dialog, [this, &dialog, form, status] {
            try {
                auto candidate = *current_;
                candidate.setProperty(selected_, "bounds", form->value());
                auto snapshot = project_->snapshot();
                for (const auto& [path, doc] : project_->scenes)
                    if (doc == current_)
                        snapshot[path] = candidate.data();
                const auto checked =
                    compile(project_->root, project_->manifest, snapshot, options_);
                if (!checked.package)
                    throw std::runtime_error(checked.error);
                (void)candidate.serialized();
                commitNodes(candidate.nodes(), tr("Change Collider Bounds"), selected_);
                dialog.accept();
            } catch (const std::exception& e) {
                status->setText(text(e.what()));
            }
        });
        dialog.exec();
    }
    void newAsset(const std::string& extension, const QString& title) {
        if (!project_)
            return;
        const auto choice = QFileDialog::getSaveFileName(
            this, title, pathText(project_->root / ("new" + extension)),
            tr("Paper asset") + " (*" + text(extension) + ")");
        if (choice.isEmpty())
            return;
        try {
            auto path = filePath(choice);
            if (path.extension().empty())
                path += extension;
            if (path.extension() != extension)
                throw std::runtime_error("Incorrect asset extension");
            ResourceStore files(project_->root);
            path = files.resolve(fs::weakly_canonical(path).lexically_relative(project_->root));
            ui::Document document;
            document.root.id = "root";
            const auto output =
                extension == ".pui" ? ui::writeDocument(document) : writeAudioBank({});
            QFile file(pathText(path));
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                throw std::runtime_error("Choose a new asset filename inside the project");
            if (file.write(output.data(), static_cast<qint64>(output.size())) !=
                    static_cast<qint64>(output.size()) ||
                !file.flush()) {
                file.close();
                file.remove();
                throw std::runtime_error("Cannot write new asset");
            }
            file.close();
            editAsset(path);
        } catch (const std::exception& e) {
            problem(text(e.what()));
        }
    }
    ContentValue projectState() const {
        auto scenes = ContentValue::object(), sources = ContentValue::object();
        for (const auto& [path, doc] : project_->scenes) {
            scenes[path.generic_string()] = doc->data();
            sources[path.generic_string()] = doc->serialized();
        }
        return {{"world", project_->world}, {"scenes", scenes}, {"sources", sources}};
    }
    bool commitProject(ContentValue after, QString title, fs::path active = {}) {
        if (!project_)
            return false;
        try {
            project_->verifySources();
            const auto before = projectState();
            if (before == after)
                return true;
            SceneDocuments documents;
            documents[project_->manifest] = after.at("world");
            (void)authoring::patchSource(project_->sources.at(project_->worldPath()),
                                         content::parse(project_->sources.at(project_->worldPath()),
                                                        project_->manifest.string()),
                                         after.at("world"), project_->manifest.string());
            for (const auto& [path, value] : after.at("scenes").items()) {
                auto candidate = *project_->knownScenes.at(fs::path(path));
                if (after.at("sources").contains(path) &&
                    (!before.at("sources").contains(path) ||
                     after.at("sources").at(path) != before.at("sources").at(path)))
                    candidate.replaceSource(after.at("sources").at(path).get<std::string>());
                candidate.replaceData(value);
                after["sources"][path] = candidate.serialized();
                documents[fs::path(path)] = value;
            }
            const auto checked = compile(project_->root, project_->manifest, documents, options_);
            if (!checked.package)
                throw std::runtime_error(checked.error);
            if (active.empty() && current_)
                active = current_->path().lexically_relative(project_->root);
            undo_.push(new SceneCommand(
                [this, active, initial = checked.package](const ContentValue& state) mutable {
                    project_->world = state.at("world");
                    project_->scenes.clear();
                    for (const auto& [path, value] : state.at("scenes").items()) {
                        auto doc = project_->knownScenes.at(fs::path(path));
                        doc->replaceSource(state.at("sources").at(path).get<std::string>());
                        doc->replaceData(value);
                        project_->scenes.emplace(fs::path(path), std::move(doc));
                    }
                    ++revision_;
                    selected_.clear();
                    {
                        QSignalBlocker block(sceneList_);
                        sceneList_->clear();
                        for (const auto& [path, doc] : project_->scenes)
                            sceneList_->addItem(
                                text(doc->data().at("scene").at("id").get<std::string>()),
                                pathText(path));
                        const auto index = sceneList_->findData(pathText(active));
                        sceneList_->setCurrentIndex(index < 0 ? 0 : index);
                    }
                    current_ = project_->scenes.at(filePath(sceneList_->currentData().toString()));
                    rebuildTree();
                    updateState();
                    if (initial)
                        publish({std::move(initial), {}});
                    else
                        requestPreview();
                },
                before, std::move(after), std::move(title)));
            return true;
        } catch (const std::exception& e) {
            problem(text(e.what()));
            return false;
        }
    }
    void createScene() {
        if (!project_)
            return;
        bool ok = false;
        const auto id =
            QInputDialog::getText(this, tr("New Scene"),
                                  tr("Scene ID (letters, digits, '.', '-' or '_'). A matching "
                                     ".dcscene file is created beside the world manifest on Save."),
                                  QLineEdit::Normal, "new-scene", &ok)
                .toStdString();
        if (!ok || id.empty())
            return;
        try {
            if (id.find('/') != std::string::npos || id.find('\\') != std::string::npos)
                throw std::runtime_error("Scene ID cannot contain path separators");
            const auto relative = project_->manifest.parent_path() / (id + ".dcscene");
            ResourceStore files(project_->root);
            const auto full = files.resolve(relative);
            if (fs::exists(full) || project_->knownScenes.contains(relative))
                throw std::runtime_error("Scene filename is already in use");
            constexpr double initialEyeMeters = 1.62, initialRoomHalfWidthMeters = 10,
                             initialRoomHalfHeightMeters = 5;
            const ContentValue room{
                {"id", id + ".room"},
                {"label", options_.initialRoomLabel.empty() ? id : options_.initialRoomLabel},
                {"floorY", 0.0},
                {"bounds",
                 ContentValue{{"center", ContentValue::array({0.0, 0.0, 0.0})},
                              {"half", ContentValue::array({initialRoomHalfWidthMeters,
                                                            initialRoomHalfHeightMeters,
                                                            initialRoomHalfWidthMeters})}}}};
            const ContentValue spawn{
                {"id", id + ".start"},
                {"position", ContentValue::array({0.0, initialEyeMeters, 0.0})},
                {"yaw", 0.0}};
            const ContentValue data{{"format", "dcmo.scene"},
                                    {"version", project_->world.at("version")},
                                    {"scene", ContentValue{{"id", id},
                                                           {"nodes", ContentValue::array()},
                                                           {"rooms", ContentValue::array({room})},
                                                           {"spawns", ContentValue::array({spawn})},
                                                           {"zones", ContentValue::array()},
                                                           {"lights", ContentValue::array()}}}};
            auto doc = std::make_shared<authoring::SceneDocument>(full, content::encode(data));
            project_->knownScenes.emplace(relative, doc);
            project_->newScenes.insert(relative);
            auto after = projectState();
            after["world"]["world"]["scenes"].push_back(relative.generic_string());
            after["scenes"][relative.generic_string()] = data;
            after["sources"][relative.generic_string()] = doc->serialized();
            if (!commitProject(std::move(after), tr("Create Scene"), relative)) {
                project_->knownScenes.erase(relative);
                project_->newScenes.erase(relative);
            }
        } catch (const std::exception& e) {
            problem(text(e.what()));
        }
    }
    void editSceneData(std::optional<std::string> recovered = std::nullopt) {
        if (!project_ || !current_ || sceneDraft_)
            return;
        const auto doc = current_;
        const auto path = doc->path().lexically_relative(project_->root);
        SceneDataEditor dialog(
            doc->serialized(),
            [this, path](const ContentValue& data, const std::string& source) {
                auto state = projectState();
                state["scenes"][path.generic_string()] = data;
                state["sources"][path.generic_string()] = source;
                return commitProject(std::move(state), tr("Edit Scene Data"), path);
            },
            this);
        std::vector<std::string> spawns;
        for (const auto& spawn : package_->spawns)
            spawns.push_back(spawn.id);
        dialog.choices(std::move(spawns));
        if (recovered)
            dialog.restoreSource(*recovered);
        sceneDraft_ = &dialog;
        sceneDraftPath_ = path;
        dialog.exec();
        sceneDraft_ = nullptr;
        sceneDraftPath_.clear();
    }
    void editObjectProperties() {
        if (!project_ || !current_ || selected_.empty())
            return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Object Properties"));
        constexpr int propertyDialogWidth = 740, propertyDialogHeight = 700;
        dialog.resize(propertyDialogWidth, propertyDialogHeight);
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new PropertyForm;
        ContentValue values{
            {"room", ""}, {"acoustic", false}, {"reach", sceneLimits::defaultReachMeters}};
        values.overlay(options_.nodePropertyDefaults);
        const auto effective = project_->effective(current_->node(selected_));
        for (const auto& [key, value] : effective.items())
            if (values.contains(key))
                values[key] = value;
        if (values.empty()) {
            problem(tr("The host has not registered object component properties"));
            return;
        }
        PropertyForm::Choices choices;
        for (const auto& [key, names] : options_.nodePropertyChoices)
            for (const auto& name : names)
                choices[key].push_back(text(name));
        choices["room"].push_back("");
        for (const auto& room : package_->rooms)
            if (room.scene == current_->data().at("scene").at("id").get<std::string>())
                choices["room"].push_back(text(room.id));
        form->setValue(values, std::move(choices));
        auto* scroll = new QScrollArea;
        scroll->setObjectName("objectPropertyScroll");
        scroll->setWidgetResizable(true);
        scroll->setWidget(form);
        layout->addWidget(scroll);
        auto* status = new QLabel;
        status->setObjectName("objectPropertyStatus");
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* apply = new QPushButton(tr("Apply"));
        apply->setObjectName("applyObjectProperties");
        layout->addWidget(apply);
        connect(apply, &QPushButton::clicked, &dialog, [this, &dialog, form, values, status] {
            try {
                const auto edited = form->value();
                auto candidate = *current_;
                auto nodes = candidate.nodes();
                for (auto& node : nodes)
                    if (node.at("id") == selected_)
                        for (const auto& [key, value] : edited.items())
                            if (value != values.at(key))
                                node[key] = value;
                // Empty optional references mean absence, including inherited values.
                candidate.replaceNodes(nodes);
                if (edited.contains("kind") && edited.at("kind").is_string() &&
                    !edited.at("kind").get<std::string>().empty() &&
                    !project_->effective(candidate.node(selected_)).contains("bounds"))
                    setEffectiveProperty(candidate, "bounds", defaultBounds());
                for (const auto& [key, value] : edited.items())
                    if (value != values.at(key) && value.is_string() &&
                        value.get<std::string>().empty())
                        clearEffective(candidate, key);
                auto state = projectState();
                const auto path = current_->path().lexically_relative(project_->root);
                state["scenes"][path.generic_string()] = candidate.data();
                if (commitProject(std::move(state), tr("Edit Object Properties"), path))
                    dialog.accept();
                else
                    status->setText(tr("Invalid component values; see Problems."));
            } catch (const std::exception& e) {
                status->setText(text(e.what()));
            }
        });
        dialog.exec();
    }
    static constexpr size_t yawField = 3, scaleField = 4, quaternionField = 5, axisScaleField = 9,
                            propertyCount = 12;
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
        const auto path = QFileDialog::getOpenFileName(this, tr("Open Project"), {},
                                                       tr("Paper Project (*.paperproject)"));
        if (!path.isEmpty())
            openProject(filePath(path));
    }
    void openProject(const fs::path& path) {
        try {
            const bool same = project_ && fs::canonical(path) == project_->file;
            if (same)
                project_->storage->finishSave();
            auto candidate = std::make_unique<Project>(path, !same);
            // Validate before publishing project state; no Lua or game session is constructed.
            auto preview =
                compile(candidate->root, candidate->manifest, candidate->snapshot(), options_);
            if (!preview.package)
                throw std::runtime_error(preview.error);
            candidate->verifySources();
            if (!closeAssetEditors())
                return;
            if (same)
                candidate->lock = std::move(project_->lock);
            ++revision_;
            undo_.clear();
            project_ = std::move(candidate);
            recoveryDeferred_ = false;
            viewport_->audio(std::nullopt);
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
            restoreRecovery();
        } catch (const std::exception& error) {
            problem(text(error.what()));
            QMessageBox::warning(this, tr("Cannot Open Project"), text(error.what()));
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
        populateSpawns();
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
                label = effective.value("kind", "Object");
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
        selection_->setText(available ? text(selected_) : tr("Select an object"));
        viewport_->selection(selected_);
        name_->setEnabled(available);
        parent_->setEnabled(available);
        resource_->setEnabled(available);
        shadow_->setEnabled(available);
        collision_->setEnabled(available);
        const bool structured =
            available && current_->data().at("version") == content::limits::sceneVersion;
        for (auto* field : euler_)
            field->setEnabled(structured);
        if (rotationAxis_) {
            rotationAxis_->setEnabled(structured);
            if (!structured)
                rotationAxis_->setCurrentIndex(1);
        }
        collisionShape_->setEnabled(structured);
        collisionCategory_->setEnabled(structured);
        collisionMask_->setEnabled(structured);
        boundsButton_->setEnabled(available);
        duplicate_->setEnabled(available);
        remove_->setEnabled(available);
        name_->clear();
        parent_->clear();
        resource_->clear();
        if (available) {
            const auto effective = project_->effective(current_->node(selected_));
            name_->setText(text(effective.value("label", "")));
            parent_->addItem(tr("Scene root"), "");
            const auto children = descendants(selected_);
            for (const auto& node : current_->nodes()) {
                const auto id = node.at("id").get<std::string>();
                if (!children.contains(id))
                    parent_->addItem(text(id), text(id));
            }
            parent_->setCurrentIndex(parent_->findData(text(effective.value("parent", ""))));
            resource_->addItem(tr("No geometry"), "");
            for (const auto& [id, asset] : package_->assets)
                resource_->addItem(text(id), text(id));
            resource_->setCurrentIndex(resource_->findData(text(effective.value("resource", ""))));
            shadow_->setChecked(effective.value("shadow", true));
            const auto c = effective.value("collision", ContentValue(false));
            const bool object = c.is_object();
            collision_->setChecked(object || (c.is_boolean() && c.get<bool>()));
            collisionShape_->setCurrentIndex(
                object && c.value("shape", std::string("bounds")) == "mesh" ? 1 : 0);
            collisionCategory_->setText(
                QString::number(object ? c.value("category", uint32_t{1}) : 1));
            collisionMask_->setText(
                QString::number(object ? c.value("mask", ~uint32_t{0}) : ~uint32_t{0}));
        }
        if (available) {
            const auto node = project_->effective(current_->node(selected_));
            const auto position = node.value("position", ContentValue::array({0, 0, 0}));
            for (size_t i = 0; i < yawField; ++i)
                fields_[i]->setValue(position.at(i).get<double>());
            fields_[yawField]->setValue(node.value("yaw", 0.0) * units::degreesPerHalfTurn / pi3);
            const auto t = readTransform(node);
            fields_[scaleField]->setValue(t.scale);
            const auto q = t.rotation.unit();
            const auto x = q.apply({1, 0, 0}), y = q.apply({0, 1, 0}), z = q.apply({0, 0, 1});
            constexpr float gimbalTolerance = .00001f;
            const auto projected = std::hypot(z.x, z.z);
            const bool locked = projected < gimbalTolerance;
            constexpr float quarterTurnRadians = pi3 / 2;
            const auto pitch =
                locked ? std::copysign(quarterTurnRadians, -z.y) : std::atan2(-z.y, projected);
            const std::array<float, 3> angles{pitch,
                                              locked ? std::atan2(-x.z, x.x) : std::atan2(z.x, z.z),
                                              locked ? 0.f : std::atan2(x.y, y.y)};
            for (size_t i = 0; i < angles.size(); ++i) {
                euler_[i]->setValue(angles[i] * units::degreesPerHalfTurn / pi3);
                displayedEuler_[i] = euler_[i]->value();
            }
            const std::array<double, 4> components{q.x, q.y, q.z, q.w};
            const std::array<double, 3> scales{t.scaleAxes.x * t.scale, t.scaleAxes.y * t.scale,
                                               t.scaleAxes.z * t.scale};
            const bool free = current_->data().at("version") == content::limits::sceneVersion;
            fields_[scaleField]->setEnabled(!free);
            fields_[yawField]->setEnabled(!node.contains("rotation") && !node.contains("basis"));
            for (size_t i = 0; i < components.size(); ++i) {
                fields_[quaternionField + i]->setValue(components[i]);
                fields_[quaternionField + i]->setEnabled(free);
            }
            for (size_t i = 0; i < scales.size(); ++i) {
                fields_[axisScaleField + i]->setValue(scales[i]);
                fields_[axisScaleField + i]->setEnabled(free);
            }

            for (size_t i = 0; i < fields_.size(); ++i)
                displayed_[i] = fields_[i]->value();
        }
        updating_ = false;
    }
    void change(size_t component) {
        if (updating_ || !current_ || selected_.empty() ||
            fields_[component]->value() == displayed_[component])
            return;
        try {
            auto candidate = *current_;
            const auto effective = project_->effective(current_->node(selected_));
            auto t = readTransform(effective);
            if (component < yawField) {
                auto value = effective.value("position", ContentValue::array({0, 0, 0}));
                value[component] = fields_[component]->value();
                candidate.setProperty(selected_, "position", value);
            } else if (component == yawField)
                candidate.setProperty(selected_, "yaw",
                                      fields_[component]->value() * pi3 /
                                          units::degreesPerHalfTurn);
            else if (component == scaleField &&
                     current_->data().at("version") == content::limits::legacySceneVersion)
                candidate.setProperty(selected_, "scale", fields_[component]->value());
            else {
                if (component >= quaternionField && component < axisScaleField) {
                    auto q = ContentValue::array(
                        {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w});
                    q[component - quaternionField] = fields_[component]->value();
                    t.rotation = readRotation(q);
                    clearEffective(candidate, "yaw");
                    candidate.setProperty(selected_, "rotation",
                                          ContentValue::array({t.rotation.x, t.rotation.y,
                                                               t.rotation.z, t.rotation.w}));
                } else {
                    auto value =
                        ContentValue::array({t.scaleAxes.x * t.scale, t.scaleAxes.y * t.scale,
                                             t.scaleAxes.z * t.scale});
                    if (component == scaleField)
                        for (auto& v : value)
                            v = v.get<double>() * fields_[component]->value() / t.scale;
                    else
                        value[component - axisScaleField] = fields_[component]->value();
                    candidate.setProperty(selected_, "scale", value);
                }
            }
            commitNodes(candidate.nodes(), tr("Change Transform"), selected_);
        } catch (const std::exception& e) {
            problem(text(e.what()));
            inspect();
        }
    }
    void changeEuler() {
        if (updating_ || !current_ || selected_.empty() ||
            current_->data().at("version") != content::limits::sceneVersion)
            return;
        bool changed = false;
        for (size_t i = 0; i < euler_.size(); ++i)
            changed |= euler_[i]->value() != displayedEuler_[i];
        if (!changed)
            return;
        try {
            const auto angle = [&](size_t index) {
                return static_cast<float>(euler_[index]->value() * pi3 / units::degreesPerHalfTurn);
            };
            const auto q = (Rotation3::axisAngle({0, 1, 0}, angle(1)) *
                            Rotation3::axisAngle({1, 0, 0}, angle(0)) *
                            Rotation3::axisAngle({0, 0, 1}, angle(2)))
                               .unit(); // numbers: Euler component order is pitch, yaw, roll.
            auto candidate = *current_;
            clearEffective(candidate, "yaw");
            candidate.setProperty(selected_, "rotation", ContentValue::array({q.x, q.y, q.z, q.w}));
            commitNodes(candidate.nodes(), tr("Change Rotation"), selected_);
        } catch (const std::exception& e) {
            problem(text(e.what()));
            inspect();
        }
    }
    void clearEffective(authoring::SceneDocument& candidate, std::string_view name) {
        auto nodes = candidate.nodes();
        for (auto& node : nodes)
            if (node.at("id") == selected_) {
                node.erase(name);
                const auto effective = project_->effective(node);
                if (effective.contains(name)) {
                    auto removed = node.value("remove", ContentValue::array());
                    if (std::ranges::none_of(removed, [&](const auto& key) { return key == name; }))
                        removed.elements().push_back(std::string(name));
                    node["remove"] = removed;
                }
            }
        candidate.replaceNodes(nodes);
    }
    void populateAssets() {
        if (!package_ || !project_)
            return;
        std::vector<ResourceEntry> resources;
        ResourceStore files(project_->root);
        const auto world = content::parse(project_->sources.at(files.resolve(project_->manifest)),
                                          project_->manifest.string());
        for (const auto& relative : world.at("world").at("resources")) {
            const fs::path path = relative.get<std::string>();
            const auto source =
                content::parse(project_->sources.at(files.resolve(path)), path.string());
            for (const auto& value : source.at("resources").at("resources")) {
                const auto id = value.at("id").get<std::string>();
                const auto& asset = package_->assets.at(id);
                resources.push_back({text(id),
                                     asset->model       ? tr("Models")
                                     : asset->billboard ? tr("Sprites")
                                                        : tr("Meshes"),
                                     pathText(path), text(id)});
            }
        }
        assets_->project(project_->root, std::move(resources));
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
    void editCollision() {
        if (updating_ || !current_ || selected_.empty())
            return;
        if (!collision_->isChecked())
            return;
        const bool legacy = current_->data().at("version") == content::limits::legacySceneVersion;
        bool categoryOk = false, maskOk = false;
        const auto category = collisionCategory_->text().toULongLong(&categoryOk),
                   mask = collisionMask_->text().toULongLong(&maskOk);
        if (!categoryOk || !maskOk || category == 0 ||
            category > std::numeric_limits<uint32_t>::max() ||
            mask > std::numeric_limits<uint32_t>::max()) {
            problem(tr("Collision layers require decimal 32-bit bitsets and a nonzero category"));
            inspect();
            return;
        }
        try {
            auto candidate = *current_;
            setEffectiveProperty(
                candidate, "collision",
                legacy ? ContentValue(true)
                       : ContentValue{
                             {"shape", collisionShape_->currentIndex() == 0 ? "bounds" : "mesh"},
                             {"category", static_cast<uint32_t>(category)},
                             {"mask", static_cast<uint32_t>(mask)}});
            if ((legacy || collisionShape_->currentIndex() == 0) &&
                !project_->effective(current_->node(selected_)).contains("bounds"))
                setEffectiveProperty(candidate, "bounds", defaultBounds());
            commitNodes(candidate.nodes(), tr("Change Collider"), selected_);
        } catch (const std::exception& e) {
            problem(text(e.what()));
            inspect();
        }
    }
    void editProperty(std::string_view name, const ContentValue& value) {
        if (updating_ || !current_ || selected_.empty())
            return;
        try {
            auto candidate = *current_;
            setEffectiveProperty(candidate, name, value);
            commitNodes(candidate.nodes(), tr("Change %1").arg(text(name)), selected_);
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
        if (assets_->selectedResource().empty()) {
            statusBar()->showMessage(tr("Select a placeable resource in the Project panel"));
            return;
        }
        createNode(assets_->selectedResource());
    }
    void createNode(const std::string& resource) {
        if (!current_)
            return;
        auto nodes = current_->nodes();
        const auto id = newId();
        const auto position = viewport_->insertionPoint();
        ContentValue node{{"id", id},
                          {"label", resource.empty() ? "New Object" : resource},
                          {"position", ContentValue::array({position.x, position.y, position.z})}};
        if (!resource.empty())
            node["resource"] = resource;
        nodes.elements().push_back(std::move(node));
        commitNodes(std::move(nodes), tr("Create Object"), id);
    }
    void deleteNode() {
        if (!current_ || selected_.empty())
            return;
        const auto ids = descendants(selected_);
        auto nodes = current_->nodes();
        std::erase_if(nodes.elements(), [&](const auto& node) {
            return ids.contains(node.at("id").template get<std::string>());
        });
        commitNodes(std::move(nodes), tr("Delete %1 object(s)").arg(ids.size()), "");
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
                    problem(tr("This object has the game binding %1. For an independent copy, add "
                               "its resource from the Project panel.")
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
        commitNodes(std::move(nodes), tr("Duplicate Object"), remap.at(selected_));
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
                destination ? destination->transform.inversePoint(node->transform.position)
                            : node->transform.position;
            candidate.setProperty(selected_, "position",
                                  ContentValue::array({position.x, position.y, position.z}));
            if (current_->data().at("version") == content::limits::sceneVersion) {
                const auto local = relativeTransform(
                    destination ? destination->transform : MeshTransform{}, node->transform);
                const auto fields = writeTransform(local);
                clearEffective(candidate, "yaw");
                for (const auto& [key, value] : fields.items())
                    candidate.setProperty(selected_, key, value);
            } else {
                candidate.setProperty(selected_, "yaw",
                                      node->yaw - (destination ? destination->yaw : 0));
                candidate.setProperty(selected_, "scale",
                                      node->transform.scale /
                                          (destination ? destination->transform.scale : 1));
            }
            commitNodes(candidate.nodes(), tr("Change Parent"), selected_);
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
                    delta = parent->transform.linear().inverse().apply(delta);
                auto pos = effective.value("position", ContentValue::array({0, 0, 0}));
                pos[0] = pos.at(0).get<float>() + delta.x;
                pos[1] = pos.at(1).get<float>() + delta.y;
                constexpr size_t zComponent = 2;
                pos[zComponent] = pos.at(zComponent).get<float>() + delta.z;
                candidate.setProperty(selected_, "position", pos);
            } else if (current_->data().at("version") == content::limits::sceneVersion) {
                auto t = readTransform(effective);
                if (tool == Viewport::Tool::Rotate) {
                    const auto* parent = compiledNode(effective.value("parent", ""));
                    const auto p = parent ? parent->transform : MeshTransform{};
                    auto world = compose(p, t);
                    world.rotation = Rotation3::axisAngle(delta, amount) * world.rotation;
                    t = relativeTransform(p, world);
                    clearEffective(candidate, "yaw");
                    const auto fields = writeTransform(t);
                    for (const auto& [key, value] : fields.items())
                        candidate.setProperty(selected_, key, value);
                } else
                    candidate.setProperty(selected_, "scale",
                                          ContentValue::array({t.scale * t.scaleAxes.x * amount,
                                                               t.scale * t.scaleAxes.y * amount,
                                                               t.scale * t.scaleAxes.z * amount}));
            } else if (tool == Viewport::Tool::Rotate) {
                if (delta.x != 0 || delta.z != 0)
                    throw std::runtime_error("Free-axis rotation requires a DCMO 3 scene copy");
                candidate.setProperty(selected_, "yaw", effective.value("yaw", 0.0) + amount);
            } else
                candidate.setProperty(selected_, "scale", effective.value("scale", 1.0) * amount);
            commitNodes(candidate.nodes(), tr("Transform Object"), selected_);
        } catch (const std::exception& error) {
            problem(text(error.what()));
            inspect();
        }
    }
    void resetTransform() {
        if (!current_ || selected_.empty())
            return;
        auto candidate = *current_;
        for (const auto key : {"position", "yaw", "rotation", "basis", "scale"})
            candidate.setProperty(selected_, key, {});
        auto nodes = candidate.nodes();
        for (auto& node : nodes)
            if (node.at("id") == selected_ && node.contains("remove")) {
                std::erase_if(node["remove"].elements(), [](const ContentValue& name) {
                    return name == "position" || name == "yaw" || name == "rotation" ||
                           name == "basis" || name == "scale";
                });
                if (node.at("remove").empty())
                    node.erase("remove");
            }
        candidate.replaceNodes(std::move(nodes));
        commitNodes(candidate.nodes(), tr("Reset Transform"), selected_);
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
        statusBar()->showMessage(tr("Validating scene…"));
        worker_.setFuture(QtConcurrent::run(compile, project_->root, project_->manifest,
                                            project_->snapshot(), options_));
    }
    void publish(const PreviewResult& result) {
        if (!result.package) {
            problem(tr("Scene validation failed: ") + text(result.error));
            return;
        }
        viewport_->editingEnabled(true);
        package_ = result.package;
        if (audioEditor_ && audioEditor_->currentSceneNodes)
            audioEditor_->sceneNodes(audioEditor_->currentSceneNodes());
        publishedRevision_ = revision_;
        problems_->clear();
        if (!viewportError_.isEmpty())
            problem(viewportError_);
        if (current_)
            viewport_->scene(package_, current_->data().at("scene").at("id").get<std::string>(),
                             false);
        populateSpawns();
        updateState();
    }
    std::vector<StoredEdit> recoveryEdits() const {
        std::vector<StoredEdit> edits;
        for (const auto& [path, doc] : project_->scenes)
            edits.push_back({doc->path(), project_->baseline(path),
                             sceneDraft_ && sceneDraftPath_ == path ? sceneDraft_->recoverySource()
                                                                    : doc->serialized()});
        if (uiEditor_ && uiEditor_->isVisible())
            edits.push_back(
                {uiEditor_->file(), uiEditor_->original(), uiEditor_->recoverySource()});
        if (audioEditor_ && audioEditor_->isVisible())
            edits.push_back(
                {audioEditor_->file(), audioEditor_->original(), audioEditor_->recoverySource()});
        for (const auto& [path, editor] : textEditors_)
            if (editor->isVisible())
                edits.push_back({path, editor->original(), editor->recoverySource()});
        const bool changed = project_->dirty() || std::ranges::any_of(edits, [](const auto& edit) {
                                 return edit.before != edit.after;
                             });
        if (!changed)
            return {};
        for (const auto& [path, source] : project_->sources)
            edits.push_back(
                {path, source, path == project_->worldPath() ? project_->worldSource() : source});
        return edits;
    }
    void autosaveProject() {
        if (!project_ || recoveryDeferred_)
            return;
        try {
            if (project_->storage->pendingSave())
                return;
            project_->storage->autosave(recoveryEdits());
            autosaveError_.clear();
        } catch (const std::exception& e) {
            const auto error = text(e.what());
            if (autosaveError_ != error) {
                problem(tr("Recovery snapshot failed: ") + error);
                autosaveError_ = error;
            }
        }
    }
    void restoreRecovery() {
        if (!project_)
            return;
        try {
            const auto edits = project_->storage->recovery();
            if (edits.empty())
                return;
            const auto answer = QMessageBox::question(
                this, tr("Recover Unsaved Work"),
                tr("An unsaved recovery snapshot exists. Restore it as editable drafts? "
                   "Discard removes the snapshot; Cancel keeps it for later."),
                QMessageBox::Yes | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Yes);
            if (answer == QMessageBox::Discard) {
                project_->storage->discardRecovery();
                return;
            }
            if (answer != QMessageBox::Yes) {
                recoveryDeferred_ = true;
                statusBar()->showMessage(
                    tr("Recovery deferred; automatic snapshots paused until Restore or Discard."));
                return;
            }
            auto state = projectState();
            std::map<fs::path, std::string> sceneSources;
            bool malformed = false;
            for (const auto& edit : edits) {
                if (edit.file == project_->worldPath()) {
                    state["world"] = content::parse(edit.after, edit.file.string());
                    continue;
                }
                const auto relative = edit.file.lexically_relative(project_->root);
                if (edit.file.extension() != ".dcscene" || relative.empty() ||
                    *relative.begin() == ".." || edit.before == edit.after)
                    continue;
                sceneSources[relative] = edit.after;
                if (!project_->knownScenes.contains(relative)) {
                    // Rebuild a valid editable baseline for an unsaved new scene. The raw
                    // recovery text is retained even if its current form/syntax is invalid.
                    const auto id = edit.file.stem().string();
                    const ContentValue shell{
                        {"format", "dcmo.scene"},
                        {"version", state.at("world").at("version")},
                        {"scene", ContentValue{{"id", id},
                                               {"nodes", ContentValue::array()},
                                               {"rooms", ContentValue::array()},
                                               {"spawns", ContentValue::array()}}}};
                    project_->knownScenes[relative] = std::make_shared<authoring::SceneDocument>(
                        edit.file, content::encode(shell));
                    project_->newScenes.insert(relative);
                }
                try {
                    state["scenes"][relative.generic_string()] =
                        content::parse(edit.after, edit.file.string());
                    state["sources"][relative.generic_string()] = edit.after;
                } catch (const std::exception&) {
                    malformed = true;
                    state["scenes"][relative.generic_string()] =
                        project_->knownScenes.at(relative)->data();
                }
            }
            std::set<std::string> declared;
            for (const auto& path : state.at("world").at("world").at("scenes"))
                declared.insert(path.get<std::string>());
            std::erase_if(state["scenes"].items(),
                          [&](const auto& entry) { return !declared.contains(entry.first); });
            SceneDocuments documents;
            documents[project_->manifest] = state.at("world");
            for (const auto& [path, data] : state.at("scenes").items())
                documents[fs::path(path)] = data;
            const auto checked = compile(project_->root, project_->manifest, documents, options_);
            if (malformed || !checked.package) {
                recoveryDeferred_ = true;
                for (auto& [path, source] : sceneSources) {
                    if (!declared.contains(path.generic_string()))
                        continue;
                    bool applied = false;
                    const auto doc = project_->knownScenes.at(path);
                    SceneDataEditor dialog(
                        doc->serialized(),
                        [&state, path, &applied](const ContentValue& data,
                                                 const std::string& stagedSource) {
                            state["scenes"][path.generic_string()] = data;
                            state["sources"][path.generic_string()] = stagedSource;
                            applied = true;
                            return true;
                        },
                        this);
                    dialog.setWindowTitle(tr("Repair Recovery Draft — ") + pathText(path));
                    dialog.stagedRecovery();
                    dialog.restoreSource(source);
                    dialog.exec();
                    if (!applied) {
                        state["scenes"][path.generic_string()] = doc->data();
                        state["sources"][path.generic_string()] = doc->serialized();
                    }
                    source = applied ? dialog.recoverySource() : doc->serialized();
                }
            }
            if (!commitProject(state, tr("Recover Project"))) {
                auto retained = edits;
                for (auto& edit : retained) {
                    const auto relative = edit.file.lexically_relative(project_->root);
                    if (sceneSources.contains(relative))
                        edit.after = sceneSources.at(relative);
                }
                project_->storage->autosave(retained);
                recoveryDeferred_ = true;
                problem(tr("The repaired recovery candidate is still invalid. Drafts were "
                           "retained; use Restore again to repair them."));
                return;
            }
            for (const auto& edit : edits) {
                if (edit.before == edit.after || edit.file == project_->worldPath() ||
                    edit.file.extension() == ".dcscene")
                    continue;
                editAsset(edit.file);
                if (uiEditor_ && uiEditor_->file() == edit.file)
                    uiEditor_->restoreSource(edit.after);
                else if (audioEditor_ && audioEditor_->file() == edit.file)
                    audioEditor_->restoreSource(edit.after);
                else if (textEditors_.contains(edit.file))
                    textEditors_.at(edit.file)->restoreSource(edit.after);
            }
            recoveryDeferred_ = false;
            autosaveProject();
        } catch (const std::exception& e) {
            recoveryDeferred_ = true;
            problem(tr("Recovery retained: ") + text(e.what()));
        }
    }
    bool save(const std::shared_ptr<authoring::SceneDocument>& document) {
        return saveBatch(project_->worldSource() != project_->sources.at(project_->worldPath())
                             ? nullptr
                             : document);
    }
    bool saveBatch(const std::shared_ptr<authoring::SceneDocument>& only) {
        try {
            project_->verifySources();
            auto savedCandidate = project_->snapshot();
            if (only) {
                savedCandidate[project_->manifest] = content::parse(
                    project_->sources.at(project_->worldPath()), project_->manifest.string());
                for (const auto& [path, doc] : project_->scenes)
                    if (doc != only)
                        savedCandidate[path] = content::parse(doc->original(), path.string());
            }
            const auto checked =
                compile(project_->root, project_->manifest, savedCandidate, options_);
            if (!checked.package)
                throw std::runtime_error(checked.error);
            std::vector<StoredEdit> edits;
            for (const auto& [path, doc] : project_->scenes)
                edits.push_back({doc->path(), project_->baseline(path),
                                 !only || doc == only ? doc->serialized() : doc->original()});
            if (!only) {
                if (uiEditor_ && uiEditor_->isVisible()) {
                    auto output = uiEditor_->snapshot();
                    if (uiEditor_->recoverySource() == uiEditor_->original())
                        output = uiEditor_->original();
                    edits.push_back({uiEditor_->file(), uiEditor_->original(), std::move(output)});
                }
                if (audioEditor_ && audioEditor_->isVisible()) {
                    auto output = audioEditor_->snapshot();
                    if (audioEditor_->recoverySource() == audioEditor_->original())
                        output = audioEditor_->original();
                    edits.push_back(
                        {audioEditor_->file(), audioEditor_->original(), std::move(output)});
                }
            }
            if (!only)
                for (const auto& [path, editor] : textEditors_)
                    if (editor->isVisible())
                        edits.push_back({path, editor->original(), editor->snapshot()});
            for (const auto& [path, source] : project_->sources)
                edits.push_back(
                    {path, source,
                     path == project_->worldPath() && !only ? project_->worldSource() : source});
            if (options_.validateContent) {
                std::map<fs::path, std::string> overrides;
                for (const auto& edit : edits) {
                    const auto relative = edit.file.lexically_relative(project_->root);
                    if (!relative.empty() && *relative.begin() != "..")
                        overrides[relative] = edit.after;
                }
                options_.validateContent(*checked.package, overrides);
            }
            const bool changed =
                std::ranges::any_of(edits, [](const auto& e) { return e.before != e.after; });
            if (changed)
                project_->storage->save(edits);
            for (const auto& edit : edits) {
                for (const auto& [path, doc] : project_->scenes)
                    if (doc->path() == edit.file && (!only || doc == only)) {
                        doc->acceptSaved(edit.after);
                        project_->newScenes.erase(path);
                    }
                if (project_->sources.contains(edit.file))
                    project_->sources[edit.file] = edit.after;
                if (uiEditor_ && uiEditor_->file() == edit.file)
                    uiEditor_->acceptSaved(edit.after);
                if (audioEditor_ && audioEditor_->file() == edit.file)
                    audioEditor_->acceptSaved(edit.after);
                if (textEditors_.contains(edit.file))
                    textEditors_.at(edit.file)->acceptSaved(edit.after);
            }
            publish(checked);
            updateState();
            autosaveProject();
            if (only)
                requestPreview();
            return true;
        } catch (const std::exception& error) {
            problem(text(error.what()));
            QMessageBox::warning(this, tr("Cannot Save Project"), text(error.what()));
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
        if (!project_->dirty())
            return true;
        const auto answer = QMessageBox::question(
            this, tr("Unsaved Changes"), tr("Save changed scenes before closing?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return false;
        if (answer == QMessageBox::Save)
            return saveBatch(nullptr);
        return true;
    }
    void populateSpawns() {
        const auto previous = spawn_->currentData();
        QSignalBlocker blocker(spawn_);
        spawn_->clear();
        if (package_ && current_) {
            const auto scene = current_->data().at("scene").at("id").get<std::string>();
            for (const auto& spawn : package_->spawns)
                if (spawn.scene == scene)
                    spawn_->addItem(text(spawn.id), text(spawn.id));
            auto index = spawn_->findData(previous);
            if (index < 0)
                index = spawn_->findData(text(package_->entrySpawn));
            if (index >= 0)
                spawn_->setCurrentIndex(index);
        }
        updatePlay();
    }
    void updatePlay() {
        if (!playAction_)
            return;
        const bool active = play_->active();
        playAction_->setEnabled(project_ && !active && !options_.playExecutable.empty() &&
                                spawn_->count());
        stopAction_->setEnabled(active && play_->state() != PlayController::State::Stopping);
        spawn_->setEnabled(!active);
        open_->setEnabled(!active);
        switch (play_->state()) {
        case PlayController::State::Idle:
            playStatus_->setText(options_.playExecutable.empty() ? tr("No host runtime configured")
                                 : spawn_->count() ? tr("Edit Mode")
                                                   : tr("No start point in this scene"));
            break;
        case PlayController::State::Preparing:
            playStatus_->setText(tr("Preparing Play…"));
            break;
        case PlayController::State::Starting:
            playStatus_->setText(tr("Starting Play…"));
            break;
        case PlayController::State::Running:
            playStatus_->setText(tr("PLAY • Changes apply on next run"));
            break;
        case PlayController::State::Stopping:
            playStatus_->setText(tr("Stopping Play…"));
            break;
        }
    }
    void startPlay() {
        if (!project_ || play_->active() || spawn_->currentIndex() < 0 ||
            options_.playExecutable.empty())
            return;
        auto* panel = findChild<QDockWidget*>("console");
        panel->show();
        panel->raise();
        try {
            project_->verifySources();
            PlayInput input;
            input.root = project_->root;
            input.manifest = project_->manifest;
            for (const auto& [path, source] : project_->sources)
                if (path != project_->file)
                    input.expected.emplace(path.lexically_relative(project_->root), source);
            for (const auto& [path, document] : project_->scenes) {
                if (!project_->newScenes.contains(path))
                    input.expected[path] = document->original();
                else
                    input.absent.insert(path);
                input.overrides[path] = document->serialized();
            }
            if (uiEditor_ && uiEditor_->isVisible()) {
                const auto path = uiEditor_->file().lexically_relative(project_->root);
                input.expected[path] = uiEditor_->original();
                input.overrides[path] = uiEditor_->snapshot();
            }
            if (audioEditor_ && audioEditor_->isVisible()) {
                audioEditor_->stopAudition();
                std::vector<std::string> nodes;
                for (const auto& [path, document] : project_->scenes)
                    for (const auto& node : document->nodes())
                        nodes.push_back(node.at("id").get<std::string>());
                audioEditor_->sceneNodes(std::move(nodes));
                const auto path = audioEditor_->file().lexically_relative(project_->root);
                input.expected[path] = audioEditor_->original();
                input.overrides[path] = audioEditor_->snapshot();
            }
            ResourceStore files(project_->root);
            auto world = project_->world;
            world["world"]["entrySpawn"] = spawn_->currentData().toString().toStdString();
            input.overrides[project_->manifest] = content::encode(world);
            for (const auto& [path, editor] : textEditors_)
                if (editor->isVisible()) {
                    const auto relative = path.lexically_relative(project_->root);
                    input.expected[relative] = editor->original();
                    input.overrides[relative] = editor->snapshot();
                }
            if (options_.validateContent) {
                const auto checked =
                    compile(project_->root, project_->manifest, project_->snapshot(), options_);
                if (!checked.package)
                    throw std::runtime_error(checked.error);
                options_.validateContent(*checked.package, input.overrides);
            }
            play_->start(std::move(input), options_.playExecutable, options_.playArguments);
        } catch (const std::exception& error) {
            console_->appendPlainText(tr("Cannot Play: ") + text(error.what()));
        }
    }
    void updateState() {
        updatePlay();
        const bool dirty = project_ && project_->dirty();
        setWindowTitle(text(options_.title) + (project_ ? " — " + text(project_->name) : "") +
                       (dirty ? " *" : ""));
        save_->setEnabled(current_ && project_ && (current_->dirty() || project_->dirty()));
        if (project_)
            statusBar()->showMessage(dirty ? tr("Unsaved changes")
                                           : tr("Scene saved • RMB: Look • Wheel: "
                                                "Move • F: Frame Selected"));
        else
            statusBar()->showMessage(tr("Open a .paperproject file"));
    }
    PlayController* play_ = nullptr;
    QPlainTextEdit* console_ = nullptr;
    QComboBox* spawn_ = nullptr;
    QLabel* playStatus_ = nullptr;
    QAction *playAction_ = nullptr, *stopAction_ = nullptr, *open_ = nullptr;
    bool closeAfterStop_ = false;
    QByteArray defaultLayout_;
    QList<QAction*> toolActions_;
    QLineEdit* name_ = nullptr;
    QLineEdit* hierarchySearch_ = nullptr;
    QComboBox* parent_ = nullptr;
    QComboBox* resource_ = nullptr;
    QCheckBox* shadow_ = nullptr;
    QCheckBox* collision_ = nullptr;
    QComboBox* collisionShape_ = nullptr;
    QPushButton* boundsButton_ = nullptr;
    QComboBox* rotationAxis_ = nullptr;
    QLineEdit *collisionCategory_ = nullptr, *collisionMask_ = nullptr;
    ResourceBrowser* assets_ = nullptr;
    QAction* duplicate_ = nullptr;
    QAction* remove_ = nullptr;
    QString viewportError_;
    Options options_;
    std::unique_ptr<Project> project_;
    std::unique_ptr<UiEditor> uiEditor_;
    std::unique_ptr<AudioBankEditor> audioEditor_;
    std::map<fs::path, std::unique_ptr<TextAssetEditor>> textEditors_;
    std::shared_ptr<authoring::SceneDocument> current_;
    SceneDataEditor* sceneDraft_ = nullptr;
    fs::path sceneDraftPath_;
    std::shared_ptr<ScenePackage> package_;
    Viewport* viewport_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QComboBox* sceneList_ = nullptr;
    QLabel* selection_ = nullptr;
    QListWidget* problems_ = nullptr;
    QAction* save_ = nullptr;
    std::array<QDoubleSpinBox*, propertyCount> fields_{};
    std::array<double, propertyCount> displayed_{};
    std::array<QDoubleSpinBox*, 3> euler_{};
    std::array<double, 3> displayedEuler_{};
    std::map<std::string, QTreeWidgetItem*, std::less<>> items_;
    std::string selected_;
    QUndoStack undo_;
    QFutureWatcher<PreviewResult> worker_;
    // Maximum ordinary crash-loss window; snapshots never overwrite authored files.
    static constexpr int autosaveMilliseconds = 30000;
    QTimer autosaveTimer_;
    QString autosaveError_;
    bool recoveryDeferred_ = false;
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
