#pragma once
#include "paper/debug/diagnostics.hpp"
#include "paper/input.hpp"
#include <SDL3/SDL_events.h>

namespace paper {
class Engine;
enum class DebugPage { Closed, Settings, Console };
class DebugOverlay {
  public:
    explicit DebugOverlay(Diagnostics& diagnostics) : diagnostics_(diagnostics) {}
    void configure(const std::filesystem::path& file);
    void beginPoll();
    bool event(Engine& engine, const SDL_Event& event);
    void finishPoll(Input& input);
    void draw(Engine& engine);
    [[nodiscard]] bool modal() const { return page_ != DebugPage::Closed; }

  private:
    void page(Engine& engine, DebugPage next);
    void persist();
    void activate(Engine& engine);
    void consoleKey(SDL_Keycode key);
    void drawSettings(Engine& engine);
    void drawConsole(Engine& engine);
    Diagnostics& diagnostics_;
    DebugPage page_ = DebugPage::Closed;
    bool blockedPoll_ = false, restoreCapture_ = false;
    size_t selected_ = 0, scroll_ = 0, historyPosition_ = 0;
    std::string command_, draft_;
    std::array<bool, SDL_SCANCODE_COUNT> suppressedKeys_{};
    bool suppressedMouse_ = false;
    std::filesystem::path settingsFile_;
    std::vector<std::string> logLines_;
    std::uint64_t cachedSequence_ = 0;
    size_t cachedLogCount_ = 0;
    LogLevel cachedLevel_ = LogLevel::Info;
    std::string cachedFilter_;
};
} // namespace paper
