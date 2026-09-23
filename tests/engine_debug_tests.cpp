#include "paper/debug/diagnostics.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace paper;
namespace {
constexpr double fastMs = 10, slowMs = 50, tolerance = .0001;
constexpr size_t fixtureSamples = 20;
constexpr int workerCount = 4, messagesPerWorker = 200;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void frameHistory() {
    Diagnostics debug;
    debug.record({});
    debug.record({std::numeric_limits<double>::quiet_NaN()});
    check(debug.summary().samples == 0, "invalid frame intervals are rejected");
    for (size_t i = 1; i < fixtureSamples; ++i)
        debug.record({fastMs});
    debug.record({slowMs});
    const auto summary = debug.summary();
    const auto expectedMean = (fastMs * (fixtureSamples - 1) + slowMs) / fixtureSamples;
    check(std::abs(summary.meanMs - expectedMean) < tolerance && summary.p95Ms == fastMs &&
              summary.maximumMs == slowMs,
          "nearest-rank p95 and mean retain a real frame spike");
    debug.clearFrames();
    for (size_t i = 0; i <= debugLimits::frameSamples; ++i)
        debug.record({static_cast<double>(i + 1)});
    const auto frames = debug.frames();
    check(frames.size() == debugLimits::frameSamples && frames.front().intervalMs == 2 &&
              frames.back().intervalMs == debugLimits::frameSamples + 1,
          "frame ring preserves the newest samples in order"); // numbers: first overwritten sample
                                                               // is 1.
}
void logging() {
    Diagnostics debug;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < workerCount; ++worker)
        workers.emplace_back([&] {
            for (int message = 0; message < messagesPerWorker; ++message)
                debug.log(LogLevel::Trace, "worker", "concurrent message");
        });
    for (auto& worker : workers)
        worker.join();
    const auto entries = debug.logs(LogLevel::Trace);
    check(entries.size() == debugLimits::logEntries &&
              debug.droppedLogs() == workerCount * messagesPerWorker - debugLimits::logEntries,
          "concurrent log producers respect the ring bound");
    for (size_t i = 1; i < entries.size(); ++i)
        check(entries[i - 1].sequence < entries[i].sequence, "log IDs remain ordered");
    debug.log(LogLevel::Error, "resource", "missing mesh");
    check(debug.logs(LogLevel::Warning, "mesh").size() == 1 &&
              debug.logs(LogLevel::Trace, "resource").size() == 1,
          "filter matches message/category and severity");
    debug.clearLogs();
    std::string longText(debugLimits::messageBytes - 1, 'x');
    longText += "я";
    debug.log(LogLevel::Info, "utf8", longText);
    check(debug.logs(LogLevel::Info).front().message.size() == debugLimits::messageBytes - 1,
          "message cap does not split a UTF-8 codepoint");
}
void commandsAndSettings() {
    Diagnostics debug;
    check(debug.addCommand("app.echo", "Echo application context",
                           [](std::string_view args) { return std::string(args); }),
          "application extends registry");
    check(!debug.addCommand("fps", "duplicate", [](std::string_view) { return std::string{}; }),
          "application cannot override built-ins accidentally");
    debug.execute(" fps on ");
    debug.execute("graph on");
    debug.execute("level warning");
    check(debug.settings().fps && debug.settings().graph &&
              debug.settings().minimumLevel == LogLevel::Warning,
          "commands change settings");
    debug.execute("fps maybe");
    check(debug.settings().fps, "invalid argument leaves setting intact");
    debug.execute("app.echo fixture");
    check(debug.logs(LogLevel::Info, "fixture").size() >= 1, "application result is logged");
    debug.execute("unknown");
    check(!debug.logs(LogLevel::Warning, "Unknown").empty(), "unknown command is visible");
    check(debug.completions("app.") == std::vector<std::string>{"app.echo"}, "name completion");
    for (size_t i = 0; i <= debugLimits::commandHistory; ++i)
        debug.execute("app.echo " + std::to_string(i));
    check(debug.history().size() == debugLimits::commandHistory, "history is bounded");
    const auto path = std::filesystem::temp_directory_path() / "dcmo-debug-settings-fixture.cfg";
    check(debug.saveSettings(path), "persist diagnostics independently of player settings");
    Diagnostics loaded;
    check(loaded.loadSettings(path) && loaded.settings() == debug.settings(), "settings roundtrip");
    const auto before = loaded.settings();
    {
        std::ofstream out(path, std::ios::app);
        out << "fps 0\n";
    }
    check(!loaded.loadSettings(path) && loaded.settings() == before,
          "duplicate or corrupt settings never partially apply");
    std::filesystem::remove(path);
}
} // namespace
int main() {
    try {
        frameHistory();
        logging();
        commandsAndSettings();
        std::cout << "Engine diagnostic contracts passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
