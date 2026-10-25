// inputprobe - what a page sees from the input Magelight hands Ultralight 2.0, offline on a CPU view. The host's own
// functions (src/InputCore.h) build the events. Sections:
//   paths    keys through Input::MakeKeyEvent as the flat window and the VR paths queue them (checked)
//   polled   the polled-keys filter around the constructor: who reads which key state, and what it hides (checked)
//   release  the modifier and button releases the host sends when focus goes (checked)
//   leave    a move outside the view as the leave the host sends a view the pointer left (checked)
//   guard    the page core's pointer guard, Input::kPointerGuardScript (checked)
//   routes   which view each pointer source's events go to, Input::PointerRoutes, with two VR hands (checked)
//   keys, dead, state, mouse, wheel   the matrix the design was made from (printed, a few checks): the events
//            Magelight 1.x built by hand, the bare 2.0 constructor and MakeKeyEvent side by side; the es-419 layout's
//            AltGr, ñ and dead key, and the dead key a layout holds; when the constructor reads the keyboard state;
//            what Ultralight does with mouse buttons, click counts, a lost release, the wheel's x/y
//   polledcontrol  the polled section run from a folder the filter does not exempt (build.bat runs it): the reads
//            it hides there, which shows the exemption is what keeps the page's modifiers
// Usage: inputprobe <sdkDir> [section ...] (default: every section but polledcontrol). Exit code = failed checks.
#include <windows.h>
#include <intrin.h>
#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>

#include "InputCore.h"

#ifndef MG_NO_MINHOOK
#include <MinHook.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>
#include "../fiber-gc-test/FiberRun.h"

using namespace ultralight;
namespace In = Magelight::Input;

static const char* kPage = R"(<!doctype html><html><head><style>
body{margin:0;font:14px sans-serif}
#t{position:absolute;left:10px;top:10px;width:300px;height:24px}
#outer{position:absolute;left:10px;top:50px;width:300px;height:200px;overflow:auto;background:#eee}
#inner{margin-top:60px;width:250px;height:100px;overflow:auto;background:#ccc}
#other{position:absolute;left:350px;top:50px;width:300px;height:200px;overflow:auto;background:#ddd}
#btn{position:absolute;left:10px;top:300px;width:200px;height:60px;background:#bbf}
#btn:hover{background:#88f}
#sel{position:absolute;left:350px;top:300px;width:300px;height:80px}
#pb{position:absolute;left:10px;top:400px;width:100px;height:30px}
#la{position:absolute;left:150px;top:400px;width:100px;height:30px;display:block}
#ak{position:absolute;left:300px;top:400px;width:100px;height:30px}
#tall{position:absolute;left:0;top:0;width:1px;height:3000px}
</style></head><body>
<div id="tall"></div>
<input id="t">
<div id="outer"><div id="inner"><div id="innerc" style="height:800px">inner</div></div><div id="outerc" style="height:800px">outer filler</div></div>
<div id="other"><input id="t2"><div id="otherc" style="height:800px">other</div></div>
<div id="btn">button area</div>
<div id="sel">selectable text to select by dragging across it and more words here</div>
<button id="pb">press</button><a id="la" href="#went">link</a><button id="ak" accesskey="x">access</button>
<script>
window.L=[];window.__pb=0;window.__ak=0;
document.getElementById('pb').onclick=function(){window.__pb++;};
document.getElementById('ak').onclick=function(){window.__ak++;};
function tg(e){return e.target&&(e.target.id||e.target.tagName);}
['keydown','keypress','keyup'].forEach(function(n){document.addEventListener(n,function(e){L.push({t:n,key:e.key,code:e.code,kc:e.keyCode,cc:e.charCode,
  loc:e.location,rep:e.repeat,s:e.shiftKey,c:e.ctrlKey,a:e.altKey,m:e.metaKey,ag:e.getModifierState?e.getModifierState('AltGraph'):false});},true);});
document.addEventListener('input',function(e){L.push({t:'input',data:e.data});},true);
['mousedown','mouseup','click','auxclick','dblclick','contextmenu','mousemove','mouseover','mouseout','mouseleave'].forEach(function(n){document.addEventListener(n,function(e){
  L.push({t:n,b:e.button,bs:e.buttons,d:e.detail,x:e.clientX,y:e.clientY,s:e.shiftKey,c:e.ctrlKey,a:e.altKey,tr:e.isTrusted,tg:tg(e)});},true);});
document.addEventListener('wheel',function(e){L.push({t:'wheel',dy:e.deltaY,wd:e.wheelDeltaY,x:e.clientX,y:e.clientY,tg:tg(e)});},true);
document.addEventListener('focus',function(e){L.push({t:'focus',tg:tg(e)});},true);
function mods(o){return (o.s?'S':'')+(o.c?'C':'')+(o.a?'A':'')+(o.m?'M':'');}
function K(){return L.filter(function(o){return o.t.indexOf('key')==0||o.t=='input';}).map(function(o){
  if(o.t.indexOf('key')==0) return o.t.replace('key','')+'{'+JSON.stringify(o.key)+' '+(o.code||'""')+' kc'+o.kc+(o.t=='keypress'?' cc'+o.cc:'')+' L'+o.loc+(o.rep?' R':'')+
     (mods(o)?' '+mods(o):'')+(o.ag?' AG':'')+'}';
  return 'input('+JSON.stringify(o.data)+')';}).join(' ')+'  || value='+JSON.stringify(document.activeElement?document.activeElement.value:null);}
function KEV(t,code){for(var i=0;i<L.length;i++){var o=L[i];if(o.t==t&&o.code==code)return JSON.stringify(o.key)+'|'+o.code+'|L'+o.loc+'|'+mods(o)+(o.rep?'|R':'');}return 'none';}
function KP(){for(var i=0;i<L.length;i++){var o=L[i];if(o.t=='keypress')return JSON.stringify(o.key)+'|'+(o.code||'-')+'|'+mods(o);}return 'none';}
function VAL(){return JSON.stringify(document.activeElement?document.activeElement.value:null);}
function M(){return L.filter(function(o){return o.t!='focus'&&o.t!='mouseover'&&o.t!='mouseout'&&o.t!='mouseleave';}).map(function(o){
  if(o.t=='wheel') return 'wheel{tg='+o.tg+' dy='+o.dy+' at '+o.x+','+o.y+'}';
  return o.t+(o.tr===false?'*':'')+'{b'+o.b+' bs'+o.bs+' d'+o.d+' '+o.x+','+o.y+' '+o.tg+((o.s||o.c||o.a)?' '+(o.s?'S':'')+(o.c?'C':'')+(o.a?'A':''):'')+'}';}).join(' ');}
function CNT(t){return L.filter(function(o){return o.t==t;}).length;}
function ST(){return 'outer='+document.getElementById('outer').scrollTop+' inner='+document.getElementById('inner').scrollTop+
  ' other='+document.getElementById('other').scrollTop+' doc='+document.scrollingElement.scrollTop;}
</script></body></html>)";

static int g_fails = 0;
static void Expect(const std::string& name, const std::string& got, const std::string& want)
{
    const bool ok = got == want;
    if (!ok) ++g_fails;
    std::printf("%s  %-58s %s%s%s\n", ok ? "ok  " : "FAIL", name.c_str(), got.c_str(), ok ? "" : "   want ", ok ? "" : want.c_str());
}

struct Settle : LoadListener { bool done = false; void OnDOMReady(View*, uint64_t, bool, const String&) override { done = true; } };

static RefPtr<Renderer> g_r;
static RefPtr<View> g_v;
static void Pump(int n = 3) { for (int i = 0; i < n; ++i) { g_r->Update(); g_r->RefreshDisplay(0); g_r->Render(); FiberRun::BetweenFrames(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); } }
static std::string Js(const std::string& s) { String ex; String out = g_v->EvaluateScript(String(s.c_str()), &ex); return std::string(out.utf8().data()); }
static void Wait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static RefPtr<View> NewView()
{
    ViewConfig vc;
    vc.is_accelerated = false;
    RefPtr<View> v = g_r->CreateView(700, 500, vc, nullptr);
    static Settle ld;
    ld.done = false;
    v->set_load_listener(&ld);
    v->LoadHTML(kPage, "file:///page.html");
    g_v = v;
    for (int i = 0; i < 400 && !ld.done; ++i) Pump(1);
    Pump(20);
    v->Focus();
    Pump();
    return v;
}

