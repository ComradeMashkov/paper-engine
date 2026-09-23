#include "paper/render/spatial.hpp"
#include <numeric>

namespace paper {
namespace {
inline constexpr size_t leafTriangles = 8;
inline constexpr float planarThicknessMeters = .00001f;
Box3 extents(Vec3 low, Vec3 high) {
    auto half = (high - low) * .5f;
    half.x = std::max(half.x, planarThicknessMeters);
    half.y = std::max(half.y, planarThicknessMeters);
    half.z = std::max(half.z, planarThicknessMeters);
    return {(low + high) * .5f, half}; // numbers: midpoint of the lower and upper box corners.
}
void include(Vec3 p, Vec3& low, Vec3& high) {
    low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
    high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
}
} // namespace
Box3 meshBounds(std::span<const Triangle3> mesh) {
    if (mesh.empty())
        return {{}, {planarThicknessMeters, planarThicknessMeters, planarThicknessMeters}};
    Vec3 low = mesh.front().v[0].p, high = low;
    for (const auto& triangle : mesh)
        for (const auto& vertex : triangle.v)
            include(vertex.p, low, high);
    return extents(low, high);
}
Box3 worldBounds(const Box3& local, const MeshTransform& transform) {
    Vec3 low = transform.point(local.center), high = low;
    for (float x : {-1.f, 1.f})
        for (float y : {-1.f, 1.f})
            for (float z : {-1.f, 1.f})
                include(transform.point(local.center + rotateY({x * local.half.x, y * local.half.y,
                                                                z * local.half.z},
                                                               local.yaw)),
                        low, high);
    return extents(low, high);
}
Box3 unionBounds(const Box3& a, const Box3& b) {
    Vec3 low = a.center - a.half, high = a.center + a.half;
    include(b.center - b.half, low, high);
    include(b.center + b.half, low, high);
    return extents(low, high);
}
bool inFrustum(const Box3& bounds, const Camera& camera, float aspect, float nearPlaneMeters,
               float farPlaneMeters) {
    // A conservative enclosing sphere avoids false negatives for rotated/animated bounds.
    const auto p = bounds.center - camera.position;
    const float radius = length(bounds.half), z = dot(p, camera.forward());
    if (z + radius < nearPlaneMeters || z - radius > farPlaneMeters)
        return false;
    const float vertical = std::tan(camera.fov * .5f), horizontal = vertical * aspect;
    return std::abs(dot(p, camera.right())) <=
               z * horizontal + radius * std::sqrt(1 + horizontal * horizontal) &&
           std::abs(dot(p, camera.up())) <=
               z * vertical + radius * std::sqrt(1 + vertical * vertical);
}
MeshIndex::MeshIndex(MeshHandle mesh) : mesh_(std::move(mesh)) {
    order_.resize(mesh_ ? mesh_->size() : 0);
    std::iota(order_.begin(), order_.end(), 0);
    build(0, order_.size());
}
size_t MeshIndex::build(size_t first, size_t count) {
    const auto index = nodes_.size();
    nodes_.push_back({});
    Box3 bounds{};
    for (size_t i = first; i < first + count; ++i) {
        const auto b = meshBounds(std::span(&(*mesh_)[order_[i]], 1));
        bounds = i == first ? b : unionBounds(bounds, b);
    }
    nodes_[index] = {bounds, first, count, 0, 0};
    if (count <= leafTriangles)
        return index;
    const int axis = bounds.half.x >= bounds.half.y && bounds.half.x >= bounds.half.z ? 0
                     : bounds.half.y >= bounds.half.z                                 ? 1
                                                                                      : 2;
    auto center = [&](size_t i) {
        const auto& t = (*mesh_)[i];
        const auto c = (t.v[0].p + t.v[1].p + t.v[2].p) / 3;
        return axis == 0 ? c.x : axis == 1 ? c.y : c.z;
    };
    const size_t middle = first + count / 2;
    std::nth_element(order_.begin() + first, order_.begin() + middle,
                     order_.begin() + first + count,
                     [&](size_t a, size_t b) { return center(a) < center(b); });
    const auto left = build(first, middle - first), right = build(middle, first + count - middle);
    nodes_[index].left = left;
    nodes_[index].right = right;
    nodes_[index].count = 0;
    return index;
}
MeshHit MeshIndex::pick(Vec3 origin, Vec3 direction, SpatialQueryStats* stats) const {
    MeshHit result;
    if (!mesh_ || mesh_->empty())
        return result;
    std::vector<size_t> pending{0};
    while (!pending.empty()) {
        const auto& n = nodes_[pending.back()];
        pending.pop_back();
        if (stats)
            ++stats->boxes;
        const float entry = rayBox(origin, direction, n.bounds);
        if (!std::isfinite(entry) || entry > result.distance)
            continue;
        if (n.count)
            for (size_t i = n.first; i < n.first + n.count; ++i) {
                if (stats)
                    ++stats->triangles;
                auto hit = pickMesh(std::span(&(*mesh_)[order_[i]], 1), origin, direction);
                if (hit.distance < result.distance)
                    result = hit;
            }
        else {
            pending.push_back(n.right);
            pending.push_back(n.left);
        }
    }
    return result;
}
MeshHit MeshQueryCache::pick(const MeshInstance& instance, Vec3 origin, Vec3 direction,
                             SpatialQueryStats* stats) {
    if (!instance.mesh || instance.transform.scale <= 0)
        return {};
    auto i = indices_.find(instance.mesh);
    if (i == indices_.end())
        i = indices_.try_emplace(instance.mesh, instance.mesh).first;
    const auto& t = instance.transform;
    const auto q = t.rotation.unit();
    const Rotation3 inverse{q.w, -q.x, -q.y, -q.z};
    auto hit = i->second.pick(inverse.apply(origin - t.position) / t.scale,
                              inverse.apply(direction), stats);
    hit.distance *= t.scale;
    return hit;
}
void MeshQueryCache::collect() {
    // One owner in the map key and one in the immutable index.
    constexpr long cacheOnlyOwners = 2;
    std::erase_if(indices_,
                  [](const auto& item) { return item.first.use_count() == cacheOnlyOwners; });
}
} // namespace paper
