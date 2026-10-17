/* Runs in an isolated browser content world, outside the site's JS namespace. */
(() => {
  if (globalThis.hardcoreBrowser) return;
  const notify = message => {
    if (typeof globalThis.hardcoreChromeNotify === 'function') globalThis.hardcoreChromeNotify(JSON.stringify(message));
    else globalThis.webkit?.messageHandlers?.hardcoreBrowser?.postMessage(message);
  };
  let actions = new Map(), nodeIds = new WeakMap(), nextNode = 1, generation = 0, pending = false;
  const blockTags = new Set(['ADDRESS','ARTICLE','ASIDE','BLOCKQUOTE','DIV','DL','DT','DD','FIGCAPTION','FIGURE','FOOTER','FORM','H1','H2','H3','H4','H5','H6','HEADER','LI','MAIN','NAV','OL','P','PRE','SECTION','TABLE','TR','UL']);
  const clean = text => String(text || '').replace(/[\x00-\x08\x0b-\x1f\x7f]/g, '');
  function changed() {
    if (pending) return;
    pending = true;
    setTimeout(() => {
      pending = false;
      notify({type:'changed'});
    }, 150);
  }
    function actionFor(node) {
      if (nodeIds.has(node)) {const id=nodeIds.get(node);actions.set(id,node);return id;}
      if (nextNode > 4000) return 0;
      const id = nextNode++; actions.set(id,node); nodeIds.set(node,id); return id;
    }
  function project() {
    actions = new Map(); generation++;
    const blocks = []; let runs = [], length = 0, kind = 'paragraph', indent = 0, currentNode = 0, images = 0;
    const flush = () => {
      if (runs.some(run => run.image || run.text.trim())) blocks.push({kind, indent, runs});
      runs = [];
    };
    function push(text, action = 0, bold = false) {
      text = clean(text);
      if (kind !== 'code') text = text.replace(/\s+/g, ' ');
      if (!text || length >= 100000 || blocks.length >= 2000) return;
      text = text.slice(0, 100000-length); length += text.length;
      runs.push({text, action, bold, node:currentNode});
    }
    function walk(node, action = 0, bold = false, depth = 0) {
      if (depth > 100 || length >= 100000 || blocks.length >= 2000) return;
      if (node.nodeType === 3) { push(node.nodeValue, action, bold); return; }
      if (node.nodeType !== 1) return;
      const tag = node.tagName;
      if (['SCRIPT','STYLE','NOSCRIPT','TEMPLATE','HEAD','SVG','CANVAS','VIDEO','AUDIO'].includes(tag) || node.hidden || node.getAttribute('aria-hidden') === 'true') return;
      const style = getComputedStyle(node);
      if (style.display === 'none' || style.visibility === 'hidden' || adapter?.skip?.(node)) return;
      if (tag === 'BR') { flush(); return; }
      if (tag === 'HR') { flush(); push('────────────────'); flush(); return; }
      const oldNode=currentNode; currentNode=actionFor(node);
      const isBlock = blockTags.has(tag), oldKind = kind, oldIndent = indent;
      if (isBlock) {
        flush();
        kind = tag === 'PRE' ? 'code' : /^H[1-6]$/.test(tag) ? 'heading' : tag === 'LI' ? 'list' : tag === 'TR' ? 'table' : 'paragraph';
        if (tag === 'LI') { indent = Math.min(12, indent+2); push('• '); }
      }
      if (tag === 'IMG') {
        const src = node.currentSrc || node.src || '', alt = clean(node.getAttribute('alt') || 'Image');
        if (src && images < 32) {
          images++;
          runs.push({text:'',action,node:currentNode,image:{src,alt,loaded:!!node.complete && node.naturalWidth>0,width:node.naturalWidth || node.width || 64,height:node.naturalHeight || node.height || 48}});
        } else if (alt) push(`[${alt}]`,action,bold);
      }
      else if (['INPUT','TEXTAREA','SELECT','BUTTON'].includes(tag) || node.getAttribute('role') === 'button') {
        if (tag === 'INPUT' && node.type === 'hidden') {currentNode=oldNode;return;}
        const id = actionFor(node);
        let label = node.getAttribute('aria-label') || (node.labels && [...node.labels].map(n => n.textContent).join(' ')) || node.placeholder || node.name || '';
        if (tag === 'BUTTON' || node.getAttribute('role') === 'button') label = node.innerText || label || 'Button';
        else if (node.type === 'checkbox' || node.type === 'radio') label = `${node.checked ? '[x]' : '[ ]'} ${label}`;
        else if (tag === 'SELECT') label = `${label}: ${node.selectedOptions?.[0]?.text || ''}`;
        else label = `${label}: ${node.type === 'password' ? '••••' : node.value || ''}`;
        push(`[${clean(label).trim()}${node.disabled ? ' (disabled)' : ''}]`, id, true); push(' ');
      } else {
        const nextAction = tag === 'A' && node.hasAttribute('href') ? actionFor(node) : action;
        for (const child of node.childNodes) walk(child, nextAction, bold || ['B','STRONG'].includes(tag) || kind === 'heading', depth+1);
      }
      if (tag === 'TD' || tag === 'TH') push(' | ');
      if (isBlock) { flush(); kind = oldKind; indent = oldIndent; }
      currentNode=oldNode;
    }
    const adapters = globalThis.hardcoreSiteAdapters || [];
    const adapter = adapters.find(a => !a.fallback && a.matches(location.hostname, location.pathname)) || adapters.find(a => a.fallback);
    const root = adapter?.root(document) || document.body;
    // Adapters describe document structure; the native grid still chooses widths
    // and wraps the cells at the current font, viewport and menu position.
    function emit(block, element) {
      if (blocks.length >= 2000 || length >= 100000) return;
      function bounded(source) {
        const result=[];
        for (const run of source || []) {
          if (length >= 100000) break;
          const text=clean(run.text).slice(0,100000-length); length+=text.length;
          if (run.image) {if(images>=32)continue;images++;}
          if (text || run.image) result.push({...run,text});
        }
        return result;
      }
      const value={hang:0,...block};
      if (block.columns) value.columns=block.columns.slice(0,16).map(bounded);
      else value.runs=bounded(block.runs);
      if(element)value.anchor=actionFor(element);
      if(value.columns?.some(c=>c.length) || value.runs?.some(r=>r.image || r.text.trim()))blocks.push(value);
    }
    function inline(element, options={}) {
      const result=[];let size=0;
      function add(text,node,action,bold,dim=false) {
        if(size>=16000)return;
        text=clean(text).replace(/[\s\u200b]+/g,' ').slice(0,16000-size);size+=text.length;
        if(text)result.push({text,node:actionFor(node),action,bold,dim});
      }
      function visit(node,action=0,bold=false,depth=0) {
        if(!node || depth>80 || size>=16000)return;
        if(node.nodeType===3){add(node.nodeValue,node.parentElement || element,action,bold);return;}
        if(node.nodeType!==1)return;
        const tag=node.tagName.toUpperCase();
        if(['SCRIPT','STYLE','NOSCRIPT','TEMPLATE','HEAD','SVG','CANVAS','VIDEO','AUDIO'].includes(tag) || node.hidden || node.getAttribute('aria-hidden')==='true' || options.skip?.(node))return;
        const style=getComputedStyle(node);
        if(style.display==='none' || style.visibility==='hidden')return;
        if(tag==='MATH' || node.matches?.('.mwe-math-element')) {
          const math=tag==='MATH'?node:node.querySelector('math');
          const alt=math?.getAttribute('alttext') || math?.querySelector('annotation[encoding="application/x-tex"]')?.textContent || node.querySelector('img')?.getAttribute('alt');
          if(alt){const wrapped=alt.match(/^\{\\displaystyle\s+([\s\S]*)\}$/);add(wrapped?wrapped[1]:alt,node,action,bold);return;}
        }
        if(tag==='ANNOTATION' || tag==='ANNOTATION-XML')return;
        if(tag==='BR') {result.push({text:options.breaks?'\n':' · ',node:actionFor(node),action:0});return;}
        if(tag==='IMG') {
          const src=node.currentSrc || node.src || '',alt=clean(node.getAttribute('alt') || '');
          if(options.images===false) {if(alt)add(alt,node,action,bold);return;}
          if(src)result.push({text:'',action,node:actionFor(node),image:{src,alt,loaded:!!node.complete && node.naturalWidth>0,width:node.naturalWidth || node.width || 64,height:node.naturalHeight || node.height || 48}});
          return;
        }
        if(tag==='A' && node.hasAttribute('href'))action=actionFor(node);
        bold=bold || ['B','STRONG','TH'].includes(tag);
        const separate=['DIV','P','LI','UL','OL','DD','DT','TR'].includes(tag) && node!==element;
        if(separate && result.length && (tag==='LI' || !/\s$/.test(result.at(-1).text)))add(tag==='LI'?' · ':' ',node,0,false);
        for(const child of node.childNodes)visit(child,action,bold,depth+1);
        if(tag==='TH' && node!==element)add(': ',node,0,false);
        else if(tag==='TD' && node!==element)add(' ',node,0,false);
        if(separate && result.length && !/\s$/.test(result.at(-1).text))add(' ',node,0,false);
      }
      visit(element,element.tagName==='IMG' && element.closest?.('a[href]') ? actionFor(element.closest('a[href]')) : 0);
      // Collapse whitespace across inline element boundaries, never around an
      // image or an intentional cell line break. Keep link/node identities.
      let space=true;
      for(const run of result) {
        if(run.image){space=false;continue;}
        if(run.text==='\n'){space=true;continue;}
        if(space)run.text=run.text.replace(/^ +/,'');
        if(run.text)space=/ $/.test(run.text);
      }
      if(result.length && !result.at(-1).image)result.at(-1).text=result.at(-1).text.trimEnd();
      return result.filter(r=>r.text || r.image);
    }
    if (root && adapter?.project) adapter.project(document,{root,emit,inline,id:actionFor,clean,walk});
    else if (root) walk(root);
    flush();
    return {generation, title:clean(document.title), site:clean(document.querySelector?.('meta[property="og:site_name"]')?.content || location.hostname.replace(/^www\./,'')), url:location.href, blocks};
  }
  function info(id, revision) {
    if (revision !== generation) return null;
    const node = actions.get(id);
    if (!node?.isConnected) return null;
    let target=0;
    if(/^H[1-6]$/.test(node.tagName))target=actionFor(node);
    if(node.tagName==='A' && node.href)try {
      const url=new URL(node.href,location.href),here=new URL(location.href);
      if(url.origin===here.origin && url.pathname===here.pathname && url.search===here.search && url.hash) {
        const element=document.getElementById(decodeURIComponent(url.hash.slice(1)));
        if(element)target=actionFor(element.closest?.('h1,h2,h3,h4,h5,h6,li') || element);
      }
    }catch(_){}
    return {tag:node.tagName, target, type:node.type || '', href:node.href || '', src:node.currentSrc || node.src || '', value:node.type === 'password' ? '' : node.value || '',
      label:node.getAttribute('aria-label') || node.placeholder || node.name || 'Value',
      options:node.tagName === 'SELECT' ? [...node.options].map(o => ({value:o.value,label:o.text})) : []};
  }
  function activate(id, revision, value) {
    if (revision !== generation) return false;
    const node = actions.get(id);
    if (!node?.isConnected || node.disabled) return false;
    if (value !== undefined) {
      const proto = node.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : node.tagName === 'SELECT' ? HTMLSelectElement.prototype : HTMLInputElement.prototype;
      const setter = Object.getOwnPropertyDescriptor(proto,'value')?.set;
      if (setter) setter.call(node,value); else node.value = value;
      node.dispatchEvent(new Event('input',{bubbles:true})); node.dispatchEvent(new Event('change',{bubbles:true}));
    } else { node.focus(); node.click(); }
    changed(); return true;
  }
  function inspect(id, revision) {
    if (revision !== generation) return null;
    const node = id ? actions.get(id) : document.body;
    if (!node?.isConnected) return null;
    const style=getComputedStyle(node);
    const attrs=[...(node.attributes || [])].slice(0,30).map(a=>({name:a.name,value:clean(a.value).slice(0,500)}));
    return {tag:node.tagName.toLowerCase(),id:actionFor(node),parent:node.parentElement ? actionFor(node.parentElement) : 0,
      attributes:attrs,text:clean(node.type === 'password' ? '' : node.textContent).slice(0,4000),
      style:['display','position','color','background-color','font-family','font-size','width','height','margin','padding','border','overflow'].map(name=>({name,value:style.getPropertyValue?.(name) || ''})),
      children:[...(node.children || [])].slice(0,50).map(child=>({id:actionFor(child),tag:child.tagName.toLowerCase(),text:clean(child.type==='password' ? '' : child.textContent).replace(/\s+/g,' ').slice(0,100)}))};
  }
  globalThis.hardcoreBrowser = {project,info,activate,inspect};
  new MutationObserver(changed).observe(document,{subtree:true,childList:true,characterData:true,attributes:true,attributeFilter:['class','style','hidden','aria-hidden','aria-label','value','checked','disabled']});
  document.addEventListener('contextmenu',event=>{
    const target=event.target?.nodeType===1 ? event.target : event.target?.parentElement;
    if(!target)return;
    event.preventDefault();
    const anchor=target.closest?.('a[href]');
    notify({type:'context',generation,node:actionFor(target),link:anchor ? actionFor(anchor) : 0});
  },true);
  globalThis.addEventListener?.('resize',changed);
  document.addEventListener('load',changed,true);
  document.addEventListener('input',changed,true); document.addEventListener('change',changed,true);
  document.addEventListener('DOMContentLoaded',changed,{once:true}); changed();
})();
