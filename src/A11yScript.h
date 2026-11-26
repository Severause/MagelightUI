#pragma once
// The page core's accessibility reader (1.10.0; A11yCore.h has the host's rules). Installed in every page after
// window.magelight and idle until the host sends '__a11y' "on", which it does only while an assistive plugin has a
// listener (SetAccessibilityListener). While on, it tells the host what a screen reader would say: the focused
// element's accessible name, role, states and value; a composite's active item (aria-activedescendant, aria-selected,
// aria-current); a field's edits; live-region text (aria-live, role alert / status / log, <output>); a dialog
// opening; and, when asked, the page's title and focus. Each message is one '__a11y' send of a JSON object beginning
// {"v":1,"type":"..." (A11yCore.h kTypes). data-ml-a11y="off" on an element, or <html>, keeps it and what it holds
// out; window.magelight.a11y.announce(text, {assertive}) is a page's own announcement. Same-origin iframes are read
// too. "off" removes every listener and observer it added. The script is a function expression: the host calls it
// with 1 or 0, the state at install. tools/a11y-test runs it in Ultralight's WebKit.

namespace Magelight::A11y {

    inline constexpr const char* kPageScript = R"JS((function(start){'use strict';
var ml=window.magelight;
if(!ml||ml.a11y)return;
var send=ml.send,on=false,docs=[],lastEl=null,lastAt=0,regions=new Map(),dialogs=new WeakSet(),said=new WeakMap();
var TEXT_MAX=600,VALUE_MAX=1000,ANNOUNCE_MAX=2000,NODES_MAX=2000,MSG_BYTES=8000;
// Whitespace collapsed, at most n UTF-16 units, never ending on half of a surrogate pair (JSON would carry it alone).
function cut(s,n){if(s.length<=n)return s;var k=n-1,c=s.charCodeAt(k-1);if(c>=0xD800&&c<=0xDBFF)k--;return s.slice(0,k)+'…';}
function clip(s,n){return cut(String(s==null?'':s).replace(/\s+/g,' ').trim(),n);}
function attr(e,n){return e&&e.getAttribute?e.getAttribute(n):null;}
function tag(e){return e&&e.tagName?e.tagName.toUpperCase():'';}
// Out of the reader: data-ml-a11y="off" on the element, an ancestor, or a frame on the way.
function optedOut(e){for(var i=0;e&&i<256;i++){if(attr(e,'data-ml-a11y')==='off')return true;
  var p=e.parentNode;if(!p&&e.nodeType===9){try{p=e.defaultView&&e.defaultView.frameElement;}catch(x){p=null;}}e=p;}return false;}
function styleOf(e){try{return e.ownerDocument.defaultView.getComputedStyle(e);}catch(x){return null;}}
function hiddenSelf(e){if(e.hidden||attr(e,'aria-hidden')==='true'||e.id==='__ml_tip'||e.id==='__ml_ime')return true;
  var s=styleOf(e);return !!s&&(s.display==='none'||s.visibility==='hidden'||s.visibility==='collapse');}
function hidden(e){for(var i=0;e&&e.nodeType===1&&i<256;i++){if(hiddenSelf(e))return true;e=e.parentNode;}return false;}
// Text a subtree gives an accessible name: its text nodes, an image's alt, a control's value, an element's own label,
// skipping what is hidden. ref: the root is an aria-labelledby / -describedby target, read even when hidden.
function text(n,ref){var budget=0,out='';
  (function walk(x,root){if(!x||budget++>NODES_MAX||out.length>TEXT_MAX)return;
    if(x.nodeType===3){out+=x.data;return;}if(x.nodeType!==1)return;if(!(root&&ref)&&hiddenSelf(x))return;
    var l=attr(x,'aria-label');if(l&&l.trim()){out+=' '+l+' ';return;}
    var t=tag(x);if(t==='IMG'||t==='AREA'){out+=' '+(attr(x,'alt')||'')+' ';return;}
    if(t==='INPUT'){var ty=(x.type||'').toLowerCase();if(ty==='password')return;out+=' '+(ty==='checkbox'||ty==='radio'?'':(x.value||''))+' ';return;}
    if(t==='SELECT'){var o=x.options&&x.options[x.selectedIndex];out+=' '+(o?o.text:'')+' ';return;}
    if(t==='TEXTAREA'||t==='SCRIPT'||t==='STYLE'||t==='TEMPLATE'||t==='NOSCRIPT')return;
    if(t==='BR'){out+=' ';return;}
    var block=t==='DIV'||t==='P'||t==='LI'||t==='TR'||t==='TD'||t==='TH'||/^H[1-6]$/.test(t);if(block)out+=' ';
    for(var c=x.firstChild;c;c=c.nextSibling)walk(c,false);
    if(x.shadowRoot)for(var c2=x.shadowRoot.firstChild;c2;c2=c2.nextSibling)walk(c2,false);
    if(block)out+=' ';})(n,true);
  return clip(out,TEXT_MAX);}
function byIds(e,a){var v=attr(e,a);if(!v)return '';var d=e.ownerDocument,parts=[];
  v.split(/\s+/).forEach(function(id){var r=id&&d.getElementById(id);if(r)parts.push(text(r,true));});return clip(parts.join(' '),TEXT_MAX);}
var IMPLICIT={A:'link',BUTTON:'button',SELECT:'combobox',TEXTAREA:'textbox',H1:'heading',H2:'heading',H3:'heading',H4:'heading',
  H5:'heading',H6:'heading',LI:'listitem',UL:'list',OL:'list',OPTION:'option',DIALOG:'dialog',IMG:'img',NAV:'navigation',
  MAIN:'main',FORM:'form',TABLE:'table',TR:'row',TD:'cell',TH:'columnheader',PROGRESS:'progressbar',METER:'meter',
  OUTPUT:'status',SUMMARY:'button',DETAILS:'group',FIELDSET:'group',HR:'separator',ARTICLE:'article',SECTION:'region'};
var INPUT_ROLE={button:'button',submit:'button',reset:'button',image:'button',checkbox:'checkbox',radio:'radio',range:'slider',
  number:'spinbutton',search:'searchbox',email:'textbox',tel:'textbox',url:'textbox',text:'textbox',password:'textbox'};
function role(e){var r=attr(e,'role');if(r&&r.trim())return r.trim().split(/\s+/)[0].toLowerCase();var t=tag(e);
  if(t==='A')return e.hasAttribute('href')?'link':'';
  if(t==='INPUT')return INPUT_ROLE[(e.type||'text').toLowerCase()]||'textbox';
  if(t==='SELECT')return (e.multiple||e.size>1)?'listbox':'combobox';
  if(e.isContentEditable&&(!e.parentElement||!e.parentElement.isContentEditable))return 'textbox';
  return IMPLICIT[t]||'';}
// The roles named from their content (ARIA's "name from content"); every other role is named by its author only.
var FROM_CONTENT={button:1,link:1,heading:1,option:1,tab:1,menuitem:1,menuitemcheckbox:1,menuitemradio:1,listitem:1,cell:1,
  columnheader:1,rowheader:1,row:1,treeitem:1,checkbox:1,radio:1,switch:1,tooltip:1,gridcell:1};
function isField(e){var t=tag(e);return t==='INPUT'||t==='SELECT'||t==='TEXTAREA'||t==='METER'||t==='PROGRESS';}
function name(e,r){var n=byIds(e,'aria-labelledby');if(n)return n;n=attr(e,'aria-label');if(n&&n.trim())return clip(n,TEXT_MAX);
  var t=tag(e);
  if(isField(e)&&e.labels&&e.labels.length){var ls=[];for(var i=0;i<e.labels.length;i++)ls.push(text(e.labels[i]));n=clip(ls.join(' '),TEXT_MAX);if(n)return n;}
  if(t==='IMG'||(t==='INPUT'&&(e.type||'').toLowerCase()==='image'))return clip(attr(e,'alt')||attr(e,'title')||'',TEXT_MAX);
  if(t==='INPUT'){var ty=(e.type||'').toLowerCase();if(ty==='submit'||ty==='reset'||ty==='button')return clip(e.value||(ty==='reset'?'Reset':ty==='submit'?'Submit':''),TEXT_MAX);}
  if(FROM_CONTENT[r]&&!isField(e)){n=text(e);if(n)return n;}
  if(r==='dialog'||r==='alertdialog'){var h=e.querySelector&&e.querySelector('h1,h2,h3,h4,h5,h6,[role=heading]');if(h)return text(h);}
  n=attr(e,'placeholder')||attr(e,'aria-placeholder');if(n&&n.trim())return clip(n,TEXT_MAX);
  return clip(attr(e,'title')||'',TEXT_MAX);}
function detailsOf(e){var t=tag(e);if(t==='DETAILS')return e;if(t==='SUMMARY'&&e.parentElement&&tag(e.parentElement)==='DETAILS')return e.parentElement;return null;}
function states(e,r){var s=[],v;
  if(e.disabled||attr(e,'aria-disabled')==='true')s.push('disabled');
  v=attr(e,'aria-checked');if(v==='true')s.push('checked');else if(v==='mixed')s.push('mixed');
  else if((r==='checkbox'||r==='radio'||r==='switch')&&tag(e)==='INPUT')s.push(e.checked?'checked':(e.indeterminate?'mixed':'not checked'));
  else if(v==='false')s.push('not checked');
  v=attr(e,'aria-selected');if(v==='true')s.push('selected');
  if(tag(e)==='OPTION'&&e.selected&&attr(e,'aria-selected')==null)s.push('selected');
  v=attr(e,'aria-expanded');if(v==='true')s.push('expanded');else if(v==='false')s.push('collapsed');
  else{var dt=detailsOf(e);if(dt)s.push(dt.open?'expanded':'collapsed');}
  v=attr(e,'aria-pressed');if(v==='true')s.push('pressed');else if(v==='false')s.push('not pressed');else if(v==='mixed')s.push('mixed');
  v=attr(e,'aria-current');if(v&&v!=='false')s.push('current');
  if(e.required||attr(e,'aria-required')==='true')s.push('required');
  if(e.readOnly||attr(e,'aria-readonly')==='true')s.push('read only');
  v=attr(e,'aria-invalid');if(v&&v!=='false')s.push('invalid');
  if(attr(e,'aria-haspopup')&&attr(e,'aria-haspopup')!=='false')s.push('has popup');
  return s;}
function isSecret(e){return tag(e)==='INPUT'&&(e.type||'').toLowerCase()==='password';}
function value(e,r){var t=tag(e);
  if(t==='INPUT'||t==='TEXTAREA'){var ty=(e.type||'').toLowerCase();
    if(ty==='checkbox'||ty==='radio'||ty==='button'||ty==='submit'||ty==='reset'||ty==='image'||ty==='hidden')return null;
    if(isSecret(e))return null;return clip(e.value,VALUE_MAX);}
  if(t==='SELECT'){var o=[];for(var i=0;i<e.options.length&&o.length<20;i++)if(e.options[i].selected)o.push(e.options[i].text);return clip(o.join(', '),VALUE_MAX);}
  var vt=attr(e,'aria-valuetext');if(vt)return clip(vt,VALUE_MAX);
  var vn=attr(e,'aria-valuenow');if(vn!=null)return clip(vn,VALUE_MAX);
  if(t==='PROGRESS'||t==='METER')return clip(String(e.value),VALUE_MAX);
  if(r==='textbox'&&e.isContentEditable)return clip(e.innerText||e.textContent,VALUE_MAX);
  return null;}
var SET_ROLES={option:1,tab:1,listitem:1,menuitem:1,menuitemcheckbox:1,menuitemradio:1,radio:1,treeitem:1,row:1};
function position(e,r,o){var p=attr(e,'aria-posinset'),z=attr(e,'aria-setsize');
  if(p!=null&&z!=null){o.position=+p;o.setSize=+z;return;}
  if(!SET_ROLES[r]||!e.parentElement)return;var k=e.parentElement.children,n=0,at=0;
  for(var i=0;i<k.length&&i<500;i++){var c=k[i];if(role(c)!==r||hiddenSelf(c))continue;n++;if(c===e)at=n;}
  if(at&&n>1){o.position=at;o.setSize=n;}}
function describe(e){var r=role(e),o={name:name(e,r),role:r,states:states(e,r)};
  var v=value(e,r);if(v!=null&&v!==o.name)o.value=v;
  if(isSecret(e)){o.secret=true;o.length=(e.value||'').length;}
  var d=byIds(e,'aria-describedby');if(!d){var ti=attr(e,'title');if(ti&&clip(ti,TEXT_MAX)!==o.name)d=clip(ti,TEXT_MAX);}
  if(d)o.description=d;
  var t=tag(e),lv=attr(e,'aria-level');if(lv)o.level=+lv;else if(/^H[1-6]$/.test(t))o.level=+t.charAt(1);
  position(e,r,o);
  var key=attr(e,'aria-keyshortcuts');if(key)o.shortcut=clip(key,100);
  return o;}
// UTF-8 bytes, the unit the host's cap counts (A11yCore.h kMaxMessageBytes).
function bytes(s){var n=0;for(var i=0;i<s.length;i++){var c=s.charCodeAt(i);
  if(c<0x80)n++;else if(c<0x800)n+=2;else if(c>=0xD800&&c<=0xDBFF){n+=4;i++;}else n+=3;}return n;}
function json(type,o){var j=JSON.stringify(o||{});return '{"v":1,"type":"'+type+'"'+(j.length>2?','+j.slice(1):'}');}
// Every string of o (nested ones too) cut to n units.
function shorten(o,n){for(var k in o){var v=o[k];if(typeof v==='string')o[k]=cut(v,n);else if(v&&typeof v==='object'&&!Array.isArray(v))shorten(v,n);}}
function emit(type,o,from){if(!on)return;if(from&&optedOut(from))return;o=o||{};
  var m;try{m=json(type,o);
    // Over the cap: the parts a screen reader can do without go first, then every string is shortened.
    if(m.length*3>MSG_BYTES&&bytes(m)>MSG_BYTES){delete o.active;delete o.description;if(o.focus)delete o.focus.description;
      m=json(type,o);for(var n=400;bytes(m)>MSG_BYTES&&n>=50;n=Math.floor(n/2)){shorten(o,n);m=json(type,o);}
      if(bytes(m)>MSG_BYTES)return;}}catch(x){return;}
  try{send.call(ml,'__a11y',m);}catch(x){}}
// The element focus is on, through same-origin iframes.
function deepActive(){var e=document.activeElement;
  for(var i=0;e&&i<16&&tag(e)==='IFRAME';i++){var d=null;try{d=e.contentDocument;}catch(x){}if(!d||!d.activeElement)break;e=d.activeElement;}
  return e;}
function isRoot(e){return !e||e.nodeType!==1||e===e.ownerDocument.documentElement||(e===e.ownerDocument.body&&!e.isContentEditable);}
function onFocus(ev){var e=ev.target;if(!e||e.nodeType!==1)return;
  if(tag(e)==='IFRAME'){hookFrame(e);e=deepActive();if(!e||tag(e)==='IFRAME')return;}
  if(isRoot(e))return;var now=Date.now();if(e===lastEl&&now-lastAt<100)return;lastEl=e;lastAt=now;
  var o=describe(e);var ad=attr(e,'aria-activedescendant');
  if(ad){var a=e.ownerDocument.getElementById(ad);if(a)o.active=describe(a);}
  emit('focus',o,e);}
function isText(e){var t=tag(e);if(t==='TEXTAREA')return true;if(e.isContentEditable)return true;
  return t==='INPUT'&&['button','checkbox','radio','submit','reset','file','image','hidden','range','color'].indexOf((e.type||'text').toLowerCase())<0;}
function onInput(ev){var e=ev.target;if(!e||e.nodeType!==1)return;if(isText(e)){var r=role(e),o={name:name(e,r),role:r};
    if(isSecret(e)){o.secret=true;o.length=(e.value||'').length;}else o.value=value(e,r)||'';emit('edit',o,e);return;}
  if(ev.type==='change'||(e.type||'').toLowerCase()==='range')emit('state',describe(e),e);}
// A composite's active item: aria-activedescendant on the focused element, or an item that became selected or current
// in the focused composite (or with nothing focused, a page that moves a highlight without focus).
var COMPOSITE='[role=tablist],[role=listbox],[role=grid],[role=tree],[role=treegrid],[role=menu],[role=menubar],[role=radiogroup],[role=toolbar]';
function nothingFocused(d){var a=d.activeElement;return isRoot(a);}
function onAttr(rec){var e=rec.target,n=rec.attributeName;if(!e||e.nodeType!==1)return;var d=e.ownerDocument,act=d.activeElement;
)JS"
        // (split: MSVC caps one string literal at 16 KB)
        R"JS(  if(n==='aria-activedescendant'){if(e!==act)return;var id=attr(e,n),a=id&&d.getElementById(id);if(a)emit('active',describe(a),a);return;}
  if(n==='aria-selected'||n==='aria-current'){var v=attr(e,n);if(!v||v==='false')return;
    // A roving focus moves focus and the selection together: the focus message already said it.
    if(e===lastEl&&Date.now()-lastAt<100)return;
    var c=e.closest&&e.closest(COMPOSITE);
    if(e===act||(c&&c.contains(act))||(nothingFocused(d)&&!hidden(e)))emit('active',describe(e),e);return;}
  if(n==='open'){if(tag(e)==='DIALOG'){if(e.open)dialogShown(e);else dialogs.delete(e);}
    else if(tag(e)==='DETAILS'&&act&&detailsOf(act)===e)emit('state',describe(act),act);return;}
  if(n==='aria-busy'){if(attr(e,n)!=='true'){var rg=regionOf(e);if(rg)queueAnnounce(rg,rg);}return;}
  // A state of the focused element, or of its active descendant.
  var ad=act&&attr(act,'aria-activedescendant');
  if(e===act||(ad&&e.id===ad))emit('state',describe(e),e);}
