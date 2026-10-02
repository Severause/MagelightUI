# Magelight.InspectTarget — using both tiers together

Look at an NPC, press a key, and a panel shows their live stats. It exists to
show **the C++ tier and the Papyrus tier of Magelight cooperating over one
mod**, each doing what only it can.

## Who does what

| | Tier | Job |
|---|---|---|
| `scripts/source/MagelightInspect.psc` | **Papyrus** | The trigger. A hotkey reads the crosshair actor (`Game.GetCurrentCrosshairRef`), the **Magelight Papyrus tier** finds the view by modId (`Magelight.FindView`), and the script calls the plugin's own native `MagelightInspect.Open(Actor)`. |
| `src/plugin.cpp` | **C++** | The mod, the view, the live data, opening and closing. It `RegisterMod`s `MagelightInspect` and creates the `inspect` view (which Papyrus then finds by that id), gathers the actor's live snapshot from `RE::` into JSON, pushes it to the page, shows the view and takes UI mode. |
| `views/inspect/index.html` | page | Renders the snapshot; its Close button calls back into C++. |

The handoff is the author's own native, **`MagelightInspect.Open(Actor)`** —
Papyrus hands C++ the actor, C++ reads its name/race/level/H·M·S/weapon/combat
state (clumsy and slow in Papyrus, one call in C++), `InvokeJS`es it into the
page and opens it. Neither tier could do this alone: Papyrus can't cheaply read
that snapshot or hold a JS listener; wiring a quest hotkey from C++ is far more
work than `RegisterForKey`. So each does its strength, sharing one `modId`.

That the two tiers share a mod is the key move: **C++ registers the mod first
(at `kDataLoaded`); Papyrus reaches the same mod's views by its id string** —
`Magelight.FindView("MagelightInspect", "inspect")`. A mod registered from C++
is plugin-owned, so since host 0.30.0 a script can only read it: `FindView`,
`IsVisible`, `GetLastError` work, while `ShowView`, `RequestUIMode`,
`RegisterListener`, `Eval` and the other calls that act on its views are
refused, and so is Papyrus `RegisterMod` with its slug. That is why the plugin
exposes `Open` instead of letting the script drive the view
([PAPYRUS.md](../../docs/PAPYRUS.md#which-mods-a-script-can-drive)).

## Build

- **C++** — an ordinary CommonLibSSE-NG SKSE plugin. Put `api/MagelightUI_API.h`
  and `api/MagelightUI_Mod.h` on the include path and compile `src/plugin.cpp`.
  No link against Magelight (it's reached by `GetProcAddress`).
- **Papyrus** — compile `scripts/source/MagelightInspect.psc` with Magelight's
  `Magelight.psc` on the import path (it declares the `Magelight.*` natives;
  this script declares its own `Open`). Ships as `MagelightInspect.pex`.
- **ESP** — `RegisterForKey` needs a script that runs, so add the script to a
  **start-game-enabled quest** (Creation Kit / xEdit: a new quest, Start Game
  Enabled, with `MagelightInspect` attached). Nothing else in the plugin is
  needed.

## Install

```
Data\SKSE\Plugins\MagelightInspect.dll
Data\Scripts\MagelightInspect.pex
Data\Magelight\MagelightInspect\views\inspect\index.html
Data\MagelightInspect.esp            (the start-game-enabled quest)
```

Plus Magelight UI (0.16.0+). In game, look at an NPC and press **O** (change
`InspectKey` in the script, DXScanCode). `Magelight.log` and the game's Papyrus
log name any problem.

See [../../docs/CPP.md](../../docs/CPP.md) and [../../docs/PAPYRUS.md](../../docs/PAPYRUS.md)
for the two tiers, and [../Magelight.CppPanel](../Magelight.CppPanel) for the
C++-only starting point.
