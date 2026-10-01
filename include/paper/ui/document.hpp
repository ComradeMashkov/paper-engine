#pragma once
#include "paper/content/document.hpp"
#include "paper/text/parameters.hpp"
#include "paper/ui/ui.hpp"
namespace paper::ui {
namespace documentLimits {
inline constexpr size_t nodes = 4096, depth = 64;
inline constexpr size_t fileBytes = 4 * 1024 * 1024;
inline constexpr float viewportPixels = 8192;
inline constexpr float fontPixels = textParameters::maximumLogicalSizePixels;
} // namespace documentLimits
// Versioned, device-independent UI asset. Hosts may override the authoring viewport.
struct Document {
    Node root;
    Theme theme;
    Vec2 viewport{1280, 720};
};
[[nodiscard]] Document readDocument(const ContentValue& value);
[[nodiscard]] Document parseDocument(std::string_view text, std::string_view name = "UI document");
[[nodiscard]] Document loadDocument(const std::filesystem::path& file);
[[nodiscard]] ContentValue documentValue(const Document& document);
[[nodiscard]] std::string writeDocument(const Document& document);
} // namespace paper::ui
