cmake_minimum_required(VERSION 3.25)
project(unionized_mech LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Engine-wide compile and link discipline (CLAUDE.md sections 3, 4, 7).
# Linked by our targets only. Third-party code keeps its own flags.
add_library(engine_options INTERFACE)
target_compile_options(engine_options INTERFACE
    -Wall -Wextra -Werror
    -fno-exceptions -fno-rtti
    -ffp-contract=off
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    $<$<CONFIG:Debug>:-fno-sanitize-recover=all>)
target_link_options(engine_options INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

include(dependencies)
include(compile_shaders)

# Smoke-test the FetchContent pins and network path. src/gpu reuses this target.
FetchContent_MakeAvailable(VulkanHeaders)

add_subdirectory(src/core)
add_subdirectory(src/sim)
add_subdirectory(src/platform)

add_executable(unionized_mech src/main.cpp)
target_link_libraries(unionized_mech PRIVATE platform)

# Headless determinism runner: steps the sim and prints the world hash.
add_executable(um_headless tools/headless.cpp)
target_link_libraries(um_headless PRIVATE sim)

enable_testing()

add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(sim_tests tests/sim_tests.cpp)
target_link_libraries(sim_tests PRIVATE sim)
add_test(NAME sim_tests COMMAND sim_tests)

# Module libraries land in later M0 commits:
#   add_subdirectory(src/gpu)       # instance, device, swapchain, bindless
#   add_subdirectory(src/render)    # triangle pass, interpolated draw
