#pragma once
#include "paper/core/content_value.hpp"
#include <QWidget>
#include <functional>
#include <map>
class QFormLayout;
namespace paper::editor {
// Batched edits allow dependent fields (ranges, references, vectors) to change together.
class PropertyForm final : public QWidget {
  public:
    using Choices = std::map<std::string, QStringList, std::less<>>;
    explicit PropertyForm(QWidget* parent = nullptr);
    void setValue(const ContentValue& value, Choices choices = {});
    [[nodiscard]] ContentValue value() const;
    [[nodiscard]] bool dirty() const { return !fields_.empty() && value() != displayed_; }

  private:
    using Read = std::function<ContentValue()>;
    Read field(const std::string& path, const ContentValue& value, QWidget* parent,
               const Choices& choices);
    QFormLayout* layout_ = nullptr;
    std::map<std::string, Read, std::less<>> fields_;
    ContentValue displayed_;
};
} // namespace paper::editor
