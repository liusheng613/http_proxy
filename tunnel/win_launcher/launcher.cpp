#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

// ---- 简易 ANSI 颜色 ----
#define CLR_RESET  "\033[0m"
#define CLR_GREEN  "\033[1;32m"
#define CLR_YELLOW "\033[1;33m"
#define CLR_CYAN   "\033[1;36m"
#define CLR_RED    "\033[1;31m"

// ---- 状态变量 ----
static std::string g_tun_ip;
static std::string g_server_ip;
static bool        g_connected = false;
static bool        g_authed   = false;
static bool        g_registered = false;
static HANDLE      g_hChildStdOut = NULL;
static HANDLE      g_hProcess = NULL;

void ClearScreen() {
    printf("\033[2J\033[H");
}

void DrawPanel() {
    ClearScreen();
    printf("%s╔══════════════════════════════════════╗%s\n", CLR_CYAN, CLR_RESET);
    printf("%s║       Tunnel Client  状态面板       ║%s\n", CLR_CYAN, CLR_RESET);
    printf("%s╠══════════════════════════════════════╣%s\n", CLR_CYAN, CLR_RESET);

    printf("║  连接状态: ");
    if (g_connected) printf("%s已连接%s",    CLR_GREEN, CLR_RESET);
    else             printf("%s未连接%s",    CLR_RED,   CLR_RESET);
    printf("                          ║\n");

    printf("║  鉴　权:   ");
    if (g_authed)   printf("%s已通过%s",    CLR_GREEN, CLR_RESET);
    else             printf("%s等待中%s",    CLR_YELLOW,CLR_RESET);
    printf("                          ║\n");

    printf("║  注　册:   ");
    if (g_registered) printf("%s已注册%s",  CLR_GREEN, CLR_RESET);
    else               printf("%s等待中%s",  CLR_YELLOW,CLR_RESET);
    printf("                          ║\n");

    printf("╠══════════════════════════════════════╣%s\n", CLR_CYAN, CLR_RESET);
    printf("║  TUN IP:   ");
    if (!g_tun_ip.empty())
        printf("%s%-20s%s", CLR_GREEN, g_tun_ip.c_str(), CLR_RESET);
    else
        printf("%s(未分配)%s", CLR_YELLOW, CLR_RESET);
    printf("               ║\n");

    printf("%s╚══════════════════════════════════════╝%s\n", CLR_CYAN, CLR_RESET);
    printf("\n  按 Ctrl+C 退出\n");
}

void ParseLine(const std::string& line) {
    // 检测关键日志
    if (line.find("connected to server") != std::string::npos) {
        g_connected = true;
        // 提取 server IP
        size_t pos = line.find("connected to server ");
        if (pos != std::string::npos) {
            g_server_ip = line.substr(pos + 21);
        }
    }
    if (line.find("sent AUTH") != std::string::npos) {
        g_authed = true;
    }
    if (line.find("REGISTER success") != std::string::npos) {
        g_registered = true;
    }
    if (line.find("TUN IP assigned:") != std::string::npos) {
        size_t pos = line.find("TUN IP assigned:");
        if (pos != std::string::npos) {
            g_tun_ip = line.substr(pos + 16);
            // trim
            while (!g_tun_ip.empty() && g_tun_ip[0] == ' ') g_tun_ip.erase(0, 1);
            while (!g_tun_ip.empty() && (g_tun_ip.back() == '\r' || g_tun_ip.back() == '\n' || g_tun_ip.back() == ' '))
                g_tun_ip.pop_back();
        }
    }
    if (line.find("heartbeat timeout") != std::string::npos ||
        line.find("server closed") != std::string::npos) {
        g_connected = false;
        g_authed = false;
        g_registered = false;
        g_tun_ip.clear();
    }
}

DWORD WINAPI ReaderThread(LPVOID) {
    char buf[4096];
    DWORD n;
    while (ReadFile(g_hChildStdOut, buf, sizeof(buf) - 1, &n, NULL) && n > 0) {
        buf[n] = '\0';
        // 可能包含多行
        std::string chunk(buf, n);
        size_t start = 0, end;
        while ((end = chunk.find('\n', start)) != std::string::npos) {
            std::string line = chunk.substr(start, end - start);
            ParseLine(line);
            start = end + 1;
        }
        DrawPanel();
    }
    // 子进程退出
    g_connected = false;
    DrawPanel();
    printf("\n%s客户端已退出，按任意键关闭窗口...%s\n", CLR_YELLOW, CLR_RESET);
    return 0;
}

int main(int argc, char** argv) {
    // 启用 ANSI 转义 (Win10+)
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hStdOut, &mode);
    SetConsoleMode(hStdOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    const char* cfg = "tunnel_client.conf";
    if (argc > 1) cfg = argv[1];

    // 构造命令行: tunnel_client.exe -c <config>
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    // 去掉自身文件名，拼接 tunnel_client.exe
    std::string dir(exe_path);
    size_t last = dir.find_last_of("\\/");
    if (last != std::string::npos) dir = dir.substr(0, last + 1);

    std::string cmd = "\"" + dir + "tunnel_client.exe\" -c \"" + cfg + "\"";

    // 创建管道
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        fprintf(stderr, "CreatePipe failed: %lu\n", GetLastError());
        return 1;
    }
    g_hChildStdOut = hRead;

    // 启动子进程
    STARTUPINFOA si = { sizeof(si) };
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    DrawPanel();
    printf("  启动中...\n");

    if (!CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, TRUE, 0,
                        NULL, dir.c_str(), &si, &pi)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }

    CloseHandle(hWrite);
    g_hProcess = pi.hProcess;

    // 启动读取线程
    HANDLE hThread = CreateThread(NULL, 0, ReaderThread, NULL, 0, NULL);

    // 等待子进程退出或用户按 Ctrl+C
    WaitForSingleObject(pi.hProcess, INFINITE);

    WaitForSingleObject(hThread, 2000);
    CloseHandle(hThread);
    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    printf("\n按任意键关闭...");
    getchar();
    return 0;
}

#else
#include <stdio.h>
int main() {
    fprintf(stderr, "launcher 仅支持 Windows\n");
    return 1;
}
#endif
