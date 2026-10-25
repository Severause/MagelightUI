#pragma once
// What keeps a page error from ending the overlay on Ultralight 2.0 (invariant 1). Shared by the host,
// tools/fiber-gc-test and tools/stack-test. No Ultralight header: everything here goes through WebCore exports by the
// MSVC names below, which the SDK does not document and may rename. Re-check every one at each SDK update by running
// tools/fiber-gc-test.
//
// On the page fiber (ulFiberMB > 0): WebKit takes the stack the thread is on when its first page work runs (the
// fiber's) as the thread's stack, and the collector's helper threads scan the thread with those bounds. With the
// thread back on the game's stack between frames, that scan reads past the fiber (an access violation or a ud2 in a
// Heap Helper thread). VmLock held whenever the thread is off the fiber prevents it: the holder has heap access, so a
// collection waits for the thread to stop at a safepoint, and the thread reaches one only on the fiber.
//
// On the game's stack (ulFiberMB 0, or VmLock's exports missing): JavaScriptCore stops scripts short of the end of
// the stack by its reserved zones, but its error path (any error a page leaves uncaught, any script at the recursion
// limit) commits the stack down to the hard zone first, and a stack guarantee there (CrashLogger 1.23+ sets 64 KB)
// turns that into 0xC00000FD. ReserveFor sizes the zones past the guarantee.

#include <windows.h>

#include <cstddef>
#include <cstdio>

namespace Magelight::PageStack {

    inline constexpr const char* kLockHolderCtor = "??0JSLockHolder@JSC@@QEAA@PEAVVM@1@@Z";   // JSLockHolder(VM*)
    inline constexpr const char* kLockHolderDtor = "??1JSLockHolder@JSC@@QEAA@XZ";
    inline constexpr const char* kCommonVm       = "?g_commonVMOrNull@WebCore@@3PEAVVM@JSC@@EA";   // VM* (data)
    inline constexpr const char* kOptionsInit    = "?initialize@Options@JSC@@SAXXZ";
    inline constexpr const char* kSetOption      = "?setOption@Options@JSC@@SA_NPEBD_N@Z";   // (const char*, bool)

    struct Exports {
        using HolderCtorFn = void* (*)(void* self, void* vm);
        using HolderDtorFn = void (*)(void* self);
        using OptionsInitFn = void (*)();
        using SetOptionFn = bool (*)(const char* nameEqualsValue, bool verify);

        HolderCtorFn  holderCtor = nullptr;
        HolderDtorFn  holderDtor = nullptr;
        void* const*  commonVm = nullptr;      // WebCore's one VM for every page and frame; null until it is made
        OptionsInitFn optionsInit = nullptr;
        SetOptionFn   setOption = nullptr;
        const char*   missingLock = nullptr;      // the first name VmLock needs that the module does not export
        const char*   missingOptions = nullptr;   // the same for ApplyReserve

        bool LockReady() const noexcept { return holderCtor && holderDtor && commonVm; }
        bool OptionsReady() const noexcept { return optionsInit && setOption; }
    };

    inline Exports Resolve(HMODULE webcore) noexcept
    {
        Exports e;
        if (!webcore) {
            e.missingLock = e.missingOptions = "(no WebCore module)";
            return e;
        }
        const auto find = [webcore](const char* name, const char*& missing) {
            FARPROC p = GetProcAddress(webcore, name);
            if (!p && !missing) missing = name;
            return p;
        };
        e.holderCtor  = reinterpret_cast<Exports::HolderCtorFn>(find(kLockHolderCtor, e.missingLock));
        e.holderDtor  = reinterpret_cast<Exports::HolderDtorFn>(find(kLockHolderDtor, e.missingLock));
        e.commonVm    = reinterpret_cast<void* const*>(find(kCommonVm, e.missingLock));
        e.optionsInit = reinterpret_cast<Exports::OptionsInitFn>(find(kOptionsInit, e.missingOptions));
        e.setOption   = reinterpret_cast<Exports::SetOptionFn>(find(kSetOption, e.missingOptions));
        return e;
    }