// Live regions: what changed in one, read when the region has been quiet for 250 ms (1.5 s at most). Each change keeps
// its node (an added node, a text node whose text changed); a node inside another kept one is read with it, and past
// 50 the kept nodes become their nearest common ancestor. A flush reads only what was not read before: the text nodes
// it has not read, and the end a text node grew by since it was read, so a page that streams text a word at a time is
// read once, in phrases; what a region held before the reader saw it counts as read (markRead). aria-atomic reads the
// whole region each time, as ARIA says.
function liveOf(e){var l=attr(e,'aria-live');if(l)return l==='off'?'':l==='assertive'?'assertive':'polite';
  var r=attr(e,'role');if(r==='alert'||r==='alertdialog')return 'assertive';if(r==='status'||r==='log')return 'polite';
  if(tag(e)==='OUTPUT')return 'polite';return null;}
function regionOf(n){var e=n&&n.nodeType===1?n:n&&n.parentNode;for(var i=0;e&&e.nodeType===1&&i<64;i++){var l=liveOf(e);
    if(l!=null)return l?e:null;e=e.parentNode;}return null;}
function common(a,b,rg){var up=new Set();for(var p=a;p&&p!==rg;p=p.parentNode)up.add(p);
  for(var q=b;q&&q!==rg;q=q.parentNode)if(up.has(q))return q;return rg;}
