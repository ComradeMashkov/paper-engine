#include "paper/scenes/scene.hpp"
#include "paper/content/document.hpp"
#include "paper/core/pixel_format.hpp"
#include <functional>
#include <set>

namespace paper {
namespace {
using namespace sceneLimits;
using Value = ContentValue;
void require(bool ok, const std::string& why) {
    if (!ok)
        throw std::runtime_error(why);
}
float number(const Value& j, float low = -sceneLimits::coordinateMeters,
             float high = sceneLimits::coordinateMeters) {
    require(j.is_number(), "expected number");
    const float value = j.get<float>();
    require(std::isfinite(value) && value >= low && value <= high, "number out of range");
    return value;
}
Vec3 vec(const Value& j) {
    require(j.is_array() && j.size() == 3,
            "expected vector [x,y,z]"); // numbers: scene schema field/component count.
    return {number(j[0]), number(j[1]),
            number(j[2])}; // numbers: scene schema field/component count.
}
Box3 box(const Value& j) {
    require(j.is_object() && j.contains("center") && j.contains("half"),
            "expected box center/half");
    for (const auto& [field, child] : j.items())
        require(field == "center" || field == "half" || field == "yaw",
                "unknown box field: " + field);
    Box3 b{vec(j.at("center")), vec(j.at("half")), number(j.value("yaw", Value(0)))};
    require(b.half.x > 0 && b.half.y > 0 && b.half.z > 0, "box half extents must be positive");
    return b;
}
void keys(const Value& j, std::initializer_list<std::string_view> allowed) {
    require(j.is_object(), "expected object");
    for (const auto& [field, child] : j.items())
        require(std::ranges::find(allowed, field) != allowed.end(), "unknown field: " + field);
}
std::string id(const Value& j) {
    const auto value = j.get<std::string>();
    require(!value.empty() && value.size() <= idBytes &&
                std::ranges::all_of(value,
                                    [](unsigned char c) {
                                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                               (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                                               c == '-' || c == '/';
                                    }),
            "invalid ID: " + value);
    return value;
}
void nodeKeys(const Value& j) {
    keys(j, {"id",           "template",       "parent",     "position",
             "yaw",          "scale",          "room",       "kind",
             "label",        "bounds",         "detail",     "reach",
             "resource",     "activeResource", "poses",      "inspectResource",
             "inspectScale", "openAngle",      "openOffset", "legacyDoor",
             "stateKey",     "visibleWhen",    "collision",  "acoustic",
             "shadow",       "animation",      "light",      "pose",
             "actions",      "itemInstance"});
}
struct Loader {
    const SceneDocuments* documents = nullptr;
    std::shared_ptr<ScenePackage> package;
    std::map<std::string, MaterialId, std::less<>> materials;
    std::map<std::string, Value, std::less<>> templates;
    std::vector<std::shared_ptr<const Resource>> sources;
    size_t primitiveCount = 0, pixels = 0;
    Value read(const std::filesystem::path& path, std::string_view field) {
        require(path.extension() == std::string(".dc") + std::string(field),
                "Expected native DCMO document extension: " + path.string());
        auto file = package->store->file(path);
        sources.push_back(file);
        try {
            const auto override = documents->find(path);
            const auto document =
                override != documents->end()
                    ? override->second
                    : content::parse(
                          {reinterpret_cast<const char*>(file->data.data()), file->data.size()},
                          path.string());
            constexpr size_t envelopeFields = 3; // format, version, payload.
            require(
                document.size() == envelopeFields && document.contains("format") &&
                    document.contains("version") && document.contains(field),
                "Expected format, version and native document payload; imports are unsupported");
            require(document.at("format").get<std::string>() == "dcmo." + std::string(field) &&
                        document.at("version").is_number_integer() &&
                        document.at("version") == content::limits::sceneVersion,
                    "Unsupported DCMO document format/version");
            auto payload = document.at(field);
            require(payload.is_object() && !payload.contains("version"),
                    "Version belongs to the DCMO envelope");
            payload["version"] = content::limits::sceneVersion;
            return payload;
        } catch (const std::exception& e) {
            throw std::runtime_error(path.string() + ": " + e.what());
        }
    }
    MaterialId material(const Value& value) {
        auto name = id(value);
        auto i = materials.find(name);
        require(i != materials.end(), "unknown material ID: " + name);
        return i->second;
    }
    MaterialId addTexture(PaintedTexture texture) {
        require(texture.pixels.size() + texture.alteredFace.size() <=
                    sceneLimits::materialPixels - pixels,
                "scene material pixel limit exceeded");
        pixels += texture.pixels.size() + texture.alteredFace.size();
        return package->materials.append(std::move(texture));
    }
    std::shared_ptr<const SceneAsset> asset(const std::string& name) {
        auto i = package->assets.find(name);
        require(i != package->assets.end(), "unknown resource ID: " + name);
        return i->second;
    }
    void resource(const Value& j) {
        keys(j, {"id", "type", "primitives", "material", "height", "path", "altered", "faceCenter",
                 "faceRadius", "replaceMaterial"});
        const auto name = id(j.at("id"));
        require(!package->assets.contains(name), "duplicate resource ID: " + name);
        auto value = std::make_unique<SceneAsset>();
        value->dependencies = sources;
        const auto type = j.at("type").get<std::string>();
        Mesh3 mesh;
        if (type == "procedural") {
            for (const auto& p : j.at("primitives")) {
                require(++primitiveCount <= sceneLimits::primitives, "primitive limit exceeded");
                keys(p, {"box", "quad", "material", "uv", "detail"});
                const auto first = mesh.size();
                const auto mat = material(p.at("material"));
                require(p.contains("box") != p.contains("quad"),
                        "expected exactly one primitive shape");
                if (p.contains("box"))
                    boxMesh(mesh, box(p.at("box")), mat);
                else {
                    const auto& q = p.at("quad");
                    require(
                        q.size() == 4,
                        "quad needs four vertices"); // numbers: scene schema field/component count.
                    const auto uv = p.value("uv", Value::array({1, 1}));
                    require(uv.size() == 2,
                            "UV scale needs two values"); // numbers: scene schema field/component
                                                          // count.
                    quad(mesh, vec(q[0]), vec(q[1]), vec(q[2]), vec(q[3]), mat,
                         number(uv[0], 0), // numbers: scene schema field/component count.
                         number(uv[1], 0));
                }
                for (auto i = first; i < mesh.size(); ++i)
                    mesh[i].detail = static_cast<DetailTag>(p.value("detail", 0u));
            }
            value->mesh = makeMesh(std::move(mesh));
            value->byteSize = value->mesh->size() * sizeof(Triangle3);
        } else if (type == "sprite") {
            MaterialId mat{};
            if (j.contains("path")) {
                auto image = package->store->image(j.at("path").get<std::string>());
                require(std::ranges::any_of(
                            image->texture.pixels,
                            [](const Pixel& p) { return p.a >= pixelFormat::opaqueAlpha; }),
                        "empty sprite: " + name);
                value->dependencies.push_back(image);
                value->sprite = image->layout;
                auto texture = image->texture;
                if (j.contains("altered")) {
                    auto altered = package->store->image(j.at("altered").get<std::string>());
                    require(texture.width == altered->texture.width &&
                                texture.height == altered->texture.height,
                            "alternate sprite canvas mismatch");
                    texture.alteredFace = altered->texture.pixels;
                    value->dependencies.push_back(altered);
                    const auto& c = j.at("faceCenter");
                    const auto& r = j.at("faceRadius");
                    require(c.size() == 2 && r.size() == 2,
                            "expected face coordinate pair"); // numbers: scene schema
                                                              // field/component count.
                    texture.faceCenter = {number(c[0], 0, 1), number(c[1], 0, 1)};
                    texture.faceRadius = {number(r[0], minimumFaceRadius, 1),
                                          number(r[1], minimumFaceRadius, 1)};
                }
                if (j.contains("replaceMaterial")) {
                    mat = material(j.at("replaceMaterial"));
                    const auto& old = package->materials.art(mat);
                    pixels -= old.pixels.size() + old.alteredFace.size();
                    require(texture.pixels.size() + texture.alteredFace.size() <=
                                sceneLimits::materialPixels - pixels,
                            "scene material pixel limit exceeded");
                    pixels += texture.pixels.size() + texture.alteredFace.size();
                    package->materials.setArt(mat, std::move(texture));
                } else
                    mat = addTexture(std::move(texture));
            } else
                mat = material(j.at("material"));
            const float height = number(j.value("height", Value(defaultSpriteHeightMeters)),
                                        minimumExtentMeters, maximumSpriteHeightMeters);
            const float width = height * value->sprite.widthOverHeight * .5f;
            quad(mesh, {-width, height, 0}, {width, height, 0}, {width, 0, 0}, {-width, 0, 0}, mat,
                 spriteUvMaximum, spriteUvMaximum);
            for (auto& triangle : mesh)
                for (auto& v : triangle.v)
                    v.uv.y =
                        value->sprite.top + v.uv.y * (value->sprite.bottom - value->sprite.top);
            value->mesh = makeMesh(std::move(mesh));
            value->billboard = true;
            value->byteSize = value->mesh->size() * sizeof(Triangle3);
        } else if (type == "gltf") {
            auto model = package->store->model(j.at("path").get<std::string>());
            value->dependencies.push_back(model);
            auto mapped = std::make_shared<ModelResource>(*model);
            std::vector<MaterialId> map;
            for (const auto& m : model->materials)
                map.push_back(addTexture(m));
            std::map<MeshHandle, MeshHandle> meshes;
            for (auto& node : mapped->nodes)
                for (auto& handle : node.meshes) {
                    if (!meshes.contains(handle)) {
                        auto copy = *handle;
                        for (auto& triangle : copy)
                            triangle.material = map.at(static_cast<size_t>(triangle.material));
                        value->byteSize += copy.size() * sizeof(Triangle3);
                        meshes[handle] = makeMesh(std::move(copy));
                    }
                    handle = meshes.at(handle);
                }
            mapped->materials.clear();
            value->model = std::move(mapped);
            for (const auto& diagnostic : model->diagnostics)
                package->diagnostics.push_back(name + ": " + diagnostic);
        } else
            throw std::runtime_error("unsupported resource type: " + type);
        package->assets.emplace(name, package->store->publish("scene:" + name, std::move(value)));
    }
};
} // namespace
bool contains(const Box3& b, Vec3 p) {
    const auto q = rotateY(p - b.center, -b.yaw);
    return std::abs(q.x) <= b.half.x && std::abs(q.y) <= b.half.y && std::abs(q.z) <= b.half.z;
}
std::shared_ptr<ScenePackage> loadScenes(const std::filesystem::path& root,
                                         std::vector<PaintedTexture> art,
                                         std::span<const std::string_view> names,
                                         const std::filesystem::path& manifest,
                                         const SceneDocuments& documents) {
    Loader l;
    l.documents = &documents;
    l.package = std::make_shared<ScenePackage>(std::move(art));
    l.package->store = std::make_shared<ResourceStore>(root);
    require(names.size() == l.package->materials.size(), "material names must match library");
    for (size_t i = 0; i < names.size(); ++i) {
        l.materials.emplace(names[i], static_cast<MaterialId>(i));
        const auto& texture = l.package->materials.art(static_cast<MaterialId>(i));
        l.pixels += texture.pixels.size() + texture.alteredFace.size();
    }
    require(l.pixels <= sceneLimits::materialPixels, "base material pixel limit exceeded");
    try {
        auto m = l.read(manifest, "world");
        keys(m, {"version", "entrySpawn", "resources", "templates", "scenes"});
        l.package->entrySpawn = id(m.at("entrySpawn"));
        for (const auto& path : m.at("resources")) {
            auto j = l.read(path.get<std::string>(), "resources");
            keys(j, {"version", "resources"});
            for (const auto& value : j.at("resources"))
                l.resource(value);
        }
        for (const auto& path : m.at("templates")) {
            auto j = l.read(path.get<std::string>(), "templates");
            keys(j, {"version", "templates"});
            require(j.at("templates").is_array(), "templates must be an array");
            for (const auto& t : j.at("templates")) {
                nodeKeys(t);
                require(l.templates.size() < sceneLimits::nodes, "template count limit exceeded");
                for (const char* field : {"resource", "activeResource", "inspectResource"})
                    if (t.contains(field))
                        (void)l.asset(id(t.at(field)));
                if (t.contains("bounds"))
                    (void)box(t.at("bounds"));
                if (t.contains("position"))
                    (void)vec(t.at("position"));
                if (t.contains("openOffset"))
                    (void)vec(t.at("openOffset"));
                if (t.contains("scale"))
                    (void)number(t.at("scale"), sceneLimits::scaleMinimum,
                                 sceneLimits::scaleMaximum);
                if (t.contains("reach"))
                    (void)number(t.at("reach"), minimumExtentMeters, maximumReachMeters);
                if (t.contains("inspectScale"))
                    (void)number(t.at("inspectScale"), minimumExtentMeters, maximumInspectScale);
                require(l.templates.emplace(id(t.at("id")), t).second, "duplicate template ID");
            }
        }
        require(m.at("scenes").size() <= sceneLimits::scenes, "scene count limit exceeded");
        std::map<std::string, Value> rawNodes;
        std::map<std::string, std::string> nodeScenes;
        std::set<std::string> ids, sceneIds, roomIds, spawnIds;
        auto unique = [&](const Value& j) {
            auto v = id(j.at("id"));
            require(ids.insert(v).second, "duplicate ID: " + v);
            return v;
        };
        for (const auto& path : m.at("scenes")) {
            auto j = l.read(path.get<std::string>(), "scene");
            keys(j, {"version", "id", "rooms", "nodes", "spawns", "zones", "lights"});
            const auto scene = id(j.at("id"));
            require(sceneIds.insert(scene).second, "duplicate scene ID");
            for (const auto& r : j.at("rooms")) {
                keys(r, {"id", "label", "bounds", "floorY"});
                const auto rid = unique(r);
                roomIds.insert(rid);
                l.package->rooms.push_back({rid, scene, r.at("label").get<std::string>(),
                                            box(r.at("bounds")), number(r.at("floorY"))});
            }
            for (const auto& s : j.at("spawns")) {
                keys(s, {"id", "position", "yaw"});
                const auto sid = unique(s);
                spawnIds.insert(sid);
                Camera c;
                c.position = vec(s.at("position"));
                c.yaw = number(s.value("yaw", Value(0)), -pi3, pi3);
                l.package->spawns.push_back({sid, scene, c});
            }
            for (const auto& z : j.value("zones", Value::array())) {
                keys(z, {"id", "bounds", "targetSpawn"});
                l.package->zones.push_back(
                    {unique(z), scene, z.value("targetSpawn", std::string{}), box(z.at("bounds"))});
            }
            for (const auto& v : j.value("lights", Value::array())) {
                keys(v, {"id", "position", "color", "intensity", "range", "attenuation"});
                Light light;
                light.position = vec(v.at("position"));
                light.color = vec(v.at("color"));
                require(light.color.x >= 0 && light.color.y >= 0 && light.color.z >= 0,
                        "negative light color");
                light.intensity = number(v.at("intensity"), 0, maximumLightIntensity);
                light.rangeMeters =
                    number(v.at("range"), minimumExtentMeters, maximumLightRangeMeters);
                light.attenuationPerMeterSquared =
                    number(v.at("attenuation"), 0, maximumAttenuationPerMeterSquared);
                l.package->lights.push_back({unique(v), scene, light});
            }
            for (const auto& n : j.at("nodes")) {
                const auto nid = unique(n);
                auto merged = Value::object();
                if (n.contains("template")) {
                    const auto t = id(n.at("template"));
                    require(l.templates.contains(t), "unknown template: " + t);
                    merged = l.templates.at(t);
                }
                auto overrides = n;
                if (overrides.contains("remove")) {
                    const auto& removed = overrides.at("remove");
                    require(removed.is_array(), "remove must be an array of inherited field names");
                    std::set<std::string> fields;
                    for (const auto& value : removed) {
                        const auto field = id(value);
                        require(field != "id" && field != "template" && field != "remove" &&
                                    fields.insert(field).second && merged.contains(field) &&
                                    !overrides.contains(field),
                                "Invalid or conflicting inherited field removal: " + field);
                        merged.erase(field);
                    }
                    overrides.erase("remove");
                }
                merged.overlay(overrides);
                rawNodes.emplace(nid, std::move(merged));
                nodeScenes[nid] = scene;
                require(rawNodes.size() <= sceneLimits::nodes, "node count limit exceeded");
            }
        }
        std::map<std::string, SceneNode> resolved;
        std::set<std::string> visiting;
        std::function<SceneNode(const std::string&, size_t)> resolve = [&](const std::string& name,
                                                                           size_t depth) {
            if (resolved.contains(name))
                return resolved.at(name);
            require(rawNodes.contains(name), "unknown parent: " + name);
            require(depth < sceneLimits::hierarchyDepth && visiting.insert(name).second,
                    "cyclic or deep hierarchy: " + name);
            const auto& j = rawNodes.at(name);
            nodeKeys(j);
            SceneNode n;
            n.id = name;
            n.scene = nodeScenes.at(name);
            n.room = j.value("room", std::string{});
            n.kind = j.value("kind", std::string{});
            n.label = j.value("label", std::string{});
            n.yaw = number(j.value("yaw", Value(0)));
            n.transform = {vec(j.value("position", Value::array({0, 0, 0}))),
                           Rotation3::axisAngle({0, 1, 0}, n.yaw),
                           number(j.value("scale", Value(1)), sceneLimits::scaleMinimum,
                                  sceneLimits::scaleMaximum)};
            if (j.contains("parent")) {
                n.parent = id(j.at("parent"));
                auto parent = resolve(n.parent, depth + 1);
                require(parent.scene == n.scene, "parent belongs to another scene");
                require(
                    parent.animation < 0 && length(parent.openOffset) == 0 &&
                        parent.openAngle == 0 && (!parent.asset || !parent.asset->billboard) &&
                        parent.poses.empty(),
                    "scene parents must be static; put moving hierarchy inside a glTF resource: " +
                        name);
                n.transform = compose(parent.transform, n.transform);
                n.yaw += parent.yaw;
                if (n.room.empty())
                    n.room = parent.room;
            }
            require(n.room.empty() || std::ranges::any_of(l.package->rooms,
                                                          [&](const auto& r) {
                                                              return r.id == n.room &&
                                                                     r.scene == n.scene;
                                                          }),
                    "unknown room in scene: " + n.room);
            require(std::isfinite(n.transform.scale) &&
                        n.transform.scale >= sceneLimits::scaleMinimum &&
                        n.transform.scale <= sceneLimits::scaleMaximum,
                    "composed scale out of range: " + name);
            for (float value :
                 {n.transform.position.x, n.transform.position.y, n.transform.position.z})
                require(std::isfinite(value) && std::abs(value) <= sceneLimits::coordinateMeters,
                        "composed position out of range: " + name);
            if (j.contains("light")) {
                const auto& v = j.at("light");
                keys(v, {"position", "color", "intensity", "range", "attenuation"});
                Light light;
                light.position = n.transform.point(vec(v.at("position")));
                light.color = vec(v.at("color"));
                require(light.color.x >= 0 && light.color.y >= 0 && light.color.z >= 0,
                        "negative light color");
                light.intensity = number(v.at("intensity"), 0, maximumLightIntensity);
                light.rangeMeters =
                    number(v.at("range"), minimumExtentMeters, maximumLightRangeMeters);
                light.attenuationPerMeterSquared =
                    number(v.at("attenuation"), 0, maximumAttenuationPerMeterSquared);
                n.light = light;
            }
            n.bounds =
                j.contains("bounds")
                    ? box(j.at("bounds"))
                    : Box3{{}, {minimumExtentMeters, minimumExtentMeters, minimumExtentMeters}};
            n.bounds.center = n.transform.point(n.bounds.center);
            n.bounds.half = n.bounds.half * n.transform.scale;
            n.bounds.yaw += n.yaw;
            n.detail = j.value("detail", 0);
            n.reach =
                number(j.value("reach", Value(n.reach)), minimumExtentMeters, maximumReachMeters);
            n.openAngle = number(j.value("openAngle", Value(0)));
            n.openOffset = rotateY(
                vec(j.value("openOffset", Value::array({0, 0, 0}))) * n.transform.scale, n.yaw);
            n.legacyDoor = j.value("legacyDoor", -1);
            n.stateKey = j.value("stateKey", std::string{});
            n.visibleWhen = j.value("visibleWhen", std::string{});
            n.actions = j.contains("actions") ? id(j.at("actions")) : "";
            n.itemInstance = j.contains("itemInstance") ? id(j.at("itemInstance")) : "";
            n.collidable = j.value("collision", false);
            require((!n.collidable && n.kind.empty()) || j.contains("bounds"),
                    "physical node needs bounds: " + name);
            n.acoustic = j.value("acoustic", false);
            n.shadow = j.value("shadow", true);
            n.inspectScale = number(j.value("inspectScale", Value(n.inspectScale)),
                                    minimumExtentMeters, maximumInspectScale);
            if (j.contains("resource"))
                n.asset = l.asset(id(j.at("resource")));
            if (j.contains("activeResource"))
                n.activeAsset = l.asset(id(j.at("activeResource")));
            if (j.contains("inspectResource"))
                n.inspection = l.asset(id(j.at("inspectResource")));
            for (const auto& pose : j.value("poses", Value::array()))
                n.poses.push_back(l.asset(id(pose)));
            n.pose = j.value("pose", size_t{0});
            require(n.pose == 0 || n.pose < n.poses.size(), "pose index out of range: " + name);
            n.animation = j.value("animation", -1);
            require(n.animation == -1 ||
                        (n.asset && n.asset->model && n.animation >= 0 &&
                         static_cast<size_t>(n.animation) < n.asset->model->animations.size()),
                    "unknown animation index");
            visiting.erase(name);
            resolved.emplace(name, n);
            return n;
        };
        for (const auto& [name, _] : rawNodes)
            l.package->nodes.push_back(resolve(name, 0));
        for (const auto& n : l.package->nodes)
            require(n.visibleWhen.empty() || (resolved.contains(n.visibleWhen) &&
                                              !resolved.at(n.visibleWhen).kind.empty()),
                    "unknown visibleWhen object: " + n.visibleWhen);
        require(spawnIds.contains(l.package->entrySpawn), "missing entry spawn");
        for (const auto& z : l.package->zones)
            require(z.targetSpawn.empty() || spawnIds.contains(z.targetSpawn),
                    "unknown target spawn: " + z.targetSpawn);
        for (const auto& s : l.package->spawns)
            require(std::ranges::any_of(l.package->rooms,
                                        [&](const auto& r) {
                                            return r.scene == s.scene &&
                                                   contains(r.bounds, s.camera.position);
                                        }),
                    "spawn outside scene rooms: " + s.id);
        for (const auto& n : l.package->nodes)
            if (n.light)
                l.package->lights.push_back({n.id, n.scene, *n.light});
        l.package->dependencies = l.sources;
        return l.package;
    } catch (const std::exception& e) {
        throw std::runtime_error("Scene " + manifest.string() + ": " + e.what());
    }
}
} // namespace paper
