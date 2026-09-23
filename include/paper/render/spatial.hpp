#pragma once
#include "paper/render/scene.hpp"
#include <map>

namespace paper {
[[nodiscard]] Box3 meshBounds(std::span<const Triangle3> mesh);
[[nodiscard]] Box3 worldBounds(const Box3& local, const MeshTransform& transform);
[[nodiscard]] Box3 unionBounds(const Box3& a, const Box3& b);
[[nodiscard]] bool inFrustum(const Box3& bounds, const Camera& camera, float aspect,
                             float nearPlaneMeters, float farPlaneMeters);
struct SpatialQueryStats {
    size_t boxes = 0, triangles = 0;
};
// Median-split BVH for immutable triangle resources. Tiny meshes stay in one leaf.
class MeshIndex {
  public:
    explicit MeshIndex(MeshHandle mesh);
    [[nodiscard]] MeshHit pick(Vec3 origin, Vec3 direction,
                               SpatialQueryStats* stats = nullptr) const;
    [[nodiscard]] const Box3& bounds() const { return nodes_.front().bounds; }

  private:
    struct Node {
        Box3 bounds;
        size_t first = 0, count = 0, left = 0, right = 0;
    };
    size_t build(size_t first, size_t count);
    MeshHandle mesh_;
    std::vector<size_t> order_;
    std::vector<Node> nodes_;
};
class MeshQueryCache {
  public:
    [[nodiscard]] MeshHit pick(const MeshInstance& instance, Vec3 origin, Vec3 direction,
                               SpatialQueryStats* stats = nullptr);
    void collect();

  private:
    std::map<MeshHandle, MeshIndex> indices_;
};
} // namespace paper
