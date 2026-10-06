// Font fallback test: runs Ultralight (stock SDK runtime, CPU renderer, no window) over a page
// under a font loader that imitates a broken system, with and without MagelightFonts' wrapper.
//
//   font_test.exe            runs every scenario as a child process and checks the results
//   font_test.exe <name>     runs one scenario; prints "widths a,b,c,d,e" and exits 0 (3 when the
//                            measuring script throws), or dies naming the faulting module and offset
//
// Each child prints the rendered widths of five spans (see kPage); the parent compares them.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>

#include "MagelightFonts.h"

#pragma comment(lib, "Ultralight.lib")
#pragma comment(lib, "UltralightCore.lib")
#pragma comment(lib, "WebCore.lib")
#pragma comment(lib, "AppCore.lib")

using namespace ultralight;

namespace
{
    // a: a family no system has (falls to the standard font, then the last resort)
    // b: the bundled family's name     c: PathTest (the non-ASCII path scenario)
    // d: Times New Roman               e: Arial
    const char* kPage = R"(<html><body style="margin:0">
<span id="a" style="font-family:NoSuchFontAnywhere;font-size:40px">Hamburgefonstiv 0123</span><br>
<span id="b" style="font-family:'Magelight Fallback';font-size:40px">Hamburgefonstiv 0123</span><br>
<span id="c" style="font-family:PathTest;font-size:40px">Hamburgefonstiv 0123</span><br>
<span id="d" style="font-family:'Times New Roman';font-size:40px">Hamburgefonstiv 0123</span><br>
<span id="e" style="font-family:Arial;font-size:40px">Hamburgefonstiv 0123</span>
</body></html>)";

    const char* kMeasure =
        "['a','b','c','d','e'].map(function(i){return document.getElementById(i).getBoundingClientRect().width.toFixed(2)}).join(',')";

    std::wstring g_pathTestFile;   // a copy of DejaVu Sans under a non-ASCII folder name

    // Imitates a system: "stripped" has no fonts at all, "badpath" hands back a path that does not
    // exist, "nonascii" serves PathTest from a non-ASCII folder and everything else normally.
    class FakeSystem final : public FontLoader
    {
    public:
        FakeSystem(FontLoader* real, std::string mode) : real_(real), mode_(std::move(mode)) {}
        String fallback_font() const override { return real_->fallback_font(); }
        String fallback_font_for_characters(const String& c, int w, bool i) const override
        {
            if (mode_ == "stripped" || mode_ == "badpath") return real_->fallback_font();
            return real_->fallback_font_for_characters(c, w, i);
        }
        RefPtr<FontFile> Load(const String& family, int weight, bool italic) override
        {
            if (mode_ == "stripped") return nullptr;
            if (mode_ == "badpath") return FontFile::Create(String("C:\\no-such-folder\\arial.ttf"));
            if (mode_ == "nonascii" && std::string(family.utf8().data()) == "PathTest")
                return FontFile::Create(String(reinterpret_cast<const Char16*>(g_pathTestFile.c_str()), g_pathTestFile.size()));
            return real_->Load(family, weight, italic);
        }

    private:
        FontLoader* real_;
        std::string mode_;
    };

    void Log(int level, const char* m) { std::printf("  [%s] %s\n", level ? "warn" : "info", m); }

    LONG WINAPI DieQuietly(EXCEPTION_POINTERS* e)
    {
        // Name the faulting module and offset, to match a field log's "(module +0xOFFSET)".
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        const auto at = reinterpret_cast<std::uintptr_t>(e->ExceptionRecord->ExceptionAddress);
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(at), &mod))
            GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        std::printf("  crash 0x%08lX at %s +0x%llX\n", e->ExceptionRecord->ExceptionCode, base ? base + 1 : name,
                    static_cast<unsigned long long>(at - reinterpret_cast<std::uintptr_t>(mod)));
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), e->ExceptionRecord->ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    std::filesystem::path ExeDir()
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path();
    }

    // scenario = <loader>-<system>: loader "stock" (the platform's, as Magelight 0.31.2 used it) or
    // "safe" (wrapped); system "normal", "stripped", "badpath", "nonascii".
    int RunScenario(const std::string& scenario)
    {
        // Only an unhandled fault ends the child: JavaScriptCore handles some access violations itself.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        SetUnhandledExceptionFilter(DieQuietly);

        const auto dash = scenario.find('-');
        const std::string loader = scenario.substr(0, dash), system = scenario.substr(dash + 1);
        const auto dir = ExeDir();
        g_pathTestFile = (dir / L"f\u00f6nts-\u30c6\u30b9\u30c8" / L"DejaVuSans.ttf").wstring();

        FontLoader* real = GetPlatformFontLoader();
        FontLoader* fonts = system == "normal" ? real : new FakeSystem(real, system);
        if (loader == "safe") fonts = MagelightFonts::CreateLoader(fonts, &Log);

        Config cfg;
        Platform::instance().set_config(cfg);
        Platform::instance().set_font_loader(fonts);
        const auto base = dir.u8string();
        Platform::instance().set_file_system(GetPlatformFileSystem(String(reinterpret_cast<const char*>(base.c_str()))));
        Platform::instance().set_logger(GetDefaultLogger("ultralight.log"));
        std::printf("  fallback_font() = %s\n", fonts->fallback_font().utf8().data());

        RefPtr<Renderer> renderer = Renderer::Create();
        ViewConfig vc;
        vc.is_accelerated = false;
        RefPtr<View> view = renderer->CreateView(1200, 400, vc, nullptr);
        view->LoadHTML(kPage);
        for (int i = 0; i < 120; ++i) {
            renderer->Update();
            renderer->RefreshDisplay(0);
            renderer->Render();
            Sleep(5);
        }
        String exception;
        const String widths = view->EvaluateScript(kMeasure, &exception);
        renderer->Update();
        renderer->Render();
        std::printf("widths %s\n", widths.utf8().data());
        std::fflush(stdout);
        return exception.empty() ? 0 : 3;
    }

    constexpr DWORD kTimedOut = 0xDEAD0001;   // the exit code a child killed after 60 s gets

    struct Result
    {
        DWORD code = 0;
        std::vector<double> w;   // a..e
        std::string out;
    };

    Result Child(const std::string& scenario)
    {
        Result r;
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE rd = nullptr, wr = nullptr;
        // A child writes a few hundred bytes; the buffer holds them all, so the parent can wait for
        // the child (with a timeout) before reading.
        CreatePipe(&rd, &wr, &sa, 1 << 16);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = wr;
        PROCESS_INFORMATION pi{};
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string cmd = std::string("\"") + exe + "\" " + scenario;
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            r.code = 0xFFFFFFFF;
            return r;
        }
        CloseHandle(wr);
        if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, kTimedOut);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        GetExitCodeProcess(pi.hProcess, &r.code);
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) r.out.append(buf, got);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(rd);
        if (const auto at = r.out.find("widths "); at != std::string::npos) {
            const char* p = r.out.c_str() + at + 7;
            for (int i = 0; i < 5; ++i) {
                char* end = nullptr;
                r.w.push_back(std::strtod(p, &end));
                p = (*end == ',') ? end + 1 : end;
            }
        }
        return r;
    }

    bool Near(double a, double b) { return std::fabs(a - b) < 0.5; }

    // The field log's crash: lastResortFallbackFont dereferencing a null font (Ultralight 1.4.0b.081c48b).
    bool FieldCrash(const Result& r)
    {
        return r.code == EXCEPTION_ACCESS_VIOLATION && r.out.find("at WebCore.dll +0xFB7F77") != std::string::npos;
    }
}

