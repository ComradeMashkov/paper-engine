#include "paper/render/gpu_renderer.hpp"
#include "paper/core/units.hpp"
#include "paper/render/gpu_data.hpp"
#include "paper/render/spatial.hpp"
#include "paper/sdl/resources.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>

namespace paper {
namespace {
template <class T, auto Release> struct GpuDeleter {
    SDL_GPUDevice* device = nullptr;
    void operator()(T* value) const noexcept { Release(device, value); }
};
template <class T, auto Release> using GpuOwner = std::unique_ptr<T, GpuDeleter<T, Release>>;
using Texture = GpuOwner<SDL_GPUTexture, SDL_ReleaseGPUTexture>;
using Buffer = GpuOwner<SDL_GPUBuffer, SDL_ReleaseGPUBuffer>;
using Transfer = GpuOwner<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer>;
using Shader = GpuOwner<SDL_GPUShader, SDL_ReleaseGPUShader>;
using Pipeline = GpuOwner<SDL_GPUGraphicsPipeline, SDL_ReleaseGPUGraphicsPipeline>;
using Sampler = GpuOwner<SDL_GPUSampler, SDL_ReleaseGPUSampler>;
constexpr auto colorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr auto depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
constexpr size_t verticesPerTriangle = 3;
constexpr size_t depthBytesPerPixel = sizeof(float);
enum class PassKind { World, Shadow, Particle, Post };

struct Commands {
    SDL_GPUCommandBuffer* value;
    explicit Commands(SDL_GPUDevice* device) : value(SDL_AcquireGPUCommandBuffer(device)) {
        sdl::check(value != nullptr, "Acquire GPU commands");
    }
    ~Commands() {
        if (value)
            SDL_CancelGPUCommandBuffer(value);
    }
    void submit() {
        auto* submitted = std::exchange(value, nullptr);
        sdl::check(SDL_SubmitGPUCommandBuffer(submitted), "Submit GPU scene");
    }
};
struct CopyPass {
    SDL_GPUCopyPass* value;
    explicit CopyPass(SDL_GPUCommandBuffer* commands) : value(SDL_BeginGPUCopyPass(commands)) {
        sdl::check(value != nullptr, "Begin GPU upload");
    }
    ~CopyPass() { SDL_EndGPUCopyPass(value); }
};
struct RenderPass {
    SDL_GPURenderPass* value;
    RenderPass(SDL_GPUCommandBuffer* commands, const SDL_GPUColorTargetInfo* colors,
               Uint32 colorCount, const SDL_GPUDepthStencilTargetInfo* depth = nullptr)
        : value(SDL_BeginGPURenderPass(commands, colors, colorCount, depth)) {
        sdl::check(value != nullptr, "Begin GPU render pass");
    }
    ~RenderPass() { SDL_EndGPURenderPass(value); }
};
struct Properties {
    SDL_PropertiesID value = SDL_CreateProperties();
    Properties() { sdl::check(value != 0, "Create texture properties"); }
    ~Properties() { SDL_DestroyProperties(value); }
};
Uint32 checkedSize(size_t size) {
    if (size > std::numeric_limits<Uint32>::max())
        throw std::length_error("GPU resource exceeds the transfer size limit");
    return static_cast<Uint32>(size);
}
std::vector<Uint8> readShader(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<Uint8> bytes(std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{});
    if (bytes.empty() || input.bad())
        throw std::runtime_error("Cannot read GPU shader: " + path.string());
    return bytes;
}
} // namespace

struct GpuRenderer::Impl {
    struct Batch {
        MaterialId material;
        bool twoSided;
        Uint32 first, count;
    };
    struct Mesh {
        Buffer buffer;
        std::vector<Batch> batches;
        Box3 bounds;
        size_t bytes = 0;
    };
    struct Material {
        Texture base, alternate;
        gpu::MaterialUniforms uniforms;
        std::uint64_t revision = 0;
        size_t bytes = 0;
    };
    SDL_GPUDevice* device;
    SDL_Renderer* renderer;
    RenderConfig config;
    RenderStats stats;
    Pipeline solid, twoSided, shadow, particle, post;
    Sampler sampler, clampSampler, shadowSampler;
    Texture color, depth, output, shadowMap, shadowFallback;
    Uint32 shadowResolution = 0;
    bool fallbackInitialized = false;
    // SDL wraps output without taking ownership; the wrapper must die first.
    sdl::Texture presentation;
    std::map<MeshHandle, Mesh, std::owner_less<MeshHandle>> meshes;
    std::vector<Material> materials;
    std::vector<gpu::ModelUniforms> models;

