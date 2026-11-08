#pragma once
// The low-memory guard (1.8.0; the sites it gates: CLAUDE.md invariant 28). Free commit is the room left under Windows'
// commit limit (MEMORYSTATUSEX::ullAvailPageFile), shared by every process. When it runs out, the next allocation
// inside Ultralight throws through Ultralight's own frames and the overlay turns itself off for the session: nobody
// can say what such a throw left half done in closed-source code, so the host keeps the throw from happening instead of
// carrying on after one. Below a floor it shows or loads nothing new, and releases hidden views; it lifts with
// hysteresis.
//
// std only: tools/lowmemory-test runs all of it on the desktop.

#include <algorithm>
#include <cstdint>

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
