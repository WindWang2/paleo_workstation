# vendor/sbm — Seismic-Body-Management engine

Vendored copy of the seismic engine from
https://github.com/changyanyanchang/Seismic-Body-Management (SeismicF3Viewer),
pinned at upstream commit `aae56c77f8233e206523717ad8fdd854b3a5156e`
("Fix paged workspace progress and slice rendering").

## What is vendored

| Path | Content |
| --- | --- |
| `src/Data/Sgy/` | SEG-Y domain layer (index, rule layout, sequential scan, volume, caches) |
| `src/Engine/` | Headless engine pipeline (workspace format, paged backend, LOD, transcode, `sdk::Dataset` facade, `SdkC` ABI) |
| `src/Render/ColorMap.*` | Pure colormap math (no GL) |

Not vendored: `App/` (GLFW shell), `UI/` (ImGui), `Mesh/`, `Model/`,
`Platform/`, `Core/`, `main.cpp`, `ThirdParty/` (we use our own
`vendor/segyio` + `vendor/glm`, same versions).

The upstream `Engine` includes resolve via include root `vendor/sbm/src`,
e.g. `#include "Data/Sgy/SgyIndex.h"`. Do not re-root these includes; they are
upstream-canonical.

## Local patches (see PATCHES.md)

`PATCHES.md` lists the deltas applied on top of upstream — mostly the
`paleo_workstation` improvements that predated vendoring (companion `.sgyidx`
files, mmap/parallel `ReadSlice`, `sgyio::ToUtf8Path`) plus POSIX fixes.

## Upstream sync

Replace `src/` with a newer upstream snapshot, re-apply `PATCHES.md`, update
the pinned commit above.

## License

Upstream publishes no top-level license; `licenses/` upstream covers only its
third-party deps. Internal project asset — keep it that way until upstream
clarifies terms. Vendored `segyio`/`glm` retain their own licenses.
