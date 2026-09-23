#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace paper {
namespace debugLimits {
// Bounded memory, independent of session duration; timings are in milliseconds.
inline constexpr size_t frameSamples = 240, logEntries = 512, messageBytes = 2048,
                        categoryBytes = 64, commandBytes = 256, commandHistory = 64;
inline constexpr double spikeMilliseconds = 50, graphMinimumMilliseconds = 40;
inline constexpr int settingsVersion = 1;
inline constexpr std::uintmax_t settingsBytes = 4096;
} // namespace debugLimits
enum class LogLevel { Trace, Info, Warning, Error };
[[nodiscard]] std::string_view logLevelName(LogLevel level);
struct DebugSettings {
    bool fps = false, graph = false, renderStats = false, inputLog = false, spikes = false,
         mirrorLog = true;
    LogLevel minimumLevel = LogLevel::Info;
    bool operator==(const DebugSettings&) const = default;
};
struct LogEntry {
    std::uint64_t sequence = 0;
    double seconds = 0;
    LogLevel level = LogLevel::Info;
    std::string category, message;
};
struct DebugFrame {
    double intervalMs = 0, updateMs = 0, encodeMs = 0, compositeMs = 0, presentMs = 0;
};
struct FrameSummary {
    double fps = 0, meanMs = 0, p95Ms = 0, maximumMs = 0;
    size_t samples = 0;
};
// Frame samples and commands belong to the main thread. Logging/settings snapshots are
// synchronized so worker/SDL log callbacks never touch UI state or the command registry.
class Diagnostics {
  public:
    using Command = std::function<std::string(std::string_view)>;
    Diagnostics();
    [[nodiscard]] DebugSettings settings() const;
    void settings(DebugSettings value);
    void log(LogLevel level, std::string_view category, std::string_view message);
    [[nodiscard]] std::vector<LogEntry> logs(LogLevel minimum, std::string_view filter = {}) const;
    void clearLogs();
    [[nodiscard]] std::uint64_t droppedLogs() const;
    void record(DebugFrame frame);
    [[nodiscard]] std::vector<DebugFrame> frames() const;
    [[nodiscard]] FrameSummary summary() const;
    void clearFrames();
    // Names are unique. Applications can extend this registry without an engine->game link.
    bool addCommand(std::string name, std::string help, Command command);
    void execute(std::string_view line);
    [[nodiscard]] std::vector<std::string> completions(std::string_view prefix) const;
    [[nodiscard]] const std::deque<std::string>& history() const { return history_; }
    [[nodiscard]] const std::string& filter() const { return filter_; }
    bool loadSettings(const std::filesystem::path& path);
    [[nodiscard]] bool saveSettings(const std::filesystem::path& path) const;

  private:
    struct RegisteredCommand {
        std::string help;
        Command run;
    };
    mutable std::mutex mutex_;
    DebugSettings settings_;
    std::deque<LogEntry> logs_;
    std::uint64_t sequence_ = 0, dropped_ = 0;
    std::array<DebugFrame, debugLimits::frameSamples> frames_{};
    size_t nextFrame_ = 0, frameCount_ = 0;
    std::map<std::string, RegisteredCommand, std::less<>> commands_;
    std::deque<std::string> history_;
    std::string filter_;
};
} // namespace paper
