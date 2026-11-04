// Polled keys (src/PolledKeysCore.h): which of another plugin's key reads a page's rule lets through, how a caller's
// lists are checked, and the manifest's key names. Exit 0 when every check passes.
#include "PolledKeysCore.h"

#include <cstdio>
#include <string>
#include <vector>

namespace PK = Magelight::PolledKeys;

namespace
{
    int g_fails = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++g_fails;
    }

    PK::Rule Built(std::vector<std::wstring> modules, std::vector<int> keys)
    {
        PK::Rule r;
        const std::string e = PK::Build(modules, keys, r);
        if (!e.empty()) std::printf("      (build refused: %s)\n", e.c_str());
        return r;
    }
}

int main()
{
    constexpr int kF5 = 0x74, kG = 'G', kA = 'A';

    // File names.
    Check(PK::FileNameLower(L"c:\\games\\data\\skse\\plugins\\VoiceChat.DLL") == L"voicechat.dll", "file name: path and case dropped");
    Check(PK::FileNameLower(L"Plain.dll") == L"plain.dll", "file name: no path");
    Check(PK::FileNameLower(L"a/b/Forward.dll") == L"forward.dll", "file name: forward slashes");

    // An empty rule passes everything.
    const PK::Rule all;
    Check(PK::Passes(all, L"other.dll", kA), "empty rule: any plugin, any key");

    // Modules alone.
    const PK::Rule mods = Built({ L"VoiceChat.dll" }, {});
    Check(PK::Passes(mods, L"voicechat.dll", kA), "modules: listed plugin, any key");
    Check(!PK::Passes(mods, L"other.dll", kA), "modules: other plugin hidden");

    // Keys alone.
    const PK::Rule keys = Built({}, { kF5, kG });
    Check(PK::Passes(keys, L"other.dll", kF5), "keys: listed key, any plugin");
    Check(!PK::Passes(keys, L"other.dll", kA), "keys: other key hidden");

    // Both: the plugin AND the key.
    const PK::Rule both = Built({ L"VoiceChat.dll" }, { kF5 });
    Check(PK::Passes(both, L"voicechat.dll", kF5), "both: listed plugin and key");
    Check(!PK::Passes(both, L"voicechat.dll", kA), "both: listed plugin, other key");
    Check(!PK::Passes(both, L"other.dll", kF5), "both: other plugin, listed key");

    // Modifiers: the neutral key covers both sides; one side covers itself.
    const PK::Rule ctrl = Built({}, { PK::kVkControl });
    Check(PK::Passes(ctrl, L"x.dll", PK::kVkControl), "VK_CONTROL listed: VK_CONTROL read");
    Check(PK::Passes(ctrl, L"x.dll", PK::kVkLControl) && PK::Passes(ctrl, L"x.dll", PK::kVkRControl), "VK_CONTROL listed: both sides read");
    Check(!PK::Passes(ctrl, L"x.dll", PK::kVkLShift), "VK_CONTROL listed: Shift hidden");
    const PK::Rule lshift = Built({}, { PK::kVkLShift });
    Check(PK::Passes(lshift, L"x.dll", PK::kVkLShift), "VK_LSHIFT listed: left read");
    Check(!PK::Passes(lshift, L"x.dll", PK::kVkRShift) && !PK::Passes(lshift, L"x.dll", PK::kVkShift),
          "VK_LSHIFT listed: right and neutral hidden");
    Check(!PK::Passes(keys, L"x.dll", 0) && !PK::Passes(keys, L"x.dll", 0x1FF), "keys: out-of-range reads hidden");

    // Build: what is refused, and what is folded.
    PK::Rule r;
    Check(!PK::Build({ L"" }, {}, r).empty(), "build refuses an empty name");
    Check(!PK::Build({ L"C:\\x\\VoiceChat.dll" }, {}, r).empty(), "build refuses a path");
    Check(!PK::Build({ L"VoiceChat" }, {}, r).empty(), "build refuses a name without .dll");
    Check(!PK::Build({}, { 0 }, r).empty() && !PK::Build({}, { 255 }, r).empty(), "build refuses keys outside 1-254");
    Check(!PK::Build(std::vector<std::wstring>(33, L"a.dll"), {}, r).empty(), "build refuses 33 modules");
    Check(!PK::Build({}, std::vector<int>(65, kA), r).empty(), "build refuses 65 keys");
    Check(PK::Build({ L"A.DLL", L"a.dll" }, { kG, kF5, kG }, r).empty() && r.modules.size() == 1 &&
          r.modules[0] == L"a.dll" && r.keys.size() == 2 && r.keys[0] == kG && r.keys[1] == kF5,
          "build lower-cases, removes duplicates, sorts keys");
    PK::Rule kept = Built({ L"kept.dll" }, {});
    PK::Build({ L"bad" }, {}, kept);
    Check(kept.modules.size() == 1 && kept.modules[0] == L"kept.dll", "a refused build leaves the rule alone");

    // Manifest key names.
    Check(PK::ParseKeyName("g") == 'G' && PK::ParseKeyName("Z") == 'Z' && PK::ParseKeyName("7") == '7', "names: letters and digits");
    Check(PK::ParseKeyName("F5") == 0x74 && PK::ParseKeyName("f24") == 0x87 && PK::ParseKeyName("F25") == 0, "names: F1-F24");
    Check(PK::ParseKeyName("Numpad0") == 0x60 && PK::ParseKeyName("numpad9") == 0x69 && PK::ParseKeyName("numpad10") == 0,
          "names: Numpad0-9");
    Check(PK::ParseKeyName("Ctrl") == PK::kVkControl && PK::ParseKeyName("RAlt") == PK::kVkRMenu &&
          PK::ParseKeyName("LShift") == PK::kVkLShift, "names: modifiers");
    Check(PK::ParseKeyName("PageDown") == 0x22 && PK::ParseKeyName("grave") == 0xC0, "names: named keys");
    Check(PK::ParseKeyName("f") == 'F', "names: a lone f is the F key, not a function key");
    Check(PK::ParseKeyName("") == 0 && PK::ParseKeyName("fx") == 0 && PK::ParseKeyName("banana") == 0, "names: unknown is 0");

    std::printf("%s (%d failed)\n", g_fails ? "FAILED" : "ALL PASSED", g_fails);
    return g_fails ? 1 : 0;
}
