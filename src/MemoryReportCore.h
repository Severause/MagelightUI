#pragma once
// Ultralight's memory figures for Magelight.log. Free 2.0 has no structured memory API (GCInfo / gc_status are Pro
// only), so the figures are read out of Renderer::GetMemoryUsage()'s text. Every label is looked up at the start of
// a line inside its own section, so a label another section also uses is never picked, and a figure a later SDK
// renames or drops reads as missing ('?'), never as another line's number. Engine-free, so tools/memreport-test
// runs it against reports captured from the SDK.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace Magelight::MemoryReport {

    // The section running from the line that starts with `section` to the next "-- " or "=====" line ("" if none).
    inline std::string_view Section(std::string_view text, std::string_view section)
    {
        std::size_t at = 0;
        while (at < text.size()) {
            std::size_t eol = text.find('\n', at);
            if (eol == std::string_view::npos) eol = text.size();
            std::string_view line = text.substr(at, eol - at);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            if (line.substr(0, section.size()) == section) {
                const std::size_t body = eol < text.size() ? eol + 1 : eol;
                std::size_t end = body;
                while (end < text.size()) {
                    std::size_t next = text.find('\n', end);
                    if (next == std::string_view::npos) next = text.size();
                    std::string_view l = text.substr(end, next - end);
                    while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
                    if (l.substr(0, 3) == "-- " || l.substr(0, 5) == "=====") break;
                    end = next < text.size() ? next + 1 : next;
                }
                return text.substr(body, end - body);
            }
            at = eol < text.size() ? eol + 1 : eol;
        }
        return {};
    }

    // What follows `label` on the first line of `body` that starts with it (leading spaces aside), spaces skipped;
    // "" when no line does.
    inline std::string_view After(std::string_view body, std::string_view label)
    {
        std::size_t at = 0;
        while (at < body.size()) {
            std::size_t eol = body.find('\n', at);
            if (eol == std::string_view::npos) eol = body.size();
            std::string_view line = body.substr(at, eol - at);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            if (!label.empty() && line.substr(0, label.size()) == label) {
                line.remove_prefix(label.size());
                while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
                return line;
            }
            at = eol < body.size() ? eol + 1 : eol;
        }
        return {};
    }

    // Bytes for "<label> <number> <unit>" in `section` (unit B, KB, MB or GB), or -1 when the section, the label,
    // the number or the unit is missing.
    inline double Bytes(std::string_view text, std::string_view section, std::string_view label)
    {
        std::string_view v = After(Section(text, section), label);
        std::size_t n = 0;
        bool digit = false;
        while (n < v.size() && ((v[n] >= '0' && v[n] <= '9') || v[n] == '.')) {
            digit = digit || v[n] != '.';
            ++n;
        }
        if (!digit) return -1;
        const double value = std::strtod(std::string(v.substr(0, n)).c_str(), nullptr);
        v.remove_prefix(n);
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        std::size_t u = 0;
        while (u < v.size() && v[u] >= 'A' && v[u] <= 'Z') ++u;
        const std::string_view unit = v.substr(0, u);
        if (u < v.size() && ((v[u] >= 'a' && v[u] <= 'z') || (v[u] >= '0' && v[u] <= '9'))) return -1;
        const double mul = unit == "B" ? 1.0 : unit == "KB" ? 1024.0 : unit == "MB" ? 1048576.0
                         : unit == "GB" ? 1073741824.0 : -1.0;
        return mul < 0 ? -1 : value * mul;
    }

    // The whole number after `label` in `section`, or -1.
    inline long long Count(std::string_view text, std::string_view section, std::string_view label)
    {
        const std::string_view v = After(Section(text, section), label);
        std::size_t n = 0;
        while (n < v.size() && v[n] >= '0' && v[n] <= '9' && n < 18) ++n;
        if (n == 0) return -1;
        return std::strtoll(std::string(v.substr(0, n)).c_str(), nullptr, 10);
    }

    struct Figures {
        double js = -1;               // JavaScript: GC Object Space
        double webcore = -1;          // Process Memory: bmalloc committed (WebCore and JavaScriptCore's heap)
        double scripts = -1;          // WebCore Resource Cache: Scripts
        double images = -1;           // WebCore Resource Cache: Images
        double tiles = -1;            // WebCore Compositor: BitmapTexturePool (composited layer tiles)
        double gpuTextures = -1;      // GPU Resources: Textures
        double renderTargets = -1;    // GPU Resources: Render Targets
        double processPrivate = -1;   // Process Memory: Private Commit (in game, the whole Skyrim process)
        long long documents = -1;     // JavaScript: Live Documents
    };
    inline constexpr int kFigures = 9;

    inline Figures Parse(std::string_view t)
    {
        Figures f;
        f.js = Bytes(t, "-- JavaScript --", "GC Object Space:");
        f.documents = Count(t, "-- JavaScript --", "Live Documents:");
        f.webcore = Bytes(t, "-- Process Memory --", "bmalloc committed:");
        f.processPrivate = Bytes(t, "-- Process Memory --", "Private Commit:");
        f.scripts = Bytes(t, "-- WebCore Resource Cache --", "Scripts:");
        f.images = Bytes(t, "-- WebCore Resource Cache --", "Images:");
        f.tiles = Bytes(t, "-- WebCore Compositor --", "BitmapTexturePool:");
        f.gpuTextures = Bytes(t, "-- GPU Resources", "Textures:");
        f.renderTargets = Bytes(t, "-- GPU Resources", "Render Targets:");
        return f;
    }

    // How many figures the report did not have (0 to kFigures).
    inline int Missing(const Figures& f)
    {
        int n = 0;
        for (double v : { f.js, f.webcore, f.scripts, f.images, f.tiles, f.gpuTextures, f.renderTargets, f.processPrivate })
            n += v < 0 ? 1 : 0;
        return n + (f.documents < 0 ? 1 : 0);
    }

    // "12.3 MB", "7.8 GB" from 1 GB up, "?" for a missing figure.
    inline std::string Size(double bytes)
    {
        if (bytes < 0) return "?";
        char s[32];
        if (bytes >= 1073741824.0) std::snprintf(s, sizeof s, "%.1f GB", bytes / 1073741824.0);
        else std::snprintf(s, sizeof s, "%.1f MB", bytes / 1048576.0);
        return s;
    }

    inline std::string Line(const Figures& f)
    {
        return "JS " + Size(f.js) + " in " + (f.documents < 0 ? std::string("?") : std::to_string(f.documents)) +
               " documents, WebCore " + Size(f.webcore) + ", script cache " + Size(f.scripts) + ", image cache " +
               Size(f.images) + ", page tiles " + Size(f.tiles) + ", GPU textures " + Size(f.gpuTextures) +
               " + render targets " + Size(f.renderTargets) + "; game process private " + Size(f.processPrivate);
    }

}  // namespace Magelight::MemoryReport
