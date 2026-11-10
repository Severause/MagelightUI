// The low-memory guard (src/LowMemoryCore.h): what a page needs to open, the guard's floor, the lift's hysteresis and
// minimum time, when released views load again and its backoff, the page files' growth room, the setting's clamp.
// Exit 0 when every check passes.
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
    Check(LM::ResumeBytes(MB(768)) == MB(768 + 384), "resume: floor + half the floor");
    Check(LM::ResumeBytes(MB(256)) == MB(256 + 256), "resume: at least 256 MB above a small floor");
    Check(LM::Below(MB(767), MB(768)) && !LM::Below(MB(768), MB(768)), "below: strictly under the floor");
    Check(!LM::Below(0, 0), "below: never while off");

    // What a page needs.
    {
        constexpr std::uint64_t k1080 = 1920ull * 1080, k1440 = 2560ull * 1440, k4K = 3840ull * 2160;
        Check(LM::kDefaultFloorMb == 320, "need: the default floor");
        Check(LM::ShowNeedBytes(320, k1080) == MB(128), "need: showing a loaded 1080p page is two fifths of the floor");
        Check(LM::LoadNeedBytes(320, k1080) == MB(320), "need: loading a 1080p page is the floor");
        Check(LM::ShowNeedBytes(320, 800 * 600) == MB(128), "need: a small view counts as 1080p");
        Check(LM::ShowNeedBytes(320, 0) == MB(128), "need: an unknown size counts as 1080p");
        Check(LM::ShowNeedBytes(320, k4K) == MB(512), "need: a 4K view's target is four times a 1080p one");
        Check(LM::LoadNeedBytes(320, k4K) == MB(512 + 192), "need: the page itself does not grow with the view");
        Check(LM::ShowNeedBytes(320, k1440) > MB(227) && LM::ShowNeedBytes(320, k1440) < MB(228), "need: 1440p in between");
        Check(LM::ShowNeedBytes(320, k1080 * 100) == MB(128 * 16), "need: the pixel scale is capped");
        Check(LM::OpenNeedBytes(320, true, k1080) == MB(128) && LM::OpenNeedBytes(320, false, k1080) == MB(320),
              "need: an open shows a loaded page and loads any other");
        Check(LM::ShowNeedBytes(0, k4K) == 0 && LM::LoadNeedBytes(0, k4K) == 0, "need: nothing while off");
        Check(LM::OpenShort(MB(127), 320, true, k1080) && !LM::OpenShort(MB(128), 320, true, k1080),
              "open: a loaded page opens with exactly its need free");
        Check(LM::OpenShort(MB(319), 320, false, k1080) && !LM::OpenShort(MB(320), 320, false, k1080),
              "open: a page that must load needs the floor");
        Check(!LM::OpenShort(0, 0, false, k4K), "open: never short while off");

        // The guard's sample as a budget.
        Check(LM::LoadFits(MB(320), 320, k1080, 0) && !LM::LoadFits(MB(319), 320, k1080, 0),
              "budget: a shown view loads on exactly its need");
        Check(!LM::LoadFits(MB(639), 320, k1080, MB(320)) && LM::LoadFits(MB(640), 320, k1080, MB(320)),
              "budget: a background load leaves the guard's floor");
        Check(LM::LoadFits(0, 0, k4K, MB(320)), "budget: anything fits while off");
        Check(LM::AfterLoad(MB(1000), 320, k1080) == MB(680), "budget: a load takes its need out");
        Check(LM::AfterLoad(MB(100), 320, k1080) == 0, "budget: never under nothing");
        std::uint64_t budget = MB(700);
        int loads = 0;
        while (LM::LoadFits(budget, 320, k1080, 0)) {
            budget = LM::AfterLoad(budget, 320, k1080);
            ++loads;
        }
        Check(loads == 2, "budget: views shown together load only as far as one sample covers");
    }

    // The guard over time.
    {
        LM::Guard g;
        Check(g.Update(MB(4000), 1000, MB(768)) == LM::Change::None && !g.up, "guard: plenty of room stays down");
        Check(!g.ReloadDue(100000), "guard: never raised, no reload");
        Check(g.Update(MB(700), 1000, MB(768)) == LM::Change::Raised && g.up, "guard: under the floor raises it");
        Check(g.Update(MB(100), 2000, MB(768)) == LM::Change::None && g.up, "guard: stays up lower still");
        Check(g.Update(MB(900), 3000, MB(768)) == LM::Change::None && g.up, "guard: above the floor is not enough");
        Check(g.Update(MB(1152), 5000, MB(768)) == LM::Change::None && g.up, "guard: recovered, but up under 10 s");
        Check(g.Update(MB(1151), 11000, MB(768)) == LM::Change::None && g.up, "guard: 10 s, one MB short of the lift");
        Check(!g.ReloadDue(11000), "guard: up, no reload");
        Check(g.Update(MB(1152), 11000, MB(768)) == LM::Change::Lifted && !g.up, "guard: 10 s and the margin lift it");
        Check(!g.ReloadDue(11000 + LM::kReloadAfterLiftMs - 1), "reload: down, not long enough");
        Check(g.ReloadDue(11000 + LM::kReloadAfterLiftMs), "reload: down a minute");
        Check(g.Update(MB(900), 12000, MB(768)) == LM::Change::None && !g.up, "guard: above the floor after a lift stays down");
        Check(g.Update(MB(767), 14000, MB(768)) == LM::Change::Raised && g.upSinceMs == 14000, "guard: raised again, timer anew");
        Check(!g.ReloadDue(200000) && g.reloadStrikes == 0, "guard: raised again, no reload and no strike without one");
    }

    // The reload's backoff: the guard back up soon after a reload doubles the wait, up to the cap.
    {
        LM::Guard g;
        std::uint64_t t = 1000;
        g.Update(MB(100), t, MB(768));
        t += LM::kMinUpMs;
        g.Update(MB(2000), t, MB(768));                    // lifted
        t += LM::kReloadAfterLiftMs;
        Check(g.ReloadDue(t), "backoff: the first reload after a minute");
        g.NoteReload(t);
        t += 30000;
        Check(g.Update(MB(100), t, MB(768)) == LM::Change::Raised && g.reloadStrikes == 1, "backoff: up again soon is a strike");
        t += LM::kMinUpMs;
        g.Update(MB(2000), t, MB(768));
        Check(!g.ReloadDue(t + LM::kReloadAfterLiftMs) && g.ReloadDue(t + 2 * LM::kReloadAfterLiftMs),
              "backoff: one strike doubles the wait");
        t += 2 * LM::kReloadAfterLiftMs;
        g.NoteReload(t);
        t += LM::kReloadSettleMs;                      // the pages held this time
        Check(g.Update(MB(100), t, MB(768)) == LM::Change::Raised && g.reloadStrikes == 1, "backoff: up again later is no strike");
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
    Check(LM::NeedsGrowth(MB(100), MB(768)) && LM::NeedsGrowth(MB(1151), MB(768)) && !LM::NeedsGrowth(MB(1152), MB(768)),
          "growth: asked only while the room is short of the lift");
    Check(!LM::NeedsGrowth(0, 0), "growth: never while the guard is off");
    Check(!LM::JobCapped(MB(500), MB(500)) && !LM::JobCapped(MB(500), MB(560)), "job: the same room, or within the tolerance");
    Check(LM::JobCapped(MB(500), MB(4000)), "job: a process room under the system's is a job's cap");

    // The registry's page-file settings, and the files Windows manages.
    {
        const auto custom = LM::ParsePagingSetting(L"c:\\pagefile.sys 16 40000");
        Check(custom.drive == L'C' && !custom.anyDrive && !custom.systemManaged, "setting: a custom size");
        const auto fixed = LM::ParsePagingSetting(L"Z:\\pagefile.sys 40000 40000");
        Check(fixed.drive == L'Z' && !fixed.systemManaged, "setting: a fixed size");
        const auto managed = LM::ParsePagingSetting(L"C:\\pagefile.sys 0 0");
        Check(managed.drive == L'C' && managed.systemManaged, "setting: 0 0 is managed by Windows");
        const auto bare = LM::ParsePagingSetting(L"  d:\\pagefile.sys  ");
        Check(bare.drive == L'D' && bare.systemManaged, "setting: the path alone is managed by Windows");
        const auto any = LM::ParsePagingSetting(L"?:\\pagefile.sys");
        Check(any.anyDrive && any.drive == 0 && any.systemManaged, "setting: ?: is managed on a drive Windows picks");
        const auto junk = LM::ParsePagingSetting(L"pagefile.sys 0 0");
        Check(junk.drive == 0 && !junk.anyDrive && !junk.systemManaged, "setting: no drive, nothing claimed");
        Check(!LM::ParsePagingSetting(L"").systemManaged, "setting: an empty entry");
        Check(!LM::ParsePagingSetting(L"C:\\pagefile.sys 0 4096").systemManaged, "setting: a zero minimum with a maximum is custom");

        const std::uint64_t ram16 = MB(16384);
        Check(LM::SystemManagedMaximum(ram16, MB(1000000)) == MB(49152), "managed: three times the RAM");
        Check(LM::SystemManagedMaximum(MB(1024), MB(1000000)) == MB(4096), "managed: at least 4 GB");
        Check(LM::SystemManagedMaximum(ram16, MB(240000)) == MB(30000), "managed: at most an eighth of the volume");

        using PF = LM::PageFile;
        using PS = LM::PagingSetting;
        const auto bigVolume = [](wchar_t) { return std::optional<std::uint64_t>(MB(1000000)); };
        std::vector<PF> files{ { L'C', MB(4096), MB(4096) } };
        LM::RaiseSystemManaged(files, std::vector<PS>{ managed }, ram16, bigVolume);
        Check(files[0].maximumBytes == MB(49152), "managed: a file reported at its current size gets the documented maximum");
        files = { { L'C', MB(4096), MB(60000) } };
        LM::RaiseSystemManaged(files, std::vector<PS>{ managed }, ram16, bigVolume);
        Check(files[0].maximumBytes == MB(60000), "managed: a larger reported maximum stands");
        files = { { L'C', MB(4096), MB(4096) } };
        LM::RaiseSystemManaged(files, std::vector<PS>{ custom }, ram16, bigVolume);
        Check(files[0].maximumBytes == MB(4096), "managed: a custom size is left as read");
        files = { { L'D', MB(4096), MB(4096) } };
        LM::RaiseSystemManaged(files, std::vector<PS>{ managed }, ram16, bigVolume);
        Check(files[0].maximumBytes == MB(4096), "managed: a setting for another drive does not apply");
        files = { { L'D', MB(4096), MB(4096) } };
        LM::RaiseSystemManaged(files, std::vector<PS>{ any }, ram16, bigVolume);
        Check(files[0].maximumBytes == MB(49152), "managed: ?: applies to whichever drive holds the file");
        files = { { L'C', MB(4096), MB(4096) } };
        LM::RaiseSystemManaged(files, std::vector<PS>{ any, fixed, LM::ParsePagingSetting(L"C:\\pagefile.sys 4096 4096") }, ram16,
                               bigVolume);
        Check(files[0].maximumBytes == MB(4096), "managed: the drive's own setting wins over ?:");
        files = { { L'C', MB(4096), MB(4096) } };
        LM::RaiseSystemManaged(files, std::vector<PS>{ managed }, ram16, [](wchar_t) { return std::optional<std::uint64_t>(); });
        Check(files[0].maximumBytes == MB(4096), "managed: an unknown volume size leaves the file as read");
        files = { { 0, MB(4096), MB(4096) } };
        LM::RaiseSystemManaged(files, std::vector<PS>{ any }, ram16, bigVolume);
        Check(files[0].maximumBytes == MB(4096), "managed: a file on an unknown drive is left as read");
    }

    // Windows' own reading, as the log says it.
    {
        using CE = LM::CommitEvents;
        Check(std::string_view(LM::CommitEventsText(CE{})) == "unread", "events: unread");
        Check(std::string_view(LM::CommitEventsText(CE{ true, true, false, false })) == "low use", "events: low");
        Check(std::string_view(LM::CommitEventsText(CE{ true, false, false, false })) == "normal", "events: none set");
        Check(std::string_view(LM::CommitEventsText(CE{ true, false, true, false })).substr(0, 4) == "high", "events: high");
        Check(std::string_view(LM::CommitEventsText(CE{ true, false, true, true })).substr(0, 14) == "at the maximum",
              "events: the maximum wins over high");
    }

    std::printf("%s (%d failed)\n", g_fails ? "FAILED" : "ALL PASSED", g_fails);
    return g_fails ? 1 : 0;
}
