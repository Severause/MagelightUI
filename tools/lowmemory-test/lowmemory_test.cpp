// The low-memory guard (src/LowMemoryCore.h): the floor, the lift's hysteresis and minimum time, when released views
// load again and its backoff, the setting's clamp. Exit 0 when every check passes.
#include "LowMemoryCore.h"

#include <cstdio>
#include <limits>

namespace LM = Magelight::LowMemory;

namespace
{
    int g_fails = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++g_fails;
    }

    constexpr std::uint64_t MB(std::uint64_t n) { return n * LM::kMiB; }
}

int main()
{
    // The setting.
    Check(LM::ClampFloorMb(768) == 768, "clamp: a plain number");
    Check(LM::ClampFloorMb(0) == 0, "clamp: 0 is off");
    Check(LM::ClampFloorMb(-5) == 0, "clamp: a negative is off");
    Check(LM::ClampFloorMb(std::numeric_limits<double>::quiet_NaN()) == 0, "clamp: NaN is off");
    Check(LM::ClampFloorMb(1e9) == LM::kMaxFloorMb, "clamp: capped");
    Check(LM::ClampFloorMb(512.9) == 512, "clamp: a fraction rounds down");

    // Thresholds.
    Check(LM::ResumeBytes(768) == MB(768 + 384), "resume: floor + half the floor");
    Check(LM::ResumeBytes(256) == MB(256 + 256), "resume: at least 256 MB above a small floor");
    Check(LM::Below(MB(767), 768) && !LM::Below(MB(768), 768), "below: strictly under the floor");
    Check(!LM::Below(0, 0), "below: never while off");

    // The guard over time.
    {
        LM::Guard g;
        Check(g.Update(MB(4000), 1000, 768) == LM::Change::None && !g.up, "guard: plenty of room stays down");
        Check(!g.ReloadDue(100000), "guard: never raised, no reload");
        Check(g.Update(MB(700), 1000, 768) == LM::Change::Raised && g.up, "guard: under the floor raises it");
        Check(g.Update(MB(100), 2000, 768) == LM::Change::None && g.up, "guard: stays up lower still");
        Check(g.Update(MB(900), 3000, 768) == LM::Change::None && g.up, "guard: above the floor is not enough");
        Check(g.Update(MB(1152), 5000, 768) == LM::Change::None && g.up, "guard: recovered, but up under 10 s");
        Check(g.Update(MB(1151), 11000, 768) == LM::Change::None && g.up, "guard: 10 s, one MB short of the lift");
        Check(!g.ReloadDue(11000), "guard: up, no reload");
        Check(g.Update(MB(1152), 11000, 768) == LM::Change::Lifted && !g.up, "guard: 10 s and the margin lift it");
        Check(!g.ReloadDue(11000 + LM::kReloadAfterLiftMs - 1), "reload: down, not long enough");
        Check(g.ReloadDue(11000 + LM::kReloadAfterLiftMs), "reload: down a minute");
        Check(g.Update(MB(900), 12000, 768) == LM::Change::None && !g.up, "guard: above the floor after a lift stays down");
        Check(g.Update(MB(767), 14000, 768) == LM::Change::Raised && g.upSinceMs == 14000, "guard: raised again, timer anew");
        Check(!g.ReloadDue(200000) && g.reloadStrikes == 0, "guard: raised again, no reload and no strike without one");
    }

    // The reload's backoff: the guard back up soon after a reload doubles the wait, up to the cap.
    {
        LM::Guard g;
        std::uint64_t t = 1000;
        g.Update(MB(100), t, 768);
        t += LM::kMinUpMs;
        g.Update(MB(2000), t, 768);                    // lifted
        t += LM::kReloadAfterLiftMs;
        Check(g.ReloadDue(t), "backoff: the first reload after a minute");
        g.NoteReload(t);
        t += 30000;
        Check(g.Update(MB(100), t, 768) == LM::Change::Raised && g.reloadStrikes == 1, "backoff: up again soon is a strike");
        t += LM::kMinUpMs;
        g.Update(MB(2000), t, 768);
        Check(!g.ReloadDue(t + LM::kReloadAfterLiftMs) && g.ReloadDue(t + 2 * LM::kReloadAfterLiftMs),
              "backoff: one strike doubles the wait");
        t += 2 * LM::kReloadAfterLiftMs;
        g.NoteReload(t);
        t += LM::kReloadSettleMs;                      // the pages held this time
        Check(g.Update(MB(100), t, 768) == LM::Change::Raised && g.reloadStrikes == 1, "backoff: up again later is no strike");
        g.reloadStrikes = 50;
        Check(g.ReloadDelayMs() == (LM::kReloadAfterLiftMs << LM::kMaxReloadDoublings), "backoff: capped");
    }

    {
        LM::Guard g;
        Check(g.Update(0, 1000, 0) == LM::Change::None && !g.up, "guard off: nothing raises it");
    }

    // What the page files can still grow by.
    Check(LM::DriveOfPageFile(L"\\??\\C:\\pagefile.sys") == L'C', "drive: the native name");
    Check(LM::DriveOfPageFile(L"z:\\pagefile.sys") == L'Z', "drive: a plain path, lower case");
    Check(LM::DriveOfPageFile(L"\\Device\\HarddiskVolume3\\pagefile.sys") == 0, "drive: a device path is unknown");
    Check(LM::DriveOfPageFile(L"") == 0 && LM::DriveOfPageFile(L"\\??\\") == 0, "drive: nothing is unknown");
    {
        const auto plenty = [](wchar_t) { return std::optional<std::uint64_t>(MB(500000)); };
        const auto none = [](wchar_t) { return std::optional<std::uint64_t>(); };
        using PF = LM::PageFile;
        Check(LM::GrowthRoom(std::vector<PF>{ { L'C', MB(4096), MB(4096) } }, plenty) == 0, "growth: a fixed page file");
        Check(LM::GrowthRoom(std::vector<PF>{ { L'C', MB(4096), MB(49152) } }, plenty) == MB(45056),
              "growth: a system-managed one grows to its maximum");
        Check(LM::GrowthRoom(std::vector<PF>{ { L'C', MB(4096), MB(49152) } },
                             [](wchar_t) { return std::optional<std::uint64_t>(MB(3000)); }) == MB(3000 - 1024),
              "growth: at most the disk's free space less the reserve");
        Check(LM::GrowthRoom(std::vector<PF>{ { L'C', MB(4096), MB(49152) } },
                             [](wchar_t) { return std::optional<std::uint64_t>(MB(900)); }) == 0,
              "growth: a disk under the reserve gives nothing");
        Check(LM::GrowthRoom(std::vector<PF>{ { L'C', MB(1000), MB(9000) }, { L'C', MB(1000), MB(9000) } },
                             [](wchar_t) { return std::optional<std::uint64_t>(MB(11024)); }) == MB(10000),
              "growth: two files on one disk share its free space");
        Check(LM::GrowthRoom(std::vector<PF>{ { L'C', MB(1000), MB(3000) }, { L'D', MB(1000), MB(2000) } }, plenty) ==
                  MB(3000),
              "growth: files on two disks add up");
        Check(LM::GrowthRoom(std::vector<PF>{ { 0, MB(1000), MB(9000) } }, plenty) == 0, "growth: an unknown volume is not counted");
        Check(LM::GrowthRoom(std::vector<PF>{ { L'C', MB(1000), MB(9000) } }, none) == 0,
              "growth: a disk whose free space is unknown is not counted");
        Check(LM::GrowthRoom(std::vector<PF>{ { L'C', MB(9000), MB(1000) } }, plenty) == 0,
              "growth: a file over its maximum gives nothing");
        Check(LM::GrowthRoom(std::vector<PF>{}, plenty) == 0, "growth: no page file");
    }
    Check(LM::NeedsGrowth(MB(100), 768) && LM::NeedsGrowth(MB(1151), 768) && !LM::NeedsGrowth(MB(1152), 768),
          "growth: asked only while the room is short of the lift");
    Check(!LM::NeedsGrowth(0, 0), "growth: never while the guard is off");
    Check(!LM::JobCapped(MB(500), MB(500)) && !LM::JobCapped(MB(500), MB(560)), "job: the same room, or within the tolerance");
    Check(LM::JobCapped(MB(500), MB(4000)), "job: a process room under the system's is a job's cap");

    std::printf("%s (%d failed)\n", g_fails ? "FAILED" : "ALL PASSED", g_fails);
    return g_fails ? 1 : 0;
}
