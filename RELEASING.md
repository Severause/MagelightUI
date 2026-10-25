# Releasing Magelight UI

The mechanics of a public build. Everything except the smoke test is scripted; everything
scripted fails loudly rather than shipping a bad zip.

## 0. Before every release: the license basis

- **Ultralight licence.** A public release relies on section 2.3 (Limited Commercial
  Distribution) of the Ultralight Free License Agreement; section 2.2 covers academic
  research only. Section 2.3 is void for a company or incorporated entity whose gross turnover
  over the trailing twelve months (its parent companies' and affiliates' included), or whose
  total investment funding raised, is US$200,000 or more. The licensee is Magelight's author, an
  individual. If that ever changes, stop and talk to Ultralight, Inc. before releasing.
- **The DLL rename.** The three Ultralight DLLs ship renamed, with the module-name strings
  inside them rewritten in place, under Ultralight, Inc.'s written permission. Keep that
  permission with the release records, and re-verify the rename (`tools/check_stage.ps1`)
  on every SDK bump.
- **The page stack.** On every SDK bump, `tools\fiber-gc-test\build.bat` must end `GATE PASS`
  (the frame model crashes without the page VM's lock and passes with it, CPU and GPU): the
  WebCore exports the lock and the game-stack zones use are undocumented names (invariant 1).

## 1. Version and changelog

- Bump `project(... VERSION x.y.z)` in `CMakeLists.txt` — the single source (the DLL
  version resources, `SKSEPlugin_Version`, the zip name and the stage lint all read it).
- `CHANGELOG.md` gets the entry; the "added in" table is how consumers pick `minHost`.
- The example log line at the top of `docs/TROUBLESHOOTING.md` names the version: update it.
- The npm packages version with the host (`docs/SDK.md`); publish them per section 6 when
  the page contract changed.

## 2. Build and package

```
npm ci
npm run build
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean -Examples
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version X.Y.Z
```

- `extern/ultralight/` is not in git. Fetch it per `extern/README.md`; `build.ps1` refuses any
  SDK whose `extern/ultralight/VERSION.txt` is not the pinned build of record (`2.0.0-beta.2`,
  the Free edition). Ultralight may stop offering a build once newer ones are out, so keep the
  archive (`ul-sdk-2.0.0-beta.2-win-x64-free.7z`, SHA-256
  `e17b3ca2f3c2063f1d14971e7ef756f28e71f2d426a91d2e21c5236d2dc67e6d`) with the release records.
- **CI's SDK is the Actions cache entry `ultralight-sdk-2.0.0-beta.2`**, saved by a passing run
  on `main` and restored by every branch and pull request. GitHub removes a cache that goes
  unused for seven days (or when the repository's caches pass their size limit); the fetch step
  then asks Ultralight's release API for the pinned archive and checks its SHA-256 before
  caching it. Once Ultralight no longer serves that build, every run fails with the pin message.
  To re-seed the cache then:
  1. Put Ultralight's own archive (the SHA-256 above) behind a link the runner can download
     without credentials. Keep the link private and short-lived: it appears in the run's log,
     and the SDK is Ultralight's, not a download for us to publish.
  2. Actions → CI → Run workflow on `main`, with `sdk_url` set to that link. The step checks
     the archive's SHA-256 and its `VERSION.txt` against the pin, and the cache is saved only if
     the whole job passes. Once it has, take the link down.

  Moving the pin instead is a deliberate change: `build.ps1` (`$ulBuildOfRecord`), the CI cache
  key and its fetch step's version and SHA-256, and `extern/README.md` name it. The new build
  needs section 0's rename check and the notices re-verified.
- `npm ci && npm run build` at the repo root first: it builds the `packages/` the
  ReactConfig example compiles against. On a fresh clone without it, `build.ps1` only
  warns and stages the example without its page, so check its output for
  `example Magelight.ReactConfig: built`.
- `-Clean` because the stage is shared build output — package from the branch you mean to
  ship (the packager also refuses a version mismatch against the staged DLL).
- Packaging runs `tools/check_stage.ps1` as a **release gate**: namespaced-runtime patch
  verification, required files, version coherence of both DLLs. A stage that fails lint
  does not zip. A release must carry `Magelight.pex`, so package on a machine with the
  Papyrus compiler (the game installs it).
- Output in `dist\`, each a plain Data-root zip that a mod manager installs with no
  installer: `MagelightUI-X.Y.Z.zip` (the runtime, the only thing a player needs),
  `MagelightUI-X.Y.Z-DevTools.zip` (the Web Inspector and the host's dev pages) and
  `MagelightUI-X.Y.Z-Examples.zip` (the example mods), plus `MagelightUI-X.Y.Z-pdb.zip`
  (symbols — **keep with the release, never ship to players**; the crash diagnostics'
  offsets decode only against the exact build's PDBs).

## 3. Smoke test (manual — the step you do not skip)

Install the runtime zip and then the Examples zip into a **clean MO2 profile** (no earlier
Magelight, current SKSE):

- The game launches; `Magelight.log` line one names the new version, then `pages run on a stack of
  their own` and, once a page loads, `WebKit's VM is up ...`, with no `does not export` line.
- F6 shows and hides the `Magelight.Badge` badge, F7 opens its panel.
- `Magelight.ReactConfig` opens with F8: UI mode takes the cursor, typing lands in the
  page, Escape closes, game controls resume.
- A `Magelight.json` override (`toggleKey`) applies.
- If the release touched the VR presenter: one head-locked quad opens/closes on
  SteamVR-native AND on OpenComposite.
- If the CommonLib pin (`ports/commonlibsse-ng`) changed since the last public release,
  smoke-test **every runtime**: SE 1.5.97, AE 1.6.x, AE 1.7.x and VR 1.4.15. On each,
  `Magelight.log` shows line one, `renderer created`, and one view opens.

## 4. History

The public repository began at its first public release, without the private repository's history.

## 5. Tag and publish

From the first public release on, every release is cut from the public repository's own history: work lands
there by pull request, and the release commit is a commit on its `main`. Nothing from the
private repository is ever pushed to the public one: a pushed commit carries its whole
ancestry, so a branch or tag made in a private clone would publish that clone's history.

- CI is green on the release commit.
- Annotated tag on the release commit, in a clone of the public repository:
  `git tag -a vX.Y.Z -m "Magelight UI X.Y.Z"`, then `git push origin vX.Y.Z` once
  `git remote -v` shows `origin` is `https://github.com/Severause/MagelightUI.git`.
- No GitHub release: mod authors build the DevTools and examples from the repository. Keep
  the `-pdb.zip` with the release records (never ship it to players).
- npm, when the packages changed: section 6.
- Nexus: after the tag. The runtime zip is the only file. Set the requirements per runtime
  as the page lists them.

## 6. npm packages

The four packages (`@magelight/sdk`, `@magelight/react`, `@magelight/vite-plugin`,
`create-magelight-view`) version with the host whose page contract they match, bumped only
when that contract changes (docs/SDK.md). From 1.0 a caret range takes every later 1.x, so
the ranges move only with a new major; move them anyway when a release needs the new
version's API (the template and the example should ask for what they use).

1. On the release branch: set `version` in `packages/sdk`, `packages/react`,
   `packages/vite-plugin`, `packages/create-magelight-view` and the root `package.json`.
   Move `@magelight/react`'s peer range on `@magelight/sdk` (`>=X.Y.0`) and its dev range
   (`^X.Y.0`), and every `@magelight/*` range in `packages/create-magelight-view/template`
   and `examples/Magelight.ReactConfig` to `^X.Y.0`. Run `npm install` at the root (not
   `npm ci`) to regenerate `package-lock.json`, and commit it with the bump.
2. From the merged release commit with a clean `git status`: `npm run clean && npm run build`.
3. `npm pack --dry-run` in each package folder; check the file list and the version.
4. The registry shows the publishing account's email address to anyone who queries it
   (`npm view <package> maintainers`). Check which address that is before publishing:
   `npm profile get email`, and change it with `npm profile set email <address>` or on
   npmjs.com.
5. Publish in dependency order, `npm.cmd publish` from each folder, each asking for a 2FA
   code: `packages/sdk`, then `packages/react` (its build and its peer need the new SDK),
   then `packages/vite-plugin`, then `packages/create-magelight-view` last (its template
   asks for the other three at the new version).
6. Check: `npm view <package>@X.Y.Z version` for all four, and in a temp folder
   `npm create magelight-view@X.Y.Z T`, then `npm install` and `npm run build` in it.
