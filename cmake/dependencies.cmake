# Third-party dependencies, pinned by exact tag (CLAUDE.md section 2 and 7).
#
# Only the OS boundary and external file-format parsers are allowed here.
# Declares live in one place; each module calls FetchContent_MakeAvailable for
# what it needs, so a fresh scaffold configure stays fast and pulls nothing it
# does not use yet.

include(FetchContent)

# vulkan-sdk-1.4.341.0 matches the installed loader, validation layer, and
# vulkan-tools on the dev box. We request Vulkan 1.3 core at runtime regardless.
set(VULKAN_HEADERS_TAG "vulkan-sdk-1.4.341.0")
set(VOLK_TAG           "vulkan-sdk-1.4.341.0")
set(SDL_TAG            "release-3.4.12")
set(CGLTF_TAG          "v1.15")
set(STB_COMMIT         "31c1ad37456438565541f4919958214b6e762fb4")

FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
    GIT_TAG        ${VULKAN_HEADERS_TAG}
    GIT_SHALLOW    TRUE)

# Used by src/gpu (M0 commit 5). volk is the loader; we do not link libvulkan.
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk
    GIT_TAG        ${VOLK_TAG}
    GIT_SHALLOW    TRUE)

# Used by src/platform (M0 commit 3).
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL
    GIT_TAG        ${SDL_TAG}
    GIT_SHALLOW    TRUE)

# Used by src/asset (M2). Header-only; SOURCE_SUBDIR points at a directory with
# no CMakeLists so MakeAvailable fetches without building upstream's project.
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf
    GIT_TAG        ${CGLTF_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  fetch-only)

# Used by src/asset (M2). stb has no release tags, so it is pinned by commit.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb
    GIT_TAG        ${STB_COMMIT}
    SOURCE_SUBDIR  fetch-only)
