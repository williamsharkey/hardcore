/* Bounded structural fixture DOM. This is not a browser compatibility test.
   HTML tokenization uses Python's standard parser; selectors cover adapters. */
const fs=require('node:fs'),vm=require('node:vm'),cp=require('node:child_process');
class Element {
  constructor(data,parent,url) {
    this.nodeType=1;this.tagName=data.tag;this.attrs=data.attrs;this.parentElement=parent;this.url=url;
    this.isConnected=true;this.childNodes=data.children.map(child=>typeof child==='string'?{nodeType:3,nodeValue:child,parentElement:this}:new Element(child,this,url));
  }
  get children(){return this.childNodes.filter(n=>n.nodeType===1);}
  get textContent(){return this.childNodes.map(n=>n.nodeType===3?n.nodeValue:n.textContent).join('');}
  get innerText(){return this.textContent;}
  get attributes(){return Object.entries(this.attrs).map(([name,value])=>({name,value}));}
  getAttribute(name){return this.attrs[name]??null;}
  hasAttribute(name){return name in this.attrs;}
  get id(){return this.attrs.id || '';}
  get lang(){return this.attrs.lang || '';}
  get hidden(){return this.hasAttribute('hidden');}
  get href(){return this.attrs.href?new URL(this.attrs.href,this.url).href:'';}
  get src(){return this.attrs.src?new URL(this.attrs.src,this.url).href:'';}
  get currentSrc(){return this.src;}
  get width(){return +this.attrs.width || 0;}
  get height(){return +this.attrs.height || 0;}
  get naturalWidth(){return this.width;}
  get naturalHeight(){return this.height;}
  get content(){return this.attrs.content;}
  matches(selector) {
    return selector.split(',').some(part=>{
      const pieces=part.trim().split(/\s+/);let node=this;
      function simple(element,s) {
        if(!element)return false;
        const tag=s.match(/^[a-zA-Z][\w-]*|^\*/)?.[0];
        if(tag && tag!=='*' && element.tagName!==tag.toUpperCase())return false;
        for(const [,kind,name] of s.matchAll(/([.#])([\w-]+)/g)) {
          if(kind==='#' && element.id!==name)return false;
          if(kind==='.' && !(element.attrs.class || '').split(/\s+/).includes(name))return false;
        }
        for(const [,key,,value] of s.matchAll(/\[([\w:-]+)(?:=(["']?)([^\]"']*)\2)?\]/g)) {
          if(!element.hasAttribute(key) || (value!==undefined && element.getAttribute(key)!==value))return false;
        }
        return true;
      }
      if(!simple(node,pieces.pop()))return false;
      while(pieces.length){const expected=pieces.pop();node=node.parentElement;while(node && !simple(node,expected))node=node.parentElement;if(!node)return false;}
      return true;
    });
  }
  closest(s){return this.matches(s)?this:this.parentElement?.closest(s) || null;}
  querySelectorAll(s){const out=[];for(const child of this.children){if(child.matches(s))out.push(child);out.push(...child.querySelectorAll(s));}return out;}
  querySelector(s){return this.querySelectorAll(s)[0] || null;}
  focus(){this.focused=true;}
  click(){this.clicked=true;}
}
function fixture(file,url) {
  const raw=cp.execFileSync('python3',['tests/html_fixture.py',file],{encoding:'utf8',maxBuffer:24*1024*1024});
  const document=new Element(JSON.parse(raw),null,url);
  document.body=document.querySelector('body');document.documentElement=document.querySelector('html');document.title=document.querySelector('title')?.textContent || '';
  document.getElementById=id=>document.querySelectorAll('[id]').find(n=>n.id===id) || null;
  document.addEventListener=()=>{};
  const context=vm.createContext({document,location:new URL(url),URL,
    getComputedStyle:n=>({display:/display\s*:\s*none/.test(n.attrs.style || '')?'none':'block',visibility:'visible'}),
    MutationObserver:class{observe(){}},setTimeout(){},HTMLInputElement:Element,HTMLSelectElement:Element,HTMLTextAreaElement:Element,Event:class{}});
  const encoded=fs.readFileSync('build/browser.inc','utf8').match(/browser_script\[\] = \{([\s\S]*?)0\};/)[1];
  const source=Buffer.from(encoded.split(',').map(s=>s.trim()).filter(Boolean).map(Number)).toString('utf8');
  vm.runInContext(source,context);
  return {api:context.hardcoreBrowser,document};
}
module.exports={fixture};
