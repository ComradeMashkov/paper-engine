#include "viewport.hpp"
namespace paper::editor {
SDL_Window* wrapNativeWindow(WId id) {
    const auto properties = SDL_CreateProperties();
    sdl::check(properties != 0, "Create native viewport properties");
#if defined(_WIN32)
    SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER,
                           reinterpret_cast<void*>(id));
#elif defined(__linux__)
    if (std::string_view(SDL_GetCurrentVideoDriver()) != "x11") {
        SDL_DestroyProperties(properties);
        throw std::runtime_error("This editor viewport currently requires X11 on Linux");
    }
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X11_WINDOW_NUMBER,
                          static_cast<Sint64>(id));
#else
    SDL_DestroyProperties(properties);
    throw std::runtime_error("Native Qt/SDL viewport is unavailable on this platform");
#endif
    auto* result = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
    return result;
}
} // namespace paper::editor
