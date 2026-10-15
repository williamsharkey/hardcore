/* Windows orchestration and classic-console attachment. Included by hardcore.c. */
#include "build/slurp-version.h"
__asm__(".section .rdata,\"dr\"\n.balign 16\n.global slurp_linux\nslurp_linux:\n"
        ".incbin \"build/slurp-linux\"\n.global slurp_linux_end\nslurp_linux_end:\n.byte 0\n.text\n");
extern const unsigned char slurp_linux[], slurp_linux_end[];
__asm__(".section .rdata,\"dr\"\n.global slurp_license\nslurp_license:\n"
        ".incbin \"slurp/reptyr/COPYING\"\n.byte 0\n.text\n");
extern const char slurp_license[];
static int extract(const wchar_t *, const unsigned char *, size_t);
static void cli(const char *, ...);
static void load_conpty(int);

#define WM_APP_SLURP (WM_APP + 10)
#define SLURP_MAX_CELLS (4u * 1024u * 1024u)
enum { SLURP_TEXT = 1, SLURP_KEY, SLURP_COMMIT };
typedef struct { DWORD type, size; } SlurpInput;
typedef struct {
    DWORD magic, bytes;
    int cols, rows, first, top, view, cx, cy;
    CONSOLE_CURSOR_INFO cursor;
    CONSOLE_SELECTION_INFO selection;
    COLORREF colors[16];
    wchar_t title[256];
    /* CHAR_INFO cells follow, rows * cols of them */
} SlurpFrame;
typedef struct {
    DWORD pid, uid;
    unsigned long long start;
    HWND hwnd;
    int kind;
    wchar_t distro[128];
    char cwd[1024], label[256], note[512];
} SlurpTarget;
static LONG g_slurp_running;
static int g_slurp_report;
static wchar_t g_slurp_linux_path[MAX_PATH];
static int g_slurp_mode; /* 0 all, 1 Windows PID, 2 WSL PID */
static DWORD g_slurp_pid;
static wchar_t g_slurp_distro[128];

static int slurp_filter(const char *request) {
    DWORD pid; int used = 0;
    if (g_slurp_running) return 0;
    if (!strcmp(request, "slurp")) { g_slurp_mode = 0; g_slurp_pid = 0; g_slurp_distro[0] = 0; return 1; }
    if (sscanf(request, "slurp-win %lu%n", &pid, &used) == 1 && used && !request[used] && pid) {
        g_slurp_mode = 1; g_slurp_pid = pid; return 1;
    }
    if (sscanf(request, "slurp-wsl %lu %n", &pid, &used) == 1 && used && pid > 1 && request[used] &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request + used, -1, g_slurp_distro, 128) &&
        !wcspbrk(g_slurp_distro, L"\" \t\r\n")) {
        g_slurp_mode = 2; g_slurp_pid = pid; return 1;
    }
    return 0;
}

static int slurp_io(HANDLE h, void *p, DWORD n, int writing) {
    while (n) {
        DWORD done = 0;
        BOOL ok = writing ? WriteFile(h, p, n, &done, NULL) : ReadFile(h, p, n, &done, NULL);
        if (!ok || !done) return 0;
        p = (char *)p + done; n -= done;
    }
    return 1;
}

static void slurp_packet(Tab *tb, DWORD type, const void *data, DWORD n) {
    SlurpInput h = {type, n};
    if (!tb->pin_w) return;
    if (slurp_io(tb->pin_w, &h, sizeof(h), 1) && n) slurp_io(tb->pin_w, (void *)data, n, 1);
}
static void slurp_write(Tab *tb, const void *s, int n) {
    /* One UTF-8 string per packet; cap each packet at a complete character. */
    const unsigned char *p = s;
    while (n > 0) {
        int k = n > 4000 ? 4000 : n;
        if (k < n) while (k > 0 && (p[k] & 0xc0) == 0x80) k--;
        slurp_packet(tb, SLURP_TEXT, p, (DWORD)k);
        p += k; n -= k;
    }
}
static int slurp_key(Tab *tb, WPARAM vk) {
    INPUT_RECORD r = {0};
    if (!((vk >= VK_F1 && vk <= VK_F24) || (vk >= VK_PRIOR && vk <= VK_DOWN) ||
          vk == VK_INSERT || vk == VK_DELETE || vk == VK_BACK || vk == VK_TAB ||
          vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_PAUSE)) return 0;
    r.EventType = KEY_EVENT;
    r.Event.KeyEvent.bKeyDown = TRUE;
    r.Event.KeyEvent.wRepeatCount = 1;
    r.Event.KeyEvent.wVirtualKeyCode = (WORD)vk;
    r.Event.KeyEvent.wVirtualScanCode = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (GetKeyState(VK_SHIFT) & 0x8000) r.Event.KeyEvent.dwControlKeyState |= SHIFT_PRESSED;
    if (GetKeyState(VK_CONTROL) & 0x8000) r.Event.KeyEvent.dwControlKeyState |= LEFT_CTRL_PRESSED;
    if (GetKeyState(VK_MENU) & 0x8000) r.Event.KeyEvent.dwControlKeyState |= LEFT_ALT_PRESSED;
    r.Event.KeyEvent.uChar.UnicodeChar = vk == VK_BACK ? '\b' : vk == VK_TAB ? '\t' : vk == VK_RETURN ? '\r' : vk == VK_ESCAPE ? 27 : 0;
    slurp_packet(tb, SLURP_KEY, &r, sizeof(r));
    tb->t.view = 0; g_sel_on = 0;
    return 1;
}

static unsigned long long slurp_birth(HANDLE p) {
    FILETIME a, b, c, d;
    if (!GetProcessTimes(p, &a, &b, &c, &d)) return 0;
    return ((unsigned long long)a.dwHighDateTime << 32) | a.dwLowDateTime;
}
static void slurp_mutex_name(HWND w, wchar_t *name) {
    _snwprintf(name, 80, L"Local\\hardcore-slurp-%llx", (unsigned long long)(uintptr_t)w);
}

/* A process cannot attach to two consoles, hence a short-lived probe/helper.
   Never changes the source console during discovery. */
static HWND slurp_console(DWORD pid, unsigned long long start) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    HWND w = NULL;
    wchar_t cls[80];
    if (!p || (start && slurp_birth(p) != start)) goto done;
    FreeConsole();
    if (!AttachConsole(pid)) goto done;
    w = GetConsoleWindow();
    GetClassNameW(w, cls, 80);
    if (wcscmp(cls, L"ConsoleWindowClass") || !IsWindowVisible(w)) w = NULL;
done:
    if (p) CloseHandle(p);
    return w;
}

