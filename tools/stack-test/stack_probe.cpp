// stack_probe - the stack stress tests (MgStackTest/views/test/stack-tests.js) in Ultralight, offline, on a thread
// with the game's main-thread stack (1 MB reserve, from SkyrimSE.exe's header) of which <usedKB> is already in use at
// every Ultralight call, the way Magelight runs pages with "ulFiberMB": 0. <guaranteeKB> sets a stack guarantee on
// the thread first (SetThreadStackGuarantee), as CrashLogger 1.23+ does on the game's main thread. "host" sets the
// JavaScript stack zones Magelight sets for that stack (src/PageStackCore.h's ReserveFor of the guarantee read back,
// through JSC::Options before Renderer::Create); "default" keeps JavaScriptCore's. One line per test; a test that
// runs the real stack out ends the probe (a caught 0xC00000FD exits 3; one in a thread the probe cannot catch it in
// ends the process with 0xC00000FD).
//   build.bat, then: stack_probe.exe <usedKB> <guaranteeKB> <sdkDir> <stack-tests.js> [host|default] [ids]
// [ids]: test ids or indexes, comma-separated (default: all, in order).

#include <windows.h>
#include <malloc.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>

#include "PageStackCore.h"

using namespace ultralight;
namespace PS = Magelight::PageStack;

namespace
{
    int         g_usedKb = 0;
    int         g_guaranteeKb = 0;
    bool        g_hostReserve = true;
    std::string g_sdk;
    std::string g_script;
    std::string g_only;
    RefPtr<Renderer> g_renderer;
    RefPtr<View>     g_view;
    bool             g_ready = false;

    class Loader : public LoadListener
    {
    public:
        void OnDOMReady(View*, uint64_t, bool isMainFrame, const String&) override
        {
            if (isMainFrame) g_ready = true;
        }
    } g_loader;

    std::string Js(const std::string& js)
    {
        String ex;
        const String r = g_view->EvaluateScript(String(js.c_str()), &ex);
        std::string out = r.utf8().data();
        if (!ex.empty()) out += std::string("  (exception: ") + ex.utf8().data() + ")";
        return out;
    }

    // One step at the probe's depth: start test `index` (index >= 0), or a frame and a look for its result.
    void Step(int index, char* out, std::size_t outSize)
    {
        std::string r;
        if (index >= 0) {
            Js("window.__mgr='';MgStackTests.start(MgStackTests.tests[" + std::to_string(index) +
               "],function(r){window.__mgr=MgStackTests.line(r);});''");
        } else {
            g_renderer->Update();
            g_renderer->Render();
        }
        r = Js("window.__mgr");
        strncpy_s(out, outSize, r.c_str(), _TRUNCATE);
    }

    // Uses about `kb` of stack in 16 KB frames, then runs the step at that depth.
    __declspec(noinline) void Deep(int kb, int index, char* out, std::size_t outSize)
    {
        if (kb > 0) {
            volatile char* p = static_cast<volatile char*>(_alloca(16 * 1024));
            p[0] = 1;
            p[16 * 1024 - 1] = 1;
            Deep(kb - 16, index, out, outSize);
            p[1] = p[0];   // keeps the frame live past the call (no tail call)
            return;
        }
        Step(index, out, outSize);
    }