// ── Messages ─────────────────────────────────────────────────────────────
struct Msg { UINT msg; WPARAM w; LPARAM l; };
static LPARAM KL(UINT scan, bool ext, bool up, bool repeat = false, bool ctx = false)
{
    LPARAM l = 1 | (static_cast<LPARAM>(scan) << 16) | (ext ? (1 << 24) : 0) | (ctx ? (1 << 29) : 0);
    if (up) l |= (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
    else if (repeat) l |= (static_cast<LPARAM>(1) << 30);
    return l;
}
static Msg KD(UINT vk, UINT scan, bool ext = false, bool rep = false) { return { WM_KEYDOWN, vk, KL(scan, ext, false, rep) }; }
static Msg KU(UINT vk, UINT scan, bool ext = false) { return { WM_KEYUP, vk, KL(scan, ext, true) }; }
static Msg SKD(UINT vk, UINT scan, bool ext = false) { return { WM_SYSKEYDOWN, vk, KL(scan, ext, false, false, true) }; }
static Msg SKU(UINT vk, UINT scan, bool ext = false) { return { WM_SYSKEYUP, vk, KL(scan, ext, true, false, false) }; }
static Msg CH(wchar_t c, UINT scan = 0) { return { WM_CHAR, static_cast<WPARAM>(c), scan ? KL(scan, false, false) : 0 }; }

static bool IsKeyMsg(UINT m) { return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP; }
static KeyEvent::Type TypeOf(UINT msg)
{
    return (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? KeyEvent::kType_RawKeyDown
         : (msg == WM_KEYUP || msg == WM_SYSKEYUP) ? KeyEvent::kType_KeyUp : KeyEvent::kType_Char;
}

// The modifier state a window message sequence implies, as GetKeyState reads it while the window proc handles each
// message (the host's CaptureKeyBits(true)).
struct ModTrack {
    std::uint16_t bits = In::kKbNumLock;
    void Apply(const Msg& m)
    {
        const bool down = (m.msg == WM_KEYDOWN || m.msg == WM_SYSKEYDOWN);
        if (!IsKeyMsg(m.msg)) return;
        const UINT scan = (m.l >> 16) & 0xFF; const bool ext = (m.l >> 24) & 1;
        std::uint16_t b = 0;
        if (m.w == VK_SHIFT) b = scan == 0x36 ? In::kKbRShift : In::kKbLShift;
        if (m.w == VK_CONTROL) b = ext ? In::kKbRCtrl : In::kKbLCtrl;
        if (m.w == VK_MENU) b = ext ? In::kKbRAlt : In::kKbLAlt;
        if (m.w == VK_LWIN) b = In::kKbLWin;
        if (m.w == VK_RWIN) b = In::kKbRWin;
        bits = static_cast<std::uint16_t>(down ? (bits | b) : (bits & ~b));
    }
    unsigned Mods() const { return In::ModsFromKeyBits(bits); }
    // The thread's own state: in sync with the sequence (as a window thread) or all up (the VR window thread).
    void ToThread(bool sync) const
    {
        BYTE ks[256]{};
        if (sync) In::ApplyKeyBits(ks, bits);
        ks[VK_NUMLOCK] = 1;
        SetKeyboardState(ks);
    }
};

static void ThreadAllUp() { BYTE ks[256]{}; ks[VK_NUMLOCK] = 1; SetKeyboardState(ks); }

// What the host fires for one queued message (DrainInputQueue's key cases): every key event through MakeKeyEvent,
// with the bits captured when it was queued; the window's control characters dropped. Each one also checks that
// MakeKeyEvent left the thread's whole keyboard state as it found it.
struct HostMsg { UINT msg; WPARAM w; LPARAM l; std::uint16_t keys; };
static int g_made = 0, g_notApplied = 0, g_notRestored = 0;
static void FireHost(const HostMsg& e)
{
    if (e.msg == WM_CHAR) {
        const Char16 wch = static_cast<Char16>(e.w);
        if (wch < 0x20 && wch != L'\t' && wch != L'\r' && wch != L'\n') return;
    }
    const bool sys = e.msg == WM_SYSKEYDOWN || e.msg == WM_SYSKEYUP;
    bool applied = false;
    BYTE before[256]{}, after[256]{};
    GetKeyboardState(before);
    const KeyEvent evt = In::MakeKeyEvent(TypeOf(e.msg), e.w, e.l, sys, e.keys, &applied);
    GetKeyboardState(after);
    ++g_made;
    if (!applied) { ++g_notApplied; std::printf("      !! the thread's keyboard state could not be read\n"); }
    if (std::memcmp(before, after, sizeof before) != 0) { ++g_notRestored; std::printf("      !! the thread's state was not put back\n"); }
    g_v->FireKeyEvent(evt);
    Pump(1);
}

static std::string Hex(const String& s)
{
    std::string u = s.utf8().data(), out;
    for (unsigned char c : u) { char b[4]; std::snprintf(b, sizeof b, "%02X", c); out += b; }
    return out.empty() ? "-" : out;
}

static std::string Fields(const KeyEvent& e)
{
    char b[512];
    std::snprintf(b, sizeof b, "type=%d mods=%u vk=0x%02X native=0x%X kid='%s' text=%s utext=%s keypad=%d rep=%d sys=%d pe=%d",
                  static_cast<int>(e.type), e.modifiers, e.virtual_key_code, e.native_key_code, e.key_identifier.utf8().data(),
                  Hex(e.text).c_str(), Hex(e.unmodified_text).c_str(), e.is_keypad, e.is_auto_repeat, e.is_system_key,
                  e.platform_event != nullptr);
    return b;
}

// ── keys: the matrix ─────────────────────────────────────────────────────
enum class Build { Old, New, NewFix, Scoped };

// The hand-filled events Magelight 1.x built (Magelight.cpp's DrainInputQueue before 1.5.0, in substance), except
// that it also takes WM_SYSKEY* (1.x dropped those at the window proc).
static bool OldEvt(const Msg& e, unsigned mods, KeyEvent& evt)
{
    switch (e.msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        evt.type = KeyEvent::kType_RawKeyDown;
        evt.virtual_key_code = static_cast<int>(e.w);
        evt.native_key_code = static_cast<int>((e.l >> 16) & 0x1FF);
        GetKeyIdentifierFromVirtualKeyCode(evt.virtual_key_code, evt.key_identifier);
        evt.modifiers = mods;
        evt.is_system_key = (e.msg == WM_SYSKEYDOWN);
        evt.is_auto_repeat = (e.l & (1 << 30)) != 0;
        evt.is_keypad = false;
        return true;
    case WM_KEYUP: case WM_SYSKEYUP:
        evt.type = KeyEvent::kType_KeyUp;
        evt.virtual_key_code = static_cast<int>(e.w);
        evt.native_key_code = static_cast<int>((e.l >> 16) & 0x1FF);
        GetKeyIdentifierFromVirtualKeyCode(evt.virtual_key_code, evt.key_identifier);
        evt.modifiers = mods;
        evt.is_system_key = (e.msg == WM_SYSKEYUP);
        evt.is_keypad = false;
        return true;
    case WM_CHAR: {
        const Char16 wch = static_cast<Char16>(e.w);
        if (wch < 0x20 && wch != L'\t' && wch != L'\r' && wch != L'\n') return false;
        evt.type = KeyEvent::kType_Char;
        evt.modifiers = mods;
        evt.text = String(String16(&wch, 1));
        evt.unmodified_text = evt.text;
        return true;
    }
    }
    return false;
}

static void RunSeq(const char* name, const std::vector<Msg>& seq, Build b, bool sync, bool showFields, const char* field = "t")
{
    Js(std::string("document.getElementById('") + field + "').value='';document.getElementById('" + field + "').focus();L.length=0;");
    Pump();
    ModTrack mt;
    for (const Msg& e : seq) {
        mt.Apply(e);
        mt.ToThread(sync);
        if (e.msg == WM_CHAR) {
            const Char16 wch = static_cast<Char16>(e.w);
            if (wch < 0x20 && wch != L'\t' && wch != L'\r' && wch != L'\n') continue;   // Magelight's filter, both builds
        }
        if (b == Build::Old) {
            KeyEvent evt;
            if (!OldEvt(e, mt.Mods(), evt)) continue;
            g_v->FireKeyEvent(evt);
            Pump(1);
        } else if (b == Build::Scoped) {
            mt.ToThread(false);   // the thread knows nothing (VR); the bits carry the state
            FireHost({ e.msg, e.w, e.l, mt.bits });
        } else {
            KeyEvent evt(TypeOf(e.msg), e.w, e.l, e.msg == WM_SYSKEYDOWN || e.msg == WM_SYSKEYUP);
            if (showFields) std::printf("      ctor %-12s %s\n", e.msg == WM_CHAR ? "WM_CHAR" : (e.msg == WM_KEYUP || e.msg == WM_SYSKEYUP) ? "up" : "down", Fields(evt).c_str());
            if (b == Build::NewFix) {
                evt.modifiers = mt.Mods();
                if (evt.type == KeyEvent::kType_KeyUp) evt.is_auto_repeat = false;
            }
            g_v->FireKeyEvent(evt);
            Pump(1);
        }
    }
    Pump();
    const char* bn = b == Build::Old ? "old    " : b == Build::New ? "new    " : b == Build::Scoped ? "scoped " : "newfix ";
    std::printf("  %-22s %s%s %s\n", name, bn, sync ? "sync  " : "nosync", Js("K()").c_str());
    ThreadAllUp();
}

struct Case { const char* name; std::vector<Msg> seq; };

static HKL FindLayout(WORD layoutLo, WORD langLo)
{
    HKL list[32]; const int n = GetKeyboardLayoutList(32, list);
    for (int i = 0; i < n; ++i) {
        const auto v = reinterpret_cast<uintptr_t>(list[i]);
        if (LOWORD(v) == langLo || HIWORD(v) == layoutLo) return list[i];
    }
    return nullptr;
}
static HKL UseUs()
{
    HKL us = FindLayout(0x0409, 0x0409);
    if (us) ActivateKeyboardLayout(us, 0);
    return us;
}

static void KeysTest()
{
    std::printf("== keys (US layout): 1.x's hand-filled events, the bare constructor, MakeKeyEvent (scoped)\n");
    UseUs();
    std::printf("  active layout %p\n", static_cast<void*>(GetKeyboardLayout(0)));
    std::vector<Case> cases = {
        { "a", { KD('A', 0x1E), CH('a', 0x1E), KU('A', 0x1E) } },
        { "Shift+a", { KD(VK_SHIFT, 0x2A), KD('A', 0x1E), CH('A', 0x1E), KU('A', 0x1E), KU(VK_SHIFT, 0x2A) } },
        { "1", { KD('1', 0x02), CH('1', 0x02), KU('1', 0x02) } },
        { "Numpad5(NumLock)", { KD(VK_NUMPAD5, 0x4C), CH('5', 0x4C), KU(VK_NUMPAD5, 0x4C) } },
        { "Numpad4(NumLockOff)", { KD(VK_LEFT, 0x4B), KU(VK_LEFT, 0x4B) } },
        { "ArrowLeft", { KD(VK_LEFT, 0x4B, true), KU(VK_LEFT, 0x4B, true) } },
        { "ArrowLeft(noext)", { KD(VK_LEFT, 0x4B, false), KU(VK_LEFT, 0x4B, false) } },
        { "Enter", { KD(VK_RETURN, 0x1C), CH('\r', 0x1C), KU(VK_RETURN, 0x1C) } },
        { "NumpadEnter", { KD(VK_RETURN, 0x1C, true), CH('\r', 0x1C), KU(VK_RETURN, 0x1C, true) } },
        { "F5", { KD(VK_F5, 0x3F), KU(VK_F5, 0x3F) } },
        { "F10(sys)", { SKD(VK_F10, 0x44), SKU(VK_F10, 0x44) } },
        { "ShiftLeft", { KD(VK_SHIFT, 0x2A), KU(VK_SHIFT, 0x2A) } },
        { "ShiftRight", { KD(VK_SHIFT, 0x36), KU(VK_SHIFT, 0x36) } },
        { "ControlRight", { KD(VK_CONTROL, 0x1D, true), KU(VK_CONTROL, 0x1D, true) } },
        { "AltLeft(sys)", { SKD(VK_MENU, 0x38), SKU(VK_MENU, 0x38) } },
        { "Alt+x(sys)", { SKD(VK_MENU, 0x38), SKD('X', 0x2D), SKU('X', 0x2D), SKU(VK_MENU, 0x38) } },
        { "Escape", { KD(VK_ESCAPE, 0x01), CH(0x1B, 0x01), KU(VK_ESCAPE, 0x01) } },
        { "Space", { KD(VK_SPACE, 0x39), CH(' ', 0x39), KU(VK_SPACE, 0x39) } },
        { "Delete", { KD(VK_DELETE, 0x53, true), KU(VK_DELETE, 0x53, true) } },
        { "Backquote", { KD(VK_OEM_3, 0x29), CH('`', 0x29), KU(VK_OEM_3, 0x29) } },
        { "a repeat", { KD('A', 0x1E), CH('a', 0x1E), KD('A', 0x1E, false, true), CH('a', 0x1E), KU('A', 0x1E) } },
        { "char-only(vk0 VR)", { KD(0, 0), CH('z'), { WM_KEYUP, 0, (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31) } } },
        { "bare WM_CHAR", { CH('q') } },
    };
    for (const Case& c : cases) {
        RunSeq(c.name, c.seq, Build::Old, true, false);
        RunSeq(c.name, c.seq, Build::New, true, true);
        RunSeq(c.name, c.seq, Build::New, false, false);
        RunSeq(c.name, c.seq, Build::NewFix, false, false);
        RunSeq(c.name, c.seq, Build::Scoped, false, false);
    }
    // MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) vs _EX for the keys Magelight makes up from a bare VK.
    const UINT vks[] = { VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_DELETE, VK_HOME, VK_BACK, VK_RETURN, VK_TAB, VK_ESCAPE, VK_SPACE,
                         VK_RCONTROL, VK_RMENU, VK_RSHIFT };
    for (UINT vk : vks)
        std::printf("  MapVirtualKeyW vk=0x%02X  VSC=0x%02X  VSC_EX=0x%04X  SyntheticKeyLParam=0x%08llX\n", vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC),
                    MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX), static_cast<unsigned long long>(In::SyntheticKeyLParam(vk, false)));
}

