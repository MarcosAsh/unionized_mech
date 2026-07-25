# volk is the Vulkan loader. It pulls in the pinned Vulkan headers.
FetchContent_MakeAvailable(volk)

add_library(gpu STATIC
    gpu_instance.cpp
    gpu_device.cpp)

target_include_directories(gpu PUBLIC ${CMAKE_SOURCE_DIR}/src)

# gpu.h exposes Vulkan handle types, so the headers are public. volk is an
# implementation detail.
target_link_libraries(gpu PUBLIC core Vulkan::Headers)
target_link_libraries(gpu PRIVATE volk)