function queueAnnounce(rg,node){if(hidden(rg))return;
  var q=regions.get(rg),now=Date.now();if(!q){q={nodes:[],whole:false,first:now,timer:0};regions.set(rg,q);}
  if(attr(rg,'aria-atomic')==='true')q.whole=true;
  else if(q.nodes.indexOf(node)<0){if(q.nodes.length<50)q.nodes.push(node);
    else{var c=node;q.nodes.forEach(function(x){if(x.isConnected&&rg.contains(x))c=common(c,x,rg);});q.nodes=[c];}}
  clearTimeout(q.timer);q.timer=setTimeout(function(){flush(rg);},now-q.first>1250?0:250);}
function inside(n,list){for(var p=n.parentNode;p;p=p.parentNode)if(list.indexOf(p)>=0)return true;return false;}
function fresh(t){var cur=t.data,prev=said.get(t);said.set(t,cur);if(prev==null)return cur;if(cur===prev)return '';
  return cur.indexOf(prev)===0?cur.slice(prev.length):cur;}
// What a node holds that was not read yet, skipping what is hidden.
function unread(n){var budget=0,out='';
  var p=n.parentNode;if(p&&p.nodeType===1&&hidden(p))return '';
  (function walk(x){if(!x||budget++>NODES_MAX||out.length>ANNOUNCE_MAX)return;
    if(x.nodeType===3){out+=fresh(x);return;}if(x.nodeType!==1||hiddenSelf(x))return;
    var t=tag(x),l=attr(x,'aria-label')||((t==='IMG'||t==='AREA')?attr(x,'alt'):null);
    if(l&&l.trim()){if(said.get(x)!==l){said.set(x,l);out+=' '+l+' ';}return;}
    if(t==='INPUT'||t==='SELECT'){var v=controlText(x);if(v&&said.get(x)!==v){said.set(x,v);out+=' '+v+' ';}return;}
    if(t==='TEXTAREA'||t==='SCRIPT'||t==='STYLE'||t==='TEMPLATE'||t==='NOSCRIPT')return;
    var block=t==='DIV'||t==='P'||t==='LI'||t==='TR'||t==='TD'||t==='BR'||/^H[1-6]$/.test(t);if(block)out+=' ';
    for(var c=x.firstChild;c;c=c.nextSibling)walk(c);if(block)out+=' ';})(n);
  return clip(out,ANNOUNCE_MAX);}
