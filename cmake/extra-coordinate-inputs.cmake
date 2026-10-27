# QGIS 坐标输入、格式、转换与原生范围控件。
target_sources(paleo_qgis PRIVATE
  src/qgis/coordinateformat.cpp
  src/qgis/crsselector.cpp
  src/qgis/coordinateeditor.cpp
  src/qgis/coordinatetoolsdialog.cpp
  src/qgis/coordinatemaptool.cpp
  src/qgis/mapextentwidget.cpp
)
target_sources(paleo_ui PRIVATE
  src/ui/paleomainwindow_coordinates.cpp
  src/ui/datapreview/previewmapcoordinates.cpp
  src/ui/dialogs/provisionalregistrationdialog.cpp
)
add_paleo_test(tst_coordinateinputs LIBS paleo_ui)
