#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

static std::string g_tun_ip;
static bool g_connected = false;
static bool g_authed     = false;
static bool g_registered = false;
static HANDLE g_hRead = NULL;

// ---- 刷新一行 ----
void GoToLine(int row) { printf("\033[%d;1H", row); }

void DrawPanel() {
    printf("\033[2J\033[H"); // clear screen

    printf("  ====== Tunnel Client ======\n\n");
    printf("  Status:   ");
    if (g_connected)  printf("\033[1;32mConnected\033[0m");
    else               printf("\033[1;31mDisconnected\033[0m");
    printf("\n");

    printf("  Auth:     ");
    if (g_authed)     printf("\033[1;32mOK\033[0m");
    else               printf("\033[1;33mPending\033[0m");
    printf("\n");

    printf("  Register: ");
    if (g_registered) printf("\033[1;32mOK\033[0m");
    else               printf("\033[1;33mPending\033[0m");
    printf("\n");

    printf("  ----------\n");
    printf("  TUN IP:   ");
    if (!g_tun_ip.empty()) printf("\033[1;36m%s\033[0m", g_tun_ip.c_str());
    else                   printf("\033[1;33m(waiting)\033[0m");
    printf("\n\n");

    printf("  Press Ctrl+C to stop\n");
}

void ParseLine(const std::string& line) {
    if (line.find("connected to server") != std::string::npos) {
        g_connected = true;
    }
    if (line.find("sent AUTH") != std::string::npos) {
        g_authed = true;
    }
    if (line.find("REGISTER success") != std::string::npos) {
        g_registered = true;
    }
    if (line.find("TUN IP assigned:") != std::string::npos) {
        size_t p = line.find("TUN IP assigned:");
        if (p != std::string::npos) {
            g_tun_ip = line.substr(p + 16);
            while (!g_tun_ip.empty() && g_tun_ip[0] == ' ') g_tun_ip.erase(0, 1);
            while (!g_tun_ip.empty() && (g_tun_ip.back() == '\r' || g_tun_ip.back() == '\n' || g_tun_ip.back() == ' '))
                g_tun_ip.pop_back();
        }
    }
    if (line.find("heartbeat timeout") != std::string::npos ||
        line.find("server closed") != std::string::npos ||
        line.find("connect failed") != std::string::npos) {
        g_connected = false;
        g_authed = false;
        g_registered = false;
        g_tun_ip.clear();
    }
}

DWORD WINAPI ReaderThread(LPVOID) {
    char buf[4096];
    DWORD n;
    while (ReadFile(g_hRead, buf, sizeof(buf) - 1, &n, NULL) && n > 0) {
        buf[n] = '\0';
        std::string chunk(buf, n);
        size_t start = 0, end;
        while ((end = chunk.find('\n', start)) != std::string::npos) {
            ParseLine(chunk.substr(start, end - start));
            start = end + 1;
        }
        DrawPanel();
    }
    g_connected = false;
    DrawPanel();
    printf("\n  Client stopped. Press any key to exit...\n");
    return 0;
}

int main(int argc, char** argv) {
    // 设置 UTF-8 避免中文乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // 启用虚拟终端 ANSI 转义
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hStdOut, &mode);
    SetConsoleMode(hStdOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // 配置文件: 命令行指定 > 自动查找
    std::string cfg;
    if (argc > 1) {
        cfg = argv[1];
    } else {
        const char* defaults[] = { "tunnel_client.conf", "../tunnel_client.conf" };
        for (const char* p : defaults) {
            if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) { cfg = p; break; }
        }
    }
    if (cfg.empty()) {
        fprintf(stderr, "ERROR: config file not found\n");
        fprintf(stderr, "  usage: tunnel_launcher.exe [config_file]\n");
        return 1;
    }

    // 转成绝对路径，避免子进程工作目录不同导致找不到
    char abs_cfg[MAX_PATH];
    GetFullPathNameA(cfg.c_str(), MAX_PATH, abs_cfg, NULL);
    cfg = abs_cfg;

    // 查找 tunnel_client.exe
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    std::string dir(self);
    size_t last = dir.find_last_of("\\/");
    if (last != std::string::npos) dir = dir.substr(0, last + 1);

    std::string exe = dir + "tunnel_client.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 可能在 build/tunnel/ 下
        exe = dir + "..\\tunnel\\tunnel_client.exe";
        if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "ERROR: cannot find tunnel_client.exe\n");
            fprintf(stderr, "  looked at: %stunnel_client.exe\n", dir.c_str());
            return 1;
        }
    }

    std::string cmd = "\"" + exe + "\" -c \"" + cfg + "\"";

    // 创建管道
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hWrite;
    if (!CreatePipe(&g_hRead, &hWrite, &sa, 0)) {
        fprintf(stderr, "CreatePipe failed: %lu\n", GetLastError());
        return 1;
    }
    SetHandleInformation(g_hRead, HANDLE_FLAG_INHERIT, 0);

    // 启动子进程
    STARTUPINFOA si = { sizeof(si) };
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    std::vector<char> cbuf(cmd.begin(), cmd.end());
    cbuf.push_back('\0');

    DrawPanel();
    printf("  Starting: %s\n\n", cmd.c_str());

    if (!CreateProcessA(NULL, cbuf.data(), NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, dir.c_str(), &si, &pi)) {
        fprintf(stderr, "CreateProcess failed: %lu\n  cmd: %s\n", GetLastError(), cmd.c_str());
        CloseHandle(hWrite);
        CloseHandle(g_hRead);
        return 1;
    }

    CloseHandle(hWrite);

    // 读取线程
    HANDLE hThread = CreateThread(NULL, 0, ReaderThread, NULL, 0, NULL);

    // 等待结束
    WaitForSingleObject(pi.hProcess, INFINITE);
    WaitForSingleObject(hThread, 2000);

    CloseHandle(hThread);
    CloseHandle(g_hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    printf("\nPress any key to exit...");
    getchar();
    return 0;
}

#else
#include <stdio.h>
int main() {
    fprintf(stderr, "launcher is Windows-only\n");
    return 1;
}
#endif
