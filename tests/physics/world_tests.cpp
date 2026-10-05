#include "paper/content/document.hpp"
#include "paper/physics/scene.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
using namespace paper;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        if (!ok) {
            ++failures;
            std::cerr << text << '\n';
        }
    };
    auto rejects = [&](auto f) {
        try {
            f();
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    auto floor = [](PhysicsWorld& w) { return w.addBox({{0, -.5f, 0}, {10, .5f, 10}}); };
    constexpr float dt = 1.f / 60;
    {
        PhysicsWorld w;
        const auto support = floor(w);
        const auto ceiling = w.addBox({{0, 1.5f, 0}, {1, .1f, 1}}, {}, {2, 4});
        check(!w.overlapsCapsule({}, .22f, 1.2f, {4, 3}),
              "crouched placement permits touching support and clears a low ceiling");
        check(w.overlapsCapsule({}, .22f, 1.8f, {4, 3}),
              "standing placement detects head penetration");
        check(!w.overlapsCapsule({}, .22f, 1.8f, {4, 1}),
              "placement respects symmetric category/mask filtering");
        check(!w.overlapsCapsule({}, .22f, 1.8f, {4, 3}, support) &&
                  w.overlapsCapsule({}, .22f, 1.8f, {4, 3}, ceiling),
              "placement can isolate a moving collider without including other contacts");
        w.move(ceiling, {5, 0, 0});
        check(!w.overlapsCapsule({}, .22f, 1.8f, {4, 3}, ceiling),
              "placement uses current collider pose");
        w.remove(ceiling);
        check(rejects([&] { (void)w.overlapsCapsule({}, .22f, 1.8f, {4, 3}, ceiling); }),
              "placement rejects stale collider identifiers");
        check(rejects([&] { (void)w.overlapsCapsule({}, .3f, .4f); }) &&
                  rejects([&] { (void)w.overlapsCapsule({}, .3f, 1.8f, {}, {}, -.1f); }),
              "placement rejects invalid dimensions and tolerance before dependency calls");
    }
    {
        PhysicsWorld w;
        floor(w);
        CharacterController c(w, {0, 0, 0});
        for (int k = 0; k < 60; ++k)
            c.update({{2, 0, 0}}, dt);
        check(c.state().feet.x > 1.9f && c.state().feet.x < 2.1f && c.state().grounded,
              "flat-floor movement and support");
        c.update({{}, false, true}, dt);
        check(c.state().velocity.y > 0 && !c.state().grounded,
              "jump leaves support without floor snapping");
        for (int k = 0; k < 120; ++k)
            c.update({}, dt);
        check(c.state().grounded && c.state().feet.y < .05f, "gravity returns to floor");
    }
    {
        PhysicsWorld w;
        floor(w);
        w.addBox({{0, 1, 1}, {2, 1, .01f}});
        CharacterController c(w, {});
        c.update({{0, 0, 100}}, dt);
        check(c.state().feet.z < .71f,
              "continuous capsule collision prevents thin-wall tunnelling");
    }
    {
        PhysicsWorld w;
        floor(w);
        for (int k = 0; k < 5; ++k)
            w.addBox({{0, .1f * (k + 1), 1.f + k * .6f}, {1, .1f * (k + 1), .3f}});
        CharacterController c(w, {0, 0, 0});
        for (int k = 0; k < 75; ++k)
            c.update({{0, 0, 2}}, dt);
        check(c.state().feet.z > 1.8f && c.state().feet.y > .35f,
              "controller climbs a sequence of steps");
        std::cout << "stairs feet=" << c.state().feet.y << "," << c.state().feet.z << '\n';
    }
    {
        PhysicsWorld w;
        Mesh3 ramp;
        quad(ramp, {-2, 0, 0}, {-2, 2, 4}, {2, 2, 4}, {2, 0, 0}, MaterialId{});
        w.addMesh(ramp);
        CharacterController c(w, {0, .25f, .5f});
        for (int k = 0; k < 60; ++k)
            c.update({{0, 0, 2}}, dt);
        check(c.state().feet.z > 1.7f && c.state().feet.y > .7f && c.state().grounded,
              "walkable slope maintains ground support");
        std::cout << "ramp feet=" << c.state().feet.y << "," << c.state().feet.z << '\n';
    }
    {
        PhysicsWorld w;
        floor(w);
        w.addBox({{0, 1.6f, 2}, {2, .3f, 1.5f}});
        CharacterController c(w, {0, 0, -1});
        for (int k = 0; k < 90; ++k)
            c.update({{0, 0, 2}, true}, dt);
        c.update({}, dt);
        check(c.state().standingBlocked && c.state().crouched,
              "standing checks entire head clearance");
        for (int k = 0; k < 90; ++k)
            c.update({{0, 0, 2}, true}, dt);
        c.update({}, dt);
        check(!c.state().crouched && !c.state().standingBlocked,
              "standing resumes after leaving ceiling");
    }
    {
        PhysicsWorld w;
        floor(w);
        const auto id = w.addBox({{0, 1, 1}, {1, 1, .1f}}, {}, {2, 2});
        check(!w.raycast({0, 1, -1}, {0, 0, 1}, 4, {1, 2}), "mutual category and mask filtering");
        auto hit = w.raycast({0, 1, -1}, {0, 0, 1}, 4, {2, 2});
        check(hit && hit->collider == id && std::abs(hit->distance - 1.9f) < .01f,
              "filtered ray returns stable collider and world distance");
        CharacterSettings settings;
        settings.filter = {1, 1};
        CharacterController c(w, {}, settings);
        for (int k = 0; k < 60; ++k)
            c.update({{0, 0, 2}}, dt);
        check(c.state().feet.z > 1.9f, "character ignores excluded layers");
        w.remove(id);
        check(!w.raycast({0, 1, -1}, {0, 0, 1}, 4, {2, 2}),
              "removed collider is no longer queried");
        check(rejects([&] { w.remove(id); }), "stale collider ID is rejected");
        bool threadRejected = false;
        std::thread worker(
            [&] { threadRejected = rejects([&] { (void)w.raycast({}, {0, 0, 1}, 1); }); });
        worker.join();
        check(threadRejected, "world enforces owner-thread contract");
    }
    {
        PhysicsWorld w;
        check(rejects([&] {
                  CharacterSettings s;
                  s.radiusMeters = 0;
                  CharacterController c(w, {}, s);
              }),
              "invalid capsule rejected");
        CharacterController c(w, {});
        check(rejects([&] { c.update({}, 1); }), "unbounded simulation interval rejected");
        MeshTransform t{{1, 0, 0}, Rotation3::axisAngle({0, 0, 1}, pi3 / 2), 1, {2, 1, 1}};
        const auto id = w.addBox({{}, {1, .5f, .5f}}, t);
        const auto hit = w.raycast({1, -4, 0}, {0, 1, 0}, 10);
        check(hit && hit->collider == id && std::abs(hit->distance - 2) < .02f,
              "physics matches free scene rotation and axis scale");
    }
    {
        PhysicsWorld w;
        floor(w);
        w.addBox({{0, 1, 1}, {2, 1, .1f}});
        CharacterController c(w, {});
        for (int k = 0; k < 60; ++k)
            c.update({{1, 0, 2}}, dt);
        check(c.state().feet.z < .65f && c.state().feet.x > .8f,
              "capsule slides along a wall without climbing a tall ledge");
        CharacterSettings pathological;
        pathological.maximumSubstepSeconds = 1e-30f;
        check(rejects([&] { CharacterController bad(w, {}, pathological); }),
              "substep count cannot overflow");
        check(rejects([&] { w.addBox({{}, {1, 1, 1}, 0, {0, 0, 0, 0}}); }),
              "invalid local collider quaternion rejected");
    }
    {
        PhysicsWorld w;
        floor(w);
        Mesh3 ramp;
        quad(ramp, {-2, 0, 0}, {-2, 4, 2}, {2, 4, 2}, {2, 0, 0}, MaterialId{});
        w.addMesh(ramp);
        CharacterController c(w, {0, 0, -.5f});
        for (int k = 0; k < 90; ++k)
            c.update({{0, 0, 2}}, dt);
        check(c.state().feet.z < .5f && c.state().feet.y < .5f,
              "slope above authored limit cannot be walked up");
    }
    {
        PaintedTexture material;
        material.width = material.height = 1;
        material.pixels.push_back({});
        ScenePackage package({material});
        SceneNode n;
        n.id = "box";
        n.scene = "room";
        n.collidable = true;
        n.localBounds = {{}, {1, 1, 1}};
        n.transform = {{0, 1, 3}, {}, 1, {2, 1, 1}};
        n.collisionCategory = 4;
        n.collisionMask = 4;
        package.nodes.push_back(n);
        PhysicsWorld w;
        const auto colliders = addSceneColliders(w, package, "room");
        const auto hit = w.raycast({0, 1, 0}, {0, 0, 1}, 10, {4, 4});
        check(hit && hit->collider == colliders.at("box") && std::abs(hit->distance - 2) < .01f,
              "scene collision binding retains shape, pose and layers");
        auto invalid = n;
        invalid.id = "invalid";
        invalid.localBounds.half.x = 0;
        package.nodes.push_back(invalid);
        check(rejects([&] { (void)addSceneColliders(w, package, "room"); }),
              "scene collider installation rejects invalid shape");
        w.remove(colliders.at("box"));
        check(!w.raycast({0, 1, 0}, {0, 0, 1}, 10, {4, 4}),
              "failed scene installation rolls back all newly added colliders");
        PhysicsWorld tiny({1, 1, 1});
        tiny.addBox({{}, {1, 1, 1}});
        check(rejects([&] { tiny.addBox({{}, {1, 1, 1}}); }), "body capacity fails explicitly");
    }
    {
        PaintedTexture material;
        material.width = material.height = 1;
        material.pixels.push_back({});
        const std::array<std::string_view, 1> names{"neutral"};
        const std::filesystem::path root = PAPER_EXAMPLE_ASSETS;
        auto document = content::read(root / "boxes.dcscene");
        document["version"] = 3;
        document["scene"]["nodes"][0]["collision"] =
            ContentValue{{"shape", "mesh"}, {"category", 4}, {"mask", 4}};
        const auto package =
            loadScenes(root, {material}, names, "world.dcworld", {{"boxes.dcscene", document}});
        PhysicsWorld w;
        const auto ids = addSceneColliders(w, *package, "boxes");
        check(ids.size() == 1 && w.raycast({0, 0, -2}, {0, 0, 1}, 4, {4, 4}).has_value(),
              "structured mesh collider survives native scene loader");
        document["version"] = 2;
        check(rejects([&] {
                  (void)loadScenes(root, {material}, names, "world.dcworld",
                                   {{"boxes.dcscene", document}});
              }),
              "v2 structured collisions require explicit migration");
    }
    {
        const auto copy =
            std::filesystem::temp_directory_path() /
            ("paper-template-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{copy};
        std::filesystem::copy(PAPER_EXAMPLE_ASSETS, copy, std::filesystem::copy_options::recursive);
        auto manifest = content::read(copy / "world.dcworld");
        manifest["version"] = 3;
        manifest["world"]["templates"] = ContentValue::array({"box.dctemplates"});
        auto scene = content::read(copy / "boxes.dcscene");
        scene["version"] = 3;
        scene["scene"]["nodes"][0]["template"] = "free.box";
        const auto templates = ContentValue{
            {"format", "dcmo.templates"},
            {"version", 3},
            {"templates",
             ContentValue{
                 {"templates", ContentValue::array({ContentValue{
                                   {"id", "free.box"},
                                   {"rotation", ContentValue::array({0, .70710678, 0, .70710678})},
                                   {"scale", ContentValue::array({2, 1, 1})}}})}}}};
        std::ofstream(copy / "box.dctemplates") << content::encode(templates);
        PaintedTexture material;
        material.width = material.height = 1;
        material.pixels.push_back({});
        const std::array<std::string_view, 1> names{"neutral"};
        const auto package = loadScenes(copy, {material}, names, "world.dcworld",
                                        {{"world.dcworld", manifest}, {"boxes.dcscene", scene}});
        const auto& box = *std::ranges::find(package->nodes, "box.parent", &SceneNode::id);
        check(length(box.transform.vector({1, 0, 0}) - Vec3{0, 0, -2}) < .0001f,
              "v3 templates retain quaternion and axis-scale inheritance");
    }
    {
        PhysicsWorld w;
        check(rejects([&] { w.addBox({{}, {1e30f, 1, 1}}); }),
              "overflowing collider geometry rejected before dependency calls");
    }
    std::cout << "physics failures=" << failures << '\n';
    return failures ? 1 : 0;
}
