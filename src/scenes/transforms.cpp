#include "paper/scenes/transforms.hpp"
#include "paper/scenes/scene.hpp"
#include <stdexcept>
namespace paper {
namespace {
float scalar(const ContentValue& v) {
    if (!v.is_number() || !std::isfinite(v.get<float>()))
        throw std::invalid_argument("Transform requires finite numeric values");
    return v.get<float>();
}
Vec3 vector(const ContentValue& v) {
    constexpr size_t components = 3;
    if (!v.is_array() || v.size() != components)
        throw std::invalid_argument("Expected XYZ vector");
    return {scalar(v[0]), scalar(v[1]), scalar(v[2])}; // numbers: XYZ wire layout.
}
ContentValue xyz(Vec3 v) {
    return ContentValue::array({v.x, v.y, v.z});
}
} // namespace
Rotation3 readRotation(const ContentValue& v) {
    constexpr size_t components = 4;
    if (!v.is_array() || v.size() != components)
        throw std::invalid_argument("Expected XYZW quaternion");
    Rotation3 q{scalar(v[3]), scalar(v[0]), scalar(v[1]),
                scalar(v[2])}; // numbers: XYZW wire layout.
    const float magnitude = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (!std::isfinite(magnitude) || magnitude <= geometryTolerance::quaternionLength)
        throw std::invalid_argument("Zero/overflow quaternion");
    return q.unit();
}
MeshTransform readTransform(const ContentValue& n) {
    if (!n.is_object())
        throw std::invalid_argument("Expected transform object");
    MeshTransform t;
    if (n.contains("position"))
        t.position = vector(n.at("position"));
    if (n.contains("rotation")) {
        if (n.contains("yaw"))
            throw std::invalid_argument("Use rotation or yaw, never both");
        t.rotation = readRotation(n.at("rotation"));
    } else
        t.rotation = Rotation3::axisAngle({0, 1, 0}, scalar(n.value("yaw", ContentValue(0))));
    const auto s = n.value("scale", ContentValue(1));
    if (s.is_array())
        t.scaleAxes = vector(s);
    else
        t.scale = scalar(s);
    if (n.contains("basis")) {
        const auto& b = n.at("basis");
        constexpr size_t columns = 3;
        if (!b.is_array() || b.size() != columns)
            throw std::invalid_argument("Expected three basis columns");
        t.basis = {vector(b[0]), vector(b[1]), vector(b[2])}; // numbers: matrix column wire layout.
    }
    for (float v : {t.scale * t.scaleAxes.x, t.scale * t.scaleAxes.y, t.scale * t.scaleAxes.z})
        if (v < sceneLimits::scaleMinimum || v > sceneLimits::scaleMaximum)
            throw std::invalid_argument("Scale outside scene limits");
    if (!t.valid() || std::abs(t.position.x) > sceneLimits::coordinateMeters ||
        std::abs(t.position.y) > sceneLimits::coordinateMeters ||
        std::abs(t.position.z) > sceneLimits::coordinateMeters)
        throw std::invalid_argument("Invalid scene transform");
    const auto m = t.linear();
    for (const auto axis : {m.x, m.y, m.z})
        if (length(axis) < sceneLimits::scaleMinimum || length(axis) > sceneLimits::scaleMaximum)
            throw std::invalid_argument("Basis outside scene scale limits");
    return t;
}
ContentValue writeTransform(const MeshTransform& t) {
    if (!t.valid())
        throw std::invalid_argument("Invalid scene transform");
    const auto q = t.rotation.unit();
    auto v = ContentValue::object();
    v["position"] = xyz(t.position);
    v["rotation"] = ContentValue::array({q.x, q.y, q.z, q.w});
    v["scale"] = xyz(t.scaleAxes * t.scale);
    v["basis"] = ContentValue::array({xyz(t.basis.x), xyz(t.basis.y), xyz(t.basis.z)});
    (void)readTransform(v);
    return v;
}
MeshTransform relativeTransform(const MeshTransform& parent, const MeshTransform& world) {
    if (!parent.valid() || !world.valid())
        throw std::invalid_argument("Invalid relative transform");
    MeshTransform result;
    result.position = parent.inversePoint(world.position);
    result.rotation = (parent.rotation.inverse() * world.rotation).unit();
    const auto matrix = parent.linear().inverse() * world.linear();
    const auto q = result.rotation.inverse();
    result.scaleAxes = {length(matrix.x), length(matrix.y), length(matrix.z)};
    if (!finite3(result.scaleAxes) || result.scaleAxes.x <= 0 || result.scaleAxes.y <= 0 ||
        result.scaleAxes.z <= 0)
        throw std::invalid_argument("Degenerate relative transform");
    result.basis = {q.apply(matrix.x) / result.scaleAxes.x, q.apply(matrix.y) / result.scaleAxes.y,
                    q.apply(matrix.z) / result.scaleAxes.z};
    if (!result.valid())
        throw std::invalid_argument("Invalid relative transform");
    return result;
}
} // namespace paper
