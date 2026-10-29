// 层：QGIS 封装
#include "coordinateeditor.h"
#include "crsselector.h"
#include "qgisprojectservice.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QToolButton>
#include <QWidget>
#include <QVBoxLayout>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgsproject.h>

PaleoCoordinateEditor::PaleoCoordinateEditor(QWidget *parent)
  : QObject(parent), m_widget(new QWidget(parent))
{
  auto *root = new QVBoxLayout(m_widget);
  root->setContentsMargins(0, 0, 0, 0);
  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  m_source = new PaleoCrsSelector(false, m_widget);
  m_source->setObjectName(QStringLiteral("coordinateSourceCrs"));
  form->addRow(tr("来源坐标系"), m_source->widget());
  m_format = new QComboBox(m_widget);
  m_format->setObjectName(QStringLiteral("coordinateFormat"));
  m_format->addItem(tr("十进制度／平面坐标"), static_cast<int>(paleo::coordinates::OrdinateFormat::Decimal));
  m_format->addItem(tr("度分（仅经纬度）"), static_cast<int>(paleo::coordinates::OrdinateFormat::DegreesMinutes));
  m_format->addItem(tr("度分秒（仅经纬度）"), static_cast<int>(paleo::coordinates::OrdinateFormat::DegreesMinutesSeconds));
  form->addRow(tr("输入格式"), m_format);
  root->addLayout(form);
  auto *grid = new QGridLayout;
  grid->setColumnStretch(1, 1);
  m_xCaption = new QLabel(tr("X／东坐标"), m_widget);
  m_yCaption = new QLabel(tr("Y／北坐标"), m_widget);
  m_x = new QLineEdit(m_widget);
  m_y = new QLineEdit(m_widget);
  m_x->setObjectName(QStringLiteral("coordinateXEdit"));
  m_y->setObjectName(QStringLiteral("coordinateYEdit"));
  m_x->setClearButtonEnabled(true);
  m_y->setClearButtonEnabled(true);
  m_mapPick = new QToolButton(m_widget);
  m_mapPick->setObjectName(QStringLiteral("coordinateMapPickButton"));
  m_mapPick->setIcon(QgsApplication::getThemeIcon(QStringLiteral("mActionIdentify.svg")));
  m_mapPick->setText(tr("地图选点"));
  m_mapPick->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_mapPick->setToolTip(tr("在地图上单击拾取坐标，Esc 或右键取消；地图拾取不会自动保存业务数据。"));
  m_mapPick->setAutoRaise(true);
  grid->addWidget(m_xCaption, 0, 0);
  grid->addWidget(m_x, 0, 1);
  grid->addWidget(m_mapPick, 0, 2, 2, 1);
  grid->addWidget(m_yCaption, 1, 0);
  grid->addWidget(m_y, 1, 1);
  root->addLayout(grid);
  m_error = new QLabel(m_widget);
  m_error->setObjectName(QStringLiteral("coordinateError"));
  m_error->setWordWrap(true);
  m_error->hide();
  root->addWidget(m_error);
  for (auto *edit : {m_x, m_y})
  {
    edit->setMinimumWidth(0);
    connect(edit, &QLineEdit::textEdited, this, [this] {
      m_error->hide();
      emit pointChanged();
    });
  }
  connect(m_source, &PaleoCrsSelector::definitionChanged, this, [this](const QString &) {
    updateFormat();
    emit pointChanged();
  });
  connect(m_format, &QComboBox::activated, this, [this](int) { updateFormat(); });
  connect(m_mapPick, &QToolButton::clicked, this, [this] { emit mapPickRequested(this); });
  setMapCanvas(nullptr);
  updateFormat();
}

PaleoCoordinateEditor::~PaleoCoordinateEditor()
{
  delete m_widget.data();
}

QWidget *PaleoCoordinateEditor::widget() const
{
  return m_widget.data();
}

void PaleoCoordinateEditor::setSourceCrs(const QgsCoordinateReferenceSystem &crs)
{
  m_source->setDefinition(paleo::coordinates::crsDefinition(crs));
  updateFormat();
}

void PaleoCoordinateEditor::setTargetCrs(const QgsCoordinateReferenceSystem &crs,
                                          const QgsCoordinateTransformContext &context)
{
  m_targetCrs = crs;
  m_context = context;
}

void PaleoCoordinateEditor::setProjectStorage(QgisProjectService *project)
{
  auto storage = paleo::coordinates::engineeringGrid();
  if (project && project->georeference() && project->georeference()->kind == QLatin1String("crs"))
    storage = paleo::coordinates::crsFromDefinition(project->georeference()->sourceCrs);
  setSourceCrs(storage);
  setTargetCrs(storage, project && project->project()
      ? project->project()->transformContext() : QgsCoordinateTransformContext());
}

QString PaleoCoordinateEditor::targetCrsDefinition() const
{
  return m_targetCrs.isValid() ? paleo::coordinates::crsDefinition(m_targetCrs)
                               : tr("未设置坐标系");
}

