# SDL 3.4.12 accepts an external NSView but its Metal path still uses the whole
# NSWindow content view. Keep the upstream archive intact; apply these bounded,
# checked substitutions only in FetchContent's extracted build-tree copy.
function(paper_replace file before after)
    file(READ "${file}" source)
    string(FIND "${source}" "${after}" already_patched)
    if(NOT already_patched EQUAL -1)
        return()
    endif()
    string(FIND "${source}" "${before}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Pinned SDL Cocoa patch no longer matches ${file}")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")
    file(WRITE "${file}" "${source}")
endfunction()
paper_replace("${SDL_SOURCE}/src/video/cocoa/SDL_cocoametalview.m"
    "NSView *view = data.nswindow.contentView;"
    "NSView *view = data.sdlContentView; // Paper: honor the external viewport.")
paper_replace("${SDL_SOURCE}/src/video/cocoa/SDL_cocoametalview.m"
    "initWithFrame:view.frame"
    "initWithFrame:view.bounds")
paper_replace("${SDL_SOURCE}/src/video/cocoa/SDL_cocoawindow.m"
    "*w = (int)windata.viewport.size.width;\n        *h = (int)windata.viewport.size.height;"
    "// Paper: a Qt dock resize need not resize its top-level NSWindow.\n        NSRect viewport = [windata.sdlContentView bounds];\n        if (window->flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) {\n            viewport = [windata.sdlContentView convertRectToBacking:viewport];\n        }\n        *w = (int)viewport.size.width;\n        *h = (int)viewport.size.height;")

# Embedding must never replace Qt's complete content view with just its viewport.
paper_replace("${SDL_SOURCE}/src/video/cocoa/SDL_cocoawindow.m"
    "[nswindow setContentView:nsview];"
    "if (!(window->flags & SDL_WINDOW_EXTERNAL)) { // Paper: host owns the view tree.\n            [nswindow setContentView:nsview];\n        }")
