add_library(sim STATIC
    src/sim.cpp
    src/sim_level.cpp
    src/sim_math.cpp
    src/sim_tape.cpp)

# sim depends only on core. It never sees platform, gpu, render, or SDL.
target_include_directories(sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sim PUBLIC core)
