// Paths as text (src/PathTextCore.h): the conversions Magelight makes between paths, UTF-8 and file:/// URLs, and a
// page loaded through Ultralight 2.0 from a folder whose name the ANSI code page cannot spell, read through a file
// system that resolves URLs the way MlFileSystem::Resolve does. Exit 0 when every check passes.
#include "PathTextCore.h"

#include <AppCore/Platform.h>
#include <Ultralight/Ultralight.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "Ultralight.lib")
#pragma comment(lib, "UltralightCore.lib")
#pragma comment(lib, "WebCore.lib")
#pragma comment(lib, "AppCore.lib")

namespace fs = std::filesystem;
namespace PT = Magelight::PathText;

namespace
{
    int g_fails = 0;

    void Check(bool ok, const std::string& what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
        if (!ok) ++g_fails;
    }

    std::string U8(const std::u8string& s) { return std::string(reinterpret_cast<const char*>(s.data()), s.size()); }

    bool IsAscii(const std::string& s)
    {
        for (const char c : s) if (static_cast<unsigned char>(c) >= 0x80) return false;
        return true;
    }

    // Names no single ANSI code page spells together: Chinese, Cyrillic, Japanese, an accent, a character outside
    // the BMP, and the URL's own delimiters.
    const std::wstring kNames[] = {
        L"D:\\\u6e38\u620f\\Skyrim Special Edition",                          // 游戏
        L"C:\\\u0418\u0433\u0440\u044b\\Skyrim",                                // Игры
        L"E:\\\u30b2\u30fc\u30e0\\Caf\u00e9 #1 50% ?x",                         // ゲーム
        L"F:\\Games\\\U0001F3AE\\Skyrim",                                       // a game controller emoji
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skyrim Special Edition",
    };

