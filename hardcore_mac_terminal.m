/* Terminal.app live bridge: the original host owns the job throughout.
 * Apple Events expose its text; targeted CGEvents provide interactive input.
 * No process injection, command replay, or closing of the source tab.
 * Terminal's AppleScript API does not expose cell colours or cursor position,
 * so this is explicitly a plain-text live bridge, not a PTY transfer.
 */
#import <ApplicationServices/ApplicationServices.h>
#include <libproc.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

static int mac_terminal_valid_tty(NSString *tty) {
    if (![tty isKindOfClass:[NSString class]] || ![tty hasPrefix:@"/dev/ttys"] || tty.length <= 9 || tty.length > 64) return 0;
    const char *path = tty.UTF8String;
    for (const char *p = path + 9; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* argv is used only to identify a launcher and its first script/module. It
 * is never displayed, logged, or interpreted as a command. */
static NSArray *mac_terminal_arguments(pid_t pid) {
    int mib[] = {CTL_KERN, KERN_PROCARGS2, pid};
    size_t size = 1024 * 1024;
    char *data = malloc(size);
    if (!data) return @[];
    NSMutableArray *arguments = [NSMutableArray array];
    if (!sysctl(mib, 3, data, &size, NULL, 0) && size > sizeof(int)) {
        int argc; memcpy(&argc, data, sizeof(argc));
        char *p = data + sizeof(argc), *end = data + size;
        while (p < end && *p) p++; /* executable path precedes argv */
        while (p < end && !*p) p++;
        for (int i = 0; i < argc && i < 16 && p < end; i++) {
            char *next = memchr(p, 0, (size_t)(end - p));
            if (!next) break;
            NSString *argument = [[[NSString alloc] initWithBytes:p length:(NSUInteger)(next - p)
                                                        encoding:NSUTF8StringEncoding] autorelease];
            [arguments addObject:argument ?: @""];
            p = next + 1;
        }
    }
    free(data);
    return arguments;
}

static NSString *mac_terminal_known_program(NSString *path) {
    NSString *base = path.lastPathComponent;
    if ([base isEqual:@"claude"] || [path containsString:@"/claude-code/"] ||
        [path containsString:@"/.local/share/claude/"]) return @"claude";
    if ([base isEqual:@"codex"] || [path containsString:@"/@openai/codex/"]) return @"codex";
    if ([base isEqual:@"grok"] || [path containsString:@"/grok-cli/"]) return @"grok";
    if ([base isEqual:@"agy"] || [base isEqual:@"ruffian"]) return base;
    return nil;
}

/* The installed CLI on PATH can be newer than a running native Claude.
 * Only the kernel's executable path identifies that process's release. */
static NSString *mac_terminal_native_version(NSString *path) {
    if (![mac_terminal_known_program(path) isEqual:@"claude"]) return @"";
    NSString *parent = path.stringByDeletingLastPathComponent;
    if (![parent.lastPathComponent isEqual:@"versions"] ||
        ![parent.stringByDeletingLastPathComponent.lastPathComponent isEqual:@"claude"]) return @"";
    NSString *version = path.lastPathComponent;
    NSArray *parts = [version componentsSeparatedByString:@"."];
    if (parts.count != 3) return @"";
    NSCharacterSet *notDigit = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789"] invertedSet];
    for (NSString *part in parts)
        if (!part.length || (part.length > 1 && [part hasPrefix:@"0"]) ||
            [part rangeOfCharacterFromSet:notDigit].location != NSNotFound) return @"";
    return version;
}

static int mac_terminal_is_shell(NSString *name) {
    return [@[@"sh", @"bash", @"zsh", @"fish", @"dash", @"ksh", @"csh", @"tcsh"] containsObject:name];
}

static NSString *mac_terminal_program_name(NSString *process, NSArray *arguments) {
    NSString *name = process.lastPathComponent ?: @"";
    NSString *arg0 = arguments.count ? arguments[0] : @"";
    NSString *known = mac_terminal_known_program(arg0) ?: mac_terminal_known_program(name);
    if (known) return known;
    NSString *lower = name.lowercaseString;
    int python = [lower isEqual:@"python"] || [lower hasPrefix:@"python3"] || [lower hasPrefix:@"python2"];
    int runtime = python || [@[@"node", @"bun", @"deno", @"ruby", @"perl"] containsObject:lower];
    if (!runtime) return name;
    for (NSUInteger i = 1; i < arguments.count; i++) {
        NSString *arg = arguments[i];
        if ([arg isEqual:@"-e"] || [arg isEqual:@"--eval"] || (python && [arg isEqual:@"-c"])) break;
        if (python && [arg isEqual:@"-m"] && i + 1 < arguments.count)
            return [[arguments[i + 1] componentsSeparatedByString:@"."] firstObject] ?: name;
        if ([@[@"-r", @"--require", @"--loader", @"--import"] containsObject:arg]) { i++; continue; }
        if ([arg hasPrefix:@"-"] || !arg.length ||
            (([lower isEqual:@"bun"] || [lower isEqual:@"deno"]) && [arg isEqual:@"run"])) continue;
        known = mac_terminal_known_program(arg);
        if (known) return known;
        NSString *base = arg.lastPathComponent;
        if ([@[@"js", @"mjs", @"cjs", @"py", @"rb", @"pl"] containsObject:base.pathExtension])
            base = base.stringByDeletingPathExtension;
        return base.length ? base : name;
    }
    return name;
}

/* Kernel-only metadata: no Apple Events, input injection, source resizing,
 * title heuristics, or terminal-output parsing. The bridge's own PTY does not
 * describe the source job, so callers must pass its original Terminal TTY. */
static NSDictionary *mac_terminal_metadata(NSString *tty) {
    if (!mac_terminal_valid_tty(tty)) return @{};
    struct stat device;
    if (stat(tty.fileSystemRepresentation, &device) || !S_ISCHR(device.st_mode)) return @{};
    int bytes = proc_listpids(PROC_TTY_ONLY, (uint32_t)device.st_rdev, NULL, 0);
    if (bytes <= 0 || bytes > 1024 * 1024) return @{};
    bytes += 32 * (int)sizeof(pid_t);
    pid_t *pids = calloc(1, (size_t)bytes);
    if (!pids) return @{};
    int got = proc_listpids(PROC_TTY_ONLY, (uint32_t)device.st_rdev, pids, bytes);
    struct proc_bsdinfo selected = {0};
    for (int i = 0; i < MIN(got, bytes) / (int)sizeof(pid_t); i++) {
        struct proc_bsdinfo info;
        if (pids[i] <= 1 || proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) ||
            info.pbi_uid != getuid() || info.e_tdev != (uint32_t)device.st_rdev ||
            !info.e_tpgid || info.pbi_pgid != info.e_tpgid) continue;
        /* Keep the foreground group leader, or its oldest surviving member
         * when a launcher has exited but the foreground job is still alive. */
        int older = info.pbi_start_tvsec < selected.pbi_start_tvsec ||
            (info.pbi_start_tvsec == selected.pbi_start_tvsec && info.pbi_start_tvusec < selected.pbi_start_tvusec);
        if (!selected.pbi_pid || info.pbi_pid == info.e_tpgid ||
            (selected.pbi_pid != selected.e_tpgid && older)) selected = info;
    }
    free(pids);
    if (!selected.pbi_pid) return @{};
    char process[256] = {0};
    proc_name((pid_t)selected.pbi_pid, process, sizeof(process));
    NSString *program = mac_terminal_program_name([NSString stringWithUTF8String:process] ?: @"",
                                                  mac_terminal_arguments((pid_t)selected.pbi_pid));
    char executable[PROC_PIDPATHINFO_MAXSIZE] = {0};
    NSString *programVersion = @"";
    if (proc_pidpath((pid_t)selected.pbi_pid, executable, sizeof(executable)) > 0) {
        NSString *path = [NSString stringWithUTF8String:executable] ?: @"";
        programVersion = mac_terminal_native_version(path);
        program = mac_terminal_known_program(path) ?: program;
    }
    struct proc_vnodepathinfo paths;
    NSString *cwd = @"";
    if (proc_pidinfo((pid_t)selected.pbi_pid, PROC_PIDVNODEPATHINFO, 0, &paths, sizeof(paths)) == sizeof(paths))
        cwd = [NSString stringWithUTF8String:paths.pvi_cdir.vip_path] ?: @"";
    /* Revalidate after reading argv/cwd: do not mix two jobs across PID/TTY
     * reuse or a foreground transition. The next refresh can retry. */
    struct proc_bsdinfo current;
    if (proc_pidinfo((pid_t)selected.pbi_pid, PROC_PIDTBSDINFO, 0, &current, sizeof(current)) != sizeof(current) ||
        current.pbi_start_tvsec != selected.pbi_start_tvsec || current.pbi_start_tvusec != selected.pbi_start_tvusec ||
        current.e_tdev != (uint32_t)device.st_rdev || current.e_tpgid != current.pbi_pgid) return @{};
    if (executable[0]) {
        char currentExecutable[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (proc_pidpath((pid_t)selected.pbi_pid, currentExecutable, sizeof(currentExecutable)) <= 0 ||
            strcmp(executable, currentExecutable)) return @{};
    }
    int shell = mac_terminal_is_shell(program);
    return @{@"tty":tty, @"cwd":cwd, @"program":shell ? @"" : program,
             @"programVersion":programVersion, @"shell":shell ? program : @"", @"pid":@(selected.pbi_pid)};
}

static NSAppleEventDescriptor *mac_terminal_script(NSString *body, int report) {
    NSString *source = [NSString stringWithFormat:@"with timeout of 3 seconds\ntell application id \"com.apple.Terminal\"\n%@\nend tell\nend timeout", body];
    static NSMutableDictionary *scripts;
    if (!scripts) scripts = [[NSMutableDictionary alloc] init];
    NSAppleScript *script = scripts[source];
    if (!script) {
        if (scripts.count >= 16) [scripts removeAllObjects];
        script = [[[NSAppleScript alloc] initWithSource:source] autorelease];
        scripts[source] = script;
    }
    NSDictionary *error = nil;
    NSAppleEventDescriptor *result = [script executeAndReturnError:&error];
    if (!result && report) fprintf(stderr, "hardcore: Terminal automation failed: %s\n", [error.description UTF8String]);
    return result;
}

static int g_mac_terminal_discovery_failed;
static NSArray *mac_terminal_sessions(int report) {
    g_mac_terminal_discovery_failed = 0;
    NSArray *apps = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.Terminal"];
    if (!apps.count) return @[];
    NSAppleEventDescriptor *result = mac_terminal_script(
        @"set found to {}\nrepeat with w in windows\nrepeat with t in tabs of w\n"
         "if (count of processes of t) > 0 then set end of found to {tty of t, id of w, custom title of t}\nend repeat\nend repeat\nreturn found", report);
    if (!result) g_mac_terminal_discovery_failed = 1;
    NSMutableArray *sessions = [NSMutableArray array];
    for (NSInteger i = 1; i <= result.numberOfItems; i++) {
        NSAppleEventDescriptor *item = [result descriptorAtIndex:i];
        NSString *tty = [item descriptorAtIndex:1].stringValue;
        if (mac_terminal_valid_tty(tty)) {
            int window = [item descriptorAtIndex:2].int32Value;
            pid_t host = ((NSRunningApplication *)apps[0]).processIdentifier;
            NSMutableDictionary *session = [NSMutableDictionary dictionaryWithDictionary:@{
                @"tty":tty, @"window":@(window), @"host":@(host),
                @"title":[item descriptorAtIndex:3].stringValue ?: @"",
                @"cwd":@"", @"program":@"", @"programVersion":@"", @"shell":@"", @"pid":@0,
                @"identity":[NSString stringWithFormat:@"terminal:%d:%d:%@", host, window, tty]}];
            [session addEntriesFromDictionary:mac_terminal_metadata(tty)];
            [sessions addObject:session];
        }
    }
    return sessions;
}

static NSString *mac_terminal_target(NSString *tty, int window, NSString *action) {
    /* TTY is validated as a device name before it reaches AppleScript. */
    return [NSString stringWithFormat:
        @"set w to window id %d\nrepeat with t in tabs of w\n"
         "if tty of t is \"%@\" then\n%@\nend if\nend repeat\nerror \"Source Terminal tab closed\"", window, tty, action];
}

static void mac_terminal_key(pid_t pid, CGKeyCode code, CGEventFlags flags, NSString *text) {
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, code, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, code, false);
    CGEventSetFlags(down, flags); CGEventSetFlags(up, flags);
    if (text) {
        UniChar chars[2]; NSUInteger n = text.length > 2 ? 2 : text.length;
        [text getCharacters:chars range:NSMakeRange(0, n)];
        CGEventKeyboardSetUnicodeString(down, n, chars);
        CGEventKeyboardSetUnicodeString(up, n, chars);
    }
    CGEventPostToPid(pid, down); CGEventPostToPid(pid, up);
    CFRelease(down); CFRelease(up);
}

/* Return bytes consumed, leaving incomplete UTF-8/escape sequences for the next read. */
static size_t mac_terminal_input(pid_t pid, const unsigned char *s, size_t n, int flush_escape) {
    static const CGKeyCode letters[26] = {0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c == 27) {
            if (n - i == 1 && !flush_escape) break;
            if (n - i >= 2 && (s[i+1] == '[' || s[i+1] == 'O')) {
                size_t j = i + 2;
                while (j < n && !(s[j] >= 0x40 && s[j] <= 0x7e)) j++;
                if (j == n && !flush_escape) break;
                if (j < n) {
                    CGKeyCode key = 0; int known = 1;
                    switch (s[j]) { case 'A': key=126; break; case 'B': key=125; break;
                        case 'C': key=124; break; case 'D': key=123; break; case 'H': key=115; break;
                        case 'F': key=119; break; case 'Z': key=48; break;
                        case '~': key = s[i+2]=='3' ? 117 : s[i+2]=='5' ? 116 : s[i+2]=='6' ? 121 : 0; known = key != 0; break;
                        default: known=0; }
                    if (known) {
                        CGEventFlags flags = s[j]=='Z' ? kCGEventFlagMaskShift : 0;
                        for (size_t k=i+2; k+1<j; k++) if (s[k]==';' && s[k+1]>='2' && s[k+1]<='8') {
                            int mod=s[k+1]-'1';
                            if (mod&1) flags|=kCGEventFlagMaskShift;
                            if (mod&2) flags|=kCGEventFlagMaskAlternate;
                            if (mod&4) flags|=kCGEventFlagMaskControl;
                        }
                        mac_terminal_key(pid,key,flags,nil); i=j+1; continue;
                    }
                }
            }
            mac_terminal_key(pid,53,0,nil); i++; continue;
        }
        if (c=='\r') mac_terminal_key(pid,36,0,nil);
        else if (c=='\t') mac_terminal_key(pid,48,0,nil);
        else if (c==127) mac_terminal_key(pid,51,0,nil);
        else if (c==0) mac_terminal_key(pid,49,kCGEventFlagMaskControl,nil);
        else if (c==28 || c==29) mac_terminal_key(pid,c==28 ? 42 : 30,kCGEventFlagMaskControl,nil);
        else if (c==30 || c==31) mac_terminal_key(pid,c==30 ? 22 : 27,kCGEventFlagMaskControl|kCGEventFlagMaskShift,nil);
        else if (c>0 && c<=26) mac_terminal_key(pid,letters[c-1],kCGEventFlagMaskControl,nil);
        else if (c>=32) {
            size_t len = c<128 ? 1 : (c&0xe0)==0xc0 ? 2 : (c&0xf0)==0xe0 ? 3 : (c&0xf8)==0xf0 ? 4 : 1;
            if (i+len>n) break;
            NSString *text = [[[NSString alloc] initWithBytes:s+i length:len encoding:NSUTF8StringEncoding] autorelease];
            if (text) mac_terminal_key(pid,0,0,text);
            i+=len; continue;
        }
        i++;
    }
    return i;
}

