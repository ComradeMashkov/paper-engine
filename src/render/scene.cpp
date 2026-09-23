#include "paper/render/scene.hpp"
#include "paper/render/spatial.hpp"
#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace paper {
namespace {
std::uint64_t nextRevision() {
    static std::atomic<std::uint64_t> revision{0};
    return ++revision;
}
} // namespace
MaterialLibrary::MaterialLibrary(std::vector<PaintedTexture> textures)
    : textures_(std::move(textures)) {
    if (textures_.empty() ||
        textures_.size() > std::numeric_limits<std::uint16_t>::max() + size_t{1} ||
        !std::ranges::all_of(textures_, &PaintedTexture::valid))
        throw std::invalid_argument("Materials require valid textures");
    for (size_t i = 0; i < textures_.size(); ++i)
        revisions_.push_back(nextRevision());
}
MaterialId MaterialLibrary::append(PaintedTexture texture) {
    if (!texture.valid() || textures_.size() > std::numeric_limits<uint16_t>::max())
        throw std::invalid_argument("Invalid or excessive scene materials");
    auto id = static_cast<MaterialId>(textures_.size());
    textures_.push_back(std::move(texture));
    revisions_.push_back(nextRevision());
    return id;
}
void MaterialLibrary::setArt(MaterialId id, PaintedTexture texture) {
    if (!texture.valid())
        throw std::invalid_argument("Invalid material texture");
    textures_.at(static_cast<unsigned>(id)) = std::move(texture);
    revisions_.at(static_cast<unsigned>(id)) = nextRevision();
}
MeshHit pickScene(RenderScene scene, Vec3 origin, Vec3 direction) {
    thread_local MeshQueryCache cache;
    cache.collect();
    MeshHit nearest;
    for (const auto& instance : scene) {
        auto hit = cache.pick(instance, origin, direction);
        if (hit.distance < nearest.distance)
            nearest = hit;
    }
    return nearest;
}
} // namespace paper
