#include "paper/core/pixel_format.hpp"
#include "paper/render/scene.hpp"
#include <algorithm>
#include <cmath>

namespace paper {
namespace {
constexpr float parallelTriangleDeterminant = 1e-8f;
constexpr float minimumHitDistanceMeters = .001f;
constexpr float minimumBoxUvExtent = .05f;
} // namespace
MeshHit pickMesh(std::span<const Triangle3> mesh, Vec3 origin, Vec3 direction) {
    MeshHit nearest;
    for (const auto& triangle : mesh) {
        const Vec3 a = triangle.v[0].p, ab = triangle.v[1].p - a, ac = triangle.v[2].p - a;
        const Vec3 h = cross(direction, ac);
        const float determinant = dot(ab, h);
        if (std::abs(determinant) < parallelTriangleDeterminant)
            continue;
        const float inverse = 1 / determinant;
        const Vec3 s = origin - a;
        const float u = dot(s, h) * inverse;
        if (u < 0 || u > 1)
            continue;
        const Vec3 q = cross(s, ab);
        const float v = dot(direction, q) * inverse;
        if (v < 0 || u + v > 1)
            continue;
        const float distance = dot(ac, q) * inverse;
        if (distance > minimumHitDistanceMeters && distance < nearest.distance)
            nearest = {distance, triangle.detail};
    }
    return nearest;
}
bool PaintedTexture::valid() const noexcept {
    return width > 0 && height > 0 &&
           pixels.size() / static_cast<size_t>(width) == static_cast<size_t>(height) &&
           pixels.size() % static_cast<size_t>(width) == 0 &&
           (alteredFace.empty() ||
            (alteredFace.size() == pixels.size() && std::isfinite(faceCenter.x) &&
             std::isfinite(faceCenter.y) && std::isfinite(faceRadius.x) &&
             std::isfinite(faceRadius.y) && faceRadius.x > 0 && faceRadius.y > 0));
}
Pixel PaintedTexture::sample(float u, float v) const noexcept {
    constexpr Pixel missingTexture{255, 0, 255, 255};
    if (!valid())
        return missingTexture;
    u -= std::floor(u);
    v -= std::floor(v);
    int x = std::clamp(static_cast<int>(u * width), 0, width - 1),
        y = std::clamp(static_cast<int>(v * height), 0, height - 1);
    return pixels[static_cast<size_t>(y * width + x)];
}
Pixel PaintedTexture::perceive(Pixel original, float u, float v, float amount, float ripple,
                               const RenderStyle& style) const noexcept {
    if (alteredFace.empty() || amount <= 0 || original.a < pixelFormat::opaqueAlpha)
        return original;
    const float dx = (u - faceCenter.x) / faceRadius.x;
    const float dy = (v - faceCenter.y) / faceRadius.y;
    const float mask = std::clamp((1 - dx * dx - dy * dy) * style.faceFeather, 0.f, 1.f);
    if (mask <= 0)
        return original;
    // The painted anatomy persists between disturbances. A rare, tiny ripple is secondary.
    const float shiftedU = u + std::sin(dy * style.rippleSpatialFrequency) * ripple * mask;
    const int x = std::clamp(static_cast<int>(shiftedU * width), 0, width - 1);
    const int y = std::clamp(static_cast<int>(v * height), 0, height - 1);
    const Pixel other = alteredFace[static_cast<size_t>(y) * width + x];
    if (other.a < pixelFormat::opaqueAlpha)
        return original;
    const float blend = mask * std::clamp(amount, 0.f, 1.f);
    const auto channel = [blend](std::uint8_t from, std::uint8_t to) {
        return static_cast<std::uint8_t>(from + (static_cast<float>(to) - from) * blend + .5f);
    };
    return {channel(original.r, other.r), channel(original.g, other.g),
            channel(original.b, other.b), original.a};
}
void quad(Mesh3& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d, MaterialId mat, float u, float v) {
    m.push_back(Triangle3{{Vertex3{a, {0, 0}}, Vertex3{b, {u, 0}}, Vertex3{c, {u, v}}}, mat});
    m.push_back(Triangle3{{Vertex3{a, {0, 0}}, Vertex3{c, {u, v}}, Vertex3{d, {0, v}}}, mat});
}
void boxMesh(Mesh3& m, const Box3& b, MaterialId mat, float scale) {
    const auto firstTriangle = m.size();
    Vec3 h = b.half;
    const auto p = [&](float x, float y, float z) { return b.center + rotateY({x, y, z}, b.yaw); };
    float u = std::max(minimumBoxUvExtent, h.x * 2 * scale),
          v = std::max(minimumBoxUvExtent, h.y * 2 * scale),
          w = std::max(minimumBoxUvExtent, h.z * 2 * scale);
    quad(m, p(-h.x, h.y, -h.z), p(h.x, h.y, -h.z), p(h.x, -h.y, -h.z), p(-h.x, -h.y, -h.z), mat, u,
         v);
    quad(m, p(h.x, h.y, h.z), p(-h.x, h.y, h.z), p(-h.x, -h.y, h.z), p(h.x, -h.y, h.z), mat, u, v);
    quad(m, p(-h.x, h.y, h.z), p(-h.x, h.y, -h.z), p(-h.x, -h.y, -h.z), p(-h.x, -h.y, h.z), mat, w,
         v);
    quad(m, p(h.x, h.y, -h.z), p(h.x, h.y, h.z), p(h.x, -h.y, h.z), p(h.x, -h.y, -h.z), mat, w, v);
    quad(m, p(-h.x, h.y, h.z), p(h.x, h.y, h.z), p(h.x, h.y, -h.z), p(-h.x, h.y, -h.z), mat, u, w);
    quad(m, p(-h.x, -h.y, -h.z), p(h.x, -h.y, -h.z), p(h.x, -h.y, h.z), p(-h.x, -h.y, h.z), mat, u,
         w);
    // All six box faces wind outward. Loose quads and resident sprites stay two-sided.
    for (auto i = firstTriangle; i < m.size(); ++i)
        m[i].twoSided = false;
}
void transformedMesh(Mesh3& dst, const Mesh3& source, Vec3 origin, float yaw) {
    for (auto t : source) {
        for (auto& v : t.v)
            v.p = origin + rotateY(v.p, yaw);
        dst.push_back(t);
    }
}
} // namespace paper
