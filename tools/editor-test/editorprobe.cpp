// editorprobe - the host's text-focus rules (src/EditStateCore.h) and the Ultralight 2.0 Editor behaviour they rely
// on, offline on CPU views, no game. Sections:
//   core      the rules alone: SplitTyped, PairUnit, FieldJson, CommandByName, the Tracker's decisions, DocLoads
//             (checked)
//   fields    FieldJson from real EditableState reports for each kind of field (checked)
//   host      a two-view host run frame by frame as Magelight runs it: the listener records, the frame decides
//             (Tracker), View focus follows UI mode with ViewConfig::initial_focus false; clicks, the re-raise on a
//             click inside the field (where it is now, after a scroll) and not on a control that keeps the focus,
//             entry, exit, switch, navigation, a release (which reports nothing), rebuild, Tab, inputmode changes,
//             readonly, inputmode=none, iframe and contenteditable, the re-raise after another view's reset; a page's
//             own navigation (location, a reload, a form's submit by Enter, into a page with an autofocus field) from
//             file:/// pages, which reports nothing, a fragment change and a failed load, the host's Navigate and
//             Reload that fail, stop another load or keep the page, a script-less page's first window object, made
//             by the host's first script, and a new UI-mode View whose autofocus field reports before its first
//             window object (a deferred or end-of-body script) (checked)
//   unfocused what a View created with initial_focus false does before it is focused: reports, focus events,
//             animation and timers, keys (checked where the host relies on it, printed otherwise)
//   typed     typed text through SplitTyped as the host fires it: InsertText runs, Backspace / Enter / Tab as key
//             events from Input::MakeKeyEvent; InsertText's answers; the edit commands (checked)
//   keyboard  views/keyboard/index.html with a stub page core: layouts per field, Enter labels, the edit buttons,
//             bad field values, shown false (checked); four layouts written as PNGs beside the probe
// Usage: editorprobe <sdkDir> <repoRoot> [section ...] (default: all). Exit code = failed checks. Re-run it at every
// SDK bump.
#include <windows.h>
#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/Editor.h>
#include <Ultralight/Listener.h>
#include <Ultralight/ClipboardData.h>
#include <Ultralight/platform/Clipboard.h>
#include <Ultralight/platform/FileSystem.h>
#include <Ultralight/platform/Surface.h>

#include "EditStateCore.h"
#include "InputCore.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "../fiber-gc-test/FiberRun.h"

using namespace ultralight;
namespace Ed = Magelight::EditState;
namespace In = Magelight::Input;

static int g_fails = 0;
static void Expect(const std::string& name, const std::string& got, const std::string& want)
{
    const bool ok = got == want;
    if (!ok) ++g_fails;
    std::printf("%s  %-62s %s%s%s\n", ok ? "ok  " : "FAIL", name.c_str(), got.c_str(), ok ? "" : "   want ", ok ? "" : want.c_str());
}
static void Note(const std::string& name, const std::string& got) { std::printf("      %-62s %s\n", name.c_str(), got.c_str()); }
static std::string B(bool b) { return b ? "1" : "0"; }

static const char* KbName(Ed::Keyboard k) { return k == Ed::Keyboard::Raise ? "raise" : k == Ed::Keyboard::Hide ? "hide" : "keep"; }
static std::string Desc(const EditableState& s)
{
    char b[160];
    std::snprintf(b, sizeof b, "type=%d mutable=%d focus=%d mode=%d enter=%d", static_cast<int>(s.type), s.is_mutable,
                  s.view_has_focus, static_cast<int>(s.input_mode), static_cast<int>(s.enter_key_hint));
    return b;
}

// ── Core ─────────────────────────────────────────────────────────────────────
static std::string Pieces(const std::wstring& s)
{
    std::string out;
    for (const auto& p : Ed::SplitTyped(s.data(), s.size())) {
        if (!out.empty()) out += ' ';
        if (p.vk) { out += "vk" + std::to_string(p.vk); continue; }
        out += '"';
        for (wchar_t c : p.text) {
            if (c < 0x80) out += static_cast<char>(c);
            else { char b[12]; std::snprintf(b, sizeof b, "\\u%04X", static_cast<unsigned>(c)); out += b; }
        }
        out += '"';
    }
    return out.empty() ? "(none)" : out;
}

static EditableState St(EditableType t, bool focus, InputMode m = InputMode::Unspecified, Rect r = { 10, 10, 210, 34 })
{
    EditableState s;
    s.type = t;
    s.is_mutable = t == EditableType::Text || t == EditableType::Password || t == EditableType::TextArea ||
                   t == EditableType::ContentEditable;
    s.view_has_focus = focus;
    s.input_mode = m;
    if (t != EditableType::None) s.bounds = r;
    return s;
}

static void SectionCore()
{
    std::printf("\n== core ==\n");
    Expect("split: letters", Pieces(L"abc"), "\"abc\"");
    Expect("split: Backspace, Enter, Tab are keys", Pieces(L"a\bb\rc\td"), "\"a\" vk8 \"b\" vk13 \"c\" vk9 \"d\"");
    Expect("split: CR LF is one Enter, a lone LF is one", Pieces(L"x\r\ny\nz"), "\"x\" vk13 \"y\" vk13 \"z\"");
    Expect("split: other controls and DEL dropped", Pieces(std::wstring(L"a\x1b\x01\x7f" L"b")), "\"ab\"");
    Expect("split: a surrogate pair stays in the run", Pieces(L"a\xD83D\xDE00z"), "\"a\\uD83D\\uDE00z\"");
    Expect("split: lone halves dropped", Pieces(std::wstring(L"\xDE00" L"a\xD83D")), "\"a\"");
    Expect("split: empty", Pieces(L""), "(none)");
    {
        wchar_t held = 0, out[2]{};
        const size_t a = Ed::PairUnit(held, 0xD83D, out);
        const size_t b = Ed::PairUnit(held, 0xDE00, out);
        Expect("pair: high held, low completes", std::to_string(a) + "," + std::to_string(b) + (out[0] == 0xD83D && out[1] == 0xDE00 ? " pair" : " ?"), "0,2 pair");
        const size_t c = Ed::PairUnit(held, 0xDE00, out);
        Ed::PairUnit(held, 0xD83D, out);
        const size_t d = Ed::PairUnit(held, L'x', out);
        Expect("pair: lone low dropped, high then x keeps x", std::to_string(c) + "," + std::to_string(d) + (out[0] == L'x' ? " x" : " ?") + " held=" + B(held != 0), "0,1 x held=0");
    }
    {
        EditableState s = St(EditableType::Text, true, InputMode::Email);
        s.enter_key_hint = EnterKeyHint::Send;
        Expect("field: email + send", Ed::FieldJson(s), "{\"mode\":\"email\",\"enter\":\"send\",\"multiline\":false,\"secret\":false}");
        Expect("field: password", Ed::FieldJson(St(EditableType::Password, true)), "{\"mode\":\"text\",\"enter\":\"\",\"multiline\":false,\"secret\":true}");
        Expect("field: textarea", Ed::FieldJson(St(EditableType::TextArea, true, InputMode::Numeric)), "{\"mode\":\"numeric\",\"enter\":\"\",\"multiline\":true,\"secret\":false}");
        Expect("field: inputmode none", Ed::FieldJson(St(EditableType::Text, true, InputMode::None)), "null");
        Expect("field: view unfocused", Ed::FieldJson(St(EditableType::Text, false)), "null");
        Expect("field: a button", Ed::FieldJson(St(EditableType::Other, true)), "null");
        Expect("field: a number field gets the decimal keypad", Ed::FieldJson(St(EditableType::Text, true), true),
               "{\"mode\":\"decimal\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");
        Expect("field: a number field with its own inputmode keeps it",
               Ed::FieldJson(St(EditableType::Text, true, InputMode::Numeric), true),
               "{\"mode\":\"numeric\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");
        Expect("ime: a text field, a number field, a password field",
               B(Ed::WantsIme(St(EditableType::Text, true))) + B(Ed::WantsIme(St(EditableType::Text, true), true)) +
                   B(Ed::WantsIme(St(EditableType::Password, true))), "100");
        Expect("number field: asked only of a focused single-line text field",
               B(Ed::MayBeNumberField(St(EditableType::Text, true))) + B(Ed::MayBeNumberField(St(EditableType::Text, false))) +
                   B(Ed::MayBeNumberField(St(EditableType::TextArea, true))) + B(Ed::MayBeNumberField(St(EditableType::Password, true))),
               "1000");
    }
    {
        auto fmt = [](const Rect& r) {
            char b[64];
            std::snprintf(b, sizeof b, "%.0f,%.0f,%.0f,%.0f", r.left, r.top, r.right, r.bottom);
            return std::string(b);
        };
        const Rect at{ 10.f, 100.f, 210.f, 124.f }, c0{ 50.f, 104.f, 51.f, 120.f }, c1{ 80.f, 74.f, 81.f, 90.f }, none{ 0.f, 0.f, 0.f, 0.f };
        Expect("field now: a single-line field follows its caret vertically", fmt(Ed::FieldNow(St(EditableType::Text, true, InputMode::Unspecified, at), c0, c1)), "10,70,210,94");
        Expect("field now: a textarea keeps its reported bounds", fmt(Ed::FieldNow(St(EditableType::TextArea, true, InputMode::Unspecified, at), c0, c1)), "10,100,210,124");
        Expect("field now: no caret then or now keeps them", fmt(Ed::FieldNow(St(EditableType::Password, true, InputMode::Unspecified, at), none, c1)) + " " +
               fmt(Ed::FieldNow(St(EditableType::Password, true, InputMode::Unspecified, at), c0, none)), "10,100,210,124 10,100,210,124");
    }
    {
        EditorCommand c{};
        std::string got;
        for (const char* n : { "undo", "redo", "selectAll", "cut", "copy", "paste", "Paste", "insertTab", "" })
            got += std::string(n) + "=" + (Ed::CommandByName(n, c) ? std::to_string(static_cast<int>(c)) : "-") + " ";
        Expect("commands", got, "undo=5 redo=6 selectAll=4 cut=0 copy=1 paste=2 Paste=- insertTab=- =- ");
        std::string names;
        for (const char* n : { "undo", "redo", "selectAll", "cut", "copy", "paste" })
            if (Ed::CommandByName(n, c)) names += std::string(Ed::CommandName(c)) + " ";
        Expect("command names round trip", names + Ed::CommandName(EditorCommand::InsertTab), "undo redo selectAll cut copy paste ?");
    }
    // The Tracker on made-up reports (the host section runs it on real ones).
    {
        Ed::Tracker t;
        const EditableState none{}, field = St(EditableType::Text, true), unf = St(EditableType::Text, false);
        std::vector<Ed::Change> ch;
        std::vector<Ed::Click> cl;
        auto step = [&](std::uint64_t ui, const EditableState& st, bool resync = false) {
            const Ed::Verdict v = t.Step(ui, ch, st, cl, resync);
            ch.clear();
            cl.clear();
            return std::string(v.touched ? "T" : "-") + (v.targetChanged ? "C" : "-") + (v.ime ? "I" : "-") + " " + KbName(v.keyboard);
        };
        Expect("tracker: entry, nothing focused", step(1, none), "TC- hide");
        ch.push_back({ 1, none, field, false });
        Expect("tracker: a field focused in the page", step(1, field), "T-I raise");
        ch.push_back({ 1, unf, field, true });
        Expect("tracker: the host's View::Focus never raises", step(1, field), "T-I keep");
        EditableState moved = field;
        moved.input_mode = InputMode::Numeric;
        ch.push_back({ 1, field, moved, false });
        Expect("tracker: inputmode change on the same field keeps", step(1, moved), "T-I keep");
        cl.push_back({ 50.f, 20.f });
        Expect("tracker: a click inside the field raises", step(1, moved), "T-I raise");
        cl.push_back({ 50.f, 90.f });
        Expect("tracker: a click outside it does nothing", step(1, moved), "--- keep");
        ch.push_back({ 2, none, field, false });
        Expect("tracker: another view's field is only recorded", step(1, moved), "--- keep");
        Expect("tracker: switch to a view with a field hides", step(2, field), "TCI hide");
        Expect("tracker: exit hides", step(0, none), "TC- hide");
        Expect("tracker: idle", B(t.Idle(0, false, false, false)) + B(t.Idle(1, false, false, false)), "10");
        EditableState pw = St(EditableType::Password, true);
        ch.push_back({ 3, none, pw, false });
        Expect("tracker: password raises, no IME", step(3, pw), "TC- raise");
        ch.push_back({ 3, none, field, false });
        {
            const Ed::Verdict nv = t.Step(3, ch, field, cl, false, nullptr, true);
            ch.clear();
            Expect("tracker: a number field raises, no IME", std::string(nv.ime ? "I" : "-") + " " + KbName(nv.keyboard), "- raise");
        }
        Expect("tracker: a reset with the field gone hides", step(3, none, true), "T-- hide");
    }
    // DocLoads on made-up Views (the host section runs it on real loads).
    {
        Ed::DocLoads d;
        int a = 0, b = 0;
        d.Begun(&a);
        const bool before = d.HasWindow(&a);
        Expect("doc loads: a View's first window object is no new document, even in a load", B(d.WindowReady(&a)), "0");
        Expect("doc loads: HasWindow from that first window object on", B(before) + B(d.HasWindow(&a)) + B(d.HasWindow(&b)),
               "010");
        d.Begun(&a);
        d.Begun(&a);
        const bool first = d.WindowReady(&a), second = d.WindowReady(&a);
        Expect("doc loads: after it, a begun load's window object is a new document, once", B(first) + B(second), "10");
        d.Begun(&a);
        d.Ended(&a);
        Expect("doc loads: none after DOM ready or a failed load", B(d.WindowReady(&a)), "0");
        d.WindowReady(&b);   // b's first
        d.Begun(&a);
        d.Begun(&b);
        d.Ended(&b);
        Expect("doc loads: per View", B(d.WindowReady(&a)) + B(d.WindowReady(&b)), "10");
        d.Begun(&a);
        d.Released(&a);
        const bool released = d.HasWindow(&a);
        d.Begun(&a);
        Expect("doc loads: a released View is forgotten (a new View at its address starts afresh)",
               B(released) + B(d.WindowReady(&a)), "00");
    }
}

