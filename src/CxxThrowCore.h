#pragma once
// A thrown C++ exception, read from its SEH record inside an exception filter: the stack that threw is still there,
// which a catch can no longer see. Windows only, no allocation, so tools/pagethread-test runs it on the desktop.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>

namespace Magelight::CxxThrow {

    inline constexpr DWORD kCode = 0xE06D7363;   // MSVC's RaiseException code for a throw

    struct Info {
        char type[128] = {};   // "std::bad_alloc"; empty when the record cannot be read
        char what[256] = {};   // its what(), when it is a std::exception
    };

    namespace detail {
        // MSVC's x64 throw tables. Every int is an offset from the thrower's image base (ExceptionInformation[3]).
        struct TypeDescriptor { const void* vftable; void* spare; char name[1]; };
        struct CatchableType { unsigned properties; int type; int mdisp, pdisp, vdisp; int size; int copy; };
        struct CatchableTypeArray { int count; int types[1]; };
        struct ThrowInfo { unsigned attributes; int unwind; int forwardCompat; int catchable; };

        // ".?AVbad_alloc@std@@" -> "std::bad_alloc". A template's name stays decorated.
        inline void Readable(const char* decorated, char* out, std::size_t size)
        {
            out[0] = 0;
            if (std::strncmp(decorated, ".?A", 3) != 0 || !decorated[3]) {
                strncpy_s(out, size, decorated, _TRUNCATE);
                return;
            }
            const char* name = decorated + 4;
            const char* end = std::strstr(name, "@@");
            if (!end || std::strstr(name, "?$")) {
                strncpy_s(out, size, name, _TRUNCATE);
                return;
            }
            // The parts run innermost first, so the output is built from the last one back.
            std::size_t used = 0;
            for (const char* partEnd = end; partEnd > name;) {
                const char* partStart = partEnd;
                while (partStart > name && partStart[-1] != '@') --partStart;
                const auto n = static_cast<std::size_t>(partEnd - partStart);
                if (used + n + 3 > size) break;
                if (used) { out[used++] = ':'; out[used++] = ':'; }
                std::memcpy(out + used, partStart, n);
                used += n;
                out[used] = 0;
                partEnd = partStart > name ? partStart - 1 : name;
            }
        }

        inline void ReadUnguarded(const EXCEPTION_RECORD* rec, Info& out)
        {
            if (rec->ExceptionCode != kCode || rec->NumberParameters < 4 || !rec->ExceptionInformation[2]) return;
            const auto base = static_cast<std::uintptr_t>(rec->ExceptionInformation[3]);
            const auto* info = reinterpret_cast<const ThrowInfo*>(rec->ExceptionInformation[2]);
            const auto* list = reinterpret_cast<const CatchableTypeArray*>(base + info->catchable);
            for (int i = 0; i < list->count; ++i) {
                const auto* type = reinterpret_cast<const CatchableType*>(base + list->types[i]);
                const auto* desc = reinterpret_cast<const TypeDescriptor*>(base + type->type);
                if (i == 0) Readable(desc->name, out.type, sizeof(out.type));
                if (std::strcmp(desc->name, ".?AVexception@std@@") == 0 && type->pdisp == -1 && rec->ExceptionInformation[1]) {
                    const auto* e = reinterpret_cast<const std::exception*>(rec->ExceptionInformation[1] + type->mdisp);
                    if (const char* text = e->what()) strncpy_s(out.what, text, _TRUNCATE);   // null is a CRT fast-fail
                    return;
                }
            }
        }
    }

    // True when `rec` is a C++ throw. A table it cannot read leaves `out` as far as it got.
    inline bool Read(const EXCEPTION_RECORD* rec, Info& out) noexcept
    {
        if (!rec || rec->ExceptionCode != kCode) return false;
        __try {
            detail::ReadUnguarded(rec, out);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return true;
    }

    // Tells `note` of a C++ exception about to leave a callback, while the stack that threw is there. Any other
    // exception goes on to the guard above, which is what makes a fault still end the frame.
    using Note = void (*)(const char* what, std::uint64_t view, EXCEPTION_POINTERS* ep);
    inline int NoteThrow(Note note, const char* what, std::uint64_t view, EXCEPTION_POINTERS* ep)
    {
        if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == kCode) note(what, view, ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Separate from Call: try and __try cannot share a function. Not inlined, so the catch in Call cannot be optimized
    // away for a callee /EHsc takes as never throwing.
    template <class F>
    __declspec(noinline) void CallNoting(Note note, const char* what, std::uint64_t view, const F& f)
    {
        __try {
            f();
        } __except (NoteThrow(note, what, view, GetExceptionInformation())) {
        }
    }

    // Runs code another mod owns: a C++ exception it lets out stops here, reported through `note`.
    template <class F>
    void Call(Note note, const char* what, std::uint64_t view, const F& f)
    {
        try {
            CallNoting(note, what, view, f);
        } catch (...) {
        }
    }
}

namespace Magelight {
    // Magelight.cpp: logs the first few, with the stack that threw.
    void NoteModThrow(const char* what, std::uint64_t view, EXCEPTION_POINTERS* ep);

    // Mod code runs inside WebKit's frames and the frame guards. A C++ exception it lets out must end that call, not the
    // overlay, so every callback into a mod on those paths goes through here. A fault still reaches the guard.
    template <class F>
    void CallMod(const char* what, std::uint64_t view, const F& f)
    {
        CxxThrow::Call(&NoteModThrow, what, view, f);
    }
}