static int slurp_probe(DWORD pid) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    unsigned long long birth = p ? slurp_birth(p) : 0;
    HWND w;
    char result[160];
    DWORD n;
    if (p) CloseHandle(p);
    w = slurp_console(pid, birth);
    if (w) {
        wchar_t name[80];
        slurp_mutex_name(w, name);
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);
        if (mutex) { CloseHandle(mutex); w = NULL; }
    }
    snprintf(result, sizeof(result), "%lu %llu %llx\n", pid, birth, (unsigned long long)(uintptr_t)w);
    WriteFile(out, result, (DWORD)strlen(result), &n, NULL);
    FreeConsole();
    return 0;
}

static HWND g_slurp_source;
static volatile LONG g_slurp_hidden;
static SRWLOCK g_slurp_visibility = SRWLOCK_INIT;
static DWORD WINAPI slurp_guard(LPVOID owner) {
    WaitForSingleObject(owner, INFINITE);
    AcquireSRWLockExclusive(&g_slurp_visibility);
    if (InterlockedCompareExchange(&g_slurp_hidden, 0, 0)) ShowWindowAsync(g_slurp_source, SW_SHOWNOACTIVATE);
    /* This helper owns no child processes. Detaching does not close their console. */
    FreeConsole();
    ExitProcess(0);
    return 0;
}

static void slurp_console_key(HANDLE input, INPUT_RECORD *r) {
    DWORD n;
    if (r->EventType != KEY_EVENT) return;
    WriteConsoleInputW(input, r, 1, &n);
    r->Event.KeyEvent.bKeyDown = FALSE;
    WriteConsoleInputW(input, r, 1, &n);
}

static int slurp_bridge(DWORD pid, unsigned long long birth, HWND expected, HANDLE input, HANDLE output, HANDLE owner) {
    HANDLE ci = INVALID_HANDLE_VALUE, co = INVALID_HANDLE_VALUE, mutex = NULL, guard;
    SlurpFrame *previous = NULL;
    size_t previous_n = 0;
    wchar_t name[80];
    int status = 1;
    HWND w = slurp_console(pid, birth);
    if (!w || w != expected) goto done;
    slurp_mutex_name(w, name);
    mutex = CreateMutexW(NULL, FALSE, name);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) goto done;
    ci = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (ci == INVALID_HANDLE_VALUE) goto done;
    SetConsoleCtrlHandler(NULL, TRUE);
    g_slurp_source = w;
    guard = CreateThread(NULL, 0, slurp_guard, owner, 0, NULL);
    if (!guard) goto done;
    CloseHandle(guard);
    for (;;) {
        CONSOLE_SCREEN_BUFFER_INFOEX info = {sizeof(info)};
        DWORD avail;
        DWORD clients[2];
        if (GetConsoleProcessList(clients, 2) <= 1) break;
        if (!PeekNamedPipe(input, NULL, 0, NULL, &avail, NULL) || !IsWindow(w)) break;
        while (avail >= sizeof(SlurpInput)) {
            SlurpInput h;
            unsigned char data[4096];
            if (!slurp_io(input, &h, sizeof(h), 0) || h.size > sizeof(data) ||
                (h.size && !slurp_io(input, data, h.size, 0))) goto done;
            if (h.type == SLURP_COMMIT && !g_slurp_hidden) {
                CONSOLE_SELECTION_INFO selection = {0};
                GetConsoleSelectionInfo(&selection);
                /* Escape is consumed by conhost's selection mode; never type it at a shell. */
                if (selection.dwFlags & CONSOLE_SELECTION_IN_PROGRESS) {
                    DWORD_PTR ignored;
                    SendMessageTimeoutW(w, WM_KEYDOWN, VK_ESCAPE, 0, SMTO_ABORTIFHUNG, 200, &ignored);
                    SendMessageTimeoutW(w, WM_KEYUP, VK_ESCAPE, 0, SMTO_ABORTIFHUNG, 200, &ignored);
                }
                AcquireSRWLockExclusive(&g_slurp_visibility);
                if (WaitForSingleObject(owner, 0) == WAIT_OBJECT_0) {
                    ReleaseSRWLockExclusive(&g_slurp_visibility); goto done;
                }
                InterlockedExchange(&g_slurp_hidden, 1);
                ShowWindowAsync(w, SW_HIDE);
                ReleaseSRWLockExclusive(&g_slurp_visibility);
            } else if (h.type == SLURP_KEY && h.size == sizeof(INPUT_RECORD)) {
                INPUT_RECORD r; memcpy(&r, data, sizeof(r)); slurp_console_key(ci, &r);
            } else if (h.type == SLURP_TEXT && h.size) {
                wchar_t text[4096];
                int count = MultiByteToWideChar(CP_UTF8, 0, (char *)data, (int)h.size, text, 4096);
                for (int i = 0; i < count; i++) {
                    INPUT_RECORD r = {0};
                    if (text[i] == 3) { GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0); continue; }
                    r.EventType = KEY_EVENT; r.Event.KeyEvent.bKeyDown = TRUE;
                    r.Event.KeyEvent.wRepeatCount = 1; r.Event.KeyEvent.uChar.UnicodeChar = text[i];
                    SHORT key = VkKeyScanW(text[i]);
                    if (key != -1) {
                        r.Event.KeyEvent.wVirtualKeyCode = LOBYTE(key);
                        r.Event.KeyEvent.wVirtualScanCode = (WORD)MapVirtualKeyW(LOBYTE(key), MAPVK_VK_TO_VSC);
                        if (HIBYTE(key) & 1) r.Event.KeyEvent.dwControlKeyState |= SHIFT_PRESSED;
                        if (HIBYTE(key) & 2) r.Event.KeyEvent.dwControlKeyState |= LEFT_CTRL_PRESSED;
                        if (HIBYTE(key) & 4) r.Event.KeyEvent.dwControlKeyState |= LEFT_ALT_PRESSED;
                    }
                    slurp_console_key(ci, &r);
                }
            }
            if (!PeekNamedPipe(input, NULL, 0, NULL, &avail, NULL)) goto done;
        }
        /* Reopen CONOUT$ to follow SetConsoleActiveScreenBuffer (full-screen apps). */
        co = CreateFileW(L"CONOUT$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (co == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfoEx(co, &info)) break;
        int width = info.dwSize.X, height = info.srWindow.Bottom - info.srWindow.Top + 1;
        int top = info.dwCursorPosition.Y - height + 1;
        if (top < info.srWindow.Top) top = info.srWindow.Top;
        int last = top + height, first = last > HIST_LINES ? last - HIST_LINES : 0;
        if (width < 1 || width > 1000 || height < 1 || height > 500 || last > info.dwSize.Y) break;
        if ((unsigned)(last - first) * (unsigned)width > SLURP_MAX_CELLS)
            first = last - (int)(SLURP_MAX_CELLS / (unsigned)width);
        size_t bytes = sizeof(SlurpFrame) + (size_t)(last - first) * width * sizeof(CHAR_INFO);
        SlurpFrame *f = calloc(1, bytes);
        if (!f) break;
        f->magic = 0x48435331; f->bytes = (DWORD)bytes;
        f->cols = width; f->rows = last - first; f->first = first; f->top = top - first;
        f->view = top - info.srWindow.Top; f->cx = info.dwCursorPosition.X; f->cy = info.dwCursorPosition.Y - top;
        GetConsoleCursorInfo(co, &f->cursor); GetConsoleSelectionInfo(&f->selection);
        memcpy(f->colors, info.ColorTable, sizeof(f->colors)); GetConsoleTitleW(f->title, 256);
        CHAR_INFO *cells = (CHAR_INFO *)(f + 1);
        int ok = 1;
        /* Small reads avoid the console API's per-call transfer limit. */
        for (int y = first; y < last;) {
            int count = 4096 / width;
            if (count > last - y) count = last - y;
            SMALL_RECT rect = {0, (SHORT)y, (SHORT)(width - 1), (SHORT)(y + count - 1)};
            COORD sz = {(SHORT)width, (SHORT)count}, origin = {0, 0};
            if (!ReadConsoleOutputW(co, cells + (size_t)(y - first) * width, sz, origin, &rect) ||
                rect.Right != width - 1 || rect.Bottom != y + count - 1) { ok = 0; break; }
            y += count;
        }
        CloseHandle(co); co = INVALID_HANDLE_VALUE;
        if (!ok) { free(f); break; }
        if (bytes != previous_n || memcmp(f, previous, bytes)) {
            if (!slurp_io(output, f, (DWORD)bytes, 1)) { free(f); break; }
            free(previous); previous = f; previous_n = bytes;
        } else free(f);
        Sleep(100);
    }
    status = 0;