// ── Engine harness ───────────────────────────────────────────────────────────
struct Rec { View* ul; EditableState st; bool hostFocus; std::string inside; };
static std::vector<Rec> g_recs;   // the host's queue: a frame takes them, a reset drops a view's (MiniHost::Forget)
static std::vector<Rec> g_recLog;   // every report, kept
static bool g_inHostFocus = false;
static std::string g_inside = "-";
struct RecL : EditorListener {
    void OnChangeEditableState(View* v, const EditableState& s) override
    {
        g_recs.push_back({ v, s, g_inHostFocus, g_inside });
        g_recLog.push_back(g_recs.back());
    }
};
static RecL g_recL;

struct TestClipboard : Clipboard {
    int reads = 0, writes = 0;
    std::string last;
    void Clear() override {}
    RefPtr<ClipboardData> Read() override { ++reads; return ClipboardData::Create(String("PASTED")); }
    void Write(RefPtr<ClipboardData> d) override { ++writes; if (d) last = d->AsText().utf8().data(); }
};
static TestClipboard g_clip;

// The SDK's file system, plus the pages the host section's navigations load from file:/// (by path; a query or
// fragment is ignored, as MlFileSystem does). A file named *slow* arrives 300 ms late: OpenFile runs on the background
// thread that loads file:/// URLs, so a page that waits for af-slow.js renders, and reports its autofocus field, before
// the script runs, and a load of nav-slow.html is still on its way when the next call comes.
struct ProbeFs : FileSystem {
    FileSystem* base = nullptr;
    std::map<std::string, std::string> pages;
    const std::string* Page(const String& p)
    {
        std::string s = p.utf8().data();
        if (const size_t q = s.find_first_of("?#"); q != std::string::npos) s.resize(q);
        while (!s.empty() && (s[0] == '/' || s[0] == '\\')) s.erase(0, 1);
        const auto it = pages.find(s);
        return it == pages.end() ? nullptr : &it->second;
    }
    bool FileExists(const String& p) override { return Page(p) || base->FileExists(p); }
    String GetFileMimeType(const String& p) override
    {
        if (!Page(p)) return base->GetFileMimeType(p);
        std::string s = p.utf8().data();
        if (const size_t q = s.find_first_of("?#"); q != std::string::npos) s.resize(q);
        return String(s.size() > 3 && s.compare(s.size() - 3, 3, ".js") == 0 ? "text/javascript" : "text/html");
    }
    String GetFileCharset(const String& p) override { return Page(p) ? String("utf-8") : base->GetFileCharset(p); }
    RefPtr<Buffer> OpenFile(const String& p) override
    {
        if (const std::string* s = Page(p)) {
            if (std::string(p.utf8().data()).find("slow") != std::string::npos)
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return Buffer::CreateFromCopy(s->data(), s->size());
        }
        return base->OpenFile(p);
    }
};
static ProbeFs g_fs;

struct Settle : LoadListener {
    bool done = false;
    void OnDOMReady(View*, uint64_t, bool main, const String&) override { if (main) done = true; }
};

