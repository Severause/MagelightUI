#pragma once
// The low-memory guard (1.8.0; the sites it gates: CLAUDE.md invariant 28). Free commit is the room left under Windows'
// commit limit (MEMORYSTATUSEX::ullAvailPageFile), shared by every process, plus what the page files can still grow by
// (1.8.1, GrowthRoom): Windows grows a page file only when commit reaches the limit its current size sets, so on a
// system-managed page file the room alone sits low for long stretches while Windows would still give memory. When
// free commit runs out, the next allocation
// inside Ultralight throws through Ultralight's own frames and the overlay turns itself off for the session: nobody
// can say what such a throw left half done in closed-source code, so the host keeps the throw from happening instead of
// carrying on after one. Below a floor it shows or loads nothing new, and releases hidden views; it lifts with
// hysteresis.
//
// std only: tools/lowmemory-test runs all of it on the desktop.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Magelight::LowMemory {

    inline constexpr std::uint64_t kMiB = 1024ull * 1024ull;
    inline constexpr int kDefaultFloorMb = 768;   // Magelight.json "lowMemoryFloorMB"; 0 turns the guard off
    inline constexpr int kMaxFloorMb = 16384;
    // The lift: free commit back above the floor by half the floor (at least 256 MB), with the guard up this long at
    // least. The game's own use swings by hundreds of MB, so a guard that lifted at the floor would come and go.
    inline constexpr std::uint64_t kMinUpMs = 10000;
    inline constexpr std::uint64_t kMinResumeMarginMb = 256;
    // How often the host reads free commit (GlobalMemoryStatusEx, a few microseconds).
    inline constexpr std::uint64_t kSampleMs = 1000;
    // A view hidden this recently is not released yet: a page a mod hides and shows again at once keeps its View.
    inline constexpr std::uint64_t kReleaseHiddenAfterMs = 2000;
    // A released view that never hibernates loads again, hidden, once the guard has been down this long. Each time the
    // guard comes back up within kReloadSettleMs of such a reload (the pages did not fit), the wait doubles, to at most
    // 2^kMaxReloadDoublings times, so a game that cannot hold them does not release and reload them in a loop.
    inline constexpr std::uint64_t kReloadAfterLiftMs = 60000;
    inline constexpr std::uint64_t kReloadSettleMs = 300000;
    inline constexpr int kMaxReloadDoublings = 6;

    // Magelight.json's number, in MB: 0 (off) to kMaxFloorMb.
    inline int ClampFloorMb(double mb)
    {
        if (!(mb > 0.0)) return 0;   // NaN and negatives too
        return static_cast<int>(std::min(mb, static_cast<double>(kMaxFloorMb)));
    }

    inline std::uint64_t ResumeBytes(int floorMb)
    {
        const std::uint64_t floor = static_cast<std::uint64_t>(std::max(floorMb, 0));
        return (floor + std::max<std::uint64_t>(floor / 2, kMinResumeMarginMb)) * kMiB;
    }

    // One reading under the floor (never while the guard is off).
    inline bool Below(std::uint64_t freeBytes, int floorMb)
    {
        return floorMb > 0 && freeBytes < static_cast<std::uint64_t>(floorMb) * kMiB;
    }

    // ── What the page files can still grow by (1.8.1) ──
    struct PageFile {
        wchar_t drive = 0;                 // the volume it is on, an upper-case drive letter; 0 = unknown, not counted
        std::uint64_t currentBytes = 0;    // its size now
        std::uint64_t maximumBytes = 0;    // the size Windows may grow it to (equal to the size for a fixed one)
    };
    // A page file does not grow into the last of its disk: that is left for everything else that writes there.
    inline constexpr std::uint64_t kDiskReserveMb = 1024;
    // How long one reading of the page files and their disks is kept.
    inline constexpr std::uint64_t kGrowthCacheMs = 5000;

    // The drive letter of a page file as Windows names it ("\??\C:\pagefile.sys", or "C:\pagefile.sys"), upper
    // case; 0 for any other form.
    inline wchar_t DriveOfPageFile(std::wstring_view name)
    {
        if (name.size() >= 4 && name.substr(0, 4) == L"\\??\\") name.remove_prefix(4);
        if (name.size() < 2 || name[1] != L':') return 0;
        const wchar_t d = name[0] >= L'a' && name[0] <= L'z' ? static_cast<wchar_t>(name[0] - L'a' + L'A') : name[0];
        return d >= L'A' && d <= L'Z' ? d : 0;
    }

    // The commit the page files can still add: on each volume, the room under its files' maximum sizes, at most that
    // volume's free space less kDiskReserveMb. diskFree(drive) is a volume's free bytes, or nullopt (not counted).
    template <class DiskFree>
    inline std::uint64_t GrowthRoom(const std::vector<PageFile>& files, DiskFree&& diskFree)
    {
        std::uint64_t total = 0;
        for (wchar_t d = L'A'; d <= L'Z'; ++d) {
            std::uint64_t room = 0;
            for (const PageFile& f : files)
                if (f.drive == d && f.maximumBytes > f.currentBytes) room += f.maximumBytes - f.currentBytes;
            if (!room) continue;
            const std::optional<std::uint64_t> free = diskFree(d);
            if (!free) continue;
            const std::uint64_t reserve = kDiskReserveMb * kMiB;
            total += std::min(room, *free > reserve ? *free - reserve : 0);
        }
        return total;
    }

    // Whether a reading needs the page files asked too: only while the room under the commit limit alone is short of
    // what lifts the guard, so a system with room to spare never asks.
    inline bool NeedsGrowth(std::uint64_t roomBytes, int floorMb)
    {
        return floorMb > 0 && roomBytes < ResumeBytes(floorMb);
    }

    // The game's own room (ullAvailPageFile) is capped by a job object's commit limit when one holds the process, and
    // growing a page file does not raise that cap: then the system's room (commit limit less commit charge) is larger,
    // and no growth counts. The two are read moments apart, hence the tolerance.
    inline constexpr std::uint64_t kJobToleranceMb = 64;
    inline bool JobCapped(std::uint64_t processRoomBytes, std::uint64_t systemRoomBytes)
    {
        return systemRoomBytes > processRoomBytes + kJobToleranceMb * kMiB;
    }

    enum class Change { None, Raised, Lifted };

    // The guard's state over the samples, on one thread (the host's: Ultralight's thread).
    struct Guard {
        bool up = false;
        std::uint64_t upSinceMs = 0;
        std::uint64_t downSinceMs = 0;    // the last lift; 0 = never raised
        std::uint64_t reloadedAtMs = 0;   // the last reload of released views (NoteReload); 0 = none since a raise
        int reloadStrikes = 0;            // reloads the guard came back up soon after

        Change Update(std::uint64_t freeBytes, std::uint64_t nowMs, int floorMb)
        {
            if (!up) {
                if (!Below(freeBytes, floorMb)) return Change::None;
                if (reloadedAtMs && nowMs - reloadedAtMs < kReloadSettleMs) ++reloadStrikes;
                reloadedAtMs = 0;
                up = true;
                upSinceMs = nowMs;
                return Change::Raised;
            }
            if (freeBytes < ResumeBytes(floorMb) || nowMs - upSinceMs < kMinUpMs) return Change::None;
            up = false;
            downSinceMs = nowMs;
            return Change::Lifted;
        }

        std::uint64_t ReloadDelayMs() const { return kReloadAfterLiftMs << std::min(reloadStrikes, kMaxReloadDoublings); }

        // Released views may load again: down since a lift for ReloadDelayMs.
        bool ReloadDue(std::uint64_t nowMs) const
        {
            return !up && downSinceMs != 0 && nowMs - downSinceMs >= ReloadDelayMs();
        }

        void NoteReload(std::uint64_t nowMs) { reloadedAtMs = nowMs; }
    };
}