done:
    if (g_slurp_hidden) ShowWindowAsync(w, SW_SHOWNOACTIVATE);
    if (co != INVALID_HANDLE_VALUE) CloseHandle(co);
    if (ci != INVALID_HANDLE_VALUE) CloseHandle(ci);
    free(previous);
    FreeConsole();
    if (mutex) CloseHandle(mutex);
    return status;
}

static int slurp_frame_valid(const SlurpFrame *f) {
    if (f->magic != 0x48435331 || f->cols < 1 || f->cols > 1000 || f->rows < 1 ||
        f->rows > HIST_LINES || f->top < 0 || f->top >= f->rows || f->rows - f->top > 500 ||
        (unsigned)f->cols * (unsigned)f->rows > SLURP_MAX_CELLS) return 0;
    return f->bytes == sizeof(*f) + (size_t)f->cols * f->rows * sizeof(CHAR_INFO);
}
static DWORD WINAPI slurp_reader(LPVOID arg) {
    Tab *tb = arg;
    SlurpFrame h;
    while (slurp_io(tb->pout_r, &h, sizeof(h), 0) && slurp_frame_valid(&h)) {
        char *frame = malloc(h.bytes);
        if (!frame) break;
        memcpy(frame, &h, sizeof(h));
        if (!slurp_io(tb->pout_r, frame + sizeof(h), h.bytes - sizeof(h), 0)) { free(frame); break; }
        EnterCriticalSection(&tb->lock);
        free(tb->in); tb->in = frame; tb->in_n = (int)h.bytes;
        LeaveCriticalSection(&tb->lock);
        if (g_hwnd && !InterlockedExchange(&tb->posted, 1)) PostMessageW(g_hwnd, WM_APP_OUTPUT, tb->id, 0);
    }
    return 0;
}

static int slurp_drain(Tab *tb) {
    SlurpFrame *f;
    EnterCriticalSection(&tb->lock);
    f = (SlurpFrame *)tb->in; tb->in = NULL; tb->in_n = 0; InterlockedExchange(&tb->posted, 0);
    LeaveCriticalSection(&tb->lock);
    if (!f) return 0;
    Term next;
    if (!term_init(&next, f->cols, f->rows - f->top)) { free(f); return 0; }
    Cell *row = calloc((size_t)f->cols, sizeof(Cell));
    if (!row) { term_free(&next); free(f); return 0; }
    CHAR_INFO *source = (CHAR_INFO *)(f + 1);
    for (int y = 0; y < f->rows; y++) {
        memset(row, 0, (size_t)f->cols * sizeof(Cell));
        for (int x = 0; x < f->cols; x++) {
            CHAR_INFO c = source[(size_t)y * f->cols + x];
            COLORREF fg = f->colors[c.Attributes & 15], bg = f->colors[(c.Attributes >> 4) & 15];
            row[x].ch = c.Char.UnicodeChar ? c.Char.UnicodeChar : ' ';
            row[x].fg = C_RGB(GetRValue(fg), GetGValue(fg), GetBValue(fg));
            row[x].bg = C_RGB(GetRValue(bg), GetGValue(bg), GetBValue(bg));
            if (c.Attributes & COMMON_LVB_REVERSE_VIDEO) row[x].attr |= A_INV;
            if (c.Attributes & COMMON_LVB_UNDERSCORE) row[x].attr |= A_UNDER;
            if (c.Attributes & COMMON_LVB_LEADING_BYTE) row[x].wflag = W_WIDE;
            if (c.Attributes & COMMON_LVB_TRAILING_BYTE) row[x].wflag = W_CONT;
            if (x && row[x].ch >= 0xdc00 && row[x].ch <= 0xdfff && row[x - 1].ch >= 0xd800 && row[x - 1].ch <= 0xdbff) {
                row[x - 1].ch = 0x10000 + ((row[x - 1].ch - 0xd800) << 10) + row[x].ch - 0xdc00;
                row[x - 1].wflag = W_WIDE; row[x].wflag = W_CONT; row[x].ch = 0;
            }
        }
        if (y < f->top) hist_push(&next, row, f->cols, 0);
        else { memcpy(next.row[0][y - f->top], row, (size_t)f->cols * sizeof(Cell)); next.rlen[0][y - f->top] = (uint16_t)f->cols; }
    }
    free(row);
    next.cx = f->cx < 0 ? 0 : f->cx >= f->cols ? f->cols - 1 : f->cx;
    next.cy = f->cy < 0 ? 0 : f->cy >= next.rows ? next.rows - 1 : f->cy;
    next.m_cursor_vis = f->cursor.bVisible && f->cy >= 0;
    next.cursor_shape = f->cursor.dwSize < 30 ? 1 : 0;
    memcpy(next.title, f->title, sizeof(next.title)); next.title[255] = 0;
    next.hpushed += f->first;
    next.view = tb->slurp_ready ? 0 : f->view;
    term_resize(&next, g_cols, g_rows);
    if (tb->slurp_ready && tb->t.view) {
        long long anchored = next.hpushed - tb->t.hpushed + tb->t.view;
        next.view = anchored < 0 ? 0 : anchored > next.hcount ? next.hcount : (int)anchored;
    }
    if (next.view > next.hcount) next.view = next.hcount;
    if (!tb->slurp_ready && f->cols == g_cols && (f->selection.dwFlags & CONSOLE_SELECTION_NOT_EMPTY)) {
        tb->slurp_sel_on = 1;
        tb->slurp_sel_top = f->selection.srSelection.Top;
        tb->slurp_sel_bottom = f->selection.srSelection.Bottom;
        tb->slurp_sel_left = f->selection.srSelection.Left; tb->slurp_sel_right = f->selection.srSelection.Right;
    }
    term_free(&tb->t); tb->t = next; tb->t.dirty = 1; tb->slurp_ready = 1;
    if (tb == active_tab()) slurp_selection(tb);
    free(f);
    return 1;
}
static void slurp_selection(Tab *tb) {
    if (!tb || !tb->slurp_sel_on) return;
    g_sel_on = 1;
    g_sel_a_row = tb->slurp_sel_top; g_sel_b_row = tb->slurp_sel_bottom;
    g_sel_a_col = tb->slurp_sel_left; g_sel_b_col = tb->slurp_sel_right;
    tb->slurp_sel_on = 0;
}
static void slurp_commit_all(void) {
    for (int i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (tb->slurp_hwnd && tb->slurp_ready && !tb->slurp_committed) {
            slurp_packet(tb, SLURP_COMMIT, NULL, 0); tb->slurp_committed = 1;
        }
    }
}
static void slurp_detach(Tab *tb) {
    if (tb->slurp_committed) ShowWindowAsync(tb->slurp_hwnd, SW_SHOWNOACTIVATE);
    if (tb->pin_w) { CloseHandle(tb->pin_w); tb->pin_w = NULL; }
}

