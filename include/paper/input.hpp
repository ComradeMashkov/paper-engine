#pragma once
#include <SDL3/SDL_keyboard.h>
#include <array>
#include <vector>

namespace paper {
struct InputSample {
    float x = -1, y = -1;
    float clickX = -1, clickY = -1;
    float dx = 0, dy = 0;
    bool click = false, released = false, heldClick = false, focusLost = false, focusGained = false,
         windowModeChanged = false, canvasChanged = false, debugCaptured = false;
    // Widgets can opt into key repeat without repeating discrete game actions.
    SDL_Keycode key = 0, repeatedKey = 0, releasedKey = 0;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    std::array<bool, SDL_SCANCODE_COUNT> held{};
};
struct Input : InputSample {
    // Ordered edges with the pointer/modifier/held state at the event, not at poll end.
    std::vector<InputSample> events;
    // Keep lifecycle signals for the host, discard every gameplay edge/held value.
    void captureForDebug() {
        const bool lost = focusLost, gained = focusGained, canvas = canvasChanged,
                   mode = windowModeChanged;
        *this = {};
        focusLost = lost;
        focusGained = gained;
        canvasChanged = canvas;
        windowModeChanged = mode;
        debugCaptured = true;
    }
    void recordEvent() {
        auto sample = static_cast<const InputSample&>(*this);
        events.push_back(sample);
        dx = dy = 0;
        key = repeatedKey = releasedKey = 0;
        scancode = SDL_SCANCODE_UNKNOWN;
        click = released = false;
    }
};
} // namespace paper
