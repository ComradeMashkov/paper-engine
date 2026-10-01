#include "ui_editor.hpp"
#include "paper/core/units.hpp"
#include "paper/resources/resource_store.hpp"
#include "property_form.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFile>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <set>
namespace paper::editor {
namespace {
constexpr int editorWidth = 1300, editorHeight = 800, tickMilliseconds = 40;
constexpr int hierarchyWidth = 220, propertiesWidth = 480, previewWidth = 600;
constexpr float wheelPixels = 40;
QString text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
QString pathText(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}
QByteArray read(const std::filesystem::path& path) {
    QFile file(pathText(path));
    if (!file.open(QIODevice::ReadOnly) ||
        file.size() > static_cast<qint64>(ui::documentLimits::fileBytes))
        throw std::runtime_error("Cannot read UI document or file budget exceeded");
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error("Cannot read UI document");
    return bytes;
}
QFont previewFont(float pixels) {
    QFont result = QApplication::font();
    result.setPixelSize(std::max(1, static_cast<int>(std::ceil(pixels))));
    return result;
}
QRectF rectangle(Rect r) {
    return {r.x, r.y, r.w, r.h};
}
QColor color(Color c) {
    return {c.r, c.g, c.b, c.a};
}
std::string id() {
    return "ui." + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}
void select(QTreeWidget* tree, const std::string& id) {
    QSignalBlocker blocked(tree);
    for (QTreeWidgetItemIterator item(tree); *item; ++item)
        if ((*item)->data(0, Qt::UserRole).toString().toStdString() == id) {
            tree->setCurrentItem(*item);
            return;
        }
}
} // namespace
UiPreview::UiPreview(QWidget* parent) : QWidget(parent) {
    setObjectName("uiPreview");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    timer_.setInterval(tickMilliseconds);
    elapsed_.start();
    connect(&timer_, &QTimer::timeout, this, [this] {
        const auto seconds = static_cast<float>(elapsed_.restart() / units::millisecondsPerSecond);
        if (context_) {
            context_->advance(seconds);
            update();
        }
    });
    timer_.start();
}
void UiPreview::document(const ui::Document& document) {
    auto candidate = std::make_unique<ui::Context>(
        [](std::string_view value, float pixels) {
            return static_cast<float>(
                QFontMetricsF(previewFont(pixels)).horizontalAdvance(text(value)));
        },
        document.theme);
    candidate->setTree(document.root);
    candidate->layout({0, 0, document.viewport.x, document.viewport.y});
    std::vector<std::string> order;
    const auto visit = [&](const auto& self, const ui::Node& node) -> void {
        order.push_back(node.id);
        for (const auto& child : node.children)
            self(self, child);
    };
    visit(visit, document.root);
    context_ = std::move(candidate);
    order_ = std::move(order);
    setFixedSize(static_cast<int>(std::ceil(document.viewport.x)),
                 static_cast<int>(std::ceil(document.viewport.y)));
    update();
}
void UiPreview::selection(std::string id) {
    selection_ = std::move(id);
    update();
}
void UiPreview::interactive(bool value) {
    input({ui::InputType::Cancel, {}});
    interactive_ = value;
}
void UiPreview::input(ui::Input event) {
    if (!context_)
        return;
    for (const auto& result : context_->input(std::span(&event, 1)))
        if (action)
            action(result);
    update();
}
void UiPreview::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());
    if (!context_)
        return;
    for (const auto& draw : context_->draw()) {
        painter.save();
        painter.setClipRect(rectangle(draw.clip));
        painter.setPen(color(draw.color));
        if (draw.type == ui::Draw::Type::Fill)
            painter.fillRect(rectangle(draw.bounds), color(draw.color));
        else if (draw.type == ui::Draw::Type::Outline)
            painter.drawRect(rectangle(draw.bounds));
        else {
            const auto f = previewFont(draw.fontPixels);
            painter.setFont(f);
            painter.drawText(QPointF(draw.bounds.x, draw.bounds.y + QFontMetricsF(f).ascent()),
                             text(draw.text));
        }
        painter.restore();
    }
    if (!interactive_ && !selection_.empty()) {
        try {
            const auto box = context_->box(selection_);
            painter.setClipRect(rectangle(box.clip));
            painter.setPen(QPen(QColor(255, 194, 92),
                                2)); // numbers: editor selection color and outline width.
            painter.drawRect(rectangle(box.bounds));
        } catch (const std::out_of_range&) {
        }
    }
}
void UiPreview::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !context_)
        return;
    setFocus();
    const Vec2 p{static_cast<float>(event->position().x()),
                 static_cast<float>(event->position().y())};
    if (interactive_)
        input({ui::InputType::Down, p});
    else
        for (auto i = order_.rbegin(); i != order_.rend(); ++i) {
            try {
                const auto box = context_->box(*i);
                if (box.bounds.contains(p.x, p.y) && box.clip.contains(p.x, p.y)) {
                    if (selected)
                        selected(*i);
                    break;
                }
            } catch (const std::out_of_range&) {
            }
        }
}
void UiPreview::mouseReleaseEvent(QMouseEvent* event) {
    if (interactive_ && event->button() == Qt::LeftButton)
        input({ui::InputType::Up,
               {static_cast<float>(event->position().x()),
                static_cast<float>(event->position().y())}});
}
void UiPreview::mouseMoveEvent(QMouseEvent* event) {
    if (interactive_)
        input({ui::InputType::Move,
               {static_cast<float>(event->position().x()),
                static_cast<float>(event->position().y())}});
}
void UiPreview::wheelEvent(QWheelEvent* event) {
    if (interactive_) {
        constexpr float angleUnitsPerStep = 120;
        const auto pixels = event->pixelDelta().isNull()
                                ? event->angleDelta().y() / angleUnitsPerStep * wheelPixels
                                : float(event->pixelDelta().y());
        input(
            {ui::InputType::Wheel,
             {static_cast<float>(event->position().x()), static_cast<float>(event->position().y())},
             -pixels});
        event->accept();
    } else
        QWidget::wheelEvent(event);
}
void UiPreview::keyPressEvent(QKeyEvent* event) {
    if (!interactive_) {
        QWidget::keyPressEvent(event);
        return;
    }
    const std::map<int, ui::Key> keys{
        {Qt::Key_Tab, ui::Key::Tab},      {Qt::Key_Backtab, ui::Key::Tab},
        {Qt::Key_Return, ui::Key::Enter}, {Qt::Key_Enter, ui::Key::Enter},
        {Qt::Key_Space, ui::Key::Space},  {Qt::Key_Left, ui::Key::Left},
        {Qt::Key_Right, ui::Key::Right},  {Qt::Key_Up, ui::Key::Up},
        {Qt::Key_Down, ui::Key::Down},    {Qt::Key_Home, ui::Key::Home},
        {Qt::Key_End, ui::Key::End},      {Qt::Key_Escape, ui::Key::Escape}};
    if (keys.contains(event->key())) {
        input({ui::InputType::Key,
               {},
               0,
               keys.at(event->key()),
               event->key() == Qt::Key_Backtab || event->modifiers().testFlag(Qt::ShiftModifier),
               event->isAutoRepeat()});
        event->accept();
    } else
        QWidget::keyPressEvent(event);
}
bool UiPreview::event(QEvent* event) {
    if (interactive_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        }
    }
    return QWidget::event(event);
}
void UiPreview::focusOutEvent(QFocusEvent* event) {
    input({ui::InputType::FocusLost, {}});
    QWidget::focusOutEvent(event);
}
UiEditor::UiEditor(std::filesystem::path file, std::filesystem::path assets, QWidget* parent)
    : QDialog(parent), assets_(std::filesystem::canonical(assets)) {
    ResourceStore files(assets_);
    file_ = files.resolve(std::filesystem::canonical(file).lexically_relative(assets_));
    baseline_ = read(file_);
    if (QString::fromUtf8(baseline_).toUtf8() != baseline_)
        throw std::invalid_argument("UI document must be valid UTF-8");
    setObjectName("uiEditor");
    setWindowTitle(tr("UI — ") + pathText(file_.filename()));
    resize(editorWidth, editorHeight);
    auto* layout = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout;
    palette_ = new QComboBox;
    palette_->setObjectName("uiPalette");
    palette_->addItems({"panel", "label", "button", "toggle", "slider", "list"});
    controls->addWidget(palette_);
    const auto button = [&](const char* name, const QString& title,
                            const std::function<void()>& action) {
        auto* item = new QPushButton(title);
        item->setObjectName(name);
        controls->addWidget(item);
        connect(item, &QPushButton::clicked, this, [this, action] { report(action); });
        return item;
    };
    button("addUiNode", tr("Add"), [this] { add(); });
    button("duplicateUiNode", tr("Duplicate"), [this] { duplicate(); });
    button("deleteUiNode", tr("Delete"), [this] { remove(); });
    button("uiMoveUp", tr("Up"), [this] { reorder(-1); });
    button("uiMoveDown", tr("Down"), [this] { reorder(1); });
    auto* interactive = new QCheckBox(tr("Interact"));
    interactive->setObjectName("uiInteract");
    controls->addWidget(interactive);
    layout->addLayout(controls);
    auto* tabs = new QTabWidget;
    auto* split = new QSplitter;
    tree_ = new QTreeWidget;
    tree_->setObjectName("uiHierarchy");
    tree_->setHeaderLabel(tr("Components / Theme"));
    tree_->setMinimumWidth(hierarchyWidth);
    split->addWidget(tree_);
    auto* properties = new QWidget;
    properties->setMinimumWidth(propertiesWidth);
    auto* propertyLayout = new QVBoxLayout(properties);
    form_ = new PropertyForm;
    form_->setObjectName("uiProperties");
    auto* formScroll = new QScrollArea;
    formScroll->setWidgetResizable(true);
    formScroll->setWidget(form_);
    propertyLayout->addWidget(formScroll);
    auto* apply = new QPushButton(tr("Apply Properties"));
    apply->setObjectName("applyUiProperties");
    propertyLayout->addWidget(apply);
    split->addWidget(properties);
    preview_ = new UiPreview;
    auto* previewScroll = new QScrollArea;
    previewScroll->setWidget(preview_);
    split->addWidget(previewScroll);
    split->setStretchFactor(1, 1); // numbers: inspector is splitter pane 1.
    split->setStretchFactor(2, 1); // numbers: preview is splitter pane 2.
    split->setSizes({hierarchyWidth, propertiesWidth, previewWidth});
    tabs->addTab(split, tr("Design"));
    source_ = new QPlainTextEdit;
    source_->setObjectName("uiSource");
    source_->setPlainText(QString::fromUtf8(baseline_));
    displayedBaseline_ = source_->toPlainText();
    tabs->addTab(source_, tr("Source"));
    layout->addWidget(tabs, 1);
    status_ = new QLabel;
    status_->setObjectName("uiStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* footer = new QHBoxLayout;
    for (const auto& [name, title, action] :
         std::initializer_list<std::tuple<const char*, QString, std::function<void()>>>{
             {"undoUi", tr("Undo"),
              [this] {
                  if (form_->isEnabled() && form_->dirty()) {
                      inspect();
                      status_->setText(tr("Pending property changes discarded"));
                  } else
                      source_->undo();
              }},
             {"redoUi", tr("Redo"), [this] { source_->redo(); }},
             {"saveUi", tr("Save"), [this] { save(); }},
             {"validateUi", tr("Validate"), [this] {
                  if (form_->isEnabled() && form_->dirty())
                      this->apply();
                  validate();
              }}}) {
        auto* item = new QPushButton(title);
        item->setObjectName(name);
        footer->addWidget(item);
        connect(item, &QPushButton::clicked, this, [this, action] { report(action); });
    }
    auto* close = new QPushButton(tr("Close"));
    footer->addWidget(close);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    layout->addLayout(footer);
    connect(source_, &QPlainTextEdit::textChanged, this, [this] { rebuild(); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] {
        const auto next = tree_->currentItem()
                              ? tree_->currentItem()->data(0, Qt::UserRole).toString().toStdString()
                              : "";
        if (form_->isEnabled() && form_->dirty()) {
            try {
                this->apply();
            } catch (const std::exception& e) {
                status_->setText(text(e.what()));
                select(tree_, selected_);
                return;
            }
        }
        selected_ = next;
        inspect();
        select(tree_, next);
    });
    connect(apply, &QPushButton::clicked, this, [this] { report([this] { this->apply(); }); });
    for (auto* control : findChildren<QPushButton*>())
        control->setAutoDefault(false);
    apply->setDefault(true);
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, [this] { report([this] { save(); }); });
    connect(interactive, &QCheckBox::toggled, preview_, &UiPreview::interactive);
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index) {
        if (index == 1 && form_->isEnabled() && form_->dirty()) {
            try {
                this->apply();
            } catch (const std::exception& e) {
                status_->setText(text(e.what()));
                QSignalBlocker blocked(tabs);
                tabs->setCurrentIndex(0);
            }
        }
    });
    preview_->selected = [this](std::string selected) {
        for (auto* item : tree_->findItems(text(selected), Qt::MatchExactly | Qt::MatchRecursive))
            tree_->setCurrentItem(item);
    };
    preview_->action = [this](const ui::Action& action) {
        status_->setText(tr("Preview action: ") + text(action.id) + " = " +
                         QString::number(action.value));
    };
    rebuild();
}
void UiEditor::rebuild() {
    try {
        const auto document =
            ui::parseDocument(source_->toPlainText().toUtf8().toStdString(), file_.string());
        preview_->document(document);
        value_ = ui::documentValue(document);
        QSignalBlocker blocked(tree_);
        tree_->clear();
        auto* theme = new QTreeWidgetItem(tree_, {tr("Theme / Viewport")});
        // Empty IDs are invalid in UI assets, so this cannot collide with a component ID.
        theme->setData(0, Qt::UserRole, "");
        const auto visit = [&](const auto& self, const ui::Node& node,
                               QTreeWidgetItem* parent) -> void {
            auto* item = new QTreeWidgetItem({text(node.id)});
            item->setData(0, Qt::UserRole, text(node.id));
            if (parent)
                parent->addChild(item);
            else
                tree_->addTopLevelItem(item);
            if (node.id == selected_)
                tree_->setCurrentItem(item);
            for (const auto& child : node.children)
                self(self, child, item);
        };
        visit(visit, document.root, nullptr);
        tree_->expandAll();
        if (selectionKnown_ && selected_.empty())
            tree_->setCurrentItem(theme);
        if (!tree_->currentItem()) {
            tree_->setCurrentItem(tree_->topLevelItem(1));
            selected_ = document.root.id;
        }
        selectionKnown_ = true;
        tree_->setEnabled(true);
        form_->setEnabled(true);
        inspect();
        status_->setText(tr("UI document is valid"));
    } catch (const std::exception& e) {
        value_ = {};
        tree_->setEnabled(false);
        form_->setEnabled(false);
        status_->setText(text(e.what()));
    }
}
void UiEditor::inspect() {
    if (!value_.is_object())
        return;
    const auto& data = value_.at("ui");
    if (selected_.empty())
        form_->setValue(
            ContentValue{{"theme", data.at("theme")}, {"viewport", data.at("viewport")}});
    else {
        const auto i = std::ranges::find_if(
            data.at("nodes"), [&](const auto& node) { return node.at("id") == selected_; });
        if (i == data.at("nodes").end())
            return;
        PropertyForm::Choices choices{
            {"kind", {"panel", "label", "button", "toggle", "slider", "list"}},
            {"direction", {"row", "column"}},
            {"align", {"start", "center", "end", "stretch"}},
            {"parent", {""}}};
        for (const auto& node : data.at("nodes"))
            if (node.at("kind") == "panel" && node.at("id") != selected_)
                choices["parent"].push_back(text(node.at("id").get<std::string>()));
        form_->setValue(*i, std::move(choices));
    }
    preview_->selection(selected_);
}
void UiEditor::replace(ContentValue value, std::string selected) {
    if (value == value_)
        return;
    const auto document = ui::readDocument(value);
    // Exact host measurement validates layout before publishing or adding an Undo command.
    UiPreview checked;
    checked.document(document);
    const auto encoded = text(ui::writeDocument(document));
    if (encoded == source_->toPlainText())
        return;
    selected_ = std::move(selected);
    auto cursor = source_->textCursor();
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(encoded);
    cursor.endEditBlock();
}
void UiEditor::apply() {
    if (!value_.is_object())
        throw std::runtime_error("Repair UI source before editing properties");
    auto candidate = value_;
    auto edited = form_->value();
    if (selected_.empty()) {
        candidate["ui"]["theme"] = edited.at("theme");
        candidate["ui"]["viewport"] = edited.at("viewport");
    } else {
        const auto next = edited.at("id").get<std::string>();
        for (auto& node : candidate["ui"]["nodes"]) {
            if (node.at("id") == selected_)
                node = edited;
            else if (node.at("parent") == selected_)
                node["parent"] = next;
        }
        if (candidate.at("ui").at("root") == selected_)
            candidate["ui"]["root"] = next;
        replace(std::move(candidate), next);
        return;
    }
    replace(std::move(candidate), selected_);
}
void UiEditor::add() {
    if (form_->isEnabled() && form_->dirty())
        apply();
    if (!value_.is_object())
        throw std::runtime_error("Repair UI source first");
    const auto& nodes = value_.at("ui").at("nodes");
    const auto selected =
        std::ranges::find_if(nodes, [&](const auto& n) { return n.at("id") == selected_; });
    std::string parent = value_.at("ui").at("root").get<std::string>();
    if (selected != nodes.end())
        parent =
            selected->at("kind") == "panel" ? selected_ : selected->at("parent").get<std::string>();
    ui::Document stub;
    stub.root.id = id();
    stub.root.kind = static_cast<ui::Kind>(palette_->currentIndex());
    stub.root.text = palette_->currentText().toStdString();
    if (stub.root.kind == ui::Kind::List)
        stub.root.value = -1;
    auto node = ui::documentValue(stub).at("ui").at("nodes").at(0);
    node["parent"] = parent;
    auto candidate = value_;
    candidate["ui"]["nodes"].elements().push_back(node);
    replace(std::move(candidate), stub.root.id);
}
void UiEditor::remove() {
    if (!value_.is_object() || selected_.empty())
        return;
    if (value_.at("ui").at("root") == selected_)
        throw std::runtime_error("The UI root cannot be deleted");
    auto candidate = value_;
    std::set<std::string> removed{selected_};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& n : candidate.at("ui").at("nodes"))
            if (removed.contains(n.at("parent").get<std::string>()) &&
                removed.insert(n.at("id").get<std::string>()).second)
                changed = true;
    }
    std::erase_if(candidate["ui"]["nodes"].elements(), [&](const auto& n) {
        return removed.contains(n.at("id").template get<std::string>());
    });
    const auto root = candidate.at("ui").at("root").get<std::string>();
    replace(std::move(candidate), root);
}
void UiEditor::duplicate() {
    if (form_->isEnabled() && form_->dirty())
        apply();
    if (!value_.is_object() || selected_.empty())
        return;
    if (value_.at("ui").at("root") == selected_)
        throw std::runtime_error("Duplicate a child of the root");
    auto candidate = value_;
    std::map<std::string, std::string> copied{{selected_, id()}};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& n : value_.at("ui").at("nodes"))
            if (copied.contains(n.at("parent").get<std::string>()) &&
                copied.emplace(n.at("id").get<std::string>(), id()).second)
                changed = true;
    }
    for (auto n : value_.at("ui").at("nodes"))
        if (copied.contains(n.at("id").get<std::string>())) {
            n["id"] = copied.at(n.at("id").get<std::string>());
            const auto parent = n.at("parent").get<std::string>();
            if (copied.contains(parent))
                n["parent"] = copied.at(parent);
            candidate["ui"]["nodes"].elements().push_back(n);
        }
    replace(std::move(candidate), copied.at(selected_));
}
void UiEditor::reorder(int delta) {
    if (form_->isEnabled() && form_->dirty())
        apply();
    if (!value_.is_object())
        return;
    auto candidate = value_;
    auto& nodes = candidate["ui"]["nodes"].elements();
    auto selected =
        std::ranges::find_if(nodes, [&](const auto& n) { return n.at("id") == selected_; });
    if (selected == nodes.end())
        return;
    const auto index = std::distance(nodes.begin(), selected),
               count = static_cast<ptrdiff_t>(nodes.size());
    for (auto next = index + delta; next >= 0 && next < count; next += delta)
        if (nodes[static_cast<size_t>(next)].at("parent") == selected->at("parent")) {
            std::swap(*selected, nodes[static_cast<size_t>(next)]);
            replace(std::move(candidate), selected_);
            return;
        }
}
void UiEditor::validate() const {
    UiPreview checked;
    checked.document(
        ui::parseDocument(source_->toPlainText().toUtf8().toStdString(), file_.string()));
}
std::string UiEditor::snapshot() {
    if (form_->isEnabled() && form_->dirty())
        apply();
    validate();
    if (read(file_) != baseline_)
        throw std::runtime_error("UI document changed externally; reopen before Play");
    return source_->toPlainText().toUtf8().toStdString();
}
void UiEditor::save() {
    if (form_->isEnabled() && form_->dirty())
        apply();
    validate();
    if (read(file_) != baseline_)
        throw std::runtime_error("UI document changed externally; reopen before saving");
    if (source_->toPlainText() == displayedBaseline_)
        return;
    auto bytes = source_->toPlainText().toUtf8();
    if (baseline_.contains("\r\n"))
        bytes.replace("\n", "\r\n");
    QSaveFile file(pathText(file_));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        read(file_) != baseline_ || !file.commit())
        throw std::runtime_error("Cannot atomically save UI document");
    baseline_ = bytes;
    displayedBaseline_ = source_->toPlainText();
    source_->document()->setModified(false);
}
void UiEditor::report(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::exception& e) {
        status_->setText(text(e.what()));
    }
}
bool UiEditor::discardOrSave() {
    if (source_->toPlainText() == displayedBaseline_ && (!form_->isEnabled() || !form_->dirty()))
        return true;
    const auto choice = QMessageBox::question(
        this, tr("Unsaved UI"), tr("Save UI changes before closing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice == QMessageBox::Discard)
        return true;
    if (choice != QMessageBox::Save)
        return false;
    try {
        save();
        return true;
    } catch (const std::exception& e) {
        status_->setText(text(e.what()));
        return false;
    }
}
void UiEditor::closeEvent(QCloseEvent* event) {
    if (discardOrSave())
        event->accept();
    else
        event->ignore();
}
void UiEditor::reject() {
    if (discardOrSave())
        QDialog::reject();
}
} // namespace paper::editor
