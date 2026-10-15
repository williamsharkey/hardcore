/* Terminal.app live bridge: the original host owns the job throughout.
 * Apple Events expose its text; targeted CGEvents provide interactive input.
 * No process injection, command replay, or closing of the source tab.
 * Terminal's AppleScript API does not expose cell colours or cursor position,
 * so this is explicitly a plain-text live bridge, not a PTY transfer.
 */
#import <ApplicationServices/ApplicationServices.h>

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

static NSArray *mac_terminal_sessions(int report) {
    NSArray *apps = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.Terminal"];
    if (!apps.count) return @[];
    NSAppleEventDescriptor *result = mac_terminal_script(
        @"set found to {}\nrepeat with w in windows\nrepeat with t in tabs of w\n"
         "if (count of processes of t) > 0 then set end of found to {tty of t, id of w, custom title of t}\nend repeat\nend repeat\nreturn found", report);
    NSMutableArray *sessions = [NSMutableArray array];
    for (NSInteger i = 1; i <= result.numberOfItems; i++) {
        NSAppleEventDescriptor *item = [result descriptorAtIndex:i];
        NSString *tty = [item descriptorAtIndex:1].stringValue;
        if ([tty hasPrefix:@"/dev/ttys"])
            [sessions addObject:@{@"tty": tty, @"window": @([item descriptorAtIndex:2].int32Value),
                                  @"title": [item descriptorAtIndex:3].stringValue ?: @"",
                                  @"host": @(((NSRunningApplication *)apps[0]).processIdentifier)}];
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
    if (![tty hasPrefix:@"/dev/ttys"] || tty.length > 64) return 2;
    for (const char *p = device+9; *p; p++) if (!isdigit((unsigned char)*p)) return 2;
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
