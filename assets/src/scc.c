#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define ENV_FILE_LIMIT (1024 * 1024)
#define DEFINED_NAME_LIMIT 256

typedef struct {
    wchar_t *text;
    size_t length;
    size_t capacity;
} WideString;

static wchar_t *defined_names[DEFINED_NAME_LIMIT];
static size_t defined_count = 0;

static const wchar_t *base_environment_names[] = {
    L"SystemRoot", L"WINDIR", L"SystemDrive", L"ComSpec", L"PATHEXT",
    L"NUMBER_OF_PROCESSORS", L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_IDENTIFIER",
    L"PROCESSOR_LEVEL", L"PROCESSOR_REVISION", L"OS", L"ProgramData",
    L"ProgramFiles", L"ProgramFiles(x86)", L"CommonProgramFiles",
    L"CommonProgramFiles(x86)", L"PUBLIC", L"ALLUSERSPROFILE", L"USERNAME",
    L"USERDOMAIN", L"USERDNSDOMAIN", L"COMPUTERNAME", L"LOGONSERVER",
    L"LANG", L"LC_ALL", L"LC_CTYPE"
};

static int name_in_list(const wchar_t *name, const wchar_t *const *list, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (_wcsicmp(name, list[i]) == 0) return 1;
    }
    return 0;
}

static int is_base_environment_name(const wchar_t *name) {
    return name_in_list(name, base_environment_names,
                        sizeof(base_environment_names) / sizeof(base_environment_names[0]));
}

static int is_defined_name(const wchar_t *name) {
    return name_in_list(name, (const wchar_t *const *)defined_names, defined_count);
}

static int remember_defined_name(const wchar_t *name) {
    wchar_t *copy;
    if (is_defined_name(name)) return 1;
    if (defined_count >= DEFINED_NAME_LIMIT) return 0;
    copy = _wcsdup(name);
    if (!copy) return 0;
    defined_names[defined_count++] = copy;
    return 1;
}

static void free_defined_names(void) {
    size_t i;
    for (i = 0; i < defined_count; i++) free(defined_names[i]);
    defined_count = 0;
}

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
        if (capacity > 32768 * 4) return 0;
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

static int ws_set(WideString *s, const wchar_t *value) {
    s->length = 0;
    if (s->text) s->text[0] = L'\0';
    return ws_append(s, value);
}

static int fail_message(const wchar_t *message) {
    fwprintf(stderr, L"scc: %ls\n", message);
    return 1;
}

static int fail_windows(const wchar_t *operation) {
    DWORD error = GetLastError();
    fwprintf(stderr, L"scc: %ls failed (Windows error %lu).\n", operation,
             (unsigned long)error);
    return 1;
}

static int fail_line(size_t line, const wchar_t *reason) {
    fwprintf(stderr, L"scc: portable.env line %llu: %ls\n",
             (unsigned long long)line, reason);
    return 1;
}

static int path_join(WideString *out, const wchar_t *root, const wchar_t *relative) {
    size_t length;
    if (!ws_set(out, root)) return 0;
    length = out->length;
    if (length && out->text[length - 1] != L'\\' && out->text[length - 1] != L'/') {
        if (!ws_append_char(out, L'\\')) return 0;
    }
    return ws_append(out, relative);
}

static int get_module_root(WideString *root) {
    size_t capacity = 1024;
    for (;;) {
        DWORD length;
        wchar_t *slash;
        if (!ws_reserve(root, capacity)) return 0;
        SetLastError(ERROR_SUCCESS);
        length = GetModuleFileNameW(NULL, root->text, (DWORD)root->capacity);
        if (!length) return 0;
        if ((size_t)length < root->capacity - 1) {
            root->length = (size_t)length;
            root->text[root->length] = L'\0';
            slash = wcsrchr(root->text, L'\\');
            if (!slash) return 0;
            if (slash == root->text + 2 && root->text[1] == L':') {
                root->length = (size_t)(slash - root->text) + 1;
                root->text[root->length] = L'\0';
            } else {
                *slash = L'\0';
                root->length = (size_t)(slash - root->text);
            }
            return 1;
        }
        if (root->capacity >= 32768) return 0;
        capacity = root->capacity * 2;
    }
}

