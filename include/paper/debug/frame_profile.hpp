#pragma once
#include "paper/engine.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace paper {
// Bounded RAM recording. Disk writes happen once on a normal exit, outside the frame loop.
class FrameProfile {
  public:
    explicit FrameProfile(std::filesystem::path file) : file_(std::move(file)) {
        if (!file_.empty())
            samples_.reserve(maxSamples);
    }
    void record(double interval, double game, FrameTimings world, double present, double frame) {
        if (!file_.empty() && samples_.size() < maxSamples)
            samples_.push_back({interval, game, world.world.encodeMs, world.compositeMs, present,
                                frame, world.world});
    }
    void write(std::string_view renderer, bool vsync) const {
        if (file_.empty())
            return;
        if (!file_.parent_path().empty())
            std::filesystem::create_directories(file_.parent_path());
        std::ofstream out(file_);
        if (!out)
            throw std::runtime_error("Cannot write profile: " + file_.string());
        out.imbue(std::locale::classic());
        out << "# renderer=" << renderer << ", vsync=" << vsync << '\n';
        out << "frame,interval_ms,game_ms,world_encode_ms,composite_ms,present_ms,frame_ms,draw_"
               "calls,triangles,mesh_uploads,texture_uploads,mesh_bytes,texture_bytes,light_count,"
               "shadow_passes,shadow_draw_calls,shadow_triangles,shadow_map_bytes,"
               "objects_tested,objects_culled,rooms_tested,rooms_culled\n";
        out << std::fixed << std::setprecision(csvFractionDigits);
        for (size_t i = 0; i < samples_.size(); ++i) {
            const auto& s = samples_[i];
            out << i << ',' << s.interval << ',' << s.game << ',' << s.encode << ',' << s.composite
                << ',' << s.present << ',' << s.frame << ',' << s.world.drawCalls << ','
                << s.world.triangles << ',' << s.world.meshUploads << ',' << s.world.textureUploads
                << ',' << s.world.meshBytes << ',' << s.world.textureBytes << ','
                << s.world.lightCount << ',' << s.world.shadowPasses << ','
                << s.world.shadowDrawCalls << ',' << s.world.shadowTriangles << ','
                << s.world.shadowMapBytes << ',' << s.world.objectsTested << ','
                << s.world.objectsCulled << ',' << s.world.roomsTested << ',' << s.world.roomsCulled
                << '\n';
        }
        out.close();
        if (!out)
            throw std::runtime_error("Profile write failed: " + file_.string());
    }

  private:
    struct Sample {
        double interval, game, encode, composite, present, frame;
        RenderStats world;
    };
    static constexpr int csvFractionDigits = 4;
    static constexpr size_t maxSamples = 36'000;
    std::filesystem::path file_;
    std::vector<Sample> samples_;
};
} // namespace paper
