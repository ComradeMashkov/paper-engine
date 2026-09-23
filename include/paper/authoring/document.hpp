#pragma once
#include "paper/core/content_value.hpp"
#include <filesystem>
#include <string>

namespace paper::authoring {
// Owns the authored values and original text, never flattened runtime transforms.
class SceneDocument {
  public:
    SceneDocument(std::filesystem::path path, std::string source);
    const std::filesystem::path& path() const { return path_; }
    const std::string& original() const { return source_; }
    const ContentValue& data() const { return data_; }
    const ContentValue& nodes() const { return data_.at("scene").at("nodes"); }
    const ContentValue& node(std::string_view id) const;
    ContentValue property(std::string_view id, std::string_view name) const;
    void setProperty(std::string_view id, std::string_view name, const ContentValue& value);
    bool dirty() const { return data_ != saved_; }
    // Patches only position/yaw/scale source ranges; verifies semantic equality.
    // Unsupported source layouts fail without producing a replacement file.
    std::string serialized() const;
    void acceptSaved(std::string source);

  private:
    std::filesystem::path path_;
    std::string source_;
    ContentValue data_, saved_;
};
} // namespace paper::authoring
