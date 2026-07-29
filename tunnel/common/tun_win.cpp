#ifdef _WIN32

#include "tun.h"
#include "logger.h"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>   // for ConvertInterfaceLuidToGuid if needed
#include <stdio.h>
#include <string>
#include <cstring>
#include <cerrno>
#include <cstdlib>

#include "wintun/wintun/include/wintun.h"

namespace tunnel {
namespace tun {

// ========== 动态加载 wintun.dll ==========

static HMODULE                              g_dll   = nullptr;
static WINTUN_CREATE_ADAPTER_FUNC*          g_fnCreateAdapter    = nullptr;
static WINTUN_OPEN_ADAPTER_FUNC*            g_fnOpenAdapter      = nullptr;
static WINTUN_CLOSE_ADAPTER_FUNC*           g_fnCloseAdapter     = nullptr;
static WINTUN_GET_ADAPTER_LUID_FUNC*        g_fnGetAdapterLUID   = nullptr;
static WINTUN_START_SESSION_FUNC*           g_fnStartSession     = nullptr;
static WINTUN_END_SESSION_FUNC*             g_fnEndSession       = nullptr;
static WINTUN_GET_READ_WAIT_EVENT_FUNC*     g_fnGetReadWaitEvent = nullptr;
static WINTUN_RECEIVE_PACKET_FUNC*          g_fnReceivePacket    = nullptr;
static WINTUN_RELEASE_RECEIVE_PACKET_FUNC*  g_fnReleaseReceivePacket = nullptr;
static WINTUN_ALLOCATE_SEND_PACKET_FUNC*    g_fnAllocateSendPacket   = nullptr;
static WINTUN_SEND_PACKET_FUNC*             g_fnSendPacket       = nullptr;

static WINTUN_ADAPTER_HANDLE  g_adapter  = nullptr;
static WINTUN_SESSION_HANDLE  g_session  = nullptr;
static HANDLE                 g_read_evt = nullptr;
static std::string            g_dev_name;
static bool                   g_ok       = false;

// ---- helper: load DLL & resolve functions ----
static bool load_wintun() {
    if (g_dll) return true;

    // 1) 尝试从当前目录加载
    g_dll = LoadLibraryW(L"wintun.dll");
    if (!g_dll) {
        // 2) 尝试从 exe 所在目录加载
        wchar_t exe_path[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            // 去掉 exe 文件名, 拼 wintun.dll
            wchar_t* last = wcsrchr(exe_path, L'\\');
            if (last) *(last + 1) = L'\0';
            wcscat_s(exe_path, L"wintun.dll");
            g_dll = LoadLibraryW(exe_path);
        }
    }
    if (!g_dll) {
        LOG_ERROR("Cannot load wintun.dll (err=%lu)", GetLastError());
        return false;
    }

#define LOAD(fn, TYPE) do { \
    g_fn##fn = (WINTUN_##TYPE##_FUNC*)GetProcAddress(g_dll, "Wintun" #fn); \
    if (!g_fn##fn) { LOG_ERROR("Wintun" #fn " not found"); return false; } \
} while(0)

    LOAD(CreateAdapter,        CREATE_ADAPTER);
    LOAD(OpenAdapter,          OPEN_ADAPTER);
    LOAD(CloseAdapter,         CLOSE_ADAPTER);
    LOAD(GetAdapterLUID,       GET_ADAPTER_LUID);
    LOAD(StartSession,         START_SESSION);
    LOAD(EndSession,           END_SESSION);
    LOAD(GetReadWaitEvent,     GET_READ_WAIT_EVENT);
    LOAD(ReceivePacket,        RECEIVE_PACKET);
    LOAD(ReleaseReceivePacket, RELEASE_RECEIVE_PACKET);
    LOAD(AllocateSendPacket,   ALLOCATE_SEND_PACKET);
    LOAD(SendPacket,           SEND_PACKET);
#undef LOAD

    return true;
}

// ---- helper: utf-8 → wchar ----
static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    ws.resize(len - 1);  // drop null terminator
    return ws;
}

// ========== 公共接口 ==========

int create(const std::string& name_hint) {
    if (!load_wintun()) return -1;

    // 先清理旧的
    if (g_session) { g_fnEndSession(g_session); g_session = nullptr; }
    if (g_adapter) { g_fnCloseAdapter(g_adapter); g_adapter = nullptr; }
    g_read_evt = nullptr;
    g_dev_name.clear();
    g_ok = false;

    // 适配器名: Linux 用 "tun%d" 模板, Windows 用友好名称
    if (name_hint.empty() || name_hint == "tun%d") {
        g_dev_name = "Tunnel VPN";
    } else {
        g_dev_name = name_hint;
    }
    auto wide_name = to_wide(g_dev_name);

    // 先尝试打开已存在的适配器
    g_adapter = g_fnOpenAdapter(wide_name.c_str());
    if (!g_adapter) {
        // 不存在则创建 (TunnelType 影响 "描述" 字段显示)
        g_adapter = g_fnCreateAdapter(wide_name.c_str(), L"Tunnel VPN Adapter", nullptr);
        if (!g_adapter) {
            LOG_ERROR("WintunCreateAdapter('%s') failed (err=%lu, run as admin?)",
                      g_dev_name.c_str(), GetLastError());
            return -1;
        }
        LOG_INFO("Wintun adapter '%s' created", g_dev_name.c_str());
    } else {
        LOG_INFO("Wintun adapter '%s' opened", g_dev_name.c_str());
    }

    // 启动会话 —— 最小 ring buffer
    g_session = g_fnStartSession(g_adapter, WINTUN_MIN_RING_CAPACITY);
    if (!g_session) {
        LOG_ERROR("WintunStartSession failed (err=%lu)", GetLastError());
        g_fnCloseAdapter(g_adapter);
        g_adapter = nullptr;
        return -1;
    }

    g_read_evt = g_fnGetReadWaitEvent(g_session);
    g_ok = true;
    LOG_INFO("TUN ready (adapter='%s')", g_dev_name.c_str());
    return 1;  // pseudo-fd, 表示成功
}

std::string get_dev_name() { return g_dev_name; }

bool set_ip(const std::string& dev_name, const std::string& ip, int prefix) {
    char mask[16];
    if (prefix >= 24)      snprintf(mask, sizeof(mask), "255.255.255.0");
    else if (prefix >= 16) snprintf(mask, sizeof(mask), "255.255.0.0");
    else                   snprintf(mask, sizeof(mask), "255.0.0.0");

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "netsh interface ip set address \"%s\" static %s %s >nul 2>&1",
             dev_name.c_str(), ip.c_str(), mask);
    if (system(cmd) != 0) {
        LOG_WARN("netsh set ip failed (admin needed?): %s", cmd);
        // 不 fatal —— 用户可手动配
    } else {
        LOG_INFO("TUN %s: IP %s/%d configured", dev_name.c_str(), ip.c_str(), prefix);
    }
    return true;
}

