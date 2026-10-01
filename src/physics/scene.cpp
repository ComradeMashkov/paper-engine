#include "paper/physics/scene.hpp"
namespace paper {
SceneColliders addSceneColliders(PhysicsWorld& world, const ScenePackage& package,
                                 std::string_view scene) {
    SceneColliders result;
    try {
        for (const auto& node : package.nodes)
            if (node.scene == scene && node.collidable) {
                if (result.contains(node.id))
                    throw std::invalid_argument("Duplicate collider node ID");
                const CollisionFilter filter{node.collisionCategory, node.collisionMask};
                ColliderId id;
                if (!node.meshCollision)
                    id = world.addBox(node.localBounds, node.transform, filter);
                else {
                    if (!node.asset || node.asset->billboard || node.animation >= 0)
                        throw std::invalid_argument("Mesh collider needs static geometry");
                    Mesh3 mesh;
                    if (node.asset->mesh)
                        mesh = *node.asset->mesh;
                    else if (node.asset->model) {
                        for (const auto& instance : node.asset->model->sample())
                            if (instance.mesh)
                                for (auto triangle : *instance.mesh) {
                                    for (auto& vertex : triangle.v)
                                        vertex.p = instance.transform.point(vertex.p);
                                    if (mesh.size() >= physicsLimits::triangles)
                                        throw std::invalid_argument(
                                            "Scene collider exceeds triangle budget");
                                    mesh.push_back(triangle);
                                }
                    }
                    id = world.addMesh(mesh, node.transform, filter);
                }
                try {
                    result.emplace(node.id, id);
                } catch (...) {
                    world.remove(id);
                    throw;
                }
            }
    } catch (...) {
        for (const auto& [node, id] : result)
            world.remove(id);
        throw;
    }
    return result;
}
} // namespace paper
