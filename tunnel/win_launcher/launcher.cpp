#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <string>
#include <vector>

// ---- globals ----
static PROCESS_INFORMATION g_pi = {};
static HWND g_hStatus = nullptr;
static HWND g_hTunIp  = nullptr;
static HWND g_hBtn    = nullptr;
static HWND g_hPeers  = nullptr;
static bool  g_running = false;
static std::string g_cfg_path;

static HFONT g_hFont = nullptr;

// ---- helpers ----
static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    w.resize(len - 1);
    return w;
}

static std::string GetTunIp() {
    ULONG sz = 0;
    GetAdaptersInfo(nullptr, &sz);
    if (!sz) return "";
    std::vector<char> buf(sz);
    auto* ad = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    if (GetAdaptersInfo(ad, &sz) != ERROR_SUCCESS) return "";
    for (auto* a = ad; a; a = a->Next) {
        std::string desc(a->Description);
        if (desc.find("Tunnel") != std::string::npos ||
            desc.find("Wintun") != std::string::npos) {
            return a->IpAddressList.IpAddress.String;
        }
    }
    return "";
}

// ---- auto-elevate ----
static bool IsAdmin() {
    BOOL is = FALSE;
    PSID admins = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                             DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &admins);
    CheckTokenMembership(nullptr, admins, &is);
    FreeSid(admins);
    return is;
}

static void RelaunchAsAdmin() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring args = L"\"";
    args += self;
    args += L"\" " + ToWide(g_cfg_path);
    ShellExecuteW(nullptr, L"runas", self, args.c_str(), nullptr, SW_SHOW);
}

// ---- tunnel control ----
static void StartTunnel() {
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string dir(self);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);

    std::string exe = dir + "tunnel_client.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        exe = dir + "..\\tunnel\\tunnel_client.exe";
    }

    std::string cmd = "\"" + exe + "\" -c \"" + g_cfg_path + "\"";

    STARTUPINFOA si = { sizeof(si) };
    std::vector<char> cbuf(cmd.begin(), cmd.end());
    cbuf.push_back('\0');

    if (CreateProcessA(nullptr, cbuf.data(), nullptr, nullptr, FALSE,
                       0, nullptr, dir.c_str(), &si, &g_pi)) {
        g_running = true;
    } else {
        MessageBoxA(nullptr, cmd.c_str(), "启动 tunnel_client 失败", MB_OK | MB_ICONERROR);
    }
}

static void StopTunnel() {
    if (g_pi.hProcess) {
        TerminateProcess(g_pi.hProcess, 0);
        CloseHandle(g_pi.hProcess);
        CloseHandle(g_pi.hThread);
        g_pi = {};
    }
    g_running = false;
}

static bool IsRunning() {
    if (!g_pi.hProcess) return false;
    DWORD code = 0;
    GetExitCodeProcess(g_pi.hProcess, &code);
    if (code != STILL_ACTIVE) {
        CloseHandle(g_pi.hProcess);
        CloseHandle(g_pi.hThread);
        g_pi = {};
        g_running = false;
        return false;
    }
    return true;
}

// ---- refresh UI ----
static void RefreshUI() {
    bool alive = IsRunning();
    std::string ip = GetTunIp();

    SetWindowTextW(g_hStatus, alive ? L"Connected" : L"Disconnected");
    SetWindowTextW(g_hTunIp,  ip.empty() ? L"(waiting)" : ToWide(ip).c_str());
    SetWindowTextW(g_hBtn,    alive ? L"Stop" : L"Start");
    InvalidateRect(g_hStatus, nullptr, TRUE);

    // Read peers from file
    std::string peers;
    FILE* f = fopen("tunnel_peers.txt", "r");
    if (f) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), f)) peers += buf;
        fclose(f);
    }
    if (peers.empty()) peers = "(no peers)";
    SetWindowTextW(g_hPeers, ToWide(peers).c_str());
}

// ---- window proc ----
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Create font
        g_hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        // Title
        CreateWindowW(L"STATIC", L"Tunnel Client",
                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                      20, 20, 340, 30, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(GetDlgItem(hwnd, 1), WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Status
        CreateWindowW(L"STATIC", L"Status:", WS_CHILD | WS_VISIBLE,
                      30, 70, 80, 25, hwnd, nullptr, nullptr, nullptr);
        g_hStatus = CreateWindowW(L"STATIC", L"Disconnected",
                                  WS_CHILD | WS_VISIBLE | SS_CENTER,
                                  110, 70, 230, 25, hwnd, nullptr, nullptr, nullptr);

        // TUN IP
        CreateWindowW(L"STATIC", L"TUN IP:", WS_CHILD | WS_VISIBLE,
                      30, 110, 80, 25, hwnd, nullptr, nullptr, nullptr);
        g_hTunIp = CreateWindowW(L"STATIC", L"(waiting)",
                                 WS_CHILD | WS_VISIBLE | SS_CENTER,
                                 110, 110, 230, 30, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(g_hTunIp, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Start/Stop button
        g_hBtn = CreateWindowW(L"BUTTON", L"Start",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               110, 170, 160, 35, hwnd, (HMENU)100, nullptr, nullptr);

        // Peer list label
        CreateWindowW(L"STATIC", L"Peers:", WS_CHILD | WS_VISIBLE,
                      30, 225, 80, 20, hwnd, nullptr, nullptr, nullptr);
        g_hPeers = CreateWindowW(L"STATIC", L"(no peers)",
                                 WS_CHILD | WS_VISIBLE,
                                 30, 245, 320, 80, hwnd, nullptr, nullptr, nullptr);

        // Timer for refresh
        SetTimer(hwnd, 1, 1000, nullptr);

        // Auto-start
        StartTunnel();
        RefreshUI();
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == 100) { // button
            if (IsRunning()) {
                StopTunnel();
            } else {
                StartTunnel();
            }
            RefreshUI();
        }
        return 0;

    case WM_TIMER:
        if (wp == 1) RefreshUI();
        return 0;

    case WM_DESTROY:
        StopTunnel();
        if (g_hFont) DeleteObject(g_hFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Find config
    g_cfg_path = "tunnel_client.conf";
    LPSTR cmdline = GetCommandLineA();
    // Parse: if an argument is passed that looks like a path, use it
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        char buf[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, buf, MAX_PATH, nullptr, nullptr);
        g_cfg_path = buf;
    } else {
        // 未指定参数: 优先 exe 同目录的 tunnel_client.conf (双击运行时 cwd 不可靠)
        char self[MAX_PATH];
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string dir(self);
        size_t pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
        std::string exe_dir_cfg = dir + "tunnel_client.conf";
        if (GetFileAttributesA(exe_dir_cfg.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_cfg_path = exe_dir_cfg;
        } else {
            g_cfg_path = "tunnel_client.conf";  // 回退: 当前工作目录
        }
    }
    LocalFree(argv);

    // Resolve to absolute path
    char abs[MAX_PATH];
    GetFullPathNameA(g_cfg_path.c_str(), MAX_PATH, abs, nullptr);
    g_cfg_path = abs;

    // Must be admin
    if (!IsAdmin()) {
        RelaunchAsAdmin();
        return 0;
    }

    // Register window class
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TunnelLauncher";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(L"TunnelLauncher", L"Tunnel Client",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 400, 380,
                              nullptr, nullptr, hInst, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

#else
#include <stdio.h>
int main() { fprintf(stderr, "Windows only\n"); return 1; }
#endif
