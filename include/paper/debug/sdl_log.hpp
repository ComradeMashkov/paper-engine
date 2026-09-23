#pragma once
#include "paper/debug/diagnostics.hpp"
#include <SDL3/SDL_log.h>

namespace paper {
// One SDL session owns this adapter; restore the host callback before Diagnostics dies.
class SdlLogCapture {
  public:
    explicit SdlLogCapture(Diagnostics& diagnostics) : diagnostics_(diagnostics) {
        SDL_GetLogOutputFunction(&previous_, &previousData_);
        SDL_SetLogOutputFunction(receive, this);
    }
    ~SdlLogCapture() {
        SDL_LogOutputFunction current = nullptr;
        void* data = nullptr;
        SDL_GetLogOutputFunction(&current, &data);
        if (current == receive && data == this)
            SDL_SetLogOutputFunction(previous_, previousData_);
    }
    SdlLogCapture(const SdlLogCapture&) = delete;
    SdlLogCapture& operator=(const SdlLogCapture&) = delete;

  private:
    static std::string categoryName(int category) {
        switch (category) {
        case SDL_LOG_CATEGORY_APPLICATION:
            return "app";
        case SDL_LOG_CATEGORY_ERROR:
            return "error";
        case SDL_LOG_CATEGORY_ASSERT:
            return "assert";
        case SDL_LOG_CATEGORY_SYSTEM:
            return "system";
        case SDL_LOG_CATEGORY_AUDIO:
            return "audio";
        case SDL_LOG_CATEGORY_VIDEO:
            return "video";
        case SDL_LOG_CATEGORY_RENDER:
            return "render";
        case SDL_LOG_CATEGORY_INPUT:
            return "input";
        case SDL_LOG_CATEGORY_GPU:
            return "gpu";
        default:
            return "SDL/" + std::to_string(category);
        }
    }
    static void SDLCALL receive(void* data, int category, SDL_LogPriority priority,
                                const char* message) noexcept {
        auto& self = *static_cast<SdlLogCapture*>(data);
        bool mirror = true;
        try {
            const auto level = priority >= SDL_LOG_PRIORITY_ERROR  ? LogLevel::Error
                               : priority == SDL_LOG_PRIORITY_WARN ? LogLevel::Warning
                               : priority == SDL_LOG_PRIORITY_INFO ? LogLevel::Info
                                                                   : LogLevel::Trace;
            self.diagnostics_.log(level, categoryName(category), message ? message : "");
            mirror = self.diagnostics_.settings().mirrorLog;
        } catch (...) {
            // A log callback must not throw across SDL or recursively call SDL_Log.
        }
        if (mirror && self.previous_)
            self.previous_(self.previousData_, category, priority, message);
    }
    Diagnostics& diagnostics_;
    SDL_LogOutputFunction previous_ = nullptr;
    void* previousData_ = nullptr;
};
} // namespace paper
