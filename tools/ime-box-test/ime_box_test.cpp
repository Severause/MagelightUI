// Where the page core's IME candidate box lands, in Ultralight's own WebKit (CPU renderer, no window, no game): loads
// page.html, installs the page core's IME block (extract.cjs) behind a stub window.magelight, and for each case in
// cases.js composes the reading with the view's Editor, reads the anchor as the host does (the Editor's bounds of the
// character src/ImeCore.h AnchorChar names), sends the '__ime' payload Ime::Payload builds, and
// prints what the page measured (the box's place, and its text against the field's), writing scale*_case<N>.png. Every case runs at device scale 1 and 1.5. build.bat builds and
// runs it; MG_PAGE_FIBER=1 runs it on the page fiber (../fiber-gc-test/FiberRun.h).
//
//   ime_box_test.exe <ultralight sdk dir> <tool dir> <bridge-ime.js> <out dir>

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/Editor.h>
#include <Ultralight/Listener.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ImeCore.h"
#include "../fiber-gc-test/FiberRun.h"

using namespace ultralight;
namespace Ime = Magelight::Ime;

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

// The few fields of __imeCase's answer this driver needs (a flat JSON object it wrote itself).
static std::string Field(const std::string& json, const std::string& key)
{
    const std::string k = "\"" + key + "\":";
    const auto at = json.find(k);
    if (at == std::string::npos) return {};
    std::size_t i = at + k.size(), j = i;
    if (json[i] == '"') {
        for (j = i + 1; j < json.size() && json[j] != '"'; ++j)
            if (json[j] == '\\') ++j;
        return json.substr(i + 1, j - i - 1);
    }
    int depth = 0;
    for (; j < json.size(); ++j) {
        const char c = json[j];
        if (c == '[') ++depth;
        else if (c == ']') --depth;
        else if ((c == ',' || c == '}') && depth == 0) break;
    }
    return json.substr(i, j - i);
}

static std::vector<std::string> Strings(const std::string& arr)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; (i = arr.find('"', i)) != std::string::npos;) {
        const std::size_t j = arr.find('"', i + 1);
        if (j == std::string::npos) break;
        out.push_back(arr.substr(i + 1, j - i - 1));
        i = j + 1;
    }
    return out;
}

static std::vector<int> Ints(const std::string& arr)
{
    std::vector<int> out;
    std::stringstream s(arr.substr(arr.empty() ? 0 : 1));
    for (std::string n; std::getline(s, n, ',');)
        if (!n.empty() && n != "]") out.push_back(std::atoi(n.c_str()));
    return out;
}

static std::size_t Utf16Units(const std::string& u8)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < u8.size();) {
        const unsigned char c = static_cast<unsigned char>(u8[i]);
        const std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        n += len == 4 ? 2 : 1;
        i += len;
    }
    return n;
}

