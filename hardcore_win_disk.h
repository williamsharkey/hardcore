/* WSL disk reclaim, the Windows part: find each distro's disk file, measure
   it, and give the free Linux space back to the drive with the Windows
   virtual disk API (CompactVirtualDisk). No admin. See
   design/WSL-DISK-RECLAIM.md.

   Included by hardcore.c in its _WIN32 part, before wsl_distros(). The tab
   column card, its keys and the tab restart live in hardcore.c; this file
   has the parts that also run from the command line (hardcore disk ...).

   Rules kept here (design section 8):
   - hardcore never writes inside a disk file with its own code;
   - it compacts only while it holds the file open itself, so WSL cannot
     start that distro during the run;
   - it never changes the sparse flag and never writes .wslconfig;
   - it never stops Linux without a key (or --yes on the command line);
   - it stops only the one distro (wsl --terminate), never wsl --shutdown. */

#include <virtdisk.h>
#include <winioctl.h>
#include <time.h>
#include "hardcore_disk_policy.h"

#ifndef OPEN_VIRTUAL_DISK_FLAG_SUPPORT_SPARSE_FILES_ANY_FS
#define OPEN_VIRTUAL_DISK_FLAG_SUPPORT_SPARSE_FILES_ANY_FS 0x00000400
#endif

static void cli(const char *fmt, ...);   /* hardcore.c; g_cfg_dir too */

#define DISK_GIB (1024.0 * 1024.0 * 1024.0)

/* ── the distros and their disk files (HKCU\...\Lxss, design 4.1) ── */

typedef struct {
    wchar_t name[128];
    wchar_t base[MAX_PATH];      /* BasePath, without a \\?\ prefix */
    wchar_t vhd[MAX_PATH];       /* BasePath\VhdFileName */
    DWORD version, flags;
    int is_default;
} WslDistro;

#define WSL_MAX_DISTROS 32

static int wsl_distro_list(WslDistro *out, int max, wchar_t *def_name, int def_cap) {
    HKEY k, d;
    wchar_t def[64] = L"";
    DWORD i, sz;
    int n = 0;
    if (def_name && def_cap) def_name[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &k)) return 0;
    sz = sizeof(def);
    RegGetValueW(k, NULL, L"DefaultDistribution", RRF_RT_REG_SZ, NULL, def, &sz);
    for (i = 0; n < max; i++) {
        wchar_t sub[64], file[MAX_PATH] = L"ext4.vhdx";
        DWORD subn = 64, v = 2, fl = 0;
        WslDistro *w = &out[n];
        if (RegEnumKeyExW(k, i, sub, &subn, NULL, NULL, NULL, NULL)) break;
        if (RegOpenKeyExW(k, sub, 0, KEY_READ, &d)) continue;
        memset(w, 0, sizeof(*w));
        sz = sizeof(w->name);
        if (!RegGetValueW(d, NULL, L"DistributionName", RRF_RT_REG_SZ, NULL, w->name, &sz)) {
            sz = sizeof(w->base);
            RegGetValueW(d, NULL, L"BasePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, w->base, &sz);
            if (!wcsncmp(w->base, L"\\\\?\\", 4)) memmove(w->base, w->base + 4, (wcslen(w->base + 4) + 1) * sizeof(wchar_t));
            sz = sizeof(file);
            if (RegGetValueW(d, NULL, L"VhdFileName", RRF_RT_REG_SZ, NULL, file, &sz) || !file[0]) wcscpy(file, L"ext4.vhdx");
            sz = sizeof(v);
            RegGetValueW(d, NULL, L"Version", RRF_RT_REG_DWORD, NULL, &v, &sz);
            sz = sizeof(fl);
            RegGetValueW(d, NULL, L"Flags", RRF_RT_REG_DWORD, NULL, &fl, &sz);
            w->version = v;
            w->flags = fl;
            _snwprintf(w->vhd, MAX_PATH, L"%ls\\%ls", w->base, file);
            w->vhd[MAX_PATH - 1] = 0;
            w->is_default = !_wcsicmp(sub, def);
            if (def_name && (w->is_default || !def_name[0])) _snwprintf(def_name, def_cap, L"%ls", w->name);
            n++;
        }
        RegCloseKey(d);
    }
    RegCloseKey(k);
    return n;
}

/* WSL 2: Version 2, or the VM-mode flag (LXSS_DISTRO_FLAGS_VM_MODE) */
static int wsl_distro_is_wsl2(const WslDistro *w) { return w->version >= 2 || (w->flags & 0x8); }

/* name NULL or "": the default distro */
static int wsl_distro_find(const wchar_t *name, WslDistro *out) {
    WslDistro all[WSL_MAX_DISTROS];
    wchar_t def[128];
    int n = wsl_distro_list(all, WSL_MAX_DISTROS, def, 128), i;
    if (!name || !name[0]) name = def;
    for (i = 0; i < n; i++)
        if (!_wcsicmp(all[i].name, name)) { *out = all[i]; return 1; }
    return 0;
}

/* ── sizes (design 4.2) ── */

static double disk_free_gb(const wchar_t *path) {
    wchar_t root[MAX_PATH];
    ULARGE_INTEGER avail;
    if (!GetVolumePathNameW(path, root, MAX_PATH)) return -1;
    if (!GetDiskFreeSpaceExW(root, &avail, NULL, NULL)) return -1;
    return (double)avail.QuadPart / DISK_GIB;
}

static void disk_drive_name(const wchar_t *path, wchar_t *out /* 4 */) {
    if (path[0] && path[1] == L':') { out[0] = towupper(path[0]); out[1] = L':'; out[2] = 0; }
    else wcscpy(out, L"C:");
}

