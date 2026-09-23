#include "paper/debug/diagnostics.hpp"
#include "paper/core/units.hpp"
#include "paper/core/utf8.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

namespace paper {
namespace {
constexpr double p95Fraction = .95;
constexpr int displayPrecision = 1;
const auto logEpoch = std::chrono::steady_clock::now();
std::string bounded(std::string_view text, size_t limit) {
    size_t end = 0, offset = 0;
    while (offset < text.size()) {
        (void)nextCodepoint(text, offset);
        if (offset > limit)
            break;
        end = offset;
    }
    std::string result(text.substr(0, end));
    for (char& c : result)
        if (static_cast<unsigned char>(c) < ' ')
            c = ' ';
    return result;
}
std::string_view trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
        return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
constexpr std::array levelNames{std::string_view("trace"), std::string_view("info"),
                                std::string_view("warning"), std::string_view("error")};
struct Toggle {
    std::string_view name, help;
    bool DebugSettings::* field;
};
constexpr std::array toggles{
    Toggle{"fps", "FPS counter", &DebugSettings::fps},
    Toggle{"graph", "Frame-time graph", &DebugSettings::graph},
    Toggle{"stats", "Renderer counters and CPU timings", &DebugSettings::renderStats},
    Toggle{"inputlog", "Key/button and focus messages (no text input)", &DebugSettings::inputLog},
    Toggle{"spikes", "Warnings for frames over 50 ms", &DebugSettings::spikes},
    Toggle{"mirror", "Mirror SDL messages to the process log", &DebugSettings::mirrorLog}};
} // namespace
std::string_view logLevelName(LogLevel level) {
    const auto index = static_cast<size_t>(level);
    return index < levelNames.size() ? levelNames[index] : "unknown";
}
Diagnostics::Diagnostics() {
    for (const auto& toggle : toggles)
        addCommand(std::string(toggle.name), std::string(toggle.help) + " [on|off]",
                   [this, field = toggle.field, name = toggle.name](std::string_view value) {
                       auto options = settings();
                       if (value == "on")
                           options.*field = true;
                       else if (value == "off")
                           options.*field = false;
                       else if (!value.empty())
                           return std::string("Usage: ") + std::string(name) + " [on|off]";
                       settings(options);
                       return std::string(name) + (options.*field ? " = on" : " = off");
                   });
    addCommand("help", "List engine/application commands", [this](std::string_view) {
        for (const auto& [name, command] : commands_)
            log(LogLevel::Info, "console", name + " - " + command.help);
        return std::string{};
    });
    addCommand("clear", "Clear the log", [this](std::string_view) {
        clearLogs();
        return std::string{};
    });
    addCommand("filter", "Filter log category/message by text; empty resets",
               [this](std::string_view text) {
                   filter_ = bounded(text, debugLimits::commandBytes);
                   return "Filter: " + (filter_.empty() ? "(all)" : filter_);
               });
    addCommand("level", "Minimum visible severity: trace|info|warning|error",
               [this](std::string_view name) {
                   const auto it = std::ranges::find(levelNames, name);
                   if (it == levelNames.end())
                       return std::string("Usage: level trace|info|warning|error");
                   auto options = settings();
                   options.minimumLevel = static_cast<LogLevel>(it - levelNames.begin());
                   settings(options);
                   return std::string("Visible level: ") + std::string(name);
               });
    addCommand("timings", "Report rolling FPS, p95 and worst frame", [this](std::string_view) {
        const auto value = summary();
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(displayPrecision) << value.fps << " FPS; mean "
            << value.meanMs << " ms; p95 " << value.p95Ms << " ms; max " << value.maximumMs
            << " ms; " << value.samples << " samples";
        return out.str();
    });
    addCommand("reset_timings", "Clear frame history after changing settings",
               [this](std::string_view) {
                   clearFrames();
                   return std::string("Frame history cleared");
               });
    addCommand("reset_debug", "Restore diagnostic defaults", [this](std::string_view) {
        settings({});
        filter_.clear();
        return std::string("Diagnostic defaults restored");
    });
}
DebugSettings Diagnostics::settings() const {
    const std::lock_guard lock(mutex_);
    return settings_;
}
void Diagnostics::settings(DebugSettings value) {
    if (static_cast<size_t>(value.minimumLevel) >= levelNames.size())
        value.minimumLevel = LogLevel::Info;
    const std::lock_guard lock(mutex_);
    settings_ = value;
}
void Diagnostics::log(LogLevel level, std::string_view category, std::string_view message) {
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - logEpoch);
    const std::lock_guard lock(mutex_);
    if (logs_.size() == debugLimits::logEntries) {
        logs_.pop_front();
        ++dropped_;
    }
    logs_.push_back({++sequence_, seconds.count(), level,
                     bounded(category, debugLimits::categoryBytes),
                     bounded(message, debugLimits::messageBytes)});
}
std::vector<LogEntry> Diagnostics::logs(LogLevel minimum, std::string_view filter) const {
    const std::lock_guard lock(mutex_);
    std::vector<LogEntry> result;
    for (const auto& entry : logs_)
        if (entry.level >= minimum &&
            (filter.empty() || entry.category.find(filter) != std::string::npos ||
             entry.message.find(filter) != std::string::npos))
            result.push_back(entry);
    return result;
}
void Diagnostics::clearLogs() {
    const std::lock_guard lock(mutex_);
    logs_.clear();
    dropped_ = 0;
}
std::uint64_t Diagnostics::droppedLogs() const {
    const std::lock_guard lock(mutex_);
    return dropped_;
}
void Diagnostics::record(DebugFrame frame) {
    if (!std::isfinite(frame.intervalMs) || frame.intervalMs <= 0)
        return;
    for (double* value : {&frame.updateMs, &frame.encodeMs, &frame.compositeMs, &frame.presentMs})
        if (!std::isfinite(*value) || *value < 0)
            *value = 0;
    frames_[nextFrame_] = frame;
    nextFrame_ = (nextFrame_ + 1) % frames_.size();
    frameCount_ = std::min(frameCount_ + 1, frames_.size());
}
std::vector<DebugFrame> Diagnostics::frames() const {
    std::vector<DebugFrame> result;
    result.reserve(frameCount_);
    const auto start = (nextFrame_ + frames_.size() - frameCount_) % frames_.size();
    for (size_t i = 0; i < frameCount_; ++i)
        result.push_back(frames_[(start + i) % frames_.size()]);
    return result;
}
FrameSummary Diagnostics::summary() const {
    auto samples = frames();
    FrameSummary result;
    result.samples = samples.size();
    if (samples.empty())
        return result;
    double total = 0;
    for (const auto& frame : samples)
        total += frame.intervalMs;
    result.meanMs = total / static_cast<double>(samples.size());
    result.fps = units::millisecondsPerSecond / result.meanMs;
    std::ranges::sort(samples, {}, &DebugFrame::intervalMs);
    const auto percentile =
        static_cast<size_t>(std::ceil(p95Fraction * static_cast<double>(samples.size()))) - 1;
    result.p95Ms = samples[percentile].intervalMs;
    result.maximumMs = samples.back().intervalMs;
    return result;
}
void Diagnostics::clearFrames() {
    nextFrame_ = frameCount_ = 0;
}
bool Diagnostics::addCommand(std::string name, std::string help, Command command) {
    if (name.empty() || name.size() > debugLimits::commandBytes || !command ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.") != std::string::npos)
        return false;
    return commands_
        .emplace(std::move(name), RegisteredCommand{std::move(help), std::move(command)})
        .second;
}
void Diagnostics::execute(std::string_view line) {
    line = trim(line);
    if (line.empty())
        return;
    if (line.size() > debugLimits::commandBytes) {
        log(LogLevel::Warning, "console", "Command exceeds input limit");
        return;
    }
    // Copy before changing history: a caller may be executing a history entry.
    const std::string owned(line);
    if (history_.empty() || history_.back() != owned) {
        if (history_.size() == debugLimits::commandHistory)
            history_.pop_front();
        history_.push_back(owned);
    }
    log(LogLevel::Info, "console", "> " + owned);
    const auto split = owned.find_first_of(" \t");
    const std::string_view text(owned);
    const auto found = commands_.find(text.substr(0, split));
    if (found == commands_.end()) {
        log(LogLevel::Warning, "console", "Unknown command; use help");
        return;
    }
    try {
        const auto result = found->second.run(
            split == std::string::npos ? std::string_view{} : trim(text.substr(split)));
        if (!result.empty())
            log(LogLevel::Info, "console", result);
    } catch (const std::exception& error) {
        log(LogLevel::Error, "console", error.what());
    }
}
std::vector<std::string> Diagnostics::completions(std::string_view prefix) const {
    std::vector<std::string> result;
    for (const auto& [name, command] : commands_)
        if (name.starts_with(prefix))
            result.push_back(name);
    return result;
}
bool Diagnostics::loadSettings(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::file_size(path, error) > debugLimits::settingsBytes || error)
        return false;
    std::ifstream in(path);
    std::string magic;
    int version = 0;
    if (!(in >> magic >> version) || (magic != "PAPER_DEBUG" && magic != "DCMO_DEBUG") ||
        version != debugLimits::settingsVersion)
        return false;
    DebugSettings candidate;
    std::set<std::string> seen;
    std::string key;
    int value = 0;
    while (in >> key) {
        if (!(in >> value) || !seen.insert(key).second)
            return false;
        if (key == "level") {
            if (value < 0 || static_cast<size_t>(value) >= levelNames.size())
                return false;
            candidate.minimumLevel = static_cast<LogLevel>(value);
        } else {
            const auto it = std::ranges::find(toggles, key, &Toggle::name);
            if (it == toggles.end() || (value != 0 && value != 1))
                return false;
            candidate.*(it->field) = value != 0;
        }
    }
    if (!in.eof() || seen.size() != toggles.size() + 1)
        return false;
    settings(candidate);
    return true;
}
bool Diagnostics::saveSettings(const std::filesystem::path& path) const {
    if (path.empty())
        return false;
    const auto options = settings();
    std::error_code error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    out << "PAPER_DEBUG " << debugLimits::settingsVersion << '\n';
    for (const auto& toggle : toggles)
        out << toggle.name << ' ' << options.*(toggle.field) << '\n';
    out << "level " << static_cast<int>(options.minimumLevel) << '\n';
    out.close();
    if (!out)
        return false;
    std::filesystem::rename(temporary, path, error);
    return !error;
}
} // namespace paper
