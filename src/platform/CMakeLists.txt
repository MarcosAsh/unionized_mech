# SDL3 is built from the pinned source. Shared library, no test harness.
# XTEST is X11 input synthesis, which a game does not use, so it is disabled to
# avoid a libXtst dev dependency.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

add_library(platform STATIC platform.cpp)
target_include_directories(platform PUBLIC ${CMAKE_SOURCE_DIR}/src)

# core and sim are in the public header; SDL is an implementation detail. Its
# headers are marked SYSTEM so they never trip our -Werror.
target_link_libraries(platform PUBLIC core sim)
target_link_libraries(platform PRIVATE SDL3::SDL3)

get_target_property(_sdl_includes SDL3::SDL3 INTERFACE_INCLUDE_DIRECTORIES)
if(_sdl_includes)
    target_include_directories(platform SYSTEM PRIVATE ${_sdl_includes})
endif()
