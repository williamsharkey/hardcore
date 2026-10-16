/* Site adapters are embedded at build time. More specific helpers take priority.
   Helpers can provide matches(host,path), root(document), skip(element), and
   project(document, {root, emit, inline, id, clean, walk}) for semantic layout. */
globalThis.hardcoreSiteAdapters.push({
  fallback:true,
  matches:()=>true,
  root:document=>document.body,
});