// What a control in a live region reads as: an input's value (never a password's), a select's chosen option.
function controlText(x){var t=tag(x);if(t==='SELECT'){var o=x.options&&x.options[x.selectedIndex];return o?o.text:'';}
  var ty=(x.type||'').toLowerCase();return ty==='password'||ty==='checkbox'||ty==='radio'||ty==='hidden'?'':(x.value||'');}
// Marks what a region holds as read: its history is not news. Run on the regions a page has when the reader starts,
// and on a region added later (but an alert, which is read as it appears).
function markRead(n){var budget=0;
  (function walk(x){if(!x||budget++>20*NODES_MAX)return;if(x.nodeType===3){said.set(x,x.data);return;}if(x.nodeType!==1)return;
    var t=tag(x),l=attr(x,'aria-label')||((t==='IMG'||t==='AREA')?attr(x,'alt'):null);if(l&&l.trim()){said.set(x,l);return;}
    if(t==='INPUT'||t==='SELECT'){said.set(x,controlText(x));return;}
    for(var c=x.firstChild;c;c=c.nextSibling)walk(c);})(n);}
var LIVE='[aria-live],[role=log],[role=status],[role=alert],[role=alertdialog],output';
function flush(rg){var q=regions.get(rg);if(!q)return;regions.delete(rg);clearTimeout(q.timer);
  if(!on||!rg.isConnected||attr(rg,'aria-busy')==='true'||hidden(rg))return;var t;
  if(q.whole)t=text(rg);else{var parts=[];q.nodes.forEach(function(n){if(!n.isConnected||!rg.contains(n)||inside(n,q.nodes))return;
      var s=unread(n);if(s)parts.push(s);});
    t=clip(parts.join(' '),ANNOUNCE_MAX);}
  if(!t)return;
  var o={text:t,politeness:liveOf(rg)||'polite'},r=attr(rg,'role');if(r)o.role=r;var nm=name(rg,role(rg));if(nm&&nm!==t)o.name=nm;
  emit('announce',o,rg);}
