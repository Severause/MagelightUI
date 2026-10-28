// The page core's gamepad helper (src/GamepadScript.h, 1.6.0) in Ultralight's own WebKit (CPU renderer, no window,
// no game): loads page.html, installs cases.js's stub window.magelight and then the helper, and runs each case in
// cases.js, which hands the helper '__gamepad' packets as the host sends them and checks where focus went. Every case
// runs at device scale 1 and 1.5. build.bat builds and runs it; MG_PAGE_FIBER=1 runs it on the page fiber
// (../fiber-gc-test/FiberRun.h).
//
//   gamepad_page_test.exe <ultralight sdk dir> <tool dir> <out dir>

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/Listener.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "GamepadScript.h"
#include "../fiber-gc-test/FiberRun.h"

using namespace ultralight;

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

static int RunAtScale(const std::string& tool, double scale)
{
    ViewConfig vc;
    vc.is_accelerated = false;
    vc.initial_device_scale = scale;
    RefPtr<View> view = g_r->CreateView(1600, 900, vc, nullptr);
    Loaded loaded;
    view->set_load_listener(&loaded);
    view->LoadHTML(ReadFile(tool + "/page.html").c_str(), "file:///gamepad-page-test/page.html");
    for (int i = 0; i < 600 && !loaded.ready; ++i) Pump(1);
    if (!loaded.ready) {
        std::fprintf(stderr, "page never reached DOM ready\n");
        return 1;
    }
    view->Focus();
    String exc;
    Eval(*view, ReadFile(tool + "/cases.js"), exc);
    if (exc.empty()) Eval(*view, Magelight::Gamepad::kPageScript, exc);
    if (!exc.empty()) {
        std::fprintf(stderr, "setup threw: %s\n", exc.utf8().data());
        return 1;
    }
    Pump(2);
    const int count = std::atoi(Eval(*view, "__gpCount()", exc).c_str());
    int failed = 0;
    for (int n = 0; n < count; ++n) {
        const std::string name = Eval(*view, "__gpName(" + std::to_string(n) + ")", exc);
        std::string verdict;
        for (int s = 0; s < 16; ++s) {
            verdict = Eval(*view, "__gpStep(" + std::to_string(n) + "," + std::to_string(s) + ")", exc);
            Pump(3);
            if (!exc.empty()) {
                verdict = std::string("threw: ") + exc.utf8().data();
                break;
            }
            if (verdict != "next") break;
        }
        const bool ok = verdict.find("\"ok\":true") != std::string::npos;
        if (!ok) ++failed;
        std::printf("%s  scale %.1f  %s  %s\n", ok ? "ok  " : "FAIL", scale, name.c_str(), verdict.c_str());
    }
    if (count == 0) {
        std::printf("FAIL  scale %.1f  no cases ran\n", scale);
        ++failed;
    }
    return failed;
}

static int ToolMain(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: gamepad_page_test <sdk dir> <tool dir> <out dir>\n");
        return 2;
    }
    const std::string sdk = argv[1], tool = argv[2], out = argv[3];
    Config config;
    Platform::instance().set_config(config);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    Platform::instance().set_file_system(GetPlatformFileSystem(sdk.c_str()));
    Platform::instance().set_logger(GetDefaultLogger((out + "/ultralight.log").c_str()));
    g_r = Renderer::Create();
    int failed = RunAtScale(tool, 1.0);
    failed += RunAtScale(tool, 1.5);
    std::printf("%s\n", failed ? "FAILED" : "all passed");
    std::fflush(stdout);
    g_r = nullptr;
    return failed ? 1 : 0;
}

// MG_PAGE_FIBER=1: on the page fiber, as Magelight runs pages (../fiber-gc-test/FiberRun.h).
int main(int argc, char** argv) { return FiberRun::Run(argc, argv, &ToolMain); }
