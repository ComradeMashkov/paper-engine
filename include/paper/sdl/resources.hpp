#pragma once
#include <SDL3/SDL.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace paper::sdl {
template <class T, auto Destroy> struct Deleter {
    void operator()(T* resource) const noexcept { Destroy(resource); }
};
template <class T, auto Destroy> using Owner = std::unique_ptr<T, Deleter<T, Destroy>>;
using Window = Owner<SDL_Window, SDL_DestroyWindow>;
using Renderer = Owner<SDL_Renderer, SDL_DestroyRenderer>;
using GPUDevice = Owner<SDL_GPUDevice, SDL_DestroyGPUDevice>;
using Texture = Owner<SDL_Texture, SDL_DestroyTexture>;
using Cursor = Owner<SDL_Cursor, SDL_DestroyCursor>;
using Surface = Owner<SDL_Surface, SDL_DestroySurface>;
using AudioStream = Owner<SDL_AudioStream, SDL_DestroyAudioStream>;
using String = Owner<char, SDL_free>;

inline void check(bool success, std::string_view operation) {
    if (!success)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

class Session {
  public:
    explicit Session(bool headless);
    ~Session() noexcept { SDL_Quit(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
};
} // namespace paper::sdl
