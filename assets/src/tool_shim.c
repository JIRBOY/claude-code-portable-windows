#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

typedef struct {
    wchar_t *text;
    size_t length;
    size_t capacity;
} WideString;

static void ws_free(WideString *s) {
    free(s->text);
    s->text = NULL;
    s->length = 0;
    s->capacity = 0;
}

static int ws_reserve(WideString *s, size_t needed) {
    wchar_t *next;
    size_t capacity;
    if (needed <= s->capacity) return 1;
    capacity = s->capacity ? s->capacity : 256;
    while (capacity < needed) {
        if (capacity > 65536) return 0;
        capacity *= 2;
    }
    next = (wchar_t *)realloc(s->text, capacity * sizeof(wchar_t));
    if (!next) return 0;
    s->text = next;
    s->capacity = capacity;
    return 1;
}

static int ws_append_n(WideString *s, const wchar_t *value, size_t length) {
    if (length > (size_t)-1 - s->length - 1) return 0;
    if (!ws_reserve(s, s->length + length + 1)) return 0;
    if (length) memcpy(s->text + s->length, value, length * sizeof(wchar_t));
    s->length += length;
    s->text[s->length] = L'\0';
    return 1;
}

static int ws_append(WideString *s, const wchar_t *value) {
    return ws_append_n(s, value, wcslen(value));
}

static int ws_append_char(WideString *s, wchar_t value) {
    return ws_append_n(s, &value, 1);
}

static int append_quoted_argument(WideString *command, const wchar_t *argument) {
    const wchar_t *p;
    size_t backslashes = 0;
    if (!ws_append_char(command, L'"')) return 0;
    for (p = argument; *p; p++) {
        if (*p == L'\\') {
            backslashes++;
        } else if (*p == L'"') {
            size_t i;
            for (i = 0; i < backslashes * 2 + 1; i++) {
                if (!ws_append_char(command, L'\\')) return 0;
            }
            backslashes = 0;
            if (!ws_append_char(command, L'"')) return 0;
        } else {
            size_t i;
            for (i = 0; i < backslashes; i++) {
                if (!ws_append_char(command, L'\\')) return 0;
            }
            backslashes = 0;
            if (!ws_append_char(command, *p)) return 0;
        }
    }
    while (backslashes--) {
        if (!ws_append_char(command, L'\\') || !ws_append_char(command, L'\\')) return 0;
    }
    return ws_append_char(command, L'"');
}

static int get_module_directory(WideString *directory) {
    size_t capacity = 1024;
    for (;;) {
        DWORD length;
        wchar_t *slash;
        if (!ws_reserve(directory, capacity)) return 0;
        length = GetModuleFileNameW(NULL, directory->text, (DWORD)directory->capacity);
        if (!length) return 0;
        if ((size_t)length < directory->capacity - 1) {
            directory->length = length;
            directory->text[length] = L'\0';
            slash = wcsrchr(directory->text, L'\\');
            if (!slash) return 0;
            *slash = L'\0';
            directory->length = (size_t)(slash - directory->text);
            return 1;
        }
        if (directory->capacity >= 32768) return 0;
        capacity = directory->capacity * 2;
    }
}

static const wchar_t *module_for_name(const wchar_t *name) {
    if (_wcsicmp(name, L"pip.exe") == 0) return L"pip";
    if (_wcsicmp(name, L"pytest.exe") == 0) return L"pytest";
    if (_wcsicmp(name, L"playwright.exe") == 0) return L"playwright";
    if (_wcsicmp(name, L"markitdown.exe") == 0) return L"markitdown";
    return NULL;
}

int wmain(int argc, wchar_t **argv) {
    WideString bin_directory = {0}, runtime_root = {0}, python = {0}, command = {0};
    const wchar_t *module;
    const wchar_t *name;
    wchar_t *slash;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = 1;
    int result = 1;
    int i;
    if (!get_module_directory(&bin_directory)) {
        fwprintf(stderr, L"tool shim: cannot determine its directory\n");
        goto done;
    }
    name = wcsrchr(argv[0], L'\\');
    name = name ? name + 1 : argv[0];
    module = module_for_name(name);
    if (!module) {
        fwprintf(stderr, L"tool shim: unsupported executable name\n");
        goto done;
    }
    if (!ws_append(&runtime_root, bin_directory.text)) goto done;
    slash = wcsrchr(runtime_root.text, L'\\');
    if (!slash) goto done;
    *slash = L'\0';
    runtime_root.length = (size_t)(slash - runtime_root.text);
    if (!ws_append(&python, runtime_root.text) ||
        !ws_append(&python, L"\\venv\\Scripts\\python.exe")) goto done;
    if (!append_quoted_argument(&command, python.text) ||
        !ws_append(&command, L" -m ") || !ws_append(&command, module)) goto done;
    for (i = 1; i < argc; i++) {
        if (!ws_append_char(&command, L' ') || !append_quoted_argument(&command, argv[i]))
            goto done;
    }
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(python.text, command.text, NULL, NULL, TRUE, 0, NULL, NULL,
                        &startup, &process)) {
        fwprintf(stderr, L"tool shim: cannot start the portable Python environment\n");
        goto done;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) exit_code = 1;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result = (int)exit_code;
done:
    ws_free(&bin_directory);
    ws_free(&runtime_root);
    ws_free(&python);
    ws_free(&command);
    return result;
}
