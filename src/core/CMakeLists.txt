add_library(core STATIC
    src/arena.cpp
    src/assert.cpp
    src/file.cpp
    src/log.cpp
    src/timer.cpp)

# Public headers live under include/core so callers write "core/<header>.h".
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(core PUBLIC engine_options)