/* L, A, the flags and F for one disk file; U and V stay as they were */
static int disk_measure_file(const wchar_t *vhd, DiskSizes *s) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    DWORD hi = 0, lo;
    size_t n = wcslen(vhd);
    s->len = s->alloc = -1;
    s->is_vhdx = n > 5 && !_wcsicmp(vhd + n - 5, L".vhdx");
    s->free = disk_free_gb(vhd);
    if (!GetFileAttributesExW(vhd, GetFileExInfoStandard, &fa)) return 0;
    s->len = ((double)fa.nFileSizeHigh * 4294967296.0 + fa.nFileSizeLow) / DISK_GIB;
    s->sparse = (fa.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
    s->compressed = (fa.dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
    s->encrypted = (fa.dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0;
    lo = GetCompressedFileSizeW(vhd, &hi);
    if (lo != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) s->alloc = ((double)hi * 4294967296.0 + lo) / DISK_GIB;
    return 1;
}

/* HARDCORE_DISK_FAKE=FILE: sizes from a JSON file in place of the real ones
   (tests). {"Debian": {"L": 268.1, "A": 200.9, "U": 165, "V": 1007, "F": 15.2,
   "dumps": 0, "phase": "running", "pct": 42, "freed": 31}} — GB. */
static char *disk_read_all(const wchar_t *path, size_t *len) {
    FILE *f = _wfopen(path, L"rb");
    char *b = NULL;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n >= 0 && n < (1 << 24) && (b = (char *)malloc((size_t)n + 1))) {
        n = (long)fread(b, 1, (size_t)n, f);
        b[n] = 0;
        if (len) *len = (size_t)n;
    }
    fclose(f);
    return b;
}

/* the text of "key": ... inside the object that starts at obj (the first match) */
static const char *disk_json_find(const char *obj, const char *end, const char *key) {
    char pat[80];
    const char *p;
    size_t n;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    n = strlen(pat);
    for (p = obj; p && p + n <= end; p++) {
        p = strstr(p, pat);
        if (!p || p >= end) return NULL;
        {
            const char *q = p + n;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
            if (q < end && *q == ':') {
                q++;
                while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
                return q;
            }
        }
    }
    return NULL;
}

typedef struct {
    int on;
    double dumps;                /* GB */
    char phase[16];              /* "", "running", "done", "failed", "busy", "countdown", "early" */
    int pct;
    double freed;
} DiskFake;

static int disk_fake(const wchar_t *distro, DiskSizes *s, DiskFake *fk) {
    const wchar_t *path = _wgetenv(L"HARDCORE_DISK_FAKE");
    char *all, name[200], *obj, *end;
    const char *v;
    int depth;
    if (fk) memset(fk, 0, sizeof(*fk));
    if (!path || !path[0] || !(all = disk_read_all(path, NULL))) return 0;
    WideCharToMultiByte(CP_UTF8, 0, distro, -1, name, sizeof(name), NULL, NULL);
    v = disk_json_find(all, all + strlen(all), name);
    if (!v || *v != '{') { free(all); return 0; }
    obj = (char *)v;
    for (end = obj, depth = 0; *end; end++) {
        if (*end == '{') depth++;
        else if (*end == '}' && !--depth) break;
    }
#define FAKE_NUM(k, dst) if ((v = disk_json_find(obj, end, k))) dst = strtod(v, NULL)
    FAKE_NUM("L", s->len);
    FAKE_NUM("A", s->alloc);
    FAKE_NUM("U", s->used);
    FAKE_NUM("V", s->fs_size);
    FAKE_NUM("F", s->free);
    if (fk) {
        double pct = 0;
        fk->on = 1;
        FAKE_NUM("dumps", fk->dumps);
        FAKE_NUM("pct", pct);
        FAKE_NUM("freed", fk->freed);
        fk->pct = (int)pct;
        if ((v = disk_json_find(obj, end, "phase")) && *v == '"') {
            size_t i;
            for (i = 0, v++; *v && *v != '"' && i + 1 < sizeof(fk->phase); ) fk->phase[i++] = *v++;
            fk->phase[i] = 0;
        }
    }
#undef FAKE_NUM
    s->wsl2 = 1;
    s->is_vhdx = 1;
    free(all);
    return 1;
}

/* ── the files: lock, state, log (design 8, rules 6, 7, 9) ── */

static void disk_file(wchar_t *out, const wchar_t *name) {
    const wchar_t *test = _wgetenv(L"HARDCORE_TEST_CONFIG"), *la = _wgetenv(L"LOCALAPPDATA");
    if (g_cfg_dir[0]) _snwprintf(out, MAX_PATH, L"%ls\\%ls", g_cfg_dir, name);
    else if (test) _snwprintf(out, MAX_PATH, L"%ls\\%ls", test, name);
    else _snwprintf(out, MAX_PATH, L"%ls\\hardcore\\%ls", la ? la : L".", name);
    out[MAX_PATH - 1] = 0;
}

/* One run at a time per PC. The file holds our PID and goes away when the
   handle closes, also when hardcore is killed. */
static HANDLE disk_lock_take(void) {
    wchar_t p[MAX_PATH];
    char pid[32];
    DWORD w;
    HANDLE h;
    disk_file(p, L"disk.lock");
    h = CreateFileW(p, GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    snprintf(pid, sizeof(pid), "%lu\n", (unsigned long)GetCurrentProcessId());
    WriteFile(h, pid, (DWORD)strlen(pid), &w, NULL);
    FlushFileBuffers(h);
    return h;
}

static void disk_lock_drop(HANDLE h) { if (h) CloseHandle(h); }

static int disk_pid_alive(DWORD pid) {
    HANDLE p;
    DWORD code = 0;
    if (!pid) return 0;
    if (!(p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) return GetLastError() == ERROR_ACCESS_DENIED;
    GetExitCodeProcess(p, &code);
    CloseHandle(p);
    return code == STILL_ACTIVE;
}

/* the PID of the process that holds the lock, 0 = none */
static DWORD disk_lock_holder(void) {
    wchar_t p[MAX_PATH];
    char buf[32] = "";
    DWORD got = 0, pid;
    HANDLE h;
    disk_file(p, L"disk.lock");
    h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    pid = strtoul(buf, NULL, 10);
    return disk_pid_alive(pid) ? pid : 0;
}

/* disk-state.json: a flat object of strings */
typedef struct { char k[64]; char v[192]; } DiskKV;
static DiskKV g_dkv[96];
static int g_dkv_n;

static void disk_state_load(void) {
    wchar_t p[MAX_PATH];
    char *all, *s;
    disk_file(p, L"disk-state.json");
    g_dkv_n = 0;
    if (!(all = disk_read_all(p, NULL))) return;
    for (s = all; (s = strchr(s, '"')) && g_dkv_n < 96; ) {
        DiskKV *kv = &g_dkv[g_dkv_n];
        size_t i = 0;
        for (s++; *s && *s != '"' && i + 1 < sizeof(kv->k); ) kv->k[i++] = *s++;
        kv->k[i] = 0;
        if (*s != '"') break;
        s = strchr(s + 1, ':');
        if (!s) break;
        s = strchr(s, '"');
        if (!s) break;
        for (i = 0, s++; *s && *s != '"' && i + 1 < sizeof(kv->v); s++) {
            if (*s == '\\' && s[1]) s++;
            kv->v[i++] = *s;
        }
        kv->v[i] = 0;
        if (*s == '"') s++;
        g_dkv_n++;
    }
    free(all);
}

static const char *disk_state_get(const char *k) {
    int i;
    for (i = 0; i < g_dkv_n; i++) if (!strcmp(g_dkv[i].k, k)) return g_dkv[i].v;
    return "";
}

static double disk_state_num(const char *k) { const char *v = disk_state_get(k); return *v ? strtod(v, NULL) : -1; }

static void disk_state_set(const char *k, const char *fmt, ...) {
    va_list ap;
    int i;
    for (i = 0; i < g_dkv_n && strcmp(g_dkv[i].k, k); i++) {}
    if (i == g_dkv_n) {
        if (g_dkv_n == 96) return;
        snprintf(g_dkv[g_dkv_n++].k, sizeof(g_dkv[0].k), "%s", k);
    }
    va_start(ap, fmt);
    vsnprintf(g_dkv[i].v, sizeof(g_dkv[i].v), fmt, ap);
    va_end(ap);
}

static void disk_state_save(void) {
    wchar_t p[MAX_PATH], tmp[MAX_PATH];
    FILE *f;
    int i;
    disk_file(p, L"disk-state.json");
    disk_file(tmp, L"disk-state.json.new");
    if (!(f = _wfopen(tmp, L"wb"))) return;
    fputs("{\n", f);
    for (i = 0; i < g_dkv_n; i++) {
        const char *c;
        fprintf(f, "  \"%s\": \"", g_dkv[i].k);
        for (c = g_dkv[i].v; *c; c++) {
            if (*c == '"' || *c == '\\') fputc('\\', f);
            fputc(*c, f);
        }
        fprintf(f, "\"%s\n", i + 1 < g_dkv_n ? "," : "");
    }
    fputs("}\n", f);
    fclose(f);
    MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING);
}

/* a state key for one distro: "Debian.used" */
static const char *disk_key(const wchar_t *distro, const char *what) {
    static char k[4][64];
    static int r;
    char n[48];
    r = (r + 1) & 3;
    WideCharToMultiByte(CP_UTF8, 0, distro, -1, n, sizeof(n), NULL, NULL);
    snprintf(k[r], sizeof(k[r]), "%s.%s", n, what);
    return k[r];
}

/* disk-reclaim.log: one line per action; kept under 1 MB, then rotated once */
static void disk_log(const wchar_t *distro, const char *fmt, ...) {
    wchar_t p[MAX_PATH], old[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    char line[900], name[128];
    SYSTEMTIME t;
    va_list ap;
    FILE *f;
    disk_file(p, L"disk-reclaim.log");
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (1u << 20)) {
        disk_file(old, L"disk-reclaim.log.1");
        MoveFileExW(p, old, MOVEFILE_REPLACE_EXISTING);
    }
    if (!(f = _wfopen(p, L"ab"))) return;
    GetLocalTime(&t);
    WideCharToMultiByte(CP_UTF8, 0, distro && distro[0] ? distro : L"-", -1, name, sizeof(name), NULL, NULL);
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, name, line);
    fclose(f);
}

/* ── hidden programs: wsl.exe for df, fstrim, ps, terminate ── */

/* Runs cmd with no window, feeds it stdin (or nothing), and returns its exit
   code (-1: did not start, -2: timed out and was stopped). *out gets what it
   printed (malloc'd, NULs of UTF-16 text dropped), or NULL. */
static long disk_run(const wchar_t *cmd, const char *in, char **out, DWORD timeout_ms) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, ird = NULL, iwr = NULL;
    wchar_t *line;
    char *buf = NULL;
    size_t n = 0, cap = 0, i, k;
    DWORD got, avail, t0 = GetTickCount(), code = (DWORD)-1;
    long rc = -1;
    if (out) *out = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&ird, &iwr, &sa, 1 << 16)) { CloseHandle(rd); CloseHandle(wr); return -1; }
    SetHandleInformation(iwr, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = ird;
    line = _wcsdup(cmd);
    if (line && CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(wr); wr = NULL;
        CloseHandle(ird); ird = NULL;
        if (in && *in) { DWORD w; WriteFile(iwr, in, (DWORD)strlen(in), &w, NULL); }
        CloseHandle(iwr); iwr = NULL;
        for (;;) {
            if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;   /* it exited */
            if (!avail) {
                if (GetTickCount() - t0 > timeout_ms) { TerminateProcess(pi.hProcess, 1); rc = -2; break; }
                Sleep(20);
                continue;
            }
            if (n + avail + 1 > cap) {
                char *b = (char *)realloc(buf, cap = (n + avail + 1) * 2);
                if (!b) break;
                buf = b;
            }
            if (!ReadFile(rd, buf + n, avail, &got, NULL)) break;
            n += got;
            buf[n] = 0;
            if (n > (4 << 20)) break;
        }
        if (rc != -2) {
            WaitForSingleObject(pi.hProcess, 5000);
            GetExitCodeProcess(pi.hProcess, &code);
            rc = (long)code;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    free(line);
    if (wr) CloseHandle(wr);
    if (ird) CloseHandle(ird);
    if (iwr) CloseHandle(iwr);
    CloseHandle(rd);
    if (buf) {
        for (i = k = 0; i < n; i++) if (buf[i]) buf[k++] = buf[i];   /* wsl.exe's own text is UTF-16 */
        buf[k] = 0;
    }
    if (out) *out = buf; else free(buf);
    return rc;
}

/* The distros that run now: wsl -l --running -q. It never starts one.
   1 when name runs; others gets the other running distros ("docker-desktop, Ubuntu"). */
static int wsl_running(const wchar_t *name, char *others, size_t cap) {
    char *o = NULL, n[128], *p, *e;
    int yes = 0;
    if (others && cap) others[0] = 0;
    if (disk_run(L"wsl.exe -l --running -q", NULL, &o, 15000) != 0 || !o) { free(o); return 0; }
    WideCharToMultiByte(CP_UTF8, 0, name, -1, n, sizeof(n), NULL, NULL);
    for (p = o; *p; p = e) {
        size_t len;
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        for (e = p; *e && *e != '\r' && *e != '\n'; e++) {}
        len = (size_t)(e - p);
        while (len && p[len - 1] == ' ') len--;
        if (!len) continue;
        if (len == strlen(n) && !_strnicmp(p, n, len)) yes = 1;
        else if (others && strlen(others) + len + 3 < cap) {
            if (others[0]) strcat(others, ", ");
            strncat(others, p, len);
        }
    }
    free(o);
    return yes;
}

static int wsl_distro_running(const wchar_t *name) { return wsl_running(name, NULL, 0); }

/* .wslconfig [wsl2] vmIdleTimeout, ms (WSL's default: 60000). WSL keeps a
   stopped distro's disk file attached until its Linux VM stops; the VM
   stops this long after its last distro stops. */
static long wsl_vm_idle_ms(void) {
    wchar_t p[MAX_PATH], *up = _wgetenv(L"USERPROFILE");
    char line[600];
    long ms = 60000;
    FILE *f;
    int in_wsl2 = 0;
    _snwprintf(p, MAX_PATH, L"%ls\\.wslconfig", up ? up : L".");
    p[MAX_PATH - 1] = 0;
    if (!(f = _wfopen(p, L"rb"))) return ms;
    while (fgets(line, sizeof(line), f)) {
        char *s = line, *v;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_wsl2 = !_strnicmp(s, "[wsl2]", 6); continue; }
        if (in_wsl2 && !_strnicmp(s, "vmIdleTimeout", 13) && (v = strchr(s, '='))) ms = strtol(v + 1, NULL, 10);
    }
    fclose(f);
    return ms;
}

/* What Linux reports, from one hidden sh run as root: the disk (df), the
   load, and the programs that a stop would end (design 6, check 5): each
   process on a terminal that is not a shell, and tmux or screen servers.
   Only call it while the distro runs: wsl.exe would start it. */
typedef struct {
    double used, fs_size;        /* GB, -1 = not known */
    double load;
    char busy[400];              /* "make, claude, node" */
    int nbusy;
} LinuxInfo;

static const char disk_probe_sh[] =
    "df -Pk / 2>/dev/null | awk 'NR==2{print \"df\", $2, $3}'\n"
    "read a b < /proc/loadavg; echo \"load $a\"\n"
    "me=$$\n"
    "for d in /proc/[0-9]*; do\n"
    "  p=${d#/proc/}; [ \"$p\" = \"$me\" ] && continue\n"
    "  read -r c < $d/comm 2>/dev/null || continue\n"
    "  case \"$c\" in\n"
    "    tmux*|screen) echo \"proc $p - $c\"; continue;;\n"
    "    init|systemd|systemd-*|\\(sd-pam\\)|bash|sh|dash|zsh|fish|ash|ksh|login|su|sudo|agetty|Relay*|plan9|ps|awk|readlink|cat|sleep) continue;;\n"
    "  esac\n"
    "  t=$(readlink $d/fd/0 2>/dev/null)\n"
    "  case \"$t\" in /dev/pts/*|/dev/tty*) echo \"proc $p $t $c\";; esac\n"
    "done\n";

static int linux_info(const wchar_t *distro, LinuxInfo *li) {
    wchar_t cmd[300];
    char *o = NULL, *line, *save = NULL;
    long rc;
    memset(li, 0, sizeof(*li));
    li->used = li->fs_size = -1;
    li->load = -1;
    _snwprintf(cmd, 300, L"wsl.exe -d %ls -u root -e sh -s", distro);
    cmd[299] = 0;
    rc = disk_run(cmd, disk_probe_sh, &o, 20000);
    if (!o) return 0;
    for (line = strtok_r(o, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        double kb_size, kb_used;
        char tty[64], comm[64];
        if (sscanf(line, "df %lf %lf", &kb_size, &kb_used) == 2) {
            li->fs_size = kb_size / (1024.0 * 1024.0);
            li->used = kb_used / (1024.0 * 1024.0);
        } else if (!strncmp(line, "load ", 5)) li->load = strtod(line + 5, NULL);
        else if (sscanf(line, "proc %*s %63s %63s", tty, comm) == 2) {
            char pat[70];
            snprintf(pat, sizeof(pat), "%s, ", comm);
            li->nbusy++;
            if (!strstr(li->busy, pat) && strlen(li->busy) + strlen(pat) + 1 < sizeof(li->busy)) strcat(li->busy, pat);
        }
    }
    if (li->busy[0]) li->busy[strlen(li->busy) - 2] = 0;   /* the last ", " */
    free(o);
    return rc == 0 && li->used >= 0;
}

static long wsl_trim(const wchar_t *distro, char *msg, size_t cap) {
    wchar_t cmd[300];
    char *o = NULL;
    long rc;
    _snwprintf(cmd, 300, L"wsl.exe -d %ls -u root -e sh -s", distro);
    cmd[299] = 0;
    rc = disk_run(cmd, "PATH=/usr/sbin:/sbin:$PATH; fstrim -v /\n", &o, 10 * 60 * 1000);
    if (msg && cap) {
        size_t i;
        snprintf(msg, cap, "%s", o ? o : "");
        for (i = 0; msg[i]; i++) if (msg[i] == '\r' || msg[i] == '\n') msg[i] = ' ';
        while (i && msg[i - 1] == ' ') msg[--i] = 0;
    }
    free(o);
    return rc;
}

static long wsl_terminate(const wchar_t *distro) {
    wchar_t cmd[300];
    _snwprintf(cmd, 300, L"wsl.exe --terminate %ls", distro);
    cmd[299] = 0;
    return disk_run(cmd, NULL, NULL, 60000);
}

/* ── who else uses the distro (design 6, check 4) ── */

typedef LONG (NTAPI *PFN_NtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);

/* the command line of another process (Windows 8.1+), "" when not readable */
static void process_cmdline(DWORD pid, wchar_t *out, size_t cap) {
    static PFN_NtQIP q;
    HANDLE p;
    BYTE buf[8192];
    ULONG n = 0;
    out[0] = 0;
    if (!q) q = (PFN_NtQIP)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!q || !(p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) return;
    if (q(p, 60 /* ProcessCommandLineInformation */, buf, sizeof(buf) - 2, &n) >= 0) {
        struct { USHORT Length, MaximumLength; PWSTR Buffer; } *u = (void *)buf;   /* UNICODE_STRING */
        size_t len = u->Length / sizeof(wchar_t);
        if (len >= cap) len = cap - 1;
        if (u->Buffer) { memcpy(out, u->Buffer, len * sizeof(wchar_t)); out[len] = 0; }
    }
    CloseHandle(p);
}

/* The distro a wsl.exe command line opens a shell in. 0: none (wsl -l,
   --terminate, --manage and friends, which open no session). */
static int wsl_cmdline_distro(const wchar_t *cl, wchar_t *out, size_t cap, const wchar_t *def) {
    static const wchar_t *admin[] = {L"-l", L"--list", L"--terminate", L"-t", L"--manage", L"--shutdown", L"--status",
                                     L"--version", L"-v", L"--help", L"--unregister", L"--export", L"--import",
                                     L"--import-in-place", L"--set-default", L"-s", L"--set-version", L"--update",
                                     L"--install", L"--mount", L"--unmount", L"--set-default-version", NULL};
    const wchar_t *p = cl;
    wchar_t tok[260];
    int first = 1, k;
    _snwprintf(out, cap, L"%ls", def);
    while (*p) {
        size_t n = 0;
        int q = 0;
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        while (*p && (q || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'"') q = !q;
            else if (n + 1 < 260) tok[n++] = *p;
            p++;
        }
        tok[n] = 0;
        if (first) { first = 0; continue; }          /* the program itself */
        if (!wcscmp(tok, L"-e") || !wcscmp(tok, L"--exec") || !wcscmp(tok, L"--")) break;   /* the rest is Linux's */
        for (k = 0; admin[k]; k++) if (!wcscmp(tok, admin[k])) return 0;
        if ((!wcscmp(tok, L"-d") || !wcscmp(tok, L"--distribution")) && *p) {
            while (*p == L' ') p++;
            n = 0; q = 0;
            while (*p && (q || *p != L' ')) {
                if (*p == L'"') q = !q;
                else if (n + 1 < 260) tok[n++] = *p;
                p++;
            }
            tok[n] = 0;
            _snwprintf(out, cap, L"%ls", tok);
        }
    }
    return 1;
}

typedef struct {
    int inside;                  /* this process runs inside the distro: stopping it stops us */
    int tabs;                    /* hardcore tabs of the distro (wsl.exe under a hardcore.exe) */
    int others;                  /* other WSL clients of the distro: Windows Terminal, VS Code, ... */
    wchar_t other_names[200];    /* their programs: "WindowsTerminal.exe, Code.exe" */
} WslClients;

static void wsl_clients(const wchar_t *distro, WslClients *wc) {
    PROCESSENTRY32W pe;
    HANDLE snap;
    DWORD pid[2048], ppid[2048];
    wchar_t exe[2048][40], def[128];
    WslDistro all[WSL_MAX_DISTROS];
    int n = 0, i, j;
    DWORD self = GetCurrentProcessId();
    memset(wc, 0, sizeof(*wc));
    wsl_distro_list(all, WSL_MAX_DISTROS, def, 128);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
        do {
            pid[n] = pe.th32ProcessID;
            ppid[n] = pe.th32ParentProcessID;
            _snwprintf(exe[n], 40, L"%ls", pe.szExeFile);
            exe[n][39] = 0;
            n++;
        } while (n < 2048 && Process32NextW(snap, &pe));
    CloseHandle(snap);
    for (i = 0; i < n; i++) {
        wchar_t cl[2048], target[128];
        int at = i, depth, under_hc = 0, ancestor_of_me = 0, root = i;
        if (_wcsicmp(exe[i], L"wsl.exe")) continue;
        process_cmdline(pid[i], cl, 2048);
        if (!cl[0] || !wsl_cmdline_distro(cl, target, 128, def) || _wcsicmp(target, distro)) continue;
        /* is it one of our ancestors? (we were started from inside it) */
        for (at = -1, j = 0; j < n; j++) if (pid[j] == self) at = j;
        for (depth = 0; at >= 0 && depth < 40; depth++) {
            int up = -1;
            if (pid[at] == pid[i]) { ancestor_of_me = 1; break; }
            for (j = 0; j < n; j++) if (pid[j] == ppid[at] && j != at) up = j;
            at = up;
        }
        if (ancestor_of_me) wc->inside = 1;
        /* under a hardcore.exe? (a tab: hardcore -> OpenConsole -> wsl.exe) */
        for (at = i, depth = 0; at >= 0 && depth < 40; depth++) {
            int up = -1;
            if (!_wcsicmp(exe[at], L"hardcore.exe")) { under_hc = 1; break; }
            for (j = 0; j < n; j++) if (pid[j] == ppid[at] && j != at) up = j;
            if (up >= 0) root = up;
            at = up;
        }
        /* a wsl.exe started by another wsl.exe is the same client */
        for (j = 0; j < n; j++) if (pid[j] == ppid[i] && !_wcsicmp(exe[j], L"wsl.exe")) break;
        if (j < n) continue;
        if (under_hc) wc->tabs++;
        else {
            /* the program that started it: the nearest ancestor that is not a shell or a console host */
            int up = -1;
            for (j = 0; j < n; j++) if (pid[j] == ppid[i]) up = j;
            while (up >= 0 && (!_wcsicmp(exe[up], L"OpenConsole.exe") || !_wcsicmp(exe[up], L"conhost.exe") ||
                               !_wcsicmp(exe[up], L"cmd.exe") || !_wcsicmp(exe[up], L"powershell.exe") || !_wcsicmp(exe[up], L"pwsh.exe"))) {
                int u2 = -1;
                for (j = 0; j < n; j++) if (pid[j] == ppid[up] && j != up) u2 = j;
                if (u2 < 0) break;
                up = u2;
            }
            wc->others++;
            if (up >= 0 && !wcsstr(wc->other_names, exe[up]) && wcslen(wc->other_names) + wcslen(exe[up]) + 3 < 200) {
                if (wc->other_names[0]) wcscat(wc->other_names, L", ");
                wcscat(wc->other_names, exe[up]);
            }
        }
        (void)root;
    }
}

/* ── crash dumps (design 5, tier A step 2): offer deletion only ── */

static void disk_dumps_dir(wchar_t *out) {
    wchar_t p[MAX_PATH], *up = _wgetenv(L"USERPROFILE"), *tmp = _wgetenv(L"TEMP");
    char line[600];
    FILE *f;
    int in_wsl2 = 0;
    _snwprintf(out, MAX_PATH, L"%ls\\wsl-crashes", tmp ? tmp : L".");
    out[MAX_PATH - 1] = 0;
    _snwprintf(p, MAX_PATH, L"%ls\\.wslconfig", up ? up : L".");
    if (!(f = _wfopen(p, L"rb"))) return;      /* read only: hardcore never writes .wslconfig */
    while (fgets(line, sizeof(line), f)) {
        char *s = line, *v;
        size_t n;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_wsl2 = !_strnicmp(s, "[wsl2]", 6); continue; }
        if (!in_wsl2 || _strnicmp(s, "crashDumpFolder", 15) || !(v = strchr(s, '='))) continue;
        for (v++; *v == ' '; v++) {}
        n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) v[--n] = 0;
        if (n) {
            char *c;
            for (c = v; *c; c++) if (*c == '/') *c = '\\';
            MultiByteToWideChar(CP_UTF8, 0, v, -1, out, MAX_PATH);
            out[MAX_PATH - 1] = 0;
        }
    }
    fclose(f);
}

