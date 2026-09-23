#pragma once
#include "paper/core/content_value.hpp"
#include <filesystem>

namespace paper::content {
namespace limits {
inline constexpr size_t fileBytes = 64 * 1024 * 1024;
inline constexpr size_t nesting = 64, values = 2 * 1024 * 1024;
inline constexpr int sceneVersion = 2;
} // namespace limits
[[nodiscard]] ContentValue parse(std::string_view source, std::string_view name);
[[nodiscard]] ContentValue read(const std::filesystem::path& path);
// Canonical TOML serialization; not a comment-preserving editor writer.
[[nodiscard]] std::string encode(const ContentValue& value);
} // namespace paper::content