static int file_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static int directory_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static int ensure_directory(const wchar_t *path) {
    wchar_t *copy;
    wchar_t *slash;
    size_t length;
    int ok = 1;
    if (directory_exists(path)) return 1;
    if (file_exists(path)) return 0;
    copy = _wcsdup(path);
    if (!copy) return 0;
    length = wcslen(copy);
    while (length > 1 && (copy[length - 1] == L'\\' || copy[length - 1] == L'/')) {
        if (length == 3 && copy[1] == L':') break;
        copy[--length] = L'\0';
    }
    slash = wcsrchr(copy, L'\\');
    if (!slash) slash = wcsrchr(copy, L'/');
    if (slash) {
        if (slash == copy + 2 && copy[1] == L':') {
            slash[1] = L'\0';
        } else if (slash != copy) {
            *slash = L'\0';
        } else {
            slash = NULL;
        }
        if (slash && wcslen(copy) && !directory_exists(copy)) {
            ok = ensure_directory(copy);
        }
    }
    free(copy);
    if (!ok) return 0;
    if (CreateDirectoryW(path, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS && directory_exists(path);
}

static int read_utf8_file(const wchar_t *path, wchar_t **text_out) {
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    char *bytes = NULL;
    wchar_t *text = NULL;
    DWORD total = 0;
    int converted;
    int result = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > ENV_FILE_LIMIT)
        goto done;
    bytes = (char *)malloc((size_t)size.QuadPart + 1);
    if (!bytes) goto done;
    while (total < (DWORD)size.QuadPart) {
        DWORD count = 0;
        if (!ReadFile(file, bytes + total, (DWORD)size.QuadPart - total, &count, NULL) || !count)
            goto done;
        total += count;
    }
    if (memchr(bytes, 0, total)) goto done;
    converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)total, NULL, 0);
    if (converted <= 0 && total != 0) goto done;
    text = (wchar_t *)malloc(((size_t)converted + 1) * sizeof(wchar_t));
    if (!text) goto done;
    if (converted && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)total,
                                         text, converted) != converted)
        goto done;
    text[converted] = L'\0';
    *text_out = text;
    text = NULL;
    result = 1;
done:
    free(text);
    free(bytes);
    CloseHandle(file);
    return result;
}

static wchar_t *trim(wchar_t *value) {
    wchar_t *end;
    while (*value == L' ' || *value == L'\t' || *value == L'\r' || *value == L'\n') value++;
    end = value + wcslen(value);
    while (end > value && (end[-1] == L' ' || end[-1] == L'\t' ||
                           end[-1] == L'\r' || end[-1] == L'\n')) end--;
    *end = L'\0';
    return value;
}

static int valid_env_name(const wchar_t *name) {
    size_t i;
    if (!name || !(iswalpha(name[0]) || name[0] == L'_')) return 0;
    for (i = 1; name[i]; i++) {
        if (!(iswalnum(name[i]) || name[i] == L'_')) return 0;
    }
    return 1;
}

static int get_environment(const wchar_t *name, WideString *value, int *found) {
    DWORD length;
    DWORD required;
    *found = 0;
    if (!ws_reserve(value, 256)) return 0;
    SetLastError(ERROR_SUCCESS);
    length = GetEnvironmentVariableW(name, value->text, (DWORD)value->capacity);
    if (!length) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            value->length = 0;
            value->text[0] = L'\0';
            *found = 1;
            return 1;
        }
        return 0;
    }
    if ((size_t)length >= value->capacity) {
        required = length + 1;
        if (!ws_reserve(value, required)) return 0;
        length = GetEnvironmentVariableW(name, value->text, (DWORD)value->capacity);
        if (!length || (size_t)length >= value->capacity) return 0;
    }
    value->length = (size_t)length;
    value->text[value->length] = L'\0';
    *found = 1;
    return 1;
}