static RefPtr<Renderer> g_r;
static std::string g_outDir;   // the probe's own folder: the keyboard section writes its layouts there as PNGs
static void Pump(int n = 3)
{
    for (int i = 0; i < n; ++i) {
        g_inside = "Update";
        g_r->Update();
        g_r->RefreshDisplay(0);
        g_inside = "Render";
        g_r->Render();
        g_inside = "-";
        FiberRun::BetweenFrames();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
static std::string Js(View* v, const std::string& s, bool pump = true)
{
    String ex;
    g_inside = "EvaluateScript";
    String out = v->EvaluateScript(String(s.c_str()), &ex);
    g_inside = "-";
    if (!ex.empty()) std::printf("   [js exception] %s :: %s\n", s.c_str(), ex.utf8().data());
    if (pump) Pump(2);
    return std::string(out.utf8().data());
}

// As LoadViewLocked creates every page's View: initial focus off.
static RefPtr<View> MakeView(const char* html, const char* url, bool initialFocus, int w = 640, int h = 420)
{
    ViewConfig vc;
    vc.is_accelerated = false;
    vc.is_transparent = true;
    vc.clipboard_read_policy = ClipboardReadPolicy::Deny;
    vc.initial_focus = initialFocus;
    RefPtr<View> v = g_r->CreateView(w, h, vc, nullptr);
    Settle ld;
    v->set_load_listener(&ld);
    v->set_editor_listener(&g_recL);
    v->LoadHTML(html, url);
    for (int i = 0; i < 400 && !ld.done; ++i) Pump(1);
    Pump(10);
    v->set_load_listener(nullptr);
    return v;
}
static void Reload(View* v, const char* html, const char* url)
{
    Settle ld;
    v->set_load_listener(&ld);
    v->LoadHTML(html, url);
    for (int i = 0; i < 400 && !ld.done; ++i) Pump(1);
    Pump(10);
    v->set_load_listener(nullptr);
}

static bool Center(View* v, const std::string& expr, int& x, int& y)
{
    const std::string r = Js(v, "(function(){var b=" + expr + ".getBoundingClientRect();return Math.round(b.left+b.width/2)+','+Math.round(b.top+b.height/2);})()", false);
    return sscanf_s(r.c_str(), "%d,%d", &x, &y) == 2;
}
static void ClickAt(View* v, int x, int y)
{
    g_inside = "FireMouseEvent";
    MouseEvent m{ MouseEvent::kType_MouseMoved, x, y, MouseEvent::kButton_None };
    v->FireMouseEvent(m);
    m.type = MouseEvent::kType_MouseDown;
    m.button = MouseEvent::kButton_Left;
    v->FireMouseEvent(m);
    m.type = MouseEvent::kType_MouseUp;
    v->FireMouseEvent(m);
    g_inside = "-";
    Pump(2);
}

// Keys as DrainInputQueue fires them: Input::MakeKeyEvent with the lParam the host makes up for a bare vk.
static void FireKey(View* v, KeyEvent::Type type, WPARAM w, LPARAM l)
{
    g_inside = "FireKeyEvent";
    v->FireKeyEvent(In::MakeKeyEvent(type, w, l, false, 0));
    g_inside = "-";
}
// The host's QueueVirtualKey: key down, the character Enter and Tab type, key up.
static void VirtualKey(View* v, unsigned vk)
{
    const LPARAM down = In::SyntheticKeyLParam(vk, false);
    FireKey(v, KeyEvent::kType_RawKeyDown, vk, down);
    if (vk == VK_RETURN || vk == VK_TAB) FireKey(v, KeyEvent::kType_Char, vk == VK_RETURN ? L'\r' : L'\t', down);
    FireKey(v, KeyEvent::kType_KeyUp, vk, In::SyntheticKeyLParam(vk, true));
    Pump(2);
}
// The host's typed-text path: SplitTyped, then InsertText per run and the keys.
static std::string Type(View* v, const std::wstring& s)
{
    std::string oks;
    for (const auto& p : Ed::SplitTyped(s.data(), s.size())) {
        if (p.vk) { VirtualKey(v, p.vk); continue; }
        const String16 t16(reinterpret_cast<const Char16*>(p.text.data()), p.text.size());
        g_inside = "InsertText";
        oks += B(v->editor()->InsertText(String(t16)));
        g_inside = "-";
        Pump(2);
    }
    return oks;
}

// ── fields ───────────────────────────────────────────────────────────────────
static const char* kFieldsPage = R"(<!doctype html><html><body style="margin:4px;font:16px sans-serif">
<input id="a"><input id="num" type="number"><input id="pw" type="password"><input id="imn" inputmode="numeric" enterkeyhint="go">
<input id="imd" inputmode="decimal" enterkeyhint="send"><input id="imt" inputmode="tel" enterkeyhint="next">
<input id="ime" inputmode="email" enterkeyhint="done"><input id="imu" inputmode="url" enterkeyhint="previous">
<input id="ims" inputmode="search" enterkeyhint="search"><input id="imx" inputmode="text" enterkeyhint="enter">
<input id="imnone" inputmode="none"><input id="ro" readonly><textarea id="ta"></textarea>
<div id="ce" contenteditable="true" style="width:200px;height:30px"></div><button id="btn">b</button><select id="sel"><option>1</option></select>
<iframe id="f" srcdoc="<input id=fi inputmode=email><input id=fnum type=number>" style="width:200px;height:40px"></iframe>
</body></html>)";

static void SectionFields()
{
    std::printf("\n== fields ==\n");
    RefPtr<View> v = MakeView(kFieldsPage, "file:///fields.html", false);
    g_inHostFocus = true;
    v->Focus();
    g_inHostFocus = false;
    Pump();
    struct Row { const char* id; const char* want; };
    const Row rows[] = {
        { "a", "{\"mode\":\"text\",\"enter\":\"\",\"multiline\":false,\"secret\":false}" },
        { "num", "{\"mode\":\"text\",\"enter\":\"\",\"multiline\":false,\"secret\":false}" },
        { "pw", "{\"mode\":\"text\",\"enter\":\"\",\"multiline\":false,\"secret\":true}" },
        { "imn", "{\"mode\":\"numeric\",\"enter\":\"go\",\"multiline\":false,\"secret\":false}" },
        { "imd", "{\"mode\":\"decimal\",\"enter\":\"send\",\"multiline\":false,\"secret\":false}" },
        { "imt", "{\"mode\":\"tel\",\"enter\":\"next\",\"multiline\":false,\"secret\":false}" },
        { "ime", "{\"mode\":\"email\",\"enter\":\"done\",\"multiline\":false,\"secret\":false}" },
        { "imu", "{\"mode\":\"url\",\"enter\":\"previous\",\"multiline\":false,\"secret\":false}" },
        { "ims", "{\"mode\":\"search\",\"enter\":\"search\",\"multiline\":false,\"secret\":false}" },
        { "imx", "{\"mode\":\"text\",\"enter\":\"enter\",\"multiline\":false,\"secret\":false}" },
        { "imnone", "null" },
        { "ro", "null" },
        { "ta", "{\"mode\":\"text\",\"enter\":\"\",\"multiline\":true,\"secret\":false}" },
        { "ce", "{\"mode\":\"text\",\"enter\":\"\",\"multiline\":true,\"secret\":false}" },
        { "btn", "null" },
        { "sel", "null" },
    };
    for (const Row& r : rows) {
        Js(v.get(), "document.activeElement&&document.activeElement.blur()");
        g_recs.clear();
        Js(v.get(), std::string("document.getElementById('") + r.id + "').focus()");
        Expect(std::string("field ") + r.id, g_recs.empty() ? "(no report)" : Ed::FieldJson(g_recs.back().st), r.want);
    }
    Js(v.get(), "document.activeElement&&document.activeElement.blur()");
    g_recs.clear();
    Js(v.get(), "document.getElementById('f').contentDocument.getElementById('fi').focus()");
    Expect("field in an iframe", g_recs.empty() ? "(no report)" : Ed::FieldJson(g_recs.back().st),
           "{\"mode\":\"email\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");
    // A number field reports as a text field; only the page tells them apart (kNumberFieldScript).
    std::string answers;
    for (const char* id : { "a", "num", "imn", "pw", "ta", "ce" }) {
        Js(v.get(), std::string("document.getElementById('") + id + "').focus()");
        answers += std::string(id) + "=" + Js(v.get(), Ed::kNumberFieldScript, false) + " ";
    }
    Js(v.get(), "document.getElementById('f').contentDocument.getElementById('fi').focus()");
    answers += "fi=" + Js(v.get(), Ed::kNumberFieldScript, false) + " ";
    Js(v.get(), "document.getElementById('f').contentDocument.getElementById('fnum').focus()");
    answers += "fnum=" + Js(v.get(), Ed::kNumberFieldScript, false) + " " + (g_recs.empty() ? std::string("?") : B(g_recs.back().st.type == EditableType::Text));
    Expect("number field: the page's answer for each field, a number field in an iframe too", answers,
           "a=0 num=1 imn=0 pw=0 ta=0 ce=0 fi=0 fnum=1 1");
    g_recs.clear();
}

// ── host ─────────────────────────────────────────────────────────────────────
static const char* kHostPage = R"(<!doctype html><html><head><style>
body{margin:0;font:16px sans-serif} .p{position:absolute;left:10px;width:200px;height:24px;box-sizing:border-box}
</style></head><body>
<input id="a" class="p" style="top:10px"><input id="b" class="p" style="top:50px">
<ul id="sug" style="position:absolute;left:10px;top:80px;width:200px;margin:0;padding:0;list-style:none"><li id="s1" style="height:24px">one</li></ul>
<button id="btn" style="position:absolute;left:300px;top:10px;width:80px;height:30px">b</button>
<input id="none" class="p" inputmode="none" style="top:130px"><input id="ro" class="p" readonly style="top:170px">
<textarea id="ta" style="position:absolute;left:10px;top:210px;width:200px;height:50px"></textarea>
<div id="ce" contenteditable="true" style="position:absolute;left:10px;top:280px;width:200px;height:30px"></div>
<iframe id="f" srcdoc="<body style='margin:0'><input id=fi style='width:180px'><input id=fn type=number style='width:180px'></body>" style="position:absolute;left:300px;top:100px;width:220px;height:50px;border:0"></iframe>
<input id="n" type="number" class="p" style="top:330px">
<div id="scr" style="position:absolute;left:300px;top:200px;width:220px;height:100px;overflow:auto"><div style="height:40px"></div><input id="si" style="width:180px"><div id="pd" style="height:40px">pick</div><div style="height:400px"></div></div>
<script>
window.__picked=0;
document.getElementById('sug').addEventListener('mousedown',function(e){e.preventDefault();window.__picked++;});
document.getElementById('pd').addEventListener('mousedown',function(e){e.preventDefault();window.__picked++;});
</script></body></html>)";
static const char* kAutofocusPage = R"(<!doctype html><html><body style="margin:0">
<input id="auto" autofocus style="position:absolute;left:10px;top:10px;width:200px" inputmode="numeric"></body></html>)";

struct HostView { std::uint64_t id = 0; RefPtr<View> ul; EditableState edit; bool number = false; };
struct MiniHost {
    std::vector<HostView*> views;
    std::uint64_t ui = 0;            // the UI-mode view, 0 = UI mode off
    std::uint64_t focusView = 0;     // the view the host Focus()ed
    bool kbShown = false, ime = false;
    std::vector<std::uint64_t> resets;   // the views reset since the last frame (as s_editResets)
    std::vector<Ed::Click> clicks;
    Ed::Tracker tracker;
    Ed::Verdict last;

    HostView* Find(std::uint64_t id) { for (HostView* v : views) if (v->id == id) return v; return nullptr; }
    // DrainInputQueue's focus marker: only the UI-mode view has View focus.
    void FocusMarker(bool on)
    {
        HostView* u = Find(ui);
        g_inHostFocus = true;
        if (HostView* p = Find(focusView); p && (p != u || !on)) p->ul->Unfocus();
        if (u) { if (on) u->ul->Focus(); else u->ul->Unfocus(); }
        g_inHostFocus = false;
        focusView = (on && u) ? u->id : 0;
        Pump(2);
    }
    void Enter(std::uint64_t id) { ui = id; FocusMarker(true); }
    void Exit() { FocusMarker(false); ui = 0; kbShown = false; }   // SetUIModeImpl hides the keyboard on exit
    void Switch(std::uint64_t id) { ui = id; FocusMarker(true); }
    void Click(HostView& v, const std::string& expr)
    {
        int x = 0, y = 0;
        if (!Center(v.ul.get(), expr, x, y)) { std::printf("   no element %s\n", expr.c_str()); return; }
        ClickAt(v.ul.get(), x, y);
        if (v.id == ui) clicks.push_back({ static_cast<float>(x), static_cast<float>(y) });
    }
    // ForgetEditStateLocked: a load, reload, navigation, release or destroy.
    void Forget(HostView& v)
    {
        std::erase_if(g_recs, [&](const Rec& r) { return r.ul == v.ul.get(); });
        v.edit = {};
        resets.push_back(v.id);
    }
    // DrainEditableStates.
    Rect caretAtReport{ 0.f, 0.f, 0.f, 0.f };
    std::uint64_t caretView = 0;
    Ed::Verdict Frame()
    {
        std::vector<Ed::Change> ch;
        bool uiReported = false;
        for (const Rec& r : g_recs) {
            HostView* hv = nullptr;
            for (HostView* v : views) if (v->ul.get() == r.ul) hv = v;
            if (!hv) continue;
            ch.push_back({ hv->id, hv->edit, r.st, r.hostFocus });
            hv->edit = r.st;
            if (hv->id == ui) uiReported = true;
        }
        g_recs.clear();
        last = {};
        std::vector<std::uint64_t> reset;
        reset.swap(resets);
        const bool resync = !reset.empty();
        if (tracker.Idle(ui, !ch.empty(), !clicks.empty(), resync)) return last;
        const HostView* u = Find(ui);
        const EditableState st = u ? u->edit : EditableState{};
        if (ui != caretView || std::find(reset.begin(), reset.end(), ui) != reset.end()) {
            caretView = ui;
            caretAtReport = Rect{ 0.f, 0.f, 0.f, 0.f };
        }
        const bool wants = u && Ed::WantsKeyboard(st);
        if (uiReported) caretAtReport = wants ? u->ul->editor()->GetCaretBounds() : Rect{ 0.f, 0.f, 0.f, 0.f };
        Rect field{ 0.f, 0.f, 0.f, 0.f };
        const Rect* fieldPtr = nullptr;
        if (wants && !clicks.empty()) {
            field = Ed::FieldNow(st, caretAtReport, u->ul->editor()->GetCaretBounds());
            fieldPtr = &field;
        }
        // The host asks the page whether a reported single-line field is a number field (kNumberFieldScript).
        bool number = false;
        if (HostView* hu = Find(ui)) {
            if (!Ed::MayBeNumberField(st)) hu->number = false;
            else if (uiReported) hu->number = Js(hu->ul.get(), Ed::kNumberFieldScript, false) == "1";
            number = hu->number;
        }
        last = tracker.Step(ui, ch, st, clicks, resync, fieldPtr, number);
        clicks.clear();
        if (!last.touched) return last;
        ime = last.ime;
        if (last.keyboard == Ed::Keyboard::Hide) kbShown = false;
        else if (last.keyboard == Ed::Keyboard::Raise) kbShown = true;
        return last;
    }
    std::string State()
    {
        const HostView* u = Find(ui);
        return std::string("kb=") + B(kbShown) + " ime=" + B(ime) + " field=" + (u ? Ed::FieldJson(u->edit, u->number) : "null");
    }
    std::string Kb() { return std::string("kb=") + B(kbShown) + " ime=" + B(ime); }
};

