#include "viewport.hpp"
#import <Cocoa/Cocoa.h>
namespace paper::editor {
SDL_Window* wrapNativeWindow(WId id) {
    NSView* view = reinterpret_cast<NSView*>(id);
    NSView* hostContent = [[view window] contentView];
    NSView* hostParent = [view superview];
    NSResponder* viewResponder = [view nextResponder];
    NSResponder* windowResponder = [[view window] nextResponder];
    const auto properties = SDL_CreateProperties();
    sdl::check(properties != 0, "Create native viewport properties");
    SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_CREATE_COCOA_WINDOW_POINTER, [view window]);
    SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_CREATE_COCOA_VIEW_POINTER, view);
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    auto* result = SDL_CreateWindowWithProperties(properties);
    // SDL installs notification observers; Qt retains the native responder chain.
    [view setNextResponder:viewResponder];
    [[view window] setNextResponder:windowResponder];
    SDL_DestroyProperties(properties);
    if ([[view window] contentView] != hostContent || [view superview] != hostParent) {
        SDL_DestroyWindow(result);
        throw std::runtime_error(
            "SDL changed the Qt view hierarchy; rebuild with the current Cocoa patch");
    }
    return result;
}
} // namespace paper::editor
