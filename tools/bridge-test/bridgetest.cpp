// bridgetest - src/JsBridgeCore.h and the page's __mlNative on Ultralight 2.0, on a CPU view, no game:
//   A  __mlNative as InstallBridgeShims makes it (JSObjectMakeFunctionWithData + the view id as user data): a real
//      function, the id on every call form (call, apply, bind, a method, Reflect.apply, a microtask, a timer), a
//      re-install, a navigation, a page whose policy forbids eval and inline script
//   B  Describe: the text, url, line, column and stack of every kind of thrown value the bridge sees
//   C  IsErrorObject
//   D  Evaluate against View::EvaluateScript: the same result and exception strings, the same UTF-8 handling, the
//      same global scope, no microtask drained inside the call, a relative import() resolved the same way (a mod's
//      script runs unnamed)
//   E  ConsoleArgText on real console messages (an Error with fields of its own keeps their JSON); a thrown host
//      script reaches no console listener (why the host reports it itself); a console message from a JSC built-in
//      has no source and a line (why the devMode banner names no unnamed line for one)
//   F  LogText / FullText / StackHasLocation / ScriptLine: the unnamed-script line only for an error made with no named
//      script on its stack, whoever owns the code that made it
// Build and run: build.bat [out dir]. Exit code = the number of failed checks.
#include <AppCore/Platform.h>
#include <JavaScriptCore/JavaScript.h>
#include <Ultralight/Ultralight.h>

#include "JsBridgeCore.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "../fiber-gc-test/FiberRun.h"

using namespace ultralight;
namespace JB = Magelight::JsBridge;

static const char* kLibJs =
    "function thrower(a) { return inner(a); }\n"
    "function inner(a) {\n"
    "  throw new TypeError('bad ' + a);\n"
    "}\n"
    "function logErr() { try { inner('logged'); } catch (e) { console.error(e); console.error('ctx', e); console.warn({ plain: 1 }); } }\n"
    "function logOther() { var c = {}; c.c = c; console.log(function f() {}, c, Symbol('s'), 5, null, undefined); }\n"
    "function uncaughtLater() { setTimeout(function () { inner('late'); }, 0); }\n"
    "function domErr() { document.querySelector('<<'); }\n"
    "function logFields() {\n"
    "  var e = new Error('x'); e.code = 'E_FOO'; e.status = 404; console.error(e);\n"
    "  class HttpError extends Error { constructor(s, b) { super('http ' + s); this.status = s; this.body = b; } }\n"
    "  console.error('request failed', new HttpError(404, { reason: 'no such page' }));\n"
    "  var c = new Error('cyc'); c.self = c; console.error(c);\n"
    "  var g = new Error('get');\n"
    "  Object.defineProperty(g, 'bad', { enumerable: true, get: function () { throw new Error('no'); } });\n"
    "  console.error(g);\n"
    "}\n"
    "class ImplicitErr extends Error {}\n"
    "function throwImplicit() { throw new ImplicitErr('implicit'); }\n"
    "function throwEval() { eval('var q = 1;\\nundefined.e'); }\n"
    "function eachLog() { ['each'].forEach(console.error); }\n"
    "function each(a, f) { for (var i = 0; i < a.length; i++) f(a[i]); }\n";
static const char* kP1 =
    "<!doctype html><html><head><title>p1</title></head><body>p1<script src=\"lib.js\"></script>"
    "<script>window.ready=1;</script></body></html>";
static const char* kP2 = "<!doctype html><html><head><title>p2</title></head><body>p2<script>window.ready=2;</script></body></html>";
static const char* kBase = "<!doctype html><html><head><base href=\"sub/\"><title>base</title></head><body>base</body></html>";
static const char* kP3 =
    "<!doctype html><html><head><meta http-equiv=\"Content-Security-Policy\" content=\"default-src file:; "
    "script-src file:; object-src 'none'\"><title>p3</title></head><body>p3<script src=\"lib.js\"></script></body></html>";

static RefPtr<Renderer> g_r;
static RefPtr<View> g_v;
static int g_fail = 0;