    int GuardedStep(int index, char* out, std::size_t outSize)
    {
        __try {
            Deep(g_usedKb, index, out, outSize);
            return 0;
        } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER
                                                                     : EXCEPTION_CONTINUE_SEARCH) {
            return 1;
        }
    }

    std::vector<int> Selected(int count)
    {
        std::vector<int> out;
        if (g_only.empty() || g_only == "all") {
            for (int i = 0; i < count; ++i) out.push_back(i);
            return out;
        }
        std::stringstream ss(g_only);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (item.empty()) continue;
            if (item.find_first_not_of("0123456789") == std::string::npos) {
                out.push_back(std::atoi(item.c_str()));
                continue;
            }
            const int i = std::atoi(Js("String(MgStackTests.tests.map(function(t){return t.id;}).indexOf('" + item + "'))").c_str());
            if (i >= 0) out.push_back(i);
            else std::printf("no test '%s'\n", item.c_str());
        }
        return out;
    }

    DWORD WINAPI ProbeThread(void*)
    {
        if (g_guaranteeKb > 0) {
            ULONG guarantee = static_cast<ULONG>(g_guaranteeKb) * 1024;
            SetThreadStackGuarantee(&guarantee);
        }
        ULONG held = 0;   // 0 reads it back, as Magelight does
        SetThreadStackGuarantee(&held);
        std::printf("stack guarantee on the thread: %lu KB\n", held / 1024);
        if (g_hostReserve) {
            const PS::Exports e = PS::Resolve(GetModuleHandleW(L"WebCore.dll"));
            const PS::Reserve r = PS::ReserveFor(held);
            char refused[96];
            if (!e.OptionsReady()) std::printf("WebCore does not export %s: JavaScriptCore's own zones\n", e.missingOptions);
            else if (!PS::ApplyReserve(e, r, refused, sizeof refused)) std::printf("JSC refused %s\n", refused);
            std::printf("JavaScript stack zones (the host's): hard %zu KB, soft %zu KB, at most %zu KB a thread\n",
                        r.hard / 1024, r.soft / 1024, r.maxUsage / 1024);
        } else {
            std::printf("JavaScript stack zones: JavaScriptCore's defaults\n");
        }
        Config cfg;
        Platform::instance().set_config(cfg);
        Platform::instance().set_font_loader(GetPlatformFontLoader());
        Platform::instance().set_file_system(GetPlatformFileSystem(String(g_sdk.c_str())));
        Platform::instance().set_logger(GetDefaultLogger("stack_probe_ul.log"));
        g_renderer = Renderer::Create();
        if (!g_renderer) {
            std::printf("the renderer could not be created\n");
            return 2;
        }
        ViewConfig vc;
        vc.is_accelerated = false;
        g_view = g_renderer->CreateView(320, 240, vc, nullptr);
        g_view->set_load_listener(&g_loader);
        g_view->LoadHTML("<html><body>stack probe</body></html>");
        for (int i = 0; i < 1000 && !g_ready; ++i) {
            g_renderer->Update();
            g_renderer->Render();
            Sleep(5);
        }
        if (!g_ready) {
            std::printf("the page never loaded\n");
            return 2;
        }
        const std::string threw = Js(g_script + "\n;''");
        if (!threw.empty()) {
            std::printf("stack-tests.js: %s\n", threw.c_str());
            return 2;
        }
        const int count = std::atoi(Js("String(MgStackTests.tests.length)").c_str());
        std::printf("used %d KB of a 1 MB stack at every call\n", g_usedKb);
        std::fflush(stdout);
        static char line[2048];
        int failures = 0;
        for (const int i : Selected(count)) {
            line[0] = 0;
            std::printf("  running test %d...\r", i);
            std::fflush(stdout);
            bool overflow = GuardedStep(i, line, sizeof line) != 0;
            // An asynchronous test ends in a later frame.
            for (int f = 0; !overflow && !line[0] && f < 400; ++f) {
                overflow = GuardedStep(-1, line, sizeof line) != 0;
                Sleep(5);
            }
            if (overflow) {
                std::printf("STACK OVERFLOW in test %d - the probe stops\n", i);
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 3);
            }
            if (!line[0]) std::snprintf(line, sizeof line, "FAIL  test %d never finished", i);
            if (std::string(line).rfind("ok", 0) != 0) ++failures;
            std::printf("%s\n", line);
            std::fflush(stdout);
        }
        std::printf("done: %d failure(s)\n", failures);
        std::fflush(stdout);
        // Ultralight's teardown at process exit fails fast with the renderer still alive.
        TerminateProcess(GetCurrentProcess(), failures ? 1 : 0);
        return 0;
    }
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::printf("usage: stack_probe <usedKB> <guaranteeKB> <sdkDir> <stack-tests.js> [host|default] [ids]\n");
        return 1;
    }
    g_usedKb = std::atoi(argv[1]);
    g_guaranteeKb = std::atoi(argv[2]);
    g_sdk = argv[3];
    if (argc > 5) g_hostReserve = std::string(argv[5]) != "default";
    if (argc > 6) g_only = argv[6];
    std::ifstream in(argv[4], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    g_script = ss.str();
    if (g_script.empty()) {
        std::printf("could not read %s\n", argv[4]);
        return 1;
    }
    // The game's main thread: a 1 MB reserve.
    HANDLE thread = CreateThread(nullptr, 1u << 20, ProbeThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) return 1;
    WaitForSingleObject(thread, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(thread, &code);
    return static_cast<int>(code);
}
