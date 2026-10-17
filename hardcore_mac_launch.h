/* Plain CLI launches are independent GUI apps. Keep all command dispatch,
 * adoption handshakes and explicit foreground/debug runs on their caller. */
static int mac_gui_launch(int argc, char **argv) {
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--foreground"))return -1;
    if(getenv("HARDCORE_CTL"))return -1; /* owned GUI fixtures track this PID */
    const char *directory=NULL;
    if(argc==1) {
        /* Launch Services already detached a Finder/Dock launch. It should
           retain the normal saved-folder behavior instead of starting at /. */
        if(getppid()==1 && !isatty(0) && !isatty(1) && !isatty(2))return -1;
    } else if(argc==2 && argv[1][0]!='-')directory=argv[1];
    else if(argc==3 && (!strcmp(argv[1],"--cd") || !strcmp(argv[1],"-d")))directory=argv[2];
    else return -1;

    char cwd[MAX_PATH],resolved[MAX_PATH];
    if(!directory) {
        if(!getcwd(cwd,sizeof(cwd))){perror("hardcore: current directory");return 1;}
        directory=cwd;
    }
    struct stat st;
    if(!realpath(directory,resolved) || stat(resolved,&st)) {
        fprintf(stderr,"hardcore: cannot open directory '%s': %s\n",directory,strerror(errno));return 1;
    }
    if(!S_ISDIR(st.st_mode)) {
        fprintf(stderr,"hardcore: not a directory: %s\n",directory);return 1;
    }
    pid_t child;
    /* No shell interpolation, inherited cwd/environment, closed unrelated
       descriptors, a new session and /dev/null stdio. Pipes can reach EOF and
       closing the launching terminal cannot hang up the new window. */
    if(!mac_spawn(@(g_mac_exe_path),@[@"--foreground",@"--cd",@(resolved)],-1,1,&child)) {
        perror("hardcore: could not launch app");return 1;
    }
    return 0;
}
