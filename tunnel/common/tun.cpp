#include "tun.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "logger.h"

namespace tunnel {
namespace tun {

// ---- 私有全局状态 ----
static int    g_fd   = -1;
static std::string g_dev_name;

int create(const std::string& name_hint) {
    g_dev_name.clear();

    // 先清理可能残留的同名旧设备（上次非正常退出遗留）
    char clean_cmd[128];
    for (int i = 0; i < 10; ++i) {
        snprintf(clean_cmd, sizeof(clean_cmd), "ip link del tun%d 2>/dev/null", i);
        if (system(clean_cmd) != 0) {}  // 设备不存在时删除失败, 忽略
    }

    g_fd = open("/dev/net/tun", O_RDWR);
    if (g_fd < 0) {
        LOG_ERROR("open /dev/net/tun failed: %s (need root/CAP_NET_ADMIN?)",
                  strerror(errno));
        return -1;
    }

    ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;  // TUN (IP 层), 不带 packet info
    std::strncpy(ifr.ifr_name, name_hint.c_str(), IFNAMSIZ - 1);

    if (ioctl(g_fd, TUNSETIFF, &ifr) < 0) {
        LOG_ERROR("ioctl TUNSETIFF failed: %s", strerror(errno));
        ::close(g_fd);
        g_fd = -1;
        return -1;
    }

    g_dev_name = ifr.ifr_name;
    LOG_INFO("TUN device %s created (fd=%d)", ifr.ifr_name, g_fd);
    return g_fd;
}

std::string get_dev_name() { return g_dev_name; }

bool set_ip(const std::string& dev_name, const std::string& ip, int prefix) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip addr add %s/%d dev %s 2>/dev/null",
             ip.c_str(), prefix, dev_name.c_str());
    if (system(cmd) != 0) {
        LOG_ERROR("failed to set IP %s on %s (need root?)", ip.c_str(), dev_name.c_str());
        return false;
    }
    snprintf(cmd, sizeof(cmd), "ip link set %s up 2>/dev/null", dev_name.c_str());
    if (system(cmd) != 0) {}  // ignore failure
    LOG_INFO("TUN %s: IP %s/%d configured", dev_name.c_str(), ip.c_str(), prefix);
    return true;
}

bool add_route(const std::string& subnet, const std::string& dev_name) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip route add %s dev %s 2>/dev/null",
             subnet.c_str(), dev_name.c_str());
    if (system(cmd) != 0) {}  // ignore failure
    return true;
}

int read_packet(uint8_t* buf, int max_len) {
    if (g_fd < 0) return -1;
    ssize_t n = ::read(g_fd, buf, max_len);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

int write_packet(const uint8_t* buf, int len) {
    if (g_fd < 0) return -1;
    ssize_t n = ::write(g_fd, buf, len);
    if (n >= 0) return static_cast<int>(n);
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

void close() {
    if (g_fd >= 0) {
        ::close(g_fd);
        g_fd = -1;
    }
    g_dev_name.clear();
}

}  // namespace tun
}  // namespace tunnel
