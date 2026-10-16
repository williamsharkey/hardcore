/* CLI uses only Foundation and the same-user window socket. No AppKit startup. */
static int browser_positive(const char *s) {
    if (!s || !*s) return 0;
    char *end=NULL; errno=0; long value=strtol(s,&end,10);
    return !errno && !*end && value>0 && value<INT_MAX ? (int)value : 0;
}
static int mac_browser_cli(int argc, char **argv) {
    const char *help="usage: hardcore browser [open [URL] | list | mode ID hardcore|native | close ID | back ID | forward ID | reload ID]\n"
        "       [--parent TAB_ID] [--window PID] [--mode hardcore|native]\n";
    if (argc && (!strcmp(argv[0],"--help") || !strcmp(argv[0],"-h"))) { fputs(help,stdout); return 0; }
    NSMutableArray *args=[NSMutableArray array];
    NSMutableDictionary *request=[NSMutableDictionary dictionary];
    int window=browser_positive(getenv("HARDCORE_WINDOW_PID"));
    int parent=browser_positive(getenv("HARDCORE_TAB_ID"));
    int explicitParent=0, explicitWindow=0;
    for (int i=0;i<argc;i++) {
        if (!strcmp(argv[i],"--window") || !strcmp(argv[i],"--parent") || !strcmp(argv[i],"--mode")) {
            const char *flag=argv[i];
            if (++i>=argc) { fputs(help,stderr); return 2; }
            if (!strcmp(flag,"--mode")) {
                if (strcmp(argv[i],"hardcore") && strcmp(argv[i],"native")) { fputs(help,stderr); return 2; }
                request[@"mode"]=[NSString stringWithUTF8String:argv[i]];
            } else {
                int value=browser_positive(argv[i]); if (!value) { fputs(help,stderr); return 2; }
                if (!strcmp(flag,"--window")) { window=value; explicitWindow=1; }
                else { parent=value; explicitParent=1; }
            }
        } else if (argv[i][0]=='-') { fputs(help,stderr); return 2; }
        else [args addObject:[NSString stringWithUTF8String:argv[i]] ?: @""];
    }
    if (explicitWindow && !explicitParent) parent=0;
    NSString *op=args.count ? args[0] : @"open"; request[@"op"]=op;
    if ([op isEqualToString:@"open"]) {
        if (args.count>2) { fputs(help,stderr); return 2; }
        NSString *url=browser_url(args.count>1 ? args[1] : @"hardcore:welcome");
        if (!url) { fprintf(stderr,"hardcore: invalid browser URL; use http(s), a local file, or hardcore:welcome\n"); return 2; }
        request[@"url"]=url;
        if (parent) request[@"parent"]=@(parent);
    } else if ([op isEqualToString:@"list"]) {
        if (args.count!=1) { fputs(help,stderr); return 2; }
    } else if ([@[@"mode",@"close",@"back",@"forward",@"reload"] containsObject:op]) {
        int identity=args.count>1 ? browser_positive([args[1] UTF8String]) : 0;
        if (!identity || args.count!=([op isEqual:@"mode"] ? 3 : 2)) { fputs(help,stderr); return 2; }
        request[@"id"]=@(identity);
        if ([op isEqual:@"mode"]) {
            if (![@[@"hardcore",@"native"] containsObject:args[2]]) { fputs(help,stderr); return 2; }
            request[@"mode"]=args[2];
        }
    } else { fputs(help,stderr); return 2; }
    NSString *dir=mac_runtime_dir(), *target=nil;
    if (window) target=[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%d.sock",window]];
    else {
        for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]) {
            if (![name hasSuffix:@".sock"]) continue;
            NSString *path=[dir stringByAppendingPathComponent:name];
            NSData *data=mac_request(path,"info\n",1);
            NSDictionary *info=data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            if (![info isKindOfClass:NSDictionary.class] || ![info[@"exe"] isEqual:g_mac_exe]) continue;
            if (target) { fprintf(stderr,"hardcore: multiple windows; choose --window PID (or run from a newly opened terminal tab)\n"); return 1; }
            target=path;
        }
    }
    if (!target) { fprintf(stderr,"hardcore: no matching running window; open Hardcore or reload its new build first\n"); return 1; }
    NSData *data=[NSJSONSerialization dataWithJSONObject:request options:0 error:nil];
    NSString *json=[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
    NSString *command=[NSString stringWithFormat:@"browser %@\n",json];
    NSData *reply=mac_request(target,command.UTF8String,3);
    NSDictionary *result=reply.length ? [NSJSONSerialization JSONObjectWithData:reply options:0 error:nil] : nil;
    if (![result isKindOfClass:NSDictionary.class]) {
        fprintf(stderr,"hardcore: no browser response; reload this window into the new build first\n"); return 1;
    }
    if (![result[@"ok"] boolValue]) { fprintf(stderr,"hardcore: %s\n",[browser_string(result[@"error"]) UTF8String]); return 1; }
    fwrite(reply.bytes,1,reply.length,stdout); putchar('\n'); return 0;
}
