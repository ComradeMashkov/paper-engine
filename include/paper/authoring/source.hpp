#pragma once
#include "paper/core/content_value.hpp"
#include <string>
#include <string_view>

namespace paper::authoring {
// Patch authored TOML using source ranges. Untouched values/tables keep their
// original bytes. Unsupported layouts fail before any replacement is produced.
[[nodiscard]] std::string patchSource(std::string_view source, const ContentValue& before,
                                      const ContentValue& after, std::string_view name);
} // namespace paper::authoring
