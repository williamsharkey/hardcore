"""Terminal-hosted Hardcore chrome and interaction, backed by persistent PTYs."""
from __future__ import annotations
from dataclasses import dataclass, replace
from concurrent.futures import ThreadPoolExecutor
import codecs
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time
from here_terminal import Cell, Style, Terminal, BOLD, clean_text, text_cells, repair_line
from here_settings import THEMES, theme_for, load_settings, save_settings, HostFontSession, HostFontError
from here import HostTerminal, InputDecoder, render_line


@dataclass
class ViewTab:
    id: int
    pid: int
    terminal: Terminal
    label: str = 'term'
    cwd: str = ''
    exited: int | None = None
    scroll: int = 0
    unread: bool = False
    history_pushed: int | None = None
    activity: int | None = None
    program: str = ''
    pending: int = 0


class StudioApplication:
    """Fixed terminal viewport keeps applications coherent while chrome changes.

    Floating previews use spare chrome space. Settings/search are deliberate
    overlays; pointer hover never changes the actual input destination.
    """
    def __init__(self, client, session, settings=None, host=None):
        self.client,self.session=client,session
        self.settings=load_settings() if settings is None else dict(settings)
        self.theme=theme_for(self.settings.get('theme','g'))
        self.host=host or HostTerminal()
        self.font=HostFontSession(self.host.input_fd)
        self.tabs=[];self.active=0;self.preview=None;self.tvs=set();self.tv_focus=None;self.tv_cursor=None
        self.total_cols,self.total_rows,self.cols,self.rows=80,24,80,23
        self.menu=True;self.menu_width=0;self.menu_offset=0
        self.overlay=None;self.overlay_offset=0;self.hits=[]
        self.prompt=None;self.prompt_value='';self.prompt_decoder=codecs.getincrementaldecoder('utf-8')('replace');self.message=''
        self.search_query='';self.search_hits=[];self.search_at=-1
        self.pending_confirm=None;self.help=False
        self.focused=True;self.stop=False;self.end_session=False;self.reload=False
        self.resize_pending=True;self.suspend=False;self.dirty=True;self.previous=[]
        self.prefix=False;self.last_paint=0.;self.last_modes=None;self.old_signals={}
        self.caf=None;self.caf_started=0.
        self.font_pending=None;self.last_state=None;self.watched=();self.pressed=None
        self.font_worker=None;self.font_future=None;self.font_queued=None;self.font_action=None
        self.input_waiting={}
        self.decoder=InputDecoder(self.send,self.control,self.mouse,self.focus)

    @property
    def current(self):return self.tabs[self.active] if self.tabs else None

    @property
    def input_tab(self):
        return next((t for t in self.tabs if t.id==self.tv_focus),self.current)

    def request(self, op, **args):
        self.client.send(op, **args)

    def apply_state(self, state):
        self.last_state=state
        old={t.id:t for t in self.tabs};tabs=[]
        for item in state.get('tabs',[]):
            ident=item['id'];tab=old.get(ident)
            if tab is None:tab=ViewTab(ident,item.get('pid',0),Terminal(self.cols,self.rows))
            terminal=self.client.terminals.get(ident)
            if terminal is not None:
                tab.terminal=terminal
                tab.terminal.set_theme(self.theme.palette,self.theme.fg,self.theme.bg)
            revision=item.get('revision',0)
            activity=tab.terminal.activity
            if tab.activity is not None and tab.activity!=activity:tab.unread=True
            tab.activity=activity
            pushed=tab.terminal.history_pushed
            if tab.scroll and tab.history_pushed is not None:
                tab.scroll+=max(0,pushed-tab.history_pushed)
            tab.history_pushed=pushed
            tab.revision=revision
            tab.label=item.get('label',tab.label);tab.cwd=item.get('cwd') or tab.terminal.cwd or tab.cwd
            tab.program=item.get('program') or tab.label
            tab.pid=item.get('pid',tab.pid);tab.exited=item.get('exited')
            tab.pending=max(0,int(item.get('pending',tab.pending)))
            tab.scroll=min(tab.scroll,len(tab.terminal.history));tabs.append(tab)
        self.tabs=tabs
        self.active=max(0,min(int(state.get('active',self.active)),len(tabs)-1))
        if self.current and self.focused and not self.current.scroll and self.preview is None:self.current.unread=False
        self.tvs.intersection_update(t.id for t in tabs)
        if self.tv_focus not in self.tvs:self.tv_focus=None
        if self.preview is not None and self.preview>=len(tabs):self.preview=None
        self.dirty=True

    def watch(self):
        wanted=set(self.tvs)
        # Keep small live screens ready for instant rollover, without retaining
        # separate processes or replaying input to a preview.
        wanted.update(t.id for t in self.tabs)
        wanted=tuple(sorted(wanted))
        if wanted!=self.watched:self.request('watch',tabs=list(wanted));self.watched=wanted

    def poll(self, timeout=0):
        events=self.client.poll(timeout)
        changed=any(m.get('event') in ('state','terminal','terminal_delta') for m in events)
        if changed or self.last_state is None:self.apply_state(self.client.state)
        for message in events:
            if message.get('event')=='state':
                pending={item['id']:item.get('pending',0) for item in message.get('state',{}).get('tabs',[])}
                for tab in self.tabs:
                    if tab.id in pending:tab.pending=max(0,int(pending[tab.id]))
            elif message.get('event')=='reply':
                request=message.get('request');sent=self.input_waiting.pop(request,None)
                if sent and 'pending' in message:
                    tab=next((t for t in self.tabs if t.id==sent[0]),None)
                    if tab:tab.pending=max(0,int(message['pending']))
            if message.get('error'):self.message=str(message['error']);self.dirty=True
            if message.get('event') in ('disconnected','detached'):
                self.message=message.get('reason','Session connection closed');self.stop=True
        if not self.stop:self.watch()

    def child_input(self,data,tab):
        # Count requests until acknowledged as well as the broker's queued PTY
        # bytes. Reading can outrun its periodic metadata during a large paste.
        requests=self.client.input(data,tab=tab)
        if requests:
            for request,offset in zip(requests,range(0,len(data),65536)):
                self.input_waiting[request]=(tab,min(65536,len(data)-offset))

    def input_ready(self):
        tab=self.input_tab
        queued=sum(size for ident,size in self.input_waiting.values() if tab and ident==tab.id)
        return len(getattr(self.client,'pending',()))<1024*1024 and (not tab or tab.pending+queued<1024*1024)

    def resize(self):
        size=os.get_terminal_size(self.host.output_fd)
        self.total_cols=max(2,min(500,size.columns));self.total_rows=max(2,min(201,size.lines))
        self.menu_width=28 if self.menu and self.total_cols>=76 else 0
        self.cols=max(2,self.total_cols-self.menu_width-(1 if self.menu_width else 0));self.rows=self.total_rows-1
        self.request('resize',cols=self.cols,rows=self.rows)
        self.previous=[];self.resize_pending=False;self.preview=None;self.tv_focus=None;self.dirty=True

    def choose(self,index):
        if not self.tabs:return
        self.active=index%len(self.tabs);self.preview=None;self.tv_focus=None;self.current.unread=False
        self.request('choose',tab=self.current.id)
        self.previous=[];self.dirty=True;self.last_modes=None

    def label(self,tab):
        title=tab.terminal.title
        folder=Path(tab.cwd).name if tab.cwd else ''
        program=Path(tab.program or tab.label).name.lstrip('-')
        if program in ('sh','zsh','bash','fish','dash','ksh','csh','tcsh','pwsh','powershell','nu'):
            program='<new>' if tab.terminal.prompt_state in ('prompt','input') and not tab.terminal.command_count else 'term'
        return title or (folder+':'+program if folder else program)

    def set_theme(self,key):
        self.theme=theme_for(key);self.settings['theme']=self.theme.key
        for tab in self.tabs:tab.terminal.set_theme(self.theme.palette,self.theme.fg,self.theme.bg)
        self.request('theme',palette=[list(c) for c in self.theme.palette],foreground=list(self.theme.fg),background=list(self.theme.bg))
        self.persist();self.previous=[];self.dirty=True

    def persist(self):
        try:save_settings(self.settings)
        except OSError as e:self.message='Settings could not be saved: '+str(e)

    def toggle_tv(self,index):
        if not 0<=index<len(self.tabs):return
        ident=self.tabs[index].id
        if ident in self.tvs:
            self.tvs.remove(ident)
            if self.tv_focus==ident:self.tv_focus=None
        elif len(self.tvs)<4:self.tvs.add(ident)
        else:self.message='Four previews are already open'
        self.dirty=True

    def control(self,key):
        if self.stop:return
        self.prefix=key is None;self.dirty=True
        if key is None:return
        if key==-1:return
        if key==ord('c'):self.request('new')
        elif key in (ord('n'),ord('p')):self.choose(self.active+(1 if key==ord('n') else -1))
        elif ord('1')<=key<=ord('9'):self.choose(key-ord('1'))
        elif key==ord('x'):self.pending_confirm=('close',self.current.id if self.current else None)
        elif key==ord('q'):self.pending_confirm=('quit',None)
        elif key in (ord('D'),):self.stop=True
        elif key==ord('R'):self.reload=True;self.stop=True
        elif key in (ord('s'),ord(',')):self.overlay=None if self.overlay=='settings' else 'settings';self.overlay_offset=0
        elif key==ord('m'):self.menu=not self.menu;self.resize_pending=True
        elif key==ord('v'):self.toggle_tv(self.active)
        elif key in (ord('/'),ord('f')):self.prompt='search';self.prompt_value=self.search_query
        elif key==ord('N'):self.find_next(-1)
        elif key==ord('j'):self.find_next(1)
        elif key==ord('F'):self.open_files()
        elif key==ord('w'):self.prompt='url';self.prompt_value='https://'
        elif key==ord('?'):self.overlay=None if self.overlay=='help' else 'help'
        elif key in (ord('u'),ord('[')) and self.current:self.current.scroll=min(len(self.current.terminal.history),self.current.scroll+self.rows)
        elif key==ord('d') and self.current:self.current.scroll=max(0,self.current.scroll-self.rows)
        elif key==ord(']') and self.current:self.current.scroll=0

    def open_files(self):
        helper=Path(__file__).with_name('files.py')
        if helper.is_file():self.request('new',command=[sys.executable,'-B',str(helper)],cwd=self.current.cwd if self.current else None)
        else:self.message='Files is not installed in this frontend'

    def open_web(self,url):
        if not url.strip():return
        helper=Path(__file__).with_name('here_web.py')
        self.request('new',command=[sys.executable,'-B',str(helper),url])

    def find_next(self,direction=1):
        if not self.current or not self.search_query:return
        tab=self.current;lines=list(tab.terminal.history)+tab.terminal.normal.grid
        query=self.search_query.casefold()
        hits=[i for i,line in enumerate(lines) if query in ''.join(c.text for c in line if c.width).casefold()]
        self.search_hits=hits
        if not hits:self.message='No matches';self.dirty=True;return
        self.search_at=(self.search_at+direction)%len(hits)
        at=hits[self.search_at];tab.scroll=max(0,min(len(tab.terminal.history),len(lines)-self.rows-at))
        self.message=f'Match {self.search_at+1}/{len(hits)} · ^B j next · ^B N previous';self.dirty=True

    def send(self,data):
        if self.stop:return
        if self.pending_confirm:
            action,ident=self.pending_confirm;self.pending_confirm=None
            if data[:1].lower()==b'y':
                if action=='quit':self.end_session=True;self.stop=True
                elif ident is not None:self.request('close',tab=ident)
            self.dirty=True;return
        if self.prompt:
            # Decode across reads, and consume editing keys even when the host
            # batches a typed URL and Return into a single read.
            if data.startswith(b'\x1b['):return
            for value in data:
                if value in (27,3):self.prompt=None;self.prompt_decoder.reset();break
                if value in (13,10):
                    kind,text=self.prompt,self.prompt_value;self.prompt=None;self.prompt_decoder.reset()
                    if kind=='search':self.search_query=text;self.search_at=-1;self.find_next()
                    elif kind=='url':self.open_web(text)
                    elif kind=='font':self.settings['font_name']=text;self.apply_font()
                    break
                if value in (127,8):self.prompt_value=self.prompt_value[:-1];self.prompt_decoder.reset()
                elif value>=32 or value>=128:
                    self.prompt_value=clean_text(self.prompt_value+self.prompt_decoder.decode(bytes([value])),2048)
            self.dirty=True;return
        if self.overlay:
            if data in (b'\x1b',b'q'):self.overlay=None
            elif self.overlay=='settings':
                if data in (b'\x1b[B',b'j'):self.overlay_offset=min(len(THEMES)-1,self.overlay_offset+1)
                elif data in (b'\x1b[A',b'k'):self.overlay_offset=max(0,self.overlay_offset-1)
                elif data in (b'\r',b'\n'):self.set_theme(THEMES[self.overlay_offset].key)
                elif data==b'h':self.action(('hostfont',None))
                elif data==b'f':self.action(('font',None))
                elif data==b'a':self.apply_font()
                elif data in (b'+',b'-'):self.action(('fontsize',1 if data==b'+' else -1))
            self.dirty=True;return
        if self.tv_focus is not None and data in (b'\x1b',):
            self.tv_focus=None;self.dirty=True;return
        if not self.current:return
        if self.tv_focus is None and self.current.scroll:
            if data in (b'\x1b[5~',b'\x1b[A'):self.current.scroll=min(len(self.current.terminal.history),self.current.scroll+(self.rows if data==b'\x1b[5~' else 1));self.dirty=True;return
            if data in (b'\x1b[6~',b'\x1b[B'):self.current.scroll=max(0,self.current.scroll-(self.rows if data==b'\x1b[6~' else 1));self.dirty=True;return
            self.current.scroll=0
        self.child_input(data,tab=self.input_tab.id);self.dirty=True

    def apply_font(self):
        if not self.settings.get('allow_host_font'):
            self.message='Enable host font changes in config first';return
        self.queue_font(('apply',self.settings.get('font_name','Menlo-Regular'),self.settings.get('font_size',14)))

    def queue_font(self,request):
        # Apple events may wait for a permission prompt. Keep PTY reads and
        # redraws running; serialize operations and coalesce repeated +/- taps.
        if self.font_future is not None:
            self.font_queued=request
        else:
            if self.font_worker is None:self.font_worker=ThreadPoolExecutor(max_workers=1,thread_name_prefix='hardcore-font')
            action,*args=request;self.font_action=action
            fn=(lambda:self.font.apply(*args,allowed=True)) if action=='apply' else getattr(self.font,action)
            self.font_future=self.font_worker.submit(fn)
        self.message='Applying host font…' if request[0] in ('apply','resume') else 'Restoring host font…'
        self.dirty=True

    def poll_font(self):
        if self.font_future is None or not self.font_future.done():return
        action=self.font_action;future=self.font_future;self.font_future=None
        try:
            future.result()
            if action=='apply':self.persist()
            self.message='Original host font restored' if action=='restore' else 'Host font changed; original will be restored'
        except Exception as e:self.message=str(e)[:240]
        queued=self.font_queued;self.font_queued=None
        if queued and not self.stop:self.queue_font(queued)
        self.resize_pending=True;self.dirty=True

    def finish_font(self):
        # Exit/suspend must wait for an in-flight apply before restoration, so
        # a delayed Apple event cannot change the host after we have left it.
        self.font_queued=None
        if self.font_future is not None:
            try:self.font_future.result()
            except Exception as e:self.message=str(e)[:240]
            self.font_future=None
        if self.font_worker is not None:
            self.font_worker.shutdown(wait=True);self.font_worker=None

    def caf_toggle(self):
        if self.caf and self.caf.poll() is None:self.caf.terminate();self.caf.wait(timeout=2);self.caf=None
        elif sys.platform=='darwin':
            self.caf=subprocess.Popen(['/usr/bin/caffeinate','-di','-w',str(os.getpid())],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            self.caf_started=time.monotonic()
        else:self.message='CAF is available on macOS'
        self.dirty=True

    def action(self,action):
        op,value=action
        if op=='choose':self.choose(value)
        elif op=='tv':self.toggle_tv(value)
        elif op=='tv_input':self.tv_focus=value;self.preview=None
        elif op=='close':self.pending_confirm=('close',value)
        elif op=='new':self.request('new')
        elif op=='settings':self.overlay='settings'
        elif op=='theme':self.set_theme(value)
        elif op=='files':self.open_files()
        elif op=='web':self.prompt='url';self.prompt_value='https://'
        elif op=='caf':self.caf_toggle()
        elif op=='dismiss':self.overlay=None
        elif op=='hostfont':
            self.settings['allow_host_font']=not self.settings.get('allow_host_font',False)
            if not self.settings['allow_host_font']:
                self.queue_font(('restore',))
            self.persist()
        elif op=='font':self.prompt='font';self.prompt_value=self.settings.get('font_name','Menlo-Regular')
        elif op=='fontsize':self.settings['font_size']=max(6,min(48,self.settings.get('font_size',14)+value));self.apply_font()
        elif op=='applyfont':self.apply_font()
        self.dirty=True

    def mouse(self,button,x,y,down):
        if self.stop or not self.focused:return
        x-=1;y-=1
        hit=next((action for left,top,right,bottom,action in reversed(self.hits) if left<=x<right and top<=y<bottom),None)
        if button&32:
            target=hit[1] if hit and hit[0]=='choose' else None
            if self.preview!=target:self.preview=target;self.previous=[];self.dirty=True
            if hit or x>=self.cols:return
        if hit:
            if button==0:
                if down:self.pressed=hit
                else:
                    if self.pressed==hit:self.action(hit)
                    self.pressed=None
            elif down and button in (64,65):
                if self.overlay=='settings':self.overlay_offset=max(0,min(len(THEMES)-1,self.overlay_offset+(-1 if button==64 else 1)))
                else:self.menu_offset=max(0,min(max(0,len(self.tabs)-1),self.menu_offset+(-1 if button==64 else 1)))
                self.dirty=True
            return
        if self.pressed is not None:
            if not down:self.pressed=None
            return
        if self.overlay:return
        self.preview=None
        if not self.current or not(0<=x<self.cols and 0<=y<self.rows):return
        if button==0 and down:self.tv_focus=None;self.dirty=True
        term=self.current.terminal
        if not term.mouse:
            if down and button in (64,65):self.current.scroll=max(0,min(len(term.history),self.current.scroll+(3 if button==64 else -3)));self.dirty=True
            return
        if button&32 and (term.mouse==1000 or (term.mouse==1002 and button&3==3)):return
        if term.mouse_sgr:data=f'\x1b[<{button};{x+1};{y+1}{"M" if down else "m"}'.encode()
        elif x<223 and y<223:data=b'\x1b[M'+bytes((32+(button if down else 3),33+x,33+y))
        else:return
        self.child_input(data,tab=self.current.id)

    def focus(self,inside):
        if self.stop:return
        self.focused=inside
        if not inside:self.preview=None;self.pressed=None;self.previous=[];self.dirty=True
        elif self.current and not self.current.scroll:self.current.unread=False
        if self.input_tab and self.input_tab.terminal.focus:
            self.child_input(b'\x1b[I' if inside else b'\x1b[O',tab=self.input_tab.id)

    def put(self,grid,x,y,text,width=None,style=None,action=None):
        if not 0<=y<len(grid) or x<0:return
        width=min(width if width is not None else self.total_cols-x,self.total_cols-x)
        if width<=0:return
        cells=text_cells(text,width,style or Style(fg=self.theme.fg,bg=self.theme.bg))
        cells.extend([Cell(style=style or Style(fg=self.theme.fg,bg=self.theme.bg))]*(width-len(cells)))
        grid[y][x:x+width]=cells
        if action:self.hits.append((x,y,x+width,y+1,action))

    def box(self,grid,x,y,w,h,title):
        style=Style(fg=self.theme.fg,bg=self.theme.bg)
        for row in range(y,min(y+h,self.total_rows-1)):self.put(grid,x,row,'',w,style)
        self.put(grid,x,y,'╭'+('─'*(w-2))+'╮',w,style)
        self.put(grid,x,y+h-1,'╰'+('─'*(w-2))+'╯',w,style)
        for row in range(y+1,y+h-1):
            self.put(grid,x,row,'│',1,style);self.put(grid,x+w-1,row,'│',1,style)
        self.put(grid,x+2,y,title,w-5,style)
        self.put(grid,x+w-3,y,'−',1,style,('dismiss',None))

    def compose(self):
        base=Style(fg=self.theme.fg,bg=self.theme.bg)
        grid=[[Cell(style=base)]*self.total_cols for _ in range(self.total_rows)]
        self.hits=[];self.tv_cursor=None
        shown=self.tabs[self.preview] if self.preview is not None and self.preview<len(self.tabs) else self.current
        if shown:
            view=shown.terminal.visible(shown.scroll)
            for y,line in enumerate(view[:self.rows]):
                segment=list(line[:self.cols]);repair_line(segment)
                grid[y][:len(segment)]=segment
                if self.search_query and self.search_query.casefold() in ''.join(c.text for c in line if c.width).casefold():
                    grid[y][:self.cols]=[replace(c,style=replace(c.style,bg=self.theme.selection,bg_index=None)) for c in grid[y][:self.cols]]
        accent=Style(fg=self.theme.cursor,bg=self.theme.bg,flags=BOLD)
        dim=Style(fg=self.theme.palette[8],bg=self.theme.bg)
        visible_tvs=set()
        if self.menu_width:
            x=self.cols+1;w=self.menu_width
            self.put(grid,x,0,'⚙config',9,accent,('settings',None));self.put(grid,x+10,0,'+term',7,base,('new',None))
            self.put(grid,x+19,0,'files',7,base,('files',None))
            self.put(grid,x,1,'+www',7,base,('web',None));self.put(grid,x+9,1,'CAF' if self.caf and self.caf.poll() is None else 'caf',5,base,('caf',None))
            self.put(grid,x+15,1,'^B ? help',12,dim)
            available=max(1,self.rows-3);offset=min(self.menu_offset,max(0,len(self.tabs)-available))
            y=3
            for index,tab in enumerate(self.tabs[offset:offset+available],offset):
                style=accent if index==self.active else base
                mark='●' if index==self.active else '!' if tab.exited is not None else '·' if tab.unread else ' '
                self.put(grid,x,y,mark+self.label(tab),w-5,style,('choose',index))
                self.put(grid,x+w-5,y,'−' if tab.id in self.tvs else '□',2,style,('tv',index))
                self.put(grid,x+w-2,y,'×',2,style,('close',tab.id));y+=1
            for index,tab in enumerate(self.tabs):
                if tab.id not in self.tvs or y+5>=self.rows:continue
                visible_tvs.add(tab.id)
                h=min(9,self.rows-y-1);y+=1
                focused=tab.id==self.tv_focus
                strong=index in (self.active,self.preview) or focused
                edge='═' if strong else '─';side='║' if strong else '│'
                self.put(grid,x,y,('╔' if strong else '┌')+edge*(w-2)+('╗' if strong else '┐'),w,accent if strong else base)
                self.put(grid,x+2,y,self.label(tab),w-7,accent if strong else base,('choose',index))
                self.put(grid,x+w-5,y,'↗',2,base,('choose',index))
                self.put(grid,x+w-3,y,'−',2,base,('tv',index))
                lines=tab.terminal.visible(0);body=h-2
                # Follow the cursor in both directions. This leaves the input
                # being edited visible even in a narrow terminal-hosted TV.
                bottom=min(len(lines),max(body,tab.terminal.screen.y+1))
                start=max(0,bottom-body)
                left=max(0,tab.terminal.screen.x-(w-4)) if focused else 0
                for row in range(body):
                    self.put(grid,x,y+row+1,side+' '*(w-2)+side,w,base)
                    line=lines[start+row] if start+row<len(lines) else []
                    segment=list(line[left:left+w-2]);repair_line(segment)
                    grid[y+row+1][x+1:x+1+len(segment)]=segment
                    self.hits.append((x+1,y+row+1,x+w-1,y+row+2,('tv_input',tab.id)))
                self.put(grid,x,y+h-1,('╚' if strong else '└')+edge*(w-2)+('╝' if strong else '┘'),w,base)
                if focused and start<=tab.terminal.screen.y<start+body:
                    self.tv_cursor=(x+1+tab.terminal.screen.x-left,y+1+tab.terminal.screen.y-start)
                y+=h

        else:
            x=0
            for index,tab in enumerate(self.tabs):
                label=f' {index+1} {self.label(tab)} ';width=min(max(8,self.total_cols//max(1,len(self.tabs))),24)
                if x>=self.total_cols:break
                self.put(grid,x,self.rows,label,width,accent if index==self.active else base,('choose',index));x+=width
        if self.tv_focus not in visible_tvs:self.tv_focus=None
        status=f'{self.session} · ^B c new  D detach  s config  / search  ? help'
        if self.prefix:status='Ctrl-B: c new · n/p tabs · v TV · s config · F files · w web · D detach · R reload · q quit'
        if self.message:status=self.message
        if self.current and self.current.scroll:status=f'History ↑{self.current.scroll} · ^B ] live · '+status
        if self.preview is not None:status='Preview only — click to select · '+status
        if self.tv_focus is not None:status='Typing in TV · Esc returns to main tab · '+status
        hidden_tvs=self.tvs-visible_tvs
        if hidden_tvs:status='TV kept open; enlarge terminal or hide other TVs to view · '+status
        if self.current and self.current.exited is not None:status='Program exited; ^B c new / x close · '+status
        if self.menu_width or self.message or self.prefix or self.preview is not None or hidden_tvs or (self.current and self.current.exited is not None):self.put(grid,0,self.rows,status,self.total_cols,dim)
        if self.overlay=='help':
            lines=['Ctrl-B then:', 'c new tab    n/p previous/next    1–9 select','s config     m menu on/off       v mini TV','F files      w text web          / search','j next match N previous match    u/d history','] live       D detach           R reload UI','x close tab  q end session       ? help','Click a tab to select; hover previews without sending input.','Click TV content to type there; Esc returns; ↗ expands.','Closing the host leaves the session available to reattach.','Shift-drag uses host selection; Esc closes this panel.']
            w=min(72,self.total_cols);h=min(len(lines)+2,self.rows);x=(self.total_cols-w)//2;y=max(0,(self.rows-h)//2)
            self.box(grid,x,y,w,h,'Hardcore here')
            for i,line in enumerate(lines[:h-2],1):self.put(grid,x+2,y+i,line,w-4,base)
        elif self.overlay=='settings':
            w=min(54,self.total_cols);h=min(23,self.rows);x=(self.total_cols-w)//2;y=max(0,(self.rows-h)//2)
            self.box(grid,x,y,w,h,'⚙ config')
            cap=max(0,h-(8 if h>=12 else 2));start=min(self.overlay_offset,max(0,len(THEMES)-cap))
            for i,theme in enumerate(THEMES[start:start+cap],1):
                self.put(grid,x+2,y+i,('● ' if theme.key==self.theme.key else '  ')+theme.name,w-4,accent if theme.key==self.theme.key else base,('theme',theme.key))
            if h>=12:
                row=y+h-6
                self.put(grid,x+2,row,('[x]' if self.settings.get('allow_host_font') else '[ ]')+' Host font changes (h)',w-4,base,('hostfont',None))
                self.put(grid,x+2,row+1,'Font (f): '+self.settings.get('font_name','Menlo-Regular'),w-4,base,('font',None))
                self.put(grid,x+2,row+2,'−',3,base,('fontsize',-1));self.put(grid,x+6,row+2,str(self.settings.get('font_size',14))+' pt',8,base)
                self.put(grid,x+15,row+2,'+',3,base,('fontsize',1));self.put(grid,x+20,row+2,'Apply(a)',8,accent,('applyfont',None))
                self.put(grid,x+2,row+3,'Restores original font on exit/suspend.',w-4,dim)
                self.put(grid,x+2,row+4,self.font.reason if not self.font.available else 'Font control: this Terminal tab only',w-4,dim)
        if self.pending_confirm:self.put(grid,0,self.rows,'End all session processes? y/N' if self.pending_confirm[0]=='quit' else 'Close this tab and its programs? y/N',self.total_cols,accent)
        if self.prompt:self.put(grid,0,self.rows,self.prompt+': '+self.prompt_value+'▏',self.total_cols,accent)
        return grid

    def paint(self):
        grid=self.compose();out=['\x1b[?25l']
        for row,line in enumerate(grid):
            if row>=len(self.previous) or line!=self.previous[row]:out.append(f'\x1b[{row+1};1H'+render_line(line,theme=self.theme))
        self.previous=grid
        term=self.input_tab.terminal if self.input_tab else None
        if term:
            modes=(term.app_cursor,term.app_keypad,term.cursor_shape)
            if modes!=self.last_modes:
                out.append('\x1b[?1000l\x1b[?1002l\x1b[?1003h')
                out.append(f'\x1b[?1{"h" if term.app_cursor else "l"}'+ ('\x1b=' if term.app_keypad else '\x1b>')+f'\x1b[{term.cursor_shape} q')
                self.last_modes=modes
            if not self.overlay and not self.prompt and not self.pending_confirm and not (self.current.scroll if self.tv_focus is None else 0) and self.preview is None and term.cursor_visible:
                if self.tv_focus is not None:
                    if self.tv_cursor:out.append(f'\x1b[{self.tv_cursor[1]+1};{self.tv_cursor[0]+1}H\x1b[?25h')
                else:
                    out.append(f'\x1b[{term.screen.y+1};{term.screen.x+1}H\x1b[?25h')
        self.host.write(''.join(out));self.dirty=False;self.last_paint=time.monotonic()

    def run(self):
        try:
            for sig in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP):self.old_signals[sig]=signal.signal(sig,lambda *_:setattr(self,'stop',True))
            self.old_signals[signal.SIGWINCH]=signal.signal(signal.SIGWINCH,lambda *_:setattr(self,'resize_pending',True))
            self.old_signals[signal.SIGTSTP]=signal.signal(signal.SIGTSTP,lambda *_:setattr(self,'suspend',True))
            self.host.enter()
            self.request('theme',palette=[list(c) for c in self.theme.palette],foreground=list(self.theme.fg),background=list(self.theme.bg))
            while not self.stop:
                if self.suspend:
                    self.finish_font()
                    try:self.font.suspend()
                    except HostFontError as e:self.message=str(e)
                    self.host.restore();os.kill(os.getpid(),signal.SIGSTOP)
                    self.suspend=False;self.host.enter()
                    self.queue_font(('resume',))
                    self.resize_pending=True;self.previous=[];self.last_modes=None
                self.poll_font()
                if self.resize_pending:self.resize()
                self.poll();self.decoder.flush_escape()
                if self.stop:break
                self.client.flush()
                if self.dirty and time.monotonic()-self.last_paint>=1/60:self.paint()
                readers=[self.client.fileno()]
                if self.input_ready():readers.append(self.host.input_fd)
                ready,_,_=select.select(readers,[],[],.03)
                if self.client.fileno() in ready:self.poll()
                if self.host.input_fd in ready:
                    data=os.read(self.host.input_fd,65536)
                    if not data:break
                    self.decoder.feed(data,lambda:bool(not self.prompt and self.input_tab and self.input_tab.terminal.bracketed_paste))
            if not self.client.closed:
                if self.end_session:self.request('stop')
                else:self.request('detach')
                deadline=time.monotonic()+1
                while self.client.pending and time.monotonic()<deadline:
                    self.client.flush();select.select([], [self.client.fileno()], [], .01)
        finally:
            self.host.restore()
            self.finish_font()
            try:self.font.restore()
            except HostFontError as e:print('Hardcore: '+str(e),file=sys.stderr)
            if self.caf and self.caf.poll() is None:self.caf.terminate()
            self.client.close()
            for sig,handler in self.old_signals.items():signal.signal(sig,handler)
        if self.reload:
            os.execv(sys.executable,[sys.executable,'-B',str(Path(__file__).with_name('here.py')),'--attach',self.session])
        if not self.end_session:print(f'Detached. Resume with: hardcore here --attach {self.session}')
        return 0
