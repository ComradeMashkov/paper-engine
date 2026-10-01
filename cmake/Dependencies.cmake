include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 URL "${PROJECT_SOURCE_DIR}/vendor/SDL-3.4.12.tar.gz"
    URL_HASH SHA256=b68381f06a7580e63400b3b6eb547ec57d8c3ebde70f9f40e0aba530ba05da27
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL3)
# Run on every configure, including existing build trees after a patch update.
set(SDL_SOURCE "${sdl3_SOURCE_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/PatchSDLCocoa.cmake")

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

# Pinned Jolt Physics 5.6.0 (MIT). Baseline CPU ISA, no upstream applications or LTO.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
foreach(feature SSE4_1 SSE4_2 AVX AVX2 AVX512 LZCNT TZCNT F16C FMADD)
    set(USE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(paper_jolt URL "${PROJECT_SOURCE_DIR}/vendor/JoltPhysics-5.6.0.tar.gz"
    URL_HASH SHA256=6e069ee0172478cc78182047aac87e5310ba14a67a53348ae14cc37801fd3f8e
    SOURCE_SUBDIR Build DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(paper_jolt)
if(PAPER_ENABLE_SANITIZERS)
    target_compile_options(Jolt PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
