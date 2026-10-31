#pragma once
// Runs a test tool's main the way Magelight runs its frame work (invariant 1) when the environment sets
// MG_PAGE_FIBER=1: a 1 MB thread holding back 64 KB of its stack (CrashLogger's stack guarantee) runs main on an 8 MB
// fiber with a 64 KB guarantee of its own, and BetweenFrames(), called from the tool's frame pump, takes the thread
// off the fiber once and back, holding the page VM's lock while it is off, as the host does (src/PageStackCore.h).
// With MG_PAGE_THREAD=1 instead (it wins when both are set), main runs as Magelight's page thread runs pages
// (pageThread, invariant 27): on a thread of its own with an 8 MB stack and a 64 KB guarantee, made by the process's
// main thread, which loaded the runtime and goes on with work of its own until main returns; BetweenFrames does
// nothing there. Without either, Run calls main directly and BetweenFrames does nothing. The tool's build puts src\
// on the include path.

#ifndef NOMINMAX
#define NOMINMAX   // a no-op when the tool included <windows.h> already; otherwise keeps std::min / std::max usable
#endif
#include <windows.h>
#include <malloc.h>

#include <cstdio>

#include "PageStackCore.h"

namespace FiberRun {

    namespace detail {
        inline LPVOID g_gameFiber = nullptr;
        inline LPVOID g_pageFiber = nullptr;
        inline DWORD  g_thread = 0;
        inline int (*g_main)(int, char**) = nullptr;
        inline int    g_argc = 0;
        inline char** g_argv = nullptr;
        inline int    g_result = 0;
        inline bool   g_done = false;
        inline long   g_leaves = 0;
        inline Magelight::PageStack::Exports g_exports;
        inline Magelight::PageStack::VmLock  g_lock;

        inline VOID CALLBACK PageFiber(LPVOID)
        {
            ULONG g = 64 * 1024;
            SetThreadStackGuarantee(&g);
            g_result = g_main(g_argc, g_argv);
            g_done = true;
            SwitchToFiber(g_gameFiber);
        }

        // The game's own work between frames, on its own stack.
        __declspec(noinline) inline void GameWork()
        {
            volatile char* p = static_cast<volatile char*>(_alloca(64 * 1024));
            p[0] = 1;
            p[64 * 1024 - 1] = 1;
        }

        inline DWORD WINAPI GameThread(LPVOID)
        {
            g_thread = GetCurrentThreadId();
            ULONG g = 64 * 1024;
            SetThreadStackGuarantee(&g);
            g_gameFiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
            g_pageFiber = CreateFiberEx(256 * 1024, static_cast<SIZE_T>(8) << 20, FIBER_FLAG_FLOAT_SWITCH, &PageFiber, nullptr);
            if (!g_gameFiber || !g_pageFiber) {
                std::printf("FiberRun: no fiber (%lu)\n", GetLastError());
                return 99;
            }
            while (!g_done) {
                SwitchToFiber(g_pageFiber);
                GameWork();
            }
            return static_cast<DWORD>(g_result);
        }

        inline DWORD WINAPI PageThread(LPVOID)
        {
            SetThreadDescription(GetCurrentThread(), L"FiberRun page thread");
            ULONG g = 64 * 1024;
            SetThreadStackGuarantee(&g);
            g_result = g_main(g_argc, g_argv);
            return static_cast<DWORD>(g_result);
        }

        inline bool EnvIs1(const char* name)
        {
            char b[8] = {};
            return GetEnvironmentVariableA(name, b, sizeof b) > 0 && b[0] == '1';
        }
    }

    inline bool ThreadEnabled() { return detail::EnvIs1("MG_PAGE_THREAD"); }
    inline bool Enabled() { return !ThreadEnabled() && detail::EnvIs1("MG_PAGE_FIBER"); }

    inline int Run(int argc, char** argv, int (*main)(int, char**))
    {
        if (ThreadEnabled()) {
            std::printf("FiberRun: main on a thread of its own (8 MB stack, 64 KB guarantee), as Magelight's page "
                        "thread (MG_PAGE_THREAD=1)\n");
            detail::g_main = main;
            detail::g_argc = argc;
            detail::g_argv = argv;
            HANDLE t = CreateThread(nullptr, static_cast<SIZE_T>(8) << 20, &detail::PageThread, nullptr,
                                    STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (!t) return 99;
            while (WaitForSingleObject(t, 1) == WAIT_TIMEOUT) detail::GameWork();   // the game goes on meanwhile
            DWORD code = 0;
            GetExitCodeThread(t, &code);
            CloseHandle(t);
            std::fflush(nullptr);
            // The tool's Ultralight objects belong to that thread: their static destructors must not run on this one.
            TerminateProcess(GetCurrentProcess(), code);
            return static_cast<int>(code);
        }
        if (!Enabled()) return main(argc, argv);
        std::printf("FiberRun: main on an 8 MB fiber of a 1 MB thread (64 KB guarantees), the page VM's lock held "
                    "whenever the thread is off it (MG_PAGE_FIBER=1)\n");
        detail::g_main = main;
        detail::g_argc = argc;
        detail::g_argv = argv;
        HANDLE t = CreateThread(nullptr, static_cast<SIZE_T>(1) << 20, &detail::GameThread, nullptr,
                                STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!t) return 99;
        WaitForSingleObject(t, INFINITE);
        DWORD code = 0;
        GetExitCodeThread(t, &code);
        CloseHandle(t);
        std::printf("FiberRun: the thread left the fiber %ld times, the VM lock %s\n", detail::g_leaves,
                    detail::g_lock.Held() ? "held" : "never taken (no VM)");
        std::fflush(nullptr);
        // The tool's Ultralight objects belong to that thread: their static destructors must not run on this one.
        TerminateProcess(GetCurrentProcess(), code);
        return static_cast<int>(code);
    }

    // From the tool's frame pump: on the page fiber, leave it once (holding the VM lock once the VM exists) and return.
    inline void BetweenFrames()
    {
        if (!detail::g_pageFiber || GetCurrentThreadId() != detail::g_thread || GetCurrentFiber() != detail::g_pageFiber)
            return;
        if (!detail::g_exports.LockReady()) {
            detail::g_exports = Magelight::PageStack::Resolve(GetModuleHandleW(L"WebCore.dll"));
            if (!detail::g_exports.LockReady()) {
                std::printf("FiberRun: WebCore does not export %s\n", detail::g_exports.missingLock);
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 98);
            }
        }
        detail::g_lock.Hold(detail::g_exports);
        ++detail::g_leaves;
        SwitchToFiber(detail::g_gameFiber);
    }

}