// ── dead: es-419 ─────────────────────────────────────────────────────────
static HKL FindEs() { return FindLayout(0x080A, 0x580A); }

static void DeadTest()
{
    std::printf("== non-US layout (es-419, 080A)\n");
    HKL es = FindEs();
    HKL prev = GetKeyboardLayout(0);
    if (!es) { std::printf("  es-419 layout not loaded in this session - skipped\n"); return; }
    ActivateKeyboardLayout(es, 0);
    std::printf("  active layout %p\n", static_cast<void*>(GetKeyboardLayout(0)));
    BYTE ks0[256]{}; ks0[VK_NUMLOCK] = 1;
    wchar_t buf[8];
    auto clear = [&]() { for (int i = 0; i < 3; ++i) if (ToUnicodeEx(VK_SPACE, 0x39, ks0, buf, 8, 0, es) >= 0) break; };
    std::vector<UINT> dead;
    for (UINT vk = 0x30; vk < 0xFF; ++vk) {
        const UINT sc = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, es);
        if (!sc) continue;
        const int n = ToUnicodeEx(vk, sc, ks0, buf, 8, 0, es);
        if (n < 0) { dead.push_back(vk); clear(); }
    }
    for (UINT vk : dead) std::printf("  dead key vk=0x%02X scan=0x%02X\n", vk, MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, es));
    std::vector<Msg> altgr = { KD(VK_CONTROL, 0x1D), KD(VK_MENU, 0x38, true), KD('Q', 0x10), CH(L'@', 0x10), KU('Q', 0x10),
                               KU(VK_MENU, 0x38, true), KU(VK_CONTROL, 0x1D) };
    RunSeq("AltGr+Q", altgr, Build::Old, true, false);
    RunSeq("AltGr+Q", altgr, Build::New, true, true);
    RunSeq("AltGr+Q", altgr, Build::New, false, false);
    RunSeq("AltGr+Q", altgr, Build::Scoped, false, false);
    const UINT nyVk = VkKeyScanExW(L'ñ', es) & 0xFF;
    const UINT nySc = MapVirtualKeyExW(nyVk, MAPVK_VK_TO_VSC, es);
    std::printf("  ñ is vk=0x%02X scan=0x%02X\n", nyVk, nySc);
    std::vector<Msg> ny = { KD(nyVk, nySc), CH(L'ñ', nySc), KU(nyVk, nySc) };
    RunSeq("ñ", ny, Build::Old, true, false);
    RunSeq("ñ", ny, Build::Scoped, false, false);
    if (!dead.empty()) {
        const UINT dv = dead[0], ds = MapVirtualKeyExW(dv, MAPVK_VK_TO_VSC, es);
        int n = ToUnicodeEx(dv, ds, ks0, buf, 8, 0, es); const int n2 = ToUnicodeEx('E', 0x12, ks0, buf, 8, 0, es);
        const wchar_t composed = n2 > 0 ? buf[0] : 0;
        clear();
        std::printf("  dead 0x%02X then e: %d, %d -> U+%04X\n", dv, n, n2, static_cast<unsigned>(composed));
        std::vector<Msg> de = { KD(dv, ds), KU(dv, ds), KD('E', 0x12), CH(composed ? composed : L'e', 0x12), KU('E', 0x12) };
        RunSeq("dead+e", de, Build::Old, true, false);
        RunSeq("dead+e", de, Build::Scoped, false, false);
        // Does making a key event disturb the dead key the thread holds? (TranslateMessage and the VR path's
        // ToUnicode keep it there.)
        auto trial = [&](const char* what, auto between) {
            clear();
            const int a = ToUnicodeEx(dv, ds, ks0, buf, 8, 0, es);
            between();
            const int c = ToUnicodeEx('E', 0x12, ks0, buf, 8, 0, es);
            Expect(std::string("dead key kept across ") + what, (a == -1 && c == 1 && buf[0] == composed) ? "composed" : "lost", "composed");
            clear();
        };
        trial("nothing", []() {});
        trial("MakeKeyEvent(dead) + Fire", [&]() {
            const KeyEvent e = In::MakeKeyEvent(KeyEvent::kType_RawKeyDown, dv, KL(ds, false, false), false, In::kKbNumLock | In::kKbLShift);
            g_v->FireKeyEvent(e); });
        trial("MakeKeyEvent('E') + Fire", [&]() {
            const KeyEvent e = In::MakeKeyEvent(KeyEvent::kType_RawKeyDown, 'E', KL(0x12, false, false), false, In::kKbNumLock);
            g_v->FireKeyEvent(e); });
        trial("MakeKeyEvent(Char) + Fire", [&]() {
            const KeyEvent e = In::MakeKeyEvent(KeyEvent::kType_Char, L'e', KL(0x12, false, false), false, In::kKbNumLock);
            g_v->FireKeyEvent(e); });
        trial("Pump", [&]() { Pump(2); });
    }
    ActivateKeyboardLayout(prev, 0);
}

// ── state: when the constructor reads the keyboard state ─────────────────
static void StateTest()
{
    std::printf("== keyboard state: constructor time or fire time\n");
    UseUs();
    BYTE up[256]{}; up[VK_NUMLOCK] = 1;
    BYTE sh[256]{}; sh[VK_NUMLOCK] = 1; sh[VK_SHIFT] = sh[VK_LSHIFT] = 0x80;
    auto fire = [](const char* what, BYTE* atCtor, BYTE* atFire, unsigned pubMods) {
        Js("document.getElementById('t').value='';document.getElementById('t').focus();L.length=0;"); Pump();
        SetKeyboardState(atCtor);
        KeyEvent e(KeyEvent::kType_RawKeyDown, 'A', KL(0x1E, false, false), false);
        e.modifiers = pubMods;
        SetKeyboardState(atFire);
        g_v->FireKeyEvent(e);
        Pump();
        ThreadAllUp();
        std::printf("  %-44s %s\n", what, Js("K()").c_str());
    };
    fire("shift at ctor, up at fire:", sh, up, 0);
    fire("up at ctor, shift at fire:", up, sh, 0);
    fire("up at both, public modifiers=Shift:", up, up, KeyEvent::kMod_ShiftKey);
    // A constructor keydown then a hand-built Char (the host builds a surrogate pair's Char by hand).
    Js("document.getElementById('t').value='';document.getElementById('t').focus();L.length=0;"); Pump();
    FireHost({ WM_KEYDOWN, 0, 0, In::kKbNumLock });
    const Char16 pair[2] = { 0xD83D, 0xDE00 };
    KeyEvent c; c.type = KeyEvent::kType_Char; c.text = String(String16(pair, 2)); c.unmodified_text = c.text; g_v->FireKeyEvent(c); Pump(1);
    FireHost({ WM_KEYUP, 0, (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31), In::kKbNumLock });
    Pump();
    Expect("emoji: MakeKeyEvent down(vk0) + hand Char pair", Js("VAL()"), "\"\xF0\x9F\x98\x80\"");
    // Keys made up with lparam 0 still edit, but read code "Unidentified" or the keypad.
    auto edit = [&](const char* what, UINT vk, LPARAM l) {
        Js("var t=document.getElementById('t');t.value='abcd';t.focus();t.setSelectionRange(2,2);L.length=0;"); Pump();
        FireHost({ WM_KEYDOWN, vk, l, In::kKbNumLock });
        FireHost({ WM_KEYUP, vk, l | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31), In::kKbNumLock });
        Pump();
        std::printf("  %-40s value=%s caret=%s  %s\n", what, Js("VAL()").c_str(),
                    Js("String(document.getElementById('t').selectionStart)").c_str(), Js("K()").c_str());
    };
    edit("Backspace, lparam 0:", VK_BACK, 0);
    edit("Delete, lparam 0:", VK_DELETE, 0);
    edit("ArrowLeft, lparam 0:", VK_LEFT, 0);
    edit("ArrowLeft, scan 0x4B no ext:", VK_LEFT, KL(0x4B, false, false));
    edit("ArrowLeft, SyntheticKeyLParam:", VK_LEFT, In::SyntheticKeyLParam(VK_LEFT, false));
    // A thread whose own state is busy (keys down, Caps Lock on, the other side's modifiers): MakeKeyEvent's bits win
    // for the event, and the thread gets its own state back (FireHost compares all 256 bytes).
    {
        Js("var t=document.getElementById('t');t.value='';t.focus();L.length=0;"); Pump();
        BYTE busy[256]{};
        busy[VK_CAPITAL] = 1; busy['Q'] = 0x80; busy[VK_RCONTROL] = busy[VK_CONTROL] = 0x80; busy[VK_NUMLOCK] = 0;
        SetKeyboardState(busy);
        FireHost({ WM_KEYDOWN, 'A', KL(0x1E, false, false), In::kKbNumLock | In::kKbLShift });
        FireHost({ WM_CHAR, 'A', KL(0x1E, false, false), In::kKbNumLock | In::kKbLShift });
        FireHost({ WM_KEYUP, 'A', KL(0x1E, false, true), In::kKbNumLock | In::kKbLShift });
        BYTE now[256]{};
        GetKeyboardState(now);
        Expect("busy thread state: the event has the bits", Js("KEV('keydown','KeyA')"), "\"A\"|KeyA|L0|S");
        Expect("busy thread state: kept", (now[VK_CAPITAL] & 1) && (now['Q'] & 0x80) && (now[VK_RCONTROL] & 0x80) ? "kept" : "changed", "kept");
        ThreadAllUp();
    }
    // A key-up's bit 30 reads as a repeat unless MakeKeyEvent drops it.
    Js("document.getElementById('t').focus();L.length=0;"); Pump();
    { KeyEvent e(KeyEvent::kType_KeyUp, 'A', KL(0x1E, false, true), false); g_v->FireKeyEvent(e); Pump(); }
    Expect("bare constructor key-up with bit 30", Js("KEV('keyup','KeyA')"), "\"a\"|KeyA|L0||R");
    Js("L.length=0;"); Pump();
    FireHost({ WM_KEYUP, 'A', KL(0x1E, false, true), In::kKbNumLock });
    Expect("MakeKeyEvent key-up with bit 30", Js("KEV('keyup','KeyA')"), "\"a\"|KeyA|L0|");
}

