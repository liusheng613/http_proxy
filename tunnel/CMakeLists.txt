# 隧道公共库
find_package(OpenSSL REQUIRED)

add_library(tunnel_common STATIC
    common/logger.cpp
    common/netutil.cpp
    common/frame.cpp
    common/write_buffer.cpp
    common/crypto.cpp
)
target_include_directories(tunnel_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(tunnel_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(tunnel_common PUBLIC OpenSSL::Crypto)

# server
add_executable(tunnel_server
    server/main.cpp
    server/tunnel_server.cpp
    server/port_mapper.cpp
)
target_link_libraries(tunnel_server PRIVATE tunnel_common)

# client
add_executable(tunnel_client
    client/main.cpp
    client/tunnel_client.cpp
)
target_link_libraries(tunnel_client PRIVATE tunnel_common)
