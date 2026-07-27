# Game content, downloaded pinned by hash and converted to the native formats
# by um_import at build time. Nothing here is committed. All CC0: the Khronos
# sample Duck, Kenney's Blaster Kit (kenney.nl), and the Quaternius Animated
# Robot (quaternius.com, via poly.pizza).

set(DUCK_URL
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Duck/glTF-Binary/Duck.glb")
set(DUCK_SHA256 "65bf938f54d6073e619e76e007820bbf980cdc3dc0daec0d94830ffc4ae54ab5")
set(DUCK_GLB "${CMAKE_BINARY_DIR}/sample/Duck.glb")

if(NOT EXISTS "${DUCK_GLB}")
    message(STATUS "Downloading sample asset: Duck.glb")
    file(DOWNLOAD "${DUCK_URL}" "${DUCK_GLB}" EXPECTED_HASH SHA256=${DUCK_SHA256})
endif()

# The first-person weapon, from Kenney's CC0 Blaster Kit.
set(BLASTER_URL
    "https://kenney.nl/media/pages/assets/blaster-kit/261d80a716-1753959510/kenney_blaster-kit_2.1.zip")
set(BLASTER_SHA256 "91e3093e95427d59625e7e2ce2d0399b861600160fd0b4ada7714796b67cea8c")
set(BLASTER_ZIP "${CMAKE_BINARY_DIR}/sample/blaster-kit.zip")
set(BLASTER_GLB "${CMAKE_BINARY_DIR}/sample/blaster/Models/GLB format/blaster-d.glb")

if(NOT EXISTS "${BLASTER_GLB}")
    message(STATUS "Downloading sample asset: Blaster Kit")
    file(DOWNLOAD "${BLASTER_URL}" "${BLASTER_ZIP}" EXPECTED_HASH SHA256=${BLASTER_SHA256})
    file(ARCHIVE_EXTRACT INPUT "${BLASTER_ZIP}"
         DESTINATION "${CMAKE_BINARY_DIR}/sample/blaster")
endif()

# The character, the Quaternius CC0 Animated Robot: rigged, with clips for the
# animation milestone.
set(ROBOT_URL "https://static.poly.pizza/7d95dbce-8c73-489b-8298-f430b1f0dbdf.glb")
set(ROBOT_SHA256 "54f1a6999cca701cdc2f8fb67bcd9a283e1154cb80cac356f031bed7c2e74cbe")
set(ROBOT_GLB "${CMAKE_BINARY_DIR}/sample/Robot.glb")

if(NOT EXISTS "${ROBOT_GLB}")
    message(STATUS "Downloading sample asset: Robot.glb")
    file(DOWNLOAD "${ROBOT_URL}" "${ROBOT_GLB}" EXPECTED_HASH SHA256=${ROBOT_SHA256})
endif()

set(ASSET_DIR "${CMAKE_BINARY_DIR}/assets")
file(MAKE_DIRECTORY "${ASSET_DIR}")

add_custom_command(
    OUTPUT "${ASSET_DIR}/duck.umesh"
    COMMAND um_import "${DUCK_GLB}" "${ASSET_DIR}/duck"
    DEPENDS um_import "${DUCK_GLB}"
    COMMENT "um_import duck"
    VERBATIM)
add_custom_command(
    OUTPUT "${ASSET_DIR}/blaster.umesh"
    COMMAND um_import "${BLASTER_GLB}" "${ASSET_DIR}/blaster"
    DEPENDS um_import "${BLASTER_GLB}"
    COMMENT "um_import blaster"
    VERBATIM)
add_custom_command(
    OUTPUT "${ASSET_DIR}/robot.umesh"
    COMMAND um_import "${ROBOT_GLB}" "${ASSET_DIR}/robot"
    DEPENDS um_import "${ROBOT_GLB}"
    COMMENT "um_import robot"
    VERBATIM)
add_custom_target(sample_assets
    DEPENDS "${ASSET_DIR}/duck.umesh" "${ASSET_DIR}/blaster.umesh" "${ASSET_DIR}/robot.umesh")