    Texture texture(Uint32 width, Uint32 height, SDL_GPUTextureFormat format,
                    SDL_GPUTextureUsageFlags usage) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = format;
        info.usage = usage;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        Texture result{SDL_CreateGPUTexture(device, &info), {device}};
        sdl::check(bool(result), "Create GPU texture");
        return result;
    }
    Transfer transfer(const void* data, size_t size) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = checkedSize(size);
        Transfer result{SDL_CreateGPUTransferBuffer(device, &info), {device}};
        sdl::check(bool(result), "Create GPU transfer buffer");
        void* mapped = SDL_MapGPUTransferBuffer(device, result.get(), false);
        sdl::check(mapped != nullptr, "Map GPU upload");
        std::memcpy(mapped, data, size);
        SDL_UnmapGPUTransferBuffer(device, result.get());
        return result;
    }
    Shader shader(const std::filesystem::path& root, const char* entry, SDL_GPUShaderStage stage,
                  Uint32 samplers, Uint32 uniforms) {
        const auto formats = SDL_GetGPUShaderFormats(device);
        const bool metal = (formats & SDL_GPU_SHADERFORMAT_MSL) != 0;
        const bool dxil = (formats & SDL_GPU_SHADERFORMAT_DXIL) != 0;
        const auto format = metal  ? SDL_GPU_SHADERFORMAT_MSL
                            : dxil ? SDL_GPU_SHADERFORMAT_DXIL
                                   : SDL_GPU_SHADERFORMAT_SPIRV;
        const auto path =
            root / (metal ? "scene.metal" : std::string(entry) + (dxil ? ".dxil" : ".spv"));
        auto bytes = readShader(path);
        const size_t codeSize = bytes.size();
        bytes.push_back(0); // SDL's MSL source reader expects a terminated string.
        SDL_GPUShaderCreateInfo info{};
        info.code = bytes.data();
        info.code_size = codeSize;
        info.entrypoint = entry;
        info.format = format;
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        Shader result{SDL_CreateGPUShader(device, &info), {device}};
        if (!result)
            throw std::runtime_error("Create shader " + path.string() + " (" + entry +
                                     "): " + SDL_GetError());
        return result;
    }
    Pipeline pipeline(SDL_GPUShader* vertex, SDL_GPUShader* fragment, PassKind kind,
                      SDL_GPUCullMode cull) {
        const SDL_GPUVertexBufferDescription binding{0, sizeof(gpu::Vertex),
                                                     SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        const std::array<SDL_GPUVertexAttribute, 3> attributes{
            {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(gpu::Vertex, position)},
             {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(gpu::Vertex, uv)},
             {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(gpu::Vertex, normal)}}};
        SDL_GPUColorTargetDescription target{};
        target.format = colorFormat;
        if (kind == PassKind::Particle) {
            target.blend_state.enable_blend = true;
            target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        const bool geometry = kind != PassKind::Post;
        if (geometry && kind != PassKind::Particle)
            info.vertex_input_state = {&binding, 1, attributes.data(),
                                       checkedSize(attributes.size())};
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = cull;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.rasterizer_state.enable_depth_clip = true;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.enable_depth_test = geometry;
        info.depth_stencil_state.enable_depth_write = geometry && kind != PassKind::Particle;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        if (kind != PassKind::Shadow) {
            info.target_info.color_target_descriptions = &target;
            info.target_info.num_color_targets = 1;
        }
        info.target_info.has_depth_stencil_target = geometry;
        info.target_info.depth_stencil_format = depthFormat;
        Pipeline result{SDL_CreateGPUGraphicsPipeline(device, &info), {device}};
        sdl::check(bool(result), "Create GPU graphics pipeline");
        return result;
    }
    Impl(SDL_GPUDevice& gpuDevice, SDL_Renderer& presentationRenderer,
         const std::filesystem::path& root, RenderConfig renderConfig)
        : device(&gpuDevice), renderer(&presentationRenderer), config(renderConfig) {
        if (!config.width || !config.height)
            throw std::invalid_argument("Empty GPU render target");
        if (!SDL_GPUTextureSupportsFormat(device, depthFormat, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                                              SDL_GPU_TEXTUREUSAGE_SAMPLER))
            throw std::runtime_error("GPU device lacks sampled D32 depth required for shadows");
        auto worldVertex = shader(root, "world_vertex", SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
        auto worldFragment = shader(root, "world_fragment", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 2);
        auto screenVertex = shader(root, "screen_vertex", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        auto screenFragment = shader(root, "screen_fragment", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        auto shadowVertex = shader(root, "shadow_vertex", SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
        auto shadowFragment = shader(root, "shadow_fragment", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        auto particleVertex = shader(root, "particle_vertex", SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
        auto particleFragment =
            shader(root, "particle_fragment", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
        particle = pipeline(particleVertex.get(), particleFragment.get(), PassKind::Particle,
                            SDL_GPU_CULLMODE_NONE);
        shadow = pipeline(shadowVertex.get(), shadowFragment.get(), PassKind::Shadow,
                          SDL_GPU_CULLMODE_NONE);
        solid = pipeline(worldVertex.get(), worldFragment.get(), PassKind::World,
                         SDL_GPU_CULLMODE_BACK);
        twoSided = pipeline(worldVertex.get(), worldFragment.get(), PassKind::World,
                            SDL_GPU_CULLMODE_NONE);
        post = pipeline(screenVertex.get(), screenFragment.get(), PassKind::Post,
                        SDL_GPU_CULLMODE_NONE);
        SDL_GPUSamplerCreateInfo sampling{};
        sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        sampler = Sampler{SDL_CreateGPUSampler(device, &sampling), {device}};
        sdl::check(bool(sampler), "Create GPU sampler");
        sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        clampSampler = Sampler{SDL_CreateGPUSampler(device, &sampling), {device}};
        sdl::check(bool(clampSampler), "Create GPU clamp sampler");
        sampling.enable_compare = true;
        sampling.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        shadowSampler = Sampler{SDL_CreateGPUSampler(device, &sampling), {device}};
        sdl::check(bool(shadowSampler), "Create shadow comparison sampler");
        shadowFallback =
            texture(1, 1, depthFormat,
                    SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        resize({config.width, config.height});
    }
    void resize(RenderSize size) {
        if (!size.width || !size.height)
            throw std::invalid_argument("Empty GPU render target");
        if (presentation && size == RenderSize{config.width, config.height})
            return;
        const auto usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        auto nextColor = texture(size.width, size.height, colorFormat, usage);
        auto nextOutput = texture(size.width, size.height, colorFormat, usage);
        auto nextDepth = texture(size.width, size.height, depthFormat,
                                 SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        Properties properties;
        sdl::check(SDL_SetNumberProperty(properties.value, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                         SDL_PIXELFORMAT_RGBA32) &&
                       SDL_SetNumberProperty(properties.value,
                                             SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                             SDL_TEXTUREACCESS_STATIC) &&
                       SDL_SetNumberProperty(properties.value, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                                             size.width) &&
                       SDL_SetNumberProperty(properties.value,
                                             SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, size.height) &&
                       SDL_SetPointerProperty(properties.value,
                                              SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER,
                                              nextOutput.get()),
                   "Wrap GPU scene target");
        sdl::Texture nextPresentation(SDL_CreateTextureWithProperties(renderer, properties.value));
        sdl::check(bool(nextPresentation), "Create GPU scene presentation");
        sdl::check(SDL_SetTextureScaleMode(nextPresentation.get(), SDL_SCALEMODE_NEAREST) &&
                       SDL_SetTextureBlendMode(nextPresentation.get(), SDL_BLENDMODE_NONE),
                   "Set GPU scene sampling");
        // Settings can change after the world was queued for composition this frame.
        // Submit those commands before releasing its SDL wrapper. GPU releases are deferred.
        sdl::check(SDL_FlushRenderer(renderer), "Flush scene before resizing");
        presentation.swap(nextPresentation);
        color.swap(nextColor);
        output.swap(nextOutput);
        depth.swap(nextDepth);
        config.width = size.width;
        config.height = size.height;
    }
    Texture uploadTexture(SDL_GPUCopyPass* pass, int width, int height,
                          std::span<const Pixel> pixels) {
        auto result = texture(static_cast<Uint32>(width), static_cast<Uint32>(height), colorFormat,
                              SDL_GPU_TEXTUREUSAGE_SAMPLER);
        auto upload = transfer(pixels.data(), pixels.size_bytes());
        const SDL_GPUTextureTransferInfo source{upload.get(), 0, static_cast<Uint32>(width),
                                                static_cast<Uint32>(height)};
        SDL_GPUTextureRegion destination{};
        destination.texture = result.get();
        destination.w = static_cast<Uint32>(width);
        destination.h = static_cast<Uint32>(height);
        destination.d = 1;
        SDL_UploadToGPUTexture(pass, &source, &destination, false);
        ++stats.textureUploads;
        return result;
    }
    void syncMaterials(SDL_GPUCopyPass* pass, const MaterialLibrary& library) {
        materials.resize(library.size());
        for (size_t i = 0; i < library.size(); ++i) {
            const auto id = static_cast<MaterialId>(i);
            auto& stored = materials[i];
            if (stored.revision != library.revision(id)) {
                const auto& art = library.art(id);
                Material next;
                next.base = uploadTexture(pass, art.width, art.height, art.pixels);
                next.bytes = art.pixels.size() * sizeof(Pixel);
                if (!art.alteredFace.empty()) {
                    next.alternate = uploadTexture(pass, art.width, art.height, art.alteredFace);
                    next.bytes += art.alteredFace.size() * sizeof(Pixel);
                }
                next.uniforms.region = {art.faceCenter.x, art.faceCenter.y, art.faceRadius.x,
                                        art.faceRadius.y};
                next.uniforms.flags = {art.emissive ? 1.f : 0.f, next.alternate ? 1.f : 0.f, 0, 0};
                next.revision = library.revision(id);
                stored = std::move(next);
            }
            stats.textureBytes += stored.bytes;
        }
    }
    void syncMesh(SDL_GPUCopyPass* pass, const MeshHandle& handle) {
        if (!handle || handle->empty() || meshes.contains(handle))
            return;
        std::map<std::pair<MaterialId, bool>, std::vector<gpu::Vertex>> groups;
        for (const auto& triangle : *handle) {
            const Vec3 normal = normalized(
                cross(triangle.v[1].p - triangle.v[0].p, triangle.v[2].p - triangle.v[0].p));
            auto& group = groups[{triangle.material, triangle.twoSided}];
            for (const auto& vertex : triangle.v)
                group.push_back({vertex.p, vertex.uv, normal});
        }
        Mesh mesh;
        mesh.bounds = meshBounds(*handle);
        std::vector<gpu::Vertex> vertices;
        for (const auto& [key, group] : groups) {
            mesh.batches.push_back(
                {key.first, key.second, checkedSize(vertices.size()), checkedSize(group.size())});
            vertices.insert(vertices.end(), group.begin(), group.end());
        }
        mesh.bytes = vertices.size() * sizeof(gpu::Vertex);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = checkedSize(mesh.bytes);
        mesh.buffer = Buffer{SDL_CreateGPUBuffer(device, &info), {device}};
        sdl::check(bool(mesh.buffer), "Create immutable GPU mesh");
        auto upload = transfer(vertices.data(), mesh.bytes);
        const SDL_GPUTransferBufferLocation source{upload.get(), 0};
        const SDL_GPUBufferRegion destination{mesh.buffer.get(), 0, info.size};
        SDL_UploadToGPUBuffer(pass, &source, &destination, false);
        meshes.emplace(handle, std::move(mesh));
        ++stats.meshUploads;
    }
    static SDL_GPUDepthStencilTargetInfo depthTarget(SDL_GPUTexture* texture,
                                                     SDL_GPUStoreOp store) {
        SDL_GPUDepthStencilTargetInfo target{};
        target.texture = texture;
        target.clear_depth = 1;
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = store;
        target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        return target;
    }
    void renderShadow(SDL_GPUCommandBuffer* commands, RenderScene scene,
                      const gpu::FrameUniforms& frame, const ShadowSettings& settings) {
        if (!fallbackInitialized) {
            const auto target = depthTarget(shadowFallback.get(), SDL_GPU_STOREOP_STORE);
            RenderPass clear(commands, nullptr, 0, &target);
            fallbackInitialized = true;
        }
        const auto quality = shadowQualityInfo(settings.quality);
        if (!quality.mapSize) {
            shadowMap.reset();
            shadowResolution = 0;
        }
        if (frame.shadowForward.w >= 0) {
            if (shadowResolution != quality.mapSize) {
                shadowMap = texture(quality.mapSize, quality.mapSize, depthFormat,
                                    SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER);
                shadowResolution = quality.mapSize;
            }
            const auto target = depthTarget(shadowMap.get(), SDL_GPU_STOREOP_STORE);
            RenderPass pass(commands, nullptr, 0, &target);
            SDL_BindGPUGraphicsPipeline(pass.value, shadow.get());
            SDL_PushGPUVertexUniformData(commands, 0, &frame, sizeof(frame));
            SDL_PushGPUFragmentUniformData(commands, 0, &frame, sizeof(frame));
            for (size_t i = 0; i < scene.size(); ++i) {
                if (!scene[i].castsShadow)
                    continue;
                const auto found = meshes.find(scene[i].mesh);
                if (found == meshes.end())
                    continue;
                const SDL_GPUBufferBinding binding{found->second.buffer.get(), 0};
                SDL_BindGPUVertexBuffers(pass.value, 0, &binding, 1);
                SDL_PushGPUVertexUniformData(commands, 1, &models[i], sizeof(models[i]));
                for (const auto& batch : found->second.batches) {
                    const auto& material = materials[static_cast<size_t>(batch.material)];
                    const SDL_GPUTextureSamplerBinding textureBinding{material.base.get(),
                                                                      sampler.get()};
                    SDL_BindGPUFragmentSamplers(pass.value, 0, &textureBinding, 1);
                    SDL_DrawGPUPrimitives(pass.value, batch.count, 1, batch.first, 0);
                    ++stats.shadowDrawCalls;
                    ++stats.drawCalls;
                    stats.shadowTriangles += batch.count / verticesPerTriangle;
                }
            }
            stats.shadowPasses = 1;
        }
        stats.shadowMapBytes =
            (size_t{shadowResolution} * shadowResolution + 1) * depthBytesPerPixel;
    }
    void render(RenderScene scene, const MaterialLibrary& library, const Camera& camera,
                const RenderOptions& options, const RenderStyle& style, bool profile) {
        const Uint64 started = profile ? SDL_GetTicksNS() : 0;
        stats = {};
        const auto frame = gpu::frameUniforms(camera, options, config, style);
        const auto particles = sortedParticles(options.particles, camera);
        stats.lightCount = options.lights.size();
        // No frame can reference an expired asset; SDL defers GPU deletion for in-flight uses.
        std::erase_if(meshes, [](const auto& item) { return item.first.use_count() == 1; });
        models.clear();
        models.reserve(scene.size());
        for (const auto& instance : scene)
            models.push_back(gpu::modelUniforms(instance.transform));
        Commands commands(device);
        {
            CopyPass copy(commands.value);
            syncMaterials(copy.value, library);
            for (const auto& instance : scene)
                syncMesh(copy.value, instance.mesh);
        }
        for (const auto& [key, mesh] : meshes) {
            (void)key;
            stats.meshBytes += mesh.bytes;
        }
        for (const auto& instance : scene) {
            const auto found = meshes.find(instance.mesh);
            if (found == meshes.end())
                continue;
            for (const auto& batch : found->second.batches)
                if (static_cast<size_t>(batch.material) >= materials.size())
                    throw std::out_of_range("Mesh references an unknown material");
        }
        std::vector<Box3> instanceBounds(scene.size());
        std::map<std::string, Box3> roomBounds;
        for (size_t i = 0; i < scene.size(); ++i) {
            auto found = meshes.find(scene[i].mesh);
            if (found == meshes.end())
                continue;
            auto b = worldBounds(found->second.bounds, scene[i].transform);
            instanceBounds[i] = b;
            if (!scene[i].room.empty()) {
                auto [room, inserted] = roomBounds.try_emplace(scene[i].room, b);
                if (!inserted)
                    room->second = unionBounds(room->second, b);
            }
        }
        std::map<std::string, bool> visibleRooms;
        for (const auto& [id, bounds] : roomBounds)
            visibleRooms[id] = inFrustum(bounds, camera, options.aspect, config.nearPlaneMeters,
                                         config.farPlaneMeters);
        stats.roomsTested = visibleRooms.size();
        stats.roomsCulled =
            std::ranges::count_if(visibleRooms, [](const auto& pair) { return !pair.second; });
        SDL_GPUColorTargetInfo target{};
        target.texture = color.get();
        target.clear_color = {style.fogColor.x, style.fogColor.y, style.fogColor.z, 1};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        renderShadow(commands.value, scene, frame, options.shadows);
        const auto worldDepth = depthTarget(depth.get(), SDL_GPU_STOREOP_DONT_CARE);
        for (const bool foreground : {false, true}) {
            if (foreground) {
                if (!std::ranges::any_of(scene, &MeshInstance::foreground) &&
                    !std::ranges::any_of(particles, &Particle::foreground))
                    break;
                target.load_op = SDL_GPU_LOADOP_LOAD;
            }
            // Preserve world colour, clear depth for camera-attached hands and their smoke.
            RenderPass pass(commands.value, &target, 1, &worldDepth);
            SDL_PushGPUVertexUniformData(commands.value, 0, &frame, sizeof(frame));
            SDL_PushGPUFragmentUniformData(commands.value, 0, &frame, sizeof(frame));
            for (size_t i = 0; i < scene.size(); ++i) {
                if (scene[i].foreground != foreground)
                    continue;
                if (!meshes.contains(scene[i].mesh))
                    continue;
                ++stats.objectsTested;
                if ((!scene[i].room.empty() && !visibleRooms.at(scene[i].room)) ||
                    !inFrustum(instanceBounds[i], camera, options.aspect, config.nearPlaneMeters,
                               config.farPlaneMeters)) {
                    ++stats.objectsCulled;
                    continue;
                }
                const auto found = meshes.find(scene[i].mesh);
                if (found == meshes.end())
                    continue;
                const auto& mesh = found->second;
                const SDL_GPUBufferBinding binding{mesh.buffer.get(), 0};
                SDL_BindGPUVertexBuffers(pass.value, 0, &binding, 1);
                SDL_PushGPUVertexUniformData(commands.value, 1, &models[i], sizeof(models[i]));
                for (const auto& batch : mesh.batches) {
                    const auto& material = materials[static_cast<size_t>(batch.material)];
                    SDL_BindGPUGraphicsPipeline(pass.value,
                                                batch.twoSided ? twoSided.get() : solid.get());
                    const std::array<SDL_GPUTextureSamplerBinding, 3> bindings{
                        {{material.base.get(), sampler.get()},
                         {material.alternate ? material.alternate.get() : material.base.get(),
                          clampSampler.get()},
                         {shadowMap ? shadowMap.get() : shadowFallback.get(),
                          shadowSampler.get()}}};
                    SDL_BindGPUFragmentSamplers(pass.value, 0, bindings.data(),
                                                checkedSize(bindings.size()));
                    SDL_PushGPUFragmentUniformData(commands.value, 1, &material.uniforms,
                                                   sizeof(material.uniforms));
                    SDL_DrawGPUPrimitives(pass.value, batch.count, 1, batch.first, 0);
                    ++stats.drawCalls;
                    stats.triangles += batch.count / verticesPerTriangle;
                }
            }
            if (!particles.empty()) {
                SDL_BindGPUGraphicsPipeline(pass.value, particle.get());
                for (const auto& puff : particles) {
                    if (puff.foreground != foreground)
                        continue;
                    const auto centerDepth = dot(puff.position - camera.position, camera.forward());
                    if (puff.opacity <= 0 || centerDepth <= config.nearPlaneMeters ||
                        centerDepth >= config.farPlaneMeters)
                        continue;
                    const gpu::ParticleUniforms uniforms{
                        {puff.position.x, puff.position.y, puff.position.z, puff.radiusMeters},
                        {puff.color.x, puff.color.y, puff.color.z, puff.opacity},
                        {puff.style == ParticleStyle::Faceted ? 1.f : 0.f, puff.rotationRadians}};
                    SDL_PushGPUVertexUniformData(commands.value, 1, &uniforms, sizeof(uniforms));
                    const unsigned triangles = puff.style == ParticleStyle::Faceted
                                                   ? PAPER_SMOKE_FACE_COUNT
                                                   : 2; // numbers: two billboard triangles.
                    SDL_DrawGPUPrimitives(pass.value, verticesPerTriangle * triangles, 1, 0, 0);
                    ++stats.drawCalls;
                    ++stats.particles;
                    stats.triangles += triangles;
                }
            }
        }
        target.texture = output.get();
        target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        {
            RenderPass pass(commands.value, &target, 1);
            SDL_BindGPUGraphicsPipeline(pass.value, post.get());
            const SDL_GPUTextureSamplerBinding binding{color.get(), clampSampler.get()};
            SDL_BindGPUFragmentSamplers(pass.value, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(commands.value, 0, &frame, sizeof(frame));
            SDL_DrawGPUPrimitives(pass.value, verticesPerTriangle, 1, 0, 0);
            ++stats.drawCalls;
        }
        // Submitted before SDL's UI batch, which samples this GPU texture on the same device.
        // Do not cycle output: the external SDL wrapper must refer to the exact written image.
        commands.submit();
        if (profile)
            stats.encodeMs =
                static_cast<double>(SDL_GetTicksNS() - started) / units::nanosecondsPerMillisecond;
    }
};
GpuRenderer::GpuRenderer(SDL_GPUDevice& device, SDL_Renderer& renderer, std::filesystem::path root,
                         RenderConfig config)
    : impl_(std::make_unique<Impl>(device, renderer, root, config)) {}
GpuRenderer::~GpuRenderer() = default;
void GpuRenderer::resize(RenderSize size) {
    impl_->resize(size);
}
RenderSize GpuRenderer::size() const noexcept {
    return {impl_->config.width, impl_->config.height};
}
void GpuRenderer::render(RenderScene scene, const MaterialLibrary& materials, const Camera& camera,
                         const RenderOptions& options, const RenderStyle& style, bool profile) {
    impl_->render(scene, materials, camera, options, style, profile);
}
SDL_Texture* GpuRenderer::image() const noexcept {
    return impl_->presentation.get();
}
RenderStats GpuRenderer::stats() const noexcept {
    return impl_->stats;
}
} // namespace paper
