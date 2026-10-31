// The page thread's handoff (src/PageThreadCore.h, 1.7.0), on the desktop: whether the page thread may run, and how a
// page frame reaches the game thread. build.bat builds and runs this; exit 0 when every check passes.

#include "PageThreadCore.h"

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace Magelight::PageThread;
using Clock = std::chrono::steady_clock;

static int s_failed = 0;

static void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++s_failed;
}

static bool Same(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

static double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Packet {
    int serial = 0;
};

// One frame per Go, each costing 0..30 ms, as a page of uneven weight.
struct Producer {
    Handoff<Packet>* handoff = nullptr;
    HANDLE           stop = nullptr;
    bool             stuck = false;   // never comes back from its first frame
    int              made = 0;
};

static DWORD WINAPI ProducerMain(void* p)
{
    auto& pr = *static_cast<Producer*>(p);
    std::mt19937 rng(1187);
    while (pr.handoff->WaitGo(pr.stop, INFINITE)) {
        if (pr.stuck) WaitForSingleObject(pr.stop, INFINITE);   // a page in a script loop
        if (WaitForSingleObject(pr.stop, 0) == WAIT_OBJECT_0) break;
        Sleep(rng() % 31);
        auto packet = std::make_unique<Packet>();
        packet->serial = pr.made++;
        pr.handoff->Push(std::move(packet));
    }
    return 0;
}

// A fault inside a lock_guard on another thread, caught by an SEH guard further up the stack, as the page thread's
// GuardedPageFrame catches one: built /EHsc like the host, no destructor runs on the way out, so the lock stays held
// unless the guard gives it back. The lock_guard sits in its own function: __try may not share a frame with one.
static std::mutex  g_plain;
static OwnedMutex  g_owned;

__declspec(noinline) static void FaultHoldingPlain()
{
    std::lock_guard<std::mutex> lk(g_plain);
    *static_cast<volatile int*>(nullptr) = 1;
}

__declspec(noinline) static void FaultHoldingOwned()
{
    std::lock_guard<OwnedMutex> lk(g_owned);
    *static_cast<volatile int*>(nullptr) = 1;
}

struct FaultRun {
    void (*fn)();
    bool release;     // the guard gives back what this thread still holds
    bool faulted;
    bool released;
};

static void GuardedFault(FaultRun& r)
{
    __try {
        r.fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.faulted = true;
        if (r.release) r.released = g_owned.ReleaseIfHeldByThisThread();
    }
}

static DWORD WINAPI FaultThread(void* p)
{
    GuardedFault(*static_cast<FaultRun*>(p));
    return 0;
}

static void RunOnThread(FaultRun& r)
{
    HANDLE t = CreateThread(nullptr, 0, &FaultThread, &r, 0, nullptr);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static DWORD WINAPI HoldOwned(void* p)
{
    g_owned.lock();
    SetEvent(static_cast<HANDLE>(p));
    Sleep(200);
    g_owned.unlock();
    return 0;
}

int main()
{
    // ---- Decide -----------------------------------------------------------------------------------------------------
    Check(Decide(0, true) == nullptr, "decide: a free-threaded device with a deferred context: the thread runs");
    Check(Same(Decide(D3D11_CREATE_DEVICE_SINGLETHREADED, true), "the game's device is single-threaded"),
          "decide: a single-threaded device refuses");
    Check(Same(Decide(0, false), "the device refused a deferred context"), "decide: a refused deferred context refuses");
    Check(Same(Decide(D3D11_CREATE_DEVICE_SINGLETHREADED, false), "the game's device is single-threaded"),
          "decide: the device's threading is named before the deferred context");
    Check(Decide(D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG, true) == nullptr,
          "decide: other creation flags do not refuse");

    // ---- Main-menu pages ----------------------------------------------------------------
    Check(StartsAtMainMenu(true, true, true), "start: the page thread starts at the main menu for a main-menu page");
    Check(!StartsAtMainMenu(false, true, true), "start: not once it is refused or already running");
    Check(!StartsAtMainMenu(true, false, true), "start: not with no main-menu page");
    Check(!StartsAtMainMenu(true, true, false), "start: not once the main menu has closed");
    Check(MainMenuAskOpens(true, true, true, false), "ask: opens the page at the main menu");
    Check(!MainMenuAskOpens(false, true, true, false), "ask: not without the page thread");
    Check(!MainMenuAskOpens(true, false, true, false), "ask: not away from the main menu");
    Check(!MainMenuAskOpens(true, true, false, false), "ask: not before the main thread takes over Present");
    Check(!MainMenuAskOpens(true, true, true, true), "ask: not on a dead overlay");
    Check(ViewShowsNow(false, true, true, false), "view: a main-menu page shows at the first main menu");
    Check(ViewShowsNow(false, true, true, true), "view: and on a return to it");
    Check(ViewShowsNow(false, true, false, false), "view: and through the load");
    Check(ViewShowsNow(false, true, false, true), "view: and in the game");
    Check(!ViewShowsNow(false, false, true, false), "view: another page waits at the first main menu");
    Check(!ViewShowsNow(false, false, false, false), "view: and through the load, until the first in-game frame");
    Check(ViewShowsNow(false, false, false, true), "view: and shows in the game");
    Check(!ViewShowsNow(false, false, true, true), "view: and is not drawn over a return to the main menu");
    Check(ViewShowsNow(true, false, true, true), "view: without the page thread every view shows, as before");

    // ---- Handoff: a page of uneven weight ---------------------------------------------------------------------------
    {
        Handoff<Packet> handoff;
        Check(handoff.Valid(), "handoff: it has the event that paces the page thread");
        Producer pr;
        pr.handoff = &handoff;
        pr.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE t = StartThread(&ProducerMain, &pr, 8, L"pagethread_test producer");
        Check(t != nullptr, "start: the thread starts");
        std::vector<int> seen;
        std::size_t maxPending = 0;
        double worstTake = 0;
        for (int round = 0; round < 600; ++round) {
            maxPending = std::max(maxPending, handoff.Pending());
            const auto t0 = Clock::now();
            handoff.Go();
            auto got = handoff.TakeAll();
            worstTake = std::max(worstTake, MsSince(t0));
            for (auto& p : got) seen.push_back(p->serial);
            Sleep(16);
        }
        SetEvent(pr.stop);
        handoff.Go();   // wakes a producer waiting for its next frame
        WaitForSingleObject(t, INFINITE);
        for (auto& p : handoff.TakeAll()) seen.push_back(p->serial);
        bool inOrder = static_cast<int>(seen.size()) == pr.made;
        for (std::size_t i = 0; inOrder && i < seen.size(); ++i) inOrder = seen[i] == static_cast<int>(i);
        std::printf("      %d frames made, %zu taken, at most %zu waiting, the slowest Go+TakeAll %.3f ms\n", pr.made,
                    seen.size(), maxPending, worstTake);
        Check(pr.made > 100, "handoff: the page thread made frames");
        Check(inOrder, "handoff: every frame reaches the game thread exactly once, in order (none dropped)");
        Check(maxPending <= 2, "handoff: at most two frames ever wait");
        Check(worstTake < 1.0, "handoff: the game thread's Go and TakeAll never take a millisecond");
        CloseHandle(t);
        CloseHandle(pr.stop);
    }

    // ---- Handoff: a page that never comes back ---------------------------------------------------------------------
    {
        Handoff<Packet> handoff;
        Producer pr;
        pr.handoff = &handoff;
        pr.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pr.stuck = true;
        HANDLE t = StartThread(&ProducerMain, &pr, 8, L"pagethread_test stuck producer");
        double worst = 0;
        std::size_t taken = 0;
        for (int round = 0; round < 100; ++round) {
            const auto t0 = Clock::now();
            handoff.Go();
            taken += handoff.TakeAll().size();
            worst = std::max(worst, MsSince(t0));
            Sleep(5);
        }
        Check(worst < 1.0 && taken == 0, "handoff: a stuck page thread never holds the game thread up");
        SetEvent(pr.stop);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        CloseHandle(pr.stop);
    }

    // ---- WaitGo: the stop event and the timeout ----------------------------------------------------------------------
    {
        Handoff<Packet> handoff;
        HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(!handoff.WaitGo(stop, 0), "wait: no Go yet is a false after the timeout");
        handoff.Go();
        handoff.Go();
        Check(handoff.WaitGo(stop, 0), "wait: a Go wakes the page thread");
        Check(!handoff.WaitGo(stop, 0), "wait: two Gos before a wait are one frame, not two");
        handoff.Go();
        SetEvent(stop);
        Check(!handoff.WaitGo(stop, INFINITE), "wait: a set stop event wins over a pending Go");
        Check(handoff.WaitGo(nullptr, 0), "wait: with no stop event, the pending Go still wakes it");
        CloseHandle(stop);
    }

    // ---- A fault inside a held lock -----------------------------------------------------------------------------
    {
        FaultRun plain{ &FaultHoldingPlain, false, false, false };
        RunOnThread(plain);
        const bool stillHeld = !g_plain.try_lock();
        if (!stillHeld) g_plain.unlock();
        Check(plain.faulted && stillHeld,
              "fault: a plain mutex a faulting thread held stays held after its SEH guard (no destructor ran, /EHsc)");

        FaultRun owned{ &FaultHoldingOwned, true, false, false };
        RunOnThread(owned);
        const bool free = g_owned.try_lock();
        if (free) g_owned.unlock();
        Check(owned.faulted && owned.released && free,
              "fault: an OwnedMutex the faulting thread still held is given back by its guard");

        HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE t = CreateThread(nullptr, 0, &HoldOwned, held, 0, nullptr);
        WaitForSingleObject(held, INFINITE);
        const bool releasedOther = g_owned.ReleaseIfHeldByThisThread();
        const bool otherStillHolds = !g_owned.try_lock();
        if (!otherStillHolds) g_owned.unlock();
        Check(!releasedOther && otherStillHolds, "fault: another thread's hold is never released by mistake");
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        CloseHandle(held);
    }

    std::printf("%d check(s) failed\n", s_failed);
    return s_failed ? 1 : 0;
}
