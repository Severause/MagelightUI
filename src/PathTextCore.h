#pragma once
// Paths as text. Every std::string path in Magelight is UTF-8: what Ultralight hands the file system, what a manifest
// or Magelight.json names, what a page's URL carries. std::filesystem's own string conversions (path(std::string),
// path::string()) use the ANSI code page instead: they misread UTF-8 and throw for a character the code page cannot
// spell, which takes the game down under a folder like D:\游戏\. Convert through these, never through those.

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace Magelight::PathText {

    // UTF-8 text as a path. Text that is not valid UTF-8 is read in the ANSI code page, which is what an older API
    // caller passing path::string() output sends; plain ASCII is the same either way. Never throws.
    inline std::filesystem::path FromUtf8(std::string_view s)
    {
        if (s.empty()) return {};
        const int len = static_cast<int>(s.size());
        UINT cp = CP_UTF8;
        int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.data(), len, nullptr, 0);
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (n <= 0) {
            cp = CP_ACP;
            flags = 0;
            n = MultiByteToWideChar(cp, flags, s.data(), len, nullptr, 0);
        }
        if (n <= 0) return {};
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(cp, flags, s.data(), len, w.data(), n);
        return std::filesystem::path(std::move(w));
    }

    // A path as UTF-8. Never throws (an unpaired surrogate becomes U+FFFD).
    inline std::string ToUtf8(const std::filesystem::path& p)
    {
        const std::wstring& w = p.native();
        if (w.empty()) return {};
        const int len = static_cast<int>(w.size());
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), len, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), len, out.data(), n, nullptr, nullptr);
        return out;
    }

    // A UTF-8 path appended to a URL: forward slashes, and every byte outside the ASCII set a path keeps (letters,
    // digits, - . _ ~ / : and the sub-delims WebKit leaves alone) percent-encoded, so the URL is plain ASCII.
    inline void AppendUrlPath(std::string& out, std::string_view utf8)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        for (char ch : utf8) {
            const auto c = static_cast<unsigned char>(ch);
            if (c == '\\') { out += '/'; continue; }
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                              c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':' || c == '!' ||
                              c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' ||
                              c == ',' || c == ';' || c == '=' || c == '@';
            if (keep) {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += kHex[c >> 4];
                out += kHex[c & 0xF];
            }
        }
    }

    // An absolute path as a file:/// URL, plain ASCII whatever the folder is called. PathOfFileUrl is its inverse.
    inline std::string ToFileUrl(const std::filesystem::path& p)
    {
        std::string out = "file:///";
        AppendUrlPath(out, ToUtf8(p));
        return out;
    }

    // A path relative to the runtime dir (UTF-8, or ANSI as FromUtf8 reads it) as a file:/// URL, encoded the same
    // way: a '#' or '%' in it is part of a file name, never URL syntax.
    inline std::string ToRelativeFileUrl(std::string_view relative)
    {
        std::string out = "file:///";
        AppendUrlPath(out, ToUtf8(FromUtf8(relative)));
        return out;
    }

    // %XX escapes decoded to their bytes; anything else, a malformed escape included, kept as it is.
    inline std::string UrlDecode(std::string_view in)
    {
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string out;
        out.reserve(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%' && i + 2 < in.size()) {
                const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out += static_cast<char>((hi << 4) | lo);
                    i += 2;
                    continue;
                }
            }
            out += in[i];
        }
        return out;
    }

    // The file a file:/// URL (one a mod navigates to) names, as a page path (UTF-8): no query string or fragment,
    // percent-decoded, no leading slash. A drive-letter path is absolute; anything else is relative to the runtime
    // dir. Non-ASCII left raw in the URL reads the same as escaped.
    inline std::string PathOfFileUrl(std::string p)
    {
        if (const auto q = p.find('?'); q != std::string::npos) p.resize(q);
        if (const auto h = p.find('#'); h != std::string::npos) p.resize(h);
        if (p.rfind("file:///", 0) == 0) p = p.substr(8);
        p = UrlDecode(p);
        while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(p.begin());
        return p;
    }

    // The file Ultralight's FileSystem is asked for, as a page path (UTF-8). Ultralight 2.0 passes the path itself,
    // already decoded ("C:/Games/页面/index.html"), so it is never decoded again: a folder named "x%41y" stays one.
    // A query string is dropped ('?' cannot be in a Windows file name); a '#' is part of the name. A file:/// URL is
    // read as one.
    inline std::string PathOfFileRequest(std::string p)
    {
        if (p.rfind("file:", 0) == 0) return PathOfFileUrl(std::move(p));
        if (const auto q = p.find('?'); q != std::string::npos) p.resize(q);
        while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(p.begin());
        return p;
    }

}
