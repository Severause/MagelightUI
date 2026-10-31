# Stack test kit

Magelight runs its pages on the game's main thread. The game gives that thread 1 MB of stack (from
`SkyrimSE.exe`'s header) and calls Present deep inside its frame, so before 1.3.0 pages ran in whatever the
game's frame left. A page that recursed in its scripts and then laid itself out, or a deeply nested page,
could use that up: `SEH exception 0xC00000FD ... in frame work` in `Magelight.log`, and the overlay off for
the session. A stack guarantee on that thread makes it worse: CrashLogger 1.23 and later holds back its last
64 KB, which JavaScriptCore's error path needs, so there any error a page leaves uncaught, or any script at the
recursion limit, overflows with nothing deep on the stack at all. Since 1.3.0 the page work runs on a stack of
its own (`"ulFiberMB"`, 8 MB by default). The Ultralight 2.0 build (1.5.0) keeps it, holding the page VM's lock
whenever the thread is off it (`src/PageStackCore.h`; its gate is `tools/fiber-gc-test`), and with
`"ulFiberMB": 0` sets JavaScriptCore's stack zones past the thread's guarantee. With `"pageThread": true` (1.7.0)
pages run on Magelight's page thread instead, a thread with a stack of its own (`ulFiberMB`, 8 MB when 0): every
test passes there as on the page stack (offline: `fiber-gc-test --thread`).

## In game: `MgStackTest/`

A manifest mod (`Data/Magelight/MgStackTest/`). `F7` opens it; `"hotkeys": { "MgStackTest/test": "F8" }`
in `Magelight.json` rebinds it. **Run all tests** runs 16 tests: 11 push the page into runaway recursion in its
scripts, two leave an error uncaught (in a timer callback, and in a click listener reached by a DOM click
dispatch), and three make the engine lay out from the bottom of a recursion or a deeply nested page. Each must
end cleanly (a `RangeError`, the page's `error` event, or a finished layout) and the page must stay up. Results
show on the page and go to `Magelight.log` as `[MgStackTest]` lines; the two uncaught errors also write their
own `MgStackTest: an expected uncaught error` (or `Script error.`) console lines.

What to expect:

- 1.5.0 with the page stack (the default): every test passes, with or without a stack guarantee, at any
  `ulFiberMB` it takes (8 to 64; a smaller number is raised to 8).
- 1.5.0 with `"ulFiberMB": 0`: the 11 script tests end in `RangeError` and the two uncaught errors are reported,
  with or without a guarantee (the log's `JavaScript stack zones for pages on the game's stack` line gives the
  zones). The three layout tests pass with what the game's frame normally leaves; they are the known gap of that
  setting and fail once a few hundred KB of the stack are in use (offline from about 400 KB with a 64 KB
  guarantee).
- A Magelight before 1.3.0, or 1.3.x and the earlier 1.5.0 test builds with `"ulFiberMB": 0`: with no guarantee the
  overlay goes off at "Layout forced from the bottom of a recursion"; with a 64 KB guarantee (CrashLogger) at the
  first test.

Remove the folder when done. `package.ps1` zips it for a mod manager (`MgStackTest-<version>.zip`, the version
from its manifest, holding `Magelight/MgStackTest/...`; default output `dist\`, `-Out <folder>` for another).

## Offline: the probes

`build.bat` builds `stack_probe.exe` into `C:\b\mgl-stackprobe` (`MG_PROBE_DIR`) against the stock SDK in
`extern/ultralight`. It runs `MgStackTest/views/test/stack-tests.js` in Ultralight on a thread with a 1 MB stack,
of which `<usedKB>` is in use at every Ultralight call, the way the game's frame uses it, the way pages run with
`"ulFiberMB": 0`:

```
stack_probe.exe <usedKB> <guaranteeKB> <sdkDir> <tests.js> [host|default] [ids]
```

`guaranteeKB` sets a stack guarantee on the thread (`SetThreadStackGuarantee`), as CrashLogger does on the game's
main thread. `host` (the default) sets the JavaScript stack zones Magelight sets for that guarantee
(`PageStackCore.h`'s `ReserveFor`, through `JSC::Options::setOption` before `Renderer::Create`; Ultralight 2.0
reads no `JSC_` environment variable); `default` keeps JavaScriptCore's own. `ids` picks tests by id or index
(comma-separated). The page stack's own test is `tools/fiber-gc-test`: the frame model with garbage collection
running between frames, with and without the page VM's lock.

Measured on Ultralight 1.4.0 (2026-10-18 and 2026-10-24): on the 1 MB stack the process dies with `0xC00000FD` at
the layout test with nothing used, and a 500-level page fails to lay out at 600 KB used; on an 8 MB fiber every
test passes at any amount used, five rounds over with concurrent collection on or off.

Measured on Ultralight 2.0.0-beta.2 (2026-10-24): with JavaScriptCore's zones on the 1 MB stack, the 11 script
tests and the two uncaught errors pass with no guarantee and the layout test ends the process with `0xC00000FD`
at 0 KB used (as on 1.4); with a 64 KB guarantee every script test and both uncaught errors end it, at 0 KB used.
With the host's zones every test passes at 0 KB used for guarantees of 0, 64, 128 and 256 KB, and at 200 KB up to
64 KB; the layout test fails from 400 KB used with no guarantee or 64 KB (from 200 KB with 128 or 256 KB), at
400 KB with 128 or 256 KB the two DOM-depth tests fail too, and a 600 KB start loses more. Those zones give a script 384 KB of stack from where it enters the VM: a
`String.fromCharCode.apply` over 32768 arguments (about 261 KB, a page's chunk) finishes at every amount used
up to 400 KB, except 400 KB with a 256 KB guarantee; a 256 KB budget made it throw `RangeError` at 0 KB used, and a
448 KB one overflows the layout test at 0 KB used with a 256 KB guarantee. On an 8 MB fiber without
the VM lock, a collector thread crashes the process within the first round (an access violation at
`WebCore.dll+0x2B34EA`); with it, every test passes, CPU and GPU (`tools/fiber-gc-test`). On a fiber of 5 MB or
less the lock does not save the first test (`fibergc --fibermb 5`: 0xC00000FD): JavaScriptCore's script limit
(`maxPerThreadStackUsage`, 5 MB by default) lies past the fiber's end, so its limits fall to that end and its error
path commits into the fiber's own 64 KB stack guarantee. 6 MB passes; Magelight takes 8 MB at least.

A stack guarantee moves the overflow point up past where JavaScriptCore stops scripts, which its own zones do not
account for: from 60 KB, plain recursion dies at the first test with only 4 KB used, in the loop of
JavaScriptCore's `VM::updateStackLimits` that commits the stack down to its hard zone before an error is thrown or
reported (`MgWCore.dll+0x18CB970` on 1.4, `+0x173F003` on 2.0; crash logs name it `JSC::VM::throwException+0x160`,
the nearest function the DLL exports). The game did the same with `"ulFiberMB": 0` and 1020 KB left at Present,
at the address of the report that started this.