// MlLoadListener's text-focus half, on a view of its own: a main-frame load noted when it begins, the replaced page's
// state dropped at the new document's window object (MiniHost::Forget, as ForgetEditStateLocked), the note cleared at
// DOM ready or a failed load (EditState::DocLoads).
struct HostLoads : LoadListener {
    MiniHost* host = nullptr;
    HostView* view = nullptr;
    Ed::DocLoads loads;
    bool done = false;
    int windows = 0, begun = 0, doms = 0, fails = 0;
    std::string windowInside = "-";   // the call the last main-frame window object came in
    size_t windowAtReport = 0;        // g_recLog's size when the last main-frame window object came
    void OnBeginLoading(View* v, uint64_t, bool main, const String&) override
    {
        if (!main) return;
        ++begun;
        loads.Begun(v);
    }
    void OnWindowObjectReady(View* v, uint64_t, bool main, const String&) override
    {
        if (!main) return;
        ++windows;
        windowInside = g_inside;
        windowAtReport = g_recLog.size();
        if (loads.WindowReady(v) && view && view->ul.get() == v) host->Forget(*view);
    }
    void OnDOMReady(View* v, uint64_t, bool main, const String&) override
    {
        if (!main) return;
        ++doms;
        loads.Ended(v);
        done = true;
    }
    void OnFailLoading(View* v, uint64_t, bool main, const String&, const String&, const String&, int) override
    {
        if (!main) return;
        ++fails;
        loads.Ended(v);
        done = true;
    }
};
// A View as LoadViewLocked makes it, with HostLoads, loading url.
static void NavView(MiniHost& h, HostView& hv, HostLoads& ld, const char* url)
{
    ViewConfig vc;
    vc.is_accelerated = false;
    vc.is_transparent = true;
    vc.clipboard_read_policy = ClipboardReadPolicy::Deny;
    vc.initial_focus = false;
    ld.host = &h;
    ld.view = &hv;
    hv.ul = g_r->CreateView(640, 420, vc, nullptr);
    hv.ul->set_load_listener(&ld);
    hv.ul->set_editor_listener(&g_recL);
    h.views.push_back(&hv);
    ld.done = false;
    g_inside = "LoadURL";
    hv.ul->LoadURL(url);
    g_inside = "-";
    for (int i = 0; i < 400 && !ld.done; ++i) Pump(1);
    Pump(10);
}
// Waits for the document a page's own navigation brings (its DOM ready, or the load's failure).
static void AwaitLoad(HostLoads& ld)
{
    for (int i = 0; i < 400 && !ld.done; ++i) Pump(1);
    Pump(10);
}
// NavigateView (url) or ReloadView (nullptr) as ApplyPendingLifecycle runs it: true when the call began a main-frame
// load inside it (else it kept the page). A load it began in a View that has had a window object leaves the page's
// state to the new document's window object (HostLoads), so a load that fails keeps the page and its field; in one
// that has had none, the state goes at once. await: wait for the load's DOM ready or failure (else a few frames).
static bool HostCall(MiniHost& h, HostView& hv, HostLoads& ld, const char* url, bool await = true)
{
    const int b0 = ld.begun;
    ld.done = false;
    g_inside = url ? "LoadURL" : "Reload";
    if (url) hv.ul->LoadURL(url);
    else hv.ul->Reload();
    g_inside = "-";
    const bool began = ld.begun != b0;
    if (began && !ld.loads.HasWindow(hv.ul.get())) h.Forget(hv);
    if (began && await) AwaitLoad(ld);
    else Pump(3);
    return began;
}
static std::string Url(View* v) { return v->url().utf8().data(); }
static const char* kNavA = R"(<!doctype html><html><body style="margin:0">
<form action="nav-a.html"><input id="q" name="q" style="position:absolute;left:10px;top:10px;width:200px;height:24px"></form>
<script>window.__a=1</script></body></html>)";
static const char* kNavB = R"(<!doctype html><html><body style="margin:0">
<button id="btn" style="position:absolute;left:10px;top:10px;width:200px;height:24px">b</button></body></html>)";
static const char* kPlain = R"(<!doctype html><html><body style="margin:0">
<input id="p" style="position:absolute;left:10px;top:10px;width:200px;height:24px"></body></html>)";
// An autofocus field, with the page's only script (the slow one) deferred or at the end of the body. Each page names
// its own script URL, so none finds it in the memory cache.
static const char* kAfNav = R"(<!doctype html><html><head><script defer src="af-slow.js?n"></script></head><body style="margin:0">
<input id="af" autofocus style="position:absolute;left:10px;top:10px;width:200px;height:24px"></body></html>)";
static const char* kAfDefer = R"(<!doctype html><html><head><script defer src="af-slow.js?d"></script></head><body style="margin:0">
<input id="af" autofocus style="position:absolute;left:10px;top:10px;width:200px;height:24px"></body></html>)";
static const char* kAfTail = R"(<!doctype html><html><body style="margin:0">
<input id="af" autofocus style="position:absolute;left:10px;top:10px;width:200px;height:24px"><script src="af-slow.js?t"></script></body></html>)";