static int expand_environment(const wchar_t *input, WideString *output, size_t line) {
    size_t i = 0;
    output->length = 0;
    if (output->text) output->text[0] = L'\0';
    while (input[i]) {
        if (input[i] == L'$' && input[i + 1] == L'{') {
            size_t end = i + 2;
            wchar_t name[128];
            size_t name_length;
            WideString value = {0};
            int found = 0;
            while (input[end] && input[end] != L'}') end++;
            if (!input[end]) return fail_line(line, L"unclosed variable reference");
            name_length = end - i - 2;
            if (!name_length || name_length >= sizeof(name) / sizeof(name[0]))
                return fail_line(line, L"invalid variable name");
            memcpy(name, input + i + 2, name_length * sizeof(wchar_t));
            name[name_length] = L'\0';
            if (!valid_env_name(name)) return fail_line(line, L"invalid variable name");
            if (!is_defined_name(name) && !is_base_environment_name(name))
                return fail_line(line, L"referenced variable is not defined by this file");
            if (!get_environment(name, &value, &found) || !found || !value.length) {
                ws_free(&value);
                return fail_line(line, L"referenced variable is not defined or is empty");
            }
            if (!ws_append_n(output, value.text, value.length)) {
                ws_free(&value);
                return fail_line(line, L"expanded value is too long");
            }
            ws_free(&value);
            i = end + 1;
        } else {
            if (!ws_append_char(output, input[i])) return fail_line(line, L"value is too long");
            i++;
        }
    }
    return 0;
}

static int apply_environment_file(const wchar_t *text) {
    wchar_t *copy = _wcsdup(text);
    wchar_t *cursor;
    size_t line = 1;
    int result = 0;
    if (!copy) return fail_message(L"out of memory");
    if (copy[0] == 0xFEFF) memmove(copy, copy + 1, (wcslen(copy + 1) + 1) * sizeof(wchar_t));
    cursor = copy;
    while (*cursor) {
        wchar_t *start = cursor;
        wchar_t *key;
        wchar_t *value;
        wchar_t *equals;
        WideString expanded = {0};
        while (*cursor && *cursor != L'\r' && *cursor != L'\n') cursor++;
        if (*cursor) {
            wchar_t delimiter = *cursor;
            *cursor++ = L'\0';
            if (delimiter == L'\r' && *cursor == L'\n') cursor++;
        }
        key = trim(start);
        if (!*key || *key == L'#' || *key == L';') {
            line++;
            continue;
        }
        equals = wcschr(key, L'=');
        if (!equals) {
            result = fail_line(line, L"expected NAME=VALUE");
            break;
        }
        *equals = L'\0';
        value = trim(equals + 1);
        key = trim(key);
        if (!valid_env_name(key)) {
            result = fail_line(line, L"invalid environment variable name");
            break;
        }
        if (expand_environment(value, &expanded, line)) {
            ws_free(&expanded);
            result = 1;
            break;
        }
        if (!SetEnvironmentVariableW(key, expanded.text ? expanded.text : L"")) {
            ws_free(&expanded);
            result = fail_line(line, L"SetEnvironmentVariable failed");
            break;
        }
        if (!remember_defined_name(key)) {
            ws_free(&expanded);
            result = fail_line(line, L"too many environment variables");
            break;
        }
        ws_free(&expanded);
        line++;
    }
    free(copy);
    return result;
}

static int clear_host_environment(void) {
    static const wchar_t *names[] = {
        L"PYTHONHOME", L"PYTHONPATH", L"PYTHONSTARTUP", L"NODE_PATH",
        L"NODE_OPTIONS", L"GIT_EXEC_PATH", L"GIT_TEMPLATE_DIR", L"GIT_DIR",
        L"GIT_WORK_TREE", L"GIT_CONFIG_SYSTEM", L"GIT_CONFIG_GLOBAL",
        L"VIRTUAL_ENV", L"PIP_CONFIG_FILE", L"UV_PYTHON",
        L"UV_PROJECT_ENVIRONMENT", L"NPM_CONFIG_PREFIX", L"NPM_CONFIG_CACHE",
        L"NPM_CONFIG_USERCONFIG", L"PSModulePath", L"PLAYWRIGHT_BROWSERS_PATH"
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!SetEnvironmentVariableW(names[i], NULL)) return 0;
    }
    return 1;
}

static int require_environment(const wchar_t *name) {
    WideString value = {0};
    int found = 0;
    int ok = is_defined_name(name) && get_environment(name, &value, &found) &&
             found && value.length;
    ws_free(&value);
    return ok;
}