var DIALOG='[role=dialog],[role=alertdialog],dialog[open],[aria-modal=true]';
function dialogShown(e){if(dialogs.has(e)||hidden(e))return;dialogs.add(e);var r=role(e)||'dialog',o={name:name(e,r),role:r};
  var d=byIds(e,'aria-describedby');if(!d&&r==='alertdialog')d=text(e);if(d&&d!==o.name)o.description=d;emit('dialog',o,e);}
function onMutations(list){for(var i=0;i<list.length;i++){var m=list[i];
  if(m.type==='attributes'){if(m.oldValue!==attr(m.target,m.attributeName))onAttr(m);continue;}
  if(m.type==='characterData'){var rg=regionOf(m.target);if(rg)queueAnnounce(rg,m.target);continue;}
  for(var j=0;j<m.removedNodes.length;j++){var rn=m.removedNodes[j];if(rn.nodeType===1&&dialogs.has(rn))dialogs.delete(rn);}
  for(var k=0;k<m.addedNodes.length;k++){var a=m.addedNodes[k];if(a.nodeType!==1&&a.nodeType!==3)continue;
    if(a.nodeType===1){if(a.matches&&a.matches(DIALOG))dialogShown(a);
      var ds=a.querySelectorAll?a.querySelectorAll(DIALOG):[];for(var q=0;q<ds.length&&q<20;q++)dialogShown(ds[q]);
      if(tag(a)==='IFRAME')hookFrame(a);}
    // A region added whole: an alert is read as it appears; any other region's content is its history.
    if(a.nodeType===1){var r0=attr(a,'role');if(r0==='alert'||r0==='alertdialog'){queueAnnounce(a,a);continue;}
      if(liveOf(a)){markRead(a);continue;}
      var inner=a.querySelectorAll?a.querySelectorAll(LIVE):[];for(var z=0;z<inner.length&&z<50;z++)markRead(inner[z]);}
    var reg=regionOf(m.target);if(reg)queueAnnounce(reg,a);}}}
