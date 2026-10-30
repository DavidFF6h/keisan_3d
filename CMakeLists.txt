cmake_minimum_required(VERSION 3.16)
project(keisan_3d CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimisations matter for a renderer and ESPECIALLY for a benchmark.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)

# --- common: everything the programs share ----------------------------------
add_library(common STATIC
    common/rasterizer.cpp
    common/mesh.cpp
    common/protocol.cpp
    common/parser.cpp
    common/simd_eval.cpp
)
target_include_directories(common PUBLIC common)

# Windows only: sockets live in the Winsock library, and the Windows headers define
# min()/max() macros that would break std::min / std::max unless told not to.
if(WIN32)
    target_link_libraries(common PUBLIC ws2_32)
    target_compile_definitions(common PUBLIC NOMINMAX)
endif()

# Turn on the CPU's vector instructions if the compiler knows how. On an Intel/AMD
# machine this enables AVX2; on Apple Silicon the flag is rejected and we use NEON,
# which the compiler enables by default.
include(CheckCXXCompilerFlag)
if(MSVC)
    target_compile_options(common PRIVATE /arch:AVX2)
else()
    check_cxx_compiler_flag(-mavx2 COMPILER_SUPPORTS_AVX2)
    if(COMPILER_SUPPORTS_AVX2)
        target_compile_options(common PRIVATE -mavx2)
    endif()
endif()

# --- the programs ----------------------------------------------------------
add_executable(server server/main.cpp)
target_link_libraries(server PRIVATE common Threads::Threads)

add_executable(bench bench/main.cpp)
target_link_libraries(bench PRIVATE common)

# A sandbox for the tutorial's checkpoints: paste a small test program into
# playground/main.cpp, build, and run it. Delete these two lines when you're done with it.
add_executable(playground playground/main.cpp)
target_link_libraries(playground PRIVATE common)

# --- Dear ImGui, compiled from its source files ------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL2::SDL2)

add_executable(client client/main.cpp)
target_link_libraries(client PRIVATE common imgui Threads::Threads
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>)