static int environment_entry_allowed(const wchar_t *entry) {
    const wchar_t *equals = wcschr(entry, L'=');
    wchar_t name[128];
    size_t length;
    if (!equals || equals == entry) return 0;
    length = (size_t)(equals - entry);
    if (length >= sizeof(name) / sizeof(name[0])) return 0;
    memcpy(name, entry, length * sizeof(wchar_t));
    name[length] = L'\0';
    return is_base_environment_name(name) || is_defined_name(name);
}

static int compare_environment_entries(const void *left, const void *right) {
    const wchar_t *a = *(const wchar_t *const *)left;
    const wchar_t *b = *(const wchar_t *const *)right;
    return _wcsicmp(a, b);
}

static int build_environment_block(WideString *block) {
    wchar_t *environment = GetEnvironmentStringsW();
    wchar_t **entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    wchar_t *cursor;
    size_t i;
    int result = 0;
    if (!environment) return 0;
    for (cursor = environment; *cursor;) {
        size_t length = wcslen(cursor);
        if (environment_entry_allowed(cursor)) {
            wchar_t *copy;
            if (count == capacity) {
                size_t next_capacity = capacity ? capacity * 2 : 32;
                wchar_t **next = (wchar_t **)realloc(entries, next_capacity * sizeof(wchar_t *));
                if (!next) goto done;
                entries = next;
                capacity = next_capacity;
            }
            copy = _wcsdup(cursor);
            if (!copy) goto done;
            entries[count++] = copy;
        }
        cursor += length + 1;
    }
    qsort(entries, count, sizeof(wchar_t *), compare_environment_entries);
    if (!count) {
        if (!ws_append_char(block, L'\0') || !ws_append_char(block, L'\0')) goto done;
    } else {
        for (i = 0; i < count; i++) {
            if (!ws_append_n(block, entries[i], wcslen(entries[i]) + 1)) goto done;
        }
        if (!ws_append_char(block, L'\0')) goto done;
    }
    result = 1;
done:
    for (i = 0; i < count; i++) free(entries[i]);
    free(entries);
    FreeEnvironmentStringsW(environment);
    return result;
}

static int write_utf8_file_atomic(const wchar_t *path, const wchar_t *text) {
    WideString temporary = {0};
    int converted;
    char *bytes = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD total = 0;
    DWORD written;
    int result = 0;
    if (!ws_set(&temporary, path) || !ws_append(&temporary, L".scc-new")) goto done;
    converted = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (converted <= 1) goto done;
    bytes = (char *)malloc((size_t)converted);
    if (!bytes) goto done;
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, converted, NULL, NULL) != converted)
        goto done;
    file = CreateFileW(temporary.text, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    while (total < (DWORD)(converted - 1)) {
        DWORD chunk = (DWORD)(converted - 1) - total;
        if (!WriteFile(file, bytes + total, chunk, &written, NULL) || !written) goto done;
        total += written;
    }
    if (!FlushFileBuffers(file)) goto done;
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary.text, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        goto done;
    result = 1;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(bytes);
    ws_free(&temporary);
    return result;
}

static int valid_python_directory_name(const wchar_t *name) {
    size_t i;
    if (!name[0] || wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) return 0;
    for (i = 0; name[i]; i++) {
        if (!(iswalnum(name[i]) || name[i] == L'.' || name[i] == L'-' || name[i] == L'_'))
            return 0;
    }
    return 1;
}

