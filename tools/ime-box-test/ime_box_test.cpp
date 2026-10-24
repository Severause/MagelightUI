// Where the page core's IME box lands, in Ultralight's own WebKit (CPU renderer, no window, no game): loads
// page.html, installs the bridge's IME block (extract.cjs) with a stub window.magelight, and for each case in
// cases.js prints what it measured and writes case<N>.png. build.bat builds and runs it.
//
//   ime_box_test.exe <ultralight sdk dir> <tool dir> <bridge-ime.js> <out dir>

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

static void Pump(Renderer& renderer, int frames)
{
    for (int i = 0; i < frames; ++i) {
        renderer.Update();
        renderer.RefreshDisplay(0);
        renderer.Render();
    }
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: ime_box_test <sdk dir> <tool dir> <bridge-ime.js> <out dir>\n");
        return 2;
    }
    const std::string sdk = argv[1], tool = argv[2], bridge = ReadFile(argv[3]), out = argv[4];
    Config config;
    Platform::instance().set_config(config);
    Platform::instance().set_font_loader(GetPlatformFontLoader());
    Platform::instance().set_file_system(GetPlatformFileSystem(sdk.c_str()));
    Platform::instance().set_logger(GetDefaultLogger((out + "/ultralight.log").c_str()));

    RefPtr<Renderer> renderer = Renderer::Create();
    ViewConfig vc;
    vc.is_accelerated = false;
    RefPtr<View> view = renderer->CreateView(1920, 1080, vc, nullptr);
    Loaded loaded;
    view->set_load_listener(&loaded);
    view->LoadHTML(ReadFile(tool + "/page.html").c_str(), "file:///ime-box-test/page.html");
    for (int i = 0; i < 600 && !loaded.ready; ++i) Pump(*renderer, 1);
    if (!loaded.ready) {
        std::fprintf(stderr, "page never reached DOM ready\n");
        return 1;
    }
    view->Focus();
    String exc;
    view->EvaluateScript((ReadFile(tool + "/cases.js") + "\n" + bridge).c_str(), &exc);
    if (!exc.empty()) {
        std::fprintf(stderr, "setup threw: %s\n", exc.utf8().data());
        return 1;
    }
    int failed = 0;
    for (int n = 0; n < 9; ++n) {
        exc = String();
        String r = view->EvaluateScript(("__imeCase(" + std::to_string(n) + ")").c_str(), &exc);
        Pump(*renderer, 3);
        // A case that needs typed characters: real key events, one per character, then its check.
        const std::string type = view->EvaluateScript("window.__imeTypeText||''", &exc).utf8().data();
        for (std::size_t i = 0; i < type.size();) {
            const unsigned char c = static_cast<unsigned char>(type[i]);
            const std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            KeyEvent key;
            key.type = KeyEvent::kType_Char;
            key.text = String(type.substr(i, len).c_str());
            key.unmodified_text = key.text;
            view->FireKeyEvent(key);
            Pump(*renderer, 1);
            i += len;
        }
        if (!type.empty()) r = view->EvaluateScript("__imeAfterType()", &exc);
        if (!exc.empty()) {
            std::printf("case %d threw: %s\n", n, exc.utf8().data());
            ++failed;
            continue;
        }
        const std::string json = r.utf8().data();
        const bool ok = json.find("\"ok\":true") != std::string::npos;
        if (!ok) ++failed;
        std::printf("%s  case %d  %s\n", ok ? "ok  " : "FAIL", n, json.c_str());
        auto* surface = static_cast<BitmapSurface*>(view->surface());
        const std::string png = out + "/case" + std::to_string(n) + ".png";
        surface->bitmap()->WritePNG(png.c_str());
    }
    std::printf("%s\n", failed ? "FAILED" : "all passed");
    return failed ? 1 : 0;
}
