// Page roots (src/PageRootCore.h): containment, the rules for a folder AddPageRoot takes, and which root a page reads
// from. Exit 0 when every check passes.
#include "PageRootCore.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace PR = Magelight::PageRootRules;
using std::filesystem::path;

namespace
{
    int g_fails = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++g_fails;
    }

    const path kData = LR"(C:\Games\Skyrim Special Edition\Data)";
    const path kMods = kData / L"Magelight";
    const path kRuntime = kData / L"SKSE" / L"Plugins" / L"Magelight";

    const char* Refuse(const wchar_t* folder) { return PR::Refusal(PR::Normalize(folder), kData, kMods, kRuntime); }
}

int main()
{
    // Containment.
    Check(PR::Under(LR"(C:\a\b\c.js)", LR"(C:\a\b)"), "a file under a folder");
    Check(PR::Under(LR"(C:\A\B)", LR"(c:\a\b\)"), "the folder itself, letter case and a trailing separator aside");
    Check(!PR::Under(LR"(C:\a\bc\x.js)", LR"(C:\a\b)"), "a sibling whose name starts the same is not under it");
    Check(!PR::Under(LR"(C:\a\b\..\x.js)", LR"(C:\a\b)"), "\"..\" is collapsed before the test");
    Check(PR::Under(LR"(C:/a/b/c.js)", LR"(C:\a\b)"), "forward slashes");
    Check(!PR::Under(L"", LR"(C:\a)") && !PR::Under(LR"(C:\a)", L""), "an empty path is under nothing");

    // Normalize.
    Check(PR::Normalize(LR"(C:\a\b\)") == path(LR"(C:\a\b)"), "a trailing separator is dropped");
    Check(PR::Normalize(LR"(C:/a/./b/../c)") == path(LR"(C:\a\c)"), "\".\" and \"..\" collapse, slashes turn");
    Check(PR::Normalize(LR"(C:\)") == path(LR"(C:\)"), "a drive root keeps its separator");
    Check(PR::Normalize(LR"(views\UD)").empty(), "a relative folder is empty");

    // The folder rules: the issue's layout is taken, wider or host folders are not.
    const wchar_t* prisma = LR"(C:\Games\Skyrim Special Edition\Data\PrismaUI\views\UD)";
    Check(Refuse(prisma) == nullptr, "a folder under Data (another UI's views) is taken");
    Check(Refuse(LR"(c:\games\skyrim special edition\data\prismaui\views\ud\)") == nullptr,
          "letter case and a trailing separator do not matter");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition\Data\SKSE\Plugins\OtherMod\ui)") == nullptr,
          "another plugin's folder beside Magelight's is taken");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition\Data)") != nullptr, "Data itself is refused");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition)") != nullptr, "the game folder is refused");
    Check(Refuse(LR"(C:\)") != nullptr, "a drive root is refused");
    Check(Refuse(LR"(D:\MyUI\views)") != nullptr, "a folder outside the game is refused");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition\Data\..\Data2\x)") != nullptr, "\"..\" cannot climb out of Data");
    Check(Refuse(LR"(views\UD)") != nullptr, "a relative folder is refused");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition\Data\Magelight\Mod)") != nullptr,
          "a folder in Data\\Magelight is refused (served already)");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition\Data\Magelight)") != nullptr, "Data\\Magelight itself is refused");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition\Data\SKSE)") != nullptr, "a folder holding the runtime is refused");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition\Data\SKSE\Plugins)") != nullptr,
          "the runtime's parent is refused");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition\Data\SKSE\Plugins\Magelight\views)") != nullptr,
          "a folder inside the runtime is refused");
    Check(Refuse(LR"(C:\Games\Skyrim Special Edition\Data\SKSE\Plugins\MagelightX)") == nullptr,
          "a sibling whose name starts like the runtime's is taken");

    // Which root a page reads from.
    const std::vector<path> roots = { PR::Normalize(prisma), PR::Normalize(std::wstring(prisma) + LR"(\Minigames)") };
    Check(PR::Deepest(roots, PR::Normalize(std::wstring(prisma) + LR"(\Hud\Hud.html)")) == roots[0],
          "a page under one root reads from it");
    Check(PR::Deepest(roots, PR::Normalize(std::wstring(prisma) + LR"(\Minigames\Lock\index.html)")) == roots[1],
          "the deepest root that holds the page wins");
    Check(PR::Deepest(roots, PR::Normalize(LR"(C:\Games\Skyrim Special Edition\Data\PrismaUI\views\Other\x.html)"))
              .empty(),
          "a page under no root reads from none");
    Check(PR::Deepest(roots, PR::Normalize(L"views/app/index.html")).empty(), "a relative page reads from none");
    Check(PR::Contains(roots, PR::Normalize(LR"(c:\games\skyrim special edition\data\prismaui\views\ud\)")),
          "a root added again is found, letter case aside");
    Check(!PR::Contains(roots, PR::Normalize(LR"(C:\Games\Skyrim Special Edition\Data\PrismaUI\views)")),
          "a wider folder is not already a root");

    std::printf("%s (%d failed)\n", g_fails ? "FAILED" : "ALL PASSED", g_fails);
    return g_fails ? 1 : 0;
}
