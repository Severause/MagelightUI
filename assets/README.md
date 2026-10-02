# assets/

Loose files staged into the runtime dir root (`Data/SKSE/Plugins/Magelight/`)
by `build.ps1`.

## cursor.png

The UI-mode cursor art (WIC-decodable PNG with alpha, up to 512x512). Drawn
at `cursorHeight` px (at 1080p, scaled with resolution) with its
`cursorHotspotX/Y` pixel on the cursor position — all three configurable in
`Magelight.json`; defaults 44 px, hotspot 0.5/1.0 (bottom-centre, the tip of
a map-pin shape). Missing file = the baked arrow.

The intended art is the College of Winterhold insignia in its map-pin
frame — Magelight is a College spell, and the pin's point is a natural
pointer tip.