bool add_route(const std::string& subnet, const std::string& dev_name) {
    char cmd[512];
    // 注意: route add 在 Windows 上 if 编号不容易直接映射, 用 netsh 替代
    // 或直接 route add, 但需要知道接口索引。
    // 这里只添加路由到 "Tunnel" 网卡, 假设子网是 /24
    snprintf(cmd, sizeof(cmd),
             "route add %s mask 255.255.255.0 0.0.0.0 >nul 2>&1",
             subnet.c_str());
    if (system(cmd) != 0) {
        LOG_WARN("route add failed (admin needed?): %s", cmd);
    }
    return true;
}

int read_packet(uint8_t* buf, int max_len) {
    if (!g_session) return -1;

    DWORD pkt_size = 0;
    BYTE* pkt = g_fnReceivePacket(g_session, &pkt_size);
    if (!pkt) {
        DWORD err = GetLastError();
        if (err == ERROR_NO_MORE_ITEMS) return 0;
        if (err == ERROR_HANDLE_EOF) return -1;
        return 0;  // other transient errors → no data
    }

    if (static_cast<DWORD>(max_len) < pkt_size) {
        LOG_WARN("TUN read packet too large: %lu > %d", pkt_size, max_len);
        g_fnReleaseReceivePacket(g_session, pkt);
        return -1;
    }

    memcpy(buf, pkt, pkt_size);
    g_fnReleaseReceivePacket(g_session, pkt);
    return static_cast<int>(pkt_size);
}

int write_packet(const uint8_t* buf, int len) {
    if (!g_session) return -1;
    if (len <= 0 || len > WINTUN_MAX_IP_PACKET_SIZE) return -1;

    BYTE* pkt = g_fnAllocateSendPacket(g_session, static_cast<DWORD>(len));
    if (!pkt) {
        DWORD err = GetLastError();
        if (err == ERROR_BUFFER_OVERFLOW) return 0;  // 缓冲区满, 稍后重试
        return -1;
    }
    memcpy(pkt, buf, len);
    g_fnSendPacket(g_session, pkt);
    return len;
}

void* get_read_event() { return g_read_evt; }

void close() {
    if (g_session) {
        g_fnEndSession(g_session);
        g_session = nullptr;
    }
    if (g_adapter) {
        g_fnCloseAdapter(g_adapter);
        g_adapter = nullptr;
    }
    g_read_evt = nullptr;
    g_dev_name.clear();
    g_ok = false;
}

}  // namespace tun
}  // namespace tunnel

#endif  // _WIN32
