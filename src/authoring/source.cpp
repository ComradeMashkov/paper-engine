#include "paper/authoring/source.hpp"
#include "paper/content/document.hpp"
#include "paper/core/utf8.hpp"
#include <set>
#include <tomlplusplus/toml.hpp>

namespace paper::authoring {
namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
size_t offset(std::string_view source, toml::source_position at) {
    size_t result = 0;
    for (size_t line = 1; line < at.line; ++line) {
        const auto next = source.find('\n', result);
        require(next != std::string_view::npos, "Invalid TOML source line");
        result = next + 1;
    }
    for (size_t column = 1; column < at.column; ++column) {
        require(result < source.size(), "Invalid TOML source column");
        (void)nextCodepoint(source, result);
    }
    return result;
}
size_t lineEnd(std::string_view source, size_t at) {
    const auto end = source.find('\n', at);
    return end == std::string_view::npos ? source.size() : end + 1;
}
size_t lineStart(std::string_view source, size_t at) {
    if (!at)
        return 0;
    const auto start = source.rfind('\n', at - 1);
    return start == std::string_view::npos ? 0 : start + 1;
}
size_t regionEnd(std::string_view source, const toml::node& node) {
    auto end = offset(source, node.source().end);
    if (const auto* table = node.as_table())
        for (const auto& [key, child] : *table)
            end = std::max(end, regionEnd(source, child));
    if (const auto* array = node.as_array())
        for (const auto& child : *array)
            end = std::max(end, regionEnd(source, child));
    return end;
}
std::string literal(const ContentValue& value) {
    if (value.is_object()) {
        std::string result = "{";
        bool first = true;
        for (const auto& [key, child] : value.items()) {
            if (!first)
                result += ", ";
            first = false;
            result += literal(ContentValue(key)) + " = " + literal(child);
        }
        return result + "}";
    }
    if (value.is_array()) {
        std::string result = "[";
        bool first = true;
        for (const auto& child : value) {
            if (!first)
                result += ", ";
            first = false;
            result += literal(child);
        }
        return result + "]";
    }
    const auto encoded = content::encode(ContentValue{{"value", value}});
    const auto equals = encoded.find('=');
    require(equals != std::string::npos, "Cannot encode authored value");
    auto result = encoded.substr(encoded.find_first_not_of(" \t", equals + 1));
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}
struct Patch {
    size_t begin, end;
    std::string value;
};
std::string apply(std::string source, std::vector<Patch> patches) {
    std::ranges::stable_sort(patches, [](const auto& a, const auto& b) {
        return a.begin != b.begin ? a.begin > b.begin : a.end > b.end;
    });
    size_t boundary = source.size();
    for (const auto& patch : patches) {
        require(patch.begin <= patch.end && patch.end <= boundary, "Overlapping TOML patches");
        source.replace(patch.begin, patch.end - patch.begin, patch.value);
        boundary = patch.begin;
    }
    return source;
}
class Writer {
  public:
    explicit Writer(std::string_view source) : source_(source) {}
    void table(const toml::table& source, const ContentValue& before, const ContentValue& after,
               const std::string& prefix, std::vector<Patch>& patches) {
        std::set<std::string> fields;
        for (const auto& [key, value] : before.items())
            fields.insert(key);
        for (const auto& [key, value] : after.items())
            fields.insert(key);
        std::string added;
        for (const auto& key : fields) {
            const auto old = before.contains(key) ? before.at(key) : ContentValue{};
            const auto next = after.contains(key) ? after.at(key) : ContentValue{};
            if (old == next)
                continue;
            const auto path = prefix.empty() ? key : prefix + "." + key;
            const auto* original = source.get(key);
            if (!original) {
                require(!next.is_null(), "Missing property removal");
                added += literal(ContentValue(key)) + " = " + literal(next) + "\n";
            } else if (const auto* child = original->as_table(); child && !child->is_inline()) {
                if (next.is_object())
                    table(*child, old, next, path, patches);
                else {
                    patches.push_back({lineStart(source_, offset(source_, child->source().begin)),
                                       lineEnd(source_, regionEnd(source_, *child)),
                                       {}});
                    if (!next.is_null())
                        added += literal(ContentValue(key)) + " = " + literal(next) + "\n";
                }
            } else if (const auto* array = original->as_array();
                       array && array->size() && array->front().is_table() &&
                       !array->front().as_table()->is_inline()) {
                arrayTables(*array, old, next, path, patches);
                if (next.is_array() && next.empty())
                    added += literal(ContentValue(key)) + " = []\n";
            } else {
                auto begin = offset(source_, original->source().begin),
                     end = offset(source_, original->source().end);
                if (next.is_null())
                    begin = lineStart(source_, begin);
                patches.push_back({begin, end, next.is_null() ? "" : literal(next)});
            }
        }
        if (!added.empty()) {
            const auto begin = offset(source_, source.source().begin);
            // Only explicit table headers or the root can receive new assignments.
            require(prefix.empty() || source_.substr(begin).starts_with('['),
                    "New properties require an explicit TOML table header");
            const auto insertion = prefix.empty() ? 0 : lineEnd(source_, begin);
            patches.push_back({insertion, insertion, std::move(added)});
        }
    }

