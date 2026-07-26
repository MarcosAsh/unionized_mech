# cgltf and stb are header-only file-format parsers, the one third-party
# category allowed beyond the OS boundary. Fetched at their pins, no upstream
# build, exposed as SYSTEM includes so they never trip -Werror.
FetchContent_MakeAvailable(cgltf stb)

add_library(asset STATIC
    src/asset_import.cpp
    src/asset_io.cpp
    src/asset_skin.cpp
    src/asset_third_party.cpp)

# The implementation-expansion TU is third-party code, compiled without warnings.
set_source_files_properties(src/asset_third_party.cpp PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(asset PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(asset SYSTEM PRIVATE ${cgltf_SOURCE_DIR} ${stb_SOURCE_DIR})

# anim is public: asset.h exposes skeletons and clips on imported models.
target_link_libraries(asset PUBLIC core anim)
