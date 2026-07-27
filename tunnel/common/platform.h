#ifndef TUNNEL_COMMON_PLATFORM_H_
#define TUNNEL_COMMON_PLATFORM_H_

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <windows.h>

  // Winsock 初始化/清理
  inline bool winsock_init() {
      WSADATA wsa;
      return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }
  inline void winsock_cleanup() { WSACleanup(); }

  // socket 关闭
  inline int sock_close(int fd) { return closesocket(fd); }
  #define close_socket(fd) closesocket(fd)

  // sleep (ms)
  inline void platform_sleep_ms(int ms) { Sleep(ms); }

  #pragma comment(lib, "ws2_32.lib")

#else
  // Linux / POSIX
  #include <unistd.h>
  inline void platform_sleep_ms(int ms) { usleep(ms * 1000); }
  inline int sock_close(int fd) { return close(fd); }
  #define close_socket(fd) close(fd)
#endif

#endif  // TUNNEL_COMMON_PLATFORM_H_
