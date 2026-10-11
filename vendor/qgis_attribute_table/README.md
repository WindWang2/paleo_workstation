# QGIS native attribute table

Source: QGIS **final-4_2_2**, https://github.com/qgis/QGIS/tree/final-4_2_2.
License: GPL-2.0-or-later (upstream copyright headers retained; see COPYING).

The original `QgsAttributeTableDialog` is compiled here because it belongs to
QGIS's application library and requires the `QgisApp` singleton. The embedded
workstation supplies these application entry points through
`PaleoAttributeTableHost`, using the existing map canvas and edit service.

Included upstream files:

- `src/app/qgsattributetabledialog.{h,cpp}`
- `src/app/qgsclipboard.{h,cpp}`
- `src/app/qgsdelattrdialog.{h,cpp}`
- `src/gui/attributetable/qgsfeaturefilterwidget_p.h` (private declaration for
  the implementation already provided by `libqgis_gui`; not recompiled)

The UI generated header, `QgsDualView`, filtering widget, field calculator,
column organizer, expression selection dialog, conditional formatting and
attribute forms are supplied by the selected QGIS installation.

Embedding changes:

- The dialog is a `Qt::Widget` inside the attribute table tab. Dock management,
  XML restoration and close ownership belong to Paleo's surrounding dock.
- `QgisApp` calls are routed through the host. Saves still use the project's
  serialized edit service; the native Save Edits action resumes editing. The
  field calculator enters editing through that service before changing fields.
- App-only feature creation uses QGIS's feature defaults and attribute dialog.
- The clipboard's coordinate transform uses the project's transform context.
- Clipboard collection values use QGIS's exported `encodeValue` JSON serializer
  so the embedded app does not need QGIS's private nlohmann JSON headers on Windows.
- Upstream explicit moc includes are replaced by CMake AUTOMOC.

The layer picker is application chrome; the table's toolbar, filter bar and
form/table view are QGIS's original UI and actions.

`QgisRuntime` registers the original QGIS editor widgets on startup and the
application loads the QGIS Chinese catalog alongside its own translations.
