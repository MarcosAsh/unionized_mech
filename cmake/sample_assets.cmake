# Game content, downloaded pinned by hash and converted to the native formats
# by um_import at build time. Nothing here is committed. All CC0: the Khronos
# sample Duck, Kenney's Blaster Kit (kenney.nl), the Quaternius Animated Robot
# (quaternius.com, via poly.pizza), and a concrete surface from ambientCG.

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

# The character: Quaternius CC0 SWAT. Fully vertex skinned, unlike the Animated
# Robot it replaces, which was rigid meshes parented to bones — the importer
# bound 85% of that model to joint 0, so it followed the root and never
# deformed. Authored in metres, 62 joints, 24 clips.
set(ROBOT_URL "https://static.poly.pizza/713f6535-f4f3-4367-a4c6-ced126ae0936.glb")
set(ROBOT_SHA256 "a835107bac833eb916c494e10997ae1709e85957ea6f6c59ace3c9a66f6d1fec")
set(ROBOT_GLB "${CMAKE_BINARY_DIR}/sample/Robot.glb")

if(NOT EXISTS "${ROBOT_GLB}")
    message(STATUS "Downloading sample asset: Robot.glb")
    file(DOWNLOAD "${ROBOT_URL}" "${ROBOT_GLB}" EXPECTED_HASH SHA256=${ROBOT_SHA256})
endif()

# The level surface, CC0 from ambientCG. Only the colour map is wired up today;
# the same pack carries NormalGL and Roughness for when the shading grows into
# them, which is why a whole material set is fetched rather than one image.
set(GROUND_URL "https://ambientcg.com/get?file=Concrete034_1K-JPG.zip")
set(GROUND_SHA256 "5839d284d94ffb8d2a56df742ec522b13dd311c52dbd42b8fd33f0409ceedb81")
set(GROUND_ZIP "${CMAKE_BINARY_DIR}/sample/ground.zip")
set(GROUND_JPG "${CMAKE_BINARY_DIR}/sample/ground/Concrete034_1K-JPG_Color.jpg")

if(NOT EXISTS "${GROUND_JPG}")
    message(STATUS "Downloading sample asset: ambientCG Concrete034")
    file(DOWNLOAD "${GROUND_URL}" "${GROUND_ZIP}" EXPECTED_HASH SHA256=${GROUND_SHA256})
    file(ARCHIVE_EXTRACT INPUT "${GROUND_ZIP}" DESTINATION "${CMAKE_BINARY_DIR}/sample/ground")
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
add_custom_command(
    OUTPUT "${ASSET_DIR}/ground.utex"
    COMMAND um_import "${GROUND_JPG}" "${ASSET_DIR}/ground"
    DEPENDS um_import "${GROUND_JPG}"
    COMMENT "um_import ground"
    VERBATIM)
add_custom_target(sample_assets
    DEPENDS "${ASSET_DIR}/duck.umesh" "${ASSET_DIR}/blaster.umesh" "${ASSET_DIR}/robot.umesh"
            "${ASSET_DIR}/ground.utex")
