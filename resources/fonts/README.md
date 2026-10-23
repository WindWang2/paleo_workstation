# Vendored Fonts

All fonts are embedded in `resources/paleo_fonts.qrc` and registered once per
application process. Windows and Linux load the same files without installing
system fonts or downloading assets at runtime. Font sizes remain Qt points.
Native OS rasterizers can still differ slightly in antialiasing.

## IBM Plex Sans SC — default UI and new figure text

- Upstream: https://github.com/IBM/plex
- Package: `@ibm/plex-sans-sc` 1.1.0
- Pinned commit: `71f558992315a12ca1f06a2215d57962a1992162`
- Files: complete, unmodified hinted OTF Regular (400), Medium (500),
  SemiBold (600), Bold (700). These are full Chinese fonts, not UI text subsets.
- Exact source URLs, sizes and SHA-256: `ibm-plex.lock.json`.
- Copyright © 2017 IBM Corp., Reserved Font Name "Plex".
- License: SIL Open Font License 1.1, original notice in `OFL-IBMPlex.txt`.

## Noto Sans SC — bundled CJK/legacy fallback

- Regular and Bold, retained for missing glyphs and projects explicitly using Noto.
- Source: Google Fonts / Google Noto Fonts.
- Copyright (c) 2014-2021 Adobe Systems Incorporated, Reserved Font Name "Source".
- License: SIL Open Font License 1.1 (`OFL.txt`, copyright also in font metadata).

## JetBrains Mono — coordinates, depths and numerical columns

- Regular and Bold; tabular numerals remain unchanged.
- Source: https://www.jetbrains.com/lp/mono/
- Copyright (c) 2020 JetBrains s.r.o.
- License: SIL Open Font License 1.1 (`OFL.txt`, copyright also in font metadata).

Font notices are available in Help > About > Show Details and installed under
`share/paleo/licenses/fonts/` (the configured CMake data directory).
