#include "property_form.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSizePolicy>
#include <array>
namespace paper::editor {
namespace {
constexpr int numberDecimals = 6, listEditorHeight = 100;
QString text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
} // namespace
PropertyForm::PropertyForm(QWidget* parent) : QWidget(parent), layout_(new QFormLayout(this)) {
    layout_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
}
void PropertyForm::setValue(const ContentValue& value, Choices choices) {
    fields_.clear();
    while (layout_->rowCount())
        layout_->removeRow(0);
    for (const auto& [key, child] : value.items()) {
        auto* holder = new QWidget(this);
        auto read = field(key, child, holder, choices);
        layout_->addRow(text(key), holder);
        fields_.emplace(key, std::move(read));
    }
    displayed_ = this->value();
}
ContentValue PropertyForm::value() const {
    auto value = ContentValue::object();
    for (const auto& [key, read] : fields_)
        value[key] = read();
    return value;
}
PropertyForm::Read PropertyForm::field(const std::string& path, const ContentValue& value,
                                       QWidget* parent, const Choices& choices) {
    auto* row = new QHBoxLayout(parent);
    row->setContentsMargins(0, 0, 0, 0);
    if (value.is_object()) {
        auto* nested = new QWidget(parent);
        auto* form = new QFormLayout(nested);
        form->setContentsMargins(0, 0, 0, 0);
        std::map<std::string, Read, std::less<>> reads;
        for (const auto& [key, child] : value.items()) {
            auto* holder = new QWidget(nested);
            reads.emplace(key, field(path + "." + key, child, holder, choices));
            form->addRow(text(key), holder);
        }
        row->addWidget(nested);
        return [reads = std::move(reads)] {
            auto result = ContentValue::object();
            for (const auto& [key, read] : reads)
                result[key] = read();
            return result;
        };
    }
    if (value.is_array()) {
        if (std::ranges::all_of(value, [](const auto& entry) { return entry.is_string(); })) {
            auto* list = new QPlainTextEdit(parent);
            list->setObjectName(text(path));
            list->setToolTip(tr("One item per line. Empty text is an empty list."));
            QStringList lines;
            for (const auto& item : value)
                lines.push_back(text(item.get<std::string>()));
            list->setPlainText(lines.join('\n'));
            list->setMaximumHeight(listEditorHeight);
            row->addWidget(list);
            return [list, original = value, displayed = list->toPlainText()] {
                if (list->toPlainText() == displayed)
                    return original;
                auto result = ContentValue::array();
                if (!list->toPlainText().isEmpty())
                    for (const auto& line : list->toPlainText().split('\n'))
                        result.elements().push_back(line.toUtf8().toStdString());
                return result;
            };
        }
        std::vector<Read> entries;
        for (size_t i = 0; i < value.size(); ++i) {
            auto* holder = new QWidget(parent);
            entries.push_back(field(path + "." + std::to_string(i), value[i], holder, choices));
            row->addWidget(holder);
        }
        return [entries = std::move(entries)] {
            auto result = ContentValue::array();
            for (const auto& read : entries)
                result.elements().push_back(read());
            return result;
        };
    }
    if (value.is_boolean()) {
        auto* checkbox = new QCheckBox(parent);
        checkbox->setObjectName(text(path));
        checkbox->setChecked(value.get<bool>());
        row->addWidget(checkbox);
        return [checkbox] { return ContentValue(checkbox->isChecked()); };
    }
    const bool maximum = path == "width.maximum" || path == "height.maximum";
    if (value.is_number() || maximum) {
        constexpr std::array<std::string_view, 6> vectors{"center.",   "half.",    "offset.",
                                                          "rotation.", "padding.", "viewport."};
        const bool integral =
            value.is_number_integer() &&
            !std::ranges::any_of(vectors, [&](auto prefix) { return path.starts_with(prefix); });
        auto* spin = new QDoubleSpinBox(parent);
        // Float ranges must not force a field as wide as the longest possible value.
        constexpr int integerMinimumWidth = 60, realMinimumWidth = 90;
        spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        spin->setMinimumWidth(integral ? integerMinimumWidth : realMinimumWidth);
        spin->setObjectName(text(path));
        const auto limit = integral ? double(std::numeric_limits<uint32_t>::max())
                                    : double(std::numeric_limits<float>::max());
        spin->setRange(maximum ? -1 : -limit, limit);
        spin->setDecimals(integral ? 0 : numberDecimals);
        spin->setValue(value.is_number() ? value.get<double>() : -1);
        if (maximum)
            spin->setSpecialValueText(tr("Unlimited"));
        row->addWidget(spin);
        return [spin, original = value, displayed = spin->value(), integral, maximum] {
            if (spin->value() == displayed)
                return original;
            if (maximum && spin->value() < 0)
                return ContentValue("unlimited");
            return integral ? ContentValue(static_cast<int64_t>(spin->value()))
                            : ContentValue(spin->value());
        };
    }
    if (value.is_string()) {
        if (choices.contains(path)) {
            auto* combo = new QComboBox(parent);
            combo->setObjectName(text(path));
            combo->setEditable(true);
            combo->addItems(choices.at(path));
            combo->setCurrentText(text(value.get<std::string>()));
            row->addWidget(combo);
            return [combo] { return ContentValue(combo->currentText().toUtf8().toStdString()); };
        }
        auto* line = new QLineEdit(text(value.get<std::string>()), parent);
        line->setObjectName(text(path));
        row->addWidget(line);
        return [line] { return ContentValue(line->text().toUtf8().toStdString()); };
    }
    throw std::invalid_argument("Unsupported authoring property type");
}
} // namespace paper::editor