var ATTRS=['aria-activedescendant','aria-selected','aria-current','aria-checked','aria-expanded','aria-pressed','aria-valuenow',
  'aria-valuetext','aria-disabled','aria-invalid','aria-busy','disabled','open'];
function release(h){var d=h.d;try{d.removeEventListener('focusin',onFocus,true);d.removeEventListener('input',onInput,true);
    d.removeEventListener('change',onInput,true);d.removeEventListener('load',onLoad,true);if(h.mo)h.mo.disconnect();}catch(x){}}
function hook(d){if(!d)return;
  // A frame that navigated or was removed leaves a document with no window: let it go.
  docs=docs.filter(function(h){if(h.d===document||h.d.defaultView)return true;release(h);return false;});
  if(docs.some(function(h){return h.d===d;}))return;var h={d:d,mo:null};
  try{d.addEventListener('focusin',onFocus,true);d.addEventListener('input',onInput,true);d.addEventListener('change',onInput,true);
    d.addEventListener('load',onLoad,true);
    h.mo=new d.defaultView.MutationObserver(function(l){try{onMutations(l);}catch(x){}});
    h.mo.observe(d.documentElement||d,{subtree:true,childList:true,characterData:true,attributes:true,attributeOldValue:true,attributeFilter:ATTRS});}catch(x){}
  docs.push(h);try{var lr=d.querySelectorAll(LIVE);for(var j=0;j<lr.length&&j<200;j++)markRead(lr[j]);}catch(x){}
  try{var fs=d.querySelectorAll('iframe');for(var i=0;i<fs.length;i++)hookFrame(fs[i]);}catch(x){}}
