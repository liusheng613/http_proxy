#ifdef _WIN32
// Windows 不支持 TUN 虚拟网卡——提供空实现，上层代码无需 ifdef
#include "tun.h"

namespace tunnel {
namespace tun {

int create(const std::string&) { return -1; }
bool set_ip(const std::string&, const std::string&, int) { return false; }
bool add_route(const std::string&, const std::string&) { return false; }

}  // namespace tun
}  // namespace tunnel
#endif
