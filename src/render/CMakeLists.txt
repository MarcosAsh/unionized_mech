# Compile the scene shaders to SPIR-V and make render depend on them.
add_shaders(SCENE_SPV SOURCES
    ${CMAKE_SOURCE_DIR}/shaders/scene.vert
    ${CMAKE_SOURCE_DIR}/shaders/scene.frag
    ${CMAKE_SOURCE_DIR}/shaders/mesh.vert
    ${CMAKE_SOURCE_DIR}/shaders/mesh.frag
    ${CMAKE_SOURCE_DIR}/shaders/cull.comp
    ${CMAKE_SOURCE_DIR}/shaders/skin.comp
    ${CMAKE_SOURCE_DIR}/shaders/shadow_level.vert
    ${CMAKE_SOURCE_DIR}/shaders/shadow_mesh.vert
    ${CMAKE_SOURCE_DIR}/shaders/scene_rt.frag
    ${CMAKE_SOURCE_DIR}/shaders/mesh_rt.frag
    ${CMAKE_SOURCE_DIR}/shaders/overlay.frag)
add_custom_target(scene_shaders DEPENDS ${SCENE_SPV})

add_library(render STATIC
    src/render_model.cpp
    src/render_props.cpp
    src/render_pipeline.cpp
    src/render_scene.cpp
    src/render_scene_setup.cpp
    src/render_sun.cpp
    src/render_text.cpp)
add_dependencies(render scene_shaders)

target_include_directories(render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# render reads sim types (World) as const in its public header, and loads
# imported models through asset.
target_link_libraries(render PUBLIC core gpu sim)
target_link_libraries(render PRIVATE asset volk)

# Absolute paths to the compiled SPIR-V and converted assets, as C string
# literals.
target_compile_definitions(render PRIVATE
    "SHADER_DIR=\"${CMAKE_BINARY_DIR}/shaders\""
    "ASSET_DIR=\"${CMAKE_BINARY_DIR}/assets\"")
