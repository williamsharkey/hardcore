/* Test-only driver using the production transports and terminal parser.
   No application window, session saving, config writing, or user-keyboard input. */
#define wWinMain hardcore_application_main
#include "../hardcore.c"
#undef wWinMain

typedef struct { const wchar_t *title; HWND hwnd; int count; DWORD owner; } MatrixWindow;
static BOOL CALLBACK matrix_find(HWND hwnd, LPARAM arg) {
    MatrixWindow *window = (MatrixWindow *)arg;
    wchar_t title[256], cls[128];
    GetWindowTextW(hwnd, title, 256); GetClassNameW(hwnd, cls, 128);
    DWORD pid; GetWindowThreadProcessId(hwnd, &pid);
    /* A host may append its own title suffix; fixture names include a random ID. */
    if ((window->owner ? pid == window->owner : !wcsncmp(title, window->title, wcslen(window->title))) &&
        (!wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") || !wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"mintty")))
        { window->hwnd = hwnd; window->count++; }
    return TRUE;
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--capture-fixture")) {
        fputs("CAPTURE-OK\n", stdout); fflush(stdout); fclose(stdout);
        Sleep(80); return 0;
    }
    if (argc != 2 || wcscmp(argv[1], L"--matrix"))
        return hardcore_application_main(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_HIDE);
    char line[8192]; Tab *tb = NULL; PROCESS_INFORMATION source = {0};
    QueryPerformanceFrequency(&g_qpf);
    GetModuleFileNameW(NULL, g_exe_path, MAX_PATH);
    palette_init(); config_load(); load_conpty(0);
    g_cols = 100; g_rows = 30;
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0; else arg = line + strlen(line);
        int ok = 0;
        if (!strcmp(line, "capture-check")) {
            ok = 1;
            for (int i = 0; i < 10; i++) {
                wchar_t command[4096]; DWORD length;
                _snwprintf(command, 4096, L"\"%ls\" --capture-fixture", g_exe_path);
                char *text = slurp_capture(command, &length);
                if (!text || !strstr(text, "CAPTURE-OK")) ok = 0;
                free(text);
            }
        } else if (!strcmp(line, "launch") && !source.hProcess) {
            wchar_t command[4096]; utf8_to_wide(arg, command, 4096);
            STARTUPINFOW si = {sizeof(si)};
            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            ok = CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &source);
        } else if (!strcmp(line, "wsl") && !tb) {
            DWORD pid, uid; unsigned long long birth; char distro[128];
            wchar_t wd[128], cmd[4096], args[180];
            if (sscanf(arg, "%lu %llu %lu %127s", &pid, &birth, &uid, distro) == 4 && slurp_prepare()) {
                utf8_to_wide(distro, wd, 128);
                _snwprintf(args, 180, L"--attach %lu %llu %lu", pid, birth, uid);
                if (slurp_wsl_command(cmd, wd, args, 1)) tb = slurp_test_tab(cmd);
                /* Full-screen redraws can erase the attachment banner in the same read.
                   Subsequent application-specific assertions prove the takeover. */
                ok = tb != NULL;
            }
        } else if (!strcmp(line, "native") && !tb) {
            wchar_t cmd[4096]; DWORD bytes; SlurpTarget target = {0}; unsigned long long hwnd;
            _snwprintf(cmd, 4096, L"\"%ls\" --slurp-probe %lu", g_exe_path, strtoul(arg, NULL, 10));
            char *probe = slurp_capture(cmd, &bytes);
            fprintf(stderr, "native probe: %s\n", probe ? probe : "failed to capture"); fflush(stderr);
            if (probe && sscanf(probe, "%lu %llu %llx", &target.pid, &target.start, &hwnd) == 3 && hwnd && target.start) {
                target.hwnd = (HWND)(uintptr_t)hwnd;
                tb = calloc(1, sizeof(*tb));
                if (tb && term_init(&tb->t, g_cols, g_rows)) {
                    InitializeCriticalSection(&tb->lock);
                    ok = slurp_spawn_native(tb, &target);
                } else { free(tb); tb = NULL; }
            }
            free(probe);
        } else if (!strcmp(line, "send") && tb) {
            unsigned char data[4096]; size_t n = strlen(arg); ok = !(n & 1) && n <= sizeof(data) * 2;
            for (size_t i = 0; ok && i < n; i += 2) {
                unsigned a;
                if (sscanf(arg + i, "%2x", &a) != 1) ok = 0; else data[i / 2] = (unsigned char)a;
            }
            if (ok) tab_write(tb, data, (int)n / 2);
        } else if (!strcmp(line, "wait") && tb) ok = slurp_wait_text(tb, arg);
        else if (!strcmp(line, "resize") && tb) {
            int cols, rows;
            if (sscanf(arg, "%d %d", &cols, &rows) == 2 && cols >= 10 && cols <= 300 && rows >= 3 && rows <= 100) {
                tab_drain(tb); term_resize(&tb->t, cols, rows); tab_set_size(tb, cols, rows); ok = 1;
            }
        } else if (!strcmp(line, "text") && tb) {
            tab_drain(tb); char *text = term_text(&tb->t); FILE *out = fopen(arg, "wb");
            if (out && text) { fputs(text, out); ok = 1; }
            if (out) fclose(out);
            free(text);
        } else if (!strcmp(line, "alive") && tb) ok = WaitForSingleObject(tb->proc, 0) == WAIT_TIMEOUT;
        else if (!strcmp(line, "commit") && tb && tb->slurp_hwnd) {
            tab_drain(tb);
            if (tb->slurp_ready) { slurp_packet(tb, SLURP_COMMIT, NULL, 0); tb->slurp_committed = 1; ok = 1; }
        } else if (!strcmp(line, "detach") && tb && tb->slurp_hwnd) { slurp_detach(tb); ok = 1; }
        else if ((!strcmp(line, "close-source") || !strcmp(line, "source-visible") || !strcmp(line, "show-source")) && !strncmp(arg, "hc-slurp-", 9)) {
            wchar_t title[256]; utf8_to_wide(arg, title, 256);
            MatrixWindow window = {title, NULL, 0}; EnumWindows(matrix_find, (LPARAM)&window);
            if (!strcmp(line, "show-source")) {
                if (!window.count && source.hProcess) {
                    window.owner = source.dwProcessId; EnumWindows(matrix_find, (LPARAM)&window);
                }
                if (window.count == 1) {
                    SetWindowTextW(window.hwnd, title);
                    ShowWindow(window.hwnd, SW_SHOWNOACTIVATE); ok = 1;
                }
            } else if (!strcmp(line, "source-visible")) ok = window.count == 1 && IsWindowVisible(window.hwnd);
            else if (window.count <= 1) {
                if (window.hwnd) PostMessageW(window.hwnd, WM_CLOSE, 0, 0);
                DWORD start = GetTickCount();
                while (window.hwnd && IsWindow(window.hwnd) && GetTickCount() - start < 3000) Sleep(20);
                ok = !window.hwnd || !IsWindow(window.hwnd);
            }
        } else if (!strcmp(line, "sleep")) {
            DWORD start = GetTickCount(), ms = (DWORD)atoi(arg);
            if (ms <= 10000) { do { if (tb) tab_drain(tb); Sleep(20); } while (GetTickCount() - start < ms); ok = 1; }
        } else if (!strcmp(line, "quit")) { puts("OK"); fflush(stdout); break; }
        puts(ok ? "OK" : "FAIL"); fflush(stdout);
    }
    if (tb) tab_teardown(tb);
    if (source.hProcess) {
        if (WaitForSingleObject(source.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(source.hProcess, 1);
        CloseHandle(source.hProcess); CloseHandle(source.hThread);
    }
    return 0;
}
