# Sample content for M2 development: the Khronos glTF sample Duck, downloaded
# pinned by hash and converted to the native formats by um_import at build
# time. Nothing here is committed; shipping content replaces this later.

set(DUCK_URL
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Duck/glTF-Binary/Duck.glb")
set(DUCK_SHA256 "65bf938f54d6073e619e76e007820bbf980cdc3dc0daec0d94830ffc4ae54ab5")
set(DUCK_GLB "${CMAKE_BINARY_DIR}/sample/Duck.glb")

if(NOT EXISTS "${DUCK_GLB}")
    message(STATUS "Downloading sample asset: Duck.glb")
    file(DOWNLOAD "${DUCK_URL}" "${DUCK_GLB}" EXPECTED_HASH SHA256=${DUCK_SHA256})
endif()

set(ASSET_DIR "${CMAKE_BINARY_DIR}/assets")
file(MAKE_DIRECTORY "${ASSET_DIR}")

add_custom_command(
    OUTPUT "${ASSET_DIR}/duck.umesh" "${ASSET_DIR}/duck.utex"
    COMMAND um_import "${DUCK_GLB}" "${ASSET_DIR}/duck"
    DEPENDS um_import "${DUCK_GLB}"
    COMMENT "um_import duck"
    VERBATIM)
add_custom_target(sample_assets DEPENDS "${ASSET_DIR}/duck.umesh")
