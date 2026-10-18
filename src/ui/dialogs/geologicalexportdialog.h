// 层：视图
#pragma once
#include <QDialog>
#include "../../qgis/geologicalmaplayout.h"
#include "../../workflow/geologicalexport.h"
#include <functional>
#include <memory>
class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QgsLayoutView;
class GeologicalExportDialog : public QDialog {
  Q_OBJECT
public:
  using Builder = std::function<QgsPrintLayout *(const PaleoGeologicalMapLayout::Settings &, QString *)>;
  GeologicalExportDialog(const PaleoGeologicalMapLayout::Settings &settings, Builder builder,
      bool hasInputs, QWidget *parent = nullptr, const QStringList &unboundLayers = {});
  ~GeologicalExportDialog() override;
  QgsPrintLayout *layout() const;
  PaleoGeologicalExport::Request request() const;
private:
  void rebuild();
  Builder m_builder;
  PaleoGeologicalMapLayout::Settings m_settings;
  std::unique_ptr<QgsPrintLayout> m_layout;
  QgsLayoutView *m_preview;
  QComboBox *m_kind, *m_paper, *m_orientation;
  QLineEdit *m_subtitle;
  QDoubleSpinBox *m_scale;
  QLineEdit *m_title, *m_directory, *m_author, *m_reviewer;
  QSpinBox *m_dpi;
  QCheckBox *m_transparent, *m_world, *m_geoPdf, *m_textPaths;
  QCheckBox *m_png, *m_pdf, *m_svg, *m_tiff, *m_data, *m_sources;
  class QLabel *m_status;
  class QPushButton *m_save;
};
