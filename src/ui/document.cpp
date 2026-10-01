#include "paper/ui/document.hpp"
#include <set>
namespace paper::ui {
namespace {
using V = ContentValue;
constexpr std::array<std::string_view, 6> kinds{"panel",  "label",  "button",
                                                "toggle", "slider", "list"};
constexpr std::array<std::string_view, 4> aligns{"start", "center", "end", "stretch"};
void require(bool condition, const char* message) {
    if (!condition)
        throw std::invalid_argument(message);
}
void keys(const V& value, std::initializer_list<std::string_view> allowed) {
    require(value.is_object(), "Expected UI table");
    for (const auto& [key, child] : value.items())
        require(std::ranges::find(allowed, key) != allowed.end(), "Unknown UI property");
}
float number(const V& value) {
    require(value.is_number() && std::isfinite(value.get<float>()), "Expected finite UI number");
    return value.get<float>();
}
Dimension dimension(const V& value) {
    keys(value, {"minimum", "preferred", "maximum", "grow", "shrink"});
    Dimension result;
    result.minimum = number(value.value("minimum", V(result.minimum)));
    result.preferred = number(value.value("preferred", V(result.preferred)));
    if (value.contains("maximum")) {
        const auto& maximum = value.at("maximum");
        if (maximum.is_string())
            require(maximum == "unlimited", "Unknown UI maximum size");
        else
            result.maximum = number(maximum);
    }
    result.grow = number(value.value("grow", V(result.grow)));
    result.shrink = number(value.value("shrink", V(result.shrink)));
    return result;
}
V dimension(Dimension d) {
    V result{
        {"minimum", d.minimum}, {"preferred", d.preferred}, {"grow", d.grow}, {"shrink", d.shrink}};
    result["maximum"] = std::isfinite(d.maximum) ? V(d.maximum) : V("unlimited");
    return result;
}
Color color(const V& value) {
    constexpr size_t channels = 4;
    require(value.is_array() && value.size() == channels, "Expected RGBA UI color");
    std::array<uint8_t, channels> bytes{};
    for (size_t i = 0; i < channels; ++i) {
        require(value[i].is_number_integer() && value[i].get<int>() >= 0 &&
                    value[i].get<int>() <= 255,
                "UI color channel outside byte range"); // numbers: RGBA wire byte range.
        bytes[i] = value[i].get<uint8_t>();
    }
    return {bytes[0], bytes[1], bytes[2], bytes[3]}; // numbers: RGBA wire layout.
}
V color(Color c) {
    return V::array({c.r, c.g, c.b, c.a});
}
Theme theme(const V& value) {
    keys(value, {"background", "surface", "hover", "pressed", "accent", "text", "disabled", "focus",
                 "fontPixels", "lineHeight", "inset", "thumbPixels", "scrollbarPixels",
                 "tooltipDelaySeconds"});
    Theme t;
    for (const auto& [key, field] :
         std::initializer_list<std::pair<const char*, Color*>>{{"background", &t.background},
                                                               {"surface", &t.surface},
                                                               {"hover", &t.hover},
                                                               {"pressed", &t.pressed},
                                                               {"accent", &t.accent},
                                                               {"text", &t.text},
                                                               {"disabled", &t.disabled},
                                                               {"focus", &t.focus}})
        if (value.contains(key))
            *field = color(value.at(key));
    for (const auto& [key, field] : std::initializer_list<std::pair<const char*, float*>>{
             {"fontPixels", &t.fontPixels},
             {"lineHeight", &t.lineHeight},
             {"inset", &t.inset},
             {"thumbPixels", &t.thumbPixels},
             {"scrollbarPixels", &t.scrollbarPixels},
             {"tooltipDelaySeconds", &t.tooltipDelaySeconds}})
        if (value.contains(key))
            *field = number(value.at(key));
    return t;
}
V theme(const Theme& t) {
    return V{{"background", color(t.background)},
             {"surface", color(t.surface)},
             {"hover", color(t.hover)},
             {"pressed", color(t.pressed)},
             {"accent", color(t.accent)},
             {"text", color(t.text)},
             {"disabled", color(t.disabled)},
             {"focus", color(t.focus)},
             {"fontPixels", t.fontPixels},
             {"lineHeight", t.lineHeight},
             {"inset", t.inset},
             {"thumbPixels", t.thumbPixels},
             {"scrollbarPixels", t.scrollbarPixels},
             {"tooltipDelaySeconds", t.tooltipDelaySeconds}};
}
void validate(const Document& document) {
    require(document.theme.fontPixels <= documentLimits::fontPixels,
            "UI document font exceeds authoring limit");
    require(std::isfinite(document.viewport.x) && std::isfinite(document.viewport.y) &&
                document.viewport.x >= 1 && document.viewport.y >= 1 &&
                document.viewport.x <= documentLimits::viewportPixels &&
                document.viewport.y <= documentLimits::viewportPixels,
            "UI viewport outside authoring limits");
    // Tree validation does not depend on font width; actual hosts measure during layout.
    Context context([](std::string_view, float) { return 0.f; }, document.theme);
    context.setTree(document.root);
}
} // namespace
Document readDocument(const ContentValue& value) {
    keys(value, {"format", "version", "ui"});
    require(value.at("format") == "paper.ui" && value.at("version").is_number_integer() &&
                value.at("version") == 1,
            "Unsupported UI format/version");
    const auto& data = value.at("ui");
    keys(data, {"root", "viewport", "theme", "nodes"});
    Document result;
    if (data.contains("theme"))
        result.theme = theme(data.at("theme"));
    if (data.contains("viewport")) {
        const auto& viewport = data.at("viewport");
        constexpr size_t axes = 2;
        require(viewport.is_array() && viewport.size() == axes,
                "Expected UI viewport width/height");
        result.viewport = {number(viewport[0]),
                           number(viewport[1])}; // numbers: viewport wire pair.
    }
    const auto& nodes = data.at("nodes");
    require(nodes.is_array() && !nodes.empty() && nodes.size() <= documentLimits::nodes,
            "UI node budget exceeded");
    std::map<std::string, Node, std::less<>> entries;
    std::map<std::string, std::vector<std::string>, std::less<>> children;
    const auto root = data.at("root").get<std::string>();
    for (const auto& v : nodes) {
        keys(v, {"id",      "parent", "kind",    "text",    "tooltip", "enabled", "visible",
                 "checked", "value",  "minimum", "maximum", "step",    "items",   "direction",
                 "align",   "scroll", "gap",     "padding", "width",   "height"});
        Node n;
        n.id = v.at("id").get<std::string>();
        const auto kind = v.at("kind").get<std::string>();
        const auto found = std::ranges::find(kinds, kind);
        require(found != kinds.end(), "Unknown UI component kind");
        n.kind = static_cast<Kind>(found - kinds.begin());
        n.text = v.value("text", std::string{});
        n.tooltip = v.value("tooltip", std::string{});
        n.enabled = v.value("enabled", true);
        n.visible = v.value("visible", true);
        n.checked = v.value("checked", false);
        n.value = v.value("value", n.kind == Kind::List ? -1.0 : 0.0);
        n.minimum = v.value("minimum", n.minimum);
        n.maximum = v.value("maximum", n.maximum);
        n.step = v.value("step", n.step);
        if (v.contains("items")) {
            require(v.at("items").is_array(), "Expected UI list items");
            for (const auto& item : v.at("items"))
                n.items.push_back(item.get<std::string>());
        }
        const auto direction = v.value("direction", std::string("column"));
        require(direction == "row" || direction == "column", "Unknown UI direction");
        n.layout.direction = direction == "row" ? Direction::Row : Direction::Column;
        const auto align = std::ranges::find(aligns, v.value("align", std::string("stretch")));
        require(align != aligns.end(), "Unknown UI alignment");
        n.layout.align = static_cast<Align>(align - aligns.begin());
        n.layout.scroll = v.value("scroll", false);
        n.layout.gap = number(v.value("gap", V(0)));
        if (v.contains("width"))
            n.layout.width = dimension(v.at("width"));
        if (v.contains("height"))
            n.layout.height = dimension(v.at("height"));
        if (v.contains("padding")) {
            const auto& p = v.at("padding");
            constexpr size_t sides = 4;
            require(p.is_array() && p.size() == sides, "Expected left/top/right/bottom padding");
            n.layout.padding = {number(p[0]), number(p[1]), number(p[2]),
                                number(p[3])}; // numbers: padding wire layout.
        }
        const auto parent = v.value("parent", std::string{});
        require((n.id == root) == parent.empty(), "UI requires exactly one declared root");
        const auto id = n.id;
        require(entries.emplace(id, std::move(n)).second, "Duplicate UI node ID");
        children[parent].push_back(v.at("id").get<std::string>());
    }
    require(entries.contains(root), "Missing UI root");
    std::set<std::string> visited;
    const auto build = [&](const auto& self, const std::string& id, size_t depth) -> Node {
        require(depth < documentLimits::depth && visited.insert(id).second,
                "Cyclic/deep UI hierarchy");
        Node node = std::move(entries.at(id));
        for (const auto& child : children[id])
            node.children.push_back(self(self, child, depth + 1));
        return node;
    };
    result.root = build(build, root, 0);
    require(visited.size() == entries.size(), "Disconnected/missing/cyclic UI parent");
    validate(result);
    return result;
}
Document parseDocument(std::string_view text, std::string_view name) {
    require(text.size() <= documentLimits::fileBytes, "UI document exceeds byte budget");
    return readDocument(content::parse(text, name));
}
Document loadDocument(const std::filesystem::path& file) {
    require(std::filesystem::file_size(file) <= documentLimits::fileBytes,
            "UI document exceeds byte budget");
    return readDocument(content::read(file));
}
ContentValue documentValue(const Document& document) {
    validate(document);
    V nodes = V::array();
    const auto flatten = [&](const auto& self, const Node& n, const std::string& parent) -> void {
        V v{{"id", n.id},
            {"parent", parent},
            {"kind", kinds.at(static_cast<size_t>(n.kind))},
            {"text", n.text},
            {"tooltip", n.tooltip},
            {"enabled", n.enabled},
            {"visible", n.visible},
            {"checked", n.checked},
            {"value", n.value},
            {"minimum", n.minimum},
            {"maximum", n.maximum},
            {"step", n.step},
            {"items", V::array()},
            {"direction", n.layout.direction == Direction::Row ? "row" : "column"},
            {"align", aligns.at(static_cast<size_t>(n.layout.align))},
            {"scroll", n.layout.scroll},
            {"gap", n.layout.gap},
            {"padding", V::array({n.layout.padding.left, n.layout.padding.top,
                                  n.layout.padding.right, n.layout.padding.bottom})},
            {"width", dimension(n.layout.width)},
            {"height", dimension(n.layout.height)}};
        for (const auto& item : n.items)
            v["items"].elements().push_back(item);
        nodes.elements().push_back(std::move(v));
        for (const auto& child : n.children)
            self(self, child, n.id);
    };
    flatten(flatten, document.root, "");
    return V{{"format", "paper.ui"},
             {"version", 1},
             {"ui", V{{"root", document.root.id},
                      {"viewport", V::array({document.viewport.x, document.viewport.y})},
                      {"theme", theme(document.theme)},
                      {"nodes", nodes}}}};
}
std::string writeDocument(const Document& document) {
    auto encoded = content::encode(documentValue(document));
    require(encoded.size() <= documentLimits::fileBytes, "UI document exceeds byte budget");
    return encoded;
}
} // namespace paper::ui
