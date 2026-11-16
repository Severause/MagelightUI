#pragma once
// Content hit testing (1.9.2, SetViewHitTest; CLAUDE.md invariant 31): a view in content mode takes the pointer only
// where its page has an element, so the empty parts of a fullscreen view let it through to the view below. The page
// answers (document.elementFromPoint, which leaves out pointer-events:none elements): pixels are on the GPU, and a
// transparent drag handle or a dim backdrop is the page's to decide.
//
// std only: tools/hittest-test runs all of it on the desktop.

#include <cstdint>
#include <string>
#include <string_view>

namespace Magelight::HitTest {

    // SetViewHitTest's modes (MAGELIGHT_API::kHitTestRect / kHitTestContent).
    inline constexpr std::uint32_t kRect = 0;      // the view's whole rectangle takes the pointer
    inline constexpr std::uint32_t kContent = 1;   // only where the page has an element other than <html> and <body>

    inline bool KnownMode(std::uint32_t mode) { return mode == kRect || mode == kContent; }

    // The manifest's "hitTest" value and the page's '__hittest' argument. False for any other text.
    inline bool ParseMode(std::string_view text, std::uint32_t& mode)
    {
        if (text == "rect") { mode = kRect; return true; }
        if (text == "content") { mode = kContent; return true; }
        return false;
    }

    // One view the pointer is over, topmost first.
    struct Candidate {
        bool content = false;   // in content mode
        bool ready = false;     // its page can be asked: it reached DOM ready
    };

    // Whether the page is asked. A page that cannot answer takes the pointer as a rectangle does, so a view never
    // lets clicks through before it has drawn anything.
    inline bool Asks(const Candidate& c) { return c.content && c.ready; }

    // The candidate the pointer goes to, or -1 when every one let it through (the caller's fallback, as outside
    // every view). hasElement(i) asks candidate i's page and is called only for one that Asks, each at most once,
    // and for none below the one that takes the pointer.
    template <class Probe>
    int Resolve(const Candidate* list, int count, Probe&& hasElement)
    {
        for (int i = 0; i < count; ++i)
            if (!Asks(list[i]) || hasElement(i)) return i;
        return -1;
    }

    // The cursor is picked on the game thread, which cannot ask a page: it skips a view whose last answer to the
    // mouse was "nothing here" (lastEmpty), the answer Resolve's caller keeps per view.
    inline bool CursorSkips(const Candidate& c, bool lastEmpty) { return Asks(c) && lastEmpty; }

    // Installed with the page core, once per window: '1' when the page has an element at (x, y) in viewport CSS px,
    // '0' when only <html>, <body> or nothing is there. An <iframe> is an element whatever its document shows. A
    // document still loading answers '1': the host's DOM-ready flag does not see a reload the page starts itself, and
    // a page with no <body> yet would otherwise let every click through. It keeps the document's own
    // elementFromPoint, so a page that replaces it runs nothing inside the host's question, and answers '1' when
    // the lookup throws.
    inline constexpr const char* kPageScript =
        "(function(){var F=Document.prototype.elementFromPoint;"
        "try{Object.defineProperty(window,'__mlHit',{value:function(x,y){var d=document,e;"
        "if(d.readyState==='loading')return '1';"
        "try{e=F.call(d,x,y);}catch(z){return '1';}"
        "return(!e||e===d.documentElement||e===d.body)?'0':'1';}});}catch(x){}})();";

    // The host's question for one point. A page without __mlHit (no page core) answers '1'.
    inline std::string CallScript(int x, int y)
    {
        return "window.__mlHit?window.__mlHit(" + std::to_string(x) + "," + std::to_string(y) + "):'1'";
    }

    // Whether the page's answer says nothing is there. Anything but '0' (a failed script included, whose result is
    // empty) is an element.
    inline bool Empty(std::string_view answer) { return answer == "0"; }

}  // namespace Magelight::HitTest
