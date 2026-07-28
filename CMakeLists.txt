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

# volk provides the Vulkan entry points at runtime, so no static prototypes.
target_compile_definitions(engine_options INTERFACE VK_NO_PROTOTYPES)

include(dependencies)
include(compile_shaders)

# Smoke-test the FetchContent pins and network path. src/gpu reuses this target.
FetchContent_MakeAvailable(VulkanHeaders)

add_subdirectory(src/core)
add_subdirectory(src/sim)
add_subdirectory(src/anim)
add_subdirectory(src/asset)
add_subdirectory(src/platform)
add_subdirectory(src/gpu)
add_subdirectory(src/render)

# The one level. The game, the determinism harness, and the movement tests all
# load this file, so none of them can drift onto geometry the others never see.
set(UM_MAP_DEFINE "MAP_PATH=\"${CMAKE_SOURCE_DIR}/maps/skyline.umap\"")

add_executable(unionized_mech src/main.cpp tools/sanitizer_suppressions.cpp)
target_link_libraries(unionized_mech PRIVATE platform gpu render asset)
target_compile_definitions(unionized_mech PRIVATE "${UM_MAP_DEFINE}")

# Headless determinism runner: steps the sim and prints the world hash.
add_executable(um_headless tools/headless.cpp)
target_link_libraries(um_headless PRIVATE sim)
target_compile_definitions(um_headless PRIVATE "${UM_MAP_DEFINE}")

# Headless Vulkan bring-up check: real device, validation active, no window.
add_executable(gpu_probe tools/gpu_probe.cpp)
target_link_libraries(gpu_probe PRIVATE gpu)

# Offline asset importer: glTF to the native formats.
add_executable(um_import tools/um_import.cpp)
target_link_libraries(um_import PRIVATE asset)

include(sample_assets)
add_dependencies(unionized_mech sample_assets)

enable_testing()

add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(sim_tests tests/sim_tests.cpp)
target_link_libraries(sim_tests PRIVATE sim)
target_compile_definitions(sim_tests PRIVATE "${UM_MAP_DEFINE}")
add_test(NAME sim_tests COMMAND sim_tests)

add_executable(math_tests tests/math_tests.cpp)
target_link_libraries(math_tests PRIVATE core)
add_test(NAME math_tests COMMAND math_tests)

add_executable(asset_tests tests/asset_tests.cpp)
target_link_libraries(asset_tests PRIVATE asset)
add_test(NAME asset_tests COMMAND asset_tests)

add_executable(anim_tests tests/anim_tests.cpp)
target_link_libraries(anim_tests PRIVATE anim)
add_test(NAME anim_tests COMMAND anim_tests)

# Still to land in M0:
#   gpu swapchain, frame contexts, allocator, bindless set (next: clear + present)
#   add_subdirectory(src/render)    # triangle pass, interpolated draw
