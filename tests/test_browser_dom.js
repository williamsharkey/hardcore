/* Tests the actual embedded script against a small DOM fixture. No packages,
   browser process, networking, or AppKit registration. */
const assert=require('node:assert/strict'), fs=require('node:fs'), vm=require('node:vm');
class Node {
  constructor(tag,attrs={},children=[]) {
    this.nodeType=1; this.tagName=tag; this.attrs=attrs; this.childNodes=children;
    this.hidden=false; this.isConnected=true; this.type=attrs.type||'';
    this.name=attrs.name||''; this.placeholder=attrs.placeholder||'';
    this._value=attrs.value||''; this.checked=false; this.disabled=false;
    this.options=[]; this.selectedOptions=[]; this.events=[];for(const child of children)child.parentElement=this;
  }
  get children(){return this.childNodes.filter(n=>n.nodeType===1);}
  get attributes(){return Object.entries(this.attrs).map(([name,value])=>({name,value}));}
  closest(selector){return selector==='a[href]' ? this.tagName==='A' ? this : this.parentElement?.closest(selector) : null;}
  getAttribute(name){return this.attrs[name]??null;}
  hasAttribute(name){return name in this.attrs;}
  get textContent(){return this.childNodes.map(n=>n.nodeValue||n.textContent).join('');}
  get innerText(){return this.textContent;}
  get value(){return this._value;}
  set value(value){this._value=value;}
  dispatchEvent(event){this.events.push(event.type);}
  focus(){this.focused=true;}
  click(){this.clicked=true;if(this.type==='checkbox')this.checked=!this.checked;}
}
const text=s=>({nodeType:3,nodeValue:s}), el=(tag,children=[],attrs={})=>new Node(tag,attrs,children);
const link=el('A',[text('Real link')],{href:'https://example.com'});
const input=el('INPUT',[],{name:'name',placeholder:'Name',value:'Ada'});
const checkbox=el('INPUT',[],{type:'checkbox',name:'agree'});
const password=el('INPUT',[],{type:'password',name:'password',value:'never-project-this'});
const button=el('BUTTON',[text('Update')]);
const picture=el('IMG',[],{alt:'Tiny image'});picture.src='https://example.com/picture.png';picture.naturalWidth=64;picture.naturalHeight=48;
const body=el('BODY',[
  el('P',[picture,text(' text flows around the image')]),el('H1',[text('Heading')]),el('P',[text('Before '),link,text(' after.')]),
  el('PRE',[text('alpha\n  beta')]),el('DIV',[input,checkbox,password,button]),
  el('SCRIPT',[text('do not show')]),el('P',[text('hidden')],{'aria-hidden':'true'}),
  el('TABLE',[el('TR',[el('TD',[text('left')]),el('TD',[text('right')])])]),
]);
const events={},messages=[];
const document={body,title:'Test title',querySelector:()=>({content:'Test Site'}),addEventListener(name,callback){events[name]=callback;}};
const context=vm.createContext({document,location:{href:'https://example.com',hostname:'example.com',pathname:'/'},
  getComputedStyle:()=>({display:'block',visibility:'visible'}),
  webkit:{messageHandlers:{hardcoreBrowser:{postMessage:message=>messages.push(message)}}},
  MutationObserver:class {observe(){}},setTimeout(){},
  HTMLInputElement:Node,HTMLSelectElement:Node,HTMLTextAreaElement:Node,Event:class{constructor(type){this.type=type;}}
});
const embedded=fs.readFileSync('build/browser.inc','utf8').match(/browser_script\[\] = \{([\s\S]*?)0\};/)[1];
const source=Buffer.from(embedded.split(',').map(s=>s.trim()).filter(Boolean).map(Number)).toString('utf8');
vm.runInContext(source,context);
const api=context.hardcoreBrowser, snap=api.project();
const words=snap.blocks.flatMap(b=>b.runs).map(r=>r.text).join('');
assert(words.includes('Before Real link after.'));
assert.equal(snap.site,'Test Site');
const image=snap.blocks.flatMap(b=>b.runs).find(run=>run.image);
assert.equal(image.image.src,picture.src);assert.equal(image.image.width,64);assert(image.node>0);
const inspection=api.inspect(image.node,snap.generation);
assert.equal(inspection.tag,'img');assert(inspection.attributes.some(a=>a.name==='alt' && a.value==='Tiny image'));
let prevented=false;events.contextmenu({target:link,preventDefault(){prevented=true;}});
assert(prevented && messages.at(-1).type==='context' && messages.at(-1).link>0);

assert(words.includes('left | right |'));
assert(!words.includes('do not show') && !words.includes('hidden') && !words.includes('never-project-this'));
assert.equal(snap.blocks.find(b=>b.kind==='code').runs[0].text,'alpha\n  beta');
const runs=snap.blocks.flatMap(b=>b.runs), id=needle=>runs.find(r=>r.text.includes(needle)).action;
assert(api.activate(id('Real link'),snap.generation)); assert(link.clicked);
assert.equal(api.info(id('Name: Ada'),snap.generation).value,'Ada');
assert(api.activate(id('Name: Ada'),snap.generation,'Grace'));
assert.equal(input.value,'Grace'); assert.equal(input.events.join(','),'input,change');
assert(api.activate(id('agree'),snap.generation)); assert(checkbox.checked);
assert.equal(api.info(id('password'),snap.generation).value,'');
const refreshed=api.project();assert.equal(refreshed.blocks.flatMap(b=>b.runs).find(r=>r.text==='Real link').action,id('Real link'));
assert.equal(refreshed.title,'Test title');
assert.equal(api.activate(id('Real link'),snap.generation),false);
link.isConnected=false;
assert.equal(api.info(id('Real link'),snap.generation+1),null);
document.title='A different page title';assert.equal(api.project().title,'A different page title');
// Specific adapters must beat the always-matching default helper.
context.hardcoreSiteAdapters.push({matches:()=>true,root:()=>el('P',[text('Adapter root')])});
assert.equal(api.project().blocks[0].runs[0].text,'Adapter root');
console.log('PASS browser DOM: semantic projection, live controls, password masking, stale actions, tables and adapter precedence');