// ── paths: the host's messages, flat and VR ──────────────────────────────
enum class Phase { Down, Repeat, Up };
struct Dik { std::uint32_t scan; Phase phase; };

// The VR sink: Magelight.cpp's QueueScancodeAsText, through the same Input::FromScancode, with the keyboard's live
// modifier state (`bits`, tracked from the scancodes) and its ToUnicode for the text.
static void SimScancode(std::vector<HostMsg>& out, std::uint32_t scan, Phase phase, bool numLock, std::uint16_t bits)
{
    const In::ScancodeKey key = In::FromScancode(scan, numLock);
    if (!key.vk) return;
    if (phase == Phase::Up) {
        out.push_back({ WM_KEYUP, key.vk, key.lparam | 1 | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31), bits });
        return;
    }
    const LPARAM down = key.lparam | 1 | (phase == Phase::Repeat ? (static_cast<LPARAM>(1) << 30) : 0);
    out.push_back({ WM_KEYDOWN, key.vk, down, bits });
    BYTE ks[256]{};
    if (bits & (In::kKbLShift | In::kKbRShift)) ks[VK_SHIFT] = 0x80;
    if (bits & (In::kKbLCtrl | In::kKbRCtrl)) ks[VK_CONTROL] = 0x80;
    if (bits & (In::kKbLAlt | In::kKbRAlt)) ks[VK_MENU] = 0x80;
    if (bits & In::kKbCaps) ks[VK_CAPITAL] = 1;
    wchar_t buf[8]{};
    const int n = ToUnicode(key.vk, key.make, ks, buf, 7, 0);
    for (int i = 0; i < n && i < 7; ++i)
        if (buf[i] >= 0x20 || buf[i] == L'\t' || buf[i] == L'\r') out.push_back({ WM_CHAR, buf[i], down, bits });
}
static std::uint16_t DikBit(std::uint32_t scan)
{
    switch (scan) {
    case 0x2A: return In::kKbLShift; case 0x36: return In::kKbRShift;
    case 0x1D: return In::kKbLCtrl;  case 0x9D: return In::kKbRCtrl;
    case 0x38: return In::kKbLAlt;   case 0xB8: return In::kKbRAlt;
    case 0xDB: return In::kKbLWin;   case 0xDC: return In::kKbRWin;
    default: return 0;
    }
}

struct PathCase {
    const char* name;
    std::vector<Msg> flat;     // the window's messages (empty: no flat path for it)
    std::vector<Dik> vr;       // the engine keyboard's scancodes (empty: no VR sink path)
    const char* code;          // the key the checks look at
    const char* down;          // its keydown: "key"|code|Llocation|modifiers
    const char* press;         // the first keypress ("key"|code|modifiers), or nullptr
    const char* value;         // the field afterwards, or nullptr
    bool numLock = true;
    std::uint16_t vrExtra = 0; // keyboard state the scancodes do not show (AltGr's left Ctrl)
};

static void RunPath(const PathCase& c, bool vr)
{
    Js("var t=document.getElementById('t');t.value='hello';t.focus();t.setSelectionRange(5,5);L.length=0;"); Pump();
    std::vector<HostMsg> q;
    if (!vr) {
        ModTrack mt;
        if (!c.numLock) mt.bits = static_cast<std::uint16_t>(mt.bits & ~In::kKbNumLock);
        for (const Msg& m : c.flat) { mt.Apply(m); q.push_back({ m.msg, m.w, m.l, mt.bits }); }
    } else {
        std::uint16_t bits = static_cast<std::uint16_t>((c.numLock ? In::kKbNumLock : 0) | c.vrExtra);
        for (const Dik& d : c.vr) {
            const std::uint16_t b = DikBit(d.scan);
            bits = static_cast<std::uint16_t>(d.phase == Phase::Up ? (bits & ~b) : (bits | b));
            SimScancode(q, d.scan, d.phase, c.numLock, bits);
        }
    }
    ThreadAllUp();   // the window thread's own state is neither the queue's nor current: MakeKeyEvent carries it
    for (const HostMsg& e : q) FireHost(e);
    Pump();
    const std::string tag = std::string(vr ? "VR   " : "flat ") + c.name;
    Expect(tag + " keydown", Js(std::string("KEV('keydown','") + c.code + "')"), c.down);
    if (c.press) Expect(tag + " keypress", Js("KP()"), c.press);
    if (c.value) Expect(tag + " field", Js("VAL()"), c.value);
    Expect(tag + " keyup", Js(std::string("KEV('keyup','") + c.code + "').split('|').slice(0,3).join('|')"),
           std::string(c.down).substr(0, std::string(c.down).rfind('|')));
}