static void SectionHost()
{
    std::printf("\n== host ==\n");
    g_recs.clear();
    HostView page{ 1, MakeView(kHostPage, "file:///host.html", false), {} };
    HostView hud{ 2, MakeView(kAutofocusPage, "file:///hud.html", false), {} };
    MiniHost h;
    h.views = { &page, &hud };
    Js(page.ul.get(), "0");

    h.Frame();
    Expect("H1 a background page's autofocus field: no keyboard, no IME", h.Kb(), "kb=0 ime=0");
    Expect("H1 its View has no focus (initial_focus false)", B(hud.ul->HasFocus()) + " report focus=" + B(hud.edit.view_has_focus), "0 report focus=0");

    h.Enter(1);
    h.Frame();
    Expect("H2 UI-mode entry, nothing focused", h.State(), "kb=0 ime=0 field=null");
    Expect("H2 the page's View has focus", B(page.ul->HasFocus()), "1");

    h.Click(page, "document.getElementById('a')");
    h.Frame();
    Expect("H3 click a text field: keyboard and IME", h.State(), "kb=1 ime=1 field={\"mode\":\"text\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");

    h.kbShown = false;   // the player closes it ({close:true})
    h.Click(page, "document.getElementById('a')");
    const bool reported = !g_recs.empty();
    h.Frame();
    Expect("H4 click the focused field again: no report, keyboard back", B(reported) + " " + h.Kb(), "0 kb=1 ime=1");

    h.kbShown = false;
    h.Click(page, "document.getElementById('s1')");
    const bool reported5 = !g_recs.empty();
    h.Frame();
    Expect("H5 pick a suggestion (mousedown preventDefault): field keeps focus, keyboard stays closed",
           B(reported5) + " " + h.Kb() + " active=" + Js(page.ul.get(), "document.activeElement.id", false) + " picked=" +
               Js(page.ul.get(), "String(__picked)", false), "0 kb=0 ime=1 active=a picked=1");

    h.Click(page, "document.getElementById('a')");
    h.Frame();
    h.Click(page, "document.getElementById('btn')");
    h.Frame();
    Expect("H6 click a button: keyboard and IME off", h.State(), "kb=0 ime=0 field=null");

    Js(page.ul.get(), "document.getElementById('b').focus()");
    h.Frame();
    Expect("H7 the page's own focus(), no click: keyboard up", h.Kb(), "kb=1 ime=1");

    h.kbShown = false;
    Js(page.ul.get(), "document.getElementById('b').setAttribute('inputmode','numeric')");
    h.Frame();
    Expect("H8 inputmode change on the focused field: relaid out, not raised", h.State(),
           "kb=0 ime=1 field={\"mode\":\"numeric\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");
    Expect("H8 its verdict", std::string(h.last.touched ? "touched " : "- ") + KbName(h.last.keyboard), "touched keep");

    VirtualKey(page.ul.get(), VK_TAB);   // b -> the suggestion list is not focusable -> btn? (whatever is next)
    Js(page.ul.get(), "document.getElementById('a').focus()");
    h.Frame();
    h.kbShown = false;
    VirtualKey(page.ul.get(), VK_TAB);
    const std::string tabbedTo = Js(page.ul.get(), "document.activeElement.id", false);
    h.Frame();
    Expect("H9 Tab from a to the next field raises", tabbedTo + " " + h.Kb(), "b kb=1 ime=1");

    Js(page.ul.get(), "document.getElementById('none').focus()");
    h.Frame();
    Expect("H10 inputmode=none: no keyboard, IME on", h.State(), "kb=0 ime=1 field=null");

    h.Click(page, "document.getElementById('a')");
    h.Frame();
    Js(page.ul.get(), "document.getElementById('ro').focus()");
    h.Frame();
    Expect("H11 a readonly field: keyboard and IME off", h.Kb(), "kb=0 ime=0");

    h.Click(page, "document.getElementById('ta')");
    h.Frame();
    Expect("H12 textarea", h.State(), "kb=1 ime=1 field={\"mode\":\"text\",\"enter\":\"\",\"multiline\":true,\"secret\":false}");
    h.Click(page, "document.getElementById('ce')");
    h.Frame();
    Expect("H12 contenteditable", h.State(), "kb=1 ime=1 field={\"mode\":\"text\",\"enter\":\"\",\"multiline\":true,\"secret\":false}");
    h.kbShown = false;
    {
        int fx = 0, fy = 0, ix = 0, iy = 0;
        Center(page.ul.get(), "document.getElementById('f')", fx, fy);
        Center(page.ul.get(), "document.getElementById('f').contentDocument.getElementById('fi')", ix, iy);
        const std::string fr = Js(page.ul.get(), "(function(){var b=document.getElementById('f').getBoundingClientRect();return Math.round(b.left)+','+Math.round(b.top);})()", false);
        int ox = 0, oy = 0;
        sscanf_s(fr.c_str(), "%d,%d", &ox, &oy);
        ClickAt(page.ul.get(), ox + ix, oy + iy);
        h.clicks.push_back({ static_cast<float>(ox + ix), static_cast<float>(oy + iy) });
        h.Frame();
        Expect("H12 a field in an iframe (click)", h.Kb() + " bounds in the top page's px=" +
               B(Ed::Inside(page.edit.bounds, static_cast<float>(ox + ix), static_cast<float>(oy + iy))), "kb=1 ime=1 bounds in the top page's px=1");
    }

    // Exit with a field focused, then re-entry.
    h.Click(page, "document.getElementById('a')");
    h.Frame();
    h.Exit();
    h.Frame();
    Expect("H13 exit: keyboard and IME off, the field reported unfocused", h.Kb() + " focus=" + B(page.edit.view_has_focus), "kb=0 ime=0 focus=0");
    h.Enter(1);
    h.Frame();
    Expect("H14 re-entry with the field still focused: IME on, no keyboard", h.Kb() + " focus=" + B(page.edit.view_has_focus), "kb=0 ime=1 focus=1");
    h.Click(page, "document.getElementById('a')");
    h.Frame();
    Expect("H14 then a click in it raises", h.Kb(), "kb=1 ime=1");

    // Switch to the HUD page, whose autofocus field was focused while its View had no focus.
    h.Switch(2);
    h.Frame();
    Expect("H15 switch to a page with a focused field: hidden, IME on, old View unfocused", h.Kb() + " old=" + B(page.ul->HasFocus()) +
           " new=" + B(hud.ul->HasFocus()), "kb=0 ime=1 old=0 new=1");
    h.Click(hud, "document.getElementById('auto')");
    h.Frame();
    Expect("H15 then a click in its field raises a numeric keyboard", h.State(),
           "kb=1 ime=1 field={\"mode\":\"numeric\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");
    h.Switch(1);
    h.Frame();
    Expect("H16 switch back: hidden, old View unfocused", h.Kb() + " old=" + B(hud.ul->HasFocus()), "kb=0 ime=1 old=0");

    // A navigation of the open page (NavigateView / ReloadView): the View keeps its focus; the new page's autofocus
    // reports from Update and raises, as a focus change inside the page does.
    h.Click(page, "document.getElementById('a')");
    h.Frame();
    h.Forget(page);
    Reload(page.ul.get(), kAutofocusPage, "file:///nav.html");
    std::string where;
    for (const Rec& r : g_recs) where += (where.empty() ? "" : ",") + r.inside;
    h.Frame();
    Note("H17 navigation to a page with an autofocus field: reported from", where.empty() ? "(none)" : where);
    Expect("H17 keyboard up for it", h.State(), "kb=1 ime=1 field={\"mode\":\"numeric\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");
    h.Forget(page);
    Reload(page.ul.get(), "<!doctype html><p>plain</p>", "file:///plain.html");
    h.Frame();
    Expect("H18 navigation to a page with no field: keyboard and IME off", h.Kb(), "kb=0 ime=0");

    // A release (RebuildViewAtScale, hibernation): a new View, created unfocused, then the focus marker.
    {
        Js(page.ul.get(), "document.body.innerHTML='<input id=z>';document.getElementById('z').focus()");
        h.Frame();
        Expect("H19 a field focused in the page about to be released", h.Kb(), "kb=1 ime=1");
        h.Forget(page);   // before the release, as ReleaseViewKeepRecordLocked does
        View* old = page.ul.get();
        page.ul = nullptr;
        Pump(2);
        size_t fromOld = 0;
        for (const Rec& r : g_recs) if (r.ul == old) ++fromOld;
        Expect("H19 releasing a View with a focused field reports nothing", std::to_string(fromOld), "0");
        h.Frame();
        Expect("H19 the release hides the keyboard", h.Kb(), "kb=0 ime=0");
        page.ul = MakeView(kAutofocusPage, "file:///rebuilt.html", false);
        std::string st = g_recs.empty() ? "(none)" : "focus=" + B(g_recs.back().st.view_has_focus);
        h.Frame();
        Expect("H19 rebuilt page's autofocus before the focus marker", st + " " + h.Kb(), "focus=0 kb=0 ime=0");
        h.FocusMarker(true);
        h.Frame();
        Expect("H19 after the focus marker: IME on, no keyboard", h.Kb(), "kb=0 ime=1");
        Reload(page.ul.get(), kHostPage, "file:///host.html");
        h.Forget(page);
        h.Frame();
    }

    // Bounds are reported when focus moves, not when the field moves: a click on a field that scrolled since.
    {
        h.Click(page, "document.getElementById('si')");
        h.Frame();
        const std::string before = std::to_string(static_cast<int>(page.edit.bounds.top));
        g_recs.clear();
        Js(page.ul.get(), "document.getElementById('scr').scrollTop=30");
        Pump(3);
        const std::string moved = g_recs.empty() ? "no report" : "report";
        h.kbShown = false;
        h.Click(page, "document.getElementById('si')");
        h.Frame();
        Expect("H20 field scrolled 30px after focus: no report, reported bounds unchanged, the re-click raises",
               moved + " " + before + "->" + std::to_string(static_cast<int>(page.edit.bounds.top)) + " kb=" + B(h.kbShown),
               "no report " + before + "->" + before + " kb=1");
        h.kbShown = false;
        int sx = 0, sy = 0;
        Center(page.ul.get(), "document.getElementById('si')", sx, sy);
        ClickAt(page.ul.get(), sx, sy + 30);   // where the field was before the scroll: now a control that keeps the focus
        h.clicks.push_back({ static_cast<float>(sx), static_cast<float>(sy + 30) });
        h.Frame();
        Expect("H20 a click where it was before the scroll does not", "kb=" + B(h.kbShown) + " active=" +
               Js(page.ul.get(), "document.activeElement.id", false), "kb=0 active=si");
    }
    // The same with another view's state reset after the raise (a load, wake, rebuild or navigation elsewhere): the
    // caret kept for the UI-mode view's field stays.
    {
        Js(page.ul.get(), "document.getElementById('scr').scrollTop=0");
        h.Click(page, "document.getElementById('btn')");
        h.Frame();
        h.Click(page, "document.getElementById('si')");
        h.Frame();
        h.Forget(hud);
        h.Frame();
        Js(page.ul.get(), "document.getElementById('scr').scrollTop=30");
        Pump(3);
        h.kbShown = false;
        h.Click(page, "document.getElementById('si')");
        h.Frame();
        const std::string reraised = "kb=" + B(h.kbShown);
        h.kbShown = false;
        int sx = 0, sy = 0;
        Center(page.ul.get(), "document.getElementById('si')", sx, sy);
        ClickAt(page.ul.get(), sx, sy + 30);
        h.clicks.push_back({ static_cast<float>(sx), static_cast<float>(sy + 30) });
        h.Frame();
        Expect("H20 after another view's reset: the re-click where the field is raises, where it was does not",
               reraised + " kb=" + B(h.kbShown) + " active=" + Js(page.ul.get(), "document.activeElement.id", false),
               "kb=1 kb=0 active=si");
    }

    // A page that replaces its own document (location, a reload, a form's submit by Enter) reports nothing, before
    // or after: the new document's window object drops the old page's state (HostLoads, as MlLoadListener). A
    // fragment change and a load that fails keep the page and its field.
    {
        HostLoads ld;
        HostView nav{ 3, nullptr, {} };
        NavView(h, nav, ld, "file:///nav-a.html");
        h.Switch(3);
        h.Frame();
        auto focusQ = [&]() {
            h.Click(nav, "document.getElementById('q')");
            h.Frame();
            return h.Kb();
        };
        auto reportsFrom = [&](size_t since) {
            size_t n = 0;
            for (size_t i = since; i < g_recLog.size(); ++i) n += g_recLog[i].ul == nav.ul.get();
            return std::to_string(n);
        };
        Expect("H21 a field focused in a page that navigates itself", focusQ(), "kb=1 ime=1");
        size_t mark = g_recLog.size();
        ld.done = false;
        Js(nav.ul.get(), "location.href='nav-b.html'", false);
        AwaitLoad(ld);
        const std::string sentHref = reportsFrom(mark);
        h.Frame();
        Expect("H21 location.href: no report; keyboard and IME off", sentHref + " " + h.State() + " " + Url(nav.ul.get()),
               "0 kb=0 ime=0 field=null file:///nav-b.html");
        ClickAt(nav.ul.get(), 110, 22);   // where the field was: the new page's button
        h.clicks.push_back({ 110.f, 22.f });
        h.Frame();
        Expect("H21 then a click where the field was does not raise it", h.Kb(), "kb=0 ime=0");
        ld.done = false;
        Js(nav.ul.get(), "location.href='nav-a.html'", false);
        AwaitLoad(ld);
        h.Frame();
        Expect("H21 back on the field's page", focusQ(), "kb=1 ime=1");
        mark = g_recLog.size();
        ld.done = false;
        Js(nav.ul.get(), "location.reload()", false);
        AwaitLoad(ld);
        const std::string sentReload = reportsFrom(mark);
        h.Frame();
        Expect("H21 location.reload(): no report; keyboard and IME off", sentReload + " " + h.Kb(), "0 kb=0 ime=0");
        focusQ();
        mark = g_recLog.size();
        ld.done = false;
        Type(nav.ul.get(), L"x\r");   // the VR keyboards' Enter: a key, so a plain form submits
        AwaitLoad(ld);
        const std::string sentSubmit = reportsFrom(mark);
        h.Frame();
        Expect("H21 Enter submits the form: no report; keyboard and IME off", sentSubmit + " " + h.Kb() + " " + Url(nav.ul.get()),
               "0 kb=0 ime=0 file:///nav-a.html?q=x");
        focusQ();
        Js(nav.ul.get(), "location.hash='z'");
        h.Frame();
        Expect("H21 a fragment change keeps the field", h.Kb() + " active=" + Js(nav.ul.get(), "document.activeElement.id", false),
               "kb=1 ime=1 active=q");
        const std::string before = Url(nav.ul.get());
        ld.done = false;
        Js(nav.ul.get(), "location.href='missing.html'", false);
        AwaitLoad(ld);
        h.Frame();
        Expect("H23 a load that fails keeps the page and its field", h.Kb() + " same url=" + B(Url(nav.ul.get()) == before) +
               " active=" + Js(nav.ul.get(), "document.activeElement.id", false), "kb=1 ime=1 same url=1 active=q");
        Expect("H23 and leaves no load noted", B(ld.loads.WindowReady(nav.ul.get())), "0");
        // Into a page whose autofocus field reports as it loads: this View has had a window object, so the new
        // document's comes as it begins, before that report, and its field raises the keyboard the player closed.
        h.kbShown = false;
        const size_t from = g_recLog.size();
        ld.done = false;
        Js(nav.ul.get(), "location.href='af-nav.html'", false);
        AwaitLoad(ld);
        size_t early = 0;
        for (size_t i = from; i < ld.windowAtReport && i < g_recLog.size(); ++i) early += g_recLog[i].ul == nav.ul.get();
        h.Frame();
        Expect("H24 location.href into a page with an autofocus field: its window object first, its field raises",
               "reports before it=" + std::to_string(early) + " " + h.State() + " active=" +
                   Js(nav.ul.get(), "document.activeElement.id", false) + " script=" +
                   Js(nav.ul.get(), "String(window.__af)", false),
               "reports before it=0 kb=1 ime=1 field={\"mode\":\"text\",\"enter\":\"\",\"multiline\":false,\"secret\":false} active=af script=1");

        // The same through the host (NavigateView, ReloadView): OnBeginLoading inside the call says a load began, and
        // a load that fails keeps the page and its field, as the page's own does.
        auto counts = [&](int b0, int d0, int f0, int w0) {
            return "dom=" + std::to_string(ld.doms - d0) + " fail=" + std::to_string(ld.fails - f0) + " windows=" +
                   std::to_string(ld.windows - w0) + " begun=" + std::to_string(ld.begun - b0);
        };
        auto active = [&]() { return Js(nav.ul.get(), "document.activeElement.id", false); };
        std::string at = Url(nav.ul.get());
        int b0 = ld.begun, d0 = ld.doms, f0 = ld.fails, w0 = ld.windows;
        bool began = HostCall(h, nav, ld, "file:///missing.html");
        h.Frame();
        Expect("H23 host Navigate to a missing page: a load begins inside the call, then fails",
               B(began) + " " + counts(b0, d0, f0, w0), "1 dom=0 fail=1 windows=0 begun=1");
        Expect("H23 ... and the page keeps its field and its keyboard", h.Kb() + " same url=" + B(Url(nav.ul.get()) == at) +
               " active=" + active(), "kb=1 ime=1 same url=1 active=af");
        const std::string afNav = g_fs.pages["af-nav.html"];
        g_fs.pages.erase("af-nav.html");
        b0 = ld.begun, d0 = ld.doms, f0 = ld.fails, w0 = ld.windows;
        began = HostCall(h, nav, ld, nullptr);
        g_fs.pages["af-nav.html"] = afNav;
        h.Frame();
        Expect("H23 host Reload with the page's file gone: a load begins inside the call, then fails",
               B(began) + " " + counts(b0, d0, f0, w0), "1 dom=0 fail=1 windows=0 begun=1");
        Expect("H23 ... and the page keeps its field and its keyboard", h.Kb() + " same url=" + B(Url(nav.ul.get()) == at) +
               " active=" + active(), "kb=1 ime=1 same url=1 active=af");
        // Each host call stops the load before it with no callback: no DOM ready or failure for nav-slow.html.
        b0 = ld.begun, d0 = ld.doms, f0 = ld.fails, w0 = ld.windows;
        HostCall(h, nav, ld, "file:///nav-slow.html", false);
        began = HostCall(h, nav, ld, "file:///missing.html");
        Pump(80);
        h.Frame();
        Expect("H23 host Navigate during another's load: that load stops with no callback, this one fails",
               B(began) + " " + counts(b0, d0, f0, w0) + " same url=" + B(Url(nav.ul.get()) == at) + " " + h.Kb(),
               "1 dom=0 fail=1 windows=0 begun=2 same url=1 kb=1 ime=1");
        b0 = ld.begun, d0 = ld.doms, f0 = ld.fails, w0 = ld.windows;
        HostCall(h, nav, ld, "file:///nav-slow.html", false);
        began = HostCall(h, nav, ld, (at + "#k").c_str());
        Pump(80);
        h.Frame();
        Expect("H23 host Navigate to the page's own fragment during a load: no load begins, the other stops with no callback",
               B(began) + " " + counts(b0, d0, f0, w0) + " " + h.Kb() + " active=" + active(),
               "0 dom=0 fail=0 windows=0 begun=1 kb=1 ime=1 active=af");
        b0 = ld.begun, d0 = ld.doms, f0 = ld.fails, w0 = ld.windows;
        began = HostCall(h, nav, ld, "file:///nav-b.html");
        h.Frame();
        Expect("H23 host Navigate to a page that loads: its window object drops the field",
               B(began) + " " + counts(b0, d0, f0, w0) + " " + h.State() + " " + Url(nav.ul.get()),
               "1 dom=1 fail=0 windows=1 begun=1 kb=0 ime=0 field=null file:///nav-b.html");
        h.Switch(1);
        h.Frame();
        std::erase(h.views, &nav);
        nav.ul->set_load_listener(nullptr);
    }

    // A page with no script of its own, in a View that never ran one, gets its window object only when a script first
    // runs there (here the host's, as an InteropCall does): no new document, so the focused field keeps its keyboard.
    {
        HostLoads ld;
        HostView plain{ 4, nullptr, {} };
        NavView(h, plain, ld, "file:///plain.html");
        const int windowsAtLoad = ld.windows;
        h.Switch(4);
        h.Frame();
        ClickAt(plain.ul.get(), 110, 22);
        h.clicks.push_back({ 110.f, 22.f });
        h.Frame();
        Expect("H22 a field clicked in a page with no script", h.Kb() + " window objects=" + std::to_string(windowsAtLoad),
               "kb=1 ime=1 window objects=0");
        Js(plain.ul.get(), "0");
        h.Frame();
        Expect("H22 the host's first script there makes its window object; the field keeps its keyboard",
               h.State() + " window objects=" + std::to_string(ld.windows) + " (" + ld.windowInside + ")",
               "kb=1 ime=1 field={\"mode\":\"text\",\"enter\":\"\",\"multiline\":false,\"secret\":false} window objects=1 (EvaluateScript)");
        h.Switch(1);
        h.Frame();
        std::erase(h.views, &plain);
        plain.ul->set_load_listener(nullptr);
    }

    // A new View that holds UI mode as its page loads (a rebuild or a wake of the open page, an entry while it waited
    // in the load queue: the focus marker runs on the View before its page exists). With the page's first script
    // deferred or at the end of the body, its autofocus field can report before the View's first window object, which
    // is no new document: the field keeps its keyboard, and a click in it brings back a keyboard the player closed.
    auto freshUiLoad = [&](std::uint64_t id, const char* label, const char* url) {
        HostLoads ld;
        HostView fresh{ id, nullptr, {} };
        ViewConfig vc;
        vc.is_accelerated = false;
        vc.is_transparent = true;
        vc.clipboard_read_policy = ClipboardReadPolicy::Deny;
        vc.initial_focus = false;
        ld.host = &h;
        ld.view = &fresh;
        fresh.ul = g_r->CreateView(640, 420, vc, nullptr);
        fresh.ul->set_load_listener(&ld);
        fresh.ul->set_editor_listener(&g_recL);
        h.views.push_back(&fresh);
        h.Switch(id);
        h.Frame();
        const size_t from = g_recLog.size();
        g_inside = "LoadURL";
        fresh.ul->LoadURL(url);
        g_inside = "-";
        for (int i = 0; i < 400 && !ld.done; ++i) { Pump(1); h.Frame(); }
        for (int i = 0; i < 20; ++i) { Pump(1); h.Frame(); }
        size_t before = 0;
        for (size_t i = from; i < ld.windowAtReport && i < g_recLog.size(); ++i)
            before += g_recLog[i].ul == fresh.ul.get() && g_recLog[i].st.is_mutable;
        Expect(std::string("H25 ") + label + ": the field reports before the View's first window object",
               std::to_string(before), "1");
        Expect(std::string("H25 ") + label + ": a new UI-mode View's autofocus field keeps its keyboard",
               h.State() + " active=" + Js(fresh.ul.get(), "document.activeElement.id", false) + " script=" +
                   Js(fresh.ul.get(), "String(window.__af)", false) + " window objects=" + std::to_string(ld.windows),
               "kb=1 ime=1 field={\"mode\":\"text\",\"enter\":\"\",\"multiline\":false,\"secret\":false} active=af script=1 window objects=1");
        h.kbShown = false;
        h.Click(fresh, "document.getElementById('af')");
        h.Frame();
        Expect(std::string("H25 ") + label + ": a click in it after the keyboard closed brings it back", h.Kb(), "kb=1 ime=1");
        h.Switch(1);
        h.Frame();
        std::erase(h.views, &fresh);
        fresh.ul->set_load_listener(nullptr);
    };
    freshUiLoad(5, "deferred script", "file:///af-defer.html");
    freshUiLoad(6, "end-of-body script", "file:///af-tail.html");

    // <input type=number>: the keyboard (the decimal keypad), never the IME, as a browser leaves the IME off there.
    h.Click(page, "document.getElementById('a')");
    h.Frame();
    Expect("H26 a text field takes the IME", h.Kb(), "kb=1 ime=1");
    h.Click(page, "document.getElementById('n')");
    h.Frame();
    Expect("H26 from it to a number field: keyboard, no IME", h.State(),
           "kb=1 ime=0 field={\"mode\":\"decimal\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");
    Js(page.ul.get(), "document.getElementById('f').contentDocument.getElementById('fi').focus()");
    h.Frame();
    Expect("H26 a text field in an iframe takes it again", h.Kb(), "kb=1 ime=1");
    Js(page.ul.get(), "document.getElementById('f').contentDocument.getElementById('fn').focus()");
    h.Frame();
    Expect("H26 a number field in an iframe: no IME", h.State(),
           "kb=1 ime=0 field={\"mode\":\"decimal\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");
    h.Click(page, "document.getElementById('b')");
    h.Frame();
    Expect("H26 back to a text field", h.State(), "kb=1 ime=1 field={\"mode\":\"text\",\"enter\":\"\",\"multiline\":false,\"secret\":false}");
    h.Exit();
    h.Frame();
    g_recs.clear();
}

