add_library(sim STATIC sim.cpp sim_math.cpp)

# sim depends only on core. It never sees platform, gpu, render, or SDL.
target_include_directories(sim PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sim PUBLIC core)
