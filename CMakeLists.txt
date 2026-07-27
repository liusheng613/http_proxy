cmake_minimum_required(VERSION 3.16)
project(http_proxy LANGUAGES CXX)

if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
endif()

# practice/ 和 proxy/ 仅 Linux 编译（依赖 epoll / POSIX）
if(NOT WIN32)
    add_subdirectory(practice)
    add_subdirectory(proxy)
endif()
add_subdirectory(tunnel)
