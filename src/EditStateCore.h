#pragma once
// Text focus and typed text: the host half of the VR keyboards and the IME on Ultralight 2.0. The EditorListener
// reports each View's text focus (EditableState); the host collects those reports during a frame and decides once,
// after the input (Tracker), whether the keyboard rises, hides or stays and whether the IME is attached. Typed text
// from every VR keyboard goes through SplitTyped: printable runs become Editor::InsertText, while Backspace, Enter
// and Tab stay key events, because pages act on their keydown. Ultralight headers only, so tools/editor-test runs
// all of it on the desktop against real EditableState reports.

#include <Ultralight/Editor.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Magelight::EditState {

    using ultralight::EditableState;
    using ultralight::EditableType;
    using ultralight::EnterKeyHint;
    using ultralight::InputMode;

    // The virtual keys SplitTyped turns control characters into (VK_BACK, VK_TAB, VK_RETURN).
    inline constexpr unsigned kVkBack = 0x08;
    inline constexpr unsigned kVkTab = 0x09;
    inline constexpr unsigned kVkReturn = 0x0D;

    // An on-screen keyboard is wanted while the view has focus, the focused element takes typed text, and the page
    // has not asked for none (inputmode="none": the page draws its own keypad).
    inline bool WantsKeyboard(const EditableState& s)
    {
        return s.view_has_focus && s.is_mutable && s.input_mode != InputMode::None;
    }

    // The OS IME is attached under the same focus rule, except in a password field or a number field (numberField:
    // kNumberFieldScript's answer), as a browser leaves them; inputmode does not matter to it.
    inline bool WantsIme(const EditableState& s, bool numberField = false)
    {
        return s.view_has_focus && s.is_mutable && s.type != EditableType::Password && !numberField;
    }

    // Whether the focused field may be an <input type=number>: Ultralight reports one as a single-line Text field
    // with no input mode of its own, so only the page can tell (kNumberFieldScript).
    inline bool MayBeNumberField(const EditableState& s)
    {
        return s.view_has_focus && s.is_mutable && s.type == EditableType::Text;
    }

    // Run in the page: "1" when the focused element, followed through same-origin iframes, is an <input type=number>,
    // else "0". Self-contained (a page may replace window.magelight) and never throws.
    inline constexpr const char* kNumberFieldScript =
        "(function(){try{var e=document.activeElement;"
        "for(var i=0;e&&i<16&&String(e.tagName).toUpperCase()==='IFRAME';i++){var d=e.contentDocument;if(!d)break;e=d.activeElement;}"
        "return e&&String(e.tagName).toUpperCase()==='INPUT'&&String(e.type).toLowerCase()==='number'?'1':'0';}"
        "catch(x){return '0';}})()";

    // The same element, as far as bounds can tell (reports carry no element identity). Half a CSS px of slack.
    inline bool SameRect(const ultralight::Rect& a, const ultralight::Rect& b)
    {
        auto within = [](float x, float y) { return (x > y ? x - y : y - x) <= 0.5f; };
        return within(a.left, b.left) && within(a.top, b.top) && within(a.right, b.right) && within(a.bottom, b.bottom);
    }

    // A point (viewport CSS px) on a non-empty rect.
    inline bool Inside(const ultralight::Rect& r, float x, float y)
    {
        return r.right > r.left && r.bottom > r.top && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }

    // Where the focused field is now. A report's bounds are where the field was when it took the focus, and no
    // report follows when it moves (a scroll, a modal sliding in). A single-line field's caret moves only with it
    // vertically, so its bounds follow the caret's vertical move since the report (Editor::GetCaretBounds then and
    // now). Any other field, or an empty caret rect, keeps the reported bounds.
    inline ultralight::Rect FieldNow(const EditableState& s, const ultralight::Rect& caretAtReport,
                                     const ultralight::Rect& caretNow)
    {
        ultralight::Rect r = s.bounds;
        if (s.type != EditableType::Text && s.type != EditableType::Password) return r;
        if (!(caretAtReport.bottom > caretAtReport.top) || !(caretNow.bottom > caretNow.top)) return r;
        const float dy = caretNow.top - caretAtReport.top;
        r.top += dy;
        r.bottom += dy;
        return r;
    }

    // The field the keyboard page lays itself out for, or "null" when no keyboard is wanted. Every value is one of
    // these fixed literals: the page is shared by every mod and never sees a page's text (invariant 20). A number
    // field with no inputmode gets the decimal keypad (its digits, '-' and '.').
    inline std::string FieldJson(const EditableState& s, bool numberField = false)
    {
        if (!WantsKeyboard(s)) return "null";
        const char* mode = numberField ? "decimal" : "text";
        switch (s.input_mode) {
        case InputMode::Numeric:   mode = "numeric"; break;
        case InputMode::Decimal:   mode = "decimal"; break;
        case InputMode::Telephone: mode = "tel"; break;
        case InputMode::Email:     mode = "email"; break;
        case InputMode::Url:       mode = "url"; break;
        case InputMode::Search:    mode = "search"; break;
        default: break;
        }
        const char* enter = "";
        switch (s.enter_key_hint) {
        case EnterKeyHint::Enter:    enter = "enter"; break;
        case EnterKeyHint::Done:     enter = "done"; break;
        case EnterKeyHint::Go:       enter = "go"; break;
        case EnterKeyHint::Next:     enter = "next"; break;
        case EnterKeyHint::Previous: enter = "previous"; break;
        case EnterKeyHint::Search:   enter = "search"; break;
        case EnterKeyHint::Send:     enter = "send"; break;
        default: break;
        }
        const bool multiline = s.type == EditableType::TextArea || s.type == EditableType::ContentEditable;
        const bool secret = s.type == EditableType::Password;
        return std::string("{\"mode\":\"") + mode + "\",\"enter\":\"" + enter + "\",\"multiline\":" +
               (multiline ? "true" : "false") + ",\"secret\":" + (secret ? "true" : "false") + "}";
    }

    // The keyboard page's edit buttons ({"cmd":name}); false for any other name.
    inline bool CommandByName(std::string_view name, ultralight::EditorCommand& out)
    {
        using C = ultralight::EditorCommand;
        if (name == "undo") out = C::Undo;
        else if (name == "redo") out = C::Redo;
        else if (name == "selectAll") out = C::SelectAll;
        else if (name == "cut") out = C::Cut;
        else if (name == "copy") out = C::Copy;
        else if (name == "paste") out = C::Paste;
        else return false;
        return true;
    }

    // The name CommandByName takes, for the log; "?" for a command no keyboard button runs.
    inline const char* CommandName(ultralight::EditorCommand c)
    {
        using C = ultralight::EditorCommand;
        switch (c) {
        case C::Undo:      return "undo";
        case C::Redo:      return "redo";
        case C::SelectAll: return "selectAll";
        case C::Cut:       return "cut";
        case C::Copy:      return "copy";
        case C::Paste:     return "paste";
        default:           return "?";
        }
    }

    // Typed text, in order: a run of printable characters (one InsertText), or a key (vk != 0) for Backspace (\b),
    // Enter (\r, \n, or CR LF as one) and Tab (\t). Other control characters and DEL are dropped. A surrogate half
    // with no partner in the string is dropped: inserted alone it would be U+FFFD (PairUnit pairs across calls).
    struct Piece {
        std::wstring text;
        unsigned vk = 0;
    };
    inline std::vector<Piece> SplitTyped(const wchar_t* s, std::size_t n)
    {
        std::vector<Piece> out;
        auto run = [&]() -> std::wstring& {
            if (out.empty() || out.back().vk) out.push_back({});
            return out.back().text;
        };
        auto key = [&](unsigned vk) { out.push_back({ {}, vk }); };
        for (std::size_t i = 0; i < n; ++i) {
            const wchar_t c = s[i];
            if (c >= 0xD800 && c <= 0xDBFF) {
                if (i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                    std::wstring& r = run();
                    r += c;
                    r += s[++i];
                }
                continue;
            }
            if (c >= 0xDC00 && c <= 0xDFFF) continue;
            if (c == L'\b') key(kVkBack);
            else if (c == L'\t') key(kVkTab);
            else if (c == L'\r' || c == L'\n') {
                if (c == L'\r' && i + 1 < n && s[i + 1] == L'\n') ++i;
                key(kVkReturn);
            } else if (c >= 0x20 && c != 0x7F) {
                run() += c;
            }
        }
        return out;
    }

    // For a source that delivers one UTF-16 unit at a time: holds a high surrogate in `held` until its low half
    // arrives, and returns how many units of `out` to pass on (0, 1 or 2). A low half with no high, or a high
    // followed by anything else, is dropped.
    inline std::size_t PairUnit(wchar_t& held, wchar_t unit, wchar_t (&out)[2])
    {
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            held = unit;
            return 0;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (!held) return 0;
            out[0] = held;
            out[1] = unit;
            held = 0;
            return 2;
        }
        held = 0;
        out[0] = unit;
        return 1;
    }

    // One report the listener recorded, resolved to its view: the view's state before it (the previous report, or
    // what a load reset left) and the report. hostFocus: it fired inside the host's own View::Focus or Unfocus.
    struct Change {
        std::uint64_t view = 0;
        EditableState prev{};
        EditableState now{};
        bool hostFocus = false;
    };

    // Which main-frame window object is a new document's, where the host drops the replaced page's text focus (a page
    // that replaces its own document reports no loss of it); the host keeps one, under its leaf lock. Once a View has
    // had a window object, each later document's comes as it begins, before its scripts and its reports. A View's first
    // one comes only when a script first runs there, which can follow the page's autofocus report (a deferred or
    // end-of-body script) or come after DOM ready (the host's first script), so it is never a new document's: the host
    // reset the View when it made it, and resets it when its own Navigate or Reload begins a load in a View that has
    // had none (HasWindow).
    class DocLoads {
    public:
        // OnBeginLoading let a main-frame load through.
        void Begun(const void* view)
        {
            if (std::find(loads_.begin(), loads_.end(), view) == loads_.end()) loads_.push_back(view);
        }
        // OnWindowObjectReady, main frame: true when it is the window object of a document a load brought into a View
        // that had one before, so the page before it is gone.
        bool WindowReady(const void* view)
        {
            const bool begun = std::erase(loads_, view) > 0;
            const bool seen = std::find(windows_.begin(), windows_.end(), view) != windows_.end();
            if (!seen) windows_.push_back(view);
            return begun && seen;
        }
        // OnDOMReady or OnFailLoading, main frame (a failed load keeps the document).
        void Ended(const void* view) { std::erase(loads_, view); }
        // The View has had a main-frame window object, so the next document a load brings is told by WindowReady.
        bool HasWindow(const void* view) const
        {
            return std::find(windows_.begin(), windows_.end(), view) != windows_.end();
        }
        // The View is released: all of it is forgotten, since a new View can get its address.
        void Released(const void* view)
        {
            std::erase(loads_, view);
            std::erase(windows_, view);
        }

    private:
        std::vector<const void*> loads_;     // a main-frame load began, its DOM ready not yet come
        std::vector<const void*> windows_;   // the View has had a main-frame window object
    };

    // A left press on the UI-mode view, at its viewport CSS px.
    struct Click {
        float x = 0.f, y = 0.f;
    };

    enum class Keyboard : std::uint8_t { Keep, Hide, Raise };

    struct Verdict {
        bool touched = false;         // something the keyboard and IME follow changed: act on the rest
        bool targetChanged = false;   // UI mode moved to another view, began or ended
        bool ime = false;             // the IME is attached (flat), when touched
        Keyboard keyboard = Keyboard::Keep;
    };

    // The frame's decision. Raise when focus moves to an editable field inside the UI-mode page that has focus
    // (a click, Tab, the page's own focus()), or when the player clicks inside the field that already has it, where
    // it is now (that click fires no report, and is how a keyboard the player closed comes back; a click elsewhere,
    // on a control that keeps the focus with mousedown preventDefault, does not). Never raise for the host's own
    // View::Focus (UI-mode entry, a switch, a late load) or for a new UI-mode view. Hide whenever the UI-mode page
    // stops wanting a keyboard, and when UI mode begins, ends or moves without a raise. Otherwise keep: an inputmode
    // change on the same field relays out the keyboard without raising one the player closed.
    class Tracker {
    public:
        // Nothing to decide: no report, click or reset, and the same UI-mode view as last time.
        bool Idle(std::uint64_t ui, bool changes, bool clicks, bool resync) const
        {
            return !changes && !clicks && !resync && ui == lastUi_;
        }

        // ui: the UI-mode view (0 when UI mode is off); changes: this frame's reports for every view, in order;
        // uiState: the UI-mode view's state after them; clicks: this frame's left presses on the UI-mode view;
        // resync: a load or release reset a view's state; field: where the focused field is now (FieldNow), when
        // known, else its reported bounds; numberField: the UI-mode view's field is an <input type=number>.
        Verdict Step(std::uint64_t ui, const std::vector<Change>& changes, const EditableState& uiState,
                     const std::vector<Click>& clicks, bool resync, const ultralight::Rect* field = nullptr,
                     bool numberField = false)
        {
            Verdict v;
            v.targetChanged = ui != lastUi_;
            lastUi_ = ui;
            v.touched = resync || v.targetChanged;
            bool raise = false;
            for (const Change& c : changes) {
                if (!ui || c.view != ui) continue;
                v.touched = true;
                if (c.hostFocus || !WantsKeyboard(c.now)) continue;
                if (WantsKeyboard(c.prev) && SameRect(c.prev.bounds, c.now.bounds)) continue;   // the same field
                raise = true;
            }
            if (ui && WantsKeyboard(uiState)) {
                const ultralight::Rect& r = field ? *field : uiState.bounds;
                for (const Click& k : clicks) {
                    if (!Inside(r, k.x, k.y)) continue;
                    raise = true;
                    v.touched = true;
                    break;
                }
            }
            if (!v.touched) return v;
            v.ime = ui && WantsIme(uiState, numberField);
            const bool want = ui && WantsKeyboard(uiState);
            if (!want || (v.targetChanged && !raise)) v.keyboard = Keyboard::Hide;
            else if (raise) v.keyboard = Keyboard::Raise;
            return v;
        }

    private:
        std::uint64_t lastUi_ = 0;
    };

}  // namespace Magelight::EditState
