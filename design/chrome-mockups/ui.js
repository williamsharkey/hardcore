/* Review-only canvas: terminal cells, chrome and hit targets share one grid. */
const $ = id => document.getElementById(id);
const canvas = $('screen'), ctx = canvas.getContext('2d', {willReadFrequently:true});
const captures = JSON.parse($('data').textContent), glyphs = JSON.parse($('glyphs').textContent);
const cw=8, ch=16, W=28;
let C=128, R=36;
const colors={bg:'#101411',text:'#c5cdc2',border:'#84967a',active:'#efc184',chrome:'#172019',hover:'#2d382a',marker:'#7b8d71'};
const atlasCache={}, glyphIndex=Object.fromEntries(Object.keys(glyphs).map((k,i)=>[k,i]));
function atlas(color){
  if(atlasCache[color]) return atlasCache[color];
  const a=document.createElement('canvas'); a.width=128; a.height=Math.ceil(Object.keys(glyphs).length/16)*16;
  const g=a.getContext('2d');g.fillStyle=color;
  Object.keys(glyphs).forEach((k,i)=>glyphs[k].forEach((bits,y)=>{for(let p=0;p<8;p++)if(bits&(128>>p))g.fillRect(i%16*8+p,Math.floor(i/16)*16+y,1,1)}));
  return atlasCache[color]=a;
}
function glyph(c,x,y,color){const i=glyphIndex[c.codePointAt(0)]??glyphIndex[63];ctx.drawImage(atlas(color),i%16*8,Math.floor(i/16)*16,8,16,x*cw,y*ch,8,16);}
function text(s,x,y,color=colors.text,max=C-x,opaque=false){Array.from(s).slice(0,max).forEach((c,i)=>{if(c.trim()){if(opaque)fill(x+i,y,1,1,colors.bg);glyph(c,x+i,y,color);}});}
function fill(x,y,w,h,color){ctx.fillStyle=color;ctx.fillRect(x*cw,y*ch,w*cw,h*ch);}
function escapeHTML(s){return s.replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;');}
const tabs=['hardcore:codex','hardcore:claude','slurp:grok','design:agy','build:zsh','server:python','notes:vim','docs:zsh','tests:zsh','fonts:zsh','release:zsh','scratch:zsh'];
const fonts=['Unifont       8×16','Terminus      8×16','Tamzen        8×15','Fixed         7×13','Sony          8×16','ETL          12×24','Terminus     14×28','Unifont      16×32'];
const themes=['Hardcore       ● ○','Dracula        ● ○','Monokai        ● ○','Nord           ● ○','Gruvbox        ● ○','Solarized      ● ○','One            ● ○','Catppuccin     ● ○','Tokyo Night    ● ○','Rose Pine      ● ○'];
let state={layout:'underlay',settings:false,overflow:'pages',border:'edge',capture:'reflow-blank',reflow:true,markers:true,dock:0,opacity:.35,theme:false,tabOffset:0,setOffset:0,hover:null,original:false,preset:3,active:0,font:0,scheme:0};
let hit=[],lastRows=[],lastResult=null,lastOriginal=[],lastDock=null,drag=null;
const presets=[
  {name:'Sidebar / one row',sub:'One-row tabs with inline pages',layout:'sidebar',settings:false,capture:'reflow-work',title:'A frame without a row tax',note:'Every tab occupies exactly one character row. Thin rules are drawn in the top and bottom bitmap margin, with the label on its normal baseline. The alternative draws box glyphs only at the ends.'},
  {name:'Sidebar / settings',sub:'Selection rows carry page buttons',layout:'sidebar',settings:true,capture:'reflow-work',title:'One compact control surface',note:'Settings, Font and Theme form one block. Tab and setting lists paginate independently; their first real item carries the navigation buttons. Selected font/theme and the active terminal stay identifiable.'},
  {name:'Sidebar / pages',sub:'Independent, explicit pages',layout:'sidebar',settings:true,overflow:'pages',capture:'reflow-work',title:'Predictable page-sized lists',note:'Separate previous and next buttons sit beside actual selectable entries. Clicking an arrow pages the list; clicking its middle selects that item. There is no page-only navigation row.'},
  {name:'Reflow / blank row',sub:'Your fox example, inspectable',capture:'reflow-blank',columns:45,rows:16,title:'Borrow a following blank row',note:'The three-line paragraph becomes four shorter lines, consuming the blank row. The numeric header below it remains on exactly the same row. Compare Original and Reflowed.'},
  {name:'Reflow / tail space',sub:'No blank line required',capture:'reflow-tail',title:'A short final line can absorb words',note:'This paragraph fits around the fixed menu using only its existing three rows. No blank separator is needed and the next heading stays in place.'},
  {name:'Underlay / mixed',sub:'Prose reflows; code stays intact',settings:true,capture:'reflow-work',title:'Local decisions, not whole-page wrapping',note:'Same-indent prose may reflow. Code and tables remain exact; where they intersect the menu, terminal cells cover it. The menu keeps its width and location.'},
  {name:'Underlay / no fit',sub:'Hover reveals the entire menu',settings:true,capture:'reflow-blocked',title:'An honest fallback when nothing fits',note:'No words are cut or displaced. The full menu stays underneath at reduced opacity. Hover anywhere inside the fixed menu to bring the entire menu forward. There is no protected rightmost column.'},
  {name:'Reflow / joined lines',sub:'Your multiline paragraph',capture:'reflow-joined',columns:50,rows:16,title:'Detect and rewrap a whole paragraph',note:'The repeated sentence and short final line form one same-indent paragraph. Rewrap at whole words, preserving the space between over and the; the following heading never moves.'},
  {name:'Left underlay / prose',sub:'Rewrap, then shift beside the menu',layout:'underlay-left',settings:true,capture:'reflow-tail',title:'Make room on the left',note:'Eligible paragraph rows beside the left menu shift right by its full width, preserving the original indentation. Text outside the menu’s vertical span gets the full width again. Unsafe content stays in place; the menu remains underneath it.'},
  {name:'Left underlay / middle',sub:'Full width above and below',layout:'underlay-left',dock:5,capture:'reflow-left-band',title:'A local inset, not a page-wide margin',note:'This paragraph spans above, beside and below the menu. Only rows intersecting it have a 28-column inset. The following section stays on its original row. Drag the grip or change Dock row to inspect the boundary.'}
];
function resize(){canvas.width=C*cw;canvas.height=R*ch;canvas.style.width=(C<80?C*cw*2:C*cw)+'px';}
function sync(){
  for(const k of ['layout','border','capture'])$(k).value=state[k];
  for(const k of ['settings','reflow','markers'])$(k).checked=state[k];
  $('columns').value=C;$('height').value=R;
  $('dock').max=Math.max(0,R-(state.settings?12:6));$('dock').value=state.dock;$('dockValue').textContent=state.dock;
  $('opacity').value=Math.round(state.opacity*100);$('opacityValue').textContent=Math.round(state.opacity*100)+'%';
  $('before').classList.toggle('primary',state.original);$('after').classList.toggle('primary',!state.original);
  document.querySelectorAll('.presets button').forEach((b,i)=>b.classList.toggle('active',state.preset===i));
}
function preset(i){
  const p=presets[i];C=p.columns||128;R=p.rows||36;
  Object.assign(state,{preset:i,layout:p.layout||'underlay',settings:!!p.settings,overflow:'pages',capture:p.capture,reflow:true,dock:p.dock||0,tabOffset:0,setOffset:0,hover:null,original:false,active:0,theme:false});
  $('studyTitle').textContent=p.title;$('studyNote').textContent=p.note;resize();sync();render();
}
function chrome(){
  const under=state.layout!=='sidebar',avail=R-(under?state.dock:0),list=state.theme?themes:fonts;
  const tabCount=state.settings?Math.max(1,Math.min(4,Math.floor((avail-5)/2))):Math.min(6,avail-2);
  const settingCount=state.settings?Math.max(1,Math.min(3,avail-tabCount-5)):0;
  const clampOffset=(offset,len,n)=>Math.max(0,Math.min(Math.floor(offset/n)*n,Math.floor((len-1)/n)*n));
  state.tabOffset=clampOffset(state.tabOffset,tabs.length,tabCount);state.setOffset=clampOffset(state.setOffset,list.length,settingCount||1);
  let rows=[{kind:'grip',group:'grip',label:'⠿⠿ hardcore ⠿⠿'}];
  for(let i=state.tabOffset;i<Math.min(tabs.length,state.tabOffset+tabCount);i++)rows.push({kind:'tab',group:'tab'+i,label:(i===state.active?'● ':'  ')+tabs[i],index:i,active:i===state.active,
    pager:i===state.tabOffset&&tabs.length>tabCount?{kind:'tabs',size:tabCount,page:Math.floor(state.tabOffset/tabCount)+1,pages:Math.ceil(tabs.length/tabCount)}:null});
  rows.push({kind:'settingsHeader',group:'settings',label:state.settings?'Settings −':'Settings +'});
  if(state.settings){
    rows.push({kind:'settingsTabs',group:'settings',label:state.theme?' Font │● Theme':'● Font│  Theme'});
    for(let i=state.setOffset;i<Math.min(list.length,state.setOffset+settingCount);i++)rows.push({kind:'settingItem',group:'settings',label:(i===(state.theme?state.scheme:state.font)?'● ':'  ')+list[i],item:i,
      pager:i===state.setOffset&&list.length>settingCount?{kind:'settings',size:settingCount,page:Math.floor(state.setOffset/settingCount)+1,pages:Math.ceil(list.length/settingCount)}:null});
    rows.push({kind:'reflowToggle',group:'settings',label:`[${state.reflow?'x':' '}] Local reflow`});
    rows.push({kind:'activeInfo',group:'settings',label:'active: '+tabs[state.active]});
  }
  return rows;
}
function drawFrame(x,y,color){
  const px=x*cw,py=y*ch,w=W*cw;ctx.fillStyle=color;
  if(state.border==='edge'){
    // Font ink begins below these edge strokes. No horizontal mid-cell glyph.
    ctx.fillRect(px+1,py,w-2,1);ctx.fillRect(px+1,py+ch-1,w-2,1);
    ctx.fillRect(px,py+1,1,ch-2);ctx.fillRect(px+w-1,py+1,1,ch-2);
  }else{glyph('│',x,y,color);glyph('│',x+W-1,y,color);}
}
function drawRow(row,x,y,foreground,under){
  const fg=row.active?colors.active:colors.border;
  ctx.globalAlpha=under&&!foreground?state.opacity:1;
  if(!under||foreground)fill(x,y,W,1,foreground?colors.hover:colors.chrome);
  drawFrame(x,y,fg);
  if(row.pager){
    const counter=`${row.pager.page}/${row.pager.pages}`,counterX=W-1-counter.length,nextX=counterX-4,centerX=5,centerWidth=nextX-centerX;
    let label=row.label.trim().replace(/ +/g,' ');if(label.length>centerWidth)label=label.slice(0,centerWidth-1)+'…';
    const disabled='#4b5748';
    text('[<-]',x+1,y,row.pager.page>1?fg:disabled,4);
    text(label,x+centerX,y,fg,centerWidth);
    text('[->]',x+nextX,y,row.pager.page<row.pager.pages?fg:disabled,4);
    text(counter,x+counterX,y,fg,counter.length);
  }else{
    let label=row.label;if(label.length>W-2)label=label.slice(0,W-3)+'…';
    text(label,x+1,y,fg,W-2);
  }
  if(row.kind==='settingsTabs'){
    const split=x+7;ctx.fillStyle=colors.border;ctx.fillRect(split*cw,y*ch+1,1,ch-2);
  }
  ctx.globalAlpha=1;
}
function drawMarker(marker){
  if(marker.placed===false||marker.column==null||marker.column<0)return;
  const x=marker.column*cw,y=marker.row*ch;ctx.fillStyle=colors.marker;
  // One custom cell: a faint arrow into a stem on its rightmost pixel (->|).
  ctx.fillRect(x+7,y+3,1,10);ctx.fillRect(x+1,y+8,5,1);
  ctx.fillRect(x+4,y+6,1,1);ctx.fillRect(x+5,y+7,1,1);ctx.fillRect(x+5,y+9,1,1);ctx.fillRect(x+4,y+10,1,1);
}
function overlap(lines,dock){let n=0;for(let r=dock.y;r<Math.min(lines.length,dock.y+dock.height);r++)for(const c of Array.from(lines[r]).slice(dock.x,dock.x+dock.width))if(c.trim())n++;return n;}
function render(){
  const cap=captures.find(c=>c.id===state.capture)||captures[0];
  const original=Array.from({length:R},(_,r)=>cap.lines[r]||'');
  const under=state.layout!=='sidebar';state.dock=Math.min(state.dock,Math.max(0,R-(state.settings?12:6)));
  const rows=chrome(),x=state.layout==='underlay'?C-W:0,y=under?state.dock:0,dock={x,y,width:W,height:rows.length,side:state.layout==='underlay-left'?'left':'right'};
  const result=HardcoreReflow.reflow(original,{columns:C,dock,enabled:under&&state.reflow,maxBlankRows:3});
  lastRows=rows;lastResult=result;lastOriginal=original;lastDock=dock;
  const lines=state.original?original:result.lines,termX=under?0:W,termWidth=under?C:C-W;
  fill(0,0,C,R,colors.bg);hit=[];
  rows.forEach((row,i)=>{drawRow(row,x,y+i,false,under);hit.push({x,y:y+i,w:W,group:row.group,kind:row.kind,row});});
  // Terminal cells have priority, including their background: never mix glyphs.
  for(let r=0;r<R;r++)text(lines[r],termX,r,colors.text,termWidth,true);
  if(state.markers&&!state.original&&under)result.markers.forEach(drawMarker);
  // Hover anywhere in either underlay raises its entire fixed menu.
  if(under&&state.hover)rows.forEach((row,i)=>drawRow(row,x,y+i,true,true));
  let zrow=rows.findIndex(r=>r.group===state.hover);if(zrow<0)zrow=rows.findIndex(r=>r.kind==='tab');
  const z=$('zoom'),zc=z.getContext('2d');zc.imageSmoothingEnabled=false;zc.fillStyle=colors.bg;zc.fillRect(0,0,z.width,z.height);
  // Inspect an isolated row so terminal occlusion does not hide its border.
  const saved=ctx.getImageData(0,0,canvas.width,canvas.height);drawRow(rows[zrow],x,y+zrow,true,under);
  zc.drawImage(canvas,x*cw,(y+zrow)*ch,W*cw,ch,0,0,W*cw*3,ch*3);ctx.putImageData(saved,0,0);
  const blanks=result.changes.reduce((s,c)=>s+(c.consumedBlankRows||0),0),before=under?overlap(original,dock):0,after=under?overlap(result.lines,dock):0;
  $('source').textContent=cap.source;
  $('gridLabel').textContent=`${termWidth} terminal cols × ${R} rows · ${under?'no reserved column':'28-col sidebar'}`;
  $('origin').textContent=`0,0 · drawable top-left · 8×16 grid${C<80?' · 2× display':''}`;
  $('changedCount').textContent=result.changes.length;$('blankCount').textContent=blanks;$('overlap').textContent=`${before} → ${after}`;$('position').textContent=`row ${y} · ${rows.length} rows`;
  $('hoverState').textContent=under?(state.hover?'Foreground: entire menu':'Terminal characters cover the dim menu'):'Opaque sidebar · inline pagination';
  $('summary').textContent=under?`${result.changes.length} paragraph${result.changes.length===1?'':'s'} changed · ${blanks} blank rows used · later rows fixed`:`${rows.filter(r=>r.kind==='tab').length} visible tabs · exactly 1 row each`;
  $('reflowNote').textContent=!under?'Sidebar layout: text is not reflowed.':state.original?'Showing original text. The menu stays at the same position.':!state.reflow?'Reflow disabled. Menu remains underneath terminal cells.':result.changes.length?'Changed lines are marked just left of their text. No following content moves.':'No safe beneficial reflow here. The menu remains dim underneath the terminal.';
  $('decisions').innerHTML=result.blocks.filter(b=>b.status!=='unchanged').slice(0,12).map(b=>`<div><b>rows ${b.startRow+1}–${b.endRow+1}</b> ${escapeHTML(b.reason||b.status)}</div>`).join('')||'<p>No eligible paragraph needs a change.</p>';
  $('heat').innerHTML=original.map((s,r)=>`<div class="${r>=y&&r<y+rows.length?'picked':''}"><i style="width:${Math.min(C,s.trimEnd().length)/C*100}%;height:1px"></i><i style="width:${Math.min(C,result.lines[r].trimEnd().length)/C*100}%;height:2px;background:#708d6b"></i></div>`).join('');
  function numbered(ls){return ls.map((s,r)=>`<span class="${original[r]!==result.lines[r]?(original[r].trim()?'changed':'blank'):''}"><span class="line-num">${String(r+1).padStart(2)} │ </span>${escapeHTML(s)||' '}</span>`).join('\n');}
  $('originalText').innerHTML=numbered(original);$('reflowedText').innerHTML=numbered(result.lines);
  window.mockupState={side:dock.side,layout:state.layout,dock:y,rows:rows.length,tabRows:rows.filter(r=>r.kind==='tab').length,changes:result.changes.length,blanks,overlapBefore:before,overlapAfter:after,terminalCols:termWidth,source:cap.id,markers:result.markers};
  sync();
}
function pointer(e){const b=canvas.getBoundingClientRect();return{x:Math.floor((e.clientX-b.left)/b.width*C),y:Math.floor((e.clientY-b.top)/b.height*R)}}
function target(e){const p=pointer(e);return hit.find(h=>p.y===h.y&&p.x>=h.x&&p.x<h.x+h.w)}
canvas.addEventListener('pointerdown',e=>{const h=target(e);if(h?.kind==='grip'&&state.layout!=='sidebar'){drag={row:pointer(e).y,origin:state.dock};canvas.setPointerCapture(e.pointerId);}});
canvas.addEventListener('pointerup',e=>{drag=null;if(canvas.hasPointerCapture(e.pointerId))canvas.releasePointerCapture(e.pointerId);});
canvas.addEventListener('pointermove',e=>{if(drag){state.dock=Math.max(0,Math.min(drag.origin+pointer(e).y-drag.row,Number($('dock').max)));render();return;}const next=target(e)?.group||null;if(next!==state.hover){state.hover=next;render();}});
canvas.addEventListener('pointerleave',()=>{if(!drag){state.hover=null;render();}});
function advance(kind,direction=1){const settings=kind==='settings'||kind==='setnav',key=settings?'setOffset':'tabOffset',n=lastRows.find(r=>r.pager?.kind===(settings?'settings':'tabs'))?.pager.size||lastRows.filter(r=>r.kind===(settings?'settingItem':'tab')).length,len=settings?(state.theme?themes:fonts).length:tabs.length;state[key]=Math.max(0,Math.min(state[key]+direction*n,Math.floor((len-1)/n)*n));render();}
canvas.addEventListener('click',e=>{const h=target(e);if(!h)return;
  if(h.row.pager){const col=pointer(e).x-h.x,counter=`${h.row.pager.page}/${h.row.pager.pages}`,nextX=W-1-counter.length-4;if(col>=1&&col<5){advance(h.row.pager.kind,-1);return;}if(col>=nextX&&col<nextX+4){advance(h.row.pager.kind,1);return;}if(col>=nextX+4)return;}
  if(h.kind==='settingsHeader'){state.settings=!state.settings;state.hover=null;}
  if(h.kind==='settingsTabs'){state.theme=pointer(e).x-h.x>=7;state.setOffset=0;}
  if(h.kind==='tab')state.active=h.row.index;
  if(h.kind==='settingItem')state[state.theme?'scheme':'font']=h.row.item;
  if(h.kind==='reflowToggle')state.reflow=!state.reflow;
  render();
});

for(const k of ['layout','border','capture','settings','reflow','markers'])$(k).addEventListener('change',()=>{state[k]=$(k).type==='checkbox'?$(k).checked:$(k).value;state.hover=null;if(k==='capture'){state.original=false;const cap=captures.find(c=>c.id===state.capture);if(cap?.columns){C=cap.columns;resize();}}state.tabOffset=state.setOffset=0;render();});
$('columns').onchange=()=>{C=Number($('columns').value);resize();render();};$('height').onchange=()=>{R=Number($('height').value);state.dock=0;resize();render();};
$('dock').oninput=()=>{state.dock=Number($('dock').value);state.hover=null;render();};$('opacity').oninput=()=>{state.opacity=Number($('opacity').value)/100;render();};
$('before').onclick=()=>{state.original=true;state.hover=null;render();};$('after').onclick=()=>{state.original=false;state.hover=null;render();};
$('export').onclick=()=>{const a=document.createElement('a');a.download=`hardcore-reflow-${state.preset+1}${state.original?'-original':''}.png`;a.href=canvas.toDataURL();a.click();};
$('apply').onclick=()=>{const lines=$('editor').value.replaceAll('\r','').split('\n');let cap=captures.find(c=>c.id==='custom');if(!cap){cap={id:'custom',title:'Your text',source:'Custom local input. No model inference or network calls.'};captures.push(cap);const option=document.createElement('option');option.value=cap.id;option.textContent=cap.title;$('capture').appendChild(option);}cap.lines=lines;state.capture='custom';state.original=false;if(state.layout==='sidebar')state.layout='underlay';state.hover=null;render();};
$('saveCase').onclick=()=>{const cap=captures.find(c=>c.id===state.capture);const data={schema:'hardcore-reflow-case-1',title:cap?.title||'Local example',source:cap?.source||'Custom text',columns:C,rows:R,dock:lastDock,maxBlankRows:3,lines:lastOriginal,observed:lastResult.lines,expected:null,notes:'Add the desired rows to expected after reviewing; observed is current prototype output, not a correctness claim.'};const blob=new Blob([JSON.stringify(data,null,2)+'\n'],{type:'application/json'});const url=URL.createObjectURL(blob),a=document.createElement('a');a.download='hardcore-reflow-case.json';a.href=url;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);};
$('presets').innerHTML=presets.map((p,i)=>`<button onclick="setPreset(${i})"><b>${String(i+1).padStart(2,'0')}</b>${p.name}<small>${p.sub}</small></button>`).join('');
$('capture').innerHTML=captures.map(c=>`<option value="${c.id}">${escapeHTML(c.title)}</option>`).join('');
$('editor').value='123\n  the quick brown fox jumps over\n  the sleeping\n  dog\n\n456\n  hello world';
window.setPreset=preset;window.renderMockup=render;preset(9);
