add_library(session STATIC
    src/session.cpp)

# The one place sim and net meet: net carries bytes and knows nothing about the
# world, sim simulates and knows nothing about the wire.
target_include_directories(session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(session PUBLIC core sim net)
