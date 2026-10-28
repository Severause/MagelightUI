#pragma once
// The page core's gamepad helper (1.6.0): window.magelight.gamepad, installed in every page after window.magelight and
// idle until the host sends a packet for an opted-in view on the reserved '__gamepad' channel (GamepadCore.h
// PacketJson). In the navigation style it moves focus between the controls of the scopes the page attaches, by
// geometry; in the keys style the host types the pad as keys and the helper only keeps the state (device, glyphs) for
// the page's prompts. tools/gamepad-page-test runs it in Ultralight's WebKit.

namespace Magelight::Gamepad {

    inline constexpr const char* kPageScript = R"JS((function(){'use strict';
var ml=window.magelight;
if(!ml||ml.gamepad)return;
var raw=(window.crypto&&crypto.randomUUID)?crypto.randomUUID():Date.now()+'-'+Math.random().toString(36).slice(2);
var page=String(raw).replace(/[^A-Za-z0-9_-]/g,'').slice(0,80)||'page';
var actions=new Set(),stateHandlers=new Set(),scopes=[],generation=-1,sequence=-1,editing=null,lastX=0,lastY=0;
var state=Object.freeze({enabled:false,active:false,connected:false,style:'keys',mode:'navigation',device:'keyboardMouse',
  glyphFamily:'xbox',bindings:Object.freeze({}),generation:-1});
function request(op,extra){var o={op:op,page:page};if(extra)for(var k in extra)o[k]=extra[k];try{ml.send('__gamepad',JSON.stringify(o));}catch(e){}}
var threw=new WeakSet();
function reportThrow(fn,e){if(threw.has(fn))return;threw.add(fn);console.error('[magelight] gamepad handler threw',e);}
function call(fn,arg){try{return fn(arg)===true;}catch(e){reportThrow(fn,e);return false;}}
function isText(e){return !!e&&(e.isContentEditable||e.tagName==='TEXTAREA'||(e.tagName==='INPUT'&&
  ['button','checkbox','radio','submit','reset','file','image','hidden','range','color'].indexOf(e.type)<0));}
function identity(e){return (e&&e.getAttribute&&(e.getAttribute('data-ml-id')||e.id))||'';}
function top(){for(var i=scopes.length-1;i>=0;--i)if(scopes[i].root.isConnected)return scopes[i];return undefined;}
function marked(e){return e.hasAttribute('data-ml-focusable')||e.getAttribute('role')==='tab';}
function visible(e){
  if(!e||!e.isConnected||(e.tabIndex<0&&!marked(e))||e.matches(':disabled,[aria-disabled=true]')||e.closest('[hidden],[inert],[aria-hidden=true]'))return false;
  var s=getComputedStyle(e);return s.visibility!=='hidden'&&s.visibility!=='collapse'&&e.getClientRects().length>0;}
function candidates(scope){
  if(!scope||!scope.root.isConnected)return [];
  var nodes=null;
  if(scope.getCandidates){try{nodes=scope.getCandidates();}catch(e){reportThrow(scope.getCandidates,e);}}
  if(!nodes||typeof nodes.length!=='number')nodes=scope.root.querySelectorAll('button,input,select,textarea,a[href],[tabindex],[data-ml-focusable]');
  return Array.prototype.filter.call(nodes,function(e){return !!e&&scope.root.contains(e)&&visible(e);});}
// One control is marked at a time: a modal's scope takes the mark from the page under it, and gives it back on dispose.
function mark(e){document.querySelectorAll('[data-ml-focused]').forEach(function(o){if(o!==e)o.removeAttribute('data-ml-focused');});
  if(e)e.setAttribute('data-ml-focused','');}
function focus(scope,e){
  if(!e||!visible(e))return;
  if(editing&&editing!==e)editing=null;
  mark(e);scope.selected=e;scope.selectedId=identity(e);
  // DOM focus only while the pad is in use: a scope that recovers from a DOM change must not pull focus out of a
  // field the player is typing in with the keyboard.
  if(!state.active||state.device!=='gamepad')return;
  if(marked(e)&&!e.hasAttribute('tabindex'))e.setAttribute('tabindex','-1');   // focusable by script, still out of Tab order
  e.focus({preventScroll:true});
  if(e.scrollIntoView)e.scrollIntoView({block:'nearest',inline:'nearest'});}
function recover(scope){
  if(!scope||scope!==top())return;
  // The selection still standing needs no full scan (a held stick recovers every batch).
  if(!scope.getCandidates&&!scope.restore&&scope.selected&&scope.root.contains(scope.selected)&&visible(scope.selected)){
    if(!scope.selected.hasAttribute('data-ml-focused'))mark(scope.selected);
    return;}
  var items=candidates(scope);
  if(!scope.restore&&items.indexOf(scope.selected)>=0){if(!scope.selected.hasAttribute('data-ml-focused'))mark(scope.selected);return;}
  var restore=scope.restore;scope.restore=null;
  var desired=items.indexOf(restore)>=0?restore:scope.selectedId?items.find(function(e){return identity(e)===scope.selectedId;}):null;
  var i=scope.initialFocus,initial=null;
  if(typeof i==='function'){try{initial=i();}catch(e){reportThrow(i,e);}}
  else initial=typeof i==='string'?items.find(function(e){return identity(e)===i;}):i;
  focus(scope,desired||(items.indexOf(initial)>=0?initial:items[0]));}
// WebKit 615 reports an element's rect in the units of the CSS zoom it is drawn at, so a zoomed panel's controls would
// seem nearer than they are: the rect times that zoom is where it is drawn (the page core's IME box does the same).
function zoomOf(e){var z=1;for(;e&&e.nodeType===1;e=e.parentNode){var v=parseFloat(getComputedStyle(e).zoom);if(v>0)z*=v;}return z;}
function centre(e){var r=e.getBoundingClientRect(),z=zoomOf(e);return {x:(r.x+r.width/2)*z,y:(r.y+r.height/2)*z};}
function navigate(scope,dir){
  recover(scope);
  var items=candidates(scope),cur=scope.selected;
  if(!cur||!items.length)return;
  var explicit=cur.getAttribute('data-ml-'+dir);
  if(explicit){var n=items.find(function(e){return identity(e)===explicit;});if(n)focus(scope,n);return;}
  var c=centre(cur),x=c.x,y=c.y;
  var horizontal=dir==='left'||dir==='right',positive=dir==='right'||dir==='down';
  var ranked=items.filter(function(e){return e!==cur;}).map(function(e,index){
    var q=centre(e),dx=q.x-x,dy=q.y-y;
    var along=(horizontal?dx:dy)*(positive?1:-1),across=Math.abs(horizontal?dy:dx);
    return {e:e,index:index,along:along,score:along+across*2};
  }).filter(function(c){return c.along>1;}).sort(function(a,b){return a.score-b.score||a.index-b.index;});
  if(ranked.length)focus(scope,ranked[0].e);
  else if(scope.wrap)focus(scope,positive?items[0]:items[items.length-1]);}
function fire(e,type){e.dispatchEvent(new Event(type,{bubbles:true}));}
function adjust(e,dir){
  var sign=dir==='left'||dir==='down'?-1:1;
  if(e.matches('input[type=range]')){
    var before=e.value;
    try{if(sign>0)e.stepUp();else e.stepDown();}
    catch(x){var min=Number(e.min||0),max=Number(e.max||100),step=Number(e.step||1);
      e.value=String(Math.min(max,Math.max(min,Number(e.value)+sign*(Number.isFinite(step)?step:1))));}
    if(e.value!==before){fire(e,'input');fire(e,'change');}
    return;}
  if(e.matches('select')){
    var d=dir==='up'||dir==='left'?-1:1,n=e.selectedIndex+d;
    while(n>=0&&n<e.options.length&&e.options[n].disabled)n+=d;
    if(n>=0&&n<e.options.length){e.selectedIndex=n;fire(e,'input');fire(e,'change');}}}
function scrollable(t){var s=getComputedStyle(t);
  return /(auto|scroll)/.test(s.overflowY+s.overflowX)&&(t.scrollHeight>t.clientHeight||t.scrollWidth>t.clientWidth);}
function scroll(scope,ev){
  if(!ev.x&&!ev.y)return;
  var t=state.mode==='cursor'?document.elementFromPoint(lastX,lastY):scope.selected;
  if(t&&!scope.root.contains(t))t=null;   // under a modal: the page beneath is not this scope's
  var found=null;
  for(;t&&t!==scope.root;t=t.parentElement)if(scrollable(t)){found=t;break;}
  var r=scope.root,page=r===document.body||r===document.documentElement;
  if(!found)found=page?(document.scrollingElement||r):scrollable(r)?r:null;
  if(!found)return;
  t=found;
  var left=ev.x*600*ev.dt,topBy=-ev.y*600*ev.dt;
  try{t.scrollBy({left:left,top:topBy,behavior:'instant'});}catch(e){t.scrollBy(left,topBy);}}
function defaultAction(ev){
  var scope=top();if(!scope)return;
  if(ev.phase!=='change')recover(scope);
  if(scope.onAction&&call(scope.onAction,ev))return;
  if(ev.control==='rightStick'&&ev.phase==='change'){scroll(scope,ev);return;}
  var a=ev.action;
  if(ev.phase!=='press'&&ev.phase!=='repeat')return;
  if((a==='previousTab'||a==='nextTab')&&ev.phase==='press'){
    var sel=scope.selected,list=(sel&&sel.closest&&sel.closest('[role=tablist]'))||scope.root.querySelector('[role=tablist]')||scope.root;
    var tabs=Array.prototype.filter.call(list.querySelectorAll('[role=tab]'),function(e){
      return scope.root.contains(e)&&!e.matches(':disabled,[aria-disabled=true]')&&!e.closest('[hidden],[inert],[aria-hidden=true]')&&e.getClientRects().length>0;});
    var i=tabs.findIndex(function(e){return e.getAttribute('aria-selected')==='true';});
    var j=i<0?0:i+(a==='nextTab'?1:-1);
    if(j<0||j>=tabs.length||j===i)return;
    tabs[j].click();focus(scope,tabs[j]);
    return;}
  if(a==='cancel'){
    if(ev.phase!=='press')return;
    if(editing){if(editing.blur)editing.blur();editing=null;return;}
    if(scope.onBack)call(scope.onBack,ev);
    return;}
  if(a==='up'||a==='down'||a==='left'||a==='right'){
    if(editing){if(!isText(editing))adjust(editing,a);return;}
    navigate(scope,a);return;}
  if(a==='accept'&&ev.phase==='press'&&state.mode==='navigation'){
    var sel=scope.selected;
    if(!sel||!visible(sel))return;
    if(isText(sel)||sel.matches('input[type=range],select')){
      if(editing===sel){editing=null;sel.blur();}else{editing=sel;sel.focus();}}
    else sel.click();}}
document.addEventListener('mousemove',function(e){lastX=e.clientX;lastY=e.clientY;},true);
document.addEventListener('mousedown',function(e){editing=isText(e.target)?e.target:null;},true);
document.addEventListener('focusin',function(e){
  var scope=top();
  if(scope&&candidates(scope).indexOf(e.target)>=0){
    mark(e.target);scope.selected=e.target;scope.selectedId=identity(e.target);
    if(editing&&editing!==e.target)editing=null;}});
document.addEventListener('keydown',function(){if(isText(document.activeElement))editing=document.activeElement;},true);
window.addEventListener('blur',function(){editing=null;});
function receive(p){
  if(!p||p.version!==1||p.page!==page||!Number.isSafeInteger(p.generation)||p.generation<generation||
     !Number.isSafeInteger(p.sequence)||p.sequence<=sequence||!Array.isArray(p.events)||p.events.length>128)return;
  if(generation>=0&&(p.generation!==generation||p.reset||(state.active&&!p.active))){
    var cancel=Object.freeze({control:'none',action:'none',phase:'cancel',generation:p.generation,sequence:p.sequence,x:0,y:0,value:0,dt:0});
    actions.forEach(function(h){call(h,cancel);});
    scopes.slice().forEach(function(s){if(s.onAction)call(s.onAction,cancel);});}
  if(p.generation!==generation||p.reset||!p.active)editing=null;
  generation=p.generation;sequence=p.sequence;
  var prev=JSON.stringify(state),next={};
  for(var k in state)next[k]=state[k];
  ['enabled','connected','active','style','mode','device','glyphFamily'].forEach(function(k){if(Object.prototype.hasOwnProperty.call(p,k))next[k]=p[k];});
  if(p.bindings&&typeof p.bindings==='object'){var b={};for(var key in p.bindings)b[key]=p.bindings[key];next.bindings=Object.freeze(b);}
  next.generation=generation;
  state=Object.freeze(next);
  document.documentElement.setAttribute('data-ml-input',state.device);
  document.documentElement.setAttribute('data-ml-gamepad-mode',state.mode);
  if(JSON.stringify(state)!==prev)stateHandlers.forEach(function(h){call(h,state);});
  if(!state.active||state.style!=='navigation')return;
  recover(top());
  var dt=Math.max(0,Math.min(0.05,Number(p.dt)||0));
  for(var i=0;i<p.events.length;++i){
    var r=p.events[i];
    if(!r||typeof r.control!=='string'||typeof r.action!=='string'||['press','release','repeat','change','cancel'].indexOf(r.phase)<0)continue;
    if(!Number.isFinite(r.x)||!Number.isFinite(r.y)||!Number.isFinite(r.value))continue;
    var ev=Object.freeze({control:r.control,action:r.action,phase:r.phase,value:r.value,x:r.x,y:r.y,generation:generation,sequence:sequence,dt:dt});
    var handled=false;
    for(var h of actions)if(call(h,ev)){handled=true;break;}
    if(!handled)defaultAction(ev);}}
function attachNavigation(o){
  if(!o||!o.root||!o.root.querySelectorAll)throw new TypeError('magelight.gamepad.attachNavigation: a root element is required');
  var scope={};for(var k in o)scope[k]=o[k];scope.selected=null;scope.selectedId='';
  scopes.push(scope);recover(scope);
  var observer=new MutationObserver(function(){recover(scope);});
  observer.observe(scope.root,{childList:true,subtree:true,attributes:true,
    attributeFilter:['disabled','hidden','inert','aria-disabled','class','style','tabindex']});
  var disposed=false;
  return function dispose(){
    if(disposed)return;disposed=true;observer.disconnect();
    var at=scopes.indexOf(scope);
    var wasTop=at>=0&&scopes.slice(at+1).every(function(s){return !s.root.isConnected;});
    if(at>=0)scopes.splice(at,1);
    scope.root.querySelectorAll('[data-ml-focused]').forEach(function(e){e.removeAttribute('data-ml-focused');});
    if(editing&&scope.root.contains(editing))editing=null;
    if(wasTop){var parent=top();if(parent){parent.restore=parent.selected;parent.selected=null;recover(parent);}}};}
var labels={
  xbox:{south:'A',east:'B',west:'X',north:'Y',leftShoulder:'LB',rightShoulder:'RB',leftTrigger:'LT',rightTrigger:'RT',leftThumb:'LS',rightThumb:'RS',start:'Menu',back:'View'},
  playstation:{south:'Cross',east:'Circle',west:'Square',north:'Triangle',leftShoulder:'L1',rightShoulder:'R1',leftTrigger:'L2',rightTrigger:'R2',leftThumb:'L3',rightThumb:'R3',start:'Options',back:'Share'},
  generic:{south:'South',east:'East',west:'West',north:'North'}};
var dpad={dpadUp:'D-pad Up',dpadDown:'D-pad Down',dpadLeft:'D-pad Left',dpadRight:'D-pad Right'};
function subscribe(set,h){if(typeof h!=='function')throw new TypeError('magelight.gamepad: a handler function is required');set.add(h);return function(){set.delete(h);};}
Object.defineProperty(ml,'gamepad',{enumerable:true,value:Object.freeze({
  version:1,
  getState:function(){return state;},
  onAction:function(h){return subscribe(actions,h);},
  onStateChange:function(h){return subscribe(stateHandlers,h);},
  setMode:function(m){if(m!=='navigation'&&m!=='cursor')throw new TypeError('magelight.gamepad.setMode: navigation or cursor');request('mode',{mode:m});},
  getPrompt:function(a){var c=state.bindings[a]||'none',f=labels[state.glyphFamily]||labels.generic;
    return Object.freeze({action:a,control:c,family:state.glyphFamily,label:c==='none'?'':f[c]||dpad[c]||c});},
  attachNavigation:attachNavigation})});
ml.on('__gamepad',function(arg){var p;try{p=typeof arg==='string'?JSON.parse(arg):arg;}catch(e){return;}receive(p);});
request('ready');
})();)JS";

}  // namespace Magelight::Gamepad
