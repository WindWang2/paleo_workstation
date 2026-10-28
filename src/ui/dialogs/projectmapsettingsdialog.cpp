// 层：视图
#include "projectmapsettingsdialog.h"
#include "ui/paleotheme.h"
#include "qgis/crsselector.h"
#include "qgis/coordinateeditor.h"
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
#include <QToolButton>
#include <QVBoxLayout>

namespace
{

// 度米系数的物理合理下限（米/度）：地球上 1° 纬度 ≈ 110.6-111.7 km、1° 经度
// ≤ 111.3 km。低于该值的配准换算必然失败——isComplete 只查 > 0，spinbox 下限
// 静默夹出的 1e-9 能过检，落库一份「形式完整、换算必败」的配准。新建配准
// 在 accept 处按此下限拦，不靠钳位值糊过去。
constexpr double kMinMetersPerDegree = 1.0e3;

} // namespace

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
  const bool native = g.kind == QLatin1String("crs");
  m_registered->setEnabled(!native);
  m_registered->setToolTip(native ? tr("原生投影 CRS 是现有工程的计算基准，不能在此清除；如需更换，请新建工程并重新导入。")
                                  : tr("启用后，以明确参数将局部米制网格配准到 WGS84。"));
  if (native) {
    reference->setTitle(tr("工程原生坐标系"));
    description->setText(tr("地质数据使用已声明的原生投影坐标；地图按精确投影转换显示，不需要再拟合局部网格配准。"));
    auto *source = new QLabel(g.sourceCrs, reference); source->setWordWrap(true);
    fields->addRow(tr("资料／计算坐标系"), source);
  }
  if (!configuration.georeference) g.a = 1;
  // 无既有配准 = 首次启用：度米系数不预填钳位值（原实现下限 1e-9 会把 0 静默
  // 夹成 1e-9 并原样存回），改留 0 + 特殊文案明示必填，accept 处再拦。
  const bool firstRegistration = !configuration.georeference.has_value();
  const QStringList captions = {tr("a（缩放 × cos 旋转角）"), tr("b（缩放 × sin 旋转角）"),
      tr("东向平移 tE（米）"), tr("北向平移 tN（米）"),
      tr("每经度对应米数"), tr("每纬度对应米数")};
  const QVector<double> values = {g.a, g.b, g.tE, g.tN,
                                 g.metersPerDegLon, g.metersPerDegLat};
  for (int i = 0; i < values.size(); ++i) {
    auto *spin = new QDoubleSpinBox(reference);
    spin->setRange(i >= 4 ? (firstRegistration ? 0.0 : 1e-9) : -1e9, 1e9);
    if (i >= 4 && firstRegistration)
      spin->setSpecialValueText(tr("未设置（必填）"));
    spin->setDecimals(12);
    spin->setSuffix(i == 2 || i == 3 ? tr(" m") : i >= 4 ? tr(" m/°") : QString());
    spin->setValue(values[i]);
    spin->setFont(PaleoTheme::monoFont());
    spin->setObjectName(QStringLiteral("georeferenceParameter%1").arg(i < 4 ? i : i + 2));
    spin->setEnabled(m_registered->isChecked());
    if (!native) fields->addRow(captions[i], spin);
    else spin->hide();
    m_parameters << spin;
    connect(m_registered, &QCheckBox::toggled, spin, &QWidget::setEnabled);
  }
  m_anchor = new PaleoCoordinateEditor(reference);
  m_anchor->setObjectName(QStringLiteral("georeferenceAnchorEditor"));
  m_anchor->setSourceCrs(paleo::coordinates::crsFromDefinition(QStringLiteral("EPSG:4326")));
  m_anchor->sourceSelector()->widget()->setEnabled(false);
  m_anchor->sourceSelector()->widget()->setToolTip(tr("局部网格配准的目标固定为 WGS84 经纬度。"));
  m_anchor->xEdit()->setObjectName(QStringLiteral("georeferenceParameter4"));
  m_anchor->yEdit()->setObjectName(QStringLiteral("georeferenceParameter5"));
  m_anchor->xEdit()->setFont(PaleoTheme::monoFont());
  m_anchor->yEdit()->setFont(PaleoTheme::monoFont());
  m_anchor->setPoint(paleo::coordinates::point(g.anchorLonDeg, g.anchorLatDeg));
  m_anchor->mapPickButton()->hide();
  m_anchor->widget()->setEnabled(m_registered->isChecked());
  if (native) m_anchor->widget()->hide();
  else fields->addRow(tr("WGS84 锚点"), m_anchor->widget());
  connect(m_registered, &QCheckBox::toggled, m_anchor->widget(), &QWidget::setEnabled);
  auto *formula = new QLabel(tr("E = a·x − b·y + tE；N = b·x + a·y + tN\n经度 = 锚点经度 + E / 每经度米数\n纬度 = 锚点纬度 + N / 每纬度米数"), reference);
  formula->setWordWrap(true);
  if (!native) fields->addRow(formula);
  else formula->hide();
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
  m_crs = new PaleoCrsSelector(true, map);
  m_crs->setObjectName(QStringLiteral("projectMapCrsSelector"));
  m_crs->setNotSetText(tr("局部工程网格（米，未投影）"));
  m_crs->setDefinition(configuration.mapCrs);
  mapForm->addRow(tr("地图坐标系"), m_crs->widget());
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
      const auto selected = QFileDialog::getOpenFileName(this, tr("选择已下载的离线底图"), edit->text(), tr("离线栅格底图 (*.mbtiles *.tif *.tiff *.img *.gpkg *.png *.jpg *.jpeg)"));
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
  connect(buttons, &QDialogButtonBox::accepted, this,
          [this, firstRegistration, captions] {
    QString error;
    if (!m_crs->validate(&error))
    {
      showError(error);
      return;
    }
    if (m_registered->isChecked() &&
        m_original.georeference.value_or(PaleoGeoreference{}).kind != QLatin1String("crs"))
    {
      // 首次启用配准：度米系数必须显式给出且物理合理（不靠 spinbox 钳位值
      // 蒙过去——1e-9 能过 isComplete 的 > 0 检查，换算却必然失败）。
      if (firstRegistration)
      {
        for (const int index : {4, 5})
        {
          if (m_parameters[index]->value() >= kMinMetersPerDegree)
            continue;
          showError(tr("「%1」须按工区实际填写（≥ %2 米/度），否则换算必然失败。")
                        .arg(captions[index])
                        .arg(kMinMetersPerDegree, 0, 'g', 3));
          return;
        }
      }
      auto anchor = paleo::coordinates::point();
      if (!m_anchor->sourcePoint(&anchor, &error))
      {
        m_anchor->showError(error);
        showError(error);
        return;
      }
    }
    emit saveRequested();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

PaleoProjectFile ProjectMapSettingsDialog::configuration() const
{
  auto file = m_original;
  file.mapCrs = m_crs->definition();
  file.basemapEnabled = m_basemapEnabled->isChecked();
  file.basemapTopo = m_topo->text().trimmed();
  file.basemapHillshade = m_hillshade->text().trimmed();
  if (m_registered->isChecked()) {
    auto g = file.georeference.value_or(PaleoGeoreference{});
    g.targetCrs = QStringLiteral("EPSG:4326");
    if (g.kind != QLatin1String("crs")) {
      auto anchor = paleo::coordinates::point();
      if (!m_anchor->sourcePoint(&anchor)) return m_original;
      g.a = m_parameters[0]->value(); g.b = m_parameters[1]->value();
      g.tE = m_parameters[2]->value(); g.tN = m_parameters[3]->value();
      g.anchorLonDeg = anchor.x(); g.anchorLatDeg = anchor.y();
      g.metersPerDegLon = m_parameters[4]->value(); g.metersPerDegLat = m_parameters[5]->value();
    }
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
