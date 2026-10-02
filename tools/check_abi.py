#!/usr/bin/env python3
"""check_abi.py — the append-only ABI guard (invariant 10).

The exported API tables in src/MagelightApiExport.cpp are positional: their
initializer order MUST equal the member order of the structs in
api/MagelightUI_API.h — a swapped pair compiles fine and calls the wrong
function in every consumer. This script parses both and fails on any drift:

  1. s_apiN initializer order == MagelightApiN member order (N = 1..4),
  2. each older struct's members are an exact prefix of the next one
     (v1 < v2 < v3 < v4, up to v4's mid-struct hostVersionNumber),
  3. every published enum value keeps its number (PINNED_ENUMS).

Stdlib only. Run: python3 tools/check_abi.py  (exit 1 = ABI broken)
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "api" / "MagelightUI_API.h"
EXPORT = ROOT / "src" / "MagelightApiExport.cpp"

failures = []


def fail(msg):
    failures.append(msg)
    print(f"FAIL  {msg}")


def ok(msg):
    print(f"ok    {msg}")


def struct_body(text, name):
    """The {...} body of `struct name {` with braces balanced."""
    m = re.search(rf"struct {name} \{{", text)
    if not m:
        fail(f"{name}: struct not found in {HEADER.name}")
        return ""
    depth, i = 0, m.end() - 1
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[m.end():i]
        i += 1
    fail(f"{name}: unbalanced braces")
    return ""


# A member is either a function pointer `Ret (*Name)(args)` (possibly spanning
# lines) or a data field (`std::uint32_t apiVersion;`, `const char* hostVersion;`).
# LIMITATION: only those two data-field types are recognised. A future data
# member of another type (bool, float, a struct) is invisible to this check on
# BOTH sides and could drift unseen — extend the second alternative when one
# is added, or the guard silently stops guarding that slot.
MEMBER = re.compile(
    r"\(\s*\*\s*(\w+)\s*\)\s*\("                                   # (*Name)(
    r"|^\s*(?:std::uint\d+_t|const char\*)\s+(\w+)\s*;",           # data field
    re.M,
)


def header_members(text, name):
    body = struct_body(text, name)
    return [g1 or g2 for g1, g2 in MEMBER.findall(body)]


ENTRY = re.compile(
    r"&Magelight::Api4::(\w+)"        # &Magelight::Api4::RegisterMod
    r"|&Api(\w+)"                     # &ApiCreateView
    r"|MAGELIGHT_API::kApiVersion\d"  # -> apiVersion
    r"|\bPLUGIN_VERSION\b"            # -> hostVersion
    r"|MAGELIGHT_API::PackVersion\("  # -> hostVersionNumber
)


def export_entries(text, name):
    m = re.search(rf"{name}\{{(.*?)^\s*\}};", text, re.S | re.M)
    if not m:
        fail(f"{name}: initializer not found in {EXPORT.name}")
        return []
    out = []
    for match in ENTRY.finditer(m.group(1)):
        g4, gshared = match.group(1), match.group(2)
        token = match.group(0)
        if g4:
            out.append(g4)
        elif gshared:
            out.append(gshared)  # &ApiCreateView -> CreateView (strip 'Api')
        elif "kApiVersion" in token:
            out.append("apiVersion")
        elif token == "PLUGIN_VERSION":
            out.append("hostVersion")
        else:
            out.append("hostVersionNumber")
    return out


# Enums never renumber: every value a release published, by name. A changed
# or missing one fails; a new one passes with a note to pin it here.
PINNED_ENUMS = {
    "Result": {"Ok": 0, "HostAbsent": 1, "HostTooOld": 2, "NotReady": 3, "RenderDead": 4,
               "InvalidView": 5, "InvalidMod": 6, "FileNotFound": 7, "Busy": 8, "Denied": 9,
               "Unsupported": 10, "InvalidArgument": 11, "Internal": 12},
    "CallbackThread": {"GameThread": 0, "RenderThread": 1},
    "Layer": {"Hud": 0, "Panel": 1, "Popup": 2, "System": 3},
    "Anchor": {"TopLeft": 0, "TopRight": 1, "BottomLeft": 2, "BottomRight": 3},
    "NetworkPolicy": {"LoopbackOnly": 0, "Any": 1, "FileOnly": 2},
    "Event": {"ViewDomReady": 0, "ViewLoadFailed": 1, "ViewReloaded": 2, "ViewDestroyed": 3,
              "UIModeEntered": 4, "UIModeExited": 5, "FocusDenied": 6, "DisplayResized": 7,
              "ConsoleMessage": 8, "RenderDead": 9, "HostShutdown": 10, "UIModeRefused": 11},
}

ENUM = re.compile(r"enum class (\w+)\s*:\s*[\w:]+\s*\{(.*?)\};", re.S)


def check_enums(header):
    found = {}
    for name, body in ENUM.findall(header):
        body = re.sub(r"//[^\n]*", "", body)
        values = {}
        for item in body.split(","):
            m = re.match(r"\s*(\w+)\s*=\s*(-?\d+)\s*$", item)
            if m:
                values[m.group(1)] = int(m.group(2))
            elif item.strip():
                fail(f"enum {name}: '{item.strip()}' has no explicit value (published enums spell every value out)")
        found[name] = values
    for name, pinned in PINNED_ENUMS.items():
        got = found.get(name)
        if got is None:
            fail(f"enum {name}: not found in {HEADER.name}")
            continue
        bad = [f"{k}={v} (now {got.get(k, 'missing')})" for k, v in pinned.items() if got.get(k) != v]
        if bad:
            fail(f"enum {name}: RENUMBERED or removed: {', '.join(bad)}")
        else:
            ok(f"enum {name}: {len(pinned)} published values unchanged")
        extra = [f"{k}={v}" for k, v in got.items() if k not in pinned]
        if extra:
            print(f"note  enum {name}: new value(s) {', '.join(extra)} - pin them in PINNED_ENUMS")


def main():
    header = HEADER.read_text(encoding="utf-8")
    export = EXPORT.read_text(encoding="utf-8")

    members = {}
    for n in (1, 2, 3, 4):
        want = header_members(header, f"MagelightApi{n}")
        got = export_entries(export, f"s_api{n}")
        members[n] = want
        if not want or not got:
            continue
        if want == got:
            ok(f"MagelightApi{n}: initializer order matches struct order ({len(want)} members)")
            continue
        fail(f"MagelightApi{n}: ORDER DRIFT — export table calls the wrong functions")
        for i, (w, g) in enumerate(zip(want, got)):
            if w != g:
                print(f"      first mismatch at member {i}: struct has '{w}', table initializes '{g}'")
                break
        if len(want) != len(got):
            print(f"      member count: struct {len(want)} vs table {len(got)}")
            missing = [w for w in want if w not in got]
            extra = [g for g in got if g not in want]
            if missing:
                print(f"      never initialized: {', '.join(missing)}")
            if extra:
                print(f"      initialized but not in struct: {', '.join(extra)}")

    for a, b in ((1, 2), (2, 3), (3, 4)):
        if members.get(a) and members.get(b):
            if members[b][: len(members[a])] == members[a]:
                ok(f"MagelightApi{a} members are an exact prefix of MagelightApi{b}")
            else:
                fail(f"MagelightApi{a} is NOT a prefix of MagelightApi{b} — old consumers break")

    check_enums(header)

    if failures:
        print(f"\n{len(failures)} ABI check(s) FAILED (invariant 10: append-only, order = struct order)")
        return 1
    print("\nABI checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
