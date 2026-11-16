# MgPageRootTest: AddPageRoot in game (1.10.0)

A test plugin that keeps its pages outside `Data\Magelight`, laid out the way a page set made for another UI host
is: sibling page folders that load files from a shared folder above them.

```
Data\SKSE\Plugins\MgPageRootTest.dll
Data\MgPageRootTest\views\Hud\index.html              ../Shared/shared.js, ../Shared/shared.css
Data\MgPageRootTest\views\Minigames\Lock\index.html   ../../Shared/shared.js, ../../Shared/shared.css
Data\MgPageRootTest\views\Shared\                     shared.js, shared.css, cursor.png
Data\MgPageRootTest\control\Page\index.html           ../Shared/* (no page root: must stay refused)
Data\MgPageRootTest\control\Shared\
```

## Build

Not part of a normal build. After `build.ps1` (or `C:\b\mgl-build.ps1`) has configured a build folder, from a VS x64
prompt:

```
cmake -DMG_TEST_PLUGINS=ON <build folder>
cmake --build <build folder> --config Release --target MgPageRootTest
```

The DLL lands in `<build folder>\tools\pageroot-test\ingame\`. Copy it to `Data\SKSE\Plugins\` and `data\*` to
`Data\` (or make both one mod in a mod manager), beside a Magelight 1.10.0 or later.

## What to check

1. Load a save. Three boxes show at the top right, each starting PASS or FAIL:
   - `hud` and `lock`: shared script loaded, style loaded, network loopback.
   - `control`: shared script refused, style refused, network loopback.
2. `SKSE\MgPageRootTest.log`: every line PASS. At kDataLoaded it checks that `AddPageRoot` refuses Data, the game
   folder, a drive root, a folder in `Data\Magelight`, `Data\SKSE`, a folder inside Magelight's own, a missing
   folder and a relative one; takes `Data\MgPageRootTest\views` (and takes it again with a trailing separator);
   resolves the relative cursor image `Shared/cursor.png` for the `lock` view against the root; then one line per
   page report once a save loads.
3. `Magelight.log`: `mod N ('MgPageRootTest') pages may read ...\MgPageRootTest\views`, and exactly the `control`
   page's two reads refused (`file read outside Data\Magelight or any page root refused - ...\control\Shared\...`).

A FAIL on `hud` or `lock` is the bug this fixes; a FAIL on `control` means the root reached further than it should.