static void PathsTest()
{
    std::printf("== paths: the flat window's messages and the VR sink's scancodes, through MakeKeyEvent (thread state all up)\n");
    UseUs();
    const std::vector<PathCase> us = {
        { "a", { KD('A', 0x1E), CH('a', 0x1E), KU('A', 0x1E) }, { { 0x1E, Phase::Down }, { 0x1E, Phase::Up } },
          "KeyA", "\"a\"|KeyA|L0|", "\"a\"|KeyA|", "\"helloa\"" },
        { "Shift+a", { KD(VK_SHIFT, 0x2A), KD('A', 0x1E), CH('A', 0x1E), KU('A', 0x1E), KU(VK_SHIFT, 0x2A) },
          { { 0x2A, Phase::Down }, { 0x1E, Phase::Down }, { 0x1E, Phase::Up }, { 0x2A, Phase::Up } },
          "KeyA", "\"A\"|KeyA|L0|S", "\"A\"|KeyA|S", "\"helloA\"" },
        { "Digit1", { KD('1', 0x02), CH('1', 0x02), KU('1', 0x02) }, { { 0x02, Phase::Down }, { 0x02, Phase::Up } },
          "Digit1", "\"1\"|Digit1|L0|", "\"1\"|Digit1|", "\"hello1\"" },
        { "Numpad5 (NumLock on)", { KD(VK_NUMPAD5, 0x4C), CH('5', 0x4C), KU(VK_NUMPAD5, 0x4C) },
          { { 0x4C, Phase::Down }, { 0x4C, Phase::Up } }, "Numpad5", "\"5\"|Numpad5|L3|", "\"5\"|Numpad5|", "\"hello5\"" },
        { "Numpad4 (NumLock off)", { KD(VK_LEFT, 0x4B), KU(VK_LEFT, 0x4B) }, { { 0x4B, Phase::Down }, { 0x4B, Phase::Up } },
          "Numpad4", "\"ArrowLeft\"|Numpad4|L3|", nullptr, nullptr, false },
        { "NumpadDivide", { KD(VK_DIVIDE, 0x35, true), CH('/', 0x35), KU(VK_DIVIDE, 0x35, true) },
          { { 0xB5, Phase::Down }, { 0xB5, Phase::Up } }, "NumpadDivide", "\"/\"|NumpadDivide|L3|", nullptr, "\"hello/\"" },
        { "NumpadEnter", { KD(VK_RETURN, 0x1C, true), CH('\r', 0x1C), KU(VK_RETURN, 0x1C, true) },
          { { 0x9C, Phase::Down }, { 0x9C, Phase::Up } }, "NumpadEnter", "\"Enter\"|NumpadEnter|L3|", nullptr, nullptr },
        { "ArrowLeft", { KD(VK_LEFT, 0x4B, true), KU(VK_LEFT, 0x4B, true) }, { { 0xCB, Phase::Down }, { 0xCB, Phase::Up } },
          "ArrowLeft", "\"ArrowLeft\"|ArrowLeft|L0|", nullptr, nullptr },
        { "ArrowRight", { KD(VK_RIGHT, 0x4D, true), KU(VK_RIGHT, 0x4D, true) }, { { 0xCD, Phase::Down }, { 0xCD, Phase::Up } },
          "ArrowRight", "\"ArrowRight\"|ArrowRight|L0|", nullptr, nullptr },
        { "ArrowUp", { KD(VK_UP, 0x48, true), KU(VK_UP, 0x48, true) }, { { 0xC8, Phase::Down }, { 0xC8, Phase::Up } },
          "ArrowUp", "\"ArrowUp\"|ArrowUp|L0|", nullptr, nullptr },
        { "ArrowDown", { KD(VK_DOWN, 0x50, true), KU(VK_DOWN, 0x50, true) }, { { 0xD0, Phase::Down }, { 0xD0, Phase::Up } },
          "ArrowDown", "\"ArrowDown\"|ArrowDown|L0|", nullptr, nullptr },
        { "Home", { KD(VK_HOME, 0x47, true), KU(VK_HOME, 0x47, true) }, { { 0xC7, Phase::Down }, { 0xC7, Phase::Up } },
          "Home", "\"Home\"|Home|L0|", nullptr, nullptr },
        { "Delete", { KD(VK_DELETE, 0x53, true), KU(VK_DELETE, 0x53, true) }, { { 0xD3, Phase::Down }, { 0xD3, Phase::Up } },
          "Delete", "\"Delete\"|Delete|L0|", nullptr, nullptr },
        { "Backspace", { KD(VK_BACK, 0x0E), CH(0x08, 0x0E), KU(VK_BACK, 0x0E) }, { { 0x0E, Phase::Down }, { 0x0E, Phase::Up } },
          "Backspace", "\"Backspace\"|Backspace|L0|", nullptr, "\"hell\"" },
        { "ShiftRight", { KD(VK_SHIFT, 0x36), KU(VK_SHIFT, 0x36) }, { { 0x36, Phase::Down }, { 0x36, Phase::Up } },
          "ShiftRight", "\"Shift\"|ShiftRight|L2|S", nullptr, nullptr },
        { "ControlRight", { KD(VK_CONTROL, 0x1D, true), KU(VK_CONTROL, 0x1D, true) }, { { 0x9D, Phase::Down }, { 0x9D, Phase::Up } },
          "ControlRight", "\"Control\"|ControlRight|L2|C", nullptr, nullptr },
        { "AltRight", { SKD(VK_MENU, 0x38, true), SKU(VK_MENU, 0x38, true) }, { { 0xB8, Phase::Down }, { 0xB8, Phase::Up } },
          "AltRight", "\"Alt\"|AltRight|L2|A", nullptr, nullptr },
        { "AltLeft", { SKD(VK_MENU, 0x38), SKU(VK_MENU, 0x38) }, { { 0x38, Phase::Down }, { 0x38, Phase::Up } },
          "AltLeft", "\"Alt\"|AltLeft|L1|A", nullptr, nullptr },
        // The constructor reads no Windows-key state (GetKeyState of Shift, Ctrl, Alt and Caps Lock only, the polled
        // section shows), so metaKey is false whatever the bits say.
        { "MetaLeft", { KD(VK_LWIN, 0x5B, true), KU(VK_LWIN, 0x5B, true) }, { { 0xDB, Phase::Down }, { 0xDB, Phase::Up } },
          "MetaLeft", "\"Meta\"|MetaLeft|L1|", nullptr, nullptr },
        { "F10", { SKD(VK_F10, 0x44), SKU(VK_F10, 0x44) }, { { 0x44, Phase::Down }, { 0x44, Phase::Up } },
          "F10", "\"F10\"|F10|L0|", nullptr, nullptr },
        { "Ctrl+A (select all) then x", { KD(VK_CONTROL, 0x1D), KD('A', 0x1E), CH(0x01, 0x1E), KU('A', 0x1E), KU(VK_CONTROL, 0x1D),
                                          KD('X', 0x2D), CH('x', 0x2D), KU('X', 0x2D) },
          { { 0x1D, Phase::Down }, { 0x1E, Phase::Down }, { 0x1E, Phase::Up }, { 0x1D, Phase::Up }, { 0x2D, Phase::Down }, { 0x2D, Phase::Up } },
          "KeyA", "\"a\"|KeyA|L0|C", "\"x\"|KeyX|", "\"x\"" },
        { "Ctrl+Shift+ArrowLeft (select a word)", { KD(VK_CONTROL, 0x1D), KD(VK_SHIFT, 0x2A), KD(VK_LEFT, 0x4B, true), KU(VK_LEFT, 0x4B, true),
                                                   KU(VK_SHIFT, 0x2A), KU(VK_CONTROL, 0x1D), KD('X', 0x2D), CH('x', 0x2D), KU('X', 0x2D) },
          { { 0x1D, Phase::Down }, { 0x2A, Phase::Down }, { 0xCB, Phase::Down }, { 0xCB, Phase::Up }, { 0x2A, Phase::Up }, { 0x1D, Phase::Up },
            { 0x2D, Phase::Down }, { 0x2D, Phase::Up } },
          "ArrowLeft", "\"ArrowLeft\"|ArrowLeft|L0|SC", "\"x\"|KeyX|", "\"x\"" },
        { "a held (repeat)", { KD('A', 0x1E), CH('a', 0x1E), KD('A', 0x1E, false, true), CH('a', 0x1E), KU('A', 0x1E) },
          { { 0x1E, Phase::Down }, { 0x1E, Phase::Repeat }, { 0x1E, Phase::Up } }, "KeyA", "\"a\"|KeyA|L0|", "\"a\"|KeyA|", "\"helloaa\"" },
    };
    for (const PathCase& c : us) {
        if (!c.flat.empty()) RunPath(c, false);
        if (!c.vr.empty()) RunPath(c, true);
    }
    // The keys the host makes up from a bare virtual key (SyntheticKeyLParam): the VR laser keyboard (OnOwnKeyboardKey),
    // Magelight_DeliverVKey, OpenComposite's control keys, the sink's Escape, the runtime keyboard's Backspace.
    struct Made { UINT vk; const char* code; const char* down; const char* value; };
    const Made made[] = {
        { VK_LEFT, "ArrowLeft", "\"ArrowLeft\"|ArrowLeft|L0|", nullptr }, { VK_RIGHT, "ArrowRight", "\"ArrowRight\"|ArrowRight|L0|", nullptr },
        { VK_UP, "ArrowUp", "\"ArrowUp\"|ArrowUp|L0|", nullptr }, { VK_DOWN, "ArrowDown", "\"ArrowDown\"|ArrowDown|L0|", nullptr },
        { VK_HOME, "Home", "\"Home\"|Home|L0|", nullptr }, { VK_END, "End", "\"End\"|End|L0|", nullptr },
        { VK_DELETE, "Delete", "\"Delete\"|Delete|L0|", nullptr }, { VK_BACK, "Backspace", "\"Backspace\"|Backspace|L0|", "\"hell\"" },
        { VK_RETURN, "Enter", "\"Enter\"|Enter|L0|", nullptr }, { VK_TAB, "Tab", "\"Tab\"|Tab|L0|", nullptr },
        { VK_ESCAPE, "Escape", "\"Escape\"|Escape|L0|", nullptr },
    };
    for (const Made& m : made) {
        Js("var t=document.getElementById('t');t.value='hello';t.focus();t.setSelectionRange(5,5);L.length=0;"); Pump();
        ThreadAllUp();
        FireHost({ WM_KEYDOWN, m.vk, In::SyntheticKeyLParam(m.vk, false), In::kKbNumLock });
        FireHost({ WM_KEYUP, m.vk, In::SyntheticKeyLParam(m.vk, true), In::kKbNumLock });
        Pump();
        const std::string tag = std::string("made-up ") + m.code;
        Expect(tag + " keydown", Js(std::string("KEV('keydown','") + m.code + "')"), m.down);
        Expect(tag + " keyup", Js(std::string("KEV('keyup','") + m.code + "')"), m.down);
        if (m.value) Expect(tag + " field", Js("VAL()"), m.value);
    }
    // A layout that moves keys (AZERTY's A sits where US has Q: VK 'A', scancode 0x10). `code` is the scancode's place
    // on a US keyboard and keyCode the layout's virtual key, so a page turning a keydown into a Windows VK (a hotkey
    // its plugin polls) must read keyCode; the docs promise both.
    {
        Js("var t=document.getElementById('t');t.value='';t.focus();L.length=0;"); Pump();
        ThreadAllUp();
        FireHost({ WM_KEYDOWN, 'A', KL(0x10, false, false), In::kKbNumLock });
        FireHost({ WM_KEYUP, 'A', KL(0x10, false, true), In::kKbNumLock });
        Pump();
        Expect("VK 'A' at the US Q place: keydown key|code|keyCode",
               Js("(function(){for(var i=0;i<L.length;i++){var o=L[i];if(o.t=='keydown')return JSON.stringify(o.key)+'|'+o.code+'|'+o.kc;}return 'none';})()"),
               "\"a\"|KeyQ|65");
    }

    HKL es = FindEs();
    if (!es) { std::printf("  es-419 layout not loaded in this session - its paths skipped\n"); return; }
    HKL prev = GetKeyboardLayout(0);
    ActivateKeyboardLayout(es, 0);
    // AltGr: Windows sends a left Ctrl with the right Alt, and the keyboard's state holds both (vrExtra); the engine
    // keyboard shows only the right Alt.
    const std::vector<PathCase> esCases = {
        { "es-419 AltGr+Q", { KD(VK_CONTROL, 0x1D), KD(VK_MENU, 0x38, true), KD('Q', 0x10), CH(L'@', 0x10), KU('Q', 0x10),
                              KU(VK_MENU, 0x38, true), KU(VK_CONTROL, 0x1D) },
          { { 0xB8, Phase::Down }, { 0x10, Phase::Down }, { 0x10, Phase::Up }, { 0xB8, Phase::Up } },
          "KeyQ", "\"@\"|KeyQ|L0|CA", "\"@\"|KeyQ|CA", "\"hello@\"", true, In::kKbLCtrl },
        { "es-419 AltGr key", { KD(VK_CONTROL, 0x1D), KD(VK_MENU, 0x38, true), KU(VK_MENU, 0x38, true), KU(VK_CONTROL, 0x1D) },
          { { 0xB8, Phase::Down }, { 0xB8, Phase::Up } },
          "AltRight", "\"AltGraph\"|AltRight|L2|CA", nullptr, nullptr, true, In::kKbLCtrl },
        { "es-419 ñ", { KD(0xC0, 0x27), CH(L'ñ', 0x27), KU(0xC0, 0x27) }, { { 0x27, Phase::Down }, { 0x27, Phase::Up } },
          "Semicolon", "\"ñ\"|Semicolon|L0|", "\"ñ\"|Semicolon|", "\"helloñ\"" },
        { "es-419 dead key then e", { KD(0xBA, 0x1A), KU(0xBA, 0x1A), KD('E', 0x12), CH(L'é', 0x12), KU('E', 0x12) },
          { { 0x1A, Phase::Down }, { 0x1A, Phase::Up }, { 0x12, Phase::Down }, { 0x12, Phase::Up } },
          "BracketLeft", "\"Dead\"|BracketLeft|L0|", "\"é\"|KeyE|", "\"helloé\"" },
        { "es-419 Shift+ñ", { KD(VK_SHIFT, 0x2A), KD(0xC0, 0x27), CH(L'Ñ', 0x27), KU(0xC0, 0x27), KU(VK_SHIFT, 0x2A) },
          { { 0x2A, Phase::Down }, { 0x27, Phase::Down }, { 0x27, Phase::Up }, { 0x2A, Phase::Up } },
          "Semicolon", "\"Ñ\"|Semicolon|L0|S", "\"Ñ\"|Semicolon|S", "\"helloÑ\"" },
    };
    for (const PathCase& c : esCases) {
        RunPath(c, false);
        RunPath(c, true);
    }
    ActivateKeyboardLayout(prev, 0);
}

// ── polled: the polled-keys filter around the constructor ────────────────
#ifndef MG_NO_MINHOOK
using KeyStateFn = SHORT(WINAPI*)(int);
using KbStateFn = BOOL(WINAPI*)(PBYTE);
static KeyStateFn g_nextKeyState = nullptr, g_nextAsync = nullptr;
static KbStateFn g_nextKbState = nullptr;
static const void* g_self = nullptr;
static bool g_record = false;
struct Read { const char* fn; int vk; const void* base; bool hidden; };
static std::vector<Read> g_reads;

