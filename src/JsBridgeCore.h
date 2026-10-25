#pragma once
// The JavaScriptCore half of the page bridge: evaluating a host script on a locked context, and turning a thrown
// value or a console argument into text. JSC C API only (Ultralight 2.0: JSValueGetErrorDetails), so
// tools/bridge-test runs it on the desktop against a CPU view.
//
// Every function that takes a JSContextRef takes one the caller holds through View::LockJSContext, and can run page
// script (a toString, a getter, an instanceof), which can call __mlNative: never call one with s_viewsMutex held.
// ToStd, StackHasLocation, ScriptLine, FullText and LogText are plain string helpers.

#include <JavaScriptCore/JavaScript.h>

#include <cstddef>
#include <string>

namespace Magelight::JsBridge {

    inline std::string ToStd(JSStringRef s)
    {
        if (!s) return {};
        const std::size_t cap = JSStringGetMaximumUTF8CStringSize(s);
        std::string out(cap, '\0');
        const std::size_t n = JSStringGetUTF8CString(s, out.data(), cap);
        out.resize(n ? n - 1 : 0);
        return out;
    }

    // String(v); "" when the conversion throws (a Symbol, a toString that throws) or v is null. The same strings
    // View::EvaluateScript reports for a result and for an exception, which EvalJS hands to mods unchanged.
    inline std::string ValueText(JSContextRef ctx, JSValueRef v)
    {
        if (!v) return {};
        JSStringRef s = JSValueToStringCopy(ctx, v, nullptr);
        std::string out = ToStd(s);
        if (s) JSStringRelease(s);
        return out;
    }

    // A thrown value. url, line, column and stack are filled for an Error object only: a thrown string or plain
    // object has none, a SyntaxError no stack, a DOMException only its text.
    struct ErrorInfo {
        std::string text;    // ValueText of the value, e.g. "TypeError: x"
        std::string url;     // the script that created it: a page file, the name Evaluate was given, "" if unnamed
        std::string stack;   // JSC frames, one per line: "fn@file:///views/app/x.js:3:22"
        unsigned line = 0;   // 1-based, 0 = unknown
        unsigned column = 0;
    };

    inline ErrorInfo Describe(JSContextRef ctx, JSValueRef v)
    {
        ErrorInfo e;
        e.text = ValueText(ctx, v);
        if (!v || !JSValueIsObject(ctx, v)) return e;
        // The message is not asked for: text is the value's string, which keeps the exception string EvalJS reports
        // and converts the value once.
        JSStringRef url = nullptr;
        JSStringRef stack = nullptr;
        JSValueGetErrorDetails(ctx, v, nullptr, nullptr, &url, &e.line, &e.column, &stack);
        e.url = ToStd(url);
        e.stack = ToStd(stack);
        if (url) JSStringRelease(url);
        if (stack) JSStringRelease(stack);
        return e;
    }

    // v instanceof the page's own Error (DOMException included). An Error from another realm (an iframe) is false.
    inline bool IsErrorObject(JSContextRef ctx, JSValueRef v)
    {
        if (!v || !JSValueIsObject(ctx, v)) return false;
        JSStringRef name = JSStringCreateWithUTF8CString("Error");
        const JSValueRef ctor = JSObjectGetProperty(ctx, JSContextGetGlobalObject(ctx), name, nullptr);
        JSStringRelease(name);
        if (!ctor || !JSValueIsObject(ctx, ctor)) return false;
        const JSObjectRef ctorObj = JSValueToObject(ctx, ctor, nullptr);
        return ctorObj && JSValueIsInstanceOfConstructor(ctx, v, ctorObj, nullptr);
    }

    // Runs script on ctx as a script named sourceUrl, which locations and stack frames of code it defines then carry.
    // nullptr runs it unnamed, as View::EvaluateScript does: a relative import() in it, or in a function it defines,
    // resolves against the document's base URL. A name that is not a hierarchical URL (magelight:x) makes every such
    // import() fail to resolve, so only a script the host wrote, which never imports, is named. true: result =
    // String(completion value). false: err describes the exception. A script that is not valid UTF-8 runs as an empty
    // script, as it does through View::EvaluateScript. Microtasks it queues run at the next Renderer::Update, as they
    // do after View::EvaluateScript.
    inline bool Evaluate(JSContextRef ctx, const std::string& script, const char* sourceUrl, std::string& result,
                         ErrorInfo& err)
    {
        JSStringRef src = JSStringCreateWithUTF8CString(script.c_str());
        JSStringRef url = sourceUrl ? JSStringCreateWithUTF8CString(sourceUrl) : nullptr;
        JSValueRef exc = nullptr;
        const JSValueRef r = JSEvaluateScript(ctx, src, nullptr, url, 1, &exc);
        JSStringRelease(src);
        if (url) JSStringRelease(url);
        if (exc) {
            result.clear();
            err = Describe(ctx, exc);
            return false;
        }
        result = ValueText(ctx, r);
        return true;
    }

