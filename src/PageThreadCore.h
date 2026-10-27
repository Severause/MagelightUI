#pragma once
// The page thread (1.7.0, "pageThread"): whether it may run, and how a page frame reaches the game thread. With it on,
// a thread of Magelight's own owns Ultralight and records each frame's GPU work into a deferred context; the game
// thread, at Present, takes every finished frame (TakeAll), runs their command lists in order and composites the
// newest, and lets the page thread start the next one (Go). The page thread makes at most one frame per Go and the
// game thread takes them all, so none is dropped (each carries uploads later frames build on) and at most a couple
// ever wait. The game thread never waits on the page thread: TakeAll and Go hold only the queue's own lock, for a
// swap. Windows and the standard library only, so tools/pagethread-test runs it on the desktop.

#ifndef NOMINMAX
#define NOMINMAX   // a no-op when <windows.h> came first; otherwise keeps std::min / std::max usable
#endif
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Magelight::PageThread {

    // Magelight.json "pageThread": Off (the in-frame path), Wanted (asked for, not yet checked against the device),
    // On (running).
    enum class Mode : int { Off = 0, Wanted = 1, On = 2 };

    struct Refusal {
        bool        refused = false;
        const char* why = nullptr;   // for the log line; null when not refused
    };

    // Whether the page thread can run on this game and device. VR keeps the in-frame path (its presenter is untested off
    // the main thread); a single-threaded device may not be used from a second thread; a device (or a wrapper of one)
    // that refuses a deferred context leaves the driver nothing to record into. Named in that order.
    inline Refusal Decide(bool isVR, UINT deviceCreationFlags, bool deferredContextOk)
    {
        if (isVR) return { true, "Skyrim VR" };
        if (deviceCreationFlags & D3D11_CREATE_DEVICE_SINGLETHREADED) return { true, "the game's device is single-threaded" };
        if (!deferredContextOk) return { true, "the device refused a deferred context" };
        return {};
    }

    // A mutex that knows which thread holds it. The host is built /EHsc, so when the page thread's SEH guard catches a
    // fault no C++ destructor runs on the way out: a lock_guard inside the page work never unlocks, and the page
    // thread then parks holding it, where the next game-thread lock would wait forever (a hung game where the frame
    // path only lost the overlay). The guard gives back what its thread still holds (ReleaseIfHeldByThisThread).
    // Otherwise it is a std::mutex: lock_guard, unique_lock and try_to_lock all work on it.
    class OwnedMutex {
    public:
        void lock()
        {
            m_.lock();
            owner_.store(GetCurrentThreadId(), std::memory_order_relaxed);
        }
        bool try_lock()
        {
            if (!m_.try_lock()) return false;
            owner_.store(GetCurrentThreadId(), std::memory_order_relaxed);
            return true;
        }
        void unlock()
        {
            owner_.store(0, std::memory_order_relaxed);
            m_.unlock();
        }
        // After a fault: unlocks it when the calling thread holds it (only the holder ever reads its own id here).
        bool ReleaseIfHeldByThisThread()
        {
            if (owner_.load(std::memory_order_relaxed) != GetCurrentThreadId()) return false;
            unlock();
            return true;
        }

    private:
        std::mutex         m_;
        std::atomic<DWORD> owner_{ 0 };
    };

    // Finished page frames, from the page thread to the game thread. Made once and never destroyed by the host (its
    // packets may hold D3D references at process exit).
    template <class Packet>
    class Handoff {
    public:
        Handoff() : m_go(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
        ~Handoff()
        {
            if (m_go) CloseHandle(m_go);
        }
        Handoff(const Handoff&) = delete;
        Handoff& operator=(const Handoff&) = delete;

        // Page thread: a finished frame.
        void Push(std::unique_ptr<Packet> packet)
        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_packets.push_back(std::move(packet));
        }

        // Game thread: every finished frame, oldest first.
        std::vector<std::unique_ptr<Packet>> TakeAll()
        {
            std::vector<std::unique_ptr<Packet>> out;
            std::lock_guard<std::mutex> lk(m_mx);
            out.swap(m_packets);
            return out;
        }

        // Game thread: the page thread may make its next frame. Gos made before it wakes are one frame.
        void Go()
        {
            if (m_go) SetEvent(m_go);
        }

        // Page thread: true once a Go came; false when `stop` (may be null) is set first or `ms` passed. A set stop
        // wins over a pending Go, which stays pending.
        bool WaitGo(HANDLE stop, DWORD ms)
        {
            if (!m_go) return false;
            if (!stop) return WaitForSingleObject(m_go, ms) == WAIT_OBJECT_0;
            const HANDLE both[2] = { stop, m_go };
            return WaitForMultipleObjects(2, both, FALSE, ms) == WAIT_OBJECT_0 + 1;
        }

        std::size_t Pending() const
        {
            std::lock_guard<std::mutex> lk(m_mx);
            return m_packets.size();
        }

    private:
        HANDLE                               m_go;
        mutable std::mutex                   m_mx;
        std::vector<std::unique_ptr<Packet>> m_packets;
    };

    // A thread of Magelight's own: `stackMb` of reserved stack, named for crash logs and debuggers. The handle is the
    // caller's to keep; the host never joins it (the process ends it at exit).
    inline HANDLE StartThread(LPTHREAD_START_ROUTINE fn, void* arg, int stackMb, const wchar_t* name)
    {
        HANDLE t = CreateThread(nullptr, static_cast<SIZE_T>(stackMb) << 20, fn, arg, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                nullptr);
        if (t && name) SetThreadDescription(t, name);
        return t;
    }

}  // namespace Magelight::PageThread
