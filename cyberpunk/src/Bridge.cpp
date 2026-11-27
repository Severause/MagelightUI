#include "Bridge.h"

#include "Log.h"

#include "../../src/InputCore.h"
#include "../../src/JsBridgeCore.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>

namespace mlcp::bridge {
namespace {
struct Name {
    std::string mod, view;
};

// Messages wait here through a load (Pump.cpp), so the cap is what a long load can hold.
constexpr std::size_t kOutboxCap = 4096;

std::mutex g_mutex;
auto& g_names = *new std::map<int, Name>();
auto& g_outbox = *new std::vector<views::Message>();
std::atomic<std::size_t> g_waiting{0};
std::size_t g_dropped = 0;

void Push(views::Message m) {   // g_mutex held
    if (g_outbox.size() >= kOutboxCap) {
        ++g_dropped;
        return;
    }
    g_outbox.push_back(std::move(m));
    g_waiting.store(g_outbox.size(), std::memory_order_relaxed);
}

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

std::string Arg(JSContextRef ctx, size_t argc, const JSValueRef argv[], size_t i) {
    return i < argc ? Magelight::JsBridge::ValueText(ctx, argv[i]) : std::string();
}

// userData is the id this page's __mlNative was made with; the id a page passes is only checked against it, so a
// page can act only as itself (Magelight's MlNativeDispatch).
JSValueRef Native(JSContextRef ctx, void* userData, JSObjectRef, JSObjectRef, size_t argc, const JSValueRef argv[],
                  JSValueRef*) {
    const int id = static_cast<int>(reinterpret_cast<std::intptr_t>(userData));
    const std::string claimed = Arg(ctx, argc, argv, 0), channel = Arg(ctx, argc, argv, 1);
    if (std::to_string(id) != claimed) {
        Log("bridge: view %d called as view '%s', refused", id, claimed.c_str());
        return JSValueMakeUndefined(ctx);
    }
    if (channel.rfind("__", 0) == 0) return JSValueMakeUndefined(ctx);   // the host's reserved channels: none yet
    // A control character would split the CET door's record ('\x1f' separates its fields).
    if (channel.empty() || std::any_of(channel.begin(), channel.end(), [](unsigned char c) { return c < 0x20; })) {
        Log("bridge: view %d sent on an empty channel or one with a control character, refused", id);
        return JSValueMakeUndefined(ctx);
    }
    std::lock_guard lock(g_mutex);
    const auto it = g_names.find(id);
    if (it != g_names.end()) Push({it->second.mod, it->second.view, channel, Arg(ctx, argc, argv, 2)});
    return JSValueMakeUndefined(ctx);
}
}

void InstallCore(ultralight::View* view, int id, const std::string& mod, const std::string& name) {
    {
        std::lock_guard lock(g_mutex);
        g_names[id] = {mod, name};
    }
    auto scope = view->LockJSContext();
    if (!scope) return;
    JSContextRef ctx = *scope;
    JSStringRef fnName = JSStringCreateWithUTF8CString("__mlNative");
    const JSObjectRef fn = JSObjectMakeFunctionWithData(ctx, fnName, &Native, reinterpret_cast<void*>(std::intptr_t(id)),
                                                        nullptr);
    if (fn) JSObjectSetProperty(ctx, JSContextGetGlobalObject(ctx), fnName, fn, kJSPropertyAttributeNone, nullptr);
    JSStringRelease(fnName);

    const std::string ids = std::to_string(id);
    const std::string core =
        "if(!window.__mlCore){Object.defineProperty(window,'__mlCore',{value:true});"
        "window.__MAGELIGHT__={version:'" MLCP_VERSION "-cyberpunk',versionNumber:" +
        std::to_string(MLCP_VERSION_MAJOR * 10000 + MLCP_VERSION_MINOR * 100 + MLCP_VERSION_PATCH) + ",viewId:" + ids +
        ",modId:" + Quote(mod) + ",viewName:" + Quote(name) + ",capabilities:{},dev:false,runtimeUrl:''};"
        "window.magelight=(function(){var subs={},buf={},id='" + ids + "';"
        "function dispatch(ch,arg){var s=subs[ch];if(!s||!s.length){(buf[ch]=buf[ch]||[]).push(arg);return;}"
        "for(var i=0;i<s.length;i++){try{s[i](arg);}catch(e){console.error('[magelight] handler for '+ch+' threw',e);}}}"
        "return{send:function(ch,p){var a=p==null?'':(typeof p==='string'?p:JSON.stringify(p));__mlNative(id,ch,a);},"
        "sound:function(){},hostTheme:function(){},"
        "on:function(ch,fn){(subs[ch]=subs[ch]||[]).push(fn);var b=buf[ch];if(b){delete buf[ch];"
        "for(var i=0;i<b.length;i++){try{fn(b[i]);}catch(e){console.error('[magelight] replay for '+ch+' threw',e);}}}"
        "return function(){window.magelight.off(ch,fn);};},"
        "off:function(ch,fn){var s=subs[ch];if(!s)return;var i=s.indexOf(fn);if(i>=0)s.splice(i,1);},"
        "_dispatch:dispatch,pending:function(ch){return (buf[ch]||[]).length;}};})();" +
        // No WebKit context menu in the page or its frames: over selected text in a field it faults (InputCore.h).
        std::string(Magelight::Input::kPointerGuardScript) + "}";
    std::string result;
    Magelight::JsBridge::ErrorInfo err;
    if (!Magelight::JsBridge::Evaluate(ctx, core, "magelight:core", result, err))
        Log("bridge: page core failed in %s/%s: %s", mod.c_str(), name.c_str(), Magelight::JsBridge::LogText(err).c_str());
}

void ArmMenuGuard(ultralight::View* view) {
    auto scope = view->LockJSContext();
    if (!scope) return;
    std::string result;
    Magelight::JsBridge::ErrorInfo err;
    if (!Magelight::JsBridge::Evaluate(*scope, Magelight::Input::kArmMenuGuardScript, "magelight:menuguard", result, err))
        Log("bridge: menu guard failed: %s", Magelight::JsBridge::LogText(err).c_str());
}

void Dispatch(ultralight::View* view, const std::string& channel, const std::string& text) {
    auto scope = view->LockJSContext();
    if (!scope) return;
    std::string result;
    Magelight::JsBridge::ErrorInfo err;
    const std::string script = "(function(c,a){var f=window[c];if(typeof f==='function')f(a);"
                               "else if(window.magelight)window.magelight._dispatch(c,a);})(" +
                               Quote(channel) + "," + Quote(text) + ")";
    if (!Magelight::JsBridge::Evaluate(*scope, script, "magelight:dispatch", result, err))
        Log("bridge: '%s' threw: %s", channel.c_str(), Magelight::JsBridge::LogText(err).c_str());
}

void Post(const std::string& mod, const std::string& view, const std::string& channel, const std::string& payload) {
    std::lock_guard lock(g_mutex);
    Push({mod, view, channel, payload});
}

bool Waiting() { return g_waiting.load(std::memory_order_relaxed) != 0; }

void TakeMessages(std::vector<views::Message>& out) {
    out.clear();
    std::size_t dropped;
    {
        std::lock_guard lock(g_mutex);
        out.swap(g_outbox);
        g_waiting.store(0, std::memory_order_relaxed);
        dropped = std::exchange(g_dropped, 0);
    }
    if (dropped) Log("bridge: %zu message(s) dropped, the outbox held %zu", dropped, kOutboxCap);
}
}
