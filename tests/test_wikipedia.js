/* Exercises embedded adapter + production C grid over structural article
   fixtures. --preview also writes readable layouts for design review. */
const assert=require('node:assert/strict'),fs=require('node:fs'),cp=require('node:child_process');
const {fixture}=require('./browser_dom_fixture');
const manifest=require('./fixtures/wikipedia/manifest.json');
const allRuns=b=>b.columns?b.columns.flat():b.runs || [];
const words=s=>s.blocks.flatMap(allRuns).map(r=>r.image?'[image]':r.text).join('');
const width=c=>/\p{Mark}/u.test(c)?0:c.codePointAt(0)>=0x2e80 && c.codePointAt(0)<=0x9fff?2:1;
function protocol(snapshot) {
  const result=[`B ${snapshot.blocks.length}`];let image=0;
  for(const b of snapshot.blocks) {
    const cells=[],ends=[],columns=b.columns || [b.runs];
    for(const runs of columns) {
      for(const r of runs) {
        if(r.image){cells.push([++image,8,3,0,8,r.action || 0,r.node || 0]);continue;}
        for(let ch of r.text) {
          if(ch==='\n' && b.kind!=='code' && !b.columns)ch=' ';
          const w=width(ch);if(!w)continue;
          const attrs=(r.bold || b.kind==='heading'?1:0)|(r.dim?2:0)|(r.italic?4:0);
          const cell=[ch.codePointAt(0),r.action?4:0,0,attrs,w===2?1:0,r.action || 0,r.node || 0];
          cells.push(cell);if(w===2)cells.push([32,cell[1],0,attrs,2,cell[5],cell[6]]);
        }
      }
      ends.push(cells.length);
    }
    result.push([cells.length,b.indent||0,b.gap??1,b.kind==='code'?1:0,b.rule?1:0,b.table||0,b.header?1:0,b.columns?.length||0,b.numeric||0,b.hang??2].join(' '));
    if(b.columns)result.push(ends.join(' '));
    result.push(...cells.map(c=>c.join(' ')));
  }
  return result.join('\n')+'\n';
}
function render(snapshot,cols,side=0,scroll=0) {
  const dw=side?Math.min(28,Math.floor(cols/3)):0,dx=side===1?cols-dw:0,dy=5,dh=12;
  const data=JSON.parse(cp.execFileSync('build/browser-document-dump',[cols,500,scroll,side,dx,dy,dw,dh].map(String),{input:protocol(snapshot),encoding:'utf8',maxBuffer:20*1024*1024}));
  assert(data.total<500,'fixture should fit the captured document');
  if(side)for(let y=dy;y<dy+dh;y++)for(let x=dx;x<dx+dw;x++)assert.equal(data.cells[y*cols+x][0],32,'menu lane stays clear');
  for(const [ch,attr,link,node] of data.cells)if('─│┼'.includes(String.fromCodePoint(ch)))assert.equal(attr&2,2,'rules stay dim');
  return data;
}
fs.mkdirSync('build/wikipedia-preview',{recursive:true});
const previews=[];
for(const sample of manifest.cases) {
  const {api,document}=fixture('tests/fixtures/wikipedia/'+sample.file,sample.url),snap=api.project(),value=words(snap);
  assert(snap.blocks[0].runs[0].text.startsWith('WIKIPEDIA'));
  assert(snap.blocks[1].rule);
  assert(!/boilerplate|\bAppearance\b|↑|\bedit\b/.test(value));
  assert(snap.blocks.filter(b=>b.kind==='heading').length>=3);
  if(sample.file!=='printing-press.html')assert(snap.blocks.some(b=>b.table));
  for(const b of snap.blocks) {
    assert(b.gap>=0 && b.gap<=2);
    for(const r of allRuns(b))if(r.action)assert(api.info(r.action,snap.generation),'links retain their actual DOM targets');
    if(b.columns)assert(b.columns.length>=1 && b.columns.length<=16);
  }
  for(const r of snap.blocks.find(b=>b.kind==='contents')?.runs || [])if(r.action) {
    const target=api.info(r.action,snap.generation).target;
    assert(target && snap.blocks.some(b=>b.anchor===target),'contents maps to projected heading');
  }
  if(sample.file==='ada-lovelace.html') {
    assert(snap.blocks.findIndex(b=>b.kind==='paragraph')<snap.blocks.findIndex(b=>b.kind==='table'));
    assert(value.includes('Byron · Anne · Ralph'));
    const citation=snap.blocks.flatMap(allRuns).find(r=>r.text==='[1]');
    const target=api.info(citation.action,snap.generation).target;
    assert(snap.blocks.some(b=>b.anchor===target),'citation navigates to source entry');
  }
  if(sample.file==='fourier-transform.html') {
    assert.equal((value.match(/F\(k\)/g)||[]).length,1,'one accessible equation, no duplicate fallback');
    assert(!snap.blocks.flatMap(allRuns).some(r=>r.image),'equation fallback not counted as thumbnail');
  }
  if(sample.file==='complex-tables.html') {
    const table=snap.blocks.filter(b=>b.columns && b.columns.length===5);
    assert(table[0].header && table[0].columns.map(c=>c.map(r=>r.text).join('')).join('|').includes('First survey / People'));
    assert.equal(table[1].columns[0].map(r=>r.text).join(''),table[2].columns[0].map(r=>r.text).join(''),'rowspan carries its label');
    assert(value.includes('Fast: Reduced detail') && value.includes('Careful: More detail'),'nested tables flatten to meaningful pairs');
  }
  for(const cols of [38,72,110,160])for(const side of [0,1,2]) {
    const view=render(snap,cols,side);
    // Every original non-space character retains its node/action; duplicates
    // from row spans/repeated narrow labels are allowed, omissions are not.
    const seen=new Set(view.cells.filter(c=>!(c[6]&8)).map(c=>`${c[0]}/${c[2]}/${c[3]}`));
    for(const r of snap.blocks.flatMap(allRuns))if(!r.image)for(const ch of r.text)if(ch.trim() && width(ch))assert(seen.has(`${ch.codePointAt(0)}/${r.action||0}/${r.node||0}`),`lost ${ch} in ${sample.file} at ${cols}/${side}`);
    if(cols===110 && side===0) {
      fs.writeFileSync('build/wikipedia-preview/'+sample.file.replace('.html','.txt'),view.lines.slice(0,view.total).join('\n')+'\n');
      previews.push({sample,view});
    }
  }
  const again=api.project();assert.equal(again.blocks[1].anchor,snap.blocks[1].anchor,'heading identity stable');
  assert.equal(document.querySelector('.navbox')?.isConnected ?? true,true,'native DOM is untouched');
}
// More pathological structures must retain labels as well as values, even
// when they cannot use a conventional grid table.
const edge='<html><head><title>Edges</title></head><body><h1 id="firstHeading">Edges</h1><div class="mw-parser-output">'+
 '<table><tr>'+Array.from({length:17},(_,i)=>`<th>Label${i}</th>`).join('')+'</tr><tr>'+Array.from({length:17},(_,i)=>`<td>Value${i}</td>`).join('')+'</tr></table>'+
 '<ul><li>Parent<ul><li>First list</li></ul><ol><li>Second list</li></ol></li></ul>'+
 '<table><tr><th>A</th><th>B</th><th>C</th></tr><tr><th rowspan="2">Carry</th><td>X</td><td>Y</td></tr><tr><td colspan="2">Continuation</td></tr></table>'+
 '<table><tr><th>Single column</th></tr><tr><td>Only value</td></tr></table>'+
 '<p><math alttext="x^{2}"><mi>x</mi><mn>2</mn><annotation encoding="application/x-tex">duplicate math</annotation></math></p></div></body></html>';
