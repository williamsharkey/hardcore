"""Render study 03 and test whole-menu hover, local left inset and inline paging."""
from pathlib import Path
from playwright.sync_api import sync_playwright
import json
D=Path(__file__).resolve().parent
with sync_playwright() as p:
 browser=p.chromium.launch()
 page=browser.new_page(viewport={'width':1440,'height':1200},device_scale_factor=1)
 errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
 page.goto((D/'index.html').as_uri())
 stats=[]
 def flush():page.evaluate('ctx.getImageData(0,0,canvas.width,canvas.height)')
 def snap(name):flush();page.locator('#screen').screenshot(path=str(D/name))
 for i in range(10):
  page.evaluate(f'setPreset({i})');snap(f'study-{i+1:02}.png');stats.append(page.evaluate('mockupState'))
 assert not errors,errors
 assert stats[0]['tabRows']==6 and stats[0]['rows']==8,stats[0]
 assert stats[3]['changes']==1 and stats[3]['blanks']==1,stats[3]
 assert stats[4]['changes']>=1 and stats[4]['blanks']==0,stats[4]
 assert stats[5]['changes']>=1 and stats[5]['overlapAfter']>0,stats[5]
 assert stats[6]['changes']==0 and stats[6]['terminalCols']==128,stats[6]
 assert stats[7]['changes']==1,stats[7]
 assert stats[8]['side']=='left' and stats[8]['changes']>0,stats[8]
 assert stats[9]['dock']==5 and stats[9]['overlapAfter']==0,stats[9]
 result=page.evaluate('lastResult.lines');original=page.evaluate('lastOriginal')
 assert result[0]==original[0]
 assert any(len(s)-len(s.lstrip())==2 and len(s)>100 for s in result[2:5])
 assert all(s.startswith(' '*30) for s in result[5:13])
 assert any(len(s)-len(s.lstrip())==2 for s in result[13:15])
 heading=original.index('456');assert result[heading]=='456'
 page.locator('#diff').evaluate('(el)=>el.open=true');flush()
 page.screenshot(path=str(D/'overview.png'),full_page=True)
 page.locator('#diff').evaluate('(el)=>el.open=false')
 # Keep the exact original fox example and before/after toggle coverage.
 page.evaluate('setPreset(3)')
 result=page.evaluate('lastResult.lines')
 assert result[:7]==['123','  the quick brown','  fox jumps over','  the sleeping','  dog','456','  hello world'],result
 after=page.locator('#screen').screenshot();page.click('#before');snap('before-reflow.png')
 assert page.locator('#screen').screenshot()!=after
 page.click('#after');snap('after-reflow.png')
 # Hover any part of dense right/left underlay raises ALL menu rows, no text outside.
 for layout in ['underlay','underlay-left']:
  page.evaluate('setPreset(6)');page.select_option('#layout',layout);page.mouse.move(0,0);flush()
  before=page.evaluate('Array.from(ctx.getImageData(0,0,canvas.width,canvas.height).data)')
  b=page.locator('#screen').bounding_box();dock=page.evaluate('lastDock')
  page.mouse.move(b['x']+b['width']*(dock['x']+10)/128,b['y']+b['height']*1.5/36)
  assert page.locator('#hoverState').inner_text()=='Foreground: entire menu'
  after=page.evaluate('Array.from(ctx.getImageData(0,0,canvas.width,canvas.height).data)')
  changed={i//(1024*4) for i,(a,z) in enumerate(zip(before,after)) if a!=z}
  assert 0 in changed and max(changed)==dock['height']*16-1,changed
  assert all((i//4)%1024>=dock['x']*8 and (i//4)%1024<(dock['x']+dock['width'])*8 for i,(a,z) in enumerate(zip(before,after)) if a!=z)
  snap('hover-menu.png' if layout=='underlay' else 'hover-menu-left.png')
  if layout=='underlay':snap('hover-tab.png') # replace prior exported state, no stale one-row image
 page.mouse.move(0,0)
 # Page arrows are separate from an actual selection target, with no nav-only row.
 page.evaluate('setPreset(1)')
 assert page.evaluate("lastRows.every(r=>!['tabnav','setnav','selection'].includes(r.kind))")
 def click_pager(kind,part):
  coord=page.evaluate('''([kind,part])=>{const h=hit.find(h=>h.row.pager?.kind===kind),counter=`${h.row.pager.page}/${h.row.pager.pages}`,next=W-1-counter.length-4;return {x:h.x+(part==='back'?2:part==='next'?next+1:8),y:h.y,C,R};}''',[kind,part])
  b=page.locator('#screen').bounding_box();page.mouse.click(b['x']+b['width']*(coord['x']+.5)/coord['C'],b['y']+b['height']*(coord['y']+.5)/coord['R'])
 click_pager('settings','next');assert page.evaluate('state.setOffset')==3
 assert page.evaluate('state.font')==0
 click_pager('settings','middle');assert page.evaluate('state.font')==3
 click_pager('settings','next');assert page.evaluate('state.setOffset')==6
 click_pager('settings','back');assert page.evaluate('state.setOffset')==3 # final page has only two rows
 click_pager('settings','back');assert page.evaluate('state.setOffset')==0
 click_pager('settings','back');assert page.evaluate('state.setOffset')==0
 click_pager('tabs','next');assert page.evaluate('state.tabOffset')==4
 assert page.evaluate('state.setOffset')==0
 snap('page-two.png')
 # Compact viewport changes page capacity and re-aligns offsets to valid pages.
 page.select_option('#height','16');page.select_option('#layout','underlay-left')
 click_pager('tabs','next');assert page.evaluate('state.tabOffset')==4
 page.locator('#dock').fill('4');page.locator('#dock').dispatch_event('input')
 assert page.evaluate('mockupState.rows+mockupState.dock')<=16
 assert page.evaluate('state.tabOffset')==3
 snap('compact.png')
 page.uncheck('#reflow');assert page.evaluate('mockupState.changes')==0
 assert page.evaluate('mockupState.dock')==4
 # Saving a case records evidence, not a fabricated expected result.
 page.locator('#custom').evaluate('(el)=>el.open=true')
 with page.expect_download() as event:page.click('#saveCase')
 data=json.loads(Path(event.value.path()).read_text())
 assert data['schema']=='hardcore-reflow-case-1' and data['expected'] is None
 assert data['dock']['side']=='left' and data['lines']==page.evaluate('lastOriginal')
 assert not errors,errors
 (D/'render-checks.json').write_text(json.dumps(stats,indent=2)+'\n')
 browser.close()
 print('PASS: 10 studies, full-menu hover on both sides, full-width/left-inset/full-width paragraph, fixed headings, inline page buttons vs selection, last-page back, compact page alignment, case export.')