// ── unfocused ────────────────────────────────────────────────────────────────
static const char* kAnimPage = R"(<!doctype html><html><head><style>
@keyframes k{from{opacity:0}to{opacity:1}} #box{width:10px;height:10px;background:red;animation:k 0.05s linear infinite}
</style></head><body><div id="box"></div><input id="t">
<script>
window.__raf=0;window.__timer=0;window.__anim=0;window.__evs=[];
(function f(){window.__raf++;requestAnimationFrame(f);})();
setInterval(function(){window.__timer++;},10);
document.getElementById('box').addEventListener('animationiteration',function(){window.__anim++;});
window.addEventListener('focus',function(){__evs.push('window.focus');});
window.addEventListener('blur',function(){__evs.push('window.blur');});
document.getElementById('t').addEventListener('focus',function(){__evs.push('t.focus');});
document.getElementById('t').addEventListener('blur',function(){__evs.push('t.blur');});
</script></body></html>)";

static void SectionUnfocused()
{
    std::printf("\n== unfocused ==\n");
    g_recs.clear();
    RefPtr<View> u = MakeView(kAnimPage, "file:///anim.html", false);
    RefPtr<View> f = MakeView(kAnimPage, "file:///anim2.html", true);
    Expect("U1 HasFocus: initial_focus false / true", B(u->HasFocus()) + "/" + B(f->HasFocus()), "0/1");
    Js(u.get(), "__raf=0;__timer=0;__anim=0", false);
    Js(f.get(), "__raf=0;__timer=0;__anim=0", false);
    for (int i = 0; i < 30; ++i) Pump(1);
    const int ru = std::stoi(Js(u.get(), "String(__raf)", false)), rf = std::stoi(Js(f.get(), "String(__raf)", false));
    const int tu = std::stoi(Js(u.get(), "String(__timer)", false)), tf = std::stoi(Js(f.get(), "String(__timer)", false));
    const int au = std::stoi(Js(u.get(), "String(__anim)", false)), af = std::stoi(Js(f.get(), "String(__anim)", false));
    Note("U2 over 30 frames, unfocused / focused: rAF, 10 ms timer, CSS iterations",
         std::to_string(ru) + "/" + std::to_string(rf) + ", " + std::to_string(tu) + "/" + std::to_string(tf) + ", " +
             std::to_string(au) + "/" + std::to_string(af));
    Expect("U2 an unfocused View animates and runs timers as a focused one",
           B(ru > 0 && ru * 2 >= rf) + B(tu > 0 && tu * 2 >= tf) + B(au > 0 && au * 2 >= af), "111");
    Note("U3 document.hasFocus() unfocused / focused", Js(u.get(), "String(document.hasFocus())", false) + "/" + Js(f.get(), "String(document.hasFocus())", false));
    g_recs.clear();
    Js(u.get(), "__evs.length=0;document.getElementById('t').focus()");
    std::string rep = g_recs.empty() ? "(none)" : Desc(g_recs.back().st);
    Expect("U4 focus() in the unfocused View: active element, report", Js(u.get(), "document.activeElement.id", false) + " " + rep,
           "t type=1 mutable=1 focus=0 mode=0 enter=0");
    Note("U4 its events, :focus", Js(u.get(), "JSON.stringify(__evs)+' '+String(!!document.querySelector(':focus'))", false));
    FireKey(u.get(), KeyEvent::kType_RawKeyDown, 'Q', In::SyntheticKeyLParam('Q', false));
    FireKey(u.get(), KeyEvent::kType_Char, L'q', In::SyntheticKeyLParam('Q', false));
    FireKey(u.get(), KeyEvent::kType_KeyUp, 'Q', In::SyntheticKeyLParam('Q', true));
    Pump(2);
    Note("U5 a key typed into the unfocused View's field", "value=" + Js(u.get(), "JSON.stringify(document.getElementById('t').value)", false));
    g_recs.clear();
    Js(u.get(), "__evs.length=0", false);
    g_inHostFocus = true;
    u->Focus();
    g_inHostFocus = false;
    Pump(2);
    rep = g_recs.empty() ? "(none)" : Desc(g_recs.back().st) + (g_recs.back().hostFocus ? " (inside Focus)" : "");
    Expect("U6 View::Focus: report", rep, "type=1 mutable=1 focus=1 mode=0 enter=0 (inside Focus)");
    Note("U6 its events, :focus", Js(u.get(), "JSON.stringify(__evs)+' '+String(!!document.querySelector(':focus'))", false));
    g_recs.clear();
}

