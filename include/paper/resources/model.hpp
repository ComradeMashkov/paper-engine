#pragma once
#include "paper/resources/resource_store.hpp"

namespace paper {
struct SkinVertex {
    // glTF JOINTS_0/WEIGHTS_0: up to four influences per vertex.
    std::array<size_t, 4> joints{}; // numbers: glTF skin attribute layout.
    std::array<float, 4> weights{}; // numbers: glTF skin attribute layout.
};
struct ModelSkin {
    std::vector<size_t> joints;
    std::vector<std::array<float, 16>> inverseBind; // numbers: column-major glTF MAT4.
};
struct ModelNode {
    std::string name;
    int parent = -1;
    MeshTransform local;
    std::vector<MeshHandle> meshes;
    std::vector<size_t> children;
    int skin = -1;
    std::vector<std::vector<SkinVertex>> influences;
};
enum class AnimationPath { Translation, Rotation, Scale };
struct AnimationTrack {
    size_t node = 0;
    AnimationPath path = AnimationPath::Translation;
    bool step = false;
    std::vector<float> seconds;
    std::vector<std::array<float, 4>>
        values; // numbers: glTF channels store XYZW (XYZ with padding for translation).
};
struct ModelAnimation {
    std::string name;
    float durationSeconds = 0;
    std::vector<AnimationTrack> tracks;
};
// Additional absolute channels, applied after the main clip before hierarchy/skin evaluation.
struct ModelAnimationLayer {
    int animation = -1;
    float seconds = 0;
};
struct ModelResource final : Resource {
    std::vector<ModelNode> nodes;
    std::vector<size_t> roots;
    std::vector<std::string> diagnostics;
    std::vector<PaintedTexture> materials;
    std::vector<ModelAnimation> animations;
    std::vector<ModelSkin> skins;
    // Material IDs in imported meshes are local to this model.
    [[nodiscard]] std::vector<MeshInstance> sample(float seconds = 0, int animation = -1,
                                                   bool loop = true) const;
    [[nodiscard]] std::vector<MeshTransform>
    transforms(float seconds = 0, int animation = -1, bool loop = true,
               std::span<const float> localScales = {},
               std::span<const ModelAnimationLayer> layers = {}) const;
    [[nodiscard]] std::vector<MeshInstance> sample(std::span<const MeshTransform> poses) const;
};
[[nodiscard]] MeshTransform compose(const MeshTransform& parent, const MeshTransform& child);
[[nodiscard]] MeshTransform blend(const MeshTransform& from, const MeshTransform& to, float amount);
[[nodiscard]] std::unique_ptr<ModelResource> loadGltf(ResourceStore& store,
                                                      const std::filesystem::path& relative);
} // namespace paper
