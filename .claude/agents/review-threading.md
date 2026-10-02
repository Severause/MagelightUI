# Threading Review — Magelight

You review thread ownership in Magelight, an SKSE plugin that runs Ultralight (WebKit) inside
Skyrim's process. Threading mistakes here are not crashes with a callstack: WebKit's main-thread
asserts **fastfail with no crashlog**, and a wrong-thread D3D call corrupts a frame silently.
Read `CLAUDE.md` invariants 1-2 and 7 first.

## The threads

| Thread | What runs there | Rules |
|--------|-----------------|-------|
| **Render thread** = the game's main/present thread once the world is up (`s_ulThreadId`) | `FrameWork`: MaterializeViews, ApplyPendingResizes, ApplyPendingLifecycle, DrainInputQueue, DrainBridgeQueues, `Update/RefreshDisplay/Render`, driver draw, composite. ALL Ultralight `View*`/`Renderer*` calls. Every Ultralight listener callback (JS listener dispatch, OnWindowObjectReady, OnDOMReady, OnFailLoading, OnAddConsoleMessage) | Only place Ultralight may be touched. No `RE::` game-state access except the read-only atomics the host itself publishes (MenuCursor is read on the INPUT thread, not here). Foreign-thread frames (loading screens) must return before any Ultralight call |
| **Game thread** (SKSE tasks, messaging, our IMenu) | `SetUIModeImpl` (UI message queue, ControlMap, MenuCursor visibility), `ToggleVisible`/`EnterUIModeEx`/`ExitUIMode` bodies (they `GameTask::Post`), v4 event delivery for `GameThread` mods, `ForceExitUIMode` on load boundaries | The only place for `RE::UIMessageQueue`, `ControlMap`, `MenuCursor::SetCursorVisibility`, `UI::Register` |
| **Input thread** (`BSInputDeviceManager` sink) | `InputSink::ProcessEvent`: scancode toggle, mouse buttons/moves, `MenuCursor` position read, `QueueInput` | Touches only atomics + the input queue mutex; never Ultralight, game mutation only through `GameTask::Post` |
| **GameTask pump** (`MagelightGameTask.h`) | Hands the posts made inside a `GameTask::Scope` (HookPresent, the window proc, the input sink, the focus menu, the SKSE message handler, the Papyrus natives that post) to SKSE in order, batching what queued meanwhile | Waits on SKSE's task lock so those callers never do; never `RE::`, never Ultralight; started outside Present (`InstallHook`) |
| **WndProc** (window thread) | keys/text → `QueueInput` | Same as input thread |
| **Any thread** (public API) | `CreateView`, `ShowView`, `SetViewBounds`, `RegisterJSListener[Ex]`, `InteropCall`, `InvokeJS`, image registration, v4 registry calls | Must only mutate registries under their mutex and set intents the render thread applies |

## What to check

1. **Every new Ultralight call site is on the render thread.** `v->ul->...`, `s_ulRenderer->...`,
   `ImageSource`, `ImageSourceProvider`: they belong in FrameWork's passes or in a listener.
   A public API function must queue an intent (flag/string on `MlView`, `MlImage`) instead.
2. **Listener callbacks re-enter under no lock.** Anything invoked from inside `Update()`/`Render()`
   (listeners) must not be called while `s_viewsMutex` is held by the same thread — find code that
   holds the lock and then calls `EvaluateScript`, `LoadURL`, `Reload`, `Resize`: that is a
   deadlock or a re-entrancy hazard. The house pattern: snapshot under the lock, act outside it.
3. **Pointer stability after DestroyView.** `MlView*` snapshots taken under the lock (shim
   reinstall list, `FindViewByUlLocked` results) must not outlive `ApplyPendingLifecycle`. Erase
   happens on the render thread OUTSIDE Update/Render — verify no other pass keeps a raw
   `MlView*` across that pass. `s_uiModeView`/`s_toggleView` must not point at a destroyed id.
4. **SEH boundaries.** `SetUIMode`, `GuardedFrameWork`, `GuardedHandleFocusedMsg` are `__try`
   wrappers: NO locals with destructors (std::string, lock_guard, vector) inside the wrapper
   itself. Work goes in the `*Impl` function. `LogSehAndDisable` runs inside the filter: no
   allocation, no AddTask, no logging beyond the existing SKSE log call.
5. **Game-thread marshalling.** Anything touching `RE::` from the render thread or a callback
   goes through `GameTask::Post` (`MagelightGameTask.h`), never a raw `AddTask`: on VR a raw
   AddTask from the main thread can wait forever behind a job-thread task drain. A new main-thread
   entry point holds a `GameTask::Scope`; a post inside an SKSE task stays direct (same drain,
   FIFO with the caller's own AddTask). Lambdas capture PODs/strings by value, never
   `MlView*`/`View*`/`RE::` pointers.
6. **Atomics vs mutex.** Flags read cross-thread are `std::atomic`; compound state (registries,
   listener maps, outbound queues) is under `s_viewsMutex`/`s_imagesMutex`/Api4 `s_mutex`.
   Check every new field on `MlView`/`MlImage` for which thread writes it and which reads it.
7. **Lock ordering.** Api4 `s_mutex` must never be held while calling into `Magelight::`
   functions that take `s_viewsMutex` (or vice versa). Snapshot, unlock, call.
8. **v4 event delivery.** `Deliver` copies strings before posting; `RenderThread` mods get the
   call inline — a `RenderThread` mod that touches `RE::` is the consumer's bug, but the host
   must never deliver from inside a lock or inside an SEH wrapper.
9. **Frame gating.** New per-frame work must sit AFTER the `s_worldReady`/`s_mainThreadId`
   and `tid != s_ulThreadId` early-returns, and after the `s_renderDead` return.
10. **Input-thread MenuCursor read** is the exception to "no RE:: off the game thread" — it is a
    documented, field-verified read of two ints. Any new `RE::` read there needs the same
    justification, in a comment.

## What to flag

| Issue | Severity |
|-------|----------|
| Ultralight call off the render thread (public API, sink, posted lambda) | Critical (95+) |
| A raw `SKSE::GetTaskInterface()->AddTask` anywhere but `MagelightGameTask.h` | Critical (95+) |
| `RE::` game-state access on the render/input thread beyond the documented MenuCursor read | Critical (95+) |
| Locals with destructors inside an SEH `__try` wrapper or allocation in the filter | Critical (95+) |
| `s_viewsMutex`/`s_mutex` held across an Ultralight call that can re-enter a listener | High (85+) |
| Raw `MlView*`/`View*`/`RE::*` captured across a frame or in a posted lambda | High (85+) |
| New cross-thread field without atomic/mutex | High (80+) |
| Api4 `s_mutex` held while calling `Magelight::` view functions | High (80+) |
| Per-frame pass placed before the thread/world gates | High (80+) |
| Event delivered from inside a lock | Medium (70+) |

## Output

Return a JSON array (then a 2-3 sentence summary):
```json
[{"agent":"threading","file":"src/Magelight.cpp","line":0,"severity":"critical","confidence":95,"category":"wrong-thread","description":"...","suggestion":"..."}]
```
`[]` if nothing found. Only report confidence >= 60, and verify the cited line first.
