#include "paper/audio/bank.hpp"
#include "paper/scenes/transforms.hpp"
#include <set>
namespace paper {
namespace {
constexpr size_t maximumSources = 4096, maximumZones = 256;
constexpr unsigned maximumVariants = 64;
constexpr float maximumRangeMeters = 10000, zoneTransitionSeconds = .2f, maximumFeedback = .95f,
                maximumDamping = .99f;
using Value = ContentValue;
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::invalid_argument(message);
}
void keys(const Value& v, std::initializer_list<std::string_view> allowed) {
    require(v.is_object(), "Expected audio table");
    for (const auto& [key, child] : v.items())
        require(std::ranges::find(allowed, key) != allowed.end(), "Unknown audio property: " + key);
}
template <class T> T integer(const Value& v, std::string_view key, T fallback) {
    if (!v.contains(key))
        return fallback;
    const auto& n = v.at(key);
    require(n.is_number_integer(), "Expected integral audio property: " + std::string(key));
    const auto number = n.get<int64_t>();
    require(number >= static_cast<int64_t>(std::numeric_limits<T>::lowest()) &&
                number <= static_cast<int64_t>(std::numeric_limits<T>::max()),
            "Audio integer out of range");
    return static_cast<T>(number);
}
bool unit(float v) {
    return std::isfinite(v) && v >= 0 && v <= 1;
}
Value vector(Vec3 v) {
    return Value::array({v.x, v.y, v.z});
}
Vec3 vector(const Value& v) {
    constexpr size_t components = 3;
    require(v.is_array() && v.size() == components, "Expected audio XYZ");
    for (const auto& e : v)
        require(e.is_number() && std::isfinite(e.get<float>()),
                "Expected finite audio coordinates");
    return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()}; // numbers: XYZ wire layout.
}
const SoundDefinition* sound(const AudioBankDefinition& bank, std::string_view id) {
    const auto it = std::ranges::find(bank.sounds, id, &SoundDefinition::id);
    return it == bank.sounds.end() ? nullptr : &*it;
}
constexpr std::array<std::string_view, busCount> buses{"ambience", "effects", "interface",
                                                       "voices"};