static int repair_venv_paths(const wchar_t *root) {
    WideString marker_path = {0}, base_root = {0}, base_dir = {0}, base_python = {0};
    WideString config_path = {0}, desired = {0};
    wchar_t *marker = NULL;
    wchar_t *config = NULL;
    wchar_t *marker_value;
    int result = 0;
    size_t existing_offset = 0;
    if (!path_join(&marker_path, root, L"runtime\\uv-python\\ACTIVE_PYTHON") ||
        !path_join(&base_root, root, L"runtime\\uv-python") ||
        !path_join(&config_path, root, L"runtime\\venv\\pyvenv.cfg")) {
        goto done;
    }
    if (!read_utf8_file(marker_path.text, &marker)) {
        fwprintf(stderr, L"scc: cannot read runtime/uv-python/ACTIVE_PYTHON\n");
        goto done;
    }
    marker_value = marker;
    if (marker_value[0] == 0xFEFF) marker_value++;
    marker_value = trim(marker_value);
    if (!valid_python_directory_name(marker_value)) {
        fwprintf(stderr, L"scc: ACTIVE_PYTHON contains an invalid directory name\n");
        goto done;
    }
    if (!path_join(&base_dir, base_root.text, marker_value) ||
        !path_join(&base_python, base_dir.text, L"python.exe")) goto done;
    if (!file_exists(base_python.text)) {
        fwprintf(stderr, L"scc: the active managed Python interpreter is missing\n");
        goto done;
    }
    if (!ws_append(&desired, L"home = ") || !ws_append(&desired, base_dir.text) ||
        !ws_append(&desired, L"\r\ninclude-system-site-packages = false\r\nexecutable = ") ||
        !ws_append(&desired, base_python.text) || !ws_append(&desired, L"\r\n")) goto done;
    if (read_utf8_file(config_path.text, &config)) {
        if (config[0] == 0xFEFF) existing_offset = 1;
        if (wcscmp(config + existing_offset, desired.text) == 0) {
            result = 1;
            goto done;
        }
    }
    result = write_utf8_file_atomic(config_path.text, desired.text);
    if (!result) fwprintf(stderr, L"scc: cannot update runtime/venv/pyvenv.cfg\n");
done:
    free(marker);
    free(config);
    ws_free(&marker_path);
    ws_free(&base_root);
    ws_free(&base_dir);
    ws_free(&base_python);
    ws_free(&config_path);
    ws_free(&desired);
    return result;
}

static int prepare_directories(const wchar_t *root) {
    static const wchar_t *relative[] = {
        L"data\\.claude", L"data\\home", L"data\\tmp", L"data\\cache",
        L"data\\AppData\\Roaming", L"data\\AppData\\Local",
        L"data\\home\\Documents\\PowerShell\\Modules"
    };
    static const wchar_t *variables[] = {
        L"CLAUDE_CONFIG_DIR", L"HOME", L"USERPROFILE", L"APPDATA", L"LOCALAPPDATA",
        L"TEMP", L"TMP", L"UV_CACHE_DIR", L"PIP_CACHE_DIR", L"NPM_CONFIG_CACHE",
        L"PLAYWRIGHT_BROWSERS_PATH"
    };
    size_t i;
    for (i = 0; i < sizeof(relative) / sizeof(relative[0]); i++) {
        WideString path = {0};
        int ok = path_join(&path, root, relative[i]) && ensure_directory(path.text);
        ws_free(&path);
        if (!ok) return 0;
    }
    for (i = 0; i < sizeof(variables) / sizeof(variables[0]); i++) {
        WideString value = {0};
        int found = 0;
        int ok = get_environment(variables[i], &value, &found) && found && value.length &&
                 ensure_directory(value.text);
        ws_free(&value);
        if (!ok) return 0;
    }
    return 1;
}

