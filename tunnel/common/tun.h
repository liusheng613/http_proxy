#ifndef TUNNEL_COMMON_TUN_H_
#define TUNNEL_COMMON_TUN_H_

#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <string>

namespace tunnel {
namespace tun {

// 创建 TUN 虚拟网卡, 返回 fd。
// name_hint: tun 设备名提示 (如 "tun0"), 实际名由内核分配。
// 成功返回 fd, 可通过 read/write 收发原始 IP 包; 失败返回 -1。
int create(const std::string& name_hint = "tun%d");

// 配置 tun 设备 IP + 启动
bool set_ip(const std::string& dev_name, const std::string& ip, int prefix = 24);

// 配置路由
bool add_route(const std::string& subnet, const std::string& dev_name);

}  // namespace tun
}  // namespace tunnel

#endif  // TUNNEL_COMMON_TUN_H_
