#pragma once
#include "paper/core/detail_tag.hpp"
#include "paper/core/pixel_format.hpp"
#include "paper/render/lighting.hpp"
#include "paper/render/particles.hpp"
#include "paper/render/style.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace paper {
struct Pixel {
    std::uint8_t r = 0, g = 0, b = 0, a = pixelFormat::maximumChannel;
};
enum class MaterialId : std::uint16_t {};
// Transparent top/bottom margins are excluded when placing a sprite on the floor.
// Horizontal canvas center stays fixed so an outstretched hand cannot move its pivot.
struct SpriteLayout {
    float top = 0, bottom = 1, widthOverHeight = 2.f / 3.f;
};
struct PaintedTexture {
    int width = 128, height = 128;
    std::vector<Pixel> pixels;
    bool emissive = false;
    // An optional painted alternate face on the same canvas. The original owns all alpha.
    std::vector<Pixel> alteredFace;
    Vec2 faceCenter{.5f, .16f}, faceRadius{.18f, .12f};
    [[nodiscard]] Pixel sample(float u, float v) const noexcept;
    // Requires a valid texture; amount/ripple are prepared once per rendered frame.
    [[nodiscard]] Pixel perceive(Pixel original, float u, float v, float amount, float ripple,
                                 const RenderStyle& style) const noexcept;
    [[nodiscard]] bool valid() const noexcept;
};
struct Vertex3 {
    Vec3 p;
    Vec2 uv;
};
struct Triangle3 {
    std::array<Vertex3, 3> v;
    MaterialId material{};
    bool twoSided = true;
    DetailTag detail = DetailTag::None;
};
using Mesh3 = std::vector<Triangle3>;
struct MeshHit {
    float distance = std::numeric_limits<float>::infinity();
    DetailTag detail = DetailTag::None;
};
[[nodiscard]] MeshHit pickMesh(std::span<const Triangle3> mesh, Vec3 origin, Vec3 direction);
void quad(Mesh3& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 d, MaterialId mat, float u = 1, float v = 1);
void boxMesh(Mesh3& mesh, const Box3& box, MaterialId mat, float scale = 1);
void transformedMesh(Mesh3& dst, const Mesh3& source, Vec3 origin, float yaw);

struct RenderOptions {
    float elapsedSeconds = 0;
    std::span<const Light> lights;
    ShadowSettings shadows;
    float emissiveScale = 1;
    float aspect = 1.6f;
    float distortion = 0;
    std::span<const Particle> particles;
};

// Immutable geometry is shared by instances and uploaded once per resource lifetime.
using MeshHandle = std::shared_ptr<const Mesh3>;
[[nodiscard]] inline MeshHandle makeMesh(Mesh3 mesh) {
    return std::make_shared<const Mesh3>(std::move(mesh));
}
struct MeshTransform {
    Vec3 position;
    Rotation3 rotation;
    float scale = 1;
    [[nodiscard]] Vec3 point(Vec3 value) const {
        return position + rotation.unit().apply(value * scale);
    }
};
struct MeshInstance {
    MeshHandle mesh;
    MeshTransform transform;
    bool castsShadow = true;
    std::string room{};
    bool foreground = false; // Camera-attached meshes get a separate depth layer.
};
using RenderScene = std::span<const MeshInstance>;
[[nodiscard]] MeshHit pickScene(RenderScene scene, Vec3 origin, Vec3 direction);

// CPU-side resource data; contains no renderer, SDL handles or framebuffer.
class MaterialLibrary {
  public:
    explicit MaterialLibrary(std::vector<PaintedTexture> textures);
    [[nodiscard]] const PaintedTexture& art(MaterialId id) const {
        return textures_.at(static_cast<unsigned>(id));
    }
    MaterialId append(PaintedTexture texture);
    void setArt(MaterialId id, PaintedTexture texture);
    [[nodiscard]] size_t size() const noexcept { return textures_.size(); }
    [[nodiscard]] std::uint64_t revision(MaterialId id) const {
        return revisions_.at(static_cast<unsigned>(id));
    }

  private:
    std::vector<PaintedTexture> textures_;
    std::vector<std::uint64_t> revisions_;
};
} // namespace paper
