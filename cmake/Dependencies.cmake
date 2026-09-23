include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 URL "${PROJECT_SOURCE_DIR}/vendor/SDL-3.4.12.tar.gz"
    URL_HASH SHA256=b68381f06a7580e63400b3b6eb547ec57d8c3ebde70f9f40e0aba530ba05da27
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} "-DSDL_SOURCE=<SOURCE_DIR>"
        -P "${PROJECT_SOURCE_DIR}/cmake/PatchSDLCocoa.cmake")
FetchContent_MakeAvailable(SDL3)

# Third-party implementation is compiled once, separately from project warnings.
add_library(paper_stb STATIC "${PROJECT_SOURCE_DIR}/src/third_party/stb.cpp")
target_include_directories(paper_stb SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/vendor")
target_compile_features(paper_stb PRIVATE cxx_std_20)
set_target_properties(paper_stb PROPERTIES CXX_EXTENSIONS OFF)

# Pinned toml++ 3.4.0 (MIT); compiled once, with bounded parser nesting.
add_library(paper_toml STATIC "${PROJECT_SOURCE_DIR}/src/third_party/toml.cpp")
target_include_directories(paper_toml SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/vendor")
target_compile_features(paper_toml PRIVATE cxx_std_20)
# Must match content::limits::nesting; parser rejects excessive nesting before conversion.
set(PAPER_TOML_NESTING 64)
target_compile_definitions(paper_toml PUBLIC TOML_HEADER_ONLY=0 TOML_MAX_NESTED_VALUES=${PAPER_TOML_NESTING})
set_target_properties(paper_toml PROPERTIES CXX_EXTENSIONS OFF)

# Pinned cgltf 1.15 (MIT); parse only, with engine-owned file access and limits.
add_library(paper_cgltf STATIC "${PROJECT_SOURCE_DIR}/src/third_party/cgltf.cpp")
target_include_directories(paper_cgltf SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/vendor")
target_compile_features(paper_cgltf PRIVATE cxx_std_20)
