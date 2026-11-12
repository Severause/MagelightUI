// Focus sharing (src/FocusShareCore.h): which view a press moves the keyboard to while a view holds UI mode, and when
// it goes back to the holder. Exit 0 when every check passes.
#include "FocusShareCore.h"

#include <cstdio>

namespace FS = Magelight::FocusShare;

namespace
{
    int g_fails = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++g_fails;
    }

    FS::Facts View(std::uint32_t flags, bool exists = true, bool interactive = true)
    {
        FS::Facts f;
        f.exists = exists;
        f.interactive = interactive;
        f.flags = flags;
        return f;
    }
}

int main()
{
    const std::uint64_t holder = 7, other = 9, third = 11;
    const FS::Facts gives = View(FS::kGive), keeps = View(0), takes = View(FS::kTake), plain = View(0);

    // A press.
    Check(FS::OnPress(holder, holder, other, gives, takes) == other, "press: a holder that gives, a view that takes");
    Check(FS::OnPress(holder, holder, other, keeps, takes) == 0, "press: a holder that does not give keeps it");
    Check(FS::OnPress(holder, holder, other, gives, plain) == 0, "press: a view that does not take gets nothing");
    Check(FS::OnPress(holder, holder, other, gives, View(FS::kGive)) == 0, "press: giving is not taking");
    Check(FS::OnPress(holder, holder, other, gives, View(FS::kKnown)) == other, "press: a view that both gives and takes takes");
    Check(FS::OnPress(holder, other, holder, gives, keeps) == holder, "press: the holder takes it back");
    Check(FS::OnPress(holder, other, holder, keeps, keeps) == holder, "press: the holder takes it back, giving or not");
    Check(FS::OnPress(holder, other, third, gives, takes) == third, "press: from one taker to another");
    Check(FS::OnPress(holder, other, third, gives, plain) == 0, "press: a third view that does not take leaves it");
    Check(FS::OnPress(holder, other, other, gives, takes) == 0, "press: on the view that has it, nothing moves");
    Check(FS::OnPress(holder, holder, holder, gives, gives) == 0, "press: on the holder that has it, nothing moves");
    Check(FS::OnPress(0, 0, other, gives, takes) == 0, "press: outside UI mode, nothing");
    Check(FS::OnPress(holder, holder, 0, gives, takes) == 0, "press: outside every view, nothing");
    Check(FS::OnPress(holder, holder, other, gives, View(FS::kTake, false)) == 0, "press: a view being destroyed takes nothing");
    Check(FS::OnPress(holder, holder, other, gives, View(FS::kTake, true, false)) == 0, "press: a view not shown takes nothing");

    // Settling.
    Check(FS::Settle(holder, other, gives, takes) == other, "settle: the share holds");
    Check(FS::Settle(holder, other, keeps, takes) == holder, "settle: the holder stopped giving");
    Check(FS::Settle(holder, other, gives, plain) == holder, "settle: the view stopped taking");
    Check(FS::Settle(holder, other, gives, View(FS::kTake, false)) == holder, "settle: the view went away");
    Check(FS::Settle(holder, other, gives, View(FS::kTake, true, false)) == holder, "settle: the view was hidden");
    Check(FS::Settle(holder, holder, keeps, keeps) == holder, "settle: the holder has it");
    Check(FS::Settle(holder, 0, gives, plain) == holder, "settle: none yet is the holder");
    Check(FS::Settle(0, other, gives, takes) == 0, "settle: outside UI mode, nobody");

    // Flags.
    Check(FS::KnownFlags(0) && FS::KnownFlags(FS::kGive) && FS::KnownFlags(FS::kTake) && FS::KnownFlags(FS::kKnown),
          "flags: the known bits");
    Check(!FS::KnownFlags(4) && !FS::KnownFlags(0x80000000u), "flags: an unknown bit");

    std::printf("\n%s (%d failed)\n", g_fails ? "FAILED" : "all passed", g_fails);
    return g_fails ? 1 : 0;
}