  private:
    std::string_view source_;
    void arrayTables(const toml::array& source, const ContentValue& before,
                     const ContentValue& after, const std::string& prefix,
                     std::vector<Patch>& patches) {
        require(after.is_null() || after.is_array(),
                "Cannot replace authored table array with scalar");
        require(source.size() == before.size(), "Source array size mismatch");
        const auto ids = [](const ContentValue& value) {
            std::vector<std::string> result;
            std::set<std::string> unique;
            if (value.is_null())
                return result;
            for (const auto& entry : value) {
                require(entry.is_object() && entry.contains("id") && entry.at("id").is_string(),
                        "Structural table edits require stable IDs");
                auto id = entry.at("id").get<std::string>();
                require(!id.empty() && unique.insert(id).second, "Duplicate authored table ID");
                result.push_back(std::move(id));
            }
            return result;
        };
        const auto oldIds = ids(before), newIds = ids(after);
        if (oldIds == newIds) {
            for (size_t i = 0; i < source.size(); ++i)
                table(*source.get(i)->as_table(), before[i], after[i], prefix, patches);
            return;
        }
        const auto header = "[[" + prefix + "]]";
        std::map<std::string, std::pair<size_t, size_t>, std::less<>> ranges;
        size_t insertion = source_.size();
        for (size_t i = 0; i < source.size(); ++i) {
            const auto* item = source.get(i)->as_table();
            require(item && !item->is_inline(), "Structural edits require explicit table arrays");
            const auto begin = offset(source_, item->source().begin),
                       end = lineEnd(source_, regionEnd(source_, *item));
            require(source_.substr(begin).starts_with(header), "Unsupported table-array header");
            ranges.emplace(oldIds[i], std::pair{begin, end});
            insertion = std::min(insertion, begin);
            patches.push_back({begin, end, {}});
        }
        std::string replacement;
        if (!after.is_null())
            for (size_t i = 0; i < after.size(); ++i) {
                const auto found = ranges.find(newIds[i]);
                if (found != ranges.end()) {
                    const auto index =
                        static_cast<size_t>(std::ranges::find(oldIds, newIds[i]) - oldIds.begin());
                    const auto [begin, end] = found->second;
                    std::vector<Patch> edits;
                    table(*source.get(index)->as_table(), before[index], after[i], prefix, edits);
                    for (auto& edit : edits) {
                        edit.begin -= begin;
                        edit.end -= begin;
                    }
                    replacement +=
                        apply(std::string(source_.substr(begin, end - begin)), std::move(edits)) +
                        "\n";
                } else {
                    auto wrapped = ContentValue::array({after[i]});
                    std::vector<std::string> keys;
                    size_t begin = 0;
                    while (begin < prefix.size()) {
                        const auto dot = prefix.find('.', begin);
                        keys.push_back(prefix.substr(
                            begin, dot == std::string::npos ? prefix.size() - begin : dot - begin));
                        if (dot == std::string::npos)
                            break;
                        begin = dot + 1;
                    }
                    for (auto key = keys.rbegin(); key != keys.rend(); ++key)
                        wrapped = ContentValue{{*key, wrapped}};
                    const auto encoded = content::encode(wrapped);
                    const auto first = encoded.find(header);
                    require(first != std::string::npos, "Cannot encode new authored table");
                    replacement += encoded.substr(first) + "\n";
                }
            }
        if (!replacement.empty())
            patches.push_back({insertion, insertion, std::move(replacement)});
    }
};
} // namespace
std::string patchSource(std::string_view source, const ContentValue& before,
                        const ContentValue& after, std::string_view name) {
    require(content::parse(source, name) == before, "Source differs from saved authored data");
    if (before == after)
        return std::string(source);
    require(before.is_object() && after.is_object(), "Expected authored document tables");
    const auto parsed = toml::parse(source, name);
    std::vector<Patch> patches;
    Writer(source).table(parsed, before, after, {}, patches);
    auto result = apply(std::string(source), std::move(patches));
    require(content::parse(result, name) == after, "TOML edit did not preserve authored data");
    return result;
}
} // namespace paper::authoring
