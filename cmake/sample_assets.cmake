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

# The Crytek Sponza atrium, the canonical multi-material test scene. Its glTF
# form is a .gltf plus a .bin plus loose textures, so the file list is read
# from the manifest itself. The commit-pinned raw URL makes every file
# immutable without per-file hashes.
set(SPONZA_COMMIT "d7a3cc8e51d7c573771ae77a57f16b0662a905c6")
set(SPONZA_BASE
    "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Models/${SPONZA_COMMIT}/2.0/Sponza/glTF")
set(SPONZA_DIR "${CMAKE_BINARY_DIR}/sample/sponza")
set(SPONZA_GLTF "${SPONZA_DIR}/Sponza.gltf")

if(NOT EXISTS "${SPONZA_GLTF}")
    message(STATUS "Downloading sample asset: Sponza.gltf")
    file(DOWNLOAD "${SPONZA_BASE}/Sponza.gltf" "${SPONZA_GLTF}" STATUS _sponza_status)
    list(GET _sponza_status 0 _sponza_code)
    if(NOT _sponza_code EQUAL 0)
        message(FATAL_ERROR "Sponza download failed: ${_sponza_status}")
    endif()
endif()

# Pull every buffer and image URI named in the manifest.
file(READ "${SPONZA_GLTF}" _sponza_json)
set(_sponza_uris "")
foreach(section buffers images)
    string(JSON _count ERROR_VARIABLE _err LENGTH "${_sponza_json}" ${section})
    if(_err)
        continue()
    endif()
    math(EXPR _last "${_count} - 1")
    foreach(i RANGE ${_last})
        string(JSON _uri ERROR_VARIABLE _err GET "${_sponza_json}" ${section} ${i} uri)
        if(NOT _err AND NOT _uri MATCHES "^data:")
            list(APPEND _sponza_uris "${_uri}")
        endif()
    endforeach()
endforeach()
foreach(_uri IN LISTS _sponza_uris)
    if(NOT EXISTS "${SPONZA_DIR}/${_uri}")
        message(STATUS "Downloading Sponza file: ${_uri}")
        file(DOWNLOAD "${SPONZA_BASE}/${_uri}" "${SPONZA_DIR}/${_uri}" STATUS _dl_status)
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            message(FATAL_ERROR "Sponza file ${_uri} failed: ${_dl_status}")
        endif()
    endif()
endforeach()

set(ASSET_DIR "${CMAKE_BINARY_DIR}/assets")
file(MAKE_DIRECTORY "${ASSET_DIR}")

add_custom_command(
    OUTPUT "${ASSET_DIR}/duck.umesh"
    COMMAND um_import "${DUCK_GLB}" "${ASSET_DIR}/duck"
    DEPENDS um_import "${DUCK_GLB}"
    COMMENT "um_import duck"
    VERBATIM)
add_custom_command(
    OUTPUT "${ASSET_DIR}/sponza.umesh"
    COMMAND um_import "${SPONZA_GLTF}" "${ASSET_DIR}/sponza"
    DEPENDS um_import "${SPONZA_GLTF}"
    COMMENT "um_import sponza"
    VERBATIM)
add_custom_target(sample_assets
    DEPENDS "${ASSET_DIR}/duck.umesh" "${ASSET_DIR}/sponza.umesh")