    void Units()
    {
        std::printf("-- conversions (ANSI code page %u)\n", GetACP());
        Check(PT::FromUtf8(U8(u8"D:\\游戏\\Skyrim")).native() == L"D:\\\u6e38\u620f\\Skyrim", "FromUtf8 reads UTF-8");
        Check(PT::FromUtf8("").empty() && PT::ToUtf8(fs::path()).empty(), "empty in, empty out");
        for (const auto& w : kNames) {
            const fs::path p(w);
            const std::string u = PT::ToUtf8(p);
            Check(PT::FromUtf8(u) == p, "UTF-8 round trip: " + u);
            const std::string url = PT::ToFileUrl(p);
            Check(IsAscii(url) && url.rfind("file:///", 0) == 0 && url.find('\\') == std::string::npos,
                  "file URL is plain ASCII: " + url);
            Check(PT::FromUtf8(PT::PathOfFileUrl(url)) == p, "file URL decodes back: " + u);
            // WebKit may hand a URL back with non-ASCII unescaped; it names the same file.
            std::string raw = "file:///" + u;
            for (auto& c : raw) if (c == '\\') c = '/';
            if (raw.find_first_of("#?%") == std::string::npos)
                Check(PT::FromUtf8(PT::PathOfFileUrl(raw)) == fs::path(p).make_preferred(), "raw UTF-8 URL decodes: " + u);
        }
        Check(PT::PathOfFileUrl("file:///C:/a%20b/x.html?v=3#top") == "C:/a b/x.html", "query and fragment dropped");
        const std::string rel = PT::ToRelativeFileUrl(U8(u8"views\\游戏 #1\\a%41.html"));
        Check(IsAscii(rel) && PT::PathOfFileUrl(rel) == U8(u8"views/游戏 #1/a%41.html"),
              "a relative page path's URL keeps '#' and '%' as names: " + rel);
        Check(PT::UrlDecode("%zz%4") == "%zz%4", "a malformed escape is kept");
        // Ultralight's FileSystem gets the path already decoded: '%' and '#' are part of the name, a query is not.
        Check(PT::PathOfFileRequest(U8(u8"C:/游戏/x%41y #1/a.html")) == U8(u8"C:/游戏/x%41y #1/a.html"),
              "a file request is not decoded again");
        Check(PT::PathOfFileRequest("C:/a/b.png?v=3") == "C:/a/b.png", "a file request's query is dropped");
        Check(PT::PathOfFileRequest("/resources/icudt67l.dat") == "resources/icudt67l.dat", "a relative request stays relative");
        Check(PT::PathOfFileRequest("file:///C:/a%20b/x.html") == "C:/a b/x.html", "a file:/// request is read as a URL");

        // An unpaired surrogate: no throw, U+FFFD.
        const std::string lone = PT::ToUtf8(fs::path(std::wstring(L"a\xD800" L"b")));
        Check(lone == "a\xEF\xBF\xBD" "b", "an unpaired surrogate becomes U+FFFD");

        // Text in the ANSI code page that is not UTF-8 (what an older API caller sends) is read in the code page.
        for (const std::wstring w : { std::wstring(L"Caf\u00e9"), std::wstring(L"\u6e38\u620f"), std::wstring(L"\u0418\u0433\u0440\u044b") }) {
            BOOL lost = FALSE;
            const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()),
                                              nullptr, 0, nullptr, &lost);
            std::string acp(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), acp.data(), n,
                                nullptr, &lost);
            const bool validUtf8 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, acp.data(), n, nullptr, 0) > 0;
            if (lost || validUtf8 || IsAscii(acp)) {
                std::printf("skip  ANSI fallback for %s (this code page %s)\n", PT::ToUtf8(w).c_str(),
                            lost ? "cannot spell it" : "spells it as valid UTF-8 or ASCII");
                continue;
            }
            Check(PT::FromUtf8(acp).native() == w, "ANSI code page text read as such: " + PT::ToUtf8(w));
        }

        // What std::filesystem does on its own, for the record: the conversions this header replaces.
        const fs::path cn(kNames[0]);
        try {
            const std::string s = cn.string();
            std::printf("info  path::string() of %s gave %zu bytes in code page %u\n", PT::ToUtf8(cn).c_str(), s.size(), GetACP());
        } catch (const std::exception& e) {
            std::printf("info  path::string() of %s throws: %s\n", PT::ToUtf8(cn).c_str(), e.what());
        }
        try {
            const fs::path back(PT::ToUtf8(cn));   // MSVC reads a std::string in the ANSI code page
            std::printf("info  path(UTF-8 std::string) %s the original\n", back == cn ? "matches" : "does NOT match");
        } catch (const std::exception& e) {
            std::printf("info  path(UTF-8 std::string) throws: %s\n", e.what());
        }
    }

    // MlFileSystem's resolution without its page-root gate: PathOfFileUrl, FromUtf8, relative to the base folder.
    class TestFileSystem final : public ultralight::FileSystem
    {
    public:
        explicit TestFileSystem(fs::path base) : base_(std::move(base)) {}

        fs::path Resolve(const ultralight::String& s)
        {
            const std::string u = s.utf8().data();
            {
                const std::lock_guard lock(mutex_);
                asked_.push_back(u);
            }
            const std::string p = PT::PathOfFileRequest(u);
            return (p.size() > 1 && p[1] == ':') ? PT::FromUtf8(p).make_preferred()
                                                 : base_ / PT::FromUtf8(p).make_preferred();
        }
        bool FileExists(const ultralight::String& s) override
        {
            std::error_code ec;
            return fs::is_regular_file(Resolve(s), ec);
        }
        ultralight::String GetFileMimeType(const ultralight::String& s) override
        {
            const std::string ext = PT::ToUtf8(Resolve(s).extension());
            if (ext == ".html") return "text/html";
            if (ext == ".css") return "text/css";
            if (ext == ".js") return "application/javascript";
            return "application/octet-stream";
        }
        ultralight::String GetFileCharset(const ultralight::String&) override { return "utf-8"; }
        ultralight::RefPtr<ultralight::Buffer> OpenFile(const ultralight::String& s) override
        {
            std::ifstream f(Resolve(s), std::ios::binary);
            if (!f) return nullptr;
            const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return ultralight::Buffer::CreateFromCopy(data.data(), data.size());
        }
        std::vector<std::string> Asked()
        {
            const std::lock_guard lock(mutex_);
            return asked_;
        }

    private:
        fs::path base_;
        std::mutex mutex_;
        std::vector<std::string> asked_;
    };

    class LoadWatch final : public ultralight::LoadListener
    {
    public:
        bool done = false, failed = false;
        void OnFinishLoading(ultralight::View*, uint64_t, bool isMain, const ultralight::String&) override
        {
            if (isMain) done = true;
        }
        void OnFailLoading(ultralight::View*, uint64_t, bool isMain, const ultralight::String& url,
                           const ultralight::String& desc, const ultralight::String&, int code) override
        {
            std::printf("info  load failed (%d) %s: %s\n", code, url.utf8().data(), desc.utf8().data());
            if (isMain) failed = done = true;
        }
    };

    void WriteFile(const fs::path& p, const std::string& text)
    {
        std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
    }

    void EndToEnd(const fs::path& exeDir)
    {
        std::printf("-- a page through Ultralight from a non-ASCII folder\n");
        // 页面 Страница: Chinese and Cyrillic together, which no ANSI code page spells; '#' and '%' would cut or
        // mangle a URL that left them unescaped.
        const fs::path dir = exeDir / L"pages" / L"\u9875\u9762 \u0421\u0442\u0440\u0430\u043d\u0438\u0446\u0430 #1 x%41y";
        std::error_code ec;
        fs::create_directories(dir, ec);
        // The page names its style sheet and script by non-ASCII relative URLs, as a mod's page may.
        WriteFile(dir / L"index.html", U8(u8"<!doctype html><html><head><meta charset=\"utf-8\">"
                                          u8"<link rel=\"stylesheet\" href=\"样式.css?v=3#frag\"></head>"
                                          u8"<body><div id=\"t\">waiting</div><script src=\"%E8%84%9A%E6%9C%AC.js\"></script></body></html>"));
        WriteFile(dir / L"\u6837\u5f0f.css", "#t { width: 123px; }");
        WriteFile(dir / L"\u811a\u672c.js", "document.getElementById('t').textContent = 'script ran';");

        TestFileSystem files(exeDir);
        ultralight::Config cfg;
        ultralight::Platform::instance().set_config(cfg);
        ultralight::Platform::instance().set_font_loader(ultralight::GetPlatformFontLoader());
        ultralight::Platform::instance().set_file_system(&files);

        auto renderer = ultralight::Renderer::Create();
        ultralight::ViewConfig vc;
        vc.is_accelerated = false;
        auto view = renderer->CreateView(400, 200, vc, nullptr);
        LoadWatch watch;
        view->set_load_listener(&watch);
        const std::string url = PT::ToFileUrl(dir / L"index.html");
        std::printf("info  LoadURL %s\n", url.c_str());
        view->LoadURL(url.c_str());
        for (int i = 0; i < 400 && !watch.done; ++i) {
            renderer->Update();
            renderer->RefreshDisplay(0);
            renderer->Render();
            Sleep(5);
        }
        for (int i = 0; i < 20; ++i) { renderer->Update(); Sleep(5); }
        ultralight::String exception;
        const std::string got = view->EvaluateScript(
            "var t=document.getElementById('t'); t.textContent + '|' + getComputedStyle(t).width", &exception).utf8().data();
        std::printf("info  page says '%s'%s\n", got.c_str(), exception.empty() ? "" : " (script threw)");
        for (const auto& a : files.Asked()) std::printf("info  file system asked for %s\n", a.c_str());
        Check(watch.done && !watch.failed, "the page loads");
        Check(got == "script ran|123px", "its non-ASCII style sheet and script load from that folder");
    }
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const fs::path exeDir = fs::path(buf).parent_path();
    Units();
    EndToEnd(exeDir);
    std::printf("%s (%d failed)\n", g_fails ? "FAILED" : "ALL PASS", g_fails);
    return g_fails;
}
