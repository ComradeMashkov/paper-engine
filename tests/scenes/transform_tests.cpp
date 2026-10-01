#include "paper/authoring/document.hpp"
#include "paper/render/gpu_data.hpp"
#include "paper/render/spatial.hpp"
#include "paper/resources/model.hpp"
#include "paper/scenes/transforms.hpp"
#include <iostream>
using namespace paper;
int main() {
    int failed = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) {
            ++failed;
            std::cerr << message << '\n';
        }
    };
    const auto near = [](Vec3 a, Vec3 b) { return length(a - b) < .0001f; };
    const auto rejects = [&](auto operation) {
        try {
            operation();
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    MeshTransform t{{1, 2, 3}, Rotation3::axisAngle({1, 0, 0}, pi3 / 2), 1, {2, 3, 4}};
    check(near(t.point({1, 1, 1}), {3, -2, 6}), "rotation and anisotropic scale use world metres");
    check(near(t.inversePoint({3, -2, 6}), {1, 1, 1}),
          "inverse point restores authored coordinates");
    const auto normal = t.normal({1, 1, 0});
    check(near(normal, normalized(Vec3{.5f, 0, 1.f / 3})),
          "normal uses inverse transpose, not point transform");
    MeshTransform p{{1, 0, 0}, {}, 1, {2, 1, 1}},
        c{{0, 1, 0}, Rotation3::axisAngle({0, 0, 1}, pi3 / 4)};
    const auto world = compose(p, c);
    check(near(world.point({1, 0, 0}), {1 + std::sqrt(2.f), 1 + std::sqrt(.5f), 0}),
          "rotated child retains parent shear");
    const auto local = relativeTransform(p, world);
    check(near(compose(p, local).point({-2, 3, 4}), world.point({-2, 3, 4})),
          "reparent preserves exact affine geometry");
    check(near(readTransform(writeTransform(world)).point({1, 2, 3}), world.point({1, 2, 3})),
          "transform wire round trip retains shear");
    const auto packed = gpu::modelUniforms(t);
    check(near({packed.x.x, packed.x.y, packed.x.z}, {2, 0, 0}) &&
              near({packed.z.x, packed.z.y, packed.z.z}, {0, -4, 0}),
          "GPU matrix uses exact basis columns");
    Mesh3 mesh;
    quad(mesh, {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, MaterialId{});
    MeshInstance instance{makeMesh(mesh),
                          {{0, 0, 5}, Rotation3::axisAngle({0, 1, 0}, .4f), 1, {2, 3, 4}}};
    const auto hit = pickScene(std::span(&instance, 1), {}, {0, 0, 1});
    check(std::abs(hit.distance - 5) < .0001f, "anisotropic picking reports world ray parameter");
    const auto bounds = worldBounds({{}, {1, 1, 1}}, t);
    check(near(bounds.center, {1, 2, 3}) && near(bounds.half, {2, 4, 3}),
          "culling bounds enclose free rotation");
    Box3 box{{0, 0, 0}, {2, 1, 1}, 0, Rotation3::axisAngle({0, 0, 1}, pi3 / 2)};
    check(std::abs(rayBox({0, -4, 0}, {0, 1, 0}, box) - 2) < .0001f,
          "oriented collision/acoustic ray box rotates all axes");
    auto bad = t;
    bad.scaleAxes.y = 0;
    check(rejects([&] { (void)gpu::modelUniforms(bad); }),
          "zero scale rejected before GPU publication");
    bad = t;
    bad.rotation = {0, 0, 0, 0};
    check(!bad.valid(), "zero quaternion is invalid");
    check(rejects([&] { (void)readRotation(ContentValue::array({0, 0, 0, 0})); }),
          "zero wire quaternion rejected");
    authoring::SceneDocument doc("fixture.dcscene", R"(format="dcmo.scene"
version=3
[scene]
id="test"
[[scene.nodes]]
id="box"
rotation=[0,0,0,1]
scale=[1,2,3]
)");
    doc.setProperty("box", "rotation", ContentValue::array({0, .70710678, 0, .70710678}));
    doc.setProperty("box", "basis", writeTransform(world).at("basis"));
    authoring::SceneDocument again("fixture.dcscene", doc.serialized());
    check(again.data() == doc.data(), "authoring retains quaternion, axes and basis through Save");
    std::cout << "transform checks completed; failures=" << failed << '\n';
    return failed ? 1 : 0;
}
