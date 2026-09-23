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
struct Patch {
    size_t begin, end;
    std::string text;
};
std::string apply(std::string source, std::vector<Patch> patches) {
    std::ranges::stable_sort(patches, [](const auto& a, const auto& b) {
        return a.begin != b.begin ? a.begin > b.begin : a.end > b.end;
    });
    size_t boundary = source.size();
    for (const auto& patch : patches) {
        require(patch.begin <= patch.end && patch.end <= boundary, "Overlapping source edits");
        source.replace(patch.begin, patch.end - patch.begin, patch.text);
        boundary = patch.begin;
    }
    return source;
}
size_t lineEnd(std::string_view source, size_t offset) {
    const auto end = source.find('\n', offset);
    return end == std::string_view::npos ? source.size() : end + 1;
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
void validateNodes(const ContentValue& nodes) {
    require(nodes.is_array(), "Expected scene nodes array");
    std::set<std::string> ids;
    for (const auto& value : nodes) {
        require(value.is_object() && value.contains("id") && value.at("id").is_string(),
                "Expected node ID");
        require(!value.at("id").get<std::string>().empty() &&
                    ids.insert(value.at("id").get<std::string>()).second,
                "Empty or duplicate node ID");
    }
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
    validateNodes(nodes());
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
    const bool transform = name == "position" || name == "yaw" || name == "scale";
    require(transform || name == "label" || name == "resource" || name == "parent" ||
                name == "shadow",
            "Property is not editable");
    const auto validNumber = [](const ContentValue& v) {
        return v.is_number() && std::isfinite(v.get<double>()) &&
               std::abs(v.get<double>()) <= sceneLimits::coordinateMeters;
    };
    if (!value.is_null()) {
        if (name == "shadow")
            require(value.is_boolean(), "Expected shadow switch");
        else if (!transform)
            require(value.is_string(), "Expected text property");
        else if (name == "position") {
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
void SceneDocument::replaceNodes(ContentValue nodes) {
    validateNodes(nodes);
    data_["scene"]["nodes"] = std::move(nodes);
}
std::string SceneDocument::serialized() const {
    if (!dirty())
        return source_;
    const auto parsed = toml::parse(source_, path_.string());
    const auto* sourceNodes = parsed["scene"]["nodes"].as_array();
    require(sourceNodes, "Missing source nodes");
    const auto& savedNodes = saved_.at("scene").at("nodes");
    const auto ids = [](const ContentValue& values) {
        std::vector<std::string> result;
        for (const auto& node : values)
            result.push_back(node.at("id").get<std::string>());
        return result;
    };
    if (ids(nodes()) != ids(savedNodes)) {
        // Move existing source blocks verbatim. Only new nodes are formatted. Other
        // scene sections keep their original text; the final parse guards TOML scope.
        std::map<std::string, std::string, std::less<>> blocks;
        std::vector<Patch> structural;
        size_t insertion = source_.size();
        for (size_t i = 0; i < savedNodes.size(); ++i) {
            const auto* table = sourceNodes->get(i)->as_table();
            require(table && !table->is_inline(),
                    "Structural edits require [[scene.nodes]] tables");
            const auto begin = offset(source_, table->source().begin);
            require(source_.substr(begin).starts_with("[[scene.nodes]]"),
                    "Structural edits require explicit [[scene.nodes]] headers");
            const auto end = lineEnd(source_, regionEnd(source_, *table));
            blocks.emplace(savedNodes.at(i).at("id").get<std::string>(),
                           source_.substr(begin, end - begin));
            insertion = std::min(insertion, begin);
            structural.push_back({begin, end, ""});
        }
        if (savedNodes.empty()) {
            const auto begin = offset(source_, sourceNodes->source().begin);
            const auto previousLine = source_.rfind('\n', begin);
            const auto start = previousLine == std::string::npos ? 0 : previousLine + 1;
            require(source_.substr(start, begin - start).find("nodes") != std::string::npos,
                    "Cannot locate empty nodes assignment");
            structural.push_back({start, lineEnd(source_, regionEnd(source_, *sourceNodes)), ""});
            // New array tables belong after existing scene assignments, at EOF.
        }
        std::string replacement;
        for (const auto& node : nodes()) {
            const auto id = node.at("id").get<std::string>();
            if (blocks.contains(id))
                replacement += blocks.at(id) + "\n";
            else {
                const auto encoded = content::encode(
                    ContentValue{{"scene", ContentValue{{"nodes", ContentValue::array({node})}}}});
                const auto header = encoded.find("[[scene.nodes]]");
                require(header != std::string::npos, "Cannot encode new node table");
                replacement += encoded.substr(header) + "\n";
            }
        }
        if (nodes().empty()) {
            const auto* id = parsed["scene"]["id"].node();
            require(id, "Missing scene ID");
            insertion = lineEnd(source_, offset(source_, id->source().end));
            replacement = "nodes = []\n";
        }
        structural.push_back({insertion, insertion, "\n" + replacement});
        SceneDocument intermediate(path_, apply(source_, std::move(structural)));
        require(ids(intermediate.nodes()) == ids(nodes()), "Node order changed during source edit");
        intermediate.data_ = data_;
        return intermediate.serialized();
    }
    std::vector<Patch> patches;
    for (size_t i = 0; i < nodes().size(); ++i) {
        const auto& before = saved_.at("scene").at("nodes").at(i);
        const auto& after = nodes().at(i);
        const auto* table = sourceNodes->get(i)->as_table();
        require(table, "Expected source node table");
        std::string added;
        for (const auto name :
             {"position", "yaw", "scale", "label", "resource", "parent", "shadow", "remove"}) {
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
    std::string result = apply(source_, std::move(patches));
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
