#include "paper/sdl/resources.hpp"

namespace paper::sdl {
Session::Session(bool headless) {
    if (headless) {
#if !defined(__APPLE__) && !defined(_WIN32)
        // The dummy driver has no Vulkan loader. SDL's offscreen driver does.
        check(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen"), "Select offscreen GPU driver");
#endif
        // Metal and Windows GPU backends require the native video driver even
        // when the presentation renderer uses a texture target without a window.
        check(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"), "Select headless audio driver");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        const std::string error = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("Initialize SDL: " + error);
    }
}
} // namespace paper::sdl