/* Pipe inheritance is explicitly limited: a helper must not keep other tabs alive. */
static int slurp_child(wchar_t *cmd, HANDLE input, HANDLE output, HANDLE extra, PROCESS_INFORMATION *pi) {
    STARTUPINFOEXW si = {0}; SIZE_T size = 0;
    HANDLE handles[3] = {input, output, extra};
    int ok = 0;
    si.StartupInfo.cb = sizeof(si); si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = input; si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = output;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, size);
    if (!si.lpAttributeList) return 0;
    if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)) {
        if (UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles, (extra ? 3 : 2) * sizeof(HANDLE), NULL, NULL))
            ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                                NULL, NULL, &si.StartupInfo, pi);
        DeleteProcThreadAttributeList(si.lpAttributeList);
    }
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    return ok;
}

static int slurp_spawn_native_for(Tab *tb, const SlurpTarget *t, HANDLE owner_process) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE input = NULL, output = NULL, owner = NULL;
    PROCESS_INFORMATION pi;
    wchar_t cmd[1024];
    int ok = 0;
    if (!CreatePipe(&input, &tb->pin_w, &sa, 65536) || !CreatePipe(&tb->pout_r, &output, &sa, 65536)) goto done;
    SetHandleInformation(tb->pin_w, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(tb->pout_r, HANDLE_FLAG_INHERIT, 0);
    if (!DuplicateHandle(GetCurrentProcess(), owner_process, GetCurrentProcess(), &owner, SYNCHRONIZE, TRUE, 0)) goto done;
    _snwprintf(cmd, 1024, L"\"%ls\" --slurp-bridge %lu %llu %llu %llu %llu %llu", g_exe_path, t->pid, t->start,
               (unsigned long long)(uintptr_t)t->hwnd,
               (unsigned long long)(uintptr_t)input, (unsigned long long)(uintptr_t)output, (unsigned long long)(uintptr_t)owner);
    if (!slurp_child(cmd, input, output, owner, &pi)) goto done;
    tb->slurp_hwnd = t->hwnd; tb->proc = pi.hProcess; CloseHandle(pi.hThread);
    tb->reader = CreateThread(NULL, 0, slurp_reader, tb, 0, NULL);
    if (!tb->reader) goto done;
    RegisterWaitForSingleObject(&tb->wait, tb->proc, on_child_exit, tb, INFINITE, WT_EXECUTEONLYONCE);
    ok = 1;
done:
    if (input) CloseHandle(input);
    if (output) CloseHandle(output);
    if (owner) CloseHandle(owner);
    return ok;
}
static int slurp_spawn_native(Tab *tb, const SlurpTarget *t) {
    return slurp_spawn_native_for(tb, t, GetCurrentProcess());
}

/* Read-only subprocesses run on the discovery thread with a bounded timeout. */
static char *slurp_capture(wchar_t *cmd, DWORD *length) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE r = NULL, w = NULL, in = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi = {0};
    char *data = calloc(1, 131074);
    DWORD n = 0, code = 1, start = GetTickCount();
    int eof = 0;
    if (!data || !CreatePipe(&r, &w, &sa, 65536)) goto done;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE || !slurp_child(cmd, in, w, NULL, &pi)) goto done;
    CloseHandle(w); w = NULL;
    for (;;) {
        DWORD avail = 0, got;
        if (!PeekNamedPipe(r, NULL, 0, NULL, &avail, NULL)) {
            eof = GetLastError() == ERROR_BROKEN_PIPE; break;
        }
        if (avail) {
            if (n + avail > 131072) break;
            if (!ReadFile(r, data + n, avail, &got, NULL)) break;
            n += got;
        } else if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &code);
            if (PeekNamedPipe(r, NULL, 0, NULL, &avail, NULL) && avail) continue;
            break;
        }
        if (GetTickCount() - start > 15000) break;
    }
    /* A probe can close stdout before its CRT/process teardown finishes.
       EOF is not a failed probe: allow exit within the original time budget. */
    DWORD elapsed = GetTickCount() - start;
    if (WaitForSingleObject(pi.hProcess, eof && elapsed < 15000 ? 15000 - elapsed : 0) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);
    else GetExitCodeProcess(pi.hProcess, &code);
done:
    if (r) CloseHandle(r);
    if (w) CloseHandle(w);
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (code || !data) { free(data); return NULL; }
    data[n] = data[n + 1] = 0; *length = n;
    return data;
}

