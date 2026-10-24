#pragma once
// The IME's candidate list, drawn in the page: the host half. No OS IME window draws over the game, so the host
// hands each change to the page core's reserved '__ime' channel with the composition, its caret, its clauses and the
// candidate page (Payload), and the page core draws them (or the page does, having opted out). The candidate list comes
// from ImmGetCandidateList as a CANDIDATELIST byte buffer, reduced here to the page the IME shows
// (ReadCandidatePage). Engine-free and Windows-free (the buffer is read by its documented layout), so
// tools/ime-test runs it on the desktop.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "Magelight.h"   // JsonQuote

namespace Magelight::Ime {

    static_assert(sizeof(wchar_t) == 2, "IME strings are UTF-16: wchar_t as on Windows");

    // The most candidates a page is sent, and the longest one (UTF-16 units): an IME page is about nine.
    inline constexpr std::size_t kMaxCandidates = 32;
    inline constexpr std::size_t kMaxCandidateUnits = 256;

    // CANDIDATELIST (imm.h): dwSize, dwStyle, dwCount, dwSelection, dwPageStart, dwPageSize, then dwCount offsets
    // from the start of the list, each to a NUL-terminated UTF-16 string.
    inline constexpr std::size_t kHeaderBytes = 6 * sizeof(std::uint32_t);
    inline constexpr std::uint32_t kStyleCode = 4;   // IME_CAND_CODE

    struct ListHeader {
        std::uint32_t style = 0, count = 0, selection = 0, pageStart = 0, pageSize = 0;
    };

    inline std::uint32_t ReadU32(const unsigned char* p)
    {
        std::uint32_t v;
        std::memcpy(&v, p, sizeof v);
        return v;
    }

    // False when the buffer is too short for the header or for the offsets it declares.
    inline bool ReadHeader(const void* data, std::size_t bytes, ListHeader& out)
    {
        if (!data || bytes < kHeaderBytes) return false;
        const auto* b = static_cast<const unsigned char*>(data);
        out.style = ReadU32(b + 4);
        out.count = ReadU32(b + 8);
        out.selection = ReadU32(b + 12);
        out.pageStart = ReadU32(b + 16);
        out.pageSize = ReadU32(b + 20);
        return out.count <= (bytes - kHeaderBytes) / sizeof(std::uint32_t);
    }

    // A code-range list with one entry (IME_CAND_CODE, dwCount 1) holds a character code in dwOffset[0], no string.
    inline bool IsLoneCode(const ListHeader& h) { return h.style == kStyleCode && h.count == 1; }

    // Entry `index`, or "" when its offset or its string runs outside the buffer. Never reads past `bytes`.
    inline std::wstring CandidateAt(const void* data, std::size_t bytes, std::uint32_t index)
    {
        ListHeader h;
        if (!ReadHeader(data, bytes, h) || index >= h.count) return {};
        const auto* b = static_cast<const unsigned char*>(data);
        const std::uint32_t off = ReadU32(b + kHeaderBytes + index * sizeof(std::uint32_t));
        if (off < kHeaderBytes || off >= bytes) return {};
        std::wstring s;
        for (std::size_t at = off; at + 1 < bytes && s.size() < kMaxCandidateUnits; at += 2) {
            wchar_t c;
            std::memcpy(&c, b + at, sizeof c);
            if (c == 0) return s;
            s.push_back(c);
        }
        return {};   // no terminator inside the buffer, or longer than a candidate can be
    }

    struct CandidatePage {
        std::vector<std::wstring> items;
        int selected = -1;   // within items; -1 when the IME's selection is not on this page
    };

