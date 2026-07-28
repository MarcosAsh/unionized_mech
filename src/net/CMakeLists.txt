add_library(net STATIC
    src/net_connection.cpp
    src/net_socket.cpp)

# net depends only on core. It knows nothing about the simulation it will carry,
# which is what lets the ack logic be tested on its own.
target_include_directories(net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(net PUBLIC core)
