#ifndef TUNNEL_COMMON_TUN_H_
#define TUNNEL_COMMON_TUN_H_

#ifndef _WIN32
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#endif
#include <cstdint>
#include <string>

namespace tunnel {
namespace tun {

// 创建 TUN 虚拟网卡, 返回 fd(Linux) 或伪 fd(Windows)。
// name_hint: tun 设备名提示 (Linux: "tun%d", Windows: "Tunnel")。
// 成功返回 >=0 的标识符; 失败返回 -1。
int create(const std::string& name_hint = "tun%d");

// 返回设备名（Linux 为内核分配的实际名称如 "tun0"，Windows 为适配器名）
std::string get_dev_name();

// 配置 tun 设备 IP + 启动
bool set_ip(const std::string& dev_name, const std::string& ip, int prefix = 24);

// 配置路由
bool add_route(const std::string& subnet, const std::string& dev_name);

// 从 TUN 读取原始 IP 包 (非阻塞)。返回字节数, 0=暂无数据, -1=出错。
int read_packet(uint8_t* buf, int max_len);

// 向 TUN 写入原始 IP 包。返回写入字节数, -1=出错。
int write_packet(const uint8_t* buf, int len);

#ifdef _WIN32
// Windows: 获取读事件句柄 (void* = HANDLE), 用于 WaitForSingleObject.
// 未初始化返回 nullptr。
void* get_read_event();
#endif

// 关闭 TUN 设备, 释放资源。
void close();

}  // namespace tun
}  // namespace tunnel

#endif  // TUNNEL_COMMON_TUN_H_
