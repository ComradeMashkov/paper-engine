#include "paper/engine.hpp"
#include <algorithm>

namespace paper {
bool Engine::poll() {
    debugOverlay_.beginPoll();
    input_.debugCaptured = false;
    input_.events.clear();
    input_.canvasChanged = false;
    refreshCanvas();
    input_.click = false;
    input_.clickX = input_.clickY = -1;
    input_.released = false;
    input_.key = 0;
    input_.repeatedKey = 0;
    input_.releasedKey = 0;
    input_.dx = 0;
    input_.dy = 0;
    input_.focusLost = false;
    input_.focusGained = false;
    input_.windowModeChanged = false;
    SDL_Event e{};
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            refreshCanvas();
        // Preserve raw relative motion for the camera before logical UI conversion.
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            input_.dx += e.motion.xrel;
            input_.dy += e.motion.yrel;
        }
        if (!headless_)
            sdl::check(SDL_ConvertEventToRenderCoordinates(renderer_.get(), &e),
                       "Convert input coordinates");
        if (e.type == SDL_EVENT_QUIT)
            quit_ = true;
        if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
            e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
            fullscreen_ = (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_FULLSCREEN) != 0;
            input_.windowModeChanged = true;
        }
        if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
            input_.focusGained = true;
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            input_.focusLost = true;
            captureMouse(false);
        }
        auto debugEvent = e;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            debugEvent.button.x -= canvasOffset_.x;
            debugEvent.button.y -= canvasOffset_.y;
        }
        if (debugOverlay_.event(*this, debugEvent))
            continue;
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            input_.x = e.motion.x - canvasOffset_.x;
            input_.y = e.motion.y - canvasOffset_.y;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            input_.click = true;
            input_.heldClick = true;
            input_.x = e.button.x - canvasOffset_.x;
            input_.y = e.button.y - canvasOffset_.y;
            input_.clickX = input_.x;
            input_.clickY = input_.y;
            input_.recordEvent();
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            input_.released = true;
            input_.heldClick = false;
            input_.x = e.button.x - canvasOffset_.x;
            input_.y = e.button.y - canvasOffset_.y;
            input_.recordEvent();
        }
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
            input_.scancode = e.key.scancode;
            if (e.key.scancode > SDL_SCANCODE_UNKNOWN && e.key.scancode < SDL_SCANCODE_COUNT)
                input_.held[e.key.scancode] = e.type == SDL_EVENT_KEY_DOWN;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.repeat)
            input_.repeatedKey = e.key.key;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            input_.key = e.key.key;
        }
        if (e.type == SDL_EVENT_KEY_UP)
            input_.releasedKey = e.key.key;
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP)
            input_.recordEvent();
    }
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    input_.held.fill(false);
    for (int i = 0; i < std::min(count, static_cast<int>(input_.held.size())); ++i)
        input_.held[i] = keys[i];
    input_.heldClick = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
    debugOverlay_.finishPoll(input_);
    return !quit_;
}
} // namespace paper
