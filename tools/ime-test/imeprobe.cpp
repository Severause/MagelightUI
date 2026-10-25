// imeprobe - checks the Ultralight 2.0 Editor calls Magelight's native IME path relies on, on a CPU view:
// EditorListener focus states (text, password, textarea, contenteditable, a field in an iframe), SetComposition
// (value, composition events, caret / composition bounds), CommitComposition, CancelComposition, an empty
// composition, OnDiscardComposition when the page moves focus mid-composition, the space the candidate list's anchor
// is in (a field in the page, under a zoom on <body>, in an iframe), a click in the composing field, and a commit into
// a field with a maxlength, and number fields (no IME: src/EditStateCore.h). MG_PAGE_FIBER=1 runs it on the page fiber
// (../fiber-gc-test/FiberRun.h).
#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/Editor.h>
#include <Ultralight/Listener.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include "EditStateCore.h"
#include "../fiber-gc-test/FiberRun.h"

using namespace ultralight;

static const char* kPage = R"(<!doctype html><html><body style="font:16px sans-serif">
<input id="a" style="width:300px"><br><input id="p" type="password"><br><textarea id="t"></textarea><input id="m" maxlength="7"><input id="n" type="number" value="5"><br>
<div id="c" contenteditable="true" style="width:300px;height:40px;border:1px solid #888"></div>
<iframe id="f" srcdoc="<input id=fi><input id=fn type=number>" style="width:320px;height:60px"></iframe>
<script>
window.__log=[];
function hook(d){['compositionstart','compositionupdate','compositionend','input','keydown'].forEach(function(n){
  d.addEventListener(n,function(e){__log.push(n+':'+(e.data===undefined?'':e.data)+(e.isComposing?'*':''));},true);});
  d.addEventListener('mousedown',function(e){top.__hit=e.target.id||e.target.tagName;},true);}
hook(document);
document.getElementById('f').addEventListener('load',function(){try{hook(this.contentDocument);}catch(x){}});
</script></body></html>)";

struct Settle : LoadListener { bool done = false; void OnDOMReady(View*, uint64_t, bool, const String&) override { done = true; } };

struct EdL : EditorListener {
    int discards = 0, updates = 0;
    EditableState last;
    int changes = 0;
    void OnChangeEditableState(View*, const EditableState& s) override { last = s; ++changes; }
    void OnUpdateComposition(View*) override { ++updates; }
    void OnDiscardComposition(View*) override { ++discards; }
};

