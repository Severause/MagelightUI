# Font fallback test

Runs Ultralight (the stock SDK runtime, CPU renderer, no window and no game) over one page, once
with the platform font loader as Magelight 0.31.2 used it and once with `MagelightFonts`' loader
(`src/MagelightFonts.cpp`), on four imitated systems:

| System | What the fake loader does |
|---|---|
| `normal` | nothing: the real system fonts |
| `stripped` | no family loads (a PC without Arial, Times New Roman or any other font) |
| `badpath` | every family names a file that does not exist |
| `nonascii` | the family `PathTest` is DejaVu Sans in a folder named `fönts-テスト` |

Each scenario runs in a child process, so a crash is a result rather than the end of the run; the
child names the faulting module and offset. The page measures five spans (an unknown family, the
bundled family's name, `PathTest`, Times New Roman, Arial), and the parent checks:

- on a normal system both loaders measure the same text the same, so nothing changes there;
- `stripped` and `badpath` crash the stock loader at `WebCore.dll +0xFB7F77`, the offset of the
  field log's `MgWCore.dll +0xFB7F77` (MgWCore.dll is the renamed WebCore.dll), and Magelight's
  loader draws them in the bundled font;
- `nonascii`: the stock loader cannot open the font (unless the PC's ANSI code page is UTF-8), and
  Magelight's loader can.

## Build and run

```
tools\font-test\build.bat [outDir]
```

`outDir` defaults to `C:\b\mgl-fonttest`. The script compiles `font_test.cpp` with
`src\MagelightFonts.cpp` and the font resource, copies the SDK runtime (stock names) and
`resources\` beside it, runs the checks and exits 0 when they all pass. It needs `extern\ultralight`
(see `extern\README.md`) and Visual Studio (`..\desktop-harness\vcvars.bat` finds it).
