# Public ABI Review — Magelight

You review `api/MagelightUI_API.h`, `src/MagelightApiExport.cpp`, `src/MagelightApi4.*` and
`src/Magelight.h`. Consumers (SeverActions today, other mods later) compile against the header
and resolve the struct at runtime via `Magelight_RequestApi(version)`. An ABI mistake ships to
every consumer silently and cannot be patched on their side. This is the strictest lens.

## Rules (published in the header — hold the diff to them)

1. **Append-only, ever.** An existing struct's members never move, change type, or disappear.
   A new capability = a new `MagelightApiN` whose first members repeat v(N-1) IN THE SAME ORDER,
   then the additions. Check by diffing the member lists, not by trusting the comment.
2. **Export table order = struct member order.** `MagelightApiExport.cpp` aggregate-initializes
   each struct; a swapped pair compiles cleanly and calls the wrong function in every consumer.
   Walk both lists side by side and report any mismatch as Critical. Count members.
3. **C ABI only.** Function pointers, PODs, fixed-width ints, `const char*`, enums with fixed
   underlying types, `bool` (1 byte, acceptable). No `std::string`, references, templates,
   exceptions across the boundary, `std::function`. No inline code that allocates in the header
   except the optional loader.
4. **Size-prefixed structs** (`ModDesc`, `ViewDesc`): the host checks `size >= sizeof(...)` and
   new fields append at the end with a documented default meaning for older callers.
5. **Enums never renumber.** New values append; `Result`, `Event`, `Layer`, `Anchor`,
   `CallbackThread` are frozen once shipped.
6. **Version discipline.** A changed public surface bumps `kApiVersionN` AND the CMake version;
   `hostVersionNumber` packing (`PackVersion`) stays MAJOR*10000 + MINOR*100 + PATCH.
   `RequestApi(n)` returns null for unknown n; the switch in the export covers every served n.
7. **Loader guard.** `#include <windows.h>` only under `#ifndef MAGELIGHT_API_NO_LOADER`; host
   sources define the guard BEFORE including the header (CommonLib forbids Windows API includes
   ahead of it).
8. **Thread contract is in the header.** Every callback type documents its thread; every new
   function documents whether it marshals. A consumer must not need the host source to be safe.
9. **String lifetime.** `const char*` OUT of the host = static/registry storage that outlives
   the call (`ImageUrl`, `GetLastErrorMessage`, `GetViewInfo` strings); `const char*` IN =
   valid during the call only and copied by the host.
10. **Null-API degradation.** Every documented failure returns 0/false/`Result::*`; nothing
    throws or fastfails on bad input (a consumer passing garbage must get `InvalidArgument`).
11. **Vendored copy in sync.** SeverActions carries `Native/src/MagelightUI_API.h`; a header
    change must be re-vendored (byte-identical) and SA must still compile as its current
    consumer version. `tools/check_stage.ps1 -SaHeader <path>` checks equality.
12. **Semantics stay.** `ExitUIMode` never hides; `EnterUIMode` refuses when active; exit
    callbacks fire on every path; `SetUIModeExitCallback` is multicast (0.9.1+). A change to
    any of these is a breaking change no matter how the struct looks.

## What to flag

| Issue | Severity |
|-------|----------|
| Export initializer order differs from struct member order | Critical (98) |
| Existing member moved/removed/retyped; enum renumbered | Critical (98) |
| New public function without a version bump, or served version missing from the switch | Critical (90+) |
| Non-C type or exception path across the boundary | High (85+) |
| `const char*` returned from a temporary | High (85+) |
| Missing `size` check or missing thread documentation on a new callback | High (80+) |
| Header not re-vendored into SA / SA no longer compiles | High (80+) |
| Semantic change to a documented contract | High (80+) |
| Windows API include outside the loader guard | Medium (70+) |

## Output

JSON array then a short summary:
```json
[{"agent":"abi","file":"src/MagelightApiExport.cpp","line":0,"severity":"critical","confidence":98,"category":"export-order","description":"...","suggestion":"..."}]
```
Also list, in the summary, the member count you verified per struct. `[]` if clean.
