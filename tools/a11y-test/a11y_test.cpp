// The accessibility channel (1.10.0): src/A11yCore.h's rules on their own, then the page core's reader
// (src/A11yScript.h) in Ultralight's own WebKit (CPU renderer, no window, no game). The page test loads page.html,
// installs cases.js's stub window.magelight and then the reader, and runs each case in cases.js, which turns the
// reader on and off as the host does ('__a11y' "on" / "off" / "page") and checks the messages it sent. A step
// returns "next" (go on), "wait" (run it again after more frames: a live region's quiet time) or the verdict.
// build.bat builds and runs it; MG_PAGE_FIBER=1 runs it on the page fiber, MG_PAGE_THREAD=1 on a page thread
// (../fiber-gc-test/FiberRun.h).
//
//   a11y_test.exe <ultralight sdk dir> <tool dir> <out dir>

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/Listener.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "A11yCore.h"
#include "A11yScript.h"
#include "../fiber-gc-test/FiberRun.h"

using namespace ultralight;
namespace A11y = Magelight::A11y;

static int g_failed = 0;

static void Check(bool ok, const char* what)
{
    std::printf("%s  core  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

static void CoreChecks()
{
    Check(A11y::TypeOf(R"({"v":1,"type":"focus","name":"OK"})") == "focus", "a focus message names its type");
    Check(A11y::TypeOf(R"({"v":1,"type":"page"})") == "page", "a page message with nothing else passes");
    Check(A11y::TypeOf(R"({"v":1,"type":"bogus"})").empty(), "an unknown type is dropped");
    Check(A11y::TypeOf(R"({"type":"focus","v":1})").empty(), "the prefix must come first");
    Check(A11y::TypeOf(R"({"v":2,"type":"focus"})").empty(), "another version is dropped");
    Check(A11y::TypeOf(R"({"v":1,"type":"focus)").empty(), "an unterminated type is dropped");
    Check(A11y::TypeOf("").empty(), "an empty message is dropped");
    std::string big = R"({"v":1,"type":"announce","text":")" + std::string(A11y::kMaxMessageBytes, 'a') + "\"}";
    Check(A11y::TypeOf(big).empty(), "a message over the cap is dropped");

    A11y::RateGate g;
    int passed = 0;
    for (int i = 0; i < 100; ++i) passed += g.Allow(1000) ? 1 : 0;
    Check(passed == static_cast<int>(A11y::kBurst), "a burst passes up to kBurst at once");
    Check(!g.Allow(1000), "past the burst, a message at the same moment is dropped");
    Check(!g.Allow(1040), "40 ms later less than one has come back");
    Check(g.Allow(1050), "50 ms later one has (20 a second)");
    Check(!g.Allow(1050), "and only one");
    Check(!g.Allow(900), "a clock that goes back gives nothing back");
    int later = 0;
    for (int i = 0; i < 100; ++i) later += g.Allow(1000000) ? 1 : 0;
    Check(later == static_cast<int>(A11y::kBurst), "a long quiet refills to the burst, never past it");

    Check(A11y::Delivers(true, false, false), "a visible page's message is delivered");
    Check(!A11y::Delivers(false, false, false), "a hidden page's is not");
    Check(!A11y::Delivers(true, true, false), "a released (dormant) page's is not");
    Check(!A11y::Delivers(true, false, true), "the host's own page's is not");
}

static std::string ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

struct Loaded : public LoadListener {
    bool ready = false;
    void OnDOMReady(View*, uint64_t, bool isMainFrame, const String&) override
    {
        if (isMainFrame) ready = true;
    }
};

static RefPtr<Renderer> g_r;

static void Pump(int frames)
{
    for (int i = 0; i < frames; ++i) {
        g_r->Update();
        g_r->RefreshDisplay(0);
        g_r->Render();
        FiberRun::BetweenFrames();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

static std::string Eval(View& v, const std::string& js, String& exc)
{
    exc = String();
    return v.EvaluateScript(js.c_str(), &exc).utf8().data();
}

static int PageCases(const std::string& tool)
{
    ViewConfig vc;
    vc.is_accelerated = false;
    RefPtr<View> view = g_r->CreateView(1200, 800, vc, nullptr);
    Loaded loaded;
    view->set_load_listener(&loaded);
    view->LoadHTML(ReadFile(tool + "/page.html").c_str(), "file:///a11y-test/page.html");
    for (int i = 0; i < 600 && !loaded.ready; ++i) Pump(1);
    if (!loaded.ready) {
        std::fprintf(stderr, "page never reached DOM ready\n");
        return 1;
    }
    view->Focus();
    String exc;
    Eval(*view, ReadFile(tool + "/cases.js"), exc);
    // The host calls the expression with the state at install: off here, as on a page loaded before any listener.
    if (exc.empty()) Eval(*view, std::string(A11y::kPageScript) + "(0);", exc);
    if (!exc.empty()) {
        std::fprintf(stderr, "setup threw: %s\n", exc.utf8().data());
        return 1;
    }
    Pump(2);
    const int count = std::atoi(Eval(*view, "__a11yCount()", exc).c_str());
    int failed = 0;
    for (int n = 0; n < count; ++n) {
        const std::string name = Eval(*view, "__a11yName(" + std::to_string(n) + ")", exc);
        std::string verdict;
        int step = 0;
        for (int calls = 0; calls < 400 && step < 32; ++calls) {
            verdict = Eval(*view, "__a11yStep(" + std::to_string(n) + "," + std::to_string(step) + ")", exc);
            Pump(3);
            if (!exc.empty()) {
                verdict = std::string("threw: ") + exc.utf8().data();
                break;
            }
            if (verdict == "wait") continue;
            if (verdict != "next") break;
            ++step;
        }
        const bool ok = verdict.find("\"ok\":true") != std::string::npos;
        if (!ok) ++failed;
        std::printf("%s  page  %s  %s\n", ok ? "ok  " : "FAIL", name.c_str(), verdict.c_str());
    }
    if (count == 0) {
        std::printf("FAIL  page  no cases ran\n");
        ++failed;
    }
    return failed;
}

static int ToolMain(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: a11y_test <sdk dir> <tool dir> <out dir>\n");
        return 2;
    }
    const std::string sdk = argv[1], tool = argv[2], out = argv[3];
    CoreChecks();
    Config config;
    Platform::instance().set_config(config);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    Platform::instance().set_file_system(GetPlatformFileSystem(sdk.c_str()));
    Platform::instance().set_logger(GetDefaultLogger((out + "/ultralight.log").c_str()));
    g_r = Renderer::Create();
    const int failed = g_failed + PageCases(tool);
    std::printf("%s\n", failed ? "FAILED" : "all passed");
    std::fflush(stdout);
    g_r = nullptr;
    return failed ? 1 : 0;
}

int main(int argc, char** argv) { return FiberRun::Run(argc, argv, &ToolMain); }
