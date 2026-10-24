// The host half of the page-drawn IME (src/ImeCore.h), on the desktop: build.bat builds and runs this;
// exit 0 when every check passes.

#include "ImeCore.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Magelight::Ime;

static int s_failed = 0;

static void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++s_failed;
}

// A CANDIDATELIST laid out as ImmGetCandidateList fills it.
static std::vector<unsigned char> List(const std::vector<std::wstring>& items, std::uint32_t selection,
                                       std::uint32_t pageStart, std::uint32_t pageSize, std::uint32_t style = 1)
{
    const std::size_t head = kHeaderBytes + items.size() * sizeof(std::uint32_t);
    std::size_t size = head;
    for (const auto& s : items) size += (s.size() + 1) * sizeof(wchar_t);
    std::vector<unsigned char> b(size, 0);
    const std::uint32_t fields[6] = { static_cast<std::uint32_t>(size), style, static_cast<std::uint32_t>(items.size()),
                                      selection, pageStart, pageSize };
    std::memcpy(b.data(), fields, sizeof fields);
    std::size_t at = head;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto off = static_cast<std::uint32_t>(at);
        std::memcpy(b.data() + kHeaderBytes + i * sizeof(std::uint32_t), &off, sizeof off);
        std::memcpy(b.data() + at, items[i].data(), items[i].size() * sizeof(wchar_t));
        at += (items[i].size() + 1) * sizeof(wchar_t);
    }
    return b;
}

static void SetOffset(std::vector<unsigned char>& b, std::size_t index, std::uint32_t off)
{
    std::memcpy(b.data() + kHeaderBytes + index * sizeof(std::uint32_t), &off, sizeof off);
}

