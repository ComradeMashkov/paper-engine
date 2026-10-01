#include "paper/audio/audio.hpp"
#include "paper/audio/bank.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace paper;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        if (!ok) {
            ++failures;
            std::cerr << text << '\n';
        }
    };
    auto rejects = [](auto f) {
        try {
            f();
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    const auto original = R"(format="paper.audio"
version=1
[bank]
[[bank.sounds]]
id="hum"
file="machine/hum"
bus="ambience"
loop=true
gain=0.5
[[bank.sounds]]
id="click"
file="click"
bus="effects"
[[bank.sources]]
id="motor"
sound="hum"
node="one"
offset=[1,0,0]
[[bank.sources]]
id="switch"
sound="click"
[[bank.zones]]
id="room"
center=[0,1,0]
half=[4,2,4]
wet=0.3
feedback=0.4
damping=0.2
fade=1
ambience="hum"
)";
    const auto bank = parseAudioBank(original);
    check(bank.sounds.size() == 2 && bank.sources.size() == 2,
          "external bank parses sounds and bindings");
    const auto restored = parseAudioBank(writeAudioBank(bank));
    check(restored.sounds[0].file == "machine/hum" && restored.zones[0].wet == bank.zones[0].wet,
          "bank writer round trip");
    AudioBindings bindings(bank);
    bindings.bind("second", "motor", "two");
    const auto resolve = [](std::string_view node) -> std::optional<MeshTransform> {
        if (node == "one")
            return MeshTransform{{2, 0, 0}, Rotation3::axisAngle({0, 1, 0}, pi3 / 2)};
        if (node == "two")
            return MeshTransform{{-2, 0, 0}, {}};
        return {};
    };
    AudioScene scene;
    scene.listener = {0, 1, 0};
    scene.active = true;
    auto frame = bindings.frame(scene, resolve, 1);
    check(frame.loops[1].sound == "hum" && frame.loops[2].sound == "hum",
          "independent instances have separate loop slots");
    check(length(frame.loops[1].placement.position - Vec3{2, 0, -1}) < .0001f &&
              length(frame.loops[2].placement.position - Vec3{-1, 0, 0}) < .0001f,
          "bindings apply independent node transforms and offsets");
    check(std::abs(frame.scene.reverbWet - .3f) < .01f &&
              std::abs(frame.scene.reverbFeedback - .4f) < .01f,
          "acoustic zone publishes smoothed reverb profile");
    scene.listener = {10, 1, 0};
    auto outside = bindings.frame(scene, resolve, 1);
    check(outside.loops[0].placement.gain == 0 && outside.scene.reverbWet > .99f,
          "leaving zone fades ambience and returns default profile");
    bindings.unbind("second");
    auto cleared = bindings.frame(scene, resolve, 0);
    check(cleared.loops[2].sound.empty(), "removed source clears its slot");
    auto missing = bindings.frame(
        scene, [](std::string_view) -> std::optional<MeshTransform> { return {}; }, 0);
    check(missing.loops[1].sound.empty(), "missing scene node clears loop without stale sound");
    check(bindings.event("switch", resolve)->sound == "click",
          "one-shot event resolves through common source binding");
    auto bad = bank;
    bad.sounds[0].file = "../outside";
    check(rejects([&] { validateAudioBank(bad); }), "bank paths cannot escape audio root");
    bad = bank;
    bad.sounds[0].variants = 0;
    check(rejects([&] { validateAudioBank(bad); }), "zero variants rejected");
    bad = bank;
    bad.sources[0].sound = "missing";
    check(rejects([&] { validateAudioBank(bad); }), "unknown sound binding rejected");
    bad = bank;
    bad.zones[0].feedback = 1;
    check(rejects([&] { validateAudioBank(bad); }), "unstable feedback rejected");
    bad = bank;
    bad.sounds.push_back(bank.sounds[0]);
    check(rejects([&] { validateAudioBank(bad); }), "duplicate sound IDs rejected");
    for (size_t k = 2; k < AudioMixer::maxLoops; ++k)
        bindings.bind("copy." + std::to_string(k), "motor", "two");
    check(rejects([&] { bindings.bind("overflow", "motor"); }),
          "loop budget fails explicitly without evicting a source");
    bad = bank;
    for (size_t k = 0; k < 17; ++k) {
        auto source = bank.sources[0];
        source.id = "authored." + std::to_string(k);
        bad.sources.push_back(source);
    }
    check(rejects([&] { validateAudioBank(bad); }),
          "authored looping sources and zone ambience share the same capacity");
    SoundBank pcm;
    pcm.resize(bank.sounds.size());
    for (size_t k = 0; k < pcm.size(); ++k)
        pcm[k].push_back(std::vector<float>(4096, .1f));
    AudioMixer mixer(bank.sounds, std::move(pcm));
    mixer.setScene(frame.scene);
    mixer.setMix(1, {1, 1, 1, 1});
    for (size_t k = 0; k < frame.loops.size(); ++k)
        mixer.loop(k, frame.loops[k].sound, frame.loops[k].placement);
    std::array<float, 2048> output{};
    mixer.render(output);
    check(std::ranges::all_of(output, [](float v) { return std::isfinite(v) && std::abs(v) <= 1; }),
          "prepared frame renders finite bounded PCM without a device");
    {
        const auto root =
            std::filesystem::temp_directory_path() /
            ("paper-audio-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        } cleanup{root};
        std::filesystem::create_directories(root / "audio");
        auto wave = [&](const std::filesystem::path& path) {
            std::ofstream out(path, std::ios::binary);
            auto le = [&](uint32_t value, unsigned bytes) {
                for (unsigned k = 0; k < bytes; ++k)
                    out.put(static_cast<char>((value >> (8 * k)) & 255));
            };
            out.write("RIFF", 4);
            le(36 + 1024, 4);
            out.write("WAVEfmt ", 8);
            le(16, 4);
            le(1, 2);
            le(1, 2);
            le(48000, 4);
            le(96000, 4);
            le(2, 2);
            le(16, 2);
            out.write("data", 4);
            le(1024, 4);
            for (unsigned k = 0; k < 512; ++k)
                le(k % 2 ? 8000 : static_cast<uint16_t>(-8000), 2);
        };
        wave(root / "audio" / "tone-1.wav");
        AudioBankDefinition decoded;
        auto tone = bank.sounds[1];
        tone.id = "tone";
        tone.file = "tone";
        decoded.sounds = {tone};
        const auto convertedPcm = loadSoundBank(root, decoded);
        check(convertedPcm.size() == 1 && convertedPcm[0].size() == 1 &&
                  convertedPcm[0][0].size() > 460 && convertedPcm[0][0].size() < 480,
              "strict WAV decoding checks all PCM variants without a device");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        {
            Audio device(false);
            device.openBank(root, decoded);
            check(device.available(), "strict bank opens the dummy SDL audio device");
            std::ofstream(root / "audio" / "tone-1.wav", std::ios::trunc) << "invalid WAV";
            check(rejects([&] { device.openBank(root, decoded); }) && device.available(),
                  "malformed replacement fails before changing a live stream");
        }
        decoded.sounds[0].variants = 2;
        wave(root / "audio" / "tone-1.wav");
        check(rejects([&] { (void)loadSoundBank(root, decoded); }),
              "every declared variant is required");
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    {
        auto effect = bank.sounds[1];
        effect.nonSpatialReverbSend = 1;
        SoundBank wave(1);
        wave[0].push_back(std::vector<float>(512, .1f));
        AudioMixer dry(std::span(&effect, 1), wave), wet(std::span(&effect, 1), wave);
        AudioScene scene;
        scene.active = true;
        scene.reverbWet = 0;
        dry.setScene(scene);
        scene.reverbWet = 1;
        wet.setScene(scene);
        dry.setMix(1, {1, 1, 1, 1});
        wet.setMix(1, {1, 1, 1, 1});
        check(dry.play(effect.id) && wet.play(effect.id), "one-shot admitted to both mixers");
        std::array<float, 512> a{}, b{};
        float tail = 0, finalTail = 0;
        for (int block = 0; block < 200; ++block) {
            dry.render(a);
            wet.render(b);
            if (block > 8 && block < 20)
                for (size_t k = 0; k < a.size(); ++k)
                    tail += std::abs(a[k] - b[k]);
            if (block == 199)
                for (float value : b)
                    finalTail += std::abs(value);
        }
        check(tail > .001f && finalTail < .0001f,
              "wet zones produce an audible echo that decays back to silence");
        wave[0][0][0] = std::numeric_limits<float>::quiet_NaN();
        check(rejects([&] { AudioMixer malformed(std::span(&effect, 1), wave); }),
              "non-finite PCM cannot enter the callback");
    }
    bad = bank;
    bad.zones[0].damping = 1;
    check(rejects([&] { validateAudioBank(bad); }),
          "damping cannot freeze feedback into a permanent tail");
    check(rejects([&] {
              (void)parseAudioBank("format='paper.audio'\nversion=1\n[bank]\nsounds={}\n");
          }),
          "non-array bank entries rejected");
    std::cout << "audio bank failures=" << failures << '\n';
    return failures ? 1 : 0;
}