static int mac_terminal_bridge(const char *device, const char *window_arg) {
    NSString *tty = @(device);
    if (!mac_terminal_valid_tty(tty)) return 2;
    int window = atoi(window_arg); pid_t host = 0;
    for (NSDictionary *s in mac_terminal_sessions(1))
        if ([s[@"tty"] isEqual:tty] && [s[@"window"] intValue] == window) host = [s[@"host"] intValue];
    if (!host) { fprintf(stderr,"hardcore: source Terminal tab is unavailable\n"); return 1; }
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
    if (!AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options)) {
        fprintf(stderr,"hardcore: Terminal live input requires Accessibility permission for Hardcore (or its launching app). Enable it in System Settings > Privacy & Security > Accessibility, then retry slurp.\n"); return 1;
    }
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved)) return 1;
    raw=saved; cfmakeraw(&raw); tcsetattr(STDIN_FILENO,TCSANOW,&raw);
    NSMutableData *input = [NSMutableData data];
    NSString *previous = nil;
    struct winsize oldsize = {0};
    printf("\033]0;Terminal live bridge %s\007\033[?25l\033[?7l", device); fflush(stdout);
    int status = 0;
    for (;;) { @autoreleasepool {
        struct winsize size = {0}; ioctl(STDIN_FILENO,TIOCGWINSZ,&size);
        if (size.ws_col < 10) size.ws_col = 80;
        if (size.ws_row < 3) size.ws_row = 24;
        NSString *resize = @"";
        if (size.ws_col != oldsize.ws_col || size.ws_row != oldsize.ws_row) {
            resize = [NSString stringWithFormat:@"set number of columns of t to %u\nset number of rows of t to %u\n", size.ws_col, size.ws_row];
            oldsize=size;
        }
        NSAppleEventDescriptor *screen = mac_terminal_script(mac_terminal_target(tty,window,
            [resize stringByAppendingString:@"return contents of (contents of t)"]),1);
        if (!screen) { status=1; break; }
        NSString *text=screen.stringValue ?: @"";
        if (![text isEqual:previous]) {
            [previous release]; previous=[text copy];
            /* Screen text is data, not VT control sequences. */
            NSMutableString *clean=[NSMutableString string];
            for (NSUInteger i=0;i<text.length;i++) {
                unichar c=[text characterAtIndex:i];
                if (c=='\n') [clean appendString:@"\r\n"];
                else if (c=='\r') { if (i+1>=text.length || [text characterAtIndex:i+1]!='\n') [clean appendString:@"\r\n"]; }
                else if (c>=32 && c!=127) [clean appendFormat:@"%C",c];
            }
            while ([clean hasSuffix:@"\r\n"]) [clean deleteCharactersInRange:NSMakeRange(clean.length-2,2)];
            printf("\033[H\033[2J%s",clean.UTF8String); fflush(stdout);
        }
        struct pollfd p={STDIN_FILENO,POLLIN,0};
        int ready=poll(&p,1,50);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0 || (p.revents&(POLLHUP|POLLERR|POLLNVAL))) break;
        if (ready>0) { unsigned char buf[4096]; ssize_t n=read(STDIN_FILENO,buf,sizeof(buf)); if (n<=0) break; [input appendBytes:buf length:(NSUInteger)n]; }
        if (input.length) {
            /* Select the exact source tab/window without activating Terminal.
               Events are addressed to Terminal's PID, never the global keyboard. */
            if (!mac_terminal_script(mac_terminal_target(tty,window,@"set selected of t to true\nset index of w to 1\nreturn true"),1)) { status=1; break; }
            size_t used=mac_terminal_input(host,input.bytes,input.length,ready==0);
            [input replaceBytesInRange:NSMakeRange(0,used) withBytes:NULL length:0];
        }
    } }
    [previous release];
    tcsetattr(STDIN_FILENO,TCSANOW,&saved);
    printf("\033[?25h\r\nTerminal bridge detached. The original tab was left running.\r\n");
    return status;
}