int main()
{
    const std::vector<std::wstring> tokyo = { L"東京", L"とうきょう", L"トウキョウ", L"東京都", L"凍狂" };

    {
        const auto b = List(tokyo, 2, 0, 0);
        const auto p = ReadCandidatePage(b.data(), b.size());
        Check(p.items.size() == 5 && p.items[0] == L"東京" && p.items[4] == L"凍狂", "page: a zero page size is the whole list");
        Check(p.selected == 2, "page: the selection is the IME's");
    }
    {
        const auto b = List(tokyo, 3, 2, 2);
        const auto p = ReadCandidatePage(b.data(), b.size());
        Check(p.items.size() == 2 && p.items[0] == L"トウキョウ" && p.items[1] == L"東京都", "page: start and size pick the visible page");
        Check(p.selected == 1, "page: the selection counts from the page start");
    }
    {
        const auto b = List(tokyo, 0, 3, 9);
        const auto p = ReadCandidatePage(b.data(), b.size());
        Check(p.items.size() == 2 && p.selected == -1, "page: the last page is short, and a selection off the page is -1");
    }
    {
        const auto b = List(tokyo, 1, 99, 2);
        const auto p = ReadCandidatePage(b.data(), b.size());
        Check(p.items.size() == 2 && p.items[0] == L"東京" && p.selected == 1, "page: an out-of-range start reads as 0");
    }
    {
        std::vector<std::wstring> many(100, L"x");
        const auto b = List(many, 0, 0, 0);
        Check(ReadCandidatePage(b.data(), b.size()).items.size() == kMaxCandidates, "page: capped at kMaxCandidates");
    }
    {
        auto b = List(tokyo, 2, 0, 0);
        SetOffset(b, 1, static_cast<std::uint32_t>(b.size() + 10));
        const auto p = ReadCandidatePage(b.data(), b.size());
        Check(p.items.size() == 5 && p.items[1].empty() && p.items[2] == L"トウキョウ" && p.selected == 2,
              "page: an offset past the buffer is an empty entry, and the selection keeps its place");
        SetOffset(b, 1, 4);
        Check(ReadCandidatePage(b.data(), b.size()).items[1].empty(), "page: an offset into the header is an empty entry");
    }
    {
        auto b = List({ L"abc" }, 0, 0, 0);
        b.resize(b.size() - 2);   // drop the terminator
        Check(CandidateAt(b.data(), b.size(), 0).empty(), "entry: a string with no terminator inside the buffer is empty");
        const std::wstring longOne(kMaxCandidateUnits + 1, L'a');
        const auto l = List({ longOne }, 0, 0, 0);
        Check(CandidateAt(l.data(), l.size(), 0).empty(), "entry: longer than a candidate can be is empty");
    }
    {
        auto b = List(tokyo, 0, 0, 0);
        const std::uint32_t lie = 1000;
        std::memcpy(b.data() + 8, &lie, sizeof lie);
        Check(ReadCandidatePage(b.data(), b.size()).items.empty(), "header: a count the buffer cannot hold reads nothing");
        Check(ReadCandidatePage(b.data(), 10).items.empty() && ReadCandidatePage(nullptr, 100).items.empty(),
              "header: a short or missing buffer reads nothing");
    }
    {
        // ときょ typed, not converted (Microsoft IME, field trace): predictions with the first reported selected, and
        // Space converts to と居, which is not on the list. Nothing is highlighted.
        const auto predictions = List({ L"ときょ", L"徒競走", L"都教委" }, 0, 0, 9);
        const auto page = ReadCandidatePage(predictions.data(), predictions.size());
        Check(page.items[0] == L"ときょ" && page.selected == 0, "page: predictions in the IME's order");
        const std::vector<unsigned char> input(3, kAttrInput);
        Check(!Converted(input) && SelectedRow(page, Converted(input), kLangJapanese) == -1,
              "highlight: none while the reading is unconverted (predictions)");
        const std::vector<unsigned char> withError = { kAttrInput, kAttrInputError };
        Check(!Converted(withError), "converted: an input error is still unconverted");
        Check(!Converted({}), "converted: no attributes reads as unconverted");
    }
    {
        // Space again opens the conversion list on ときょ (attrs 111), the IME's selection on row 1.
        const auto shown = List({ L"と", L"ときょ", L"都", L"十" }, 1, 0, 9);
        const auto page = ReadCandidatePage(shown.data(), shown.size());
        const std::vector<unsigned char> target(3, kAttrTargetConverted);
        Check(Converted(target) && SelectedRow(page, Converted(target), kLangJapanese) == 1, "highlight: the IME's selection once converted");
        // ↓: the list changes before the composition does; the highlight is the new selection at once.
        const auto moved = List({ L"と", L"ときょ", L"都", L"十" }, 2, 0, 9);
        Check(SelectedRow(ReadCandidatePage(moved.data(), moved.size()), Converted(target), kLangJapanese) == 2,
              "highlight: follows the selection ahead of the composition");
        // A sentence with the target clause after converted ones (attrs 2 then 1): still converted.
        const std::vector<unsigned char> sentence = { kAttrConverted, kAttrTargetConverted };
        Check(Converted(sentence), "converted: a target clause after a converted one");
        const auto off = List(tokyo, 3, 0, 2);
        Check(SelectedRow(ReadCandidatePage(off.data(), off.size()), true, kLangJapanese) == -1,
              "highlight: none when the IME's selection is on another page");
    }
    {
        auto code = List({ L"x" }, 0, 0, 0, kStyleCode);
        SetOffset(code, 0, 0x6771);   // a code-range list's dwOffset[0] is the character code itself
        Check(ReadCandidatePage(code.data(), code.size()).items.empty(), "page: a lone character code is no page");
        const auto twoCodes = List({ L"a", L"b" }, 1, 0, 0, kStyleCode);
        Check(ReadCandidatePage(twoCodes.data(), twoCodes.size()).items.size() == 2,
              "page: a code-style list of strings still reads");
    }

    {
        const Snapshot empty;
        Check(Payload("end", "", empty) ==
                  "{\"state\":\"end\",\"text\":\"\",\"composition\":\"\",\"caret\":0,\"candidates\":[],\"selectedIndex\":-1,\"clauses\":[],\"target\":-1}",
              "payload: the state and text first, and an empty snapshot clears the page");
        Check(Payload("commit", "\xE6\x9D\xB1\xE4\xBA\xAC", empty).rfind("{\"state\":\"commit\",\"text\":\"\xE6\x9D\xB1\xE4\xBA\xAC\",", 0) == 0,
              "payload: UTF-8 text passes through");
        Snapshot s;
        s.composition = "a\"b";
        s.caret = 2;
        s.candidates = { "x", "y\\z" };
        s.selectedIndex = 1;
        Check(Payload("candidates", "", s) ==
                  "{\"state\":\"candidates\",\"text\":\"\",\"composition\":\"a\\\"b\",\"caret\":2,"
                  "\"candidates\":[\"x\",\"y\\\\z\"],\"selectedIndex\":1,\"clauses\":[],\"target\":-1}",
              "payload: the composition, caret, candidates and selection, escaped");
        s.selectedIndex = 5;
        s.caret = -3;
        Check(Payload("compose", "", s).find("\"caret\":0,") != std::string::npos &&
                  Payload("compose", "", s).find("\"selectedIndex\":-1,") != std::string::npos,
              "payload: a negative caret is 0 and a selection off the list is -1");
    }

    {
        // GCS_COMPCLAUSE for きょうは|いい|てんき (9 units): 0, 4, 6, 9.
        const auto clauseBuf = [](const std::vector<std::uint32_t>& v) {
            std::vector<unsigned char> b(v.size() * sizeof(std::uint32_t));
            if (!v.empty()) std::memcpy(b.data(), v.data(), b.size());
            return b;
        };
        const auto b = clauseBuf({ 0, 4, 6, 9 });
        Check(ReadClauses(b.data(), b.size(), 9) == std::vector<int>({ 0, 4, 6, 9 }), "clauses: the boundaries as the IME gives them");
        const auto one = clauseBuf({ 0, 5 });
        Check(ReadClauses(one.data(), one.size(), 5) == std::vector<int>({ 0, 5 }), "clauses: an unconverted reading is one clause");
        const auto late = clauseBuf({ 1, 5 });
        Check(ReadClauses(late.data(), late.size(), 5).empty(), "clauses: none when the first is not 0");
        const auto back = clauseBuf({ 0, 4, 3, 9 });
        Check(ReadClauses(back.data(), back.size(), 9).empty(), "clauses: none when they do not ascend");
        Check(ReadClauses(b.data(), b.size(), 8).empty(), "clauses: none when the last is not the composition's end");
        Check(ReadClauses(b.data(), b.size() - 1, 9).empty(), "clauses: none from a partial DWORD");
        Check(ReadClauses(nullptr, 0, 0).empty() && ReadClauses(b.data(), 4, 9).empty(), "clauses: none from nothing or one entry");

        const std::vector<int> three = { 0, 4, 6, 9 };
        const std::vector<unsigned char> firstTarget = { 1, 1, 1, 1, 2, 2, 2, 2, 2 };
        Check(TargetClause(three, firstTarget) == 0, "target: the first clause after Space");
        const std::vector<unsigned char> secondTarget = { 2, 2, 2, 2, 1, 1, 2, 2, 2 };
        Check(TargetClause(three, secondTarget) == 1, "target: Right moved it to the second clause");
        const std::vector<unsigned char> notConverted = { 2, 2, 2, 2, 3, 3, 0, 0, 0 };
        Check(TargetClause(three, notConverted) == 1, "target: a target clause not converted yet");
        Check(TargetClause({ 0, 5 }, std::vector<unsigned char>(5, kAttrInput)) == -1, "target: none while unconverted");
        Check(TargetClause(three, std::vector<unsigned char>(8, kAttrTargetConverted)) == -1,
              "target: none when the attributes do not cover the composition");
        Check(TargetClause({}, {}) == -1, "target: none without clauses");

        // Microsoft Pinyin (0x04 Chinese): its first row is what Space commits, unconverted as the reading is.
        const auto pinyin = List({ L"你好", L"拟好", L"你" }, 0, 0, 9);
        const auto pinyinPage = ReadCandidatePage(pinyin.data(), pinyin.size());
        Check(SelectedRow(pinyinPage, false, 0x04) == 0, "highlight: Pinyin's selection before converting");
        Check(SelectedRow(pinyinPage, false, 0x12) == 0, "highlight: the Korean Hanja list's selection");
        Check(SelectedRow(pinyinPage, false, kLangJapanese) == -1, "highlight: the Japanese IME's predictions before converting");

        Snapshot s;
        s.composition = "x";
        s.clauses = three;
        s.target = 1;
        Check(Payload("compose", "", s).find(",\"clauses\":[0,4,6,9],\"target\":1}") != std::string::npos,
              "payload: the clauses and the target clause last");
        s.target = 3;
        Check(Payload("compose", "", s).find(",\"target\":-1}") != std::string::npos, "payload: a target past the clauses is -1");
    }

    std::printf("%s\n", s_failed ? "FAILED" : "all passed");
    return s_failed ? 1 : 0;
}
