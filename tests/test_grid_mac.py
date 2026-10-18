#!/usr/bin/env python3
"""Exercise tiled PTYs and gestures in an isolated native app."""
from pathlib import Path
import tempfile
from test_chrome_mac import NativeApp, ROOT, wait_for
from test_tab_preview_extended_mac import browser_command, setup_workers, worker_data, select_tab


def click_pane(app,c,col,row):
    x=int((col+.5)*c["cell"]["width"]); y=int((row+.5)*c["cell"]["height"])
    app.send(f"cmd pointer down {x},{y}")
    app.send(f"cmd pointer up {x},{y}")

def run():
    artifacts=ROOT/'build/grid-native'
    with tempfile.TemporaryDirectory(prefix='hardcore-grid-') as root:
        with NativeApp(Path(root),2,artifacts) as app:
            app.quiet_tabs(9)
            app.feed([''])
            c=app.chrome();click_pane(app,c,*app.header_point(c,'grid'))
            c=app.chrome(); g=c['grid']
            assert g['enabled'] and (g['cols'],g['rows'])==(3,3),g
            panes=g['panes']; state=app.state()
            assert [p['id'] for p in panes]==[t['id'] for t in state['tabs']]
            # (a screen takes its size when its program answers, or 100 ms on)
            wait_for(lambda:all((t['cols'],t['rows'])==(p['width'],p['height']-1) for p,t in zip(panes,app.state()['tabs'])),
                     description='every pane screen at its pane size')
            for i in range(9):
                app.send(f'cmd tab {i}')
                app.feed([f'WINDOW {i+1}', 'abcdefghijk 0123456789', 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'])
            app.snap('nine-panes')
            p=panes[1]
            click_pane(app,c,p['x']+2,p['y']+2)
            assert app.state()['active']==1
            cw,ch=c['cell']['width'],c['cell']['height']
            x=p['x']*cw; y=(p['y']+3)*ch
            app.send(f'cmd pointer down {x},{y}')
            app.send(f'cmd pointer drag {x+4*cw},{y}')
            app.send(f'cmd pointer up {x+4*cw},{y}')
            c=app.chrome()
            assert c['grid']['panes'][0]['width']==panes[0]['width']+4,c['grid']
            p=c['grid']['panes'][8]
            click_pane(app,c,p['x']+p['width']-10,p['y'])
            c=app.chrome();assert len(c['grid']['panes'])==8 and c['grid']['extra']==1,c['grid']
            assert len(app.state()['tabs'])==9
            click_pane(app,c,c['dock']['x']+22,c['tab_rows'][8]/ch)
            c=app.chrome();assert len(c['grid']['panes'])==9
            p=c['grid']['panes'][8];click_pane(app,c,p['x']+p['width']-10,p['y'])
            c=app.chrome()
            first=c['grid']['panes'][0]['id']
            app.header_click('expand')
            c=app.chrome();assert c['grid']['panes'][0]['id']!=first
            before=c['grid']['panes']
            app.header_click('settings')
            c=app.chrome();assert c['settings_open'] and c['grid']['panes']==before
            app.snap('expanded-settings')
            app.header_click('music')
            c=app.chrome();assert c['grid']['panes']==before
            app.snap('expanded-music')
            app.send('cmd resize 1001,280')
            c=app.chrome()
            assert c['music']['placed'] and not app.tv(c,'settings')['placed'],c['music']
            row=c['dock']['y']+c['dock']['height']+1
            click_pane(app,c,2,row)
            c=app.chrome();assert app.tv(c,'settings')['placed'] and not c['music']['placed']
            app.snap('compact-utility-selectors')
            app.send('cmd resize 1001,591')
            app.header_click('grid')
            assert not app.chrome()['grid']['enabled']
            wait_for(lambda:len({(t['cols'],t['rows']) for t in app.state()['tabs']})==1,description='every tab at the window size')
            app.snap('normal-music')
            saved=app.chrome()['music']['rect']
            app.send('cmd grid');app.chrome()
            app.send('cmd grid');c=app.chrome()
            assert c['music']['rect']==saved,(saved,c['music']['rect'])
    with tempfile.TemporaryDirectory(prefix='hardcore-grid-input-') as root:
        with NativeApp(Path(root),1,artifacts) as app:
            main,source,workers,state=setup_workers(app)
            app.send('cmd grid');c=app.chrome()
            identity,path=workers[0]
            p=next(p for p in c['grid']['panes'] if p['id']==identity)
            click_pane(app,c,p['x']+2,p['y']+2)
            app.send('key 0 0 z z')
            wait_for(lambda: worker_data(path)['input']=='7a',description='grid keyboard input to chosen PTY')
            assert worker_data(workers[1][1])['input']==''
            wait_for(lambda: worker_data(path)['size']==[p['height']-1,p['width']],description='actual grid PTY winsize')
            html=app.root/'grid-browser.html'
            html.write_text('<!doctype html><meta charset=utf-8><title>Grid browser</title>'
                '<p><button onclick="this.textContent=\'Clicked\'">Tap</button></p>'
                '<p><textarea>EDIT_ME</textarea></p>'+
                ''.join(f'<p>Browser row {i}</p>' for i in range(50)))
            identity=browser_command(app,'open',str(html),'--mode','native')['id']
            def browser_line(text):
                c=app.chrome()
                for p in c['grid']['panes']:
                    if p['id']==identity:
                        for y,line in enumerate(p['browser']):
                            if text in line:return c,p,y,line.index(text)
            c,p,y,x=wait_for(lambda:browser_line('Tap'),description='grid browser projection')
            click_pane(app,c,p['x']+x,p['y']+1+y)
            wait_for(lambda:browser_line('Clicked'),description='grid browser button action')
            frame=app.snap('browser-grid')
            # A focused native browser must not make the tiled frame transparent.
            alpha={frame.data[frame.offset+3+(y*frame.width+x)*4] for x in range(0,frame.width,29) for y in range(0,frame.height,29)}
            assert alpha=={255},alpha
            panes=app.chrome()['grid']['panes']
            # Child browser labels start with a non-ASCII arrow; each pane keeps its own title.
            titles=[p['title'] for p in panes]
            assert len(set(titles))==len(titles),titles
            assert next(p for p in panes if p['id']==identity)['title'].split(' ',1)[1].startswith('\u21b3 Grid browser'),titles
            c,p,y,x=wait_for(lambda:browser_line('EDIT_ME'),description='grid browser editable control')
            click_pane(app,c,p['x']+x,p['y']+1+y)
            wait_for(lambda:not app.chrome()['grid']['enabled'],description='browser form expands into normal view')
    print('Native grid: layout, input routing, actual PTY sizing, divider drag, hide/show, expansion, utilities, browser actions and normal restore passed')

if __name__=='__main__':run()