static RefPtr<Renderer> g_r;
static RefPtr<View> g_v;
static void Pump(int n = 6)
{
    for (int i = 0; i < n; ++i) {
        g_r->Update();
        g_r->RefreshDisplay(0);
        g_r->Render();
        FiberRun::BetweenFrames();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
static std::string Js(const char* s) { String ex; String out = g_v->EvaluateScript(s, &ex); Pump(2); return std::string(out.utf8().data()); }
static void Clear() { Js("__log.length=0"); }

static int g_fail = 0;
static void Check(bool ok, const char* what, const std::string& detail = {})
{
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : "  -> ", detail.c_str());
    if (!ok) ++g_fail;
}
static const char* TypeName(EditableType t)
{
    switch (t) { case EditableType::None: return "None"; case EditableType::Text: return "Text"; case EditableType::Password: return "Password";
    case EditableType::TextArea: return "TextArea"; case EditableType::ContentEditable: return "ContentEditable"; default: return "Other"; }
}
static String U(const char* utf8) { return String(utf8); }
static std::string R(const Rect& r)
{
    char b[96];
    std::snprintf(b, sizeof b, "%.1f,%.1f %.1fx%.1f", r.left, r.top, r.right - r.left, r.bottom - r.top);
    return b;
}
// A left press and release at (x, y), viewport CSS px.
static void Click(float x, float y)
{
    MouseEvent m;
    m.x = static_cast<int>(x);
    m.y = static_cast<int>(y);
    m.type = MouseEvent::kType_MouseMoved;
    m.button = MouseEvent::kButton_None;
    g_v->FireMouseEvent(m);
    m.type = MouseEvent::kType_MouseDown;
    m.button = MouseEvent::kButton_Left;
    g_v->FireMouseEvent(m);
    Pump(2);
    m.type = MouseEvent::kType_MouseUp;
    g_v->FireMouseEvent(m);
    Pump(2);
}

static int ToolMain(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: imeprobe <sdkDir>\n"); return 2; }
    const std::string sdk = argv[1];
    Config cfg;
    Platform::instance().set_config(cfg);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    Platform::instance().set_file_system(GetPlatformFileSystem(String(sdk.c_str())));
    g_r = Renderer::Create();
    ViewConfig vc;
    vc.is_accelerated = false;
    g_v = g_r->CreateView(500, 400, vc, nullptr);
    Settle ld; EdL el;
    g_v->set_load_listener(&ld);
    g_v->set_editor_listener(&el);
    g_v->LoadHTML(kPage, "file:///page.html");
    for (int i = 0; i < 400 && !ld.done; ++i) Pump(1);
    Pump(30);
    g_v->Focus();
    Pump();
    Editor* ed = g_v->editor();

    // Focus states.
    Js("document.getElementById('a').focus()");
    Check(el.last.type == EditableType::Text && el.last.is_mutable && el.last.view_has_focus, "focus <input>: Text, mutable, view focus",
          std::string(TypeName(el.last.type)) + " mut=" + std::to_string(el.last.is_mutable) + " vf=" + std::to_string(el.last.view_has_focus));

    // Compose, update, commit.
    Clear();
    CompositionSegment seg; seg.range = { 0, 2 }; seg.thick = true;
    bool okc = ed->SetComposition(U(u8"にほ"), &seg, 1, CompositionRange{ 2, 2 });
    Pump();
    std::string v = Js("document.getElementById('a').value");
    Check(okc && v == u8"にほ" && ed->HasComposition(), "SetComposition shows the pre-edit in the field", v);
    Rect cb = ed->GetCompositionCharacterBounds(0);
    Check(!cb.IsEmpty(), "composition character bounds", std::to_string(cb.left) + "," + std::to_string(cb.top) + " " +
          std::to_string(cb.right - cb.left) + "x" + std::to_string(cb.bottom - cb.top));
    ed->SetComposition(U(u8"日本"), nullptr, 0, CompositionRange{ 2, 2 });
    Pump();
    ed->CommitComposition(U(u8"日本"));
    Pump();
    v = Js("document.getElementById('a').value");
    std::string log = Js("JSON.stringify(__log)");
    Check(v == u8"日本" && !ed->HasComposition(), "commit leaves the final text once", v);
    Check(log.find("compositionstart") != std::string::npos && log.find("compositionupdate") != std::string::npos &&
          log.find("compositionend") != std::string::npos, "composition events reach the page", log);
    Rect caret = ed->GetCaretBounds();
    Check(!caret.IsEmpty(), "caret bounds after commit");

    // Cancel removes the pre-edit and keeps the earlier text.
    ed->SetComposition(U(u8"ご"), nullptr, 0, CompositionRange{ 1, 1 });
    Pump();
    ed->CancelComposition();
    Pump();
    v = Js("document.getElementById('a').value");
    Check(v == u8"日本" && !ed->HasComposition(), "cancel removes the pre-edit", v);

    // An End with no composition (after a commit) changes nothing.
    bool cancelled = ed->CancelComposition();
    Check(!cancelled && Js("document.getElementById('a').value") == u8"日本", "cancel without a composition is a no-op");

    // Commit with no composition inserts at the caret (an IME that commits directly).
    ed->CommitComposition(U(u8"語"));
    Pump();
    v = Js("document.getElementById('a').value");
    Check(v == u8"日本語", "commit without a composition inserts", v);

    // The page moves focus mid-composition: OnDiscardComposition, the text kept.
    const int d0 = el.discards;
    ed->SetComposition(U(u8"て"), nullptr, 0, CompositionRange{ 1, 1 });
    Pump();
    Js("document.getElementById('t').focus()");
    Pump();
    v = Js("document.getElementById('a').value");
    Check(el.discards > d0, "page focus change discards the composition", "discards=" + std::to_string(el.discards - d0) + " value=" + v);
    Check(el.last.type == EditableType::TextArea, "textarea reports TextArea", TypeName(el.last.type));

    // Other field kinds.
    Js("document.getElementById('p').focus()");
    Check(el.last.type == EditableType::Password, "password reports Password", TypeName(el.last.type));
    Js("document.getElementById('c').focus()");
    Check(el.last.type == EditableType::ContentEditable && el.last.is_mutable, "contenteditable reports ContentEditable", TypeName(el.last.type));
    ed->SetComposition(U(u8"かな"), nullptr, 0, CompositionRange{ 2, 2 });
    Pump();
    ed->CommitComposition(U(u8"仮名"));
    Pump();
    Check(Js("document.getElementById('c').textContent") == u8"仮名", "compose + commit in contenteditable", Js("document.getElementById('c').textContent"));
    Js("document.getElementById('f').contentDocument.getElementById('fi').focus()");
    Check(el.last.type == EditableType::Text && el.last.is_mutable, "a field inside an iframe reports Text", TypeName(el.last.type));
    ed->SetComposition(U(u8"いふ"), nullptr, 0, CompositionRange{ 2, 2 });
    Pump();
    ed->CommitComposition(U(u8"枠"));
    Pump();
    Check(Js("document.getElementById('f').contentDocument.getElementById('fi').value") == u8"枠", "compose + commit inside the iframe",
          Js("document.getElementById('f').contentDocument.getElementById('fi').value"));

    // The candidate list's anchor (Editor::GetCompositionCharacterBounds, which the host sends the page core) is in
    // viewport CSS px, FireMouseEvent's space, wherever the field is: a press at it lands on the field.
    auto anchorHits = [&](const char* focusJs, const char* id, const char* what) {
        Js(focusJs);
        ed->SetComposition(U(u8"とうきょう"), nullptr, 0, CompositionRange{ 5, 5 });
        Pump();
        const Rect r = ed->GetCompositionCharacterBounds(0);
        ed->CancelComposition();
        Pump();
        Js("top.__hit=''");
        Click((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        const std::string hit = Js("top.__hit");
        Check(!r.IsEmpty() && hit == id, what, R(r) + " hit '" + hit + "'");
    };
    Js("document.getElementById('a').value=''");
    anchorHits("document.getElementById('a').focus()", "a", "anchor: a field in the page");
    Js("document.body.style.zoom='1.25'");
    anchorHits("document.getElementById('a').focus()", "a", "anchor: a field under a zoom on <body>");
    anchorHits("document.getElementById('f').contentWindow.focus();document.getElementById('f').contentDocument.getElementById('fi').focus()",
               "fi", "anchor: a field in an iframe, under the zoom");
    Js("document.body.style.zoom=''");
    anchorHits("document.getElementById('f').contentWindow.focus();document.getElementById('f').contentDocument.getElementById('fi').focus()",
               "fi", "anchor: a field in an iframe");
    Js("document.getElementById('f').contentDocument.getElementById('fi').value=''");

    // A click in the composing field keeps the reading: 2.0 discards nothing, the composition goes on where it was
    // (the IME still holds it), and the commit lands there. A click in another field ends it as typed.
    Js("var a=document.getElementById('a');a.focus();a.value='hello ';a.setSelectionRange(6,6)");
    const int d1 = el.discards;
    ed->SetComposition(U(u8"とうきょう"), nullptr, 0, CompositionRange{ 5, 5 });
    Pump();
    const Rect comp = ed->GetCompositionCharacterBounds(0);
    const std::string aLeft = Js("document.getElementById('a').getBoundingClientRect().left");
    Click(static_cast<float>(std::atof(aLeft.c_str())) + 8.f, (comp.top + comp.bottom) / 2);
    v = Js("document.getElementById('a').value");
    Check(v == u8"hello とうきょう" && ed->HasComposition() && el.discards == d1 && Js("top.__hit") == "a",
          "a click on the field's text mid-composition keeps the reading composing",
          v + " has=" + std::to_string(ed->HasComposition()) + " discards=" + std::to_string(el.discards - d1));
    ed->SetComposition(U(u8"東京"), &seg, 1, CompositionRange{ 2, 2 });
    Pump();
    Check(Js("document.getElementById('a').value") == u8"hello 東京", "the IME's next change replaces the reading in place",
          Js("document.getElementById('a').value"));
    const Rect onComp = ed->GetCompositionCharacterBounds(1);
    Click((onComp.left + onComp.right) / 2, (onComp.top + onComp.bottom) / 2);
    Check(ed->HasComposition() && el.discards == d1 && Js("document.getElementById('a').value") == u8"hello 東京",
          "a click on the composition itself keeps it composing");
    ed->CommitComposition(U(u8"東京"));
    Pump();
    Check(Js("document.getElementById('a').value") == u8"hello 東京" && !ed->HasComposition(), "and the commit lands there, once",
          Js("document.getElementById('a').value"));
    ed->SetComposition(U(u8"とうきょう"), nullptr, 0, CompositionRange{ 5, 5 });
    Pump();
    const std::string tMid = Js("var r=document.getElementById('t').getBoundingClientRect();(r.left+r.width/2)+','+(r.top+r.height/2)");
    const auto comma = tMid.find(',');
    Click(static_cast<float>(std::atof(tMid.c_str())), static_cast<float>(std::atof(tMid.c_str() + comma + 1)));
    Check(el.discards > d1 && !ed->HasComposition() && Js("document.getElementById('a').value") == u8"hello 東京とうきょう",
          "a click in another field keeps the reading as typed (OnDiscardComposition)", Js("document.getElementById('a').value"));

    // A commit into a field with a maxlength replaces the whole reading as one edit and fits, the caret anywhere in it.
    Js("var m=document.getElementById('m');m.focus();m.value='ab';m.setSelectionRange(2,2)");
    Clear();
    ed->SetComposition(U(u8"とうきょう"), nullptr, 0, CompositionRange{ 3, 3 });
    Pump();
    ed->CommitComposition(U(u8"とうきょう"));
    Pump();
    log = Js("JSON.stringify(__log)");
    Check(Js("document.getElementById('m').value") == u8"abとうきょう", "a commit fits a maxlength field",
          Js("document.getElementById('m').value"));
    Check(log.find(u8"compositionend:とうきょう") != std::string::npos, "compositionend carries the commit", log);

    // Number fields take no IME (as a browser leaves them): Ultralight reports one as a text field, so the host asks
    // the page (EditState::kNumberFieldScript) and the IME stays detached, so nothing reaches the page core's box.
    namespace Ed = Magelight::EditState;
    auto noIme = [&](const char* focusJs) {
        Js(focusJs);
        const std::string answer = Js(Ed::kNumberFieldScript);
        return std::string(TypeName(el.last.type)) + " " + answer + " ime=" +
               (Ed::WantsIme(el.last, Ed::MayBeNumberField(el.last) && answer == "1") ? "1" : "0");
    };
    Check(noIme("document.getElementById('n').focus()") == "Text 1 ime=0", "a number field: no IME",
          noIme("document.getElementById('n').focus()"));
    Check(noIme("document.getElementById('f').contentWindow.focus();document.getElementById('f').contentDocument.getElementById('fn').focus()") ==
              "Text 1 ime=0", "a number field in an iframe: no IME");
    Check(noIme("document.getElementById('f').contentDocument.getElementById('fi').focus()") == "Text 0 ime=1",
          "a text field in the same iframe takes it");
    Check(noIme("document.getElementById('a').focus()") == "Text 0 ime=1", "a text field takes it");
    // From a text field to a number field mid-composition: the page keeps the reading as typed and discards the
    // composition (OnDiscardComposition: the host cancels the IME's copy, and detaches it for the number field).
    Js("var a=document.getElementById('a');a.value='q';a.setSelectionRange(1,1)");
    const int d3 = el.discards;
    ed->SetComposition(U(u8"かず"), nullptr, 0, CompositionRange{ 2, 2 });
    Pump();
    const std::string toNumber = noIme("document.getElementById('n').focus()");
    Check(el.discards > d3 && !ed->HasComposition() && Js("document.getElementById('a').value") == u8"qかず" &&
              Js("document.getElementById('n').value") == "5" && toNumber == "Text 1 ime=0",
          "text to a number field mid-composition: the reading kept as typed, no IME in the number field",
          Js("document.getElementById('a').value") + " n=" + Js("document.getElementById('n').value") + " " + toNumber);

    // A long reading (50 units, four clauses) composed a character at a time, with a partial commit of its first
    // clause (Enter on it, or the IME auto-committing the front of a long reading): the host's Commit then Compose.
    {
        Js("var a=document.getElementById('a');a.focus();a.value='>';a.setSelectionRange(1,1)");
        Clear();
        const std::u16string reading = u"きょうはとてもいいてんきなのでこうえんまでさんぽにいってそれからえきまえのきっさてんでこーひーをのむ";
        for (std::size_t n = 1; n <= reading.size(); ++n) {
            const String16 t16(reinterpret_cast<const Char16*>(reading.data()), n);
            ed->SetComposition(String(t16), nullptr, 0, CompositionRange{ static_cast<uint32_t>(n), static_cast<uint32_t>(n) });
        }
        Pump();
        const std::string full = Js("document.getElementById('a').value");
        CompositionSegment segs[4];
        const uint32_t cut[5] = { 0, 4, 15, 30, static_cast<uint32_t>(reading.size()) };
        for (int i = 0; i < 4; ++i) {
            segs[i].range = { cut[i], cut[i + 1] };
            segs[i].thick = i == 0;
        }
        const String16 all16(reinterpret_cast<const Char16*>(reading.data()), reading.size());
        ed->SetComposition(String(all16), segs, 4, CompositionRange{ 0, 4 });
        Pump();
        Check(reading.size() >= 40 && Js("document.getElementById('a').value.length") == std::to_string(1 + reading.size()) &&
                  ed->HasComposition(), "a 50-unit reading in four clauses, typed one character at a time",
              std::to_string(reading.size()) + " units, value length " + Js("document.getElementById('a').value.length"));
        // Partial commit: the first clause becomes text, the rest composes on after it.
        ed->CommitComposition(U(u8"今日は"));
        const String16 rest16(reinterpret_cast<const Char16*>(reading.data()) + 4, reading.size() - 4);
        ed->SetComposition(String(rest16), nullptr, 0, CompositionRange{ 0, 0 });
        Pump();
        const std::string afterPartial = Js("document.getElementById('a').value");
        Check(afterPartial.rfind(u8">今日は", 0) == 0 && Js("document.getElementById('a').value.length") == std::to_string(1 + 3 + reading.size() - 4) &&
                  ed->HasComposition(), "a partial commit keeps the first clause and composes the rest after it", afterPartial);
        // The rest cancelled (the second Escape): only the reading goes, committed text stays.
        ed->CancelComposition();
        Pump();
        Check(Js("document.getElementById('a').value") == u8">今日は" && !ed->HasComposition(),
              "cancelling the rest leaves the committed text", Js("document.getElementById('a').value"));
        // The IME ending a composition while the game window has no focus (Alt+Tab): kept as typed.
        ed->SetComposition(U(u8"こうえん"), nullptr, 0, CompositionRange{ 4, 4 });
        Pump();
        ed->FinishComposition();
        Pump();
        log = Js("JSON.stringify(__log)");
        Check(Js("document.getElementById('a').value") == u8">今日はこうえん" && !ed->HasComposition(),
              "a composition the IME ends while the window has no focus is kept as typed", Js("document.getElementById('a').value"));
        Check(full.size() > 0 && log.find("compositionend") != std::string::npos, "the composition events came through", log.substr(0, 200));
    }

    // UI mode ending or moving mid-composition: View::Unfocus leaves the composition open (the next IME change would
    // replace the reading), so the host finishes it first (ImeFinishComposition), which keeps it as typed.
    Js("var a=document.getElementById('a');a.focus();a.value='x';a.setSelectionRange(1,1)");
    const int d2 = el.discards;
    ed->SetComposition(U(u8"とう"), nullptr, 0, CompositionRange{ 2, 2 });
    Pump();
    g_v->Unfocus();
    Pump();
    Check(ed->HasComposition() && el.discards == d2, "View::Unfocus leaves a composition open (why the host finishes it)",
          "has=" + std::to_string(ed->HasComposition()) + " discards=" + std::to_string(el.discards - d2));
    g_v->Focus();
    Pump();
    Clear();
    const bool finished = ed->FinishComposition();
    g_v->Unfocus();
    Pump();
    log = Js("JSON.stringify(__log)");
    Check(finished && !ed->HasComposition() && Js("document.getElementById('a').value") == u8"xとう" &&
              log.find(u8"compositionend:とう") != std::string::npos,
          "FinishComposition before Unfocus keeps the reading as typed, with its compositionend", log);
    g_v->Focus();
    Pump();
    ed->SetComposition(U(u8"に"), nullptr, 0, CompositionRange{ 1, 1 });
    Pump();
    ed->CommitComposition(U(u8"二"));
    Pump();
    Check(Js("document.getElementById('a').value") == u8"xとう二", "the next composition after it adds to the text",
          Js("document.getElementById('a').value"));

    // View unfocus.
    g_v->Unfocus();
    Pump();
    Check(!el.last.view_has_focus, "View::Unfocus reports view_has_focus false");
    // Blur to nothing.
    g_v->Focus(); Pump();
    Js("document.activeElement.blur(); document.body.focus()");
    Js("document.getElementById('f').blur()");
    Check(el.last.type == EditableType::None || !el.last.is_mutable, "blur reports no editable", TypeName(el.last.type));

    std::printf("updates=%d discards=%d changes=%d\n%s: %d failure(s)\n", el.updates, el.discards, el.changes, g_fail ? "FAILED" : "PASSED", g_fail);
    g_v = nullptr; g_r = nullptr;
    return g_fail ? 1 : 0;
}

// MG_PAGE_FIBER=1: on the page fiber, as Magelight runs pages (../fiber-gc-test/FiberRun.h).
int main(int argc, char** argv) { return FiberRun::Run(argc, argv, &ToolMain); }
