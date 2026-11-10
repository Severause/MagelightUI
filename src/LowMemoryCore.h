#pragma once
// The low-memory guard (1.8.0; the sites it gates: CLAUDE.md invariant 28). Free commit is the room left under Windows'
// commit limit (MEMORYSTATUSEX::ullAvailPageFile), shared by every process, plus what the page files can still grow by
// (1.8.1, GrowthRoom). When it runs out, the next allocation inside Ultralight throws through Ultralight's own frames
// and the overlay turns itself off for the session: nobody can say what such a throw left half done in closed-source
// code, so the host keeps the throw from happening instead of carrying on after one.
//
// Two questions, asked apart (1.8.1):
//   - May this page open now? One reading against what that page needs (OpenShort): less for a page already loaded,
//     more on a larger view. No hysteresis: the moment the memory is there, the page opens.
//   - Should the host give memory back? The Guard, raised under the largest load any view needs and lifted with
//     hysteresis. While it is up, hidden views are released and background loads wait. Its floor is never under
//     an open's need, so when an open is short the host is already giving back what it holds.
//
// std only: tools/lowmemory-test runs all of it on the desktop.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Magelight::LowMemory {

    inline constexpr std::uint64_t kMiB = 1024ull * 1024ull;
    // Magelight.json "lowMemoryFloorMB": the free commit a page needs before it loads on a 1920x1080 view; 0 turns the
    // guard off. A 1080p field log measured 46 to 93 MB to show a loaded page and 200 to 246 MB to load one and show
    // it, and the allocation failed with 100 MB free.
    inline constexpr int kDefaultFloorMb = 320;
    inline constexpr int kMaxFloorMb = 16384;
    // The lift: free commit back above the guard's floor by half of it (at least 256 MB), with the guard up this long
    // at least. The game's own use swings by hundreds of MB, so a guard that lifted at its floor would come and go.
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

    // ── What a page needs (1.8.1) ──
    // Two fifths of the floor is what showing a loaded page takes: its target and its layers' surfaces, which grow
    // with the view's pixel count (never counted under 1920x1080, and at most kMaxPixelScale times it). The other
    // three fifths is the page itself (script, DOM, decoded images), which does not.
    inline constexpr std::uint64_t kBasePixels = 1920ull * 1080ull;
    inline constexpr std::uint64_t kMaxPixelScale = 8;

    inline std::uint64_t ShowNeedBytes(int floorMb, std::uint64_t pixels)
    {
        if (floorMb <= 0) return 0;
        const std::uint64_t px = std::clamp(pixels, kBasePixels, kBasePixels * kMaxPixelScale);
        return static_cast<std::uint64_t>(floorMb) * kMiB * 2 / 5 * px / kBasePixels;
    }

    inline std::uint64_t LoadNeedBytes(int floorMb, std::uint64_t pixels)
    {
        if (floorMb <= 0) return 0;
        return ShowNeedBytes(floorMb, pixels) + static_cast<std::uint64_t>(floorMb) * kMiB * 3 / 5;
    }

    // What opening a view of this many pixels needs: to show it when its page is loaded, else to load it too.
    inline std::uint64_t OpenNeedBytes(int floorMb, bool loaded, std::uint64_t pixels)
    {
        return loaded ? ShowNeedBytes(floorMb, pixels) : LoadNeedBytes(floorMb, pixels);
    }

    // One reading is short of what the open needs (never while the guard is off).
    inline bool OpenShort(std::uint64_t freeBytes, int floorMb, bool loaded, std::uint64_t pixels)
    {
        return floorMb > 0 && freeBytes < OpenNeedBytes(floorMb, loaded, pixels);
    }

    // ── The guard's own floor, in bytes: LoadNeedBytes of the largest view (0 = off) ──
    inline std::uint64_t ResumeBytes(std::uint64_t floorBytes)
    {
        return floorBytes + std::max<std::uint64_t>(floorBytes / 2, kMinResumeMarginMb * kMiB);
    }

    // One reading under the floor (never while the guard is off).
    inline bool Below(std::uint64_t freeBytes, std::uint64_t floorBytes)
    {
        return floorBytes > 0 && freeBytes < floorBytes;
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

    // One entry of the registry's PagingFiles list (Session Manager\Memory Management): "C:\pagefile.sys 16 40000" is a
    // custom size in MB; "C:\pagefile.sys 0 0", or the path alone, a size Windows manages on that drive; "?:\pagefile.sys"
    // one Windows manages on a drive it picks.
    struct PagingSetting {
        wchar_t drive = 0;            // upper case; 0 with anyDrive, or for an entry that names none
        bool anyDrive = false;
        bool systemManaged = false;
    };

    inline PagingSetting ParsePagingSetting(std::wstring_view entry)
    {
        PagingSetting s;
        const auto skip = [&entry]() {
            while (!entry.empty() && (entry.front() == L' ' || entry.front() == L'\t')) entry.remove_prefix(1);
        };
        const auto token = [&entry, &skip]() {
            skip();
            std::size_t n = 0;
            while (n < entry.size() && entry[n] != L' ' && entry[n] != L'\t') ++n;
            const std::wstring_view t = entry.substr(0, n);
            entry.remove_prefix(n);
            return t;
        };
        const std::wstring_view path = token();
        if (path.size() < 2 || path[1] != L':') return s;
        if (path[0] == L'?') s.anyDrive = true;
        else if ((s.drive = DriveOfPageFile(path)) == 0) return s;
        const auto zero = [](std::wstring_view t) {
            return !t.empty() && std::all_of(t.begin(), t.end(), [](wchar_t c) { return c == L'0'; });
        };
        const std::wstring_view minimum = token(), maximum = token();
        s.systemManaged = s.anyDrive || minimum.empty() || (zero(minimum) && (maximum.empty() || zero(maximum)));
        return s;
    }

    // The size Windows lets a page file it manages grow to, as Microsoft documents it: three times the RAM or 4 GB,
    // whichever is larger, and at most an eighth of its volume.
    inline std::uint64_t SystemManagedMaximum(std::uint64_t ramBytes, std::uint64_t volumeBytes)
    {
        return std::min(std::max(ramBytes * 3, 4096 * kMiB), volumeBytes / 8);
    }

    // Each page file the settings say Windows manages gets at least the documented maximum: what
    // NtQuerySystemInformation reports as the maximum of such a file is not documented. A setting naming the file's
    // drive wins over a "?:" one. volumeSize(drive) is a volume's size in bytes, or nullopt (the file is left as read).
    template <class VolumeSize>
    inline void RaiseSystemManaged(std::vector<PageFile>& files, const std::vector<PagingSetting>& settings,
                                   std::uint64_t ramBytes, VolumeSize&& volumeSize)
    {
        for (PageFile& f : files) {
            if (!f.drive) continue;
            const PagingSetting* match = nullptr;
            for (const PagingSetting& s : settings) {
                if (s.drive == f.drive) { match = &s; break; }
                if (s.anyDrive && !match) match = &s;
            }
            if (!match || !match->systemManaged) continue;
            const std::optional<std::uint64_t> volume = volumeSize(f.drive);
            if (!volume) continue;
            f.maximumBytes = std::max(f.maximumBytes, SystemManagedMaximum(ramBytes, *volume));
        }
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
    inline bool NeedsGrowth(std::uint64_t roomBytes, std::uint64_t floorBytes)
    {
        return floorBytes > 0 && roomBytes < ResumeBytes(floorBytes);
    }

    // The game's own room (ullAvailPageFile) is capped by a job object's commit limit when one holds the process, and
    // growing a page file does not raise that cap: then the system's room (commit limit less commit charge) is larger,
    // and no growth counts. The two are read moments apart, hence the tolerance.
    inline constexpr std::uint64_t kJobToleranceMb = 64;
    inline bool JobCapped(std::uint64_t processRoomBytes, std::uint64_t systemRoomBytes)
    {
        return systemRoomBytes > processRoomBytes + kJobToleranceMb * kMiB;
    }

    // ── Windows' own reading, for the log only ──
    // The memory manager's commit events (\KernelObjects\LowCommitCondition, HighCommitCondition,
    // MaximumCommitCondition). Their thresholds are not documented, so nothing is decided on them.
    struct CommitEvents {
        bool known = false;
        bool low = false, high = false, maximum = false;
    };

    inline const char* CommitEventsText(const CommitEvents& e)
    {
        if (!e.known) return "unread";
        if (e.maximum) return "at its maximum (the page files cannot grow)";
        if (e.high) return "high (the page files may still grow)";
        return e.low ? "low" : "normal";
    }

    enum class Change { None, Raised, Lifted };

    // The guard's state over the samples, on one thread (the host's: Ultralight's thread).
    struct Guard {
        bool up = false;
        std::uint64_t upSinceMs = 0;
        std::uint64_t downSinceMs = 0;    // the last lift; 0 = never raised
        std::uint64_t reloadedAtMs = 0;   // the last reload of released views (NoteReload); 0 = none since a raise
        int reloadStrikes = 0;            // reloads the guard came back up soon after

        Change Update(std::uint64_t freeBytes, std::uint64_t nowMs, std::uint64_t floorBytes)
        {
            if (!up) {
                if (!Below(freeBytes, floorBytes)) return Change::None;
                if (reloadedAtMs && nowMs - reloadedAtMs < kReloadSettleMs) ++reloadStrikes;
                reloadedAtMs = 0;
                up = true;
                upSinceMs = nowMs;
                return Change::Raised;
            }
            if (freeBytes < ResumeBytes(floorBytes) || nowMs - upSinceMs < kMinUpMs) return Change::None;
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
