# 隧道公共库
find_package(OpenSSL REQUIRED)

set(COMMON_SOURCES
    common/logger.cpp
    common/netutil.cpp
    common/frame.cpp
    common/write_buffer.cpp
    common/crypto.cpp
    common/config.cpp
)

# 平台相关
if(WIN32)
    list(APPEND COMMON_SOURCES common/event_poller_win.cpp common/tun_win.cpp)
else()
    list(APPEND COMMON_SOURCES common/event_poller_linux.cpp common/tun.cpp)
endif()

add_library(tunnel_common STATIC ${COMMON_SOURCES})
target_include_directories(tunnel_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(tunnel_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(tunnel_common PUBLIC OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(tunnel_common PUBLIC ws2_32)
endif()

# server (仅 Linux)
if(NOT WIN32)
    add_executable(tunnel_server
        server/main.cpp
        server/tunnel_server.cpp
        server/port_mapper.cpp
    )
    target_link_libraries(tunnel_server PRIVATE tunnel_common)
endif()

# client
add_executable(tunnel_client
    client/main.cpp
    client/tunnel_client.cpp
)
target_link_libraries(tunnel_client PRIVATE tunnel_common)
