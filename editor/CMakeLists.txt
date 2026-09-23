find_package(Qt6 6.9 REQUIRED COMPONENTS Widgets Concurrent) # numbers: minimum tested Qt baseline.
add_library(paper_editor_ui STATIC application.cpp viewport.cpp)
paper_target(paper_editor_ui)
target_link_libraries(paper_editor_ui PUBLIC Paper::Authoring Paper::Resources Paper::Runtime Qt6::Widgets PRIVATE Qt6::Concurrent)
if(APPLE)
    set_target_properties(paper_editor_ui PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS OFF) # numbers: Objective-C++ uses the same C++20 language baseline.
    target_sources(paper_editor_ui PRIVATE native_window.mm)
    target_link_libraries(paper_editor_ui PRIVATE "-framework Cocoa")
else()
    target_sources(paper_editor_ui PRIVATE native_window.cpp)
endif()
add_library(Paper::Editor ALIAS paper_editor_ui)
add_executable(paper_editor MACOSX_BUNDLE main.cpp)
paper_target(paper_editor)
target_link_libraries(paper_editor PRIVATE Paper::Editor)
target_compile_definitions(paper_editor PRIVATE PAPER_EDITOR_SHADER_DIR="${PAPER_SHADER_OUTPUT_DIR}")
add_dependencies(paper_editor paper_shaders)
if(PAPER_BUILD_TESTS)
    add_executable(paper_editor_workspace_tests ../tests/editor/workspace_tests.cpp)
    paper_target(paper_editor_workspace_tests)
    target_link_libraries(paper_editor_workspace_tests PRIVATE Paper::Editor)
    target_compile_definitions(paper_editor_workspace_tests PRIVATE PAPER_TEST_SHADER_DIR="${PAPER_SHADER_OUTPUT_DIR}")
    add_dependencies(paper_editor_workspace_tests paper_shaders)
    # Requires a caller-owned copy of the Boxes project and a native desktop/GPU;
    # deliberately excluded from automatic CTest runs.
endif()

if(APPLE)
    set_target_properties(paper_editor PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "studio.paper.editor" MACOSX_BUNDLE_BUNDLE_NAME "Paper Editor")
    add_custom_command(TARGET paper_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_CURRENT_BINARY_DIR}/paper_editor"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "paper_editor.app/Contents/MacOS/paper_editor" "${CMAKE_CURRENT_BINARY_DIR}/paper_editor")
endif()
