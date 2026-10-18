#!/usr/bin/env python3
"""Check native represented-folder titles using real shell cwd changes."""
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_chrome_mac import NativeApp, ROOT, wait_for, test_native_titlebar


def run():
    artifacts=ROOT/'build/folder-titlebar'
    with tempfile.TemporaryDirectory(prefix='hc-folder-titlebar-') as root:
        root=Path(root)
        alpha=root/'Folder Alpha'; beta=root/'café Beta'
        alpha.mkdir(); beta.mkdir()
        with NativeApp(root,1,artifacts) as app:
            def folder(path):
                def ready():
                    t=app.chrome()['titlebar']
                    return t if bool(t['folder']) and Path(t['folder']).samefile(path) and t['title']==path.name else None
                t=wait_for(ready,description=f'native title for {path.name}')
                assert t['proxy_visible'] and t['buttons_visible'] and not t['title_hidden'],t
                assert not t['transparent'] and not t['full_size_content'],t
                return t
            folder(root)
            test_native_titlebar(app)
            app.send('cmd newtab')
            app.send('type cd '+shlex.quote(str(alpha))+r'\n')
            folder(alpha)
            app.send('type cd '+shlex.quote(str(beta))+r'\n')
            folder(beta)
            app.send('cmd tab 0');folder(root)
            app.send('cmd tab 1');folder(beta)
            c=app.chrome()
            app.hover(c['dock']['x']+8,c['tab_rows'][0]/c['cell']['height'])
            folder(beta)  # A tab preview must not change the represented folder.
            app.hover(-1,-1)
            app.snap('content')
            identity=app.output('winid','txt').read_text().strip()
            subprocess.run(['/usr/sbin/screencapture','-x','-o','-l',identity,str(artifacts/'window.png')],check=True)
    print('PASS native folder proxy: startup, cd, spaces/Unicode, tab switching, preview isolation, visible native controls')

if __name__=='__main__':run()