/* the GB of files in it; del: delete them too (count in *files) */
static double disk_dumps(int del, int *files) {
    wchar_t dir[MAX_PATH], pat[MAX_PATH], f[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    double gb = 0;
    if (files) *files = 0;
    disk_dumps_dir(dir);
    _snwprintf(pat, MAX_PATH, L"%ls\\*", dir);
    pat[MAX_PATH - 1] = 0;
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        gb += ((double)fd.nFileSizeHigh * 4294967296.0 + fd.nFileSizeLow) / DISK_GIB;
        if (files) (*files)++;
        if (del) {
            _snwprintf(f, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            f[MAX_PATH - 1] = 0;
            DeleteFileW(f);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return gb;
}

/* ── the compaction (design 5, tier B steps 4 to 8) ── */

typedef DWORD (WINAPI *PFN_OpenVD)(PVIRTUAL_STORAGE_TYPE, PCWSTR, VIRTUAL_DISK_ACCESS_MASK, OPEN_VIRTUAL_DISK_FLAG, POPEN_VIRTUAL_DISK_PARAMETERS, PHANDLE);
typedef DWORD (WINAPI *PFN_CompactVD)(HANDLE, COMPACT_VIRTUAL_DISK_FLAG, PCOMPACT_VIRTUAL_DISK_PARAMETERS, LPOVERLAPPED);
typedef DWORD (WINAPI *PFN_ProgressVD)(HANDLE, LPOVERLAPPED, PVIRTUAL_DISK_PROGRESS);
typedef DWORD (WINAPI *PFN_CreateVD)(PVIRTUAL_STORAGE_TYPE, PCWSTR, VIRTUAL_DISK_ACCESS_MASK, PSECURITY_DESCRIPTOR, CREATE_VIRTUAL_DISK_FLAG, ULONG, PCREATE_VIRTUAL_DISK_PARAMETERS, LPOVERLAPPED, PHANDLE);
typedef DWORD (WINAPI *PFN_InfoVD)(HANDLE, PULONG, PGET_VIRTUAL_DISK_INFO, PULONG);

static struct {
    int tried;
    PFN_OpenVD open;
    PFN_CompactVD compact;
    PFN_ProgressVD progress;
    PFN_CreateVD create;
    PFN_InfoVD info;
} g_vd;

/* virtdisk.dll at run time, like ConPTY: a build that does not need it to start */
static int vd_load(void) {
    HMODULE m;
    if (g_vd.tried) return g_vd.open != NULL;
    g_vd.tried = 1;
    if (!(m = LoadLibraryExW(L"virtdisk.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32))) return 0;
    g_vd.open = (PFN_OpenVD)(void *)GetProcAddress(m, "OpenVirtualDisk");
    g_vd.compact = (PFN_CompactVD)(void *)GetProcAddress(m, "CompactVirtualDisk");
    g_vd.progress = (PFN_ProgressVD)(void *)GetProcAddress(m, "GetVirtualDiskOperationProgress");
    g_vd.create = (PFN_CreateVD)(void *)GetProcAddress(m, "CreateVirtualDisk");
    g_vd.info = (PFN_InfoVD)(void *)GetProcAddress(m, "GetVirtualDiskInformation");
    if (!g_vd.open || !g_vd.compact || !g_vd.progress) g_vd.open = NULL;
    return g_vd.open != NULL;
}

static const GUID disk_vendor_ms = {0xEC984AEC, 0xA0F9, 0x47e9, {0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B}};

static DWORD vd_open(const wchar_t *path, int sparse, DWORD access, HANDLE *h) {
    VIRTUAL_STORAGE_TYPE st;
    const wchar_t *ext = wcsrchr(path, L'.');
    *h = NULL;
    if (!vd_load()) return ERROR_PROC_NOT_FOUND;
    st.DeviceId = ext && !_wcsicmp(ext, L".vhd") ? VIRTUAL_STORAGE_TYPE_DEVICE_VHD : VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    st.VendorId = disk_vendor_ms;
    return g_vd.open(&st, path, (VIRTUAL_DISK_ACCESS_MASK)access,
                     (OPEN_VIRTUAL_DISK_FLAG)(sparse ? OPEN_VIRTUAL_DISK_FLAG_SUPPORT_SPARSE_FILES_ANY_FS : 0), NULL, h);
}

/* the virtual size, in GB, of a disk nobody holds (-1: not known) */
static double vd_virtual_size(const wchar_t *path, int sparse) {
    HANDLE h;
    GET_VIRTUAL_DISK_INFO info;
    ULONG sz = sizeof(info), used = 0;
    double gb = -1;
    if (vd_open(path, sparse, VIRTUAL_DISK_ACCESS_GET_INFO, &h)) return -1;
    memset(&info, 0, sizeof(info));
    info.Version = GET_VIRTUAL_DISK_INFO_SIZE;
    if (g_vd.info && !g_vd.info(h, &sz, &info, &used)) gb = (double)info.Size.VirtualSize / DISK_GIB;
    CloseHandle(h);
    return gb;
}

typedef struct DiskJob DiskJob;
struct DiskJob {
    wchar_t vhd[MAX_PATH];
    int sparse;
    DWORD wait_ms;               /* wait this long for WSL to let go of the file */
    volatile LONG cancel;        /* set from outside: stop the run (it is safe) */
    volatile LONG pct;
    double free_before, free_min, free_after;     /* GB on the drive */
    double alloc_before, alloc_after, len_before, len_after;
    double secs;
    DWORD err;                   /* 0 = done */
    char why[160];               /* for the user, when err */
    void (*progress)(DiskJob *j);    /* called about once a second, from the running thread */
    void *ctx;
};

/* Opens the file (waiting while WSL holds it), compacts it, and measures.
   Blocks; run it on its own thread for the window. */
static DWORD disk_compact_run(DiskJob *j) {
    HANDLE h = NULL;
    OVERLAPPED ov;
    COMPACT_VIRTUAL_DISK_PARAMETERS cp;
    VIRTUAL_DISK_PROGRESS pr;
    DiskSizes s;
    LARGE_INTEGER f0, f1, qf;
    DWORD t0 = GetTickCount(), r;
    memset(&s, 0, sizeof(s));
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&f0);
    disk_measure_file(j->vhd, &s);
    j->sparse = s.sparse;
    j->len_before = s.len;
    j->alloc_before = s.alloc;
    j->free_before = j->free_min = s.free;
    j->err = 0;
    j->why[0] = 0;
    for (;;) {
        r = vd_open(j->vhd, j->sparse, VIRTUAL_DISK_ACCESS_GET_INFO | VIRTUAL_DISK_ACCESS_METAOPS, &h);
        if (!r) break;
        if ((r != ERROR_SHARING_VIOLATION && r != ERROR_LOCK_VIOLATION) || GetTickCount() - t0 >= j->wait_ms || j->cancel) {
            j->err = r;
            snprintf(j->why, sizeof(j->why), r == ERROR_SHARING_VIOLATION || r == ERROR_LOCK_VIOLATION
                     ? "The disk file is in use. Linux, or another program, still holds it."
                     : r == ERROR_ACCESS_DENIED ? "Windows did not let hardcore open the disk file."
                     : r == ERROR_PROC_NOT_FOUND ? "This Windows has no virtual disk API (virtdisk.dll)."
                     : "Windows could not open the disk file.");
            return j->err;
        }
        Sleep(500);
    }
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    memset(&cp, 0, sizeof(cp));
    cp.Version = COMPACT_VIRTUAL_DISK_VERSION_1;
    r = g_vd.compact(h, COMPACT_VIRTUAL_DISK_FLAG_NONE, &cp, &ov);
    if (r == ERROR_IO_PENDING) {
        int cancelled = 0;
        for (;;) {
            double fr;
            DWORD w = WaitForSingleObject(ov.hEvent, 250);   /* free space is sampled 4 times a second */
            memset(&pr, 0, sizeof(pr));
            if (!g_vd.progress(h, &ov, &pr) && pr.CompletionValue)
                InterlockedExchange(&j->pct, (LONG)(pr.CurrentValue * 100 / pr.CompletionValue));
            fr = disk_free_gb(j->vhd);
            if (fr >= 0 && fr < j->free_min) j->free_min = fr;
            if (j->progress) j->progress(j);
            if (w == WAIT_OBJECT_0) { r = pr.OperationStatus; break; }
            if (!cancelled && (j->cancel || (fr >= 0 && fr < RECLAIM_FLOOR_GB))) {
                cancelled = 1;
                if (!j->cancel) snprintf(j->why, sizeof(j->why), "The drive fell under 2 GB free, so hardcore stopped the run.");
                CancelIoEx(h, &ov);
            }
        }
        if (r == ERROR_IO_PENDING) r = ERROR_GEN_FAILURE;
        if (cancelled && !r) r = ERROR_CANCELLED;
    }
    CloseHandle(ov.hEvent);
    CloseHandle(h);
    if (r == ERROR_OPERATION_ABORTED) r = ERROR_CANCELLED;
    j->err = r;
    if (r && !j->why[0])
        snprintf(j->why, sizeof(j->why), r == ERROR_CANCELLED ? "The run was stopped. The disk file is safe; it can run again."
                 : r == ERROR_ACCESS_DENIED ? "Windows did not let hardcore change the disk file."
                 : "Windows could not compact the disk file.");
    if (!r) InterlockedExchange(&j->pct, 100);
    disk_measure_file(j->vhd, &s);
    j->len_after = s.len;
    j->alloc_after = s.alloc;
    j->free_after = s.free;
    QueryPerformanceCounter(&f1);
    j->secs = (double)(f1.QuadPart - f0.QuadPart) / (double)qf.QuadPart;
    return r;
}

/* tests: a new VHDX of mb MB (no admin), sparse or not */
static DWORD vd_make_test(const wchar_t *path, unsigned mb, int sparse) {
    VIRTUAL_STORAGE_TYPE st;
    CREATE_VIRTUAL_DISK_PARAMETERS cp;
    HANDLE h = NULL;
    DWORD r;
    if (!vd_load() || !g_vd.create) return ERROR_PROC_NOT_FOUND;
    st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    st.VendorId = disk_vendor_ms;
    memset(&cp, 0, sizeof(cp));
    cp.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    cp.Version2.MaximumSize = (ULONGLONG)mb << 20;
    r = g_vd.create(&st, path, VIRTUAL_DISK_ACCESS_NONE, NULL, CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &cp, NULL, &h);
    if (r) return r;
    CloseHandle(h);
    if (sparse) {
        HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        DWORD n;
        FILE_SET_SPARSE_BUFFER b = {TRUE};
        if (f == INVALID_HANDLE_VALUE) return GetLastError();
        if (!DeviceIoControl(f, FSCTL_SET_SPARSE, &b, sizeof(b), NULL, 0, &n, NULL)) r = GetLastError();
        CloseHandle(f);
    }
    return r;
}

/* ── reclaim= and HARDCORE_RECLAIM= ── */

static char g_reclaim_cfg[16];   /* config.ini reclaim= */

static int reclaim_mode(void) {
    const wchar_t *e = _wgetenv(L"HARDCORE_RECLAIM");
    char v[16];
    if (e && e[0]) {
        WideCharToMultiByte(CP_UTF8, 0, e, -1, v, sizeof(v), NULL, NULL);
        return reclaim_mode_parse(v);
    }
    return reclaim_mode_parse(g_reclaim_cfg);
}

/* ── hardcore disk ... (design 7, command line) ── */

static void disk_gb_text(char *out, size_t cap, double gb) {
    if (gb < 0) snprintf(out, cap, "?");
    else if (gb < 10) snprintf(out, cap, "%.2f GB", gb);
    else snprintf(out, cap, "%.1f GB", gb);
}

static void disk_when(char *out, size_t cap, double t) {
    time_t tt = (time_t)t;
    struct tm *tm;
    if (t <= 0 || !(tm = localtime(&tt))) { snprintf(out, cap, "never"); return; }
    strftime(out, cap, "%Y-%m-%d %H:%M", tm);
}

/* the last run stopped before its end (hardcore was killed or Windows stopped) */
static int disk_run_stopped_early(void) {
    DWORD pid;
    if (!disk_state_get("running_since")[0]) return 0;
    pid = (DWORD)strtoul(disk_state_get("running_pid"), NULL, 10);
    return !disk_pid_alive(pid) && !disk_lock_holder();
}

static void disk_run_begin(const wchar_t *distro, const char *what) {
    char n[128];
    WideCharToMultiByte(CP_UTF8, 0, distro, -1, n, sizeof(n), NULL, NULL);
    disk_state_load();
    disk_state_set("running_since", "%lld", (long long)time(NULL));
    disk_state_set("running_pid", "%lu", (unsigned long)GetCurrentProcessId());
    disk_state_set("running_distro", "%s", n);
    disk_state_set("running_what", "%s", what);
    disk_state_save();
}

static void disk_run_end(void) {
    disk_state_load();
    disk_state_set("running_since", "%s", "");
    disk_state_set("running_pid", "%s", "");
    disk_state_set("running_what", "%s", "");
    disk_state_save();
}

/* the sizes of one distro: the file, the drive, and Linux (live while it
   runs and live is asked for, else the last value seen) */
static void disk_sizes_for(const WslDistro *w, int live, DiskSizes *s, LinuxInfo *li, int *running) {
    memset(s, 0, sizeof(*s));
    s->used = s->fs_size = -1;
    if (li) { memset(li, 0, sizeof(*li)); li->used = li->fs_size = li->load = -1; }
    disk_measure_file(w->vhd, s);
    s->wsl2 = wsl_distro_is_wsl2(w);
    *running = live ? wsl_distro_running(w->name) : 0;
    if (live && *running && li && linux_info(w->name, li)) {
        s->used = li->used;
        s->fs_size = li->fs_size;
        disk_state_load();
        disk_state_set(disk_key(w->name, "used"), "%.3f", s->used);
        disk_state_set(disk_key(w->name, "fs"), "%.3f", s->fs_size);
        disk_state_set(disk_key(w->name, "used_at"), "%lld", (long long)time(NULL));
        disk_state_save();
    } else {
        disk_state_load();
        s->used = disk_state_num(disk_key(w->name, "used"));
        s->fs_size = disk_state_num(disk_key(w->name, "fs"));
    }
    disk_fake(w->name, s, NULL);
}

/* WSL keeps each disk file attached to its Linux VM after wsl --terminate,
   until the VM stops (measured on WSL 2.7.3, 2026-11-11: a stopped distro's
   file stayed held by System for more than 2 minutes while another distro
   ran). The VM stops only when no distro runs. So a compaction needs every
   other distro stopped too. NULL = no blocker; else the text for the user. */
static const char *disk_vm_blocker(const wchar_t *distro) {
    static char msg[700];
    char others[300];
    long idle = wsl_vm_idle_ms();
    wsl_running(distro, others, sizeof(others));
    if (others[0]) {
        snprintf(msg, sizeof(msg),
                 "Not now: other Linux distros run (%s). WSL keeps every disk file in use while its Linux VM\n"
                 "runs, and the VM runs until all distros stop. Stop them first (wsl --terminate NAME), then run\n"
                 "this again. hardcore never runs wsl --shutdown for you.\n", others);
        return msg;
    }
    if (idle <= 0) {
        snprintf(msg, sizeof(msg), "Not possible: .wslconfig sets vmIdleTimeout=%ld, so the Linux VM never stops by itself and\n"
                 "WSL never lets go of the disk file. Use wsl --shutdown, then hardcore disk compact --yes.\n", idle);
        return msg;
    }
    return NULL;
}

static int disk_cmd_status(void) {
    WslDistro all[WSL_MAX_DISTROS];
    wchar_t def[128], dd[MAX_PATH];
    int n = wsl_distro_list(all, WSL_MAX_DISTROS, def, 128), i, files;
    double dumps = disk_dumps(0, &files);
    char a[32], b[32], c[32], d[32], e[32], when[40];
    cli("mode: %s%s\n", reclaim_mode_name(reclaim_mode()), _wgetenv(L"HARDCORE_RECLAIM") ? " (HARDCORE_RECLAIM)" : "");
    if (!n) cli("No WSL distro is registered for this user.\n");
    for (i = 0; i < n; i++) {
        WslDistro *w = &all[i];
        DiskSizes s;
        LinuxInfo li;
        int running;
        double cap;
        wchar_t drive[4];
        disk_sizes_for(w, 1, &s, &li, &running);
        disk_drive_name(w->vhd, drive);
        cli("\n%ls%s: %s\n", w->name, w->is_default ? " (default)" : "", running ? "running" : "stopped");
        cli("  disk file   %ls\n", w->vhd);
        if (!wsl_distro_is_wsl2(w)) { cli("  WSL 1: no disk file\n"); continue; }
        disk_gb_text(a, 32, s.len); disk_gb_text(b, 32, s.alloc); disk_gb_text(c, 32, s.free);
        cli("  size        %s (on the drive: %s)%s%s%s\n", a, b, s.sparse ? ", sparse" : "",
            s.compressed ? ", COMPRESSED" : "", s.encrypted ? ", ENCRYPTED" : "");
        cli("  %ls free     %s\n", drive, c);
        disk_gb_text(a, 32, s.used); disk_gb_text(b, 32, s.fs_size); disk_gb_text(d, 32, disk_slack(&s));
        disk_when(when, sizeof(when), disk_state_num(disk_key(w->name, "used_at")));
        cli("  Linux uses  %s of %s%s%s%s\n", a, b, running ? "" : " (last seen ", running ? "" : when, running ? "" : ")");
        cli("  slack       %s (the most a compaction can give back)\n", d);
        cli("  state       %s\n", disk_state_name(disk_state(&s)));
        if (s.sparse && !disk_sparse_clear_ok(&s) && s.len >= s.alloc)
            cli("  sparse flag keep it: clearing it would fill %.1f GB of holes, and %ls has %.1f GB free\n", s.len - s.alloc, drive, s.free);
        if (running && li.load >= 0) cli("  load        %.2f%s\n", li.load, li.load > RECLAIM_LOAD_BUSY ? " (busy)" : "");
        if (running && li.busy[0]) cli("  programs    %s (a stop ends them)\n", li.busy);
        if ((cap = disk_cap_suggest(&s)) > 0) cli("  cap         Linux can grow past the free space; hardcore disk cap can set it to %.0f GB\n", cap);
        disk_when(when, sizeof(when), disk_state_num(disk_key(w->name, "trim_at")));
        cli("  last trim   %s\n", when);
        disk_when(when, sizeof(when), disk_state_num(disk_key(w->name, "compact_at")));
        disk_gb_text(e, 32, disk_state_num(disk_key(w->name, "freed")));
        cli("  last run    %s %s%s%s\n", when, disk_state_get(disk_key(w->name, "result")),
            disk_state_get(disk_key(w->name, "result"))[0] ? ", gave back " : "", disk_state_get(disk_key(w->name, "result"))[0] ? e : "");
    }
    disk_dumps_dir(dd);
    disk_gb_text(a, 32, dumps);
    cli("\ncrash dumps  %s in %d files (%ls)%s\n", a, files, dd, dumps > RECLAIM_DUMPS_GB ? "; hardcore disk dumps --delete removes them" : "");
    disk_state_load();
    if (disk_run_stopped_early())
        cli("\nThe last disk cleanup stopped early. Linux files are safe. Run it again: hardcore disk compact --yes\n");
    else if (disk_lock_holder()) cli("\nA disk cleanup runs now (process %lu).\n", (unsigned long)disk_lock_holder());
    return 0;
}

static int disk_cmd_trim(const wchar_t *name) {
    WslDistro w;
    char msg[400];
    long rc;
    if (!wsl_distro_find(name, &w)) { cli("hardcore disk trim: no distro %ls\n", name && name[0] ? name : L"(default)"); return 2; }
    if (!wsl_distro_running(w.name)) {
        cli("%ls is not running. fstrim needs Linux to run, and hardcore does not start it for this.\n", w.name);
        return 1;
    }
    rc = wsl_trim(w.name, msg, sizeof(msg));
    disk_state_load();
    if (!rc) {
        disk_state_set(disk_key(w.name, "trim_at"), "%lld", (long long)time(NULL));
        disk_state_save();
    }
    disk_log(w.name, "trim rc=%ld %s", rc, msg);
    cli("%s\n", msg[0] ? msg : rc ? "fstrim failed" : "fstrim done");
    return rc ? 1 : 0;
}

static int disk_cmd_log(void) {
    wchar_t p[MAX_PATH];
    char *all, *s;
    int lines = 0;
    disk_file(p, L"disk-reclaim.log");
    if (!(all = disk_read_all(p, NULL)) || !all[0]) { cli("The disk log is empty.\n"); free(all); return 0; }
    for (s = all + strlen(all); s > all; s--) if (s[-1] == '\n' && ++lines > 50) break;
    cli("%s", s);
    free(all);
    return 0;
}

static int disk_cmd_dumps(int del) {
    wchar_t dir[MAX_PATH];
    int files;
    double gb;
    char a[32];
    disk_dumps_dir(dir);
    gb = disk_dumps(del, &files);
    disk_gb_text(a, 32, gb);
    if (del) {
        disk_log(NULL, "dumps deleted files=%d gb=%.3f", files, gb);
        cli("Deleted %d crash dump files (%s) in %ls.\n", files, a, dir);
    } else cli("%d crash dump files (%s) in %ls.\n", files, a, dir);
    return 0;
}

static void disk_cli_progress(DiskJob *j) {
    static LONG shown = -1;
    LONG p = j->pct;
    char a[32];
    if (p / 10 == shown / 10 && p != 100) return;
    shown = p;
    disk_gb_text(a, 32, j->free_min);
    cli("  %3ld%%   lowest free so far: %s\n", (long)p, a);
}

/* hardcore disk compact [--distro NAME | --vhd PATH] [--yes] [--dry-run] [--wait S] */
static int disk_cmd_compact(const wchar_t *name, const wchar_t *vhd, int yes, int dry, int wait_s) {
    WslDistro w;
    DiskSizes s;
    LinuxInfo li;
    WslClients wc;
    DiskJob *j;
    HANDLE lock;
    const char *why;
    char a[32], b[32], c[32], msg[400] = "";
    int running = 0, step = 1, blocked = 0;
    DWORD holder;
    memset(&w, 0, sizeof(w));
    memset(&wc, 0, sizeof(wc));
    memset(&li, 0, sizeof(li));
    if (vhd && vhd[0]) {
        _snwprintf(w.vhd, MAX_PATH, L"%ls", vhd);
        w.version = 2;
        memset(&s, 0, sizeof(s));
        s.used = s.fs_size = -1;
        disk_measure_file(w.vhd, &s);
        s.wsl2 = 1;
    } else {
        if (!wsl_distro_find(name, &w)) { cli("hardcore disk compact: no distro %ls\n", name && name[0] ? name : L"(default)"); return 2; }
        disk_sizes_for(&w, 1, &s, &li, &running);
        wsl_clients(w.name, &wc);
    }
    disk_gb_text(a, 32, s.len); disk_gb_text(b, 32, s.alloc); disk_gb_text(c, 32, s.free);
    cli("%ls%ls%ls\n  size %s, on the drive %s%s, drive free %s\n", w.name[0] ? w.name : L"", w.name[0] ? L": " : L"", w.vhd,
        a, b, s.sparse ? " (sparse)" : "", c);
    if (s.len < 0) { cli("hardcore cannot find the disk file.\n"); return 2; }
    if ((why = disk_preflight(&s))) { cli("Not possible: %s\n", why); return 2; }
    if (wc.inside) {
        cli("%s from here: this command runs inside %ls, and the step stops %ls.\n"
            "Run it from PowerShell, or from a tab of another distro.\n", dry ? "Blocked" : "Not possible", w.name, w.name);
        blocked = 1;
    }
    if ((holder = disk_lock_holder())) { cli("Not now: another disk cleanup runs (process %lu).\n", (unsigned long)holder); blocked = 1; }
    if (w.name[0] && (why = disk_vm_blocker(w.name))) { cli("%s", why); blocked = 1; }
    if (blocked && !dry) return 2;
    if (wait_s < 0) wait_s = w.name[0] ? (int)(wsl_vm_idle_ms() / 1000) + 120 : 5;
    if (s.used >= 0) {
        disk_gb_text(a, 32, s.used); disk_gb_text(b, 32, disk_slack(&s));
        cli("  Linux uses %s; at most %s can come back\n", a, b);
    }
    cli("Steps:\n");
    if (w.name[0]) {
        if (running) cli("  %d. fstrim: Linux marks its free blocks as free\n", step++);
        cli("  %d. stop %ls only (wsl --terminate %ls)%s\n", step++, w.name, w.name, running ? "" : ": not running now");
        if (running && (li.nbusy || wc.tabs || wc.others || li.load > RECLAIM_LOAD_BUSY)) {
            cli("     This stops:");
            if (wc.tabs) cli(" %d hardcore tab%s;", wc.tabs, wc.tabs == 1 ? "" : "s");
            if (wc.others) cli(" %d other WSL window%s (%ls);", wc.others, wc.others == 1 ? "" : "s", wc.other_names);
            if (li.busy[0]) cli(" programs: %s;", li.busy);
            if (li.load > RECLAIM_LOAD_BUSY) cli(" load %.2f;", li.load);
            cli("\n");
        }
    }
    cli("  %d. open the disk file, waiting up to %d s for WSL to let go of it%s%s\n", step++, wait_s,
        w.name[0] ? " (WSL lets go when its Linux VM stops, about a minute after the last distro stops)" : "", s.sparse ? " (sparse: open flag 0x400; the sparse flag stays)" : "");
    cli("  %d. compact it (CompactVirtualDisk); stop the run if the drive falls under %.0f GB free\n", step++, RECLAIM_FLOOR_GB);
    cli("  %d. measure again and write the log%ls\n", step++, w.name[0] ? L"; Linux starts again on its next use" : L"");
    if (dry) { cli("Dry run: nothing changed.\n"); return blocked ? 2 : 0; }
    if (!yes) { cli("Nothing changed. Run again with --yes to go on.\n"); return 3; }
    if (!(lock = disk_lock_take())) { cli("Not now: hardcore could not take %ls.\n", L"disk.lock"); return 2; }
    disk_run_begin(w.name[0] ? w.name : L"-", "compact");
    disk_log(w.name, "compact start L=%.3f A=%.3f F=%.3f U=%.3f sparse=%d", s.len, s.alloc, s.free, s.used, s.sparse);
    {
        const wchar_t *pause = _wgetenv(L"HARDCORE_DISK_TEST_PAUSE");   /* tests: time to kill us mid-run */
        if (pause) Sleep((DWORD)_wtoi(pause));
    }
    if (w.name[0] && running) {
        long rc = wsl_trim(w.name, msg, sizeof(msg));
        cli("fstrim: %s\n", msg[0] ? msg : rc ? "failed" : "done");
        disk_state_load();
        if (!rc) disk_state_set(disk_key(w.name, "trim_at"), "%lld", (long long)time(NULL));
        disk_state_save();
        cli("Stopping %ls...\n", w.name);
        rc = wsl_terminate(w.name);
        disk_log(w.name, "terminate rc=%ld", rc);
    }
    j = (DiskJob *)calloc(1, sizeof(*j));
    if (!j) { disk_lock_drop(lock); return 2; }
    _snwprintf(j->vhd, MAX_PATH, L"%ls", w.vhd);
    j->wait_ms = (DWORD)wait_s * 1000;
    j->progress = disk_cli_progress;
    cli("Compacting...\n");
    disk_compact_run(j);
    disk_gb_text(a, 32, j->alloc_before - j->alloc_after);
    disk_gb_text(b, 32, j->free_after);
    disk_gb_text(c, 32, j->free_before - j->free_min);
    disk_log(w.name, "compact %s err=%lu L=%.3f->%.3f A=%.3f->%.3f F=%.3f->%.3f minF=%.3f secs=%.1f%s%s",
             j->err ? "failed" : "ok", (unsigned long)j->err, j->len_before, j->len_after, j->alloc_before, j->alloc_after,
             j->free_before, j->free_after, j->free_min, j->secs, j->why[0] ? " why=" : "", j->why);
    disk_state_load();
    if (w.name[0]) {
        disk_state_set(disk_key(w.name, "compact_at"), "%lld", (long long)time(NULL));
        disk_state_set(disk_key(w.name, "result"), "%s", j->err ? "failed" : "ok");
        disk_state_set(disk_key(w.name, "freed"), "%.3f", j->alloc_before - j->alloc_after);
        if (j->err) disk_state_set(disk_key(w.name, "fail_until"), "%lld", (long long)time(NULL) + RECLAIM_SNOOZE_DAYS * 86400LL);
    }
    disk_state_save();
    disk_run_end();
    disk_lock_drop(lock);
    if (j->err) cli("Could not give space back: %s (error %lu)\nDetails: hardcore disk log\n", j->why, (unsigned long)j->err);
    else {
        char d[32], e[32];
        disk_gb_text(d, 32, j->len_after);
        disk_gb_text(e, 32, j->alloc_after);
        cli("Gave back %s. The drive has %s free. Took %.1f s.\n  size now %s, on the drive %s%s; the most extra drive space used during the run: %s\n",
            a, b, j->secs, d, e, j->sparse ? " (still sparse)" : "", c);
    }
    {
        DWORD err = j->err;
        free(j);
        return err ? 1 : 0;
    }
}

/* hardcore disk cap [--distro NAME] [--size GB] [--yes] [--dry-run]: wsl --manage NAME --resize */
static int disk_cmd_cap(const wchar_t *name, double size_gb, int yes, int dry) {
    WslDistro w;
    DiskSizes s;
    LinuxInfo li;
    WslClients wc;
    int running;
    double cap;
    wchar_t cmd[300];
    char *o = NULL;
    const char *why;
    long rc;
    HANDLE lock;
    if (!wsl_distro_find(name, &w)) { cli("hardcore disk cap: no distro %ls\n", name && name[0] ? name : L"(default)"); return 2; }
    disk_sizes_for(&w, 1, &s, &li, &running);
    wsl_clients(w.name, &wc);
    cap = size_gb > 0 ? size_gb : disk_cap_suggest(&s);
    if (s.used < 0 || s.fs_size < 0) { cli("hardcore does not know how much Linux uses yet. Start %ls once, then run this again.\n", w.name); return 2; }
    cli("%ls: Linux uses %.1f GB of a %.1f GB disk. The drive has %.1f GB free, and the file holds %.1f GB of it.\n",
        w.name, s.used, s.fs_size, s.free, s.alloc);
    if (cap <= 0) { cli("No cap needed: Linux cannot grow past the free space now, or there is no room for a useful cap.\n"); return 0; }
    if (cap < s.used + RECLAIM_CAP_HEADROOM_GB) { cli("Not possible: %.0f GB leaves Linux less than %.0f GB of room.\n", cap, RECLAIM_CAP_HEADROOM_GB); return 2; }
    if (wc.inside) { cli("Not possible from here: this command runs inside %ls, and the step stops %ls.\n", w.name, w.name); return 2; }
    if ((why = disk_vm_blocker(w.name))) { cli("%s", why); return 2; }
    cli("Steps:\n  1. stop %ls only (wsl --terminate %ls)\n  2. wsl --manage %ls --resize %.0fGB (WSL checks and shrinks the Linux file system)\n"
        "  Then Linux gets a \"disk full\" error when it reaches %.0f GB, in place of a crash when the drive fills.\n",
        w.name, w.name, w.name, cap, cap);
    if (dry) { cli("Dry run: nothing changed.\n"); return 0; }
    if (!yes) { cli("Nothing changed. Run again with --yes to go on.\n"); return 3; }
    if (!(lock = disk_lock_take())) { cli("Not now: hardcore could not take disk.lock.\n"); return 2; }
    disk_run_begin(w.name, "cap");
    if (running) disk_log(w.name, "terminate rc=%ld", wsl_terminate(w.name));
    _snwprintf(cmd, 300, L"wsl.exe --manage %ls --resize %.0fGB", w.name, cap);
    cmd[299] = 0;
    rc = disk_run(cmd, NULL, &o, 30 * 60 * 1000);
    {
        size_t i;
        for (i = 0; o && o[i]; i++) if (o[i] == '\r' || o[i] == '\n') o[i] = ' ';
    }
    disk_log(w.name, "cap size=%.0fGB rc=%ld %s", cap, rc, o ? o : "");
    disk_run_end();
    disk_lock_drop(lock);
    cli("%s%s", o ? o : "", o && o[0] ? "\n" : "");
    cli(rc ? "The cap failed (code %ld). Details: hardcore disk log\n" : "Linux's disk is now %.0f GB at most.\n", rc ? (double)rc : cap);
    free(o);
    return rc ? 1 : 0;
}

static int disk_cmd(int argc, wchar_t **argv) {
    const wchar_t *sub = argc > 0 ? argv[0] : L"status", *name = NULL, *vhd = NULL;
    int yes = 0, dry = 0, del = 0, wait_s = -1, k;
    double size = 0;
    for (k = 1; k < argc; k++) {
        if (!wcscmp(argv[k], L"--distro") && k + 1 < argc) name = argv[++k];
        else if (!wcscmp(argv[k], L"--vhd") && k + 1 < argc) vhd = argv[++k];
        else if (!wcscmp(argv[k], L"--wait") && k + 1 < argc) wait_s = _wtoi(argv[++k]);
        else if (!wcscmp(argv[k], L"--size") && k + 1 < argc) size = _wtof(argv[++k]);
        else if (!wcscmp(argv[k], L"--yes") || !wcscmp(argv[k], L"-y")) yes = 1;
        else if (!wcscmp(argv[k], L"--dry-run") || !wcscmp(argv[k], L"-n")) dry = 1;
        else if (!wcscmp(argv[k], L"--delete")) del = 1;
        else if (wcscmp(sub, L"make-test-vhd") && wcscmp(sub, L"hold-test-vhd")) {
            cli("hardcore disk: unknown option %ls\n", argv[k]);
            return 2;
        }
    }
    if (!wcscmp(sub, L"status")) return disk_cmd_status();
    if (!wcscmp(sub, L"trim")) return disk_cmd_trim(name);
    if (!wcscmp(sub, L"compact")) return disk_cmd_compact(name, vhd, yes, dry, wait_s);
    if (!wcscmp(sub, L"cap")) return disk_cmd_cap(name, size, yes, dry);
    if (!wcscmp(sub, L"dumps")) return disk_cmd_dumps(del);
    if (!wcscmp(sub, L"log")) return disk_cmd_log();
    /* test helpers: a small VHDX made without admin, and a holder that keeps it busy */
    if (!wcscmp(sub, L"make-test-vhd") && argc >= 3) {
        DWORD r = vd_make_test(argv[1], (unsigned)_wtoi(argv[2]), argc > 3 && !wcscmp(argv[3], L"sparse"));
        if (r) cli("make-test-vhd: error %lu\n", (unsigned long)r);
        return r ? 1 : 0;
    }
    if (!wcscmp(sub, L"hold-test-vhd") && argc >= 3) {
        HANDLE f = CreateFileW(argv[1], GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) { cli("hold-test-vhd: error %lu\n", (unsigned long)GetLastError()); return 1; }
        cli("holding\n");
        Sleep((DWORD)_wtoi(argv[2]) * 1000);
        CloseHandle(f);
        return 0;
    }
    cli("hardcore disk                 sizes, slack and state of each WSL disk\n"
        "hardcore disk trim [--distro NAME]\n"
        "                              fstrim now (only while Linux runs)\n"
        "hardcore disk compact [--distro NAME | --vhd PATH] [--dry-run] [--yes] [--wait S]\n"
        "                              stop that distro and give its free space back to the drive\n"
        "hardcore disk cap [--distro NAME] [--size GB] [--dry-run] [--yes]\n"
        "                              make Linux's disk no bigger than the drive can hold\n"
        "hardcore disk dumps [--delete] WSL crash dumps\n"
        "hardcore disk log             the last 50 lines of the disk log\n");
    return !wcscmp(sub, L"help") || !wcscmp(sub, L"--help") ? 0 : 2;
}
