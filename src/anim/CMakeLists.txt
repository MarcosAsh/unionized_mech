add_library(anim STATIC
    src/anim_io.cpp
    src/anim_sample.cpp)

# anim depends only on core. Import from glTF lives in asset; playback state
# lives with whoever animates (render for visuals, sim when gameplay needs it).
target_include_directories(anim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anim PUBLIC core)