    // The page VM's lock (a JSC::JSLockHolder) in raw storage: C++ never constructs or destroys it, so a static
    // VmLock runs no runtime import at load or at process exit (invariant 3). One thread owns it. The lock is
    // recursive, so the pages' own locking inside a frame stacks on it.
    class VmLock {
    public:
        // Takes the lock once WebCore's VM exists; true while held. Taking it from no lock records the caller's stack
        // pointer as where scripts entered the VM (their depth is counted from there), so call it on the page stack,
        // near its top. Already held on the same VM: no call into WebCore at all.
        bool Hold(const Exports& e) noexcept
        {
            if (!e.LockReady()) return false;
            void* const vm = *e.commonVm;
            if (!vm) return false;
            if (m_held && m_vm == vm) return true;
            if (m_held) Release(e);   // WebCore made a new VM: the old one's lock protects nothing any more
            e.holderCtor(m_holder, vm);
            m_vm = vm;
            m_held = true;
            return true;
        }

        void Release(const Exports& e) noexcept
        {
            if (!m_held) return;
            e.holderDtor(m_holder);
            m_held = false;
            m_vm = nullptr;
        }

        bool  Held() const noexcept { return m_held; }
        void* Vm() const noexcept { return m_vm; }

    private:
        alignas(16) unsigned char m_holder[64] = {};   // JSLockHolder is one RefPtr<VM>
        void* m_vm = nullptr;
        bool  m_held = false;
    };

    // JavaScriptCore's stack zones for pages on a stack that holds `guarantee` bytes back. Its error path commits the
    // stack down to the hard zone, which clears the guarantee by 64 KB; scripts stop at the soft zone, 64 KB above
    // that; and a script gets at most 384 KB of stack from where it entered the VM: room for one
    // Function.prototype.apply over 32768 arguments (about 261 KB; pages turn binary data into text in such
    // chunks) from a page's handler, while the rest of the stack stays for the engine's own work (layout from a script
    // at its limit) and the frames of the game and the plugins below the pages. More starves that layout: 448 KB
    // already overflows it at 0 KB used under a 256 KB guarantee (tools/stack-test's stack_probe).
    struct Reserve {
        std::size_t guarantee = 0;   // as measured, rounded up to whole pages
        std::size_t hard = 0;        // JSC reservedZoneSize
        std::size_t soft = 0;        // JSC softReservedZoneSize
        std::size_t maxUsage = 0;    // JSC maxPerThreadStackUsage
    };

    constexpr Reserve ReserveFor(std::size_t guarantee) noexcept
    {
        Reserve r;
        r.guarantee = (guarantee + 4095) & ~static_cast<std::size_t>(4095);
        r.hard = r.guarantee + 64 * 1024;
        r.soft = r.hard + 64 * 1024;
        r.maxUsage = r.soft + 384 * 1024;
        return r;
    }

    // Sets the zones through JSC::Options. Call before Renderer::Create: the VM reads them when it is made, and
    // Ultralight 2.0 reads no JSC_ environment variable. True when JSC took every one; otherwise the first it refused
    // is written into `refused`.
    inline bool ApplyReserve(const Exports& e, const Reserve& r, char* refused, std::size_t refusedSize) noexcept
    {
        if (refused && refusedSize) refused[0] = '\0';
        if (!e.OptionsReady()) return false;
        e.optionsInit();
        // Soft before hard: each is at least JSC's default, and soft must stay above hard at every step.
        const struct { const char* name; std::size_t value; } opts[] = {
            { "softReservedZoneSize", r.soft },
            { "reservedZoneSize", r.hard },
            { "maxPerThreadStackUsage", r.maxUsage },
        };
        bool all = true;
        for (const auto& o : opts) {
            char arg[96];
            std::snprintf(arg, sizeof arg, "%s=%zu", o.name, o.value);
            if (!e.setOption(arg, true)) {
                if (all && refused && refusedSize) std::snprintf(refused, refusedSize, "%s", arg);
                all = false;
            }
        }
        return all;
    }

}
