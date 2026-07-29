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

  // socket 读写 (Windows socket 不能用 POSIX read/write)
  inline int sock_read(int fd, void* buf, int len) {
      int n = recv(fd, static_cast<char*>(buf), len, 0);
      // MinGW 的 recv 不设 errno, 手动处理 WSAEWOULDBLOCK
      if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
          errno = EAGAIN;
      }
      return n;
  }
  inline int sock_write(int fd, const void* buf, int len) {
      int n = send(fd, static_cast<const char*>(buf), len, 0);
      if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
          errno = EAGAIN;
      }
      return n;
  }

  // sleep (ms)
  inline void platform_sleep_ms(int ms) { Sleep(ms); }

  #pragma comment(lib, "ws2_32.lib")

#else
  // Linux / POSIX
  #include <sys/types.h>
  #include <unistd.h>
  inline void platform_sleep_ms(int ms) { usleep(ms * 1000); }
  inline int sock_close(int fd) { return close(fd); }
  #define close_socket(fd) close(fd)
  inline int sock_read(int fd, void* buf, int len) {
      return static_cast<int>(read(fd, buf, len));
  }
  inline int sock_write(int fd, const void* buf, int len) {
      return static_cast<int>(write(fd, buf, len));
  }
#endif

#endif  // TUNNEL_COMMON_PLATFORM_H_