fs.writeFileSync('build/wikipedia-edge.html',edge);
const edgeSnap=fixture('build/wikipedia-edge.html','https://en.wikipedia.org/wiki/Fixture').api.project(),edgeWords=words(edgeSnap);
for(let i=0;i<17;i++)assert(edgeWords.includes(`Label${i}`) && edgeWords.includes(`Value${i}`));
assert(edgeWords.includes('First list') && edgeWords.includes('Second list'));
assert(edgeWords.includes('x^{2}') && !edgeWords.includes('duplicate math'));
assert.equal(edgeSnap.blocks.filter(b=>b.columns?.some(c=>c.some(r=>r.text==='Carry'))).length,2);
assert(edgeSnap.blocks.some(b=>b.columns?.length===1 && b.columns[0].some(r=>r.text==='Only value')));
render(edgeSnap,38,1);
if(process.argv.includes('--preview')) {
  const escape=s=>s.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const colored=v=>v.lines.slice(0,v.total).map((_,y)=>{
    let line='';for(let x=0;x<v.cols;x++) {
      const [ch,attr,link,,, ,flags]=v.cells[y*v.cols+x];if(flags&2)continue;
      const value=flags&8?'▒':String.fromCodePoint(ch || 32);
      line+=`<span class="${attr&2?'dim ':''}${attr&1?'strong ':''}${link?'link':''}">${escape(value)}</span>`;
    }return line.trimEnd();
  }).join('\n');
  fs.writeFileSync('build/wikipedia-preview/index.html',`<!doctype html><meta charset="utf-8"><title>Wikipedia · Hardcore layout review</title><style>
  :root{color-scheme:dark}body{margin:0;background:#191c23;color:#cbd0dc;font:15px/1.5 ui-monospace,Menlo,monospace}header{padding:24px;max-width:900px}h1{font-size:20px}p{color:#9099ab}nav a{color:#8daed0;margin-right:16px}article{padding:24px 0;border-top:1px solid #313743}pre{font:14px/20px ui-monospace,Menlo,monospace;margin:0;overflow:auto}.dim{color:#626b7d}.strong{font-weight:bold;color:#ebedf2}.link{color:#8daed0}h2{font-size:14px;color:#929aac;margin:0 0 12px}button{font:inherit}
  </style><header><h1>Wikipedia / document studies</h1><p>Production C grid output at 110 columns. Six synthetic structural fixtures based on article patterns; these are not captures of live Wikipedia. Image tiles are represented by shaded cells. Rules use the document’s dim style.</p><nav>${previews.map(({sample},i)=>`<a href="#sample${i}">${escape(sample.file.replace('.html',''))}</a>`).join('')}</nav></header>${previews.map(({sample,view},i)=>`<article id="sample${i}"><h2>${escape(sample.file)} · ${view.total} rows</h2><pre>${colored(view)}</pre></article>`).join('')}`);
}
console.log('PASS Wikipedia: six structural articles × four widths × both docks and full width; links, anchors, tables, spans, math, captions and DOM preservation');
