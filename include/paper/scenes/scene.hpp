#pragma once
#include "paper/core/content_value.hpp"
#include "paper/resources/model.hpp"
#include <optional>

namespace paper {
namespace sceneLimits {
inline constexpr size_t nodes = 10000, scenes = 64, primitives = 50000, hierarchyDepth = 64;
inline constexpr size_t idBytes = 128;
inline constexpr float minimumFaceRadius = .0001f, minimumExtentMeters = .01f;
inline constexpr float defaultSpriteHeightMeters = 2, maximumSpriteHeightMeters = 100;
inline constexpr float spriteUvMaximum = .9999f;
inline constexpr float defaultReachMeters = 2.1f, maximumReachMeters = 10;
inline constexpr float defaultInspectScale = 1.5f, maximumInspectScale = 100;
inline constexpr float maximumLightIntensity = 100, maximumLightRangeMeters = 1000,
                       maximumAttenuationPerMeterSquared = 100;
inline constexpr float coordinateMeters = 10000, scaleMinimum = .001f, scaleMaximum = 1000;
inline constexpr size_t materialPixels = 32 * 1024 * 1024;
} // namespace sceneLimits
struct SceneAsset : Resource {
    MeshHandle mesh;
    std::shared_ptr<const ModelResource> model;
    SpriteLayout sprite;
    bool billboard = false;
};
struct SceneNode {
    std::string id, scene, room, parent, kind, label, visibleWhen, stateKey, actions, itemInstance;
    MeshTransform transform;
    float yaw = 0, reach = sceneLimits::defaultReachMeters, openAngle = 0,
          inspectScale = sceneLimits::defaultInspectScale;
    size_t pose = 0;
    int detail = 0, legacyDoor = -1, animation = -1;
    Box3 bounds;
    Vec3 openOffset;
    bool collidable = false, acoustic = false, shadow = true;
    std::shared_ptr<const SceneAsset> asset, activeAsset, inspection;
    std::vector<std::shared_ptr<const SceneAsset>> poses;
    std::optional<Light> light;
};
struct SceneRoom {
    std::string id, scene, label;
    Box3 bounds;
    float floorY = 0;
};
struct SceneSpawn {
    std::string id, scene;
    Camera camera;
};
struct SceneZone {
    std::string id, scene, targetSpawn;
    Box3 bounds;
};
struct SceneLight {
    std::string id, scene;
    Light light;
};
struct ScenePackage {
    std::shared_ptr<ResourceStore> store;
    std::vector<SceneNode> nodes;
    std::vector<SceneRoom> rooms;
    std::vector<SceneSpawn> spawns;
    std::vector<SceneZone> zones;
    std::vector<SceneLight> lights;
    std::map<std::string, std::shared_ptr<const SceneAsset>, std::less<>> assets;
    std::vector<std::string> diagnostics;
    std::vector<std::shared_ptr<const Resource>> dependencies;
    std::string entrySpawn;
    MaterialLibrary materials;
    explicit ScenePackage(std::vector<PaintedTexture> art) : materials(std::move(art)) {}
};
using SceneDocuments = std::map<std::filesystem::path, ContentValue>;
// Parsing, resources and all cross-file references are validated before returning a package.
[[nodiscard]] std::shared_ptr<ScenePackage>
loadScenes(const std::filesystem::path& root, std::vector<PaintedTexture> art,
           std::span<const std::string_view> materialNames,
           const std::filesystem::path& manifest = "scenes/world.dcworld",
           const SceneDocuments& documents = {});
[[nodiscard]] bool contains(const Box3& box, Vec3 point);
} // namespace paper
