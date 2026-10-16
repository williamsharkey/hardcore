from pathlib import Path
import base64
from playwright.sync_api import sync_playwright
D=Path(__file__).resolve().parent
names=['Sidebar · inline tab pages','Sidebar · font choices with page buttons','Sidebar · independent lists','Right · borrow the blank row','Right · use the short final line','Right · mixed literal content','Right · no fit, dim beneath text','Right · join a multiline paragraph','Left · shifted prose','Left · full width above and below','Hover · whole right menu foreground','Hover · whole left menu foreground','Pagination · second tab page','Compact · page capacity adapts','Original · before reflow','Reflowed · same heading position']
files=[f'study-{i:02}.png' for i in range(1,11)]+['hover-menu.png','hover-menu-left.png','page-two.png','compact.png','before-reflow.png','after-reflow.png']
html='''<!doctype html><meta charset="utf-8"><title>Hardcore — left / right underlays and inline pages</title><style>*{box-sizing:border-box}body{margin:0;padding:28px;background:#111412;color:#d8e0d4;font:14px/1.5 monospace}h1{font-size:24px;font-weight:400;margin:5px 0}p{color:#96a28e;margin:0 0 22px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:24px 18px}h2{font-size:13px;font-weight:400;color:#e9b66e;margin:0 0 8px}img{display:block;width:100%;height:400px;object-fit:contain;object-position:left top;background:#101411;border:1px solid #46523f;image-rendering:pixelated}a{color:inherit;text-decoration:none}.label{font-size:10px;color:#84917d}</style><h1>Hardcore / two sides + compact pagination</h1><p>Ten studies, plus whole-menu hover, inline page controls, compact sizing and before/after. Open index.html to interact.</p><div class="grid">'''
for n,(title,file) in enumerate(zip(names,files),1):
 data=base64.b64encode((D/file).read_bytes()).decode()
 html+=f'<section><h2>{n:02} / {title}</h2><a href="{file}"><img src="data:image/png;base64,{data}"></a></section>'
html+='</div><p style="margin-top:22px">One row per tab. Fixed menu width and position. No reserved column. Terminal cells cover either dim underlay; hover raises the whole menu.<br>Left-dock reflow indents only intersecting rows. Pagination buttons share actual selection rows. Later headings stay on their original rows.</p>'
(D/'gallery.html').write_text(html)
with sync_playwright() as p:
 b=p.chromium.launch();page=b.new_page(viewport={'width':1600,'height':1100},device_scale_factor=1);page.goto((D/'gallery.html').as_uri());page.screenshot(path=str(D/'comparison.png'),full_page=True);b.close()