static void Pump(int n = 4)
{
    for (int i = 0; i < n; ++i) {
        g_r->Update();
        g_r->RefreshDisplay(0);
        g_r->Render();
        FiberRun::BetweenFrames();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
static std::string Esc(const std::string& s)
{
    std::string o;
    for (char c : s) o += c == '\n' ? std::string("\\n") : std::string(1, c);
    return o;
}
static void Check(bool ok, const std::string& what, const std::string& detail = {})
{
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -> ", Esc(detail).c_str());
    if (!ok) ++g_fail;
}
static bool Starts(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

// ── A: the dispatcher stand-in ──
struct Call {
    unsigned id;
    bool installed;   // `function` is the __mlNative the page holds now
    std::string args;
};
static std::vector<Call> g_calls;
static JSObjectRef g_installed = nullptr;
static unsigned g_installId = 42;

static JSValueRef Dispatch(JSContextRef ctx, void* userData, JSObjectRef function, JSObjectRef, size_t argc,
                           const JSValueRef argv[], JSValueRef*)
{
    Call c{ static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(userData)), function == g_installed, {} };
    for (size_t i = 0; i < argc; ++i) c.args += (i ? "," : "") + JB::ValueText(ctx, argv[i]);
    g_calls.push_back(c);
    return JSValueMakeUndefined(ctx);
}
// InstallBridgeShims' __mlNative, as the host makes it.
static void Install(View* view, unsigned id)
{
    auto scope = view->LockJSContext();
    if (!scope) return;
    JSContextRef ctx = (*scope);
    JSStringRef name = JSStringCreateWithUTF8CString("__mlNative");
    g_installed = JSObjectMakeFunctionWithData(ctx, name, &Dispatch, reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)), nullptr);
    if (g_installed) JSObjectSetProperty(ctx, JSContextGetGlobalObject(ctx), name, g_installed, kJSPropertyAttributeNone, nullptr);
    JSStringRelease(name);
}

struct LoadL : LoadListener {
    bool dom = false;
    void OnWindowObjectReady(View* caller, uint64_t, bool main, const String&) override
    {
        if (main) Install(caller, g_installId);
    }
    void OnDOMReady(View*, uint64_t, bool main, const String&) override
    {
        if (main) dom = true;
    }
};
static LoadL g_load;

struct ConMsg {
    int level;
    std::string message, source, text;   // text = the host's formatting of every argument
    unsigned line, column, nargs;
};
static std::vector<ConMsg> g_console;
struct ConL : ViewListener {
    void OnAddConsoleMessage(View*, const ConsoleMessage& m) override
    {
        ConMsg c{ int(m.level()), m.message().utf8().data(), m.source_id().utf8().data(), {}, m.line_number(),
                  m.column_number(), m.num_arguments() };
        if (JSContextRef ctx = m.num_arguments() ? m.argument_context() : nullptr) {
            for (uint32_t i = 0; i < m.num_arguments(); ++i) {
                if (i) c.text += ' ';
                c.text += JB::ConsoleArgText(ctx, m.argument_at(i));
            }
        }
        g_console.push_back(c);
    }
};
static ConL g_conL;

static void Load(const char* url)
{
    g_load.dom = false;
    g_v->LoadURL(url);
    for (int i = 0; i < 400 && !g_load.dom; ++i) Pump(1);
    Pump(6);
}
// JsBridge::Evaluate on the view's context, locked for this call only (DrainBridgeQueues' shape).
static bool Eval(const std::string& script, const char* url, std::string& result, JB::ErrorInfo& err)
{
    auto scope = g_v->LockJSContext();
    if (!scope) {
        result.clear();
        err = {};
        return true;
    }
    JSContextRef ctx = (*scope);
    return JB::Evaluate(ctx, script, url, result, err);
}
static std::string EvalResult(const std::string& script)
{
    std::string r;
    JB::ErrorInfo e;
    return Eval(script, "bridgetest:read", r, e) ? r : "THREW " + e.text;
}
static JB::ErrorInfo Thrown(const std::string& script, const char* url = "magelight:dispatch")
{
    std::string r;
    JB::ErrorInfo e;
    if (Eval(script, url, r, e)) e.text = "<did not throw>";
    return e;
}
// window[fn]() called the InteropCall way (JSObjectCallAsFunction), its exception described.
static JB::ErrorInfo CallThrown(const char* fn)
{
    JB::ErrorInfo e;
    e.text = "<did not throw>";
    auto scope = g_v->LockJSContext();
    if (!scope) return e;
    JSContextRef ctx = (*scope);
    JSStringRef n = JSStringCreateWithUTF8CString(fn);
    JSValueRef f = JSObjectGetProperty(ctx, JSContextGetGlobalObject(ctx), n, nullptr);
    JSStringRelease(n);
    JSValueRef exc = nullptr;
    if (JSObjectRef fo = f && JSValueIsObject(ctx, f) ? JSValueToObject(ctx, f, nullptr) : nullptr)
        JSObjectCallAsFunction(ctx, fo, nullptr, 0, nullptr, &exc);
    if (exc) e = JB::Describe(ctx, exc);
    return e;
}
// import('./mod.js') into window[now] at once, and a function window[fn] that imports it into window[later].
static std::string ImportScript(const std::string& now, const std::string& fn, const std::string& later)
{
    auto one = [](const std::string& v) {
        return "window." + v + "='pending';import('./mod.js').then(function(m){window." + v + "=m.v},function(e){window." + v +
               "='ERR '+e});";
    };
    return one(now) + "window." + fn + "=function(){" + one(later) + "};";
}
static std::string Show(const JB::ErrorInfo& e)
{
    return "text=" + e.text + " url=" + e.url + " line=" + std::to_string(e.line) + " col=" + std::to_string(e.column) +
           " stack=" + e.stack.substr(0, 160);
}