float weight(const AcousticZone& zone, Vec3 listener) {
    const auto local = zone.bounds.orientation().inverse().apply(listener - zone.bounds.center);
    const float edge =
        std::min({zone.bounds.half.x - std::abs(local.x), zone.bounds.half.y - std::abs(local.y),
                  zone.bounds.half.z - std::abs(local.z)});
    return zone.fadeMeters > 0 ? std::clamp(edge / zone.fadeMeters, 0.f, 1.f) : float(edge >= 0);
}
} // namespace
void validateAudioBank(const AudioBankDefinition& bank) {
    require(bank.sounds.size() <= AudioMixer::maximumSounds &&
                bank.sources.size() <= maximumSources && bank.zones.size() <= maximumZones,
            "Audio bank exceeds capacities");
    std::set<std::string> ids;
    for (const auto& s : bank.sounds) {
        const std::filesystem::path path(s.file);
        require(!s.id.empty() && ids.insert(s.id).second && static_cast<size_t>(s.bus) < busCount &&
                    s.variants > 0 && s.variants <= maximumVariants && unit(s.gain) &&
                    std::isfinite(s.cooldownSeconds) && s.cooldownSeconds >= 0 &&
                    s.cooldownSeconds <= audioParameters::maximumCooldownSeconds &&
                    unit(s.spatialReverbSend) && unit(s.nonSpatialReverbSend),
                "Invalid sound definition: " + s.id);
        require(!path.empty() && !path.is_absolute() && path == path.lexically_normal() &&
                    std::ranges::none_of(path,
                                         [](const auto& component) {
                                             return component == ".." || component == ".";
                                         }),
                "Audio file stem must remain inside audio/: " + s.file);
    }
    ids.clear();
    for (const auto& s : bank.sources)
        require(!s.id.empty() && ids.insert(s.id).second && sound(bank, s.sound) &&
                    finite3(s.offset) && unit(s.gain) && std::isfinite(s.rangeMeters) &&
                    s.rangeMeters > 0 && s.rangeMeters <= maximumRangeMeters,
                "Invalid audio source: " + s.id);
    ids.clear();
    for (const auto& z : bank.zones) {
        require(!z.id.empty() && ids.insert(z.id).second && finite3(z.bounds.center) &&
                    finite3(z.bounds.half) && z.bounds.half.x > 0 && z.bounds.half.y > 0 &&
                    z.bounds.half.z > 0 && std::isfinite(z.bounds.yaw) &&
                    std::isfinite(z.fadeMeters) && z.fadeMeters >= 0 && unit(z.wet) &&
                    unit(z.feedback) && z.feedback <= maximumFeedback && unit(z.damping) &&
                    z.damping <= maximumDamping,
                "Invalid acoustic zone: " + z.id);
        MeshTransform t{{}, z.bounds.rotation};
        require(t.valid(), "Invalid acoustic zone quaternion");
        require(z.ambience.empty() || (sound(bank, z.ambience) && sound(bank, z.ambience)->loop),
                "Acoustic ambience must reference a looping sound");
    }
}
AudioBankDefinition parseAudioBank(std::string_view text, std::string_view name) {
    const auto root = content::parse(text, name);
    keys(root, {"format", "version", "bank"});
    require(root.at("format") == "paper.audio" && root.at("version").is_number_integer() &&
                root.at("version") == 1,
            "Unsupported audio bank format/version");
    const auto& data = root.at("bank");
    keys(data, {"sounds", "sources", "zones"});
    AudioBankDefinition bank;
    for (const auto key : {"sounds", "sources", "zones"})
        require(!data.contains(key) || data.at(key).is_array(), "Audio entries must be arrays");
    for (const auto& v : data.at("sounds")) {
        keys(v,
             {"id", "file", "bus", "variants", "gain", "cooldown", "priority", "loop",
              "triggersDucking", "receivesDucking", "spatialReverbSend", "nonSpatialReverbSend"});
        const auto bus = v.at("bus").get<std::string>();
        const auto index = std::ranges::find(buses, bus) - buses.begin();
        SoundDefinition s{v.at("id").get<std::string>(),
                          v.at("file").get<std::string>(),
                          static_cast<AudioBus>(index),
                          integer<unsigned>(v, "variants", 1u),
                          v.value("gain", 1.f),
                          v.value("cooldown", 0.f),
                          integer<unsigned>(v, "priority", 0u)};
        s.loop = v.value("loop", false);
        s.triggersDucking = v.value("triggersDucking", true);
        s.receivesDucking = v.value("receivesDucking", false);
        s.spatialReverbSend = v.value("spatialReverbSend", s.spatialReverbSend);
        s.nonSpatialReverbSend = v.value("nonSpatialReverbSend", 0.f);
        bank.sounds.push_back(s);
    }
    for (const auto& v : data.value("sources", Value::array())) {
        keys(v, {"id", "sound", "node", "offset", "gain", "range"});
        bank.sources.push_back(
            {v.at("id").get<std::string>(), v.at("sound").get<std::string>(),
             v.value("node", std::string{}), vector(v.value("offset", Value::array({0, 0, 0}))),
             v.value("gain", 1.f), v.value("range", audioParameters::defaultRangeMeters)});
    }
    for (const auto& v : data.value("zones", Value::array())) {
        keys(v, {"id", "ambience", "center", "half", "rotation", "priority", "fade", "wet",
                 "feedback", "damping"});
        AcousticZone z;
        z.id = v.at("id").get<std::string>();
        z.ambience = v.value("ambience", std::string{});
        z.bounds = {vector(v.at("center")), vector(v.at("half"))};
        if (v.contains("rotation"))
            z.bounds.rotation = readRotation(v.at("rotation"));
        z.priority = integer<int>(v, "priority", 0);
        z.fadeMeters = v.value("fade", 1.f);
        z.wet = v.value("wet", 1.f);
        z.feedback = v.value("feedback", audioParameters::echoFeedbackGain);
        z.damping = v.value("damping", 0.f);
        bank.zones.push_back(z);
    }
    validateAudioBank(bank);
    return bank;
}
AudioBankDefinition loadAudioBank(const std::filesystem::path& path) {
    return parseAudioBank(content::encode(content::read(path)), path.string());
}
std::string writeAudioBank(const AudioBankDefinition& bank) {
    validateAudioBank(bank);
    auto data = Value::object();
    data["sounds"] = Value::array();
    data["sources"] = Value::array();
    data["zones"] = Value::array();
    for (const auto& s : bank.sounds)
        data["sounds"].elements().push_back(
            Value{{"id", s.id},
                  {"file", s.file},
                  {"bus", std::string(buses[static_cast<size_t>(s.bus)])},
                  {"variants", s.variants},
                  {"gain", s.gain},
                  {"cooldown", s.cooldownSeconds},
                  {"priority", s.priority},
                  {"loop", s.loop},
                  {"triggersDucking", s.triggersDucking},
                  {"receivesDucking", s.receivesDucking},
                  {"spatialReverbSend", s.spatialReverbSend},
                  {"nonSpatialReverbSend", s.nonSpatialReverbSend}});
    for (const auto& s : bank.sources)
        data["sources"].elements().push_back(Value{{"id", s.id},
                                                   {"sound", s.sound},
                                                   {"node", s.node},
                                                   {"offset", vector(s.offset)},
                                                   {"gain", s.gain},
                                                   {"range", s.rangeMeters}});
    for (const auto& z : bank.zones) {
        const auto q = z.bounds.orientation();
        data["zones"].elements().push_back(Value{{"id", z.id},
                                                 {"ambience", z.ambience},
                                                 {"center", vector(z.bounds.center)},
                                                 {"half", vector(z.bounds.half)},
                                                 {"rotation", Value::array({q.x, q.y, q.z, q.w})},
                                                 {"priority", z.priority},
                                                 {"fade", z.fadeMeters},
                                                 {"wet", z.wet},
                                                 {"feedback", z.feedback},
                                                 {"damping", z.damping}});
    }
    return content::encode(Value{{"format", "paper.audio"}, {"version", 1}, {"bank", data}});
}
AudioBindings::AudioBindings(AudioBankDefinition bank) : bank_(std::move(bank)) {
    validateAudioBank(bank_);
    for (const auto& z : bank_.zones)
        if (!z.ambience.empty()) {
            auto slot = std::ranges::find(occupied_, false);
            require(slot != occupied_.end(), "Acoustic ambience exceeds loop budget");
            *slot = true;
        }
    for (const auto& s : bank_.sources)
        bind(s.id, s.id, s.node);
}
void AudioBindings::bind(std::string instance, std::string_view source, std::string node) {
    require(!instance.empty() && !bindings_.contains(instance) && bindings_.size() < maximumSources,
            "Duplicate/invalid audio instance");
    const auto it = std::ranges::find(bank_.sources, source, &AudioSourceDefinition::id);
    require(it != bank_.sources.end(), "Unknown audio source");
    Binding binding{static_cast<size_t>(it - bank_.sources.begin()), std::move(node), {}};
    if (sound(bank_, it->sound)->loop) {
        auto slot = std::ranges::find(occupied_, false);
        require(slot != occupied_.end(), "Source exceeds loop budget");
        binding.slot = static_cast<size_t>(slot - occupied_.begin());
    }
    bindings_.emplace(std::move(instance), binding);
    if (binding.slot)
        occupied_[*binding.slot] = true;
}
void AudioBindings::unbind(std::string_view instance) {
    const auto it = bindings_.find(instance);
    require(it != bindings_.end(), "Unknown audio instance");
    if (it->second.slot)
        occupied_[*it->second.slot] = false;
    bindings_.erase(it);
}
std::optional<SoundPlacement> AudioBindings::placement(const Binding& binding,
                                                       const Resolve& resolve) const {
    const auto& s = bank_.sources[binding.source];
    MeshTransform transform;
    if (!binding.node.empty()) {
        const auto node = resolve(binding.node);
        if (!node)
            return {};
        transform = *node;
    }
    if (!transform.valid())
        throw std::invalid_argument("Invalid bound audio transform");
    return SoundPlacement{transform.point(s.offset), s.gain, s.rangeMeters, true};
}
std::optional<AudioLoopCommand> AudioBindings::event(std::string_view instance,
                                                     const Resolve& resolve) const {
    const auto it = bindings_.find(instance);
    require(it != bindings_.end(), "Unknown audio instance");
    const auto value = placement(it->second, resolve);
    if (!value)
        return {};
    return AudioLoopCommand{bank_.sources[it->second.source].sound, *value};
}
AudioFrame AudioBindings::frame(AudioScene scene, const Resolve& resolve, float seconds) {
    require(finite3(scene.listener) && std::isfinite(seconds) && seconds >= 0,
            "Invalid acoustic frame");
    AudioFrame frame;
    frame.scene = scene;
    float wet = 1, feedback = audioParameters::echoFeedbackGain, damping = 0, total = 0;
    int priority = std::numeric_limits<int>::min();
    for (const auto& zone : bank_.zones)
        if (weight(zone, scene.listener) > 0)
            priority = std::max(priority, zone.priority);
    for (const auto& zone : bank_.zones)
        if (zone.priority == priority) {
            const float w = weight(zone, scene.listener);
            total += w;
            wet += (zone.wet - 1) * w;
            feedback += (zone.feedback - audioParameters::echoFeedbackGain) * w;
            damping += zone.damping * w;
        }
    if (total > 1) {
        wet = 1 + (wet - 1) / total;
        feedback = audioParameters::echoFeedbackGain +
                   (feedback - audioParameters::echoFeedbackGain) / total;
        damping /= total;
    }
    size_t slot = 0;
    for (const auto& zone : bank_.zones)
        if (!zone.ambience.empty()) {
            const float gain =
                zone.priority == priority ? weight(zone, scene.listener) / std::max(1.f, total) : 0;
            SoundPlacement p;
            p.gain = gain;
            frame.loops[slot++] = {zone.ambience, p};
        }
    for (const auto& [id, binding] : bindings_)
        if (binding.slot) {
            const auto value = event(id, resolve);
            if (value)
                frame.loops[*binding.slot] = *value;
        }
    const float blend = -std::expm1(-seconds / zoneTransitionSeconds);
    wet_ += (wet - wet_) * blend;
    feedback_ += (feedback - feedback_) * blend;
    damping_ += (damping - damping_) * blend;
    frame.scene.reverbWet = wet_;
    frame.scene.reverbFeedback = feedback_;
    frame.scene.reverbDamping = damping_;
    return frame;
}
} // namespace paper