static int slurp_prepare(void) {
    wchar_t old[4096] = L"", env[4200];
    _snwprintf(g_slurp_linux_path, MAX_PATH, L"%ls\\slurp-" SLURP_VERSION L".linux", g_cfg_dir);
    g_slurp_linux_path[MAX_PATH - 1] = 0;
    /* The hash names the source-built payload; verify cached contents too. */
    FILE *f = _wfopen(g_slurp_linux_path, L"rb");
    size_t length = (size_t)(slurp_linux_end - slurp_linux);
    int same = 0;
    if (f) {
        unsigned char *buf = malloc(length + 1);
        if (buf) { same = fread(buf, 1, length + 1, f) == length && !memcmp(buf, slurp_linux, length); free(buf); }
        fclose(f);
    }
    if (!same) {
        DeleteFileW(g_slurp_linux_path);
        if (!extract(g_slurp_linux_path, slurp_linux, length)) return 0;
    }
    GetEnvironmentVariableW(L"WSLENV", old, 4096);
    if (!wcsstr(old, L"HARDCORE_SLURP/p")) {
        if (wcslen(old) > 4000) return 0;
        _snwprintf(env, 4200, L"%ls%lsHARDCORE_SLURP/p", old, old[0] ? L":" : L"");
        SetEnvironmentVariableW(L"WSLENV", env);
    }
    SetEnvironmentVariableW(L"HARDCORE_SLURP", g_slurp_linux_path);
    return 1;
}

static int slurp_wsl_command(wchar_t *out, const wchar_t *distro, const wchar_t *args, int root) {
    if (!*distro || wcspbrk(distro, L"\" \t\r\n") || wcslen(distro) >= 128) return 0;
    /* WSL parses -d itself and does not strip surrounding quotes (#9792). */
    _snwprintf(out, 4096, L"wsl.exe -d %ls %ls --exec /bin/sh -c \"exec \\\"$HARDCORE_SLURP\\\" %ls\"",
               distro, root ? L"-u root" : L"", args);
    out[4095] = 0;
    return 1;
}

static void slurp_deliver(const SlurpTarget *t, int listing) {
    if (listing) {
        if (t->note[0]) cli("%s\n", t->note);
        else if (t->hwnd) cli("Windows PID %lu: %s (live console attachment)\n", t->pid, t->label);
        else cli("WSL %ls PID %lu: %s\n", t->distro, t->pid, t->cwd);
    } else {
        SlurpTarget *copy = malloc(sizeof(*copy));
        if (copy) { *copy = *t; if (!PostMessageW(g_hwnd, WM_APP_SLURP, 0, (LPARAM)copy)) free(copy); }
    }
}
static void slurp_note(const char *message, int listing) {
    SlurpTarget t = {0}; snprintf(t.note, sizeof(t.note), "%s", message); slurp_deliver(&t, listing);
}
static int slurp_unhex(char *out, size_t cap, const char *text) {
    size_t n = strlen(text);
    if (!strcmp(text, "-")) { out[0] = 0; return 1; }
    if ((n & 1) || n / 2 >= cap) return 0;
    for (size_t i = 0; i < n; i += 2) {
        unsigned a, b;
        a = text[i] >= '0' && text[i] <= '9' ? text[i] - '0' : text[i] >= 'a' && text[i] <= 'f' ? text[i] - 'a' + 10 : 99;
        b = text[i + 1] >= '0' && text[i + 1] <= '9' ? text[i + 1] - '0' : text[i + 1] >= 'a' && text[i + 1] <= 'f' ? text[i + 1] - 'a' + 10 : 99;
        if (a > 15 || b > 15 || !(a | b)) return 0;
        out[i / 2] = (char)(a * 16 + b);
    }
    out[n / 2] = 0;
    return 1;
}

static DWORD WINAPI slurp_scan(LPVOID arg) {
    int listing = (int)(intptr_t)arg, found = 0, unsupported = 0;
    DWORD bytes;
    wchar_t cmd[4096] = L"wsl.exe --list --running --quiet";
    char *distros = g_slurp_mode == 1 ? NULL : slurp_capture(cmd, &bytes);
    if (distros) {
        /* wsl.exe's redirected distro list is UTF-16LE. */
        wchar_t *p = (wchar_t *)distros;
        while (*p) {
            while (*p == '\r' || *p == '\n' || *p == 0xfeff) p++;
            wchar_t *end = p;
            while (*end && *end != '\r' && *end != '\n') end++;
            if (!*p) break;
            wchar_t saved = *end; *end = 0;
            if ((g_slurp_mode != 2 || !wcscmp(p, g_slurp_distro)) && slurp_wsl_command(cmd, p, L"--list", 0)) {
                char *list = slurp_capture(cmd, &bytes);
                if (list && !strncmp(list, "HARDCORE-SLURP 1\n", 17)) {
                    char *line = strchr(list, '\n') + 1;
                    while (*line) {
                        char *nl = strchr(line, '\n');
                        SlurpTarget t = {0};
                        char cwd[2049], comm[513];
                        if (nl) *nl = 0;
                        if (sscanf(line, "%lu %llu %lu %2048s %512s", &t.pid, &t.start, &t.uid, cwd, comm) == 5 &&
                            t.pid > 1 && t.start && slurp_unhex(t.cwd, sizeof(t.cwd), cwd) && slurp_unhex(t.label, sizeof(t.label), comm)) {
                            if (g_slurp_mode != 2 || g_slurp_pid == t.pid) {
                                wcscpy(t.distro, p); t.kind = TK_WSL; slurp_deliver(&t, listing); found++;
                            }
                        }
                        if (!nl) break;
                        line = nl + 1;
                    }
                } else slurp_note("A running WSL distro could not be inspected (helper requires x86-64 Linux and an executable Windows mount).", listing);
                free(list);
            }
            *end = saved; p = end;
        }
        free(distros);
    } else if (g_slurp_mode != 1) slurp_note("No running WSL distributions could be inspected.", listing);
    HANDLE snap = g_slurp_mode == 2 ? INVALID_HANDLE_VALUE : CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    HWND seen[256]; int nseen = 0;
    if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe)) do {
        int kind = (!_wcsicmp(pe.szExeFile, L"cmd.exe") || !_wcsicmp(pe.szExeFile, L"bash.exe")) ? TK_CMD :
                   (!_wcsicmp(pe.szExeFile, L"powershell.exe") || !_wcsicmp(pe.szExeFile, L"pwsh.exe")) ? TK_PS : -1;
        if (g_slurp_mode == 1) { if (pe.th32ProcessID != g_slurp_pid) continue; if (kind < 0) kind = TK_CMD; }
        else if (kind < 0) continue;
        _snwprintf(cmd, 4096, L"\"%ls\" --slurp-probe %lu", g_exe_path, pe.th32ProcessID);
        char *result = slurp_capture(cmd, &bytes);
        SlurpTarget t = {0}; unsigned long long hwnd = 0;
        if (result && sscanf(result, "%lu %llu %llx", &t.pid, &t.start, &hwnd) == 3 && hwnd && t.start) {
            t.hwnd = (HWND)(uintptr_t)hwnd; t.kind = kind;
            int duplicate = 0;
            for (int i = 0; i < nseen; i++) if (seen[i] == t.hwnd) duplicate = 1;
            if (!duplicate && nseen < 256) {
                seen[nseen++] = t.hwnd;
                char executable[1024];
                if (!WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, executable, sizeof(executable), NULL, NULL))
                    strcpy(executable, "console");
                snprintf(t.label, sizeof(t.label), "%.230s %lu", executable, t.pid);
                slurp_deliver(&t, listing); found++;
            }
        } else unsupported++;
        free(result);
    } while (Process32NextW(snap, &pe));
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    char summary[400];
    snprintf(summary, sizeof(summary), "%d eligible session%s. %d native shell process%s skipped (hidden, already attached, inaccessible, or hosted by Windows Terminal).",
             found, found == 1 ? "" : "s", unsupported, unsupported == 1 ? "" : "es");
    slurp_note(summary, listing);
    if (!listing) PostMessageW(g_hwnd, WM_APP_SLURP, 1, 0);
    return 0;
}

