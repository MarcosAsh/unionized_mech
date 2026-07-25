# Compile the scene shaders to SPIR-V and make render depend on them.
add_shaders(SCENE_SPV SOURCES
    ${CMAKE_SOURCE_DIR}/shaders/scene.vert
    ${CMAKE_SOURCE_DIR}/shaders/scene.frag)
add_custom_target(scene_shaders DEPENDS ${SCENE_SPV})

add_library(render STATIC
    render_pipeline.cpp
    render_scene.cpp)
add_dependencies(render scene_shaders)

target_include_directories(render PUBLIC ${CMAKE_SOURCE_DIR}/src)

# render reads sim types (World) as const in its public header.
target_link_libraries(render PUBLIC core gpu sim)
target_link_libraries(render PRIVATE volk)

# Absolute path to the compiled SPIR-V, as a C string literal.
target_compile_definitions(render PRIVATE "SHADER_DIR=\"${CMAKE_BINARY_DIR}/shaders\"")