static std::wstring ModulePath(const void* base)
{
    wchar_t buf[1024]{};
    const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(const_cast<void*>(base)), buf, 1023);
    return std::wstring(buf, n);
}
// Magelight.cpp's HidePolledKey with UI mode on: never the mouse buttons or the filtering module itself; another
// mod's plugin by Input::IsForeignPluginPath.
static bool Hidden(int vk, void* ra)
{
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2) return false;
    PVOID base = nullptr;
    if (!RtlPcToFileHeader(ra, &base) || !base || base == g_self) return false;
    std::wstring lower = ModulePath(base);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return !lower.empty() && In::IsForeignPluginPath(lower);
}
static void Note(const char* fn, int vk, void* ra, bool hidden)
{
    if (!g_record) return;
    PVOID base = nullptr;
    RtlPcToFileHeader(ra, &base);
    g_reads.push_back({ fn, vk, base, hidden });
}
static SHORT WINAPI HookKeyState(int vk)
{
    const SHORT st = g_nextKeyState(vk);
    const bool hide = (st & 0x8000) && Hidden(vk, _ReturnAddress());
    Note("GetKeyState", vk, _ReturnAddress(), hide);
    return hide ? static_cast<SHORT>(st & 1) : st;
}
static SHORT WINAPI HookAsync(int vk)
{
    const SHORT st = g_nextAsync(vk);
    const bool hide = st != 0 && Hidden(vk, _ReturnAddress());
    Note("GetAsyncKeyState", vk, _ReturnAddress(), hide);
    return hide ? 0 : st;
}
static BOOL WINAPI HookKbState(PBYTE ks)
{
    Note("GetKeyboardState", -1, _ReturnAddress(), false);
    return g_nextKbState(ks);
}