function hookFrame(f){var d=null;try{d=f.contentDocument;}catch(x){}if(d&&d.documentElement)hook(d);}
function onLoad(ev){if(tag(ev.target)==='IFRAME')hookFrame(ev.target);}
function unhook(){docs.forEach(release);docs=[];regions.forEach(function(q){clearTimeout(q.timer);});regions.clear();
  dialogs=new WeakSet();said=new WeakMap();lastEl=null;}
function page(){var o={title:clip(document.title,TEXT_MAX)};
  if(!o.title){var h=document.querySelector('h1');if(h)o.title=text(h);}
  var e=deepActive();if(e&&!isRoot(e)&&!optedOut(e))o.focus=describe(e);
  var dl=document.querySelector('[aria-modal=true],dialog[open],[role=alertdialog]');if(dl&&!hidden(dl)&&!optedOut(dl)){var r=role(dl)||'dialog';o.dialog={name:name(dl,r),role:r};}
  emit('page',o,document.documentElement);}
function set(v){if(v===on)return;on=v;if(on)hook(document);else unhook();}
ml.on('__a11y',function(a){if(a==='on')set(true);else if(a==='off')set(false);else if(a==='page'&&on)page();});
Object.defineProperty(ml,'a11y',{value:Object.freeze({
  announce:function(t,opt){emit('announce',{text:clip(t,ANNOUNCE_MAX),politeness:opt&&opt.assertive?'assertive':'polite',source:'page'},null);}})});
if(start)set(true);
}))JS";
}