    // Stands in for the text of a value whose string conversion throws.
    inline constexpr const char* kNoStringForm = "(a value with no string form)";

    // The text and the whole stack, one frame per line: the ConsoleMessage event's detail and the banner's text.
    inline std::string FullText(const ErrorInfo& e)
    {
        std::string out = e.text.empty() ? std::string(kNoStringForm) : e.text;
        if (!e.stack.empty()) out += "\n" + e.stack;
        return out;
    }

    // True when a frame of stack names a script and a position in it ("fn@file:///x.js:3:22"). A frame of code that
    // runs unnamed ends in "@", a native one in "@[native code]".
    inline bool StackHasLocation(const std::string& stack)
    {
        std::size_t pos = 0;
        while (pos < stack.size()) {
            std::size_t end = stack.find('\n', pos);
            if (end == std::string::npos) end = stack.size();
            // ":line:column" closes the frame.
            std::size_t p = end, colons = 0;
            while (colons < 2 && p > pos) {
                std::size_t digits = 0;
                while (p > pos && stack[p - 1] >= '0' && stack[p - 1] <= '9') --p, ++digits;
                if (!digits || p == pos || stack[p - 1] != ':') break;
                --p;
                ++colons;
            }
            if (colons == 2) return true;
            pos = end + 1;
        }
        return false;
    }

    // e.line when it places the error: a line of e.url, or, with no url and no frame that names a script, a line of
    // the unnamed code that made it. That is usually the mod's own script, but a page's eval or new Function code the
    // script called directly reads the same, as does an Error subclass with no constructor of its own constructed
    // there (JSC's built-in constructor, 1:28, whoever owns the class). 0 otherwise: with no url and a frame that
    // names a script, unnamed code a named script reached made it (eval or new Function code, that built-in, or a
    // function of the mod's own that page code called, whose frame is a bare "@" above the page's, printed first), and
    // the frames hold the only place there is: the first one with a file is where the named script reached it.
    inline unsigned ScriptLine(const ErrorInfo& e)
    {
        return !e.url.empty() || !StackHasLocation(e.stack) ? e.line : 0;
    }

    // One log line: the text, then " (url:line:column)", or " (line L:C of an unnamed script)" for ScriptLine's
    // unnamed case, then up to maxFrames stack frames, one per line.
    inline std::string LogText(const ErrorInfo& e, std::size_t maxFrames = 12)
    {
        std::string out = e.text.empty() ? std::string(kNoStringForm) : e.text;
        if (!e.url.empty())
            out += " (" + e.url + ":" + std::to_string(e.line) + ":" + std::to_string(e.column) + ")";
        else if (ScriptLine(e))
            out += " (line " + std::to_string(e.line) + ":" + std::to_string(e.column) + " of an unnamed script)";
        std::size_t pos = 0, frames = 0, more = 0;
        while (pos < e.stack.size()) {
            std::size_t end = e.stack.find('\n', pos);
            if (end == std::string::npos) end = e.stack.size();
            if (end > pos) {
                if (frames < maxFrames) {
                    out += "\n    " + e.stack.substr(pos, end - pos);
                    ++frames;
                } else {
                    ++more;
                }
            }
            pos = end + 1;
        }
        if (more) out += "\n    ... " + std::to_string(more) + " more";
        return out;
    }

    // JSON.stringify(v), compact; "" when it throws (a cycle, a getter that throws, a BigInt) or gives undefined.
    inline std::string JsonText(JSContextRef ctx, JSValueRef v)
    {
        JSStringRef js = v ? JSValueCreateJSONString(ctx, v, 0, nullptr) : nullptr;
        std::string out = ToStd(js);
        if (js) JSStringRelease(js);
        return out;
    }

    // One console argument as the log and the ConsoleMessage event show it: an Error as its text, then the JSON of
    // its own enumerable fields when it has any (Error: x {"code":"E_FOO"}; message and stack are not enumerable,
    // so an Error with no fields of its own is "{}" and adds nothing), then its stack; any other object as JSON (its
    // string when JSON has none: a function, a cycle); anything else as its string ("undefined" when it has none, a
    // Symbol).
    inline std::string ConsoleArgText(JSContextRef ctx, JSValueRef v)
    {
        if (!v) return "undefined";
        if (IsErrorObject(ctx, v)) {
            ErrorInfo e = Describe(ctx, v);
            const std::string fields = JsonText(ctx, v);
            if (!fields.empty() && fields != "{}")
                e.text = (e.text.empty() ? std::string(kNoStringForm) : e.text) + " " + fields;
            return FullText(e);
        }
        if (JSValueIsObject(ctx, v)) {
            std::string json = JsonText(ctx, v);
            if (!json.empty()) return json;
        }
        JSStringRef s = JSValueToStringCopy(ctx, v, nullptr);
        std::string out = s ? ToStd(s) : "undefined";
        if (s) JSStringRelease(s);
        return out;
    }

}
