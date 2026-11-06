#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <locale.h>

static void print_environment(const wchar_t *name) {
    wchar_t buffer[32768];
    DWORD length = GetEnvironmentVariableW(name, buffer, 32768);
    if (!length) {
        printf("MOCK_ENV_%ls=<unset>\n", name);
    } else if (length >= 32768) {
        printf("MOCK_ENV_%ls=<too-long>\n", name);
    } else {
        printf("MOCK_ENV_%ls=%ls\n", name, buffer);
    }
}

int wmain(int argc, wchar_t **argv) {
    wchar_t cwd[32768];
    int exit_code = 0;
    int read_stdin = 0;
    int i;
    setlocale(LC_ALL, ".UTF-8");
    if (!GetCurrentDirectoryW(32768, cwd)) wcscpy(cwd, L"<unknown>");
    printf("MOCK_ARGC=%d\n", argc);
    for (i = 0; i < argc; i++) {
        printf("MOCK_ARG_%d=%ls\n", i, argv[i]);
        if (wcscmp(argv[i], L"--mock-read-stdin") == 0) read_stdin = 1;
        if (wcsncmp(argv[i], L"--mock-exit=", 12) == 0) exit_code = _wtoi(argv[i] + 12);
    }
    printf("MOCK_CWD=%ls\n", cwd);
    print_environment(L"PORTABLE_ROOT");
    print_environment(L"CLAUDE_CONFIG_DIR");
    print_environment(L"HOME");
    print_environment(L"USERPROFILE");
    print_environment(L"TEMP");
    print_environment(L"VIRTUAL_ENV");
    print_environment(L"PYTHONHOME");
    print_environment(L"PYTHONPATH");
    print_environment(L"SCC_TEST_VALUE");
    print_environment(L"BASH_ENV");
    print_environment(L"GIT_CONFIG_COUNT");
    print_environment(L"PIP_INDEX_URL");
    print_environment(L"UV_INDEX_URL");
    print_environment(L"NODE_EXTRA_CA_CERTS");
    print_environment(L"SSL_CERT_FILE");
    print_environment(L"HTTPS_PROXY");
    print_environment(L"AWS_PROFILE");
    print_environment(L"ANTHROPIC_API_KEY");
    print_environment(L"CLAUDE_CODE_USE_BEDROCK");
    print_environment(L"PATH");
    fprintf(stderr, "MOCK_STDERR\n");
    if (read_stdin) {
        char line[4096];
        while (fgets(line, sizeof(line), stdin)) printf("MOCK_STDIN=%s", line);
    }
    fflush(stdout);
    fflush(stderr);
    return exit_code;
}
