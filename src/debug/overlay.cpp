#include "paper/debug/overlay.hpp"
#include "paper/core/units.hpp"
#include "paper/core/utf8.hpp"
#include "paper/engine.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace paper {
namespace {
// Engine canvas units; independent of game menus/artwork and native output resolution.
constexpr Rect panel{56, 64, 1328, 772}, settingsRows{92, 160, 1256, 52},
    consoleRows{80, 160, 1280, 572}, graphBox{1032, 74, 380, 112};
constexpr float inset = 24, rowTextInset = 12, lineHeight = 22, labelGap = 12, checkboxSize = 18,
                checkboxMarkInset = 4, hudWidth = 404, hudRowHeight = 23;
constexpr int titleSize = 26, bodySize = 18, logSize = 16, numberPrecision = 1;
constexpr size_t consolePageLines = 26, optionCount = 8, levelIndex = 6, consoleIndex = 7;
constexpr double referenceFps = 60, lowerReferenceFps = 30;
constexpr double bytesPerMiB = 1024 * 1024;
constexpr Color background{12, 18, 26, 242}, foreground{223, 233, 235, 255},
    muted{146, 165, 178, 255}, accent{103, 226, 181, 255}, selected{37, 65, 75, 255},
    warning{255, 179, 99, 255}, grid{63, 80, 93, 255};
constexpr std::array optionLabels{"FPS counter (F1)",
                                  "Frame-time graph",
                                  "Renderer and resource statistics",
                                  "Input log (visible at trace level)",
                                  "Slow-frame warnings (> 50 ms, at most once/second)",
                                  "Mirror SDL messages to the process log",
                                  "Minimum console level",
                                  "Open console (F4)"};
constexpr std::array optionFields{&DebugSettings::fps,         &DebugSettings::graph,
                                  &DebugSettings::renderStats, &DebugSettings::inputLog,
                                  &DebugSettings::spikes,      &DebugSettings::mirrorLog};
static_assert(consoleRows.h >= consolePageLines * lineHeight);
static_assert(settingsRows.y + optionCount * settingsRows.h < panel.y + panel.h - inset);
std::string number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(numberPrecision) << value;
    return out.str();
}
void backspace(std::string& text) {
    if (text.empty())
        return;
    size_t offset = 0, last = 0;
    while (offset < text.size()) {
        last = offset;
        (void)nextCodepoint(text, offset);
    }
    text.resize(last);
}
// Measure a bounded line using binary search over codepoint boundaries, not bytes.
size_t fit(Engine& engine, std::string_view text, float width, int size) {
    std::vector<size_t> ends;
    for (size_t offset = 0; offset < text.size();) {
        (void)nextCodepoint(text, offset);
        ends.push_back(offset);
    }
    size_t low = 0, high = ends.size();
    while (low < high) {
        const auto middle = low + (high - low) / 2; // numbers: binary search midpoint.
        if (engine.measure(text.substr(0, ends[middle]), size) <= width)
            low = middle + 1;
        else
            high = middle;
    }
    return low ? ends[low - 1] : (ends.empty() ? 0 : ends.front());
}
void wrap(Engine& engine, std::string text, std::vector<std::string>& lines) {
    std::string_view remaining(text);
    while (!remaining.empty()) {
        auto count = fit(engine, remaining, consoleRows.w, logSize);
        lines.emplace_back(remaining.substr(0, count));
        remaining.remove_prefix(count);
    }
}
void header(Engine& engine, std::string_view title, std::string_view help) {
    engine.rect(panel, background);
    engine.outline(panel, grid);
    engine.text(title, panel.x + inset, panel.y + inset, titleSize, accent);
    engine.text(help, panel.x + inset, panel.y + inset + titleSize + labelGap, logSize, muted);
}
} // namespace
void DebugOverlay::configure(const std::filesystem::path& file) {
    settingsFile_ = file;
    std::error_code error;
    if (file.empty())
        return;
    const bool exists = std::filesystem::exists(file, error);
    if (error || (exists && !diagnostics_.loadSettings(file)))
        diagnostics_.log(LogLevel::Warning, "debug",
                         "Cannot read diagnostics settings; keeping current options");
}
void DebugOverlay::persist() {
    if (!settingsFile_.empty() && !diagnostics_.saveSettings(settingsFile_))
        diagnostics_.log(LogLevel::Error, "debug", "Cannot save diagnostics settings");
}
void DebugOverlay::beginPoll() {
    blockedPoll_ = modal();
}
void DebugOverlay::page(Engine& engine, DebugPage next) {
    if (page_ == next)
        return;
    blockedPoll_ = true;
    const bool wasOpen = modal();
    if (!wasOpen) {
        restoreCapture_ = engine.mouseCaptured();
        engine.captureMouse(false);
    }
    if (page_ == DebugPage::Console)
        engine.textInput(false);
    page_ = next;
    if (next == DebugPage::Console) {
        engine.textInput(true);
        historyPosition_ = diagnostics_.history().size();
    }
    if (!modal()) {
        int count = 0;
        const bool* held = SDL_GetKeyboardState(&count);
        for (int i = 0; i < std::min(count, static_cast<int>(suppressedKeys_.size())); ++i)
            suppressedKeys_[i] = held[i];
        suppressedMouse_ = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
        if (restoreCapture_ && engine.focused())
            engine.captureMouse(true);
        restoreCapture_ = false;
    }
}
void DebugOverlay::activate(Engine& engine) {
    const auto previous = diagnostics_.settings();
    auto options = previous;
    if (selected_ < optionFields.size())
        options.*(optionFields[selected_]) = !(options.*(optionFields[selected_]));
    else if (selected_ == levelIndex) {
        const auto next = options.minimumLevel == LogLevel::Error
                              ? LogLevel::Trace
                              : static_cast<LogLevel>(static_cast<int>(options.minimumLevel) + 1);
        options.minimumLevel = next;
    } else if (selected_ == consoleIndex)
        page(engine, DebugPage::Console);
    diagnostics_.settings(options);
    if (options != previous)
        persist();
}
void DebugOverlay::consoleKey(SDL_Keycode key) {
    const auto& history = diagnostics_.history();
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        const auto previous = diagnostics_.settings();
        diagnostics_.execute(command_);
        command_.clear();
        draft_.clear();
        historyPosition_ = history.size();
        scroll_ = 0;
        if (diagnostics_.settings() != previous)
            persist();
    } else if (key == SDLK_BACKSPACE)
        backspace(command_);
    else if (key == SDLK_UP && historyPosition_ > 0) {
        if (historyPosition_ == history.size())
            draft_ = command_;
        command_ = history[--historyPosition_];
    } else if (key == SDLK_DOWN && historyPosition_ < history.size()) {
        ++historyPosition_;
        command_ = historyPosition_ == history.size() ? draft_ : history[historyPosition_];
    } else if (key == SDLK_TAB) {
        const auto matches = diagnostics_.completions(command_);
        if (matches.size() == 1)
            command_ = matches.front() + " ";
        else
            for (const auto& name : matches)
                diagnostics_.log(LogLevel::Info, "console", name);
    } else if (key == SDLK_PAGEUP)
        scroll_ += consolePageLines;
    else if (key == SDLK_PAGEDOWN)
        scroll_ -= std::min(scroll_, consolePageLines);
    else if (key == SDLK_END)
        scroll_ = 0;
}
bool DebugOverlay::event(Engine& engine, const SDL_Event& event) {
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        restoreCapture_ = false;
        suppressedKeys_.fill(false);
        suppressedMouse_ = false;
    }
    const bool keyEvent = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP;
    if (keyEvent && event.key.scancode > SDL_SCANCODE_UNKNOWN &&
        event.key.scancode < SDL_SCANCODE_COUNT && suppressedKeys_[event.key.scancode]) {
        if (event.type == SDL_EVENT_KEY_UP)
            suppressedKeys_[event.key.scancode] = false;
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT &&
        suppressedMouse_) {
        suppressedMouse_ = false;
        return true;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        if (event.key.key == SDLK_F1) {
            auto options = diagnostics_.settings();
            options.fps = !options.fps;
            diagnostics_.settings(options);
            persist();
            return true;
        }
        if (event.key.key == SDLK_F2 || event.key.key == SDLK_F4) {
            const auto next = event.key.key == SDLK_F2 ? DebugPage::Settings : DebugPage::Console;
            page(engine, page_ == next ? DebugPage::Closed : next);
            return true;
        }
        if (event.key.key == SDLK_ESCAPE && modal()) {
            page(engine, DebugPage::Closed);
            return true;
        }
    }
    if (!modal()) {
        if (diagnostics_.settings().inputLog) {
            if (keyEvent)
                diagnostics_.log(LogLevel::Trace, "input",
                                 std::string(event.type == SDL_EVENT_KEY_DOWN ? "down " : "up ") +
                                     SDL_GetScancodeName(event.key.scancode) +
                                     (event.key.repeat ? " repeat" : ""));
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                     event.type == SDL_EVENT_MOUSE_BUTTON_UP)
                diagnostics_.log(LogLevel::Trace, "input",
                                 std::string(event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                                 ? "button down "
                                                 : "button up ") +
                                     std::to_string(event.button.button));
            else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                     event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
                diagnostics_.log(LogLevel::Info, "input",
                                 event.type == SDL_EVENT_WINDOW_FOCUS_LOST ? "Focus lost"
                                                                           : "Focus gained");
        }
        return blockedPoll_;
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (page_ == DebugPage::Console) {
            // Repeating edits/navigation is allowed; commands execute only on a fresh edge.
            if (!event.key.repeat || (event.key.key != SDLK_RETURN &&
                                      event.key.key != SDLK_KP_ENTER && event.key.key != SDLK_TAB))
                consoleKey(event.key.key);
        } else if (event.key.key == SDLK_UP)
            selected_ = (selected_ + optionCount - 1) % optionCount;
        else if (event.key.key == SDLK_DOWN)
            selected_ = (selected_ + 1) % optionCount;
        else if (!event.key.repeat &&
                 (event.key.key == SDLK_RETURN || event.key.key == SDLK_SPACE ||
                  event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT))
            activate(engine);
    }
    if (page_ == DebugPage::Console && event.type == SDL_EVENT_TEXT_INPUT && event.text.text) {
        const std::string_view text(event.text.text);
        for (size_t offset = 0; offset < text.size();) {
            const auto previous = offset;
            const auto codepoint = nextCodepoint(text, offset);
            if (codepoint >= U' ' && codepoint != U'\x7f' &&
                command_.size() + offset - previous <= debugLimits::commandBytes)
                command_.append(text.substr(previous, offset - previous));
        }
    }
    if (page_ == DebugPage::Console && event.type == SDL_EVENT_MOUSE_WHEEL) {
        const auto rows = static_cast<size_t>(std::ceil(std::abs(event.wheel.y)));
        if (event.wheel.y > 0)
            scroll_ += rows;
        else
            scroll_ -= std::min(scroll_, rows);
    }
    if (page_ == DebugPage::Settings && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT) {
        const Rect options{settingsRows.x, settingsRows.y, settingsRows.w,
                           settingsRows.h * optionCount};
        if (options.contains(event.button.x, event.button.y)) {
            selected_ = static_cast<size_t>((event.button.y - settingsRows.y) / settingsRows.h);
            activate(engine);
        }
    }
    return true;
}
void DebugOverlay::finishPoll(Input& input) {
    for (size_t i = 0; i < suppressedKeys_.size(); ++i) {
        suppressedKeys_[i] = suppressedKeys_[i] && input.held[i];
        if (suppressedKeys_[i])
            input.held[i] = false;
    }
    suppressedMouse_ &= input.heldClick;
    if (suppressedMouse_)
        input.heldClick = false;
    if (blockedPoll_)
        input.captureForDebug();
}
void DebugOverlay::drawSettings(Engine& engine) {
    header(engine, "Engine diagnostics",
           "Up/Down + Enter or click   |   F2 / Esc close   |   F4 console");
    const auto options = diagnostics_.settings();
    for (size_t i = 0; i < optionCount; ++i) {
        const auto y = settingsRows.y + static_cast<float>(i) * settingsRows.h;
        if (i == selected_)
            engine.rect({settingsRows.x, y, settingsRows.w, settingsRows.h}, selected);
        const auto labelX = settingsRows.x + inset;
        if (i < optionFields.size()) {
            const Rect box{labelX, y + rowTextInset, checkboxSize, checkboxSize};
            engine.outline(box, accent);
            if (options.*(optionFields[i])) {
                // Draw inside the box independently of font bearings and baseline.
                const float left = box.x + checkboxMarkInset,
                            right = box.x + box.w - checkboxMarkInset,
                            top = box.y + checkboxMarkInset,
                            bottom = box.y + box.h - checkboxMarkInset;
                engine.line(left, top, right, bottom, accent);
                engine.line(left, bottom, right, top, accent);
            }
        }
        std::string label(optionLabels[i]);
        if (i == levelIndex)
            label += ": " + std::string(logLevelName(options.minimumLevel));
        engine.text(label, labelX + checkboxSize + labelGap, y + rowTextInset, bodySize,
                    foreground);
    }
    engine.text("The application is paused while this panel or the console is open.",
                panel.x + inset, panel.y + panel.h - inset - lineHeight, bodySize, muted);
}
void DebugOverlay::drawConsole(Engine& engine) {
    header(engine, "Engine console",
           "Enter run | Up/Down history | Tab complete | PgUp/PgDn/wheel scroll | End latest | "
           "F4/Esc close");
    const auto options = diagnostics_.settings();
    auto entries = diagnostics_.logs(LogLevel::Trace);
    // Filters apply to runtime messages; command feedback must remain discoverable.
    std::erase_if(entries, [&](const LogEntry& entry) {
        return entry.category != "console" &&
               (entry.level < options.minimumLevel ||
                (!diagnostics_.filter().empty() &&
                 entry.category.find(diagnostics_.filter()) == std::string::npos &&
                 entry.message.find(diagnostics_.filter()) == std::string::npos));
    });
    const auto sequence = entries.empty() ? 0 : entries.back().sequence;
    if (sequence != cachedSequence_ || entries.size() != cachedLogCount_ ||
        cachedLevel_ != options.minimumLevel || cachedFilter_ != diagnostics_.filter()) {
        const auto oldSize = logLines_.size();
        logLines_.clear();
        for (const auto& entry : entries)
            wrap(engine,
                 "[" + number(entry.seconds) + "] [" + std::string(logLevelName(entry.level)) +
                     "] " + entry.category + ": " + entry.message,
                 logLines_);
        if (scroll_ && logLines_.size() > oldSize)
            scroll_ += logLines_.size() - oldSize;
        cachedSequence_ = sequence;
        cachedLogCount_ = entries.size();
        cachedLevel_ = options.minimumLevel;
        cachedFilter_ = diagnostics_.filter();
    }
    scroll_ = std::min(
        scroll_, logLines_.size() > consolePageLines ? logLines_.size() - consolePageLines : 0);
    const auto end = logLines_.size() - scroll_;
    const auto start = end > consolePageLines ? end - consolePageLines : 0;
    for (size_t i = start; i < end; ++i)
        engine.text(logLines_[i], consoleRows.x,
                    consoleRows.y + static_cast<float>(i - start) * lineHeight, logSize,
                    foreground);
    const auto footerY = consoleRows.y + consoleRows.h + labelGap;
    engine.text("level=" + std::string(logLevelName(options.minimumLevel)) + " | filter=" +
                    (diagnostics_.filter().empty() ? "(all)" : diagnostics_.filter()) +
                    " | overwritten=" + std::to_string(diagnostics_.droppedLogs()) + " | " +
                    (scroll_ ? "SCROLLED" : "LIVE"),
                consoleRows.x, footerY, logSize, muted);
    std::string visible = command_;
    while (!visible.empty() && engine.measure("> " + visible + "_", bodySize) > consoleRows.w) {
        size_t offset = 0;
        (void)nextCodepoint(visible, offset);
        visible.erase(0, offset);
    }
    engine.text("> " + visible + "_", consoleRows.x, footerY + lineHeight, bodySize, accent);
}
void DebugOverlay::draw(Engine& engine) {
    const auto options = diagnostics_.settings();
    if (options.fps || options.graph || options.renderStats) {
        const auto summary = diagnostics_.summary();
        const auto samples = diagnostics_.frames();
        const auto latest = samples.empty() ? DebugFrame{} : samples.back();
        const float hudX = Engine::width - hudWidth;
        const float graphBottom = graphBox.y + graphBox.h + labelGap;
        const float statsTop = options.graph ? graphBottom : graphBox.y;
        engine.text((options.fps ? number(summary.fps) + " FPS  /  " : "Frame time: ") +
                        number(latest.intervalMs) + " ms",
                    hudX + labelGap, labelGap, bodySize, accent);
        engine.text("p95 " + number(summary.p95Ms) + " ms  / max " + number(summary.maximumMs) +
                        " ms",
                    hudX + labelGap, labelGap + hudRowHeight, logSize, muted);
        if (options.graph) {
            const double ceiling =
                std::max(debugLimits::graphMinimumMilliseconds, summary.maximumMs);
            const auto graphY = [&](double ms) {
                return graphBox.y +
                       graphBox.h * (1 - static_cast<float>(std::clamp(ms / ceiling, 0., 1.)));
            };
            for (const double fps : {referenceFps, lowerReferenceFps}) {
                const auto y = graphY(units::millisecondsPerSecond / fps);
                engine.line(graphBox.x, y, graphBox.x + graphBox.w, y, grid);
            }
            engine.outline(graphBox, grid);
            engine.text(number(ceiling) + " ms", graphBox.x, graphBox.y, logSize, muted);
            const auto step = graphBox.w / static_cast<float>(debugLimits::frameSamples - 1);
            for (size_t i = 1; i < samples.size(); ++i)
                engine.line(
                    graphBox.x + static_cast<float>(i - 1) * step,
                    graphY(samples[i - 1].intervalMs), graphBox.x + static_cast<float>(i) * step,
                    graphY(samples[i].intervalMs),
                    samples[i].intervalMs > debugLimits::spikeMilliseconds ? warning : accent);
        }
        if (options.renderStats) {
            const auto stats = engine.frameTimings().world;
            const auto size = engine.worldRenderSize();
            const char* driver = engine.rendererName();
            const std::array lines{
                std::string(driver ? driver : "unknown") + " | VSync " +
                    (engine.vsyncEnabled() ? "on" : "off") + " | " + std::to_string(size.width) +
                    "x" + std::to_string(size.height),
                "CPU update " + number(latest.updateMs) + " / present " + number(latest.presentMs) +
                    " ms",
                "CPU encode " + number(latest.encodeMs) + " / compose " +
                    number(latest.compositeMs) + " ms",
                "World draws " + std::to_string(stats.drawCalls) + " / triangles " +
                    std::to_string(stats.triangles),
                "Mesh " + number(static_cast<double>(stats.meshBytes) / bytesPerMiB) + " / tex " +
                    number(static_cast<double>(stats.textureBytes) / bytesPerMiB) + " MiB",
                "Lights " + std::to_string(stats.lightCount) + " / shadow draws " +
                    std::to_string(stats.shadowDrawCalls),
                "Culled " + std::to_string(stats.objectsCulled) + "/" +
                    std::to_string(stats.objectsTested) + " / particles " +
                    std::to_string(stats.particles)};
            float y = statsTop;
            for (const auto& line : lines) {
                engine.text(line, hudX + labelGap, y, logSize, foreground);
                y += hudRowHeight;
            }
        }
    }
    if (page_ == DebugPage::Settings)
        drawSettings(engine);
    else if (page_ == DebugPage::Console)
        drawConsole(engine);
}
} // namespace paper