static int RunAtScale(const std::string& tool, const std::string& bridge, const std::string& out, double scale)
{
    ViewConfig vc;
    vc.is_accelerated = false;
    vc.initial_device_scale = scale;
    RefPtr<View> view = g_r->CreateView(1920, 1080, vc, nullptr);
    Loaded loaded;
    view->set_load_listener(&loaded);
    view->LoadHTML(ReadFile(tool + "/page.html").c_str(), "file:///ime-box-test/page.html");
    for (int i = 0; i < 600 && !loaded.ready; ++i) Pump(1);
    if (!loaded.ready) {
        std::fprintf(stderr, "page never reached DOM ready\n");
        return 1;
    }
    view->Focus();
    String exc;
    Eval(*view, ReadFile(tool + "/cases.js") + "\n" + bridge, exc);
    if (!exc.empty()) {
        std::fprintf(stderr, "setup threw: %s\n", exc.utf8().data());
        return 1;
    }
    Editor* ed = view->editor();
    int failed = 0;
    for (int n = 0; n <= 27; ++n) {
        const std::string setup = Eval(*view, "__imeCase(" + std::to_string(n) + ")", exc);
        Pump(3);
        if (!exc.empty()) {
            std::printf("FAIL  scale %.1f case %d setup threw: %s\n", scale, n, exc.utf8().data());
            ++failed;
            continue;
        }
        // The IME's state as the host reads it, the reading composed as the host composes it.
        Ime::Snapshot snap;
        snap.composition = Field(setup, "text");
        snap.caret = std::atoi(Field(setup, "caret").c_str());
        snap.clauses = Ints(Field(setup, "clauses"));
        snap.target = std::atoi(Field(setup, "target").c_str());
        snap.candidates = Strings(Field(setup, "candidates"));
        snap.selectedIndex = std::atoi(Field(setup, "selectedIndex").c_str());
        std::vector<CompositionSegment> segs;
        for (std::size_t i = 0; i + 1 < snap.clauses.size(); ++i) {
            CompositionSegment seg;
            seg.range = { static_cast<uint32_t>(snap.clauses[i]), static_cast<uint32_t>(snap.clauses[i + 1]) };
            seg.thick = static_cast<int>(i) == snap.target;
            segs.push_back(seg);
        }
        const uint32_t caret = static_cast<uint32_t>(snap.caret);
        ed->SetComposition(String(snap.composition.c_str()), segs.empty() ? nullptr : segs.data(), segs.size(),
                           CompositionRange{ caret, caret });
        Pump(2);
        Rect r = ed->GetCompositionCharacterBounds(Ime::AnchorChar(snap));
        if (r.IsEmpty()) r = ed->GetCaretBounds();
        snap.hasAnchor = !r.IsEmpty();
        snap.anchorX = r.left;
        snap.anchorY = r.top;
        snap.anchorH = r.bottom - r.top;
        if (Utf16Units(snap.composition) != ed->GetCompositionCharacterCount())
            std::printf("note  scale %.1f case %d: the composition holds %u units\n", scale, n, ed->GetCompositionCharacterCount());
        Eval(*view, "__imeSend(" + Magelight::JsonQuote(Ime::Payload("candidates", "", snap)) + ")", exc);
        if (Field(setup, "end") == "true") Eval(*view, "__imeSend(" + Magelight::JsonQuote(Ime::Payload("end", "", {})) + ")", exc);
        Pump(3);
        std::string json = Eval(*view, "__imeCheck(" + std::to_string(n) + ")", exc);
        if (!exc.empty()) json = std::string("{\"ok\":false,\"threw\":") + Magelight::JsonQuote(exc.utf8().data()) + "}";
        const bool ok = json.find("\"ok\":true") != std::string::npos;
        if (!ok) ++failed;
        std::printf("%s  scale %.1f case %d  anchor %.1f,%.1f h %.1f  %s\n", ok ? "ok  " : "FAIL", scale, n, snap.anchorX,
                    snap.anchorY, snap.anchorH, json.c_str());
        auto* surface = static_cast<BitmapSurface*>(view->surface());
        surface->bitmap()->WritePNG((out + "/scale" + (scale == 1.0 ? "1" : "15") + "_case" + std::to_string(n) + ".png").c_str());
        ed->CancelComposition();
        Pump(2);
    }
    return failed;
}

static int ToolMain(int argc, char** argv)
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
    g_r = Renderer::Create();
    int failed = RunAtScale(tool, bridge, out, 1.0);
    failed += RunAtScale(tool, bridge, out, 1.5);
    std::printf("%s\n", failed ? "FAILED" : "all passed");
    std::fflush(stdout);
    g_r = nullptr;
    return failed ? 1 : 0;
}

// MG_PAGE_FIBER=1: on the page fiber, as Magelight runs pages (../fiber-gc-test/FiberRun.h).
int main(int argc, char** argv) { return FiberRun::Run(argc, argv, &ToolMain); }
