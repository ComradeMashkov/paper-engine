#include "cgltf.h"
#include "paper/core/pixel_format.hpp"
#include "paper/resources/model.hpp"
#include "stb_image.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

namespace paper {
namespace {
constexpr size_t accessorComponentsLimit = 16, nodeHierarchyDepth = 128;
constexpr float minimumQuaternionSquaredLength = 1e-8f;
struct ImportHeap {
    static constexpr size_t limitBytes = 128 * 1024 * 1024;
    size_t live = 0;
    struct alignas(std::max_align_t) Header {
        size_t bytes;
    };
    static void* allocate(void* user, cgltf_size bytes) {
        auto& heap = *static_cast<ImportHeap*>(user);
        if (bytes > limitBytes - heap.live || bytes > limitBytes - sizeof(Header))
            return nullptr;
        auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + bytes));
        if (!header)
            return nullptr;
        header->bytes = bytes;
        heap.live += bytes;
        return header + 1;
    }
    static void release(void* user, void* pointer) {
        if (!pointer)
            return;
        auto* header = static_cast<Header*>(pointer) - 1;
        static_cast<ImportHeap*>(user)->live -= header->bytes;
        std::free(header);
    }
};
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error("glTF: " + message);
}
bool finite(std::span<const float> values) {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}
void uniformScale(const float* scale) {
    constexpr float scaleTolerance = 1e-5f;
    require(finite({scale, 3}) && scale[0] > 0 &&
                std::abs(scale[0] - scale[1]) <
                    scaleTolerance && // numbers: glTF component/corner layout.
                std::abs(scale[0] - scale[2]) <
                    scaleTolerance, // numbers: glTF component/corner layout.
            "only positive uniform scale is supported");
}
void accessor(const cgltf_accessor* value, cgltf_type type) {
    require(value && value->type == type && !value->is_sparse && value->buffer_view,
            "expected a dense accessor with a buffer view");
    require(value->count <= resourceLimits::animationKeys * accessorComponentsLimit,
            "accessor count limit exceeded");
}
std::filesystem::path uriPath(const std::filesystem::path& parent, const char* uri) {
    std::string decoded(uri ? uri : "");
    require(!decoded.empty() && decoded.find(':') == std::string::npos,
            "external URI must be a relative file path");
    decoded.resize(cgltf_decode_uri(decoded.data()));
    require(decoded.find('\0') == std::string::npos, "NUL in URI");
    return parent / decoded;
}
PaintedTexture embeddedImage(std::span<const uint8_t> bytes) {
    int w = 0, h = 0, n = 0;
    require(bytes.size() <= resourceLimits::fileBytes &&
                stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n) &&
                w > 0 && h > 0 && static_cast<size_t>(w) <= resourceLimits::imagePixels / h,
            "invalid embedded image or image limit exceeded");
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n,
                              pixelFormat::channels),
        stbi_image_free);
    require(bool(pixels), "cannot decode embedded PNG/JPEG");
    PaintedTexture texture;
    texture.width = w;
    texture.height = h;
    texture.pixels.resize(static_cast<size_t>(w) * h);
    std::memcpy(texture.pixels.data(), pixels.get(), texture.pixels.size() * sizeof(Pixel));
    return texture;
}
} // namespace
std::unique_ptr<ModelResource> loadGltf(ResourceStore& store,
                                        const std::filesystem::path& relative) {
    auto source = store.file(relative);
    ImportHeap heap;
    cgltf_options options{};
    options.memory = {ImportHeap::allocate, ImportHeap::release, &heap};
    cgltf_data* raw = nullptr;
    const auto parsed = cgltf_parse(&options, source->data.data(), source->data.size(), &raw);
    require(parsed == cgltf_result_success, "parse failed (code " + std::to_string(parsed) + ")");
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
    auto model = std::make_unique<ModelResource>();
    model->dependencies.push_back(source);
    require(data->nodes_count <= resourceLimits::modelNodes, "node limit exceeded");
    require(data->skins_count <= resourceLimits::skinBindings, "skin limit exceeded");
    require(data->cameras_count == 0 && data->lights_count == 0,
            "glTF cameras/lights are unsupported; use scene components");
    for (size_t i = 0; i < data->extensions_used_count; ++i)
        require(std::string_view(data->extensions_used[i]) == "KHR_materials_unlit",
                "unsupported extension: " + std::string(data->extensions_used[i]));
    size_t bufferBytes = 0;
    for (size_t i = 0; i < data->buffers_count; ++i) {
        auto& buffer = data->buffers[i];
        require(buffer.size <= resourceLimits::fileBytes - bufferBytes,
                "total buffer byte limit exceeded");
        bufferBytes += buffer.size;
        if (!buffer.uri) {
            require(i == 0 && data->bin && buffer.size <= data->bin_size,
                    "missing GLB binary chunk");
            buffer.data = const_cast<void*>(data->bin);
        } else if (std::string_view(buffer.uri).starts_with("data:")) {
            const std::string_view uri(buffer.uri);
            const auto comma = uri.find(',');
            require(comma != uri.npos && uri.substr(0, comma).ends_with(";base64"),
                    "expected base64 buffer URI");
            require(uri.size() - comma - 1 >=
                        ((buffer.size + 2) / 3) * 4, // numbers: base64 encodes each three-byte
                                                     // block as four characters, rounded up.
                    "truncated base64 buffer");
            require(cgltf_load_buffer_base64(&options, buffer.size, buffer.uri + comma + 1,
                                             &buffer.data) == cgltf_result_success,
                    "invalid base64 buffer");
            buffer.data_free_method = cgltf_data_free_method_memory_free;
        } else {
            auto dependency = store.file(uriPath(relative.parent_path(), buffer.uri));
            require(dependency->data.size() >= buffer.size, "external buffer is truncated");
            buffer.data = const_cast<uint8_t*>(dependency->data.data());
            model->dependencies.push_back(std::move(dependency));
        }
    }
    require(cgltf_validate(data.get()) == cgltf_result_success,
            "invalid references, ranges or hierarchy");
    size_t bindingCount = 0;
    for (size_t i = 0; i < data->skins_count; ++i) {
        const auto& sourceSkin = data->skins[i];
        require(sourceSkin.joints_count > 0 &&
                    sourceSkin.joints_count <= resourceLimits::skinJoints &&
                    sourceSkin.joints_count <= resourceLimits::skinBindings - bindingCount,
                "skin joint limit exceeded");
        bindingCount += sourceSkin.joints_count;
        if (sourceSkin.inverse_bind_matrices) {
            accessor(sourceSkin.inverse_bind_matrices, cgltf_type_mat4);
            require(sourceSkin.inverse_bind_matrices->count == sourceSkin.joints_count,
                    "skin inverse bind count mismatch");
        }
        ModelSkin skin;
        for (size_t joint = 0; joint < sourceSkin.joints_count; ++joint) {
            skin.joints.push_back(static_cast<size_t>(sourceSkin.joints[joint] - data->nodes));
            std::array<float, 16> matrix{
                1, 0, 0, 0, 0, 1, 0, 0,
                0, 0, 1, 0, 0, 0, 0, 1}; // numbers: column-major affine identity.
            if (sourceSkin.inverse_bind_matrices)
                require(cgltf_accessor_read_float(sourceSkin.inverse_bind_matrices, joint,
                                                  matrix.data(), matrix.size()),
                        "invalid inverse bind matrix");
            require(finite(matrix) && matrix[3] == 0 && matrix[7] == 0 && matrix[11] == 0 &&
                        matrix[15] == 1,
                    "inverse bind must be finite affine MAT4"); // numbers: homogeneous row indices.
            skin.inverseBind.push_back(matrix);
        }
        model->skins.push_back(std::move(skin));
    }
    require(data->materials_count < std::numeric_limits<uint16_t>::max(),
            "material count limit exceeded");
    // A default white base-color material; the game's painterly lighting remains authoritative.
    PaintedTexture fallback;
    fallback.width = fallback.height = 1;
    constexpr Pixel whiteFallback{255, 255, 255, 255};
    fallback.pixels = {whiteFallback};
    model->materials.push_back(fallback);
    for (size_t i = 0; i < data->materials_count; ++i) {
        const auto& material = data->materials[i];
        require(material.alpha_mode != cgltf_alpha_mode_blend,
                "BLEND materials are unsupported (use OPAQUE or MASK)");
        require(!material.normal_texture.texture && !material.occlusion_texture.texture &&
                    !material.emissive_texture.texture,
                "normal, occlusion and emissive maps are unsupported");
        require(finite(material.emissive_factor) && material.emissive_factor[0] == 0 &&
                    material.emissive_factor[1] == 0 &&
                    material.emissive_factor[2] == 0, // numbers: glTF component/corner layout.
                "emissive factors are unsupported; use KHR_materials_unlit");
        require(std::isfinite(material.alpha_cutoff) && material.alpha_cutoff >= 0 &&
                    material.alpha_cutoff <= 1,
                "invalid alpha cutoff");
        const auto& pbr = material.pbr_metallic_roughness;
        require(!pbr.metallic_roughness_texture.texture, "metallic-roughness maps are unsupported");
        require(finite({pbr.base_color_factor, 4}),
                "non-finite base color"); // numbers: glTF component/corner layout.
        auto texture = fallback;
        const auto& view = pbr.base_color_texture;
        if (view.texture) {
            require(view.texcoord == 0 && !view.has_transform,
                    "only TEXCOORD_0 without texture transforms is supported");
            if (view.texture->sampler) {
                const auto& sampler = *view.texture->sampler;
                require(sampler.wrap_s == cgltf_wrap_mode_repeat &&
                            sampler.wrap_t == cgltf_wrap_mode_repeat,
                        "only REPEAT texture wrapping is supported");
            }
            const auto* image = view.texture->image;
            require(image, "base color texture has no image");
            if (image->uri) {
                auto dependency = store.image(uriPath(relative.parent_path(), image->uri));
                texture = dependency->texture;
                model->dependencies.push_back(std::move(dependency));
            } else {
                require(image->buffer_view && image->buffer_view->buffer->data,
                        "image has no buffer data");
                const auto* begin = static_cast<const uint8_t*>(image->buffer_view->buffer->data) +
                                    image->buffer_view->offset;
                texture = embeddedImage({begin, image->buffer_view->size});
            }
        }
        for (auto& pixel : texture.pixels) {
            uint8_t* components = &pixel.r;
            for (int channel = 0; channel < pixelFormat::channels; ++channel) {
                require(pbr.base_color_factor[channel] >= 0 && pbr.base_color_factor[channel] <= 1,
                        "base color out of range");
                components[channel] = static_cast<uint8_t>(
                    std::lround(components[channel] * pbr.base_color_factor[channel]));
            }
            pixel.a = material.alpha_mode == cgltf_alpha_mode_mask
                          ? (pixel.a / static_cast<float>(pixelFormat::maximumChannel) <
                                     material.alpha_cutoff
                                 ? 0
                                 : pixelFormat::maximumChannel)
                          : pixelFormat::maximumChannel;
        }
        texture.emissive = material.unlit;
        model->materials.push_back(std::move(texture));
    }
    if (data->materials_count)
        model->diagnostics.push_back(
            "Base color/alpha imported; scalar metallic/roughness and vertex normals use the "
            "game's flat painterly shading; textures use nearest filtering.");
    std::map<const cgltf_mesh*, std::vector<MeshHandle>> meshes;
    std::map<const cgltf_mesh*, std::vector<std::vector<SkinVertex>>> influences;
    size_t triangleCount = 0;
    for (size_t i = 0; i < data->meshes_count; ++i) {
        auto& mesh = data->meshes[i];
        for (size_t p = 0; p < mesh.primitives_count; ++p) {
            const auto& primitive = mesh.primitives[p];
            require(primitive.type == cgltf_primitive_type_triangles && !primitive.targets_count &&
                        !primitive.has_draco_mesh_compression,
                    "only uncompressed TRIANGLES without morph targets are supported");
            const cgltf_accessor* positions = nullptr;
            const cgltf_accessor* uv = nullptr;
            const cgltf_accessor* joints = nullptr;
            const cgltf_accessor* weights = nullptr;
            for (size_t a = 0; a < primitive.attributes_count; ++a) {
                const auto& attribute = primitive.attributes[a];
                if (attribute.type == cgltf_attribute_type_position)
                    positions = attribute.data;
                else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0)
                    uv = attribute.data;
                else if (attribute.type == cgltf_attribute_type_joints && attribute.index == 0)
                    joints = attribute.data;
                else if (attribute.type == cgltf_attribute_type_weights && attribute.index == 0)
                    weights = attribute.data;
                else
                    require(attribute.type == cgltf_attribute_type_normal ||
                                attribute.type == cgltf_attribute_type_tangent,
                            "unsupported vertex attribute: " + std::string(attribute.name));
            }
            accessor(positions, cgltf_type_vec3);
            if (uv)
                accessor(uv, cgltf_type_vec2);
            require(bool(joints) == bool(weights), "skin needs both JOINTS_0 and WEIGHTS_0");
            if (joints) {
                accessor(joints, cgltf_type_vec4);
                accessor(weights, cgltf_type_vec4);
                require(joints->count == positions->count && weights->count == positions->count &&
                            !joints->normalized &&
                            (joints->component_type == cgltf_component_type_r_8u ||
                             joints->component_type == cgltf_component_type_r_16u),
                        "invalid skin attributes");
            }
            if (primitive.indices) {
                accessor(primitive.indices, cgltf_type_scalar);
                require(primitive.indices->component_type == cgltf_component_type_r_8u ||
                            primitive.indices->component_type == cgltf_component_type_r_16u ||
                            primitive.indices->component_type == cgltf_component_type_r_32u,
                        "invalid index component type");
            }
            const size_t count = primitive.indices ? primitive.indices->count : positions->count;
            require(count % 3 == 0 &&
                        count / 3 <= resourceLimits::modelTriangles -
                                         triangleCount, // numbers: glTF component/corner layout.
                    "triangle count limit exceeded");
            triangleCount += count / 3; // numbers: glTF component/corner layout.
            Mesh3 triangles;
            std::vector<SkinVertex> skinVertices;
            triangles.reserve(count / 3); // numbers: glTF component/corner layout.
            for (size_t start = 0; start < count;
                 start += 3) { // numbers: glTF component/corner layout.
                Triangle3 triangle;
                triangle.material = static_cast<MaterialId>(
                    primitive.material ? primitive.material - data->materials + 1 : 0);
                triangle.twoSided = primitive.material && primitive.material->double_sided;
                for (size_t corner = 0; corner < std::size(triangle.v);
                     ++corner) { // numbers: glTF component/corner layout.
                    const auto index =
                        primitive.indices
                            ? cgltf_accessor_read_index(primitive.indices, start + corner)
                            : start + corner;
                    float xyz[3]{}, st[2]{};
                    require(index < positions->count &&
                                cgltf_accessor_read_float(positions, index, xyz, 3) &&
                                finite(xyz), // numbers: glTF component/corner layout.
                            "invalid vertex position");
                    if (uv)
                        require(index < uv->count &&
                                    cgltf_accessor_read_float(
                                        uv, index, st,
                                        2) && // numbers: glTF component/corner layout.
                                    finite(st),
                                "invalid UV");
                    const Vec3 position{xyz[0], xyz[1], xyz[2]};
                    triangle.v[corner] = {position,
                                          {st[0], st[1]}}; // numbers: glTF component/corner layout.
                    if (joints) {
                        SkinVertex influence;
                        std::array<cgltf_uint, 4> indices{}; // numbers: four glTF joint influences.
                        require(cgltf_accessor_read_uint(joints, index, indices.data(),
                                                         indices.size()) &&
                                    cgltf_accessor_read_float(weights, index,
                                                              influence.weights.data(),
                                                              influence.weights.size()) &&
                                    finite(influence.weights),
                                "invalid skin vertex");
                        float sum = 0;
                        for (size_t j = 0; j < indices.size(); ++j) {
                            require(influence.weights[j] >= 0 && influence.weights[j] <= 1,
                                    "invalid skin weight range");
                            influence.joints[j] = indices[j];
                            sum += influence.weights[j];
                        }
                        constexpr float normalizedWeightTolerance = .001f;
                        require(std::abs(sum - 1) <= normalizedWeightTolerance,
                                "skin weights must sum to one");
                        for (auto& weight : influence.weights)
                            weight /= sum;
                        skinVertices.push_back(influence);
                    }
                }
                triangles.push_back(triangle);
            }
            meshes[&mesh].push_back(makeMesh(std::move(triangles)));
            influences[&mesh].push_back(std::move(skinVertices));
        }
    }
    model->nodes.resize(data->nodes_count);
    for (size_t i = 0; i < data->nodes_count; ++i) {
        const auto& node = data->nodes[i];
        auto& target = model->nodes[i];
        require(!node.has_matrix && !node.camera && !node.light && !node.has_mesh_gpu_instancing,
                "node " + std::to_string(i) +
                    ": matrix, camera, light or GPU instancing is unsupported; use TRS");
        require(finite(node.translation) && finite(node.rotation), "non-finite node transform");
        uniformScale(node.scale);
        const float norm =
            node.rotation[0] * node.rotation[0] + node.rotation[1] * node.rotation[1] +
            node.rotation[2] * node.rotation[2] + node.rotation[3] * node.rotation[3];
        require(norm > minimumQuaternionSquaredLength, "zero node quaternion");
        target.name = node.name ? node.name : "node." + std::to_string(i);
        target.parent = node.parent ? static_cast<int>(node.parent - data->nodes) : -1;
        target.local = {{node.translation[0], node.translation[1],
                         node.translation[2]}, // numbers: glTF component/corner layout.
                        Rotation3{node.rotation[3], node.rotation[0], node.rotation[1],
                                  node.rotation[2]} // numbers: glTF component/corner layout.
                            .unit(),
                        node.scale[0]};
        for (size_t child = 0; child < node.children_count; ++child)
            target.children.push_back(static_cast<size_t>(node.children[child] - data->nodes));
        if (node.mesh) {
            target.meshes = meshes.at(node.mesh);
            target.influences = influences.at(node.mesh);
        }
        if (node.skin) {
            require(node.mesh, "skin node needs a mesh");
            target.skin = static_cast<int>(node.skin - data->skins);
            const auto& skin = model->skins.at(static_cast<size_t>(target.skin));
            for (size_t part = 0; part < target.meshes.size(); ++part) {
                require(target.influences[part].size() == target.meshes[part]->size() * 3,
                        "skinned mesh lacks vertex influences"); // numbers: triangle corners.
                for (const auto& vertex : target.influences[part])
                    for (size_t j = 0; j < vertex.joints.size(); ++j)
                        require(vertex.joints[j] < skin.joints.size(),
                                "skin joint index outside skin");
            }
        }
    }
    const auto* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
    if (scene)
        for (size_t i = 0; i < scene->nodes_count; ++i)
            model->roots.push_back(static_cast<size_t>(scene->nodes[i] - data->nodes));
    else
        for (size_t i = 0; i < model->nodes.size(); ++i)
            if (model->nodes[i].parent < 0)
                model->roots.push_back(i);
    // Validate all nodes, including unused nodes, before recursive sampling.
    for (size_t i = 0; i < model->nodes.size(); ++i) {
        size_t depth = 0;
        int parent = static_cast<int>(i);
        while (parent >= 0) {
            require(++depth <= nodeHierarchyDepth, "cyclic or excessively deep node hierarchy");
            parent = model->nodes.at(static_cast<size_t>(parent)).parent;
        }
    }
    size_t keyCount = 0;
    for (size_t i = 0; i < data->animations_count; ++i) {
        const auto& sourceClip = data->animations[i];
        ModelAnimation clip;
        clip.name = sourceClip.name ? sourceClip.name : "animation." + std::to_string(i);
        std::set<std::pair<size_t, AnimationPath>> targets;
        for (size_t c = 0; c < sourceClip.channels_count; ++c) {
            const auto& channel = sourceClip.channels[c];
            const auto& sampler = *channel.sampler;
            require(channel.target_node &&
                        (sampler.interpolation == cgltf_interpolation_type_linear ||
                         sampler.interpolation == cgltf_interpolation_type_step),
                    "only LINEAR/STEP animation is supported");
            require(channel.target_path == cgltf_animation_path_type_translation ||
                        channel.target_path == cgltf_animation_path_type_rotation ||
                        channel.target_path == cgltf_animation_path_type_scale,
                    "only TRS animation paths are supported");
            AnimationTrack track;
            track.node = static_cast<size_t>(channel.target_node - data->nodes);
            track.path = channel.target_path == cgltf_animation_path_type_translation
                             ? AnimationPath::Translation
                         : channel.target_path == cgltf_animation_path_type_rotation
                             ? AnimationPath::Rotation
                             : AnimationPath::Scale;
            require(targets.emplace(track.node, track.path).second,
                    "duplicate animation channel target");
            track.step = sampler.interpolation == cgltf_interpolation_type_step;
            accessor(sampler.input, cgltf_type_scalar);
            accessor(sampler.output,
                     track.path == AnimationPath::Rotation ? cgltf_type_vec4 : cgltf_type_vec3);
            require(sampler.input->count && sampler.input->count == sampler.output->count &&
                        sampler.input->count <= resourceLimits::animationKeys - keyCount,
                    "invalid animation key count");
            keyCount += sampler.input->count;
            for (size_t key = 0; key < sampler.input->count; ++key) {
                float time = 0;
                std::array<float, 4> value{};
                require(cgltf_accessor_read_float(sampler.input, key, &time, 1) &&
                            std::isfinite(time) && time >= 0 &&
                            (track.seconds.empty() || time > track.seconds.back()),
                        "animation times must increase strictly");
                require(cgltf_accessor_read_float(sampler.output, key, value.data(),
                                                  4) && // numbers: glTF component/corner layout.
                            finite(value),
                        "invalid animation value");
                if (track.path == AnimationPath::Scale)
                    uniformScale(value.data());
                if (track.path == AnimationPath::Rotation)
                    require(value[0] * value[0] + value[1] * value[1] +
                                    value[2] * value[2] + // numbers: glTF component/corner layout.
                                    value[3] * value[3] > // numbers: glTF component/corner layout.
                                minimumQuaternionSquaredLength,
                            "zero animated quaternion");
                track.seconds.push_back(time);
                track.values.push_back(value);
                clip.durationSeconds = std::max(clip.durationSeconds, time);
            }
            clip.tracks.push_back(std::move(track));
        }
        model->animations.push_back(std::move(clip));
    }
    // Bound composed transforms for every animation, before publishing the resource.
    constexpr float importCoordinateLimit = 10000, importScaleLimit = 1000;
    std::vector<float> scales, translations;
    for (const auto& node : model->nodes) {
        scales.push_back(node.local.scale);
        translations.push_back(length(node.local.position));
    }
    for (const auto& clip : model->animations)
        for (const auto& track : clip.tracks)
            for (const auto& v : track.values) {
                if (track.path == AnimationPath::Scale)
                    scales[track.node] = std::max(scales[track.node], v[0]);
                if (track.path == AnimationPath::Translation)
                    translations[track.node] = std::max(
                        translations[track.node],
                        length({v[0], v[1], v[2]})); // numbers: glTF component/corner layout.
            }
    for (size_t i = 0; i < model->nodes.size(); ++i) {
        std::vector<size_t> path;
        for (int node = static_cast<int>(i); node >= 0;
             node = model->nodes[static_cast<size_t>(node)].parent)
            path.push_back(static_cast<size_t>(node));
        float scale = 1, positionBound = 0;
        for (auto n = path.rbegin(); n != path.rend(); ++n) {
            positionBound += scale * translations[*n];
            scale *= scales[*n];
            require(std::isfinite(scale) && scale <= importScaleLimit &&
                        std::isfinite(positionBound) && positionBound <= importCoordinateLimit,
                    "composed transform exceeds scene limits");
        }
    }
    model->byteSize =
        triangleCount * sizeof(Triangle3) + model->nodes.size() * sizeof(ModelNode) +
        keyCount * (sizeof(float) +
                    sizeof(std::array<float, 4>)); // numbers: glTF component/corner layout.
    for (const auto& texture : model->materials)
        model->byteSize += texture.pixels.size() * sizeof(Pixel);
    for (const auto& node : model->nodes)
        for (const auto& part : node.influences)
            model->byteSize += part.size() * sizeof(SkinVertex);
    for (const auto& skin : model->skins)
        model->byteSize += skin.joints.size() * (sizeof(size_t) + sizeof(skin.inverseBind.front()));
    return model;
}
} // namespace paper
