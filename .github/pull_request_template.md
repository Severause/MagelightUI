## What this PR does

<!-- One paragraph. If it touches view rendering, say which page you verified it on. -->

## Checklist

- [ ] ABI unchanged, **or** append-only: new members added at the struct tail, initializer order = member order, `python tools/check_abi.py` passes (CI runs it)
- [ ] The invariants in CLAUDE.md are respected — one Ultralight thread, no synchronous listener takes `s_viewsMutex`, views erased only in `ApplyPendingLifecycle`, POD-only frames in SEH sections
- [ ] Deployable change: `CMakeLists.txt` VERSION bumped and a CHANGELOG.md entry added (docs/CI-only changes need neither)
- [ ] Rendering change: checked in `tools/desktop-harness` before any game cycle
- [ ] Full build passes (`build.ps1`) and `tools/check_stage.ps1` is green (use `-NoPex` only where no Papyrus compiler exists)

## Notes for the reviewer

<!-- Trade-offs, alternatives considered, anything you were unsure of. -->
