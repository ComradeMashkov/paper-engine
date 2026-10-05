#pragma once
#include "paper/core/content_value.hpp"
#include <map>
#include <string>
namespace paper::content {
using StringTable = std::map<std::string, std::string, std::less<>>;
// Flat entries or one level of category tables. Categories never qualify IDs.
// The caller owns locale/placeholder policy; every returned string is owned.
[[nodiscard]] StringTable stringTable(const ContentValue& strings);
} // namespace paper::content
