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
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

GeologicalExportDialog::GeologicalExportDialog(const PaleoGeologicalMapLayout::Settings &settings,
    Builder builder, bool hasInputs, QWidget *parent, const QStringList &unboundLayers, const QString &projectDirectory)
    : QDialog(parent), m_builder(std::move(builder)), m_settings(settings), m_projectDirectory(projectDirectory) {
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
  m_destinationMode = new QComboBox(controls); m_destinationMode->setObjectName("mapExportDestinationMode");
  m_destinationMode->addItem(tr("工区管理（按当前层位登记）"), true); m_destinationMode->addItem(tr("外部位置"), false);
  m_destinationMode->setCurrentIndex(projectDirectory.isEmpty() ? 1 : 0);
  form->addRow(tr("输出到"), m_destinationMode);
  m_directory = new QLineEdit(controls); m_directory->setReadOnly(true); m_directory->setObjectName("mapExportDirectory");
  auto *choose = new QPushButton(tr("选择目录…"), controls);
  form->addRow(tr("保存到"), m_directory); form->addRow(choose);
  connect(choose, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("选择图件导出父目录"), m_directory->text());
    if (!path.isEmpty()) m_directory->setText(path);
    scheduleRebuild();
  });
  const auto destinationChanged = [this, choose] {
    const bool managed = m_destinationMode->currentData().toBool();
    m_directory->setText(managed && !m_projectDirectory.isEmpty() ? QDir(m_projectDirectory).filePath("exports/maps") : QString());
    choose->setEnabled(!managed); choose->setToolTip(managed ? tr("工区图件保存在工程 exports/maps 下，并登记到当前层位") : tr("选择外部输出目录"));
    rebuild();
  };
  m_status = new QLabel(controls); m_status->setWordWrap(true); form->addRow(m_status);
  m_preview = PaleoLayoutExport::createLayoutPreview(splitter); m_preview->setObjectName("mapExportPreview");
  splitter->addWidget(scroll); splitter->addWidget(m_preview); splitter->setStretchFactor(1, 1);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  m_save = buttons->button(QDialogButtonBox::Save); m_save->setText(tr("开始导出"));
  root->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  // 预览重建防抖（#540a）：13 条输入连接全部经 200ms 单发定时器合流——逐键
  // 重建版面会把 GUI 线程占满（与 dataexportdialog.cpp 的预览定时器同口径）。
  m_rebuildTimer = new QTimer(this); m_rebuildTimer->setSingleShot(true); m_rebuildTimer->setInterval(200);
  connect(m_rebuildTimer, &QTimer::timeout, this, [this] { rebuild(); });
  for (auto *combo : {m_kind, m_paper, m_orientation}) connect(combo, &QComboBox::currentIndexChanged, this, [this] { scheduleRebuild(); });
  for (auto *edit : {m_title, m_subtitle, m_author, m_reviewer}) connect(edit, &QLineEdit::editingFinished, this, [this] { scheduleRebuild(); });
  for (auto *box : {m_png, m_pdf, m_svg, m_tiff}) connect(box, &QCheckBox::toggled, this, [this] { scheduleRebuild(); });
  connect(m_dpi, &QSpinBox::valueChanged, this, [this] { scheduleRebuild(); });
  connect(m_scale, &QDoubleSpinBox::valueChanged, this, [this] { scheduleRebuild(); });
  connect(m_destinationMode, &QComboBox::currentIndexChanged, this, destinationChanged);
  resize(1100, 720); destinationChanged();
}
GeologicalExportDialog::~GeologicalExportDialog() { delete m_preview; }
void GeologicalExportDialog::scheduleRebuild() { m_rebuildTimer->start(); }
PaleoLayoutExport::LayoutHandle *GeologicalExportDialog::layout() const { return m_layout.get(); }
void GeologicalExportDialog::rebuild() {
  m_settings.subtitle = m_subtitle->text(); m_settings.scale = m_scale->value();
  m_settings.title = m_title->text(); m_settings.kind = PaleoStandardElements::FigureKind(m_kind->currentIndex());
  m_settings.page.sizeName = m_paper->currentText(); m_settings.page.landscape = m_orientation->currentIndex() == 0;
  m_settings.author = m_author->text(); m_settings.reviewer = m_reviewer->text();
  QString error;
  // QGIS 4.2 setCurrentLayout requires a non-null layout. Bind the replacement
  // while the previous scene is alive, then retire the old scene.
  std::unique_ptr<PaleoLayoutExport::LayoutHandle> replacement(m_builder(m_settings, &error));
  const bool valid = bool(replacement);
  if (replacement) {
    PaleoLayoutExport::setLayoutPreviewLayout(m_preview, replacement->layout());
    m_layout = std::move(replacement);
    PaleoLayoutExport::zoomLayoutPreviewFull(m_preview);
    QTimer::singleShot(0, m_preview, [this] { PaleoLayoutExport::zoomLayoutPreviewFull(m_preview); });
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
  result.registerInProject = m_destinationMode->currentData().toBool();
  result.dpi = m_dpi->value(); result.includeData = m_data->isChecked(); result.includeSources = m_sources->isChecked();
  result.destination = QDir(m_directory->text()).filePath("paleo_map_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + "_" + QUuid::createUuid().toString(QUuid::Id128).left(8));
  return result;
}
