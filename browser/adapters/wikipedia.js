/* Wikipedia is a document, not a stack of its skin's DIVs. These rules use
   MediaWiki semantics, never article names, and leave the live DOM untouched. */
(() => {
  const chrome='.mw-editsection,.mw-editsection-like,.mw-cite-backlink,.mw-empty-elt,.mw-indicators,.mw-jump-link,.mw-collapsible-toggle,.sortkey,.sortheader,.sortbottom .sortkey,.navbar,.navbox,.vertical-navbox,.sistersitebox,.portalbox,#toc,.toc,#coordinates,.nomobile .printonly';
  const skip=node=>node.matches?.(chrome) || false;
  const text=runs=>runs.map(r=>r.text || '').join('').trim();
  const compact=s=>String(s || '').replace(/\s+/g,' ').trim();
  globalThis.hardcoreSiteAdapters.push({
    name:'wikipedia',
    matches:host=>/(^|\.)wikipedia\.org$/i.test(host),
    root:document=>document.querySelector('#mw-content-text .mw-parser-output') || document.querySelector('.mw-parser-output') || document.body,
    skip,
    project(document,ctx) {
      const {root,emit,id}=ctx;
      if(!root.matches?.('.mw-parser-output')) {ctx.walk(root);return;}
      const inline=(node,options={})=>ctx.inline(node,{skip,...options});
      const title=document.querySelector('#firstHeading') || document.querySelector('h1');
      const titleText=compact(title?.textContent || document.title.replace(/\s*[-–]\s*Wikipedia.*$/,''));
      let tableID=0,lead=true,outlineDone=false;
      const deferred=[], headings=[...root.querySelectorAll('h2')].filter(h=>!h.closest('table,.navbox,.sidebar'));
      const plain=(value,node,options={})=>({text:value,node:node?id(node):0,action:0,...options});
      function block(kind,node,options={}) {
        emit({kind,runs:inline(node),gap:1,...options},node);
      }
      // The small masthead is the only persistent branding; no skin menus,
      // language pickers, edit links or nested navigation boxes in the text.
      emit({kind:'caption',gap:0,runs:[plain('WIKIPEDIA  /  '+(document.documentElement?.lang || 'en').toUpperCase(),title,{dim:true})]});
      emit({kind:'heading',gap:1,rule:true,runs:[plain(titleText,title,{bold:true})]},title);
      function finishLead() {
        if(outlineDone)return;
        outlineDone=true;
        for(const node of deferred)renderTable(node,true);
        if(headings.length>1) {
          const runs=[plain('Contents  ',null,{dim:true})];
          headings.forEach((h,i)=>{
            if(i)runs.push(plain('  ·  ',null,{dim:true}));
            runs.push(plain(text(inline(h)),h,{action:id(h)}));
          });
          emit({kind:'contents',gap:1,runs});
        }
      }
      function figure(node) {
        const images=[...node.querySelectorAll('img')].filter(n=>!n.closest('.mw-editsection'));
        const caption=node.querySelector('figcaption,.thumbcaption,.gallerytext');
        const runs=[];
        for(const image of images.slice(0,8))runs.push(...inline(image));
        if(caption)runs.push(...inline(caption).map(r=>({...r,dim:true})));
        if(runs.length)emit({kind:'figure',gap:1,runs},node);
      }
      function renderTable(table,infobox=false) {
        const rows=[...table.querySelectorAll('tr')].filter(r=>r.closest('table')===table);
        const caption=[...table.children].find(n=>n.tagName==='CAPTION');
        if(caption)block('caption',caption,{gap:0,runs:inline(caption).map(r=>({...r,bold:true}))});
        else if(infobox)emit({kind:'caption',gap:0,runs:[plain('At a glance',table,{dim:true})]},table);
        let group=++tableID,carry=[],pending=[],headers=[];
        function flushRows() {
          if(!pending.length)return;
          const columns=Math.max(...pending.map(r=>r.columns.length),headers.length);
          if(columns>16) {
            // Wide calendars/periodic layouts: retain every field rather than
            // cropping or pretending an 18-column spatial diagram is prose.
            for(const row of pending)emit({kind:'paragraph',gap:1,runs:Array.from({length:columns},(_,i)=>[
              ...(i?[plain('  ·  ',null,{dim:true})]:[]),
              ...(headers[i]?.length?[...headers[i],plain(': ',null,{dim:true})]:[]),
              ...(row.columns[i] || []),
            ]).flat()},row.node);
          } else {
            const numeric=Array.from({length:columns},(_,col)=>{
              const values=pending.filter(r=>!r.header).map(r=>text(r.columns[col] || [])).filter(Boolean);
              return values.length && values.every(v=>/^[\s−+\-–—\d.,%‰$€£¥()\[\]a-z²³]+$/i.test(v) && /\d/.test(v));
            }).reduce((mask,yes,i)=>mask|(yes?1<<i:0),0);
            if(headers.some(c=>c?.length))emit({kind:'table',gap:0,table:group,header:true,columns:Array.from({length:columns},(_,i)=>headers[i] || []),numeric});
            for(let i=0;i<pending.length;i++) {
              const row=pending[i];
              emit({kind:'table',gap:i===pending.length-1?1:0,table:group,header:false,numeric,
                columns:Array.from({length:columns},(_,j)=>row.columns[j] || [])},row.node);
            }
          }
          pending=[];headers=[];group=++tableID;
        }
        for(const row of rows) {
          if(skip(row))continue;
          const cells=[...row.children].filter(c=>c.tagName==='TH' || c.tagName==='TD');
          if(!cells.length)continue;
          // Image/caption rows and section labels live outside the facts grid,
          // so there is never a table enclosing another box or thumbnail.
          if(cells.length===1 && !carry.some(c=>c?.remaining) && (infobox || Number(cells[0].getAttribute('colspan'))>1)) {
            flushRows();carry=[];
            const cell=cells[0];
            if(cell.querySelector('img')) {
              const runs=inline(cell);if(runs.length)emit({kind:'figure',gap:1,runs},cell);
            } else if(text(inline(cell)) && text(inline(cell))!==titleText)block('caption',cell,{gap:infobox?0:1,runs:inline(cell).map(r=>({...r,bold:cell.tagName==='TH',dim:cell.tagName!=='TH'}))});
            continue;
          }
          let columns=[],occupied=[],nextCarry=[];
          carry.forEach((v,col)=>{if(v?.remaining){columns[col]=v.runs;occupied[col]=true;if(v.remaining>1)nextCarry[col]={...v,remaining:v.remaining-1};}});
          let col=0;
          for(const cell of cells) {
            while(occupied[col])col++;
            const colspan=Math.min(32,Math.max(1,Number(cell.getAttribute('colspan'))||1));
            const rowspan=Math.min(1000,Math.max(1,Number(cell.getAttribute('rowspan'))||1));
            const runs=inline(cell,{images:false,breaks:true});
            for(let j=0;j<colspan;j++) {
              // A spanning header names each descendant column; a spanning
              // value appears once with open remaining cells.
              const content=j===0 || cell.tagName==='TH'?runs:[];
              columns[col+j]=content;occupied[col+j]=true;
              if(rowspan>1)nextCarry[col+j]={runs:content,remaining:rowspan-1};
            }
            col+=colspan;
          }
          carry=nextCarry;
          const header=!infobox && cells.every(c=>c.tagName==='TH') && !pending.length;
          if(header) {
            columns.forEach((runs,i)=>{
              if(!text(runs))return;
              if(!headers[i]?.length)headers[i]=runs;
              else if(text(headers[i])!==text(runs))headers[i]=[...headers[i],plain(' / ',null,{dim:true}),...runs];
            });
          } else pending.push({columns,node:row});
        }
        // A table containing only headings still contains readable information.
        if(!pending.length && headers.length)pending.push({columns:headers,node:table});
        flushRows();
      }
      function visit(node,depth=0,listDepth=0) {
        if(!node || depth>80)return;
        if(node.nodeType===3) {
          if(compact(node.nodeValue))emit({kind:'paragraph',gap:1,runs:[plain(compact(node.nodeValue),node.parentElement)]});
          return;
        }
        if(node.nodeType!==1 || skip(node) || node.hidden || node.getAttribute('aria-hidden')==='true')return;
        const tag=node.tagName.toUpperCase(),style=getComputedStyle(node);
        if(style.display==='none' || style.visibility==='hidden' || ['SCRIPT','STYLE','NOSCRIPT','TEMPLATE'].includes(tag))return;
        if(node.matches('.hatnote,.shortdescription,.ambox,.mbox-small,.dablink')) {
          emit({kind:'note',gap:1,runs:inline(node,{images:false}).map(r=>({...r,dim:!r.action}))},node);return;
        }
        if(/^H[1-6]$/.test(tag)) {
          if(tag==='H1')return;
          lead=false;finishLead();
          block('heading',node,{rule:tag==='H2',gap:tag==='H2'?1:0});return;
        }
        if(tag==='TABLE') {
          if(lead && node.matches('.infobox'))deferred.push(node);
          else renderTable(node,node.matches('.infobox'));
          return;
        }
        if(tag==='FIGURE' || node.matches('.thumb,.gallerybox')) {figure(node);return;}
        if(tag==='P' || tag==='PRE' || tag==='BLOCKQUOTE' || tag==='DT' || tag==='DD') {
          const runs=tag==='PRE'?[plain(ctx.clean(node.textContent),node)]:inline(node);
          emit({kind:tag==='PRE'?'code':tag==='DT'?'caption':'paragraph',indent:tag==='BLOCKQUOTE'||tag==='DD'?2:0,gap:tag==='DT'?0:1,
            runs:tag==='DT'?runs.map(r=>({...r,bold:true})):runs},node);return;
        }
        if(tag==='UL' || tag==='OL') {
          let number=Math.max(1,Number(node.getAttribute('start'))||1);
          for(const child of node.children)if(child.tagName==='LI') {
            const nested=[...child.querySelectorAll('ul,ol')].filter(list=>list.parentElement?.closest('li')===child);
            const runs=inline(child,{skip:n=>skip(n) || ((n.tagName==='OL' || n.tagName==='UL') && n!==child)});
            const reference=node.matches('.references') || !!node.closest('.reflist');
            const prefix=reference?`[${number}] `:tag==='OL'?`${number}. `:'• ';
            emit({kind:'list',gap:0,hang:Math.min(12,prefix.length),indent:Math.min(10,listDepth*2),runs:[plain(prefix,child,{dim:true}),...runs]},child);
            number++;
            for(const list of nested)visit(list,depth+1,listDepth+1);
          }
          return;
        }
        if(tag==='MATH' || node.matches('.mwe-math-element')) {block('paragraph',node);return;}
        if(tag==='IMG') {block('figure',node);return;}
        if(['INPUT','BUTTON','SELECT','TEXTAREA','FORM'].includes(tag)){ctx.walk(node);return;}
        // MediaWiki's wrapper DIVs/sections are transparent. Runs of inline
        // children stay together rather than turning every span into a row.
        let inlineNodes=[];
        function flushInline() {
          if(!inlineNodes.length)return;
          const runs=inlineNodes.flatMap(n=>n.nodeType===3?[plain(n.nodeValue,node)]:inline(n));
          emit({kind:'paragraph',gap:1,runs},node);inlineNodes=[];
        }
        for(const child of node.childNodes) {
          if(child.nodeType===3 || (child.nodeType===1 && ['A','ABBR','B','CITE','CODE','EM','I','SMALL','SPAN','STRONG','SUB','SUP','TIME'].includes(child.tagName) && !child.matches('.mwe-math-element')))inlineNodes.push(child);
          else {flushInline();visit(child,depth+1,listDepth);}
        }
        flushInline();
      }
      for(const node of root.childNodes)visit(node);
      finishLead();
    },
  });
})();
