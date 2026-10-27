// 层：QGIS 封装
#include "crsselector.h"

#include <QComboBox>
#include <QWidget>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <qgsprojectionselectionwidget.h>

namespace paleo::coordinates
{
QgsCoordinateReferenceSystem crsFromDefinition(const QString &definition)
{
  const QString input = definition.trimmed();
  if (input.isEmpty() || input.size() > 262144 ||
      input.startsWith(QLatin1Char('/')) || input.startsWith(QLatin1Char('\\')) ||
      QRegularExpression(QStringLiteral("^[A-Za-z]:[\\\\/]")).match(input).hasMatch() ||
      input.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive) ||
      input.startsWith(QStringLiteral("http:"), Qt::CaseInsensitive) ||
      input.startsWith(QStringLiteral("https:"), Qt::CaseInsensitive))
    return {};
  if (input.contains(QLatin1Char('[')) && input.endsWith(QLatin1Char(']')))
    return QgsCoordinateReferenceSystem::fromWkt(input);
  if (input.contains(QStringLiteral("+proj=")))
    return QgsCoordinateReferenceSystem::fromProj(input);
  return QgsCoordinateReferenceSystem::fromOgcWmsCrs(input);
}

QString crsDefinition(const QgsCoordinateReferenceSystem &crs)
{
  if (!crs.isValid()) return QString();
  return crs.authid().isEmpty() ? crs.toWkt() : crs.authid();
}
}

PaleoCrsSelector::PaleoCrsSelector(bool allowEmpty, QWidget *parent)
  : QObject(parent), m_widget(new QWidget(parent)), m_allowEmpty(allowEmpty)
{
  auto *layout = new QHBoxLayout(m_widget);
  layout->setContentsMargins(0, 0, 0, 0);
  m_native = new QgsProjectionSelectionWidget(m_widget);
  m_native->setOptionVisible(QgsProjectionSelectionWidget::LayerCrs, false);
  m_native->setOptionVisible(QgsProjectionSelectionWidget::DefaultCrs, false);
  m_native->setOptionVisible(QgsProjectionSelectionWidget::ProjectCrs, false);
  m_native->setOptionVisible(QgsProjectionSelectionWidget::Invalid, false);
  m_native->setOptionVisible(QgsProjectionSelectionWidget::CrsNotSet, allowEmpty);
  m_native->setNotSetText(tr("自动识别／未设置"));
  m_native->setAllowTopocentricCrs(false);
  m_native->setShowAccuracyWarnings(true);
  m_native->setDialogTitle(tr("选择 QGIS 坐标参考系"));
  layout->addWidget(m_native);
  if (auto *combo = m_native->findChild<QComboBox *>())
  {
    combo->setMinimumContentsLength(12);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  }
  connect(m_native, &QgsProjectionSelectionWidget::crsChanged, this, [this](const auto &) {
    if (!m_restoring)
    {
      m_invalidDefinition.clear();
      emit definitionChanged(definition());
    }
  });
  connect(m_native, &QgsProjectionSelectionWidget::cleared, this, [this] {
    if (!m_restoring)
    {
      m_invalidDefinition.clear();
      emit definitionChanged(QString());
    }
  });
}

PaleoCrsSelector::~PaleoCrsSelector()
{
  delete m_widget.data();
}

QWidget *PaleoCrsSelector::widget() const
{
  return m_widget.data();
}

void PaleoCrsSelector::setNotSetText(const QString &text)
{
  m_native->setNotSetText(text);
}

QgsCoordinateReferenceSystem PaleoCrsSelector::crs() const
{
  return m_invalidDefinition.isEmpty() ? m_native->crs() : QgsCoordinateReferenceSystem();
}

QString PaleoCrsSelector::definition() const
{
  if (!m_invalidDefinition.isEmpty()) return m_invalidDefinition;
  return paleo::coordinates::crsDefinition(crs());
}

bool PaleoCrsSelector::setDefinition(const QString &definition)
{
  m_restoring = true;
  m_invalidDefinition.clear();
  if (definition.trimmed().isEmpty()) clear();
  else
  {
    const auto selected = paleo::coordinates::crsFromDefinition(definition);
    m_native->setCrs(selected);
    if (!selected.isValid()) m_invalidDefinition = definition.trimmed();
  }
  m_restoring = false;
  emit definitionChanged(this->definition());
  return m_invalidDefinition.isEmpty();
}

void PaleoCrsSelector::clear()
{
  m_restoring = true;
  m_invalidDefinition.clear();
  if (m_allowEmpty)
  {
    if (auto *combo = m_native->findChild<QComboBox *>())
    {
      for (int i = 0; i < combo->count(); ++i)
      {
        if (combo->itemData(i, StandardCoordinateReferenceSystemsModel::RoleOption).toInt() ==
            QgsProjectionSelectionWidget::CrsNotSet)
        {
          combo->setCurrentIndex(i);
          break;
        }
      }
    }
  }
  else m_native->setCrs(QgsCoordinateReferenceSystem());
  m_restoring = false;
}

bool PaleoCrsSelector::validate(QString *error, bool metricProject) const
{
  const auto fail = [error](const QString &message) {
    if (error) *error = message;
    return false;
  };
  if (error) error->clear();
  if (!m_invalidDefinition.isEmpty()) return fail(tr("无法识别坐标系定义；请从 QGIS 坐标系选择器重新选择。"));
  const auto selected = crs();
  if (!selected.isValid())
    return m_allowEmpty ? true : fail(tr("请选择坐标参考系。"));
  if (metricProject && (selected.isGeographic() ||
                        selected.mapUnits() != Qgis::DistanceUnit::Meters))
    return fail(tr("工程计算基准必须是米制投影坐标系，不能是经纬度、英尺或未知单位。"));
  return true;
}