// ── typed ────────────────────────────────────────────────────────────────────
static const char* kTypedPage = R"(<!doctype html><html><body style="margin:0;font:16px sans-serif">
<form id="frm"><input id="a" style="width:300px"></form><input id="b"><input id="ml" maxlength="3"><input id="ro" readonly>
<textarea id="ta"></textarea><button id="btn">b</button>
<script>
window.L=[];window.__submits=0;
document.getElementById('frm').addEventListener('submit',function(e){e.preventDefault();window.__submits++;});
['keydown','keypress','keyup','beforeinput','input','textInput'].forEach(function(n){document.addEventListener(n,function(e){
  var s=n;if(e.key!==undefined)s+='['+e.key+(e.code?' '+e.code:'')+(e.location?' L'+e.location:'')+']';
  if(e.inputType)s+='{'+e.inputType+'}';if(e.data!==undefined&&e.data!==null)s+='('+e.data+')';L.push(s);},true);});
function T(){var s=L.join(' ');L.length=0;return s;}
</script></body></html>)";

static void SectionTyped()
{
    std::printf("\n== typed ==\n");
    RefPtr<View> v = MakeView(kTypedPage, "file:///typed.html", false);
    View* p = v.get();
    p->Focus();
    Pump(2);
    auto val = [&](const char* id) { return Js(p, std::string("JSON.stringify(document.getElementById('") + id + "').value)", false); };
    Js(p, "document.getElementById('a').focus();T()");
    std::string ok = Type(p, L"hi");
    Expect("T1 letters: one InsertText, input events, no key events", ok + " " + Js(p, "T()", false) + " " + val("a"),
           "1 beforeinput{insertText}(hi) input{insertText}(hi) \"hi\"");
    ok = Type(p, L"\b");
    Expect("T2 Backspace: a key event that edits", Js(p, "T()", false) + " " + val("a"),
           "keydown[Backspace Backspace] beforeinput{deleteContentBackward} input{deleteContentBackward} keyup[Backspace Backspace] \"h\"");
    ok = Type(p, L"\r");
    Expect("T3 Enter: keydown, keypress and the form's submit", Js(p, "T()", false) + " submits=" + Js(p, "String(__submits)", false),
           "keydown[Enter Enter] keypress[Enter Enter] keyup[Enter Enter] submits=1");
    ok = Type(p, L"\t");
    Expect("T4 Tab: keydown, focus moves", Js(p, "T()", false) + " active=" + Js(p, "document.activeElement.id", false),
           "keydown[Tab Tab] keypress[Tab Tab] keyup[Tab Tab] active=b");
    Js(p, "document.getElementById('a').focus();document.getElementById('a').value='abc';T()");
    VirtualKey(p, VK_LEFT);
    Expect("T5 an arrow key from a bare vk: ArrowLeft, not the keypad", Js(p, "T()", false) + " caret=" +
           Js(p, "String(document.getElementById('a').selectionStart)", false), "keydown[ArrowLeft ArrowLeft] keyup[ArrowLeft ArrowLeft] caret=2");
    Js(p, "document.getElementById('a').value='';T()");
    ok = Type(p, std::wstring(L"x\xD83D\xDE00y"));
    Expect("T6 an emoji in a run: one insert", ok + " " + Js(p, "T()", false) + " len=" + Js(p, "String(document.getElementById('a').value.length)", false),
           "1 beforeinput{insertText}(x\xF0\x9F\x98\x80y) input{insertText}(x\xF0\x9F\x98\x80y) len=4");
    ok = Type(p, L".com");
    Expect("T7 '.com' is one edit", ok + " " + Js(p, "T()", false), "1 beforeinput{insertText}(.com) input{insertText}(.com)");
    // What InsertText answers (the host logs a dropped run when it is false).
    Js(p, "document.getElementById('ml').focus();document.getElementById('ml').value='abc';T()");
    // Each call on its own line where it changes what the next reads: the operands of + run in no fixed order.
    std::string r = Type(p, L"d");
    Expect("T8 InsertText into a full maxlength field", r + " " + val("ml"), "1 \"abc\"");
    Js(p, "document.getElementById('a').focus();document.getElementById('a').value='';"
          "document.getElementById('a').addEventListener('beforeinput',function(e){if(e.data==='!')e.preventDefault();});T()");
    const std::string cancelled = Type(p, L"!");
    Note("T9 InsertText whose beforeinput the page cancels", cancelled + " " + Js(p, "T()", false) + " " + val("a"));
    Js(p, "document.getElementById('ro').focus();T()");
    r = Type(p, L"z");
    Note("T10 InsertText into a readonly field", r + " " + Js(p, "T()", false) + " " + val("ro"));
    Js(p, "document.getElementById('btn').focus();T()");
    r = Type(p, L"z");
    Note("T11 InsertText with a button focused", r + " " + Js(p, "T()", false));
    Js(p, "document.activeElement.blur();T()");
    r = Type(p, L"z");
    Note("T12 InsertText with nothing focused", r + " " + Js(p, "T()", false));

    // The keyboard page's edit buttons, through CommandByName.
    auto cmd = [&](const char* name) {
        EditorCommand c{};
        if (!Ed::CommandByName(name, c)) return std::string("?");
        g_inside = "Execute";
        const bool r = p->editor()->Execute(c);
        g_inside = "-";
        Pump(2);
        return B(r);
    };
    Js(p, "document.getElementById('ta').focus();document.getElementById('ta').value='';T()");
    Type(p, L"one");
    std::string oks = cmd("selectAll");
    oks += cmd("cut");
    const std::string afterCut = val("ta");
    oks += cmd("undo");
    const std::string afterUndo = val("ta");
    oks += cmd("redo");
    const std::string afterRedo = val("ta");
    oks += cmd("paste");
    Expect("T13 selectAll, cut, undo, redo, paste", oks + " " + afterCut + " " + afterUndo + " " + afterRedo + " " + val("ta") +
           " clipboard=" + g_clip.last, "11111 \"\" \"one\" \"\" \"PASTED\" clipboard=one");
    Js(p, "T()");
    // A multi-line field: Enter from the keyboard is still a key, and it makes a newline.
    Js(p, "document.getElementById('ta').value='';T()");
    Type(p, L"l1\rl2");
    Expect("T14 textarea: Enter key between runs", val("ta"), "\"l1\\nl2\"");
}

