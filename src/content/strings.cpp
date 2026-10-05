#include "paper/content/strings.hpp"
#include <stdexcept>
namespace paper::content {
StringTable stringTable(const ContentValue& strings) {
    if (!strings.is_object())
        throw std::invalid_argument("Expected a strings table");
    StringTable result;
    const auto insert = [&](const std::string& id, const ContentValue& value) {
        if (id.empty() || !value.is_string() || value.get<std::string>().empty())
            throw std::invalid_argument("Expected nonempty string ID/text: " + id);
        if (!result.emplace(id, value.get<std::string>()).second)
            throw std::invalid_argument("Duplicate string ID across categories: " + id);
    };
    for (const auto& [id, value] : strings.items()) {
        if (value.is_object()) {
            if (id.empty())
                throw std::invalid_argument("Empty string category");
            for (const auto& [entry, text] : value.items())
                insert(entry, text);
        } else
            insert(id, value);
    }
    return result;
}
} // namespace paper::content
