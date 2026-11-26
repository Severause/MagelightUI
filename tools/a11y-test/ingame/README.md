# MgA11yTest: the accessibility channel in game (1.10.0)

A test plugin that registers an accessibility listener (`SetAccessibilityListener`), as an assistive plugin such as a
screen reader bridge would, and writes every message it hears to `SKSE\MgA11yTest.log`. Nothing is spoken.

## Build

Not part of a normal build. After `build.ps1` (or `C:\b\mgl-build.ps1`) has configured a build folder, from a VS x64
prompt:

```
cmake -DMG_TEST_PLUGINS=ON <build folder>
cmake --build <build folder> --config Release --target MgA11yTest
```

The DLL lands in `<build folder>\tools\a11y-test\ingame\`. Copy it to `Data\SKSE\Plugins\`, beside a Magelight 1.10.0
or later.

## What to check

1. `SKSE\MgA11yTest.log` at the main menu: `QueryCapability a11y=1 uiModeView=1` and `listener registered`.
   `Magelight.log`: `mod N ('MgA11yTest') registered an accessibility listener`, then `page accessibility readers on`.
2. Load a save and open a mod's page in UI mode: one `"type":"page"` line with its title and `ui=` that view's id.
3. Tab or click through its controls: a `"type":"focus"` line per control, its `name` what a screen reader should
   say. A field you type in gives `"type":"edit"` lines; a password field gives `"secret":true` and a length, never
   its text.
4. Move through a list or tabs with the keyboard or the gamepad: `"type":"active"` lines for the item.
5. Something that pops up (a toast, a chat line, a dialog): `"type":"announce"` once it settles, `"type":"dialog"`.
6. Close the page: no lines while no page is shown (a hidden page's messages are not delivered).
7. Rapid scrolling through a long list: `Magelight.log` may say `sends reader messages faster than 20 a second`.
