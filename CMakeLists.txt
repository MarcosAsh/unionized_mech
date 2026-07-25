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

add_executable(unionized_mech src/main.cpp)
target_link_libraries(unionized_mech PRIVATE engine_options)
target_include_directories(unionized_mech PRIVATE src)

# Module libraries land in later M0 commits:
#   add_subdirectory(src/core)      # commit 2
#   add_subdirectory(src/platform)  # commit 3
#   add_subdirectory(src/sim)       # commit 4
#   add_subdirectory(src/gpu)       # commits 5-6
#   add_subdirectory(src/render)    # commit 7
