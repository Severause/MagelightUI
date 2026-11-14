// Content hit testing (src/HitTestCore.h): which of the views under the pointer takes it, which pages are asked, the
// cursor's rule, the mode names and the page's answer. Exit 0 when every check passes.
#include "HitTestCore.h"

#include <cstdio>
#include <vector>

namespace HT = Magelight::HitTest;

namespace
{
    int g_fails = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++g_fails;
    }

    HT::Candidate Rect() { return {}; }
    HT::Candidate Content(bool ready = true)
    {
        HT::Candidate c;
        c.content = true;
        c.ready = ready;
        return c;
    }

    // Resolve over `list`, the page of candidate i answering has[i]; `asked` gets the candidates asked, in order.
    int Run(const std::vector<HT::Candidate>& list, const std::vector<bool>& has, std::vector<int>& asked)
    {
        asked.clear();
        return HT::Resolve(list.data(), static_cast<int>(list.size()), [&](int i) {
            asked.push_back(i);
            return static_cast<bool>(has[static_cast<std::size_t>(i)]);
        });
    }
}

int main()
{
    std::vector<int> asked;

    // The walk.
    Check(Run({ Rect() }, { false }, asked) == 0 && asked.empty(), "walk: a rectangle view takes the pointer unasked");
    Check(Run({ Content() }, { true }, asked) == 0 && asked == std::vector<int>{ 0 }, "walk: a content view with an element there");
    Check(Run({ Content() }, { false }, asked) == -1 && asked == std::vector<int>{ 0 }, "walk: a content view with nothing there lets it through");
    Check(Run({ Content(), Rect() }, { false, false }, asked) == 1 && asked == std::vector<int>{ 0 },
          "walk: through an empty content view to the rectangle view below");
    Check(Run({ Content(), Content() }, { false, true }, asked) == 1 && asked == (std::vector<int>{ 0, 1 }),
          "walk: through one content view to an element of the next");
    Check(Run({ Content(), Content() }, { false, false }, asked) == -1 && asked == (std::vector<int>{ 0, 1 }),
          "walk: every view empty is no view");
    Check(Run({ Content(), Content(), Rect() }, { true, true, true }, asked) == 0 && asked == std::vector<int>{ 0 },
          "walk: nothing below the view that takes it is asked");
    Check(Run({ Rect(), Content() }, { false, true }, asked) == 0 && asked.empty(), "walk: a rectangle view on top hides the views below");
    Check(Run({ Content(false) }, { false }, asked) == 0 && asked.empty(), "walk: a page that is not ready takes the pointer unasked");
    Check(Run({ Content(false), Content() }, { false, true }, asked) == 0 && asked.empty(),
          "walk: a page that is not ready hides the views below");
    Check(Run({}, {}, asked) == -1 && asked.empty(), "walk: no view under the pointer");

    // Who is asked.
    Check(!HT::Asks(Rect()) && HT::Asks(Content()) && !HT::Asks(Content(false)), "asks: only a ready content view");
    {
        HT::Candidate readyRect;
        readyRect.ready = true;
        Check(!HT::Asks(readyRect), "asks: a ready rectangle view is not asked");
    }

    // The cursor.
    Check(HT::CursorSkips(Content(), true), "cursor: skips a content view whose last answer was empty");
    Check(!HT::CursorSkips(Content(), false), "cursor: stays on a content view whose last answer was an element");
    Check(!HT::CursorSkips(Rect(), true), "cursor: never skips a rectangle view");
    Check(!HT::CursorSkips(Content(false), true), "cursor: never skips a page that is not ready");

    // Modes.
    std::uint32_t mode = 99;
    Check(HT::ParseMode("content", mode) && mode == HT::kContent, "mode: content");
    Check(HT::ParseMode("rect", mode) && mode == HT::kRect, "mode: rect");
    mode = 99;
    Check(!HT::ParseMode("Content", mode) && !HT::ParseMode("", mode) && !HT::ParseMode("1", mode) && mode == 99,
          "mode: any other text is refused and changes nothing");
    Check(HT::KnownMode(HT::kRect) && HT::KnownMode(HT::kContent) && !HT::KnownMode(2) && !HT::KnownMode(0x80000000u),
          "mode: the known values");

    // The page's answer.
    Check(HT::Empty("0"), "answer: '0' is nothing there");
    Check(!HT::Empty("1") && !HT::Empty("") && !HT::Empty("undefined") && !HT::Empty("00"), "answer: anything else is an element");
    Check(HT::CallScript(12, 345) == "window.__mlHit?window.__mlHit(12,345):'1'", "question: the point as written");
    Check(HT::CallScript(-3, 0) == "window.__mlHit?window.__mlHit(-3,0):'1'", "question: a point left of the page");

    std::printf("\n%s (%d failed)\n", g_fails ? "FAILED" : "all passed", g_fails);
    return g_fails ? 1 : 0;
}
