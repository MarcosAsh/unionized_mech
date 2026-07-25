# Offline GLSL to SPIR-V compilation as a build step (CLAUDE.md section 7).
#
# Each shader compiles to build/shaders/<name>.spv for Vulkan 1.3, with an
# #include depfile so edits to a shader or anything it includes trigger a
# rebuild. Runtime hot-reload watches the resulting .spv files.
#
# Defined here in M0 commit 1; first invoked by src/render in M0 commit 7.

find_program(GLSLANG_EXECUTABLE NAMES glslang glslangValidator REQUIRED)

# add_shaders(<out_var> SOURCES <file> [<file> ...])
# Sets <out_var> to the list of generated .spv paths in the parent scope.
function(add_shaders OUT_VAR)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    set(out_dir "${CMAKE_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${out_dir}")
    set(spv_files "")
    foreach(src IN LISTS ARG_SOURCES)
        get_filename_component(name "${src}" NAME)
        set(spv "${out_dir}/${name}.spv")
        set(dep "${out_dir}/${name}.d")
        add_custom_command(
            OUTPUT  "${spv}"
            COMMAND ${GLSLANG_EXECUTABLE} -V --target-env vulkan1.3
                    --depfile "${dep}" -o "${spv}" "${src}"
            DEPENDS "${src}"
            DEPFILE "${dep}"
            COMMENT "glslang ${name}"
            VERBATIM)
        list(APPEND spv_files "${spv}")
    endforeach()
    set(${OUT_VAR} "${spv_files}" PARENT_SCOPE)
endfunction()
