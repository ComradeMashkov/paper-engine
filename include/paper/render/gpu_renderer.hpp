#pragma once
#include "paper/render/scene.hpp"
#include "paper/render/style.hpp"
#include <SDL3/SDL.h>
#include <filesystem>
#include <memory>

namespace paper {
struct RenderStats {
    size_t drawCalls = 0, triangles = 0, meshUploads = 0, textureUploads = 0;
    size_t meshBytes = 0, textureBytes = 0;
    size_t lightCount = 0, shadowPasses = 0, shadowDrawCalls = 0, shadowTriangles = 0;
    size_t shadowMapBytes = 0;
    size_t objectsTested = 0, objectsCulled = 0, roomsTested = 0, roomsCulled = 0;
    size_t particles = 0;
    double encodeMs = 0;
};
// All use is on the SDL main thread. The supplied device/renderer must outlive this object.
class GpuRenderer {
  public:
    GpuRenderer(SDL_GPUDevice& device, SDL_Renderer& renderer, std::filesystem::path shaderRoot,
                RenderConfig config = {});
    ~GpuRenderer();
    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;
    // Flushes queued SDL draws before replacing targets; retains the old targets on failure.
    void resize(RenderSize size);
    [[nodiscard]] RenderSize size() const noexcept;
    void render(RenderScene scene, const MaterialLibrary& materials, const Camera& camera,
                const RenderOptions& options, const RenderStyle& style, bool profile = false);
    [[nodiscard]] SDL_Texture* image() const noexcept;
    [[nodiscard]] RenderStats stats() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace paper
