cmake_minimum_required(VERSION 3.28)
project(flucoma_pitch_osc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── flucoma-core (manages its own deps: Eigen, HISSTools, foonathan/memory) ──
include(FetchContent)
FetchContent_Declare(
    flucoma_core
    GIT_REPOSITORY https://github.com/flucoma/flucoma-core.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(flucoma_core)

# ── JACK ─────────────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED jack)

# ── Target ───────────────────────────────────────────────────────────────────
add_executable(flucoma_pitch_osc src/main.cpp)

target_include_directories(flucoma_pitch_osc PRIVATE
    "${flucoma_core_SOURCE_DIR}/include/flucoma"
    ${JACK_INCLUDE_DIRS}
)

target_link_libraries(flucoma_pitch_osc PRIVATE
    FLUID_DECOMPOSITION
    ${JACK_LIBRARIES}
)

# Raspberry Pi 5 (Cortex-A76)
target_compile_options(flucoma_pitch_osc PRIVATE -O3 -ffast-math)