int main(int argc, char** argv)
{
    if (argc > 1) return RunScenario(argv[1]);

    // The non-ASCII folder the "nonascii" system serves PathTest from.
    const auto dir = ExeDir();
    const auto folder = dir / L"f\u00f6nts-\u30c6\u30b9\u30c8";
    std::filesystem::create_directories(folder);
    std::filesystem::copy_file(dir / L"DejaVuSans.ttf", folder / L"DejaVuSans.ttf",
                               std::filesystem::copy_options::overwrite_existing);

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };
    auto run = [&](const char* s) {
        const Result r = Child(s);
        std::printf("== %s (exit 0x%08lX)\n%s", s, r.code, r.out.c_str());
        return r;
    };

    const Result stockNormal = run("stock-normal");
    const Result safeNormal = run("safe-normal");
    const Result stockStripped = run("stock-stripped");
    const Result safeStripped = run("safe-stripped");
    const Result stockBadpath = run("stock-badpath");
    const Result safeBadpath = run("safe-badpath");
    const Result stockNonascii = run("stock-nonascii");
    const Result safeNonascii = run("safe-nonascii");

    std::printf("\n== checks\n");
    check(stockNormal.code == 0 && stockNormal.w.size() == 5, "stock loader renders on a normal system");
    check(safeNormal.code == 0 && safeNormal.w.size() == 5, "wrapped loader renders on a normal system");
    if (stockNormal.w.size() == 5 && safeNormal.w.size() == 5) {
        check(Near(stockNormal.w[0], safeNormal.w[0]) && Near(stockNormal.w[3], safeNormal.w[3]) &&
                  Near(stockNormal.w[4], safeNormal.w[4]),
              "normal system: an unknown family, Times New Roman and Arial measure the same wrapped and stock");
        check(!Near(safeNormal.w[1], safeNormal.w[3]), "normal system: 'Magelight Fallback' is a font of its own when wrapped");
    }
    check(FieldCrash(stockStripped), "no fonts at all: the stock loader crashes at the field log's WebCore.dll +0xFB7F77");
    check(safeStripped.code == 0 && safeStripped.w.size() == 5, "no fonts at all: the wrapped loader renders");
    if (safeStripped.w.size() == 5 && safeNormal.w.size() == 5)
        check(Near(safeStripped.w[0], safeNormal.w[1]), "no fonts at all: text falls back to the bundled font");
    check(FieldCrash(stockBadpath), "unreadable font files: the stock loader crashes at WebCore.dll +0xFB7F77");
    check(safeBadpath.code == 0 && safeBadpath.w.size() == 5, "unreadable font files: the wrapped loader renders");
    if (safeBadpath.w.size() == 5 && safeNormal.w.size() == 5)
        check(Near(safeBadpath.w[0], safeNormal.w[1]), "unreadable font files: text falls back to the bundled font");
    check(safeNonascii.code == 0 && safeNonascii.w.size() == 5, "non-ASCII font path: the wrapped loader renders");
    if (safeNonascii.w.size() == 5 && safeNormal.w.size() == 5)
        check(Near(safeNonascii.w[2], safeNormal.w[1]), "non-ASCII font path: the wrapped loader loads the font");
    if (stockNonascii.w.size() == 5)
        std::printf("  info: stock loader, non-ASCII path: PathTest %s\n",
                    Near(stockNonascii.w[2], stockNonascii.w[3]) ? "did not load (fell back)" : "loaded (this machine's code page is UTF-8)");

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
