#include "paper/authoring/document.hpp"
#include "paper/content/document.hpp"
#include "paper/core/utf8.hpp"
#include "paper/scenes/scene.hpp"
#include <set>
#include <tomlplusplus/toml.hpp>

namespace paper::authoring {
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
size_t offset(std::string_view text, toml::source_position position) {
    size_t result = 0;
    for (size_t line = 1; line < position.line; ++line) {
        const auto next = text.find('\n', result);
        require(next != std::string_view::npos, "Invalid source line");
        result = next + 1;
    }
    for (size_t column = 1; column < position.column; ++column) {
        require(result < text.size(), "Invalid source column");
        (void)nextCodepoint(text, result);
    }
    return result;
}
std::string scalar(const ContentValue& value) {
    const auto encoded = content::encode(ContentValue{{"value", value}});
    const auto first = encoded.find('=');
    require(first != std::string::npos, "Cannot encode property");
    auto result = encoded.substr(encoded.find_first_not_of(" \t", first + 1));
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}
} // namespace
SceneDocument::SceneDocument(std::filesystem::path path, std::string source)
    : path_(std::move(path)), source_(std::move(source)),
      data_(content::parse(source_, path_.string())), saved_(data_) {
    constexpr size_t envelopeFields = 3;
    require(data_.size() == envelopeFields && data_.at("format") == "dcmo.scene" &&
                data_.at("version").is_number_integer() &&
                data_.at("version") == content::limits::sceneVersion && nodes().is_array(),
            "Unsupported scene document");
    std::set<std::string> ids;
    for (const auto& value : nodes())
        require(ids.insert(value.at("id").get<std::string>()).second, "Duplicate node ID");
}
const ContentValue& SceneDocument::node(std::string_view id) const {
    for (const auto& value : nodes())
        if (value.at("id").get<std::string>() == id)
            return value;
    throw std::runtime_error("Unknown scene node: " + std::string(id));
}
ContentValue SceneDocument::property(std::string_view id, std::string_view name) const {
    const auto& value = node(id);
    return value.contains(name) ? value.at(name) : ContentValue{};
}
void SceneDocument::setProperty(std::string_view id, std::string_view name,
                                const ContentValue& value) {
    require(name == "position" || name == "yaw" || name == "scale", "Property is not editable");
    const auto validNumber = [](const ContentValue& v) {
        return v.is_number() && std::isfinite(v.get<double>()) &&
               std::abs(v.get<double>()) <= sceneLimits::coordinateMeters;
    };
    if (!value.is_null()) {
        if (name == "position") {
            constexpr size_t components = 3;
            require(value.is_array() && value.size() == components, "Expected XYZ position");
            for (const auto& component : value)
                require(validNumber(component), "Invalid coordinate");
        } else {
            require(validNumber(value), "Invalid transform number");
            if (name == "scale")
                require(value >= sceneLimits::scaleMinimum && value <= sceneLimits::scaleMaximum,
                        "Scale outside scene limits");
        }
    }
    // Locate first, then mutate exactly one property. Missing values mean undo of an insertion.
    (void)node(id);
    for (auto& entry : data_["scene"]["nodes"])
        if (entry.at("id").get<std::string>() == id) {
            if (value.is_null())
                entry.erase(name);
            else
                entry[name] = value;
            return;
        }
}
std::string SceneDocument::serialized() const {
    if (!dirty())
        return source_;
    const auto parsed = toml::parse(source_, path_.string());
    const auto* sourceNodes = parsed["scene"]["nodes"].as_array();
    require(sourceNodes, "Missing source nodes");
    struct Patch {
        size_t begin, end;
        std::string text;
    };
    std::vector<Patch> patches;
    for (size_t i = 0; i < nodes().size(); ++i) {
        const auto& before = saved_.at("scene").at("nodes").at(i);
        const auto& after = nodes().at(i);
        const auto* table = sourceNodes->get(i)->as_table();
        require(table, "Expected source node table");
        std::string added;
        for (const auto name : {"position", "yaw", "scale"}) {
            const auto oldValue = before.contains(name) ? before.at(name) : ContentValue{};
            const auto newValue = after.contains(name) ? after.at(name) : ContentValue{};
            if (oldValue == newValue)
                continue;
            if (const auto* original = table->get(name)) {
                auto begin = offset(source_, original->source().begin);
                auto end = offset(source_, original->source().end);
                if (newValue.is_null()) {
                    // Removing an assignment is currently only needed when undo crosses Save.
                    require(!table->is_inline(), "Cannot remove a property from an inline node");
                    const auto line = source_.rfind('\n', begin);
                    begin = line == std::string::npos ? 0 : line + 1;
                    patches.push_back({begin, end, ""}); // Keep any trailing comment/newline.
                } else
                    patches.push_back({begin, end, scalar(newValue)});
            } else {
                require(!table->is_inline(), "Cannot insert a property into an inline node");
                added += std::string(name) + " = " + scalar(newValue) + "\n";
            }
        }
        if (!added.empty()) {
            const auto* id = table->get("id");
            require(id, "Missing source node ID");
            const auto valueEnd = offset(source_, id->source().end);
            const auto lineEnd = source_.find('\n', valueEnd);
            const auto insert = lineEnd == std::string::npos ? source_.size() : lineEnd + 1;
            if (lineEnd == std::string::npos)
                added.insert(0, "\n");
            patches.push_back({insert, insert, std::move(added)});
        }
    }
    std::ranges::sort(patches, [](const auto& a, const auto& b) { return a.begin > b.begin; });
    std::string result = source_;
    size_t boundary = result.size();
    for (const auto& patch : patches) {
        require(patch.begin <= patch.end && patch.end <= boundary, "Overlapping source edits");
        result.replace(patch.begin, patch.end - patch.begin, patch.text);
        boundary = patch.begin;
    }
    require(content::parse(result, path_.string()) == data_,
            "Source edit did not preserve scene data");
    return result;
}
void SceneDocument::acceptSaved(std::string source) {
    require(content::parse(source, path_.string()) == data_, "Saved scene differs from document");
    source_ = std::move(source);
    saved_ = data_;
}
} // namespace paper::authoring
