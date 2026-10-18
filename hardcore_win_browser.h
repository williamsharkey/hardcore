/* Browser tabs on Windows. */
static int mac_browser_page_rows(Tab *tb) { return tb ? tb->t.rows : 0; }
static int mac_browser_cutie_generation(Tab *tb) { (void)tb; return 0; }
static int mac_browser_cutie_project(Tab *tb, Cell *output, int cols, int rows, int scroll) {
    (void)tb; (void)output; (void)cols; (void)rows; (void)scroll; return 0;
}
static void mac_browser_cutie_action(Tab *tb, int node) { (void)tb; (void)node; }
static void mac_browser_sync(void) {}
static void mac_browser_render_nav_overlay(void) {}
static void mac_browser_open_url(const char *url) { open_url(url); }
static void mac_browser_open_welcome(void) { mac_browser_open_url("https://github.com/williamsharkey/hardcore"); }