static int validate_runtime(const wchar_t *root) {
    static const wchar_t *relative[] = {
        L"claude.exe", L"runtime\\node\\node.exe", L"runtime\\pwsh\\pwsh.exe",
        L"runtime\\git\\bin\\bash.exe", L"runtime\\uv\\uv.exe",
        L"runtime\\venv\\Scripts\\python.exe"
    };
    size_t i;
    for (i = 0; i < sizeof(relative) / sizeof(relative[0]); i++) {
        WideString path = {0};
        int ok = path_join(&path, root, relative[i]) && file_exists(path.text);
        ws_free(&path);
        if (!ok) {
            fwprintf(stderr, L"scc: required runtime component is missing (index %lu).\n",
                     (unsigned long)i);
            return 0;
        }
    }
    return 1;
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

static int forbidden_permission_option(const wchar_t *argument) {
    return wcscmp(argument, L"--permission-mode") == 0 ||
           wcsncmp(argument, L"--permission-mode=", 18) == 0 ||
           wcscmp(argument, L"--dangerously-skip-permissions") == 0 ||
           wcscmp(argument, L"--allow-dangerously-skip-permissions") == 0;
}

static int build_child_command(WideString *command, const wchar_t *claude,
                               int argc, wchar_t **argv) {
    int i;
    if (!append_quoted_argument(command, claude) ||
        !ws_append(command, L" --permission-mode bypassPermissions"))
        return 0;
    for (i = 1; i < argc; i++) {
        if (forbidden_permission_option(argv[i])) return -1;
        if (!ws_append_char(command, L' ') || !append_quoted_argument(command, argv[i]))
            return 0;
    }
    return 1;
}

int wmain(int argc, wchar_t **argv) {
    WideString root = {0}, env_path = {0}, claude = {0}, command = {0}, environment = {0};
    wchar_t *env_text = NULL;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD wait_result;
    DWORD exit_code = 1;
    int result = 1;
    int build_result;
    if (!get_module_root(&root)) {
        result = fail_message(L"cannot determine the portable root directory");
        goto done;
    }
    if (!SetEnvironmentVariableW(L"PORTABLE_ROOT", root.text) ||
        !remember_defined_name(L"PORTABLE_ROOT") || !clear_host_environment()) {
        result = fail_windows(L"initializing the portable environment");
        goto done;
    }
    if (!path_join(&env_path, root.text, L"config\\portable.env")) {
        result = fail_message(L"portable environment path is too long");
        goto done;
    }
    if (!read_utf8_file(env_path.text, &env_text)) {
        result = fail_message(L"cannot read config/portable.env as UTF-8");
        goto done;
    }
    if (apply_environment_file(env_text)) {
        result = 1;
        goto done;
    }
    if (!require_environment(L"PATH") || !require_environment(L"CLAUDE_CONFIG_DIR") ||
        !require_environment(L"HOME") || !require_environment(L"USERPROFILE") ||
        !require_environment(L"APPDATA") || !require_environment(L"LOCALAPPDATA") ||
        !require_environment(L"TEMP") || !require_environment(L"TMP") ||
        !require_environment(L"VIRTUAL_ENV") || !require_environment(L"VIRTUAL_ENV_PROMPT") ||
        !require_environment(L"UV_PYTHON_INSTALL_DIR") ||
        !require_environment(L"UV_PROJECT_ENVIRONMENT") ||
        !require_environment(L"UV_CACHE_DIR") || !require_environment(L"PIP_CACHE_DIR") ||
        !require_environment(L"PLAYWRIGHT_BROWSERS_PATH") ||
        !require_environment(L"NPM_CONFIG_CACHE") ||
        !require_environment(L"NPM_CONFIG_USERCONFIG") ||
        !require_environment(L"CLAUDE_CODE_GIT_BASH_PATH") ||
        !require_environment(L"CLAUDE_CODE_USE_POWERSHELL_TOOL")) {
        result = fail_message(L"portable.env is missing a required variable");
        goto done;
    }
    if (!prepare_directories(root.text)) {
        result = fail_message(L"cannot create a portable data or cache directory");
        goto done;
    }
    if (!repair_venv_paths(root.text)) {
        result = fail_message(L"cannot repair the portable Python environment paths");
        goto done;
    }
    if (!validate_runtime(root.text)) {
        result = 1;
        goto done;
    }
    if (!path_join(&claude, root.text, L"claude.exe")) {
        result = fail_message(L"Claude executable path is too long");
        goto done;
    }
    build_result = build_child_command(&command, claude.text, argc, argv);
    if (build_result < 0) {
        result = fail_message(L"permission-mode overrides are not allowed; scc always uses bypassPermissions");
        goto done;
    }
    if (!build_result) {
        result = fail_message(L"child command line is too long");
        goto done;
    }
    if (!build_environment_block(&environment)) {
        result = fail_message(L"cannot build an isolated child environment");
        goto done;
    }
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(claude.text, command.text, NULL, NULL, TRUE,
                        CREATE_UNICODE_ENVIRONMENT, environment.text, NULL,
                        &startup, &process)) {
        result = fail_windows(L"starting claude.exe");
        goto done;
    }
    wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        result = fail_windows(L"waiting for claude.exe");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        goto done;
    }
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        result = fail_windows(L"reading the Claude exit code");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        goto done;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result = (int)exit_code;
done:
    free(env_text);
    free_defined_names();
    ws_free(&root);
    ws_free(&env_path);
    ws_free(&claude);
    ws_free(&command);
    ws_free(&environment);
    return result;
}
