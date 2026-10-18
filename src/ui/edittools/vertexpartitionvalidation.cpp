// 层：视图
#include "vertexeditortools.h"
#include "../../qgis/faciespartitioneditor.h"
bool PaleoVertexTool::validatePartition(QgsVectorLayer *layer) {
  if (!FaciesPartitionEditor::isPartition(layer)) return true;
  QString error;
  if (FaciesPartitionEditor::validate(layer, &error)) return true;
  emit messageEmitted(error, Qgis::MessageLevel::Warning);
  return false;
}
