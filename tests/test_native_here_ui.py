#!/usr/bin/env python3
"""Compile the production C terminal engine and native here UI against owned sinks.

Python is only the test runner. There is no alternate parser or mocked cell
renderer: the fixture feeds the shared vt_feed implementation and checks the
ANSI frame and input-routing behavior around it.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
static void set_clipboard_utf8(const char *s, int n) { (void)s; (void)n; }
static uint64_t test_now = 1000;
static SBuf writes[HERE_MAX_TABS];
static int font_calls;
/* Palette negotiation/quantization is exercised through the real transport.
 * This UI harness defaults to truecolor, so this mock is not used. */
static int here_color_index(uint32_t rgb) { (void)rgb; return 16; }
static void tab_write(Tab *tab, const void *data, int n) { (void)tab; (void)data; (void)n; }
static uint64_t here_now_ms(void) { return test_now; }
static int here_write(Here *h, int index, const char *data, size_t n) {
    if (index < 0 || index >= h->ntabs) return 0;
    sb_put(&writes[h->tabs[index].id], data, n); return 1;
}
static void here_resize_tabs(Here *h, int cols, int rows) {
    h->child_cols = cols; h->child_rows = rows;
    for (int i = 0; i < h->ntabs; i++) term_resize(&h->tabs[i].t, cols, rows);
}
static int here_new(Here *h, const char *cwd, char *const *argv) {
    (void)argv;
    if (h->ntabs >= HERE_MAX_TABS - 1) return 0;
    HereTab *t = &h->tabs[h->ntabs++]; memset(t, 0, sizeof(*t));
    t->id = ++h->next_id; t->fd = -1; t->pid = 100 + t->id;
    snprintf(t->cwd, sizeof(t->cwd), "%s", cwd ? cwd : "/fixture/project");
    snprintf(t->program, sizeof(t->program), "zsh");
    term_init(&t->t, h->child_cols ? h->child_cols : 80, h->child_rows ? h->child_rows : 23);
    return 1;
}
static void here_close(Here *h, int i) {
    term_free(&h->tabs[i].t); memmove(h->tabs + i, h->tabs + i + 1, (size_t)(--h->ntabs - i) * sizeof(HereTab));
}
static void here_caf(Here *h, int enabled) { h->caf_pid = enabled ? 1 : 0; }
static void here_font(Here *h, const char *name, int size, int allowed) {
    (void)h; (void)name; (void)size; (void)allowed; font_calls++;
}
#include "hardcore_here_helpers.h"
#include "hardcore_here_ui.h"
static int checks;
static void require(int ok, const char *why) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); exit(1); }
}
static void input(Here *h, const char *s) { here_ui_input(h, s, strlen(s)); }
static void feed(Term *t, const char *s) { vt_feed(t, (const unsigned char *)s, (int)strlen(s)); }
static void render(Here *h, SBuf *out) { out->n = 0; if (out->p) out->p[0] = 0; here_ui_render(h, out); }
static void mouse(Here *h, int button, int x, int y, int down) {
    char b[80]; snprintf(b, sizeof(b), "\x1b[<%d;%d;%d%c", button, x + 1, y + 1, down ? 'M' : 'm'); input(h, b);
}
int main(void) {
    Here *h = calloc(1, sizeof(*h)); require(h != NULL, "allocate fixture");
    snprintf(h->session, sizeof(h->session), "owned-ui-fixture");
    here_ui_init(h); here_new(h, NULL, NULL); here_new(h, NULL, NULL);
    here_ui_resize(h, 100, 36);
    require(h->child_cols == 71 && h->child_rows == 35, "wide host reserves sidebar and status row");
    Term *t = &h->tabs[0].t;
    feed(t, "\x1b[38;2;12;34;56;48;2;65;43;21;1;3;4mStyled\x1b[0m ");
    feed(t, "\x1b]8;;https://example.com/a\x1b\\link\x1b]8;;\x1b\\ ");
    feed(t, "\xe7\x95\x8c");
    SBuf frame = {0}; render(h, &frame);
    require(strstr(frame.p, "38;2;12;34;56") && strstr(frame.p, "48;2;65;43;21"), "truecolor cells preserved");
    require(strstr(frame.p, ";1;3;4;") != NULL, "bold italic underline preserved");
    require(strstr(frame.p, "\x1b]8;;https://example.com/a\x1b\\") != NULL, "OSC8 link preserved");
    require(strstr(frame.p, "\xe7\x95\x8c") != NULL, "wide Unicode glyph preserved");
    size_t full = frame.n; render(h, &frame);
    require(frame.n < full / 5, "unchanged frame emits only modes and cursor");
    here_ui_resize(h, 100, 36); render(h, &frame);
    require(frame.n > full / 2, "same-size reconnect forces repaint");

    HereHit exit_hit={0},detach_hit={0};
    for(int i=0;i<h->ui->nhits;i++) {
        if(h->ui->hits[i].op==HU_EXIT)exit_hit=h->ui->hits[i];
        if(h->ui->hits[i].op==HU_DETACH)detach_hit=h->ui->hits[i];
    }
    require(exit_hit.op&&detach_hit.op&&exit_hit.y==2&&detach_hit.y==2,"Exit and Detach occupy the third menu row");
    mouse(h,0,exit_hit.x,exit_hit.y,1);
    require(!h->action&&!h->ui->confirm,"Exit waits for mouse release");
    mouse(h,0,exit_hit.x,exit_hit.y,0);
    require(h->ui->confirm==2&&!h->action,"Exit menu asks before ending all programs");
    input(h,"n");require(!h->action,"cancelled Exit keeps programs alive");
    mouse(h,0,detach_hit.x,detach_hit.y,1);mouse(h,0,detach_hit.x,detach_hit.y,0);
    require(h->action==HERE_DETACH&&!h->ui->confirm,"Detach menu preserves session without prompting");
    h->action=0;

    mouse(h, 35, 73, 4, 1);
    require(h->ui->preview == 2 && h->active == 0, "hover previews without selecting");
    input(h, "A");
    require(writes[1].n == 1 && writes[2].n == 0, "hover never redirects keyboard input");
    input(h, "\x1b[O"); require(h->ui->preview == 0, "focus loss clears preview");
    input(h, "\x1b[I");
    mouse(h, 0, 73, 4, 1); require(h->active == 0, "tab down waits for release");
    mouse(h, 0, 73, 4, 0); require(h->active == 1, "tab release selects");
    require(writes[1].n == 1 && writes[2].n == 0, "menu click never leaks to child");
    render(h, &frame);
    mouse(h, 0, 73, 3, 1); mouse(h, 0, 10, 10, 0);
    require(h->active == 1 && writes[1].n == 1, "release outside menu cancels click");

    hu_choose(h, 0); hu_toggle_tv(h, 2); render(h, &frame);
    HereHit target = {0};
    for (int i = 0; i < h->ui->nhits; i++) if (h->ui->hits[i].op == HU_TV_INPUT && h->ui->hits[i].value == 2) { target = h->ui->hits[i]; break; }
    require(target.op == HU_TV_INPUT, "ordered sidebar TV has an input body");
    mouse(h, 0, target.x, target.y, 1); mouse(h, 0, target.x, target.y, 0);
    require(h->ui->tv_focus == 2 && h->active == 0, "TV body explicitly focuses without changing main tab");
    input(h, "B"); require(writes[2].n == 1 && writes[2].p[0] == 'B', "TV input routes to its own PTY");
    input(h, "\x1b"); test_now += 50; here_ui_tick(h, test_now);
    require(h->ui->tv_focus == 0, "standalone Escape returns from TV");
    h->ui->tv_focus = 2; here_ui_resize(h, 50, 12); render(h, &frame);
    require(h->child_cols == 50 && h->child_rows == 11, "narrow host uses bottom strip");
    require(h->ui->tv_focus == 0, "hidden TV cannot retain keyboard focus");

    here_ui_resize(h, 100, 36); hu_choose(h, 0); t = &h->tabs[0].t;
    writes[1].n = 0; t->m_bracketed = 1;
    input(h, "\x1b[20"); input(h, "0~caf\xc3"); input(h, "\xa9 \x02q\x1b[20"); input(h, "1~");
    const char expected[] = "\x1b[200~caf\xc3\xa9 \x02q\x1b[201~";
    require(writes[1].n == sizeof(expected) - 1 && !memcmp(writes[1].p, expected, sizeof(expected) - 1), "fragmented paste preserves UTF8 and literal Ctrl-B");
    require(!h->action && !h->ui->confirm, "pasted prefix cannot quit session");
    writes[1].n = 0; t->m_bracketed = 0;
    input(h, "\x1b[200~literal\x1b[201~");
    require(writes[1].n == 7 && !memcmp(writes[1].p, "literal", 7), "paste wrappers stripped for ordinary shell");

    t->m_mouse = 1000; t->m_mouse_sgr = 1; writes[1].n = 0; render(h, &frame);
    mouse(h, 0, 10, 5, 1); mouse(h, 0, 10, 5, 0);
    require(strstr(writes[1].p, "\x1b[<0;11;6M") && strstr(writes[1].p, "\x1b[<0;11;6m"), "child mouse coordinates preserved");
    size_t before = writes[1].n; mouse(h, 35, 10, 5, 1);
    require(writes[1].n == before, "click-only mouse mode does not receive motion");

    for (int n = 0; n < 80; n++) feed(t, n == 20 ? "\r\nsearch-target" : "\r\nordinary row");
    snprintf(h->ui->search, sizeof(h->ui->search), "SEARCH-TARGET"); hu_search_start(h, 1, 1);
    for (int i = 0; i < 20 && h->ui->search_left; i++) here_ui_tick(h, test_now += 20);
    require(strstr(h->ui->message, "Match on row") && t->view > 0, "bounded search finds scrollback case-insensitively");
    int old = t->view; input(h, "\x1b[6~"); require(t->view < old, "page down navigates local history");
    input(h, "\x02]"); require(t->view == 0, "live command clears scrollback view");

    input(h, "\x02s"); render(h, &frame); require(h->ui->overlay == 1, "settings panel opens");
    input(h, "j"); input(h, "\r"); require(g_scheme == &SCHEMES[1], "settings use shared native theme table");
    input(h, "h"); require(font_calls == 1, "host font change is explicit opt-in callback");
    input(h, "q"); input(h, "\x02q"); render(h, &frame);
    require(strstr(frame.p, "End all session processes? y/N") != NULL, "quit requires confirmation");
    input(h, "n"); require(!h->action, "declining quit preserves session");
    input(h, "\x02R"); require(h->action == HERE_RELOAD, "reload requests same session attachment");
    h->action = 0; input(h, "\x02" "D"); require(h->action == HERE_DETACH, "detach preserves owner");
    h->action = 0;
    h->ui->prefix = 1; h->ui->paste = h->ui->paste_wrapped = 1; h->ui->paste_id = 2;
    h->ui->input_n = 2; h->ui->input[0] = 27; h->ui->input[1] = '[';
    h->ui->preview = 2; h->ui->tv_focus = 2; h->ui->confirm = 2;
    writes[2].n = 0; int old_font_calls = font_calls;
    here_ui_attached(h);
    require(!h->ui->prefix && !h->ui->paste && !h->ui->input_n && !h->ui->preview && !h->ui->tv_focus && !h->ui->confirm, "new attachment clears interrupted controls");
    require(writes[2].n == 6 && !memcmp(writes[2].p, "\x1b[201~", 6), "new attachment terminates interrupted paste in original TV");
    require(font_calls == old_font_calls + 1, "saved explicit host font permission reapplies on attachment");
    h->ui->hostfont = 0; here_ui_attached(h);
    require(font_calls == old_font_calls + 1, "default attachment never changes host font");
    here_ui_resize(h, 2, 2); render(h, &frame);
    input(h, "\x02s"); render(h, &frame);
    mouse(h, 35, 10000, 10000, 1); mouse(h, 0, -10, -10, 0);
    require(!h->action && h->child_cols == 2 && h->child_rows == 1, "tiny terminals and out-of-bounds mouse stay safe");
    here_ui_resize(h, 100, 36); render(h, &frame);
    int old_active = h->active;
    mouse(h, 0, 73, 4, 1); mouse(h, 0, 73, 4, 0);
    require(h->active == old_active, "settings overlay blocks underlying tab click");

    input(h,"q");here_ui_resize(h,100,36);h->active=0;
    h->tabs[1].media_tab=1;
    Term *photo=&h->tabs[1].t;
    for(int y=0;y<photo->rows;y++)for(int x=0;x<photo->cols;x++)
        photo->row[photo->alt][y][x]=(Cell){.ch=0x2588,.fg=0x02000000u|(x<photo->cols/2?0xff0000u:0x0000ffu),.bg=0x02000000u};
    if(!hu_has_tv(h->ui,h->tabs[1].id))hu_toggle_tv(h,h->tabs[1].id);
    render(h,&frame);require(here_ui_media_visible(h,1),"visible media TV keeps its source visible");
    int red=0,blue=0;
    for(int y=6;y<h->child_rows;y++)for(int x=h->child_cols+1;x<h->cols;x++) {
        Cell c=h->ui->grid[(size_t)y*h->cols+x].c;
        red|=(c.fg&0xffffff)==0xff0000u||(c.bg&0xffffff)==0xff0000u;
        blue|=(c.fg&0xffffff)==0x0000ffu||(c.bg&0xffffff)==0x0000ffu;
    }
    require(red&&blue,"media TV shows both image edges instead of cropping");
    hu_toggle_tv(h,h->tabs[1].id);render(h,&frame);
    require(!here_ui_media_visible(h,1),"hidden media tab releases visibility");

    for (int i = 0; i < h->ntabs; i++) term_free(&h->tabs[i].t);
    here_ui_free(h); free(h); free(frame.p);
    for (int i = 0; i < HERE_MAX_TABS; i++) free(writes[i].p);
    printf("%d native UI checks passed\n", checks); return 0;
}
'''


class NativeHereUI(unittest.TestCase):
    def test_engine_renderer_and_input_routing(self):
        source = (ROOT / 'hardcore.c').read_text().split('\n#ifndef HARDCORE_TTY_ONLY\n', 1)[0]
        here = (ROOT / 'hardcore_here.h').read_text().split('#include "hardcore_here_helpers.h"', 1)[0]
        with tempfile.TemporaryDirectory(prefix='hc-native-ui-') as directory:
            tmp = Path(directory)
            (tmp / 'home').mkdir()
            c = tmp / ('ui.m' if sys.platform == 'darwin' else 'ui.c')
            c.write_text(source + '\n' + here + '\n#endif\n#endif\n' + HARNESS)
            exe = tmp / 'ui'
            command = ['clang', '-O1', '-g', '-fsanitize=address,undefined',
                '-Wno-unused-function', '-Wno-unused-variable', '-I', str(ROOT),
                '-DFONT_BIN="' + str(ROOT / 'build/font.bin') + '"',
                '-DFONTS_BIN="' + str(ROOT / 'build/fonts.bin') + '"', str(c)]
            if sys.platform == 'darwin':
                command += ['-framework', 'Foundation', '-framework', 'AppKit']
            command += ['-lutil', '-o', str(exe)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=45)
            self.assertEqual(build.returncode, 0, build.stderr)
            env = dict(os.environ, HOME=str(tmp / 'home'), ASAN_OPTIONS='detect_leaks=0')
            result = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('native UI checks passed', result.stdout)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
