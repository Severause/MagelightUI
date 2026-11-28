#pragma once
// The accessibility channel's rules (1.10.0, SetAccessibilityListener; CLAUDE.md invariant 32). The page core's reader
// (A11yScript.h) describes what a screen reader would say about a page and sends it on the reserved '__a11y' channel;
// the host passes each message to the listeners assistive plugins registered, as the page wrote it. These are the
// checks the host makes on the way: which messages pass, and how many a view may send.
//
// std only: tools/a11y-test runs all of it on the desktop.

#include <cstdint>
#include <string_view>

namespace Magelight::A11y {

    // A message larger than this (UTF-8 bytes) is dropped. The reader measures its messages the same way and shortens
    // one that would be over it (A11yScript.h emit).
    inline constexpr std::size_t kMaxMessageBytes = 8192;

    // The kinds of message the reader sends, each a JSON object that begins {"v":1,"type":"<kind>". The host checks
    // only that prefix, the size and (Magelight.cpp) that the text parses: everything after it is the page's own text.
    inline constexpr std::string_view kTypes[] = { "focus", "active", "state", "edit", "announce", "dialog", "page" };

    // The type the message names, or "" when it is no reader message.
    inline std::string_view TypeOf(std::string_view msg)
    {
        constexpr std::string_view head = "{\"v\":1,\"type\":\"";
        if (msg.size() > kMaxMessageBytes || msg.substr(0, head.size()) != head) return {};
        const std::string_view rest = msg.substr(head.size());
        const std::size_t end = rest.find('"');
        if (end == std::string_view::npos) return {};
        const std::string_view type = rest.substr(0, end);
        for (std::string_view t : kTypes)
            if (t == type) return t;
        return {};
    }

    // The reader's on/off messages (the host's '__a11y' InteropCall): "on" starts it, "off" stops it, "page" asks for
    // the page's title and focused element (sent with UI mode entering on the view, and when a listener registers
    // while a view holds it).
    inline constexpr const char* kOn = "on";
    inline constexpr const char* kOff = "off";
    inline constexpr const char* kPage = "page";

    // How many messages one view may send: kBurst at once, then kPerSecond. A chat that streams text, or a page in a
    // loop, gets its messages dropped past that, never queued: speech that far behind is worse than none.
    inline constexpr double kBurst = 40.0;
    inline constexpr double kPerSecond = 20.0;

    struct RateGate {
        double tokens = kBurst;
        std::uint64_t lastMs = 0;
        bool started = false;

        // True when the message may pass. nowMs is a steady clock; one that goes back counts as no time passed.
        bool Allow(std::uint64_t nowMs)
        {
            if (!started) { started = true; lastMs = nowMs; }
            if (nowMs > lastMs) {
                tokens += static_cast<double>(nowMs - lastMs) * kPerSecond / 1000.0;
                if (tokens > kBurst) tokens = kBurst;
                lastMs = nowMs;
            }
            if (tokens < 1.0) return false;
            tokens -= 1.0;
            return true;
        }
    };

    // Whether a view's message reaches the listeners. A hidden page is not on screen, so what it says is not either,
    // the 'page' answer included (UI mode shows its view first). The host's own keyboard page and an inspector are
    // the host's, never a mod's page.
    inline bool Delivers(bool visible, bool dormant, bool hostPage)
    {
        return visible && !dormant && !hostPage;
    }
}