void PaleoCoordinateEditor::setMapCanvas(QWidget *canvas)
{
  m_canvas = qobject_cast<QgsMapCanvas *>(canvas);
  m_mapPick->setEnabled(m_canvas != nullptr);
  // 双分支都写 tooltip：只写未就绪分支会让挂上真实画布后按钮永远带着
  // 禁用原因文案（启用控件带禁用说明，违反 §35 禁用原因纪律的反向情形）。
  m_mapPick->setToolTip(m_canvas
                            ? tr("在地图上单击拾取坐标，Esc 或右键取消；地图拾取不会自动保存业务数据。")
                            : tr("地图画布尚未就绪，不能地图选点。"));
}

void PaleoCoordinateEditor::setPoint(const QgsPointXY &point)
{
  QSignalBlocker blockX(m_x), blockY(m_y);
  // 回显精度按坐标系走：地理十进制度保留 9 位（≈1mm），投影米制 3 位
  //（≈1mm）——统一 9 位会让投影坐标回显一长串尾零噪声（placeholder 3 位口径）。
  const int decimals = m_source->crs().isGeographic() ? 9 : 3;
  m_x->setText(paleo::coordinates::formatOrdinate(point.x(), true, m_source->crs(), format(), decimals));
  m_y->setText(paleo::coordinates::formatOrdinate(point.y(), false, m_source->crs(), format(), decimals));
  m_error->hide();
  emit pointChanged();
}

void PaleoCoordinateEditor::setPoint(const QPointF &point)
{
  setPoint(paleo::coordinates::point(point.x(), point.y()));
}

bool PaleoCoordinateEditor::sourcePoint(QgsPointXY *point, QString *error) const
{
  QString crsError;
  if (!m_source->validate(&crsError))
  {
    if (error) *error = crsError;
    return false;
  }
  return paleo::coordinates::parsePoint(m_x->text(), m_y->text(), m_source->crs(), format(), point, error);
}

bool PaleoCoordinateEditor::targetPoint(QPointF *point, QString *error) const
{
  if (!point) return false;
  QgsPointXY result;
  if (!targetPoint(&result, error)) return false;
  *point = QPointF(result.x(), result.y());
  return true;
}

bool PaleoCoordinateEditor::targetPoint(QgsPointXY *point, QString *error) const
{
  QgsPointXY source;
  if (!sourcePoint(&source, error)) return false;
  if (!m_targetCrs.isValid())
  {
    if (error) *error = tr("尚未选择目标坐标系。");
    return false;
  }
  const auto context = m_canvas ? m_canvas->mapSettings().transformContext() : m_context;
  return paleo::coordinates::transformPoint(source, m_source->crs(), m_targetCrs, context, point, error);
}

bool PaleoCoordinateEditor::showMapPoint(const QgsPointXY &point)
{
  if (!m_canvas) return false;
  QString error;
  QgsPointXY source;
  if (!paleo::coordinates::transformPoint(point, m_canvas->mapSettings().destinationCrs(),
                                          m_source->crs(), m_canvas->mapSettings().transformContext(),
                                          &source, &error))
  {
    showError(error);
    return false;
  }
  setPoint(source);
  return true;
}

bool PaleoCoordinateEditor::showMapPoint(double x, double y)
{
  return showMapPoint(QgsPointXY(x, y));
}

bool PaleoCoordinateEditor::validatePoint()
{
  QString error;
  QgsPointXY point;
  const bool valid = sourcePoint(&point, &error);
  if (valid) m_error->hide();
  else showError(error);
  return valid;
}

void PaleoCoordinateEditor::showError(const QString &error)
{
  m_error->setText(error);
  m_error->setToolTip(error);
  m_error->setVisible(!error.isEmpty());
}

paleo::coordinates::OrdinateFormat PaleoCoordinateEditor::format() const
{
  return static_cast<paleo::coordinates::OrdinateFormat>(m_format->currentData().toInt());
}

void PaleoCoordinateEditor::updateFormat()
{
  const bool geographic = m_source->crs().isGeographic();
  if (!geographic) m_format->setCurrentIndex(0);
  m_format->setEnabled(geographic);
  m_format->setToolTip(geographic ? tr("支持十进制度、度分、度分秒及 N/S/E/W 半球后缀。")
                                  : tr("投影与局部工程坐标使用数值 XY；不会套用经纬度范围。"));
  m_xCaption->setText(geographic ? tr("经度（X）") : tr("X／东坐标"));
  m_yCaption->setText(geographic ? tr("纬度（Y）") : tr("Y／北坐标"));
  m_x->setPlaceholderText(geographic ? tr("例如 108.037366 或 108°02′14.5″E") : tr("例如 300000.000"));
  m_y->setPlaceholderText(geographic ? tr("例如 36.086627 或 36°05′11.9″N") : tr("例如 3997621.120"));
  emit pointChanged();
}
