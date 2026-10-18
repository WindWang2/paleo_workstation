// 层：视图
#include "geologicalexportdialog.h"
#include "../paleotheme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSplitter>
#include <QScrollArea>
#include <QUuid>
#include <QVBoxLayout>
#include <qgslayoutview.h>
#include <qgsprintlayout.h>

GeologicalExportDialog::GeologicalExportDialog(const PaleoGeologicalMapLayout::Settings &settings,
    Builder builder, bool hasInputs, QWidget *parent, const QStringList &unboundLayers)
    : QDialog(parent), m_builder(std::move(builder)), m_settings(settings) {
  setObjectName("geologicalExportDialog"); setWindowTitle(tr("导出地质图"));
  auto *root = new QVBoxLayout(this); root->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *splitter = new QSplitter(this); root->addWidget(splitter, 1);
  auto *scroll = new QScrollArea(splitter); scroll->setWidgetResizable(true); scroll->setMinimumWidth(360);
  auto *controls = new QWidget(scroll); scroll->setWidget(controls); auto *form = new QFormLayout(controls);
  m_title = new QLineEdit(settings.title, controls); m_title->setObjectName("mapExportTitle");
  m_subtitle = new QLineEdit(settings.subtitle, controls);
  m_scale = new QDoubleSpinBox(controls); m_scale->setRange(0, 1e10); m_scale->setDecimals(0);
  m_scale->setSpecialValueText(tr("按当前地图范围")); m_scale->setPrefix("1 : ");
  m_kind = new QComboBox(controls); m_kind->addItems({tr("井位图"), tr("单因素图"), tr("沉积相图")}); m_kind->setCurrentIndex(int(settings.kind));
  m_paper = new QComboBox(controls); m_paper->addItems({"A4", "A3"});
  m_orientation = new QComboBox(controls); m_orientation->addItems({tr("横向"), tr("纵向")});
  m_dpi = new QSpinBox(controls); m_dpi->setObjectName("mapExportDpi"); m_dpi->setRange(72, 1200); m_dpi->setValue(300);
  m_author = new QLineEdit(settings.author, controls); m_reviewer = new QLineEdit(settings.reviewer, controls);
  form->addRow(tr("图名"), m_title); form->addRow(tr("层位／时代及说明"), m_subtitle); form->addRow(tr("比例尺"), m_scale); form->addRow(tr("图件类型"), m_kind); form->addRow(tr("纸张"), m_paper);
  form->addRow(tr("方向"), m_orientation); form->addRow(tr("分辨率（DPI）"), m_dpi);
  form->addRow(tr("编制"), m_author); form->addRow(tr("审核"), m_reviewer);
  auto *formats = new QWidget(controls); auto *formatLayout = new QHBoxLayout(formats); formatLayout->setContentsMargins(0, 0, 0, 0);
  m_png = new QCheckBox(tr("PNG"), formats); m_pdf = new QCheckBox(tr("PDF"), formats); m_svg = new QCheckBox(tr("SVG"), formats); m_tiff = new QCheckBox(tr("TIFF"), formats);
  m_png->setObjectName("mapExportPng"); m_pdf->setObjectName("mapExportPdf"); m_svg->setObjectName("mapExportSvg");
  m_pdf->setChecked(true); m_tiff->setEnabled(PaleoLayoutExport::formatAvailable(PaleoLayoutExport::Format::Tiff));
  if (!m_tiff->isEnabled()) m_tiff->setToolTip(tr("当前环境缺少 TIFF 编码器"));
  for (auto *box : {m_png, m_pdf, m_svg, m_tiff}) formatLayout->addWidget(box);
  form->addRow(tr("输出格式（可多选）"), formats);
  m_transparent = new QCheckBox(tr("PNG / TIFF 使用透明纸面"), controls);
  m_world = new QCheckBox(tr("图像附带坐标定位文件与 CRS"), controls);
  m_geoPdf = new QCheckBox(tr("PDF 写入地理配准信息（GeoPDF）"), controls);
  m_textPaths = new QCheckBox(tr("PDF / SVG 文字转轮廓（取消后保留文字）"), controls); m_textPaths->setChecked(true);
  QString reason;
  m_geoPdf->setEnabled(PaleoLayoutExport::geoPdfAvailable(&reason));
  if (!reason.isEmpty()) m_geoPdf->setToolTip(reason);
  form->addRow(m_transparent); form->addRow(m_world); form->addRow(m_geoPdf); form->addRow(m_textPaths);
  m_data = new QCheckBox(tr("附带所选图层的成果数据"), controls);
  m_sources = new QCheckBox(tr("附带已登记的制作源数据"), controls);
  m_data->setEnabled(true); m_sources->setEnabled(false);
  if (!hasInputs) m_sources->setToolTip(tr("当前图层没有可追溯的资产版本；可在数据管理中导出数据"));
  connect(m_data, &QCheckBox::toggled, this, [this, hasInputs](bool on) { m_sources->setEnabled(on && hasInputs); if (!on) m_sources->setChecked(false); });
  form->addRow(m_data); form->addRow(m_sources);
  if (!unboundLayers.isEmpty()) {
    auto *coverage = new QLabel(tr("以下图层没有资产版本，源数据包仅含已登记来源：%1").arg(unboundLayers.join("、")), controls);
    coverage->setWordWrap(true); form->addRow(coverage);
  }
  m_directory = new QLineEdit(controls); m_directory->setReadOnly(true); m_directory->setObjectName("mapExportDirectory");
  auto *choose = new QPushButton(tr("选择目录…"), controls);
  form->addRow(tr("保存到"), m_directory); form->addRow(choose);
  connect(choose, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("选择图件导出父目录"), m_directory->text());
    if (!path.isEmpty()) m_directory->setText(path);
    rebuild();
  });
  m_status = new QLabel(controls); m_status->setWordWrap(true); form->addRow(m_status);
  m_preview = new QgsLayoutView(splitter); m_preview->setObjectName("mapExportPreview");
  splitter->addWidget(scroll); splitter->addWidget(m_preview); splitter->setStretchFactor(1, 1);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  m_save = buttons->button(QDialogButtonBox::Save); m_save->setText(tr("开始导出"));
  root->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  for (auto *combo : {m_kind, m_paper, m_orientation}) connect(combo, &QComboBox::currentIndexChanged, this, [this] { rebuild(); });
  for (auto *edit : {m_title, m_subtitle, m_author, m_reviewer}) connect(edit, &QLineEdit::editingFinished, this, [this] { rebuild(); });
  for (auto *box : {m_png, m_pdf, m_svg, m_tiff}) connect(box, &QCheckBox::toggled, this, [this] { rebuild(); });
  connect(m_dpi, &QSpinBox::valueChanged, this, [this] { rebuild(); });
  connect(m_scale, &QDoubleSpinBox::valueChanged, this, [this] { rebuild(); });
  resize(1100, 720); rebuild();
}
GeologicalExportDialog::~GeologicalExportDialog() { delete m_preview; }
QgsPrintLayout *GeologicalExportDialog::layout() const { return m_layout.get(); }
void GeologicalExportDialog::rebuild() {
  m_settings.subtitle = m_subtitle->text(); m_settings.scale = m_scale->value();
  m_settings.title = m_title->text(); m_settings.kind = PaleoStandardElements::FigureKind(m_kind->currentIndex());
  m_settings.page.sizeName = m_paper->currentText(); m_settings.page.landscape = m_orientation->currentIndex() == 0;
  m_settings.author = m_author->text(); m_settings.reviewer = m_reviewer->text();
  QString error;
  // QGIS 4.2 setCurrentLayout requires a non-null layout. Bind the replacement
  // while the previous scene is alive, then retire the old scene.
  std::unique_ptr<QgsPrintLayout> replacement(m_builder(m_settings, &error));
  const bool valid = bool(replacement);
  if (replacement) {
    m_preview->setCurrentLayout(replacement.get());
    m_layout = std::move(replacement);
    m_preview->zoomFull();
  }
  const bool format = m_png->isChecked() || m_pdf->isChecked() || m_svg->isChecked() || m_tiff->isChecked();
  m_save->setEnabled(valid && format && !m_directory->text().isEmpty());
  const double width = (m_settings.page.sizeName == "A3" ? 420. : 297.) * m_dpi->value() / 25.4;
  const double height = (m_settings.page.sizeName == "A3" ? 297. : 210.) * m_dpi->value() / 25.4;
  m_status->setText(error.isEmpty() ? tr("栅格图像约 %1 × %2 像素；每张渲染约 %3 MB。\n范围取当前地图。输出到新批次目录，附版面与版本说明。")
      .arg(qRound(m_settings.page.landscape ? width : height)).arg(qRound(m_settings.page.landscape ? height : width)).arg(qRound(width * height * 4 / 1024 / 1024)) : error);
}
PaleoGeologicalExport::Request GeologicalExportDialog::request() const {
  PaleoGeologicalExport::Request result;
  result.formats.clear();
  for (const auto &pair : {qMakePair(m_pdf, PaleoLayoutExport::Format::Pdf), qMakePair(m_png, PaleoLayoutExport::Format::Png),
       qMakePair(m_svg, PaleoLayoutExport::Format::Svg), qMakePair(m_tiff, PaleoLayoutExport::Format::Tiff)})
    if (pair.first->isChecked()) result.formats << pair.second;
  result.options.transparentBackground = m_transparent->isChecked();
  result.options.worldFile = m_world->isChecked(); result.options.geoPdf = m_geoPdf->isChecked(); result.options.textAsPaths = m_textPaths->isChecked();
  result.dpi = m_dpi->value(); result.includeData = m_data->isChecked(); result.includeSources = m_sources->isChecked();
  result.destination = QDir(m_directory->text()).filePath("paleo_map_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + "_" + QUuid::createUuid().toString(QUuid::Id128).left(8));
  return result;
}
