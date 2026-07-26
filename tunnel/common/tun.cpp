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

int create(const std::string& name_hint) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        LOG_ERROR("open /dev/net/tun failed: %s (need root/CAP_NET_ADMIN?)",
                  strerror(errno));
        return -1;
    }

    ifreq ifr{};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;  // TUN (IP 层), 不带 packet info
    std::strncpy(ifr.ifr_name, name_hint.c_str(), IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        LOG_ERROR("ioctl TUNSETIFF failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    LOG_INFO("TUN device %s created (fd=%d)", ifr.ifr_name, fd);
    return fd;
}

bool set_ip(const std::string& dev_name, const std::string& ip, int prefix) {
    // 用 system() 调 ip 命令, 简化实现
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

}  // namespace tun
}  // namespace tunnel