static int ToolMain(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: bridgetest <out dir holding resources\\>\n");
        return 2;
    }
    const std::filesystem::path base = argv[1];
    std::filesystem::create_directories(base / "pages" / "sub");
    const std::pair<const char*, const char*> pages[] = { { "lib.js", kLibJs },
                                                          { "p1.html", kP1 },
                                                          { "p2.html", kP2 },
                                                          { "p3.html", kP3 },
                                                          { "base.html", kBase },
                                                          { "mod.js", "export const v = 'pages/mod.js';\n" },
                                                          { "sub/mod.js", "export const v = 'pages/sub/mod.js';\n" } };
    for (const auto& [name, text] : pages) std::ofstream(base / "pages" / name, std::ios::binary) << text;

    Config cfg;
    Platform::instance().set_config(cfg);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    Platform::instance().set_file_system(GetPlatformFileSystem(String(base.string().c_str())));
    g_r = Renderer::Create();
    ViewConfig vc;
    vc.is_accelerated = false;
    g_v = g_r->CreateView(400, 300, vc, nullptr);
    g_v->set_load_listener(&g_load);
    g_v->set_view_listener(&g_conL);
    Load("file:///pages/p1.html");
    Check(EvalResult("String(window.ready)") == "1", "p1 loaded");

    // ── A ──
    std::printf("\n== A: __mlNative ==\n");
    Check(EvalResult("typeof __mlNative") == "function", "typeof __mlNative is 'function'");
    Check(EvalResult("String(Object.getPrototypeOf(__mlNative)===Function.prototype)") == "true",
          "its prototype is Function.prototype (call, apply and bind with no prototype patch)");
    Check(EvalResult("__mlNative.name") == "__mlNative", "its name is '__mlNative'", EvalResult("__mlNative.name"));
    Check(EvalResult("Object.prototype.toString.call(__mlNative)") == "[object Function]", "it is a Function");
    const char* forms[][2] = {
        { "__mlNative('42','ch','direct')", "direct" },
        { "__mlNative.call(null,'42','ch','call')", "call" },
        { "__mlNative.apply(null,['42','ch','apply'])", "apply" },
        { "__mlNative.bind(null,'42')('ch','bind')", "bind" },
        { "({m:__mlNative}).m('42','ch','method')", "method" },
        { "Reflect.apply(__mlNative,null,['42','ch','reflect'])", "reflect" },
    };
    for (auto& f : forms) {
        g_calls.clear();
        EvalResult(f[0]);
        const bool ok = g_calls.size() == 1 && g_calls[0].id == 42 && g_calls[0].installed &&
                        g_calls[0].args == std::string("42,ch,") + f[1];
        Check(ok, std::string("user data 42 on a ") + f[1] + " call", g_calls.empty() ? "no call" : g_calls[0].args);
    }
    g_calls.clear();
    EvalResult("queueMicrotask(function(){__mlNative('42','ch','microtask')});setTimeout(function(){__mlNative('42','ch','timer')},0)");
    Pump(6);
    Check(g_calls.size() == 2 && g_calls[0].id == 42 && g_calls[1].id == 42 && g_calls[0].args == "42,ch,microtask" &&
              g_calls[1].args == "42,ch,timer",
          "user data 42 on microtask and timer calls (inside Renderer::Update)");
    Check(Starts(EvalResult("(function(){try{new __mlNative('42');return 'constructed'}catch(e){return 'THROW '+e.name}})()"), "THROW TypeError"),
          "new __mlNative throws a TypeError");
    g_installId = 43;
    Install(g_v.get(), 43);   // the !full re-install: a fresh function on the same window
    g_calls.clear();
    EvalResult("__mlNative('43','ch','reinstall')");
    Check(g_calls.size() == 1 && g_calls[0].id == 43 && g_calls[0].installed, "a re-install replaces it (user data 43)");
    g_installId = 42;
    Load("file:///pages/p2.html");
    g_calls.clear();
    EvalResult("__mlNative('42','ch','p2')");
    Check(g_calls.size() == 1 && g_calls[0].id == 42, "installed again after a navigation");
    Load("file:///pages/p3.html");
    Check(Starts(EvalResult("(function(){try{return eval('1')}catch(e){return 'refused'}})()"), "refused"),
          "p3's policy refuses eval");
    g_calls.clear();
    EvalResult("__mlNative('42','ch','strict')");
    Check(g_calls.size() == 1 && g_calls[0].id == 42, "Evaluate and __mlNative work under that policy");
    Load("file:///pages/p1.html");

    // ── B ──
    std::printf("\n== B: Describe ==\n");
    {
        auto e = Thrown("throw new TypeError('t1')");
        Check(e.text == "TypeError: t1" && e.url == "magelight:dispatch" && e.line == 1 && e.column == 20 &&
                  e.stack == "global code@magelight:dispatch:1:20",
              "TypeError in a host script: text, the sourceURL, 1:20, one frame", Show(e));
        e = Thrown("\n\n  throw new TypeError('t1b')", nullptr);
        Check(e.text == "TypeError: t1b" && e.url.empty() && e.line == 3 && e.column == 22, "no sourceURL: no url, 3:22", Show(e));
        e = Thrown("thrower('b2')");
        Check(e.text == "TypeError: bad b2" && e.url == "file:///pages/lib.js" && e.line == 3 && e.column == 22 &&
                  Starts(e.stack, "inner@file:///pages/lib.js:3:22\nthrower@file:///pages/lib.js:1:35\nglobal code@magelight:dispatch:1:8"),
              "thrown in a page file: the file, 3:22, every frame", Show(e));
        e = Thrown("throw 'plain string'");
        Check(e.text == "plain string" && e.url.empty() && e.line == 0 && e.column == 0 && e.stack.empty(),
              "a thrown string: its text, no location, no stack", Show(e));
        e = Thrown("throw {code:5}");
        Check(e.text == "[object Object]" && e.url.empty() && e.stack.empty(), "a thrown plain object", Show(e));
        e = Thrown("var = ;");
        Check(Starts(e.text, "SyntaxError: ") && e.url == "magelight:dispatch" && e.line == 1 && e.stack.empty(),
              "a SyntaxError: the sourceURL, line 1, no stack", Show(e));
        e = Thrown("throw {toString:function(){throw new Error('inner-toString')}}");
        Check(e.text.empty() && e.url.empty() && e.stack.empty(), "a value whose toString throws: empty text, no crash", Show(e));
        e = Thrown("class MyErr extends Error{};throw new MyErr('m')");
        Check(e.text == "Error: m" && e.stack.find("magelight:dispatch:1:44") != std::string::npos, "an Error subclass: its stack",
              Show(e));
        e = Thrown("nosuchname.x");
        Check(e.text == "ReferenceError: Can't find variable: nosuchname" && e.line == 1 && e.column == 11, "a ReferenceError",
              Show(e));
        e = Thrown("domErr()");
        Check(e.text == "SyntaxError: The string did not match the expected pattern." && e.url.empty() && e.stack.empty(),
              "a DOMException: its text only", Show(e));
        e = Thrown("(function f(){f()})()");
        Check(Starts(e.text, "RangeError: Maximum call stack size exceeded") && e.line == 1 && !e.stack.empty(),
              "a stack overflow (RangeError)", Show(e));
        // The InteropCall path: a window function called through JSObjectCallAsFunction.
        auto scope = g_v->LockJSContext();
        JSContextRef ctx = (*scope);
        JSStringRef n = JSStringCreateWithUTF8CString("thrower");
        JSValueRef fn = JSObjectGetProperty(ctx, JSContextGetGlobalObject(ctx), n, nullptr);
        JSStringRelease(n);
        JSStringRef a = JSStringCreateWithUTF8CString("b6");
        JSValueRef arg = JSValueMakeString(ctx, a);
        JSStringRelease(a);
        JSValueRef exc = nullptr;
        JSObjectCallAsFunction(ctx, JSValueToObject(ctx, fn, nullptr), nullptr, 1, &arg, &exc);
        e = JB::Describe(ctx, exc);
        Check(exc && e.text == "TypeError: bad b6" && e.url == "file:///pages/lib.js" && e.line == 3 && e.column == 22 &&
                  e.stack == "inner@file:///pages/lib.js:3:22\nthrower@file:///pages/lib.js:1:35",
              "JSObjectCallAsFunction's exception (the InteropCall path)", Show(e));
        e = JB::Describe(ctx, nullptr);
        Check(e.text.empty() && e.url.empty() && e.line == 0, "Describe(nullptr): empty, no crash");
    }
    {
        // A function the page core defines throws later: its frames name magelight:bridge.
        std::string r;
        JB::ErrorInfo e;
        Eval("window.__coreFn=function(){throw new Error('core')};", "magelight:bridge", r, e);
        e = Thrown("__coreFn()", nullptr);
        Check(Starts(e.stack, "@magelight:bridge:1:") || e.stack.find("@magelight:bridge:1:") != std::string::npos,
              "a function defined by a named host script carries that name in later stacks", Show(e));
    }

    // ── C ──
    std::printf("\n== C: IsErrorObject ==\n");
    {
        auto scope = g_v->LockJSContext();
        JSContextRef ctx = (*scope);
        auto value = [&](const char* s) {
            JSStringRef js = JSStringCreateWithUTF8CString(s);
            JSValueRef v = JSEvaluateScript(ctx, js, nullptr, nullptr, 1, nullptr);
            JSStringRelease(js);
            return v;
        };
        Check(JB::IsErrorObject(ctx, value("new TypeError('x')")), "a TypeError is an Error");
        Check(JB::IsErrorObject(ctx, value("(function(){class E extends Error{};return new E('m')})()")), "an Error subclass is an Error");
        Check(JB::IsErrorObject(ctx, value("(function(){try{document.querySelector('<<')}catch(e){return e}})()")),
              "a DOMException is an Error");
        Check(!JB::IsErrorObject(ctx, value("'string'")), "a string is not");
        Check(!JB::IsErrorObject(ctx, value("({message:'m',stack:'s'})")), "a plain object is not");
        Check(!JB::IsErrorObject(ctx, value("5")), "a number is not");
        Check(!JB::IsErrorObject(ctx, value("null")), "null is not");
        Check(!JB::IsErrorObject(ctx, nullptr), "nullptr is not");
    }

    // ── D ──
    std::printf("\n== D: Evaluate against View::EvaluateScript ==\n");
    const char* scripts[] = { "undefined", "void 0", "var q=3", "null", "1.5", "'s'", "({a:1})", "[1,2]", "Symbol('x')",
                              "(function f(){})", "Promise.resolve(1)", "throw new TypeError('x')", "throw 'y'",
                              "throw {code:5}", "var = ;", "throw {toString:function(){throw 1}}",
                              "'caf\xC3\xA9 \xF0\x9F\x98\x80'", "'caf\xE9'", "1;\n2;\n'multi'" };
    for (const char* s : scripts) {
        String vex;
        const std::string viewRes = g_v->EvaluateScript(s, &vex).utf8().data();
        const std::string viewEx = vex.utf8().data();
        std::string res;
        JB::ErrorInfo err;
        const bool ok = Eval(s, nullptr, res, err);   // as DrainBridgeQueues runs EvalJS
        const std::string ex = ok ? "" : err.text;
        Check(viewRes == res && viewEx == ex, std::string("same strings for ") + s,
              "view=\"" + viewRes + "\" exc=\"" + viewEx + "\" | bridge=\"" + res + "\" exc=\"" + ex + "\"");
    }
    {
        // One global scope: a var or a top-level let one side declares, the other reads.
        EvalResult("var __bv=5;let __bl=7");
        g_v->EvaluateScript("var __vv=6;let __vl=8", nullptr);
        Check(String(g_v->EvaluateScript("String(window.__bv)+__bl")).utf8().data() == std::string("57") &&
                  EvalResult("String(window.__vv)+__vl") == "68" && EvalResult("String(this===window)") == "true",
              "the same global scope as View::EvaluateScript (var, top-level let, this)");
    }
    {
        EvalResult("window.__mt=0;Promise.resolve().then(function(){window.__mt=1});0");
        const std::string inCall = EvalResult("String(window.__mt)");
        Pump(1);
        const std::string afterUpdate = EvalResult("String(window.__mt)");
        Check(inCall == "0" && afterUpdate == "1", "a microtask runs at the next Renderer::Update, not inside Evaluate",
              inCall + " then " + afterUpdate);
    }
    for (const char* page : { "file:///pages/p1.html", "file:///pages/base.html" }) {
        // A relative import() in the script, and in a function it defines, called later from a timer. base.html has
        // <base href="sub/">, which View::EvaluateScript's resolution follows.
        Load(page);
        g_v->EvaluateScript(ImportScript("__iv", "__fv", "__lv").c_str(), nullptr);
        std::string r;
        JB::ErrorInfo e;
        Eval(ImportScript("__ib", "__fb", "__lb"), nullptr, r, e);
        Eval(ImportScript("__io", "__fo", "__lo"), "magelight:dispatch", r, e);
        EvalResult("setTimeout(function(){__fv();__fb();__fo()},0)");
        const char* all = "[__iv,__lv,__ib,__lb,__io,__lo].indexOf('pending')<0";
        for (int i = 0; i < 400 && EvalResult(all) != "true"; ++i) Pump(1);
        const std::string want = std::string(page).ends_with("base.html") ? "pages/sub/mod.js" : "pages/mod.js";
        const std::string view = EvalResult("__iv+'|'+__lv"), unnamed = EvalResult("__ib+'|'+__lb"),
                          opaque = EvalResult("__io+'|'+__lo");
        Check(view == want + "|" + want && unnamed == view,
              std::string("unnamed, as EvalJS and InvokeJS run: import() resolves as View::EvaluateScript's on ") + page,
              "view=" + view + " | unnamed=" + unnamed);
        Check(Starts(opaque, "ERR TypeError") && opaque.find("|ERR TypeError") != std::string::npos,
              std::string("an opaque name refuses it (why a mod's script is not named) on ") + page, opaque);
    }
    Load("file:///pages/p1.html");

    // ── E ──
    std::printf("\n== E: console ==\n");
    g_console.clear();
    EvalResult("logErr()");
    Pump(2);
    Check(g_console.size() == 3, "logErr: three console messages", std::to_string(g_console.size()));
    if (g_console.size() == 3) {
        Check(Starts(g_console[0].text, "TypeError: bad logged\ninner@file:///pages/lib.js:3:22\nlogErr@file:///pages/lib.js:5:32"),
              "console.error(err): the Error's text and stack, not {}", g_console[0].text);
        Check(Starts(g_console[1].text, "ctx TypeError: bad logged\ninner@file:///pages/lib.js:3:22"),
              "console.error('ctx', err)", g_console[1].text);
        Check(g_console[2].text == "{\"plain\":1}", "console.warn({plain:1}) stays JSON", g_console[2].text);
        Check(g_console[0].column > 0 && g_console[0].line == 5, "the message carries its line and column",
              std::to_string(g_console[0].line) + ":" + std::to_string(g_console[0].column));
    }
    g_console.clear();
    EvalResult("logFields()");
    Pump(2);
    Check(g_console.size() == 4, "logFields: four console messages", std::to_string(g_console.size()));
    if (g_console.size() == 4) {
        Check(Starts(g_console[0].text, "Error: x {\"code\":\"E_FOO\",\"status\":404}\nlogFields@file:///pages/lib.js:10:"),
              "an Error with fields of its own: its text, their JSON, its stack", g_console[0].text);
        Check(Starts(g_console[1].text, "request failed Error: http 404 {\"status\":404,\"body\":{\"reason\":\"no such page\"}}\n"
                                        "HttpError@file:///pages/lib.js:11:") &&
                  g_console[1].text.find("\nlogFields@file:///pages/lib.js:12:") != std::string::npos,
              "an Error subclass that sets fields in its constructor", g_console[1].text);
        Check(Starts(g_console[2].text, "Error: cyc\nlogFields@file:///pages/lib.js:13:"),
              "fields whose JSON throws (a cycle): the text and stack alone", g_console[2].text);
        Check(Starts(g_console[3].text, "Error: get\nlogFields@file:///pages/lib.js:14:"),
              "a field whose getter throws: the text and stack alone", g_console[3].text);
    }
    g_console.clear();
    EvalResult("eachLog()");
    Pump(2);
    Check(g_console.size() == 1 && g_console[0].source.empty() && g_console[0].line > 0,
          "a page file's forEach(console.error): no source, the line of JSC's built-in forEach",
          g_console.empty() ? "none" : "'" + g_console[0].source + "':" + std::to_string(g_console[0].line));
    g_console.clear();
    EvalResult("logOther()");
    Pump(2);
    Check(g_console.size() == 1 && Starts(g_console[0].text, "function f() {} [object Object] undefined 5 null undefined"),
          "a function, a cycle, a Symbol, a number, null, undefined", g_console.empty() ? "none" : g_console[0].text);
    g_console.clear();
    Thrown("throw new TypeError('host')", nullptr);
    {
        auto scope = g_v->LockJSContext();
        JSContextRef ctx = (*scope);
        JSStringRef n = JSStringCreateWithUTF8CString("thrower");
        JSValueRef fn = JSObjectGetProperty(ctx, JSContextGetGlobalObject(ctx), n, nullptr);
        JSStringRelease(n);
        JSValueRef exc = nullptr;
        JSObjectCallAsFunction(ctx, JSValueToObject(ctx, fn, nullptr), nullptr, 0, nullptr, &exc);
    }
    g_v->EvaluateScript("throw new TypeError('view')", nullptr);
    Pump(4);
    Check(g_console.empty(), "a thrown host script, InteropCall or View::EvaluateScript reaches no console listener",
          std::to_string(g_console.size()));
    g_console.clear();
    EvalResult("uncaughtLater()");
    Pump(6);
    if (!g_console.empty())
        std::printf("info  an uncaught timer exception: \"%s\" at %s:%u:%u, %u argument(s)\n", Esc(g_console[0].message).c_str(),
                    g_console[0].source.c_str(), g_console[0].line, g_console[0].column, g_console[0].nargs);

    // ── F ──
    std::printf("\n== F: LogText / FullText ==\n");
    {
        JB::ErrorInfo e{ "TypeError: x", "magelight:dispatch", "global code@magelight:dispatch:1:20", 1, 20 };
        Check(JB::LogText(e) == "TypeError: x (magelight:dispatch:1:20)\n    global code@magelight:dispatch:1:20", "location and frame",
              JB::LogText(e));
        Check(JB::FullText(e) == "TypeError: x\nglobal code@magelight:dispatch:1:20", "FullText", JB::FullText(e));
        JB::ErrorInfo s{ "plain", "", "", 0, 0 };
        Check(JB::LogText(s) == "plain" && JB::FullText(s) == "plain", "no location, no stack: the text alone");
        auto m = Thrown("var a=1;\nvar b=2;\nfunction f(){ return undefined.x; }\nf();", nullptr);
        Check(m.url.empty() && m.line == 3 && m.column == 31 &&
                  JB::LogText(m) == m.text + " (line 3:31 of an unnamed script)\n    f@\n    global code@",
              "a mod's unnamed script: its line and column, frames with no location", JB::LogText(m));
        Check(JB::ScriptLine(m) == 3, "ScriptLine of a mod's unnamed script: its line", std::to_string(JB::ScriptLine(m)));
        const bool noLoc = !JB::StackHasLocation("") && !JB::StackHasLocation("f@\nglobal code@") &&
                           !JB::StackHasLocation("parse@[native code]\nglobal code@") && !JB::StackHasLocation("a:1:2@") &&
                           !JB::StackHasLocation("w@[wasm code]") && !JB::StackHasLocation("x@file:///y.js:3");
        const bool loc = JB::StackHasLocation("global code@magelight:dispatch:1:20") &&
                         JB::StackHasLocation("f@\n@file:///a@b/x.js:3:22\nglobal code@");
        Check(noLoc && loc, "StackHasLocation: a frame ending in :line:column, none ending in @ or @[native code]");
        // An Error subclass with no constructor of its own is made in JSC's built-in constructor: no url, the
        // built-in's 1:28. With a named script on the stack the frames place it, so no unnamed line.
        auto ic = CallThrown("throwImplicit");
        Check(ic.text == "Error: implicit" && ic.url.empty() && ic.line == 1 && ic.column == 28 && JB::ScriptLine(ic) == 0 &&
                  Starts(JB::LogText(ic), "Error: implicit\n    ImplicitErr@\n    throwImplicit@file:///pages/lib.js:19:"),
              "a page file's Error subclass with no constructor (InteropCall): no unnamed line, the page frame",
              Show(ic) + " | " + JB::LogText(ic));
        auto icm = Thrown("throwImplicit()", nullptr);
        Check(icm.url.empty() && JB::ScriptLine(icm) == 0 && JB::LogText(icm).find("unnamed") == std::string::npos &&
                  icm.stack.find("throwImplicit@file:///pages/lib.js:19:") != std::string::npos,
              "the same, thrown into a mod's unnamed script (InvokeJS)", JB::LogText(icm));
        auto ev = CallThrown("throwEval");
        Check(ev.url.empty() && ev.line == 2 && JB::ScriptLine(ev) == 0 && JB::LogText(ev).find("unnamed") == std::string::npos &&
                  ev.stack.find("throwEval@file:///pages/lib.js:20:") != std::string::npos,
              "a page file's eval code: no unnamed line, the page frame", JB::LogText(ev));
        auto host = Thrown("class HostErr extends Error{};throw new HostErr('h')");
        Check(host.url.empty() && JB::ScriptLine(host) == 0 && JB::LogText(host).find("unnamed") == std::string::npos &&
                  host.stack.find("@magelight:dispatch:1:") != std::string::npos,
              "a named host script's Error subclass with no constructor: no unnamed line", JB::LogText(host));
        auto own = Thrown("var a=1;\nclass E extends Error {}\nthrow new E('own');", nullptr);
        Check(own.url.empty() && own.line == 1 && own.column == 28 && JB::ScriptLine(own) == 1 &&
                  JB::LogText(own) == "Error: own (line 1:28 of an unnamed script)\n    E@\n    global code@",
              "a mod's own Error subclass with no constructor: the built-in's 1:28, not its line 3 (documented)",
              JB::LogText(own));
        // The rule goes by who is on the stack, not by who owns the code (docs/CPP.md, TROUBLESHOOTING).
        auto pageClass = Thrown("var a=1;\nvar b=2;\nthrow new ImplicitErr('page class from mod');", nullptr);
        Check(pageClass.url.empty() && pageClass.line == 1 && pageClass.column == 28 && JB::ScriptLine(pageClass) == 1 &&
                  JB::LogText(pageClass) == "Error: page class from mod (line 1:28 of an unnamed script)\n    ImplicitErr@\n"
                                            "    global code@",
              "a page file's Error subclass with no constructor, made by a mod's script: the built-in's 1:28 too (documented)",
              JB::LogText(pageClass));
        auto cb = Thrown("var a = 1;\neach([1], function (x) {\n  x.y.z;\n});", nullptr);
        Check(cb.url.empty() && cb.line == 3 && JB::ScriptLine(cb) == 0 && JB::LogText(cb).find("unnamed") == std::string::npos &&
                  Starts(cb.stack, "@\neach@file:///pages/lib.js:22:"),
              "a mod's callback a page function calls: no line (documented), its frame a bare @ above the page's",
              Show(cb) + " | " + JB::LogText(cb));
        JB::ErrorInfo z{};
        Check(JB::LogText(z) == JB::kNoStringForm && JB::FullText(z) == JB::kNoStringForm, "no text: the stand-in");
        auto deep = Thrown("(function f(){f()})()");
        std::size_t frames = 1;
        for (char c : deep.stack) frames += c == '\n';
        const std::string lt = JB::LogText(deep);
        std::size_t lines = 1;
        for (char c : lt) lines += c == '\n';
        Check(frames > 12 && lines == 14 && lt.find("... " + std::to_string(frames - 12) + " more") != std::string::npos,
              "a deep stack: 12 frames and a count of the rest", std::to_string(frames) + " frames, " + std::to_string(lines) + " lines");
    }

    g_v = nullptr;
    g_r = nullptr;
    std::printf("\n%s: %d check(s) failed\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}

// MG_PAGE_FIBER=1: on the page fiber, as Magelight runs pages (../fiber-gc-test/FiberRun.h).
int main(int argc, char** argv) { return FiberRun::Run(argc, argv, &ToolMain); }
