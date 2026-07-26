# volk is the Vulkan loader. It pulls in the pinned Vulkan headers.
FetchContent_MakeAvailable(volk)

add_library(gpu STATIC
    src/gpu_instance.cpp
    src/gpu_device.cpp
    src/gpu_swapchain.cpp
    src/gpu_alloc.cpp
    src/gpu_resources.cpp
    src/gpu_rt.cpp
    src/gpu_frame.cpp
    src/gpu_renderer.cpp)

target_include_directories(gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# gpu.h exposes Vulkan handle types, so the headers are public. volk is an
# implementation detail.
target_link_libraries(gpu PUBLIC core Vulkan::Headers)
target_link_libraries(gpu PRIVATE volk)
