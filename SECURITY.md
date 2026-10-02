# Security policy

Magelight embeds a web engine inside the game and lets third-party mods drive it, so its
sandbox boundaries are security features. If you find a way across one, please report it
privately.

## The boundaries

- **Files.** A page reads its own `Data\Magelight\<modId>\` folder (or its own page directory)
  and the host's runtime dir, nothing else.
- **Network.** By default a page reaches nothing over the network (0.30.0). A mod can opt in
  to loopback (`http(s)` to `localhost`, `127.x.x.x` or `[::1]`) from its manifest
  (`"network": "loopback"`), from Papyrus (`SetNetworkPolicy`) or from its DLL, and to any
  host only from its DLL (`SetNetworkPolicy(mod, NetworkPolicy::Any)`). A manifest asking for
  `"any"` is refused. Every opt-in is logged. Loopback reaches every local service on the
  machine, not only the mod's own, so a mod opts in only when it needs a local server.
- **Storage.** By default each mod's pages get their own localStorage/IndexedDB/cookie jar,
  under `My Games\<game>\SKSE\Magelight-cache\<modId>\`. Jars are shared by name: a mod that
  asks for `"default"` shares the jar all v1-v3 views use, and a plugin that gives its mod a
  custom session name shares that jar with every mod that names it and with any mod whose
  modId is that name. A mod whose storage must stay out of other mods' pages keeps the
  isolated default. A mod's jar belongs to the game install: every save, character and
  mod-manager profile shares it, it is not stored in the save, and loading an older save does
  not roll it back.
- **Script ownership** (0.30.0). Papyrus has no caller identity, so the host sorts mods into
  two kinds. A mod registered from a DLL (`RegisterMod`/`RegisterModEx`, including a manifest
  mod a DLL adopted) is **plugin-owned**; a mod registered from Papyrus, or declared by a
  manifest no DLL adopted, is **script-owned**. Scripts may act only on script-owned mods:
  Papyrus refuses to register, create views under, release UI mode for or set the network
  policy of a plugin-owned slug, and every Papyrus call that acts on a plugin-owned mod's view
  does nothing. The same holds for a script-owned mod whose storage jar a plugin uses (the
  `"default"` jar, or a plugin's custom session name). The host records every jar a plugin
  has used in `Magelight-cache\plugin jars.txt` and keeps scripts out of those jars in
  later sessions too, even when that plugin is not installed. Deleting a jar's line (or the file)
  with the game closed hands that jar back to scripts. Read-only calls (`FindView`,
  `IsVisible`, `IsUIModeActive`, `GetUIModeView`, `GetLastError`, `GetVersion*`, `IsReady`)
  work for any mod.
- **Page calls** (0.30.0). A page's `window.magelight.send` and listener calls reach only the
  view the page is in; a page cannot fire another view's listeners or type into it.
- **What the script tier shares.** Because Papyrus cannot tell one script from another, any
  script can drive any script-owned mod's views: show and hide them, run JavaScript in them
  and read their page storage back. The page storage and page JavaScript of a script-owned
  mod are only as private as the script tier. Keep secrets (API keys, tokens, passwords) out
  of page storage.
- **Sound.** Pages play the game's own sound descriptors by name, never a file: file playback
  would be a new attack surface, and it would bypass the player's audio settings.

## What to report

- **File-read pin escapes** — a page reading files outside the folders above (e.g. via URL
  trickery, path traversal, junctions).
- **Network sandbox escapes** — a page reaching the network beyond what its mod opted into:
  anything at all under the default, a non-loopback host under a loopback opt-in, or the
  loopback check bypassed by URL parsing tricks.
- **Storage isolation breaches** — one mod's page reading another mod's isolated
  localStorage/IndexedDB/cookies (sessions). Jars shared by name, as described above, are
  not breaches.
- **Script-ownership escapes** — a Papyrus script acting on a plugin-owned mod, or getting a
  plugin-owned mod's network reach, or the storage of a plugin-owned mod that keeps the
  isolated default, by any route.
- **Page-identity escapes** — a page calling another view's listeners, sounds or keys, or
  acting as another view in any other way.
- **Bridge injection** — mod- or page-supplied strings (view names, listener names,
  payloads) breaking out of the generated page script into unintended JS execution in
  another view.
- **Trust-model violations beyond the documented cooperative model** — the C ABI
  intentionally lets any DLL drive any view by id (see "Trust model" in docs/CPP.md), and
  any script can drive any script-owned mod's views (above); both are documented, not
  vulnerabilities. Crossing them *by stealth* (e.g. forging another mod's identity) is.

## What not to report here

- Game crashes, mod conflicts, blank pages — file a regular
  [issue](https://github.com/Severause/MagelightUI/issues) (see TROUBLESHOOTING.md first).
- Vulnerabilities in Ultralight/WebKit itself — report them to Ultralight, Inc.; we ship
  their engine. Tell us too if the fix requires a Magelight change.
- Issues in mods that *consume* Magelight — report those to the mod's author.

## How to report

Use **GitHub private vulnerability reporting** on this repository (Security tab →
"Report a vulnerability"). Include the host version, a description of the boundary and
the bypass, and a proof of concept (a small manifest mod or page is ideal).

If that button is missing, open a minimal public
[issue](https://github.com/Severause/MagelightUI/issues) that only asks for a private
contact. Put no details of the problem in it; we will reply with a private channel.

You can expect an acknowledgement within 7 days. We will confirm the issue, develop a
fix, and credit you in the release notes if you want — please give us a reasonable
window to ship the fix before public disclosure, since users can only be protected by
updating.

## Supported versions

The latest release only. The host tells consumers its version at startup
(`Magelight.log` line one) and gates them with `minHost`, so fixes ride the normal
release channel.
