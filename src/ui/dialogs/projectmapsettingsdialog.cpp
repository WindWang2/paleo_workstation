// 层：视图
#include "projectmapsettingsdialog.h"
#include "ui/paleotheme.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <qgsprojectionselectionwidget.h>

ProjectMapSettingsDialog::ProjectMapSettingsDialog(const PaleoProjectFile &configuration, QWidget *parent)
  : QDialog(parent), m_original(configuration)
{
  setWindowTitle(tr("工程坐标与底图"));
  setObjectName(QStringLiteral("projectMapSettingsDialog"));
  resize(620, 700);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd,
                             PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll);
  auto *form = new QVBoxLayout(content);
  form->setSpacing(PaleoTheme::tokens().spacingMd);
  auto *description = new QLabel(tr("参数保存在 project.paleo。原始地质数据使用局部米制网格；地图按配准参数显示真实位置。"), content);
  description->setWordWrap(true);
  form->addWidget(description);
  auto *reference = new QGroupBox(tr("局部网格 → WGS84 经纬度"), content);
  auto *fields = new QFormLayout(reference);
  m_registered = new QCheckBox(tr("启用工程坐标配准"), reference);
  m_registered->setChecked(configuration.georeference.has_value());
  fields->addRow(m_registered);
  PaleoGeoreference g = configuration.georeference.value_or(PaleoGeoreference{});
  if (!configuration.georeference) g.a = 1;
  const QStringList captions = {tr("a（缩放 × cos 旋转角）"), tr("b（缩放 × sin 旋转角）"),
      tr("东向平移 tE（米）"), tr("北向平移 tN（米）"), tr("锚点经度（度）"),
      tr("锚点纬度（度）"), tr("每经度对应米数"), tr("每纬度对应米数")};
  const QVector<double> values = {g.a, g.b, g.tE, g.tN, g.anchorLonDeg, g.anchorLatDeg,
                                 g.metersPerDegLon, g.metersPerDegLat};
  for (int i = 0; i < values.size(); ++i) {
    auto *spin = new QDoubleSpinBox(reference);
    spin->setRange(-1e9, 1e9);
    spin->setDecimals(12);
    spin->setValue(values[i]);
    spin->setFont(PaleoTheme::monoFont());
    spin->setObjectName(QStringLiteral("georeferenceParameter%1").arg(i));
    spin->setEnabled(m_registered->isChecked());
    fields->addRow(captions[i], spin);
    m_parameters << spin;
    connect(m_registered, &QCheckBox::toggled, spin, &QWidget::setEnabled);
  }
  auto *formula = new QLabel(tr("E = a·x − b·y + tE；N = b·x + a·y + tN\n经度 = 锚点经度 + E / 每经度米数\n纬度 = 锚点纬度 + N / 每纬度米数"), reference);
  formula->setWordWrap(true);
  fields->addRow(formula);
  m_provenance = new QLineEdit(g.provenance, reference);
  fields->addRow(tr("参数来源"), m_provenance);
  if (!g.controlPoints.isEmpty()) {
    auto *precision = new QLabel(tr("现有控制点：%1 个；最大残差：%2 米。保存修改时重新计算残差。")
                                    .arg(g.controlPoints.size()).arg(g.maxResidualM, 0, 'f', 1), reference);
    precision->setWordWrap(true);
    fields->addRow(precision);
  }
  form->addWidget(reference);
  auto *map = new QGroupBox(tr("地图显示与离线底图"), content);
  auto *mapForm = new QFormLayout(map);
  m_crs = new QgsProjectionSelectionWidget(map);
  m_crs->setCrs(QgsCoordinateReferenceSystem(configuration.mapCrs));
  mapForm->addRow(tr("地图坐标系"), m_crs);
  m_basemapEnabled = new QCheckBox(tr("显示离线底图"), map);
  m_basemapEnabled->setChecked(configuration.basemapEnabled);
  mapForm->addRow(m_basemapEnabled);
  const auto fileField = [this, map, mapForm](const QString &caption, const QString &path) {
    auto *row = new QWidget(map);
    auto *line = new QHBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    auto *edit = new QLineEdit(path, row);
    auto *browse = new QPushButton(tr("选择…"), row);
    line->addWidget(edit, 1); line->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this, edit] {
      const auto selected = QFileDialog::getOpenFileName(this, tr("选择已下载的离线底图"), edit->text(), tr("离线瓦片 (*.mbtiles)"));
      if (!selected.isEmpty()) edit->setText(selected);
    });
    mapForm->addRow(caption, row);
    return edit;
  };
  m_topo = fileField(tr("地形底图"), configuration.basemapTopo);
  m_hillshade = fileField(tr("地形阴影"), configuration.basemapHillshade);
  form->addWidget(map);
  form->addStretch();
  scroll->setWidget(content); layout->addWidget(scroll, 1);
  m_error = new QLabel(this); m_error->setWordWrap(true); m_error->hide();
  layout->addWidget(m_error);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &ProjectMapSettingsDialog::saveRequested);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

PaleoProjectFile ProjectMapSettingsDialog::configuration() const
{
  auto file = m_original;
  file.mapCrs = m_crs->crs().authid().isEmpty() ? m_crs->crs().toWkt() : m_crs->crs().authid();
  file.basemapEnabled = m_basemapEnabled->isChecked();
  file.basemapTopo = m_topo->text().trimmed();
  file.basemapHillshade = m_hillshade->text().trimmed();
  if (m_registered->isChecked()) {
    auto g = file.georeference.value_or(PaleoGeoreference{});
    g.targetCrs = QStringLiteral("EPSG:4326");
    g.a = m_parameters[0]->value(); g.b = m_parameters[1]->value();
    g.tE = m_parameters[2]->value(); g.tN = m_parameters[3]->value();
    g.anchorLonDeg = m_parameters[4]->value(); g.anchorLatDeg = m_parameters[5]->value();
    g.metersPerDegLon = m_parameters[6]->value(); g.metersPerDegLat = m_parameters[7]->value();
    g.provenance = m_provenance->text();
    file.georeference = g;
  } else file.georeference.reset();
  return file;
}

void ProjectMapSettingsDialog::showError(const QString &error)
{
  m_error->setText(tr("保存失败：%1").arg(error));
  m_error->show();
}
