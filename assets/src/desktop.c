#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define DEFAULT_PORT 4317
#define WAIT_STEPS 75
#define WAIT_MS 200

static int port_open(unsigned short port) {
    SOCKET s;
    struct sockaddr_in addr;
    int ok;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    ok = connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    closesocket(s);
    return ok;
}

static int file_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void show_error(const wchar_t *message) {
    MessageBoxW(NULL, message, L"Claude Code", MB_OK | MB_ICONERROR);
}

static void open_browser(unsigned short port) {
    wchar_t url[64];
    swprintf(url, 64, L"http://127.0.0.1:%u", (unsigned)port);
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

int wmain(int argc, wchar_t **argv) {
    wchar_t root[32768];
    wchar_t node_path[32768];
    wchar_t script_path[32768];
    wchar_t command[65536];
    wchar_t *slash;
    DWORD length;
    unsigned short port = DEFAULT_PORT;
    int open_after = 1;
    int i;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    WSADATA wsa;
    BOOL created = FALSE;
    BOOL running = FALSE;

    for (i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--no-open") == 0) {
            open_after = 0;
        } else if (wcscmp(argv[i], L"--port") == 0 && i + 1 < argc) {
            port = (unsigned short)_wtoi(argv[++i]);
        } else if (wcsncmp(argv[i], L"--port=", 7) == 0) {
            port = (unsigned short)_wtoi(argv[i] + 7);
        }
    }
    if (port == 0) port = DEFAULT_PORT;

    length = GetModuleFileNameW(NULL, root, 32768);
    if (!length || length >= 32768) {
        show_error(L"无法确定 desktop.exe 所在目录。");
        return 1;
    }
    slash = wcsrchr(root, L'\\');
    if (!slash) {
        show_error(L"无法确定 desktop.exe 所在目录。");
        return 1;
    }
    *slash = L'\0';

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        show_error(L"网络初始化失败。");
        return 1;
    }

    if (!port_open(port)) {
        if (swprintf(node_path, 32768, L"%s\\runtime\\node\\node.exe", root) < 0 ||
            swprintf(script_path, 32768, L"%s\\gui\\server.js", root) < 0 ||
            swprintf(command, 65536, L"\"%s\" \"%s\"", node_path, script_path) < 0) {
            WSACleanup();
            show_error(L"路径过长。");
            return 1;
        }
        if (!file_exists(node_path)) {
            WSACleanup();
            show_error(L"找不到 runtime\\node\\node.exe。便携运行时可能不完整。");
            return 1;
        }
        if (!file_exists(script_path)) {
            WSACleanup();
            show_error(L"找不到 gui\\server.js。");
            return 1;
        }

        ZeroMemory(&startup, sizeof(startup));
        startup.cb = sizeof(startup);
        ZeroMemory(&process, sizeof(process));
        created = CreateProcessW(node_path, command, NULL, NULL, FALSE,
                                 CREATE_NO_WINDOW, NULL, root, &startup, &process);
        if (!created) {
            WSACleanup();
            show_error(L"启动 GUI 服务失败。");
            return 1;
        }

        for (i = 0; i < WAIT_STEPS; i++) {
            if (port_open(port)) {
                running = TRUE;
                break;
            }
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
            Sleep(WAIT_MS);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        if (!running) {
            WSACleanup();
            show_error(L"GUI 服务未能启动。请检查端口是否被占用或运行 gui\\server.js 查看错误。");
            return 1;
        }
    }

    WSACleanup();
    if (open_after) open_browser(port);
    return 0;
}