static void slurp_receive(SlurpTarget *t) {
    Tab *report = tab_by_id(g_slurp_report);
    char line[800];
    if (t->note[0]) snprintf(line, sizeof(line), "%s\r\n", t->note);
    else if (g_ntabs >= MAX_TABS) snprintf(line, sizeof(line), "Tab limit reached; PID %lu left in its original terminal.\r\n", t->pid);
    else {
        Tab *tb = calloc(1, sizeof(*tb));
        if (!tb || !term_init(&tb->t, g_cols, g_rows)) { free(tb); free(t); return; }
        tb->id = g_next_id++; tb->kind = t->kind; tb->born = GetTickCount();
        InitializeCriticalSection(&tb->lock);
        int ok;
        if (t->hwnd) {
            snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", t->label);
            ok = slurp_spawn_native(tb, t);
        } else {
            wchar_t args[180];
            _snwprintf(args, 180, L"--attach %lu %llu %lu", t->pid, t->start, t->uid);
            snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", t->cwd);
            set_cwd(&tb->t, t->cwd); tb->slurp_wsl = 1;
            ok = slurp_wsl_command(tb->slurp_command, t->distro, args, 1) && tab_spawn(tb);
        }
        if (ok) {
            g_tabs[g_ntabs++] = tb;
            snprintf(line, sizeof(line), "Attaching %s PID %lu; its tab will show any error.\r\n", t->hwnd ? "Windows" : "WSL", t->pid);
        } else {
            tab_destroy_async(tb);
            snprintf(line, sizeof(line), "Could not start attachment for PID %lu; original terminal left open.\r\n", t->pid);
        }
    }
    if (report) term_message(&report->t, line);
    free(t); InvalidateRect(g_hwnd, NULL, FALSE);
}

static int slurp_start(int list_only) {
    if (!list_only && InterlockedCompareExchange(&g_slurp_running, 1, 0)) return 0;
    if (!slurp_prepare()) { InterlockedExchange(&g_slurp_running, 0); return 0; }
    if (list_only) { slurp_scan((LPVOID)1); return 1; }
    if (g_ntabs >= MAX_TABS) { InterlockedExchange(&g_slurp_running, 0); return 0; }
    Tab *report = calloc(1, sizeof(*report));
    if (!report || !term_init(&report->t, g_cols, g_rows)) { free(report); InterlockedExchange(&g_slurp_running, 0); return 0; }
    InitializeCriticalSection(&report->lock); report->id = g_next_id++; report->help = HELP_SLURP;
    strcpy(report->start_dir, "slurp"); g_slurp_report = report->id;
    term_message(&report->t, "hardcore slurp\r\n\r\nDiscovering running WSL and classic Windows console sessions...\r\n"
        "WSL: live PTY takeover using the source-built Linux attachment code.\r\n"
        "Native: original console hidden after capture; closing its tab detaches.\r\n"
        "Windows Terminal native tabs cannot be transferred by this API.\r\n\r\n");
    g_tabs[g_ntabs++] = report; activate(g_ntabs - 1);
    HANDLE worker = CreateThread(NULL, 0, slurp_scan, NULL, 0, NULL);
    if (worker) CloseHandle(worker);
    else { term_message(&report->t, "Could not start discovery.\r\n"); InterlockedExchange(&g_slurp_running, 0); return 0; }
    return 1;
}

