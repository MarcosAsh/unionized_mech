add_library(core STATIC
    arena.cpp
    assert.cpp
    file.cpp
    log.cpp
    timer.cpp)

# One public include root so modules include as "core/<header>.h".
target_include_directories(core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(core PUBLIC engine_options)
