// 层：视图
#pragma once
#include <QDialog>
#include "../../qgis/geologicalmaplayout.h"
#include "../../qgis/layoutexport.h"
#include "../../workflow/geologicalexport.h"
#include <functional>
#include <memory>
class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QTimer;
class PaleoLayoutPreview;
class GeologicalExportDialog : public QDialog {
  Q_OBJECT
public:
  // 版面构建回调：#531 后返回 src/qgis 的图件版面句柄——视图层签名与成员
  // 不再出现 Qgs* 类型（QgsPrintLayout / QgsLayoutView 都在封装层）。
  using Builder = std::function<std::unique_ptr<PaleoLayoutExport::LayoutHandle>(
      const PaleoGeologicalMapLayout::Settings &, QString *)>;
  GeologicalExportDialog(const PaleoGeologicalMapLayout::Settings &settings, Builder builder,
      bool hasInputs, QWidget *parent = nullptr, const QStringList &unboundLayers = {}, const QString &projectDirectory = {});
  ~GeologicalExportDialog() override;
  PaleoLayoutExport::LayoutHandle *layout() const;
  PaleoGeologicalExport::Request request() const;
private:
  void rebuild();
  void scheduleRebuild(); // 预览防抖（#540a）：200ms 单发，避免逐键重建版面
  Builder m_builder;
  PaleoGeologicalMapLayout::Settings m_settings;
  std::unique_ptr<PaleoLayoutExport::LayoutHandle> m_layout;
  PaleoLayoutPreview *m_preview;
  QTimer *m_rebuildTimer;
  QComboBox *m_kind, *m_paper, *m_orientation, *m_destinationMode;
  QString m_projectDirectory;
  QLineEdit *m_subtitle;
  QDoubleSpinBox *m_scale;
  QLineEdit *m_title, *m_directory, *m_author, *m_reviewer;
  QSpinBox *m_dpi;
  QCheckBox *m_transparent, *m_world, *m_geoPdf, *m_textPaths;
  QCheckBox *m_png, *m_pdf, *m_svg, *m_tiff, *m_data, *m_sources;
  class QLabel *m_status;
  class QPushButton *m_save;
};