    // The page the IME shows: dwPageStart (0 when out of range), dwPageSize entries (all when 0), capped at
    // kMaxCandidates. An unreadable entry stays as "" so the selection keeps its place. A lone character code is
    // no page of strings: empty.
    inline CandidatePage ReadCandidatePage(const void* data, std::size_t bytes)
    {
        CandidatePage page;
        ListHeader h;
        if (!ReadHeader(data, bytes, h) || h.count == 0 || IsLoneCode(h)) return page;
        const std::uint32_t start = h.pageStart < h.count ? h.pageStart : 0;
        const std::uint32_t size = h.pageSize == 0 ? h.count : h.pageSize;
        const std::uint32_t end = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            {std::uint64_t(start) + size, std::uint64_t(h.count), std::uint64_t(start) + kMaxCandidates}));
        for (std::uint32_t i = start; i < end; ++i) page.items.push_back(CandidateAt(data, bytes, i));
        if (h.selection >= start && h.selection < end) page.selected = static_cast<int>(h.selection - start);
        return page;
    }

    // GCS_COMPATTR, one byte per UTF-16 unit of the composition (imm.h ATTR_*).
    inline constexpr unsigned char kAttrInput = 0, kAttrTargetConverted = 1, kAttrConverted = 2,
                                   kAttrTargetNotConverted = 3, kAttrInputError = 4, kAttrFixedConverted = 5;

    // Whether any of the composition is converted: a unit marked other than input.
    inline bool Converted(const std::vector<unsigned char>& attrs)
    {
        return std::any_of(attrs.begin(), attrs.end(),
                           [](unsigned char a) { return a != kAttrInput && a != kAttrInputError; });
    }

    // GCS_COMPCLAUSE: a DWORD array of clause boundaries in UTF-16 units, 0 first and the composition's length
    // last. Empty when the buffer is not one: a partial DWORD, a first entry other than 0, boundaries that do not
    // ascend, or a last one other than `compUnits`.
    inline std::vector<int> ReadClauses(const void* data, std::size_t bytes, std::size_t compUnits)
    {
        if (!data || bytes % sizeof(std::uint32_t) != 0) return {};
        const std::size_t n = bytes / sizeof(std::uint32_t);
        if (n < 2 || n > compUnits + 1) return {};
        const auto* b = static_cast<const unsigned char*>(data);
        std::vector<int> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t v = ReadU32(b + i * sizeof(std::uint32_t));
            if (i == 0 ? v != 0 : v <= static_cast<std::uint32_t>(out.back())) return {};
            out.push_back(static_cast<int>(v));
        }
        if (static_cast<std::size_t>(out.back()) != compUnits) return {};
        return out;
    }

    // The clause the IME converts next (Space, the arrows, Shift+arrows act on it): the first one marked target,
    // converted or not. -1 when none is, or when the attributes do not cover the clauses one unit each.
    inline int TargetClause(const std::vector<int>& clauses, const std::vector<unsigned char>& attrs)
    {
        if (clauses.size() < 2 || attrs.size() != static_cast<std::size_t>(clauses.back())) return -1;
        for (std::size_t i = 0; i + 1 < clauses.size(); ++i) {
            const unsigned char a = attrs[static_cast<std::size_t>(clauses[i])];
            if (a == kAttrTargetConverted || a == kAttrTargetNotConverted) return static_cast<int>(i);
        }
        return -1;
    }

    // The row to highlight: the one Space or Enter acts on, or none. The IME's own selection, except for the
    // Japanese IME while the reading is not converted yet: its list is then predictions and Space converts to its
    // own choice, which need not be on it (it reports its first prediction as selected), so nothing is highlighted.
    // Other IMEs' lists are what Space commits from (Microsoft Pinyin commits its selection, the Korean IME's Hanja
    // list picks it with Enter), converted or not.
    inline constexpr std::uint16_t kLangJapanese = 0x11;   // PRIMARYLANGID of the keyboard layout's language
    inline int SelectedRow(const CandidatePage& page, bool converted, std::uint16_t primaryLang)
    {
        return converted || primaryLang != kLangJapanese ? page.selected : -1;
    }

    // What the page draws. caret counts UTF-16 units into composition, as JS strings do.
    struct Snapshot {
        std::string composition;   // UTF-8
        int caret = 0;
        std::vector<std::string> candidates;   // UTF-8, the visible page
        int selectedIndex = -1;
        std::vector<int> clauses;   // ReadClauses: UTF-16 boundaries, empty when the IME reports none
        int target = -1;            // TargetClause: an index into the clauses, -1 for none
    };

    // The '__ime' channel's JSON: the state and its text (the committed string on 'commit', the composition on
    // 'compose'), then the composition, its caret, the candidate page and the selected index on it, the clause
    // boundaries and the target clause.
    inline std::string Payload(std::string_view state, const std::string& textUtf8, const Snapshot& snap)
    {
        std::string out = "{\"state\":" + JsonQuote(std::string(state)) + ",\"text\":" + JsonQuote(textUtf8);
        out += ",\"composition\":" + JsonQuote(snap.composition);
        out += ",\"caret\":" + std::to_string(std::max(0, snap.caret));
        out += ",\"candidates\":[";
        for (std::size_t i = 0; i < snap.candidates.size(); ++i) {
            if (i) out += ',';
            out += JsonQuote(snap.candidates[i]);
        }
        const int sel = snap.selectedIndex >= 0 && snap.selectedIndex < static_cast<int>(snap.candidates.size())
                            ? snap.selectedIndex : -1;
        out += "],\"selectedIndex\":" + std::to_string(sel) + ",\"clauses\":[";
        for (std::size_t i = 0; i < snap.clauses.size(); ++i) {
            if (i) out += ',';
            out += std::to_string(snap.clauses[i]);
        }
        const int target = snap.target >= 0 && snap.target + 1 < static_cast<int>(snap.clauses.size()) ? snap.target : -1;
        return out + "],\"target\":" + std::to_string(target) + "}";
    }
}