static int slurp_cmd(int argc, wchar_t **argv) {
    int listing = 0, mode = 0;
    DWORD pid = 0;
    wchar_t distro[128] = L"";
    char request[600] = "slurp";
    if (argc == 1 && !wcscmp(argv[0], L"--license")) {
        for (size_t i = 0, n = strlen(slurp_license); i < n; i += 500) cli("%.*s", (int)(n - i > 500 ? 500 : n - i), slurp_license + i);
        return 0;
    }
    for (int i = 0; i < argc; i++) {
        if (!wcscmp(argv[i], L"--list")) listing = 1;
        else if ((!wcscmp(argv[i], L"--windows") || !wcscmp(argv[i], L"--wsl")) && i + 1 < argc && !mode) {
            wchar_t *end;
            mode = !wcscmp(argv[i], L"--windows") ? 1 : 2;
            pid = wcstoul(argv[++i], &end, 10);
            if (!pid || *end || argv[i][0] == '-') goto usage;
        } else if (!wcscmp(argv[i], L"--distro") && i + 1 < argc && wcslen(argv[i + 1]) < 128) {
            wcscpy(distro, argv[++i]);
        } else goto usage;
    }
    if ((mode == 2 && !distro[0]) || (mode != 2 && distro[0])) goto usage;
    if (mode == 1) snprintf(request, sizeof(request), "slurp-win %lu", pid);
    if (mode == 2) {
        char dn[400];
        if (!WideCharToMultiByte(CP_UTF8, 0, distro, -1, dn, sizeof(dn), NULL, NULL)) goto usage;
        snprintf(request, sizeof(request), "slurp-wsl %lu %s", pid, dn);
    }
    if (!slurp_filter(request)) goto usage;
    if (listing) {
        cli("hardcore slurp: read-only discovery\n");
        if (!slurp_start(1)) { cli("Could not prepare attachment code.\n"); return 1; }
        return 0;
    }
    HWND w = FindWindowW(L"hardcore", NULL);
    if (w) {
        COPYDATASTRUCT cd = {0x48430002, (DWORD)strlen(request) + 1, request}; DWORD_PTR result = 0;
        if (SendMessageTimeoutW(w, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &result)) {
            if (result) { cli("hardcore slurp: discovery started; see the slurp tab for results.\n"); return 0; }
            /* An older build or full window: use the executable just invoked. */
        } else {
            cli("hardcore slurp: the existing window did not respond.\n"); return 1;
        }
    }
    wchar_t cmd[1200], wr[600]; STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi;
    utf8_to_wide(request, wr, 600);
    _snwprintf(cmd, 1200, L"\"%ls\" --slurp-window \"%ls\"", g_exe_path, wr);
    if (!CreateProcessW(g_exe_path, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        cli("hardcore slurp: could not open a window (%lu).\n", GetLastError()); return 1;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    cli("hardcore slurp: opening a window; see the slurp tab for results.\n");
    return 0;
usage:
    cli("Usage: hardcore slurp [--list] [--windows PID | --wsl PID --distro NAME]\n");
    return 2;
}

/* Isolated integration fixture: a console created by the test, never a user shell. */
static int slurp_fixture(void) {
    HANDLE reply = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n;
    FreeConsole();
    if (!AllocConsole()) return 1;
    HWND w = GetConsoleWindow();
    ShowWindow(w, SW_SHOWNOACTIVATE);
    SetConsoleTitleW(L"hardcore slurp synthetic fixture");
    HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) return 1;
    SMALL_RECT window = {0, 0, 79, 24}; COORD size = {80, 200};
    SetConsoleWindowInfo(output, TRUE, &window); SetConsoleScreenBufferSize(output, size);
    SetConsoleWindowInfo(output, TRUE, &window);
    SetConsoleTextAttribute(output, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    for (int i = 0; i < 100; i++) {
        wchar_t line[80]; _snwprintf(line, 80, L"SLURP-HISTORY-%03d\r\n", i);
        WriteConsoleW(output, line, (DWORD)wcslen(line), &n, NULL);
    }
    const wchar_t *prompt = L"fixture> ";
    WriteConsoleW(output, prompt, (DWORD)wcslen(prompt), &n, NULL);
    SlurpTarget t = {0}; t.pid = GetCurrentProcessId(); t.start = slurp_birth(GetCurrentProcess()); t.hwnd = w; t.kind = TK_CMD;
    if (!slurp_io(reply, &t, sizeof(t), 1)) return 1;
    CloseHandle(reply);
    for (;;) {
        wchar_t line[200];
        if (!ReadConsoleW(input, line, 199, &n, NULL)) break;
        line[n] = 0;
        DWORD written;
        WriteConsoleW(output, L"ECHO: ", 6, &written, NULL);
        WriteConsoleW(output, line, n, &written, NULL);
    }
    return 0;
}

static int slurp_wait_text(Tab *tb, const char *want) {
    DWORD start = GetTickCount();
    do {
        tab_drain(tb);
        char *text = term_text(&tb->t);
        int found = text && strstr(text, want);
        free(text);
        if (found) return 1;
        if (WaitForSingleObject(tb->proc, 30) == WAIT_OBJECT_0) break;
    } while (GetTickCount() - start < 8000);
    return 0;
}

static int slurp_selftest(const wchar_t *path) {
    FILE *f = _wfopen(path, L"wb");
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE r = NULL, w = NULL, input = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION fixture = {0}, owner = {0};
    wchar_t cmd[1024]; SlurpTarget target = {0}; Tab *tb = NULL;
    int failures = 0;
#define SLURP_CHECK(test, name) do { int passed = !!(test); fprintf(f, "%s %s\n", passed ? "ok" : "FAIL", name); fflush(f); failures += !passed; } while (0)
    if (!f) return 2;
    g_cols = 80; g_rows = 25;
    if (!CreatePipe(&r, &w, &sa, 65536)) goto failed;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    _snwprintf(cmd, 1024, L"\"%ls\" --slurp-fixture", g_exe_path);
    if (input == INVALID_HANDLE_VALUE || !slurp_child(cmd, input, w, NULL, &fixture)) goto failed;
    CloseHandle(w); w = NULL;
    DWORD start = GetTickCount(), avail = 0;
    while (GetTickCount() - start < 5000) {
        if (!PeekNamedPipe(r, NULL, 0, NULL, &avail, NULL) || avail >= sizeof(target)) break;
        Sleep(20);
    }
    if (avail < sizeof(target) || !slurp_io(r, &target, sizeof(target), 0)) goto failed;
    tb = calloc(1, sizeof(*tb));
    if (!tb || !term_init(&tb->t, 80, 25)) goto failed;
    InitializeCriticalSection(&tb->lock);
    if (!slurp_spawn_native(tb, &target)) goto failed;
    SLURP_CHECK(slurp_wait_text(tb, "fixture>"), "initial screen captured");
    char *text = term_text(&tb->t);
    SLURP_CHECK(text && strstr(text, "SLURP-HISTORY-000") && tb->t.hcount > 50, "scrollback captured");
    free(text);
    SLURP_CHECK(tb->t.cx == 9, "cursor column captured");
    SLURP_CHECK(tb->t.hist[0].c && tb->t.hist[0].c[0].fg != 0, "console colors captured");
    slurp_packet(tb, SLURP_COMMIT, NULL, 0); tb->slurp_committed = 1;
    start = GetTickCount();
    while (IsWindowVisible(target.hwnd) && GetTickCount() - start < 3000) Sleep(20);
    SLURP_CHECK(!IsWindowVisible(target.hwnd), "source hidden only after capture acknowledgment");
    slurp_write(tb, "LIVE-CHECK\r", 11);
    SLURP_CHECK(slurp_wait_text(tb, "ECHO: LIVE-CHECK"), "input reaches original process");
    slurp_detach(tb);
    start = GetTickCount();
    while (!IsWindowVisible(target.hwnd) && GetTickCount() - start < 3000) Sleep(20);
    SLURP_CHECK(IsWindowVisible(target.hwnd) && WaitForSingleObject(fixture.hProcess, 0) == WAIT_TIMEOUT,
                "detach restores original window and keeps its process alive");
    tab_teardown(tb); tb = NULL;
    /* Kill only a synthetic owner process to exercise crash recovery. */
    STARTUPINFOW si = {sizeof(si)};
    _snwprintf(cmd, 1024, L"\"%ls\" --slurp-owner-fixture", g_exe_path);
    if (!CreateProcessW(g_exe_path, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &owner)) goto failed;
    tb = calloc(1, sizeof(*tb));
    if (!tb || !term_init(&tb->t, 80, 25)) goto failed;
    InitializeCriticalSection(&tb->lock);
    if (!slurp_spawn_native_for(tb, &target, owner.hProcess)) goto failed;
    SLURP_CHECK(slurp_wait_text(tb, "ECHO: LIVE-CHECK"), "reattach retains source state");
    slurp_packet(tb, SLURP_COMMIT, NULL, 0); tb->slurp_committed = 1;
    start = GetTickCount();
    while (IsWindowVisible(target.hwnd) && GetTickCount() - start < 3000) Sleep(20);
    SLURP_CHECK(!IsWindowVisible(target.hwnd), "reattached window hidden");
    TerminateProcess(owner.hProcess, 0);
    WaitForSingleObject(tb->proc, 3000);
    start = GetTickCount();
    while (!IsWindowVisible(target.hwnd) && GetTickCount() - start < 3000) Sleep(20);
    SLURP_CHECK(IsWindowVisible(target.hwnd) && WaitForSingleObject(fixture.hProcess, 0) == WAIT_TIMEOUT,
                "owner crash restores source and preserves its process");
    goto done;
failed:
    fprintf(f, "FAIL could not create/attach synthetic console (%lu)\n", GetLastError()); failures++;
done:
    if (tb) tab_teardown(tb);
    if (owner.hProcess) { TerminateProcess(owner.hProcess, 0); CloseHandle(owner.hProcess); CloseHandle(owner.hThread); }
    if (fixture.hProcess) { TerminateProcess(fixture.hProcess, 0); WaitForSingleObject(fixture.hProcess, 2000); CloseHandle(fixture.hProcess); CloseHandle(fixture.hThread); }
    if (r) CloseHandle(r);
    if (w) CloseHandle(w);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    fprintf(f, "%d failed\n", failures); fclose(f);
    return failures ? 1 : 0;
#undef SLURP_CHECK
}

static Tab *slurp_test_tab(const wchar_t *command) {
    Tab *tb = calloc(1, sizeof(*tb));
    if (!tb || !term_init(&tb->t, g_cols, g_rows)) { free(tb); return NULL; }
    InitializeCriticalSection(&tb->lock);
    tb->kind = TK_WSL; wcscpy(tb->slurp_command, command);
    if (!tab_spawn(tb)) { tab_teardown(tb); return NULL; }
    return tb;
}

static int slurp_wsl_selftest(const wchar_t *path) {
    FILE *f = _wfopen(path, L"wb");
    Tab *source = NULL, *target = NULL;
    wchar_t cmd[4096], args[180];
    DWORD pid = 0, uid = 0, bytes;
    unsigned long long birth = 0;
    int failures = 0;
    if (!f) return 2;
#define SLURP_CHECK(test, name) do { int passed = !!(test); fprintf(f, "%s %s\n", passed ? "ok" : "FAIL", name); fflush(f); failures += !passed; } while (0)
    g_cols = 80; g_rows = 25;
    load_conpty(0);
    if (!wsl_distros() || !slurp_prepare()) goto failed;
    _snwprintf(cmd, 4096, L"wsl.exe -d %ls --exec /bin/sh -c \"unset HARDCORE_RC TERM_PROGRAM; exec /bin/bash --noprofile --norc -i\"", g_default_distro);
    source = slurp_test_tab(cmd);
    if (!source) goto failed;
    const char *setup = "export HC_SLURP_MEMORY=retained; printf 'FIXTURE-PID=%s\\n' \"$$\"; printf 'FIXTURE-READY\\n'\r";
    tab_write(source, setup, (int)strlen(setup));
    if (!slurp_wait_text(source, "FIXTURE-PID=")) goto failed;
    /* The echoed command contains the marker too; wait for the numeric reply. */
    DWORD start = GetTickCount();
    while (!pid && GetTickCount() - start < 8000) {
        tab_drain(source);
        char *text = term_text(&source->t), *p = text;
        while (p && (p = strstr(p, "FIXTURE-PID="))) { if (sscanf(p, "FIXTURE-PID=%lu", &pid) == 1) break; p++; }
        free(text); Sleep(20);
    }
    if (!pid || !slurp_wsl_command(cmd, g_default_distro, L"--list", 0)) goto failed;
    char *list = slurp_capture(cmd, &bytes), *p = list;
    while (p && (p = strchr(p, '\n'))) {
        DWORD candidate, owner; unsigned long long ticks;
        p++;
        if (sscanf(p, "%lu %llu %lu", &candidate, &ticks, &owner) == 3 && candidate == pid) { birth = ticks; uid = owner; break; }
    }
    if (!birth) { if (list) fprintf(f, "discovery: %s\n", list); free(list); goto failed; }
    free(list);
    _snwprintf(args, 180, L"--attach %lu %llu %lu", pid, birth, uid);
    if (!slurp_wsl_command(cmd, g_default_distro, args, 1)) goto failed;
    target = slurp_test_tab(cmd);
    if (!target) goto failed;
    int attached = slurp_wait_text(target, "attached live WSL session");
    SLURP_CHECK(attached, "actual WSL relay takeover");
    if (!attached) {
        char *text = term_text(&target->t); fprintf(f, "%s\n", text ? text : ""); free(text); goto done;
    }
    /* Closing the old ConPTY must not terminate the transferred shell/jobs. */
    tab_teardown(source); source = NULL;
    const char *check = "printf 'MEMORY-%s-PID-%s\\n' \"$HC_SLURP_MEMORY\" \"$$\"; jobs -p\r";
    tab_write(target, check, (int)strlen(check));
    char expected[100]; snprintf(expected, sizeof(expected), "MEMORY-retained-PID-%lu", pid);
    SLURP_CHECK(slurp_wait_text(target, expected), "same WSL shell and memory after original ConPTY closes");
    const char *quit = "exit\r";
    tab_write(target, quit, (int)strlen(quit));
    SLURP_CHECK(WaitForSingleObject(target->proc, 8000) == WAIT_OBJECT_0, "attached WSL shell exits cleanly");
    goto done;
failed:
    fprintf(f, "FAIL could not prepare synthetic WSL session (%lu), pid=%lu birth=%llu\n", GetLastError(), pid, birth); failures++;
    if (source) { tab_drain(source); char *text = term_text(&source->t); fprintf(f, "source: %s\n", text ? text : ""); free(text); }
done:
    if (source) tab_teardown(source);
    if (target) tab_teardown(target);
    fprintf(f, "%d failed\n", failures); fclose(f);
    return failures ? 1 : 0;
#undef SLURP_CHECK
}
