#pragma once
// Page roots (1.9.2): the folders the file system serves pages from, and the rules for a folder a plugin adds with
// AddPageRoot. Lexical and case-insensitive (Windows paths), std only: no filesystem calls, so it is safe on WebKit's
// worker threads and testable off the game (tools/pageroot-test).

#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace Magelight::PageRootRules {

    // p is base or inside it. ".." is collapsed first; a trailing separator on base is ignored.
    inline bool Under(const std::filesystem::path& p, const std::filesystem::path& base)
    {
        if (p.empty() || base.empty()) return false;
        const std::wstring a = p.lexically_normal().native();
        std::wstring b = base.lexically_normal().native();
        while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
        if (a.size() < b.size()) return false;
        for (std::size_t i = 0; i < b.size(); ++i)
            if (std::towlower(a[i]) != std::towlower(b[i])) return false;
        return a.size() == b.size() || a[b.size()] == L'\\' || a[b.size()] == L'/';
    }

    // A folder as the file system compares it: ".." collapsed, backslashes, no trailing separator. Empty for a path
    // that is not absolute.
    inline std::filesystem::path Normalize(std::filesystem::path p)
    {
        if (!p.is_absolute()) return {};
        p = p.make_preferred().lexically_normal();
        if (!p.has_filename() && p.has_relative_path()) p = p.parent_path();
        return p;
    }

    // Why `root` (Normalize'd) may not be a page root, or nullptr when it may. data = <game>\Data, mods =
    // Data\Magelight, runtime = the host's runtime folder. A root must sit inside Data, below it: a wider folder would
    // let every page read whatever lies there. Data\Magelight is already served, and a root inside it would change
    // where its mods' relative paths resolve. A root that holds the runtime folder (Data\SKSE, Data\SKSE\Plugins)
    // would hand the host's own pages its mod's network policy, and one inside it is the host's.
    inline const char* Refusal(const std::filesystem::path& root, const std::filesystem::path& data,
                               const std::filesystem::path& mods, const std::filesystem::path& runtime)
    {
        if (root.empty()) return "the folder is not an absolute path";
        if (!Under(root, data) || Under(data, root)) return "the folder is not inside the game's Data folder";
        if (Under(root, mods)) return "the folder is inside Data\\Magelight, whose pages are served already";
        if (Under(root, runtime) || Under(runtime, root)) return "the folder holds or is inside Magelight's own folder";
        return nullptr;
    }

    // The deepest of `roots` that holds `page`, or empty when none does.
    inline std::filesystem::path Deepest(const std::vector<std::filesystem::path>& roots,
                                         const std::filesystem::path& page)
    {
        const std::filesystem::path* best = nullptr;
        for (const auto& r : roots)
            if (Under(page, r) && (!best || r.native().size() > best->native().size())) best = &r;
        return best ? *best : std::filesystem::path{};
    }

    // `root` is already one of `roots` (letter case aside).
    inline bool Contains(const std::vector<std::filesystem::path>& roots, const std::filesystem::path& root)
    {
        for (const auto& r : roots)
            if (Under(r, root) && Under(root, r)) return true;
        return false;
    }

}  // namespace Magelight::PageRootRules
