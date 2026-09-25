# paleo_workstation

## Design System
Always read DESIGN.md before making any visual or UI decisions.
All font choices, colors, spacing, and aesthetic direction are defined there.
Do not deviate without explicit user approval.
In QA mode, flag any code that doesn't match DESIGN.md.

## Architecture
The authoritative architecture plan is `docs/PALEO_QGIS_PLAN.md`
(vendored QGIS C++ embed; QGIS owns GIS plumbing, Paleo owns geological semantics).
Deferred work lives in `TODOS.md`.
