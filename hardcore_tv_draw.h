/* Render each TV through the same bitmap/menu word-packing path. A surface
 * owns its last-frame hit map, opacity, geometry, and exposed corners. */
#ifndef _WIN32
static void render_tvs(void) {
    ChromeLayout primary=g_chrome;
    ChromeMenuFrame frame=chrome_frame_save();
    int origin=g_chrome_x;float alpha=g_chrome_motion_alpha;
    int joined_top=g_chrome_join_top,joined_bottom=g_chrome_join_bottom;
    for(int i=0;i<2;i++) {
        ChromeTv *tv=g_tvs+i;
        if(!tv->state.open || !tv->state.placed)continue;
        g_chrome=tv->layout;g_chrome_x=tv->state.rect.x*CW;
        g_chrome_motion_alpha=tv->state.alpha * tv->motion.alpha;
        chrome_prepare_edges(primary,origin,alpha);
        chrome_frame_load(tv->frame);
        chrome_capture_begin();
        for(int row=g_chrome.top;row<g_chrome.bottom;row++) {
            chrome_begin_row(row);
            fill_rect(0,row_y(row),(float)chrome_draw_width(),row_y(row+1),BAR_BG,1);
            chrome_sides(row,g_look.dim);
        }
        wchar_t text[96];
        grid_text(i==HC_TV_SETTINGS?L"Settings":L"CAF · awake",1,g_chrome.top,g_look.text);
        _snwprintf(text,96,L"%u",hc_tv_seconds_left(&tv->state,GetTickCount()));
        grid_text(text,SB_COLS-6,g_chrome.top,g_look.dim);
        grid_text(L"×",SB_COLS-2,g_chrome.top,g_look.text_lo);
        chrome_frame(g_chrome.top,g_look.dim);
        if(i==HC_TV_SETTINGS) {
            int idx=-1,hit=chrome_tv_hit(g_mouse_x,g_mouse_y,&idx);
            render_settings(hit,idx);
        } else {
            const wchar_t *lines[]={L"-d  Display stays awake",L"-i  System stays awake"};
            for(int n=0;n<2 && g_chrome.top+1+n<g_chrome.bottom;n++)
                grid_text(lines[n],1,g_chrome.top+1+n,g_look.text_lo);
            unsigned elapsed=(unsigned)mac_caf_elapsed();
            _snwprintf(text,96,L"Elapsed %02u:%02u:%02u",elapsed/3600,elapsed/60%60,elapsed%60);
            if(g_chrome.top+3<g_chrome.bottom)grid_text(text,1,g_chrome.top+3,g_look.detail);
            utf8_to_wide(mac_caf_power(),text,96);
            if(g_chrome.top+4<g_chrome.bottom)chrome_label(text,1,SB_COLS-1,g_chrome.top+4,g_look.detail);
            if(g_chrome.top+5<g_chrome.bottom)grid_text(L"Keep lid open · CAF to stop",1,g_chrome.top+5,g_look.dim);
        }
        chrome_begin_row(g_chrome.bottom-1);
        fill_rect(0,row_y(g_chrome.bottom)-1,(float)chrome_draw_width(),row_y(g_chrome.bottom),g_look.dim,1);
        chrome_round_edges();
        chrome_capture_end(chrome_tv_hovered(i));
        tv->frame=chrome_frame_save();
    }
    chrome_frame_load(frame);g_chrome=primary;g_chrome_x=origin;g_chrome_motion_alpha=alpha;
    g_chrome_join_top=joined_top;g_chrome_join_bottom=joined_bottom;
}
#endif
