// Ultralight's memory report parser (src/MemoryReportCore.h), on the desktop: build.bat builds and runs this; exit 0
// when every check passes. The fixtures are Renderer::GetMemoryUsage() texts captured from the SDK the host builds
// against: re-capture them at every SDK bump (CLAUDE.md, invariant 3).

#include "MemoryReportCore.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace Magelight::MemoryReport;

static int s_failed = 0;

static void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++s_failed;
}

static bool Near(double bytes, double mb) { return bytes >= 0 && std::fabs(bytes / 1048576.0 - mb) < 0.006; }

static std::string Read(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The same text with every line ending "\n" (lf) or "\r\n".
static std::string Endings(const std::string& t, bool crlf)
{
    std::string out;
    for (char c : t) {
        if (c == '\r') continue;
        if (c == '\n' && crlf) out += '\r';
        out += c;
    }
    return out;
}

static void Fixture(const std::string& dir, const char* file, double js, long long docs, double webcore,
                    double scripts, double images, double tiles, double tex, double rts, double priv)
{
    const std::string raw = Read(dir + "\\" + file);
    Check(!raw.empty(), (std::string(file) + ": fixture read").c_str());
    for (bool crlf : { false, true }) {
        const Figures f = Parse(Endings(raw, crlf));
        const std::string tag = std::string(file) + (crlf ? " (CRLF)" : " (LF)") + ": ";
        Check(Near(f.js, js) && f.documents == docs, (tag + "JS heap and live documents").c_str());
        Check(Near(f.webcore, webcore) && Near(f.processPrivate, priv), (tag + "bmalloc and private commit").c_str());
        Check(Near(f.scripts, scripts) && Near(f.images, images), (tag + "script and image caches").c_str());
        Check(Near(f.tiles, tiles), (tag + "compositor tiles").c_str());
        Check(Near(f.gpuTextures, tex) && Near(f.renderTargets, rts), (tag + "GPU textures and render targets").c_str());
        Check(Missing(f) == 0, (tag + "nothing missing").c_str());
    }
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "fixtures";

    Fixture(dir, "report-2.0.0-beta.2-gpu-6views.txt", 6.15, 6, 21.93, 11.54, 0, 75.75, 16.25, 132.36, 425.66);
    Fixture(dir, "report-2.0.0-beta.2-cpu-2views.txt", 2.27, 2, 6.75, 2.32, 0, 44.44, 0, 0, 172.02);

    {
        const Figures f = Parse(Read(dir + "\\report-2.0.0-beta.2-gpu-6views.txt"));
        Check(Line(f) == "JS 6.2 MB in 6 documents, WebCore 21.9 MB, script cache 11.5 MB, image cache 0.0 MB, "
                         "page tiles 75.8 MB, GPU textures 16.2 MB + render targets 132.4 MB; game process private 425.7 MB",
              "line: the gpu-6views figures as Magelight.log prints them");
    }

    // A label is read only inside its own section and only at the start of a line.
    const std::string anchored =
        "-- WebCore Compositor --\n"
        "    Textures:                 99.00 MB\n"
        "    Old Render Targets:        7.00 MB\n"
        "-- GPU Resources (VRAM estimated) --\n"
        "    Texture Pool:              1.00 MB\n"
        "    Textures:                  3.00 MB, VRAM: 3.00 MB  (4 textures)\n"
        "    Render Targets:            5.00 MB  (2 targets)\n"
        "-- Caches --\n"
        "    Render Targets:           11.00 MB\n";
    Check(Near(Bytes(anchored, "-- GPU Resources", "Textures:"), 3.0), "anchoring: Textures: of another section is never picked");
    Check(Near(Bytes(anchored, "-- GPU Resources", "Render Targets:"), 5.0),
          "anchoring: a label inside a longer one (Old Render Targets:) or past the section's end is never picked");
    Check(Bytes(anchored, "-- Caches --", "Textures:") < 0, "anchoring: a label the section lacks is missing");
    Check(Bytes(anchored, "-- Nowhere --", "Textures:") < 0, "anchoring: a missing section is missing");

    // Units, and what is not a figure.
    const std::string units =
        "-- U --\n"
        "  b: 512 B\n"
        "  kb: 3 KB\n"
        "  mb: 1.5 MB (x)\n"
        "  gb: 2.25 GB\n"
        "  mib: 4 MiB\n"
        "  none: n/a\n"
        "  nounit: 12\n"
        "  word: 7 Bytes\n"
        "  count:   42    (protected: 1)\n";
    Check(Bytes(units, "-- U --", "b:") == 512.0, "units: B");
    Check(Bytes(units, "-- U --", "kb:") == 3.0 * 1024, "units: KB");
    Check(Bytes(units, "-- U --", "mb:") == 1.5 * 1048576, "units: MB");
    Check(Bytes(units, "-- U --", "gb:") == 2.25 * 1073741824.0, "units: GB");
    Check(Bytes(units, "-- U --", "mib:") < 0 && Bytes(units, "-- U --", "word:") < 0, "units: an unknown unit is missing");
    Check(Bytes(units, "-- U --", "none:") < 0 && Bytes(units, "-- U --", "nounit:") < 0, "units: no number or no unit is missing");
    Check(Count(units, "-- U --", "count:") == 42 && Count(units, "-- U --", "none:") < 0, "count: a whole number, or missing");

    // A report a later SDK reshaped: every figure missing reads '?', never a number from somewhere else.
    {
        const Figures f = Parse("===== Memory Usage Report =====\nnothing here\n");
        Check(Missing(f) == kFigures, "missing: an unknown layout misses every figure");
        Check(Line(f) == "JS ? in ? documents, WebCore ?, script cache ?, image cache ?, page tiles ?, "
                         "GPU textures ? + render targets ?; game process private ?",
              "missing: every figure prints as '?'");
        Figures g = Parse(Read(dir + "\\report-2.0.0-beta.2-gpu-6views.txt"));
        g.tiles = -1;
        g.documents = -1;
        Check(Missing(g) == 2, "missing: counts each missing figure");
    }
    Check(Size(8.4e9) == "7.8 GB" && Size(0) == "0.0 MB" && Size(-1) == "?", "size: MB below 1 GB, GB from it, '?' missing");

    std::printf("%s\n", s_failed ? "SOME CHECKS FAILED" : "all checks passed");
    return s_failed ? 1 : 0;
}