static void PolledTest(bool control)
{
    std::printf("== polled keys: %s\n", control ? "CONTROL run from a folder the filter does not exempt" : "the runtime in a SKSE\\Plugins\\Magelight folder");
    UseUs();
    PVOID self = nullptr;
    RtlPcToFileHeader(reinterpret_cast<PVOID>(&PolledTest), &self);
    g_self = self;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void* ks = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyState"));
    void* as = reinterpret_cast<void*>(GetProcAddress(user32, "GetAsyncKeyState"));
    void* kb = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyboardState"));
    if (MH_Initialize() != MH_OK || MH_CreateHook(ks, reinterpret_cast<void*>(&HookKeyState), reinterpret_cast<void**>(&g_nextKeyState)) != MH_OK ||
        MH_CreateHook(as, reinterpret_cast<void*>(&HookAsync), reinterpret_cast<void**>(&g_nextAsync)) != MH_OK ||
        MH_CreateHook(kb, reinterpret_cast<void*>(&HookKbState), reinterpret_cast<void**>(&g_nextKbState)) != MH_OK ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Expect("MinHook hooks installed", "no", "yes");
        return;
    }
    const std::wstring wc = ModulePath(GetModuleHandleW(L"WebCore.dll"));
    std::printf("  WebCore: %ls\n", wc.c_str());
    struct Seq { const char* name; std::vector<HostMsg> q; const char* code; const char* want; const char* hiddenWant; };
    const std::vector<Seq> seqs = {
        { "Ctrl+A", { { WM_KEYDOWN, VK_CONTROL, KL(0x1D, false, false), In::kKbNumLock | In::kKbLCtrl },
                      { WM_KEYDOWN, 'A', KL(0x1E, false, false), In::kKbNumLock | In::kKbLCtrl },
                      { WM_KEYUP, 'A', KL(0x1E, false, true), In::kKbNumLock | In::kKbLCtrl },
                      { WM_KEYUP, VK_CONTROL, KL(0x1D, false, true), In::kKbNumLock } }, "KeyA", "\"a\"|KeyA|L0|C", "\"a\"|KeyA|L0|" },
        { "Shift+a", { { WM_KEYDOWN, VK_SHIFT, KL(0x2A, false, false), In::kKbNumLock | In::kKbLShift },
                       { WM_KEYDOWN, 'A', KL(0x1E, false, false), In::kKbNumLock | In::kKbLShift },
                       { WM_CHAR, 'A', KL(0x1E, false, false), In::kKbNumLock | In::kKbLShift },
                       { WM_KEYUP, 'A', KL(0x1E, false, true), In::kKbNumLock | In::kKbLShift },
                       { WM_KEYUP, VK_SHIFT, KL(0x2A, false, true), In::kKbNumLock } }, "KeyA", "\"A\"|KeyA|L0|S", "\"a\"|KeyA|L0|" },
        { "Alt (sys)", { { WM_SYSKEYDOWN, VK_MENU, KL(0x38, false, false, false, true), In::kKbNumLock | In::kKbLAlt },
                         { WM_SYSKEYUP, VK_MENU, KL(0x38, false, true), In::kKbNumLock } }, "AltLeft", "\"Alt\"|AltLeft|L1|A", "\"Alt\"|AltLeft|L1|" },
    };
    for (const Seq& s : seqs) {
        Js("var t=document.getElementById('t');t.value='hello';t.focus();t.setSelectionRange(5,5);L.length=0;"); Pump();
        ThreadAllUp();
        g_reads.clear();
        for (const HostMsg& e : s.q) {
            g_record = true;
            const KeyEvent evt = In::MakeKeyEvent(TypeOf(e.msg), e.w, e.l, e.msg == WM_SYSKEYDOWN || e.msg == WM_SYSKEYUP, e.keys);
            g_record = false;
            g_v->FireKeyEvent(evt);
            Pump(1);
        }
        Pump();
        int hidden = 0, byRuntime = 0;
        std::string fromWebCore;
        for (const Read& r : g_reads) {
            if (r.hidden) ++hidden;
            if (r.base != g_self) {
                ++byRuntime;
                std::wstring p = ModulePath(r.base);
                const auto sl = p.find_last_of(L"\\/");
                char line[160];
                std::snprintf(line, sizeof line, " %s(%s)%s from %ls", r.fn, r.vk < 0 ? "-" : (std::to_string(r.vk)).c_str(),
                              r.hidden ? " HIDDEN" : "", (sl == std::wstring::npos ? p : p.substr(sl + 1)).c_str());
                if (fromWebCore.size() < 1200) fromWebCore += line;
            }
        }
        std::printf("  %-10s %d read(s) by the runtime while constructing:%s\n", s.name, byRuntime, fromWebCore.c_str());
        if (!control) {
            Expect(std::string("polled: ") + s.name + " reads hidden", std::to_string(hidden), "0");
            Expect(std::string("polled: ") + s.name + " keydown", Js(std::string("KEV('keydown','") + s.code + "')"), s.want);
        } else {
            Expect(std::string("polled control: ") + s.name + " reads hidden", hidden > 0 ? "some" : "none", "some");
            Expect(std::string("polled control: ") + s.name + " keydown loses its modifier",
                   Js(std::string("KEV('keydown','") + s.code + "')"), s.hiddenWant);
        }
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
#endif

// ── release: what the host sends when focus goes (DrainInputQueue's releaseKeys / releaseButtons) ──
static void Mouse(MouseEvent::Type t, int x, int y, MouseEvent::Button b = MouseEvent::kButton_None)
{
    MouseEvent e{ t, x, y, b };
    g_v->FireMouseEvent(e);
    Pump(1);
}
static void MClear() { Js("L.length=0"); }
static void MDump(const char* what) { Pump(); std::printf("  %-34s %s\n", what, Js("M()").c_str()); }

static void ReleaseTest()
{
    std::printf("== release: key-ups for held modifiers and releases for held buttons\n");
    UseUs();
    using M = MouseEvent;
    auto click = [&]() { MClear(); Mouse(M::kType_MouseDown, 60, 320, M::kButton_Left); Mouse(M::kType_MouseUp, 60, 320, M::kButton_Left); Pump(); };
    struct Held { const char* name; std::uint16_t bit; const char* code; const char* mods; };
    const Held held[] = {
        { "ShiftLeft", In::kKbLShift, "ShiftLeft", "S" }, { "ShiftRight", In::kKbRShift, "ShiftRight", "S" },
        { "ControlLeft", In::kKbLCtrl, "ControlLeft", "C" }, { "ControlRight", In::kKbRCtrl, "ControlRight", "C" },
        { "AltLeft", In::kKbLAlt, "AltLeft", "A" }, { "AltRight", In::kKbRAlt, "AltRight", "A" },
    };
    for (const Held& h : held) {
        Wait(600);
        int i = 0;
        while ((1u << i) != h.bit) ++i;
        ThreadAllUp();
        Js("L.length=0"); Pump();
        const std::uint16_t keys = static_cast<std::uint16_t>(In::kKbNumLock | h.bit);
        const bool sys = h.bit == In::kKbLAlt || h.bit == In::kKbRAlt;
        FireHost({ static_cast<UINT>(sys ? WM_SYSKEYDOWN : WM_KEYDOWN), static_cast<WPARAM>(In::kModifierMsgVk[i]),
                   In::SyntheticKeyLParam(static_cast<UINT>(In::kModifierVk[i]), false), keys });
        click();
        Expect(std::string("release: click while ") + h.name + " held", Js("(function(){for(var i=0;i<L.length;i++)if(L[i].t=='click')return mods(L[i]);return 'none';})()"), h.mods);
        Wait(600);
        Js("L.length=0"); Pump();
        // The host's release: the side-neutral key with that side's scancode, the bits without it.
        FireHost({ WM_KEYUP, static_cast<WPARAM>(In::kModifierMsgVk[i]),
                   In::SyntheticKeyLParam(static_cast<UINT>(In::kModifierVk[i]), true), In::kKbNumLock });
        Expect(std::string("release: ") + h.name + " key-up", Js(std::string("KEV('keyup','") + h.code + "').split('|').slice(1).join('|')"),
               std::string(h.code) + "|L" + (h.bit & (In::kKbLShift | In::kKbLCtrl | In::kKbLAlt) ? "1" : "2") + "|");
        click();
        Expect(std::string("release: click after ") + h.name + "'s release", Js("(function(){for(var i=0;i<L.length;i++)if(L[i].t=='click')return mods(L[i]);return 'none';})()"), "");
    }
    // A held button released at the last position the view was sent, after the pointer went elsewhere.
    Wait(700);
    MClear();
    Mouse(M::kType_MouseMoved, 60, 320);
    Mouse(M::kType_MouseDown, 60, 320, M::kButton_Left);
    Mouse(M::kType_MouseMoved, 900, 700);   // the captured view is sent the move outside it
    Mouse(M::kType_MouseUp, 900, 700, M::kButton_Left);
    Mouse(M::kType_MouseMoved, 70, 330);
    Pump();
    Expect("release: buttons after a press, an outside move and its release",
           Js("(function(){var o=L.filter(function(o){return o.t=='mousemove';});return String(o.length?o[o.length-1].bs:'none');})()"), "0");
    Expect("release: the outside release reached the page", Js("String(CNT('mouseup'))"), "1");
}

// ── leave: the move the host sends a view the pointer left ───────────────
static void LeaveTest()
{
    std::printf("== leave: MouseMoved(-1,-1) on a view the pointer left\n");
    using M = MouseEvent;
    Mouse(M::kType_MouseMoved, 60, 320);
    Pump();
    Expect("leave: over #btn, :hover", Js("String(document.getElementById('btn').matches(':hover'))"), "true");
    MClear();
    Mouse(M::kType_MouseMoved, -1, -1);
    Pump();
    Expect("leave: after the leave, :hover", Js("String(document.getElementById('btn').matches(':hover'))"), "false");
    Expect("leave: mouseleave on #btn", Js("String(L.filter(function(o){return o.t=='mouseleave'&&o.tg=='btn';}).length)"), "1");
    MClear();
    Mouse(M::kType_MouseMoved, 60, 320);
    Pump();
    Expect("leave: back over #btn, :hover", Js("String(document.getElementById('btn').matches(':hover'))"), "true");
}

// ── guard: the page core's pointer guard ─────────────────────────────────
static void GuardTest()
{
    using M = MouseEvent;
    std::printf("== guard: Input::kPointerGuardScript\n");
    // Without the guard, then with its middle-mousedown half alone, then whole.
    auto run = [&](const char* tag, int guard) {
        NewView();
        if (guard == 1) Js("window.addEventListener('mousedown',function(e){if(e.button===1&&e.isTrusted)e.preventDefault();},true);");
        if (guard == 2) Js(In::kPointerGuardScript);
        Pump();
        struct B { const char* name; M::Button b; };
        const B kinds[] = { { "left", M::kButton_Left }, { "middle", M::kButton_Middle }, { "right", M::kButton_Right },
                            { "back", M::kButton_Back }, { "forward", M::kButton_Forward } };
        for (const B& k : kinds) {
            Wait(700);
            Js("window.__pb=0;location.hash='';L.length=0;"); Pump();
            Mouse(M::kType_MouseMoved, 50, 410);
            Mouse(M::kType_MouseDown, 50, 410, k.b); Mouse(M::kType_MouseUp, 50, 410, k.b);
            Mouse(M::kType_MouseMoved, 600, 470);   // a middle click's autoscroll ends on the next press: one away from the targets
            Mouse(M::kType_MouseDown, 600, 470, M::kButton_Left); Mouse(M::kType_MouseUp, 600, 470, M::kButton_Left);
            Wait(700);
            Mouse(M::kType_MouseMoved, 190, 410);
            Mouse(M::kType_MouseDown, 190, 410, k.b); Mouse(M::kType_MouseUp, 190, 410, k.b);
            Pump(5);
            const std::string got = "onclick " + Js("String(window.__pb)") + ", link " + Js("JSON.stringify(location.hash)") +
                ", click " + Js("String(L.filter(function(o){return o.t=='click'&&o.b!==0;}).length)") +
                ", auxclick " + Js("String(L.filter(function(o){return o.t=='auxclick';}).length)") +
                ", contextmenu " + Js("String(CNT('contextmenu'))");
            if (guard == 2) {
                const char* want = k.b == M::kButton_Left ? "onclick 1, link \"#went\", click 0, auxclick 0, contextmenu 0"
                                 : k.b == M::kButton_Right ? "onclick 0, link \"\", click 0, auxclick 0, contextmenu 2"
                                 : "onclick 0, link \"\", click 0, auxclick 2, contextmenu 0";
                Expect(std::string("guard: ") + k.name + " on <button onclick> then <a href>", got, want);
            } else {
                std::printf("  %-14s %-8s %s\n", tag, k.name, got.c_str());
            }
            Mouse(M::kType_MouseMoved, 600, 470);
            Mouse(M::kType_MouseDown, 600, 470, M::kButton_Left); Mouse(M::kType_MouseUp, 600, 470, M::kButton_Left);
        }
        // A middle click over a scroller, then moves and a left click.
        Wait(700);
        Js("document.getElementById('inner').scrollTop=0;L.length=0;"); Pump();
        Mouse(M::kType_MouseMoved, 100, 150);
        Mouse(M::kType_MouseDown, 100, 150, M::kButton_Middle); Mouse(M::kType_MouseUp, 100, 150, M::kButton_Middle);
        for (int i = 0; i < 5; ++i) Mouse(M::kType_MouseMoved, 100, 150 + 20 * i);
        Pump(10);
        const std::string scrolled = Js("String(document.getElementById('inner').scrollTop)");
        Wait(700);
        MClear();
        Mouse(M::kType_MouseDown, 60, 320, M::kButton_Left); Mouse(M::kType_MouseUp, 60, 320, M::kButton_Left);
        Pump();
        const std::string next = Js("L.filter(function(o){return o.t=='mousedown'||o.t=='mouseup'||o.t=='click';}).map(function(o){return o.t;}).join(' ')");
        if (guard == 2) {
            Expect("guard: middle over a scroller, then moves: scrollTop", scrolled, "0");
            Expect("guard: the next left click", next, "mousedown mouseup click");
        } else {
            std::printf("  %-14s middle over a scroller then moves: scrollTop %s; the next left click: %s\n", tag, scrolled.c_str(), next.c_str());
        }
        // The page's own listeners still see the side buttons' presses and releases.
        if (guard == 2) {
            Wait(700);
            MClear();
            Mouse(M::kType_MouseDown, 60, 320, M::kButton_Back); Mouse(M::kType_MouseUp, 60, 320, M::kButton_Back);
            Pump();
            Expect("guard: back press and release reach the page", Js("M()"),
                   "mousedown{b3 bs8 d1 60,320 btn} mouseup{b3 bs0 d1 60,320 btn} auxclick*{b3 bs0 d1 60,320 btn}");
        }
        // Two presses of one button 80 ms apart over an element with a dblclick handler: WebKit fires dblclick for
        // any button whose second click it counts, the stopped click's included; a browser, for the primary only.
        Js("window.__db=0;document.getElementById('btn').addEventListener('dblclick',function(){window.__db++;});");
        for (const B& k : kinds) {
            Wait(700);
            Js("window.__db=0;L.length=0;"); Pump();
            Mouse(M::kType_MouseMoved, 60, 320);
            Mouse(M::kType_MouseDown, 60, 320, k.b); Mouse(M::kType_MouseUp, 60, 320, k.b);
            Wait(80);
            Mouse(M::kType_MouseDown, 60, 320, k.b); Mouse(M::kType_MouseUp, 60, 320, k.b);
            Pump(5);
            const std::string got = "dblclick handler " + Js("String(window.__db)") + ", dblclick " + Js("String(CNT('dblclick'))");
            if (guard == 2) {
                Expect(std::string("guard: two ") + k.name + " presses 80 ms apart on <div ondblclick>", got,
                       k.b == M::kButton_Left ? "dblclick handler 1, dblclick 1" : "dblclick handler 0, dblclick 0");
            } else {
                std::printf("  %-14s %-8s two presses 80 ms apart: %s\n", tag, k.name, got.c_str());
            }
        }
    };
    run("no guard", 0);
    run("mousedown only", 1);
    run("guard", 2);
    // Alt+x with an accesskey="x" element: what a page now gets for Alt+key (informational).
    NewView();
    Js("window.__ak=0;L.length=0;"); Pump();
    UseUs();
    ThreadAllUp();
    FireHost({ WM_SYSKEYDOWN, VK_MENU, KL(0x38, false, false, false, true), In::kKbNumLock | In::kKbLAlt });
    FireHost({ WM_SYSKEYDOWN, 'X', KL(0x2D, false, false, false, true), In::kKbNumLock | In::kKbLAlt });
    FireHost({ WM_SYSKEYUP, 'X', KL(0x2D, false, true), In::kKbNumLock | In::kKbLAlt });
    FireHost({ WM_SYSKEYUP, VK_MENU, KL(0x38, false, true), In::kKbNumLock });
    Pump();
    std::printf("  Alt+x with <button accesskey=x>: its onclick ran %s time(s); %s\n", Js("String(window.__ak)").c_str(), Js("K()").c_str());
}

// ── routes: Input::PointerRoutes, as DrainInputQueue drives it ───────────
// Views are ids (1 the page, 2 the keyboard page, 3 another); `hit` is the view under a source's point. Each step
// does what the drain does: the target is the source's pressed view, else the hit; the hover is noted, then the event.
struct Router {
    In::PointerRoutes r;
    std::string left;   // the leaves and tooltip leaves the steps sent, in order
    std::uint64_t Target(unsigned src, std::uint64_t hit) const { const std::uint64_t c = r.Captured(src); return c ? c : hit; }
    void Hover(unsigned src, std::uint64_t view)
    {
        const In::PointerRoutes::Left g = r.Hover(src, view);
        if (g.tooltip) left += "tip" + std::to_string(g.tooltip) + " ";
        if (g.leave) left += "leave" + std::to_string(g.leave) + " ";
    }
    std::uint64_t Move(unsigned src, std::uint64_t hit, int x = 0, int y = 0)
    {
        const std::uint64_t t = Target(src, hit);
        Hover(src, t);
        r.Moved(src, t, x, y);
        return t;
    }
    std::uint64_t Down(unsigned src, std::uint64_t hit, int button = MouseEvent::kButton_Left, int x = 0, int y = 0)
    {
        const std::uint64_t t = Target(src, hit);
        Hover(src, t);
        r.Press(src, t, button, x, y);
        return t;
    }
    std::uint64_t Up(unsigned src, std::uint64_t hit, int button = MouseEvent::kButton_Left, int x = 0, int y = 0)
    {
        const std::uint64_t t = Target(src, hit);
        r.Release(src, t, button, x, y);
        return t;
    }
    std::string Left() { std::string s = left; left.clear(); return s.empty() ? "none" : s.substr(0, s.size() - 1); }
};

static void RoutesTest()
{
    std::printf("== routes: Input::PointerRoutes\n");
    const unsigned M = In::kPointerMouse, H0 = In::kPointerHand0, H1 = In::kPointerHand1;
    auto S = [](std::uint64_t v) { return std::to_string(v); };
    {
        // Hand 1 holds a key on the keyboard page (2); hand 0's ray rests on the page (1) and jitters.
        Router g;
        g.Move(H0, 1); g.Move(H1, 2); g.Left();
        g.Down(H1, 2, MouseEvent::kButton_Left, 40, 30);
        std::string to;
        for (int i = 0; i < 4; ++i) to += S(g.Move(H0, 1, 100 + i, 200)) + (i < 3 ? " " : "");
        Expect("routes: hand 0's moves while hand 1 holds a key", to, "1 1 1 1");
        Expect("routes: and the keyboard page gets no leave", g.Left(), "none");
        Expect("routes: hand 1's moves off the keyboard stay with it", S(g.Move(H1, 1, 500, 30)), "2");
        Expect("routes: hand 0's press goes where it points", S(g.Down(H0, 1)), "1");
        Expect("routes: hand 0's release", S(g.Up(H0, 2)), "1");
        Expect("routes: hand 1 still holds the keyboard page", S(g.r.Captured(H1)), "2");
        Expect("routes: hand 1's release, over the page", S(g.Up(H1, 1, MouseEvent::kButton_Left, 500, 30)), "2");
        Expect("routes: nobody holds a press", S(g.r.Captured(H0)) + " " + S(g.r.Captured(H1)), "0 0");
        g.Left();
        g.Move(H1, 1);
        Expect("routes: hand 1 on to the page, the keyboard page left", g.Left(), "tip2 leave2");
    }
    {
        // Two hands on two views, both rays moving every frame: no leave either way.
        Router g;
        g.Move(H0, 1); g.Move(H1, 2); g.Left();
        for (int i = 0; i < 4; ++i) { g.Move(H0, 1, i, 0); g.Move(H1, 2, i, 0); }
        Expect("routes: two hands on two views, moving", g.Left(), "none");
        // Both on the page, then one moves to the keyboard page: the page still has a pointer.
        g.Move(H1, 1); g.Left();
        g.Move(H0, 2);
        Expect("routes: one of two hands on a view leaves it", g.Left(), "none");
        g.Move(H1, 3);
        Expect("routes: the last hand on a view leaves it", g.Left(), "tip1 leave1");
    }
    {
        // Two hands press on one view; the second's release keeps the first's press.
        Router g;
        g.Down(H0, 1); g.Down(H1, 1);
        g.Up(H1, 1);
        Expect("routes: two presses on one view, one released", S(g.r.Captured(H0)) + " " + S(g.r.Captured(H1)), "1 0");
        Expect("routes: the other hand's moves stay with its press", S(g.Move(H0, 2)), "1");
        g.Up(H0, 2);
        Expect("routes: then released", S(g.r.Captured(H0)), "0");
    }
    {
        // The flat mouse alone, as before: a drag keeps its view, the move after the release leaves it.
        Router g;
        g.Move(M, 1); g.Left();
        g.Down(M, 1, MouseEvent::kButton_Left, 5, 6);
        Expect("routes: mouse drag over another view", S(g.Move(M, 2, 700, 6)), "1");
        Expect("routes: no leave while held", g.Left(), "none");
        g.Down(M, 2, MouseEvent::kButton_Right, 701, 6);   // the right button too, over another view: the press stays with the first
        Expect("routes: a second button joins the press", S(g.r.Captured(M)), "1");
        g.Up(M, 2, MouseEvent::kButton_Left, 702, 6);
        Expect("routes: one of two buttons up, still held", S(g.r.Captured(M)), "1");
        const In::PointerRoutes::Held h = g.r.TakeHeld(M);
        Expect("routes: the held button taken (focus lost)", S(h.view) + " mask " + S(h.mask) + " at " + S(h.x) + "," + S(h.y),
               "1 mask 8 at 702,6");
        Expect("routes: nothing held after", S(g.r.Captured(M)) + " mask " + S(g.r.TakeHeld(M).mask), "0 mask 0");
        g.Move(M, 2);
        Expect("routes: the next move, over the other view", g.Left(), "tip1 leave1");
    }
    {
        // A pressed view that loses its page: the hold is dropped, the next move leaves it.
        Router g;
        g.Down(H0, 1);
        g.r.Drop(H0);
        Expect("routes: dropped hold", S(g.Move(H0, 2)), "2");
        Expect("routes: the dropped view left", g.Left(), "tip1 leave1");
    }
}

// ── mouse, mouse2, wheel: what Ultralight does (printed) ──────────────────
static void MouseTest()
{
    std::printf("== mouse  (GetDoubleClickTime %u ms, SM_CXDOUBLECLK %d)\n", GetDoubleClickTime(), GetSystemMetrics(SM_CXDOUBLECLK));
    using M = MouseEvent;
    Mouse(M::kType_MouseMoved, 60, 320);
    MClear();
    Mouse(M::kType_MouseDown, 60, 320, M::kButton_Left);
    for (int i = 1; i <= 3; ++i) Mouse(M::kType_MouseMoved, 60 + 20 * i, 320);
    Mouse(M::kType_MouseUp, 120, 320, M::kButton_Left);
    MDump("drag, moves kButton_None:");
    MClear();
    Mouse(M::kType_MouseDown, 60, 320, M::kButton_Left);
    Mouse(M::kType_MouseDown, 60, 320, M::kButton_Right);
    Mouse(M::kType_MouseMoved, 70, 320);
    Mouse(M::kType_MouseUp, 70, 320, M::kButton_Right);
    Mouse(M::kType_MouseUp, 70, 320, M::kButton_Left);
    MDump("left held + right chord:");
    Wait(800);
    MClear();
    Mouse(M::kType_MouseDown, 60, 320, M::kButton_Right);
    Mouse(M::kType_MouseUp, 60, 320, M::kButton_Right);
    MDump("right click:");
    const int gaps[] = { 50, 250, 450, 600 };
    for (int g : gaps) {
        Wait(900);
        MClear();
        Mouse(M::kType_MouseDown, 60, 320, M::kButton_Left);
        Mouse(M::kType_MouseUp, 60, 320, M::kButton_Left);
        Wait(g);
        Mouse(M::kType_MouseDown, 60, 320, M::kButton_Left);
        Mouse(M::kType_MouseUp, 60, 320, M::kButton_Left);
        Pump();
        std::printf("  two clicks %3d ms apart: %s\n", g, Js("L.filter(function(o){return o.t=='click'||o.t=='dblclick'}).map(function(o){return o.t+o.d}).join(' ')").c_str());
    }
    // A lost release: the button stays down for the page until its next press.
    Wait(900);
    Js("getSelection().removeAllRanges();L.length=0;");
    Mouse(M::kType_MouseMoved, 355, 310);
    Mouse(M::kType_MouseDown, 355, 310, M::kButton_Left);
    Mouse(M::kType_MouseMoved, 450, 310);
    Wait(300);
    MClear();
    Mouse(M::kType_MouseMoved, 600, 330);
    Pump();
    std::printf("  lost-up: later plain move -> %s\n", Js("M()").c_str());
    MClear();
    Mouse(M::kType_MouseDown, 60, 320, M::kButton_Left);
    Mouse(M::kType_MouseUp, 60, 320, M::kButton_Left);
    MDump("lost-up: next click elsewhere:");
}

static void Wheel(int dy, int x = -1, int y = -1)
{
    ScrollEvent e{ ScrollEvent::kType_ScrollByPixel, 0, dy };
    e.x = x; e.y = y;
    g_v->FireScrollEvent(e);
    Pump(2);
}
static void WDump(const char* what) { Pump(); std::printf("  %-52s %-44s %s\n", what, Js("ST()").c_str(), Js("M()").c_str()); }
static void WReset() { Js("['outer','inner','other'].forEach(function(i){document.getElementById(i).scrollTop=0});document.scrollingElement.scrollTop=0;L.length=0;"); Pump(); }

static void WheelTest()
{
    std::printf("== wheel  (#inner at 10..260 x 110..210, #other at 350..650 x 50..250, focus in #other's input)\n");
    NewView();
    Js("document.getElementById('t2').focus()"); WReset();
    Wheel(-40);
    WDump("fresh view, no move, x/y unset:");
    WReset();
    Wheel(-40, 100, 150);
    WDump("fresh view, no move, x/y over #inner:");
    Expect("wheel: x/y over #inner on a view with no move scrolls it", Js("String(document.getElementById('inner').scrollTop>0)"), "true");
    WReset();
    MouseEvent mv{ MouseEvent::kType_MouseMoved, 500, 150, MouseEvent::kButton_None };
    g_v->FireMouseEvent(mv); Pump(); WReset();
    Wheel(-40, 100, 150);
    WDump("after a move over #other, x/y over #inner:");
    WReset();
    Wheel(-40);
    WDump("then x/y unset again (did x/y move the pointer?):");
    WReset();
    Wheel(-40, 2000, 150);
    WDump("x beyond the view width:");
}

static int ToolMain(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: inputprobe <sdkDir> [paths|polled|polledcontrol|release|leave|guard|routes|keys|dead|state|mouse|wheel ...]\n"); return 2; }
    const std::string sdk = argv[1];
    std::vector<std::string> want;
    for (int i = 2; i < argc; ++i) want.push_back(argv[i]);
    auto on = [&](const char* s) {
        if (want.empty()) return std::string(s) != "polledcontrol";
        return std::find(want.begin(), want.end(), std::string(s)) != want.end();
    };
    Config cfg;
    Platform::instance().set_config(cfg);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    Platform::instance().set_file_system(GetPlatformFileSystem(String(sdk.c_str())));
    g_r = Renderer::Create();
    NewView();
    if (on("paths")) PathsTest();
#ifndef MG_NO_MINHOOK
    if (on("polled")) PolledTest(false);
    if (on("polledcontrol")) PolledTest(true);
#else
    if (on("polled") || on("polledcontrol")) std::printf("== polled keys: skipped (built without MinHook)\n");
#endif
    if (on("release")) { NewView(); ReleaseTest(); }
    if (on("leave")) { NewView(); LeaveTest(); }
    if (on("guard")) GuardTest();
    if (on("routes")) RoutesTest();
    if (on("keys")) { NewView(); KeysTest(); }
    if (on("dead")) { NewView(); DeadTest(); }
    if (on("state")) { NewView(); StateTest(); }
    if (on("mouse")) { NewView(); MouseTest(); }
    if (on("wheel")) WheelTest();
    if (g_made) {
        std::printf("== MakeKeyEvent over the run (%d events)\n", g_made);
        Expect("the bits reached the constructor every time", std::to_string(g_notApplied) + " misses", "0 misses");
        Expect("the thread's keyboard state put back every time", std::to_string(g_notRestored) + " misses", "0 misses");
    }
    g_v = nullptr;
    g_r = nullptr;
    std::printf("\n%d check(s) failed\n", g_fails);
    return g_fails;
}

// MG_PAGE_FIBER=1: on the page fiber, as Magelight runs pages (../fiber-gc-test/FiberRun.h).
int main(int argc, char** argv) { return FiberRun::Run(argc, argv, &ToolMain); }
