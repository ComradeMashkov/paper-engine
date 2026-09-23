#include "nlohmann/json.hpp"
#include "paper/resources/model.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace paper;
namespace {
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void rejects(F function, const char* message) {
    bool failed = false;
    try {
        function();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, message);
}
} // namespace
int main() {
    try {
        ResourceStore store(PAPER_TEST_FIXTURE_DIR);
        const auto model = store.model("animated.gltf");
        check(store.model("animated.gltf") == model, "model identity is cached");
        const auto glb = store.model("animated.glb");
        for (const auto& imported : {model, glb}) {
            check(imported->nodes.size() == 2 && imported->animations.size() == 1,
                  "nodes and animation imported");
            const auto half = imported->sample(.5f, 0, false);
            check(half.size() == 1 && half[0].mesh->size() == 1, "triangle topology preserved");
            check(length(half[0].transform.position - Vec3{1, 1, 0}) < 1e-5f,
                  "hierarchical linear animation sampled");
            const auto end = imported->sample(4, 0, false);
            check(length(end[0].transform.position - Vec3{1, 2, 0}) < 1e-5f,
                  "non-looping animation holds last key");
            const auto loop = imported->sample(1.5f, 0, true);
            check(length(loop[0].transform.position - half[0].transform.position) < 1e-5f,
                  "looping uses seconds");
            check(loop[0].mesh == half[0].mesh, "animation shares immutable geometry");
        }
        rejects([&] { (void)store.file("../outside"); }, "resource root traversal rejected");
        const auto skinned = store.model("skinned.gltf");
        const auto flexed = skinned->sample(.5f, 0, false);
        const auto& corners = flexed.at(0).mesh->at(0).v;
        check(length(corners[0].p - Vec3{1, 0, 0}) < 1e-5f &&
                  length(corners[1].p - Vec3{2, .5f, 0}) < 1e-5f &&
                  length(corners[2].p - Vec3{1, 2, 0}) < 1e-5f,
              "weighted joints preserve the wrist and bend the connected finger");
        check(length(flexed[0].transform.position) == 0 &&
                  length(skinned->sample(.5f, 0, false)[0].mesh->at(0).v[2].p - corners[2].p) == 0,
              "skin applies joint hierarchy once and sampling is immutable/deterministic");
        const auto rest = skinned->sample(0, 0, false);
        check(length(rest[0].mesh->at(0).v[2].p - Vec3{1, 1, 0}) < 1e-5f,
              "inverse bind preserves the authored rest shape");
        const std::array<float, 2> scales{.5f, 1};
        const auto heldScale = model->transforms(.5f, 0, false, scales);
        check(length(heldScale[1].position - Vec3{1, .5f, 0}) < 1e-5f && heldScale[1].scale == .5f,
              "scale overrides affect child sockets before hierarchy composition");
        const std::array layers{ModelAnimationLayer{0, .75f}};
        const auto layered = skinned->sample(skinned->transforms(.25f, 0, false, {}, layers));
        check(length(layered[0].mesh->at(0).v[2].p - Vec3{1, 2.5f, 0}) < 1e-5f,
              "parameter layer is evaluated before hierarchy and weighted skinning");
        const std::array invalidLayers{
            ModelAnimationLayer{0, std::numeric_limits<float>::quiet_NaN()}};
        rejects([&] { (void)skinned->transforms(0, 0, false, {}, invalidLayers); },
                "non-finite layer time rejected");
        check(!store.changed(), "unmodified dependency graph is stable");
        const auto bytes = store.stats().liveBytes;
        store.collect();
        check(store.stats().liveBytes == bytes, "live handles retain their dependencies");
        std::shared_ptr<const ModelResource> retained;
        {
            ResourceStore temporary(PAPER_TEST_FIXTURE_DIR);
            retained = temporary.model("animated.gltf");
        }
        check(retained->sample(.5f, 0).size() == 1, "handle survives store destruction");
        ResourceStore tiny(PAPER_TEST_FIXTURE_DIR, 8);
        rejects([&] { (void)tiny.file("animated.bin"); }, "file larger than budget rejected");
        const auto directory = std::filesystem::temp_directory_path() / "dcmo-resource-tests";
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "change.txt") << "old";
        ResourceStore watching(directory);
        auto snapshot = watching.file("change.txt");
        std::ofstream(directory / "change.txt") << "changed";
        check(watching.changed() && snapshot->data.size() == 3,
              "changed file does not mutate the live snapshot");
        snapshot.reset();
        watching.collect();
        check(watching.stats().liveBytes == 0,
              "unreferenced resources release their accounted bytes");
        std::filesystem::copy_file(std::filesystem::path(PAPER_TEST_FIXTURE_DIR) / "animated.bin",
                                   directory / "animated.bin",
                                   std::filesystem::copy_options::overwrite_existing);
        nlohmann::json original;
        std::ifstream(std::filesystem::path(PAPER_TEST_FIXTURE_DIR) / "animated.gltf") >> original;
        const auto invalid = [&](const nlohmann::json& value) {
            std::ofstream(directory / "invalid.gltf") << value.dump();
            ResourceStore isolated(directory);
            rejects([&] { (void)isolated.model("invalid.gltf"); },
                    "unsupported or malformed glTF is rejected");
        };
        auto bad = original;
        bad["extensionsUsed"] = {"KHR_draco_mesh_compression"};
        invalid(bad);
        bad = original;
        bad["nodes"][0]["scale"] = {1, 2, 1};
        invalid(bad);
        bad = original;
        bad["animations"][0]["samplers"][0]["interpolation"] = "CUBICSPLINE";
        invalid(bad);
        bad = original;
        bad["buffers"][0]["uri"] = "../outside.bin";
        invalid(bad);
        bad = original;
        bad["buffers"][0]["uri"] = "data:application/octet-stream;base64,A";
        invalid(bad);
        bad = original;
        bad["accessors"][0]["count"] = 100000000;
        invalid(bad);
        std::filesystem::copy_file(std::filesystem::path(PAPER_TEST_FIXTURE_DIR) / "skinned.bin",
                                   directory / "skinned.bin",
                                   std::filesystem::copy_options::overwrite_existing);
        nlohmann::json skinSource;
        std::ifstream(std::filesystem::path(PAPER_TEST_FIXTURE_DIR) / "skinned.gltf") >> skinSource;
        bad = skinSource;
        bad["skins"][0].erase("inverseBindMatrices");
        bad["skins"][0]["joints"] = {2};
        invalid(bad);
        bad = skinSource;
        bad["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
        invalid(bad);
        bad = skinSource;
        bad["skins"][0]["inverseBindMatrices"] = 0;
        invalid(bad);
        // A zero-sum vertex must fail before division or a GPU upload.
        const auto weightAccessor =
            skinSource["meshes"][0]["primitives"][0]["attributes"]["WEIGHTS_0"].get<size_t>();
        const auto weightView = skinSource["accessors"][weightAccessor]["bufferView"].get<size_t>();
        const auto weightOffset = skinSource["bufferViews"][weightView]["byteOffset"].get<size_t>();
        {
            std::fstream binary(directory / "skinned.bin",
                                std::ios::in | std::ios::out | std::ios::binary);
            const float zero = 0;
            binary.seekp(static_cast<std::streamoff>(weightOffset));
            binary.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
        }
        invalid(skinSource);
        std::filesystem::remove_all(directory);
        std::cout << "Resource and glTF checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
