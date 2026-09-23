#pragma once
#include "paper/core/units.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>

namespace paper {
inline constexpr float pi3 = std::numbers::pi_v<float>;
namespace geometryTolerance {
inline constexpr float normalizedLength = .00001f;
inline constexpr float quaternionLength = 1e-7f;
inline constexpr float parallelRayDirection = 1e-7f;
} // namespace geometryTolerance
namespace cameraDefaults {
inline constexpr float fieldOfViewDegrees = 74;
}
struct Vec2 {
    float x = 0, y = 0;
};
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(Vec3 b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec3 operator-(Vec3 b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return *this * (1.f / s); }
};
inline float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) {
    return std::sqrt(dot(a, a));
}
inline Vec3 normalized(Vec3 a) {
    float n = length(a);
    return n > geometryTolerance::normalizedLength ? a / n : Vec3{};
}
inline Vec3 rotateY(Vec3 a, float angle) {
    float c = std::cos(angle), s = std::sin(angle);
    return {a.x * c + a.z * s, a.y, -a.x * s + a.z * c};
}
// Unit quaternion for free object rotation: no Euler-angle lock or accumulated scaling.
struct Rotation3 {
    float w = 1, x = 0, y = 0, z = 0;
    Rotation3 operator*(Rotation3 b) const {
        return {w * b.w - x * b.x - y * b.y - z * b.z, w * b.x + x * b.w + y * b.z - z * b.y,
                w * b.y - x * b.z + y * b.w + z * b.x, w * b.z + x * b.y - y * b.x + z * b.w};
    }
    Rotation3 unit() const {
        const float n = std::sqrt(w * w + x * x + y * y + z * z);
        return n > geometryTolerance::quaternionLength ? Rotation3{w / n, x / n, y / n, z / n}
                                                       : Rotation3{};
    }
    Vec3 apply(Vec3 p) const {
        const Vec3 axis{x, y, z};
        const Vec3 twice = cross(axis, p) * 2;
        return p + twice * w + cross(axis, twice);
    }
    static Rotation3 axisAngle(Vec3 axis, float angle) {
        axis = normalized(axis) *
               std::sin(angle * .5f); // numbers: quaternions encode half the rotation angle.
        return {std::cos(angle * .5f), axis.x, axis.y,
                axis.z}; // numbers: quaternions encode half the rotation angle.
    }
};
struct Camera {
    Vec3 position;
    float yaw = 0, pitch = 0, fov = units::radians(cameraDefaults::fieldOfViewDegrees);
    Vec3 forward() const {
        return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    }
    Vec3 right() const { return {std::cos(yaw), 0, -std::sin(yaw)}; }
    Vec3 up() const { return cross(forward(), right()); }
};
struct Box3 {
    Vec3 center, half;
    float yaw = 0;
};
// Slab intersection in the object's local frame. Returns nearest positive surface.
inline float rayBox(Vec3 origin, Vec3 direction, const Box3& box) {
    Vec3 o = rotateY(origin - box.center, -box.yaw), d = rotateY(direction, -box.yaw);
    const float ov[] = {o.x, o.y, o.z}, dv[] = {d.x, d.y, d.z},
                h[] = {box.half.x, box.half.y, box.half.z};
    float near = 0, far = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < std::size(ov); ++i) {
        if (std::abs(dv[i]) < geometryTolerance::parallelRayDirection) {
            if (ov[i] < -h[i] || ov[i] > h[i])
                return std::numeric_limits<float>::infinity();
        } else {
            float a = (-h[i] - ov[i]) / dv[i], b = (h[i] - ov[i]) / dv[i];
            if (a > b)
                std::swap(a, b);
            near = std::max(near, a);
            far = std::min(far, b);
            if (near > far)
                return std::numeric_limits<float>::infinity();
        }
    }
    return far < 0 ? std::numeric_limits<float>::infinity() : near;
}
inline bool circleOverlaps(Vec3 feet, float radius, const Box3& box, float minimumHeightMeters,
                           float maximumHeightMeters) {
    if (box.center.y + box.half.y < minimumHeightMeters ||
        box.center.y - box.half.y > maximumHeightMeters)
        return false;
    Vec3 p = rotateY(feet - box.center, -box.yaw);
    float dx = p.x - std::clamp(p.x, -box.half.x, box.half.x);
    float dz = p.z - std::clamp(p.z, -box.half.z, box.half.z);
    return dx * dx + dz * dz < radius * radius;
}
} // namespace paper
