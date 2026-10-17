#!/usr/bin/env python3
"""Terminal-hosted chrome interactions against a deterministic broker and host."""
from pathlib import Path
import os
import sys
import threading
import time
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import here_frontend as ui
from here import render_line
from here_terminal import Terminal


class FakeHost:
    input_fd=output_fd=-1
    def __init__(self):self.output=[]
    def write(self,value):self.output.append(value)


class FakeFont:
    available=False
    reason='Unsupported fixture host'
    def __init__(self,*args):self.calls=[]
    def apply(self,*args,**kwargs):self.calls.append(('apply',args,kwargs))
    def restore(self):self.calls.append(('restore',))


class FakeClient:
    def __init__(self):
        self.terminals={};self.commands=[];self.inputs=[];self.state={};self.events=[]
    def send(self,op,**fields):self.commands.append((op,fields))
    def input(self,data,tab=None):self.inputs.append((tab,data))
    def poll(self,timeout=0):result=self.events;self.events=[];return result


class FrontendTests(unittest.TestCase):
    def setUp(self):
        self.saved=[]
        self.font_patch=patch.object(ui,'HostFontSession',FakeFont);self.font_patch.start()
        self.save_patch=patch.object(ui,'save_settings',lambda settings:self.saved.append(dict(settings)));self.save_patch.start()
        self.host=FakeHost();self.client=FakeClient()
        self.app=ui.StudioApplication(self.client,'owned-test',settings={'theme':'g','allow_host_font':False},host=self.host)
        self.size(120,40)
        self.load_tabs(3)

    def tearDown(self):
        self.app.finish_font();self.save_patch.stop();self.font_patch.stop()

    def size(self,cols,rows):
        with patch.object(ui.os,'get_terminal_size',return_value=os.terminal_size((cols,rows))):self.app.resize()
        for term in self.client.terminals.values():term.resize(self.app.cols,self.app.rows)

    def load_tabs(self,count):
        tabs=[]
        for i in range(count):
            ident=100+i
            term=Terminal(self.app.cols,self.app.rows);term.feed(f'CONTENTS_{i}'.encode())
            self.client.terminals[ident]=term
            tabs.append({'id':ident,'pid':200+i,'cwd':f'/owned/folder{i}','label':'term','revision':term.revision})
        self.client.state={'active':0,'tabs':tabs};self.app.apply_state(self.client.state);self.app.compose()

    def hit(self,op,value=None):
        self.app.compose()
        return next((l+1,t+1) for l,t,r,b,action in self.app.hits if action==(op,value))

    def click(self,op,value=None):
        x,y=self.hit(op,value);self.app.mouse(0,x,y,True);self.app.mouse(0,x,y,False)

    def assert_grid(self,grid):
        self.assertEqual(len(grid),self.app.total_rows)
        for row in grid:
            self.assertEqual(len(row),self.app.total_cols)
            for x,cell in enumerate(row):
                if cell.width==0:self.assertGreater(x,0);self.assertEqual(row[x-1].width,2)
                if cell.width==2:self.assertLess(x+1,len(row));self.assertEqual(row[x+1].width,0)
        for left,top,right,bottom,_ in self.app.hits:
            self.assertTrue(0<=left<right<=self.app.total_cols)
            self.assertTrue(0<=top<bottom<=self.app.total_rows)

    def test_theme_recolors_existing_ansi_but_preserves_truecolor(self):
        term=self.app.current.terminal;term.feed(b'\r\x1b[31mA\x1b[38;2;204;36;29mB\x1b[39mC')
        self.app.set_theme('n')
        self.assertEqual(self.saved[-1]['theme'],'n')
        self.assertEqual(term.screen.grid[0][0].style.fg,self.app.theme.palette[1])
        self.assertEqual(term.screen.grid[0][1].style.fg,(204,36,29))
        rendered=render_line(term.screen.grid[0],theme=self.app.theme)
        self.assertIn('38;2;191;97;106',rendered);self.assertIn('38;2;204;36;29',rendered)
        self.assertNotIn('\x1b]4;',rendered)
        self.assertEqual(self.client.commands[-1][0],'theme');self.assertEqual(self.app.font.calls,[])

    def test_font_change_does_not_block_redraw_and_disabling_restores_after_apply(self):
        started=threading.Event();release=threading.Event();calls=[]
        class SlowFont(FakeFont):
            def apply(self,*args,**kwargs):
                started.set()
                if not release.wait(2):raise AssertionError('font fixture timed out')
                calls.append(('apply',args))
            def restore(self):calls.append(('restore',))
        self.app.font=SlowFont();self.app.settings.update(allow_host_font=True,font_name='Monaco',font_size=13)
        try:
            before=time.monotonic();self.app.apply_font()
            self.assertLess(time.monotonic()-before,.2);self.assertTrue(started.wait(1))
            self.app.paint();self.assertTrue(self.host.output)
            self.app.action(('hostfont',None))
            self.assertEqual(calls,[])
            release.set();self.app.font_future.result(timeout=1);self.app.poll_font()
            self.app.font_future.result(timeout=1);self.app.poll_font()
            self.assertEqual(calls,[('apply',('Monaco',13)),('restore',)])
        finally:release.set()

    def test_font_size_limits_match_restorable_host_adapter(self):
        self.app.settings.update(allow_host_font=False,font_size=48)
        self.app.action(('fontsize',1));self.assertEqual(self.app.settings['font_size'],48)
        self.app.settings['font_size']=6;self.app.action(('fontsize',-1))
        self.assertEqual(self.app.settings['font_size'],6);self.assertIsNone(self.app.font_future)

    def test_new_output_keeps_history_anchor_and_themes_do_not_mark_unread(self):
        tab=self.app.tabs[1]
        for n in range(65):tab.terminal.feed(f'\r\nline {n}'.encode())
        self.app.apply_state(self.client.state);tab.unread=False;tab.scroll=9
        visible=list(tab.terminal.visible(tab.scroll));before=tab.scroll
        tab.terminal.feed(b'\r\nnext row\r\nlast row');self.app.apply_state(self.client.state)
        self.assertEqual(tab.scroll,before+2);self.assertEqual(tab.terminal.visible(tab.scroll),visible)
        self.assertTrue(tab.unread)
        tab.unread=False;self.app.set_theme('n');self.app.apply_state(self.client.state)
        self.assertFalse(tab.unread)

    def test_backpressure_counts_unacknowledged_input_and_broker_queue(self):
        sequence=0
        def input(data,tab=None):
            nonlocal sequence
            sequence+=1;self.client.inputs.append((tab,data));return [sequence]
        self.client.input=input;self.client.pending=bytearray()
        for _ in range(16):self.app.send(b'x'*65536)
        self.assertFalse(self.app.input_ready())
        # Every ACK is processed but PTY output remains backlogged.
        self.client.events=[{'event':'reply','request':i,'ok':True,'tab':100,'pending':1024*1024} for i in range(1,17)]
        self.app.poll();self.assertFalse(self.app.input_ready());self.assertEqual(self.app.input_waiting,{})
        self.client.state['tabs'][0]['pending']=0
        self.client.events=[{'event':'state','state':self.client.state}]
        self.app.poll();self.assertTrue(self.app.input_ready())
        self.client.pending=bytearray(1024*1024);self.assertFalse(self.app.input_ready())

    def test_later_state_drain_supersedes_earlier_ack_queue_size(self):
        self.app.input_waiting={1:(100,3)}
        self.client.state['tabs'][0]['pending']=0
        self.client.events=[{'event':'reply','request':1,'ok':True,'tab':100,'pending':1500000},
                            {'event':'state','state':self.client.state}]
        self.app.poll();self.assertTrue(self.app.input_ready());self.assertEqual(self.app.current.pending,0)

    def test_foreground_program_and_shell_prompt_labels(self):
        tab=self.app.tabs[1];item=self.client.state['tabs'][1]
        item['program']='codex';self.app.apply_state(self.client.state)
        self.assertEqual(self.app.label(tab),'folder1:codex')
        item['program']='-zsh';tab.terminal.feed(b'\x1b]133;B\x07');self.app.apply_state(self.client.state)
        self.assertEqual(self.app.label(tab),'folder1:<new>')
        tab.terminal.feed(b'\x1b]133;C\x07\x1b]133;D;0\x07\x1b]133;B\x07')
        self.assertEqual(self.app.label(tab),'folder1:term')

    def test_unrendered_tv_and_exited_tab_explain_next_action(self):
        self.app.toggle_tv(1);self.size(60,10)
        status=''.join(c.text for c in self.app.compose()[-1])
        self.assertIn('TV kept open',status)
        self.app.current.exited=0
        self.assertIn('Program exited',''.join(c.text for c in self.app.compose()[-1]))

    def test_rollover_previews_without_switching_keyboard_destination_or_menu(self):
        before=self.app.compose();x,y=self.hit('choose',1)
        self.app.mouse(35,x,y,True)
        self.assertEqual(self.app.preview,1);self.assertEqual(self.app.active,0)
        preview=self.app.compose();self.assertTrue(''.join(c.text for c in preview[0]).startswith('CONTENTS_1'))
        self.assertEqual(preview[3][self.app.cols+1:],before[3][self.app.cols+1:])
        self.app.send(b'minor input');self.assertEqual(self.client.inputs,[(100,b'minor input')])
        self.assertFalse(any(op=='choose' for op,_ in self.client.commands))
        self.app.mouse(35,2,2,True);self.assertIsNone(self.app.preview)

    def test_tab_click_activates_only_on_release_inside_same_target(self):
        x,y=self.hit('choose',1)
        self.app.mouse(0,x,y,True);self.assertEqual(self.app.active,0)
        self.app.mouse(0,x,y,False);self.assertEqual(self.app.active,1)
        self.assertEqual(self.client.commands[-1],('choose',{'tab':101}))
        x,y=self.hit('choose',2);self.app.mouse(0,x,y,True)
        x,y=self.hit('choose',0);self.app.mouse(0,x,y,False)
        self.assertEqual(self.app.active,1)

    def test_focus_loss_cancels_preview_press_and_inactive_rollover(self):
        x,y=self.hit('choose',1);self.app.mouse(35,x,y,True);self.app.mouse(0,x,y,True)
        self.app.focus(False)
        self.assertIsNone(self.app.preview);self.assertIsNone(self.app.pressed)
        self.app.mouse(35,x,y,True);self.assertIsNone(self.app.preview)
        self.app.mouse(0,x,y,False);self.assertEqual(self.app.active,0)
        self.app.focus(True);self.app.mouse(35,x,y,True);self.assertEqual(self.app.preview,1)

    def test_chrome_press_release_outside_does_not_leak_mouse_up_to_child(self):
        self.app.current.terminal.mouse=1003;self.app.current.terminal.mouse_sgr=True
        x,y=self.hit('settings');self.app.mouse(0,x,y,True)
        self.app.mouse(0,2,2,False)
        self.assertEqual(self.client.inputs,[]);self.assertIsNone(self.app.overlay)

    def test_sidebar_reserves_child_viewport_and_files_uses_current_cwd(self):
        self.assertEqual(self.app.cols+self.app.menu_width+1,self.app.total_cols)
        self.assertEqual(self.client.commands[0],('resize',{'cols':91,'rows':39}))
        self.app.choose(2);self.app.current.cwd='/owned/moved folder'
        self.click('files')
        op,fields=self.client.commands[-1]
        self.assertEqual(op,'new');self.assertEqual(fields['cwd'],'/owned/moved folder')
        self.assertEqual(fields['command'][-1],str(ROOT/'tools/files.py'))
        self.assert_grid(self.app.compose())

    def test_resize_before_broker_response_never_leaves_half_a_wide_glyph(self):
        self.app.current.terminal.feed('\x1b[1;71H界'.encode())
        with patch.object(ui.os,'get_terminal_size',return_value=os.terminal_size((100,40))):self.app.resize()
        self.assertEqual(self.app.cols,71)
        self.assert_grid(self.app.compose())

    def test_tv_order_matches_tabs_and_compact_rows_remain_bounded(self):
        for index in (2,0,1):self.app.toggle_tv(index)
        grid=self.app.compose();self.assert_grid(grid)
        first_rows={}
        for left,top,right,bottom,action in self.app.hits:
            if action[0]=='tv_input':first_rows.setdefault(action[1],top)
        self.assertEqual(list(first_rows),(list(range(100,103))))
        for ident,row in first_rows.items():
            self.assertIn(f'folder{ident-100}:term',''.join(c.text for c in grid[row-1]))
        self.click('tv',1);self.assertNotIn(101,self.app.tvs);self.assertEqual(len(self.app.tabs),3)

    def test_explicit_tv_input_keeps_main_tab_and_escape_restores_destination(self):
        self.app.toggle_tv(1);self.app.compose();self.click('tv_input',101)
        self.assertEqual(self.app.active,0)
        self.app.send(b'quick action');self.assertEqual(self.client.inputs[-1],(101,b'quick action'))
        self.app.send(b'\x1b');self.app.send(b'main action')
        self.assertEqual(self.client.inputs[-1],(100,b'main action'))
        self.click('tv_input',101);self.app.toggle_tv(1);self.app.send(b'after minimize')
        self.assertEqual(self.client.inputs[-1],(100,b'after minimize'))

    def test_rollover_does_not_replace_explicit_tv_focus_and_hidden_tv_cannot_keep_input(self):
        self.app.toggle_tv(1);self.app.compose();self.click('tv_input',101)
        x,y=self.hit('choose',2);self.app.mouse(35,x,y,True)
        self.app.send(b'tv still owns input');self.assertEqual(self.client.inputs[-1],(101,b'tv still owns input'))
        self.size(60,12);self.app.compose();self.app.send(b'back to main')
        self.assertIsNone(self.app.tv_focus);self.assertEqual(self.client.inputs[-1],(100,b'back to main'))

    def test_history_search_is_case_insensitive_and_does_not_send_child_input(self):
        t=self.app.current.terminal
        for n in range(70):t.feed(f'\r\nROW_{n:02d} find-me'.encode())
        self.app.control(ord('/'));self.app.send(b'ROW_03');self.app.send(b'\r')
        self.assertIsNone(self.app.prompt);self.assertEqual(len(self.app.search_hits),1)
        self.assertGreater(self.app.current.scroll,0)
        visible=[''.join(c.text for c in row) for row in t.visible(self.app.current.scroll)]
        self.assertTrue(any('ROW_03' in row for row in visible));self.assertEqual(self.client.inputs,[])
        self.app.search_query='does not exist';self.app.find_next();self.assertEqual(self.app.message,'No matches')

    def test_tiny_dimensions_overlays_status_and_tabs_stay_in_bounds(self):
        for width,height in ((2,2),(3,3),(8,4),(30,8),(75,12),(76,12)):
            self.size(width,height)
            for overlay in (None,'help','settings'):
                self.app.overlay=overlay;self.assert_grid(self.app.compose())
            self.app.pending_confirm=('quit',None);self.assert_grid(self.app.compose());self.app.pending_confirm=None

    def test_close_confirmation_targets_requested_tab_and_consumes_response(self):
        self.click('close',102);self.assertEqual(self.app.pending_confirm,('close',102))
        self.app.send(b'n accidentally pasted');self.assertFalse(any(op=='close' for op,_ in self.client.commands))
        self.click('close',102);self.app.send(b'y unwanted text')
        self.assertEqual(self.client.commands[-1],('close',{'tab':102}));self.assertEqual(self.client.inputs,[])
        self.assertEqual(self.app.active,0)

    def test_quit_confirmation_ignores_remaining_batched_input(self):
        self.app.decoder.feed(b'\x02qy\x1b[A\x02c')
        self.assertTrue(self.app.stop);self.assertTrue(self.app.end_session)
        self.assertEqual(self.client.inputs,[])
        self.assertFalse(any(op=='new' for op,_ in self.client.commands))

    def test_search_prompt_accepts_split_utf8_and_batched_return_without_leaking(self):
        self.app.current.terminal.feed(' unique café 🚀'.encode())
        self.app.control(ord('/'))
        query='café 🚀'.encode()
        for byte in query:self.app.send(bytes([byte]))
        self.app.send(b'\rignored following text')
        self.assertEqual(self.app.search_query,'café 🚀');self.assertEqual(len(self.app.search_hits),1)
        self.assertEqual(self.client.inputs,[])

    def test_search_prompt_preserves_decomposed_combining_characters(self):
        self.app.current.terminal.feed('unique é'.encode());self.app.control(ord('/'))
        for byte in 'é'.encode():self.app.send(bytes([byte]))
        self.app.send(b'\r')
        self.assertEqual(self.app.search_query,'é');self.assertEqual(len(self.app.search_hits),1)

    def test_composition_diff_paints_only_changed_rows(self):
        self.app.paint();first=self.host.output[-1]
        self.app.paint();second=self.host.output[-1]
        self.assertIn('CONTENTS_0',first);self.assertNotIn('CONTENTS_0',second)
        term=self.app.current.terminal;term.feed(b'\x1b[2;1Hunique changed row')
        self.app.paint();third=self.host.output[-1]
        self.assertIn('unique changed row',third);self.assertNotIn('CONTENTS_0',third)
        self.assertLess(len(second),100)


if __name__=='__main__':unittest.main()