// ── keyboard page ────────────────────────────────────────────────────────────
static const char* kStub = R"(<script>
window.__sent=[];window.__subs={};
window.magelight={on:function(c,f){window.__subs[c]=f;},send:function(c,a){window.__sent.push(c+':'+a);}};
function host(m){window.__subs['__kbtheme'](typeof m==='string'?m:JSON.stringify(m));}
function keys(){var r=document.querySelectorAll('#rows .row'),o=[];for(var i=0;i<r.length;i++){var k=r[i].querySelectorAll('.k'),s=[];
  for(var j=0;j<k.length;j++)s.push(k[j].textContent);o.push(s.join(' '));}return o.join(' / ');}
function sent(){var s=__sent.join(' ');__sent.length=0;return s;}
</script>)";

static void SectionKeyboard(const std::string& root)
{
    std::printf("\n== keyboard ==\n");
    std::ifstream f(root + "\\views\\keyboard\\index.html", std::ios::binary);
    if (!f) { Expect("keyboard page found", "no", "yes"); return; }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string html = ss.str();
    const size_t at = html.find("<head>");
    if (at == std::string::npos) { Expect("keyboard page has <head>", "no", "yes"); return; }
    html.insert(at + 6, kStub);
    RefPtr<View> v = MakeView(html.c_str(), "file:///keyboard.html", false, 980, 330);
    View* p = v.get();
    auto host = [&](const std::string& json) { Js(p, "host(" + json + ")"); };
    auto shot = [&](const char* name) {
        Pump(4);
        auto* surface = static_cast<BitmapSurface*>(p->surface());
        if (!surface || g_outDir.empty()) return;
        const std::string path = g_outDir + "\\" + name;
        if (surface->bitmap()->WritePNG(path.c_str())) Note(std::string("wrote ") + name, path);
    };
    const std::string qwerty = Js(p, "keys()", false);
    Note("K0 QWERTY", qwerty);
    shot("kb-text.png");
    host("{\"shown\":true,\"theme\":null,\"field\":null}");
    Expect("K1 no field: QWERTY, revealed", B(Js(p, "keys()", false) == qwerty) + " wait=" + Js(p, "String(document.documentElement.classList.contains('wait'))", false), "1 wait=false");
    host("{\"shown\":true,\"theme\":null,\"field\":{\"mode\":\"numeric\",\"enter\":\"go\",\"multiline\":false,\"secret\":false}}");
    Expect("K2 numeric: keypad, Enter labelled Go", Js(p, "keys()", false), "7 8 9 Bksp / 4 5 6 \xE2\x86\x90 \xE2\x86\x92 / 1 2 3 Go / Esc - 0 Close");
    shot("kb-numeric.png");
    host("{\"shown\":true,\"theme\":null,\"field\":{\"mode\":\"decimal\",\"enter\":\"\",\"multiline\":false,\"secret\":false}}");
    Expect("K3 decimal adds '.'", Js(p, "keys()", false), "7 8 9 Bksp / 4 5 6 \xE2\x86\x90 \xE2\x86\x92 / 1 2 3 Enter / Esc - 0 . Close");
    host("{\"shown\":true,\"theme\":null,\"field\":{\"mode\":\"tel\",\"enter\":\"next\",\"multiline\":false,\"secret\":false}}");
    Expect("K4 tel adds + * #", Js(p, "keys()", false), "7 8 9 Bksp / 4 5 6 \xE2\x86\x90 \xE2\x86\x92 / 1 2 3 Next / Esc * 0 # + Close");
    shot("kb-tel.png");
    host("{\"shown\":true,\"theme\":null,\"field\":{\"mode\":\"email\",\"enter\":\"done\",\"multiline\":false,\"secret\":false}}");
    const std::string email = Js(p, "keys()", false);
    shot("kb-email.png");
    Expect("K5 email: '@' and '.com' beside Space, Enter labelled Done",
           B(email.find("Esc @ Space .com") != std::string::npos) + B(email.find(" Done ") != std::string::npos), "11");
    host("{\"shown\":true,\"theme\":null,\"field\":{\"mode\":\"url\",\"enter\":\"\",\"multiline\":false,\"secret\":false}}");
    const std::string url = Js(p, "keys()", false);
    Expect("K6 url: '/' and '.com' beside Space", B(url.find("Esc / Space .com") != std::string::npos) + B(url.find(" Enter ") != std::string::npos), "11");
    host("{\"shown\":true,\"theme\":null,\"field\":{\"mode\":\"search\",\"enter\":\"\",\"multiline\":false,\"secret\":false}}");
    Expect("K7 search: QWERTY, Enter labelled Search", B(Js(p, "keys()", false).find(" Search ") != std::string::npos), "1");
    host("{\"shown\":true,\"theme\":null,\"field\":{\"mode\":\"<b>x</b>\",\"enter\":\"<img>\",\"multiline\":5}}");
    Expect("K8 values it does not know: QWERTY, plain Enter", B(Js(p, "keys()", false) == qwerty), "1");
    host("{\"shown\":true,\"theme\":null,\"field\":\"numeric\"}");
    Expect("K8 a field that is not an object: QWERTY", B(Js(p, "keys()", false) == qwerty), "1");
    host("{\"shown\":true,\"theme\":null,\"field\":{\"mode\":\"email\",\"enter\":\"\",\"multiline\":false,\"secret\":false}}");
    // Keys send what the host reads.
    auto press = [&](const std::string& sel) {
        int x = 0, y = 0;
        if (!Center(p, sel, x, y)) return std::string("(no element)");
        Js(p, "sent()", false);
        g_inside = "FireMouseEvent";
        MouseEvent m{ MouseEvent::kType_MouseMoved, x, y, MouseEvent::kButton_None };
        p->FireMouseEvent(m);
        m.type = MouseEvent::kType_MouseDown;
        m.button = MouseEvent::kButton_Left;
        p->FireMouseEvent(m);
        m.type = MouseEvent::kType_MouseUp;
        p->FireMouseEvent(m);
        g_inside = "-";
        Pump(2);
        return Js(p, "sent()", false);
    };
    auto keyByText = [](const std::string& t) {
        return "(function(){var k=document.querySelectorAll('#rows .k');for(var i=0;i<k.length;i++)if(k[i].textContent==='" + t + "')return k[i];})()";
    };
    Expect("K9 '.com' sends one string", press(keyByText(".com")), "__key:{\"c\":\".com\"}");
    Expect("K9 '@'", press(keyByText("@")), "__key:{\"c\":\"@\"}");
    Expect("K9 Enter is a key", press(keyByText("Enter")), "__key:{\"vk\":13}");
    Expect("K9 Backspace is a key", press(keyByText("Bksp")), "__key:{\"vk\":8}");
    const char* cmds[][2] = { { "undo", "Undo" }, { "redo", "Redo" }, { "selectAll", "Select all" }, { "cut", "Cut" }, { "copy", "Copy" }, { "paste", "Paste" } };
    std::string got, want;
    for (auto& c : cmds) {
        got += press(std::string("document.querySelector('[data-cmd=\"") + c[0] + "\"]')") + " ";
        want += std::string("__key:{\"cmd\":\"") + c[0] + "\"} ";
    }
    Expect("K10 the edit buttons send their command, and no drag starts", got, want);
    Expect("K10 their labels", Js(p, "Array.prototype.map.call(document.querySelectorAll('[data-cmd]'),function(e){return e.textContent;}).join('|')", false),
           "Undo|Redo|Select all|Cut|Copy|Paste");
    Expect("K11 the bar still drags", press("document.getElementById('bar').querySelector('h1')"), "__key:{\"drag\":\"start\"} __key:{\"drag\":\"end\"}");
    host("{\"shown\":true,\"theme\":null,\"field\":{\"mode\":\"numeric\",\"enter\":\"\",\"multiline\":false,\"secret\":false}}");
    Expect("K12 the keypad's keys", press(keyByText("7")) + " " + press(keyByText("-")) + " " + press(keyByText("Close")),
           "__key:{\"c\":\"7\"} __key:{\"c\":\"-\"} __key:{\"close\":true}");
    host("{\"shown\":false,\"theme\":null,\"field\":null}");
    Expect("K13 shown false blanks the page", Js(p, "String(document.documentElement.classList.contains('wait'))", false), "true");
}

static int ToolMain(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: editorprobe <sdkDir> <repoRoot> [section ...]\n"); return 2; }
    const std::string sdk = argv[1], root = argv[2];
    g_outDir = argv[0];
    const size_t slash = g_outDir.find_last_of("\\/");
    g_outDir = slash == std::string::npos ? std::string(".") : g_outDir.substr(0, slash);
    std::set<std::string> want;
    for (int i = 3; i < argc; ++i) want.insert(argv[i]);
    auto run = [&](const char* s) { return want.empty() || want.count(s); };
    if (run("core")) SectionCore();
    Config cfg;
    Platform::instance().set_config(cfg);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    g_fs.base = GetPlatformFileSystem(String(sdk.c_str()));
    g_fs.pages["nav-a.html"] = kNavA;
    g_fs.pages["nav-b.html"] = kNavB;
    g_fs.pages["nav-slow.html"] = kNavB;
    g_fs.pages["plain.html"] = kPlain;
    g_fs.pages["af-nav.html"] = kAfNav;
    g_fs.pages["af-defer.html"] = kAfDefer;
    g_fs.pages["af-tail.html"] = kAfTail;
    g_fs.pages["af-slow.js"] = "window.__af=1;";
    Platform::instance().set_file_system(&g_fs);
    Platform::instance().set_clipboard(&g_clip);
    g_r = Renderer::Create();
    if (run("fields")) SectionFields();
    if (run("host")) SectionHost();
    if (run("unfocused")) SectionUnfocused();
    if (run("typed")) SectionTyped();
    if (run("keyboard")) SectionKeyboard(root);
    g_r = nullptr;
    std::printf("\n%d failed\n", g_fails);
    return g_fails;
}

// MG_PAGE_FIBER=1: on the page fiber, as Magelight runs pages (../fiber-gc-test/FiberRun.h).
int main(int argc, char** argv) { return FiberRun::Run(argc, argv, &ToolMain); }
