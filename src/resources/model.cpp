#include "paper/resources/model.hpp"
#include <functional>

namespace paper {
MeshTransform compose(const MeshTransform& parent, const MeshTransform& child) {
    return {parent.point(child.position), (parent.rotation * child.rotation).unit(),
            parent.scale * child.scale};
}
namespace {
Rotation3 interpolate(Rotation3 a, Rotation3 b, float t) {
    a = a.unit();
    b = b.unit();
    float cosine = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (cosine < 0) {
        b = {-b.w, -b.x, -b.y, -b.z};
        cosine = -cosine;
    }
    constexpr float linearThreshold = .9995f;
    float x = 1 - t, y = t;
    if (cosine < linearThreshold) {
        const float angle = std::acos(std::clamp(cosine, -1.f, 1.f));
        x = std::sin((1 - t) * angle) / std::sin(angle);
        y = std::sin(t * angle) / std::sin(angle);
    }
    return Rotation3{a.w * x + b.w * y, a.x * x + b.x * y, a.y * x + b.y * y, a.z * x + b.z * y}
        .unit();
}
} // namespace
MeshTransform blend(const MeshTransform& from, const MeshTransform& to, float amount) {
    const auto t = std::clamp(amount, 0.f, 1.f);
    return {from.position * (1 - t) + to.position * t, interpolate(from.rotation, to.rotation, t),
            from.scale * (1 - t) + to.scale * t};
}
std::vector<MeshTransform>
ModelResource::transforms(float seconds, int animation, bool loop,
                          std::span<const float> localScales,
                          std::span<const ModelAnimationLayer> layers) const {
    if (!std::isfinite(seconds))
        throw std::invalid_argument("Non-finite animation time");
    std::vector<MeshTransform> locals;
    for (const auto& node : nodes)
        locals.push_back(node.local);
    const auto apply = [&](int clipIndex, float at, bool repeat) {
        if (clipIndex < 0)
            return;
        if (!std::isfinite(at))
            throw std::invalid_argument("Non-finite animation layer time");
        const auto& clip = animations.at(static_cast<size_t>(clipIndex));
        const float time = repeat && clip.durationSeconds > 0
                               ? std::fmod(std::max(0.f, at), clip.durationSeconds)
                               : std::clamp(at, 0.f, clip.durationSeconds);
        for (const auto& track : clip.tracks) {
            const auto end = std::upper_bound(track.seconds.begin(), track.seconds.end(), time);
            const size_t left = end == track.seconds.begin()
                                    ? 0
                                    : static_cast<size_t>(end - track.seconds.begin() - 1);
            const size_t right = std::min(left + 1, track.seconds.size() - 1);
            const float fraction =
                track.step || left == right
                    ? 0.f
                    : std::clamp((time - track.seconds[left]) /
                                     (track.seconds[right] - track.seconds[left]),
                                 0.f, 1.f);
            const auto& a = track.values[left];
            const auto& b = track.values[right];
            auto& transform = locals.at(track.node);
            if (track.path == AnimationPath::Rotation) {
                // glTF stores XYZW, Rotation3 takes WXYZ.
                const Rotation3 first{a[3], a[0], a[1], a[2]}, second{b[3], b[0], b[1], b[2]};
                transform.rotation = interpolate(first, second, fraction);
            } else if (track.path == AnimationPath::Scale)
                transform.scale = a[0] + (b[0] - a[0]) * fraction;
            else
                transform.position = {
                    a[0] + (b[0] - a[0]) * fraction, a[1] + (b[1] - a[1]) * fraction,
                    a[2] + (b[2] - a[2]) * fraction}; // numbers: glTF XYZW/XYZ layout.
        }
    };
    apply(animation, seconds, loop);
    for (const auto& layer : layers)
        apply(layer.animation, layer.seconds, false);
    if (!localScales.empty()) {
        if (localScales.size() != nodes.size())
            throw std::invalid_argument("Scale override count does not match the rig");
        for (size_t i = 0; i < locals.size(); ++i) {
            if (!std::isfinite(localScales[i]) || localScales[i] <= 0)
                throw std::invalid_argument("Invalid model scale override");
            locals[i].scale = localScales[i];
        }
    }
    std::vector<MeshTransform> result(nodes.size());
    std::function<void(size_t, MeshTransform)> visit = [&](size_t index, MeshTransform parent) {
        const auto& node = nodes.at(index);
        const auto transform = compose(parent, locals[index]);
        result[index] = transform;
        for (const auto child : node.children)
            visit(child, transform);
    };
    for (const auto root : roots)
        visit(root, {});
    return result;
}
std::vector<MeshInstance> ModelResource::sample(float seconds, int animation, bool loop) const {
    const auto poses = transforms(seconds, animation, loop);
    return sample(poses);
}
std::vector<MeshInstance> ModelResource::sample(std::span<const MeshTransform> poses) const {
    if (poses.size() != nodes.size())
        throw std::invalid_argument("Model pose count does not match its rig");
    std::vector<MeshInstance> result;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        for (size_t part = 0; part < node.meshes.size(); ++part) {
            const auto& mesh = node.meshes[part];
            if (node.skin < 0) {
                result.push_back({mesh, poses[i]});
                continue;
            }
            const auto& skin = skins.at(static_cast<size_t>(node.skin));
            const auto& influences = node.influences.at(part);
            auto deformed = *mesh;
            size_t vertex = 0;
            for (auto& triangle : deformed)
                for (auto& corner : triangle.v) {
                    Vec3 position;
                    const auto& influence = influences.at(vertex++);
                    for (size_t j = 0; j < influence.weights.size(); ++j) {
                        if (influence.weights[j] == 0)
                            continue;
                        const auto joint = influence.joints[j];
                        const auto& m = skin.inverseBind.at(joint);
                        const auto p = corner.p;
                        // numbers: affine point multiplication by a column-major glTF MAT4.
                        const Vec3 bound{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                                         m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                                         m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
                        position = position +
                                   poses[skin.joints.at(joint)].point(bound) * influence.weights[j];
                    }
                    corner.p = position;
                }
            // Skinning already produces model-space vertices. The mesh node transform
            // must not be applied a second time. GPU cache retires the previous pose.
            result.push_back({makeMesh(std::move(deformed)), {}});
        }
    }
    return result;
}
} // namespace paper
