#ifdef _WIN32

#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <string>
#include <vector>

static PROCESS_INFORMATION g_pi = {};

// ---- 读 Tunnel VPN 适配器 IP ----
std::string GetTunIp() {
    ULONG size = 0;
    GetAdaptersInfo(nullptr, &size);
    if (size == 0) return "";

    std::vector<char> buf(size);
    auto* adapters = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    if (GetAdaptersInfo(adapters, &size) != ERROR_SUCCESS) return "";

    for (auto* a = adapters; a; a = a->Next) {
        std::string desc(a->Description);
        if (desc.find("Tunnel") != std::string::npos ||
            desc.find("Wintun") != std::string::npos) {
            return a->IpAddressList.IpAddress.String;
        }
    }
    return "";
}

void DrawPanel() {
    printf("\033[2J\033[H");
    bool alive = (WaitForSingleObject(g_pi.hProcess, 0) == WAIT_TIMEOUT);
    std::string ip = GetTunIp();

    printf("  ====== Tunnel Client ======\n\n");
    printf("  Status:   ");
    if (alive) printf("\033[1;32mRunning\033[0m"); else printf("\033[1;31mStopped\033[0m");
    printf("\n  ----------\n");
    printf("  TUN IP:   ");
    if (!ip.empty()) printf("\033[1;36m%s\033[0m", ip.c_str());
    else printf("\033[1;33m(waiting)\033[0m");
    printf("\n\n  Press Q to quit\n");
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    const char* cfg = "tunnel_client.conf";
    if (argc > 1) cfg = argv[1];
    char abs_cfg[MAX_PATH];
    GetFullPathNameA(cfg, MAX_PATH, abs_cfg, nullptr);

    // 找 tunnel_client.exe
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string dir(self);
    size_t last = dir.find_last_of("\\/");
    if (last != std::string::npos) dir = dir.substr(0, last + 1);

    std::string exe = dir + "tunnel_client.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        exe = dir + "..\\tunnel\\tunnel_client.exe";
        if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "ERROR: tunnel_client.exe not found\n");
            return 1;
        }
    }

    std::string cmd = "\"" + exe + "\" -c \"" + abs_cfg + "\"";

    STARTUPINFOA si = { sizeof(si) };
    std::vector<char> cbuf(cmd.begin(), cmd.end());
    cbuf.push_back('\0');

    if (!CreateProcessA(nullptr, cbuf.data(), nullptr, nullptr, FALSE,
                        0, nullptr, dir.c_str(), &si, &g_pi)) {
        fprintf(stderr, "ERROR: CreateProcess failed (%lu)\n  %s\n",
                GetLastError(), cmd.c_str());
        return 1;
    }

    DrawPanel();
    for (;;) {
        if (WaitForSingleObject(g_pi.hProcess, 1000) == WAIT_OBJECT_0) break;
        if (GetAsyncKeyState('Q') & 0x8000) { TerminateProcess(g_pi.hProcess, 0); break; }
        DWORD code = 0;
        GetExitCodeProcess(g_pi.hProcess, &code);
        if (code != STILL_ACTIVE) break;
        DrawPanel();
    }

    DrawPanel();
    printf("\n  Stopped. Press any key to exit...\n");
    CloseHandle(g_pi.hProcess);
    CloseHandle(g_pi.hThread);
    getchar();
    return 0;
}

#else
#include <stdio.h>
int main() { fprintf(stderr, "Windows only\n"); return 1; }
#endif
