# Load test kit

Measures what a heavy page costs the game's frame, with `"pageThread"` off (pages run in the game's frame, the
default) and on (1.7.0: pages run on a thread of their own and the game thread only runs their recorded GPU work and
draws them).

## In game: `MgLoadTest/`

A manifest mod (`Data/Magelight/MgLoadTest/`). `F10` opens its panel at the top right and gives it the mouse
(`"hotkeys": { "MgLoadTest/load": "Insert" }` in `Magelight.json` rebinds it). The game keeps running while it is
open, unless `Magelight.json` asks UI mode to pause it. The panel picks:

- **Script**: 0, 2, 5, 10 or 20 ms of script every frame (a busy loop in `requestAnimationFrame`).
- **Layout**: 300 boxes moved every frame (style, layout and paint).
- **Hang**: two clicks, then a script that never returns. The page freezes for good; reload the save or quit after.
  Only with `"pageThread": true`: with the thread off, a page that never returns freezes the whole game, which is
  what the thread exists to prevent.

The panel shows its own frames per second. Each choice is logged as `[MgLoadTest] ...` in `Magelight.log`, so the
log ties each cost line to the load that produced it.

### Protocol

1. Stand somewhere repeatable (a quiet interior is steadiest). Note the game's frame rate with the panel closed.
2. `"pageThread": false` (or absent): open the panel, then for each of 0, 5, 10 and 20 ms hold 30 s with the camera
   still and note the game's frame rate. Turn Layout on at 0 ms for 30 s.
3. Quit, set `"pageThread": true` in `Magelight.json`, repeat step 2 at the same spot.
4. With the thread on: **Hang**. The game must keep running and the log must say
   `the page thread has been in ... for N s - its pages are frozen; the game goes on`; Escape and F10 must still
   close the panel and give the controls back.

What the log shows:

- Thread off: `present cost over N frames - ul.update ..., ul.render ..., composite(+mirror) ..., total avg ...`.
  `ul.update` grows with the script load (the busy loop runs inside `Update`): that is time taken from the game's
  frame.
- Thread on: `present cost over N frames - prelude ..., execute ..., composite ..., total avg ...` is all the game
  thread spends; `page thread over N frames - ul.update ..., ul.render ..., ... frames/s, presents without a new
  frame P` is the page's own cost, off the game's frame. A page slower than the game shows as a lower frames/s and
  a higher P, never as a slower game.

Remove the folder when done. `package.ps1` zips it for a mod manager (`MgLoadTest-<version>.zip`, holding
`Magelight/MgLoadTest/...`; default output `dist\`, `-Out <folder>` for another).
